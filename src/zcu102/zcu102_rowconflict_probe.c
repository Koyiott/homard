/*
 * zcu102_blind.c — Blind prediction: 500 random VA pairs, INA only
 *
 * 1. Allocate a FRESH 512 MB buffer (never used for training)
 * 2. Pick 500 completely random VA pairs (no bank knowledge)
 * 3. For each pair: DC CIVAC flush loop for 2s, poll INA
 * 4. Output INA data ONLY (no conflict label) → blind_ina.csv
 * 5. Separately output ground truth → blind_gt.csv (sealed until after predictions)
 *
 * Build:  gcc -O2 -Wall -o zcu102_blind zcu102_blind.c -lm
 * Run:    sudo ./zcu102_blind
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

#define MEM_SIZE_MB       512
#define PAGE_SIZE         4096
#define NUM_PAIRS         500
#define ACCESS_MS         2000
#define BASELINE_MS       100
#define INA_POLL_NS       30000000LL
#define MAX_INA           200
#define MAX_SENSORS       20

#define DC_CIVAC(a) asm volatile("dc civac, %0" : : "r"(a) : "memory")
#define DSB_ISH()   asm volatile("dsb ish" ::: "memory")
#define DSB_SY()    asm volatile("dsb sy"  ::: "memory")

static inline int bank_id(uint64_t pa) { return (pa >> 14) & 3; }
static inline int row_id(uint64_t pa)  { return (pa >> 16) & 0x7FFF; }

static inline int64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static int pagemap_fd = -1;
static uint64_t virt_to_phys(void *va) {
    uint64_t e;
    if (pread(pagemap_fd, &e, 8, ((uint64_t)va / PAGE_SIZE) * 8) != 8) return 0;
    if (!(e & (1ULL << 63))) return 0;
    return ((e & ((1ULL << 55) - 1)) * PAGE_SIZE) | ((uint64_t)va & (PAGE_SIZE - 1));
}

typedef struct { char name[32]; int curr_fd; int power_fd; } ina_t;
static ina_t sensors[MAX_SENSORS];
static int n_sensors = 0;

static int read_fd(int fd) {
    if (fd < 0) return 0;
    char b[32]; lseek(fd, 0, SEEK_SET);
    int n = read(fd, b, 31); if (n <= 0) return 0;
    b[n] = '\0'; return atoi(b);
}

static void discover(void) {
    char p[256], nb[64];
    for (int i = 0; i < 30 && n_sensors < MAX_SENSORS; i++) {
        snprintf(p, 256, "/sys/class/hwmon/hwmon%d/name", i);
        int fd = open(p, O_RDONLY); if (fd < 0) continue;
        int n = read(fd, nb, 63); close(fd); if (n <= 0) continue;
        nb[n] = '\0'; char *nl = strchr(nb, '\n'); if (nl) *nl = '\0';
        if (!strstr(nb, "ina226")) continue;
        ina_t *s = &sensors[n_sensors];
        strncpy(s->name, nb, 31);
        snprintf(p, 256, "/sys/class/hwmon/hwmon%d/curr1_input", i);
        s->curr_fd = open(p, O_RDONLY);
        snprintf(p, 256, "/sys/class/hwmon/hwmon%d/power1_input", i);
        s->power_fd = open(p, O_RDONLY);
        n_sensors++;
    }
}

typedef struct { int64_t ts; int phase; int c[MAX_SENSORS]; int pw[MAX_SENSORS]; } sample_t;
static volatile uint64_t sink;

static void run_pair(volatile char *a, volatile char *b,
                     int64_t dur_ns, int64_t bl_ns,
                     sample_t *buf, int *n, int max_n, int64_t t0)
{
    *n = 0;
    /* Baseline pre */
    int64_t end = now_ns() + bl_ns;
    while (now_ns() < end && *n < max_n) {
        buf[*n].ts = now_ns() - t0; buf[*n].phase = 0;
        for (int i = 0; i < n_sensors; i++) {
            buf[*n].c[i] = read_fd(sensors[i].curr_fd);
            buf[*n].pw[i] = read_fd(sensors[i].power_fd);
        }
        (*n)++; usleep(10000);
    }
    /* Access phase */
    int64_t deadline = now_ns() + dur_ns;
    int64_t next_poll = now_ns() + INA_POLL_NS;
    while (now_ns() < deadline) {
        while (now_ns() < next_poll && now_ns() < deadline) {
            DC_CIVAC(a); DC_CIVAC(b); DSB_ISH();
            sink = *(volatile uint64_t *)a; DSB_SY();
            sink = *(volatile uint64_t *)b; DSB_SY();
        }
        if (*n < max_n) {
            buf[*n].ts = now_ns() - t0; buf[*n].phase = 1;
            for (int i = 0; i < n_sensors; i++) {
                buf[*n].c[i] = read_fd(sensors[i].curr_fd);
                buf[*n].pw[i] = read_fd(sensors[i].power_fd);
            }
            (*n)++;
        }
        next_poll = now_ns() + INA_POLL_NS;
    }
    /* Baseline post */
    end = now_ns() + bl_ns;
    while (now_ns() < end && *n < max_n) {
        buf[*n].ts = now_ns() - t0; buf[*n].phase = 2;
        for (int i = 0; i < n_sensors; i++) {
            buf[*n].c[i] = read_fd(sensors[i].curr_fd);
            buf[*n].pw[i] = read_fd(sensors[i].power_fd);
        }
        (*n)++; usleep(10000);
    }
}

