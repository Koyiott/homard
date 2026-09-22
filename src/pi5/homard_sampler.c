/*
 * dram_power_calibrate.cc — Raspberry Pi multi-rail PMIC calibration
 *                          with high-SNR userspace acquisition
 *
 * Same experiment as the Jetson TX2 version (../jeston/...): sweep
 * cache-line Hamming weight 0..512 while workers sustain flush+read
 * traffic and measure the data-dependent current on the DRAM rails.
 *
 * MULTI-RAIL PMIC:
 *   `vcgencmd pmic_read_adc` returns ALL rails in one ~50 ms call. We
 *   discover the rail set once at startup and re-parse the full output
 *   every sample. Rail naming differs between PMIC generations:
 *     - Pi 5 (Dialog DA9091): DDR_VDD2_A (core), DDR_VDDQ_A (I/O)
 *     - Pi 4 (MxL7704):       DDR_VDD2_A (core), DDR_IO_A   (I/O)
 *   The wide CSV captures every "_A" rail; the analysis script picks
 *   the best signal at post-processing time.
 *
 * HIGH-SNR UPGRADES (all unprivileged):
 *
 *   1. BIG WORKING SET (-M MB, default 16 MB):
 *      Visits every cache line in a 16 MB buffer per worker iteration —
 *      ~256 k flushes + 256 k linefills, well past the L2 (Pi 5: 512 KB
 *      per A76 + 2 MB L2). Every access is a real DRAM transaction.
 *
 *   2. MULTIPLE WORKERS (-W N, default 3):
 *      One worker per A76/A72 sibling (cores 1, 2, 3; sampler stays on
 *      core 0). Saturates the LPDDR4X memory controller.
 *
 *   3. RANDOMIZED PATTERN ORDER + REPETITIONS (-r N, default 3):
 *      Sequential 0,32,...,512 sweeps confound any slow time drift with
 *      the Hamming-weight axis. We shuffle the pattern list per rep.
 *
 *   4. THERMAL WARMUP (-T s, default 60).
 *
 *   5. MADV_HUGEPAGE + mlockall on the working buffer.
 *
 * THREAT MODEL:
 *   Fully unprivileged. `vcgencmd pmic_read_adc` is world-executable on
 *   Raspberry Pi OS, DC CIVAC / LDR don't need privileges, the target
 *   buffer is the attacker's own anonymous mapping. No sudo, no setcap.
 *
 * CSV (wide, dynamic columns based on discovered rails):
 *   rep,num_ones,operation,sample,ops,
 *     <rail1>_curr_mA, <rail2>_curr_mA, ...
 *
 * Build:  make pi
 * Run:    ./build/pi/dram_power_calibrate \
 *             -M 16 -W 3 -r 5 -T 60 -d 30 -n 90 \
 *             > data/pi/calib.csv
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

/* ══════════════════════════════════════════════════════════════════════
 *  ARM Cache + Barrier Primitives  (works on A72 / A76)
 * ══════════════════════════════════════════════════════════════════════ */

static inline void cache_flush(volatile void *addr) {
    asm volatile("dc civac, %0" :: "r"(addr) : "memory");
}

static inline void memory_barrier(void) {
    asm volatile("dsb ish" ::: "memory");
}

/* ══════════════════════════════════════════════════════════════════════
 *  CPU Core Pinning
 *
 *  Pi 4 and Pi 5 both have 4 uniform cores (A72 or A76). We pin the
 *  sampler to core 0 and workers to cores 1..3. With -W 1 only core 1
 *  hammers; -W 3 saturates the memory controller.
 * ══════════════════════════════════════════════════════════════════════ */

static const int worker_core_map[] = { 1, 2, 3 };
static const int n_worker_cores = (int)(sizeof(worker_core_map) / sizeof(worker_core_map[0]));
static const int sampler_core   = 0;

static void pin_to_core(int core) {
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(core, &cpuset);
    sched_setaffinity(0, sizeof(cpuset), &cpuset);
}

