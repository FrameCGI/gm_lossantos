from __future__ import annotations

import json
import math
import os
import struct
import time
from dataclasses import dataclass, field

import numpy as np

from . import indexdat

SCALE = 39.3700787  # metres to source units
TILE_SIZE = 4096
PHYS_CHUNK_SIZE = 1024
CELL_SIZE = 8192
MAX_TILE_VERTS = 65535
MAX_CHUNK_VERTS = 65535

TILE_MAGIC = 0x31545647  # "GVT1"
CHUNK_MAGIC = 0x31435647  # "GVC1"

MEDIAN_TEXTURE = 'models/gtav/ch3_02/mw_median_conc_01'
DEFAULT_CATEGORY_TEXTURES = [
    ('road', 'im_road_012', 5.0),
    ('sidewalk', 'im_sidewalk010', 4.0),
    ('windows', 'im_dt1_02_windows_01', 6.0),
    ('grass', 'im_grass07b', 4.0),
    ('metal', 'im_dt1_02_wall_02', 4.0),
    ('wall', 'im_dt1_02_wall_01', 6.0),
    ('ground', 'im_sidewalk010', 4.0),
    ('ceiling', 'im_roof_underside003', 8.0),
    ('median', MEDIAN_TEXTURE, 2.5),
]
CAT_ROAD, CAT_SIDEWALK, CAT_WINDOWS, CAT_GRASS, CAT_METAL, CAT_WALL, CAT_GROUND, CAT_CEILING, CAT_MEDIAN = range(9)

TEXTURE_PALETTES = {
    'bh1_06': [
        ('models/gtav/dt1_02/im_road_012', 5.0),
        ('models/gtav/dt1_02/im_sidewalk010', 4.0),
        ('models/gtav/bh1_06/bh1_06_rsn_os_window_0001', 3.0),
        ('models/gtav/bh1_06/im_grass11b_fur', 3.0),
        ('models/gtav/bh1_06/bh1_06_rsn_mc_metal_0001', 4.0),
        ('models/gtav/bh1_06/bh1_06_rsn_os_marblewall_0003', 4.0),
        ('models/gtav/dt1_02/im_sidewalk010', 4.0),
        ('models/gtav/bh1_06/bh1_06_rsn_os_marblewall_0003', 4.0),
        (MEDIAN_TEXTURE, 2.5),
    ],
    'bh1_17': [
        ('models/gtav/bh1_17/im_road_027b', 4.0),
        ('models/gtav/bh1_17/dc_sidewalk004', 2.5),
        ('models/gtav/bh1_06/bh1_06_rsn_os_window_0001', 3.0),
        ('models/gtav/bh1_06/im_grass11b_fur', 3.0),
        ('models/gtav/bh1_06/bh1_06_rsn_mc_metal_0001', 4.0),
        ('models/gtav/bh1_06/bh1_06_rsn_os_marblewall_0003', 4.0),
        ('models/gtav/bh1_17/dc_sidewalk004', 2.5),
        ('models/gtav/bh1_06/bh1_06_rsn_os_marblewall_0003', 4.0),
        (MEDIAN_TEXTURE, 2.5),
    ],
    'losantos': [
        ('models/gtav/bh1_17/im_road_027b', 4.0),
        ('models/gtav/bh1_17/dc_sidewalk004', 2.5),
        ('models/gtav/dt1_02/im_dt1_02_windows_01', 6.0),
        ('models/gtav/bh1_06/im_grass11b_fur', 3.0),
        ('models/gtav/dt1_02/im_dt1_02_wall_02', 4.0),
        ('models/gtav/dt1_02/im_dt1_02_wall_01', 6.0),
        ('models/gtav/bh1_17/dc_sidewalk004', 2.5),
        ('models/gtav/dt1_02/im_roof_underside003', 8.0),
        (MEDIAN_TEXTURE, 2.5),
    ],
    'dt1_02': [(tex if tex.startswith('models/')
                else f'models/gtav/dt1_02/{tex}', metres)
               for _kind, tex, metres in DEFAULT_CATEGORY_TEXTURES],
}


def resolve_texture_palette(value):
    if not value:
        return None
    if value in TEXTURE_PALETTES:
        return TEXTURE_PALETTES[value]
    return [(tex if tex.startswith('models/') else f'{value}/{tex}', metres)
            for _kind, tex, metres in DEFAULT_CATEGORY_TEXTURES]

# gta collision material ids
ROAD_MATERIALS = {4}  # tarmac
SIDEWALK_MATERIALS = {15, 68, 181}  # pavement
WINDOW_MATERIALS = {57, 59}  # tower glass
GRASS_MATERIALS = {174}  # planting
METAL_MATERIALS = {14, 50, 64, 66, 96, 108, 113}  # metal trim

MEDIAN_CELL_M = 1.0
MEDIAN_RADIUS_M = 3.0
MEDIAN_MAX_CELLS = 4_000_000


def _shift(grid, di, dj):
    out = np.zeros_like(grid)
    ny, nx = grid.shape
    si = slice(max(0, di), ny - max(0, -di))
    sj = slice(max(0, dj), nx - max(0, -dj))
    ti = slice(max(0, -di), ny - max(0, di))
    tj = slice(max(0, -dj), nx - max(0, dj))
    out[ti, tj] = grid[si, sj]
    return out


def _ray_hits(grid, di, dj, steps):
    acc = np.zeros_like(grid)
    for s in range(1, steps + 1):
        acc |= _shift(grid, di * s, dj * s)
    return acc


def _box_coverage(lo, hi, origin, cell, shape):
    ny, nx = shape
    j0 = np.floor((lo[:, 0] - origin[0]) / cell).astype(np.int64)
    j1 = np.floor((hi[:, 0] - origin[0]) / cell).astype(np.int64)
    i0 = np.floor((lo[:, 1] - origin[1]) / cell).astype(np.int64)
    i1 = np.floor((hi[:, 1] - origin[1]) / cell).astype(np.int64)
    keep = (j1 >= 0) & (i1 >= 0) & (j0 < nx) & (i0 < ny)
    j0 = np.clip(j0[keep], 0, nx - 1)
    j1 = np.clip(j1[keep], 0, nx - 1)
    i0 = np.clip(i0[keep], 0, ny - 1)
    i1 = np.clip(i1[keep], 0, ny - 1)
    if not len(j0):
        return np.zeros(shape, dtype=bool)
    diff = np.zeros((ny + 1, nx + 1), dtype=np.int32)
    np.add.at(diff, (i0, j0), 1)
    np.add.at(diff, (i1 + 1, j0), -1)
    np.add.at(diff, (i0, j1 + 1), -1)
    np.add.at(diff, (i1 + 1, j1 + 1), 1)
    return np.cumsum(np.cumsum(diff, axis=0), axis=1)[:ny, :nx] > 0


