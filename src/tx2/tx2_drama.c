/*
 * tx2_drama.c — DRAMA-style DRAM bank mapping on Jetson TX2
 *
 * Re-implementation of Pessl et al. "DRAMA: Exploiting DRAM Addressing for
 * Cross-CPU Attacks" (USENIX Security 2016) for the NVIDIA Jetson TX2
 * (Tegra186, Cortex-A57, LPDDR4).  Upstream reference:
 *   https://github.com/isec-tugraz/drama   (tools/measure.c)
 *
 * Methodology (identical to upstream, re-targeted to ARM64):
 *   - Allocate one large buffer; resolve PA via /proc/self/pagemap.
 *   - For each pair (A, B): flush both, then time  load(A); load(B).
 *   - Row conflicts (same channel+bank, different row) take measurably
 *     longer than non-conflicts.
 *   - Build a histogram of per-pair median latencies → threshold via
 *     bimodal (Otsu) split → classify each pair.
 *
 * Timer:   ARM generic counter (CNTVCT_EL0) at 31.25 MHz on Tegra186.
 *          32 ns / tick is coarser than rdtsc but fine enough when we
 *          batch N accesses per measurement and take the median.
 *
 * Harness: exactly matches tx2_dram_mapper.c / tx2_knockknock_full.c so
 *          the two side-channels (timing vs. INA3221 power) can be
 *          compared pair-for-pair with the existing analysis scripts.
 *
 * Three phases, all written into results/drama_*.csv :
 *   Phase 1 — Calibration: 300 random pairs → histogram → Otsu threshold.
 *   Phase 2 — Knock-knock: flip each PA bit (6..33) → classify flip as
 *             same-bank (ROW/COL) or different-bank (BANK).
 *   Phase 3 — Conflict set: 3000 random pairs, timing + classification,
 *             ready for GF(2) null-space bank-function extraction.
 *
 * Build: make all
 * Run:   sudo ./build/tx2_drama -o results/drama
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

/* ── Defaults (matched to tx2_dram_mapper.c for fair comparison) ── */
#define DEFAULT_MEM_MB       512
#define DEFAULT_ROUNDS       10000   /* timing samples per pair */
#define DEFAULT_BATCH        1       /* loads per timed interval */
#define DEFAULT_CALIB_PAIRS  300
#define DEFAULT_PHASE3_PAIRS 3000
#define KNOCKKNOCK_REPS      5
#define BIT_LO               6
#define BIT_HI               33
#define PAGE_SIZE            4096
#define HUGEPAGE_SIZE        (2 * 1024 * 1024)
#define CL_SIZE              64

/* ── ARM64 intrinsics ── */
static inline void dc_civac(volatile void *p)
{ asm volatile("dc civac, %0" : : "r"(p) : "memory"); }
static inline void dsb_ish(void) { asm volatile("dsb ish" ::: "memory"); }
static inline void dsb_sy(void)  { asm volatile("dsb sy"  ::: "memory"); }
static inline void isb(void)     { asm volatile("isb"     ::: "memory"); }

static inline uint64_t cntvct(void)
{
    uint64_t t;
    asm volatile("isb; mrs %0, cntvct_el0" : "=r"(t));
    return t;
}

static inline uint64_t cntfrq(void)
{
    uint64_t f;
    asm volatile("mrs %0, cntfrq_el0" : "=r"(f));
    return f;
}

static inline int64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

/* ── Pagemap ── */
static int pagemap_fd = -1;

static uint64_t virt_to_phys(volatile void *va)
{
    uint64_t entry;
    uint64_t vaddr = (uint64_t)va;
    off_t offset = (vaddr / PAGE_SIZE) * 8;
    if (pread(pagemap_fd, &entry, 8, offset) != 8) return 0;
    if (!(entry & (1ULL << 63))) return 0;
    uint64_t pfn = entry & ((1ULL << 55) - 1);
    return (pfn * PAGE_SIZE) | (vaddr & (PAGE_SIZE - 1));
}

/* ── DRAMA core: time load(A); load(B) after flush ──
 *
 * Upstream x86 reference (drama/tools/measure.c):
 *     for each round:
 *         mfence; t0 = rdtsc;
 *         *a; *b;
 *         t1 = rdtsc; mfence;
 *         clflush(a); clflush(b);
 *     median of rounds → pair latency
 *
 * ARM64 port: dc civac + dsb ish + isb + cntvct_el0.
 * We return the MEDIAN latency in timer ticks (not ns) because the
 * threshold is derived from the distribution itself — units cancel.
 */