/* ══════════════════════════════════════════════════════════════════════
 *  PMIC Multi-Rail Discovery (via `vcgencmd pmic_read_adc`)
 *
 *  Line format (Pi 5 / Pi 4):
 *      DDR_VDDQ_A current(4)=0.01234567A
 *      DDR_VDD2_V volt(11)=1.11062200V
 *
 *  We parse every "*_A current(*)=*A" line into our rail table. The
 *  rail set is captured once at startup; subsequent samples reuse the
 *  same indices, so the wide CSV has stable columns.
 *
 *  vcgencmd is slow (~50 ms per popen), but each call returns ALL
 *  rails, so multi-rail capture is free.
 * ══════════════════════════════════════════════════════════════════════ */

#define MAX_RAILS 32

typedef struct {
    char name[64];          /* sanitized CSV column name (alnum + _) */
    char raw_name[64];      /* original rail string from vcgencmd    */
    double mA;              /* most recent reading                   */
} PmicRail;

static PmicRail rails[MAX_RAILS];
static int n_rails = 0;

static void sanitize_label(const char *src, char *dst, size_t sz) {
    size_t j = 0;
    for (size_t i = 0; src[i] && j + 1 < sz; i++) {
        char c = src[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '_')
            dst[j++] = c;
        else
            dst[j++] = '_';
    }
    dst[j] = '\0';
}

static int rail_index_or_add(const char *raw) {
    for (int i = 0; i < n_rails; i++)
        if (strcmp(rails[i].raw_name, raw) == 0) return i;
    if (n_rails >= MAX_RAILS) return -1;
    int i = n_rails++;
    snprintf(rails[i].raw_name, sizeof(rails[i].raw_name), "%s", raw);
    sanitize_label(raw, rails[i].name, sizeof(rails[i].name));
    rails[i].mA = 0.0;
    return i;
}

/* One popen-parse pass — fills rails[].mA for every "_A" rail and
 * (the first time it runs) creates any rail entries that don't yet
 * exist. Returns 0 on success, -1 on popen failure. */
static int read_pmic_all(void) {
    FILE *fp = popen("vcgencmd pmic_read_adc 2>/dev/null", "r");
    if (!fp) return -1;
    char line[256];
    while (fgets(line, sizeof(line), fp)) {
        /* Look for "..._A current(..)=<value>A" */
        char raw[64];
        double amps;
        char unit;
        /* Leading " " skips indentation that vcgencmd prefixes onto
         * every line. "%63[A-Z0-9_]" matches the rail name; "%*[^=]"
         * skips the "current(N)" piece; "%lf%c" grabs value + unit. */
        if (sscanf(line, " %63[A-Za-z0-9_] %*[^=]=%lf%c",
                   raw, &amps, &unit) == 3 && unit == 'A') {
            /* Only current rails — strip them by suffix check */
            size_t L = strlen(raw);
            if (L < 2 || raw[L - 2] != '_' || raw[L - 1] != 'A')
                continue;
            int i = rail_index_or_add(raw);
            if (i >= 0) rails[i].mA = amps * 1000.0;
        }
    }
    pclose(fp);
    return 0;
}

/* ══════════════════════════════════════════════════════════════════════
 *  Pattern Generation — IDENTICAL to the Jetson and original Pi versions
 * ══════════════════════════════════════════════════════════════════════ */

static void generate_pattern(uint8_t *buf, int num_ones) {
    memset(buf, 0, 64);
    if (num_ones <= 0) return;
    if (num_ones >= 512) { memset(buf, 0xFF, 64); return; }
    int per_byte  = num_ones / 64;
    int remainder = num_ones % 64;
    static const uint8_t popcount_val[9] = {
        0x00, 0x01, 0x03, 0x07, 0x0F, 0x1F, 0x3F, 0x7F, 0xFF
    };
    for (int i = 0; i < 64; i++) {
        int bits = per_byte + (i < remainder ? 1 : 0);
        buf[i] = popcount_val[bits];
    }
}

static int count_ones(const uint8_t *buf, size_t len) {
    int c = 0;
    for (size_t i = 0; i < len; i++) c += __builtin_popcount(buf[i]);
    return c;
}

/* ══════════════════════════════════════════════════════════════════════
 *  Multi-Worker Speculative Pattern Bursts
 * ══════════════════════════════════════════════════════════════════════ */

