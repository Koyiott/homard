// SPDX-License-Identifier: GPL-2.0
/*
 * spec_power_p2_v11.c — both-spec stride-walk cross-term escalation.
 *
 *   *** DRAFT — asm to be human-reviewed before running on hardware. ***
 *
 * v11 keeps ALL v10 infrastructure verbatim (three memtype-aliased pools, the
 * SET_G/SET_V/SET_V_HW/SET_ARM ioctl plane, n_workers, WALK_ITERS_BLOCK,
 * G_OFFSET/V_OFFSET) and adds three new gadgets that push the speculative
 * G<->V cross-term harder than v10's single-spec-load arms.
 *
 * What changes vs v10:
 *
 *   1. A NEW non-cacheable STRIDE-WALK ANCHOR region (kbuf_anchor) is added,
 *      mapped MT_NORMAL_NC (pgprot_writecombine), 16 KiB == 256 cache lines.
 *      This is the min13 stride-walk anchor regime (min13.c:224, gadget
 *      gadget_hammer_spec_arch at min13.c:269), ported to the v10 alloc helper.
 *      Critically it is Normal-NC, NOT min13's pgprot_noncached: on this kernel
 *      pgprot_noncached == MT_DEVICE_nGnRnE, which forbids speculative data
 *      access (the v10 ARM_D null). Normal-NC is speculatable AND uncached, so
 *      a cold (never-civac'd) anchor line read is a slow DRAM fill that holds
 *      the RSB-mispredicted return open WITHOUT a per-iter civac on the anchor.
 *      The anchor pointer stride-walks 64 bytes/iter (wrapping at 16 KiB) so
 *      each iteration reads a structurally-cold line — no civac ever needed.
 *
 *   2. THREE new arms, both-spec (G AND V loaded speculatively), continuing
 *      v10's arm numbering (v10 ended at ARM_F=6, ARM_MAX=7):
 *
 *        7 ARM_G  cacheable G+V, civac-ONCE-before-loop, spec G/V via LDNP,
 *                 stride-walk cold NC anchor.  Tests whether a single pre-loop
 *                 flush (cache cold for the whole block) plus the wide stride
 *                 anchor lets the both-spec window reach DRAM.
 *        8 ARM_H  cacheable G+V, civac-EVERY-iter, spec G/V via LDP,
 *                 stride-walk cold NC anchor.  The aggressive per-iter flush
 *                 forces each spec load to miss L1 and dispatch a fill.
 *        9 ARM_I  realistic Spectre-v1 bounds-check-bypass on CACHEABLE G/V.
 *                 An attacker-controlled index is bounds-checked against a
 *                 limit; the mispredicted-taken path indexes G then V. Models
 *                 the actual ioctl-style victim, not an RSB toy.
 *
 *      ARM_MAX = 10.  REF(0) and ARM_F(6) reuse the v10 bodies verbatim as the
 *      committed-cross-term ceiling and the arch-G-only floor, respectively.
 *
 * SAFETY: none of the new gadgets use `brk #0` at the misspeculation tail (that
 * was min13's defense-in-depth panic, unsuitable for a long sweep). Instead the
 * cold-anchor return value is &4f (the loop tail), so an architectural
 * fall-through deliberately lands on the loop-control code and the thunk body is
 * skipped on the architectural path via `b 3f`. No fault is possible.
 *
 * HD(G,V) is set from userspace exactly as v10: V via SPEC_SET_V_HW /
 * SPEC_SET_V, G via SPEC_SET_G (the runner builds G to a target HD vs V).
 *
 * Device:        /dev/spec_power_p2_v11
 * ioctl magic:   0xF1
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/uaccess.h>
#include <linux/vmalloc.h>
#include <linux/slab.h>
#include <linux/ioctl.h>
#include <linux/string.h>
#include <linux/sched.h>
#include <linux/preempt.h>
#include <linux/kthread.h>
#include <linux/wait.h>
#include <linux/delay.h>
#include <linux/atomic.h>
#include <linux/mm.h>
#include <linux/gfp.h>
#include <linux/io.h>
#include <linux/pgtable.h>
#include <asm/pgtable.h>

#define DEVICE_NAME      "spec_power_p2_v11"
#define CLASS_NAME       "spec_power_p2_v11"
#define LINE_BYTES       64

/* G and V live on the same DRAM row, one cache line apart (COL toggle).
 * Mirrors v10/min13 G_OFFSET=0 / V_OFFSET=64. */
#define G_OFFSET         0ul
#define V_OFFSET         64ul
/* ARM_K multi-line VDDQ amplifier: number of contiguous V cache lines folded
 * per speculative trigger (Phase-1 calibration amplifier). All K lines hold the
 * same HW(V) pattern; SET_V replicates across them. Lines V_OFFSET+n*64,
 * n=0..K-1 occupy 64..1088 (< one page, same DRAM row). */
#define V_KLINES         16ul

/* vmap on this kernel rejects 1-page allocations (noted in MIN12); allocate a
 * multi-page region per buffer and use the first page. 64 pages = 256 KiB. */
#define ALLOC_PAGES      64

/* NEW: non-cacheable stride-walk anchor span — 16 KiB == 256 cache lines.
 * The anchor pointer advances 64 B/iter and wraps at this span, so every
 * iteration's anchor read hits a structurally-cold line (no civac required).
 * (min13.c:224 used 4 KiB; 16 KiB gives 256 distinct cold lines == one per
 * WALK_ITERS_BLOCK iteration, so no line is reused within a block.) */
#define ANCHOR_STRIDE_BYTES   (16ul * 1024)

#define MAX_WORKERS      3
#define WALK_ITERS_BLOCK 256        /* T19 linear regime; matches v10/min13 */

/* ---- arm ids (ioctl 21 payload) ---- */
enum spec_arm {
    ARM_REF = 0,    /* cacheable, both loads architectural (positive control) */
    ARM_A   = 1,    /* cacheable, spec V via LDNP                 (v10 carry)  */
    ARM_B   = 2,    /* cacheable, spec V via LDP                  (v10 carry)  */
    ARM_C   = 3,    /* Normal-NC, spec V via plain LDP            (v10 carry)  */
    ARM_D   = 4,    /* Device,    spec V via LDNP   (null anchor) (v10 carry)  */
    ARM_E   = 5,    /* cacheable, spec V via LDP, barriers stripped (v10 carry)*/
    ARM_F   = 6,    /* cacheable, arch G + EMPTY spec window (arch-G floor)    */
    /* ---- NEW v11 both-G+V-speculative arms (single per-worker cold anchor) ---- */
    ARM_G   = 7,    /* cacheable G+V both-spec, civac-ONCE-before-loop + LDNP. */
    ARM_H   = 8,    /* cacheable G+V both-spec, civac-EVERY-iter   + LDP.       */
    ARM_I   = 9,    /* cacheable G+V both-spec, civac-EVERY-iter   + LDNP. BEST */
    ARM_J   = 10,   /* like ARM_I but spec loads V FIRST then G (V-dominant order)*/
    ARM_K   = 11,   /* multi-line VDDQ amplifier: spec-read V_KLINES V-lines      */
    ARM_L   = 12,   /* BEST-spec GGVV full-64: spec all of G then all of V (RSB)  */
    ARM_M   = 13,   /* like ARM_L (GGVV full-64 spec RSB) but LDP (allocating) like
                       the arch loads, instead of LDNP — tests load-type vs gamma   */
    ARM_N   = 14,   /* ARCHITECTURAL GGVV on NON-CACHEABLE mem, NO flush: ARM_REF
                       minus civac/dsb, reading kbuf_nnc (always DRAM) — never flush */
    ARM_O   = 15,   /* ARM_L GGVV-LDNP spec body, RSB window held by a K=3 CHAINED
                       cold Normal-NC anchor (kbuf_anchor) — widen window so both
                       G+V fills complete on the DQ bus before squash */
    ARM_P   = 16,   /* ARM_L GGVV-LDNP spec body, cold anchor = pre-seeded Normal-NC
                       STRIDE-WALK (min13) — kills per-iter civac/dsb dead time */
    ARM_Q   = 17,   /* 32B payload: spec-load ONLY G[0:32] then V[0:32] (1 LDP each,
                       full 64B line still fills) via LDP+civac, single anchor — V
                       issues as 2nd instr => more likely to complete before squash */
    ARM_R   = 18,   /* like ARM_Q (32B LDP) but with the K=3 chained Normal-NC anchor
                       (wide window) — payload-size + LDP + window-widen combined  */
    ARM_S   = 19,   /* 32B LDP spec body, window widened by K=3 CACHEABLE civac'd
                       chained cold misses (NOT Normal-NC — NC kills spec). All
                       cacheable+flush; increases spec window the cacheable way    */
    ARM_T   = 20,   /* 32B LDP spec body, window held by a DEPENDENT ALU DELAY of
                       `anchor_k` cycles (no anchor memory traffic) — sweep anchor_k
                       to find the rate/issue sweet spot (faster anchor => higher
                       trigger rate => higher duty cycle, if loads still dispatch) */
    ARM_U   = 21,   /* FULL GGVV (4 LDP, all 64B of G then V — same as ARM_REF/ARM_L)
                       under the ALU-DELAY anchor (anchor_k). Tests whether the full-
                       line arch pattern reaches arch gamma under the minimal window */
    ARM_MAX = 22,
};

/* ---- alloc class for a given arm ---- */
enum buf_class { BUF_CACHE = 0, BUF_NNC = 1, BUF_DEV = 2 };

static enum buf_class arm_buf_class(int arm)
{
    switch (arm) {
    case ARM_C: return BUF_NNC;
    case ARM_N: return BUF_NNC;     /* arch GGVV on Normal-NC, no flush */
    case ARM_D: return BUF_DEV;
    default:    return BUF_CACHE;   /* REF, A, B, E, F, G, H, I */
    }
}

