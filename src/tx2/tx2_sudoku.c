/*
 * tx2_sudoku.c — Sudoku-style bandwidth analysis for DRAM component identification
 *
 * Implements Sudoku (Wi et al., IEEE Access 2026) Algorithm 1 (bandwidth-based
 * channel identification) on the Jetson TX2, captures BOTH timing throughput
 * (the Sudoku signal) AND VDD_SYS_DDR current (our signal) during every burst
 * so the two observables can be compared head-to-head on identical data.
 *
 * Methodology (Sudoku Alg. 1):
 *   For each candidate mapping function f with mask M:
 *     1. Partition all physical pages into two pools:
 *          pool_0 = { page | parity(page.PA & M) = 0 }
 *          pool_1 = { page | parity(page.PA & M) = 1 }
 *     2. For each constraint c in {0, 1}:
 *          Pick N_SEQ random addresses from pool_c to build an access sequence.
 *          Stream through the sequence (flush + load, round-robin) for BURST_MS.
 *          Record: throughput_cps (Sudoku's "bandwidth") + mean current (ours).
 *     3. Also run a random (unconstrained) baseline.
 *
 * Sudoku interpretation: the channel-selection function is the one whose
 * fixing halves the achievable bandwidth. With 2 sub-channels on the TX2
 * LPDDR4, the expected drop for PA[12] XOR PA[13] is ~2x. Other functions
 * (bank bits) should show a much smaller drop since bank parallelism within
 * a sub-channel remains available.
 *
 * Our power observable: if throughput halves, the DRAM is doing less work per
 * unit time, so VDD_SYS_DDR current should also drop. Whether the power-side
 * signal reproduces the bandwidth ranking is the central question.
 *
 * Build: make all
 * Run:   sudo ./build/tx2_sudoku -o results/sudoku.csv
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

#define MEM_SIZE_MB         1024
#define PAGE_SIZE           4096
#define CL_SIZE             64
#define WARMUP_SECS         90
#define BURST_MS            5000
#define ACCESSES_PER_CHUNK  50000

#define N_SEQ               16    /* Sudoku uses long sequences; 16 is enough
                                   * to let the MC pipeline within one
                                   * channel while still short enough that
                                   * every address fits in L1 addressing. */
#define N_TRIALS            5     /* different random sequences per constraint */
#define N_REPS              3     /* reps per trial */

/* Known mapping functions from kk_large analysis */
#define F1_MASK   ((1ULL<<18)|(1ULL<<19)|(1ULL<<21)|(1ULL<<22)|\
                   (1ULL<<24)|(1ULL<<25)|(1ULL<<27)|(1ULL<<28))
#define F2_MASK   ((1ULL<<14)|(1ULL<<17)|(1ULL<<18)|(1ULL<<20)|(1ULL<<21)|\
                   (1ULL<<23)|(1ULL<<24)|(1ULL<<26)|(1ULL<<27)|(1ULL<<29))
#define SUBCH_MASK ((1ULL<<12)|(1ULL<<13))

/* Candidate mapping functions to test.
 * Sudoku's claim: the channel function is the one whose fixing halves
 * the bandwidth. Everything else should barely move. */
static const struct {
    const char *name;
    uint64_t    mask;
    const char *formula;
} FUNCTIONS[] = {
    { "subch",   SUBCH_MASK, "PA[12] XOR PA[13] (known sub-channel)" },
    { "f1",      F1_MASK,    "known bank bit 0 (8 PA bits)"          },
    { "f2",      F2_MASK,    "known bank bit 1 (10 PA bits)"         },
    { "pa12",    1ULL<<12,   "PA[12] alone (partial sub-ch)"         },
    { "pa13",    1ULL<<13,   "PA[13] alone (partial sub-ch)"         },
    { "pa18",    1ULL<<18,   "PA[18] alone (single bank bit)"        },
    { "pa24",    1ULL<<24,   "PA[24] alone (single bank bit)"        },
};
#define N_FUNCTIONS (sizeof(FUNCTIONS)/sizeof(FUNCTIONS[0]))

/* ── ARM64 intrinsics ── */
static inline void clflush_fast(volatile void *p)
{ asm volatile("dc civac, %0" : : "r"(p) : "memory"); }
static inline void dsb_ish(void) { asm volatile("dsb ish" ::: "memory"); }
static inline void dsb_sy(void)  { asm volatile("dsb sy"  ::: "memory"); }
static inline int64_t now_ns(void)
{ struct timespec ts; clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
  return (int64_t)ts.tv_sec*1000000000LL + ts.tv_nsec; }

/* ── Pagemap (requires root) ── */
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

