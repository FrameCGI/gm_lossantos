"""Emit merge-group manifests for native/gtav_acc_test.c from the real map index."""
import argparse
import json
import math
import re
import sys
from pathlib import Path

KEY_RE = re.compile(r'^(-?\d+) (-?\d+)$')
MAX_BATCH = 64  # gtav_acc_test.c MAX_BATCH: grid^2 must fit


def parse_args():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('mapdir', help='map data dir (the one with index.json)')
    ap.add_argument('-o', '--out', help='manifest path (default: stdout)')
    ap.add_argument('--lod', type=int, default=0,
                    help='LOD slot to build at (default 0)')
    ap.add_argument('--grid', type=int, default=None,
                    help='merge grid (default: index merge_grid)')
    ap.add_argument('--limit', type=int, default=None,
                    help='emit at most N groups')
    ap.add_argument('--biggest', action='store_true',
                    help='order by group size, heaviest first')
    ap.add_argument('--cx', type=float, default=None,
                    help='centre X in tile coords (default: index spawn)')
    ap.add_argument('--cy', type=float, default=None,
                    help='centre Y in tile coords (default: index spawn)')
    return ap.parse_args()


def member_file(lods, lod):
    info = lods.get('lod%d' % lod)
    if info:
        return info.get('file')
    for alt in range(lod - 1, -1, -1):
        info = lods.get('lod%d' % alt)
        if info:
            return info.get('file')
    for alt in range(lod + 1, 5):
        info = lods.get('lod%d' % alt)
        if info:
            return info.get('file')
    return None


def spawn_tile(index):
    sp = index.get('spawn')
    ts = float(index.get('tile_size') or 1024)
    if isinstance(sp, dict):
        return float(sp.get('x', 0)) / ts, float(sp.get('y', 0)) / ts
    if isinstance(sp, (list, tuple)) and len(sp) >= 2:
        return float(sp[0]) / ts, float(sp[1]) / ts
    return 0.0, 0.0


def main():
    args = parse_args()
    mapdir = Path(args.mapdir)
    index_path = mapdir / 'index.json'
    if not index_path.is_file():
        print('no index at %s' % index_path, file=sys.stderr)
        return 1

    with open(index_path, 'r', encoding='utf-8') as f:
        index = json.load(f)

    grid = args.grid if args.grid else int(index.get('merge_grid') or 8)
    if grid < 1:
        grid = 1
    if grid * grid > MAX_BATCH:
        print('grid %d needs %d slots per group but gtav_acc_test.c MAX_BATCH is %d'
              % (grid, grid * grid, MAX_BATCH), file=sys.stderr)
        return 1

    tiles = index.get('tiles') or {}
    if not tiles:
        print('index has no tiles', file=sys.stderr)
        return 1

    if (args.cx is None) != (args.cy is None):
        print('--cx and --cy must be given together', file=sys.stderr)
        return 1
    cx, cy = (args.cx, args.cy) if args.cx is not None else spawn_tile(index)

    groups = {}
    fallback = 0
    for key, lods in tiles.items():
        m = KEY_RE.match(key)
        if not m:
            continue
        tx, ty = int(m.group(1)), int(m.group(2))
        gx = math.floor(tx / grid)
        gy = math.floor(ty / grid)
        g = groups.setdefault((gx, gy), [])
        file = member_file(lods, args.lod)
        if not file:
            continue
        if not (lods.get('lod%d' % args.lod) or {}).get('file'):
            fallback += 1
        g.append((file, tx, ty))

    groups = {k: v for k, v in groups.items() if v}
    if not groups:
        print('no group has a tile at lod%d' % args.lod, file=sys.stderr)
        return 1

    def dist2(item):
        (gx, gy), members = item
        ox, oy = gx * grid, gy * grid
        dx = ox + grid * 0.5 - cx
        dy = oy + grid * 0.5 - cy
        return dx * dx + dy * dy

    if args.biggest:
        order = sorted(groups.items(), key=lambda it: (-len(it[1]), it[0]))
    else:
        order = sorted(groups.items(), key=dist2)
    if args.limit is not None:
        order = order[:args.limit]
    if not order:
        print('limit left no groups', file=sys.stderr)
        return 1

    lines = ['# acc_manifest grid=%d lod=%d groups=%d' % (grid, args.lod, len(order))]
    members = 0
    for (gx, gy), tiles_in in order:
        lines.append('G %d %d' % (gx * grid, gy * grid))
        for file, tx, ty in sorted(tiles_in, key=lambda t: (t[2], t[1])):
            lines.append('F %s %d %d' % (file, tx, ty))
            members += 1
    text = '\n'.join(lines) + '\n'

    if args.out:
        Path(args.out).write_text(text, encoding='utf-8')
        print('wrote %s' % args.out, file=sys.stderr)
    else:
        sys.stdout.write(text)

    print('grid=%d lod=%d groups=%d members=%d lod_fallbacks=%d center=%.1f %.1f'
          % (grid, args.lod, len(order), members, fallback, cx, cy),
          file=sys.stderr)
    return 0


if __name__ == '__main__':
    sys.exit(main())