#define SPEC_IOC_MAGIC          0xF1
#define SPEC_SET_V              _IOW(SPEC_IOC_MAGIC, 1, struct spec_line)
#define SPEC_SET_G              _IOW(SPEC_IOC_MAGIC, 2, struct spec_line)
#define SPEC_START              _IO( SPEC_IOC_MAGIC, 3)
#define SPEC_STOP               _IO( SPEC_IOC_MAGIC, 4)
#define SPEC_GADGET_INFO        _IOR(SPEC_IOC_MAGIC, 5, struct spec_info)
#define SPEC_SET_V_HW           _IOW(SPEC_IOC_MAGIC, 7, unsigned int)
#define SPEC_SET_ARM            _IOW(SPEC_IOC_MAGIC, 21, unsigned int)
#define SPEC_GET_KBUF_SIZE      _IOR(SPEC_IOC_MAGIC, 33, __u64)
#define SPEC_GET_PA             _IOWR(SPEC_IOC_MAGIC, 30, struct spec_pa_query)
#define SPEC_SET_V_OFFSET       _IOW(SPEC_IOC_MAGIC, 31, __u64)
#define SPEC_SET_ANCHOR_K       _IOW(SPEC_IOC_MAGIC, 32, unsigned int)

struct spec_line { u8 bytes[LINE_BYTES]; };
struct spec_info {
    __u64 cache_va; __u64 nnc_va; __u64 dev_va;
    __u32 alloc_bytes; __u32 line_bytes;
    __u64 v_offset; __u64 g_offset; __u32 arm;
};
struct spec_pa_query { __u64 offset; __u64 phys_addr; };

struct krsb_anchor { u64 val; u8 _pad[64 - sizeof(u64)]; } __aligned(64);
struct krsb_worker { struct task_struct *task; int cpu; int worker_idx; struct krsb_anchor anchor; };

static struct krsb_worker workers[MAX_WORKERS];
static int n_workers = 1;          module_param(n_workers, int, 0444);
static int spec_arm  = ARM_REF;    module_param(spec_arm, int, 0444);
/* ARM_T anchor-latency sweep: length of the dependent ALU delay chain that holds
 * the RSB misprediction window open (in loop iterations ~= cycles). Set at insmod
 * to sweep window width WITHOUT a DRAM-miss anchor (no anchor bus traffic/civac).
 * Clamped >=1 in the gadget (0 would be an infinite delay loop). */
static int anchor_k  = 64;         module_param(anchor_k, int, 0444);
/* Runtime-settable V offset (SPEC_SET_V_OFFSET) — used by gadget_ref for the
 * DRAM row-boundary sweep: fix G@0, sweep V across the page, watch DDR_VDD2. */
static unsigned long v_off_dyn = V_OFFSET;

static atomic_t spec_run  = ATOMIC_INIT(0);
static atomic_t spec_quit = ATOMIC_INIT(0);
static DECLARE_WAIT_QUEUE_HEAD(spec_wait);
static u64 arch_sink;

/* ---- buffer regions (three memtype aliases, identical to v10) ---- */
static struct page *cache_pages[ALLOC_PAGES];
static struct page *nnc_pages[ALLOC_PAGES];
static struct page *dev_pages[ALLOC_PAGES];
static u8 *kbuf_cache;   /* MT_NORMAL (vmalloc) — civac to reach DRAM */
static u8 *kbuf_nnc;     /* MT_NORMAL_NC (pgprot_writecombine) — always DRAM */
static u8 *kbuf_dev;     /* MT_DEVICE_nGnRnE (pgprot_noncached) — no spec */

/* ---- NEW: non-cacheable stride-walk anchor (Normal-NC, speculatable) ---- */
static struct page *anchor_pages[ALLOC_PAGES];
static u8 *kbuf_anchor;  /* MT_NORMAL_NC — cold lines hold the spec window open */

#define ALLOC_BYTES (ALLOC_PAGES * PAGE_SIZE)

/* alloc a multi-page region, map it with the given pgprot. (verbatim v10) */
static u8 *alloc_mapped(struct page **pages_out, pgprot_t prot)
{
    int i;
    void *vaddr;
    for (i = 0; i < ALLOC_PAGES; i++) {
        pages_out[i] = alloc_page(GFP_KERNEL | __GFP_ZERO);
        if (!pages_out[i]) {
            while (--i >= 0) __free_page(pages_out[i]);
            return NULL;
        }
    }
    vaddr = vmap(pages_out, ALLOC_PAGES, VM_MAP, prot);
    if (!vaddr) {
        for (i = 0; i < ALLOC_PAGES; i++) __free_page(pages_out[i]);
        return NULL;
    }
    return (u8 *)vaddr;
}

static void free_mapped(u8 *vaddr, struct page **pages)
{
    int i;
    if (vaddr) vunmap((void *)vaddr);
    for (i = 0; i < ALLOC_PAGES; i++) if (pages[i]) __free_page(pages[i]);
}

static u8 *buf_for_class(enum buf_class c)
{
    switch (c) {
    case BUF_NNC: return kbuf_nnc;
    case BUF_DEV: return kbuf_dev;
    default:      return kbuf_cache;
    }
}
static u8 *active_buf(void) { return buf_for_class(arm_buf_class(spec_arm)); }

/* =====================================================================
 * GADGETS.
 *
 * REF and ARM_F are carried over from v10 unchanged (ceiling / floor).
 * ARM_G / ARM_H / ARM_I are new both-spec / Spectre-v1 bodies.
 *
 * Register layout (shared with v10/v9):
 *   x9  = anchor return label (4f)
 *   x11 = G pointer
 *   x13 = V pointer
 *   x14 = anchor pointer (stride-walks for the NEW arms)
 *   x15 = arch sink
 *   x12 = iter counter
 *   x16 = arch scratch
 *   x19 = anchor-span end (NEW arms; AAPCS64 callee-saved => MUST be clobbered)
 *   x30 = link / anchor scratch
 *
 * SAFE-SINK invariant for the NEW arms (no brk #0):
 *   - every cold anchor line is pre-seeded with &4f (the loop tail).
 *   - bl 2f  → RSB predicts return to PC+4 (== 3f, the spec window head).
 *   - spec window runs the G/V loads in the shadow of the slow cold-anchor ret.
 *   - the architectural path SKIPS the spec body via `b 3f`, then at 2: loads
 *     x30 from the cold NC line (slow, squashes the spec path) and rets to 4:.
 *   - so an architectural fall-through can only ever reach 4: (the loop tail) —
 *     never an undefined instruction. No panic, no fault.
 * =====================================================================
 */

/* ARM_REF — cacheable, civac G+V, BOTH loads architectural (no bl).
 * Positive control: ADC + HD model + rail wiring all work; sets slope ceiling.
 * (verbatim v10 gadget_ref) */
static noinline void gadget_ref(u8 *base)
{
    u8 *G = base + G_OFFSET;
    u8 *V = base + v_off_dyn;   /* runtime offset for the row-boundary sweep */
    asm volatile(
        "mov x11, %[Gp]\n\t"
        "mov x13, %[Vp]\n\t"
        "mov x12, %[iters]\n\t"
        "mov x15, %[sinkp]\n\t"
        "1:\n\t"
        "dc civac, x11\n\t"
        "dc civac, x13\n\t"
        "dsb ish\n\t"
        /* arch G */
        "ldp q0,q1,[x11,#0]\n\t"
        "ldp q2,q3,[x11,#32]\n\t"
        /* arch V — committed, NOT speculative */
        "ldp q4,q5,[x13,#0]\n\t"
        "ldp q6,q7,[x13,#32]\n\t"
        "umov x16, v3.d[0]\n\t"
        "str x16, [x15]\n\t"
        "umov x16, v7.d[0]\n\t"
        "str x16, [x15]\n\t"
        "subs x12, x12, #1\n\t"
        "b.ne 1b\n\t"
        :
        : [Gp]"r"(G), [Vp]"r"(V), [sinkp]"r"((uintptr_t)&arch_sink),
          [iters]"r"((uintptr_t)WALK_ITERS_BLOCK)
        : "x11","x12","x13","x15","x16",
          "q0","q1","q2","q3","q4","q5","q6","q7","memory"
    );
}

/* ARM_F — cacheable, civac G+V, arch G + RSB window but the spec window is
 * EMPTY (no V load). Arch-G-only HD floor. (verbatim v10 gadget_arm_f_nov) */
static noinline void gadget_arm_f_nov(struct krsb_worker *w, u8 *base)
{
    u8 *G = base + G_OFFSET;
    u8 *V = base + V_OFFSET;
    asm volatile(
        "mov x11, %[Gp]\n\t"
        "mov x13, %[Vp]\n\t"
        "mov x12, %[iters]\n\t"
        "mov x14, %[anchor]\n\t"
        "mov x15, %[sinkp]\n\t"
        "adr x9, 4f\n\t"
        "1:\n\t"
        /* flush G, V, anchor — same as ARM_A so DRAM/cache state matches */
        "dc civac, x11\n\t"
        "dc civac, x13\n\t"
        "str x9, [x14]\n\t"
        "dc civac, x14\n\t"
        "dsb ish\n\t"
        "dc civac, x14\n\t"
        "dsb ish\n\t"
        /* arch G */
        "ldp q0,q1,[x11,#0]\n\t"
        "ldp q2,q3,[x11,#32]\n\t"
        "umov x16, v3.d[0]\n\t"
        "str x16, [x15]\n\t"
        /* RSB window opens but loads NOTHING (no spec V) */
        "bl 2f\n\t"
        "3:\n\t"
        /* (empty speculative shadow) */
        "2:\n\t"
        "ldr x30, [x14]\n\t"
        "ret\n\t"
        "4:\n\t"
        "subs x12, x12, #1\n\t"
        "b.ne 1b\n\t"
        :
        : [Gp]"r"(G), [Vp]"r"(V), [anchor]"r"((uintptr_t)&w->anchor.val),
          [sinkp]"r"((uintptr_t)&arch_sink),
          [iters]"r"((uintptr_t)WALK_ITERS_BLOCK)
        : "x9","x11","x12","x13","x14","x15","x16","x30",
          "q0","q1","q2","q3","memory"
    );
}

