from __future__ import annotations

import argparse
import json
import os
from pathlib import Path

import numpy as np

from . import crypto, materials, rpf, ymap as ymapmod
from . import indexdat
from . import world as gtav
from .ydr import parse_ydr
from .ytd import parse_ytd

CITY_ARCHIVES = ('x64i.rpf', 'x64j.rpf', 'x64k.rpf', 'x64l.rpf',
                 'x64m.rpf', 'x64n.rpf', 'x64o.rpf', 'x64p.rpf')
DISTRICT_PREFIXES = ('_citye', '_cityw', '_hills')
TILE_SIZE = 1024
FALLBACK = 'models/gtav/dt1_02/im_dt1_02_wall_01'


def discover_lods(gta: Path, cipher):
    found = []
    for outer_name in CITY_ARCHIVES:
        outer = rpf.RpfArchive(gta / outer_name, cipher)
        try:
            dirs = {}
            for entry in outer.walk():
                if 'levels/gta5/' not in entry.path or not entry.name.lower().endswith('.rpf'):
                    continue
                rel = entry.path.split('levels/gta5/', 1)[1]
                parts = rel.split('/')
                if len(parts) < 2 or not parts[0].startswith(DISTRICT_PREFIXES):
                    continue
                if 'lod' not in entry.name.lower():
                    continue
                # this does nothing
                if 'metadata' in entry.name.lower() or entry.name.lower().endswith('_lod.rpf') is False:
                    pass
                base = '/'.join(parts[:-1])
                dirs.setdefault(base, []).append(('levels/gta5/' + rel, entry.name))
            for base, items in dirs.items():
                meta = [p for p, n in items if 'metadata' in n.lower()]
                for lod_path, lod_name in items:
                    if 'metadata' in lod_name.lower():
                        continue
                    if not meta:
                        candidates = [e for e in outer.walk()
                                      if 'levels/gta5/' + base + '/' in e.path
                                      and e.name.lower().endswith('.rpf')
                                      and 'metadata' in e.name.lower()]
                        if candidates:
                            meta = ['levels/gta5/' + base + '/' + candidates[0].name]
                    if meta:
                        found.append((outer_name, lod_path, meta[0]))
        finally:
            outer.close()
    return found


def load_metadata(outer, meta_inner, log=print):
    meta = outer.open_child(outer.get(meta_inner))
    try:
        archetypes = []
        for entry in meta.walk():
            if entry.name.lower().endswith('.ytyp'):
                try:
                    archetypes += ymapmod.parse_ytyp(
                        meta.read(entry),
                        rpf.size_from_flags(entry.system_flags), entry.name)
                except Exception:
                    pass
        entities = []
        for entry in meta.walk():
            if entry.name.lower().endswith('.ymap'):
                try:
                    entities += ymapmod.parse_ymap(
                        meta.read(entry),
                        rpf.size_from_flags(entry.system_flags), entry.name)
                except Exception:
                    pass
        by_arch = {}
        for e in entities:
            by_arch.setdefault(e.archetype, []).append(e)
        return archetypes, by_arch
    finally:
        meta.close()


