#!/usr/bin/env python3
"""Figures 8, 10b and 11b - blind byte recovery with CPA on the Pi 5.

The attack is blind: CPA only ever sees the pairs (g_i, mu_i); the 64-byte
secret is used solely for evaluation. A byte counts as recovered when its true
value reaches rank 1 among the 256 candidates.

Produced here:

  cpa_recovery_arch.pdf     16 GB board, committed gadget. 63/64 bytes.
                            This is the measured curve behind Figure 8.
  realvalue_success_vs_traces.pdf
                            Figure 8 in its published Model-vs-Measured form.
                            See docs/REPRODUCTION.md: the per-trace CSV of the
                            16 GB run did not survive, so the model overlay is
                            plotted for the 2 GB portability run (Section 7.3),
                            the one blind run for which both series were cached.
  cpa_recovery_spec.pdf     Figure 10b - speculative (RSB) vs architectural.
  cpa_recovery_nc_barrier.pdf
                            Figure 11b - uncacheable (Normal-NC) vs architectural.

Usage:  python3 scripts/fig08_10b_11b_recovery.py
"""
import json
import os
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

ROOT = Path(__file__).resolve().parents[1]
DATA = ROOT / "data" / "pi5"
OUT = Path(os.environ.get("HOMARD_FIGDIR", ROOT / "figures"))

C_BLUE = "#1f4e79"   # headline series
C_RED = "#b85450"    # comparison series

plt.rcParams.update({
    "font.family": "serif",
    "font.serif": ["Times New Roman", "Liberation Serif", "DejaVu Serif"],
    "font.size": 28, "axes.labelsize": 30,
    "xtick.labelsize": 26, "ytick.labelsize": 26, "legend.fontsize": 24,
    "axes.linewidth": 1.8, "lines.linewidth": 3.4,
    "pdf.fonttype": 42, "ps.fonttype": 42,
    "mathtext.fontset": "custom",
    "mathtext.rm": "serif", "mathtext.it": "serif:italic", "mathtext.bf": "serif:bold",
})


def write(fig, name):
    OUT.mkdir(parents=True, exist_ok=True)
    for ext in ("pdf", "png"):
        fig.savefig(OUT / f"{name}.{ext}", dpi=200)
    plt.close(fig)
    print(f"  wrote figures/{name}.pdf + .png")


def bytes_panel(series, name, annotate_idx=0):
    """Bytes-recovered-vs-N panel (Figs 8, 10b, 11b)."""
    fig, ax = plt.subplots(figsize=(6.0, 4.8), constrained_layout=True)
    ax.axhline(64, color="0.55", ls=":", lw=1.6, zorder=0)
    for i, (label, N, b, color, style, marker) in enumerate(series):
        ax.plot(N, b, style, color=color, marker=marker, ms=8.0 + 0.5 * (i == annotate_idx),
                label=label, zorder=2 + (i == annotate_idx))
    lbl, N, b, color, _, _ = series[annotate_idx]
    ax.text(N[-1] * 0.42, 59.5, f"{b[-1]}/64", fontweight="bold",
            color=color, ha="right", va="center")
    ax.set_xscale("log")
    ax.set_xlabel(r"Number of traces  $N$")
    ax.set_ylabel("Bytes recovered / 64")
    ax.set_ylim(0, 69)
    ax.set_yticks([0, 16, 32, 48, 64])
    ax.grid(True, which="major", ls=":", lw=0.8, alpha=0.5)
    ax.legend(loc="lower right", frameon=True, framealpha=0.96,
              edgecolor="0.6", handlelength=1.6)
    write(fig, name)


