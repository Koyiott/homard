#!/usr/bin/env python3
"""
analyze_hw_template.py — HW(V) template + recovery from the v11 ARM_I spec gadget
with G pinned to 0x00. Reuses analyze_multi.py's exact functions/style so the
figures match experiments/best/figures/A/ one-for-one:
    vddq.png  vdd2.png  template_curve.png  confusion.png
    (+ accuracy_vs_N.png  recovery.png  validation.txt)

Usage: python3 analyze_hw_template.py <hwtpl_v11_*.csv> [outdir]
"""
import sys, math
from pathlib import Path
from collections import defaultdict, Counter
import numpy as np
import matplotlib.pyplot as plt
from matplotlib.ticker import MultipleLocator

# import the canonical analyzer for identical style + helpers
sys.path.insert(0, str(Path(__file__).resolve().parent))
import analyze_multi as am   # applies rcParams (serif, sizes, viridis, etc.)

CSV = Path(sys.argv[1])
OUT = Path(sys.argv[2]) if len(sys.argv) > 2 else \
      Path(__file__).resolve().parents[2] / "figures" / "HW_V11"
OUT.mkdir(parents=True, exist_ok=True)
TAG = sys.argv[3] if len(sys.argv) > 3 else "HW-V11 (G=0)"
COLOR = "#cc1188"   # v11 magenta

# Mandated common figsize for every paired 0.49\linewidth paper figure.
PAIR_FIGSIZE = (6.0, 4.8)


def save_fixed(fig, path):
    """Save at the FIXED canvas (no bbox_inches='tight') so paired 0.49
    figures keep identical figsize -> identical rendered height. Relies on
    constrained_layout=True to keep labels inside the canvas. pdf is what
    LaTeX uses, so always (re)write it; png is optional."""
    fig.savefig(path.with_suffix(".pdf"))
    fig.savefig(path.with_suffix(".png"), dpi=200)
    plt.close(fig)

samples, raw, reps, hws = am.load(CSV)
print(f"loaded {CSV.name}: reps={len(reps)} classes={len(hws)} "
      f"samples={sum(1 for _ in samples)}")

# ── per-rail ΔI vs HW (vddq.png, vdd2.png) ──────────────────────────
amp_q, snr_q, N = am.fig_rail(OUT, raw, 0, am.BLUE, "vddq",
                              r"$\Delta$ VDDQ (mA)", r"$\mathtt{DDR\_VDDQ\_A}$")
amp_d, snr_d, _ = am.fig_rail(OUT, raw, 1, am.RED, "vdd2",
                              r"$\Delta$ VDD2 (mA)", r"$\mathtt{DDR\_VDD2\_A}$")
print(f"  VDDQ amp={amp_q:.3f} mA SNR={snr_q:.1f} | VDD2 amp={amp_d:.3f} mA SNR={snr_d:.1f}")

# ── template curve (z VDDQ vs z VDD2, colored by HW) ────────────────
z, (mv, sv, md, sd) = am.build_template_z(samples)
fig, ax = plt.subplots(figsize=PAIR_FIGSIZE, constrained_layout=True)
xs = [z[h][0] for h in hws]; ys = [z[h][1] for h in hws]
cmap = plt.get_cmap("viridis")
for i in range(len(hws)-1):
    ax.plot([xs[i], xs[i+1]], [ys[i], ys[i+1]],
            color=cmap(i/(len(hws)-1)), lw=2.8, solid_capstyle="round")
sc = ax.scatter(xs, ys, c=hws, cmap=cmap, s=80, zorder=3,
                edgecolor="white", linewidth=1.2)
cbar = plt.colorbar(sc, ax=ax, ticks=[0, 128, 256, 384, 512]); cbar.set_label(r"HW($\nu$)")
ax.set_xlabel(r"$\tilde\mu_{VDDQ}$ (z)"); ax.set_ylabel(r"$\tilde\mu_{VDD2}$ (z)")
# NO descriptive in-figure title — titles live in the LaTeX caption.
save_fixed(fig, OUT / "template_curve")

# ── classifier sweep (k-fold, joint z template, project onto polyline) ──
spp = max(1, int(np.median(list(Counter((s[0], s[1]) for s in samples).values()))))
print(f"  ~{spp} samples / (rep,class)")

best = dict(acc=0, agg=0, w32=0, mae=0, pairs=[], conf={})
accs = []
for agg in [1, 5, 10, 20, 40, 80, 120, 160, 240, 320, 480, 640, 800, 1200, 1600]:
    rpf = max(1, math.ceil(agg / spp))
    if len(reps) // rpf < 2: continue
    if agg > rpf * spp: continue
    a, e, c, t, pr = am.kfold(samples, reps, hws, rpf, agg)
    if t == 0: continue
    w32 = sum(n for kk, vv in c.items() for p, n in vv.items() if abs(kk-p) <= 32) / t
    mae = sum(e) / len(e)
    accs.append((agg, a*100, w32*100, mae))
    if a > best["acc"]:
        best.update(acc=a, agg=agg, w32=w32, mae=mae, pairs=pr, conf=dict(c))

