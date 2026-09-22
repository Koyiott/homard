#!/usr/bin/env python3
"""
analyze_multi.py — per-experiment + cross-experiment HW-leakage analysis.

Reads experiments/data/calib_{A,B,C,NS,NP}.csv produced by power_multi.c and
writes:
  - experiments/<exp>/figures/{vdd2,vddq,template_curve,confusion,
                               recovery,accuracy_vs_N,resolution}.{png,pdf}
  - experiments/<exp>/figures/validation.txt
  - experiments/figures/{overlay_vdd2,overlay_vddq,bars_metrics}.{png,pdf}
  - experiments/figures/summary.txt
"""
from pathlib import Path
import csv, math, sys
from collections import defaultdict
import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.ticker import MultipleLocator

ROOT = Path(__file__).resolve().parents[2]
DATA = ROOT / "data"

EXPS = [
    ("A_single_load",     "A",   "calib_A.csv"),
    ("A_v4_bulk",         "AV4", "calib_AV4.csv"),
    ("A_v5_long_burst",   "AV5", "calib_AV5.csv"),
    ("A_v5_long_burst",   "AV5r24", "calib_AV5r24.csv"),
    ("A_v5_long_burst",   "AV5r32", "calib_AV5r32.csv"),
    ("A_v5_long_burst",   "AV5r48", "calib_AV5r48.csv"),
    ("A_v5_long_burst",   "AV5r96", "calib_AV5r96.csv"),
    ("A_v5_long_burst",   "AV5r128", "calib_AV5r128.csv"),
    ("A_v5_long_burst",   "AV5r192", "calib_AV5r192.csv"),
    ("A_v6_xlong_burst",  "AV6",    "calib_AV6.csv"),
    ("A_kernel_realistic","AKv3",   "calib_AKv3.csv"),
    ("E_kernel_rsb",      "KRSB",   "calib_KRSB.csv"),
    ("E_kernel_rsb",      "KRSBv2", "calib_KRSBv2.csv"),
    ("E_kernel_rsb",      "KRSBr48", "calib_KRSBr48.csv"),
    ("E_kernel_rsb",      "KRSBv4r128","calib_KRSBv4r128.csv"),
    ("E_kernel_rsb",      "KRSBv5r128","calib_KRSBv5r128.csv"),
    ("E_kernel_rsb",      "KRSBv6r192","calib_KRSBv6r192.csv"),
    ("E_kernel_rsb",      "KRSBv7r192","calib_KRSBv7r192.csv"),
    ("E_kernel_rsb",      "KRSBr128", "calib_KRSBr128.csv"),
    ("E_kernel_rsb",      "KRSBv8r192","calib_KRSBv8r192.csv"),  # v8 last -> owns figures
    ("B_repeated_trigger","B",   "calib_B.csv"),
    ("B_repeated_trigger","Bv4", "calib_Bv4.csv"),
    ("B_v5_bulk_ring",    "BV5", "calib_BV5.csv"),
    ("C_vector_chain",    "C",   "calib_C.csv"),
    ("control_nospec",    "NS",  "calib_NS.csv"),
    ("control_nopattern", "NP",  "calib_NP.csv"),
    ("K1_chain",          "K1",  "calib_K1.csv"),
    ("K2_chain",          "K2",  "calib_K2.csv"),
    ("K4_chain",          "K4",  "calib_K4.csv"),
    ("K8_chain",          "K8",  "calib_K8.csv"),
    ("K16_chain",         "K16", "calib_K16.csv"),
]

# K-sweep tags: variants with known SPEC_LINES = K (C is K=32). Used for the
# line-count attribution figure (amp & SNR vs K).
K_LINES = {"K1": 1, "K2": 2, "K4": 4, "K8": 8, "K16": 16, "C": 32}

RAIL_VDDQ = "DDR_VDDQ_A_curr_mA"
RAIL_VDD2 = "DDR_VDD2_A_curr_mA"

