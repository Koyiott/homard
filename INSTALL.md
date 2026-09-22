# Installation

Replaying every figure and table needs a stock Linux (or macOS) machine with
Python 3.8 or newer. No hardware, no root, no network.

## 1. Dependencies

```bash
python3 -m pip install -r requirements.txt
```

Four packages: `numpy`, `pandas`, `matplotlib`, `scipy`. Debian/Ubuntu users
can instead use distribution packages:

```bash
sudo apt install python3-numpy python3-pandas python3-matplotlib python3-scipy
```

A virtual environment works and is recommended if you want isolation:

```bash
python3 -m venv .venv && . .venv/bin/activate
pip install -r requirements.txt
```

## 2. Verify

```bash
make check
```

This confirms the Python version, the four packages and their minimum versions,
that all 26 data files are present, and that each matches its recorded SHA-256.
It should end with `Environment OK - run 'make figures'.`

## 3. Run

```bash
make figures
```

About 30 seconds. Output lands in `figures/`; the reference PDFs from the paper
sit alongside in `figures/reference/`.

## Troubleshooting

**`ModuleNotFoundError`** — the packages went to a different interpreter than
the one `make` uses. Run `make check` to see which interpreter is selected, or
override it: `make figures PY=/path/to/python3`.

**Font warnings** (`findfont: Font family 'cursive' not found`, `Unable to
import Axes3D`) — harmless. Figures use a serif family with automatic
fallback; output is unaffected.

**A step fails but others pass** — `run_all.py` isolates each step, so a single
failure never hides the rest. Rerun just that step for the full traceback, e.g.
`python3 scripts/run_all.py fig07`.

**Checksum mismatch** — a data file was modified. Restore it from a clean
checkout; the analysis assumes the committed measurements are unaltered.

## Re-measuring on hardware

See [`docs/HARDWARE_SETUP.md`](docs/HARDWARE_SETUP.md). Building the Pi 5
kernel module requires kernel headers for the running kernel; the ZCU102 and
Jetson probes are plain userspace C built with `make` in their directories.