static volatile uint64_t sink;

static int cmp_u64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}

static double time_pair(volatile char *a, volatile char *b,
                         int rounds, int batch,
                         uint64_t *samples_buf)
{
    /* Warm both lines into the walker / TLB once. */
    sink = *(volatile uint64_t *)a;
    sink = *(volatile uint64_t *)b;
    dsb_ish();

    for (int r = 0; r < rounds; r++) {
        /* Evict both lines everywhere. */
        dc_civac(a);
        dc_civac(b);
        dsb_ish();
        isb();

        uint64_t t0 = cntvct();
        for (int k = 0; k < batch; k++) {
            sink = *(volatile uint64_t *)a;
            sink = *(volatile uint64_t *)b;
        }
        uint64_t t1 = cntvct();
        samples_buf[r] = t1 - t0;
    }

    qsort(samples_buf, rounds, sizeof(uint64_t), cmp_u64);
    /* Median is robust to outliers (context switches, IRQs). */
    return (double)samples_buf[rounds / 2];
}

/* ── Page table ── */
typedef struct { volatile char *va; uint64_t pa; } page_entry_t;
static page_entry_t *pages = NULL;
static int            n_pages = 0;

static int cmp_pa(const void *a, const void *b)
{
    uint64_t pa = ((const page_entry_t *)a)->pa;
    uint64_t pb = ((const page_entry_t *)b)->pa;
    return (pa > pb) - (pa < pb);
}

static void build_page_table(char *mem, size_t mem_size, size_t pg_size)
{
    int max_pages = (int)(mem_size / pg_size) + 1;
    pages = malloc(max_pages * sizeof(page_entry_t));
    n_pages = 0;
    for (size_t off = 0; off < mem_size; off += pg_size) {
        volatile char *va = (volatile char *)(mem + off);
        uint64_t pa = virt_to_phys(va);
        if (pa == 0) continue;
        pages[n_pages].va = va;
        pages[n_pages].pa = pa;
        n_pages++;
    }
    qsort(pages, n_pages, sizeof(page_entry_t), cmp_pa);
    fprintf(stderr, "  Page table: %d entries (%.1f MB), sorted by PA\n",
            n_pages, (double)n_pages * pg_size / (1 << 20));
}

static volatile char *find_va_for_pa(uint64_t target_pa)
{
    uint64_t target_page = target_pa & ~(uint64_t)(PAGE_SIZE - 1);
    int lo = 0, hi = n_pages - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        uint64_t mid_page = pages[mid].pa & ~(uint64_t)(PAGE_SIZE - 1);
        if (mid_page == target_page) {
            return pages[mid].va + (target_pa & (PAGE_SIZE - 1));
        } else if (mid_page < target_page) lo = mid + 1;
        else hi = mid - 1;
    }
    return NULL;
}

/* ── Otsu split (copied from tx2_dram_mapper.c so the two tools score
 *    their calibration identically) ── */
static double otsu_threshold(double *vals, int n)
{
    for (int i = 0; i < n - 1; i++)
        for (int j = i + 1; j < n; j++)
            if (vals[j] < vals[i]) { double t = vals[i]; vals[i] = vals[j]; vals[j] = t; }

    double best_t = vals[n / 2], best_var = -1.0;
    for (int k = 3; k < n - 3; k++) {
        double t = vals[k];
        double sum_lo = 0, sum_hi = 0;
        int n_lo = k + 1, n_hi = n - k - 1;
        for (int i = 0; i <= k; i++) sum_lo += vals[i];
        for (int i = k + 1; i < n; i++) sum_hi += vals[i];
        double mu_lo = sum_lo / n_lo, mu_hi = sum_hi / n_hi;
        double var = ((double)n_lo / n) * ((double)n_hi / n)
                     * (mu_lo - mu_hi) * (mu_lo - mu_hi);
        if (var > best_var) { best_var = var; best_t = t; }
    }
    return best_t;
}

/* ════════════════════════════════════════════════════════════════════════
 *  Phase 1 — Calibration
 * ════════════════════════════════════════════════════════════════════════ */

static double calibration_threshold = 0.0;  /* in timer ticks */

