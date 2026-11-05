from __future__ import annotations

import argparse
import json
import os
import struct
import time
from concurrent.futures import ProcessPoolExecutor
from pathlib import Path

import numpy as np

from . import indexdat

TILE_MAGIC = 0x31545647
LOD_CELLS = {1: 10.0, 2: 25.0, 3: 50.0, 4: 100.0}  # Source units


def read_tile(path):
    data = open(path, 'rb').read()
    magic, ver, flags, vc, ic, sc, mc, stride, res = struct.unpack_from(
        '<IHHIIIIII', data, 0)
    if magic != TILE_MAGIC:
        raise ValueError(f'bad tile {path}')
    subs = [struct.unpack_from('<IIII', data, 32 + 16 * i) for i in range(sc)]
    names = [data[32 + 16 * sc + 64 * i:32 + 16 * sc + 64 * (i + 1)]
             .split(b'\0')[0].decode() for i in range(mc)]
    off = 32 + 16 * sc + 64 * mc
    # pos 0, uv 12, packed uv 20, nrm 24
    raw = np.frombuffer(data[off:off + vc * 28], np.uint8).reshape(vc, 28)
    pos = raw[:, :12].copy().view('<f4').reshape(vc, 3).astype(np.float64)
    uv = raw[:, 12:20].copy().view('<f4').reshape(vc, 2).astype(np.float64)
    # some drawables ship NaN or huge uvs
    bad_uv = ~np.isfinite(uv) | (np.abs(uv) > 1e6)
    uv[bad_uv] = 0.0
    # int8 first or the negatives come back wrong
    nrm = raw[:, 24:27].astype(np.int8).astype(np.float64) / 127.0
    idx = np.frombuffer(data[off + vc * 28:off + vc * 28 + ic * 2],
                        '<u2').astype(np.int64)
    # fold vertex offsets into one global buffer
    if any(int(v) for (_m, _o, _c, v) in subs):
        idx = idx.copy()
        for _m, o, c, v in subs:
            if c and v:
                idx[o:o + c] += int(v)
        subs = [(m, o, c, 0) for (m, o, c, _v) in subs]
    return pos, uv, nrm, idx, subs, names


def encode_vertices(pos, uv, nrm):
    pos = np.asarray(pos, np.float32)
    uv = np.asarray(uv, np.float32)
    nrm = np.clip(np.round(np.asarray(nrm) * 127.0), -127, 127).astype(np.int8)
    packed = np.clip(np.round(uv * 65535.0), 0, 65535).astype('<u2')
    raw = np.zeros((len(pos), 28), np.uint8)
    raw[:, :12] = pos.view(np.uint8).reshape(-1, 12)
    raw[:, 12:20] = uv.view(np.uint8).reshape(-1, 8)
    raw[:, 20:24] = packed.view(np.uint8).reshape(-1, 4)
    raw[:, 24:27] = nrm.view(np.uint8).reshape(-1, 3)
    return raw.tobytes()


