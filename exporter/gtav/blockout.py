"""A second map whose VISUALS are the collision shapes."""

from __future__ import annotations

import argparse
import time
from pathlib import Path

import numpy as np

from . import city, world as gtav


def main(argv=None):
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--stage', required=True,
                    help='staging directory holding one folder per block')
    ap.add_argument('--out', required=True,
                    help='GMod data root, i.e. data/lossantos')
    ap.add_argument('--name', default='lossantos_blockout')
    ap.add_argument('--tile-size', type=int, default=1024)
    ap.add_argument('--spawn', default='-516.1,-372.0,37.5',
                    help='GTA metres x,y,z')
    ap.add_argument('--include-detail', action='store_true',
                    help='also merge the *_detail folders (then it is not a '
                         'pure collision blockout)')
    args = ap.parse_args(argv)

    stage = Path(args.stage)
    if not stage.is_dir():
        raise SystemExit(f'not a directory: {stage}')

    include = None
    # detail folders sit beside the collision ones
    if not args.include_detail:
        include = lambda n: not n.endswith('_detail')

    staging = sorted(p.parent.name for p in stage.glob('*/index.json')
                     if include is None or include(p.parent.name))
    print(f'blockout: merging {len(staging)} collision-only blocks -> '
          f'{args.out}\\{args.name}', flush=True)
    if not staging:
        raise SystemExit('no staging folders to merge')

    started = time.time()
    spawn = np.array([float(v) for v in args.spawn.split(',')]) * gtav.SCALE
    city.merge_staging(stage, Path(args.out), args.name, spawn,
                       args.tile_size, include=include)

    target = Path(args.out) / args.name
    for dat in ('index_tiles.dat', 'index_phys.dat'):
        if not (target / dat).is_file():
            raise SystemExit(f'missing {dat} in {target}')

    print(f'blockout: done in {time.time() - started:.1f}s -> {target}', flush=True)
    print('blockout: load it like any gtav map; visuals are collision '
          'shapes with planar UVs', flush=True)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
