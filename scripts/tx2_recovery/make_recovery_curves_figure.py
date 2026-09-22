#!/usr/bin/env python3
"""
make_recovery_curves_figure.py
==============================

Standalone, conference-quality figure: empirical vs theoretical recovery
curves for the hierarchy-aware GF(2) null-space recovery on Jetson TX2
LPDDR4 (kk_large dataset, 9000 anchor/probe pairs).

Outputs:
    figures/fig_recovery_curves.{pdf,png}
"""
from __future__ import annotations

import json
from pathlib import Path

import numpy as np
import pandas as pd

# --- HOMARD artifact: resolve data/output relative to the repository root ---
import os as _os
from pathlib import Path as _P
_ROOT = _P(__file__).resolve().parents[2]
_DATA = _ROOT / "data"
_OUT  = _P(_os.environ.get("HOMARD_FIGDIR", _ROOT / "figures"))
_OUT.mkdir(parents=True, exist_ok=True)

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

# ---- shared with make_hierarchy_recovery_figure.py --------------------------
from make_hierarchy_recovery_figure import (
    Lattice, loadkk_clean, trial_completions,
    theory_binary_eps, theory_hierarchy_eps,
    N_BITS, N_TRIALS, MAX_N,
)

OUT_PDF = _OUT / "fig_recovery_curves.pdf"
OUT_PNG = _OUT / "fig_recovery_curves.png"