def median_strips(triangles, material, scale=SCALE):
    pavement = np.isin(material, list(SIDEWALK_MATERIALS))
    tarmac = np.isin(material, list(ROAD_MATERIALS))
    if not pavement.any() or not tarmac.any():
        return np.zeros(len(triangles), dtype=bool)

    pts = triangles[:, :, :2]
    lo = pts.min(axis=1)
    hi = pts.max(axis=1)
    area = float((hi[:, 0].max() - lo[:, 0].min())
                 * (hi[:, 1].max() - lo[:, 1].min()))
    cell = max(MEDIAN_CELL_M, (area / MEDIAN_MAX_CELLS) ** 0.5) * scale
    steps = max(1, int(round(MEDIAN_RADIUS_M * scale / cell)))

    origin = lo.min(axis=0) - cell
    nx = int((hi[:, 0].max() - origin[0]) / cell) + 2
    ny = int((hi[:, 1].max() - origin[1]) / cell) + 2
    covered = _box_coverage(lo[tarmac], hi[tarmac], origin, cell, (ny, nx))

    # tarmac on both sides = median
    ok = np.zeros((ny, nx), dtype=bool)
    for di, dj in ((1, 0), (0, 1), (1, 1), (1, -1)):
        ok |= _ray_hits(covered, di, dj, steps) & _ray_hits(covered, -di, -dj, steps)

    centroids = pts.mean(axis=1)
    ci = np.clip(((centroids[:, 1] - origin[1]) / cell).astype(np.int64), 0, ny - 1)
    cj = np.clip(((centroids[:, 0] - origin[0]) / cell).astype(np.int64), 0, nx - 1)
    return pavement & ok[ci, cj]


def assign_categories(triangles, normals, material, scale=SCALE):
    cat = np.full(len(triangles), CAT_GROUND, dtype=np.int64)
    up = normals[:, 2] > 0.7
    down = normals[:, 2] < -0.7
    vertical = np.abs(normals[:, 2]) < 0.4
    cat[vertical] = CAT_WALL
    cat[down] = CAT_CEILING
    cat[np.isin(material, list(WINDOW_MATERIALS))] = CAT_WINDOWS
    cat[np.isin(material, list(METAL_MATERIALS)) & vertical] = CAT_METAL
    cat[np.isin(material, list(ROAD_MATERIALS)) & up] = CAT_ROAD
    cat[np.isin(material, list(SIDEWALK_MATERIALS))] = CAT_SIDEWALK
    cat[np.isin(material, list(GRASS_MATERIALS))] = CAT_GRASS
    cat[median_strips(triangles, material, scale)] = CAT_MEDIAN
    return cat


@dataclass
class ExportReport:
    tiles: int = 0
    tile_triangles: int = 0
    chunks: int = 0
    chunk_triangles: int = 0
    dropped_tiles: int = 0
    dropped_chunks: int = 0
    dropped_tile_tris: int = 0
    dropped_chunk_tris: int = 0
    duplicates_removed: int = 0
    spawn: tuple = (0.0, 0.0, 0.0)
    bounds_min: tuple = (0.0, 0.0, 0.0)
    bounds_max: tuple = (0.0, 0.0, 0.0)
    materials: dict = field(default_factory=dict)


def scaled_triangles(world, scale=SCALE):
    return world.triangles * float(scale)


def triangle_normals(triangles):
    a, b, c = triangles[:, 0], triangles[:, 1], triangles[:, 2]
    n = np.cross(b - a, c - a)
    length = np.linalg.norm(n, axis=1)
    good = length > 1e-12
    n[good] /= length[good, None]
    n[~good] = (0.0, 0.0, 1.0)
    return n, length * 0.5


def grid_groups(triangles, cell):
    cx = np.floor(triangles[:, :, 0].mean(axis=1) / cell).astype(np.int64)
    cy = np.floor(triangles[:, :, 1].mean(axis=1) / cell).astype(np.int64)
    order = np.lexsort((cy, cx))
    sx, sy = cx[order], cy[order]
    if len(order) == 0:
        return
    cuts = np.flatnonzero((np.diff(sx) != 0) | (np.diff(sy) != 0)) + 1
    starts = np.concatenate(([0], cuts))
    ends = np.concatenate((cuts, [len(sx)]))
    for s, e in zip(starts, ends):
        yield int(sx[s]), int(sy[s]), order[s:e]


def dedupe_positions(positions, quantum=0.01):
    packed = np.round(positions * (1.0 / quantum)).astype(np.int64)
    unique, inverse = np.unique(packed, axis=0, return_inverse=True)
    return unique.astype(np.float64) * quantum, inverse.reshape(-1)


def cap_by_area(triangles, keep_indices, max_verts, warn):
    limit = max_verts // 3
    if len(triangles) <= limit:
        return triangles, keep_indices, 0
    _, areas = triangle_normals(triangles)
    order = np.argsort(-areas)[:limit]
    order.sort()
    warn(f'dropped {len(triangles) - limit} small triangles (vertex budget)')
    return triangles[order], keep_indices[order], len(triangles) - limit


def weld_key(pos, nrm, uv):
    return np.concatenate([
        np.round(np.asarray(pos, np.float64) * 100.0).astype(np.int64),
        np.round(np.asarray(nrm, np.float64) * 127.0).astype(np.int64),
        np.round(np.asarray(uv, np.float64) * 1000.0).astype(np.int64),
    ], axis=1)


def corners_of(triangles, normals, uvs, ids):
    pos = np.asarray(triangles, np.float64)[ids].reshape(-1, 3)
    n = np.asarray(normals, np.float64)[ids]
    nrm = n.reshape(-1, 3) if np.ndim(n) == 3 else np.repeat(n, 3, axis=0)
    uv = np.asarray(uvs, np.float64)[ids].reshape(-1, 2)
    return pos, nrm, uv


