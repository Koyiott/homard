/*
 * tx2_chipselect.c — Dual-anchor chip-select function extraction
 *
 * Uses TWO anchors known to be on different chips to break the symmetry
 * that prevents single-anchor experiments from recovering the chip-select
 * function.
 *
 * Protocol:
 *   1. Allocate 1 GB buffer, resolve all PAs via pagemap
 *   2. Use anchor_A (from kk_large: 0x19d411000) as reference chip-0
 *   3. Find anchor_B: a probe confirmed at ~264 mA (different chip from A)
 *      by measuring 20 candidates and picking the most stable one
 *   4. Run N random probes against BOTH anchors (interleaved)
 *   5. For each probe, classify:
 *      - level(A,P) < 262 → same chip as A → chip_select(P) = 0
 *      - level(A,P) >= 262 → diff chip from A → chip_select(P) = 1
 *      - level(B,P) provides cross-validation
 *   6. Output CSV with both measurements for GF(2) chip-select extraction
 *
 * Build: make all
 * Run:   sudo ./build/tx2_chipselect -o results/chipselect.csv
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <time.h>
#include <sched.h>
#include <errno.h>
#include <dirent.h>
#include <math.h>

#define MEM_SIZE_MB         1024
#define PAGE_SIZE           4096
#define CL_SIZE             64
#define WARMUP_SECS         90
#define BURST_MS            5000
#define ACCESSES_PER_CHUNK  50000

#define N_PROBES            1000    /* probes against each anchor */
#define N_REPS              3       /* reps per (anchor, probe) pair */
#define N_ANCHOR_CANDIDATES 20      /* candidates to find anchor_B */
#define ANCHOR_B_REPS       5       /* extra reps to confirm anchor_B */

/* Known bank functions from kk_large analysis */
#define F1_MASK  ((1ULL<<18)|(1ULL<<19)|(1ULL<<21)|(1ULL<<22)|(1ULL<<24)|(1ULL<<25)|(1ULL<<27)|(1ULL<<28))
#define F2_MASK  ((1ULL<<14)|(1ULL<<17)|(1ULL<<18)|(1ULL<<20)|(1ULL<<21)|(1ULL<<23)|(1ULL<<24)|(1ULL<<26)|(1ULL<<27)|(1ULL<<29))
#define SUBCH_MASK ((1ULL<<12)|(1ULL<<13))

/* ── ARM64 ── */
static inline void clflush_fast(volatile void *p)
{ asm volatile("dc civac, %0" : : "r"(p) : "memory"); }
static inline void dsb_ish(void) { asm volatile("dsb ish" ::: "memory"); }
static inline void dsb_sy(void)  { asm volatile("dsb sy"  ::: "memory"); }
static inline int64_t now_ns(void)
{ struct timespec ts; clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
  return (int64_t)ts.tv_sec*1000000000LL + ts.tv_nsec; }

/* ── Pagemap ── */
static int pagemap_fd = -1;
static uint64_t virt_to_phys(volatile void *va)
{ uint64_t e; if(pread(pagemap_fd,&e,8,((uint64_t)va/PAGE_SIZE)*8)!=8) return 0;
  if(!(e&(1ULL<<63))) return 0;
  return ((e&((1ULL<<55)-1))*PAGE_SIZE)|((uint64_t)va&(PAGE_SIZE-1)); }

/* ── INA3221 ── */
static int ddr_fd = -1;
static int read_sysfs_str(const char *p, char *b, int sz)
{ int fd=open(p,O_RDONLY); if(fd<0) return -1;
  int n=read(fd,b,sz-1); close(fd); if(n<=0) return -1;
  b[n]='\0'; char *nl=strchr(b,'\n'); if(nl)*nl='\0'; return 0; }
static int read_fd_int(int fd)
{ if(fd<0) return 0; char b[32]; lseek(fd,0,SEEK_SET);
  int n=read(fd,b,sizeof(b)-1); if(n<=0) return 0; b[n]='\0'; return atoi(b); }
