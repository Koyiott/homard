# Measurement code

This is the code that produced the data in `data/`, not a reimplementation.
None of it is needed to replay the figures — see the top-level `README.md` for
that. Use it to re-acquire on hardware; `docs/HARDWARE_SETUP.md` has the wiring
and run instructions.

## `pi5/` — HOMARD-Data (Sections 6, 7)

| File | Role |
|---|---|
| `homard_gadget.c` | Kernel module holding all three gadgets of Appendix C. Selected by the `spec_arm` module parameter: `0` committed (Listing 1), `14` uncacheable Normal-NC (Listing 2), `21` Spectre-RSB (Listing 3). Also exposes the `GET_PA` ioctls used to confirm bank/row/channel placement. |
| `build.sh` | Builds the module against the running kernel. |
| `homard_sampler.c` | Userspace PMIC sampler: drives the stimulus and reads the DA9091 rails through `/dev/vcio`. |
| `spec_v11_hw_template.py` | Hamming-weight template acquisition (Figures 6, 10a, 11a). |
| `block_hd_v11ref_sweep.py` | HD(G,V) sweep with interleaved drift anchors (Figure 7). |
| `v11_cpa_realvalue.py` | Blind 64-byte value recovery by CPA (Figures 8, 10b, 11b). |
| `spec_v11_cpa_recovery.py` | CPA analysis for the speculative arm. |
| `v11_layout_probe.py` | Resolves physical addresses and picks G/V placements by bank and channel (Table 1). |

The gadget writes the loaded data to a sink to defeat dead-code elimination, so
the `G→V` bus transition is actually issued. The committed and uncacheable arms
differ only in how the transfer reaches DRAM: an explicit `dc civac` flush
versus a Normal non-cacheable mapping where every load reaches DRAM on its own.

## `zcu102/` — HOMARD-RE and HD probe

| File | Role |
|---|---|
| `zcu102_rowconflict_probe.c` | Row-conflict pair probe with INA226 co-sampling (Figures 3a, 3d). |
| `zcu102_ddr_probe.c` | Rail reachability and DRAM-reach verification. |
| `zcu102_hd_line.c` | HD(G,V) line gadget ported to the ZCU102. |
| `zcu102_hd_cpa.py` | CPA analysis for the ZCU102 HD runs. |

## `tx2/` — HOMARD-RE on multi-chip LPDDR4

| File | Role |
|---|---|
| `tx2_knockknock_full.c` | 3000-pair probe with INA3221 power and throughput (Figures 3c, 3f, 4, Table 2). |
| `tx2_drama.c` | DRAMA row-conflict timing baseline, the prior-work oracle HOMARD is compared against. |
| `tx2_ina_burst.c` | INA3221 burst sampling loop. |
| `tx2_sudoku.c` | Sudoku-style timing baseline. |
| `tx2_chipselect.c` | Chip-selector isolation experiment. |

Build with `make` in each directory. The Jetson needs 90 s of rail warm-up
before acquiring; see `docs/HARDWARE_SETUP.md`.
