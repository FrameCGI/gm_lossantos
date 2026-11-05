import json
import os
import struct
import sys
import tempfile

import numpy as np

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "..", "exporter"))
from gtav import world as gtav
from gtav.ybn import BoundStats, YbnWorld


def check(label, ok):
    print("%-74s %s" % (label, "OK" if ok else "<-- WRONG"))


def quad(x0, y0, x1, y1, z):
    return [
        [[x0, y0, z], [x1, y0, z], [x0, y1, z]],
        [[x1, y0, z], [x1, y1, z], [x0, y1, z]],
    ]


def grid_quads(x0, y0, x1, y1, step, z):
    out = []
    for y in np.arange(y0, y1, step):
        for x in np.arange(x0, x1, step):
            out += quad(x, y, x + step, y + step, z)
    return out


tris = np.array(grid_quads(0, 0, 200, 200, 25, 10)
                + grid_quads(0, 0, 100, 100, 25, 30), float)
world = YbnWorld(
    triangles=tris,
    material=np.ones(len(tris), np.uint8),
    bounds_min=tris.reshape(-1, 3).min(axis=0),
    bounds_max=tris.reshape(-1, 3).max(axis=0),
    stats=BoundStats(),
    source="synthetic",
)

P = gtav.PHYS_CHUNK_SIZE
T = gtav.TILE_SIZE

neg = np.array([[[-10, -10, 0], [-1, -10, 0], [-10, -1, 0]]], float)
groups = list(gtav.grid_groups(neg, P))
check("a negative triangle is in chunk -1 -1", len(groups) == 1 and groups[0][:2] == (-1, -1))
groups = list(gtav.grid_groups(np.array([[[0, 0, 0], [P, 0, 0], [0, P, 0]]], float), P))
check("a triangle on the grid line belongs to the cell it spans from",
      len(groups) == 1 and groups[0][:2] == (0, 0))

with tempfile.TemporaryDirectory() as tmp:
    report = gtav.export(world, tmp, "synthetic")
    root = os.path.join(tmp, "synthetic")
    index = json.load(open(os.path.join(root, "index.json")))
    tiles = sorted(index["tiles"])
    chunks = sorted(index["phys_chunks"])
    check("tiles cover the 200 m block", len(tiles) == 4)
    check("chunks cover the 200 m block", len(chunks) == 8 * 8)
    check("every LOD points at a real file",
          all(os.path.isfile(os.path.join(root, "tiles", v["lod%d" % lod]["file"]))
              for v in index["tiles"].values() for lod in range(5)))
    check("the reported spawn is in the open quadrant",
          report.spawn[0] > 100 * gtav.SCALE or report.spawn[1] > 100 * gtav.SCALE)
    check("the spawn is above the z=10 m floor, not the deck",
          abs(report.spawn[2] - (10 * gtav.SCALE + 128)) < 1e-6)

    path = os.path.join(root, "tiles", index["tiles"][tiles[0]]["lod0"]["file"])
    data = open(path, "rb").read()
    magic, ver, flags, vc, ic, sc, mc, stride, reserved = struct.unpack_from("<IHHIIIIII", data, 0)
    subs = [struct.unpack_from("<IIII", data, 32 + 16 * i) for i in range(sc)]
    mats = [data[32 + 16 * sc + 64 * i:32 + 16 * sc + 64 * (i + 1)].split(b"\0")[0]
            for i in range(mc)]
    blob = data[32 + 16 * sc + 64 * mc:]
    check("the tile header", magic == gtav.TILE_MAGIC and ver == 1 and stride == 28
          and flags == 3 and len(blob) == vc * 28 + ic * 2)
    check("... with a material table and submeshes", mc >= 1 and sc >= 1
          and all(s[0] < mc and s[1] + s[2] <= ic for s in subs))
    check("... material names are GTA surface ids",
          all(m.startswith(b"models/lossantos/gta_surface_") for m in mats))

    verts = np.frombuffer(blob[:vc * 28], dtype=np.uint8).reshape(vc, 28)
    pos = verts[:, :12].copy().view("<f4").reshape(vc, 3)
    nrm = verts[:, 24:27].astype(np.int8) / 127.0
    ind = np.frombuffer(blob[vc * 28:], "<u2").reshape(-1, 3)
    check("the indices are 0-based and in range", ind.max() < vc)
    rh = np.cross(pos[ind[:, 1]] - pos[ind[:, 0]], pos[ind[:, 2]] - pos[ind[:, 0]])
    rh /= (np.linalg.norm(rh, axis=1)[:, None] + 1e-9)
    outward = nrm[ind].mean(axis=1)
    dots = (rh * outward).sum(axis=1)
    check("every triangle is wound to match its stored normal",
          bool(np.all(dots < -0.9)))
    check("both sides are emitted (collision panels face either way)",
          bool((outward[:, 2] > 0.9).any() and (outward[:, 2] < -0.9).any()))
    check("the floor is at 10 m in Source units",
          abs(pos[:, 2].max() - 30 * gtav.SCALE) < 1 or abs(pos[:, 2].min() - 10 * gtav.SCALE) < 1)

    cpath = os.path.join(root, "phys", index["phys_chunks"][chunks[0]]["file"])
    cdata = open(cpath, "rb").read()
    cmagic, cver, cflags, tc, cvc, cres = struct.unpack_from("<IHHIII", cdata, 0)
    cverts = np.frombuffer(cdata, "<f4", cvc * 3, 20).reshape(cvc, 3)
    cind = np.frombuffer(cdata, "<u2", tc * 3, 20 + cvc * 12).reshape(-1, 3)
    check("the chunk header", cmagic == gtav.CHUNK_MAGIC and cver == 1
          and len(cdata) == 20 + cvc * 12 + tc * 6)
    check("... with a valid index buffer", cind.max() < cvc and tc > 0)
    check("... and absolute positions", cverts[:, 2].min() > 0)

    check("chunks stay well under the uint16 vertex limit",
          all(struct.unpack_from("<I", open(os.path.join(root, "phys", v["file"]), "rb").read(), 12)[0]
              <= gtav.MAX_CHUNK_VERTS for v in index["phys_chunks"].values()))

