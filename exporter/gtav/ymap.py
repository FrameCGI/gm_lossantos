from __future__ import annotations

from dataclasses import dataclass

import numpy as np

from .resource import ResourceReader, split_payload

META_ROOT = 0x50000000
CMAPDATA_SIZE = 512  # CMapData block length
CMAPTYPES_SIZE = 80  # CMapTypes block length


@dataclass
class Archetype:
    name: int
    asset_name: int
    bb_min: np.ndarray
    bb_max: np.ndarray
    lod_dist: float
    source: str = ''


@dataclass
class YmapEntity:
    archetype: int
    position: np.ndarray
    rotation: tuple  # x, y, z, w
    scale_xy: float
    scale_z: float
    flags: int
    lod_level: int
    source: str = ''


def _blocks(r):
    # block table rows, name hash, length, data pointer
    pointer = r.u64(META_ROOT + 0x30)
    count = r.i16(META_ROOT + 0x4C)
    return [(r.u32(pointer + i * 16),
             r.u32(pointer + i * 16 + 4),
             r.u64(pointer + i * 16 + 8)) for i in range(count)]


def _resolve(blocks, pointer):
    # (block index + 1) | (offset << 12)
    index = (pointer & 0xFFF) - 1
    offset = (pointer >> 12) & 0xFFFFF
    if index < 0 or index >= len(blocks):
        return None
    return blocks[index][2] + offset


def _reader(data, system_size):
    system, graphics = split_payload(data, system_size)
    return ResourceReader(system, graphics)


def parse_ymap(data, system_size, source=''):
    r = _reader(data, system_size)
    blocks = _blocks(r)
    block = next((b for b in blocks if b[1] == CMAPDATA_SIZE), None)
    if block is None:
        return []
    cmap = block[2]
    pointer = r.u64(cmap + 96)
    count = r.u16(cmap + 104)
    array = _resolve(blocks, pointer)
    if array is None or not count:
        return []
    entities = []
    for packed in r.u64s(array, count):
        addr = _resolve(blocks, int(packed))
        if addr is None:
            continue
        rotation = tuple(float(v) for v in np.frombuffer(
            r.slice(addr + 48, 16), '<f4'))
        entities.append(YmapEntity(
            archetype=r.u32(addr + 8),
            position=np.array(r.vec3(addr + 32), np.float64),
            rotation=rotation,
            scale_xy=r.f32(addr + 64),
            scale_z=r.f32(addr + 68),
            flags=r.u32(addr + 12),
            lod_level=r.u32(addr + 84),
            source=source,
        ))
    return entities


def parse_ytyp(data, system_size, source=''):
    r = _reader(data, system_size)
    blocks = _blocks(r)
    block = next((b for b in blocks if b[1] == CMAPTYPES_SIZE), None)
    if block is None:
        return []
    cmaptypes = block[2]
    pointer = r.u64(cmaptypes + 24)
    count = r.u16(cmaptypes + 32)
    array = _resolve(blocks, pointer)
    if array is None or not count:
        return []
    result = []
    for packed in r.u64s(array, count):
        addr = _resolve(blocks, int(packed))
        if addr is None:
            continue
        result.append(Archetype(
            name=r.u32(addr + 88),
            asset_name=r.u32(addr + 112),
            bb_min=np.array(r.vec3(addr + 32), np.float64),
            bb_max=np.array(r.vec3(addr + 48), np.float64),
            lod_dist=r.f32(addr + 8),
            source=source,
        ))
    return result


def name_hash(name: str) -> int:
    # jenkins one at a time
    h = 0
    for ch in name.lower().encode('ascii', 'ignore'):
        h = (h + ch) & 0xFFFFFFFF
        h = (h + (h << 10)) & 0xFFFFFFFF
        h ^= h >> 6
    h = (h + (h << 3)) & 0xFFFFFFFF
    h ^= h >> 11
    h = (h + (h << 15)) & 0xFFFFFFFF
    return h


def match_archetype(drawable, archetypes, tolerance=8.0):
    low = drawable.bounds_min
    high = drawable.bounds_max
    centre_z = (low[2] + high[2]) * 0.5

    def key_of(archetype):
        xy_error = (np.abs(archetype.bb_min[:2] - low[:2]).sum()
                    + np.abs(archetype.bb_max[:2] - high[:2]).sum())
        z_error = abs((archetype.bb_min[2] + archetype.bb_max[2]) * 0.5
                      - centre_z)
        return (xy_error, z_error)

    source = getattr(drawable, 'source', '') or ''
    stem = source.replace('\\', '/').rsplit('/', 1)[-1]
    if stem.lower().endswith('.ydr'):
        stem = stem[:-4]
    # name hash first, bbox is the fallback
    if stem:
        h = name_hash(stem)
        hits = [a for a in archetypes
                if a.name == h or a.asset_name == h]
        if hits:
            return min(hits, key=key_of)

    best = None
    for archetype in archetypes:
        key = key_of(archetype)
        if key[0] > tolerance:
            continue
        if best is None or key < best[0]:
            best = (key, archetype)
    return best[1] if best is not None else None


def quaternion_matrix(rotation):
    x, y, z, w = rotation
    return np.array([
        [1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)],
        [2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)],
        [2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)],
    ], np.float64)


def transform_drawable(drawable, entity, log=None):
    from dataclasses import replace
    from .ydr import YdrDrawable

    rot = quaternion_matrix(entity.rotation)
    scale = np.array([entity.scale_xy, entity.scale_xy, entity.scale_z])
    offset = np.asarray(entity.position)

    geometries = []
    for geom in drawable.geometries:
        positions = (geom.positions * scale) @ rot.T + offset
        normals = None
        if geom.normals is not None:
            # scale is 1 on map entities
            normals = geom.normals @ rot.T
        geometries.append(replace(geom, positions=positions, normals=normals))

    corners = np.array([[x, y, z] for x in (drawable.bounds_min[0], drawable.bounds_max[0])
                        for y in (drawable.bounds_min[1], drawable.bounds_max[1])
                        for z in (drawable.bounds_min[2], drawable.bounds_max[2])])
    corners = (corners * scale) @ rot.T + offset
    return YdrDrawable(
        bounds_min=corners.min(axis=0),
        bounds_max=corners.max(axis=0),
        geometries=geometries,
        textures=drawable.textures,
        source=drawable.source,
    )