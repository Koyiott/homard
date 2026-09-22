# Reproduction notes

This file records exactly what each figure is regenerated from, and — more
importantly — **every place where the regenerated output is not a bit-identical
replay of the published figure**. Please read the deviations before reporting a
discrepancy.

## How the results are replayed

The artifact replays results at two different levels, and the distinction
matters when judging what has actually been reproduced.

**Level A — recomputed from raw measurements.** The script reads the per-pair
or per-trace measurements and redoes the analysis (classification, GF(2) null
space, template fit, regression, Monte-Carlo). A bug in the analysis would show
up here.

| Item | Raw input |
|---|---|
| Figure 3 (all six panels) | per-pair latency and rail current, 3 platforms |
| Figure 4 | 3000-pair TX2 power set, 400 Monte-Carlo trials |
| Figure 5 | 14 850-row HW(V) rail sweep |
| Figures 6, 10a, 11a | 5 415–5 420-row joint-rail template sets |
| Figure 7 | 420-row HD sweep with interleaved drift anchors |
| Table 2 | 3000-pair TX2 power set, GF(2) null space recovered live |

**Level B — replayed from cached per-N curves.** The blind CPA runs are long
(12–34 h each) and their per-trace inputs are tens of gigabytes. What is
committed is the derived recovery curve: bytes recovered at rank 1 as a
function of trace count, on the grid the paper plots. The figure is redrawn
from that curve; the CPA itself is not rerun.

| Item | Cached input |
|---|---|
| Figure 8, Figure 10b | `data/pi5/cpa_recovery_spec_vs_arch.json` |
| Figure 11b | `data/pi5/cpa_recovery_uncacheable.json` |
| Section 7.3 portability run | `data/pi5/cpa_blind_*.json` |
| Table 1 | `data/pi5/placement_gamma.json` |

To reproduce a Level-B item from scratch you need the board and the acquisition
time; `src/pi5/` contains the code that does it.

---

## Deviations from the published figures

### 1. Figure 8 — the Model overlay is plotted for the 2 GB board

**What the paper shows.** Figure 8 plots per-byte success probability against
trace count for the blind CPA on the 16 GB Pi 5, with a fitted Model curve
beside the Measured one, reaching 63/64 bytes.

**What happened.** The model overlay needs the per-byte correlation ρ_true,
which is computed from the per-trace CSV. For the 16 GB run that CSV was written
to `/tmp` and did not survive. The derived recovery curve did survive.

**What the artifact does.** Two files, both honest:

- `figures/cpa_recovery_arch.pdf` — the measured 16 GB architectural curve,
  reaching **63/64 bytes**. This is the headline claim of Figure 8 and it is
  fully backed by committed data.
- `figures/realvalue_success_vs_traces.pdf` — Figure 8 in its published
  Model-vs-Measured form, plotted for the **2 GB portability run of Section
  7.3**, the one blind run where both series were cached. It reaches 47/64 at
  rank 1 over 112 279 traces, with all 64 bytes at rank ≤ 5 and a residual key
  rank of 2^24.

**Residual mismatch.** The paper's text puts 63/64 at N ≈ 44.5 k traces. The
committed curve is sampled on a coarser logarithmic grid and first reaches
63/64 at its final point, N = 64 515. The endpoint claim (63/64, one byte at
rank 2) reproduces; the exact trace count at which the 63rd byte lands does not,
because the grid resolution does not permit it.

### 2. Figure 3e — the Pi 5 power panel is converted from power to current

The committed export for this run stores DDR_VDD2 **burst power** in watts; the
paper's x-axis is rail **current** in mA. The script converts at the nominal
1.1 V VDD2 rail voltage, giving 88.70 ± 0.79 mA (conflict) and 102.74 ± 1.74 mA
(non-conflict) over the 50/50 pair set that matches the figure legend.

The paper's *text* quotes 87.99 ± 1.96 and 102.92 ± 2.47 mA over a 240-pair set.
That extended set is also committed
(`data/pi5/rowconflict_power_240pairs.csv`); its standard deviations reproduce
**exactly** (1.96 and 2.47), confirming it is the same distribution. The means
differ by roughly a constant because the original figure used a directly
sampled current channel that the power-only export does not carry. Separation
and AUC = 1.0 are unaffected.

### 3. Figures 1, 2a, 2b — static schematics

`DRAMorgScheme.pdf`, `TH_PI_SCU.pdf` and `tx2_architecture_fix.pdf` are drawn
diagrams, not plots of measurements. They are preserved verbatim in
`figures/schematics/`. Figure 9 (the Spectre-RSB gadget flow) is drawn in LaTeX
in the paper source and has no separate file.

### 4. Table 1 — reported from run logs, not raw CSVs

The placement sweep (γ probe and paired ρ comparison) was run interactively on
the Pi 5 and its per-trace outputs were not retained. The measured values are
committed in `data/pi5/placement_gamma.json` and the full narrative record of
those runs, including physical addresses and bank/channel assignments, is in
[`CROSSBANK_CPA.md`](CROSSBANK_CPA.md). The ratio for “same bank, different
row” prints as 57 % against the paper's 56 % — this is rounding of 15.6 / 27.6,
not a different measurement.

### 5. Figure 3c — panel shape

The TX2 timing panel is a kernel-density estimate over the same 3000 pairs the
paper uses, labelled by power level. Bandwidth selection is automatic
(Scott's rule), so peak heights differ cosmetically from the published render.
The claim the panel carries — that the four power classes are *not* separable
in timing, and that the binary DRAMA oracle sees only 46 of the ~188 predicted
conflicts — reproduces exactly.

### 6. Table 2 — ZCU102 and Pi 5 rows are asserted, TX2 is recovered live

The Jetson TX2 bank selectors are recovered by the artifact from the raw
3000-pair power measurements: the power oracle selects the row-conflict pool,
the pool is restricted to ker(*c*, *s*), and the selectors are read off as the
GF(2) null space. The script verifies the result against the recorded basis and
prints `matches the recorded rank-2 basis: True`.

The ZCU102 and Pi 5 rows are reported from their own recovery runs; their
per-pair address sets are not committed here, so those two rows are printed as
recorded values rather than recomputed.

---

## Environment

Developed and verified on Ubuntu with Python 3.12.3, numpy 1.26.4,
pandas 2.3.0, matplotlib 3.9.2, scipy 1.15.3. `make check` enforces the
minimum versions in `requirements.txt` and validates all 26 data files against
`data/_meta/SHA256SUMS`.

Full regeneration takes about 30 seconds and needs roughly 200 MB of RAM. No
network access, no GPU and no privileged operation is involved.

## Data provenance

`data/_meta/PROVENANCE.tsv` maps every committed file to the experiment that
produced it and the figure it feeds. Nothing in `data/` is synthetic — every
file is the recorded output of a run on real hardware.
