"""
Generate a dataset's tile Z-bounds sidecar (tile_bounds.json).
"""

import argparse
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
if str(REPO) not in sys.path:
    sys.path.insert(0, str(REPO))

from exporter.gtav import tile_bounds


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[1])
    ap.add_argument('--map',
                    help='dataset name under <gmod>/data/lossantos')
    ap.add_argument('--root',
                    help='dataset directory (contains index.json)')
    ap.add_argument('--gmod',
                    default=r'C:\Program Files (x86)\Steam\steamapps\common'
                            r'\GarrysMod\garrysmod',
                    help='Garry\'s Mod install root')
    ap.add_argument('--workers', type=int, default=None,
                    help='scan processes (default: CPU count)')
    ap.add_argument('--check', action='store_true',
                    help='recompute and diff against the sidecar on disk')
    a = ap.parse_args()

    if a.root:
        map_dir = a.root
    elif a.map:
        map_dir = str(Path(a.gmod) / 'data' / 'lossantos' / a.map)
    else:
        ap.error('need --map or --root')

    bounds, nfiles, skipped = tile_bounds.generate(map_dir, workers=a.workers)
    print(f'{map_dir}')
    print(f'  {len(bounds)} tiles bounded from {nfiles} files '
          f'({skipped} unparsed)')

    if a.check:
        have = tile_bounds.load(map_dir)
        missing = sorted(set(bounds) - set(have))
        extra = sorted(set(have) - set(bounds))
        diff = [k for k in bounds if k in have and bounds[k] != have[k]]
        ok = not missing and not extra and not diff
        print(f'  check: {"OK" if ok else "MISMATCH"} '
              f'(missing {len(missing)}, extra {len(extra)}, '
              f'changed {len(diff)})')
        for k in (missing + extra + diff)[:10]:
            print(f'    {k}: on disk {have.get(k)} computed {bounds.get(k)}')
        return 0 if ok else 1

    path = tile_bounds.write_sidecar(map_dir, bounds)
    print(f'  wrote {path}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
