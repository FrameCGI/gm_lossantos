from __future__ import annotations

import argparse
import json
import os
import shutil
import struct
import subprocess
import sys
import time
from pathlib import Path

import numpy as np

from . import crypto, rpf
from . import flags_sidecar
from . import indexdat
from . import world as gtav
from .ybn import parse_ybn, ybn_bounds

TILE_MAGIC = 0x31545647
CHUNK_MAGIC = 0x31435647
CITY_ARCHIVES = ('x64i.rpf', 'x64j.rpf', 'x64k.rpf', 'x64l.rpf',
                 'x64m.rpf', 'x64n.rpf', 'x64o.rpf', 'x64p.rpf')
DISTRICT_PREFIXES = ('_citye', '_cityw', '_hills')
SKIP_WORDS = ('interior', '_lod', 'metadata', 'prologue', 'destruction',
              'props')


def discover(gta_dir: Path, cipher):
    blocks, roads = [], []
    for outer_name in CITY_ARCHIVES:
        outer = rpf.RpfArchive(gta_dir / outer_name, cipher)
        try:
            for entry in outer.walk():
                if 'levels/gta5/' not in entry.path:
                    continue
                if entry.kind != rpf.BINARY or not entry.name.lower().endswith('.rpf'):
                    continue
                rel = entry.path.split('levels/gta5/', 1)[1]
                parts = rel.split('/')
                if len(parts) < 2 or not parts[0].startswith(DISTRICT_PREFIXES):
                    continue
                low = entry.name.lower()
                if any(word in low for word in SKIP_WORDS):
                    continue
                try:
                    child = outer.open_child(entry)
                    has_ybn = any(x.name.lower().endswith('.ybn')
                                  for x in child.walk())
                    child.close()
                except Exception:
                    continue
                if not has_ybn:
                    continue
                is_road = gtav.is_road_pack(entry.name)
                (roads if is_road else blocks).append(
                    (outer_name, 'levels/gta5/' + rel, entry.name))
        finally:
            outer.close()
    return blocks, roads


def block_world(outer: rpf.RpfArchive, inner: str, log=print):
    child = outer.open_child(outer.get(inner))
    try:
        entries = [x for x in child.walk()
                   if x.name.lower().endswith('.ybn')]
        bases = [x for x in entries
                 if not x.name.lower().startswith(('hi@', 'ma@'))]
        if not bases:
            bases = entries
        by_name = {x.name.lower(): x for x in entries}
        parts = []
        for base in bases:
            data = child.read(base)
            size = rpf.size_from_flags(base.system_flags)
            parsed = parse_ybn(data, size, base.name)
            hi_entry = by_name.get('hi@' + base.name.lower())
            if hi_entry is not None:
                hi_data = child.read(hi_entry)
                hi = parse_ybn(hi_data, rpf.size_from_flags(hi_entry.system_flags),
                               hi_entry.name)
                parsed = gtav.merge_hi(parsed, hi)
            parts.append(parsed)
        return gtav.combine(parts) if len(parts) > 1 else parts[0], len(parts)
    finally:
        child.close()


def staging_name(outer_name: str, inner: str) -> str:
    rel = inner[len('levels/gta5/'):] if inner.startswith('levels/gta5/') else inner
    return outer_name.split('.')[0] + '_' + rel.replace('/', '_').replace('.rpf', '')


def export_one(gta_dir: Path, cipher, outer_name: str, inner: str, stage_root: Path,
               tile_size: int, scale: float, spawn):
    name = staging_name(outer_name, inner)
    target = stage_root / name
    if (target / 'index.json').is_file():
        return name, 'cached'
    outer = rpf.RpfArchive(gta_dir / outer_name, cipher)
    try:
        world, nparts = block_world(outer, inner)
    finally:
        outer.close()
    if world.triangle_count == 0:
        return name, 'empty'
    os.makedirs(target, exist_ok=True)
    report = gtav.export(world, str(stage_root), name, scale=scale,
                       tile_size=tile_size, texture_prefix='losantos',
                       double_sided=True, log=lambda *_: None)
    idx_path = target / 'index.json'
    idx = json.load(open(idx_path))
    # merged index uses the city spawn
    idx['spawn'] = list(spawn)
    idx['spawn_angles'] = [0.0, 0.0, 0.0]
    json.dump(idx, open(idx_path, 'w'))
    indexdat.write_index_dat(idx, str(target))
    return name, (f'{report.tiles} tiles, {world.triangle_count} tris '
                  f'({nparts} ybn) BUDGET_DROPPED={report.dropped_tile_tris}')


