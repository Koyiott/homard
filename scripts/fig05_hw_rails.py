#!/usr/bin/env python3
"""Figure 5 - Hamming-weight response of the two Pi 5 DRAM rails.

Sweeps the target line's Hamming weight HW(V) from 0 to 512 and reads both
rails the PMIC exposes:

    (a) DDR_VDD2_A  ~1.1 V core-array supply -> rises linearly, R^2 ~ 0.98,
        one fixed current increment per set bit.
    (b) DDR_VDDQ_A  ~0.6 V DQ I/O supply     -> inverted V: climbs to
        HW ~ 256 then falls back toward all-ones. This fold is the JEDEC
        LPDDR4 Data Bus Inversion (DBI) signature; it appears on the bus
        rail and not on the core rail because DBI inverts bus bits while
        the array drives the un-inverted data.

Both plots use amplification: the target line is replicated across contiguous
cache lines and read repeatedly per trigger.

Usage:  python3 scripts/fig05_hw_rails.py
"""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import numpy as np
import pandas as pd

from common.style import DATA, PANEL, save
import matplotlib.pyplot as plt

RAILS = [
    ("DDR_VDD2_A_curr_mA", r"$\Delta$ VDD2 (mA)", "pi_hw_rail_vdd2", "#c1272d", "o"),
    ("DDR_VDDQ_A_curr_mA", r"$\Delta$ VDDQ (mA)", "pi_hw_rail_vddq", "#2e5c8a", "s"),
]


def main():
    df = pd.read_csv(DATA / "pi5/hw_sweep_rails_calib.csv")
    for col, ylabel, name, color, marker in RAILS:
        g = df.groupby("num_ones")[col].agg(["mean", "std", "count"])
        hw = g.index.values.astype(float)
        # Report each rail as a delta against its own HW = 0 baseline.
        delta = g["mean"].values - g["mean"].values[0]
        sem = g["std"].values / np.sqrt(g["count"].values)

        fig, ax = plt.subplots(figsize=PANEL)
        ax.errorbar(hw, delta, yerr=sem, color=color, marker=marker, ms=2.2,
                    lw=0.9, elinewidth=0.6, capsize=1.2, zorder=3)
        ax.set_xlabel("Hamming weight HW($v$)")
        ax.set_ylabel(ylabel)
        ax.set_xlim(-10, 522)
        ax.set_xticks([0, 128, 256, 384, 512])
        save(fig, name)

        if "VDD2" in col:
            sl, ic = np.polyfit(hw, delta, 1)
            pred = sl * hw + ic
            r2 = 1 - ((delta - pred) ** 2).sum() / ((delta - delta.mean()) ** 2).sum()
            print(f"    VDD2 linear fit: {sl*1000:.1f} uA/bit, R^2 = {r2:.3f}")
        else:
            pk = int(hw[int(np.argmax(delta))])
            print(f"    VDDQ peak at HW = {pk} (DBI fold), "
                  f"max {delta.max():.1f} mA, all-ones {delta[-1]:.1f} mA")


if __name__ == "__main__":
    main()
