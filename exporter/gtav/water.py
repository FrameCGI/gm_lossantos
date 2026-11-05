from __future__ import annotations

import argparse
import os
import json
import re
import struct
import sys
import tempfile
import time
from concurrent.futures import ProcessPoolExecutor
from pathlib import Path

import numpy as np

from . import crypto, indexdat, rpf, world
from .materials import vmt_text
from .ytd import FMT_A8R8G8B8, GtaTexture, write_vtf

WATER_MATERIAL = 'models/gtav/water/ocean'
UV_TILE = 2048.0  # source units per texture repeat (52 m)
VTF_SIZE = 256

OCEAN_RGB = (115, 140, 160)  # slate blue
OCEAN_ALPHA = 217

TILE_MAGIC = 0x31545647  # "GVT1"
TILE_STRIDE = 28
LODS = ('lod0', 'lod1', 'lod2', 'lod3', 'lod4')


def parse_water_quads(text):
    section = re.search(r'<WaterQuads>(.*?)</WaterQuads>', text, re.S)
    if not section:
        return []

    def field(block, tag, default=None):
        m = re.search(tag + r'\s+value="([^"]+)"', block)
        return m.group(1) if m else default

    quads = []
    for block in re.findall(r'<Item>(.*?)</Item>', section.group(1), re.S):
        if field(block, 'IsInvisible', 'false') == 'true':
            continue
        try:
            x0 = float(field(block, 'minX'))
            x1 = float(field(block, 'maxX'))
            y0 = float(field(block, 'minY'))
            y1 = float(field(block, 'maxY'))
            z = float(field(block, 'z', '0'))
        except (TypeError, ValueError):
            continue
        if x1 > x0 and y1 > y0:
            quads.append((x0, x1, y0, y1, z))
    return quads


def load_quads(gta, cipher, log=print):
    outer = rpf.RpfArchive(gta / 'common.rpf', cipher)
    try:
        wanted = None
        for entry in outer.walk():
            if entry.name.lower() != 'water.xml':
                continue
            path = (getattr(entry, 'path', None) or entry.name).lower()
            if wanted is None or 'levels' in path:
                wanted = entry
                if 'levels' in path:
                    break
        if wanted is None:
            log('  ! water.xml not found in common.rpf')
            return []
        text = outer.read(wanted).decode('utf-8', 'replace')
    finally:
        outer.close()
    quads = parse_water_quads(text)
    log(f'  water.xml: {len(quads)} water quads')
    return [(x0 * world.SCALE, x1 * world.SCALE,
             y0 * world.SCALE, y1 * world.SCALE, z * world.SCALE)
            for x0, x1, y0, y1, z in quads]


def collect_cell_rects(quads, tile_size, bounds=None):
    gx0, gx1, gy0, gy1 = bounds if bounds else (-10 ** 9, 10 ** 9, -10 ** 9, 10 ** 9)
    cells = {}
    for x0, x1, y0, y1, z in quads:
        cx0 = int(np.floor(x0 / tile_size))
        cx1 = int(np.floor((x1 - 1e-6) / tile_size))
        cy0 = int(np.floor(y0 / tile_size))
        cy1 = int(np.floor((y1 - 1e-6) / tile_size))
        for gx in range(max(cx0, gx0), min(cx1, gx1) + 1):
            wx0 = gx * tile_size
            rx0, rx1 = max(x0, wx0), min(x1, wx0 + tile_size)
            for gy in range(max(cy0, gy0), min(cy1, gy1) + 1):
                wy0 = gy * tile_size
                ry0, ry1 = max(y0, wy0), min(y1, wy0 + tile_size)
                cells.setdefault((gx, gy), []).append(
                    (round(rx0, 3), round(rx1, 3),
                     round(ry0, 3), round(ry1, 3), round(z, 3)))
    for key, rects in cells.items():
        seen = set()
        unique = []
        for r in rects:
            if r not in seen:
                seen.add(r)
                unique.append(r)
        cells[key] = resolve_overlaps(unique)
    return cells