def chunk_materials(triangles, normals, uvs, material_dense, limit):
    tri = np.asarray(triangles, np.float64)
    nrm = np.asarray(normals, np.float64)
    uvs = np.asarray(uvs, np.float64)
    mat = np.asarray(material_dense, dtype=np.int64)

    def weld_count(ids):
        if not len(ids):
            return 0
        return len(np.unique(weld_key(*corners_of(tri, nrm, uvs, ids)),
                             axis=0))

    spills = []
    for m in np.unique(mat):
        ids = np.flatnonzero(mat == m)
        if weld_count(ids) <= limit:
            continue
        lo = 0
        while lo < len(ids):
            rest = ids[lo:]
            if weld_count(rest) <= limit:
                spills.append((m, rest))
                break
            low, high = 1, len(rest) - 1
            while low < high:
                mid = (low + high + 1) // 2
                if weld_count(ids[lo:lo + mid]) <= limit:
                    low = mid
                else:
                    high = mid - 1
            spills.append((m, ids[lo:lo + low]))
            lo += low

    if not spills:
        keep = np.ones(len(tri), dtype=bool)
        return [keep]

    spilled_mats = {m for m, _ in spills}
    masks = []
    whole = np.isin(mat, list(spilled_mats), invert=True)
    if whole.any():
        masks.append(whole)
    for _m, ids in spills:
        mask = np.zeros(len(tri), dtype=bool)
        mask[ids] = True
        masks.append(mask)
    return masks


def encode_vertices(pos, uv, nrm):
    pos64 = np.asarray(pos, np.float64)
    uv64 = np.asarray(uv, np.float64)
    nrm64 = np.asarray(nrm, np.float64)
    packed = np.clip(uv64 * 65535.0, 0, 65535).astype(np.uint16)
    nrm8 = np.clip(np.round(nrm64 * 127.0), -127, 127).astype(np.int8)
    # 0   pos f32x3
    # 12  uv f32x2
    # 20  uv u16x2
    # 24  nrm i8x3
    # 27  pad
    raw = np.zeros((len(pos64), 28), np.uint8)
    raw[:, :12] = pos64.astype(np.float32).view(np.uint8).reshape(-1, 12)
    raw[:, 12:20] = uv64.astype(np.float32).view(np.uint8).reshape(-1, 8)
    raw[:, 20:24] = packed.view(np.uint8).reshape(-1, 4)
    raw[:, 24:27] = nrm8.view(np.uint8).reshape(-1, 3)
    return raw.tobytes()


# only sorts by x, this fucking sucks but it mostly works
def dedupe_triangles(triangles, material, quantum=0.1):
    if len(triangles) == 0:
        return triangles, material, 0
    key = np.round(triangles / quantum).astype(np.int64)
    order = np.argsort(key[:, :, 0], axis=1)
    packed = np.take_along_axis(
        key, order[:, :, None].repeat(3, axis=2), axis=1).reshape(len(triangles), 9)
    _, first = np.unique(packed, axis=0, return_index=True)
    first.sort()
    return triangles[first], material[first], len(triangles) - len(first)


def merge_hi(base, hi, margin=2.0, cell=1.0):
    from .ybn import BoundStats, YbnWorld

    if base is None or base.triangle_count == 0:
        return hi
    if hi is None or hi.triangle_count == 0:
        return base

    lo = hi.bounds_min[:2] - margin
    up = hi.bounds_max[:2] + margin
    span = up - lo
    if not np.all(np.isfinite(span)) or np.any(span <= 0):
        return base

    step = float(cell)
    limit = 2048.0
    if span.max() / step > limit:
        step = float(span.max()) / limit
    nx = int(math.ceil(span[0] / step)) + 1
    ny = int(math.ceil(span[1] / step)) + 1

    def to_idx(xy):
        return np.floor((xy - lo) / step).astype(np.int64)

    hxy = hi.triangles[:, :, :2]
    a = to_idx(hxy.min(axis=1))
    b = to_idx(hxy.max(axis=1))
    a[:, 0] = np.clip(a[:, 0], 0, nx - 1)
    a[:, 1] = np.clip(a[:, 1], 0, ny - 1)
    b[:, 0] = np.clip(b[:, 0], 0, nx - 1)
    b[:, 1] = np.clip(b[:, 1], 0, ny - 1)

    diff = np.zeros((nx + 1, ny + 1), dtype=np.int32)
    np.add.at(diff, (a[:, 0], a[:, 1]), 1)
    np.add.at(diff, (b[:, 0] + 1, a[:, 1]), -1)
    np.add.at(diff, (a[:, 0], b[:, 1] + 1), -1)
    np.add.at(diff, (b[:, 0] + 1, b[:, 1] + 1), 1)
    occ = diff.cumsum(axis=0).cumsum(axis=1) > 0

    grown = occ.copy()
    grown[:-1, :] |= occ[1:, :]
    grown[1:, :] |= occ[:-1, :]
    grown[:, :-1] |= occ[:, 1:]
    grown[:, 1:] |= occ[:, :-1]
    occ = grown

    k = 9
    raw = np.array([[1.0 - (i + j) / k, i / k, j / k]
                    for i in range(k) for j in range(k - i)])
    w = raw / raw.sum(axis=1, keepdims=True)

    bxy = base.triangles[:, :, :2]
    centroids = bxy.mean(axis=1)

    inside = ((centroids[:, 0] >= lo[0]) & (centroids[:, 0] <= up[0])
              & (centroids[:, 1] >= lo[1]) & (centroids[:, 1] <= up[1]))

    replaced = np.zeros(base.triangle_count, dtype=bool)
    chunk = 20000
    for s in range(0, base.triangle_count, chunk):
        e = min(s + chunk, base.triangle_count)
        pts = (bxy[s:e, 0][:, None, :] * w[None, :, 0, None]
               + bxy[s:e, 1][:, None, :] * w[None, :, 1, None]
               + bxy[s:e, 2][:, None, :] * w[None, :, 2, None])
        flat = pts.reshape(-1, 2)
        c = to_idx(flat)
        ok = ((c[:, 0] >= 0) & (c[:, 0] < nx)
              & (c[:, 1] >= 0) & (c[:, 1] < ny))
        frac = np.zeros(len(flat), dtype=np.float64)
        if ok.any():
            frac[ok] = occ[np.clip(c[:, 0], 0, nx - 1)[ok],
                           np.clip(c[:, 1], 0, ny - 1)[ok]]
        frac = frac.reshape(-1, len(w)).mean(axis=1)
        replaced[s:e] = (frac >= 0.9) & inside[s:e]

    keep = ~replaced
    triangles = np.concatenate([hi.triangles, base.triangles[keep]], axis=0)
    material = np.concatenate([hi.material, base.material[keep]], axis=0)
    return YbnWorld(
        triangles=triangles,
        material=material,
        bounds_min=np.minimum(base.bounds_min, hi.bounds_min),
        bounds_max=np.maximum(base.bounds_max, hi.bounds_max),
        stats=BoundStats(),
        source=f'{hi.source} + {base.source} (not covered by hi)',
    )


