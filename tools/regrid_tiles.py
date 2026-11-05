"""tools/regrid_tiles.py -- cell-box rebin of the streamed map tiles (Option S)."""

import argparse
import json
import multiprocessing as mp
import os
import pickle
import shutil
import struct
import sys
from collections import Counter, defaultdict

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from exporter.gtav import lod as gtav_lod

DEFAULT_ROOT = (r'C:\Program Files (x86)\Steam\steamapps\common\GarrysMod'
                r'\garrysmod\data\lossantos\lossantos')

AREA_EPS = 1e-9  # u^2, degenerate below this
PROGRESS_EVERY = 2000
CHUNK_TRIS = 20000  # 21845 tris is the u16 limit

G = {}


def fam(name):
    return 's' if name.startswith('s_') else 't'


def build_roles(root):
    with open(os.path.join(root, 'index.json'), encoding='utf-8') as f:
        idx = json.load(f)
    tiles = idx['tiles']
    cellidx = {}
    for k, ent in tiles.items():
        cell = tuple(int(v) for v in k.split())
        cellidx[cell] = [(ent.get(f'lod{i}') or {}).get('file') or None
                         for i in range(5)]

    roles = {}
    for cell, slots in cellidx.items():
        for i, fn in enumerate(slots):
            if not fn:
                continue
            roles.setdefault(fn, {'cell': cell, 'slots': set()})['slots'].add(i)

    tiles_dir = os.path.join(root, 'tiles')
    orphans = []
    for cell, slots in cellidx.items():
        name = 's_%d_%d.bin' % cell
        if name not in roles and os.path.isfile(os.path.join(tiles_dir, name)):
            orphans.append(name)
            roles[name] = {'cell': cell, 'slots': {2, 3, 4}, 'orphan': True}

    for fn, r in roles.items():
        slots = cellidx[r['cell']]
        r.setdefault('orphan', False)
        r['family'] = fam(fn)
        r['has_detail'] = any(slots[i] and fam(slots[i]) == 't' for i in (0, 1))
        fill_src = None
        if slots[1] and fam(slots[1]) == 't':
            fill_src = slots[1]
        elif slots[0] and fam(slots[0]) == 't':
            fill_src = slots[0]
        r['is_fill_source'] = (not r['orphan'] and fn == fill_src
                               and r['family'] == 't')
        r['far_fill_slots'] = tuple(
            s for s in (2, 3, 4)
            if not (slots[s] and fam(slots[s]) == 't'))
    return idx, cellidx, roles, orphans


def lerp(a, b, t):
    v = [x + (y - x) * t for x, y in zip(a, b)]
    nx, ny, nz = v[5], v[6], v[7]
    ln = (nx * nx + ny * ny + nz * nz) ** 0.5
    if ln <= 1e-9:
        for c in (a, b):
            ln = (c[5] * c[5] + c[6] * c[6] + c[7] * c[7]) ** 0.5
            if ln > 1e-9:
                v[5], v[6], v[7] = c[5] / ln, c[6] / ln, c[7] / ln
                return tuple(v)
        return tuple(v)
    v[5] = nx / ln
    v[6] = ny / ln
    v[7] = nz / ln
    return tuple(v)


def clip_poly(poly, rect):
    x0, y0, x1, y1 = rect
    out = poly
    for axis, bound, keep_lo in ((0, x0, True), (0, x1, False),
                                 (1, y0, True), (1, y1, False)):
        if not out:
            return None
        src = out
        out = []
        n = len(src)
        for i in range(n):
            a = src[i]
            b = src[(i + 1) % n]
            da = a[axis] - bound
            db = b[axis] - bound
            ina = da >= 0 if keep_lo else da <= 0
            inb = db >= 0 if keep_lo else db <= 0
            if ina:
                out.append(a)
            if ina != inb:
                out.append(lerp(a, b, da / (da - db)))
    return out or None


def poly_area3(poly):
    a = 0.0
    v0 = poly[0]
    for i in range(1, len(poly) - 1):
        ax = poly[i][0] - v0[0]
        ay = poly[i][1] - v0[1]
        az = poly[i][2] - v0[2]
        bx = poly[i + 1][0] - v0[0]
        by = poly[i + 1][1] - v0[1]
        bz = poly[i + 1][2] - v0[2]
        cx = ay * bz - az * by
        cy = az * bx - ax * bz
        cz = ax * by - ay * bx
        a += (cx * cx + cy * cy + cz * cz) ** 0.5
    return a * 0.5


