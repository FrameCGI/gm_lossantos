"""
Generate or check a dataset's offline material flags sidecar.
"""

import argparse
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
if REPO not in sys.path:
    sys.path.insert(0, str(REPO))

from exporter.gtav import flags_sidecar


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[1])
    ap.add_argument("--map",
                    help="dataset name under <gmod>/data/lossantos")
    ap.add_argument("--root",
                    help="dataset directory (contains index.json); an "
                         "alternative to --map/--gmod")
    ap.add_argument("--gmod",
                    help="GarrysMod content root (default: the standard "
                         "Steam install for --map, derived from --root's "
                         "parents otherwise)")
    ap.add_argument("--check", action="store_true",
                    help="recompute and diff against the file on disk; write nothing")
    a = ap.parse_args()

    if a.root:
        root = Path(a.root)
        gmod = Path(a.gmod) if a.gmod else None
    elif a.map:
        gmod = Path(a.gmod) if a.gmod else flags_sidecar.DEFAULT_GMOD
        root = gmod / "data" / "lossantos" / a.map
    else:
        ap.error("pass --map or --root")

    return flags_sidecar.generate(root, gmod=gmod, check=a.check)


if __name__ == "__main__":
    sys.exit(main())
