from __future__ import annotations

from dataclasses import dataclass, field

import numpy as np

from .resource import ResourceReader, ResourceError

BOUNDS_FILE_ROOT = 0x50000000

TYPE_SPHERE = 0
TYPE_CAPSULE = 1
TYPE_BOX = 3
TYPE_GEOMETRY = 4
TYPE_BVH = 8
TYPE_COMPOSITE = 10
TYPE_DISC = 12
TYPE_CYLINDER = 13
TYPE_CLOTH = 15

TYPE_NAMES = {
    TYPE_SPHERE: 'sphere',
    TYPE_CAPSULE: 'capsule',
    TYPE_BOX: 'box',
    TYPE_GEOMETRY: 'geometry',
    TYPE_BVH: 'bvh',
    TYPE_COMPOSITE: 'composite',
    TYPE_DISC: 'disc',
    TYPE_CYLINDER: 'cylinder',
    TYPE_CLOTH: 'cloth',
}

POLY_TRIANGLE = 0
POLY_SPHERE = 1
POLY_CAPSULE = 2
POLY_BOX = 3
POLY_CYLINDER = 4

VERTEX_DTYPE = np.dtype([('x', '<i2'), ('y', '<i2'), ('z', '<i2')])
# 16 bytes, poly type in the low 3 bits of area
POLYGON_DTYPE = np.dtype([('area', '<u4'), ('w1', '<u2'),
                          ('w2', '<u2'), ('w3', '<u2'),
                          ('w4', '<u2'), ('w5', '<u2'), ('w6', '<u2')])
MATRIX_DTYPE = np.dtype([('c1', '<3f4'), ('f1', '<u4'),
                         ('c2', '<3f4'), ('f2', '<u4'),
                         ('c3', '<3f4'), ('f3', '<u4'),
                         ('c4', '<3f4'), ('f4', '<u4')])


@dataclass
class BoundStats:
    blocks: int = 0
    composites: int = 0
    geometries: int = 0
    bvhs: int = 0
    boxes: int = 0
    skipped: dict = field(default_factory=dict)


@dataclass
class YbnWorld:

    triangles: np.ndarray  # (T, 3, 3) float64, world space
    material: np.ndarray  # (T,) uint8
    bounds_min: np.ndarray  # (3,)
    bounds_max: np.ndarray  # (3,)
    stats: BoundStats
    source: str = ''

    @property
    def triangle_count(self):
        return int(self.triangles.shape[0])

    def scaled(self, scale):
        return self.triangles * scale


def _matrix_from_struct(struct):
    # c1..c3 basis, c4 translation
    m = np.eye(4, dtype=np.float64)
    m[:3, 0] = struct['c1']
    m[:3, 1] = struct['c2']
    m[:3, 2] = struct['c3']
    m[:3, 3] = struct['c4']
    return m


def _default_transform():
    return np.eye(4, dtype=np.float64)