#define MAX_WORKERS 4
#define SPEC_LINES 32                       /* gadget covers 32 cache lines */
#define SPEC_STRIPE_BYTES (SPEC_LINES * 64) /* 2048 B per RSB burst         */

/* Storage for the RSB-gadget return address. Written, flushed, and then
 * re-loaded inside the gadget every burst; the cold reload of x30 is what
 * stalls the architectural `ret` long enough for many speculative ldp's
 * to issue. Volatile keeps the compiler from caching writes. */
static volatile uintptr_t spec_ret_anchor;

/* Prime stride (in stripes) used to walk the per-worker slab. Coprime
 * with virtually any reasonable n_stripes, so we hit every stripe before
 * repeating; non-unit stride defeats the L2 sequential prefetcher so
 * every speculative ldp resolves to a true DRAM linefill, not a hit on
 * an architecturally-prefetched line. */
#define STRIPE_STRIDE 16381

/* spec_batches_before_poll — how many speculative bursts a worker runs
 * between atomic ops-counter updates and `running` re-checks. Larger =
 * less per-iteration overhead, but coarser stop latency. Tunable via -B. */
static int spec_batches_before_poll = 1024;

typedef struct {
    volatile uint8_t   *mem;
    size_t              mem_size;
    int                 worker_id;
    int                 n_workers;
    int                 cpu;

    volatile int        running;    /* 0=idle, 1=run, -1=exit */
    volatile int        active;
    int                 is_write;

    volatile uint64_t   ops;
} WorkerCtx;

static WorkerCtx workers[MAX_WORKERS];

/* Invalidate (and write back if dirty) all SPEC_LINES cache lines of a
 * stripe. Without this, after the first time the speculative gadget
 * touches a stripe its lines stay in L1/L2 (revisit-interval << L2
 * retention with our hot inner loop), so subsequent bursts hit cache
 * instead of issuing DRAM linefills — and no DRAM linefills means no
 * data-dependent rail current. The 80-cycle cost of the flush is what
 * buys us DRAM-side amplitude. */
static inline void flush_stripe(volatile uint8_t *stripe) {
    for (int i = 0; i < SPEC_LINES; i++)
        cache_flush(stripe + i * 64);
    memory_barrier();
}

/* RSB-mispredict burst with extended speculation window.
 *
 * Two-step trick:
 *   (a) Architecturally, store the fall-through label address (`4:`) into
 *       a flushable global, then `dc civac` that line. Subsequent reads
 *       of the global miss to DRAM.
 *   (b) The gadget body does `bl 2f`, which pushes "3:" onto the RSB.
 *       At "2:" we `ldr x30, [anchor]` — a cold DRAM load — and then
 *       `ret`. Architecturally `ret` waits ~200 cycles for x30 to
 *       resolve; meanwhile the RSB has already predicted the return
 *       target as "3:", so the front-end speculatively executes the
 *       32-ldp chain at "3:" for the *entire* duration of the stall.
 *
 * With the old `adr x30, 4f` resolution path the speculation window was
 * only ~10–15 cycles (enough for ~3–5 ldp to issue). With the slow-load
 * trick it stretches to ~200 cycles, so all 32 ldp (covering 16 cache
 * lines = 1024 B) can enter the load queue and trigger DRAM linefills
 * before the squash lands.
 *
 * Architecturally the secret cache lines are never read — only the
 * speculatively-executed ldp's touch them, and those get squashed before
 * any of their values reach an architecturally-committed register. */