/* =====================================================================
 * ARM_G — cacheable G+V, civac-ONCE-before-loop, BOTH G and V loaded
 * speculatively via LDNP. SINGLE per-worker cold anchor line (no stride-walk).
 *
 * Window mechanism = the proven v10 ARM_A one: each iter stores the landing-pad
 * address into the worker's own anchor line, civac+dsb's it cold, then the
 * thunk's `ldr x30,[anchor]` misses to DRAM (slow) -> wide spec window. The RSB
 * (bl pushes PC+4 == 3:) speculatively runs the G/V loads at 3:.
 *
 * civac G+V ONCE before the loop; LDNP (non-temporal, non-allocating) keeps the
 * lines uncached so every iteration's spec read still misses to DRAM.
 * Everything is single-cache-line: G@gv+0, V@gv+64, anchor = w->anchor.val.
 * ===================================================================== */
static noinline void gadget_arm_g_bothspec_civac_once(struct krsb_worker *w)
{
    u8 *G = kbuf_cache + G_OFFSET;
    u8 *V = kbuf_cache + V_OFFSET;
    asm volatile(
        "mov x11, %[Gp]\n\t"
        "mov x13, %[Vp]\n\t"
        "mov x12, %[iters]\n\t"
        "mov x14, %[anchor]\n\t"
        "mov x15, %[sinkp]\n\t"
        "adr x9, 4f\n\t"                 /* arch ret target = landing pad 4: */
        /* civac G+V ONCE; LDNP keeps them uncached for the whole block */
        "dc civac, x11\n\t"
        "dc civac, x13\n\t"
        "dsb ish\n\t"
        "1:\n\t"
        /* arm the single cold anchor line (slow ret -> wide spec window) */
        "str x9, [x14]\n\t"
        "dc civac, x14\n\t"
        "dsb ish\n\t"
        "dc civac, x14\n\t"
        "dsb ish\n\t"
        "bl 2f\n\t"
        "3:\n\t"                         /* SPEC WINDOW = PC+4 = RSB target */
        "ldnp q0, q1, [x11, #0]\n\t"     /* spec G */
        "ldnp q2, q3, [x13, #0]\n\t"     /* spec V (G->V toggle) */
        "ldnp q4, q5, [x11, #0]\n\t"     /* spec G (V->G toggle) */
        "ldnp q6, q7, [x13, #0]\n\t"     /* spec V (G->V toggle) */
        "2:\n\t"
        "ldr x30, [x14]\n\t"             /* cold anchor (== &4f) -> slow ret */
        "ret\n\t"                        /* arch -> 4:,  spec(RSB) -> 3: */
        "4:\n\t"                         /* landing pad (arch ret target) */
        "umov x16, v3.d[0]\n\t"
        "str x16, [x15]\n\t"
        "subs x12, x12, #1\n\t"
        "b.ne 1b\n\t"
        :
        : [Gp]"r"(G), [Vp]"r"(V), [anchor]"r"((uintptr_t)&w->anchor.val),
          [sinkp]"r"((uintptr_t)&arch_sink),
          [iters]"r"((uintptr_t)WALK_ITERS_BLOCK)
        : "x9","x11","x12","x13","x14","x15","x16","x30",
          "q0","q1","q2","q3","q4","q5","q6","q7","memory"
    );
}

/* =====================================================================
 * ARM_H — cacheable G+V, civac-EVERY-iter, BOTH G and V loaded speculatively
 * via LDP. SINGLE per-worker cold anchor line (no stride-walk).
 *
 * Per-iter civac of G and V makes both lines cold before the spec window, so
 * every speculative LDP misses L1 and dispatches a DRAM fill. (LDP —
 * cache-allocating — vs ARM_G/ARM_I's LDNP isolates the non-temporal hint.)
 * Same proven single-anchor RSB window as ARM_G.
 * ===================================================================== */
static noinline void gadget_arm_h_bothspec_civac_every(struct krsb_worker *w)
{
    u8 *G = kbuf_cache + G_OFFSET;
    u8 *V = kbuf_cache + V_OFFSET;
    asm volatile(
        "mov x11, %[Gp]\n\t"
        "mov x13, %[Vp]\n\t"
        "mov x12, %[iters]\n\t"
        "mov x14, %[anchor]\n\t"
        "mov x15, %[sinkp]\n\t"
        "adr x9, 4f\n\t"
        "1:\n\t"
        /* civac G+V EVERY iter (LDP allocates, so re-flush each iteration) */
        "dc civac, x11\n\t"
        "dc civac, x13\n\t"
        "str x9, [x14]\n\t"
        "dc civac, x14\n\t"
        "dsb ish\n\t"
        "dc civac, x14\n\t"
        "dsb ish\n\t"
        "bl 2f\n\t"
        "3:\n\t"                         /* SPEC WINDOW = PC+4 = RSB target */
        "ldp q0, q1, [x11, #0]\n\t"      /* spec G */
        "ldp q2, q3, [x13, #0]\n\t"      /* spec V (G->V toggle) */
        "ldp q4, q5, [x11, #0]\n\t"      /* spec G (V->G toggle) */
        "ldp q6, q7, [x13, #0]\n\t"      /* spec V (G->V toggle) */
        "2:\n\t"
        "ldr x30, [x14]\n\t"             /* cold anchor (== &4f) -> slow ret */
        "ret\n\t"                        /* arch -> 4:,  spec(RSB) -> 3: */
        "4:\n\t"
        "umov x16, v3.d[0]\n\t"
        "str x16, [x15]\n\t"
        "subs x12, x12, #1\n\t"
        "b.ne 1b\n\t"
        :
        : [Gp]"r"(G), [Vp]"r"(V), [anchor]"r"((uintptr_t)&w->anchor.val),
          [sinkp]"r"((uintptr_t)&arch_sink),
          [iters]"r"((uintptr_t)WALK_ITERS_BLOCK)
        : "x9","x11","x12","x13","x14","x15","x16","x30",
          "q0","q1","q2","q3","q4","q5","q6","q7","memory"
    );
}

/* =====================================================================
 * ARM_I — cacheable G+V, civac-EVERY-iter, BOTH G and V loaded speculatively
 * via LDNP. SINGLE per-worker cold anchor line (no stride-walk).
 *
 * Completes the 2x2 flush x load matrix (all RSB, the preferred mechanism):
 *   ARM_G = civac-once  + LDNP        ARM_H = civac-every + LDP
 *   ARM_I = civac-every + LDNP  -> vs ARM_H isolates LDNP-vs-LDP at flush-every,
 *                                  vs ARM_G isolates once-vs-every at LDNP.
 * Identical body to ARM_H but LDNP spec loads.
 * ===================================================================== */
static noinline void gadget_arm_i_bothspec_every_ldnp(struct krsb_worker *w)
{
    u8 *G = kbuf_cache + G_OFFSET;
    u8 *V = kbuf_cache + V_OFFSET;
    asm volatile(
        "mov x11, %[Gp]\n\t"
        "mov x13, %[Vp]\n\t"
        "mov x12, %[iters]\n\t"
        "mov x14, %[anchor]\n\t"
        "mov x15, %[sinkp]\n\t"
        "adr x9, 4f\n\t"
        "1:\n\t"
        "dc civac, x11\n\t"
        "dc civac, x13\n\t"
        "str x9, [x14]\n\t"
        "dc civac, x14\n\t"
        "dsb ish\n\t"
        "dc civac, x14\n\t"
        "dsb ish\n\t"
        "bl 2f\n\t"
        "3:\n\t"                         /* SPEC WINDOW = PC+4 = RSB target */
        "ldnp q0, q1, [x11, #0]\n\t"     /* spec G */
        "ldnp q2, q3, [x13, #0]\n\t"     /* spec V (G->V toggle) */
        "ldnp q4, q5, [x11, #0]\n\t"     /* spec G (V->G toggle) */
        "ldnp q6, q7, [x13, #0]\n\t"     /* spec V (G->V toggle) */
        "2:\n\t"
        "ldr x30, [x14]\n\t"             /* cold anchor (== &4f) -> slow ret */
        "ret\n\t"                        /* arch -> 4:,  spec(RSB) -> 3: */
        "4:\n\t"
        "umov x16, v3.d[0]\n\t"
        "str x16, [x15]\n\t"
        "subs x12, x12, #1\n\t"
        "b.ne 1b\n\t"
        :
        : [Gp]"r"(G), [Vp]"r"(V), [anchor]"r"((uintptr_t)&w->anchor.val),
          [sinkp]"r"((uintptr_t)&arch_sink),
          [iters]"r"((uintptr_t)WALK_ITERS_BLOCK)
        : "x9","x11","x12","x13","x14","x15","x16","x30",
          "q0","q1","q2","q3","q4","q5","q6","q7","memory"
    );
}

/* =====================================================================
 * ARM_J — identical to ARM_I but the speculative window loads V FIRST then G
 * (V, G, V, G) instead of (G, V, G, V). With G pinned to 0x00 this front-loads
 * the HW(V) data burst onto the DQ bus each speculative pass. Used for the
 * HW(V) templating attack (fix G=0 -> HD(G,V) = HW(V)).
 * ===================================================================== */
