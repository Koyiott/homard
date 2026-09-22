#!/usr/bin/env python3
"""Table 1 - G/V placement vs leakage on the Pi 5.

The reference gadget places the attacker block G and the secret block V in the
same DRAM row, but that is not a requirement. This table reports the per-bit
rail swing gamma and the CPA outcome as V is moved progressively further from
G. The only binding condition is that the two share a DRAM *channel*: that is
what puts them on the same DQ bus, which is what the HD(G,V) cross-term needs.

Usage:  python3 scripts/tab01_placement.py
"""
import json
import os
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from common.style import DATA

ROOT = Path(__file__).resolve().parents[1]


def main():
    d = json.load(open(DATA / "pi5/placement_gamma.json"))
    ref = d["gamma_reference_uA_per_bit"]

    rows = []
    for p in d["placements"]:
        pct = 100.0 * p["gamma_uA_per_bit"] / ref
        rows.append((p["label"], p["gamma_uA_per_bit"], pct, p["recovery"]))

    w = max(len(r[0]) for r in rows)
    print("\nTable 1 - G/V placement vs leakage on the Pi 5")
    print("-" * (w + 42))
    print(f"{'Placement':<{w}}  {'gamma':>7}  {'vs same-row':>12}  Recovery")
    print("-" * (w + 42))
    for label, g, pct, rec in rows:
        # The different-channel row still shows a G-driven swing, but it carries
        # no per-position information about V, so the paper prints no ratio.
        shown = "---" if rec.startswith("no") else f"{pct:.0f}%"
        print(f"{label:<{w}}  {g:>5.1f}  {shown:>12}  {rec}")
    print("-" * (w + 42))
    print("\n" + d["conclusion"])

    out = Path(os.environ.get("HOMARD_FIGDIR", ROOT / "figures"))
    out.mkdir(parents=True, exist_ok=True)
    (out / "table1_placement.txt").write_text(
        "\n".join(f"{l}\t{g:.1f}\t{p:.0f}%\t{r}" for l, g, p, r in rows) + "\n")
    print(f"\n  wrote figures/table1_placement.txt")


if __name__ == "__main__":
    main()
