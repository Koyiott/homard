# Cross-placement CPA on uncacheable memory: relaxing the same-row constraint

**Board:** pi-min5 (Raspberry Pi 5, 16 GB LPDDR4X), DDR_VDD2 rail · **Date:** 2026-06-11
**Gadget:** `ARM_N_DSBSY` (arm 23) — uncacheable Normal-NC, no flush, per-iteration `dsb sy` barrier (the strongest gadget, ρ≈0.050 same-row, see [NC_BARRIER_RECOVERY.md](NC_BARRIER_RECOVERY.md)).
**Question:** must the attacker block `G` and the secret `V` sit in the *same DRAM row*, or does the HD(G,V) cross-term survive when they are in **different banks of the same channel** (a far more realistic placement)?

## What we did

The attack reads `G` then `V` back to back; the DQ-bus toggle is `HD(G,V)=Σ_i HW(g_i⊕v_i)`. Same-row colocation (G@+0, V@+64, one open row) gives the cleanest toggle. To test other placements we needed to move `V` to an arbitrary DRAM bank/row while the gadget still reads it.

**Module changes** (`src/spec_power_p2_v11.c`, rebuilt on pi-min5):
1. **Write V at `v_off_dyn`** — `write_v_all_regions` previously wrote the secret at a fixed `V_OFFSET=64`; now it writes at the runtime read offset `v_off_dyn`, so `SPEC_SET_V_OFFSET(X)` + `SPEC_SET_V(secret)` places the secret at any offset `X` the gadget then reads.
2. **`SPEC_GET_PA_NNC` ioctl** (nr 34) — resolves the physical address of an offset in `kbuf_nnc` (the Normal-NC buffer the NC arms actually drive). The pre-existing `GET_PA` only resolved `kbuf_cache`, so cross-bank/channel selection had to be done on the rail the gadget really uses.

**Pi-5 DRAM address function** (from `scripts/t8a_pa_probe.py`): bank `b3=PA[12], b2=PA[13], b1=PA[31]⊕PA[32], b0=PA[12]⊕PA[33]` (16 banks); coupling **channel = PA bit 31**. `kbuf_nnc` is 64 separately-mapped 4 KiB pages, so its pages scatter across banks and both channel halves. We map all 64 pages by this function, fix `G` at offset 0 (page 0) in the strong channel (bit31=1), and pick `V` pages by category.

## γ assessment (fast proxy, amplified swing, `scripts/crossbank_gamma.py`)

One allocation, G fixed, V swept by placement; γ = swing per toggled bit:

| V placement | γ (µA/bit) | vs same-row |
|---|---:|---:|
| Same row (V in G's row) | 27.6 | 100% |
| **Different bank, same channel** | **21.5** (avg) / **27.6** (best page) | **78% / ~100%** |
| Same bank, different row | 15.6 | 56% |
| Different channel | 17.1 | 62% |

**Key physics:** different-bank (78%) *beats* same-bank-different-row (56%). What hurts the cross-term is a **row-buffer conflict**, not "leaving the row": across different banks the controller keeps both rows open (bank parallelism) so the G→V toggle stays clean; within one bank a different row forces a close+activate on every access. So "another bank, same channel" is a *good*, realistic placement. With page selection (attacker picks a strong diff-bank page) γ matches same-row.

## Cross-bank CPA: DIFFERENT BANK, SAME CHANNEL (confirmed on hardware)

Full non-redundant CPA (`scripts/v11_cpa_crossbank.py`), real 64-distinct-byte kernel secret, independent random `G` per trace, same secret as the same-row barrier run for direct comparison.

- **Placement (confirmed):** `G` PA `0x2817d4000` → **bank 3**, `V` PA `0x38097e000` → **bank 5**, both **channel bit31 = 1**. `V` pinned at NC offset `0xa000` (page 10). Different bank, same channel, **not same row**.
- **γ = 27.6 µA/bit** (attacker selected the best diff-bank/same-channel page from 6 probed).
- **Measured per-byte correlation `ρ_byte = 0.046`** at N=5,450 traces, **19/64 bytes recovered and climbing** (noise floor 0.014, so ρ sits ~3× above it). This matches the same-row barrier ρ≈0.050 (~92%).

**Verdict: the cross-term survives across banks.** The same-row colocation is **not** required; same-channel is the only placement constraint. Projected full recovery: N(64/64) ≈ 33k × (0.050/0.046)² ≈ **~39k traces ≈ ~12 h**, on par with same-row.

## Different bank, DIFFERENT channel (the hard boundary)

`scripts/crossbank_rho_compare.py` — paired ρ calibration on ONE allocation, same secret and same random-G seed, measuring all three placements side by side (≈4.3k traces each, ARM_N_DSBSY):

| placement | V bank | V channel | γ (µA/bit) | median ρ_byte | bytes @ 4.3k | verdict |
|---|---:|---:|---:|---:|---:|---|
| same row | 0 | same | 27.1 | 0.060 | 35/64 | works (best) |
| **diff bank, same channel** | 13 | same | 22.9 | **0.037** | 12/64 | **works** |
| diff bank, **diff channel** | 8 | **diff** | 11.7 | **0.0097** | **0/64** | **fails** |

**Crossing to a different channel collapses the attack.** With V on a different channel the per-byte ρ (0.0097) sits *at* the noise floor (0.011) and recovers **0/64** — the HD(G,V) cross-term requires G and V to share the same DQ bus, which only happens within one channel. The γ probe (11.7) still shows some G-driven swing, but it carries no per-position V information, so the CPA distinguisher has nothing to lock onto.

Every **same-channel** placement keeps a clear cross-term (ρ 0.037–0.060 across banks/rows); the separate full run on another allocation corroborates the same-channel case at ρ=0.046, 19/64. The per-page coupling varies (ρ 0.037–0.046 for diff-bank/same-channel depending on which page is picked), but it always recovers.

**Bottom line: the only placement requirement is _same channel_.** Same-row is not needed; same-bank/different-row is actually the *weakest* of the working placements (row-buffer conflict); different-bank/same-channel works at near-same-row cost; and different-channel is where the attack stops.

## Artifacts

Module: `src/spec_power_p2_v11.c` (write-at-`v_off_dyn`, `GET_PA_NNC`), rebuilt `armL_build/` on pi-min5.
Scripts: `crossbank_gamma.py` (γ map), `v11_cpa_crossbank.py` (full cross-bank CPA), `crossbank_rho_compare.py` (3-way ρ).
Data: `realrun_crossbank.csv` (diff-bank/same-channel run).
