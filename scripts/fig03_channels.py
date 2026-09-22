#!/usr/bin/env python3
"""Figure 3 - Timing (top) vs power (bottom) on the three evaluation platforms.

Six panels, one per (platform, channel):

    (a) timing_zcu102  (d) zcu102_u93_best     AMD ZCU102,    DDR4-2400
    (b) timing_pi5     (e) pi5_ddr_vdd2_best   Raspberry Pi 5, LPDDR4X
    (c) tx2_drama_collapse (f) tx2_power_density  Jetson TX2,  multi-chip LPDDR4

The claim under test: on the ZCU102 and the Pi 5 both channels separate
row-buffer conflicts cleanly (AUC = 1.0), while on the Jetson TX2 the timing
populations collapse into one and only the power channel resolves the four
levels of the DRAM hierarchy.

Usage:  python3 scripts/fig03_channels.py
"""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import numpy as np
import pandas as pd
from scipy.stats import gaussian_kde

from common.style import (DATA, C_CONFLICT, C_NONCONF, C_LEVELS, PANEL,
                          save, auc, cohens_d)
import matplotlib.pyplot as plt

# tx2_knockknock_full.c issues this many accesses per timed chunk.
ACC_PER_CHUNK = 50_000
# Current thresholds (mA) separating the four TX2 hierarchy levels on
# VDD_SYS_DDR. Derived from the non-overlapping peaks of panel (f).
TX2_LEVELS = (254.0, 258.0, 262.0)
# DDR_VDD2 nominal rail voltage on the Pi 5, used to convert the sampled
# burst power (W) into the rail current (mA) plotted in the paper.
PI5_VDD2_V = 1.1


def kde(ax, v, xs, color, label):
    v = np.asarray(v, float)
    if len(v) < 3 or v.std() == 0:
        mu, sig = v.mean(), max(v.std(), 0.2)
        y = np.exp(-0.5 * ((xs - mu) / sig) ** 2) / (sig * np.sqrt(2 * np.pi))
    else:
        y = gaussian_kde(v)(xs)
    ax.plot(xs, y, color=color, lw=1.2, label=label)
    ax.fill_between(xs, y, color=color, alpha=0.30, lw=0)
    return y


def two_class(conf, noconf, xlabel, name, pad=0.10):
    """A conflict / non-conflict density panel (panels a, b, d, e)."""
    fig, ax = plt.subplots(figsize=PANEL)
    lo = min(conf.min(), noconf.min())
    hi = max(conf.max(), noconf.max())
    span = hi - lo
    xs = np.linspace(lo - pad * span, hi + pad * span, 700)
    kde(ax, noconf, xs, C_NONCONF, f"Non-conflict ({len(noconf)})")
    kde(ax, conf, xs, C_CONFLICT, f"Conflict ({len(conf)})")
    ax.set_xlim(xs[0], xs[-1])
    ax.set_ylim(bottom=0)
    ax.set_xlabel(xlabel)
    ax.set_ylabel("Density")
    ax.legend(loc="upper center")
    save(fig, name)
    a = auc(conf, noconf)
    print(f"    conflict   n={len(conf):4d}  {conf.mean():8.2f} +/- {conf.std(ddof=1):.2f}")
    print(f"    non-confl. n={len(noconf):4d}  {noconf.mean():8.2f} +/- {noconf.std(ddof=1):.2f}")
    print(f"    AUC = {a:.4f}   Cohen's d = {abs(cohens_d(conf, noconf)):.2f}")
    return a


def four_class(groups, xlabel, name, pad=0.06):
    """A four-level TX2 panel (panels c and f)."""
    fig, ax = plt.subplots(figsize=PANEL)
    allv = np.concatenate([v for _, v in groups])
    # Clip to a robust range: a handful of far outliers would otherwise stretch
    # the axis and squash the four peaks the panel exists to show.
    lo, hi = np.percentile(allv, [0.2, 99.5])
    span = hi - lo
    xs = np.linspace(lo - pad * span, hi + pad * span, 700)
    for (lbl, v), col in zip(groups, C_LEVELS):
        kde(ax, v, xs, col, f"{lbl} ({len(v)})")
    ax.set_xlim(xs[0], xs[-1])
    ax.set_ylim(bottom=0)
    ax.set_xlabel(xlabel)
    ax.set_ylabel("Density")
    ax.legend(loc="upper right")
    save(fig, name)
    for lbl, v in groups:
        print(f"    {lbl:22s} n={len(v):5d}  {v.mean():8.2f} +/- {v.std(ddof=1):.2f}")