static noinline void gadget_arm_j_vfirst(struct krsb_worker *w)
{
    u8 *G = kbuf_cache + G_OFFSET;
    u8 *V = kbuf_cache + V_OFFSET;
    asm volatile(
        "mov x11, %[Gp]\n\t"
        "mov x13, %[Vp]\n\t"
        "mov x12, %[iters]\n\t"
        "mov x14, %[anchor]\n\t"
        "mov x15, %[sinkp]\n\t"
        "adr x9, 4f\n\t"
        "1:\n\t"
        "dc civac, x11\n\t"
        "dc civac, x13\n\t"
        "str x9, [x14]\n\t"
        "dc civac, x14\n\t"
        "dsb ish\n\t"
        "dc civac, x14\n\t"
        "dsb ish\n\t"
        "bl 2f\n\t"
        "3:\n\t"                         /* SPEC WINDOW = PC+4 = RSB target */
        "ldnp q0, q1, [x13, #0]\n\t"     /* spec V FIRST */
        "ldnp q2, q3, [x11, #0]\n\t"     /* spec G (V->G toggle) */
        "ldnp q4, q5, [x13, #0]\n\t"     /* spec V (G->V toggle) */
        "ldnp q6, q7, [x11, #0]\n\t"     /* spec G (V->G toggle) */
        "2:\n\t"
        "ldr x30, [x14]\n\t"             /* cold anchor (== &4f) -> slow ret */
        "ret\n\t"                        /* arch -> 4:,  spec(RSB) -> 3: */
        "4:\n\t"
        "umov x16, v0.d[0]\n\t"          /* consume V (v0 = [x13]) */
        "str x16, [x15]\n\t"
        "subs x12, x12, #1\n\t"
        "b.ne 1b\n\t"
        :
        : [Gp]"r"(G), [Vp]"r"(V), [anchor]"r"((uintptr_t)&w->anchor.val),
          [sinkp]"r"((uintptr_t)&arch_sink),
          [iters]"r"((uintptr_t)WALK_ITERS_BLOCK)
        : "x9","x11","x12","x13","x14","x15","x16","x30",
          "q0","q1","q2","q3","q4","q5","q6","q7","memory"
    );
}

/* =====================================================================
 * ARM_K — multi-line VDDQ amplifier. Speculatively reads V_KLINES contiguous
 * V cache lines (V+0, V+64, ..., V+(K-1)*64), each holding the same redundant-
 * byte HW(V) pattern (SET_V replicates across them). Folding K refills per
 * speculative trigger sums ~K* the DQ-bus current so the DDR_VDDQ rail clears
 * the PMIC ADC floor — the Phase-1 calibration amplifier, inside the v11 RSB
 * spec gadget. G is unused (this is the HW(V) template, G pinned to 0 anyway).
 * ===================================================================== */
static noinline void gadget_arm_k_multiline(struct krsb_worker *w)
{
    u8 *V = kbuf_cache + V_OFFSET;
    asm volatile(
        "mov x13, %[Vp]\n\t"
        "mov x12, %[iters]\n\t"
        "mov x14, %[anchor]\n\t"
        "mov x15, %[sinkp]\n\t"
        "adr x9, 4f\n\t"
        "1:\n\t"
        /* flush all K V-lines (architectural, before the spec window) */
        "mov x17, x13\n\t"
        "mov x18, %[k]\n\t"
        "5:\n\t"
        "dc civac, x17\n\t"
        "add x17, x17, #64\n\t"
        "subs x18, x18, #1\n\t"
        "b.ne 5b\n\t"
        /* arm the RSB cold anchor */
        "str x9, [x14]\n\t"
        "dc civac, x14\n\t"
        "dsb ish\n\t"
        "dc civac, x14\n\t"
        "dsb ish\n\t"
        "bl 2f\n\t"
        "3:\n\t"                         /* SPEC WINDOW: read 16 distinct V lines */
        "ldnp q0,q1,[x13,#0]\n\t"   "ldnp q2,q3,[x13,#64]\n\t"
        "ldnp q4,q5,[x13,#128]\n\t" "ldnp q6,q7,[x13,#192]\n\t"
        "ldnp q0,q1,[x13,#256]\n\t" "ldnp q2,q3,[x13,#320]\n\t"
        "ldnp q4,q5,[x13,#384]\n\t" "ldnp q6,q7,[x13,#448]\n\t"
        "ldnp q0,q1,[x13,#512]\n\t" "ldnp q2,q3,[x13,#576]\n\t"
        "ldnp q4,q5,[x13,#640]\n\t" "ldnp q6,q7,[x13,#704]\n\t"
        "ldnp q0,q1,[x13,#768]\n\t" "ldnp q2,q3,[x13,#832]\n\t"
        "ldnp q4,q5,[x13,#896]\n\t" "ldnp q6,q7,[x13,#960]\n\t"
        "2:\n\t"
        "ldr x30, [x14]\n\t"             /* cold anchor (== &4f) -> slow ret */
        "ret\n\t"                        /* arch -> 4:, spec(RSB) -> 3: */
        "4:\n\t"
        "umov x16, v3.d[0]\n\t"
        "str x16, [x15]\n\t"
        "subs x12, x12, #1\n\t"
        "b.ne 1b\n\t"
        :
        : [Vp]"r"(V), [anchor]"r"((uintptr_t)&w->anchor.val),
          [sinkp]"r"((uintptr_t)&arch_sink),
          [iters]"r"((uintptr_t)WALK_ITERS_BLOCK),
          [k]"r"((uintptr_t)V_KLINES)
        : "x9","x12","x13","x14","x15","x16","x17","x18","x30",
          "q0","q1","q2","q3","q4","q5","q6","q7","memory"
    );
}

/* =====================================================================
 * ARM_L — BEST-spec, GGVV over the FULL 64-byte lines. Cacheable G+V,
 * civac-EVERY-iter, BOTH G and V loaded speculatively via LDNP. SINGLE
 * per-worker cold anchor line (the proven ARM_I RSB mechanism).
 *
 * Speculative analog of ARM_REF: the spec window reads ALL of G (bytes 0..63,
 * two LDNP pairs covering one cache line) THEN ALL of V (bytes 0..63) — i.e.
 * G,G,V,V over the full line, not ARM_I's G,V,G,V over only the first 32 bytes.
 *   - covers every one of the 64 byte/beat lanes (ARM_I only touches 0..31),
 *     so a full 64-distinct-byte secret can be recovered, and
 *   - makes a SINGLE clean G->V bus boundary per pass => leakage =
 *     Sum_i HW(g_i ^ v_i), the unbiased linear-HW model the real-value CPA
 *     assumes. (ARM_I's V->G->V extra toggles inject the bus-direction
 *     coupling asymmetry that biases per-byte HW-CPA.)
 *
 * Same load COUNT and same two cache lines as ARM_I (4 LDNP, 128 B), so the
 * speculative-window budget is identical — only the four addresses change.
 * Architectural fall-through lands on 4: (loop tail); no brk, no fault.
 * ===================================================================== */
static noinline void gadget_arm_l_bothspec_ggvv_full(struct krsb_worker *w)
{
    u8 *G = kbuf_cache + G_OFFSET;
    u8 *V = kbuf_cache + V_OFFSET;
    asm volatile(
        "mov x11, %[Gp]\n\t"
        "mov x13, %[Vp]\n\t"
        "mov x12, %[iters]\n\t"
        "mov x14, %[anchor]\n\t"
        "mov x15, %[sinkp]\n\t"
        "adr x9, 4f\n\t"
        "1:\n\t"
        "dc civac, x11\n\t"
        "dc civac, x13\n\t"
        "str x9, [x14]\n\t"
        "dc civac, x14\n\t"
        "dsb ish\n\t"
        "dc civac, x14\n\t"
        "dsb ish\n\t"
        "bl 2f\n\t"
        "3:\n\t"                         /* SPEC WINDOW = PC+4 = RSB target */
        "ldnp q0, q1, [x11, #0]\n\t"     /* spec G[0:32]                    */
        "ldnp q2, q3, [x11, #32]\n\t"    /* spec G[32:64]  (G fully driven) */
        "ldnp q4, q5, [x13, #0]\n\t"     /* spec V[0:32]   (G->V toggle)    */
        "ldnp q6, q7, [x13, #32]\n\t"    /* spec V[32:64]  (V fully driven) */
        "2:\n\t"
        "ldr x30, [x14]\n\t"             /* cold anchor (== &4f) -> slow ret */
        "ret\n\t"                        /* arch -> 4:,  spec(RSB) -> 3: */
        "4:\n\t"
        "umov x16, v3.d[0]\n\t"
        "str x16, [x15]\n\t"
        "subs x12, x12, #1\n\t"
        "b.ne 1b\n\t"
        :
        : [Gp]"r"(G), [Vp]"r"(V), [anchor]"r"((uintptr_t)&w->anchor.val),
          [sinkp]"r"((uintptr_t)&arch_sink),
          [iters]"r"((uintptr_t)WALK_ITERS_BLOCK)
        : "x9","x11","x12","x13","x14","x15","x16","x30",
          "q0","q1","q2","q3","q4","q5","q6","q7","memory"
    );
}

/* =====================================================================
 * ARM_M — identical to ARM_L (GGVV full-64 both-spec RSB, civac-EVERY, single
 * cold anchor) but the four speculative loads are LDP (cache-ALLOCATING) instead
 * of LDNP (non-temporal).  ARM_REF (the arch reference, gamma=14.8) uses LDP; the
 * LSU may schedule LDP fills more aggressively / less deprioritized than the
 * non-temporal LDNP, so this tests whether matching the arch load TYPE raises the
 * speculative gamma.  civac-EVERY (already present) re-flushes G,V each iter so
 * the allocating LDP still misses to DRAM.  Same safety contract as ARM_L.
 * ===================================================================== */
