/*
 * zcu102_ddr_probe.c — verify DRAM reach + map ALL INA226 rails for DQ (column)
 * vs ACTIVATE (row) behavior.
 *
 * Answers two questions decisively, with a 3-mode factorial (fixed data G=0x00,
 * V=0xFF so HD=HW=max), measuring ns/iter AND all 18 INA226 rails (idle baseline
 * subtracted):
 *
 *   A  NOFLUSH  same-row     — loads hit L1/L2, must NOT reach DRAM
 *   B  FLUSH    same-row     — civac forces DRAM, row stays open: CAS/column + DQ
 *   C  FLUSH    row-conflict — civac forces DRAM, G and V in DIFFERENT rows of the
 *                              SAME bank: every access is PRECHARGE+ACTIVATE (row).
 *
 * Expected if the flush is correct:
 *   ns/iter:   A << B < C        (cache << row-hit DRAM < row-miss DRAM)
 *   signal:    A flat;  B lights the DQ/data rail; C additionally lights the
 *              activation/command rail (the row-level "VDD2 analog").
 * The C-minus-B per-rail delta isolates the row-activation rail.
 *
 * Build: gcc -O2 -march=armv8-a -mtune=cortex-a53 -o zcu102_ddr_probe zcu102_ddr_probe.c -lrt
 * Run:   sudo ./zcu102_ddr_probe
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
#define ROWCONFLICT_OFF 0x10000ul   /* 64 KiB: flips PA[16] -> different row, same bank */

static inline int64_t now_ns(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC_RAW,&t);
    return (int64_t)t.tv_sec*1000000000LL+t.tv_nsec;}
static inline int bank_id(uint64_t pa){return (pa>>14)&0x3;}
static inline int row_id (uint64_t pa){return (pa>>16)&0x7FFF;}

static int pmfd=-1;
static uint64_t v2p(void*v){uint64_t e,o=((uint64_t)v/PAGE)*8;
    if(pread(pmfd,&e,8,o)!=8)return 0; if(!(e&(1ULL<<63)))return 0;
    return ((e&((1ULL<<55)-1))*PAGE)|((uint64_t)v&(PAGE-1));}

/* flush+read G then V (DQ toggles g->v). flush=0 -> loads hit cache. */
static inline void iter(uint8_t*G,uint8_t*V,int flush){
    if(flush) asm volatile("dc civac,%0\n dc civac,%1\n dsb ish\n"::"r"(G),"r"(V):"memory");
    asm volatile(
        "ldp q0,q1,[%1,#0]\n ldp q2,q3,[%1,#32]\n"
        "ldp q4,q5,[%2,#0]\n ldp q6,q7,[%2,#32]\n"
        "umov x16,v3.d[0]\n str x16,%0\n umov x16,v7.d[0]\n str x16,%0\n"
        :"=m"(*(volatile uint64_t*)G):"r"(G),"r"(V)
        :"memory","x16","q0","q1","q2","q3","q4","q5","q6","q7");
}

#define MAXR 24
static char rname[MAXR][16]; static int rfd[MAXR]; static int nr=0;
static void disc(void){
    glob_t g; glob("/sys/class/hwmon/hwmon*",0,NULL,&g);
    for(size_t i=0;i<g.gl_pathc&&nr<MAXR;i++){
        char p[300],nm[64]={0}; FILE*f;
        snprintf(p,sizeof p,"%s/name",g.gl_pathv[i]); f=fopen(p,"r"); if(!f)continue;
        if(!fgets(nm,sizeof nm,f)){fclose(f);continue;} fclose(f);
        char*nl=strchr(nm,'\n'); if(nl)*nl=0;
        if(!strstr(nm,"ina226"))continue;
        snprintf(p,sizeof p,"%s/curr1_input",g.gl_pathv[i]);
        int fd=open(p,O_RDONLY); if(fd<0)continue;
        const char*u=strstr(nm,"u"); strncpy(rname[nr],u?u:nm,sizeof rname[nr]-1);
        rfd[nr++]=fd;
    }
    globfree(&g);
}
static inline int rd(int fd){char b[32];lseek(fd,0,SEEK_SET);int n=read(fd,b,sizeof b-1);
    if(n<=0)return -1;b[n]=0;return atoi(b);}