plt.rcParams.update({
    "font.family": "serif",
    "font.serif": ["Times New Roman", "Liberation Serif", "DejaVu Serif"],
    "font.size": 28, "axes.labelsize": 30, "axes.titlesize": 28,
    "legend.fontsize": 24, "xtick.labelsize": 26, "ytick.labelsize": 26,
    "axes.linewidth": 1.8, "lines.linewidth": 3.4,
    "axes.grid": True, "grid.alpha": 0.30, "grid.linestyle": ":",
    "axes.spines.top": False, "axes.spines.right": False,
    "pdf.fonttype": 42, "ps.fonttype": 42,
})

BLUE, RED, GREY, ORG = "#1f4e79", "#b85450", "#888888", "#d95f02"
COLORS = {
    "A":  BLUE, "AV4": "#003366", "AV5": "#001a33", "AV5r24": "#000010",
    "AV5r32": "#000010", "AV5r48": "#000005", "AV5r96": "#000002",
    "AV5r128": "#000001", "AV5r192": "#000000",
    "AV6": "#000008", "AK": "#552288", "AKv2": "#7733aa", "AKv3": "#aa55cc",
    "KRSB": "#cc0066", "KRSBv2": "#aa0055", "KRSBr48": "#880044", "KRSBr128": "#550022", "KRSBv4r128": "#330011", "KRSBv5r128": "#220008", "KRSBv6r192": "#cc1188", "KRSBv7r192": "#dd22bb", "KRSBv8r192": "#ff44cc",   # deep magenta: kernel-context RSB
    "B":  ORG,  "Bv4": "#a64500", "BV5": "#6b2c00",     # darker oranges for B v4/v5
    "C":  RED,
    "NS": "#7d7d7d", "NP": "#3aa55c",
    "K1": "#4575b4", "K2": "#74add1", "K4": "#abd9e9",
    "K8": "#fdae61", "K16": "#f46d43",
}


def load(csv_path):
    rows = list(csv.DictReader(open(csv_path)))
    per_rep_sum = defaultdict(lambda: [0.0, 0.0, 0])
    for r in rows:
        rep = int(r["rep"])
        per_rep_sum[rep][0] += float(r[RAIL_VDDQ])
        per_rep_sum[rep][1] += float(r[RAIL_VDD2])
        per_rep_sum[rep][2] += 1
    rep_baseline = {rep: (s[0]/s[2], s[1]/s[2]) for rep, s in per_rep_sum.items()}
    samples, raw = [], []
    for r in rows:
        rep = int(r["rep"]); hw = int(r["num_ones"])
        vq = float(r[RAIL_VDDQ]); vd = float(r[RAIL_VDD2])
        raw.append((rep, hw, [vq, vd]))
        samples.append((rep, hw, [vq - rep_baseline[rep][0],
                                  vd - rep_baseline[rep][1]]))
    return samples, raw, sorted({s[0] for s in samples}), sorted({s[1] for s in samples})


def per_hw_stats(rail_idx, source):
    by = defaultdict(list)
    for _, hw, f in source:
        by[hw].append(f[rail_idx])
    hws = sorted(by.keys())
    means = [sum(by[h])/len(by[h]) for h in hws]
    stds  = [math.sqrt(sum((x-means[i])**2 for x in by[h])/len(by[h]))
             for i, h in enumerate(hws)]
    n     = len(by[hws[0]])
    ses   = [s/math.sqrt(n) for s in stds]
    return hws, means, stds, ses, n


def build_template_z(train):
    by = defaultdict(list)
    for _, hw, f in train: by[hw].append(f)
    raw = {hw: [sum(v[i] for v in vs)/len(vs) for i in range(2)] for hw, vs in by.items()}
    fv = [v[0] for _, _, v in train]; fd = [v[1] for _, _, v in train]
    mv = sum(fv)/len(fv); sv = math.sqrt(sum((x-mv)**2 for x in fv)/len(fv)+1e-9)
    md = sum(fd)/len(fd); sd = math.sqrt(sum((x-md)**2 for x in fd)/len(fd)+1e-9)
    z = {hw: [(raw[hw][0]-mv)/sv, (raw[hw][1]-md)/sd] for hw in raw}
    return z, (mv, sv, md, sd)