int main(void)
{
    fprintf(stderr, "=== ZCU102 Blind Prediction: 500 Random VA Pairs ===\n");

    cpu_set_t cs; CPU_ZERO(&cs); CPU_SET(0, &cs);
    sched_setaffinity(0, sizeof(cs), &cs);
    mlockall(MCL_CURRENT | MCL_FUTURE);
    discover();
    fprintf(stderr, "  %d INA226 sensors\n", n_sensors);

    pagemap_fd = open("/proc/self/pagemap", O_RDONLY);
    if (pagemap_fd < 0) { perror("pagemap"); return 1; }

    /* Fresh 512 MB buffer */
    size_t sz = (size_t)MEM_SIZE_MB << 20;
    char *mem = mmap(NULL, sz, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
    if (mem == MAP_FAILED) { perror("mmap"); return 1; }
    for (size_t i = 0; i < sz; i += PAGE_SIZE) mem[i] = (char)(i ^ 0xAB);

    /* Resolve PAs */
    size_t np = sz / PAGE_SIZE;
    typedef struct { char *va; uint64_t pa; } pe_t;
    pe_t *pg = malloc(np * sizeof(pe_t));
    int vld = 0;
    for (size_t i = 0; i < np; i++) {
        char *va = mem + i * PAGE_SIZE;
        uint64_t pa = virt_to_phys(va);
        if (pa) { pg[vld].va = va; pg[vld].pa = pa; vld++; }
    }
    fprintf(stderr, "  %d pages resolved\n", vld);

    /* 500 random pairs — seed 99999 (never used before) */
    srand(99999);
    typedef struct {
        char *va_a, *va_b;
        uint64_t pa_a, pa_b;
        int bank_a, bank_b, row_a, row_b, conflict;
    } pair_t;
    pair_t pairs[NUM_PAIRS];
    for (int i = 0; i < NUM_PAIRS; i++) {
        int i1 = rand() % vld, i2 = rand() % vld;
        while (i2 == i1) i2 = rand() % vld;
        pairs[i].va_a = pg[i1].va; pairs[i].va_b = pg[i2].va;
        pairs[i].pa_a = pg[i1].pa; pairs[i].pa_b = pg[i2].pa;
        pairs[i].bank_a = bank_id(pg[i1].pa); pairs[i].bank_b = bank_id(pg[i2].pa);
        pairs[i].row_a = row_id(pg[i1].pa);   pairs[i].row_b = row_id(pg[i2].pa);
        pairs[i].conflict = (pairs[i].bank_a == pairs[i].bank_b &&
                              pairs[i].row_a != pairs[i].row_b) ? 1 : 0;
    }
    int nc = 0; for (int i = 0; i < NUM_PAIRS; i++) nc += pairs[i].conflict;
    fprintf(stderr, "  500 random pairs: %d conflict (%.1f%%)\n", nc, 100.0*nc/500);

    /* ── INA output (NO conflict label — this is the blind data) ── */
    FILE *fp = fopen("blind_ina.csv", "w");
    fprintf(fp, "pair_id,phase,ina_idx,ina_ts_ns");
    for (int i = 0; i < n_sensors; i++)
        fprintf(fp, ",%s_curr,%s_power", sensors[i].name, sensors[i].name);
    fprintf(fp, "\n");

    /* ── Ground truth (sealed envelope) ── */
    FILE *gt = fopen("blind_gt.csv", "w");
    fprintf(gt, "pair_id,conflict,bank_a,bank_b,row_a,row_b,pa_a,pa_b\n");
    for (int i = 0; i < NUM_PAIRS; i++)
        fprintf(gt, "%d,%d,%d,%d,%d,%d,0x%lx,0x%lx\n",
                i, pairs[i].conflict, pairs[i].bank_a, pairs[i].bank_b,
                pairs[i].row_a, pairs[i].row_b,
                (unsigned long)pairs[i].pa_a, (unsigned long)pairs[i].pa_b);
    fclose(gt);

    /* ── Run blind experiment ── */
    sample_t *buf = malloc(MAX_INA * sizeof(sample_t));
    int64_t dur = (int64_t)ACCESS_MS * 1000000LL;
    int64_t bl  = (int64_t)BASELINE_MS * 1000000LL;
    int64_t t_start = now_ns();

    for (int p = 0; p < NUM_PAIRS; p++) {
        if (p % 25 == 0) {
            fprintf(stderr, "\r  pair %d/%d  elapsed=%.0fs  ",
                    p, NUM_PAIRS, (now_ns() - t_start) / 1e9);
            fflush(stderr);
        }
        int ns = 0;
        int64_t t0 = now_ns();
        run_pair(pairs[p].va_a, pairs[p].va_b, dur, bl, buf, &ns, MAX_INA, t0);
        for (int s = 0; s < ns; s++) {
            fprintf(fp, "%d,%d,%d,%lld", p, buf[s].phase, s, (long long)buf[s].ts);
            for (int i = 0; i < n_sensors; i++)
                fprintf(fp, ",%d,%d", buf[s].c[i], buf[s].pw[i]);
            fprintf(fp, "\n");
        }
    }
    fclose(fp);
    fprintf(stderr, "\r  Done: %.1fs\n", (now_ns() - t_start) / 1e9);
    fprintf(stderr, "  blind_ina.csv  — INA measurements (no labels)\n");
    fprintf(stderr, "  blind_gt.csv   — ground truth (sealed)\n");

    free(buf); free(pg);
    munmap(mem, sz); close(pagemap_fd);
    for (int i = 0; i < n_sensors; i++) {
        if (sensors[i].curr_fd >= 0) close(sensors[i].curr_fd);
        if (sensors[i].power_fd >= 0) close(sensors[i].power_fd);
    }
    return 0;
}