static void phase1_calibrate(char *mem, size_t mem_size, int n_pairs,
                              int rounds, int batch, const char *out_prefix)
{
    fprintf(stderr, "\n══════ Phase 1: Calibration (%d random pairs) ══════\n", n_pairs);

    size_t n_cls = mem_size / CL_SIZE;
    double *lat = malloc(n_pairs * sizeof(double));
    uint64_t *samples = malloc(rounds * sizeof(uint64_t));

    char csv_path[512];
    snprintf(csv_path, sizeof(csv_path), "%s_calibration.csv", out_prefix);
    FILE *fp = fopen(csv_path, "w");
    fprintf(fp, "pair_id,pa_a,pa_b,pa_xor,latency_ticks,latency_ns\n");

    uint64_t freq = cntfrq();
    double tick_ns = 1e9 / (double)freq;

    int64_t t0 = now_ns();
    for (int p = 0; p < n_pairs; p++) {
        if (p % 20 == 0) {
            fprintf(stderr, "\r  [%5.1f%%] pair %d/%d  elapsed=%.0fs",
                    100.0 * p / n_pairs, p, n_pairs, (now_ns() - t0) / 1e9);
            fflush(stderr);
        }
        volatile char *va_a, *va_b;
        uint64_t pa_a, pa_b;
        do {
            size_t cl = (size_t)rand() % n_cls;
            va_a = (volatile char *)(mem + cl * CL_SIZE);
            pa_a = virt_to_phys(va_a);
        } while (pa_a == 0);
        do {
            size_t cl = (size_t)rand() % n_cls;
            va_b = (volatile char *)(mem + cl * CL_SIZE);
            pa_b = virt_to_phys(va_b);
        } while (pa_b == 0 || pa_b == pa_a);

        double med = time_pair(va_a, va_b, rounds, batch, samples);
        lat[p] = med;
        fprintf(fp, "%d,0x%lx,0x%lx,0x%lx,%.1f,%.1f\n",
                p, pa_a, pa_b, pa_a ^ pa_b, med, med * tick_ns);
    }
    fclose(fp);
    fprintf(stderr, "\r  Phase 1 done in %.0fs                          \n",
            (now_ns() - t0) / 1e9);

    double *lat_copy = malloc(n_pairs * sizeof(double));
    memcpy(lat_copy, lat, n_pairs * sizeof(double));
    calibration_threshold = otsu_threshold(lat_copy, n_pairs);

    /* DRAMA semantics: HIGH latency == conflict. */
    int n_lo = 0, n_hi = 0;
    double s_lo = 0, s_hi = 0;
    for (int i = 0; i < n_pairs; i++) {
        if (lat[i] <= calibration_threshold) { n_lo++; s_lo += lat[i]; }
        else                                 { n_hi++; s_hi += lat[i]; }
    }
    double mu_lo = n_lo ? s_lo / n_lo : 0;
    double mu_hi = n_hi ? s_hi / n_hi : 0;
    double frac_hi = (double)n_hi / n_pairs;

    fprintf(stderr, "  ┌──────────────────────────────────────────────────────┐\n");
    fprintf(stderr, "  │  Otsu threshold:   %.1f ticks  (%.1f ns)           \n",
            calibration_threshold, calibration_threshold * tick_ns);
    fprintf(stderr, "  │  Low  (no-conf.):  n=%d  mean=%.1f  (%.1f%%)        \n",
            n_lo, mu_lo, (1 - frac_hi) * 100);
    fprintf(stderr, "  │  High (conflict):  n=%d  mean=%.1f  (%.1f%%)        \n",
            n_hi, mu_hi, frac_hi * 100);
    fprintf(stderr, "  │  Δ = %.1f ticks (%.1f ns),  theoretical 1/16 = 6.25%%\n",
            mu_hi - mu_lo, (mu_hi - mu_lo) * tick_ns);
    fprintf(stderr, "  └──────────────────────────────────────────────────────┘\n");
    fprintf(stderr, "  Output: %s\n", csv_path);

    free(samples); free(lat); free(lat_copy);
}

/* ════════════════════════════════════════════════════════════════════════
 *  Phase 2 — Knock-knock
 * ════════════════════════════════════════════════════════════════════════ */