def poly_tris(poly):
    return [(poly[0], poly[i], poly[i + 1]) for i in range(1, len(poly) - 1)]


def tris_to_arrays(tris):
    k = len(tris)
    pos = np.empty((k, 3, 3), np.float64)
    uv = np.empty((k, 3, 2), np.float64)
    nrm = np.empty((k, 3, 3), np.float64)
    for i, tri in enumerate(tris):
        for j in range(3):
            c = tri[j]
            pos[i, j] = c[0:3]
            uv[i, j] = c[3:5]
            nrm[i, j] = c[5:8]
    return pos, uv, nrm


def detect_double(run, pos):
    count = len(run)
    if count <= 3 or count % 6 != 0:
        return None
    half = count // 2
    a = run[:half].reshape(-1, 3)
    b = run[half:].reshape(-1, 3)
    if len(a) != len(b):
        return None
    pa, pb = pos[a], pos[b]
    forward = (np.all(pa[:, 0] == pb[:, 0], axis=1)
               & np.all(pa[:, 1] == pb[:, 2], axis=1)
               & np.all(pa[:, 2] == pb[:, 1], axis=1))
    same = (np.all(pa[:, 0] == pb[:, 0], axis=1)
            & np.all(pa[:, 1] == pb[:, 1], axis=1)
            & np.all(pa[:, 2] == pb[:, 2], axis=1))
    if not np.all(forward | same):
        return None
    return half


def chunk_runs(auth, mir):
    runs = []
    n = len(auth)
    step = 10000 if mir is not None else 20000
    i = 0
    while i < n:
        j = min(i + step, n)
        part = list(auth[i:j])
        if mir is not None:
            part += list(mir[i:j])
        runs.append(np.array(part, np.int64).reshape(-1))
        i = j
    return runs


def route(piece_cell, slotset, family, has_detail, is_fill_source,
          far_fill_slots, cellidx):
    ent = cellidx.get(piece_cell)
    targets = {}
    C = G['counters']

    for s in sorted(slotset):
        fn = ent[s] if ent else None
        if not fn:
            C['drop_no_target'] += 1
            continue
        ff = fam(fn)
        if ff == family:
            targets.setdefault(fn, 'same')
        elif family == 's':
            if has_detail:
                C['drop_slod'] += 1
            else:
                targets.setdefault(fn, 'fill_sssss')
        else:
            C['drop_t2s'] += 1

    if is_fill_source:
        for s in far_fill_slots:
            fn = ent[s] if ent else None
            if fn and fam(fn) == 't' and fn not in targets:
                targets[fn] = 'fill'
    return targets


def init_worker(ctx):
    G.update(ctx)


