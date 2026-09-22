#!/usr/bin/env python3
"""
make_hierarchy_landscape_figure.py
==================================

Theoretical comparison of binary same-bank vs hierarchy-aware GF(2)
recovery across the well-known DRAM mapping configurations of the
literature (DDR3, DDR4, DDR5, LPDDR4, LPDDR5, HBM2). Our empirical
result on the Jetson TX2 LPDDR4 is overlaid as a black diamond to
position this work in the landscape.

Cost model (Plin et al., 2026 + the paper's general r-level extension):
    N_binary    = 2^{K_r} * log2((2^{n-K_r}-1)/eps)
    N_hierarchy = sum_j 2^{q_j} * log2(r*(2^{n-K_j}-1)/eps)
where q_j = rank increment at hierarchy level j and K_j = cumulative.

Outputs:
    figures/fig_hierarchy_landscape.{pdf,png}
"""
from __future__ import annotations

from pathlib import Path
import numpy as np

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
from matplotlib.lines import Line2D

OUT_PDF = _OUT / "fig_hierarchy_landscape.pdf"
OUT_PNG = _OUT / "fig_hierarchy_landscape.png"

# Common candidate-bit window across configurations (typical PA mapping
# bits on real platforms). Held fixed so the comparison is across
# *hierarchy structure*, not window size.
N_BITS = 20
EPS    = 0.01

# (label, q_vector, family, citation)
# The q vector lists rank increments at each hierarchy level (chip /
# channel, sub-channel, bank-group, bank, ...). K_r = sum(q) is the
# total number of independent selectors recovered.
CONFIGS = [
    # mobile / embedded
    ("LPDDR4  Pixel\n2 ch $\\times$ 8 bk",
     (1, 3),       "mobile", r"Frigo \etal'20"),
    ("LPDDR4  Jetson TX2\n2 chip $\\times$ 2 sch $\\times$ 4 bk",
     (1, 1, 2),    "ours",   r"this work"),
    ("LPDDR5  mobile\n2 ch $\\times$ 4 BG $\\times$ 4 bk",
     (1, 2, 2),    "mobile", r"Hynix LPDDR5'21"),
    # desktop / server
    ("DDR3  Sandy/Ivy Bridge\n2 ch $\\times$ 2 rk $\\times$ 8 bk",
     (1, 1, 3),    "desktop", r"Pessl \etal'16"),
    ("DDR4  Skylake / Zen\n2 ch $\\times$ 2 rk $\\times$ 4 BG $\\times$ 4 bk",
     (1, 1, 2, 2), "desktop", r"Pessl \etal'16, Cojocar \etal'20"),
    ("DDR5  Alder Lake\n2 ch $\\times$ 2 sch $\\times$ 8 BG $\\times$ 4 bk",
     (1, 1, 3, 2), "desktop", r"Steiner \etal'23"),
    # high-end / HBM
    ("HBM2  server GPU\n2 pch $\\times$ 4 BG $\\times$ 4 bk",
     (1, 2, 2),    "hbm",    r"Frigo \etal'22"),
]


def n_binary(q, n=N_BITS, eps=EPS):
    Kr = sum(q)
    return float((2 ** Kr) * np.log2((2 ** (n - Kr) - 1) / eps))


def n_hierarchy(q, n=N_BITS, eps=EPS):
    r = len(q)
    cum = 0
    total = 0.0
    for qj in q:
        cum += qj
        total += (2 ** qj) * np.log2(r * (2 ** (n - cum) - 1) / eps)
    return float(total)