typedef struct { volatile char *va; uint64_t pa; } page_t;
typedef struct {
    double   tp_cps;       /* Sudoku's bandwidth proxy: chunks/sec */
    double   curr_mA;      /* Our power signal */
    int      n_samples;    /* INA3221 samples collected */
    double   elapsed_s;
    int64_t  total_accesses;
} result_t;

/* Parity (XOR reduction) of masked bits */
static int pa_fn(uint64_t pa, uint64_t mask)
{
    uint64_t v = pa & mask;
    int c = 0;
    while (v) { c ^= 1; v &= v - 1; }
    return c;
}

/* ══════════════════════════════════════════════════════════════
 * Stream measurement -- Sudoku-style bandwidth analysis
 * ══════════════════════════════════════════════════════════════
 * Given a sequence of N addresses all satisfying f(PA) = constraint,
 * flush them all, then load them all, in a tight loop. This gives
 * the memory controller maximum opportunity to pipeline across
 * whichever banks/sub-channels ARE available (i.e., those not
 * constrained by f). If f is the channel selector, the MC is
 * restricted to one channel → bandwidth halves. If f is a bank
 * bit, the MC still has both channels and 2 of 4 banks per
 * sub-channel → much smaller drop. */
static result_t stream_measure(volatile char **seq, int n, int ms)
{
    /* Touch once to warm TLB + populate caches */
    for (int i = 0; i < n; i++) sink = *(volatile uint64_t*)seq[i];
    dsb_sy();

    int64_t t_start = now_ns();
    int64_t deadline = t_start + (int64_t)ms * 1000000LL;
    int chunks = 0;
    double sum_curr = 0;
    int ns = 0;
    int64_t total_acc = 0;

    /* Keep each chunk roughly constant in total accesses so throughput
     * numbers are comparable across different sequence lengths. */
    int inner = ACCESSES_PER_CHUNK / n;

    while (now_ns() < deadline) {
        for (int c = 0; c < inner; c++) {
            for (int i = 0; i < n; i++) clflush_fast(seq[i]);
            dsb_ish();
            for (int i = 0; i < n; i++) sink = *(volatile uint64_t*)seq[i];
            total_acc += n;
        }
        chunks++;
        int cur = read_fd_int(ddr_fd);
        sum_curr += cur;
        ns++;
    }

    double elapsed = (now_ns() - t_start) / 1e9;
    result_t r = {
        .tp_cps         = elapsed > 0 ? chunks / elapsed : 0,
        .curr_mA        = ns > 0      ? sum_curr / ns    : 0,
        .n_samples      = ns,
        .elapsed_s      = elapsed,
        .total_accesses = total_acc,
    };
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

int main(int argc, char **argv)
{
    const char *out = "results/sudoku.csv";
    int warmup_s = WARMUP_SECS;
    int mem_mb = MEM_SIZE_MB;

    for(int i=1;i<argc;i++){
        if(!strcmp(argv[i],"-o")&&i+1<argc) out=argv[++i];
        else if(!strcmp(argv[i],"-w")&&i+1<argc) warmup_s=atoi(argv[++i]);
        else if(!strcmp(argv[i],"-M")&&i+1<argc) mem_mb=atoi(argv[++i]);
        else if(!strcmp(argv[i],"-h")){
            fprintf(stderr,"sudo %s [-o csv] [-w warmup] [-M MB]\n",argv[0]);
            fprintf(stderr,"  Sudoku-style bandwidth analysis for %d candidate functions\n",
                    (int)N_FUNCTIONS);
            fprintf(stderr,"  Bursts: %d funcs x 2 constraints x %d trials x %d reps = %d\n",
                    (int)N_FUNCTIONS, N_TRIALS, N_REPS,
                    (int)N_FUNCTIONS*2*N_TRIALS*N_REPS);
            return 0;
        }
    }

    fprintf(stderr,"==============================================\n");
    fprintf(stderr," JestINA -- Sudoku Bandwidth Analysis\n");
    fprintf(stderr," Functions: %d, trials/constraint: %d, reps: %d\n",
            (int)N_FUNCTIONS, N_TRIALS, N_REPS);
    fprintf(stderr," Burst: %d ms, seq length: %d\n", BURST_MS, N_SEQ);
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

    size_t max_pg = mem_size/PAGE_SIZE;
    page_t *pages = malloc(max_pg*sizeof(page_t));
    int n_pages = 0;
    uint64_t pa_min=UINT64_MAX, pa_max=0;
    for(size_t i=0;i<max_pg;i++){
        volatile char *va = (volatile char*)(mem + i*PAGE_SIZE);
        uint64_t pa = virt_to_phys(va);
        if(pa){
            pages[n_pages].va=va; pages[n_pages].pa=pa; n_pages++;
            if(pa<pa_min) pa_min=pa; if(pa>pa_max) pa_max=pa;
        }
    }
    fprintf(stderr,"  %d pages, PA [0x%lx, 0x%lx] = %.1f GB\n",
            n_pages, pa_min, pa_max, (pa_max-pa_min)/1e9);

    srand((unsigned)time(NULL));
    thermal_warmup(mem, mem_size, warmup_s);
    int64_t t0 = now_ns();

    FILE *fp = fopen(out, "w");
    fprintf(fp, "function,mask,formula,constraint,trial,rep,"
                "throughput_cps,mean_curr_mA,n_samples,total_accesses,elapsed_s\n");

    /* ── Per-function bandwidth tests ── */
    for (int fi = 0; fi < (int)N_FUNCTIONS; fi++) {
        const char *name    = FUNCTIONS[fi].name;
        uint64_t    mask    = FUNCTIONS[fi].mask;
        const char *formula = FUNCTIONS[fi].formula;

        /* Partition pages by the candidate function's parity */
        page_t *pool0 = malloc(n_pages*sizeof(page_t));
        page_t *pool1 = malloc(n_pages*sizeof(page_t));
        int n0=0, n1=0;
        for (int i = 0; i < n_pages; i++) {
            if (pa_fn(pages[i].pa, mask) == 0) pool0[n0++] = pages[i];
            else                                pool1[n1++] = pages[i];
        }

        fprintf(stderr, "\n== [%s] mask=0x%lx  (pool0=%d, pool1=%d) ==\n",
                name, mask, n0, n1);
        fprintf(stderr, "    %s\n", formula);

        for (int c = 0; c < 2; c++) {
            page_t *pool = (c == 0) ? pool0 : pool1;
            int     ps   = (c == 0) ? n0    : n1;
            if (ps < N_SEQ) {
                fprintf(stderr, "    skip constraint=%d (pool too small)\n", c);
                continue;
            }

            for (int t = 0; t < N_TRIALS; t++) {
                volatile char *seq[N_SEQ];
                for (int i = 0; i < N_SEQ; i++) seq[i] = pool[rand()%ps].va;

                for (int r = 0; r < N_REPS; r++) {
                    result_t res = stream_measure(seq, N_SEQ, BURST_MS);
                    fprintf(fp, "%s,0x%lx,\"%s\",%d,%d,%d,%.2f,%.2f,%d,%ld,%.3f\n",
                            name, mask, formula, c, t, r,
                            res.tp_cps, res.curr_mA, res.n_samples,
                            res.total_accesses, res.elapsed_s);
                    fflush(fp);
                }
                fprintf(stderr, "\r    constraint=%d trial=%d/%d  elapsed=%.0fs  ",
                        c, t+1, N_TRIALS, (now_ns()-t0)/1e9); fflush(stderr);
            }
            fprintf(stderr, "\n");
        }

        free(pool0); free(pool1);
    }

    /* ── Random baseline ── */
    fprintf(stderr, "\n== [random] unconstrained baseline ==\n");
    for (int t = 0; t < N_TRIALS; t++) {
        volatile char *seq[N_SEQ];
        for (int i = 0; i < N_SEQ; i++) seq[i] = pages[rand()%n_pages].va;
        for (int r = 0; r < N_REPS; r++) {
            result_t res = stream_measure(seq, N_SEQ, BURST_MS);
            fprintf(fp, "random,0x0,\"unconstrained baseline\",-1,%d,%d,%.2f,%.2f,%d,%ld,%.3f\n",
                    t, r, res.tp_cps, res.curr_mA, res.n_samples,
                    res.total_accesses, res.elapsed_s);
            fflush(fp);
        }
        fprintf(stderr, "\r    trial=%d/%d  elapsed=%.0fs  ",
                t+1, N_TRIALS, (now_ns()-t0)/1e9); fflush(stderr);
    }
    fprintf(stderr, "\n");

    fclose(fp);
    double total=(now_ns()-t0)/1e9;
    fprintf(stderr,"\n==============================================\n");
    fprintf(stderr," Done: %.0fs (%.1fh). Output: %s\n", total, total/3600, out);
    fprintf(stderr," Next: python3 analyze_sudoku.py %s\n", out);
    fprintf(stderr,"==============================================\n");

    free(pages); munmap(mem, mem_size); close(pagemap_fd);
    if(ddr_fd>=0) close(ddr_fd);
    return 0;
}