def main():
    spec = json.load(open(DATA / "cpa_recovery_spec_vs_arch.json"))
    unc = json.load(open(DATA / "cpa_recovery_uncacheable.json"))

    arch = (np.array(spec["arch"]["N"]), np.array(spec["arch"]["bytes"]))
    sp = (np.array(spec["spec"]["N"]), np.array(spec["spec"]["bytes"]))
    nc = (np.array(unc["uncacheable"]["N"]), np.array(unc["uncacheable"]["bytes"]))

    # ---- Figure 8 (measured architectural curve) ----
    print("\n[Fig 8] blind CPA, architectural gadget, 16 GB Pi 5")
    bytes_panel([("Architectural", arch[0], arch[1], C_BLUE, "-", "o")],
                "cpa_recovery_arch")
    print(f"    {arch[1][-1]}/64 bytes at rank 1 after N = {arch[0][-1]} traces")

    # ---- Figure 10b ----
    print("\n[Fig 10b] speculative (RSB) vs architectural")
    bytes_panel([("Speculative (RSB)", sp[0], sp[1], C_BLUE, "-", "o"),
                 ("Architectural", arch[0], arch[1], C_RED, "--", "s")],
                "cpa_recovery_spec")
    print(f"    speculative {sp[1][-1]}/64 at N = {sp[0][-1]}; "
          f"architectural {arch[1][-1]}/64 at N = {arch[0][-1]}")

    # ---- Figure 11b ----
    print("\n[Fig 11b] uncacheable (Normal-NC) vs architectural")
    bytes_panel([("Uncacheable", nc[0], nc[1], C_BLUE, "-", "o"),
                 ("Architectural", arch[0], arch[1], C_RED, "--", "s")],
                "cpa_recovery_nc_barrier")
    print(f"    uncacheable {nc[1][-1]}/64 at N = {nc[0][-1]} "
          f"(vs {arch[1][-1]}/64 at N = {arch[0][-1]} cacheable)")

    # ---- Figure 8, published Model-vs-Measured form (2 GB replicate) ----
    print("\n[Fig 8 / Sec 7.3] Model vs Measured, 2 GB Pi 5 portability run")
    s = json.load(open(DATA / "cpa_blind_success_vs_rate.json"))
    N = np.array(s["N"])
    meas = np.array(s["success_measured"]) * 100
    mod = np.array(s["success_model"]) * 100
    fig, ax = plt.subplots(figsize=(10.0, 5.6), constrained_layout=True)
    ax.plot(N, mod, "--", color=C_RED, lw=3.2, marker="s", ms=9, label="Model", zorder=2)
    ax.plot(N, meas, "-", color=C_BLUE, lw=3.2, marker="o", ms=9, label="Measured", zorder=3)
    ax.set_xscale("log")
    ax.set_xlabel("Number of traces")
    ax.set_ylabel("Success Probability")
    ax.set_ylim(0, 105)
    ax.set_yticks([0, 25, 50, 75, 100])
    ax.set_yticklabels(["0%", "25%", "50%", "75%", "100%"])
    ax.grid(True, which="major", ls=":", lw=1.0, alpha=0.4)
    ax.legend(frameon=True, framealpha=0.96, edgecolor="0.6",
              loc="upper left", handlelength=1.6)
    ax2 = ax.twinx()
    ax2.set_ylim(0, 105 * 64 / 100)
    ax2.set_yticks([0, 16, 32, 48, 64])
    ax2.set_ylabel("Bytes recovered / 64")
    ax2.tick_params(direction="in")
    write(fig, "realvalue_success_vs_traces")

    a = json.load(open(DATA / "cpa_blind_attack_summary.json"))
    ranks = np.array(a["ranks"])
    print(f"    {a['rank1']}/64 bytes at rank 1 after {a['n_traces']} traces; "
          f"all 64 at rank <= {ranks.max()}; residual key rank "
          f"2^{a['keyrank_bits']:.1f}")

    v = json.load(open(DATA / "cpa_blind_verification.json"))
    print(f"\n[control] shuffle test on the uncacheable run "
          f"({v['n_traces']} traces)")
    print(f"    real pairing            : {v['rank1']}/64 bytes at rank 1")
    print(f"    g_i re-paired at random : {v['shuffle_control_rank1']}/64 "
          f"-> the signal comes from the data-dependent G->V transfer, "
          f"not from a measurement artifact")


if __name__ == "__main__":
    main()