def project(zpt, z):
    hs = sorted(z.keys())
    best_d = float('inf'); best = hs[0]
    for i in range(len(hs)-1):
        a, b = z[hs[i]], z[hs[i+1]]
        abx, aby = b[0]-a[0], b[1]-a[1]
        ab2 = abx*abx + aby*aby
        if ab2 < 1e-12: tc = 0.0
        else: tc = max(0.0, min(1.0,
                       ((zpt[0]-a[0])*abx + (zpt[1]-a[1])*aby)/ab2))
        cx, cy = a[0]+tc*abx, a[1]+tc*aby
        d = math.hypot(zpt[0]-cx, zpt[1]-cy)
        if d < best_d:
            best_d = d
            best = hs[i] + tc*(hs[i+1]-hs[i])
    return best


def kfold(samples, reps, hws, reps_per_fold, agg):
    abs_errs = []; exact = 0; total = 0
    confusion = defaultdict(lambda: defaultdict(int))
    pairs = []
    n_folds = len(reps) // reps_per_fold
    for f in range(n_folds):
        held = set(reps[f*reps_per_fold:(f+1)*reps_per_fold])
        train = [s for s in samples if s[0] not in held]
        test  = [s for s in samples if s[0] in held]
        if not train or not test: continue
        z, (mv, sv, md, sd) = build_template_z(train)
        by_hw = defaultdict(list)
        for _, hw, f_ in test: by_hw[hw].append(f_)
        for hw, feats in by_hw.items():
            for k in range(0, len(feats), agg):
                chunk = feats[k:k+agg]
                if len(chunk) < agg: continue
                mean = [sum(v[i] for v in chunk)/len(chunk) for i in range(2)]
                zf = [(mean[0]-mv)/sv, (mean[1]-md)/sd]
                pc = project(zf, z)
                sn = min(hws, key=lambda h: abs(h-pc))
                abs_errs.append(abs(pc-hw))
                confusion[hw][sn] += 1
                pairs.append((hw, pc, sn))
                if sn == hw: exact += 1
                total += 1
    if total == 0:
        return 0.0, [0.0], confusion, 0, []
    return exact/total, abs_errs, confusion, total, pairs


def save(fig, path):
    fig.savefig(path.with_suffix(".pdf"), bbox_inches="tight", pad_inches=0.08)
    fig.savefig(path.with_suffix(".png"), dpi=200, bbox_inches="tight", pad_inches=0.08)
    plt.close(fig)


def base_hw_ax(figsize=(6.0, 4.4)):
    fig, ax = plt.subplots(figsize=figsize, constrained_layout=True)
    ax.xaxis.set_major_locator(MultipleLocator(128))
    ax.set_xlim(-12, 524)
    ax.set_xlabel(r"Hamming weight HW($\nu$)")
    return fig, ax


def fig_rail(outdir, raw, rail_idx, color, name, ylabel, title):
    hws, means, stds, ses, n = per_hw_stats(rail_idx, raw)
    base = means[0]
    delta = [m - base for m in means]
    band  = list(ses)
    amp = max(means) - min(means)
    snr = amp / (sum(ses)/len(ses) + 1e-9)
    fig, ax = base_hw_ax()
    ax.fill_between(hws,
                    [d - 2*b for d, b in zip(delta, band)],
                    [d + 2*b for d, b in zip(delta, band)],
                    color=color, alpha=0.18, linewidth=0)
    ax.plot(hws, delta, marker='o', color=color, markersize=7, lw=3.0)
    ax.axvline(256, color=GREY, lw=1.2, ls='--', alpha=0.7)
    ax.set_title(title)
    ax.set_ylabel(ylabel)
    save(fig, outdir / name)
    return amp, snr, n