def process_file(rec):
    (name, owner, slotset, family, has_detail, is_fill_source,
     far_fill_slots) = rec
    C = Counter()
    G['counters'] = C
    path = os.path.join(G['tiles_dir'], name)
    pos, uv, nrm, ind, subs, names = gtav_lod.read_tile(path)

    TS = G['TS']
    ox0, oy0 = owner[0] * TS, owner[1] * TS
    box = (ox0, oy0, ox0 + TS, oy0 + TS)
    keep_cell = None if G['orphan'][name] else owner

    new_pos = [pos]
    new_uv = [uv]
    new_nrm = [nrm]
    vbase = len(pos)
    export_acc = {}
    out_runs = []  # (mat, auth triples, mirror triples or None)
    changed = False

    def add_verts(poly):
        nonlocal vbase
        P = np.array([c[0:3] for c in poly], np.float64)
        U = np.array([c[3:5] for c in poly], np.float64)
        N = np.array([c[5:8] for c in poly], np.float64)
        new_pos.append(P)
        new_uv.append(U)
        new_nrm.append(N)
        base = vbase
        vbase += len(poly)
        return list(range(base, base + len(poly)))

    for sub_ix, (mat, off, count, _v) in enumerate(subs):
        run = np.asarray(ind[off:off + count], np.int64)
        if count <= 0:
            continue
        half = detect_double(run, pos)
        if half is not None:
            auth = run[:half].reshape(-1, 3)
            mir = run[half:].reshape(-1, 3)
        else:
            auth = run.reshape(-1, 3)
            mir = None
        auth_out = []
        mir_out = []
        mat_name = names[mat] if mat < len(names) else '?%d' % mat

        for i, tri in enumerate(auth):
            P = pos[tri]
            U = uv[tri]
            N = nrm[tri]
            if (keep_cell is not None
                    and P[:, 0].min() >= box[0] and P[:, 0].max() <= box[2]
                    and P[:, 1].min() >= box[1] and P[:, 1].max() <= box[3]):
                auth_out.append(tuple(int(v) for v in tri))
                if mir is not None:
                    mir_out.append(tuple(int(v) for v in mir[i]))
                C['kept_tris'] += 1
                continue

            changed = True
            minx, miny = float(P[:, 0].min()), float(P[:, 1].min())
            maxx, maxy = float(P[:, 0].max()), float(P[:, 1].max())
            verts = [tuple(P[j]) + tuple(U[j]) + tuple(N[j]) for j in range(3)]
            for cy in range(int(np.floor(miny / TS)),
                            int(np.floor(maxy / TS)) + 1):
                for cx in range(int(np.floor(minx / TS)),
                                int(np.floor(maxx / TS)) + 1):
                    rect = (cx * TS, cy * TS, (cx + 1) * TS, (cy + 1) * TS)
                    poly = clip_poly(verts, rect)
                    if not poly or len(poly) < 3:
                        continue
                    px = sum(c[0] for c in poly) / len(poly)
                    py = sum(c[1] for c in poly) / len(poly)
                    cxy = (int(np.floor(px / TS)), int(np.floor(py / TS)))
                    if cxy != (cx, cy):
                        C['reject_offcell'] += 1
                        continue
                    if poly_area3(poly) <= AREA_EPS:
                        C['drop_degenerate'] += 1
                        continue
                    if keep_cell is not None and cxy == keep_cell:
                        ids = add_verts(poly)
                        for k in range(1, len(ids) - 1):
                            auth_out.append((ids[0], ids[k], ids[k + 1]))
                            if mir is not None:
                                mir_out.append((ids[0], ids[k + 1], ids[k]))
                        C['kept_pieces'] += 1
                        continue
                    targets = route(cxy, slotset, family, has_detail,
                                    is_fill_source, far_fill_slots,
                                    G['cellidx'])
                    if not targets:
                        continue
                    ptris = poly_tris(poly)
                    parr = tris_to_arrays(ptris)
                    for tgt, why in targets.items():
                        C['export_' + why] += 1
                        key = (tgt, sub_ix, mat_name, mir is not None)
                        export_acc.setdefault(key, []).append(parr)

        if auth_out:
            out_runs.append((mat, auth_out,
                             mir_out if mir is not None else None))

    exports = defaultdict(list)
    for (tgt, sub_ix, mat_name, doubled), arrs in export_acc.items():
        exports[tgt].append((sub_ix, mat_name, doubled,
                             np.concatenate([a[0] for a in arrs]),
                             np.concatenate([a[1] for a in arrs]),
                             np.concatenate([a[2] for a in arrs])))

    if not changed or keep_cell is None:
        return name, False, dict(exports), C

    P = np.concatenate(new_pos) if len(new_pos) > 1 else pos
    U = np.concatenate(new_uv) if len(new_uv) > 1 else uv
    Nn = np.concatenate(new_nrm) if len(new_nrm) > 1 else nrm
    all_idx = []
    new_subs = []
    ioff = 0
    for mat, auth_out, mir_out in out_runs:
        auth = [np.array(t, np.int64) for t in auth_out]
        mir = ([np.array(t, np.int64) for t in mir_out]
               if mir_out is not None else None)
        for run in chunk_runs(auth, mir):
            new_subs.append((mat, ioff, len(run), 0))
            all_idx.append(run)
            ioff += len(run)
    if new_subs:
        idx_out = np.concatenate(all_idx)
        gtav_lod.write_tile(os.path.join(G['tmp'], 'kept', name),
                          P, U, Nn, idx_out, new_subs, names)
    else:
        write_empty_tile(os.path.join(G['tmp'], 'kept', name), names)
    return name, True, dict(exports), C