static noinline void gadget_arm_m_bothspec_ggvv_ldp(struct krsb_worker *w)
{
    u8 *G = kbuf_cache + G_OFFSET;
    u8 *V = kbuf_cache + V_OFFSET;
    asm volatile(
        "mov x11, %[Gp]\n\t"
        "mov x13, %[Vp]\n\t"
        "mov x12, %[iters]\n\t"
        "mov x14, %[anchor]\n\t"
        "mov x15, %[sinkp]\n\t"
        "adr x9, 4f\n\t"
        "1:\n\t"
        "dc civac, x11\n\t"
        "dc civac, x13\n\t"
        "str x9, [x14]\n\t"
        "dc civac, x14\n\t"
        "dsb ish\n\t"
        "dc civac, x14\n\t"
        "dsb ish\n\t"
        "bl 2f\n\t"
        "3:\n\t"                         /* SPEC WINDOW = PC+4 = RSB target */
        "ldp q0, q1, [x11, #0]\n\t"      /* spec G[0:32]  (LDP, allocating) */
        "ldp q2, q3, [x11, #32]\n\t"     /* spec G[32:64] (G fully driven)  */
        "ldp q4, q5, [x13, #0]\n\t"      /* spec V[0:32]  (G->V toggle)     */
        "ldp q6, q7, [x13, #32]\n\t"     /* spec V[32:64] (V fully driven)  */
        "2:\n\t"
        "ldr x30, [x14]\n\t"             /* cold anchor (== &4f) -> slow ret */
        "ret\n\t"                        /* arch -> 4:,  spec(RSB) -> 3: */
        "4:\n\t"
        "umov x16, v3.d[0]\n\t"
        "str x16, [x15]\n\t"
        "subs x12, x12, #1\n\t"
        "b.ne 1b\n\t"
        :
        : [Gp]"r"(G), [Vp]"r"(V), [anchor]"r"((uintptr_t)&w->anchor.val),
          [sinkp]"r"((uintptr_t)&arch_sink),
          [iters]"r"((uintptr_t)WALK_ITERS_BLOCK)
        : "x9","x11","x12","x13","x14","x15","x16","x30",
          "q0","q1","q2","q3","q4","q5","q6","q7","memory"
    );
}

/* =====================================================================
 * ARM_N — the "works-well" ARCHITECTURAL gadget, but with the FLUSH REMOVED and
 * run on NON-CACHEABLE memory. It is gadget_ref (ARM_REF: committed ldp G(64B)
 * then ldp V(64B), one clean G->V bus toggle = Sum_i HW(g_i^v_i)) with the
 * per-iteration `dc civac, G/V` and `dsb ish` DELETED.
 *
 * Rationale: ARM_REF needs civac because its buffer is cacheable (MT_NORMAL) —
 * without the flush, iteration 2+ would hit L1 and never reach DRAM. Here
 * arm_buf_class(ARM_N) routes `base` to kbuf_nnc (MT_NORMAL_NC, pgprot_
 * writecombine), which is NEVER cached, so EVERY committed load goes to DRAM and
 * drives the DQ bus WITHOUT any flush. More realistic (a victim reading an
 * uncached / DMA buffer) and a tighter loop (no civac, no barrier per iter).
 *
 * Open empirical questions this gadget answers (the point of the study):
 *   - gamma: does dropping civac+dsb (faster loop, more G->V toggles/sec) raise,
 *     match, or lower the per-bit slope vs cacheable ARM_REF?
 *   - model purity: is the NC leakage still clean linear HW on VDD2, or does NC
 *     reintroduce a DBI / coupling bias (the min7/min13 NC gadgets needed a
 *     DBI-aware / profiled model)? -> graded by the Tier-3 structural-bias check.
 * Committed loads only; no bl/ret/anchor; cannot fault. ===================== */
static noinline void gadget_arm_n_arch_nc_noflush(u8 *base)
{
    u8 *G = base + G_OFFSET;
    u8 *V = base + v_off_dyn;   /* same runtime V offset as gadget_ref */
    asm volatile(
        "mov x11, %[Gp]\n\t"
        "mov x13, %[Vp]\n\t"
        "mov x12, %[iters]\n\t"
        "mov x15, %[sinkp]\n\t"
        "1:\n\t"
        /* NO dc civac, NO dsb: kbuf_nnc is uncached => every load reaches DRAM */
        "ldp q0,q1,[x11,#0]\n\t"
        "ldp q2,q3,[x11,#32]\n\t"
        "ldp q4,q5,[x13,#0]\n\t"
        "ldp q6,q7,[x13,#32]\n\t"
        "umov x16, v3.d[0]\n\t"
        "str x16, [x15]\n\t"
        "umov x16, v7.d[0]\n\t"
        "str x16, [x15]\n\t"
        "subs x12, x12, #1\n\t"
        "b.ne 1b\n\t"
        :
        : [Gp]"r"(G), [Vp]"r"(V), [sinkp]"r"((uintptr_t)&arch_sink),
          [iters]"r"((uintptr_t)WALK_ITERS_BLOCK)
        : "x11","x12","x13","x15","x16",
          "q0","q1","q2","q3","q4","q5","q6","q7","memory"
    );
}

/* =====================================================================
 * ARM_O — ARM_L's GGVV full-64 LDNP spec body, but the RSB window-holder is a
 * CHAIN of K=3 dependent COLD Normal-NC misses in kbuf_anchor, so the
 * mispredicted RET resolves only after ~3 serialized DRAM round-trips. That
 * widens the speculative window enough for BOTH the 64B G fill and the 64B V
 * fill to issue AND put their DQ bursts on the bus before the squash — the fix
 * for the low gamma + 29% sign-instability (intermittent V-burst landing).
 * kbuf_anchor is MT_NORMAL_NC (always DRAM, never cached) so the chain is
 * structurally cold with NO per-iter civac. Chain (node0->node1->node2->&4f)
 * built ONCE; terminal node = &4f so the architectural ret always lands on the
 * loop tail (no fault). x20 (chain head) is AAPCS64 callee-saved -> clobbered.
 * ===================================================================== */
static noinline void gadget_arm_o_chained_nc_anchor(struct krsb_worker *w)
{
    u8 *G = kbuf_cache + G_OFFSET;
    u8 *V = kbuf_cache + V_OFFSET;
    u8 *A = kbuf_anchor;            /* Normal-NC stride-walk region (16 KiB) */
    asm volatile(
        "mov x11, %[Gp]\n\t"
        "mov x13, %[Vp]\n\t"
        "mov x12, %[iters]\n\t"
        "mov x15, %[sinkp]\n\t"
        "adr x9, 4f\n\t"
        /* build node0->node1->node2->&4f ONCE (stride 4160 = 4096+64 anti-prefetch) */
        "mov x20, %[Ap]\n\t"
        "mov x3, #4160\n\t"
        "add x1, x20, x3\n\t"
        "add x2, x1, x3\n\t"
        "str x1, [x20]\n\t"
        "str x2, [x1]\n\t"
        "str x9, [x2]\n\t"
        "dsb ish\n\t"
        "1:\n\t"
        "dc civac, x11\n\t"              /* flush G to DRAM (cacheable) */
        "dc civac, x13\n\t"              /* flush V to DRAM */
        "dsb ish\n\t"
        "mov x14, x20\n\t"              /* x14 = chain head */
        "bl 2f\n\t"
        "3:\n\t"                         /* SPEC WINDOW = RSB target */
        "ldnp q0, q1, [x11, #0]\n\t"     /* spec G[0:32]  */
        "ldnp q2, q3, [x11, #32]\n\t"    /* spec G[32:64] */
        "ldnp q4, q5, [x13, #0]\n\t"     /* spec V[0:32]  */
        "ldnp q6, q7, [x13, #32]\n\t"    /* spec V[32:64] */
        "2:\n\t"
        "ldr x14, [x14]\n\t"             /* chase node0->node1 (cold NC miss 1) */
        "ldr x14, [x14]\n\t"             /* node1->node2     (cold NC miss 2) */
        "ldr x30, [x14]\n\t"             /* node2->&4f       (cold NC miss 3) */
        "ret\n\t"                        /* arch -> 4:, spec(RSB) -> 3: */
        "4:\n\t"
        "umov x16, v3.d[0]\n\t"
        "str x16, [x15]\n\t"
        "subs x12, x12, #1\n\t"
        "b.ne 1b\n\t"
        :
        : [Gp]"r"(G), [Vp]"r"(V), [Ap]"r"(A), [sinkp]"r"((uintptr_t)&arch_sink),
          [iters]"r"((uintptr_t)WALK_ITERS_BLOCK)
        : "x1","x2","x3","x9","x11","x12","x13","x14","x15","x16","x20","x30",
          "q0","q1","q2","q3","q4","q5","q6","q7","memory"
    );
}

/* =====================================================================
 * ARM_P — ARM_L's GGVV full-64 LDNP spec body, but the cold anchor is a
 * pre-seeded Normal-NC STRIDE-WALK (the proven min13 mechanism): every anchor
 * line is seeded = &4f ONCE, then x14 walks 64 B/iter (wrap at 16 KiB). Because
 * kbuf_anchor is MT_NORMAL_NC, each walked line is structurally cold WITHOUT
 * civac, so the per-iter `str + 2x dc civac + 2x dsb ish` anchor arming of ARM_L
 * VANISHES (duty-cycle / trigger-rate win). x19 (walk end) is AAPCS64
 * callee-saved -> clobbered. Arch ret target is always &4f (loop tail); no fault.
 * ===================================================================== */
static noinline void gadget_arm_p_nc_stride_anchor(struct krsb_worker *w)
{
    u8 *G = kbuf_cache + G_OFFSET;
    u8 *V = kbuf_cache + V_OFFSET;
    u8 *A = kbuf_anchor;
    asm volatile(
        "adr x9, 4f\n\t"
        "mov x14, %[Ap]\n\t"
        "add x19, x14, %[span]\n\t"      /* x19 = A + 16 KiB (walk end) */
        "5:\n\t"                         /* seed every NC line = &4f, ONCE */
        "str x9, [x14]\n\t"
        "add x14, x14, #64\n\t"
        "cmp x14, x19\n\t"
        "b.lo 5b\n\t"
        "dsb ish\n\t"
        "mov x11, %[Gp]\n\t"
        "mov x13, %[Vp]\n\t"
        "mov x12, %[iters]\n\t"
        "mov x15, %[sinkp]\n\t"
        "mov x14, %[Ap]\n\t"            /* x14 = anchor walk ptr */
        "1:\n\t"
        "dc civac, x11\n\t"              /* flush G,V (cacheable) — anchor needs NO arming */
        "dc civac, x13\n\t"
        "dsb ish\n\t"
        "bl 2f\n\t"
        "3:\n\t"                         /* SPEC WINDOW */
        "ldnp q0, q1, [x11, #0]\n\t"
        "ldnp q2, q3, [x11, #32]\n\t"
        "ldnp q4, q5, [x13, #0]\n\t"
        "ldnp q6, q7, [x13, #32]\n\t"
        "2:\n\t"
        "ldr x30, [x14]\n\t"             /* cold NC line (== &4f) -> slow ret */
        "ret\n\t"
        "4:\n\t"
        "umov x16, v3.d[0]\n\t"
        "str x16, [x15]\n\t"
        "add x14, x14, #64\n\t"          /* stride-walk to next cold line */
        "cmp x14, x19\n\t"
        "csel x14, x14, %[Ap], lo\n\t"   /* wrap to A at the end of the span */
        "subs x12, x12, #1\n\t"
        "b.ne 1b\n\t"
        :
        : [Gp]"r"(G), [Vp]"r"(V), [Ap]"r"(A), [span]"r"((uintptr_t)ANCHOR_STRIDE_BYTES),
          [sinkp]"r"((uintptr_t)&arch_sink), [iters]"r"((uintptr_t)WALK_ITERS_BLOCK)
        : "x9","x11","x12","x13","x14","x15","x16","x19","x30",
          "q0","q1","q2","q3","q4","q5","q6","q7","memory"
    );
}