class _Parser:
    def __init__(self, reader, source=''):
        self.r = reader
        self.source = source
        self.stats = BoundStats()
        self._triangles = []
        self._material = []
        self._min = np.array([np.inf] * 3)
        self._max = np.array([-np.inf] * 3)

    def parse(self, addr=BOUNDS_FILE_ROOT):
        self._read_bounds(addr, _default_transform())
        if not self._triangles:
            raise ResourceError('no collision triangles in this .ybn')
        triangles = np.concatenate(self._triangles, axis=0)
        material = np.concatenate(self._material, axis=0)
        return YbnWorld(
            triangles=triangles,
            material=material,
            bounds_min=self._min,
            bounds_max=self._max,
            stats=self.stats,
            source=self.source,
        )

    def _read_bounds(self, addr, parent):
        r = self.r
        typ = r.u8(addr + 0x10)
        self.stats.blocks += 1

        if typ == TYPE_COMPOSITE:
            self.stats.composites += 1
            self._read_composite(addr, parent)
        elif typ in (TYPE_GEOMETRY, TYPE_BVH):
            if typ == TYPE_BVH:
                self.stats.bvhs += 1
            else:
                self.stats.geometries += 1
            self._read_geometry(addr, parent)
        elif typ == TYPE_BOX:
            self.stats.boxes += 1
            self._read_box(addr, parent)
        else:
            name = TYPE_NAMES.get(typ, f'type{typ}')
            self.stats.skipped[name] = self.stats.skipped.get(name, 0) + 1

    def _read_composite(self, addr, parent):
        r = self.r
        children_ptr = r.u64(addr + 0x70)
        xform1_ptr = r.u64(addr + 0x78)
        xform2_ptr = r.u64(addr + 0x80)
        count = r.u16(addr + 0xA0)
        if not children_ptr or count == 0:
            return

        pointers = r.u64s(children_ptr, count)
        xform_ptr = xform1_ptr or xform2_ptr
        xforms = r.array(xform_ptr, MATRIX_DTYPE, count) if xform_ptr else None

        for i, child in enumerate(pointers):
            if not child:
                continue
            local = (_matrix_from_struct(xforms[i]) if xforms is not None
                     else _default_transform())
            self._read_bounds(int(child), parent @ local)

    def _read_geometry(self, addr, parent):
        r = self.r
        pages = r.u64(addr + 0x88)  # PolygonsPointer
        quantum = np.array(r.vec3(addr + 0x90), dtype=np.float64)
        center = np.array(r.vec3(addr + 0xA0), dtype=np.float64)
        verts_ptr = r.u64(addr + 0xB0)
        vert_count = r.u32(addr + 0xD0)
        poly_count = r.u32(addr + 0xD4)
        mats_ptr = r.u64(addr + 0xF0)
        mat_count_raw = r.u8(addr + 0x120)
        poly_mat_ptr = r.u64(addr + 0x118)

        if not pages or not verts_ptr or not vert_count or not poly_count:
            return

        raw = r.array(verts_ptr, VERTEX_DTYPE, vert_count)
        verts = np.stack([raw['x'], raw['y'], raw['z']], axis=1).astype(np.float64)
        verts = verts * quantum + center
        # apply composite once
        verts = verts @ parent[:3, :3].T + parent[:3, 3]

        polys = r.array(pages, POLYGON_DTYPE, poly_count)

        poly_type = (polys['area'] & 7).astype(np.uint8)

        # per polygon material index -> type byte
        mat_type = np.zeros(poly_count, dtype=np.uint8)
        if mats_ptr and mat_count_raw:
            mats = r.array(mats_ptr, '<u4', max(4, mat_count_raw) * 2)
            types = (mats[0::2] & 0xFF).astype(np.uint8)
            if poly_mat_ptr:
                poly_mat = r.array(poly_mat_ptr, 'u1', poly_count)
                valid = poly_mat < len(types)
                mat_type[valid] = types[poly_mat[valid]]

        tri_mask = poly_type == POLY_TRIANGLE
        if np.any(tri_mask):
            tri = polys[tri_mask]
            # top bit is not part of the index
            i1 = (tri['w1'] & 0x7FFF).astype(np.int64)
            i2 = (tri['w2'] & 0x7FFF).astype(np.int64)
            i3 = (tri['w3'] & 0x7FFF).astype(np.int64)
            ok = ((i1 < vert_count) & (i2 < vert_count) & (i3 < vert_count)
                  & (i1 >= 0) & (i2 >= 0) & (i3 >= 0))
            if not np.all(ok):
                i1, i2, i3 = i1[ok], i2[ok], i3[ok]
                tri_mat = mat_type[tri_mask][ok]
            else:
                tri_mat = mat_type[tri_mask]
            if len(i1):
                self._append(verts[i1], verts[i2], verts[i3], tri_mat)

        box_mask = poly_type == POLY_BOX
        if np.any(box_mask):
            boxes = polys[box_mask]
            box_mat = mat_type[box_mask]
            for row, mat in zip(boxes, box_mat):
                # 4 vertex indices
                idx = [int(row['w1']), int(row['w2']),
                       int(row['w3']), int(row['w4'])]
                if max(idx) >= vert_count or min(idx) < 0:
                    continue
                a, b, c, d = (verts[i] for i in idx)
                self._append(a[None], b[None], c[None], np.array([mat]))
                self._append(a[None], c[None], d[None], np.array([mat]))

        unsupported = int(poly_count - np.count_nonzero(tri_mask | box_mask))
        if unsupported:
            key = 'polygons'
            self.stats.skipped[key] = self.stats.skipped.get(key, 0) + unsupported

    def _read_box(self, addr, parent):
        r = self.r
        box_min = np.array(r.vec3(addr + 0x30), dtype=np.float64)
        box_max = np.array(r.vec3(addr + 0x20), dtype=np.float64)
        lo = np.minimum(box_min, box_max)
        hi = np.maximum(box_min, box_max)
        corners = np.array([(x, y, z) for x in (lo[0], hi[0])
                            for y in (lo[1], hi[1])
                            for z in (lo[2], hi[2])], dtype=np.float64)
        corners = corners @ parent[:3, :3].T + parent[:3, 3]
        # 12 tris over 8 corners, wound outward
        faces = [(0, 1, 3, 2), (4, 6, 7, 5), (0, 4, 5, 1),
                 (2, 3, 7, 6), (0, 2, 6, 4), (1, 5, 7, 3)]
        for a, b, c, d in faces:
            self._append(corners[[a]][None], corners[[b]][None],
                         corners[[c]][None], np.array([0], dtype=np.uint8))
            self._append(corners[[a]][None], corners[[c]][None],
                         corners[[d]][None], np.array([0], dtype=np.uint8))

    def _append(self, a, b, c, material):
        if not len(a):
            return
        tri = np.stack([a, b, c], axis=1)  # (N, 3, 3)
        self._triangles.append(tri)
        self._material.append(np.asarray(material, dtype=np.uint8))
        self._min = np.minimum(self._min, tri.reshape(-1, 3).min(axis=0))
        self._max = np.maximum(self._max, tri.reshape(-1, 3).max(axis=0))


def parse_ybn(data, system_size, source=''):
    from .resource import split_payload

    system, graphics = split_payload(data, system_size)
    reader = ResourceReader(system, graphics)
    return _Parser(reader, source).parse()


def ybn_bounds(data, system_size):
    from .resource import split_payload

    system, graphics = split_payload(data, system_size)
    reader = ResourceReader(system, graphics)
    root = 0x50000000
    return (np.array(reader.vec3(root + 0x30), dtype=np.float64),
            np.array(reader.vec3(root + 0x40), dtype=np.float64))
