# Data

Every file here is the recorded output of a run on real hardware. Nothing is
synthetic or simulated.

| Directory | Platform |
|---|---|
| `zcu102/` | AMD ZCU102, Zynq UltraScale+, DDR4-2400 |
| `pi5/` | Raspberry Pi 5, BCM2712, LPDDR4X |
| `tx2/` | NVIDIA Jetson TX2, Tegra186, multi-chip LPDDR4 |
| `_meta/` | Provenance table and SHA-256 checksums |

`_meta/PROVENANCE.tsv` maps each file to the experiment that produced it and
the figure it feeds. `_meta/SHA256SUMS` is verified by `make check`.

## Conventions

- Rail currents are in **mA**, rail power in **W**, latencies in **ns** unless a
  column name says otherwise.
- `conflict` / `label` / `is_conflict` = 1 means the probed address pair hits a
  row-buffer conflict (same bank, different row).
- `num_ones` is the Hamming weight HW(*v*) of the 64-byte target line, so it
  ranges 0–512. `hd_total` is the Hamming distance HD(*G*,*V*) over the same
  range.
- `is_anchor` = 1 marks a drift-reference trace, excluded from the signal means
  and used to fit the drift model that is subtracted from the rest.
- Pi 5 rail columns are named `<RAIL>_curr_mA`, e.g. `DDR_VDD2_A_curr_mA`.
- ZCU102 INA226 columns are named by board designator, e.g. `ina226_u93_curr`
  is `VCCO_PSDDR`.

## A note on size

The blind CPA runs produced tens of gigabytes of per-trace data. What is
committed here is the derived per-N recovery curve rather than the raw traces —
see `docs/REPRODUCTION.md` for which figures that affects and why.
