/*
 * tx2_ina_burst.c — Burst-mode DDR conflict detection via INA3221
 *                   on NVIDIA Jetson TX2 (Tegra186 / LPDDR4)
 *
 * Ported from ZCU102 burst mode (ConflictINA) which achieved d=4.80.
 *
 * Key adaptations from ZCU102 → TX2:
 *   - INA3221 (IIO sysfs) instead of INA226 (hwmon)
 *   - LPDDR4 bank mapping unknown → use random pairs for calibration,
 *     then use the per-pair current to sort conflict vs non-conflict
 *   - Thermal warmup phase: 60s of hammering before data collection
 *     (TX2 A57 @ 2 GHz runs hot, causing thermal transients)
 *   - TLB warmup: read all pages + re-read before each burst
 *   - Pin CPU frequency via sysfs (avoid DVFS during experiment)
 *   - Poll only VDD_SYS_DDR for speed
 *
 * Protocol:
 *   Phase 0: Thermal warmup (60s hammering, no data)
 *   Phase 1: Baseline idle INA capture (5s)
 *   Phase 2: For each trial:
 *     a. Set A burst: 5s of sustained access on pair-set A + INA polling
 *     b. Idle gap: 2s
 *     c. Set B burst: 5s of sustained access on pair-set B + INA polling
 *     d. Idle gap: 2s
 *   Analysis: Otsu split on per-set mean current → conflict vs non-conflict
 *
 * Build:  make all
 * Run:    sudo ./build/tx2_ina_burst -t 20 -o results/burst.csv
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

/* ── Configuration ── */
#define MEM_SIZE_MB        512
#define PAGE_SIZE          4096
#define CL_SIZE            64
#define WARMUP_SECS        60      /* thermal stabilization */
#define BURST_DURATION_MS  5000    /* longer than ZCU102: INA3221 is slower */
#define IDLE_GAP_MS        2000
#define BASELINE_MS        5000
#define NUM_TRIALS         20
#define ACCESSES_PER_CHUNK 5000
#define N_PAIR_SETS        40      /* number of random pair-sets */
#define PAIRS_PER_SET      10      /* addresses per set */

/* ── ARM64 intrinsics (Cortex-A57) ── */
static inline void clflush(volatile void *p)
{
    asm volatile("dc civac, %0\ndsb ish" : : "r"(p) : "memory");
}

static inline void mfence(void)
{
    asm volatile("dsb sy" ::: "memory");
}

static inline int64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

/* ── INA3221: discover VDD_SYS_DDR only ── */
static int ddr_curr_fd = -1;
static char ddr_label[64] = "VDD_SYS_DDR";

static int read_sysfs_str(const char *path, char *buf, int bufsz)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    int n = read(fd, buf, bufsz - 1);
    close(fd);
    if (n <= 0) return -1;
    buf[n] = '\0';
    char *nl = strchr(buf, '\n'); if (nl) *nl = '\0';
    return 0;
}

static int read_fd_int(int fd)
{
    if (fd < 0) return 0;
    char buf[32];
    lseek(fd, 0, SEEK_SET);
    int n = read(fd, buf, sizeof(buf) - 1);
    if (n <= 0) return 0;
    buf[n] = '\0';
    return atoi(buf);
}

static int discover_ddr_sensor(void)
{
    const char *i2c_base = "/sys/devices/3160000.i2c/i2c-0";
    const char *addrs[] = { "0-0040", "0-0041", "0-0042", "0-0043" };

    for (int a = 0; a < 4; a++) {
        char dev_dir[256];
        snprintf(dev_dir, sizeof(dev_dir), "%s/%s", i2c_base, addrs[a]);
        DIR *dp = opendir(dev_dir);
        if (!dp) continue;
        struct dirent *de;
        char iio_dir[256] = {0};
        while ((de = readdir(dp)) != NULL) {
            if (strncmp(de->d_name, "iio:device", 10) == 0) {
                snprintf(iio_dir, sizeof(iio_dir), "%s/%s", dev_dir, de->d_name);
                break;
            }
        }
        closedir(dp);
        if (!iio_dir[0]) continue;

        for (int ch = 0; ch < 3; ch++) {
            char label_path[256], label[64], curr_path[256];
            snprintf(label_path, sizeof(label_path), "%s/rail_name_%d", iio_dir, ch);
            if (read_sysfs_str(label_path, label, sizeof(label)) != 0) continue;
            if (!strstr(label, "VDD_SYS_DDR")) continue;

            snprintf(curr_path, sizeof(curr_path), "%s/in_current%d_input", iio_dir, ch);
            ddr_curr_fd = open(curr_path, O_RDONLY);
            if (ddr_curr_fd < 0) continue;
            strncpy(ddr_label, label, sizeof(ddr_label));
            fprintf(stderr, "  Found %s at %s\n", label, curr_path);
            return 0;
        }
    }
    return -1;
}

