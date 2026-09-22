#!/usr/bin/env python3
"""Regenerate every data-driven figure and table in the HOMARD paper.

Each step is independent: a failure is reported and the run continues, so one
missing optional dependency cannot hide the rest of the results.

Usage:
    python3 scripts/run_all.py            # everything
    python3 scripts/run_all.py --quick    # skip the Monte-Carlo steps
    python3 scripts/run_all.py fig03 tab02
"""
import argparse
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
S = ROOT / "scripts"

# key, description, command, slow?
STEPS = [
    ("fig03", "Fig 3  - timing vs power on ZCU102 / Pi 5 / Jetson TX2",
     [sys.executable, S / "fig03_channels.py"], False),
    ("fig04", "Fig 4  - recovery probability vs measurements (TX2)",
     [sys.executable, S / "tx2_recovery" / "make_recovery_curves_figure.py"], True),
    ("fig05", "Fig 5  - Hamming-weight response of the two DRAM rails",
     [sys.executable, S / "fig05_hw_rails.py"], False),
    ("fig06", "Fig 6/10a/11a - HW templating for the three kernel gadgets",
     [sys.executable, S / "fig06_10a_11a_templates.py"], False),
    ("fig07", "Fig 7  - linear HD response of DDR_VDD2_A",
     [sys.executable, S / "fig07_hd_toggle.py"], False),
    ("fig08", "Fig 8/10b/11b - blind byte recovery with CPA",
     [sys.executable, S / "fig08_10b_11b_recovery.py"], False),
    ("fig12", "Fig 12 - HOMARD-RE genericity across hierarchy depth",
     [sys.executable, S / "tx2_recovery" / "make_hierarchy_landscape_figure.py"], False),
    ("tab01", "Tab 1  - G/V placement vs leakage on the Pi 5",
     [sys.executable, S / "tab01_placement.py"], False),
    ("tab02", "Tab 2  - recovered bank-selection functions",
     [sys.executable, S / "tab02_mapping.py"], False),
]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("steps", nargs="*", help="subset of step keys to run")
    ap.add_argument("--quick", action="store_true",
                    help="skip slow Monte-Carlo steps (fig04)")
    a = ap.parse_args()

    todo = [s for s in STEPS
            if (not a.steps or s[0] in a.steps) and not (a.quick and s[3])]
    if a.steps:
        unknown = set(a.steps) - {s[0] for s in STEPS}
        if unknown:
            sys.exit(f"unknown step(s): {', '.join(sorted(unknown))}")

    print(f"HOMARD artifact: regenerating {len(todo)} step(s)\n")
    results = []
    for key, desc, cmd, slow in todo:
        print("=" * 78)
        print(f"{key}  {desc}" + ("   [slow]" if slow else ""))
        print("=" * 78)
        t0 = time.time()
        r = subprocess.run([str(c) for c in cmd], cwd=ROOT)
        dt = time.time() - t0
        results.append((key, r.returncode == 0, dt))
        print(f"  -> {'OK' if r.returncode == 0 else 'FAILED'} in {dt:.1f}s\n")

    print("=" * 78)
    print("SUMMARY")
    print("=" * 78)
    for key, ok, dt in results:
        print(f"  {'PASS' if ok else 'FAIL'}  {key:<6} {dt:7.1f}s")
    nfail = sum(1 for _, ok, _ in results if not ok)
    print(f"\n{len(results) - nfail}/{len(results)} steps succeeded")
    figdir = ROOT / "figures"
    print(f"Figures in {figdir}")
    sys.exit(1 if nfail else 0)


if __name__ == "__main__":
    main()
