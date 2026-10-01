<p align="center">
  <img src="assets/homard_logo.png" alt="HOMARD logo" width="240">
</p>

<h1 align="center">HOMARD</h1>

<p align="center">
  <b>Hammering Off-chip Memory via Aggregate power Rail Disclosure</b><br>
  Unprivileged, timer-free DRAM reverse-engineering and data leakage from power telemetry on Arm SoCs.
</p>

---

HOMARD shows that the millisecond-resolution power sensors modern Arm SoCs already
expose to userspace are enough to (i) reverse-engineer DRAM address-mapping functions
and (ii) leak the *contents* of a memory block — with no timer, no performance counter
and no `CAP_SYS_*` capability. This repository holds the measurements behind every
quantitative claim in the paper, the scripts that replay them into the published
figures and tables, and the measurement code for all three platforms (AMD ZCU102,
Raspberry Pi 5, NVIDIA Jetson TX2).

- 📄 **Read the paper:** HOMARD, *IEEE Symposium on Security and Privacy (S&P) 2026* — DOI: `10.1109/SP.XXXXX` *(added once published on IEEE Xplore)*
- 🌐 **Learn more:** <https://equere.fr/homard>

## Quick start

No hardware is required to replay the results — a stock Linux laptop with Python 3.8+
and four standard packages is enough.

```bash
python3 -m pip install -r requirements.txt
make check      # verify environment + data integrity   (~5 s)
make figures    # regenerate every figure and table      (~30 s)
```

## Where to go next

| | |
|---|---|
| **Full artifact guide** | [`docs/ARTIFACT.md`](docs/ARTIFACT.md) — what each step reproduces, claim-to-figure map |
| **Setup & troubleshooting** | [`INSTALL.md`](INSTALL.md), [`REQUIREMENTS.md`](REQUIREMENTS.md) |
| **Reproduction notes & deviations** | [`docs/REPRODUCTION.md`](docs/REPRODUCTION.md) |
| **Measurement rigs** | [`docs/HARDWARE_SETUP.md`](docs/HARDWARE_SETUP.md) |

## License

- **Code** (`scripts/`, `src/`, build files): MIT — see [`LICENSE`](LICENSE)
- **Data & figures** (`data/`, `figures/`): CC BY 4.0 — see [`LICENSE-data`](LICENSE-data)

## Citation

Please cite the IEEE S&P 2026 paper. A `CITATION` entry with the final BibTeX will be
added here once the DOI is assigned.

---

<p align="center">
  <img src="assets/sushi_team_logo.png" alt="SUSHI team" width="90"><br>
  <sub>I'm in the <b>SUSHI</b> team!</sub>
</p>
