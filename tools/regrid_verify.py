"""tools/regrid_verify.py -- post-migration invariant check."""

import argparse
import multiprocessing as mp
import os
import re
import sys
from collections import Counter

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from exporter.gtav import lod as gtav_lod

DEFAULT_ROOT = (r'C:\Program Files (x86)\Steam\steamapps\common\GarrysMod'
                r'\garrysmod\data\lossantos\lossantos')

EPS = 0.5  # u of float slop allowed past the box
G = {}


def counts_of(path, box):
    C = Counter()
    pos, uv, nrm, ind, subs, names = gtav_lod.read_tile(path)
    if len(ind):
        P = np.asarray(pos)[np.asarray(ind)].reshape(-1, 3, 3)
        mn = P.min(axis=1)
        mx = P.max(axis=1)
        out = ((mn[:, 0] < box[0]) | (mx[:, 0] > box[2])
               | (mn[:, 1] < box[1]) | (mx[:, 1] > box[3]))
        C['out'] = int(out.sum())
        ln = np.linalg.norm(np.asarray(nrm), axis=1)
        zero = ln < 1e-6
        C['nrm0'] = int(zero.sum())
        C['nrm'] = int((((ln < 0.9) | (ln > 1.1)) & ~zero).sum())
        if int(np.asarray(ind).max()) >= len(pos):
            C['idx_range'] += 1
    if subs and max(s[0] for s in subs) >= len(names):
        C['mat_range'] += 1
    return C


def check_one(name):
    m = re.match(r'([tsw])_(-?\d+)_(-?\d+)', name)
    if not m:
        return name, Counter({'unparsed_name': 1}), Counter()
    tx, ty = int(m.group(2)), int(m.group(3))
    x0, y0 = tx * 1024.0, ty * 1024.0
    box = (x0 - EPS, y0 - EPS, x0 + 1024.0 + EPS, y0 + 1024.0 + EPS)
    new = Counter()
    old = Counter()
    try:
        new = counts_of(os.path.join(G['tiles'], name), box)
    except Exception as e:
        new['read_fail'] += 1
        new['err'] = str(e)[:80]
    try:
        old = counts_of(os.path.join(G['bak'], name), box)
    except Exception:
        old['read_fail'] += 1
    return name, new, old


def init_worker(ctx):
    G.update(ctx)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('root', nargs='?', default=DEFAULT_ROOT)
    ap.add_argument('--strict', action='store_true',
                    help='also require out_of_box == 0 (full-map runs)')
    ap.add_argument('--limit', type=int, default=0,
                    help='check only the first N installed files')
    ap.add_argument('--jobs', type=int,
                    default=max(1, (os.cpu_count() or 8) - 2))
    args = ap.parse_args()

    root = os.path.abspath(args.root)
    tiles = os.path.join(root, 'tiles')
    bak = os.path.join(root, 'tiles_bak_regrid')
    if not os.path.isdir(bak):
        print('no backup dir', bak)
        return 1

    names = sorted(os.listdir(bak))
    if args.limit:
        names = names[:args.limit]
    print(f'verifying {len(names)} installed files against '
          f'{os.path.basename(bak)} (strict={args.strict})')

    ctx = {'tiles': tiles, 'bak': bak}
    bad = Counter()
    tot_new = Counter()
    tot_old = Counter()
    worse = []
    done = 0
    with mp.Pool(args.jobs, initializer=init_worker, initargs=(ctx,)) as pool:
        for name, new, old in pool.imap_unordered(check_one, names, chunksize=16):
            done += 1
            tot_new.update(new)
            tot_old.update(old)
            for key in ('read_fail', 'idx_range', 'mat_range', 'unparsed_name'):
                if new[key] > old[key]:
                    bad[key] += 1
            if new['out'] > old['out']:
                bad['out_worse'] += 1
                if len(worse) < 15:
                    worse.append(f"{name}: out {old['out']} -> {new['out']}")
            if new['nrm'] > old['nrm']:
                bad['nrm_worse'] += 1
            if args.strict and new['out'] > 0:
                bad['out_strict'] += 1
                if len(worse) < 15:
                    worse.append(f"{name}: strict out {new['out']}")
            if done % 20000 == 0:
                print(f'  {done}/{len(names)}')

    print(f"\ntotals   new: out={tot_new['out']} nrm={tot_new['nrm']} nrm0={tot_new['nrm0']} "
          f"read_fail={tot_new['read_fail']} idx={tot_new['idx_range']} "
          f"mat={tot_new['mat_range']}")
    print(f"backup  : out={tot_old['out']} nrm={tot_old['nrm']} nrm0={tot_old['nrm0']} "
          f"read_fail={tot_old['read_fail']} idx={tot_old['idx_range']} "
          f"mat={tot_old['mat_range']}")
    if worse:
        print('worst offenders:')
        for w in worse:
            print('  ', w)
    print('violations:', dict(bad) or 'NONE')
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
