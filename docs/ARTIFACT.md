# HOMARD — Artifact

Artifact for **“HOMARD: Hammering Off-chip Memory via Aggregate power Rail Disclosure.”**

HOMARD shows that the unprivileged, millisecond-resolution power sensors that
modern Arm SoCs already expose are enough to (i) reverse-engineer DRAM address
mapping functions and (ii) leak the *contents* of a memory block — with no
timer, no performance counter and no `CAP_SYS_*` capability. This repository
contains the measurements behind every quantitative claim in the paper, the
scripts that turn them back into the published figures and tables, and the
measurement code that produced them on three platforms.

```
make check      # verify environment + data integrity   (~5 s)
make figures    # regenerate every figure and table      (~30 s)
```

Everything runs on a stock Linux laptop with Python 3.8+ and four standard
packages. **No hardware is required to replay the results.** The target boards
are only needed to re-measure from scratch (see [`docs/HARDWARE_SETUP.md`](HARDWARE_SETUP.md)).

---

## Contents

| Path | What it holds |
|---|---|
| `data/` | Every measurement the paper reports, by platform, with provenance and checksums |
| `scripts/` | One script per figure group; `run_all.py` drives them all |
| `src/` | Measurement code: the kernel gadgets and samplers, per platform |
| `figures/` | Regenerated output lands here |
| `figures/reference/` | The exact PDFs used in the submitted paper, for side-by-side comparison |
| `docs/` | Reproduction notes, hardware setup, deviations, limitations |
| `artifact/` | Claim-to-figure map (`figure_map.csv`), badge status |

## Platforms

| Platform | SoC / DRAM | Power interface | Rail |
|---|---|---|---|
| AMD ZCU102 | Zynq UltraScale+, DDR4-2400, 4 banks | TI INA226 via Linux `hwmon`, ~28 Hz | `VCCO_PSDDR` |
| Raspberry Pi 5 | BCM2712, 16-bank LPDDR4X | DA9091 PMIC via `/dev/vcio`, ~44 Hz | `DDR_VDD2_A`, `DDR_VDDQ_A` |
| NVIDIA Jetson TX2 | Tegra186, multi-chip LPDDR4 (2 chips × 2 sub-channels × 4 banks) | INA3221 via Linux IIO, 1 kHz | `VDD_SYS_DDR` |

## Quick start

```bash
git clone <this repository> && cd Homardtefact
python3 -m pip install -r requirements.txt

make check       # Python + packages + 26 data files + SHA-256 checksums
make figures     # all 19 data-driven figures and both tables
```

Regenerate a single item:

```bash
python3 scripts/run_all.py fig03        # Figure 3, all six panels
python3 scripts/run_all.py tab02        # Table 2
python3 scripts/run_all.py --quick      # skip the Monte-Carlo step
```

Output goes to `figures/`. Set `HOMARD_FIGDIR` to write elsewhere.

## What each step reproduces

Full mapping in [`artifact/figure_map.csv`](../artifact/figure_map.csv). Headline results:

| Step | Paper item | Result you should see |
|---|---|---|
| `fig03` | Figure 3 | ZCU102 and Pi 5: AUC = 1.0 on both channels. TX2: timing collapses (46/3000 conflicts vs 188 predicted) while power resolves four levels at 252.3 / 256.3 / 259.4 / 263.7 mA with counts 177 / 795 / 509 / 1519 |
| `fig04` | Figure 4 | Hierarchy-aware recovery completes at N = 221 vs N = 476 for the binary oracle → **2.15× fewer measurements** |
| `fig05` | Figure 5 | `DDR_VDD2_A` linear in HW(V) (R² = 0.97); `DDR_VDDQ_A` shows the inverted-V DBI fold |
| `fig06` | Figures 6, 10a, 11a | Hamming-weight recovery at **81.5 %** (committed), **88.9 %** (Spectre-RSB), **85.2 %** (uncacheable), against an 11 % random baseline |
| `fig07` | Figure 7 | **+19.7 µA per toggled bit**, R² = 0.958 |
| `fig08` | Figures 8, 10b, 11b | Blind CPA: **63/64** bytes architectural, **63/64** speculative (N = 59 402), **64/64** uncacheable (N = 33 330) |
| `fig12` | Figure 12 | Per-trace saving grows with hierarchy depth: 1.5× (LPDDR4) to 6.5× (DDR5) |
| `tab01` | Table 1 | Channel co-location is the only binding placement constraint (100 % / 78 % / 56 % / fails) |
| `tab02` | Table 2 | TX2 bank selectors recovered **live** from the raw power measurements over GF(2), matching the published basis |

## Verifying against the paper

`figures/reference/` holds the PDFs from the submitted paper. After
`make figures`, compare any regenerated file against its reference of the same
name. The scripts also print the numeric result on stdout, so most claims can
be checked from the console alone without opening a PDF.

Every number in the table above was produced by running this repository from a
clean checkout. Where a regenerated figure intentionally differs from the
published one, it is listed and explained in
[`docs/REPRODUCTION.md`](REPRODUCTION.md) — please read that file before
filing a discrepancy.

## Re-measuring on hardware

`src/` contains the measurement code, not a reimplementation:

- `src/pi5/homard_gadget.c` — the kernel module implementing all three gadgets of
  Appendix C, selected by the `spec_arm` module parameter: `0` = committed
  (Listing 1), `14` = uncacheable Normal-NC (Listing 2), `21` = Spectre-RSB
  (Listing 3).
- `src/pi5/homard_sampler.c` — userspace PMIC sampler and acquisition driver.
- `src/zcu102/`, `src/tx2/` — row-conflict probes, DRAMA/Knock-Knock baselines
  and the INA sampling loops for the other two boards.

See [`docs/HARDWARE_SETUP.md`](HARDWARE_SETUP.md) for wiring, rail access
and run times. Acquisitions are long: the blind value-recovery runs take 12–34
hours per board.

## Ethics and disclosure

Findings were disclosed to the French national CERT on 9 June 2026 and to
Raspberry Pi Ltd on 10 June 2026. Disclosure to other affected vendors is
ongoing with CERT assistance. The primary mitigation — revoking unprivileged
access to PMIC registers — is discussed in Section 7.7 of the paper.

This artifact contains measurement data and the code that produced it. It
contains no exploit against a third-party system and no secret material beyond
the synthetic 64-byte blocks the experiments themselves planted.

## License

Code and data are released under the MIT License (`LICENSE`). Please cite the
paper if you build on this artifact.
