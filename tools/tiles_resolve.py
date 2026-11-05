from __future__ import annotations

import argparse
import json
import os
import random
import sys
import time
from concurrent.futures import ProcessPoolExecutor

import numpy as np

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
if REPO not in sys.path:
    sys.path.insert(0, REPO)

from exporter.gtav import lod

BAND = 4.0
SAME_PLANE = 0.1
INFLATE = 2.0
CELL = 256.0  # XY bucket size for overlap candidates
NZ_MIN = 0.15  # only near-horizontal sheets take part
PAIR_CHUNK = 16384  # candidate pairs per batched evaluation chunk
TRIU_BLOCK_CAP = 1 << 24  # pair-candidates touched per enumeration block


STAMP_FILE = '.tiles_resolve_stamps.json'
RESOLVE_VERSION = '1'


def stamp_key():
    return (f'{RESOLVE_VERSION}:band={BAND}:same={SAME_PLANE}:'
            f'inflate={INFLATE}:cell={CELL}:nzmin={NZ_MIN}')


def overlap_pairs_iter(lo, hi):
    n = len(lo)
    block = max(1, TRIU_BLOCK_CAP // max(n, 1))
    cols = np.arange(n)
    for s in range(0, n, block):
        e = min(s + block, n)
        gi = np.arange(s, e)
        keep = gi[:, None] < cols[None, :]
        keep &= lo[s:e, None, 0] <= hi[None, :, 0]
        keep &= hi[s:e, None, 0] >= lo[None, :, 0]
        keep &= lo[s:e, None, 1] <= hi[None, :, 1]
        keep &= hi[s:e, None, 1] >= lo[None, :, 1]
        if keep.any():
            bi, bj = np.nonzero(keep)  # row-major: i ascending, then j
            yield s + bi, bj


def overlap_pairs(lo, hi):
    out_i, out_j = [], []
    for pi, pj in overlap_pairs_iter(lo, hi):
        out_i.append(pi)
        out_j.append(pj)
    if not out_i:
        return np.zeros(0, np.int64), np.zeros(0, np.int64)
    return np.concatenate(out_i), np.concatenate(out_j)


def lattice(tri):
    a, b, c = tri[0], tri[1], tri[2]
    pts = []
    for i in range(4):
        for j in range(4 - i):
            k = 3 - i - j
            pts.append((i * a + j * b + k * c) / 3.0)
    return np.asarray(pts)


def cover_hits(p, tri2, inflate):
    a, b, c = tri2[0], tri2[1], tri2[2]
    v0 = b - a
    v1 = c - a
    den = v0[0] * v1[1] - v1[0] * v0[1]
    if abs(den) < 1e-9:
        return np.zeros(len(p), bool)
    w = p - a
    u = (w[:, 0] * v1[1] - v1[0] * w[:, 1]) / den
    v = (v0[0] * w[:, 1] - w[:, 0] * v0[1]) / den
    inside = (u >= -1e-6) & (v >= -1e-6) & (u + v <= 1 + 1e-6)

    lim = inflate * inflate
    near = np.zeros(len(p), bool)
    for s0, s1 in ((a, b), (b, c), (c, a)):
        d = s1 - s0
        ll = float(d[0] * d[0] + d[1] * d[1])
        if ll <= 0:
            continue
        t = np.clip(((p - s0) * d).sum(axis=1) / ll, 0.0, 1.0)
        proj = s0 + t[:, None] * d
        near |= ((p - proj) ** 2).sum(axis=1) <= lim
    return inside | near


def plane_z(tri, p):
    a, b, c = tri[0], tri[1], tri[2]
    n = np.cross(b - a, c - a)
    if abs(n[2]) < 1e-9:
        return None
    return a[2] + (n[0] * (p[:, 0] - a[0]) + n[1] * (p[:, 1] - a[1])) / -n[2]


_IJK = [(i, j, 3 - i - j) for i in range(4) for j in range(4 - i)]


def lattice_batch(tri):
    a, b, c = tri[:, 0], tri[:, 1], tri[:, 2]
    return np.stack([((i * a + j * b + k * c) / 3.0) for i, j, k in _IJK],
                    axis=1)


def cover_hits_batch(p, tri2, inflate):
    a, b, c = tri2[:, 0], tri2[:, 1], tri2[:, 2]
    v0 = b - a
    v1 = c - a
    den = v0[:, 0] * v1[:, 1] - v1[:, 0] * v0[:, 1]
    ok = np.abs(den) >= 1e-9
    den = np.where(ok, den, 1.0)
    w = p - a[:, None, :]
    u = (w[:, :, 0] * v1[:, None, 1] - v1[:, None, 0] * w[:, :, 1]) / den[:, None]
    v = (v0[:, None, 0] * w[:, :, 1] - w[:, :, 0] * v0[:, None, 1]) / den[:, None]
    inside = (u >= -1e-6) & (v >= -1e-6) & (u + v <= 1 + 1e-6)

    lim = inflate * inflate
    near = np.zeros(p.shape[:2], bool)
    for s0, s1 in ((a, b), (b, c), (c, a)):
        d = s1 - s0
        ll = d[:, 0] * d[:, 0] + d[:, 1] * d[:, 1]
        ok_ll = ll > 0
        ll = np.where(ok_ll, ll, 1.0)
        t = np.clip(((p - s0[:, None, :]) * d[:, None, :]).sum(axis=2)
                    / ll[:, None], 0.0, 1.0)
        proj = s0[:, None, :] + t[:, :, None] * d[:, None, :]
        near |= (((p - proj) ** 2).sum(axis=2) <= lim) & ok_ll[:, None]
    return ok[:, None] & (inside | near)


def plane_z_batch(tri, p):
    a, b, c = tri[:, 0], tri[:, 1], tri[:, 2]
    n = np.cross(b - a, c - a)
    ok = np.abs(n[:, 2]) >= 1e-9
    nz = np.where(ok, n[:, 2], 1.0)
    z = (a[:, 2][:, None]
         + (n[:, 0][:, None] * (p[:, :, 0] - a[:, 0][:, None])
            + n[:, 1][:, None] * (p[:, :, 1] - a[:, 1][:, None]))
         / (-nz)[:, None])
    return z, ok


def eval_pairs(rt, up, dn, covered):
    for s in range(0, len(up), PAIR_CHUNK):
        u = up[s:s + PAIR_CHUNK]
        d = dn[s:s + PAIR_CHUNK]
        tu = rt[u]
        td = rt[d]
        samples = lattice_batch(td)
        pxy = samples[:, :, :2]
        hit = cover_hits_batch(pxy, tu[:, :, :2], INFLATE).all(axis=1)
        zu, oku = plane_z_batch(tu, pxy)
        zd, okd = plane_z_batch(td, pxy)
        dz = zu - zd
        valid = ((dz > -SAME_PLANE) & (dz < BAND)
                 & oku[:, None] & okd[:, None] & hit[:, None])
        if not valid.any():
            continue
        o = np.argsort(d, kind="stable")
        ds = d[o]
        vs = valid[o]
        starts = np.concatenate((np.zeros(1, np.int64),
                                 np.flatnonzero(np.diff(ds)) + 1))
        covered[ds[starts]] |= np.logical_or.reduceat(vs, starts, axis=0)


def sort_corners(arr):
    order = np.lexsort((arr[:, :, 2], arr[:, :, 1], arr[:, :, 0]))[:, ::-1]
    return np.take_along_axis(arr, order[:, :, None], axis=1)


def plan_pairs(rows, tri, subs, nz):
    rep, other = [], []
    for mat, off, count, voff in subs:
        off, count = int(off), int(count)
        n = count // 3
        if n == 0:
            continue
        base = off // 3
        if n % 2 == 0:
            k = n // 2
            if k > 0:
                f = sort_corners(tri[base:base + k])
                b = sort_corners(tri[base + k:base + 2 * k])
                if np.all(f == b):
                    pick_f = nz[base:base + k] >= nz[base + k:base + 2 * k]
                    fi = np.arange(base, base + k)
                    bi = np.arange(base + k, base + 2 * k)
                    rep.append(np.where(pick_f, fi, bi))
                    other.append(np.where(pick_f, bi, fi))
                    continue
        rep.append(np.arange(base, base + n))
        other.append(np.full(n, -1, np.int64))
    if not rep:
        return np.zeros(0, np.int64), np.zeros(0, np.int64)
    return np.concatenate(rep), np.concatenate(other)


def resolve_tile(path, parsed=None):
    if parsed is None:
        parsed = lod.read_tile(path)
    pos, uv, nrm, idx, subs, names = parsed
    rows = idx.reshape(-1, 3)
    n_rows = len(rows)
    keep = np.ones(n_rows, bool)
    stats = {'rows': n_rows, 'dropped': 0, 'pairs': 0, 'covered': 0}
    if n_rows == 0:
        return keep, stats

    tri = pos[rows]
    geo = np.cross(tri[:, 1] - tri[:, 0], tri[:, 2] - tri[:, 0])
    area = np.linalg.norm(geo, axis=1) * 0.5
    glen = np.linalg.norm(geo, axis=1)
    nz = np.divide(geo[:, 2], glen, out=np.zeros(n_rows), where=glen > 0)

    rep, other = plan_pairs(rows, tri, subs, nz)
    stats['pairs'] = int((other >= 0).sum())
    sel = nz[rep] >= NZ_MIN
    rep, other = rep[sel], other[sel]
    if len(rep) < 2:
        return keep, stats

    rt = tri[rep]
    rarea = area[rep]
    cent = rt.mean(axis=1)

    cell = np.floor(cent[:, :2] / CELL).astype(np.int64)
    order = np.lexsort((cell[:, 1], cell[:, 0]))
    sx, sy = cell[order, 0], cell[order, 1]
    cuts = np.flatnonzero((np.diff(sx) != 0) | (np.diff(sy) != 0)) + 1
    groups = np.split(order, cuts)

    covered = np.zeros((len(rep), 10), bool)

    for g in groups:
        cx, cy = cell[g[0]]
        neigh = [h for h in groups
                 if abs(int(cell[h[0]][0]) - int(cx)) <= 1
                 and abs(int(cell[h[0]][1]) - int(cy)) <= 1]
        idxs = np.unique(np.concatenate(neigh)) if len(neigh) > 1 else g
        if len(idxs) < 2:
            continue
        lo = rt[idxs][:, :, :2].min(axis=1)
        hi = rt[idxs][:, :, :2].max(axis=1)
        for bi, bj in overlap_pairs_iter(lo, hi):
            ii, jj = idxs[bi], idxs[bj]
            dzi = cent[jj, 2] - cent[ii, 2]
            same = np.abs(dzi) <= SAME_PLANE
            up = np.where(dzi > SAME_PLANE, jj, ii)
            dn = np.where(dzi > SAME_PLANE, ii, jj)
            swap = same & (rarea[ii] < rarea[jj])  # coplanar: the feature stays up
            up = np.where(swap, jj, up)
            dn = np.where(swap, ii, dn)
            eval_pairs(rt, up, dn, covered)

    drop = covered.all(axis=1)
    for i in np.flatnonzero(drop):
        keep[rep[i]] = False
        stats['dropped'] += 1
        if other[i] >= 0:
            keep[other[i]] = False
            stats['dropped'] += 1
        stats['covered'] += 1
    return keep, stats


def rewrite(path, keep, parsed=None):
    if parsed is None:
        parsed = lod.read_tile(path)
    pos, uv, nrm, idx, subs, names = parsed
    rows = idx.reshape(-1, 3)
    parts = []
    out_subs = []
    src = 0
    off = 0
    for mat, o, c, v in subs:
        n = int(c) // 3
        seg = keep[src:src + n]
        kept = rows[src:src + n][seg]
        out_subs.append((mat, off, len(kept) * 3, 0))
        parts.append(kept)
        off += len(kept) * 3
        src += n
    out_idx = (np.concatenate(parts, axis=0).reshape(-1) if parts
               else np.zeros(0, np.int64))
    tmp = path + '.tmp'
    lod.write_tile(tmp, pos, uv, nrm, out_idx, out_subs, names)
    os.replace(tmp, path)
    return len(out_idx) // 3


def process(item):
    path, dry = item
    try:
        parsed = lod.read_tile(path)
        keep, stats = resolve_tile(path, parsed)
        has_drops = int(keep.sum()) != stats['rows']
        if not dry and has_drops:
            rewrite(path, keep, parsed)
        if dry and has_drops:
            return path, stats, None
        st = os.stat(path)
        return path, stats, [st.st_size, st.st_mtime_ns]
    except Exception as exc:
        return path, {'error': f'{type(exc).__name__}: {exc}'}, None


def _ref_module(path):
    import importlib.util
    spec = importlib.util.spec_from_file_location('tiles_resolve_ref', path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


_REF = None


def refcmp_one(item):
    path, ref_path = item
    global _REF
    if _REF is None:
        _REF = _ref_module(ref_path)
    name = os.path.basename(path)
    t0 = time.time()
    try:
        keep_new, stats_new = resolve_tile(path)
    except Exception as exc:
        return (name, 'NEW_CRASH', {'error': str(exc)}, {}, time.time() - t0,
                0.0, -1, -1)
    t_new = time.time() - t0
    t0 = time.time()
    try:
        keep_ref, stats_ref = _REF.resolve_tile(path)
    except Exception as exc:
        return (name, 'REF_CRASH', stats_new, {'error': str(exc)},
                t_new, time.time() - t0, int(keep_new.sum()), -1)
    t_ref = time.time() - t0
    ok = bool(np.array_equal(keep_new, keep_ref)) and stats_new == stats_ref
    return (name, 'OK' if ok else 'MISMATCH', stats_new, stats_ref,
            t_new, t_ref, int(keep_new.sum()), int(keep_ref.sum()))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', required=True, help='map data dir (contains tiles/)')
    ap.add_argument('--dry-run', action='store_true')
    ap.add_argument('--limit', type=int, default=0)
    ap.add_argument('--match', default='', metavar='FNMATCH',
                    help='only process LOD0 tiles whose filename matches this '
                         'fnmatch pattern (e.g. "t_4_-24_0.bin"); for targeted '
                         'reruns without touching the rest of the map')
    ap.add_argument('--force', action='store_true',
                    help='reprocess every tile, ignoring incremental stamps')
    ap.add_argument('--workers', type=int,
                    default=max(1, (os.cpu_count() or 4) - 1))
    ap.add_argument('--refcmp', type=int, default=0, metavar='N',
                    help='bit-identity gate: compare N sample tiles against the '
                         'pre-vectorisation reference and exit')
    ap.add_argument('--expect-clean', action='store_true',
                    help='idempotency gate: with --dry-run, exit non-zero unless '
                         'zero rows would still be dropped')
    ap.add_argument('--ref', default=os.path.join(
        os.environ.get('TEMP', '.'), 'opencode', 'tiles_resolve_ref.py'))
    a = ap.parse_args()

    tiles = os.path.join(a.root, 'tiles')
    files = sorted(f for f in os.listdir(tiles)
                   if f.startswith('t_') and f.endswith('_0.bin'))
    if a.match:
        import fnmatch
        files = [f for f in files if fnmatch.fnmatch(f, a.match)]

    if a.refcmp:
        picks = ['t_-20_-15_0.bin', 't_-21_-15_0.bin', 't_0_0_0.bin',
                 't_-1_0_0.bin', 't_0_-1_0.bin', 't_-1_-1_0.bin']
        sample = [f for f in picks if f in files]
        rnd = random.Random(20261028)
        rest = [f for f in files if f not in sample]
        sample += rnd.sample(rest, min(a.refcmp, len(rest)))
        print(f'refcmp: {len(sample)} tiles vs {a.ref}', flush=True)
        t0 = time.time()
        bad = 0
        ref_crash = 0
        tn = tr = 0.0
        with ProcessPoolExecutor(max_workers=a.workers) as ex:
            for name, status, sn, sr, t_new, t_ref, kn, kr in ex.map(
                    refcmp_one,
                    [(os.path.join(tiles, f), a.ref) for f in sample],
                    chunksize=4):
                tn += t_new
                tr += t_ref
                if status == 'REF_CRASH':
                    ref_crash += 1
                    print(f'  REF_CRASH {name}: {sr.get("error", "")[:80]} '
                          f'(new keeps {kn})', flush=True)
                elif status != 'OK':
                    bad += 1
                    print(f'  {status} {name}: keep {kn} vs {kr} '
                          f'stats {sn} vs {sr}', flush=True)
        print(f'refcmp done in {time.time() - t0:.0f}s  '
              f'new={tn:.1f}s ref={tr:.1f}s ({tr / max(tn, 1e-9):.1f}x)  '
              f'mismatches {bad}/{len(sample)}  ref-crashes {ref_crash}')
        print(f'RESULT REFCMP={"OK" if bad == 0 else "FAIL"} '
              f'mismatch={bad} tiles={len(sample)} refcrash={ref_crash}')
        return 1 if bad else 0

    stamp_path = os.path.join(a.root, STAMP_FILE)
    stamps = {}
    if not a.force:
        try:
            with open(stamp_path) as fh:
                data = json.load(fh)
            if data.get('key') == stamp_key():
                stamps = data.get('tiles', {})
        except (OSError, ValueError):
            stamps = {}
    if a.limit:
        files = files[:a.limit]
    if stamps:
        keep_files = []
        skipped = 0
        for f in files:
            st = os.stat(os.path.join(tiles, f))
            if stamps.get(f) == [st.st_size, st.st_mtime_ns]:
                skipped += 1
            else:
                keep_files.append(f)
        files = keep_files
    else:
        skipped = 0
    print(f'{len(files)} LOD0 tiles, workers={a.workers}, '
          f'{"dry run" if a.dry_run else "writing"}'
          + (f', {skipped} stamped and skipped' if skipped else ''), flush=True)

    t0 = time.time()
    tot = {'rows': 0, 'dropped': 0, 'pairs': 0, 'covered': 0}
    errors = []
    done = 0
    with ProcessPoolExecutor(max_workers=a.workers) as ex:
        for path, stats, stamp in ex.map(
                process, [(os.path.join(tiles, f), a.dry_run)
                          for f in files], chunksize=8):
            done += 1
            if 'error' in stats:
                errors.append((os.path.basename(path), stats['error']))
                continue
            if stamp is not None:
                stamps[os.path.basename(path)] = stamp
            for k in tot:
                tot[k] += stats[k]
            if done % 5000 == 0:
                print(f'  {done}/{len(files)}  {time.time() - t0:.0f}s  '
                      f'rows {tot["rows"]}  dropped {tot["dropped"]}', flush=True)

    if files and stamps:
        tmp = stamp_path + '.tmp'
        with open(tmp, 'w') as fh:
            json.dump({'key': stamp_key(), 'tiles': stamps}, fh,
                      separators=(',', ':'))
        os.replace(tmp, stamp_path)

    pct = 100.0 * tot['dropped'] / max(1, tot['rows'])
    print(f'done in {time.time() - t0:.0f}s')
    print(f'  rows {tot["rows"]}  pairs {tot["pairs"]}  dropped {tot["dropped"]} '
          f'({pct:.2f}%)  covered-lowers {tot["covered"]}')
    if errors:
        print(f'  {len(errors)} tiles failed:')
        for name, err in errors[:20]:
            print(f'    {name}: {err}')
    status = 'ERR' if errors else 'OK'
    if a.expect_clean and tot['dropped']:
        status = 'ERR'
    print(f'RESULT TILES_RESOLVE={status} '
          f'rows={tot["rows"]} dropped={tot["dropped"]} pct={pct:.2f}')
    return 0 if status == 'OK' else 1


if __name__ == '__main__':
    sys.exit(main())