def write_empty_tile(path, names):
    with open(path, 'wb') as f:
        f.write(struct.pack('<IHHIIIII', gtav_lod.TILE_MAGIC, 1, 3,
                            0, 0, 0, len(names), 28))
        f.write(struct.pack('<I', 0))
        for nm in names:
            raw = nm.encode('ascii')
            f.write(raw + b'\0' * (64 - len(raw)))


def append_groups(name):
    exp_path = os.path.join(G['tmp'], 'exp', name + '.groups')
    groups = []
    with open(exp_path, 'rb') as f:
        while True:
            try:
                groups.append(pickle.load(f))
            except EOFError:
                break
    kept = os.path.join(G['tmp'], 'kept', name)
    base = kept if os.path.isfile(kept) else os.path.join(G['tiles_dir'], name)
    pos, uv, nrm, ind, subs, names = gtav_lod.read_tile(base)

    new_pos = [pos]
    new_uv = [uv]
    new_nrm = [nrm]
    vbase = len(pos)
    all_idx = [np.asarray(ind, np.int64)]
    out_subs = [(m, o, c, 0) for (m, o, c, _v) in subs]
    ioff = len(ind)
    name_ix = {nm: i for i, nm in enumerate(names)}
    names = list(names)
    n_added = 0

    for (sub_ix, mat_name, doubled, gpos, guv, gnrm) in groups:
        if doubled:
            tri_pos = np.concatenate([gpos, gpos[:, [0, 2, 1], :]], axis=0)
            tri_uv = np.concatenate([guv, guv[:, [0, 2, 1], :]], axis=0)
            tri_nrm = np.concatenate([gnrm, -gnrm[:, [0, 2, 1], :]], axis=0)
        else:
            tri_pos, tri_uv, tri_nrm = gpos, guv, gnrm
        flat_p = tri_pos.reshape(-1, 3)
        flat_u = tri_uv.reshape(-1, 2)
        flat_n = tri_nrm.reshape(-1, 3)
        ids = np.arange(vbase, vbase + len(flat_p), dtype=np.int64)
        vbase += len(flat_p)
        new_pos.append(flat_p)
        new_uv.append(flat_u)
        new_nrm.append(flat_n)
        mi = name_ix.get(mat_name)
        if mi is None:
            mi = len(names)
            names.append(mat_name)
            name_ix[mat_name] = mi
        k = len(gpos)
        triples = ids.reshape(-1, 3)
        auth = [triples[j] for j in range(k)]
        mir = ([triples[k + j] for j in range(k)] if doubled else None)
        for run in chunk_runs(auth, mir):
            out_subs.append((mi, ioff, len(run), 0))
            all_idx.append(run)
            ioff += len(run)
        n_added += k

    P = np.concatenate(new_pos)
    U = np.concatenate(new_uv)
    Nn = np.concatenate(new_nrm)
    idx_out = np.concatenate(all_idx)
    gtav_lod.write_tile(os.path.join(G['tmp'], 'final', name),
                      P, U, Nn, idx_out, out_subs, names)
    return name, n_added