def overlap_count(rects):
    bad = 0
    for i, a in enumerate(rects):
        for b in rects[i + 1:]:
            if a[4] != b[4]:
                continue
            if a[0] < b[1] - 1e-3 and b[0] < a[1] - 1e-3 \
                    and a[2] < b[3] - 1e-3 and b[2] < a[3] - 1e-3:
                bad += 1
    return bad


def rect_subtract(b, a):
    if a[0] <= b[0] and b[1] <= a[1] and a[2] <= b[2] and b[3] <= a[3]:
        return []
    out = []
    if b[2] < a[2]:
        out.append((b[0], b[1], b[2], min(b[3], a[2])))
    if a[3] < b[3]:
        out.append((b[0], b[1], max(b[2], a[3]), b[3]))
    if b[0] < a[0]:
        out.append((b[0], min(b[1], a[0]), max(b[2], a[2]), min(b[3], a[3])))
    if a[1] < b[1]:
        out.append((max(b[0], a[1]), b[1], max(b[2], a[2]), min(b[3], a[3])))
    return [r + (b[4],) for r in out
            if r[1] > r[0] + 1e-6 and r[3] > r[2] + 1e-6]


def resolve_overlaps(rects):
    kept = []  # same-z candidates only interact within their z
    out = []
    for r in rects:
        pieces = [r]
        for k in kept:
            if k[4] != r[4]:
                continue
            pieces = [p for piece in pieces
                      for p in rect_subtract(piece, k)] or []
            if not pieces:
                break
        out.extend(pieces)
        kept.append(r)
    return out


def water_soup(rects):
    n = len(rects)
    tri = np.zeros((n * 2, 3, 3), np.float64)
    nrm = np.zeros((n * 2, 3, 3), np.float64)
    uv = np.zeros((n * 2, 3, 2), np.float64)
    for i, (x0, x1, y0, y1, z) in enumerate(rects):
        corners = ((x0, y0, z), (x1, y0, z), (x1, y1, z), (x0, y1, z))
        for j, tri_idx in enumerate(((0, 1, 2), (0, 2, 3))):
            row = tri[i * 2 + j]
            for k, ci in enumerate(tri_idx):
                x, y, zz = corners[ci]
                row[k] = (x, y, zz)
                nrm[i * 2 + j, k] = (0.0, 0.0, 1.0)
                uv[i * 2 + j, k] = (x / UV_TILE, y / UV_TILE)
    return tri, nrm, uv


def write_water_tile(path, rects):
    if not rects:
        return False
    tri, nrm, uv = water_soup(rects)
    dense = np.zeros(len(tri), np.int64)
    info = world.write_tile(path, tri, nrm, dense, [WATER_MATERIAL],
                            double_sided=False, uvs=uv)
    return bool(info)


def read_tile(path):
    try:
        data = Path(path).read_bytes()
    except OSError:
        return None
    if len(data) < 32:
        return None
    # same GVT1 header as world.write_tile
    magic, version, flags, vcount, icount, scount, mcount, stride = \
        struct.unpack_from('<IHHIIIII', data, 0)
    if magic != TILE_MAGIC or version != 1 or stride != TILE_STRIDE:
        return None
    pos = 32
    if len(data) < pos + scount * 16 + mcount * 64 \
            + vcount * TILE_STRIDE + icount * 2:
        return None
    subs = []
    for _ in range(scount):
        m, io, ic, vo = struct.unpack_from('<IIII', data, pos)
        pos += 16
        subs.append([m, io, ic, vo])
    mats = []
    for _ in range(mcount):
        raw = data[pos:pos + 64]
        pos += 64
        mats.append(raw.split(b'\0', 1)[0].decode('ascii', 'replace'))
    vbytes = data[pos:pos + vcount * TILE_STRIDE]
    pos += vcount * TILE_STRIDE
    ibytes = data[pos:pos + icount * 2]
    return {'flags': flags, 'subs': subs, 'mats': mats,
            'vbytes': vbytes, 'ibytes': ibytes}


