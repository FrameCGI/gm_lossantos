from __future__ import annotations

import argparse
import json
import os
import struct
import sys
from pathlib import Path

import numpy as np

from . import city, indexdat, lod

TILE_MAGIC = 0x31545647


_STAGE_SCAN = {}  # one scan per stage dir


def _scan_key(stage: Path):
    stage = Path(stage)
    n = 0
    newest = 0.0
    for p in stage.glob('*/index.json'):
        n += 1
        try:
            m = p.stat().st_mtime
        except OSError:
            continue
        if m > newest:
            newest = m
    # merges hide folders through .supersedes
    marks = 0
    mnewest = 0.0
    for p in stage.glob('*/.supersedes'):
        marks += 1
        try:
            m = p.stat().st_mtime
        except OSError:
            continue
        if m > mnewest:
            mnewest = m
    return (n, newest, marks, mnewest)


def _superseded(stage: Path):
    stage = Path(stage)
    out = set()
    for p in stage.glob('*/.supersedes'):
        try:
            name = p.read_text(encoding='utf-8').strip()
        except OSError:
            continue
        if name:
            out.add(name)
    return out


def _scan_stage(stage: Path):
    stage = Path(stage)
    skey = _scan_key(stage)
    cached = _STAGE_SCAN.get(stage)
    if cached is not None and cached[0] == skey:
        return cached[1]

    dead = _superseded(stage)
    tile_files, chunk_files = {}, {}
    for idx_path in sorted(stage.glob('*/index.json')):
        name = idx_path.parent.name
        if name in dead:
            continue
        try:
            idx = json.load(open(idx_path))
        except Exception:
            continue
        base = idx_path.parent
        for tkey, info in idx.get('tiles', {}).items():
            tile_files.setdefault(tkey, []).append(
                (name, base / 'tiles' / info['lod0']['file']))
        for ckey, info in idx.get('phys_chunks', {}).items():
            chunk_files.setdefault(ckey, []).append(
                (name, base / 'phys' / info['file']))

    _STAGE_SCAN[stage] = (skey, (tile_files, chunk_files))
    return tile_files, chunk_files


def load_stage(stage: Path, skip: str):
    tile_files, chunk_files = _scan_stage(stage)
    out_tiles = {k: [p for n, p in v if n != skip]
                 for k, v in tile_files.items()}
    out_chunks = {k: [p for n, p in v if n != skip]
                  for k, v in chunk_files.items()}
    return out_tiles, out_chunks


def update_lod1(path: Path, cell=10.0):
    pos, uv, nrm, ind, subs, names = lod.read_tile(path)
    cpos, cuv, cn, cidx, csubs = lod.cluster(pos, uv, nrm, ind, subs, cell)
    out = path.with_name(path.stem + '_1.bin')
    lod.write_tile(out, cpos, cuv, cn, cidx, csubs, names)
    return out


def _tile_material_names(path: Path):
    with open(path, 'rb') as f:
        hdr = f.read(32)
        magic, ver, flags, vc, ic, sc, mc, stride, res = \
            struct.unpack_from('<IHHIIIIII', hdr, 0)
        f.seek(32 + 16 * sc)
        raw = f.read(64 * mc)
    return [raw[64 * i:64 * (i + 1)].split(b'\0')[0].decode()
            for i in range(mc)]


def merge_keys(index, tile_files, chunk_files, tile_keys, chunk_keys, root,
               lod1=True, log=print):
    covered = 0
    for key in sorted(tile_keys):
        paths = [p for p in tile_files.get(key, []) if p.is_file()]
        if not paths:
            continue
        tx, ty = key.split()
        out_name = f't_{tx}_{ty}_0.bin'
        out = Path(root) / 'tiles' / out_name
        verts = city.merge_tile(paths, out, log=log)
        if not verts:
            continue
        info = index['tiles'].setdefault(key, {})
        info['lod0'] = {'file': out_name}
        if lod1:
            lod1_file = update_lod1(out)
            info['lod1'] = {'file': lod1_file.name}
        covered += 1
    merged_chunks = 0
    for key in sorted(chunk_keys):
        paths = [p for p in chunk_files.get(key, []) if p.is_file()]
        if not paths:
            continue
        cx, cy = key.split()
        out_name = f'g_{cx}_{cy}.bin'
        city.merge_chunk(paths, Path(root) / 'phys' / out_name, log=log)
        index['phys_chunks'].setdefault(key, {})['file'] = out_name
        merged_chunks += 1
    return covered, merged_chunks


def collect_materials(index, tile_keys, root):
    mats = index.setdefault('materials', {})
    known = set(mats.values())
    for key in tile_keys:
        p = Path(root) / 'tiles' / f't_{key.split()[0]}_{key.split()[1]}_0.bin'
        if not p.is_file():
            continue
        for name in _tile_material_names(p):
            if name and name not in known:
                known.add(name)
                mats[str(len(mats))] = name


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument('--stage', required=True)
    ap.add_argument('--root', required=True)
    ap.add_argument('--replace',
                    help='collision-only staging folder for this block')
    ap.add_argument('--detail',
                    help='detail staging folder to merge in')
    ap.add_argument('--sweep', action='store_true',
                    help='re-merge EVERY key the stage knows about - the '
                         'final consistency pass once all blocks are in')
    args = ap.parse_args(argv)
    stage = Path(args.stage)
    root = Path(args.root)
    if not args.sweep and not (args.replace and args.detail):
        ap.error('pass --replace and --detail for one block, or --sweep')

    index_path = root / 'index.json'
    index = json.load(open(index_path))

    if args.sweep:
        tile_files, chunk_files = load_stage(stage, '')
        covered, nchunks = merge_keys(index, tile_files, chunk_files,
                                      set(tile_files), set(chunk_files), root,
                                      lod1=False)
        stale = sorted(set(index.get('tiles', {})) - set(tile_files))
        collect_materials(index, set(tile_files), root)
        json.dump(index, open(index_path, 'w'), separators=(',', ':'))
        indexdat.write_index_dat(index, str(root))
        print(f'sweep: re-merged {covered} tiles and {nchunks} chunk keys '
              f'from the stage')
        if stale:
            print(f'  ! {len(stale)} tile keys in the root have no stage '
                  f'source and keep whatever they had: '
                  f'{", ".join(stale[:10])}')
        print(f'RESULT TILES={covered} CHUNKS={nchunks} '
              f'STALE_KEYS={len(stale)} OK=1')
        return 0

    detail_idx = json.load(open(stage / args.detail / 'index.json'))
    # before the merge on purpose
    if args.replace:
        (stage / args.detail / '.supersedes').write_text(
            args.replace, encoding='utf-8')
    replace_idx = {}
    rp = stage / args.replace / 'index.json'
    if rp.is_file():
        replace_idx = json.load(open(rp))
    affected = set(detail_idx.get('tiles', {}))
    affected |= set(replace_idx.get('tiles', {}))
    affected_chunks = set(detail_idx.get('phys_chunks', {}))
    affected_chunks |= set(replace_idx.get('phys_chunks', {}))

    tile_files, chunk_files = load_stage(stage, args.replace)
    covered, nchunks = merge_keys(index, tile_files, chunk_files,
                                  affected, affected_chunks, root)
    collect_materials(index, affected, root)
    json.dump(index, open(index_path, 'w'), separators=(',', ':'))
    indexdat.write_index_dat(index, str(root))
    print(f'merged detail {args.detail}: {covered} tiles, '
          f'{len(affected_chunks)} chunk keys')


if __name__ == '__main__':
    raise SystemExit(main())