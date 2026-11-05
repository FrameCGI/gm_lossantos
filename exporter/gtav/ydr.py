from __future__ import annotations

from dataclasses import dataclass, field

import numpy as np

from .resource import ResourceReader, split_payload
from .ytd import GtaTexture, parse_dictionary_at

DRAWABLE_ROOT = 0x50000000


@dataclass
class YdrGeometry:
    positions: np.ndarray  # (V, 3) local
    uvs: np.ndarray  # (V, 2)
    normals: np.ndarray | None  # (V, 3)
    indices: np.ndarray  # (T, 3), as stored
    shader: int
    texture: str | None
    flip: bool = False  # stored winding is CCW, Source needs CW

    @property
    def triangle_count(self):
        return int(len(self.indices))


@dataclass
class YdrDrawable:
    bounds_min: np.ndarray
    bounds_max: np.ndarray
    geometries: list[YdrGeometry]
    textures: list[GtaTexture] = field(default_factory=list)
    source: str = ''

    @property
    def triangle_count(self):
        return sum(g.triangle_count for g in self.geometries)


# component type -> (dtype, count, bytes)
_COMPONENTS = {
    1: ('<f2', 2, 4),  # Half2
    2: ('<f4', 1, 4),  # Float
    3: ('<f2', 4, 8),  # Half4
    5: ('<f4', 2, 8),  # Float2
    6: ('<f4', 3, 12),  # Float3
    7: ('<f4', 4, 16),  # Float4
    8: ('u1', 4, 4),  # UByte4
    9: ('u1', 4, 4),  # Colour
    10: ('i1', 4, 4),  # RGBA8SNorm
}
# declaration bit -> name
_SEMANTICS = {0: 'position', 3: 'normal', 4: 'colour', 6: 'uv0'}


def _declaration_layout(flags, types):
    layout = {}
    offset = 0
    for i in range(16):
        if not ((flags >> i) & 1):
            continue
        ctype = (types >> (i * 4)) & 0xF
        dtype, components, size = _COMPONENTS.get(ctype, (None, 0, 0))
        if size:
            layout[_SEMANTICS.get(i, i)] = (offset, dtype, components, size)
        offset += size
    return layout


def _read_component(raw, offset, dtype, components):
    width = np.dtype(dtype).itemsize * components
    block = raw[:, offset:offset + width].copy()
    values = block.view(dtype).reshape(-1, components)
    return values.astype(np.float32)


def _read_vertices(r, vb):
    stride = r.u16(vb + 0x08)
    data = r.u64(vb + 0x10)
    count = r.u32(vb + 0x18)
    decl = r.u64(vb + 0x30)
    if not data or not count or not stride or not decl:
        return None, None, None
    flags = r.u32(decl)
    types = r.u64(decl + 8)
    layout = _declaration_layout(flags, types)
    if 'position' not in layout:
        return None, None, None
    raw = np.frombuffer(r.bytes_at(data, count * stride), np.uint8).reshape(count, stride)

    pos = _read_component(raw, *layout['position'][:3]) if 'position' in layout else None
    nrm = _read_component(raw, *layout['normal'][:3]) if 'normal' in layout else None
    uv = None
    if 'uv0' in layout:
        uv = _read_component(raw, *layout['uv0'][:3])
    else:
        uv = np.zeros((count, 2), np.float32)
    return pos, nrm, uv


def _read_indices(r, ib):
    count = r.u32(ib + 0x08)
    ptr = r.u64(ib + 0x10)
    if not count or not ptr:
        return np.empty((0, 3), np.int64)
    flat = r.u16s(ptr, count)
    return flat.reshape(-1, 3).astype(np.int64)


def _shader_textures(r, group):
    shaders_ptr = r.u64(group + 0x10)
    count = r.u16(group + 0x18)
    if not shaders_ptr or not count:
        return []
    result = []
    for sp in r.u64s(shaders_ptr, count):
        sp = int(sp)
        if not sp:
            result.append([])
            continue
        params = r.u64(sp)
        pcount = r.u8(sp + 0x10)
        names = []
        for pi in range(pcount):
            off = params + pi * 16
            # texture params only
            if r.u8(off) != 0:
                continue
            data_ptr = r.u64(off + 8)
            if not data_ptr:
                continue
            try:
                name = r.string_at(r.u64(data_ptr + 0x28))
            except Exception:
                name = ''
            if name:
                names.append(name)
        result.append(names)
    return result