def clip_world(world, bounds_min, bounds_max, margin=0.0):
    from .ybn import YbnWorld

    lo = np.asarray(bounds_min[:2], dtype=np.float64) - margin
    up = np.asarray(bounds_max[:2], dtype=np.float64) + margin
    centroids = world.triangles[:, :, :2].mean(axis=1)
    keep = ((centroids[:, 0] >= lo[0]) & (centroids[:, 0] <= up[0])
            & (centroids[:, 1] >= lo[1]) & (centroids[:, 1] <= up[1]))
    triangles = world.triangles[keep]
    material = world.material[keep]
    if len(triangles) == 0:
        return None
    return YbnWorld(
        triangles=triangles,
        material=material,
        bounds_min=triangles.reshape(-1, 3).min(axis=0),
        bounds_max=triangles.reshape(-1, 3).max(axis=0),
        stats=world.stats,
        source=world.source,
    )


def combine(parts):
    from .ybn import BoundStats, YbnWorld

    parts = [p for p in parts if p is not None and p.triangle_count]
    if not parts:
        raise ValueError('nothing to combine')
    if len(parts) == 1:
        return parts[0]
    first = parts[0]
    triangles = np.concatenate([p.triangles for p in parts], axis=0)
    material = np.concatenate([p.material for p in parts], axis=0)
    bounds_min = np.min(np.stack([p.bounds_min for p in parts]), axis=0)
    bounds_max = np.max(np.stack([p.bounds_max for p in parts]), axis=0)
    return YbnWorld(
        triangles=triangles,
        material=material,
        bounds_min=bounds_min,
        bounds_max=bounds_max,
        stats=first.stats,
        source=' + '.join(p.source for p in parts),
    )


def planar_uv(triangles, normals, repeat_units=None):
    pick = np.argmax(np.abs(normals), axis=1)
    x, y, z = triangles[:, :, 0], triangles[:, :, 1], triangles[:, :, 2]
    uv = np.empty((len(triangles), 3, 2), dtype=np.float64)
    uv[:, :, 0] = np.where(pick[:, None] == 0, y, x)
    uv[:, :, 1] = np.where(pick[:, None] == 2, y, z)
    if repeat_units is None:
        repeat_units = np.full(len(triangles), 128.0)
    return uv / np.asarray(repeat_units, dtype=np.float64)[:, None, None]


def write_tile(path, triangles, normals, material_dense, material_names,
               double_sided=True, repeat_units=None, uvs=None, warn=None):
    triangles = np.asarray(triangles, np.float64)
    normals = np.asarray(normals, np.float64)
    material_dense = np.asarray(material_dense, dtype=np.int64)
    if uvs is not None:
        uvs = np.asarray(uvs, np.float64)
    source_count = len(triangles)
    if source_count == 0:
        return None

    if uvs is None:
        uvs = planar_uv(triangles, normals, repeat_units)

    # front and mirror never weld, half the window
    limit = MAX_TILE_VERTS // (2 if double_sided else 1)
    chunks = chunk_materials(triangles, normals, uvs, material_dense, limit)

    out_submeshes = []
    final_indices = []
    vert_pos, vert_nrm, vert_uv = [], [], []
    idx_offset = 0
    vertex_base = 0
    for keep in chunks:
        src = triangles[keep]
        src_nrm = normals[keep]
        src_uv = uvs[keep]
        src_mat = material_dense[keep]
        count = len(src)

        if double_sided:
            positions = np.concatenate([src, src], axis=0)
            side_normals = np.concatenate([src_nrm, -src_nrm], axis=0)
            side_uvs = np.concatenate([src_uv, src_uv], axis=0)
            side_material = np.concatenate([src_mat, src_mat], axis=0)
            front = np.concatenate([np.ones(count, dtype=bool),
                                    np.zeros(count, dtype=bool)])
        else:
            positions = src
            side_normals = src_nrm
            side_uvs = src_uv
            side_material = src_mat
            front = np.ones(count, dtype=bool)

        verts = positions.reshape(-1, 3)
        if np.ndim(side_normals) == 3:
            vnorm = side_normals.reshape(-1, 3)
        else:
            vnorm = np.repeat(side_normals, 3, axis=0)
        vuv = side_uvs.reshape(-1, 2)

        mat_ids = np.asarray(side_material, dtype=np.int64)
        order_tris = np.argsort(mat_ids, kind='stable')
        if len(order_tris):
            cuts = np.flatnonzero(np.diff(mat_ids[order_tris])) + 1
            runs = [r for r in np.split(order_tris, cuts) if len(r)]
        else:
            runs = []

        for tris_here in runs:
            m = int(mat_ids[tris_here[0]])

            corners = (tris_here[:, None] * 3 + np.arange(3)).reshape(-1)
            key = weld_key(verts[corners], vnorm[corners], vuv[corners])
            _, first, inverse = np.unique(key, axis=0, return_index=True,
                                          return_inverse=True)
            block = np.asarray(inverse).reshape(-1, 3)

            is_front = front[tris_here]
            # source front face is clockwise
            if np.any(is_front):
                block[is_front] = block[is_front][:, [0, 2, 1]]
            flat = block.reshape(-1).astype(np.uint16)

            out_submeshes.append((m, idx_offset, len(flat), vertex_base))
            final_indices.append(flat)
            vert_pos.append(verts[corners][first])
            vert_nrm.append(vnorm[corners][first])
            vert_uv.append(vuv[corners][first])
            vertex_base += len(first)
            idx_offset += len(flat)

    if not final_indices:
        return None
    indices_out = np.concatenate(final_indices)
    out_verts = np.concatenate(vert_pos, axis=0)
    out_norm = np.concatenate(vert_nrm, axis=0)
    out_uv = np.concatenate(vert_uv, axis=0)

    with open(path, 'wb') as f:
        # 0   magic "GVT1"
        # 4   version
        # 6   flags
        # 8   vertex count
        # 12  index count
        # 16  submesh count
        # 20  material count
        # 24  stride
        # 28  reserved
        f.write(struct.pack('<IHHIIIII', TILE_MAGIC, 1, 3,
                            len(out_verts), len(indices_out),
                            len(out_submeshes), len(material_names), 28))
        f.write(struct.pack('<I', 0))
        for dense, index_offset, index_count, vertex_offset in out_submeshes:
            f.write(struct.pack('<IIII', dense, index_offset, index_count,
                                vertex_offset))
        # 64 byte name slots
        for name in material_names:
            raw = name.encode('ascii')
            if len(raw) > 63:
                raise ValueError(
                    f'material name does not fit GVT1 ({len(raw)} > 63): {name}')
            f.write(raw + b'\0' * (64 - len(raw)))
        f.write(encode_vertices(out_verts, out_uv, out_norm))
        f.write(indices_out.astype('<u2').tobytes())

    return {
        'triangles': source_count,
        'primitives': int(source_count * (2 if double_sided else 1)),
        'vertices': int(len(out_verts)),
        'indices': int(len(indices_out)),
        'submeshes': len(out_submeshes),
        'materials': len(material_names),
        'dropped': 0,  # always 0 now, old callers still read it
    }


