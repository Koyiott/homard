#!/usr/bin/env python3
"""zcu102_hd_cpa.py — HW(g xor v) CPA on the ZCU102 architectural HD(G,V) gadget.

Port of the Pi's v11_family_cpa.py / spec_v11_cpa_recovery.py to the ZCU102 INA226
acquisition.  The secret byte V is recovered from the DRAM bus-switching leakage
HD(G,V) = 64 * HW(g ^ v), measured on the PS full-power rail VCCPSINTFP (ina226_u76)
— the ZCU102 analog of the Pi's DDR_VDD2 cross-term rail.  The DQ rail VCCO_PSDDR
(u93 / PSDDR) carries only the additive HW(data) term and is reported for contrast.

Modes:
  single <csv> <Vhex> [--rail R]      one detailed recovery figure (rank/rho vs N)
  family <glob>       [--rail R]      grid + summary across many V files

Examples:
  ./zcu102_hd_cpa.py single data/hd_v0x33_n600.csv 0x33
  ./zcu102_hd_cpa.py family 'data/hd_v0x*_fam.csv'
"""
import os, sys, glob, json, argparse
import numpy as np, pandas as pd
import matplotlib; matplotlib.use("Agg")
import matplotlib.pyplot as plt

HERE = os.path.dirname(os.path.abspath(__file__))
FIG  = os.path.join(HERE, "figures")
DATA = os.path.join(HERE, "data")
PRIMARY = "VCCPSINTFP"          # u76 — HD(G,V) cross-term rail (ZCU102 ~ Pi VDD2)
HW_LUT  = np.array([bin(x).count("1") for x in range(256)], dtype=float)

def hw(x): return int(HW_LUT[x])

MAD_K = 5.0   # drop traces whose drift-corrected current is > K*MAD from the median
              # (rejects rare INA spikes from background PS activity; Pearson is
              #  not outlier-robust, and a handful of spikes wreck the CPA)

def load_and_correct(path, rail=PRIMARY, mad_k=MAD_K):
    """anchor-baseline linear drift correction + MAD outlier rejection."""
    df = pd.read_csv(path)
    df["g"] = df["g_byte"].apply(lambda s: int(str(s).strip(), 16))
    col = f"mean_{rail}_mA"
    anc = df[df.is_anchor == 1]
    m   = df[df.is_anchor == 0].copy()
    if len(anc) >= 4 and anc[col].std() >= 0:
        a, b = np.polyfit(anc.time_s, anc[col], 1)
        m["y"] = m[col] - (a * m.time_s + b) + anc[col].mean()
    else:
        m["y"] = m[col]
    if mad_k and len(m) > 10:
        med = m["y"].median()
        mad = (m["y"] - med).abs().median() * 1.4826
        if mad > 0:
            m = m[(m["y"] - med).abs() <= mad_k * mad].copy()
    return m

def cpa_curves(df, v_true, seed=0xC0FFEE, n_pts=60):
    """rank-vs-N + rho-vs-N over all 256 candidates (signed Pearson, leakage is +)."""
    rng  = np.random.default_rng(seed)
    perm = rng.permutation(len(df))
    g = df.g.values[perm]; y = df.y.values[perm]
    M = HW_LUT[(g[None, :] ^ np.arange(256)[:, None])]              # 256 x n
    Ng = np.unique(np.clip(np.round(
        np.logspace(np.log10(5), np.log10(len(g)), n_pts)).astype(int), 5, len(g)))
    r_all = np.zeros((len(Ng), 256)); ranks = np.zeros(len(Ng), dtype=int)
    for k, N in enumerate(Ng):
        yN = y[:N]; MN = M[:, :N]
        yc = yN - yN.mean(); my = MN - MN.mean(1, keepdims=True)
        den = np.sqrt((my ** 2).sum(1) * (yc ** 2).sum())
        r = np.where(den > 0, (my @ yc) / den, 0.0)
        r_all[k] = r; ranks[k] = int((r > r[v_true]).sum()) + 1
    return Ng, r_all, ranks

def stable_rank1(Ng, ranks):
    one = ranks == 1
    for k in range(len(Ng)):
        if one[k:].all(): return int(Ng[k])
    return None