def _model_list(r, pointer):
    if not pointer:
        return []
    header = r.u64(pointer)
    capacity = r.u16(pointer + 0x08)
    if not header or not capacity:
        return []
    return [int(p) for p in r.u64s(header, capacity) if int(p)]


def parse_ydr(data, system_size, source=''):
    system, graphics = split_payload(data, system_size)
    r = ResourceReader(system, graphics)
    root = DRAWABLE_ROOT
    bounds_min = np.array(r.vec3(root + 0x30), np.float64)
    bounds_max = np.array(r.vec3(root + 0x40), np.float64)
    group = r.u64(root + 0x10)

    textures = []
    shader_tex = []
    if group:
        dict_addr = r.u64(group + 0x08)
        if dict_addr:
            try:
                textures = parse_dictionary_at(r, int(dict_addr))
            except Exception:
                textures = []
        shader_tex = _shader_textures(r, group)

    # high detail first
    models = (_model_list(r, r.u64(root + 0x50))
              or _model_list(r, r.u64(root + 0x58))
              or _model_list(r, r.u64(root + 0x60))
              or _model_list(r, r.u64(root + 0x68)))

    geometries = []
    for mp in models:
        gptr = r.u64(mp + 0x08)
        gcount = r.u16(mp + 0x10)
        smap_ptr = r.u64(mp + 0x20)
        shader_map = r.u16s(smap_ptr, gcount) if smap_ptr else np.zeros(gcount, np.uint16)
        for gi, gp in enumerate(r.u64s(gptr, gcount)):
            gp = int(gp)
            if not gp:
                continue
            vb = r.u64(gp + 0x18)
            ib = r.u64(gp + 0x38)
            if not vb or not ib:
                continue
            pos, nrm, uv = _read_vertices(r, vb)
            if pos is None:
                continue
            ind = _read_indices(r, ib)
            if len(ind) == 0:
                continue
            shader = int(shader_map[gi]) if gi < len(shader_map) else 0
            tex = None
            if shader < len(shader_tex) and shader_tex[shader]:
                tex = shader_tex[shader][0]  # first texture = diffuse
            geometries.append(YdrGeometry(
                positions=pos, uvs=uv, normals=nrm, indices=ind,
                shader=shader, texture=tex,
            ))

    return YdrDrawable(
        bounds_min=bounds_min, bounds_max=bounds_max,
        geometries=geometries, textures=textures, source=source,
    )


def place_drawable(drawable, target_min, target_max, log=print):
    src_min = drawable.bounds_min
    src_max = drawable.bounds_max
    dst_min = np.asarray(target_min, np.float64)
    dst_max = np.asarray(target_max, np.float64)
    src = src_max - src_min
    dst = dst_max - dst_min

    rotate = (abs(src[0] - dst[1]) < abs(src[0] - dst[0])
              and abs(src[1] - dst[0]) < abs(src[1] - dst[1]))
    if rotate and abs(src[0] - dst[1]) < 3.0 and abs(src[1] - dst[0]) < 3.0:
        log('    ydr: rotating 90 degrees to match the collision footprint')
        for geom in drawable.geometries:
            x = geom.positions[:, 0].copy()
            y = geom.positions[:, 1].copy()
            geom.positions[:, 0] = -y
            geom.positions[:, 1] = x
            if geom.normals is not None:
                nx = geom.normals[:, 0].copy()
                ny = geom.normals[:, 1].copy()
                geom.normals[:, 0] = -ny
                geom.normals[:, 1] = nx
        drawable.bounds_min = np.array([
            -drawable.bounds_max[1], drawable.bounds_min[0], drawable.bounds_min[2]])
        drawable.bounds_max = np.array([
            -drawable.bounds_min[1], drawable.bounds_max[0], drawable.bounds_max[2]])
        src_min, src_max = drawable.bounds_min, drawable.bounds_max

    offset = dst_min - src_min
    for geom in drawable.geometries:
        geom.positions += offset
    drawable.bounds_min = src_min + offset
    drawable.bounds_max = src_max + offset
    return offset