def write_chunk(path, triangles):
    tri_count = len(triangles)
    if tri_count == 0:
        return None

    verts = triangles.reshape(-1, 3)
    unique, inverse = dedupe_positions(verts)
    indices = inverse.reshape(-1, 3)[:, [0, 2, 1]]

    with open(path, 'wb') as f:
        # 0   magic "GVC1"
        # 4   version
        # 6   flags
        # 8   triangle count
        # 12  vertex count
        # 16  reserved
        f.write(struct.pack('<IHHIII', CHUNK_MAGIC, 1, 0,
                            tri_count, len(unique), 0))
        f.write(unique.astype('<f4').tobytes())
        f.write(indices.astype('<u2').tobytes())

    return {'triangles': tri_count, 'vertices': int(len(unique))}


ROAD_DRAWABLE_WORDS = ('sect', 'ovly', 'road', '_rd', 'fwy', 'tunnel', 'tnl',
                       'kerb', 'pave', 'walk', 'median', 'crossing')
SURFACE_DRAWABLE_HEIGHT = 3.0  # metres, flatter is surface art


def is_road_pack(name):
    low = (name or '').replace('\\', '/').rsplit('/', 1)[-1].lower()
    return '_rd' in low or 'roads' in low


SHELL_GLASS_WORDS = ('glass', 'window', 'shop')
COVER_SEETHROUGH_WORDS = ('decal', 'puddle', 'ovly', 'overlay')


def _is_cover_texture(key, texture_materials):
    if texture_materials.get(key) is False:
        return False
    if any(w in key for w in SHELL_GLASS_WORDS) or any(w in key for w in COVER_SEETHROUGH_WORDS):
        return False
    from . import materials as _matmod
    return not (_matmod.is_overlay_name(key) or _matmod.is_marking_name(key))
SHELL_CELL_M = 1.5  # occupancy voxel edge, metres
SHELL_MAX_SUB = 48  # cap on per-edge subdivisions of one triangle
_BARY_CACHE = {}


def _bary_grid(n):
    g = _BARY_CACHE.get(n)
    if g is None:
        pts = [(i / n, j / n) for i in range(n + 1) for j in range(n + 1 - i)]
        g = np.array(pts, dtype=np.float64)
        _BARY_CACHE[n] = g
    return g


def _pack_voxels(v):
    v = v.astype(np.int64) + (1 << 20)
    return (v[:, 0] << 42) | (v[:, 1] << 21) | v[:, 2]


def _shell_covered(coll_tris, drawables, texture_materials, scale):
    cell = SHELL_CELL_M * scale
    keys = []
    for d in drawables:
        for g in d.geometries:
            k = (g.texture or '').lower()
            if not _is_cover_texture(k, texture_materials):
                continue
            if len(g.indices) == 0:
                continue
            tri = g.positions[g.indices] * scale  # (T, 3, 3)
            edge = np.max(np.linalg.norm(
                tri - np.roll(tri, 1, axis=1), axis=2), axis=1)
            sub = np.clip(np.ceil(edge / cell), 1, SHELL_MAX_SUB).astype(np.int64)
            for n in np.unique(sub):
                t = tri[sub == n]
                b = _bary_grid(int(n))  # (P, 2)
                a, c = b[:, 0], b[:, 1]
                p = (t[:, None, 0, :] * (1.0 - a - c)[None, :, None]
                     + t[:, None, 1, :] * a[None, :, None]
                     + t[:, None, 2, :] * c[None, :, None]).reshape(-1, 3)
                keys.append(np.unique(_pack_voxels(np.floor(p / cell))))
    covered = np.zeros(len(coll_tris), dtype=bool)
    if not keys or len(coll_tris) == 0:
        return covered
    occ = np.unique(np.concatenate(keys))
    cen = coll_tris.mean(axis=1)
    samples = [cen] + [cen + (coll_tris[:, i] - cen) * (4.0 / 6.0) for i in range(3)]
    hits = np.zeros(len(coll_tris), dtype=np.int64)
    offsets = np.array([(dx, dy, dz) for dx in (-1, 0, 1)
                        for dy in (-1, 0, 1) for dz in (-1, 0, 1)], dtype=np.int64)
    for s in samples:
        base = np.floor(s / cell).astype(np.int64)
        hit = np.zeros(len(coll_tris), dtype=bool)
        for off in offsets:
            q = _pack_voxels(base + off)
            pos = np.searchsorted(occ, q)
            pos[pos >= len(occ)] = len(occ) - 1
            hit |= occ[pos] == q
        hits += hit
    covered = hits >= 2  # 2 of 4 samples
    return covered