two = np.array([[[0, 0, 0], [10, 0, 0], [0, 10, 0]],
                [[500, 500, 0], [510, 500, 0], [500, 510, 0]]], float)
w2 = YbnWorld(
    triangles=two,
    material=np.array([1, 2], np.uint8),
    bounds_min=two.reshape(-1, 3).min(axis=0),
    bounds_max=two.reshape(-1, 3).max(axis=0),
    stats=BoundStats(),
    source="two",
)
clipped = gtav.clip_world(w2, np.array([0.0, 0.0, 0.0]),
                        np.array([100.0, 100.0, 10.0]), margin=0)
check("clip keeps only the triangle inside the box",
      clipped is not None and clipped.triangle_count == 1
      and clipped.source == "two")
merged = gtav.combine([w2, clipped])
check("combine concatenates triangle soups", merged.triangle_count == 3)

cat_tris = np.array([
    [[0, 0, 0], [10, 0, 0], [0, 10, 0]],  # floor, road id
    [[0, 0, 0], [0, 10, 0], [0, 0, 10]],  # wall, window id
    [[0, 0, 0], [10, 0, 0], [0, 0, 10]],  # wall, concrete
    [[0, 0, 0], [10, 0, 0], [0, 10, 0]],  # floor, planting
], float)
cat_normals, _ = gtav.triangle_normals(cat_tris)
cat = gtav.assign_categories(cat_tris, cat_normals, np.array([4, 57, 1, 174], np.uint8))
check("collision ids pick road / windows / wall / grass",
      cat.tolist() == [gtav.CAT_ROAD, gtav.CAT_WINDOWS, gtav.CAT_WALL, gtav.CAT_GRASS])

with tempfile.TemporaryDirectory() as tmp:
    gtav.export(world, tmp, "textured", texture_prefix="models/gtav/dt1_02")
    index = json.load(open(os.path.join(tmp, "textured", "index.json")))
    check("a textured index lists the eight categories",
          len(index["materials"]) == 8
          and index["materials"]["6"] == "models/gtav/dt1_02/im_sidewalk010"
          and index["materials"]["7"] == "models/gtav/dt1_02/im_roof_underside003")
    tile = index["tiles"][sorted(index["tiles"])[0]]["lod0"]["file"]
    data = open(os.path.join(tmp, "textured", "tiles", tile), "rb").read()
    _, _ver, _flags, _vc, _ic, _sc, mc, _stride, _res = struct.unpack_from(
        "<IHHIIIIII", data, 0)
    names = [data[32 + 16 * _sc + 64 * i:32 + 16 * _sc + 64 * (i + 1)]
             .split(b"\0")[0].decode() for i in range(mc)]
    check("the tile references the VMT names, not ids",
          len(names) == 8 and all(n.startswith("models/gtav/dt1_02/") for n in names))
    check("the index carries a spawn yaw", len(index.get("spawn_angles", [])) == 3)

with tempfile.TemporaryDirectory() as tmp:
    gtav.export(world, tmp, "bh", texture_prefix="bh1_17")
    index = json.load(open(os.path.join(tmp, "bh", "index.json")))
    check("a named palette mixes full material paths",
          index["materials"]["0"] == "models/gtav/bh1_17/im_road_027b"
          and index["materials"]["1"] == "models/gtav/bh1_17/dc_sidewalk004"
          and index["materials"]["5"]
          == "models/gtav/bh1_06/bh1_06_rsn_os_marblewall_0003")

from gtav.ydr import YdrDrawable, YdrGeometry