# ── accuracy vs N ───────────────────────────────────────────────────
if accs:
    ns_, exes, w32s, _ = zip(*accs)
    fig, ax = plt.subplots(figsize=(6.0, 4.4), constrained_layout=True)
    ax.plot(ns_, exes, marker='o', color=am.BLUE, markersize=9, lw=3.0, label="exact")
    ax.plot(ns_, w32s, marker='s', color=am.RED,  markersize=9, lw=3.0, label=r"$\pm$32")
    chance = 100.0/len(hws)
    ax.axhline(chance, color=am.GREY, lw=1.2, ls='--', alpha=0.7, label=f"chance ({chance:.1f}%)")
    ax.set_xscale("log")
    ax.set_xlabel(r"Probe length $N$ (samples)"); ax.set_ylabel("Accuracy (%)")
    ax.set_ylim(0, 105)
    leg = ax.legend(loc='upper left', framealpha=0.9); leg.get_frame().set_linewidth(0.9)
    am.save(fig, OUT / "accuracy_vs_N")

# ── confusion + recovery (best N) ───────────────────────────────────
if best["pairs"]:
    mat = np.array([[best["conf"].get(t, {}).get(p, 0) for p in hws] for t in hws], dtype=float)
    fig, ax = plt.subplots(figsize=PAIR_FIGSIZE, constrained_layout=True)
    im = ax.imshow(mat, cmap="viridis", aspect="auto")
    tick_idx = [i for i, h in enumerate(hws) if h % 128 == 0]
    ax.set_xticks(tick_idx); ax.set_xticklabels([str(hws[i]) for i in tick_idx])
    ax.set_yticks(tick_idx); ax.set_yticklabels([str(hws[i]) for i in tick_idx])
    ax.set_xlabel("Predicted HW"); ax.set_ylabel("True HW")
    # Confusion matrices keep a minimal title with ONLY the key numbers
    # (no TAG/prefix) — descriptive titles live in the LaTeX caption.
    ax.set_title(f"exact={best['acc']*100:.1f}%   "
                 f"±32={best['w32']*100:.0f}%   N={best['agg']}")
    cbar = plt.colorbar(im, ax=ax, shrink=0.85); cbar.set_label("count")
    save_fixed(fig, OUT / "confusion")

    by_true = defaultdict(list)
    for true_hw, pred, _ in best["pairs"]: by_true[true_hw].append(pred)
    fig, ax = plt.subplots(figsize=(5.4, 5.2), constrained_layout=True)
    tx = np.array([t for t, _, _ in best["pairs"]]); py = np.array([p for _, p, _ in best["pairs"]])
    if len(best["pairs"]) >= 30:
        ax.hexbin(tx, py, gridsize=22, cmap="Greys", mincnt=1,
                  extent=(-10, 522, -10, 522), linewidths=0.0)
    xs_ = sorted(by_true.keys()); means_ = [sum(by_true[h])/len(by_true[h]) for h in xs_]
    ax.plot([0, 512], [0, 512], color=am.GREY, lw=1.2, ls='--', label="y=x")
    ax.scatter(xs_, means_, s=85, color=COLOR, edgecolor='white', linewidth=1.2,
               label=f"mean (MAE={best['mae']:.1f})")
    ax.set_xlim(-10, 522); ax.set_ylim(-10, 522)
    ax.xaxis.set_major_locator(MultipleLocator(128)); ax.yaxis.set_major_locator(MultipleLocator(128))
    ax.set_xlabel("True HW"); ax.set_ylabel("Recovered HW")
    # NO descriptive in-figure title — titles live in the LaTeX caption.
    leg = ax.legend(loc='upper left', framealpha=0.9); leg.get_frame().set_linewidth(0.9)
    am.save(fig, OUT / "recovery")

# ── validation.txt ──────────────────────────────────────────────────
with open(OUT / "validation.txt", "w") as f:
    f.write(f"# HW(V) template via v11 ARM_I (spec), G pinned to 0x00\n")
    f.write(f"source: {CSV.name}\nreps={len(reps)} hw_classes={len(hws)} samples/pattern~={N}\n\n")
    f.write("## Signal amplitudes (raw, mean over reps)\n")
    f.write(f"  DDR_VDDQ amplitude (max-min): {amp_q:.3f} mA  SNR(amp/SE): {snr_q:.1f}\n")
    f.write(f"  DDR_VDD2 amplitude (max-min): {amp_d:.3f} mA  SNR(amp/SE): {snr_d:.1f}\n\n")
    f.write("## Classifier (k-fold, joint VDDQ+VDD2 z-scored template)\n")
    for ag, ex, w32, m in accs:
        f.write(f"  N={ag:4d}  exact={ex:5.1f}%  ±32={w32:5.1f}%  MAE={m:6.2f}\n")
    if best["pairs"]:
        f.write(f"\n  BEST: N={best['agg']}  exact={best['acc']*100:.2f}%  "
                f"±32={best['w32']*100:.2f}%  MAE={best['mae']:.2f}\n")
    f.write(f"\nchance = {100.0/len(hws):.2f}% ({len(hws)} HW classes)\n")
print(f"  figures -> {OUT}")
if best["pairs"]:
    print(f"  BEST exact={best['acc']*100:.1f}% ±32={best['w32']*100:.1f}% "
          f"MAE={best['mae']:.1f} @N={best['agg']}")