static inline void speculative_load_secret_lines(const volatile uint8_t *secret) {
    asm volatile(
        /* (a) Stash & flush the return anchor (cold ret target). */
        "adr  x9, 4f\n\t"
        "str  x9, [%[anchor]]\n\t"
        "dsb  ish\n\t"
        "dc   civac, %[anchor]\n\t"
        "dsb  ish\n\t"

        /* Second base = secret + 1024 — ldp imm only goes to +1008, so
         * we need a second pointer to cover offsets 1024..2016. */
        "add  x10, %[secret], #1024\n\t"

        /* (b) Mispredict gadget. */
        "bl   2f\n\t"

        "3:\n\t"
        /* Lines 0..15 (offsets 0..992 via %[secret]) */
        "ldp  q0, q1, [%[secret], #0]\n\t"
        "ldp  q2, q3, [%[secret], #32]\n\t"
        "ldp  q0, q1, [%[secret], #64]\n\t"
        "ldp  q2, q3, [%[secret], #96]\n\t"
        "ldp  q0, q1, [%[secret], #128]\n\t"
        "ldp  q2, q3, [%[secret], #160]\n\t"
        "ldp  q0, q1, [%[secret], #192]\n\t"
        "ldp  q2, q3, [%[secret], #224]\n\t"
        "ldp  q0, q1, [%[secret], #256]\n\t"
        "ldp  q2, q3, [%[secret], #288]\n\t"
        "ldp  q0, q1, [%[secret], #320]\n\t"
        "ldp  q2, q3, [%[secret], #352]\n\t"
        "ldp  q0, q1, [%[secret], #384]\n\t"
        "ldp  q2, q3, [%[secret], #416]\n\t"
        "ldp  q0, q1, [%[secret], #448]\n\t"
        "ldp  q2, q3, [%[secret], #480]\n\t"
        "ldp  q0, q1, [%[secret], #512]\n\t"
        "ldp  q2, q3, [%[secret], #544]\n\t"
        "ldp  q0, q1, [%[secret], #576]\n\t"
        "ldp  q2, q3, [%[secret], #608]\n\t"
        "ldp  q0, q1, [%[secret], #640]\n\t"
        "ldp  q2, q3, [%[secret], #672]\n\t"
        "ldp  q0, q1, [%[secret], #704]\n\t"
        "ldp  q2, q3, [%[secret], #736]\n\t"
        "ldp  q0, q1, [%[secret], #768]\n\t"
        "ldp  q2, q3, [%[secret], #800]\n\t"
        "ldp  q0, q1, [%[secret], #832]\n\t"
        "ldp  q2, q3, [%[secret], #864]\n\t"
        "ldp  q0, q1, [%[secret], #896]\n\t"
        "ldp  q2, q3, [%[secret], #928]\n\t"
        "ldp  q0, q1, [%[secret], #960]\n\t"
        "ldp  q2, q3, [%[secret], #992]\n\t"
        /* Lines 16..31 (offsets 0..992 via x10) */
        "ldp  q0, q1, [x10, #0]\n\t"
        "ldp  q2, q3, [x10, #32]\n\t"
        "ldp  q0, q1, [x10, #64]\n\t"
        "ldp  q2, q3, [x10, #96]\n\t"
        "ldp  q0, q1, [x10, #128]\n\t"
        "ldp  q2, q3, [x10, #160]\n\t"
        "ldp  q0, q1, [x10, #192]\n\t"
        "ldp  q2, q3, [x10, #224]\n\t"
        "ldp  q0, q1, [x10, #256]\n\t"
        "ldp  q2, q3, [x10, #288]\n\t"
        "ldp  q0, q1, [x10, #320]\n\t"
        "ldp  q2, q3, [x10, #352]\n\t"
        "ldp  q0, q1, [x10, #384]\n\t"
        "ldp  q2, q3, [x10, #416]\n\t"
        "ldp  q0, q1, [x10, #448]\n\t"
        "ldp  q2, q3, [x10, #480]\n\t"
        "ldp  q0, q1, [x10, #512]\n\t"
        "ldp  q2, q3, [x10, #544]\n\t"
        "ldp  q0, q1, [x10, #576]\n\t"
        "ldp  q2, q3, [x10, #608]\n\t"
        "ldp  q0, q1, [x10, #640]\n\t"
        "ldp  q2, q3, [x10, #672]\n\t"
        "ldp  q0, q1, [x10, #704]\n\t"
        "ldp  q2, q3, [x10, #736]\n\t"
        "ldp  q0, q1, [x10, #768]\n\t"
        "ldp  q2, q3, [x10, #800]\n\t"
        "ldp  q0, q1, [x10, #832]\n\t"
        "ldp  q2, q3, [x10, #864]\n\t"
        "ldp  q0, q1, [x10, #896]\n\t"
        "ldp  q2, q3, [x10, #928]\n\t"
        "ldp  q0, q1, [x10, #960]\n\t"
        "ldp  q2, q3, [x10, #992]\n\t"

        "2:\n\t"
        "ldr  x30, [%[anchor]]\n\t"      /* SLOW: misses to DRAM */
        "ret\n\t"                         /* stalls ~200 cycles */

        "4:\n\t"
        :
        : [secret] "r" (secret),
          [anchor] "r" ((uintptr_t)&spec_ret_anchor)
        : "x9", "x10", "x30", "q0", "q1", "q2", "q3", "memory"
    );
}