def build_slod(gta, cipher, out_root, map_name, material_root, log=print):
    lods = discover_lods(gta, cipher)
    log(f'SLOD packs: {len(lods)}')
    drawables = []
    texture_items = []
    seen = set()
    for outer_name, lod_inner, meta_inner in lods:
        outer = rpf.RpfArchive(gta / outer_name, cipher)
        try:
            archetypes, by_arch = load_metadata(outer, meta_inner, log)
            lod = outer.open_child(outer.get(lod_inner))
            try:
                ydrs = [e for e in lod.walk() if e.name.lower().endswith('.ydr')]
                # slod textures live in sibling ytds
                for entry in [e for e in lod.walk() if e.name.lower().endswith('.ytd')]:
                    try:
                        ytd_textures = parse_ytd(lod.read(entry),
                                                 rpf.size_from_flags(entry.system_flags),
                                                 entry.name)
                    except Exception as exc:
                        log(f'  ! {lod_inner}/{entry.name}: {exc}')
                        continue
                    for tex in ytd_textures:
                        texture_items.append((entry.name, tex))
                    log(f'  {lod_inner}/{entry.name}: {len(ytd_textures)} textures')
                for entry in ydrs:
                    low = entry.name.lower()
                    # night shells, no night layer yet
                    if 'emissive' in low or '_emi_' in low or 'proxy' in low:
                        log(f'  - {lod_inner}/{entry.name}: emissive/proxy shell skipped')
                        continue
                    drawable = parse_ydr(lod.read(entry),
                                         rpf.size_from_flags(entry.system_flags),
                                         entry.name)
                    arch = ymapmod.match_archetype(drawable, archetypes,
                                                   tolerance=300.0)
                    if arch is None:
                        log(f'  ! {lod_inner}/{entry.name}: no archetype')
                        continue
                    placements = by_arch.get(arch.name, [])
                    if not placements:
                        log(f'  ! {lod_inner}/{entry.name}: no entity')
                        continue
                    for entity in placements:
                        key = (entry.name, tuple(np.round(entity.position, 1)))
                        if key in seen:
                            continue
                        seen.add(key)
                        placed = ymapmod.transform_drawable(drawable, entity)
                        drawables.append(placed)
                        for tex in drawable.textures:
                            texture_items.append((entry.name, tex))
                        log(f'  {lod_inner}/{entry.name}: {drawable.triangle_count} tris '
                            f'at {tuple(np.round(entity.position, 1))}')
            finally:
                lod.close()
        finally:
            outer.close()
    if not drawables:
        log('no SLOD drawables placed')
        return
    installed, skipped = materials.install_textures(
        texture_items, material_root, 'models/gtav/slod', log=log)
    for name, why in skipped:
        log(f'  ! skipped {name}: {why}')
    texture_materials = {name.lower(): path for name, path in installed.items()}
    triangles, uvs, normals, dense, names = [], [], [], [], []
    name_index = {}
    fallback_geoms = 0
    unmatched = {}
    for drawable in drawables:
        for geom in drawable.geometries:
            key = (geom.texture or '').lower()
            mat = texture_materials.get(key)
            if mat is False:
                continue
            if not mat:
                # borrow from another region
                borrowed = materials.find_installed(material_root, key)
                if borrowed:
                    texture_materials[key] = borrowed
                    mat = borrowed
            if not mat:
                fallback_geoms += 1
                unmatched[key] = unmatched.get(key, 0) + 1
                mat = FALLBACK
            if mat not in name_index:
                name_index[mat] = len(names)
                names.append(mat)
            idx = geom.indices
            tri = geom.positions[idx] * gtav.SCALE
            if geom.normals is not None:
                nrm = geom.normals[idx]
            else:
                nrm = np.repeat(gtav.triangle_normals(tri)[0][:, None, :], 3, axis=1)
            triangles.append(tri)
            uvs.append(geom.uvs[idx].astype(np.float64))
            normals.append(nrm)
            dense.append(np.full(len(idx), name_index[mat], np.int64))
    tri = np.concatenate(triangles, axis=0)
    uv = np.concatenate(uvs, axis=0)
    nrm = np.concatenate(normals, axis=0)
    mat = np.concatenate(dense, axis=0)
    log(f'SLOD visual: {len(tri)} triangles, {len(names)} materials')
    if fallback_geoms:
        top = sorted(unmatched.items(), key=lambda kv: -kv[1])[:15]
        log(f'  ! {fallback_geoms} geometries fell back to {FALLBACK}:')
        for name, n in top:
            log(f'      {n:5d}x {name}')

    target = out_root
    tiles_dir = target / 'tiles'
    os.makedirs(tiles_dir, exist_ok=True)
    index_path = target / 'index.json'
    index = json.load(open(index_path))
    tile_size = index.get('tile_size', TILE_SIZE)
    count = 0
    for tx, ty, idx in gtav.grid_groups(tri, tile_size):
        out_name = f's_{tx}_{ty}.bin'
        info = gtav.write_tile(tiles_dir / out_name, tri[idx], nrm[idx],
                             mat[idx], names, double_sided=False,
                             uvs=uv[idx])
        if not info:
            continue
        key = f'{tx} {ty}'
        slot = index['tiles'].setdefault(key, {})
        for lod in (2, 3, 4):
            slot[f'lod{lod}'] = {'file': out_name}
        # no tile here, slod covers near too
        if 'lod0' not in slot:
            for lod in (0, 1):
                slot[f'lod{lod}'] = {'file': out_name}
        count += 1
    mats = index.setdefault('materials', {})
    for m in names:
        if m not in mats.values():
            mats[str(len(mats))] = m
    index['stream_radius'] = index.get('stream_radius', 24000)
    json.dump(index, open(index_path, 'w'), separators=(',', ':'))
    indexdat.write_index_dat(index, str(target))
    log(f'SLOD tiles written: {count}; updated {index_path}')


def main(argv=None):
    ap = argparse.ArgumentParser(description='Wire district SLODs into LOD2-4')
    ap.add_argument('--gta', default=r'D:\Games\Grand Theft Auto V Legacy')
    ap.add_argument('--root', required=True,
                    help='data/lossantos/<map>')
    ap.add_argument('--map-name', default='lossantos')
    ap.add_argument('--materials', required=True,
                    help='addon materials folder')
    args = ap.parse_args(argv)
    cipher = crypto.NgCipher.load(Path(__file__).with_name('keys'))
    build_slod(Path(args.gta), cipher, Path(args.root), args.map_name,
               args.materials)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())