def write_tile_raw(path, tile):
    subs, mats = tile['subs'], tile['mats']
    vbytes, ibytes = tile['vbytes'], tile['ibytes']
    vcount = len(vbytes) // TILE_STRIDE
    icount = len(ibytes) // 2
    out = bytearray()
    out += struct.pack('<IHHIIIII', TILE_MAGIC, 1, tile['flags'],
                       vcount, icount, len(subs), len(mats), TILE_STRIDE)
    out += struct.pack('<I', 0)
    for m, io, ic, vo in subs:
        out += struct.pack('<IIII', m, io, ic, vo)
    for name in mats:
        raw = name.encode('ascii')
        if len(raw) > 63:
            raise ValueError(f'material name does not fit GVT1: {name}')
        out += raw + b'\0' * (64 - len(raw))
    out += vbytes
    out += ibytes
    tmp = Path(str(path) + '.tmp')
    tmp.write_bytes(out)
    tmp.replace(path)


def strip_submeshes(tile, mat_name):
    subs = tile['subs']
    drop = {i for i, s in enumerate(subs) if tile['mats'][s[0]] == mat_name}
    if not drop:
        return True

    vtotal = len(tile['vbytes']) // TILE_STRIDE
    itotal = len(tile['ibytes']) // 2

    def block_sizes(index_slot, total):
        order = sorted(range(len(subs)), key=lambda i: subs[i][index_slot])
        sizes = {}
        for k, i in enumerate(order):
            nxt = (subs[order[k + 1]][index_slot] if k + 1 < len(order)
                   else total)
            sizes[i] = nxt - subs[i][index_slot]
            if sizes[i] < 0:
                return None
        return sizes

    vsize = block_sizes(3, vtotal)
    isize = block_sizes(1, itotal)
    if vsize is None or isize is None:
        return False

    vbytes = tile['vbytes']
    ibytes = tile['ibytes']
    # back to front so offsets stay valid
    for i in sorted(drop, key=lambda i: subs[i][3], reverse=True):
        vo, vo_end = subs[i][3] * TILE_STRIDE, (subs[i][3] + vsize[i]) * TILE_STRIDE
        vbytes = vbytes[:vo] + vbytes[vo_end:]
    for i in sorted(drop, key=lambda i: subs[i][1], reverse=True):
        io, io_end = subs[i][1] * 2, (subs[i][1] + isize[i]) * 2
        ibytes = ibytes[:io] + ibytes[io_end:]

    keep = []
    for i, s in enumerate(subs):
        if i in drop:
            continue
        vshift = sum(vsize[j] for j in drop if subs[j][3] < s[3])
        ishift = sum(isize[j] for j in drop if subs[j][1] < s[1])
        keep.append([s[0], s[1] - ishift, s[2], s[3] - vshift])
    tile['subs'] = keep
    tile['vbytes'] = vbytes
    tile['ibytes'] = ibytes
    return True


def append_submeshes(dst_path, src_path, mat_name=WATER_MATERIAL):
    dst = read_tile(dst_path)
    src = read_tile(src_path)
    if dst is None or src is None or not src['subs']:
        return 'skip'
    # strip old water first
    if not strip_submeshes(dst, mat_name):
        return 'skip'

    if mat_name in dst['mats']:
        mi = dst['mats'].index(mat_name)
    else:
        mi = len(dst['mats'])
        dst['mats'].append(mat_name)

    vbase = len(dst['vbytes']) // TILE_STRIDE
    ibase = len(dst['ibytes']) // 2
    for m, io, ic, vo in src['subs']:
        dst['subs'].append([mi, ibase + io, ic, vbase + vo])
    dst['vbytes'] += src['vbytes']
    dst['ibytes'] += src['ibytes']
    write_tile_raw(dst_path, dst)
    return 'appended'


def ocean_rgba(size=VTF_SIZE):
    yy, xx = np.mgrid[0:size, 0:size].astype(np.float64) / size
    # int frequencies so it tiles
    swell = (np.sin(2 * np.pi * (xx * 2 + yy) + 0.7)
             + 0.7 * np.sin(2 * np.pi * (xx - yy * 3) + 2.1)
             + 0.5 * np.sin(2 * np.pi * (xx * 4 + yy * 2) + 4.0)
             + 0.35 * np.sin(2 * np.pi * (xx * 7 - yy * 5) + 1.2)) / 2.55
    ripple = (np.sin(2 * np.pi * (xx * 13 + yy * 9) + 5.5)
              + np.sin(2 * np.pi * (xx * 17 - yy * 11) + 3.3)) / 2.0
    value = 13.0 * swell + 4.0 * ripple
    rgba = np.zeros((size, size, 4), np.uint8)
    for c, base in enumerate(OCEAN_RGB):
        rgba[:, :, c] = np.clip(base + value, 0, 255).astype(np.uint8)
    rgba[:, :, 3] = OCEAN_ALPHA
    return rgba


