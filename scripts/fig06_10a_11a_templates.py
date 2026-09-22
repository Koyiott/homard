#!/usr/bin/env python3
"""Figures 6a/6b, 10a and 11a - Hamming-weight templating on the Pi 5.

The primitive runs in its G = 0 mode, so the bus transition reduces to HW(V).
An offline joint (VDDQ, VDD2) template is built, then matched online per line.
The same analysis is applied to the three kernel gadgets of Appendix C:

    Fig 6a/6b   committed / architectural gadget (Listing 1, ARM_REF)
    Fig 10a     Spectre-RSB gadget, squashed path only (Listing 3)
    Fig 11a     uncacheable Normal-NC gadget, no flush (Listing 2)

Paper values: 81.5 %, 88.9 % and 85.2 % exact over the nine weight classes
at N = 160 traces per line (11 % uniform-random baseline).

Usage:  python3 scripts/fig06_10a_11a_templates.py
"""
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DATA = ROOT / "data" / "pi5"
OUT = Path(__import__("os").environ.get("HOMARD_FIGDIR", ROOT / "figures"))
ANALYZER = ROOT / "scripts" / "pi5_template" / "analyze_hw_template.py"

# (input csv, tag, paper name for the template panel, paper name for the confusion panel)
VARIANTS = [
    ("hw_template_arch_arm0.csv", "Architectural (Listing 1)",
     "pi_hw_arch_template", "pi_hw_arch_confusion"),
    ("hw_template_spectre_arm21.csv", "Spectre-RSB (Listing 3)",
     None, "pi_hw_spectre_confusion"),
    ("hw_template_uncacheable_arm14.csv", "Uncacheable NC (Listing 2)",
     None, "pi_hw_nc_confusion"),
]


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    for csv, tag, tpl_name, cm_name in VARIANTS:
        work = OUT / "_template_work" / Path(csv).stem
        print(f"\n[{cm_name}] {tag}")
        r = subprocess.run(
            [sys.executable, str(ANALYZER), str(DATA / csv), str(work), tag],
            capture_output=True, text=True)
        for line in r.stdout.splitlines():
            if line.strip():
                print("   " + line.strip())
        if r.returncode != 0:
            print(r.stderr[-2000:])
            raise SystemExit(f"analyzer failed for {csv}")
        for src, dst in (("confusion", cm_name), ("template_curve", tpl_name)):
            if dst is None:
                continue
            for ext in ("pdf", "png"):
                s = work / f"{src}.{ext}"
                if s.exists():
                    shutil.copy(s, OUT / f"{dst}.{ext}")
            print(f"   wrote figures/{dst}.pdf + .png")


if __name__ == "__main__":
    main()