/* ── INA sample storage ── */
#define MAX_SAMPLES 100000

typedef struct {
    int64_t ts_ns;
    int     phase;      /* 0=baseline, 1..N_PAIR_SETS=burst set id, 99=idle */
    int     trial;
    int     set_id;     /* which pair-set was active */
    int     curr_mA;
} ina_sample_t;

static ina_sample_t *samples;
static int n_samples = 0;

static void take_snapshot(int phase, int trial, int set_id, int64_t t0)
{
    if (n_samples >= MAX_SAMPLES) return;
    ina_sample_t *s = &samples[n_samples];
    s->ts_ns   = now_ns() - t0;
    s->phase   = phase;
    s->trial   = trial;
    s->set_id  = set_id;
    s->curr_mA = read_fd_int(ddr_curr_fd);
    n_samples++;
}

/* ── Memory access ── */
static volatile uint64_t sink;

/* TLB warmup: read every cache line in the pair set */
static void tlb_warmup(volatile char **addrs, int n)
{
    for (int i = 0; i < n; i++) {
        sink = *(volatile uint64_t *)addrs[i];
        mfence();
    }
}

/* Sustained burst: hammer pair-set for duration_ms, polling INA */
static void burst_access(volatile char **va_a, volatile char **va_b,
                          int n_pairs, int duration_ms,
                          int phase, int trial, int set_id, int64_t t0)
{
    /* TLB warmup before measurement */
    tlb_warmup(va_a, n_pairs);
    tlb_warmup(va_b, n_pairs);

    int64_t deadline = now_ns() + (int64_t)duration_ms * 1000000LL;
    int pair_idx = 0;

    while (now_ns() < deadline) {
        volatile char *a = va_a[pair_idx % n_pairs];
        volatile char *b = va_b[pair_idx % n_pairs];
        for (int i = 0; i < ACCESSES_PER_CHUNK; i++) {
            clflush(a);
            clflush(b);
            mfence();
            sink = *(volatile uint64_t *)a;
            mfence();
            sink = *(volatile uint64_t *)b;
            mfence();
        }
        pair_idx++;

        take_snapshot(phase, trial, set_id, t0);
    }
}

static void idle_with_ina(int duration_ms, int trial, int64_t t0)
{
    int64_t deadline = now_ns() + (int64_t)duration_ms * 1000000LL;
    while (now_ns() < deadline) {
        take_snapshot(99, trial, -1, t0);
        usleep(5000);
    }
}

/* ── Thermal warmup: sustained random hammering ── */
static void thermal_warmup(char *mem, size_t mem_size, int seconds)
{
    fprintf(stderr, "  Thermal warmup: %ds of sustained hammering...\n", seconds);
    size_t n_cls = mem_size / CL_SIZE;
    int64_t deadline = now_ns() + (int64_t)seconds * 1000000000LL;

    /* Pick 2 random addresses */
    volatile char *a = mem + ((size_t)rand() % n_cls) * CL_SIZE;
    volatile char *b = mem + ((size_t)rand() % n_cls) * CL_SIZE;

    int64_t last_print = now_ns();
    while (now_ns() < deadline) {
        for (int i = 0; i < 50000; i++) {
            clflush(a); clflush(b); mfence();
            sink = *(volatile uint64_t *)a; mfence();
            sink = *(volatile uint64_t *)b; mfence();
        }
        int64_t now = now_ns();
        if (now - last_print > 5000000000LL) {
            int remaining = (int)((deadline - now) / 1000000000LL);
            fprintf(stderr, "\r    warmup: %ds remaining  curr=%d mA  ",
                    remaining, read_fd_int(ddr_curr_fd));
            fflush(stderr);
            last_print = now;
        }
    }
    fprintf(stderr, "\r    warmup complete, curr=%d mA                  \n",
            read_fd_int(ddr_curr_fd));
}