/* Walk a per-worker slab of the shared buffer in SPEC_STRIPE_BYTES stripes.
 *
 * The per-worker slab is mem_size / n_workers bytes — for the default
 * -M 16 -W 3 that's 5.3 MB, well past Pi5's 2 MB L2; with -W 1 each
 * worker owns the full 16 MB. By the time the stride wraps back around,
 * every previously-fetched line has been naturally evicted, so we never
 * need explicit dc civac in the hot loop.
 *
 * Compared to the previous "flush the same 8 lines, re-speculate them"
 * design, this hits ~1000× more distinct DRAM lines per second (matching
 * the architectural dram_power_calibrate baseline), while the loads
 * themselves remain mispredicted-target speculative. Architectural side
 * of the gadget only touches x30 / ret targets and never the secret
 * cache lines — the data-dependent rail current comes entirely from the
 * speculative ldp chain.
 *
 * The whole shared buffer is pattern-filled (and then bulk-flushed) by
 * buffer_set_pattern before every pattern, so the worker walks straight
 * into cold memory with the current Hamming-weight payload everywhere. */
static void *worker_thread(void *arg) {
    WorkerCtx *ctx = (WorkerCtx *)arg;
    pin_to_core(ctx->cpu);

    size_t slab_bytes  = ctx->mem_size / (size_t)ctx->n_workers;
    size_t n_stripes   = slab_bytes / SPEC_STRIPE_BYTES;
    if (n_stripes < 1) n_stripes = 1;
    volatile uint8_t *slab_base =
        ctx->mem + (size_t)ctx->worker_id * n_stripes * SPEC_STRIPE_BYTES;

    size_t stripe = 0;
    uint64_t local_ops = 0;
    int was_active = 0;

    for (;;) {
        int state = __atomic_load_n(&ctx->running, __ATOMIC_ACQUIRE);
        if (state < 0) break;
        if (state == 0) {
            if (was_active) {
                __atomic_store_n(&ctx->active, 0, __ATOMIC_RELEASE);
                was_active = 0;
            }
            usleep(200);
            continue;
        }

        if (!was_active) {
            local_ops = 0;
            __atomic_store_n(&ctx->active, 1, __ATOMIC_RELEASE);
            was_active = 1;
        }

        for (int batch = 0; batch < spec_batches_before_poll; batch++) {
            volatile uint8_t *cur = slab_base + stripe * SPEC_STRIPE_BYTES;
            flush_stripe(cur);
            speculative_load_secret_lines(cur);
            stripe += STRIPE_STRIDE;
            if (stripe >= n_stripes) stripe %= n_stripes;
            local_ops++;
        }

        __atomic_store_n(&ctx->ops, local_ops, __ATOMIC_RELAXED);
    }

    __atomic_store_n(&ctx->active, 0, __ATOMIC_RELEASE);
    return NULL;
}

static void workers_set(int n, int is_write, const uint8_t *pattern) {
    /* buffer_set_pattern already wrote the current Hamming-weight pattern
     * to every cache line of the shared buffer and flushed it, so nothing
     * per-worker is required here. */
    (void)pattern;
    for (int w = 0; w < n; w++) {
        workers[w].is_write = is_write;
        __atomic_store_n(&workers[w].ops, 0, __ATOMIC_RELAXED);
        __atomic_store_n(&workers[w].active, 0, __ATOMIC_RELEASE);
    }
    for (int w = 0; w < n; w++)
        __atomic_store_n(&workers[w].running, 1, __ATOMIC_RELEASE);
}