def _read_tile(path):
    data = open(path, 'rb').read()
    magic, ver, flags, vc, ic, sc, mc, stride, res = struct.unpack_from(
        '<IHHIIIIII', data, 0)
    if magic != TILE_MAGIC:
        raise ValueError(f'bad tile magic {magic:#x} in {path}')
    subs = [struct.unpack_from('<IIII', data, 32 + 16 * i) for i in range(sc)]
    names = [data[32 + 16 * sc + 64 * i:32 + 16 * sc + 64 * (i + 1)]
             .split(b'\0')[0].decode() for i in range(mc)]
    off = 32 + 16 * sc + 64 * mc
    verts = data[off:off + vc * 28]
    idx = np.frombuffer(data[off + vc * 28:off + vc * 28 + ic * 2],
                        dtype='<u2').copy()
    return verts, idx, subs, names


def _write_tile(path, verts, idx, subs, names):
    header = struct.pack('<IHHIIIII', TILE_MAGIC, 1, 3,
                         len(verts) // 28, len(idx), len(subs), len(names), 28)
    with open(path, 'wb') as f:
        f.write(header)
        f.write(struct.pack('<I', 0))
        for sub in subs:
            f.write(struct.pack('<IIII', *sub))
        for name in names:
            raw = name.encode('ascii')
            f.write(raw + b'\0' * (64 - len(raw)))
        f.write(verts)
        f.write(idx.astype('<u2').tobytes())


def merge_tile(paths, out_path, max_verts=65535, log=print):
    names, name_index = [], {}

    def name_id(name):
        if name not in name_index:
            name_index[name] = len(names)
            names.append(name)
        return name_index[name]

    # gather runs per material first
    order = []
    pending = {}
    out_name = Path(out_path).name
    for path in paths:
        verts, idx, subs, part_names = _read_tile(path)
        idx = np.asarray(idx, np.int64)
        vrows = (np.frombuffer(verts, np.uint8).reshape(-1, 28)
                 if len(verts) else np.zeros((0, 28), np.uint8))
        for mat, off, count, voff in subs:
            count = int(count)
            if count <= 0 or int(off) + count > len(idx):
                continue
            name = part_names[mat] if mat < len(part_names) else 'error'
            run = idx[int(off):int(off) + count] + int(voff)
            if int(run.min()) < 0 or int(run.max()) >= len(vrows):
                raise ValueError(
                    f'{path}: submesh {name} references vertex '
                    f'{int(run.max())} of {len(vrows)}')
            used, local = np.unique(run, return_inverse=True)
            local = np.asarray(local, np.int64).reshape(-1)
            pending.setdefault(name, []).append((vrows[used], local))
            if name not in order:
                order.append(name)

    sub_parts, idx_parts, vert_parts = [], [], []
    index_base = 0
    vertex_base = 0
    dropped_runs = 0
    dropped_tris = 0
    for name in order:
        block_start = vertex_base
        block_verts = 0
        for rows, local in pending[name]:
            need = len(rows)
            if need > max_verts:
                dropped_runs += 1
                dropped_tris += len(local) // 3
                log(f'  ! material vertex budget at {out_name}: dropping '
                    f'{len(local) // 3} tris of {name} ({need} verts)')
                continue
            # window full, next vertex block for this material
            if block_verts + need > max_verts:
                vertex_base += block_verts
                block_start = vertex_base
                block_verts = 0
            sub_parts.append((name_id(name), index_base, len(local),
                              block_start))
            idx_parts.append(local + block_verts)
            vert_parts.append(rows)
            block_verts += need
            index_base += len(local)
        vertex_base += block_verts

    if not vert_parts:
        return 0
    verts = np.concatenate(vert_parts, axis=0).tobytes()
    idx = np.concatenate(idx_parts)
    _write_tile(out_path, verts, idx, sub_parts, names)
    return len(verts) // 28


def _read_chunk(path):
    data = open(path, 'rb').read()
    magic, ver, flags, tri_count, vc, res = struct.unpack_from('<IHHIII', data, 0)
    if magic != CHUNK_MAGIC:
        raise ValueError(f'bad chunk magic {magic:#x} in {path}')
    off = 20
    verts = np.frombuffer(data[off:off + vc * 12], dtype='<f4').reshape(vc, 3)
    idx = np.frombuffer(data[off + vc * 12:off + vc * 12 + tri_count * 6],
                        dtype='<u2').copy()
    return verts, idx


def merge_chunk(paths, out_path, max_verts=65535, log=print):
    verts_parts, idx_parts = [], []
    base = 0
    tri_total = 0
    out_name = Path(out_path).name
    for path in paths:
        verts, idx = _read_chunk(path)
        if base + len(verts) > max_verts:
            log(f'  ! chunk vertex budget at {out_name}: dropping {Path(path).name}')
            continue
        verts_parts.append(verts)
        idx_parts.append(idx + base)
        base += len(verts)
        tri_total += len(idx) // 3
    if not verts_parts:
        return 0
    verts = np.concatenate(verts_parts)
    idx = np.concatenate(idx_parts)
    with open(out_path, 'wb') as f:
        f.write(struct.pack('<IHHIII', CHUNK_MAGIC, 1, 0, tri_total, len(verts), 0))
        f.write(verts.astype('<f4').tobytes())
        f.write(idx.astype('<u2').tobytes())
    return tri_total


def _merge_with_tool(stage_root, target, tile_groups, chunk_groups, log=print):
    exe = Path(__file__).resolve().parents[2] / 'native' / 'build' / 'gtav_mergetest.exe'
    if not exe.is_file():
        return None

    manifest = target / 'gtav_mergetest.manifest'
    report = target / 'gtav_mergetest.report'

    # tabs, paths have spaces
    with open(manifest, 'w', encoding='utf-8', newline='\n') as f:
        for key, paths in sorted(tile_groups.items()):
            tx, ty = key.split()
            out = target / 'tiles' / f't_{tx}_{ty}_0.bin'
            f.write('T\t' + str(out) + '\t'
                    + '\t'.join(str(p) for p in paths) + '\n')
        for key, paths in sorted(chunk_groups.items()):
            cx, cy = key.split()
            out = target / 'phys' / f'g_{cx}_{cy}.bin'
            f.write('C\t' + str(out) + '\t'
                    + '\t'.join(str(p) for p in paths) + '\n')

    t0 = time.time()
    res = subprocess.run([str(exe), str(manifest), str(report)],
                         capture_output=True, text=True)
    for line in (res.stderr or '').splitlines():
        log('  ' + line)
    if not report.is_file():
        raise RuntimeError(f'gtav_mergetest produced no report (exit {res.returncode})')

    tiles, chunks, tri_count = {}, {}, 0
    dropped = 0
    with open(report, encoding='utf-8') as f:
        for line in f:
            parts = line.rstrip('\n').split('\t')
            if len(parts) != 3:
                continue
            out_path, count, drop = parts[0], int(parts[1]), int(parts[2])
            dropped += drop
            if count <= 0:
                continue
            out = Path(out_path)
            if out.parent.name == 'tiles':
                key = f'{out.stem.split("_")[1]} {out.stem.split("_")[2]}'
                tiles[key] = {f'lod{lod}': {'file': out.name}
                              for lod in range(5)}
            else:
                key = f'{out.stem.split("_")[1]} {out.stem.split("_")[2]}'
                chunks[key] = {'file': out.name}
                tri_count += count

    log(f'  merged via gtav_mergetest.exe in {time.time() - t0:.1f}s: '
        f'{len(tiles)} tiles, {len(chunks)} chunks, '
        f'{dropped} runs over the material vertex budget')
    if res.returncode != 0:
        raise RuntimeError(f'gtav_mergetest exited {res.returncode}')
    return tiles, chunks, tri_count


def merge_staging(stage_root: Path, out_root: Path, name: str, spawn,
                  tile_size: int, log=print, include=None):
    tile_groups, chunk_groups = {}, {}
    materials = {}
    blocks = 0
    for idx_path in sorted(stage_root.glob('*/index.json')):
        if include and not include(idx_path.parent.name):
            continue
        idx = json.load(open(idx_path))
        base = idx_path.parent
        blocks += 1
        for mat in idx.get('materials', {}).values():
            if mat not in materials:
                materials[mat] = True
        for key, info in idx.get('tiles', {}).items():
            tile_groups.setdefault(key, []).append(
                base / 'tiles' / info['lod0']['file'])
        for key, info in idx.get('phys_chunks', {}).items():
            chunk_groups.setdefault(key, []).append(
                base / 'phys' / info['file'])
    target = out_root / name
    tiles_dir = target / 'tiles'
    phys_dir = target / 'phys'
    os.makedirs(tiles_dir, exist_ok=True)
    os.makedirs(phys_dir, exist_ok=True)

    merged = _merge_with_tool(stage_root, target, tile_groups, chunk_groups,
                              log=log)
    if merged is not None:
        tiles, chunks, chunk_count = merged
        tri_count = 0
    else:
        log('  gtav_mergetest.exe not built - merging in Python (slow)')
        tiles = {}
        chunk_count = 0
        tri_count = 0
        for n, (key, paths) in enumerate(sorted(tile_groups.items())):
            tx, ty = key.split()
            out_name = f't_{tx}_{ty}_0.bin'
            verts = merge_tile(paths, tiles_dir / out_name, log=log)
            if verts:
                tiles[key] = {f'lod{lod}': {'file': out_name} for lod in range(5)}
                tri_count += 0  # does nothing, whatever
            if n % 500 == 0:
                log(f'  tiles {n}/{len(tile_groups)}')
        chunks = {}
        for n, (key, paths) in enumerate(sorted(chunk_groups.items())):
            cx, cy = key.split()
            out_name = f'g_{cx}_{cy}.bin'
            tris = merge_chunk(paths, phys_dir / out_name, log=log)
            if tris:
                chunks[key] = {'file': out_name}
                chunk_count += tris
            if n % 2000 == 0:
                log(f'  chunks {n}/{len(chunk_groups)}')
    index = {
        'map': name,
        'tile_size': tile_size,
        'cell_size': gtav.CELL_SIZE,
        'phys_chunk_size': gtav.PHYS_CHUNK_SIZE,
        # tuned in game
        'stream_radius': 90000,
        'max_tiles': 24000,
        'merge_grid': 8,
        'fog': {'enabled': True, 'color': [170, 181, 194],
                'near': 30000, 'far': 88000},
        'spawn': [float(x) for x in spawn],
        'spawn_angles': [0.0, 0.0, 0.0],
        'materials': {str(i): m for i, m in enumerate(sorted(materials))},
        'tiles': tiles,
        'phys_chunks': chunks,
        'source': {'blocks': blocks, 'tiles': len(tiles),
                   'chunks': len(chunks), 'chunk_triangles': chunk_count},
    }
    with open(target / 'index.json', 'w') as f:
        json.dump(index, f, separators=(',', ':'))
    indexdat.write_index_dat(index, str(target))
    log(f'merged {blocks} staging blocks -> {len(tiles)} tiles, '
        f'{len(chunks)} chunks, {chunk_count} collision tris')
    flags_sidecar.write_sidecar(target, log=log)


def main(argv=None):
    ap = argparse.ArgumentParser(description='Export whole Los Santos blocks')
    ap.add_argument('--gta', default=r'D:\Games\Grand Theft Auto V Legacy')
    ap.add_argument('--staging', default=r'C:\Users\Frame\AppData\Local\Temp\opencode\gtav\city_stage')
    ap.add_argument('--out', required=True, help='GMod data root')
    ap.add_argument('--name', default='losantos')
    ap.add_argument('--tile-size', type=int, default=1024)
    ap.add_argument('--spawn', default='-516.1,-372.0,37.5',
                    help='GTA metres x,y,z')
    ap.add_argument('--districts', action='append', default=[],
                    help='only blocks whose rel path starts with this prefix')
    ap.add_argument('--skip', action='append', default=[],
                    help='skip this rpf name (e.g. bh1_17.rpf for a detailed export)')
    ap.add_argument('--merge-only', action='store_true')
    ap.add_argument('--limit', type=int, default=0)
    args = ap.parse_args(argv)
    gta = Path(args.gta)
    cipher = crypto.NgCipher.load(Path(__file__).with_name('keys'))
    spawn = np.array([float(v) for v in args.spawn.split(',')]) * gtav.SCALE
    stage = Path(args.staging)
    stage.mkdir(parents=True, exist_ok=True)
    if not args.merge_only:
        blocks, roads = discover(gta, cipher)
        entries = blocks + roads
        if args.districts:
            entries = [e for e in entries
                       if any(e[1].startswith('levels/gta5/' + d)
                              for d in args.districts)]
        entries = [e for e in entries if e[2] not in set(args.skip)]
        if args.limit:
            entries = entries[:args.limit]
        print(f'exporting {len(entries)} block/road rpfs to {stage}', flush=True)
        for n, (outer_name, inner, rpf_name) in enumerate(entries):
            try:
                name, status = export_one(gta, cipher, outer_name, inner, stage,
                                          args.tile_size, gtav.SCALE, spawn)
            except Exception as exc:
                print(f'  [{n+1}/{len(entries)}] {rpf_name}: ERROR {exc}',
                      flush=True)
                continue
            print(f'  [{n+1}/{len(entries)}] {rpf_name}: {status}', flush=True)
    merge_staging(stage, Path(args.out), args.name, spawn, args.tile_size)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())