def install_water_material(material_root, log=print):
    target = Path(material_root) / 'models' / 'gtav' / 'water'
    target.mkdir(parents=True, exist_ok=True)
    vmt = target / 'ocean.vmt'
    vmt.write_text(vmt_text(WATER_MATERIAL, surfaceprop='water',
                            translucent=True, opacity=0.95),
                   encoding='ascii', newline='\n')
    tex = GtaTexture(name='ocean', width=VTF_SIZE, height=VTF_SIZE, depth=1,
                     stride=VTF_SIZE * 4, format=FMT_A8R8G8B8, levels=1,
                     data=b'')
    ok = write_vtf(target / 'ocean.vtf', tex, rgba_map=ocean_rgba())
    log(f'  water material: {vmt} (vtf {"ok" if ok else "FAILED"})')
    return WATER_MATERIAL


def _water_chunk(args):
    tiles_dir, items = args
    tiles_dir = Path(tiles_dir)
    st = {'appended': 0, 'written': 0, 'skip': 0, 'files': 0}
    new = []
    tmp = Path(tempfile.mkdtemp(prefix='gtav_water_')) / 'cell.bin'
    try:
        for gx, gy, rects, files in items:
            if files:
                if not write_water_tile(tmp, rects):
                    continue
                for name in files:
                    path = tiles_dir / name
                    if not path.exists():
                        st['skip'] += 1
                        continue
                    result = append_submeshes(path, tmp)
                    st['files'] += 1
                    if result == 'appended':
                        st['appended'] += 1
                    elif result == 'skip':
                        st['skip'] += 1
            else:
                # water only cell
                name = f'w_{gx}_{gy}.bin'
                if write_water_tile(tiles_dir / name, rects):
                    new.append((f'{gx} {gy}', name))
                    st['written'] += 1
                else:
                    st['skip'] += 1
    finally:
        try:
            tmp.unlink()
            tmp.parent.rmdir()
        except OSError:
            pass
    return st, new


def _water_chunks(items, jobs):
    parent = list(range(len(items)))

    def find(a):
        while parent[a] != a:
            parent[a] = parent[parent[a]]
            a = parent[a]
        return a

    # cells sharing a file stay in one chunk
    owner = {}
    for i, (_gx, _gy, _rects, files) in enumerate(items):
        for f in files or ():
            j = owner.setdefault(f, i)
            if j != i:
                parent[find(i)] = find(j)
    groups = {}
    for i in range(len(items)):
        groups.setdefault(find(i), []).append(i)
    n = max(1, jobs * 8)
    chunks = [[] for _ in range(n)]
    for k, members in enumerate(sorted(groups.values(), key=lambda m: m[0])):
        chunks[k % n].extend(members)
    return [[items[i] for i in sorted(c)] for c in chunks if c]