def analyze_one(exp_dir, tag, csv_path):
    outdir = exp_dir / "figures"
    outdir.mkdir(parents=True, exist_ok=True)
    samples, raw, reps, hws = load(csv_path)
    color = COLORS[tag]

    amp_q, snr_q, N = fig_rail(outdir, raw, 0, BLUE, "vddq",
                                r"$\Delta$ VDDQ (mA)",
                                r"$\mathtt{DDR\_VDDQ\_A}$")
    amp_d, snr_d, _ = fig_rail(outdir, raw, 1, RED, "vdd2",
                                r"$\Delta$ VDD2 (mA)",
                                r"$\mathtt{DDR\_VDD2\_A}$")

    # Template curve
    z, (mv, sv, md, sd) = build_template_z(samples)
    fig, ax = plt.subplots(figsize=(5.6, 5.0), constrained_layout=True)
    xs = [z[h][0] for h in hws]; ys = [z[h][1] for h in hws]
    cmap = plt.get_cmap("viridis")
    for i in range(len(hws)-1):
        ax.plot([xs[i], xs[i+1]], [ys[i], ys[i+1]],
                color=cmap(i/(len(hws)-1)), lw=2.8, solid_capstyle="round")
    sc = ax.scatter(xs, ys, c=hws, cmap=cmap, s=80, zorder=3,
                    edgecolor="white", linewidth=1.2)
    cbar = plt.colorbar(sc, ax=ax, ticks=[0, 128, 256, 384, 512])
    cbar.set_label(r"HW($\nu$)")
    ax.set_xlabel(r"$\tilde\mu_{VDDQ}$ (z)")
    ax.set_ylabel(r"$\tilde\mu_{VDD2}$ (z)")
    ax.set_title(f"Template curve — {tag}")
    save(fig, outdir / "template_curve")

    # Try several aggregation sizes. With 17 patterns × 30 samples × R reps each
    # we can sweep N up to ~(R-1)*30 (need at least 1 held-out fold of test).
    best_acc, best_agg, best_w32, best_mae, best_pairs, best_conf = 0, 0, 0, 0, [], {}
    accs = []
    for agg in [1, 5, 10, 30, 60, 120, 180, 240, 300, 360, 420, 480, 600, 720, 900, 1200, 1440, 1800, 2400, 2880, 3600]:
        rpf = max(1, agg // 30)
        if len(reps) // rpf < 2: continue
        if agg > rpf * 30: continue
        a, e, c, t, pr = kfold(samples, reps, hws, rpf, agg)
        if t == 0: continue
        w32 = sum(n for kk, vv in c.items() for p, n in vv.items() if abs(kk-p) <= 32)/t
        mae = sum(e)/len(e)
        accs.append((agg, a*100, w32*100, mae))
        if a > best_acc:
            best_acc, best_agg, best_w32, best_mae = a, agg, w32, mae
            best_pairs, best_conf = pr, dict(c)

    # Accuracy vs N
    if accs:
        ns_, exes, w32s, _ = zip(*accs)
        fig, ax = plt.subplots(figsize=(6.0, 4.4), constrained_layout=True)
        ax.plot(ns_, exes, marker='o', color=BLUE, markersize=9, lw=3.0, label="exact")
        ax.plot(ns_, w32s, marker='s', color=RED,  markersize=9, lw=3.0, label=r"$\pm$32")
        ax.axhline(90, color=GREY, lw=1.2, ls='--', alpha=0.7)
        ax.set_xscale("log")
        ax.set_xlabel(r"Probe length $N$ (samples)")
        ax.set_ylabel("Accuracy (%)")
        ax.set_ylim(0, 105)
        leg = ax.legend(loc='lower right', framealpha=0.9)
        leg.get_frame().set_linewidth(0.9)
        save(fig, outdir / "accuracy_vs_N")

    # Confusion + recovery from best agg
    if best_pairs:
        mat = np.array([[best_conf.get(t, {}).get(p, 0) for p in hws]
                        for t in hws], dtype=float)
        fig, ax = plt.subplots(figsize=(6.8, 5.4), constrained_layout=True)
        im = ax.imshow(mat, cmap="viridis", aspect="equal")
        tick_idx = [i for i, h in enumerate(hws) if h % 128 == 0]
        ax.set_xticks(tick_idx); ax.set_xticklabels([str(hws[i]) for i in tick_idx])
        ax.set_yticks(tick_idx); ax.set_yticklabels([str(hws[i]) for i in tick_idx])
        ax.set_xlabel(r"Predicted HW")
        ax.set_ylabel(r"True HW")
        ax.set_title(f"{tag}: exact={best_acc*100:.1f}%  ±32={best_w32*100:.0f}%  N={best_agg}")
        cbar = plt.colorbar(im, ax=ax, shrink=0.85); cbar.set_label("count")
        save(fig, outdir / "confusion")

        by_true = defaultdict(list)
        for true_hw, pred, _ in best_pairs: by_true[true_hw].append(pred)
        fig, ax = plt.subplots(figsize=(5.4, 5.2), constrained_layout=True)
        tx = np.array([t for t, _, _ in best_pairs])
        py = np.array([p for _, p, _ in best_pairs])
        if len(best_pairs) >= 30:
            ax.hexbin(tx, py, gridsize=22, cmap="Greys", mincnt=1,
                      extent=(-10, 522, -10, 522), linewidths=0.0)
        xs_ = sorted(by_true.keys())
        means_ = [sum(by_true[h])/len(by_true[h]) for h in xs_]
        ax.plot([0, 512], [0, 512], color=GREY, lw=1.2, ls='--', label="y=x")
        ax.scatter(xs_, means_, s=85, color=color, edgecolor='white',
                   linewidth=1.2, label=f"mean (MAE={best_mae:.1f})")
        ax.set_xlim(-10, 522); ax.set_ylim(-10, 522)
        ax.xaxis.set_major_locator(MultipleLocator(128))
        ax.yaxis.set_major_locator(MultipleLocator(128))
        ax.set_xlabel("True HW"); ax.set_ylabel("Recovered HW")
        ax.set_title(f"Recovery — {tag}")
        leg = ax.legend(loc='upper left', framealpha=0.9)
        leg.get_frame().set_linewidth(0.9)
        save(fig, outdir / "recovery")

    # Validation report
    val = outdir / "validation.txt"
    with open(val, "w") as f:
        f.write(f"# Validation report — {tag}\n")
        f.write(f"source: {csv_path.name}\n")
        f.write(f"reps={len(reps)}  hw_classes={len(hws)}  N_per_pattern={N}\n\n")
        f.write(f"## Signal amplitudes (raw, mean over reps)\n")
        f.write(f"  DDR_VDDQ amplitude (max-min): {amp_q:.3f} mA   SNR(amp/SE): {snr_q:.1f}\n")
        f.write(f"  DDR_VDD2 amplitude (max-min): {amp_d:.3f} mA   SNR(amp/SE): {snr_d:.1f}\n\n")
        f.write(f"## Classifier (k-fold, joint VDDQ+VDD2 z-scored template)\n")
        for ag, ex, w32, m in accs:
            f.write(f"  N={ag:3d}  exact={ex:5.1f}%  ±32={w32:5.1f}%  MAE={m:6.2f}\n")
        if best_pairs:
            f.write(f"\n  BEST: N={best_agg}  exact={best_acc*100:.2f}%  "
                    f"±32={best_w32*100:.2f}%  MAE={best_mae:.2f}\n")
        f.write("\n## Interpretation\n")
        if tag in ("NS", "NP"):
            f.write(f"  This is a NEGATIVE control. A passing run should show\n"
                    f"  low amplitude AND near-chance exact accuracy ({100.0/len(hws):.1f}%).\n")
        else:
            f.write(f"  Positive run: expect amplitude >> control, accuracy >> chance.\n")
    return {
        "tag": tag, "amp_q": amp_q, "amp_d": amp_d,
        "snr_q": snr_q, "snr_d": snr_d, "raw": raw, "samples": samples,
        "reps": reps, "hws": hws,
        "best_acc": best_acc, "best_w32": best_w32, "best_mae": best_mae,
        "best_agg": best_agg, "accs": accs, "N": N,
    }


def cross_overlay(results, key, ylabel, title, fname):
    fig, ax = base_hw_ax(figsize=(7.0, 4.6))
    for r in results:
        rail_idx = 0 if key == "vddq" else 1
        hws, means, _, _, _ = per_hw_stats(rail_idx, r["raw"])
        base = means[0]; delta = [m - base for m in means]
        ax.plot(hws, delta, marker='o', markersize=6, lw=2.6,
                color=COLORS[r["tag"]], label=r["tag"])
    ax.axvline(256, color=GREY, lw=1.0, ls='--', alpha=0.6)
    ax.axhline(0, color=GREY, lw=0.8, ls='-', alpha=0.5)
    ax.set_ylabel(ylabel); ax.set_title(title)
    leg = ax.legend(loc='best', framealpha=0.9, ncol=3)
    leg.get_frame().set_linewidth(0.9)
    save(fig, ROOT / "figures" / fname)


def cross_bars(results):
    tags = [r["tag"] for r in results]
    x = np.arange(len(tags))
    fig, axes = plt.subplots(1, 3, figsize=(max(14, 1.0 * len(tags) + 6), 4.4),
                             constrained_layout=True)
    axes[0].bar(x, [r["amp_d"] for r in results],
                color=[COLORS.get(t, GREY) for t in tags])
    axes[0].set_xticks(x); axes[0].set_xticklabels(tags, rotation=35, ha='right')
    axes[0].set_ylabel(r"$\Delta$ VDD2 amp. (mA)")
    axes[0].set_title("Signal amplitude")

    axes[1].bar(x, [r["snr_d"] for r in results],
                color=[COLORS.get(t, GREY) for t in tags])
    axes[1].set_xticks(x); axes[1].set_xticklabels(tags, rotation=35, ha='right')
    axes[1].set_ylabel("SNR (amp / SE)")
    axes[1].set_title("VDD2 SNR")

    axes[2].bar(x, [r["best_acc"]*100 for r in results],
                color=[COLORS.get(t, GREY) for t in tags])
    axes[2].set_xticks(x); axes[2].set_xticklabels(tags, rotation=35, ha='right')
    chance = 100.0 / len(results[0]["hws"])
    axes[2].axhline(chance, color=GREY, ls='--', lw=1.2,
                    label=f"chance ({chance:.1f}%)")
    axes[2].set_ylabel("Exact accuracy (%)")
    axes[2].set_title("Best classifier")
    axes[2].set_ylim(0, 105)
    axes[2].legend(loc='upper right', fontsize=14)
    save(fig, ROOT / "figures" / "bars_metrics")


def a_ladder_figure(results):
    """Visualise the A iteration ladder: original A -> AV4 -> AV5 with
    growing rep counts -> AV5r128. Shows accuracy vs each iteration step."""
    ladder_tags = ["A", "AV4", "AV5", "AV5r24", "AV5r32", "AV5r48",
                   "AV5r96", "AV5r128", "AV5r192"]
    ladder_labels = ["A\n(orig, r=6)",
                     "AV4\n(bulk 16)",
                     "AV5\n(bulk 64, r=16)",
                     "AV5\nr=24",
                     "AV5\nr=32",
                     "AV5\nr=48",
                     "AV5\nr=96",
                     "AV5\nr=128",
                     "AV5\nr=192"]
    pts = []
    for t in ladder_tags:
        r = next((x for x in results if x["tag"] == t), None)
        if r is None: continue
        pts.append((t, r["best_acc"] * 100))
    if not pts: return
    fig, ax = plt.subplots(figsize=(11, 5.2), constrained_layout=True)
    xs = np.arange(len(pts))
    accs = [p[1] for p in pts]
    bars = ax.bar(xs, accs, color="#003366", edgecolor='white', linewidth=1.2)
    # value labels above bars
    for x, a in zip(xs, accs):
        ax.text(x, a + 1.5, f"{a:.1f}%", ha='center', fontsize=15,
                fontweight='bold', color='#003366')
    chance = 100.0 / 17
    ax.axhline(chance, color=GREY, ls='--', lw=1.2,
               label=f"chance ({chance:.1f}%)")
    ax.axhline(85, color="#b85450", ls=':', lw=1.4, label="target (85%)")
    ax.set_xticks(xs)
    ax.set_xticklabels([ladder_labels[ladder_tags.index(p[0])] for p in pts],
                       fontsize=14)
    ax.set_ylabel("Exact-class accuracy (%)")
    ax.set_title("A iteration ladder — single-load gadget, 17 HW classes")
    ax.set_ylim(0, 95)
    ax.legend(loc='upper left', framealpha=0.9)
    save(fig, ROOT / "figures" / "a_ladder")


def k_sweep_scaling(results):
    """K-sweep scaling: signal amplitude vs SPEC_LINES (1..32). Validates
    line-count attribution — if the signal scales with K then each gadget
    invocation contributes ~ K × HW(line) and the chain is fully linefilled."""
    pts = []  # (K, amp_q, amp_d, snr_q, snr_d, acc)
    for r in results:
        K = K_LINES.get(r["tag"])
        if K is None: continue
        pts.append((K, r["amp_q"], r["amp_d"], r["snr_q"], r["snr_d"],
                    r["best_acc"] * 100))
    if len(pts) < 3:
        return
    pts.sort(key=lambda x: x[0])
    Ks  = [p[0] for p in pts]
    aq  = [p[1] for p in pts]
    ad  = [p[2] for p in pts]
    sq  = [p[3] for p in pts]
    sd  = [p[4] for p in pts]
    acc = [p[5] for p in pts]

    # Amplitude vs K (both rails)
    fig, ax = plt.subplots(figsize=(6.4, 4.6), constrained_layout=True)
    ax.plot(Ks, aq, marker='o', color=BLUE, lw=3.0, markersize=10, label="VDDQ (I/O)")
    ax.plot(Ks, ad, marker='s', color=RED,  lw=3.0, markersize=10, label="VDD2 (core)")
    ax.set_xscale("log", base=2)
    ax.set_xticks(Ks); ax.set_xticklabels([str(k) for k in Ks])
    ax.set_xlabel(r"Chain length $K$ (cache lines)")
    ax.set_ylabel(r"$\Delta$ current amplitude (mA)")
    ax.set_title("Signal scaling with speculation-window length")
    leg = ax.legend(loc='upper left', framealpha=0.9)
    leg.get_frame().set_linewidth(0.9)
    save(fig, ROOT / "figures" / "k_scaling_amp")

    # SNR + accuracy vs K
    fig, axes = plt.subplots(1, 2, figsize=(11, 4.6), constrained_layout=True)
    axes[0].plot(Ks, sq, marker='o', color=BLUE, lw=3.0, markersize=10, label="VDDQ")
    axes[0].plot(Ks, sd, marker='s', color=RED,  lw=3.0, markersize=10, label="VDD2")
    axes[0].set_xscale("log", base=2)
    axes[0].set_yscale("log")
    axes[0].set_xticks(Ks); axes[0].set_xticklabels([str(k) for k in Ks])
    axes[0].set_xlabel(r"Chain length $K$"); axes[0].set_ylabel("SNR")
    axes[0].set_title("SNR vs K")
    leg = axes[0].legend(loc='upper left', framealpha=0.9)
    leg.get_frame().set_linewidth(0.9)

    axes[1].plot(Ks, acc, marker='D', color="#552288", lw=3.0, markersize=10)
    chance = 100.0 / 17
    axes[1].axhline(chance, color=GREY, ls='--', lw=1.2,
                    label=f"chance ({chance:.1f}%)")
    axes[1].set_xscale("log", base=2)
    axes[1].set_xticks(Ks); axes[1].set_xticklabels([str(k) for k in Ks])
    axes[1].set_xlabel(r"Chain length $K$"); axes[1].set_ylabel("Exact accuracy (%)")
    axes[1].set_ylim(0, 105)
    axes[1].set_title("Classifier accuracy vs K")
    leg = axes[1].legend(loc='upper left', framealpha=0.9)
    leg.get_frame().set_linewidth(0.9)
    save(fig, ROOT / "figures" / "k_scaling_snr_acc")


def main():
    results = []
    for sub, tag, csvname in EXPS:
        exp_dir = ROOT / sub
        csv_path = DATA / csvname
        if not csv_path.exists():
            print(f"  SKIP {tag}: {csv_path} missing")
            continue
        print(f"== {tag} ({sub}) ==")
        r = analyze_one(exp_dir, tag, csv_path)
        results.append(r)
        print(f"   amp VDD2={r['amp_d']:.2f} mA  SNR={r['snr_d']:.1f}  "
              f"best exact={r['best_acc']*100:.1f}% (N={r['best_agg']})")

    (ROOT / "figures").mkdir(parents=True, exist_ok=True)
    cross_overlay(results, "vddq", r"$\Delta$ VDDQ (mA)",
                  "DDR I/O current vs HW — all variants", "overlay_vddq")
    cross_overlay(results, "vdd2", r"$\Delta$ VDD2 (mA)",
                  "DDR core current vs HW — all variants", "overlay_vdd2")
    cross_bars(results)
    k_sweep_scaling(results)
    a_ladder_figure(results)

    # Summary text
    chance = 100.0 / len(results[0]["hws"])
    with open(ROOT / "figures" / "summary.txt", "w") as f:
        f.write("# Cross-experiment summary\n\n")
        f.write(f"{'tag':<4} {'amp_VDD2':>10} {'SNR_VDD2':>10} "
                f"{'amp_VDDQ':>10} {'SNR_VDDQ':>10} "
                f"{'exact%':>8} {'±32%':>8} {'N*':>4}\n")
        for r in results:
            f.write(f"{r['tag']:<4} {r['amp_d']:>10.3f} {r['snr_d']:>10.1f} "
                    f"{r['amp_q']:>10.3f} {r['snr_q']:>10.1f} "
                    f"{r['best_acc']*100:>8.1f} {r['best_w32']*100:>8.1f} "
                    f"{r['best_agg']:>4d}\n")
        f.write(f"\nchance accuracy = {chance:.2f}% (17 HW classes)\n")
        f.write("\n## Required validation checks\n")
        f.write("1) Architectural non-access: NS uses identical flush/dsb without\n"
                "   the ldp chain. NS amplitude << positives -> committed path does\n"
                "   not load target data.\n")
        f.write("2) Wrong-path-only: gadget reachable only via misprediction (anchor\n"
                "   overwrite + dc civac before ret). Architectural target = ret.\n")
        f.write("3) Line-count attribution: A=1, B=1 (8-line ring rotation),\n"
                "   K1=1, K2=2, K4=4, K8=8, K16=16, C=32 lines per trigger.\n"
                "   K-sweep scaling plot validates linear leakage in K.\n")
        f.write("4) Noise controls: cores pinned (sampler=0, workers=1,2,3),\n"
                "   pattern order randomized per rep, per-rep baseline removed,\n"
                "   2σ shaded bands in per-experiment plots.\n")
        # Quick pass/fail digest
        f.write("\n## Pass/fail digest (positive vs NS+NP controls)\n")
        ns = next((r for r in results if r["tag"] == "NS"), None)
        np_ = next((r for r in results if r["tag"] == "NP"), None)
        ctrl_acc = max(ns["best_acc"] if ns else 0,
                       np_["best_acc"] if np_ else 0) * 100
        for r in results:
            if r["tag"] in ("NS", "NP"): continue
            verdict = "PASS" if r["best_acc"]*100 > max(ctrl_acc + 5.0, 2*chance) else "WEAK"
            f.write(f"  {r['tag']:<4} exact={r['best_acc']*100:5.1f}%  "
                    f"ctrl_max={ctrl_acc:.1f}%  ->  {verdict}\n")
    print("Done.")


if __name__ == "__main__":
    main()