static void phase2_knockknock(char *mem, size_t mem_size,
                               int rounds, int batch, const char *out_prefix)
{
    fprintf(stderr, "\n══════ Phase 2: Knock-knock (bit %d–%d) ══════\n", BIT_LO, BIT_HI);
    if (calibration_threshold <= 0) {
        fprintf(stderr, "  ERROR: run Phase 1 first\n"); return;
    }

    volatile char *base_va = (volatile char *)mem;
    uint64_t base_pa = virt_to_phys(base_va);
    while (base_pa == 0 && base_va < (volatile char *)(mem + mem_size)) {
        base_va += CL_SIZE; base_pa = virt_to_phys(base_va);
    }
    if (base_pa == 0) { fprintf(stderr, "  ERROR: base PA unresolved\n"); return; }
    fprintf(stderr, "  Base VA=%p  PA=0x%lx\n", (void *)base_va, base_pa);

    char csv_path[512];
    snprintf(csv_path, sizeof(csv_path), "%s_knockknock.csv", out_prefix);
    FILE *fp = fopen(csv_path, "w");
    fprintf(fp, "bit,base_pa,target_pa,pa_xor,latency_ticks,is_conflict,rep\n");

    uint64_t *samples = malloc(rounds * sizeof(uint64_t));

    fprintf(stderr, "  %-4s  %-18s %-18s  %-8s  %-8s  verdict\n",
            "bit", "base_pa", "target_pa", "lat", "conflict");
    fprintf(stderr, "  ──── ────────────────── ──────────────────  ────────  ────────  ─────────────\n");

    for (int b = BIT_LO; b <= BIT_HI; b++) {
        uint64_t target_pa = base_pa ^ (1ULL << b);
        volatile char *target_va = find_va_for_pa(target_pa);
        if (!target_va && b < 12) {
            uint64_t off = (uint64_t)(base_va - (volatile char *)mem);
            uint64_t toff = off ^ (1ULL << b);
            if (toff < mem_size) {
                target_va = (volatile char *)(mem + toff);
                if (virt_to_phys(target_va) != target_pa) target_va = NULL;
            }
        }
        if (!target_va) {
            fprintf(stderr, "  b%-3d  --  no VA for PA 0x%lx (skipped)\n", b, target_pa);
            fprintf(fp, "%d,0x%lx,0x%lx,0x%lx,0,0,-1\n",
                    b, (unsigned long)base_pa, (unsigned long)target_pa,
                    (unsigned long)(1ULL << b));
            continue;
        }

        int n_conflict = 0;
        double sum_lat = 0;
        for (int r = 0; r < KNOCKKNOCK_REPS; r++) {
            double lat = time_pair(base_va, target_va, rounds, batch, samples);
            int is_conf = (lat > calibration_threshold) ? 1 : 0;
            n_conflict += is_conf; sum_lat += lat;
            fprintf(fp, "%d,0x%lx,0x%lx,0x%lx,%.1f,%d,%d\n",
                    b, (unsigned long)base_pa, (unsigned long)target_pa,
                    (unsigned long)(1ULL << b), lat, is_conf, r);
        }
        double avg = sum_lat / KNOCKKNOCK_REPS;
        int majority = (n_conflict > KNOCKKNOCK_REPS / 2);
        /* Flipping a BANK bit moves B into a DIFFERENT bank → no conflict.
         * Flipping a ROW/COL bit keeps B in the SAME bank → conflict.
         * (Opposite current-sign from the power channel, same topology.) */
        const char *verdict = majority ? "ROW/COL  (same bank)"
                                       : "BANK     (diff bank)";
        fprintf(stderr, "  b%-3d  0x%016lx 0x%016lx  %7.0f   %d/%d       %s\n",
                b, base_pa, target_pa, avg, n_conflict, KNOCKKNOCK_REPS, verdict);
    }

    free(samples);
    fclose(fp);
    fprintf(stderr, "  Output: %s\n", csv_path);
}

/* ════════════════════════════════════════════════════════════════════════
 *  Phase 3 — Conflict set
 * ════════════════════════════════════════════════════════════════════════ */

