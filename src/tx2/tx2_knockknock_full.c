/*
 * tx2_knockknock_full.c — Complete knock-knock mapping extraction
 *
 * Two modes of operation:
 *
 *  MODE 1 (-m 1, default): Targeted mask testing
 *    Test 8 candidate masks covering high PA bits (PA[12] through PA[28]).
 *    For each mask: 100 predicted same-bank + 40 predicted diff-bank probes.
 *    Reveals which bit combinations form the bank function.
 *
 *  MODE 2 (-m 2): Large-scale random collection
 *    3000 random probes against a single anchor with 5 reps each.
 *    No mask assumption — pure knock-knock observation.
 *    Produces ~100 confirmed conflict pairs for GF(2) null-space.
 *
 * Build: make all
 * Run:   sudo ./build/tx2_knockknock_full -m 1 -o results/kk_masks.csv
 *        sudo ./build/tx2_knockknock_full -m 2 -o results/kk_large.csv
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

#define MEM_SIZE_MB     1024      /* 1 GB — more PA bit coverage */
#define PAGE_SIZE       4096
#define CL_SIZE         64
#define WARMUP_SECS     90
#define BURST_MS        5000
#define ACCESSES_PER_CHUNK 50000

/* Mode 1: targeted masks */
#define MODE1_SAME_BANK     100
#define MODE1_DIFF_BANK     40
#define MODE1_REPS          5

/* Mode 2: large random collection */
#define MODE2_PROBES        3000
#define MODE2_REPS          3     /* fewer reps, more probes */

/* Candidate masks — covering low and high PA bits */
#define N_MASKS 8
static struct {
    const char *name;
    uint64_t mask;
} masks[N_MASKS] = {
    { "PA[12-13]",                      (1ULL<<12)|(1ULL<<13) },
    { "PA[12-13,18]",                   (1ULL<<12)|(1ULL<<13)|(1ULL<<18) },
    { "PA[12-13,20]",                   (1ULL<<12)|(1ULL<<13)|(1ULL<<20) },
    { "PA[12-13,22]",                   (1ULL<<12)|(1ULL<<13)|(1ULL<<22) },
    { "PA[12-13,18,20]",                (1ULL<<12)|(1ULL<<13)|(1ULL<<18)|(1ULL<<20) },
    { "PA[12-13,18,22]",                (1ULL<<12)|(1ULL<<13)|(1ULL<<18)|(1ULL<<22) },
    { "PA[12-13,20,22]",                (1ULL<<12)|(1ULL<<13)|(1ULL<<20)|(1ULL<<22) },
    { "PA[12-13,18,20,22]",             (1ULL<<12)|(1ULL<<13)|(1ULL<<18)|(1ULL<<20)|(1ULL<<22) },
};

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

/* ── Mode 1: mask testing ── */
static void mode1_masks(char *mem, page_t *pages, int n_pages, FILE *fp, int64_t t0)
{
    /* Two anchors from different PA regions */
    int anchor_idx[2] = { n_pages/3, 2*n_pages/3 };

    for(int mi=0; mi<N_MASKS; mi++){
        uint64_t mask = masks[mi].mask;
        fprintf(stderr,"\n══ Mask %d: %s (0x%lx) ══\n", mi, masks[mi].name, mask);

        for(int ai=0; ai<2; ai++){
            volatile char *anchor_va = pages[anchor_idx[ai]].va;
            uint64_t anchor_pa = pages[anchor_idx[ai]].pa;
            uint64_t anchor_masked = anchor_pa & mask;

            fprintf(stderr,"  Anchor %d: PA=0x%lx\n", ai, anchor_pa);

            /* Classify all pages by mask */
            int *same = malloc(n_pages*sizeof(int));
            int *diff = malloc(n_pages*sizeof(int));
            int n_s=0, n_d=0;
            for(int p=0; p<n_pages; p++){
                if(p==anchor_idx[ai]) continue;
                if((pages[p].pa & mask) == anchor_masked)
                    same[n_s++] = p;
                else
                    diff[n_d++] = p;
            }

            /* Shuffle */
            for(int i=n_s-1;i>0;i--){int j=rand()%(i+1); int t=same[i];same[i]=same[j];same[j]=t;}
            for(int i=n_d-1;i>0;i--){int j=rand()%(i+1); int t=diff[i];diff[i]=diff[j];diff[j]=t;}

            int ts = n_s < MODE1_SAME_BANK ? n_s : MODE1_SAME_BANK;
            int td = n_d < MODE1_DIFF_BANK ? n_d : MODE1_DIFF_BANK;
            fprintf(stderr,"    Testing %d+%d probes\n", ts, td);

            /* Test same-bank */
            for(int p=0; p<ts; p++){
                if(p%20==0){
                    fprintf(stderr,"\r    same %d/%d  elapsed=%.0fs  ",
                            p+1,ts,(now_ns()-t0)/1e9); fflush(stderr);
                }
                volatile char *pv=pages[same[p]].va;
                uint64_t pp=pages[same[p]].pa;
                for(int r=0;r<MODE1_REPS;r++){
                    burst_t br=burst_measure(anchor_va,pv,BURST_MS);
                    fprintf(fp,"%d,%s,0x%lx,0x%lx,0x%lx,same_bank,%d,%.2f,%.1f\n",
                            mi,masks[mi].name,anchor_pa,pp,anchor_pa^pp,r,br.mean_curr,br.tp);
                }
                fflush(fp);
            }
            /* Test diff-bank */
            for(int p=0; p<td; p++){
                volatile char *pv=pages[diff[p]].va;
                uint64_t pp=pages[diff[p]].pa;
                for(int r=0;r<MODE1_REPS;r++){
                    burst_t br=burst_measure(anchor_va,pv,BURST_MS);
                    fprintf(fp,"%d,%s,0x%lx,0x%lx,0x%lx,diff_bank,%d,%.2f,%.1f\n",
                            mi,masks[mi].name,anchor_pa,pp,anchor_pa^pp,r,br.mean_curr,br.tp);
                }
                fflush(fp);
            }
            fprintf(stderr,"\n");
            free(same); free(diff);
        }
    }
}