/* ── Try to pin CPU frequency ── */
static void pin_cpu_freq(void)
{
    /* Try to set governor to performance */
    const char *gov_path = "/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor";
    int fd = open(gov_path, O_WRONLY);
    if (fd >= 0) {
        const char *perf = "performance";
        if (write(fd, perf, strlen(perf)) > 0)
            fprintf(stderr, "  CPU governor: performance\n");
        close(fd);
    } else {
        fprintf(stderr, "  CPU governor: could not pin (run as root)\n");
    }
}

/* ══════════════════════════════════════════════════════════════════════════════
 *  Main
 * ════════════════════════════════════════════════════════════════════════════ */

int main(int argc, char **argv)
{
    int num_trials   = NUM_TRIALS;
    int burst_ms     = BURST_DURATION_MS;
    int n_sets       = N_PAIR_SETS;
    int warmup_secs  = WARMUP_SECS;
    const char *out_csv = "results/burst.csv";

    for (int i = 1; i < argc; i++) {
        if      (strcmp(argv[i], "-t") == 0 && i+1 < argc) num_trials   = atoi(argv[++i]);
        else if (strcmp(argv[i], "-b") == 0 && i+1 < argc) burst_ms     = atoi(argv[++i]);
        else if (strcmp(argv[i], "-s") == 0 && i+1 < argc) n_sets       = atoi(argv[++i]);
        else if (strcmp(argv[i], "-w") == 0 && i+1 < argc) warmup_secs  = atoi(argv[++i]);
        else if (strcmp(argv[i], "-o") == 0 && i+1 < argc) out_csv      = argv[++i];
        else if (strcmp(argv[i], "-h") == 0) {
            fprintf(stderr,
                "Usage: sudo %s [options]\n"
                "  -t N   Trials per set (default %d)\n"
                "  -b N   Burst duration ms (default %d)\n"
                "  -s N   Number of random pair-sets (default %d)\n"
                "  -w N   Thermal warmup seconds (default %d)\n"
                "  -o F   Output CSV\n",
                argv[0], NUM_TRIALS, BURST_DURATION_MS, N_PAIR_SETS, WARMUP_SECS);
            return 0;
        }
    }

    fprintf(stderr, "════════════════════════════════════════════════════\n");
    fprintf(stderr, " JestINA — Burst-Mode Conflict Detection\n");
    fprintf(stderr, " Board: Jetson TX2 (Tegra186 / LPDDR4 8 GB)\n");
    fprintf(stderr, " Sensor: VDD_SYS_DDR (INA3221)\n");
    fprintf(stderr, " Trials: %d  Burst: %dms  Sets: %d  Warmup: %ds\n",
            num_trials, burst_ms, n_sets, warmup_secs);
    fprintf(stderr, "════════════════════════════════════════════════════\n");

    /* Pin CPU */
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(0, &cpuset);
    sched_setaffinity(0, sizeof(cpuset), &cpuset);
    fprintf(stderr, "  Pinned to core 0 (Cortex-A57)\n");

    mlockall(MCL_CURRENT | MCL_FUTURE);
    pin_cpu_freq();

    /* Discover sensor */
    fprintf(stderr, "  Discovering INA3221 VDD_SYS_DDR...\n");
    if (discover_ddr_sensor() != 0) {
        fprintf(stderr, "  ERROR: VDD_SYS_DDR not found!\n");
        return 1;
    }

    /* Allocate memory */
    size_t mem_size = (size_t)MEM_SIZE_MB << 20;
    char *mem = mmap(NULL, mem_size, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
    if (mem == MAP_FAILED) { perror("mmap"); return 1; }

    /* Touch all pages (TLB + page table population) */
    fprintf(stderr, "  Touching %d MB...\n", MEM_SIZE_MB);
    for (size_t off = 0; off < mem_size; off += PAGE_SIZE)
        mem[off] = (char)(off & 0xFF);

    /* Full TLB warmup: read every cache line */
    fprintf(stderr, "  TLB warmup: reading all %lu cache lines...\n", mem_size / CL_SIZE);
    for (size_t off = 0; off < mem_size; off += CL_SIZE)
        sink = *(volatile uint64_t *)(mem + off);
    mfence();

    /* Generate N random pair-sets. Each set has PAIRS_PER_SET address pairs.
     * Some will be conflict (same bank), most won't — we discover this from INA. */
    size_t n_cls = mem_size / CL_SIZE;
    srand((unsigned)time(NULL));

    typedef struct {
        volatile char *a[PAIRS_PER_SET];
        volatile char *b[PAIRS_PER_SET];
    } pair_set_t;

    pair_set_t *sets = malloc(n_sets * sizeof(pair_set_t));
    for (int s = 0; s < n_sets; s++) {
        for (int p = 0; p < PAIRS_PER_SET; p++) {
            size_t cl_a = (size_t)rand() % n_cls;
            size_t cl_b = (size_t)rand() % n_cls;
            while (cl_b == cl_a) cl_b = (size_t)rand() % n_cls;
            sets[s].a[p] = (volatile char *)(mem + cl_a * CL_SIZE);
            sets[s].b[p] = (volatile char *)(mem + cl_b * CL_SIZE);
        }
    }
    fprintf(stderr, "  Generated %d pair-sets × %d pairs\n", n_sets, PAIRS_PER_SET);

    /* Allocate sample buffer */
    samples = malloc(MAX_SAMPLES * sizeof(ina_sample_t));
    n_samples = 0;

    /* ── Phase 0: Thermal warmup ── */
    thermal_warmup(mem, mem_size, warmup_secs);

    /* ── Phase 1: Baseline ── */
    int64_t t0 = now_ns();
    fprintf(stderr, "  Baseline capture (%dms)...\n", BASELINE_MS);
    idle_with_ina(BASELINE_MS, -1, t0);

    /* ── Phase 2: Burst trials ── */
    /* For each trial: cycle through all pair-sets with burst + idle */
    for (int trial = 0; trial < num_trials; trial++) {
        fprintf(stderr, "\r  Trial %d/%d  samples=%d/%d  ",
                trial + 1, num_trials, n_samples, MAX_SAMPLES);
        fflush(stderr);

        if (n_samples >= MAX_SAMPLES - 1000) {
            fprintf(stderr, "\n  Sample buffer full.\n");
            break;
        }

        /* Shuffle set order each trial to avoid order bias */
        for (int i = n_sets - 1; i > 0; i--) {
            int j = rand() % (i + 1);
            pair_set_t tmp = sets[i]; sets[i] = sets[j]; sets[j] = tmp;
        }

        for (int s = 0; s < n_sets; s++) {
            burst_access(sets[s].a, sets[s].b, PAIRS_PER_SET,
                         burst_ms, s + 1, trial, s, t0);
            idle_with_ina(IDLE_GAP_MS, trial, t0);
        }
    }

    double total_time = (now_ns() - t0) / 1e9;
    fprintf(stderr, "\n  Done: %.1fs, %d samples\n", total_time, n_samples);

    /* Write CSV */
    FILE *fp = fopen(out_csv, "w");
    if (!fp) { perror("fopen"); return 1; }
    fprintf(fp, "sample_idx,ts_ns,phase,trial,set_id,%s_curr\n", ddr_label);
    for (int i = 0; i < n_samples; i++) {
        ina_sample_t *s = &samples[i];
        fprintf(fp, "%d,%lld,%d,%d,%d,%d\n",
                i, (long long)s->ts_ns, s->phase, s->trial, s->set_id, s->curr_mA);
    }
    fclose(fp);
    fprintf(stderr, "  Output: %s\n", out_csv);

    /* Cleanup */
    free(samples);
    free(sets);
    munmap(mem, mem_size);
    if (ddr_curr_fd >= 0) close(ddr_curr_fd);

    return 0;
}