# ----------------------------------------------------------------------- single
def run_single(csv, v_true, rail=PRIMARY):
    m = load_and_correct(csv, rail)
    Ng, r_all, ranks = cpa_curves(m, v_true)
    n1 = stable_rank1(Ng, ranks)
    # contrast: HW(data) rail
    try:
        mp = load_and_correct(csv, "PSDDR"); _, rp, rkp = cpa_curves(mp, v_true)
        psddr_rank = int(rkp[-1])
    except Exception:
        psddr_rank = None

    print("=" * 70)
    print(f"ZCU102 HD(G,V) recovery — {os.path.basename(csv)}  V=0x{v_true:02X} HW={hw(v_true)}")
    print(f"  rail={rail}  attack_traces={len(m)}")
    print(f"  {'N':>6}{'rank':>7}{'r_true':>9}{'r_2nd':>9}")
    for k in range(len(Ng)):
        if Ng[k] in (5, 10, 15, 25, 50, 100, 200, 300, 400, 500) or k == len(Ng) - 1:
            rt = r_all[k, v_true]
            order = np.argsort(-r_all[k]); r2 = r_all[k, order[1] if order[0] == v_true else order[0]]
            print(f"  {Ng[k]:>6}{ranks[k]:>7}{rt:>9.3f}{r2:>9.3f}")
    print(f"  => rank-1 stable at N = {n1}   (final rank={ranks[-1]}, r_true={r_all[-1,v_true]:+.3f})")
    print(f"  contrast PSDDR (HW-rail) final rank = {psddr_rank}")

    fig, ax = plt.subplots(1, 2, figsize=(13, 5))
    a = ax[0]
    for vh in range(256):
        if vh == v_true: continue
        a.plot(Ng, r_all[:, vh], color="0.75", lw=0.3, alpha=0.5)
    a.plot(Ng, r_all[:, v_true], color="#C44E52", lw=2.4, label=f"true V=0x{v_true:02X}")
    if n1: a.axvline(n1, color="k", ls=":", lw=1.0); a.text(n1*1.05, 0.9, f"rank1@N={n1}", fontsize=10)
    a.set_xscale("log"); a.set_xlim(5, len(m)); a.set_ylim(-1, 1); a.axhline(0, color="k", lw=.3)
    a.set_xlabel("N traces"); a.set_ylabel(r"$\rho$  (Pearson, HW($g\oplus v_h$))")
    a.set_title(f"CPA correlation vs N — {rail}"); a.legend(loc="lower right"); a.grid(ls="--", alpha=.3)
    b = ax[1]
    b.plot(Ng, ranks, color="#4C72B0", lw=2.0, marker="o", ms=3)
    b.axhline(1, color="green", ls="--", lw=1.0)
    if n1: b.axvline(n1, color="k", ls=":", lw=1.0)
    b.set_xscale("log"); b.set_yscale("log"); b.set_xlim(5, len(m)); b.set_ylim(0.8, 256)
    b.set_xlabel("N traces"); b.set_ylabel("rank of true V (1 = recovered)")
    b.set_title("Recovery rank vs N"); b.grid(ls="--", alpha=.3)
    fig.suptitle(f"ZCU102 architectural HD(G,V) recovery of V=0x{v_true:02X}  "
                 f"(rank-1 @ N={n1})", fontweight="bold")
    fig.tight_layout(rect=[0, 0, 1, 0.96])
    out = os.path.join(FIG, f"zcu102_recovery_v0x{v_true:02X}.pdf")
    fig.savefig(out, bbox_inches="tight"); fig.savefig(out[:-4] + ".png", dpi=150, bbox_inches="tight")
    print(f"  figure: {out}")
    return dict(V=v_true, n_rank1=n1, final_rank=int(ranks[-1]),
               r_true=float(r_all[-1, v_true]), n_traces=len(m), HW=hw(v_true))

# ----------------------------------------------------------------------- family
def parse_v(path):
    import re
    mo = re.search(r"v0x([0-9A-Fa-f]{2})", os.path.basename(path))
    return int(mo.group(1), 16) if mo else None

