import json
import struct
import sys
from collections import Counter, defaultdict
from pathlib import Path

import numpy as np

MASK = (1 << 64) - 1


def read_tile(path):
    d = path.read_bytes()
    magic, ver, flags, vc, ic, sc, mc, stride, res = struct.unpack_from('<IHHIIIIII', d, 0)
    subs = [struct.unpack_from('<IIII', d, 32 + 16 * i) for i in range(sc)]
    names = [d[32 + 16 * sc + 64 * i:32 + 16 * sc + 64 * (i + 1)].split(b'\0')[0].decode()
             for i in range(mc)]
    off = 32 + 16 * sc + 64 * mc
    v = np.frombuffer(d, np.uint8, vc * 28, off).reshape(vc, 28)
    pos = v[:, 0:12].copy().view('<f4').reshape(vc, 3)
    uv = v[:, 12:20].copy().view('<f4').reshape(vc, 2)
    nrm = v[:, 24:27].copy().view(np.int8).reshape(vc, 3)
    idx = np.frombuffer(d, '<u2', ic, off + vc * 28).astype(np.int64)
    out = defaultdict(list)
    for mat, ioff, icount, voff in subs:
        run = idx[ioff:ioff + icount].reshape(-1, 3) + voff
        out[names[mat]].append(run)
    return {k: np.concatenate(r) for k, r in out.items()}, pos, uv, nrm


def tri_sums(tris, pos, uv, nrm):
    if len(tris) == 0:
        return 0, 0, 0
    p = pos.view(np.uint32)[tris]  # (T, 3, 3) bit patterns
    u = np.round(uv[tris].astype(np.float64) * 256).astype(np.int64)  # (T, 3, 2)
    n = nrm[tris].astype(np.int64)  # (T, 3, 3)
    cp = p.astype(np.uint64)
    # per-corner hashes
    def mix(x):
        x = (x ^ (x >> np.uint64(33))) * np.uint64(0xff51afd7ed558ccd)
        x = (x ^ (x >> np.uint64(33))) * np.uint64(0xc4ceb9fe1a85ec53)
        return x ^ (x >> np.uint64(33))
    with np.errstate(over='ignore'):
        hp = mix(cp[:, :, 0] * np.uint64(73856093) ^ cp[:, :, 1] * np.uint64(19349663) ^ cp[:, :, 2] * np.uint64(83492791))
        hf = mix(hp ^ (u[:, :, 0].astype(np.uint64) * np.uint64(2654435761))
                 ^ (u[:, :, 1].astype(np.uint64) * np.uint64(40503))
                 ^ (n[:, :, 0].astype(np.uint64) << np.uint64(8)) ^ (n[:, :, 1].astype(np.uint64) << np.uint64(24))
                 ^ (n[:, :, 2].astype(np.uint64) << np.uint64(40)))
        start = np.argmin(hp, axis=1)
        order = (start[:, None] + np.arange(3)[None, :]) % 3
        hp = np.take_along_axis(hp, order, axis=1)
        hf = np.take_along_axis(hf, order, axis=1)
        tp = mix(hp[:, 0] * np.uint64(3) ^ mix(hp[:, 1]) * np.uint64(5) ^ mix(mix(hp[:, 2])) * np.uint64(7))
        tf = mix(hf[:, 0] * np.uint64(3) ^ mix(hf[:, 1]) * np.uint64(5) ^ mix(mix(hf[:, 2])) * np.uint64(7))
    return len(tris), int(tp.sum(dtype=np.uint64)), int(tf.sum(dtype=np.uint64))


def stage_tiles(d):
    idx = json.load(open(d / 'index.json'))
    return {k: d / 'tiles' / v['lod0']['file'] for k, v in idx['tiles'].items()}, \
        {k: d / 'phys' / v['file'] for k, v in idx.get('phys_chunks', {}).items()}


def read_chunk(path):
    d = path.read_bytes()
    magic, ver, flags, tc, vc, res = struct.unpack_from('<IHHIII', d, 0)
    v = np.frombuffer(d, '<f4', vc * 3, 20).reshape(vc, 3)
    i = np.frombuffer(d, '<u2', tc * 3, 20 + vc * 12).reshape(tc, 3)
    t = np.sort(np.round(v[i].astype(np.float64) * 100).astype(np.int64).reshape(tc, 9), axis=0)
    return tc, vc, hash(t.tobytes())


def main():
    a, b = Path(sys.argv[1]), Path(sys.argv[2])
    ta, ca = stage_tiles(a)
    tb, cb = stage_tiles(b)
    print(f'tiles: python {len(ta)} C {len(tb)} common {len(set(ta) & set(tb))}')
    only_a, only_b = sorted(set(ta) - set(tb)), sorted(set(tb) - set(ta))
    if only_a or only_b:
        print('  only python:', only_a[:10], ' only C:', only_b[:10])
    stats = Counter()
    worst = []
    mat_delta = Counter()
    for key in sorted(set(ta) & set(tb)):
        ra, pa, ua, na = read_tile(ta[key])
        rb, pb, ub, nb = read_tile(tb[key])
        for m in sorted(set(ra) | set(rb)):
            sa = tri_sums(ra.get(m, np.zeros((0, 3), np.int64)), pa, ua, na)
            sb = tri_sums(rb.get(m, np.zeros((0, 3), np.int64)), pb, ub, nb)
            stats['materials'] += 1
            stats['tris_py'] += sa[0]
            stats['tris_c'] += sb[0]
            if sa == sb:
                stats['identical'] += 1
            elif sa[:2] == sb[:2]:
                stats['same_positions_attr_diff'] += 1
            elif sa[0] == sb[0]:
                stats['same_count_geom_diff'] += 1
                worst.append((0, key, m, sa[0], sb[0]))
            else:
                stats['count_diff'] += 1
                mat_delta[m] += sb[0] - sa[0]
                worst.append((abs(sb[0] - sa[0]), key, m, sa[0], sb[0]))
    print('tile materials:', dict(stats))
    worst.sort(reverse=True)
    for w in worst[:12]:
        print('   ', w[1], w[2], 'py', w[3], 'C', w[4])
    if mat_delta:
        print('  net tri delta by material (C - py):', mat_delta.most_common(8))
    pc = Counter()
    for key in sorted(set(ca) | set(cb)):
        if key not in ca or key not in cb:
            pc['missing'] += 1
            continue
        x, y = read_chunk(ca[key]), read_chunk(cb[key])
        pc['same' if x == y else ('count_same' if x[:2] == y[:2] else 'diff')] += 1
        pc['tris_py'] += x[0]
        pc['tris_c'] += y[0]
    print('phys chunks:', dict(pc))


if __name__ == '__main__':
    main()
