import argparse
import struct
from collections import Counter, defaultdict
from concurrent.futures import ProcessPoolExecutor
from pathlib import Path

WATER = 'models/gtav/water/'


def read_index(d):
    tiles, phys = {}, {}
    with open(d / 'index_tiles.dat', encoding='utf-8') as f:
        for ln in f:
            if ln.startswith('T\t'):
                p = ln.rstrip('\n').split('\t')
                tiles[p[1]] = p[2:7]
    with open(d / 'index_phys.dat', encoding='utf-8') as f:
        for ln in f:
            if ln.startswith('P\t'):
                p = ln.rstrip('\n').split('\t')
                phys[p[1]] = p[2]
    return tiles, phys


def read_page(path):
    try:
        raw = path.read_bytes()
    except OSError:
        return {}
    if raw[:4] != b'GVPG':
        return {}
    count = struct.unpack_from('<I', raw, 8)[0]
    out = {}
    for i in range(count):
        no, bo, bl, _ = struct.unpack_from('<IIII', raw, 16 + 16 * i)  # name off, blob off, blob len
        end = raw.index(b'\0', no)
        out[raw[no:end].decode('ascii')] = raw[bo:bo + bl]
    return out


def tile_counts(blob):
    if not blob:
        return None
    magic, ver, flags, vc, ic, sc, mc, stride, res = struct.unpack_from('<IHHIIIIII', blob, 0)
    names = [blob[32 + 16 * sc + 64 * i:32 + 16 * sc + 64 * (i + 1)].split(b'\0')[0].decode()
             for i in range(mc)]
    c = Counter()
    for i in range(sc):
        mat, ioff, icount, voff = struct.unpack_from('<IIII', blob, 32 + 16 * i)
        c[names[mat] if mat < len(names) else '?'] += icount // 3
    return c


def name_xy(name):
    p = name[:-4].split('_') if name.endswith('.bin') else name.split('_')
    return int(p[1]), int(p[2])


def page_of(d, kind, name):
    x, y = name_xy(name)
    return d / 'pages' / f'{kind}_{x >> 3}_{y >> 3}.bin'


def work(job):
    kind, la, lb, items = job
    pa_cache, pb_cache = {}, {}

    def blob(d, cache, name):
        if not name or name == '-':
            return None
        pg = page_of(d, kind, name)
        if pg not in cache:
            cache[pg] = read_page(pg)
        b = cache[pg].get(name)
        if b is None:
            sub = 'tiles' if kind == 'v' else 'phys'
            try:
                b = (d / sub / name).read_bytes()
            except OSError:
                b = None
        return b

    out = []
    for key, fa, fb in items:
        if kind == 'v':
            ca = [tile_counts(blob(la, pa_cache, n)) if fa else None for n in (fa or [None] * 5)]
            cb = [tile_counts(blob(lb, pb_cache, n)) if fb else None for n in (fb or [None] * 5)]
            out.append((key, ca, cb))
        else:
            a = blob(la, pa_cache, fa)
            b = blob(lb, pb_cache, fb)
            out.append((key, struct.unpack_from('<IHHIII', a)[3] if a else None,
                        struct.unpack_from('<IHHIII', b)[3] if b else None))
    return out


def fam(files):
    return ''.join('-' if not f or f == '-' else f[0] for f in files)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('live')
    ap.add_argument('cmap')
    ap.add_argument('--jobs', type=int, default=10)
    args = ap.parse_args()
    la, lb = Path(args.live), Path(args.cmap)
    ta, pa = read_index(la)
    tb, pb = read_index(lb)
    print(f'T keys: live {len(ta)}, C {len(tb)}; only live {len(set(ta) - set(tb))}, only C {len(set(tb) - set(ta))}')
    for lab, tt in (('live', ta), ('C', tb)):
        xs = [int(k.split()[0]) for k in tt]; ys = [int(k.split()[1]) for k in tt]
        nw = [k for k, v in tt.items() if v[0].startswith('t_') or v[0].startswith('s_')]
        lx = [int(k.split()[0]) for k in nw]; ly = [int(k.split()[1]) for k in nw]
        print(f'  {lab}: all keys x {min(xs)}..{max(xs)} y {min(ys)}..{max(ys)}; land keys x {min(lx)}..{max(lx)} y {min(ly)}..{max(ly)}')
    pat_a = Counter(fam(v) for v in ta.values())
    pat_b = Counter(fam(v) for v in tb.values())
    print('slot patterns (lod0..4 family):')
    for pat in sorted(set(pat_a) | set(pat_b), key=lambda p: -(pat_a[p] + pat_b[p])):
        print(f'   {pat}  live {pat_a[pat]:7d}  C {pat_b[pat]:7d}')

    groups = defaultdict(list)
    for k in set(ta) | set(tb):
        x, y = (int(v) for v in k.split())
        groups[(x >> 3, y >> 3)].append((k, ta.get(k), tb.get(k)))
    jobs = [('v', la, lb, v) for v in groups.values()]
    tot = [[Counter(), Counter()] for _ in range(5)]
    water = [[0, 0] for _ in range(5)]
    worst = []
    with ProcessPoolExecutor(args.jobs) as pool:
        for res in pool.map(work, jobs, chunksize=4):
            for key, ca, cb in res:
                for l in range(5):
                    for side, cc in ((0, ca[l]), (1, cb[l])):
                        if not cc:
                            continue
                        for m, n in cc.items():
                            if m.startswith(WATER):
                                water[l][side] += n
                            else:
                                tot[l][side][m] += n
                a0 = sum(n for m, n in (ca[0] or {}).items() if not m.startswith(WATER))
                b0 = sum(n for m, n in (cb[0] or {}).items() if not m.startswith(WATER))
                worst.append((abs(a0 - b0), key, a0, b0))
        for l in range(5):
            sa, sb = sum(tot[l][0].values()), sum(tot[l][1].values())
            print(f'lod{l}: live {sa:13,}  C {sb:13,}  ({(sb - sa) / max(sa, 1):+.1%})   '
                  f'water live {water[l][0]:9,} C {water[l][1]:9,}')
        delta = Counter({m: tot[0][1][m] - tot[0][0][m] for m in set(tot[0][0]) | set(tot[0][1])})
        print('lod0 biggest material deltas (C - live):')
        for m, d in sorted(delta.items(), key=lambda x: -abs(x[1]))[:12]:
            print(f'   {d:+10,}  {m}  (live {tot[0][0][m]:,}, C {tot[0][1][m]:,})')
        worst.sort(reverse=True)
        print('worst lod0 cells:', [(k, a, b) for _, k, a, b in worst[:8]])
        pgroups = defaultdict(list)
        for k in set(pa) | set(pb):
            x, y = (int(v) for v in k.split())
            pgroups[(x >> 3, y >> 3)].append((k, pa.get(k), pb.get(k)))
        xa = xb = 0
        for res in pool.map(work, [('p', la, lb, v) for v in pgroups.values()], chunksize=4):
            for key, a, b in res:
                xa += a or 0
                xb += b or 0
        print(f'phys: live {len(pa)} chunks {xa:,} tris; C {len(pb)} chunks {xb:,} tris; delta {xb - xa:+,}')


if __name__ == '__main__':
    main()