def run_family(files, rail=PRIMARY):
    # dedup by V, preferring a high-N `_hi` file over the `_fam` baseline
    best = {}
    for f in files:
        v = parse_v(f)
        if v is None:
            continue
        if v not in best or ("_hi" in os.path.basename(f) and "_hi" not in os.path.basename(best[v])):
            best[v] = f
    items = sorted(best.items(), key=lambda kv: kv[0])
    results = {}
    print("=" * 78)
    print(f"ZCU102 HD(G,V) family CPA — {len(items)} V values  rail={rail}")
    print(f"{'V':<6}{'HW':<4}{'n_tr':<7}{'rank1_N':<9}{'final_rank':<11}{'r_true':<9}")
    for V, f in items:
        m = load_and_correct(f, rail); Ng, r_all, ranks = cpa_curves(m, V)
        n1 = stable_rank1(Ng, ranks)
        results[V] = dict(Ng=Ng.tolist(), r_all=r_all, ranks=ranks.tolist(), n_rank1=n1,
                          final_rank=int(ranks[-1]), r_true=float(r_all[-1, V]),
                          n_traces=len(m), HW=hw(V))
        print(f"0x{V:02X}  {hw(V):<4}{len(m):<7}{str(n1) if n1 else 'FAIL':<9}"
              f"{ranks[-1]:<11}{r_all[-1,V]:+.3f}")
    succ = sum(1 for V in results if results[V]["n_rank1"] is not None)
    n1s = [results[V]["n_rank1"] for V in results if results[V]["n_rank1"]]
    print(f"\nrank-1 success: {succ}/{len(items)}   median rank-1 N = "
          f"{np.median(n1s):.0f}" if n1s else "n/a")

    # grid
    n = len(items); ncols = 5; nrows = (n + ncols - 1) // ncols
    fig, axes = plt.subplots(nrows, ncols, figsize=(4*ncols, 3.2*nrows), squeeze=False)
    for i, (V, f) in enumerate(items):
        ax = axes[i // ncols][i % ncols]; res = results[V]
        Ng = res["Ng"]; r_all = res["r_all"]
        for vh in range(256):
            if vh == V: continue
            ax.plot(Ng, r_all[:, vh], color="0.8", lw=0.25, alpha=0.4)
        ax.plot(Ng, r_all[:, V], color="#C44E52", lw=1.8)
        if res["n_rank1"]:
            ax.axvline(res["n_rank1"], color="k", ls=":", lw=0.8)
            ax.text(res["n_rank1"]*1.1, 0.82, f"N={res['n_rank1']}", fontsize=8)
        ax.axhline(0, color="k", lw=.3); ax.set_xscale("log")
        ax.set_xlim(5, res["n_traces"]); ax.set_ylim(-1, 1); ax.grid(ls="--", alpha=.3)
        t = f"V=0x{V:02X} HW={res['HW']} rank={res['final_rank']}"
        if not res["n_rank1"]: t += " FAIL"
        ax.set_title(t, fontsize=9, fontweight="bold")
    for j in range(n, nrows*ncols): axes[j//ncols][j%ncols].axis("off")
    fig.suptitle(f"ZCU102 architectural HD(G,V) CPA — {succ}/{len(items)} rank-1 "
                 f"({rail})", fontsize=13, fontweight="bold")
    fig.tight_layout(rect=[0, 0, 1, 0.975])
    g_out = os.path.join(FIG, "zcu102_family_recovery.pdf")
    fig.savefig(g_out, bbox_inches="tight"); fig.savefig(g_out[:-4]+".png", dpi=130, bbox_inches="tight")
    print(f"grid: {g_out}")

    # summary
    fig2, ax2 = plt.subplots(1, 3, figsize=(17, 5)); Vs = [V for V, _ in items]; xs = range(len(Vs))
    n1v = [results[V]["n_rank1"] or 0 for V in Vs]
    cols = ["green" if results[V]["n_rank1"] else "red" for V in Vs]
    ax2[0].bar(xs, n1v, color=cols); ax2[0].set_xticks(list(xs))
    ax2[0].set_xticklabels([f"0x{V:02X}" for V in Vs], rotation=90, fontsize=7)
    ax2[0].set_ylabel("N at rank-1"); ax2[0].set_title(f"rank-1 N per V ({succ}/{len(Vs)})", fontweight="bold")
    ax2[0].grid(axis="y", ls="--", alpha=.3)
    rt = [results[V]["r_true"] for V in Vs]
    ax2[1].bar(xs, rt, color=["green" if r > 0 else "red" for r in rt]); ax2[1].set_xticks(list(xs))
    ax2[1].set_xticklabels([f"0x{V:02X}" for V in Vs], rotation=90, fontsize=7)
    ax2[1].set_ylabel("r_true (final)"); ax2[1].set_title("final Pearson r at true V", fontweight="bold")
    ax2[1].axhline(0, color="k", lw=.4); ax2[1].grid(axis="y", ls="--", alpha=.3)
    for V in Vs:
        n1 = results[V]["n_rank1"]
        if n1: ax2[2].scatter(hw(V), n1, color="green", s=70, edgecolors="k", alpha=.6)
        else:  ax2[2].scatter(hw(V), max(n1v) or 1, color="red", marker="x", s=70)
    ax2[2].set_xlabel("HW(V)"); ax2[2].set_ylabel("rank-1 N"); ax2[2].set_title("rank-1 N vs HW(V)", fontweight="bold")
    ax2[2].grid(ls="--", alpha=.3)
    fig2.suptitle("ZCU102 HD(G,V) CPA — universal rank-1 verification", fontsize=13, fontweight="bold")
    fig2.tight_layout(rect=[0, 0, 1, 0.94])
    s_out = os.path.join(FIG, "zcu102_family_summary.pdf")
    fig2.savefig(s_out, bbox_inches="tight"); fig2.savefig(s_out[:-4]+".png", dpi=140, bbox_inches="tight")
    print(f"summary: {s_out}")

    save = {f"0x{V:02X}": {k: v for k, v in results[V].items() if k != "r_all"} for V in results}
    json.dump(save, open(os.path.join(DATA, "zcu102_family_results.json"), "w"), indent=2)
    return results

if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="mode", required=True)
    s = sub.add_parser("single"); s.add_argument("csv"); s.add_argument("V"); s.add_argument("--rail", default=PRIMARY)
    f = sub.add_parser("family"); f.add_argument("glob"); f.add_argument("--rail", default=PRIMARY)
    a = ap.parse_args()
    os.makedirs(FIG, exist_ok=True)
    if a.mode == "single":
        run_single(a.csv, int(a.V, 0), a.rail)
    else:
        run_family(glob.glob(a.glob), a.rail)