geom = YdrGeometry(
    positions=np.array([[120, 120, 12], [130, 120, 12], [120, 130, 12]], float),
    uvs=np.array([[0.1, 0.2], [0.9, 0.2], [0.1, 0.8]], float),
    normals=np.array([[0, 0, 1], [0, 0, 1], [0, 0, 1]], float),
    indices=np.array([[0, 1, 2]], np.int64),
    shader=0, texture="dummy",
)
drawable = YdrDrawable(
    bounds_min=np.array([120.0, 120.0, 10.0]),
    bounds_max=np.array([170.0, 170.0, 30.0]),
    geometries=[geom], source="synthetic.ydr",
)
visual = gtav.build_ydr_visual(
    world, [drawable], {"dummy": "models/test/dummy_wall"},
    palette_prefix="models/gtav/dt1_02", log=lambda _text: None)
check("a drawable is appended to the collision ground",
      len(visual["triangles"]) == 160 + 1
      and "models/test/dummy_wall" in visual["names"])
check("the drawable keeps its authored UVs",
      np.allclose(visual["uvs"][-1], geom.uvs))

with tempfile.TemporaryDirectory() as tmp:
    gtav.export(world, tmp, "visual", visual=visual, double_sided=False)
    index = json.load(open(os.path.join(tmp, "visual", "index.json")))
    tile = index["tiles"]["1 1"]["lod0"]["file"]
    data = open(os.path.join(tmp, "visual", "tiles", tile), "rb").read()
    _, _v, _f, vc, _ic, _sc, _mc, _stride, _r = struct.unpack_from("<IHHIIIIII", data, 0)
    verts = np.frombuffer(
        data[32 + 16 * _sc + 64 * _mc:32 + 16 * _sc + 64 * _mc + vc * 28],
        np.uint8).reshape(vc, 28)
    uv = verts[:, 12:20].copy().view("<f4").reshape(vc, 2)
    check("authored UVs reach the GVT1 file",
          any(abs(u - 0.1) < 1e-4 and abs(v - 0.2) < 1e-4 for u, v in uv))

from gtav.ymap import YmapEntity, transform_drawable

placed = transform_drawable(
    drawable,
    YmapEntity(archetype=1, position=np.array([10.0, 20.0, 30.0]),
               rotation=(0.0, 0.0, 0.0, 1.0), scale_xy=1.0, scale_z=1.0,
               flags=0, lod_level=0),
)
check("an entity transform offsets the drawable",
      np.allclose(placed.geometries[0].positions[0], [130, 140, 42])
      and np.allclose(placed.bounds_min, [130, 140, 40]))
turn = np.sqrt(0.5)
rotated = transform_drawable(
    drawable,
    YmapEntity(archetype=1, position=np.array([0.0, 0.0, 0.0]),
               rotation=(0.0, 0.0, turn, turn), scale_xy=1.0, scale_z=1.0,
               flags=0, lod_level=0),
)
check("a yaw quaternion rotates the drawable",
      np.allclose(rotated.geometries[0].positions[0], [-120, 120, 12], atol=1e-6))
from gtav.materials import facade_alpha, install_textures
from gtav.ytd import GtaTexture

sample = np.zeros((2, 2, 4), np.uint8)
sample[:, :, 3] = 255
sample[0, 0, :3] = (20, 25, 30)  # dark blue pane
sample[0, 1, :3] = (200, 190, 170)  # limestone
sample[1, 0, :3] = (60, 40, 30)  # dark brown mullion
sample[1, 1, :3] = (140, 40, 50)  # red sign band
mask = facade_alpha(sample)
check("the facade mask is per texel",
      mask[0, 0] == 100 and mask[0, 1] == 255
      and mask[1, 0] == 255 and mask[1, 1] == 255)

marking = GtaTexture(name="im_roadmarkings05im_roadmarkings05_a", width=4, height=4,
                     depth=1, stride=8, format=0, levels=1, data=b"")
with tempfile.TemporaryDirectory() as tmp:
    installed, skipped = install_textures(
        [("globalroads+hi.ytd", marking)], tmp, "models/test", log=lambda _text: None)
    check("road-marking dither masks are dropped",
          installed[marking.name] is False and not skipped
          and not os.path.exists(os.path.join(tmp, "models", "test",
                                              marking.name + ".vmt")))

from gtav.materials import detail_rgba, foliage_rgba

stipple = np.zeros((8, 8, 4), np.uint8)
stipple[:, :, :3] = (30, 40, 60)
stipple[::2, ::2, :3] = (220, 180, 20)
stipple[::2, ::2, 3] = 255
stipple[1::2, 1::2, 3] = 255
detail = detail_rgba(stipple)
check("a stipple detail keeps a hard alpha cutout",
      set(np.unique(detail[:, :, 3]).tolist()) <= {0, 255})
check("a stipple detail loses its confetti colours",
      np.unique(detail[:, :, 0]).size < 16)

leaves = np.zeros((8, 8, 4), np.uint8)
leaves[:, :, :3] = (180, 20, 180)
leaves[2:6, 2:6, 3] = 255
foliage = foliage_rgba(leaves)
check("foliage is rebuilt green with a cutout",
      foliage[4, 4, 1] > foliage[4, 4, 0]
      and foliage[4, 4, 1] > foliage[4, 4, 2]
      and set(np.unique(foliage[:, :, 3]).tolist()) <= {0, 255})
