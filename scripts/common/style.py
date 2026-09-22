"""Shared plotting style and paths for HOMARD artifact figure regeneration.

Every figure script imports from here so that panels which sit side by side in
the paper come out with identical fonts, sizes and colours.
"""
import json
import os
from pathlib import Path

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

ROOT = Path(__file__).resolve().parents[2]
DATA = ROOT / "data"
OUT = Path(os.environ.get("HOMARD_FIGDIR", ROOT / "figures"))

# IEEE two-column subfigure style (~3.4in wide panels).
RC = {
    "font.family": "serif",
    "font.serif": ["Times New Roman", "Times", "Nimbus Roman", "DejaVu Serif"],
    "mathtext.fontset": "stix",
    "font.size": 8,
    "axes.labelsize": 8,
    "axes.titlesize": 8,
    "legend.fontsize": 7,
    "xtick.labelsize": 7,
    "ytick.labelsize": 7,
    "figure.dpi": 150,
    "savefig.dpi": 300,
    "axes.grid": True,
    "grid.alpha": 0.25,
    "grid.linewidth": 0.4,
    "axes.linewidth": 0.6,
    "xtick.major.width": 0.5,
    "ytick.major.width": 0.5,
    "xtick.major.size": 3.0,
    "ytick.major.size": 3.0,
    "legend.frameon": True,
    "legend.framealpha": 0.85,
    "legend.edgecolor": "0.6",
    "legend.borderpad": 0.3,
    "legend.handlelength": 1.4,
}
plt.rcParams.update(RC)

# Consistent colours across every figure.
C_CONFLICT = "#c1272d"     # row-buffer conflict / red family
C_NONCONF = "#2e5c8a"      # non-conflict / blue family
C_LEVELS = ["#c1272d", "#e08a1e", "#7b6bb0", "#2e5c8a"]  # 4 TX2 hierarchy levels
C_MODEL = "#c1272d"
C_MEASURED = "#2e5c8a"
PANEL = (3.4, 2.1)


def save(fig, name, out=None):
    """Write <name>.pdf and <name>.png into the figure directory."""
    out = Path(out) if out else OUT
    out.mkdir(parents=True, exist_ok=True)
    for ext in ("pdf", "png"):
        p = out / f"{name}.{ext}"
        fig.savefig(p, bbox_inches="tight", dpi=300 if ext == "png" else None)
    plt.close(fig)
    print(f"  wrote {out.name}/{name}.pdf + .png")


def load_json(rel):
    with open(DATA / rel) as fh:
        return json.load(fh)


def cohens_d(a, b):
    import numpy as np
    a, b = np.asarray(a, float), np.asarray(b, float)
    na, nb = len(a), len(b)
    sp = ((na - 1) * a.std(ddof=1) ** 2 + (nb - 1) * b.std(ddof=1) ** 2) / (na + nb - 2)
    return (b.mean() - a.mean()) / sp ** 0.5


def auc(a, b):
    """Mann-Whitney AUC of separating a from b (1.0 = perfectly separable)."""
    import numpy as np
    from scipy.stats import rankdata
    a, b = np.asarray(a, float), np.asarray(b, float)
    r = rankdata(np.concatenate([a, b]))
    ra = r[: len(a)].sum()
    u = ra - len(a) * (len(a) + 1) / 2
    return max(u, len(a) * len(b) - u) / (len(a) * len(b))