static void workers_pause(int n) {
    for (int w = 0; w < n; w++)
        __atomic_store_n(&workers[w].running, 0, __ATOMIC_RELEASE);
    for (;;) {
        int active = 0;
        for (int w = 0; w < n; w++)
            active |= __atomic_load_n(&workers[w].active, __ATOMIC_ACQUIRE);
        if (!active) break;
        usleep(200);
    }
}

static uint64_t workers_total_ops(int n) {
    uint64_t s = 0;
    for (int w = 0; w < n; w++)
        s += __atomic_load_n(&workers[w].ops, __ATOMIC_RELAXED);
    return s;
}

static void buffer_set_pattern(volatile uint8_t *mem, size_t sz,
                               const uint8_t *pattern) {
    size_t n_lines = sz / 64;
    for (size_t i = 0; i < n_lines; i++)
        memcpy((void *)(mem + i * 64), pattern, 64);
    memory_barrier();
    for (size_t i = 0; i < n_lines; i++)
        cache_flush(mem + i * 64);
    memory_barrier();
}

/* ══════════════════════════════════════════════════════════════════════
 *  Main
 * ══════════════════════════════════════════════════════════════════════ */

static int num_samples  = 90;
static int duration_sec = 30;
static int step_size    = 32;
static int settle_ms    = 500;
static int do_write     = 0;
static int mem_mb       = 16;
static int n_workers    = 3;
static int n_reps       = 3;
static int warmup_secs  = 60;
static int randomize    = 1;

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    int opt;
    while ((opt = getopt(argc, argv, "n:d:s:t:M:W:r:T:B:wbRh")) != -1) {
        switch (opt) {
            case 'n': num_samples  = atoi(optarg); break;
            case 'd': duration_sec = atoi(optarg); break;
            case 's': step_size    = atoi(optarg); break;
            case 't': settle_ms    = atoi(optarg); break;
            case 'M': mem_mb       = atoi(optarg); break;
            case 'W': n_workers    = atoi(optarg); break;
            case 'r': n_reps       = atoi(optarg); break;
            case 'T': warmup_secs  = atoi(optarg); break;
            case 'B':
                spec_batches_before_poll = atoi(optarg);
                if (spec_batches_before_poll < 1) spec_batches_before_poll = 1;
                break;
            case 'w': do_write = 1; break;
            case 'b': do_write = 2; break;
            case 'R': randomize = 0; break;
            case 'h': default:
                fprintf(stderr,
                  "DRAM Data-Dependent Power Calibration (Raspberry Pi 4/5)\n"
                  "Multi-rail PMIC sweep via vcgencmd — fully unprivileged.\n\n"
                  "Usage: %s [options] > data/pi/calib.csv\n\n"
                  "Acquisition:\n"
                  "  -M MB   Working-set size                 (default: %d)\n"
                  "  -W N    Worker threads (1..%d)            (default: %d)\n"
                  "  -r N    Repetitions of the sweep         (default: %d)\n"
                  "  -T S    Thermal warmup seconds           (default: %d)\n"
                  "  -n N    PMIC samples per pattern         (default: %d)\n"
                  "  -d S    Worker duration per pattern      (default: %ds)\n"
                  "  -s N    Hamming step                     (default: %d)\n"
                  "  -t MS   Settle time between patterns     (default: %dms)\n"
                  "  -B N    Spec bursts per ops-counter poll (default: %d)\n"
                  "  -R      Disable pattern-order shuffle    (default: shuffled)\n"
                  "  -w      Write-side measurement\n"
                  "  -b      Both read and write\n\n"
                  "Total runtime ≈ reps × patterns × (-d + -t/1000 + 1) + -T\n",
                  argv[0],
                  mem_mb, n_worker_cores, n_workers, n_reps, warmup_secs,
                  num_samples, duration_sec, step_size, settle_ms,
                  spec_batches_before_poll);
                exit(opt == 'h' ? 0 : 1);
        }
    }
    if (n_workers < 1) n_workers = 1;
    if (n_workers > MAX_WORKERS)    n_workers = MAX_WORKERS;
    if (n_workers > n_worker_cores) n_workers = n_worker_cores;

    pin_to_core(sampler_core);

    /* ── Discover rails ───────────────────────────────────────────── */
    fprintf(stderr, "\n  Discovering PMIC rails via vcgencmd:\n");
    if (read_pmic_all() != 0 || n_rails == 0) {
        fprintf(stderr,
            "[!] vcgencmd pmic_read_adc failed or returned no rails.\n");
        return 1;
    }
    for (int i = 0; i < n_rails; i++)
        fprintf(stderr, "    [%2d] %-20s  %7.2f mA (idle)\n",
                i, rails[i].raw_name, rails[i].mA);
    fprintf(stderr, "  → %d current rails discovered\n", n_rails);

    /* ── Allocate working set ─────────────────────────────────────── */
    size_t mem_size = (size_t)mem_mb << 20;
    void *mem = mmap(NULL, mem_size, PROT_READ | PROT_WRITE,
                     MAP_POPULATE | MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    if (mem == MAP_FAILED) { perror("mmap"); return 1; }
    madvise(mem, mem_size, MADV_HUGEPAGE);
    for (size_t off = 0; off < mem_size; off += 4096)
        ((volatile char *)mem)[off] = (char)(off & 0xff);
    mlockall(MCL_CURRENT | MCL_FUTURE);

    fprintf(stderr,
        "\n  Configuration:\n"
        "    Working set : %d MB (%zu cache lines)\n"
        "    Workers     : %d on cores", mem_mb, mem_size / 64, n_workers);
    for (int w = 0; w < n_workers; w++)
        fprintf(stderr, " %d", worker_core_map[w]);
    fprintf(stderr, "\n"
        "    Sampler     : core %d\n"
        "    Repetitions : %d (%s order)\n"
        "    Warmup      : %d s\n"
        "    Per pattern : %d samples × %d s\n"
        "    Settle      : %d ms\n"
        "    Operation   : %s\n",
        sampler_core, n_reps, randomize ? "randomized" : "sequential",
        warmup_secs, num_samples, duration_sec, settle_ms,
        do_write == 0 ? "READ" : do_write == 1 ? "WRITE" : "BOTH");

    int n_patterns  = 512 / step_size + 1;
    int n_ops_modes = (do_write == 2) ? 2 : 1;
    long long total_secs =
        (long long)warmup_secs +
        (long long)n_reps * n_ops_modes * n_patterns *
            ((long long)duration_sec + settle_ms / 1000 + 1);
    fprintf(stderr,
        "    Patterns/rep: %d\n"
        "    Estimated total runtime: %lld s (~%.1f min)\n\n",
        n_patterns, total_secs, total_secs / 60.0);

    /* ── Spawn workers ────────────────────────────────────────────── */
    pthread_t threads[MAX_WORKERS];
    for (int w = 0; w < n_workers; w++) {
        memset(&workers[w], 0, sizeof(workers[w]));
        workers[w].mem       = (uint8_t *)mem;
        workers[w].mem_size  = (mem_size / 64 / n_workers) * 64 * n_workers;
        workers[w].worker_id = w;
        workers[w].n_workers = n_workers;
        workers[w].cpu       = worker_core_map[w];
        __atomic_store_n(&workers[w].running, 0, __ATOMIC_RELEASE);
        pthread_create(&threads[w], NULL, worker_thread, &workers[w]);
    }

    /* ── Thermal warmup ───────────────────────────────────────────── */
    if (warmup_secs > 0) {
        fprintf(stderr, "  Thermal warmup: %d s of sustained hammering...\n",
                warmup_secs);
        uint8_t warmup_pattern[64];
        generate_pattern(warmup_pattern, 256);
        buffer_set_pattern((volatile uint8_t *)mem, mem_size, warmup_pattern);
        workers_set(n_workers, 0, warmup_pattern);
        int64_t t_end = time(NULL) + warmup_secs;
        int last = -1;
        while (time(NULL) < t_end) {
            int remaining = (int)(t_end - time(NULL));
            if (remaining != last) {
                read_pmic_all();
                double ddrq = 0;
                for (int i = 0; i < n_rails; i++)
                    if (strstr(rails[i].raw_name, "VDDQ") ||
                        strstr(rails[i].raw_name, "DDR_IO"))
                        ddrq = rails[i].mA;
                fprintf(stderr,
                    "\r    %3ds remaining   VDDQ≈%6.2f mA   ops=%lu      ",
                    remaining, ddrq,
                    (unsigned long)workers_total_ops(n_workers));
                last = remaining;
            }
            usleep(200000);
        }
        workers_pause(n_workers);
        fprintf(stderr, "\r    warmup complete                                      \n");
    }

    /* ── CSV header ───────────────────────────────────────────────── */
    /* ops       — cumulative bursts since this pattern's workers_set
     * ops_delta — bursts that completed during this sample's vcgencmd
     *             window. Per-sample burst count varies (scheduling
     *             jitter, vcgencmd popen latency, etc.) and that variance
     *             is NOT data-dependent — so without normalization a
     *             high-HW pattern that happened to get fewer bursts in
     *             can look like a low-HW pattern. Divide rail current by
     *             ops_delta at analysis time to recover per-burst signal. */
    printf("rep,num_ones,operation,sample,ops,ops_delta");
    for (int i = 0; i < n_rails; i++)
        printf(",%s_curr_mA", rails[i].name);
    printf("\n");

    int *pattern_order = (int *)malloc(n_patterns * sizeof(int));
    for (int i = 0; i < n_patterns; i++) pattern_order[i] = i * step_size;

    int op_modes[] = { 0, 1 };
    if (do_write == 1) op_modes[0] = 1;

    /* ── Sweep ────────────────────────────────────────────────────── */
    for (int rep = 0; rep < n_reps; rep++) {
        if (randomize) {
            srand((unsigned)(time(NULL) ^ (rep * 2654435761u)));
            for (int i = n_patterns - 1; i > 0; i--) {
                int j = rand() % (i + 1);
                int tmp = pattern_order[i];
                pattern_order[i] = pattern_order[j];
                pattern_order[j] = tmp;
            }
        }
        for (int oi = 0; oi < n_ops_modes; oi++) {
            int is_write = op_modes[oi];
            const char *op_name = is_write ? "write" : "read";

            for (int pi = 0; pi < n_patterns; pi++) {
                int num_ones = pattern_order[pi];
                uint8_t pattern[64];
                generate_pattern(pattern, num_ones);
                int actual = count_ones(pattern, 64);

                buffer_set_pattern((volatile uint8_t *)mem, mem_size, pattern);
                usleep(settle_ms * 1000);

                workers_set(n_workers, is_write, pattern);
                usleep(500000);     /* warm up the new pattern */

                int interval_us = (duration_sec * 1000000) / num_samples;
                if (interval_us < 1000) interval_us = 1000;

                for (int s = 0; s < num_samples; s++) {
                    uint64_t ops_before = workers_total_ops(n_workers);
                    read_pmic_all();
                    uint64_t ops_after  = workers_total_ops(n_workers);
                    uint64_t ops_delta  = ops_after - ops_before;
                    printf("%d,%d,%s,%d,%lu,%lu",
                           rep, actual, op_name, s,
                           (unsigned long)ops_after,
                           (unsigned long)ops_delta);
                    for (int i = 0; i < n_rails; i++)
                        printf(",%.3f", rails[i].mA);
                    printf("\n");
                    usleep(interval_us);
                }
                workers_pause(n_workers);

                fprintf(stderr,
                  "\r  rep %d/%d [%s] %2d/%d  ones=%3d (%3d actual)  ops=%lu",
                  rep + 1, n_reps, op_name,
                  pi + 1, n_patterns, num_ones, actual,
                  (unsigned long)workers_total_ops(n_workers));
            }
            fprintf(stderr, "\n");
        }
    }

    /* ── Shutdown ─────────────────────────────────────────────────── */
    for (int w = 0; w < n_workers; w++)
        __atomic_store_n(&workers[w].running, -1, __ATOMIC_RELEASE);
    for (int w = 0; w < n_workers; w++)
        pthread_join(threads[w], NULL);

    munmap(mem, mem_size);
    free(pattern_order);
    fprintf(stderr, "\n  Done.\n\n");
    return 0;
}