/* =====================================================================
 * ARM_Q — 32-BYTE payload. Spec window loads ONLY G[0:32] (1 LDP) then V[0:32]
 * (1 LDP): the G->V toggle is the 2nd issued instruction (vs the 3rd/4th in the
 * 64B GGVV arms), so the V fill is far more likely to ISSUE before the squash.
 * The demand still triggers a FULL 64B cache-line fill (cache-line granularity),
 * so the DQ bus drives the whole G/V line and gamma stays comparable; only the
 * issue-latency-to-V shrinks. LDP + civac-every (classical), single per-worker
 * cold anchor (ARM_L/M mechanism). Same no-fault safety contract.
 * ===================================================================== */
static noinline void gadget_arm_q_32b_ldp(struct krsb_worker *w)
{
    u8 *G = kbuf_cache + G_OFFSET;
    u8 *V = kbuf_cache + V_OFFSET;
    asm volatile(
        "mov x11, %[Gp]\n\t"
        "mov x13, %[Vp]\n\t"
        "mov x12, %[iters]\n\t"
        "mov x14, %[anchor]\n\t"
        "mov x15, %[sinkp]\n\t"
        "adr x9, 4f\n\t"
        "1:\n\t"
        "dc civac, x11\n\t"
        "dc civac, x13\n\t"
        "str x9, [x14]\n\t"
        "dc civac, x14\n\t"
        "dsb ish\n\t"
        "dc civac, x14\n\t"
        "dsb ish\n\t"
        "bl 2f\n\t"
        "3:\n\t"                         /* SPEC WINDOW */
        "ldp q0, q1, [x11, #0]\n\t"      /* spec G[0:32]  (full 64B line fills) */
        "ldp q2, q3, [x13, #0]\n\t"      /* spec V[0:32]  (G->V toggle, 2nd instr) */
        "2:\n\t"
        "ldr x30, [x14]\n\t"
        "ret\n\t"
        "4:\n\t"
        "umov x16, v2.d[0]\n\t"
        "str x16, [x15]\n\t"
        "subs x12, x12, #1\n\t"
        "b.ne 1b\n\t"
        :
        : [Gp]"r"(G), [Vp]"r"(V), [anchor]"r"((uintptr_t)&w->anchor.val),
          [sinkp]"r"((uintptr_t)&arch_sink),
          [iters]"r"((uintptr_t)WALK_ITERS_BLOCK)
        : "x9","x11","x12","x13","x14","x15","x16","x30",
          "q0","q1","q2","q3","memory"
    );
}

/* =====================================================================
 * ARM_R — ARM_Q's 32-byte LDP spec body, but with ARM_O's K=3 CHAINED Normal-NC
 * cold anchor (wide window). Combines the three levers: small payload (fast V
 * issue) + classical LDP+flush + a window wide enough for both 32B fills to land
 * on the DQ bus before squash. x20 callee-saved -> clobbered. No-fault contract.
 * ===================================================================== */
static noinline void gadget_arm_r_32b_ldp_chained(struct krsb_worker *w)
{
    u8 *G = kbuf_cache + G_OFFSET;
    u8 *V = kbuf_cache + V_OFFSET;
    u8 *A = kbuf_anchor;
    asm volatile(
        "mov x11, %[Gp]\n\t"
        "mov x13, %[Vp]\n\t"
        "mov x12, %[iters]\n\t"
        "mov x15, %[sinkp]\n\t"
        "adr x9, 4f\n\t"
        "mov x20, %[Ap]\n\t"
        "mov x3, #4160\n\t"
        "add x1, x20, x3\n\t"
        "add x2, x1, x3\n\t"
        "str x1, [x20]\n\t"
        "str x2, [x1]\n\t"
        "str x9, [x2]\n\t"
        "dsb ish\n\t"
        "1:\n\t"
        "dc civac, x11\n\t"
        "dc civac, x13\n\t"
        "dsb ish\n\t"
        "mov x14, x20\n\t"
        "bl 2f\n\t"
        "3:\n\t"                         /* SPEC WINDOW */
        "ldp q0, q1, [x11, #0]\n\t"      /* spec G[0:32] */
        "ldp q2, q3, [x13, #0]\n\t"      /* spec V[0:32] (G->V toggle, 2nd instr) */
        "2:\n\t"
        "ldr x14, [x14]\n\t"             /* chained cold NC miss 1 */
        "ldr x14, [x14]\n\t"             /* miss 2 */
        "ldr x30, [x14]\n\t"             /* miss 3 -> &4f */
        "ret\n\t"
        "4:\n\t"
        "umov x16, v2.d[0]\n\t"
        "str x16, [x15]\n\t"
        "subs x12, x12, #1\n\t"
        "b.ne 1b\n\t"
        :
        : [Gp]"r"(G), [Vp]"r"(V), [Ap]"r"(A), [sinkp]"r"((uintptr_t)&arch_sink),
          [iters]"r"((uintptr_t)WALK_ITERS_BLOCK)
        : "x1","x2","x3","x9","x11","x12","x13","x14","x15","x16","x20","x30",
          "q0","q1","q2","q3","memory"
    );
}

/* =====================================================================
 * ARM_S — 32B LDP spec body (ARM_Q), but the spec WINDOW is widened by a chain of
 * K=3 CACHEABLE civac'd cold lines (NOT Normal-NC — the NC anchor empirically
 * kills speculation). Three distinct cacheable lines in kbuf_cache (offsets
 * 4096/8192/12288, clear of G@0..63 / V@64..127) are chained node0->node1->node2
 * ->&4f and civac'd EVERY iter, so the dependent chase misses to DRAM 3 times =>
 * ~3 round-trip window, entirely cacheable+flush. Terminal node = &4f (loop tail;
 * no fault). x20 callee-saved -> clobbered. Tests cacheable window-widening.
 * ===================================================================== */
static noinline void gadget_arm_s_32b_ldp_cache_chain(struct krsb_worker *w)
{
    u8 *G  = kbuf_cache + G_OFFSET;
    u8 *V  = kbuf_cache + V_OFFSET;
    u8 *n0 = kbuf_cache + 4096;
    u8 *n1 = kbuf_cache + 8192;
    u8 *n2 = kbuf_cache + 12288;
    asm volatile(
        "mov x11, %[Gp]\n\t"
        "mov x13, %[Vp]\n\t"
        "mov x12, %[iters]\n\t"
        "mov x15, %[sinkp]\n\t"
        "adr x9, 4f\n\t"
        "mov x20, %[n0]\n\t"
        "mov x4, %[n1]\n\t"
        "mov x5, %[n2]\n\t"
        "1:\n\t"
        "dc civac, x11\n\t"              /* flush G */
        "dc civac, x13\n\t"              /* flush V */
        "str x4, [x20]\n\t"             /* node0 -> node1 */
        "str x5, [x4]\n\t"              /* node1 -> node2 */
        "str x9, [x5]\n\t"             /* node2 -> &4f   */
        "dc civac, x20\n\t"             /* flush chain to DRAM so chase misses */
        "dc civac, x4\n\t"
        "dc civac, x5\n\t"
        "dsb ish\n\t"
        "mov x14, x20\n\t"
        "bl 2f\n\t"
        "3:\n\t"                         /* SPEC WINDOW */
        "ldp q0, q1, [x11, #0]\n\t"      /* spec G[0:32] */
        "ldp q2, q3, [x13, #0]\n\t"      /* spec V[0:32] (G->V toggle, 2nd instr) */
        "2:\n\t"
        "ldr x14, [x14]\n\t"             /* cacheable cold miss 1 */
        "ldr x14, [x14]\n\t"             /* miss 2 */
        "ldr x30, [x14]\n\t"             /* miss 3 -> &4f */
        "ret\n\t"
        "4:\n\t"
        "umov x16, v2.d[0]\n\t"
        "str x16, [x15]\n\t"
        "subs x12, x12, #1\n\t"
        "b.ne 1b\n\t"
        :
        : [Gp]"r"(G), [Vp]"r"(V), [n0]"r"(n0), [n1]"r"(n1), [n2]"r"(n2),
          [sinkp]"r"((uintptr_t)&arch_sink), [iters]"r"((uintptr_t)WALK_ITERS_BLOCK)
        : "x4","x5","x9","x11","x12","x13","x14","x15","x16","x20","x30",
          "q0","q1","q2","q3","memory"
    );
}

/* =====================================================================
 * ARM_T — 32B LDP spec body, but the RSB misprediction window is held open by a
 * DEPENDENT ALU DELAY of `anchor_k` iterations (~cycles) — NOT a slow DRAM anchor.
 * No anchor memory traffic, no anchor civac, no bus dead-time: the loop is tight,
 * so the spec TRIGGER RATE scales ~1/anchor_k. Sweeping anchor_k (insmod param)
 * probes the rate/issue tradeoff that the duty-cycle argument predicts:
 *   small anchor_k  -> high trigger rate (toward arch duty) IF the two G/V loads
 *                      still DISPATCH to memory before the ret resolves;
 *   too small       -> loads squashed pre-issue -> zero bus traffic.
 * x30 = &4f (= x9 + x6, with x6==0 after the delay) so the arch ret always lands
 * on 4: (loop tail); anchor_k clamped >=1 (0 would be an infinite delay). No fault.
 * ===================================================================== */