def main():
    rows = []
    for lbl, q, fam, cite in CONFIGS:
        Kr   = sum(q)
        Nb   = n_binary(q)
        Nh   = n_hierarchy(q)
        rows.append((lbl, q, fam, cite, Kr, Nb, Nh, Nb / Nh))

    print(f"{'Configuration':<55} {'q':<14} {'K_r':<3} "
          f"{'N_bin':<8} {'N_hier':<8} {'speed':<6}")
    for lbl, q, _, _, Kr, Nb, Nh, sp in rows:
        flat = lbl.split("\n")[0]
        print(f"  {flat:<53} {str(q):<14} {Kr:<3} "
              f"{Nb:<8.0f} {Nh:<8.0f} {sp:<6.2f}")

    # TX2 empirical point (from method_comparison_full.json, N at Pr=1.0)
    tx2_emp_N = 221

    # ------------------------ figure ------------------------------------
    # Canonical paper style (Times serif, matches the other figures). Shown at
    # width=\linewidth in one column (~3.4 in) from a ~10.4 in canvas, so source
    # fonts are large to print at the same ~8-9 pt as the rest of the article.
    plt.rcParams.update({
        "font.family": "serif",
        "font.serif": ["Times New Roman", "Liberation Serif", "DejaVu Serif"],
        "font.size": 21,
        "axes.titlesize": 24,
        "axes.labelsize": 24,
        "xtick.labelsize": 21,
        "ytick.labelsize": 21,
        "legend.fontsize": 19,
        "axes.linewidth": 1.5,
        "xtick.major.width": 1.2,
        "ytick.major.width": 1.2,
        "xtick.major.size": 6.0,
        "ytick.major.size": 6.0,
        "axes.grid": False,
        "pdf.fonttype": 42,
        "ps.fonttype": 42,
    })
    C_BIN  = "#c1272d"   # binary
    C_HIER = "#1b9e77"   # hierarchy
    C_OURS = "#222244"   # our empirical marker

    fig, ax = plt.subplots(figsize=(10.4, 8.8))
    fig.subplots_adjust(left=0.34, right=0.975, top=0.985, bottom=0.115)

    n_cfg = len(rows)
    y = np.arange(n_cfg)[::-1]   # top-to-bottom listing
    bw = 0.36

    for i, (lbl, q, fam, cite, Kr, Nb, Nh, sp) in enumerate(rows):
        yy = y[i]
        # binary bar (top of pair)
        ax.barh(yy + bw / 2, Nb, height=bw, color=C_BIN, edgecolor="white",
                lw=0.6)
        # hierarchy bar (bottom of pair)
        ax.barh(yy - bw / 2, Nh, height=bw, color=C_HIER, edgecolor="white",
                lw=0.6)
        # speedup callout to the right of the binary bar
        ax.text(Nb * 1.04, yy + bw / 2,
                r"$\bf{{{:.1f}\times}}$".format(sp),
                color="#222244", fontsize=20, ha="left", va="center",
                weight="bold")
        # K_r badge to the right of the hierarchy bar (kept clear of the TX2
        # diamond marker on the "ours" row).
        q_str = str(tuple(q)).replace(" ", "")
        badge = r"$K_r{=}" + str(Kr) + r",\,q{=}$" + q_str
        badge_x = Nh * 1.04
        if fam == "ours":
            badge_x = max(badge_x, tx2_emp_N * 1.20)
        ax.text(badge_x, yy - bw / 2, badge,
                color="#666", fontsize=15, ha="left", va="center")

    # --- TX2 empirical marker overlaid on the hierarchy bar -------------
    tx2_idx = next(i for i, r in enumerate(rows) if r[2] == "ours")
    tx2_y = y[tx2_idx] - bw / 2
    ax.scatter([tx2_emp_N], [tx2_y], s=110, marker="D",
               facecolor=C_OURS, edgecolor="white", lw=1.4, zorder=5)
    # white-region text on the right, arrow back to the diamond
    annot_x = max(r[5] for r in rows) * 2.20
    annot_y = tx2_y - 0.55
    ax.annotate(
        r"empirical" + "\n" +
        r"$N_{\Pr=1}{=}$" + "{}".format(tx2_emp_N),
        xy=(tx2_emp_N, tx2_y),
        xytext=(annot_x, annot_y),
        fontsize=18, color=C_OURS, ha="right", va="center", weight="bold",
        arrowprops=dict(arrowstyle="->", color=C_OURS, lw=1.6,
                        connectionstyle="arc3,rad=0.20",
                        shrinkA=4, shrinkB=6))

    # axes
    ax.set_yticks(y)
    ax.set_yticklabels([r[0] for r in rows], fontsize=18)
    ax.set_xscale("log")
    ax.set_xlim(60, max(r[5] for r in rows) * 2.6)
    xlab = (r"Total measurements $N$ (theory, $n{=}" + "{}".format(N_BITS) +
            r",\;\varepsilon{=}" + "{:.2f}".format(EPS) + r"$)")
    ax.set_xlabel(xlab, labelpad=8)
    ax.tick_params(axis="y", which="both", left=False)
    ax.grid(True, axis="x", which="both", alpha=0.20, lw=0.5)
    ax.set_axisbelow(True)
    ax.spines["top"].set_visible(False)
    ax.spines["right"].set_visible(False)

    # highlight the "ours" row with a soft background
    ax.axhspan(y[tx2_idx] - 0.5, y[tx2_idx] + 0.5,
               facecolor="#fffbe6", edgecolor="none", zorder=0)

    # legend at the bottom
    legend_handles = [
        Line2D([0], [0], color=C_BIN,  lw=11, label="Binary same-bank oracle"),
        Line2D([0], [0], color=C_HIER, lw=11, label="Hierarchy-aware (this work)"),
        Line2D([0], [0], color=C_OURS, lw=0, marker="D", ms=13,
               label="Jetson TX2 empirical (this work)"),
    ]
    ax.legend(handles=legend_handles, frameon=False,
              loc="upper center", bbox_to_anchor=(0.5, -0.11),
              ncol=3, fontsize=17, handlelength=1.6,
              handletextpad=0.5, columnspacing=1.5, borderaxespad=0.0)

    fig.savefig(OUT_PDF, bbox_inches="tight", pad_inches=0.05)
    fig.savefig(OUT_PNG, bbox_inches="tight", pad_inches=0.05, dpi=400)
    print("Figure -> {}".format(OUT_PDF))
    print("Figure -> {}".format(OUT_PNG))


if __name__ == "__main__":
    main()