static void phase3_conflictset(char *mem, size_t mem_size, int n_pairs,
                                int rounds, int batch, const char *out_prefix)
{
    fprintf(stderr, "\n══════ Phase 3: Conflict set (%d random pairs) ══════\n", n_pairs);
    if (calibration_threshold <= 0) {
        fprintf(stderr, "  ERROR: run Phase 1 first\n"); return;
    }

    size_t n_cls = mem_size / CL_SIZE;
    uint64_t *samples = malloc(rounds * sizeof(uint64_t));

    char csv_path[512];
    snprintf(csv_path, sizeof(csv_path), "%s_conflictset.csv", out_prefix);
    FILE *fp = fopen(csv_path, "w");
    fprintf(fp, "pair_id,pa_a,pa_b,pa_xor,latency_ticks,is_conflict\n");

    int n_conflicts = 0;
    int64_t t0 = now_ns();

    for (int p = 0; p < n_pairs; p++) {
        if (p % 50 == 0) {
            fprintf(stderr, "\r  [%5.1f%%] pair %d/%d  conflicts=%d  elapsed=%.0fs",
                    100.0 * p / n_pairs, p, n_pairs, n_conflicts,
                    (now_ns() - t0) / 1e9);
            fflush(stderr);
        }
        volatile char *va_a, *va_b;
        uint64_t pa_a, pa_b;
        do {
            size_t cl = (size_t)rand() % n_cls;
            va_a = (volatile char *)(mem + cl * CL_SIZE);
            pa_a = virt_to_phys(va_a);
        } while (pa_a == 0);
        do {
            size_t cl = (size_t)rand() % n_cls;
            va_b = (volatile char *)(mem + cl * CL_SIZE);
            pa_b = virt_to_phys(va_b);
        } while (pa_b == 0 || pa_b == pa_a);

        double lat = time_pair(va_a, va_b, rounds, batch, samples);
        int is_conf = (lat > calibration_threshold) ? 1 : 0;
        n_conflicts += is_conf;

        fprintf(fp, "%d,0x%lx,0x%lx,0x%lx,%.1f,%d\n",
                p, pa_a, pa_b, pa_a ^ pa_b, lat, is_conf);
    }
    fclose(fp);
    free(samples);

    double elapsed = (now_ns() - t0) / 1e9;
    double frac = (double)n_conflicts / n_pairs;
    fprintf(stderr, "\r  Phase 3 done in %.0fs                                        \n", elapsed);
    fprintf(stderr, "  ┌──────────────────────────────────────────────────┐\n");
    fprintf(stderr, "  │  Total pairs:    %d                               \n", n_pairs);
    fprintf(stderr, "  │  Conflicts:      %d  (%.1f%%)                     \n", n_conflicts, frac * 100);
    fprintf(stderr, "  │  Theory (1/16):  %.2f%%                           \n", 100.0 / 16);
    fprintf(stderr, "  │  Δ from theory:  %.2f pp                          \n", (frac - 1.0/16) * 100);
    fprintf(stderr, "  └──────────────────────────────────────────────────┘\n");
    fprintf(stderr, "  Output: %s\n", csv_path);
}

/* ════════════════════════════════════════════════════════════════════════
 *  Main
 * ════════════════════════════════════════════════════════════════════════ */