static noinline void gadget_arm_t_alu_anchor(struct krsb_worker *w)
{
    u8 *G = kbuf_cache + G_OFFSET;
    u8 *V = kbuf_cache + V_OFFSET;
    uintptr_t ak = anchor_k < 1 ? 1 : (uintptr_t)anchor_k;   /* guard infinite loop */
    asm volatile(
        "mov x11, %[Gp]\n\t"
        "mov x13, %[Vp]\n\t"
        "mov x12, %[iters]\n\t"
        "mov x15, %[sinkp]\n\t"
        "adr x9, 4f\n\t"
        "1:\n\t"
        "dc civac, x11\n\t"              /* flush G to DRAM */
        "dc civac, x13\n\t"              /* flush V to DRAM */
        "dsb ish\n\t"
        "bl 2f\n\t"
        "3:\n\t"                         /* SPEC WINDOW (RSB target) */
        "ldp q0, q1, [x11, #0]\n\t"      /* spec G[0:32] */
        "ldp q2, q3, [x13, #0]\n\t"      /* spec V[0:32] (G->V toggle, 2nd instr) */
        "2:\n\t"
        "mov x6, %[ak]\n\t"             /* dependent ALU delay holds the window */
        "6:\n\t"
        "subs x6, x6, #1\n\t"
        "b.ne 6b\n\t"                   /* ~anchor_k cycles, no memory traffic */
        "add x30, x9, x6\n\t"          /* x30 = &4f + 0; depends on the delay -> ret resolves late */
        "ret\n\t"                        /* arch -> 4:, spec(RSB) -> 3: */
        "4:\n\t"
        "umov x16, v2.d[0]\n\t"
        "str x16, [x15]\n\t"
        "subs x12, x12, #1\n\t"
        "b.ne 1b\n\t"
        :
        : [Gp]"r"(G), [Vp]"r"(V), [ak]"r"(ak),
          [sinkp]"r"((uintptr_t)&arch_sink), [iters]"r"((uintptr_t)WALK_ITERS_BLOCK)
        : "x6","x9","x11","x12","x13","x15","x16","x30",
          "q0","q1","q2","q3","memory"
    );
}

/* =====================================================================
 * ARM_U — FULL GGVV (4 LDP: G[0:32],G[32:64],V[0:32],V[32:64], all 64 bytes of G
 * then all 64 of V — identical load pattern to the architectural ARM_REF) under
 * the ALU-DELAY anchor of `anchor_k` cycles. ARM_T proved a 2-cycle window leaks
 * (32B, 2 loads); this tests whether the full-line arch pattern (4 loads, needing
 * a slightly wider window to dispatch all four before squash) also reaches arch
 * gamma. Same no-anchor-traffic, no-stall, no-fault structure as ARM_T.
 * ===================================================================== */
static noinline void gadget_arm_u_ggvv_alu_anchor(struct krsb_worker *w)
{
    u8 *G = kbuf_cache + G_OFFSET;
    u8 *V = kbuf_cache + V_OFFSET;
    uintptr_t ak = anchor_k < 1 ? 1 : (uintptr_t)anchor_k;
    asm volatile(
        "mov x11, %[Gp]\n\t"
        "mov x13, %[Vp]\n\t"
        "mov x12, %[iters]\n\t"
        "mov x15, %[sinkp]\n\t"
        "adr x9, 4f\n\t"
        "1:\n\t"
        "dc civac, x11\n\t"              /* flush G to DRAM */
        "dc civac, x13\n\t"              /* flush V to DRAM */
        "dsb ish\n\t"
        "bl 2f\n\t"
        "3:\n\t"                         /* SPEC WINDOW (RSB target) — full GGVV */
        "ldp q0, q1, [x11, #0]\n\t"      /* spec G[0:32]  */
        "ldp q2, q3, [x11, #32]\n\t"     /* spec G[32:64] (all of G) */
        "ldp q4, q5, [x13, #0]\n\t"      /* spec V[0:32]  (G->V toggle) */
        "ldp q6, q7, [x13, #32]\n\t"     /* spec V[32:64] (all of V) */
        "2:\n\t"
        "mov x6, %[ak]\n\t"             /* dependent ALU delay holds the window */
        "6:\n\t"
        "subs x6, x6, #1\n\t"
        "b.ne 6b\n\t"
        "add x30, x9, x6\n\t"          /* x30 = &4f + 0; ret resolves after the delay */
        "ret\n\t"                        /* arch -> 4:, spec(RSB) -> 3: */
        "4:\n\t"
        "umov x16, v3.d[0]\n\t"
        "str x16, [x15]\n\t"
        "subs x12, x12, #1\n\t"
        "b.ne 1b\n\t"
        :
        : [Gp]"r"(G), [Vp]"r"(V), [ak]"r"(ak),
          [sinkp]"r"((uintptr_t)&arch_sink), [iters]"r"((uintptr_t)WALK_ITERS_BLOCK)
        : "x6","x9","x11","x12","x13","x15","x16","x30",
          "q0","q1","q2","q3","q4","q5","q6","q7","memory"
    );
}

static int krsb_worker_fn(void *arg)
{
    struct krsb_worker *w = arg;
    pr_info("spec_power_p2_v11: worker %d cpu %d  arm=%d class=%d  G@+0x%lx V@+0x%lx anchor=%px\n",
            w->worker_idx, w->cpu, spec_arm, arm_buf_class(spec_arm),
            G_OFFSET, V_OFFSET, kbuf_anchor);
    while (!kthread_should_stop() && !atomic_read(&spec_quit)) {
        u8 *base;
        if (!atomic_read(&spec_run)) {
            wait_event_interruptible(spec_wait,
                atomic_read(&spec_run) || atomic_read(&spec_quit) ||
                kthread_should_stop());
            continue;
        }
        base = active_buf();
        switch (spec_arm) {
        case ARM_REF: gadget_ref(base);                          break;
        case ARM_F:   gadget_arm_f_nov(w, base);                 break;
        /* NEW v11 arms — these hardcode G/V from kbuf_cache and take the
         * ANCHOR region as their pointer argument (NOT active_buf()). */
        case ARM_G:   gadget_arm_g_bothspec_civac_once(w);  break;
        case ARM_H:   gadget_arm_h_bothspec_civac_every(w); break;
        case ARM_I:   gadget_arm_i_bothspec_every_ldnp(w);  break;
        case ARM_J:   gadget_arm_j_vfirst(w);               break;
        case ARM_K:   gadget_arm_k_multiline(w);            break;
        case ARM_L:   gadget_arm_l_bothspec_ggvv_full(w);   break;
        case ARM_M:   gadget_arm_m_bothspec_ggvv_ldp(w);    break;
        case ARM_N:   gadget_arm_n_arch_nc_noflush(base);   break;
        case ARM_O:   gadget_arm_o_chained_nc_anchor(w);    break;
        case ARM_P:   gadget_arm_p_nc_stride_anchor(w);     break;
        case ARM_Q:   gadget_arm_q_32b_ldp(w);              break;
        case ARM_R:   gadget_arm_r_32b_ldp_chained(w);      break;
        case ARM_S:   gadget_arm_s_32b_ldp_cache_chain(w);  break;
        case ARM_T:   gadget_arm_t_alu_anchor(w);           break;
        case ARM_U:   gadget_arm_u_ggvv_alu_anchor(w);      break;
        default:      gadget_ref(base);                          break;
        }
        cond_resched();
    }
    return 0;
}

/* ---------- ioctl plumbing (identical mechanism to v10) ----------
 *
 * SET_G / SET_V / SET_V_HW write into ALL THREE memtype regions so that
 * switching arms does not require re-loading G/V. Each region is flushed with
 * civac after the memcpy (harmless on NC/Device; required on cacheable so the
 * worker sees the fresh bytes and so the next civac+load actually reaches DRAM).
 *
 * NOTE: the NEW arms (G/H/I) read G/V from kbuf_cache, which write_*_all_regions
 * already populates — so the existing runner's SET_G-driven HD mechanism works
 * unchanged on the new arms.
 */

static void write_g_all_regions(const u8 *bytes)
{
    u8 *bufs[3] = { kbuf_cache, kbuf_nnc, kbuf_dev };
    int i;
    for (i = 0; i < 3; i++) {
        u8 *p = bufs[i] + G_OFFSET;
        memcpy(p, bytes, LINE_BYTES);
        asm volatile("dc civac, %0" :: "r"(p) : "memory");
    }
    asm volatile("dsb ish" ::: "memory");
}

static void write_v_all_regions(const u8 *bytes)
{
    u8 *bufs[3] = { kbuf_cache, kbuf_nnc, kbuf_dev };
    int i; unsigned long n;
    /* Replicate the pattern across all V_KLINES contiguous V lines so the
     * multi-line amplifier (ARM_K) folds K copies of the SAME HW(V); the
     * single-line arms only read line 0, so this is harmless for them. */
    for (i = 0; i < 3; i++) {
        for (n = 0; n < V_KLINES; n++) {
            u8 *p = bufs[i] + V_OFFSET + n * LINE_BYTES;
            memcpy(p, bytes, LINE_BYTES);
            asm volatile("dc civac, %0" :: "r"(p) : "memory");
        }
    }
    asm volatile("dsb ish" ::: "memory");
}

