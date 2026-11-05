import argparse
import random
import struct
from pathlib import Path

import numpy as np


def read_index(d):
    tiles = {}
    with open(d / 'index_tiles.dat', encoding='utf-8') as f:
        for ln in f:
            if ln.startswith('T\t'):
                p = ln.rstrip('\n').split('\t')
                tiles[tuple(int(v) for v in p[1].split())] = p[2:7]
    return tiles


_pages = {}


def blob(d, name):
    p = name[:-4].split('_')
    x, y = int(p[1]), int(p[2])
    pg = d / 'pages' / f'v_{x >> 3}_{y >> 3}.bin'
    if pg not in _pages:
        raw = pg.read_bytes()
        n = struct.unpack_from('<I', raw, 8)[0]
        ent = {}
        for i in range(n):
            no, bo, bl, _ = struct.unpack_from('<IIII', raw, 16 + 16 * i)  # name off, blob off, blob len
            ent[raw[no:raw.index(b'\0', no)].decode()] = (bo, bl)
        _pages[pg] = (raw, ent)
    raw, ent = _pages[pg]
    bo, bl = ent[name]
    return raw[bo:bo + bl]


def border_verts(b, axis, value):
    magic, ver, flags, vc, ic, sc, mc, stride, res = struct.unpack_from('<IHHIIIIII', b, 0)
    off = 32 + 16 * sc + 64 * mc  # header, submeshes, material names
    v = np.frombuffer(b, np.uint8, vc * 28, off).reshape(vc, 28)
    pos = v[:, :12].copy().view('<f4').reshape(vc, 3).astype(np.float64)
    on = np.abs(pos[:, axis] - value) < 1e-3
    return {tuple(np.round(p, 2)) for p in pos[on]}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('map')
    ap.add_argument('--pairs', type=int, default=300)
    ap.add_argument('--seed', type=int, default=1)
    a = ap.parse_args()
    d = Path(a.map)
    tiles = read_index(d)
    random.seed(a.seed)
    for l in range(1, 5):
        pairs = [(k, (k[0] + 1, k[1])) for k in tiles
                 if (k[0] + 1, k[1]) in tiles and tiles[k][l].startswith('t_')
                 and tiles[(k[0] + 1, k[1])][l].startswith('t_')]
        random.shuffle(pairs)
        hit = tot = 0
        full = 0
        for ka, kb in pairs[:a.pairs]:
            bx = kb[0] * 1024.0
            va = border_verts(blob(d, tiles[ka][l]), 0, bx)
            vb = border_verts(blob(d, tiles[kb][l]), 0, bx)
            if not va:
                continue
            h = len(va & vb)
            hit += h
            tot += len(va)
            full += h == len(va)
        print(f'lod{l}: {len(pairs)} t/t pairs, {min(len(pairs), a.pairs)} sampled: '
              f'{hit / max(tot, 1):.1%} of border vertices shared, {full} pairs fully matched')


if __name__ == '__main__':
    main()
