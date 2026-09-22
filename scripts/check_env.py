#!/usr/bin/env python3
"""Verify the artifact environment and data integrity before regenerating.

Checks, in order:
  1. Python version and the four required packages.
  2. Every data file listed in data/_meta/PROVENANCE.tsv is present.
  3. Every file matches the SHA-256 recorded in data/_meta/SHA256SUMS.

Exits non-zero if anything required is missing or altered.

Usage:  python3 scripts/check_env.py
"""
import hashlib
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
META = ROOT / "data" / "_meta"
REQUIRED = [("numpy", "1.24"), ("pandas", "2.0"),
            ("matplotlib", "3.7"), ("scipy", "1.10")]


def vtuple(s):
    out = []
    for p in s.split("."):
        d = "".join(c for c in p if c.isdigit())
        out.append(int(d) if d else 0)
    return tuple(out)


def main():
    ok = True

    print("Python")
    print(f"  {sys.version.split()[0]}  ({sys.executable})")
    if sys.version_info < (3, 8):
        print("  FAIL: Python 3.8 or newer is required")
        ok = False

    print("\nPackages")
    for name, minv in REQUIRED:
        try:
            m = __import__(name)
            have = getattr(m, "__version__", "?")
            good = have == "?" or vtuple(have) >= vtuple(minv)
            print(f"  {'ok  ' if good else 'OLD '} {name:<12} {have:<10} (need >= {minv})")
            ok &= good
        except ImportError:
            print(f"  MISS {name:<12} -- pip install -r requirements.txt")
            ok = False

    prov = META / "PROVENANCE.tsv"
    if not prov.exists():
        print(f"\nFAIL: {prov} is missing")
        sys.exit(1)

    print("\nData files")
    listed, missing = 0, []
    for line in prov.read_text().splitlines()[1:]:
        if not line.strip():
            continue
        rel = line.split("\t")[0]
        listed += 1
        if not (ROOT / rel).exists():
            missing.append(rel)
    print(f"  {listed - len(missing)}/{listed} present")
    for m in missing:
        print(f"  MISSING {m}")
        ok = False

    sums = META / "SHA256SUMS"
    if sums.exists():
        print("\nChecksums")
        bad = 0
        for line in sums.read_text().splitlines():
            if not line.strip():
                continue
            want, rel = line.split(None, 1)
            rel = rel.strip()
            p = ROOT / rel
            if not p.exists():
                continue
            h = hashlib.sha256()
            with open(p, "rb") as fh:
                for chunk in iter(lambda: fh.read(1 << 20), b""):
                    h.update(chunk)
            if h.hexdigest() != want:
                print(f"  MISMATCH {rel}")
                bad += 1
        print(f"  {'all files match' if bad == 0 else f'{bad} file(s) altered'}")
        ok &= bad == 0
    else:
        print("\nChecksums\n  (no SHA256SUMS recorded)")

    print("\n" + ("Environment OK - run 'make figures'." if ok
                  else "Environment NOT ready (see above)."))
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