/* sample all rails over dur_ms while optionally running the gadget */
static void sample(uint8_t*G,uint8_t*V,int flush,int work,int dur_ms,double*out){
    double s[MAXR]={0}; int c=0;
    int64_t dl=now_ns()+(int64_t)dur_ms*1000000LL, nx=now_ns();
    while(now_ns()<dl){
        if(work) for(int k=0;k<256;k++) iter(G,V,flush);
        else { struct timespec t={0,200000}; nanosleep(&t,0); }
        int64_t t=now_ns();
        if(t>=nx){ for(int r=0;r<nr;r++){int v=rd(rfd[r]); if(v>=0)s[r]+=v;} c++; nx=t+6000000LL; }
    }
    for(int r=0;r<nr;r++) out[r]=c?s[r]/c:0;
}

int main(void){
    cpu_set_t cs;CPU_ZERO(&cs);CPU_SET(1,&cs);sched_setaffinity(0,sizeof cs,&cs);
    mlockall(MCL_CURRENT|MCL_FUTURE);
    pmfd=open("/proc/self/pagemap",O_RDONLY); if(pmfd<0){perror("pagemap");return 1;}
    uint8_t*base=mmap(0,HUGE_2M,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_HUGETLB,-1,0);
    if(base==MAP_FAILED){perror("hugepage mmap");return 1;}
    memset(base,0,HUGE_2M);
    uint8_t*G=base, *Vsame=base+LINE, *Vconf=base+ROWCONFLICT_OFF;
    memset(G,0x00,LINE);                 /* G all zeros */
    memset(Vsame,0xFF,LINE);             /* V all ones  -> HD=HW=512 bits/line */
    memset(Vconf,0xFF,LINE);
    asm volatile("dc civac,%0\ndc civac,%1\ndc civac,%2\ndsb ish"::"r"(G),"r"(Vsame),"r"(Vconf):"memory");
    uint64_t pg=v2p(G),ps=v2p(Vsame),pc=v2p(Vconf);
    printf("G   pa=0x%lx bank=%d row=%d\n",pg,bank_id(pg),row_id(pg));
    printf("Vsame pa=0x%lx bank=%d row=%d  (same-row: bank==G && row==G expected)\n",ps,bank_id(ps),row_id(ps));
    printf("Vconf pa=0x%lx bank=%d row=%d  (row-conflict: bank==G but row!=G expected)\n",pc,bank_id(pc),row_id(pc));
    printf("  -> same-row valid:  %d   row-conflict valid: %d\n\n",
           (bank_id(ps)==bank_id(pg)&&row_id(ps)==row_id(pg)),
           (bank_id(pc)==bank_id(pg)&&row_id(pc)!=row_id(pg)));

    disc();

    /* timing: ns per iteration, tight loop (no INA), 300k iters */
    struct { const char*tag; uint8_t*V; int flush; } M[3] = {
        {"A NOFLUSH same-row ", Vsame, 0},
        {"B FLUSH   same-row ", Vsame, 1},
        {"C FLUSH   rowconfl ", Vconf, 1},
    };
    double idle[MAXR];
    printf("=== idle baseline (no work) ===\n");
    sample(G,Vsame,0,0,400,idle);

    double load[3][MAXR]; double nsit[3];
    for(int m=0;m<3;m++){
        for(int w=0;w<50000;w++) iter(G,M[m].V,M[m].flush);   /* warm */
        int64_t t0=now_ns(); int IT=300000;
        for(int i=0;i<IT;i++) iter(G,M[m].V,M[m].flush);
        nsit[m]=(double)(now_ns()-t0)/IT;
        sample(G,M[m].V,M[m].flush,1,1500,load[m]);
    }

    printf("\n=== ns / iteration (1 iter = flush?+read 64B G + read 64B V) ===\n");
    for(int m=0;m<3;m++) printf("  %s  %8.1f ns/iter\n",M[m].tag,nsit[m]);
    printf("  ratio B/A=%.1fx  C/B=%.2fx   (DRAM reached if B,C >> A)\n",
           nsit[1]/nsit[0], nsit[2]/nsit[1]);

    printf("\n=== per-rail current (mA): idle, and load-minus-idle delta per mode ===\n");
    printf("  %-8s %8s | %10s %10s %10s | %s\n","rail","idle","A-idle","B-idle","C-idle","C-B(row)");
    for(int r=0;r<nr;r++){
        double dA=load[0][r]-idle[r], dB=load[1][r]-idle[r], dC=load[2][r]-idle[r];
        printf("  %-8s %8.1f | %10.2f %10.2f %10.2f | %+8.2f\n",
               rname[r], idle[r], dA, dB, dC, dC-dB);
    }
    return 0;
}
