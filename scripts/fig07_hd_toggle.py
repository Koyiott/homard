#!/usr/bin/env python3
"""Figure 7 - Linear HD response of DDR_VDD2_A to full-line bit toggling.

The horizontal axis N_toggle is the number of toggling bits, i.e. the Hamming
distance HD(G,V) of Equation 2. Anchor traces interleaved through the sweep
fit a polynomial drift model that is subtracted before the per-HD means are
formed, so the residual slope is the data-dependent term alone.

Paper value: +19.7 uA per toggled bit, R^2 = 0.958.

Usage:  python3 scripts/fig07_hd_toggle.py
"""
import matplotlib; matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.ticker import MultipleLocator
import numpy as np, csv, os

plt.rcParams.update({
    "font.family": "serif", "font.serif": ["Times New Roman", "Liberation Serif", "DejaVu Serif"],
    "font.size": 28, "axes.labelsize": 30, "axes.titlesize": 30, "legend.fontsize": 22,
    "xtick.labelsize": 26, "ytick.labelsize": 26, "axes.linewidth": 1.8, "lines.linewidth": 3.4,
    "xtick.major.size": 7, "ytick.major.size": 7, "xtick.major.width": 1.4, "ytick.major.width": 1.4,
    "xtick.major.pad": 6, "ytick.major.pad": 5,
    "axes.grid": True, "grid.alpha": 0.30, "grid.linestyle": ":", "grid.linewidth": 1.0,
    "axes.spines.top": False, "axes.spines.right": False, "pdf.fonttype": 42, "ps.fonttype": 42,
})
BLUE = "#1f4e79"; RED = "#b85450"; GREY = "#888888"

_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CSV = os.environ.get("HD_CSV", os.path.join(_ROOT, "data", "pi5", "hd_toggle_sweep_v11ref.csv"))
OUTS = [os.path.join(os.environ.get("HOMARD_FIGDIR", os.path.join(_ROOT, "figures")), "block_hd_arch")]
os.makedirs(os.path.dirname(OUTS[0]), exist_ok=True)

def poly_r2(t, y, deg):
    c = np.polyfit(t, y, deg); yp = np.polyval(c, t)
    ssr = float(np.sum((y - yp) ** 2)); sst = float(np.sum((y - y.mean()) ** 2))
    return c, (1.0 - ssr / sst if sst > 0 else float("nan"))

def ols(x, y):
    X = np.column_stack([x, np.ones_like(x)]); beta, *_ = np.linalg.lstsq(X, y, rcond=None)
    yp = X @ beta; ssr = float(np.sum((y - yp) ** 2)); sst = float(np.sum((y - y.mean()) ** 2))
    return float(beta[0]), float(beta[1]), (1.0 - ssr / sst if sst > 0 else float("nan"))

rows = list(csv.DictReader(open(CSV)))
time_s = np.array([float(r["time_s"]) for r in rows])
is_anchor = np.array([int(float(r["is_anchor"])) for r in rows])
hd_total = np.array([float(r["hd_total"]) for r in rows])
vdd2 = np.array([float(r["mean_VDD2_mA"]) for r in rows])
anchor = (is_anchor == 1); main = (is_anchor == 0)

t_a, y_a = time_s[anchor], vdd2[anchor]
best = None
for deg in (1, 2, 3):
    if t_a.size >= deg + 1:
        c, r2 = poly_r2(t_a, y_a, deg)
        if best is None or r2 > best[1]: best = (deg, r2, c)
best_deg, best_r2, best_coeffs = best

t_m, hd_m, y_m = time_s[main], hd_total[main], vdd2[main]
y_corr = y_m - np.polyval(best_coeffs, t_m)
hds = np.array(sorted(set(hd_m.tolist())), dtype=float)
means = np.zeros_like(hds); ses = np.zeros_like(hds)
for i, h in enumerate(hds):
    vals = y_corr[hd_m == h]
    means[i] = float(np.mean(vals))
    ses[i] = (float(np.std(vals, ddof=1)) / np.sqrt(vals.size)) if vals.size > 1 else 0.0
base = means[hds == 0.0][0] if 0.0 in hds.tolist() else means[0]
delta = means - base
slope, intercept, r2 = ols(hds, delta)
slope_uA = slope * 1000.0
fit_y = slope * hds + intercept
swing = float(np.max(delta) - np.min(delta))

fig, ax = plt.subplots(figsize=(6.0, 4.6), constrained_layout=True)
ax.fill_between(hds, delta - 2 * ses, delta + 2 * ses, color=RED, alpha=0.15, linewidth=0)
ax.plot(hds, delta, color=RED, lw=3.0, zorder=3)
ax.scatter(hds, delta, marker='s', s=64, facecolor=RED, edgecolor='white', linewidth=0.9, zorder=4)
ax.plot(hds, fit_y, color="0.25", lw=1.4, ls='--', zorder=5,
        label=f"{slope_uA:+.1f} µA/bit\n$R^2$={r2:.3f}")
ax.axvline(256, color=GREY, lw=1.4, ls='--', alpha=0.7)
ax.set_xlabel(r"Number of toggling bits  $N_{\mathrm{toggles}}$")
ax.set_ylabel(r"$\Delta$ VDD2 (mA)")
ax.xaxis.set_major_locator(MultipleLocator(128)); ax.set_xlim(-8, 520)
leg = ax.legend(loc='lower right', framealpha=0.92, edgecolor="0.7",
                handlelength=1.2, handletextpad=0.5, borderaxespad=0.3)
leg.get_frame().set_linewidth(1.0)
for p in OUTS:
    fig.savefig(p + ".pdf", bbox_inches="tight", pad_inches=0.08)
    fig.savefig(p + ".png", dpi=200, bbox_inches="tight", pad_inches=0.08)
plt.close(fig)
print(f"drift_model: poly deg={best_deg} R^2={best_r2:.4f}  n_anchor={int(anchor.sum())} n_main={int(main.sum())}")
print(f"slope_uA_per_bit={slope_uA:+.4f}  R2={r2:.4f}  swing_mA={swing:.4f}  intercept={intercept:+.4f}")
print("wrote: " + " , ".join(p + ".pdf" for p in OUTS))