FILLER_CELL_M = 0.5  # occupancy voxel edge, metres (1-voxel dilation)
FILLER_SAMPLE_M = 0.25  # drawable surface sample spacing, metres
FILLER_ANGLE_DEG = 25.0  # max angle between filler and art normals
FILLER_MAX_SUB = 256  # cap on per-edge subdivisions of one drawable tri
_FILLER_CHUNK = 1 << 21  # max surface samples materialised at once
_NQ = 8  # normal quantisation: round(n * _NQ) per axis
_NQ_W = 2 * _NQ + 1
_NQ_BITS = 13  # _NQ_W ** 3 = 4913 codes < 2 ** 13


def _normal_codes(n):
    q = np.rint(np.asarray(n, dtype=np.float64) * _NQ).astype(np.int64) + _NQ
    return (q[:, 0] * _NQ_W + q[:, 1]) * _NQ_W + q[:, 2]


def _normal_code_dirs():
    g = np.arange(_NQ_W, dtype=np.float64) - _NQ
    d = np.stack(np.meshgrid(g, g, g, indexing='ij'), axis=-1).reshape(-1, 3)
    length = np.linalg.norm(d, axis=1)
    length[length == 0] = 1.0
    return d / length[:, None]


def _filler_covered(coll_tris, drawables, texture_materials, scale):
    n_coll = len(coll_tris)
    covered = np.zeros(n_coll, dtype=bool)
    if n_coll == 0:
        return covered
    cover = []
    for d in drawables:
        for g in d.geometries:
            k = (g.texture or '').lower()
            if not _is_cover_texture(k, texture_materials):
                continue
            if len(g.indices) == 0:
                continue
            cover.append(np.asarray(g.positions, dtype=np.float64)[g.indices] * scale)
    if not cover:
        return covered
    art = np.concatenate(cover, axis=0)  # (T, 3, 3)
    art_n, art_area = triangle_normals(art)
    good = art_area > 1e-9
    art, art_n = art[good], art_n[good]
    if len(art) == 0:
        return covered
    art_code = _normal_codes(art_n)

    cell = FILLER_CELL_M * scale
    lo = np.floor(np.minimum(art.reshape(-1, 3).min(axis=0),
                             coll_tris.reshape(-1, 3).min(axis=0)) / cell).astype(np.int64) - 2
    hi = np.floor(np.maximum(art.reshape(-1, 3).max(axis=0),
                             coll_tris.reshape(-1, 3).max(axis=0)) / cell).astype(np.int64) + 2
    bits = max(1, int(np.max(hi - lo)).bit_length())
    if 3 * bits + _NQ_BITS > 62:
        return covered  # absurd extent, keep all filler

    # voxel xyz, normal code in the low bits
    def pack(v):
        return (((v[:, 0] << bits) | v[:, 1]) << bits | v[:, 2]) << _NQ_BITS

    step = FILLER_SAMPLE_M * scale
    edge = np.max(np.linalg.norm(art - np.roll(art, 1, axis=1), axis=2), axis=1)
    sub = np.clip(np.ceil(edge / step), 1, FILLER_MAX_SUB).astype(np.int64)
    keys = []
    for n in np.unique(sub):
        sel = np.nonzero(sub == n)[0]
        b = _bary_grid(int(n))  # (P, 2)
        wa = (1.0 - b[:, 0] - b[:, 1])[None, :, None]
        wb = b[:, 0][None, :, None]
        wc = b[:, 1][None, :, None]
        rows = max(1, _FILLER_CHUNK // len(b))
        for s0 in range(0, len(sel), rows):
            idx = sel[s0:s0 + rows]
            t = art[idx]
            p = (t[:, None, 0, :] * wa + t[:, None, 1, :] * wb
                 + t[:, None, 2, :] * wc).reshape(-1, 3)
            v = np.floor(p / cell).astype(np.int64) - lo
            keys.append(np.unique(pack(v) | np.repeat(art_code[idx], len(b))))
    occ = np.unique(np.concatenate(keys))
    code_mask = (1 << _NQ_BITS) - 1
    dirs = _normal_code_dirs()
    cos_max = np.cos(np.radians(FILLER_ANGLE_DEG))

    coll_n, _ = triangle_normals(coll_tris)
    cen = coll_tris.mean(axis=1)
    samples = [cen] + [cen + (coll_tris[:, i] - cen) * (4.0 / 6.0) for i in range(3)]
    offsets = np.array([(dx, dy, dz) for dx in (-1, 0, 1)
                        for dy in (-1, 0, 1) for dz in (-1, 0, 1)], dtype=np.int64)
    hits = np.zeros(n_coll, dtype=np.int64)
    for s in samples:
        base = np.floor(s / cell).astype(np.int64) - lo
        hit = np.zeros(n_coll, dtype=bool)
        for off in offsets:
            todo = np.nonzero(~hit)[0]
            if len(todo) == 0:
                break
            first = pack(base[todo] + off)
            left = np.searchsorted(occ, first)
            right = np.searchsorted(occ, first + (1 << _NQ_BITS))
            cnt = right - left
            some = cnt > 0
            if not some.any():
                continue
            todo, left, cnt = todo[some], left[some], cnt[some]
            # ugly, expands every (sample, art normal) pair
            rep = np.repeat(np.arange(len(todo)), cnt)
            pos = np.repeat(left - (np.cumsum(cnt) - cnt), cnt) + np.arange(len(rep))
            dots = np.einsum('ij,ij->i', dirs[occ[pos] & code_mask], coll_n[todo[rep]])
            hit[todo[rep[dots >= cos_max]]] = True
        hits += hit
    covered = hits >= 2
    return covered


def build_ydr_visual(world, drawables, texture_materials,
                     palette_prefix='bh1_17', scale=SCALE, log=print,
                     road_pack=False):
    collisions = scaled_triangles(world, scale)
    collisions, material, _ = dedupe_triangles(collisions, world.material)

    road_boxes = []
    building_boxes = []
    for drawable in drawables:
        source = (drawable.source or '').lower()
        scaled_min = np.asarray(drawable.bounds_min, dtype=np.float64) * scale
        scaled_max = np.asarray(drawable.bounds_max, dtype=np.float64) * scale
        height_m = float(scaled_max[2] - scaled_min[2]) / scale
        surface = (road_pack
                   or height_m <= SURFACE_DRAWABLE_HEIGHT
                   or any(word in source for word in ROAD_DRAWABLE_WORDS))
        if surface:
            road_boxes.append((scaled_min, scaled_max))
        else:
            building_boxes.append((scaled_min, scaled_max))

    centroids = collisions.mean(axis=1)
    if road_boxes:
        offset = 0.03 * scale  # 3 cm under the road art
        inside_any = np.zeros(len(collisions), dtype=bool)
        for lo, hi in road_boxes:
            inside_any |= ((centroids[:, 0] >= lo[0]) & (centroids[:, 0] <= hi[0])
                           & (centroids[:, 1] >= lo[1]) & (centroids[:, 1] <= hi[1]))
        collisions[inside_any, :, 2] -= offset

    keep = np.ones(len(collisions), dtype=bool)
    lower_building = np.zeros(len(collisions), dtype=bool)
    high_any = np.zeros(len(collisions), dtype=bool)
    for lo, hi in building_boxes:
        inside_xy = ((centroids[:, 0] >= lo[0]) & (centroids[:, 0] <= hi[0])
                     & (centroids[:, 1] >= lo[1]) & (centroids[:, 1] <= hi[1]))
        high = inside_xy & (centroids[:, 2] > lo[2] + 2.0 * scale)
        high_any |= high
        lower_building |= inside_xy & ~high
    lower_building &= ~high_any
    if high_any.any():
        covered = _shell_covered(collisions[high_any], drawables,
                                 texture_materials, scale)
        idx = np.nonzero(high_any)[0]
        keep[idx[covered]] = False
        log(f'  shell: {int(covered.sum())} of {len(idx)} collision tris under '
            f'buildings dropped (covered by opaque drawable), '
            f'{int((~covered).sum())} kept (uncovered)')

    cand = np.nonzero(keep)[0]
    if len(cand) and drawables:
        covered = _filler_covered(collisions[cand], drawables,
                                  texture_materials, scale)
        keep[cand[covered]] = False
        log(f'  filler: {int(covered.sum())} of {len(cand)} collision tris '
            f'dropped (covered by same-facing opaque drawable within '
            f'~{FILLER_CELL_M:g} m), {int((~covered).sum())} kept')
    if lower_building.any():
        collisions[lower_building, :, 2] -= 0.05 * scale  # 5 cm under the building

    normals, _ = triangle_normals(collisions)

    palette = resolve_texture_palette(palette_prefix)
    names = [name for name, _metres in palette]
    categories = assign_categories(collisions, normals, material, scale=scale)
    repeat_by_cat = np.array([metres * scale for _name, metres in palette])
    collision_uv = planar_uv(collisions, normals, repeat_by_cat[categories])

    triangles = [collisions[keep]]
    uvs = [collision_uv[keep]]
    vnorm = [np.repeat(normals[keep][:, None, :], 3, axis=1)]
    mats = [categories[keep]]

    name_to_index = {n: i for i, n in enumerate(names)}
    for drawable in drawables:
        for geom in drawable.geometries:
            key = (geom.texture or '').lower()
            material_name = texture_materials.get(key)
            if material_name is False:
                continue
            if not material_name:
                material_name = names[CAT_WALL]
            if material_name not in name_to_index:
                name_to_index[material_name] = len(names)
                names.append(material_name)
            indices = geom.indices
            tri = geom.positions[indices] * scale
            if geom.normals is not None:
                nrm = geom.normals[indices]
            else:
                nrm = np.repeat(triangle_normals(tri)[0][:, None, :], 3, axis=1)
            triangles.append(tri)
            uvs.append(geom.uvs[indices].astype(np.float64))
            vnorm.append(nrm)
            mats.append(np.full(len(indices), name_to_index[material_name], np.int64))

    visual = {
        'triangles': np.concatenate(triangles, axis=0),
        'uvs': np.concatenate(uvs, axis=0),
        'normals': np.concatenate(vnorm, axis=0),
        'material': np.concatenate(mats, axis=0),
        'names': names,
    }
    log(f'  visual: {len(visual["triangles"])} triangles '
        f'({len(names)} materials, {int(keep.sum())} collision + '
        f'{sum(d.triangle_count for d in drawables)} drawable)')
    return visual


def choose_spawn(triangles, clearance_radius_m=8.0, scale=SCALE):
    if len(triangles) == 0:
        return np.array([0.0, 0.0, 512.0])
    normals, _ = triangle_normals(triangles)
    z = triangles[:, :, 2].mean(axis=1)
    z_min = float(z.min())
    low = z < np.percentile(z, 40)
    mask = low & (normals[:, 2] > 0.85)
    if not np.any(mask):
        mask = normals[:, 2] > 0.85
    if not np.any(mask):
        mask = np.ones(len(triangles), dtype=bool)

    centroids = triangles[mask].mean(axis=1)
    cz = centroids[:, 2]
    radius = clearance_radius_m * scale
    radius_sqr = radius * radius

    stride = max(1, len(centroids) // 400)
    candidate_idx = list(range(0, len(centroids), stride))
    candidate_idx.append(int(np.argmin(cz)))

    best_score = -np.inf
    best_point = None
    all_centroids = triangles.mean(axis=1)
    walls = np.abs(normals[:, 2]) < 0.5
    wall_centroids = all_centroids[walls]
    wall_radius_sqr = (14.0 * scale) ** 2
    for ci in candidate_idx:
        cx, cy, czz = centroids[ci]
        dx = all_centroids[:, 0] - cx
        dy = all_centroids[:, 1] - cy
        near = (dx * dx + dy * dy) < radius_sqr
        above = z[near]
        above = above[above > czz + 1.0 * scale]
        clearance = cap = 80.0 * scale
        if len(above):
            clearance = min(cap, float(above.min() - czz))
        score = clearance - (czz - z_min) * 0.25
        if len(wall_centroids):
            wdx = wall_centroids[:, 0] - cx
            wdy = wall_centroids[:, 1] - cy
            if np.any(wdx * wdx + wdy * wdy < wall_radius_sqr):
                score -= 2000.0  # no spawning next to a wall
        if score > best_score:
            best_score = score
            best_point = np.array([cx, cy, czz])

    best_point[2] += 128.0  # ~3 m above the surface
    return best_point


def export(
    world,
    out_root,
    map_name,
    scale=SCALE,
    tile_size=TILE_SIZE,
    phys_chunk_size=PHYS_CHUNK_SIZE,
    cell_size=CELL_SIZE,
    log=print,
    material_names=None,
    texture_prefix=None,
    visual=None,
    double_sided=True,
):
    triangles = scaled_triangles(world, scale)
    if len(triangles) == 0:
        raise ValueError('nothing to export')

    triangles, material, duplicates = dedupe_triangles(triangles, world.material)
    if duplicates:
        log(f'  removed {duplicates} duplicate collision surface(s)')

    out_dir = os.path.join(out_root, map_name)
    tiles_dir = os.path.join(out_dir, 'tiles')
    phys_dir = os.path.join(out_dir, 'phys')
    os.makedirs(tiles_dir, exist_ok=True)
    os.makedirs(phys_dir, exist_ok=True)

    report = ExportReport()
    report.duplicates_removed = duplicates
    report.bounds_min = tuple(triangles.reshape(-1, 3).min(axis=0).tolist())
    report.bounds_max = tuple(triangles.reshape(-1, 3).max(axis=0).tolist())

    def warn(text):
        log('  ! ' + text)

    palette = resolve_texture_palette(texture_prefix)
    texture_names = None
    categories = None
    repeats = None
    if visual is None and palette:
        all_normals = triangle_normals(triangles)[0]
        categories = assign_categories(triangles, all_normals, material,
                                       scale=scale)
        repeat_by_cat = np.array([metres * scale for _name, metres in palette])
        repeats = repeat_by_cat[categories]
        texture_names = [name for name, _metres in palette]
        report.materials = {str(i): name for i, name in enumerate(texture_names)}
    elif visual is not None:
        texture_names = list(visual['names'])
        report.materials = {str(i): name for i, name in enumerate(texture_names)}

    tile_source = visual['triangles'] if visual is not None else triangles

    tiles = {}
    for tx, ty, idx in grid_groups(tile_source, tile_size):
        tri = tile_source[idx]
        if len(tri) == 0:
            continue
        tri_uv = None
        if visual is not None:
            tri_normals = visual['normals'][idx]
            tri_dense = visual['material'][idx]
            tri_uv = visual['uvs'][idx]
            names = texture_names
            tri_repeat = None
        else:
            tri_normals = triangle_normals(tri)[0]
            if palette:
                tri_dense = categories[idx]
                names = texture_names
                tri_repeat = repeats[idx]
            else:
                tri_mat = material[idx]
                ids = sorted(set(int(m) for m in tri_mat))
                names = [((material_names or {}).get(i)
                          or f'models/lossantos/gta_surface_{i}') for i in ids]
                dense_of = {mid: dense for dense, mid in enumerate(ids)}
                tri_dense = np.array([dense_of[int(m)] for m in tri_mat], dtype=np.int64)
                tri_repeat = None

        filename = f't_{tx}_{ty}_0.bin'
        info = write_tile(os.path.join(tiles_dir, filename), tri, tri_normals,
                          tri_dense, names, double_sided=double_sided,
                          repeat_units=tri_repeat, uvs=tri_uv, warn=warn)
        if not info:
            continue
        if info.get('dropped'):
            report.dropped_tiles += 1
            report.dropped_tile_tris += info['dropped']
        report.tiles += 1
        report.tile_triangles += info['triangles']
        if visual is None and not palette:
            report.materials.update({i: names[dense_of[i]] for i in ids})
        # every lod is the same file here
        tiles[f'{tx} {ty}'] = {f'lod{lod}': {'file': filename} for lod in range(5)}

    chunks = {}
    for cx, cy, idx in grid_groups(triangles, phys_chunk_size):
        tri, idx, dropped = cap_by_area(triangles[idx], idx, MAX_CHUNK_VERTS, warn)
        if dropped:
            report.dropped_chunks += 1
            report.dropped_chunk_tris += dropped
        filename = f'g_{cx}_{cy}.bin'
        info = write_chunk(os.path.join(phys_dir, filename), tri)
        if not info:
            warn(f'chunk {cx} {cy} has too many vertices and was skipped')
            continue
        report.chunks += 1
        report.chunk_triangles += info['triangles']
        chunks[f'{cx} {cy}'] = {'file': filename}

    spawn = choose_spawn(triangles)
    report.spawn = tuple(float(v) for v in spawn)
    tri_z = (visual['triangles'] if visual is not None else triangles)[:, :, 2].mean(axis=1)
    tall = tri_z > np.percentile(tri_z, 75)
    if np.any(tall):
        target = (visual['triangles'] if visual is not None else triangles)[tall].reshape(-1, 3).mean(axis=0)
    else:
        target = (np.array(report.bounds_min) + np.array(report.bounds_max)) * 0.5
    yaw = math.degrees(math.atan2(target[1] - spawn[1], target[0] - spawn[0]))
    index = {
        'map': map_name,
        'tile_size': tile_size,
        'cell_size': cell_size,
        'phys_chunk_size': phys_chunk_size,
        'spawn': [round(float(v), 2) for v in spawn],
        'spawn_angles': [0.0, round(yaw, 1), 0.0],
        'materials': report.materials,
        'tiles': tiles,
        'phys_chunks': chunks,
        'source': {
            'format': 'gtav-ybn',
            'file': getattr(world, 'source', ''),
            'scale': scale,
            'gta_bounds_min': [round(float(v), 3) for v in world.bounds_min],
            'gta_bounds_max': [round(float(v), 3) for v in world.bounds_max],
            'gta_spawn': [round(float(v / scale), 3) for v in spawn],
        },
    }
    with open(os.path.join(out_dir, 'index.json'), 'w', encoding='utf-8') as f:
        json.dump(index, f, indent=2)
    indexdat.write_index_dat(index, out_dir)

    return report


def clear_output(out_root, map_name, log=print):
    out_dir = os.path.join(out_root, map_name)
    removed = 0
    skipped = 0
    for sub in ('tiles', 'phys'):
        folder = os.path.join(out_dir, sub)
        if not os.path.isdir(folder):
            continue
        for name in os.listdir(folder):
            if not name.endswith('.bin'):
                continue
            path = os.path.join(folder, name)
            # windows locks these sometimes
            for attempt in range(4):
                try:
                    os.remove(path)
                    removed += 1
                    break
                except FileNotFoundError:
                    break
                except OSError:
                    if attempt == 3:
                        skipped += 1
                        log(f'clear_output: could not remove {path} '
                            f'(locked?), leaving it')
                    else:
                        time.sleep(0.25 * (attempt + 1))
    log(f'cleared {removed} previous export file(s) in {out_dir}')
    if skipped:
        log(f'clear_output: {skipped} locked file(s) left in place')
