"""
Shrink an installed materials tree's VTFs in place - the VRAM/texture fix.
"""

import argparse
import os
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
if str(REPO) not in sys.path:
    sys.path.insert(0, str(REPO))

from exporter.gtav import vtf_shrink

# vtf image format ids
FMT_NAMES = {12: 'BGRA', 13: 'DXT1', 14: 'DXT3', 15: 'DXT5'}


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[1])
    ap.add_argument('--root', default=None,
                    help='materials tree (default: the lossantos addon)')
    ap.add_argument('--cap', type=int, default=vtf_shrink.DEFAULT_CAP,
                    help=f'max width/height to keep '
                         f'(default {vtf_shrink.DEFAULT_CAP})')
    ap.add_argument('--dry-run', action='store_true',
                    help='report only; touch nothing')
    a = ap.parse_args()

    root = a.root or os.path.join(
        r'C:\Program Files (x86)\Steam\steamapps\common\GarrysMod\garrysmod',
        'addons', 'lossantos', 'materials')

    total = shrinkable = skipped_parse = 0
    before = after = 0
    fmts = {}
    dims = {}
    biggest = []

    for dp, dn, fn in os.walk(root):
        dn.sort()
        for name in sorted(fn):
            if not name.lower().endswith('.vtf'):
                continue
            path = os.path.join(dp, name)
            total += 1
            sz = os.path.getsize(path)
            before += sz
            info = vtf_shrink.parse(path)
            if info is None:
                skipped_parse += 1
                after += sz
                continue
            w, h, fmt, mipcount, sizes = info
            fmts[fmt] = fmts.get(fmt, 0) + 1
            dims[(w, h)] = dims.get((w, h), 0) + 1

            drops, rw, rh = vtf_shrink.plan(info, a.cap)
            kept = sum(sizes[drops:])
            after += kept
            if drops:
                shrinkable += 1
                biggest.append((sz - kept, sz, kept, path))
                if not a.dry_run and vtf_shrink.shrink(path, info, a.cap) is None:
                    shrinkable -= 1

    print(f'{root}')
    print(f'  {total} VTFs | parses: {total - skipped_parse} '
          f'skipped {skipped_parse}')
    print('  formats: ' + ', '.join(
        f'{FMT_NAMES.get(f, f)}: {n}' for f, n in sorted(fmts.items())))
    print('  largest dims: ' + ', '.join(
        f'{w}x{h}: {n}' for (w, h), n in sorted(dims.items(),
                                                key=lambda kv: -max(kv[0]))[:8]))
    print(f'  shrinkable >{a.cap}px: {shrinkable} files')
    print(f'  size {before / 1e9:.2f} GB -> {after / 1e9:.2f} GB '
          f'({100 - 100 * after / max(1, before):.0f}% lighter)'
          + (' [dry run]' if a.dry_run else ''))
    biggest.sort(reverse=True)
    for saved, sz, kept, path in biggest[:10]:
        print(f'    saves {saved / 1e6:6.1f} MB  {sz / 1e6:6.1f} -> '
              f'{kept / 1e6:5.1f} MB  {os.path.relpath(path, root)}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