/* ── Mode 2: large random collection ── */
static void mode2_large(char *mem, page_t *pages, int n_pages, FILE *fp, int64_t t0)
{
    int anchor_idx = n_pages / 2;
    volatile char *anchor_va = pages[anchor_idx].va;
    uint64_t anchor_pa = pages[anchor_idx].pa;

    fprintf(stderr,"\n══ Mode 2: %d random probes vs anchor 0x%lx ══\n",
            MODE2_PROBES, anchor_pa);

    int n_probes = MODE2_PROBES < (n_pages-1) ? MODE2_PROBES : (n_pages-1);

    int *pidx = malloc(n_probes*sizeof(int));
    int picked = 0;
    while(picked < n_probes){
        int idx = rand() % n_pages;
        if(idx == anchor_idx) continue;
        int dup=0;
        for(int j=0;j<picked && !dup;j++) if(pidx[j]==idx) dup=1;
        if(!dup) pidx[picked++] = idx;
    }

    for(int p=0; p<n_probes; p++){
        if(p%50==0){
            fprintf(stderr,"\r  %d/%d  elapsed=%.0fs",
                    p+1,n_probes,(now_ns()-t0)/1e9); fflush(stderr);
        }
        volatile char *pv = pages[pidx[p]].va;
        uint64_t pp = pages[pidx[p]].pa;
        for(int r=0; r<MODE2_REPS; r++){
            burst_t br = burst_measure(anchor_va, pv, BURST_MS);
            fprintf(fp,"%d,random,0x%lx,0x%lx,0x%lx,random,%d,%.2f,%.1f\n",
                    p, anchor_pa, pp, anchor_pa^pp, r, br.mean_curr, br.tp);
        }
        if(p%50==0) fflush(fp);
    }
    fprintf(stderr,"\n");
    free(pidx);
}

int main(int argc, char **argv)
{
    int mode = 1;
    const char *out = "results/kk_masks.csv";
    int warmup_s = WARMUP_SECS;
    int mem_mb = MEM_SIZE_MB;

    for(int i=1;i<argc;i++){
        if(!strcmp(argv[i],"-m")&&i+1<argc) mode=atoi(argv[++i]);
        else if(!strcmp(argv[i],"-o")&&i+1<argc) out=argv[++i];
        else if(!strcmp(argv[i],"-w")&&i+1<argc) warmup_s=atoi(argv[++i]);
        else if(!strcmp(argv[i],"-M")&&i+1<argc) mem_mb=atoi(argv[++i]);
        else if(!strcmp(argv[i],"-h")){
            fprintf(stderr,"sudo %s [-m 1|2] [-o csv] [-w warmup] [-M MB]\n",argv[0]);
            fprintf(stderr,"  mode 1: 8 masks × 2 anchors × 140 probes × 5 reps\n");
            fprintf(stderr,"  mode 2: 1 anchor × %d random probes × 3 reps\n", MODE2_PROBES);
            return 0;
        }
    }

    fprintf(stderr,"════════════════════════════════════════════\n");
    fprintf(stderr," JestINA — Full Knock-Knock (mode %d)\n", mode);
    fprintf(stderr," Buffer: %d MB\n", mem_mb);
    fprintf(stderr,"════════════════════════════════════════════\n");

    if(geteuid()!=0){fprintf(stderr,"ERROR: root\n"); return 1;}
    cpu_set_t cs; CPU_ZERO(&cs); CPU_SET(0,&cs); sched_setaffinity(0,sizeof(cs),&cs);
    mlockall(MCL_CURRENT|MCL_FUTURE);
    {int fd=open("/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor",O_WRONLY);
     if(fd>=0){write(fd,"performance",11);close(fd);}}
    pagemap_fd=open("/proc/self/pagemap",O_RDONLY);
    if(pagemap_fd<0){perror("pagemap"); return 1;}
    if(discover_ddr()!=0){fprintf(stderr,"no sensor\n"); return 1;}

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
    fprintf(stderr,"  %d pages, PA range [0x%lx, 0x%lx] = %.1f GB\n",
            n_pages, pa_min, pa_max, (pa_max-pa_min)/1e9);

    srand((unsigned)time(NULL));

    FILE *fp = fopen(out,"w");
    fprintf(fp,"mask_id,mask_name,anchor_pa,probe_pa,pa_xor,predicted,rep,mean_curr_mA,throughput_cps\n");

    thermal_warmup(mem, mem_size, warmup_s);
    int64_t t0 = now_ns();

    if(mode == 1){
        mode1_masks(mem, pages, n_pages, fp, t0);
    } else if(mode == 2){
        mode2_large(mem, pages, n_pages, fp, t0);
    }

    fclose(fp);
    double total=(now_ns()-t0)/1e9;
    fprintf(stderr,"\n════════════════════════════════════════════\n");
    fprintf(stderr," Done: %.0fs (%.1fh). Output: %s\n", total, total/3600, out);
    fprintf(stderr,"════════════════════════════════════════════\n");

    free(pages); munmap(mem,mem_size); close(pagemap_fd);
    if(ddr_fd>=0) close(ddr_fd);
    return 0;
}