static int discover_ddr(void)
{ const char *base="/sys/devices/3160000.i2c/i2c-0";
  const char *addrs[]={"0-0040","0-0041","0-0042","0-0043"};
  for(int a=0;a<4;a++){
    char dd[256]; snprintf(dd,sizeof(dd),"%s/%s",base,addrs[a]);
    DIR *dp=opendir(dd); if(!dp) continue;
    struct dirent *de; char iio[256]={0};
    while((de=readdir(dp)))
      if(strncmp(de->d_name,"iio:device",10)==0)
        {snprintf(iio,sizeof(iio),"%s/%s",dd,de->d_name); break;}
    closedir(dp); if(!iio[0]) continue;
    for(int ch=0;ch<3;ch++){
      char lp[256],lb[64],cp[256];
      snprintf(lp,sizeof(lp),"%s/rail_name_%d",iio,ch);
      if(read_sysfs_str(lp,lb,sizeof(lb))!=0) continue;
      if(!strstr(lb,"VDD_SYS_DDR")) continue;
      snprintf(cp,sizeof(cp),"%s/in_current%d_input",iio,ch);
      ddr_fd=open(cp,O_RDONLY);
      if(ddr_fd>=0){fprintf(stderr,"  Found %s\n",cp); return 0;}
    }
  } return -1; }

static volatile uint64_t sink;

typedef struct { double mean_curr; double tp; int n; } burst_t;

static burst_t burst_measure(volatile char *a, volatile char *b, int ms)
{
    sink=*(volatile uint64_t*)a; sink=*(volatile uint64_t*)b; dsb_sy();
    int64_t t_start = now_ns();
    int64_t deadline = t_start + (int64_t)ms*1000000LL;
    int chunks=0; double sum=0; int ns=0;
    while(now_ns()<deadline){
        for(int i=0;i<ACCESSES_PER_CHUNK;i++){
            clflush_fast(a); clflush_fast(b); dsb_ish();
            sink=*(volatile uint64_t*)a; sink=*(volatile uint64_t*)b;
        }
        chunks++;
        int c=read_fd_int(ddr_fd); sum+=c; ns++;
    }
    double elapsed=(now_ns()-t_start)/1e9;
    burst_t r={ns>0?sum/ns:0, elapsed>0?chunks/elapsed:0, ns};
    return r;
}

static void thermal_warmup(char *mem, size_t sz, int secs)
{
    fprintf(stderr,"  Warmup %ds...\n",secs);
    volatile char *a=mem+((size_t)rand()%(sz/CL_SIZE))*CL_SIZE;
    volatile char *b=mem+((size_t)rand()%(sz/CL_SIZE))*CL_SIZE;
    int64_t dl=now_ns()+(int64_t)secs*1000000000LL;
    int64_t lp=now_ns();
    while(now_ns()<dl){
        for(int i=0;i<100000;i++){
            clflush_fast(a);clflush_fast(b);dsb_ish();
            sink=*(volatile uint64_t*)a;sink=*(volatile uint64_t*)b;
        }
        int64_t now=now_ns();
        if(now-lp>5000000000LL){
            fprintf(stderr,"\r    %ds left  ",(int)((dl-now)/1000000000LL));
            fflush(stderr); lp=now;
        }
    }
    fprintf(stderr,"\r    warmup done                \n");
}

typedef struct { volatile char *va; uint64_t pa; } page_t;

/* Compute parity of bits selected by mask */
static int pa_fn(uint64_t pa, uint64_t mask)
{
    uint64_t v = pa & mask;
    /* popcount parity */
    int c = 0;
    while(v) { c ^= 1; v &= v-1; }
    return c;
}