def build_water(gta, root, material_root, extent='stream', only=None,
                dry_run=False, log=print, jobs=1):
    root = Path(root)
    tiles_dir = root / 'tiles'
    index_path = root / 'index.json'
    index = json.load(open(index_path, encoding='utf-8'))
    tile_size = index.get('tile_size', 1024)
    tiles = index['tiles']
    by_cell = {tuple(int(v) for v in key.split()): key for key in tiles}

    cipher = crypto.NgCipher.load(Path(__file__).parent / 'keys')
    quads = load_quads(gta, cipher, log)

    xs = [gx for gx, _ in by_cell]
    ys = [gy for _, gy in by_cell]
    if extent == 'quads':
        bounds = None
    else:
        # one stream radius past the edge
        margin = int(np.ceil(index.get('stream_radius', 24000) / tile_size))
        bounds = (min(xs) - margin, max(xs) + margin,
                  min(ys) - margin, max(ys) + margin)
    cells = collect_cell_rects(quads, tile_size, bounds)
    if only:
        want = {(int(a), int(b)) for a, b in (p.split(',') for p in only)}
        cells = {k: v for k, v in cells.items() if k in want}

    n_rects = sum(len(v) for v in cells.values())
    n_overlap = sum(overlap_count(v) for v in cells.values())
    with_tiles = sum(1 for k in cells if k in by_cell)
    log(f'water cells: {len(cells)} ({with_tiles} with tiles, '
        f'{len(cells) - with_tiles} water-only), {n_rects} rects, '
        f'extent={extent}')
    if n_overlap:
        log(f'  ! {n_overlap} coplanar overlapping rect pairs (double blend)')
    if dry_run:
        total_files = sum(len({tiles[by_cell[k]][l]['file'] for l in LODS
                               if l in tiles[by_cell[k]]})
                         for k in cells if k in by_cell)
        log(f'dry run: would touch {total_files} tile files and write '
            f'{len(cells) - with_tiles} w_* tiles')
        return {'cells': len(cells), 'files': total_files}

    if material_root:
        install_water_material(material_root, log)

    stats = {'appended': 0, 'written': 0, 'skip': 0, 'files': 0}
    t0 = time.time()
    items = []
    for (gx, gy), rects in sorted(cells.items()):
        slot = tiles.get(f'{gx} {gy}')
        files = None
        if slot:
            files = []
            for lod in LODS:
                f = slot.get(lod, {}).get('file')
                if f and f not in files:
                    files.append(f)
            files = files or None
        items.append((gx, gy, rects, files))

    chunks = _water_chunks(items, jobs)
    log(f'  {len(items)} cells in {len(chunks)} chunks, {jobs} worker(s)')
    if jobs <= 1:
        results = [_water_chunk((str(tiles_dir), c)) for c in chunks]
    else:
        with ProcessPoolExecutor(max_workers=jobs) as pool:
            results = []
            for n, r in enumerate(pool.map(_water_chunk, [(str(tiles_dir), c) for c in chunks]), 1):
                results.append(r)
                if n % max(1, len(chunks) // 10) == 0:
                    log(f'  {n}/{len(chunks)} chunks ({time.time() - t0:.0f}s)')
    for st, new in results:
        for k in stats:
            stats[k] += st[k]
        for key, name in new:
            tiles[key] = {lod: {'file': name} for lod in LODS}

    mats = index.setdefault('materials', {})
    if WATER_MATERIAL not in mats.values():
        mats[str(len(mats))] = WATER_MATERIAL
    if not dry_run:
        with open(index_path, 'w', encoding='utf-8') as f:
            json.dump(index, f, separators=(',', ':'))
        indexdat.write_index_dat(index, str(root))
    log(f'water: {stats["appended"]} tile files gained a water submesh, '
        f'{stats["written"]} w_* tiles written, {stats["skip"]} skipped, '
        f'{time.time() - t0:.0f}s')
    return stats


def main(argv=None):
    ap = argparse.ArgumentParser(
        description='Inject GTA water surfaces into an exported dataset')
    ap.add_argument('--gta', default=r'D:\Games\Grand Theft Auto V Legacy')
    ap.add_argument('--root', required=True,
                    help='data/lossantos/<map>')
    ap.add_argument('--materials', default=None,
                    help='addon materials root (writes the water VMT/VTF)')
    ap.add_argument('--extent', choices=('stream', 'quads'), default='stream',
                    help='clip water cells to the dataset grid plus one stream '
                         'radius (default) or cover every water quad cell')
    ap.add_argument('--only', default=None,
                    help='comma-separated gx,gy cells (debug)')
    ap.add_argument('--dry-run', action='store_true')
    ap.add_argument('--jobs', type=int, default=max(1, (os.cpu_count() or 4) - 2),
                    help='worker processes (1 = serial)')
    a = ap.parse_args(argv)

    only = a.only.split(';') if a.only else None
    build_water(Path(a.gta), Path(a.root), a.materials and Path(a.materials),
                extent=a.extent, only=only, dry_run=a.dry_run, jobs=a.jobs)
    return 0


if __name__ == '__main__':
    sys.exit(main())