def write_tile(path, pos, uv, nrm, idx, subs, names):
    idx = np.asarray(idx, np.int64)
    pos = np.asarray(pos)
    uv = np.asarray(uv)
    nrm = np.asarray(nrm)

    out_subs = []
    idx_parts = []
    block_ids = []
    idx_offset = 0
    vertex_base = 0
    for sub in subs:
        mat, off, count, voff = (int(sub[0]), int(sub[1]), int(sub[2]),
                                 int(sub[3]))
        if count <= 0 or off < 0 or off + count > len(idx):
            continue
        run = idx[off:off + count] + voff
        if int(run.min()) < 0 or int(run.max()) >= len(pos):
            raise ValueError(
                f'{path}: submesh {mat} references vertex '
                f'{int(run.max())} of {len(pos)}')
        used, local = np.unique(run, return_inverse=True)
        if len(used) > 65535:
            raise ValueError(
                f'{path}: material {mat} needs {len(used)} vertices but one '
                f'submesh block is limited to 65535 (uint16 indices)')
        out_subs.append((mat, idx_offset, len(local), vertex_base))
        idx_parts.append(np.asarray(local, np.int64).reshape(-1))
        block_ids.append(used)
        vertex_base += len(used)
        idx_offset += len(local)
    if not out_subs:
        return

    order = np.concatenate(block_ids)
    out_idx = np.concatenate(idx_parts)
    if int(out_idx.min()) < 0 or int(out_idx.max()) > 65535:
        raise ValueError(
            f'{path}: local index range {int(out_idx.min())}..'
            f'{int(out_idx.max())} does not fit in uint16')

    header = struct.pack('<IHHIIIII', TILE_MAGIC, 1, 3, len(order), len(out_idx),
                         len(out_subs), len(names), 28)
    with open(path, 'wb') as f:
        f.write(header)
        f.write(struct.pack('<I', 0))
        for sub in out_subs:
            f.write(struct.pack('<IIII', *sub))
        for name in names:
            raw = name.encode('ascii')
            f.write(raw + b'\0' * (64 - len(raw)))
        f.write(encode_vertices(pos[order], uv[order], nrm[order]))
        f.write(np.asarray(out_idx, '<u2').tobytes())


def authored_mask(pos, idx, subs):
    keep = np.zeros(len(pos), bool)
    for _mat, off, count, _voff in subs:
        sl = idx[off:off + count]
        if count <= 3 or count % 6 != 0:
            keep[sl] = True
            continue
        half = count // 2
        a = sl[:half].reshape(-1, 3)
        b = sl[half:].reshape(-1, 3)
        if len(a) != len(b):
            keep[sl] = True
            continue

        pa, pb = pos[a], pos[b]
        forward = (np.all(pa[:, 0] == pb[:, 0], axis=1)
                   & np.all(pa[:, 1] == pb[:, 2], axis=1)
                   & np.all(pa[:, 2] == pb[:, 1], axis=1))
        same = (np.all(pa[:, 0] == pb[:, 0], axis=1)
                & np.all(pa[:, 1] == pb[:, 1], axis=1)
                & np.all(pa[:, 2] == pb[:, 2], axis=1))
        if not np.all(forward | same):
            keep[sl] = True
            continue

        keep[sl[:half]] = True
    return keep


