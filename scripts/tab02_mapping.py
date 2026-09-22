#!/usr/bin/env python3
"""Table 2 - Bank-selection functions recovered by HOMARD on each platform.

Notation follows prior DRAM work: an entry [i, j] denotes PA[i] xor PA[j] and a
single [i] denotes the direct selector PA[i]. The bank index concatenates the
listed functions in order.

The Jetson TX2 row is *recomputed here* from the committed 3000-pair power
measurements: pairs whose VDD_SYS_DDR current falls in the row-conflict level
are collected, their physical-address XOR differences are stacked into a matrix
over GF(2), and the bank selectors are read off as the null space. The ZCU102
and Pi 5 rows are reported from their recorded runs (see data/_meta).

Usage:  python3 scripts/tab02_mapping.py
"""
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import numpy as np
import pandas as pd

from common.style import DATA

BIT_LO, BIT_HI = 14, 30          # PA window for the bank selectors f1, f2.
                                 # PA[12] and PA[13] are excluded: they carry the
                                 # sub-channel and chip selectors s and c, which are
                                 # recovered in their own stages and reported apart.
ROW_CONFLICT_mA = 254.0          # lowest current level = same-bank conflicts


def gf2_rref(M):
    M = M.copy() % 2
    rows, cols = M.shape
    piv, r = [], 0
    for c in range(cols):
        nz = np.nonzero(M[r:, c])[0]
        if nz.size == 0:
            continue
        M[[r, r + nz[0]]] = M[[r + nz[0], r]]
        for rr in range(rows):
            if rr != r and M[rr, c]:
                M[rr] ^= M[r]
        piv.append(c)
        r += 1
        if r == rows:
            break
    return M[:r], piv


def gf2_nullspace(M):
    """Basis of {x : M x^T = 0} over GF(2)."""
    R, piv = gf2_rref(M)
    cols = M.shape[1]
    free = [c for c in range(cols) if c not in piv]
    basis = []
    for f in free:
        v = np.zeros(cols, dtype=np.uint8)
        v[f] = 1
        for i, p in enumerate(piv):
            v[p] = R[i, f]
        basis.append(v)
    return basis


def recover_tx2():
    kk = pd.read_csv(DATA / "tx2/power_3000pairs.csv")
    pair = kk.groupby("probe_pa").agg(mA=("mean_curr_mA", "mean"),
                                      xor=("pa_xor", "first")).reset_index()
    conflicts = pair[pair.mA < ROW_CONFLICT_mA]
    # Stage 3 of the hierarchy-aware recovery (Section 5.2): sample inside
    # ker(c, s). Keeping only differences with PA[12] = PA[13] = 0 restricts the
    # pool to pairs that already agree on chip and sub-channel, which both
    # purifies it against oracle label noise and isolates the bank selectors.
    xors = [x for x in (int(s, 0) for s in conflicts.xor)
            if x & ((1 << 12) | (1 << 13)) == 0]
    M = np.array([[(x >> b) & 1 for b in range(BIT_LO, BIT_HI)] for x in xors],
                 dtype=np.uint8)
    basis = gf2_nullspace(M)
    funcs = [sorted(BIT_LO + i for i, bit in enumerate(v) if bit) for v in basis]
    return len(xors), funcs


def fmt(bits):
    return "{" + ",".join(str(b) for b in bits) + "}" if len(bits) > 2 \
        else "[" + ", ".join(str(b) for b in bits) + "]"


def main():
    n_conf, funcs = recover_tx2()
    ref = json.load(open(DATA / "tx2/bank_recovery.json"))
    truth = [sorted(f) for f in ref["recovered_basis"]["power"]]

    print(f"\nJetson TX2: {n_conf} row-conflict pairs selected by the power "
          f"oracle (< {ROW_CONFLICT_mA:.0f} mA)")
    print(f"  null space dimension : {len(funcs)}")
    for i, f in enumerate(funcs):
        print(f"  f{i+1} = {fmt(f)}")
    ok = sorted(funcs) == sorted(truth)
    print(f"  matches the recorded rank-2 basis: {ok}")

    rows = [
        ("AMD ZCU102", "4 banks", "[13], [14]"),
        ("Raspberry Pi 5", "16 banks", "[12, 33], [31, 32], [31], [12]"),
        ("NVIDIA Jetson TX2", "2 chips x 2 sub-channels x 4 banks",
         "c: [13];  s: [12, 13];  (f1,f2): " + fmt(funcs[0]) + ", " + fmt(funcs[1])),
    ]
    w = max(len(r[0]) for r in rows)
    h = max(len(r[1]) for r in rows)
    print("\nTable 2 - Bank-selection functions recovered by HOMARD")
    print("-" * 110)
    print(f"{'Platform':<{w}}  {'Hierarchy':<{h}}  Retrieved Functions")
    print("-" * 110)
    for p, hi, f in rows:
        print(f"{p:<{w}}  {hi:<{h}}  {f}")
    print("-" * 110)

    out = Path(__import__("os").environ.get("HOMARD_FIGDIR",
               Path(__file__).resolve().parents[1] / "figures"))
    out.mkdir(parents=True, exist_ok=True)
    (out / "table2_mapping.txt").write_text(
        "\n".join(f"{p}\t{hi}\t{f}" for p, hi, f in rows) + "\n")
    print(f"\n  wrote figures/table2_mapping.txt")


if __name__ == "__main__":
    main()