static int set_g_bytes(void __user *src)
{
    struct spec_line ln;
    if (copy_from_user(&ln, src, sizeof(ln))) return -EFAULT;
    write_g_all_regions(ln.bytes);
    return 0;
}
static int set_v_bytes(void __user *src)
{
    struct spec_line ln;
    if (copy_from_user(&ln, src, sizeof(ln))) return -EFAULT;
    write_v_all_regions(ln.bytes);
    return 0;
}
static void v_set_hw(unsigned int num_ones)
{
    static const u8 popv[9] = {0,1,3,7,0xf,0x1f,0x3f,0x7f,0xff};
    u8 tile[LINE_BYTES]; int per, rem, i;
    if (num_ones > 512) num_ones = 512;
    memset(tile, 0, sizeof(tile));
    if (num_ones > 0) {
        per = num_ones / LINE_BYTES; rem = num_ones - per * LINE_BYTES;
        for (i = 0; i < LINE_BYTES; i++) tile[i] = popv[per + (i < rem ? 1 : 0)];
    }
    write_v_all_regions(tile);
}

/* SPEC_GET_PA — physical address of an offset in kbuf_cache (read-only,
 * bounds-checked; ported from spec_power_p2_v8.c). kbuf_cache is a vmap()
 * region so vmalloc_to_page resolves it. Used to PROVE G@0 and V@64 share a
 * DRAM bank+row (same PFN) and to label address pairs for the timing probe. */
static int spec_get_pa(struct spec_pa_query __user *uq)
{
    struct spec_pa_query q; struct page *page; phys_addr_t pa;
    unsigned long page_off, in_page_off;
    if (copy_from_user(&q, uq, sizeof(q))) return -EFAULT;
    if (q.offset >= ALLOC_BYTES) return -ERANGE;
    page_off    = q.offset & ~(PAGE_SIZE - 1ul);
    in_page_off = q.offset &  (PAGE_SIZE - 1ul);
    page = vmalloc_to_page(kbuf_cache + page_off);
    if (!page) return -EFAULT;
    pa = page_to_phys(page) + in_page_off;
    q.phys_addr = pa;
    if (copy_to_user(uq, &q, sizeof(q))) return -EFAULT;
    return 0;
}

static long spec_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
    bool was_running;
    switch (cmd) {
    case SPEC_SET_V:
        was_running = atomic_xchg(&spec_run, 0); msleep(2);
        set_v_bytes((void __user *)arg);
        if (was_running) { atomic_set(&spec_run, 1); wake_up_all(&spec_wait); }
        return 0;
    case SPEC_SET_G:
        was_running = atomic_xchg(&spec_run, 0); msleep(2);
        set_g_bytes((void __user *)arg);
        if (was_running) { atomic_set(&spec_run, 1); wake_up_all(&spec_wait); }
        return 0;
    case SPEC_SET_V_HW:
        was_running = atomic_xchg(&spec_run, 0); msleep(2);
        v_set_hw((unsigned int)arg);
        if (was_running) { atomic_set(&spec_run, 1); wake_up_all(&spec_wait); }
        return 0;
    case SPEC_SET_ARM: {
        unsigned int a = (unsigned int)arg;
        if (a >= ARM_MAX) return -EINVAL;
        was_running = atomic_xchg(&spec_run, 0); msleep(2);
        spec_arm = (int)a;
        if (was_running) { atomic_set(&spec_run, 1); wake_up_all(&spec_wait); }
        return 0;
    }
    case SPEC_SET_ANCHOR_K: {
        unsigned int k = (unsigned int)arg;          /* ARM_T window length sweep */
        was_running = atomic_xchg(&spec_run, 0); msleep(2);
        anchor_k = (int)(k < 1 ? 1 : k);
        if (was_running) { atomic_set(&spec_run, 1); wake_up_all(&spec_wait); }
        return 0;
    }
    case SPEC_GET_KBUF_SIZE: {
        u64 sz = ALLOC_BYTES;
        if (copy_to_user((void __user *)arg, &sz, sizeof(sz))) return -EFAULT;
        return 0;
    }
    case SPEC_GET_PA:
        return spec_get_pa((struct spec_pa_query __user *)arg);
    case SPEC_SET_V_OFFSET: {
        u64 off;
        if (copy_from_user(&off, (void __user *)arg, sizeof(off))) return -EFAULT;
        if (off + LINE_BYTES > ALLOC_BYTES) return -ERANGE;
        v_off_dyn = (unsigned long)off;
        return 0;
    }
    case SPEC_START: atomic_set(&spec_run, 1); wake_up_all(&spec_wait); return 0;
    case SPEC_STOP:  atomic_set(&spec_run, 0); return 0;
    case SPEC_GADGET_INFO: {
        struct spec_info info = {
            .cache_va = (u64)(uintptr_t)kbuf_cache,
            .nnc_va   = (u64)(uintptr_t)kbuf_nnc,
            .dev_va   = (u64)(uintptr_t)kbuf_dev,
            .alloc_bytes = ALLOC_BYTES, .line_bytes = LINE_BYTES,
            .v_offset = V_OFFSET, .g_offset = G_OFFSET, .arm = (u32)spec_arm,
        };
        if (copy_to_user((void __user *)arg, &info, sizeof(info))) return -EFAULT;
        return 0;
    }}
    return -ENOTTY;
}

static const struct file_operations spec_fops = {
    .owner = THIS_MODULE, .unlocked_ioctl = spec_ioctl, .compat_ioctl = spec_ioctl,
};
static int major; static struct class *spec_class; static struct device *spec_dev;

static int __init spec_init(void)
{
    int err, i;
    u8 v_init[LINE_BYTES];

    if (n_workers < 1) n_workers = 1;
    if (n_workers > MAX_WORKERS) n_workers = MAX_WORKERS;
    if (spec_arm < 0 || spec_arm >= ARM_MAX) spec_arm = ARM_REF;

    /* cacheable: vmalloc => MT_NORMAL. (page-array alloc + WB vmap so all three
     * G/V regions are symmetric; PAGE_KERNEL is WB-WA == MT_NORMAL.) */
    kbuf_cache = alloc_mapped(cache_pages, PAGE_KERNEL);
    if (!kbuf_cache) { err = -ENOMEM; goto err_none; }

    /* Normal-NC: pgprot_writecombine == MT_NORMAL_NC (index 2). */
    kbuf_nnc = alloc_mapped(nnc_pages, pgprot_writecombine(PAGE_KERNEL));
    if (!kbuf_nnc) { err = -ENOMEM; goto err_cache; }

    /* Device-nGnRnE: pgprot_noncached == MT_DEVICE_nGnRnE (index 3). */
    kbuf_dev = alloc_mapped(dev_pages, pgprot_noncached(PAGE_KERNEL));
    if (!kbuf_dev) { err = -ENOMEM; goto err_nnc; }

    /* NEW: Normal-NC stride-walk anchor (speculatable, uncached, cold lines). */
    kbuf_anchor = alloc_mapped(anchor_pages, pgprot_writecombine(PAGE_KERNEL));
    if (!kbuf_anchor) { err = -ENOMEM; goto err_dev_buf; }

    /* seed a non-trivial V so a forgotten SET_V still toggles the bus */
    memset(v_init, 0, sizeof(v_init));
    v_init[0] = v_init[16] = v_init[32] = v_init[48] = 0xFF;
    write_v_all_regions(v_init);

    major = register_chrdev(0, DEVICE_NAME, &spec_fops);
    if (major < 0) { err = major; goto err_anchor; }
    spec_class = class_create(CLASS_NAME);
    if (IS_ERR(spec_class)) { err = PTR_ERR(spec_class); goto err_unreg; }
    spec_dev = device_create(spec_class, NULL, MKDEV(major, 0), NULL, DEVICE_NAME);
    if (IS_ERR(spec_dev)) { err = PTR_ERR(spec_dev); goto err_class; }

    atomic_set(&spec_run, 0); atomic_set(&spec_quit, 0);
    for (i = 0; i < n_workers; i++) {
        workers[i].cpu = 1 + i; workers[i].worker_idx = i;
        workers[i].task = kthread_create(krsb_worker_fn, &workers[i], "krsb_v11_%d", i);
        if (IS_ERR(workers[i].task)) {
            err = PTR_ERR(workers[i].task); workers[i].task = NULL; goto err_threads;
        }
        kthread_bind(workers[i].task, workers[i].cpu);
        wake_up_process(workers[i].task);
    }
    pr_info("spec_power_p2_v11: loaded %d workers arm=%d WALK=%d "
            "cache=%px nnc=%px dev=%px anchor=%px (both-spec + Spectre-v1)\n",
            n_workers, spec_arm, WALK_ITERS_BLOCK,
            kbuf_cache, kbuf_nnc, kbuf_dev, kbuf_anchor);
    return 0;

err_threads:
    atomic_set(&spec_quit, 1); wake_up_all(&spec_wait);
    for (i = 0; i < n_workers; i++) if (workers[i].task) kthread_stop(workers[i].task);
    device_destroy(spec_class, MKDEV(major, 0));
err_class:  class_destroy(spec_class);
err_unreg:  unregister_chrdev(major, DEVICE_NAME);
err_anchor: free_mapped(kbuf_anchor, anchor_pages);
err_dev_buf: free_mapped(kbuf_dev, dev_pages);
err_nnc:    free_mapped(kbuf_nnc, nnc_pages);
err_cache:  free_mapped(kbuf_cache, cache_pages);
err_none:
    return err;
}

static void __exit spec_exit(void)
{
    int i;
    atomic_set(&spec_quit, 1); atomic_set(&spec_run, 0); wake_up_all(&spec_wait);
    for (i = 0; i < n_workers; i++) if (workers[i].task) kthread_stop(workers[i].task);
    device_destroy(spec_class, MKDEV(major, 0));
    class_destroy(spec_class);
    unregister_chrdev(major, DEVICE_NAME);
    free_mapped(kbuf_anchor, anchor_pages);
    free_mapped(kbuf_dev, dev_pages);
    free_mapped(kbuf_nnc, nnc_pages);
    free_mapped(kbuf_cache, cache_pages);
}
module_init(spec_init); module_exit(spec_exit);
MODULE_LICENSE("GPL"); MODULE_AUTHOR("HomardSpec");
MODULE_DESCRIPTION("Phase 2 v11 — both-spec stride-walk + realistic Spectre-v1 cross-term");
MODULE_VERSION("11.0");