def cluster(pos, uv, nrm, idx, subs, cell, coarse=False, nrm_step=None):
    if len(pos) == 0 or len(idx) == 0:
        return pos, uv, nrm, idx, subs


    # 4 = ~14 deg buckets, 2 = ~30 deg, 0 = position only
    NORM_STEP = (2 if coarse else 4) if nrm_step is None else nrm_step
    UV_STEP = 1024.0
    un = np.asarray(nrm, np.float64)
    lengths = np.linalg.norm(un, axis=1)
    un = un / np.maximum(lengths[:, None], 1e-9)
    nb = np.rint(un * NORM_STEP).astype(np.int64)

    q = np.floor(pos / cell).astype(np.int64)
    quv = np.rint(np.asarray(uv, np.float64) * UV_STEP).astype(np.int64)

    keep = authored_mask(pos, idx, subs)

    out_pos = []
    out_uv = []
    out_n = []
    new_idx_parts = []
    new_subs = []
    out_offset = 0
    vertex_base = 0
    # per submesh, never across materials
    for mat, off, count, _voff in subs:
        if count <= 0 or off < 0 or off + count > len(idx):
            continue
        run = np.asarray(idx[off:off + count])
        vids = np.unique(run)

        if not coarse:
            key = np.concatenate([q[vids], nb[vids], quv[vids]], axis=1)
        elif NORM_STEP:
            key = np.concatenate([q[vids], nb[vids]], axis=1)
        else:
            key = q[vids]  # position-only: every vertex in a cell merges
        uniq, inverse = np.unique(key, axis=0, return_inverse=True)
        clusters = len(uniq)
        counts = np.bincount(inverse, minlength=clusters).astype(np.float64)
        sum_pos = np.zeros((clusters, 3), np.float64)
        sum_uv = np.zeros((clusters, 2), np.float64)
        np.add.at(sum_pos, inverse, pos[vids])
        np.add.at(sum_uv, inverse, uv[vids])
        cpos = sum_pos / counts[:, None]
        cuv = sum_uv / counts[:, None]

        sum_n = np.zeros((clusters, 3), np.float64)
        cnt_n = np.zeros(clusters, np.float64)
        k = keep[vids]
        if k.any():
            np.add.at(sum_n, inverse[k], nrm[vids][k])
            np.add.at(cnt_n, inverse[k], 1.0)
        miss = cnt_n <= 0
        if miss.any():
            sum_all = np.zeros((clusters, 3), np.float64)
            cnt_all = np.zeros(clusters, np.float64)
            np.add.at(sum_all, inverse, nrm[vids])
            np.add.at(cnt_all, inverse, 1.0)
            sum_n[miss] = sum_all[miss]
            cnt_n[miss] = cnt_all[miss]

        cn = sum_n / np.maximum(cnt_n, 1.0)[:, None]
        lengths = np.linalg.norm(cn, axis=1)
        cn = cn / np.maximum(lengths[:, None], 1e-9)
        cn[lengths < 1e-6] = (0.0, 0.0, 1.0)

        vmap = np.full(len(pos), -1, np.int64)
        vmap[vids] = np.arange(len(vids))
        tri = inverse[vmap[run]].reshape(-1, 3)
        valid = ((tri[:, 0] != tri[:, 1]) & (tri[:, 1] != tri[:, 2])
                 & (tri[:, 0] != tri[:, 2]))
        tri = tri[valid]
        if len(tri) == 0:
            continue
        flat = tri.reshape(-1) + vertex_base
        new_subs.append((mat, out_offset, len(flat), 0))
        new_idx_parts.append(flat)
        out_offset += len(flat)
        out_pos.append(cpos)
        out_uv.append(cuv)
        out_n.append(cn)
        vertex_base += clusters

    if not new_idx_parts:
        return pos, uv, nrm, idx, subs
    new_idx = np.concatenate(new_idx_parts)
    return (np.concatenate(out_pos), np.concatenate(out_uv),
            np.concatenate(out_n), new_idx, new_subs)


def regen_one(item):
    key, info, root_s, cells, coarse_cells, coarse_nrm, levels, force = item
    root = Path(root_s)
    assigned = []
    written = 0
    try:
        lod0 = info.get('lod0') or {}
        src_name = lod0.get('file')
        if not src_name:
            return key, assigned, written
        src = root / 'tiles' / src_name
        if not src.is_file():
            return key, assigned, written
        stem = src_name[:-4]
        family = src_name[:1]  # t detail, s slod
        pos, uv, nrm, ind, subs, names = read_tile(src)
        src_mtime = src.stat().st_mtime
        for li, (lod, cell) in enumerate(zip((1, 2, 3, 4), cells)):
            if lod not in levels:
                continue
            cur = info.get(f'lod{lod}') or {}
            cur_file = cur.get('file') or ''
            if cur_file and cur_file[:1] != family:  # other family owns this slot
                continue
            out = f'{stem}_{lod}.bin'
            dst = root / 'tiles' / out
            assigned.append((f'lod{lod}', out))
            if (not force and cur_file == out and dst.is_file()
                    and dst.stat().st_mtime >= src_mtime):
                continue
            cc = coarse_cells[li] if li < len(coarse_cells) else 0.0
            use_coarse = cc > 0
            ns = ((coarse_nrm[li] if li < len(coarse_nrm) else 2)
                  if use_coarse else None)
            cpos, cuv, cn, cidx, csubs = cluster(
                pos, uv, nrm, ind, subs, cc if use_coarse else cell,
                coarse=use_coarse, nrm_step=ns)
            write_tile(dst, cpos, cuv, cn, cidx, csubs, names)
            written += 1
        return key, assigned, written
    except Exception as exc:
        return key, f'{type(exc).__name__}: {exc}', 0


