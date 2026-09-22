# Badges requested

We request **Artifacts Available**, **Artifacts Functional** and **Results
Reproduced**.

## Artifacts Available

The artifact will be deposited on Zenodo under a permanent DOI and mirrored in
a public Git repository, both released after evaluation. It contains no
proprietary component. Nothing here is under embargo: disclosure to the French
national CERT (9 June 2026) and to Raspberry Pi Ltd (10 June 2026) preceded
submission, and disclosure to the remaining vendors is proceeding with CERT
assistance.

## Artifacts Functional

*Documented.* `README.md` states what the artifact does and what each step
reproduces. `INSTALL.md` covers setup and troubleshooting, `REQUIREMENTS.md`
the environment, `docs/HARDWARE_SETUP.md` the measurement rigs, and
`docs/REPRODUCTION.md` explains how each result is replayed and lists every
deviation from the published figures. `data/_meta/PROVENANCE.tsv` maps every
data file to the experiment that produced it.

*Consistent.* Every quantitative claim in the paper that rests on a figure or
table is traceable through `artifact/figure_map.csv` to a script and a data
file in this repository.

*Complete.* All 19 data-driven figures and both tables regenerate. The three
remaining figures are drawn schematics, preserved as static assets. The
measurement code for all three platforms is included, not just the analysis.
The two places where a published figure cannot be replayed bit-for-bit are
stated plainly in `docs/REPRODUCTION.md` rather than papered over.

*Exercisable.* `make check` validates the environment and all 26 data files
against recorded SHA-256 checksums. `make figures` runs the full set in about
30 seconds on a stock laptop, with no hardware, no root and no network. Each
step is independently runnable and prints its numeric result to stdout, so most
claims can be checked without opening a PDF.

## Results Reproduced

The following are reproduced from the committed measurements. Each is printed
on stdout by the step that produces it.

| Claim (paper) | Reproduced value |
|---|---|
| ZCU102 and Pi 5 separate conflicts on both channels (§5.4) | AUC = 1.0 on all four panels |
| ZCU102 power clusters (§5.4) | 188.44 mA (125 conflicts) vs 192.65 mA (375 non-conflicts) |
| TX2 timing collapses (§5.4) | 46 conflicts detected of ~188 predicted over 3000 pairs |
| TX2 power resolves four levels (§5.5) | 252.3 / 256.3 / 259.4 / 263.7 mA; 177 / 795 / 509 / 1519 pairs |
| Hierarchy-aware recovery is 2.15× cheaper (§5.5, Fig. 4) | N = 221 vs N = 476 → 2.15× |
| VDD2 linear in HW(V) (§6.1) | R² = 0.97 |
| VDDQ shows the DBI fold (§6.1) | inverted V peaking near HW = 256 |
| Hamming-weight recovery, committed (§6.3) | 81.5 % exact at N = 160 |
| HD response of VDD2 (§6.4, Fig. 7) | +19.7 µA/bit, R² = 0.958 |
| Blind value recovery, committed (§6.4) | 63/64 bytes at rank 1 |
| Hamming-weight recovery under Spectre-RSB (§6.5) | 88.9 % exact |
| Blind value recovery under Spectre-RSB (§6.5) | 63/64 bytes at N = 59 402 |
| Uncacheable memory, weight and value (§7.6) | 85.2 % exact; 64/64 bytes at N = 33 330 |
| Portability to a 2 GB board (§7.3) | 47/64 at rank 1, all 64 at rank ≤ 5, residual 2^24 |
| Placement constraint is the channel (§7.4, Tab. 1) | 100 % / 78 % / 56 %; different channel fails at 0/64 |
| Recovered bank functions (Appendix A, Tab. 2) | TX2 selectors recovered live over GF(2), matching the published basis |
| Genericity across hierarchy depth (Appendix B, Fig. 12) | 1.5× (LPDDR4) to 6.5× (DDR5) |

**Scope.** These are reproduced by replaying the recorded measurements, which
is what can be done without the three boards. Re-acquiring the measurements
requires the hardware in `docs/HARDWARE_SETUP.md` and 12–34 hours per blind
value-recovery run. We are glad to arrange access to a Raspberry Pi 5 rig for
reviewers who want to exercise the acquisition path end to end.