def parse_centers(spec):
    out = []
    for part in spec.split(';'):
        part = part.strip()
        if not part:
            continue
        x, y = part.split(',')
        out.append((float(x), float(y)))
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--root', default=DEFAULT_ROOT)
    ap.add_argument('--dry-run', action='store_true')
    ap.add_argument('--apply', action='store_true')
    ap.add_argument('--jobs', type=int,
                    default=max(1, (os.cpu_count() or 8) - 2))
    ap.add_argument('--centers', default='',
                    help='world x,y pairs separated by ; to scope source files')
    ap.add_argument('--radius', type=int, default=12,
                    help='scope radius in cells around each center')
    ap.add_argument('--limit', type=int, default=0, help='debug: cap sources')
    args = ap.parse_args()
    if not args.dry_run and not args.apply:
        ap.error('pick --dry-run or --apply')

    root = os.path.abspath(args.root)
    tiles_dir = os.path.join(root, 'tiles')
    tmp = os.path.join(root, 'regrid_tmp')
    bak = os.path.join(root, 'tiles_bak_regrid')

    print(f'root {root}')
    idx, cellidx, roles, orphans = build_roles(root)
    TS = idx['tile_size']
    print(f'index: {len(cellidx)} cells, {len(roles)} files in scope universe, '
          f'{len(orphans)} orphan far-land donors')

    centers = parse_centers(args.centers) if args.centers else None
    sources = []
    for name, r in sorted(roles.items()):
        if centers:
            cx, cy = r['cell']
            if not any(abs(cx - x / TS) <= args.radius
                       and abs(cy - y / TS) <= args.radius
                       for x, y in centers):
                continue
        sources.append((name, r['cell'], tuple(sorted(r['slots'])),
                        r['family'], r['has_detail'], r['is_fill_source'],
                        r['far_fill_slots']))
    if args.limit:
        sources = sources[:args.limit]
    print(f'sources in scope: {len(sources)}'
          + (f' (centers {args.centers} r={args.radius})' if centers else ''))

    ctx = {
        'tiles_dir': tiles_dir,
        'tmp': tmp,
        'cellidx': cellidx,
        'TS': TS,
        'orphan': {rec[0]: roles[rec[0]]['orphan'] for rec in sources},
    }

    shutil.rmtree(tmp, ignore_errors=True)
    for sub in ('kept', 'exp', 'final'):
        os.makedirs(os.path.join(tmp, sub), exist_ok=True)

    totals = Counter()
    n_changed = 0
    export_targets = set()
    n_groups = 0

    with mp.Pool(args.jobs, initializer=init_worker, initargs=(ctx,)) as pool:
        done = 0
        for name, kept_changed, exports, C in pool.imap_unordered(
                process_file, sources, chunksize=4):
            totals.update(C)
            if kept_changed:
                n_changed += 1
            for tgt, groups in exports.items():
                export_targets.add(tgt)
                n_groups += len(groups)
                if args.dry_run:
                    continue
                p = os.path.join(tmp, 'exp', tgt + '.groups')
                with open(p, 'ab') as f:
                    for g in groups:
                        pickle.dump(g, f, protocol=4)
            done += 1
            if done % PROGRESS_EVERY == 0:
                print(f'  {done}/{len(sources)} files, {n_changed} changed, '
                      f'{n_groups} export groups')

    print(f'\nphase A done: {n_changed} source files change, '
          f'{len(export_targets)} targets receive {n_groups} piece groups')
    print('piece accounting:')
    for k in sorted(totals):
        print(f'  {k:20s} {totals[k]:12d}')

    if args.dry_run:
        print('\ndry run - nothing written.')
        shutil.rmtree(tmp, ignore_errors=True)
        return

    tlist = sorted(export_targets)
    with mp.Pool(args.jobs, initializer=init_worker, initargs=(ctx,)) as pool:
        done = 0
        for name, n_added in pool.imap_unordered(append_groups, tlist,
                                                 chunksize=4):
            done += 1
            if done % 2000 == 0:
                print(f'  targets {done}/{len(tlist)}')
    print(f'phase B done: {len(tlist)} targets rewritten')

    final_dir = os.path.join(tmp, 'final')
    kept_dir = os.path.join(tmp, 'kept')
    os.makedirs(bak, exist_ok=True)
    final_names = set(os.listdir(final_dir))
    n_installed = 0
    for name in sorted(final_names):
        dst = os.path.join(tiles_dir, name)
        bakp = os.path.join(bak, name)
        if not os.path.exists(bakp):
            shutil.copy2(dst, bakp)
        os.replace(os.path.join(final_dir, name), dst)
        n_installed += 1
    for name in sorted(os.listdir(kept_dir)):
        if name in final_names:
            continue
        dst = os.path.join(tiles_dir, name)
        bakp = os.path.join(bak, name)
        if not os.path.exists(bakp):
            shutil.copy2(dst, bakp)
        os.replace(os.path.join(kept_dir, name), dst)
        n_installed += 1
    print(f'phase C done: {n_installed} files installed, originals in {bak}')
    print('next: python -m exporter.gtav.pack build "%s"' % root)


if __name__ == '__main__':
    main()