def main():
    # ---------------- ZCU102 ----------------
    print("\n[Fig 3a] ZCU102 timing - cumulative loop latency")
    z = pd.read_csv(DATA / "zcu102/rowconflict_timing_200pairs.csv")
    pp = z.groupby("pair_id").agg(ms=("timing_ns", "mean"),
                                  conflict=("conflict", "first")).reset_index()
    pp["ms"] /= 1e6
    two_class(pp[pp.conflict == 1].ms.values, pp[pp.conflict == 0].ms.values,
              "Cumulative latency (ms)", "timing_zcu102")

    print("\n[Fig 3d] ZCU102 power - VCCO_PSDDR burst current")
    t = pd.read_csv(DATA / "zcu102/rowconflict_power_500pairs.csv")
    g = pd.read_csv(DATA / "zcu102/rowconflict_power_500pairs_gt.csv")
    b = (t[t.phase == 1].groupby("pair_id")["ina226_u93_curr"].mean()
         .reset_index().merge(g[["pair_id", "conflict"]], on="pair_id"))
    two_class(b[b.conflict == 1].ina226_u93_curr.values,
              b[b.conflict == 0].ina226_u93_curr.values,
              "VCCO_PSDDR current (mA)", "zcu102_u93_best")

    # ---------------- Raspberry Pi 5 ----------------
    print("\n[Fig 3b] Pi 5 timing - per-access latency")
    p = pd.read_csv(DATA / "pi5/rowconflict_timing_400pairs.csv")
    two_class(p[p.label == 1].timing_ns.values, p[p.label == 0].timing_ns.values,
              "Access latency (ns)", "timing_pi5")

    print("\n[Fig 3e] Pi 5 power - DDR_VDD2 burst current")
    q = pd.read_csv(DATA / "pi5/rowconflict_power_100pairs.csv")
    mA = q.ddr_burst.values * 1000.0 / PI5_VDD2_V
    two_class(mA[q.label.values == 1], mA[q.label.values == 0],
              "DDR_VDD2 current (mA)", "pi5_ddr_vdd2_best")

    # ---------------- Jetson TX2 ----------------
    kk = pd.read_csv(DATA / "tx2/power_3000pairs.csv")
    pair = kk.groupby("probe_pa").agg(mA=("mean_curr_mA", "mean"),
                                      tp=("throughput_cps", "mean")).reset_index()
    pair["ns"] = 1e9 / (pair.tp * ACC_PER_CHUNK)
    lo, mid, hi = TX2_LEVELS
    pair["cls"] = np.select(
        [pair.mA < lo, pair.mA < mid, pair.mA < hi],
        ["row_conflict", "same_subch", "same_chip"], default="diff_chip")
    names = [("row_conflict", "Row conflict"), ("same_subch", "Same sub-ch."),
             ("same_chip", "Same chip"), ("diff_chip", "Different chip")]

    print("\n[Fig 3c] Jetson TX2 timing - access latency, labelled by power level")
    four_class([(lbl, pair[pair.cls == k].ns.values) for k, lbl in names],
               "Access latency (ns)", "tx2_drama_collapse")

    print("\n[Fig 3f] Jetson TX2 power - VDD_SYS_DDR current")
    four_class([(lbl, pair[pair.cls == k].mA.values) for k, lbl in names],
               "VDD_SYS_DDR current (mA)", "tx2_power_density")

    # Cross-check against the binary DRAMA timing oracle of prior work.
    d = pd.read_csv(DATA / "tx2/drama_timing_3000pairs.csv")
    n_conf = int(d.is_conflict.sum())
    print(f"\n[cross-check] DRAMA binary timing oracle on the same 3000 pairs: "
          f"{n_conf} conflicts detected ({n_conf/len(d):.2%}); "
          f"a 16-bank mapping predicts ~{len(d)//16} (6.25%).")


if __name__ == "__main__":
    main()