int main(int argc, char **argv)
{
    int mem_mb       = DEFAULT_MEM_MB;
    int rounds       = DEFAULT_ROUNDS;
    int batch        = DEFAULT_BATCH;
    int calib_pairs  = DEFAULT_CALIB_PAIRS;
    int phase3_pairs = DEFAULT_PHASE3_PAIRS;
    int run_phase1 = 1, run_phase2 = 1, run_phase3 = 1;
    int warmup_secs = 0;
    const char *out_prefix = "results/drama";

    for (int i = 1; i < argc; i++) {
        if      (!strcmp(argv[i], "-R") && i+1 < argc) rounds       = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-B") && i+1 < argc) batch        = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-c") && i+1 < argc) calib_pairs  = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-p") && i+1 < argc) phase3_pairs = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-M") && i+1 < argc) mem_mb       = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-w") && i+1 < argc) warmup_secs  = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-o") && i+1 < argc) out_prefix   = argv[++i];
        else if (!strcmp(argv[i], "-1")) { run_phase1=1; run_phase2=0; run_phase3=0; }
        else if (!strcmp(argv[i], "-2")) { run_phase1=1; run_phase2=1; run_phase3=0; }
        else if (!strcmp(argv[i], "-3")) { run_phase1=1; run_phase2=0; run_phase3=1; }
        else if (!strcmp(argv[i], "-h")) {
            fprintf(stderr,
                "Usage: sudo %s [options]\n"
                "  -R N    Timing rounds per pair (median of N)   (default %d)\n"
                "  -B N    Loads per timed interval (batch)       (default %d)\n"
                "  -c N    Phase 1 calibration pairs              (default %d)\n"
                "  -p N    Phase 3 conflict-set pairs             (default %d)\n"
                "  -M N    Memory buffer in MB                    (default %d)\n"
                "  -w N    Warmup seconds of hammering before P1  (default 0)\n"
                "  -o F    Output CSV prefix (default results/drama)\n"
                "  -1      Phase 1 only\n"
                "  -2      Phases 1+2\n"
                "  -3      Phases 1+3\n",
                argv[0], DEFAULT_ROUNDS, DEFAULT_BATCH,
                DEFAULT_CALIB_PAIRS, DEFAULT_PHASE3_PAIRS, DEFAULT_MEM_MB);
            return 0;
        }
    }

    fprintf(stderr, "════════════════════════════════════════════════════\n");
    fprintf(stderr, " DRAMA (timing side-channel) — Jetson TX2 port\n");
    fprintf(stderr, " Ref: Pessl et al., USENIX Security 2016\n");
    fprintf(stderr, "      github.com/isec-tugraz/drama\n");
    fprintf(stderr, " Rounds/pair: %d   Batch: %d   Mem: %d MB\n",
            rounds, batch, mem_mb);
    fprintf(stderr, "════════════════════════════════════════════════════\n");

    if (geteuid() != 0) {
        fprintf(stderr, "ERROR: must run as root (need /proc/self/pagemap)\n");
        return 1;
    }

    pagemap_fd = open("/proc/self/pagemap", O_RDONLY);
    if (pagemap_fd < 0) { perror("open pagemap"); return 1; }

    cpu_set_t cpuset; CPU_ZERO(&cpuset); CPU_SET(0, &cpuset);
    sched_setaffinity(0, sizeof(cpuset), &cpuset);
    fprintf(stderr, "  Pinned to core 0 (Cortex-A57)\n");
    mlockall(MCL_CURRENT | MCL_FUTURE);

    uint64_t freq = cntfrq();
    fprintf(stderr, "  Generic timer freq: %.3f MHz  (%.1f ns/tick)\n",
            freq / 1e6, 1e9 / (double)freq);

    size_t mem_size = (size_t)mem_mb << 20;
    size_t page_sz = PAGE_SIZE;

    char *mem = mmap(NULL, mem_size, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB | MAP_POPULATE,
                     -1, 0);
    if (mem != MAP_FAILED) {
        page_sz = HUGEPAGE_SIZE;
        fprintf(stderr, "  Allocated %d MB (2 MB hugepages)\n", mem_mb);
    } else {
        mem = mmap(NULL, mem_size, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
        if (mem == MAP_FAILED) { perror("mmap"); return 1; }
        fprintf(stderr, "  Allocated %d MB (4K pages, hugepages unavailable)\n", mem_mb);
    }

    for (size_t off = 0; off < mem_size; off += page_sz)
        mem[off] = (char)(off & 0xFF);

    fprintf(stderr, "  Building page table...\n");
    build_page_table(mem, mem_size, page_sz);
    if (n_pages < 10) {
        fprintf(stderr, "  ERROR: only %d pages resolved\n", n_pages); return 1;
    }
    fprintf(stderr, "  Sample PAs:  [0]=0x%lx  [%d]=0x%lx  [%d]=0x%lx\n",
            pages[0].pa, n_pages/2, pages[n_pages/2].pa,
            n_pages-1, pages[n_pages-1].pa);

    /* Thermal warmup — parity with JestINA tools so the two side-channels
     * measure the SoC in the same steady state. */
    if (warmup_secs > 0) {
        fprintf(stderr, "  Warmup: %d s of continuous hammering...\n", warmup_secs);
        volatile char *wa = pages[0].va;
        volatile char *wb = pages[n_pages > 1 ? 1 : 0].va;
        int64_t t_start = now_ns();
        int64_t t_end   = t_start + (int64_t)warmup_secs * 1000000000LL;
        while (now_ns() < t_end) {
            for (int i = 0; i < 100000; i++) {
                dc_civac(wa); dc_civac(wb); dsb_ish();
                sink = *(volatile uint64_t *)wa;
                sink = *(volatile uint64_t *)wb;
            }
        }
    }

    srand((unsigned)time(NULL));
    int64_t total_start = now_ns();

    if (run_phase1) phase1_calibrate(mem, mem_size, calib_pairs, rounds, batch, out_prefix);
    if (run_phase2) phase2_knockknock(mem, mem_size, rounds, batch, out_prefix);
    if (run_phase3) phase3_conflictset(mem, mem_size, phase3_pairs, rounds, batch, out_prefix);

    double total_time = (now_ns() - total_start) / 1e9;
    fprintf(stderr, "\n════════════════════════════════════════════════════\n");
    fprintf(stderr, " DRAMA port complete. Total: %.0f s (%.1f min)\n",
            total_time, total_time / 60);
    fprintf(stderr, " Output prefix: %s\n", out_prefix);
    fprintf(stderr, "════════════════════════════════════════════════════\n");

    free(pages);
    munmap(mem, mem_size);
    close(pagemap_fd);
    return 0;
}