def main():
    # X axis stops earlier for breathing room
    x_max = 600
    n_grid = np.unique(np.round(np.geomspace(20, x_max, 36)).astype(int))

    print("Loading kk_large per-rep ...")
    df = loadkk_clean()
    g = df.reset_index(drop=True)
    in_kernel = (g["c_val"].values == 0) & (g["s_val"].values == 0)
    g_restricted = g[in_kernel].reset_index(drop=True)

    print("Running {} Monte-Carlo trials (max N = {}) ...".format(N_TRIALS, MAX_N))
    rng = np.random.default_rng(20260428)
    n_h_arr, n_b_arr, n1_arr, n2_arr = [], [], [], []
    for t in range(N_TRIALS):
        out = trial_completions(g, g_restricted, MAX_N, rng)
        n_h_arr.append(out["n_h"])
        n_b_arr.append(out["n_b"])
        n1_arr.append(out["n1"])
        n2_arr.append(out["n2"])
    n_h_arr = np.array([v if v is not None else np.nan for v in n_h_arr])
    n_b_arr = np.array([v if v is not None else np.nan for v in n_b_arr])
    n1_arr  = np.array([v if v is not None else np.nan for v in n1_arr])
    n2_arr  = np.array([v if v is not None else np.nan for v in n2_arr])

    def cdf_at(arr, N):
        a = arr[~np.isnan(arr)]
        return float((a <= N).sum()) / len(arr) if len(arr) else 0.0

    pH = np.array([cdf_at(n_h_arr, N) for N in n_grid])
    pB = np.array([cdf_at(n_b_arr, N) for N in n_grid])
    p1 = np.array([cdf_at(n1_arr,  N) for N in n_grid])
    p2 = np.array([cdf_at(n2_arr,  N) for N in n_grid])

    # Compare at full recovery (Pr = 1.0): the smallest N at which the
    # empirical CDF reaches 1.0 over all trials.
    n100_h = float(np.nanmax(n_h_arr))
    n100_b = float(np.nanmax(n_b_arr))
    print("  hierarchy N_100 : {:.0f}".format(n100_h))
    print("  binary    N_100 : {:.0f}".format(n100_b))
    print("  speedup at 1.0  : {:.2f}x".format(n100_b / n100_h))

    # Theory
    n_smooth = np.linspace(20, x_max, 600)
    p_b_theory = np.clip(1.0 - theory_binary_eps(n_smooth),    0.0, 1.0)
    p_h_theory = np.clip(1.0 - theory_hierarchy_eps(n_smooth), 0.0, 1.0)

    # ---------------- figure ------------------
    # Canonical paper style (Times serif, matches the other figures). This is a
    # full-column figure: a 10.4 in canvas printed at width=\linewidth (~3.4 in
    # column), i.e. downscale ~0.33. Source fonts are sized so the printed text
    # lands at the same ~8-9 pt as the half-column subfigures (which use ~30 pt
    # source at 6 in / 0.49 col): 26*(0.49/6.0)*10.4 ~ 22 pt ticks, ~25 pt labels.
    plt.rcParams.update({
        "font.family": "serif",
        "font.serif": ["Times New Roman", "Liberation Serif", "DejaVu Serif"],
        "font.size": 22,
        "axes.titlesize": 25,
        "axes.labelsize": 25,
        "xtick.labelsize": 22,
        "ytick.labelsize": 22,
        "legend.fontsize": 18,
        "axes.linewidth": 1.5,
        "xtick.major.width": 1.2,
        "ytick.major.width": 1.2,
        "xtick.major.size": 6.0,
        "ytick.major.size": 6.0,
        "axes.grid": False,
        "axes.spines.top": False,
        "axes.spines.right": False,
        "pdf.fonttype": 42,
        "ps.fonttype": 42,
    })
    C_HIER     = "#1b9e77"
    C_BIN      = "#c1272d"
    C_THEORY_H = "#0d6e54"
    C_THEORY_B = "#7d1416"
    # Stage 1 (chip only) and Stage 2 (chip+sub-ch) curves were drawn in
    # pale shades that disappeared next to the bolder theory/empirical
    # lines; saturate them so the progressive recovery is legible.
    C_S1       = "#4a3a9c"  # was #7570b3 -- deeper purple for stage 1
    C_S2       = "#0b3d8e"  # was #386cb0 -- deeper blue  for stage 2
    C_ANN      = "#222244"

    fig, ax = plt.subplots(figsize=(10.4, 6.2))
    fig.subplots_adjust(left=0.095, right=0.985, top=0.985, bottom=0.245)

    # Intermediate stage curves (thin, dotted, in back) -- show the
    # progressive recovery of the chip selector c, then chip+sub-ch (c, s),
    # before the final bank stage that completes the full hierarchy.
    ax.plot(n_grid, p1, color=C_S1, lw=1.4, ls=(0, (1.5, 2.0)), alpha=0.9,
            label=r"stage 1: chip $c$")
    ax.plot(n_grid, p2, color=C_S2, lw=1.4, ls=(0, (1.5, 2.0)), alpha=0.9,
            label=r"stages 1+2: chip $c$, sub-ch $s$")

    # Theory bounds (dashed)
    ax.plot(n_smooth, p_h_theory, color=C_THEORY_H, lw=1.8, ls=(0, (5, 3)),
            label="Hierarchy-aware  (theory)")
    ax.plot(n_smooth, p_b_theory, color=C_THEORY_B, lw=1.8, ls=(0, (5, 3)),
            label="Binary same-bank  (theory)")

    # Empirical curves (solid with markers)
    ax.plot(n_grid, pH, color=C_HIER, lw=2.8, marker="o", ms=7.5, mfc="white",
            mew=1.7, label="stage 3: complete hierarchy-aware")
    ax.plot(n_grid, pB, color=C_BIN,  lw=2.8, marker="s", ms=7.5, mfc="white",
            mew=1.7, label="Binary same-bank  (empirical)")

    # Guide-lines at full recovery (worst-case N over all trials)
    ax.axvline(n100_h, color=C_HIER, lw=1.0, alpha=0.45)
    ax.axvline(n100_b, color=C_BIN,  lw=1.0, alpha=0.45)

    # ---- top callout band: N_Pr=1 labels + speedup arrow + text ----------
    speedup = n100_b / n100_h
    y_n100  = 1.04
    y_arrow = 1.205
    y_text  = 1.25

    # N_Pr=1 labels: anchor both inside the axes so the right label is
    # never clipped, and put them on opposite sides of each guide-line.
    ax.text(n100_h * 0.965, y_n100,
            r"$N_{\Pr=1}{=}\mathbf{" + "{:.0f}".format(n100_h) + r"}$",
            color=C_HIER, ha="right", va="bottom",
            fontsize=22, weight="bold")
    ax.text(n100_b * 0.97, y_n100,
            r"$N_{\Pr=1}{=}\mathbf{" + "{:.0f}".format(n100_b) + r"}$",
            color=C_BIN, ha="right", va="bottom",
            fontsize=22, weight="bold")

    ax.annotate("", xy=(n100_h, y_arrow), xytext=(n100_b, y_arrow),
                arrowprops=dict(arrowstyle="<->", color=C_ANN,
                                lw=1.8, shrinkA=3, shrinkB=3))
    text_x = np.sqrt(n100_h * n100_b)
    ax.text(text_x, y_text,
            r"$\mathbf{{{:.2f}\times}}$".format(speedup) + " fewer measurements",
            ha="center", va="bottom",
            fontsize=24, weight="bold", color=C_ANN)

    ax.set_xscale("log")
    ax.set_xlim(20, x_max)
    ax.set_ylim(-0.02, 1.47)
    ax.set_xlabel(r"Total measurements  $N$  (probe pairs)")
    ax.set_ylabel(r"$\Pr[\text{full recovery}]$")

    # Hide tick labels above 1.0 so the callout band stays clean
    ax.set_yticks([0.0, 0.2, 0.4, 0.6, 0.8, 1.0])

    # Legend placed BELOW the chart in a single 3-column row so it never
    # overlaps the curves or the top callout band.
    handles, labels = ax.get_legend_handles_labels()
    order = [4, 5, 2, 3, 0, 1]  # H-emp, B-emp, H-theory, B-theory, stage1, stage2
    ax.legend([handles[i] for i in order], [labels[i] for i in order],
              frameon=False, loc="upper center",
              bbox_to_anchor=(0.5, -0.18), ncol=2,
              fontsize=18, handlelength=2.4,
              handletextpad=0.6, labelspacing=0.5,
              columnspacing=2.2, borderaxespad=0.0)
    ax.grid(True, which="major", axis="y", alpha=0.18, lw=0.5)
    ax.set_axisbelow(True)

    fig.savefig(OUT_PDF, bbox_inches="tight", pad_inches=0.05)
    fig.savefig(OUT_PNG, bbox_inches="tight", pad_inches=0.05, dpi=400)
    print("Figure -> {}".format(OUT_PDF))
    print("Figure -> {}".format(OUT_PNG))


if __name__ == "__main__":
    main()