int main(int argc, char **argv)
{
    const char *out = "results/chipselect.csv";
    int warmup_s = WARMUP_SECS;
    int mem_mb = MEM_SIZE_MB;
    int n_probes = N_PROBES;

    for(int i=1;i<argc;i++){
        if(!strcmp(argv[i],"-o")&&i+1<argc) out=argv[++i];
        else if(!strcmp(argv[i],"-w")&&i+1<argc) warmup_s=atoi(argv[++i]);
        else if(!strcmp(argv[i],"-M")&&i+1<argc) mem_mb=atoi(argv[++i]);
        else if(!strcmp(argv[i],"-n")&&i+1<argc) n_probes=atoi(argv[++i]);
        else if(!strcmp(argv[i],"-h")){
            fprintf(stderr,"sudo %s [-o csv] [-w warmup] [-M MB] [-n probes]\n",argv[0]);
            fprintf(stderr,"  Dual-anchor chip-select extraction\n");
            fprintf(stderr,"  Default: %d probes × 2 anchors × %d reps = %d bursts\n",
                    N_PROBES, N_REPS, N_PROBES*2*N_REPS);
            return 0;
        }
    }

    fprintf(stderr,"==============================================\n");
    fprintf(stderr," JestINA — Chip-Select Extraction\n");
    fprintf(stderr," Buffer: %d MB, Probes: %d\n", mem_mb, n_probes);
    fprintf(stderr,"==============================================\n");

    if(geteuid()!=0){fprintf(stderr,"ERROR: need root\n"); return 1;}
    cpu_set_t cs; CPU_ZERO(&cs); CPU_SET(0,&cs); sched_setaffinity(0,sizeof(cs),&cs);
    mlockall(MCL_CURRENT|MCL_FUTURE);
    {int fd=open("/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor",O_WRONLY);
     if(fd>=0){write(fd,"performance",11);close(fd);}}
    pagemap_fd=open("/proc/self/pagemap",O_RDONLY);
    if(pagemap_fd<0){perror("pagemap"); return 1;}
    if(discover_ddr()!=0){fprintf(stderr,"no DDR sensor\n"); return 1;}

    size_t mem_size = (size_t)mem_mb<<20;
    char *mem=mmap(NULL,mem_size,PROT_READ|PROT_WRITE,
                   MAP_PRIVATE|MAP_ANONYMOUS|MAP_POPULATE,-1,0);
    if(mem==MAP_FAILED){perror("mmap"); return 1;}
    for(size_t o=0;o<mem_size;o+=PAGE_SIZE) mem[o]=(char)(o&0xFF);
    for(size_t o=0;o<mem_size;o+=CL_SIZE) sink=*(volatile uint64_t*)(mem+o);
    dsb_sy();

    /* Build page table with PAs */
    size_t max_pg = mem_size/PAGE_SIZE;
    page_t *pages = malloc(max_pg*sizeof(page_t));
    int n_pages = 0;
    uint64_t pa_min = UINT64_MAX, pa_max = 0;
    for(size_t i=0;i<max_pg;i++){
        volatile char *va = (volatile char*)(mem + i*PAGE_SIZE);
        uint64_t pa = virt_to_phys(va);
        if(pa){
            pages[n_pages].va=va; pages[n_pages].pa=pa; n_pages++;
            if(pa<pa_min) pa_min=pa;
            if(pa>pa_max) pa_max=pa;
        }
    }
    fprintf(stderr,"  %d pages, PA [0x%lx, 0x%lx] = %.1f GB span\n",
            n_pages, pa_min, pa_max, (pa_max-pa_min)/1e9);

    srand((unsigned)time(NULL));
    thermal_warmup(mem, mem_size, warmup_s);
    int64_t t0 = now_ns();

    /* ══════════════════════════════════════════════
     * PHASE 1: Establish anchor_A and find anchor_B
     * ══════════════════════════════════════════════ */
    int anchorA_idx = n_pages / 2;
    volatile char *anchorA_va = pages[anchorA_idx].va;
    uint64_t anchorA_pa = pages[anchorA_idx].pa;

    fprintf(stderr,"\n══ Phase 1: Anchor selection ══\n");
    fprintf(stderr,"  Anchor A: PA=0x%lx (reference chip)\n", anchorA_pa);

    /* Print known mapping info for anchor A */
    fprintf(stderr,"    Sub-ch bits: PA[12]=%d PA[13]=%d\n",
            (int)((anchorA_pa>>12)&1), (int)((anchorA_pa>>13)&1));
    fprintf(stderr,"    f1(A)=%d, f2(A)=%d\n",
            pa_fn(anchorA_pa, F1_MASK), pa_fn(anchorA_pa, F2_MASK));

    /* Find anchor_B: measure N_ANCHOR_CANDIDATES pages, pick the one that:
     *   1. Has the SAME sub-channel as A (PA[12]XOR PA[13] matches A)
     *      -- without this constraint, anchor_B selection picks up a
     *         pair that differs in BOTH chip and sub-channel, and the
     *         resulting "chip-select" extraction just recovers the
     *         sub-channel function.
     *   2. Reads stably above 263 mA vs anchor_A (firmly in diff-chip
     *      band, not at the 260/264 boundary)
     *   3. Has minimum std across 3 reps (stable measurement)
     */
    fprintf(stderr,"  Searching for anchor_B (diff chip, SAME sub-channel as A)...\n");
    int anchorA_subch = pa_fn(anchorA_pa, SUBCH_MASK);
    fprintf(stderr,"    Constraint: PA[12]XOR PA[13] == %d (matches anchor A)\n",
            anchorA_subch);

    int best_B_idx = -1;
    double best_B_curr = 0;
    double best_B_std = 999;
    int tried = 0, skipped_subch = 0;

    for(int c=0; c<N_ANCHOR_CANDIDATES*4 && tried < N_ANCHOR_CANDIDATES; c++){
        int idx = rand() % n_pages;
        if(idx == anchorA_idx) continue;

        /* Sub-channel constraint: reject candidates with different subch */
        if(pa_fn(pages[idx].pa, SUBCH_MASK) != anchorA_subch){
            skipped_subch++;
            continue;
        }
        tried++;

        /* Quick 3-rep check */
        double sum=0, sum2=0;
        int ok = 1;
        for(int r=0; r<3; r++){
            burst_t br = burst_measure(anchorA_va, pages[idx].va, BURST_MS);
            sum += br.mean_curr;
            sum2 += br.mean_curr * br.mean_curr;
            /* Stricter threshold (263 not 262) to avoid 260/264 boundary */
            if(br.mean_curr < 263) { ok = 0; break; }
        }
        if(!ok) continue;

        double mean = sum / 3;
        double std = sqrt(sum2/3 - mean*mean);

        fprintf(stderr,"\r    Candidate %d: PA=0x%lx curr=%.1f±%.1f (subch=%d)  ",
                tried, pages[idx].pa, mean, std,
                pa_fn(pages[idx].pa, SUBCH_MASK));

        if(mean > 263 && std < best_B_std){
            best_B_idx = idx;
            best_B_curr = mean;
            best_B_std = std;
        }
    }
    fprintf(stderr,"\n    (tried %d same-subch candidates, skipped %d with different subch)\n",
            tried, skipped_subch);

    if(best_B_idx < 0){
        fprintf(stderr,"\nERROR: no diff-chip anchor found!\n");
        return 1;
    }

    volatile char *anchorB_va = pages[best_B_idx].va;
    uint64_t anchorB_pa = pages[best_B_idx].pa;

    /* Confirm anchor_B with extra reps */
    fprintf(stderr,"\n  Confirming anchor_B: PA=0x%lx...\n", anchorB_pa);
    double conf_sum=0;
    for(int r=0; r<ANCHOR_B_REPS; r++){
        burst_t br = burst_measure(anchorA_va, anchorB_va, BURST_MS);
        conf_sum += br.mean_curr;
        fprintf(stderr,"    Rep %d: %.2f mA\n", r, br.mean_curr);
    }
    double conf_mean = conf_sum / ANCHOR_B_REPS;
    if(conf_mean < 262){
        fprintf(stderr,"  WARNING: anchor_B confirmation failed (%.1f mA < 262)\n", conf_mean);
        fprintf(stderr,"  Continuing anyway...\n");
    }

    fprintf(stderr,"\n  Anchor B: PA=0x%lx (different chip, confirmed %.1f mA)\n",
            anchorB_pa, conf_mean);
    fprintf(stderr,"    Sub-ch bits: PA[12]=%d PA[13]=%d\n",
            (int)((anchorB_pa>>12)&1), (int)((anchorB_pa>>13)&1));
    fprintf(stderr,"    f1(B)=%d, f2(B)=%d\n",
            pa_fn(anchorB_pa, F1_MASK), pa_fn(anchorB_pa, F2_MASK));
    fprintf(stderr,"    A XOR B = 0x%lx\n", anchorA_pa ^ anchorB_pa);

    /* Print which high bits differ between A and B */
    fprintf(stderr,"    Bit diff A vs B:");
    for(int b=12; b<34; b++){
        int ba = (anchorA_pa>>b)&1;
        int bb = (anchorB_pa>>b)&1;
        if(ba != bb) fprintf(stderr," PA[%d](%d->%d)", b, ba, bb);
    }
    fprintf(stderr,"\n");

    /* ══════════════════════════════════════════════
     * PHASE 2: Probe N addresses against BOTH anchors
     * ══════════════════════════════════════════════ */
    fprintf(stderr,"\n══ Phase 2: %d probes × 2 anchors × %d reps ══\n",
            n_probes, N_REPS);

    FILE *fp = fopen(out, "w");
    fprintf(fp, "probe_id,anchor_id,anchor_pa,probe_pa,pa_xor,"
                "rep,mean_curr_mA,throughput_cps,"
                "subch_xor,f1_xor,f2_xor\n");

    /* Pick random probe indices */
    int np = n_probes < (n_pages-2) ? n_probes : (n_pages-2);
    int *pidx = malloc(np * sizeof(int));
    int picked = 0;
    while(picked < np){
        int idx = rand() % n_pages;
        if(idx == anchorA_idx || idx == best_B_idx) continue;
        int dup = 0;
        for(int j=0; j<picked && !dup; j++) if(pidx[j]==idx) dup=1;
        if(!dup) pidx[picked++] = idx;
    }

    for(int p=0; p<np; p++){
        if(p%25==0){
            double elapsed = (now_ns()-t0)/1e9;
            double eta = (elapsed / (p+1)) * (np - p - 1);
            fprintf(stderr,"\r  Probe %d/%d  elapsed=%.0fs  ETA=%.0fs     ",
                    p+1, np, elapsed, eta);
            fflush(stderr);
        }

        volatile char *pv = pages[pidx[p]].va;
        uint64_t pp = pages[pidx[p]].pa;

        /* Measure against anchor A */
        uint64_t xorA = anchorA_pa ^ pp;
        int scA = pa_fn(pp ^ anchorA_pa, SUBCH_MASK) == 0 ? 0 : 1;
        int f1A = pa_fn(xorA, F1_MASK);
        int f2A = pa_fn(xorA, F2_MASK);
        for(int r=0; r<N_REPS; r++){
            burst_t br = burst_measure(anchorA_va, pv, BURST_MS);
            fprintf(fp, "%d,A,0x%lx,0x%lx,0x%lx,%d,%.2f,%.1f,%d,%d,%d\n",
                    p, anchorA_pa, pp, xorA, r, br.mean_curr, br.tp,
                    scA, f1A, f2A);
        }

        /* Measure against anchor B */
        uint64_t xorB = anchorB_pa ^ pp;
        int scB = pa_fn(pp ^ anchorB_pa, SUBCH_MASK) == 0 ? 0 : 1;
        int f1B = pa_fn(xorB, F1_MASK);
        int f2B = pa_fn(xorB, F2_MASK);
        for(int r=0; r<N_REPS; r++){
            burst_t br = burst_measure(anchorB_va, pv, BURST_MS);
            fprintf(fp, "%d,B,0x%lx,0x%lx,0x%lx,%d,%.2f,%.1f,%d,%d,%d\n",
                    p, anchorB_pa, pp, xorB, r, br.mean_curr, br.tp,
                    scB, f1B, f2B);
        }

        if(p%25==0) fflush(fp);
    }

    fprintf(stderr,"\n");
    fclose(fp);
    free(pidx);

    double total=(now_ns()-t0)/1e9;
    fprintf(stderr,"\n==============================================\n");
    fprintf(stderr," Done: %.0fs (%.1fh). Output: %s\n", total, total/3600, out);
    fprintf(stderr," Anchor A: 0x%lx (chip 0)\n", anchorA_pa);
    fprintf(stderr," Anchor B: 0x%lx (chip 1)\n", anchorB_pa);
    fprintf(stderr,"==============================================\n");

    free(pages); munmap(mem, mem_size); close(pagemap_fd);
    if(ddr_fd>=0) close(ddr_fd);
    return 0;
}