def main(argv=None):
    ap = argparse.ArgumentParser(description='Generate LOD1-4 for GVT1 tiles')
    ap.add_argument('--root', required=True,
                    help='data/lossantos/<map> directory')
    ap.add_argument('--cells', default='10,25,50,100',
                    help='LOD1-4 cluster size in Source units')
    ap.add_argument('--coarse-cells', default='0,50,75,150',
                    help='LOD1-4 far-decimation cell in Source units. 0 keeps '
                         'the fine, lighting/UV-safe cluster for that level; '
                         '>0 runs the coarse decimating merge. lod2 keeps '
                         'creases (mid-range, readable); lod3/lod4 run '
                         'position-only (see --coarse-nrm): measured 4-12x '
                         'fewer tris than the normal-keyed merge. lod1 stays '
                         'fine so the near field keeps its detail.')
    ap.add_argument('--coarse-nrm', default='2,2,0,0',
                    help='normal quantisation for the coarse merge, one entry '
                         'per level in --coarse-cells order (0 = position-only, '
                         '2 = ~30deg crease buckets). Only levels running the '
                         'coarse merge consume it; a fine level (coarse cell 0) '
                         'keeps the fine ~14deg bucket regardless. The normal '
                         'key floors the decimation at 29-34%% of '
                         'tris; position-only keeps 7.9%% at cell 75 and 2.6%% '
                         'at cell 150, with sub-3px silhouette error past '
                         'the 819m lod3 boundary.')
    ap.add_argument('--only-levels', default='1,2,3,4',
                    help='comma list of LOD levels to regenerate (e.g. "3,4" '
                         'to retarget only the far tiers)')
    ap.add_argument('--limit', type=int, default=0)
    ap.add_argument('--workers', type=int,
                    default=max(1, (os.cpu_count() or 4) - 1))
    ap.add_argument('--force', action='store_true',
                    help='regenerate even when an output is newer than its lod0')
    args = ap.parse_args(argv)
    root = Path(args.root)
    cells = [float(v) for v in args.cells.split(',')]
    coarse_cells = [float(v) for v in args.coarse_cells.split(',')]
    coarse_nrm = [int(v) for v in args.coarse_nrm.split(',')]
    levels = {int(v) for v in args.only_levels.split(',') if v.strip()}
    idx_path = root / 'index.json'
    index = json.load(open(idx_path))
    tiles = index.get('tiles', {})

    jobs = []
    for key, info in sorted(tiles.items()):
        lod0 = info.get('lod0') or {}
        src_name = lod0.get('file')
        if not src_name:
            continue
        if not (root / 'tiles' / src_name).is_file():
            continue
        jobs.append((key, info, str(root), cells, coarse_cells, coarse_nrm,
                     levels, args.force))
        if args.limit and len(jobs) >= args.limit:
            break

    t0 = time.time()
    errors = []
    written = 0
    with ProcessPoolExecutor(max_workers=args.workers) as ex:
        for n, (key, res, n_written) in enumerate(
                ex.map(regen_one, jobs, chunksize=8), 1):
            if isinstance(res, str):
                errors.append((key, res))
            else:
                for slot, out in res:
                    tiles[key][slot] = {'file': out}
                written += n_written
            if n % 100 == 0:
                print(f'  lod {n}/{len(jobs)}', flush=True)

    json.dump(index, open(idx_path, 'w'), separators=(',', ':'))
    indexdat.write_index_dat(index, str(root))
    if errors:
        print(f'  {len(errors)} tiles failed:')
        for key, err in errors[:20]:
            print(f'    {key}: {err}')
    print(f'lod done: {len(jobs)} tiles, {written} files written '
          f'in {time.time() - t0:.0f}s, workers={args.workers}')
    if written:
        print('NOTE: tiles changed - the streamer reads the bundled sector '
              'pages (pages/v_*.bin), which now shadow these files. Run '
              '`python -m exporter.gtav.pack build <root>` or the game will '
              'keep loading the pre-regen geometry.')
    return 1 if errors else 0


if __name__ == '__main__':
    raise SystemExit(main())