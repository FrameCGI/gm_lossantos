import json
import os
import sys
import tempfile
import time

import numpy as np

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "..", "exporter"))
from gtav import world as gtav
from gtav.ybn import BoundStats, YbnWorld
from gtav.ydr import YdrDrawable, YdrGeometry

S = gtav.SCALE
FAILED = []


def check(label, ok):
    print("%-74s %s" % (label, "OK" if ok else "<-- WRONG"))
    if not ok:
        FAILED.append(label)


def floor_quads(x0, y0, x1, y1, step, z, up=True):
    out = []
    for y in np.arange(y0, y1 - 1e-9, step):
        for x in np.arange(x0, x1 - 1e-9, step):
            a, b, c, d = [x, y, z], [x + step, y, z], [x, y + step, z], [x + step, y + step, z]
            if up:
                out += [[a, b, c], [b, d, c]]
            else:
                out += [[a, c, b], [b, c, d]]
    return out


def wall_quads_x(x, y0, y1, z0, z1, step):
    out = []
    for z in np.arange(z0, z1 - 1e-9, step):
        for y in np.arange(y0, y1 - 1e-9, step):
            a, b, c, d = [x, y, z], [x, y + step, z], [x, y, z + step], [x, y + step, z + step]
            out += [[a, c, b], [b, c, d]]
    return out


def mk_world(tris):
    tris = np.asarray(tris, float)
    return YbnWorld(triangles=tris, material=np.ones(len(tris), np.uint8),
                    bounds_min=tris.reshape(-1, 3).min(axis=0),
                    bounds_max=tris.reshape(-1, 3).max(axis=0),
                    stats=BoundStats(), source="synthetic")


def mk_drawable(tris, texture, source="synthetic.ydr", bmin=None, bmax=None):
    tris = np.asarray(tris, float)
    pos = tris.reshape(-1, 3)
    geom = YdrGeometry(positions=pos, uvs=np.zeros((len(pos), 2)), normals=None,
                       indices=np.arange(len(pos), dtype=np.int64).reshape(-1, 3),
                       shader=0, texture=texture)
    return YdrDrawable(bounds_min=np.asarray(bmin if bmin is not None else pos.min(axis=0), float),
                       bounds_max=np.asarray(bmax if bmax is not None else pos.max(axis=0), float),
                       geometries=[geom], source=source)


MATS = {"road": "models/test/road", "wall": "models/test/wall",
        "shop_glass_01": "models/test/glass", "decal": False}


def run(world_tris, drawables, mats=MATS):
    lines = []
    visual = gtav.build_ydr_visual(mk_world(world_tris), drawables, mats,
                                 palette_prefix="models/gtav/dt1_02",
                                 log=lines.append)
    n_draw = sum(d.triangle_count for d in drawables
                 for g in d.geometries if mats.get((g.texture or "").lower()) is not False)
    return len(visual["triangles"]) - n_draw, lines, visual


def covered(world_tris, drawables, mats=MATS):
    return gtav._filler_covered(np.asarray(world_tris, float) * S, drawables, mats, S)


coll = floor_quads(0, 0, 10, 10, 2, 0.0)
road = mk_drawable(floor_quads(0, 0, 10, 10, 1, 0.03), "road", "x_road_sect.ydr")
n, lines, _ = run(coll, [road])
check("filler 3 cm under an authored road is dropped", n == 0)
check("... and one filler summary line is logged",
      sum(l.strip().startswith("filler:") for l in lines) == 1)
print("     " + [l for l in lines if "filler:" in l][0].strip())

coll = floor_quads(0, 0, 20, 10, 2, 0.0)
road = mk_drawable(floor_quads(0, 0, 10, 10, 1, 0.03), "road", "x_road_sect.ydr")
cov = covered(coll, [road])
cx = np.asarray(coll, float).mean(axis=1)[:, 0]
check("filler under the road part of a gapped mesh is covered", bool(cov[cx < 10].all()))
check("filler in the gap (> 2 m from the art) is kept", not cov[cx > 12].any())
n, _, _ = run(coll, [road])
check("... and reaches the visual", n == int((~cov).sum()) and n >= int((cx > 12).sum()))

coll = wall_quads_x(5.0, 0, 10, 0, 1.8, 0.9)
wall = mk_drawable(wall_quads_x(4.97, 0, 10, 0, 10, 1.0), "wall", "x_build.ydr")
n, _, _ = run(coll, [wall])
check("low collision wall 3 cm behind an authored wall is dropped", n == 0)

coll = floor_quads(4, 0, 6, 10, 1, 0.0)
cov = covered(coll, [wall])
check("floor next to/under a perpendicular wall is NOT covered", not cov.any())
n, _, _ = run(coll, [wall])
check("... and all of it stays in the visual", n == len(coll))

coll = floor_quads(0, 0, 10, 10, 2, 0.0)
glass = mk_drawable(floor_quads(0, 0, 10, 10, 1, 0.03), "shop_glass_01", "x_road_sect.ydr")
n, _, _ = run(coll, [glass])
check("glass-only art over filler does not drop it", n == len(coll))

decal = mk_drawable(floor_quads(0, 0, 10, 10, 1, 0.03), "decal", "x_road_sect.ydr")
n, _, _ = run(coll, [decal])
check("art the visual drops (material False) does not drop filler", n == len(coll))

under = mk_drawable(floor_quads(0, 0, 10, 10, 1, 0.03, up=False), "road", "x_road_sect.ydr")
check("art facing the other way does not cover filler",
      not covered(coll, [under]).any())

deck = mk_drawable(floor_quads(0, 0, 10, 10, 1, 2.0), "road", "x_road_sect.ydr")
check("art 2 m above the filler does not cover it", not covered(coll, [deck]).any())

t = np.radians(15.0)
ramp = np.asarray(floor_quads(0, 0, 10, 10, 2, 0.0), float)
ramp_art = np.asarray(floor_quads(0, 0, 10, 10, 1, 0.03), float)
for a in (ramp, ramp_art):
    x, z = a[..., 0].copy(), a[..., 2].copy()
    a[..., 0], a[..., 2] = x * np.cos(t) - z * np.sin(t), x * np.sin(t) + z * np.cos(t)
check("a sloped road's own art covers its sloped filler",
      bool(covered(ramp, [mk_drawable(ramp_art, "road")]).all()))

coll = floor_quads(0, 0, 20, 10, 2, 0.0)
road = mk_drawable(floor_quads(0, 0, 10, 10, 1, 0.03), "road", "x_road_sect.ydr")
n, _, visual = run(coll, [road])
with tempfile.TemporaryDirectory() as tmp:
    a = gtav.export(mk_world(coll), tmp, "plain", log=lambda _t: None)
    b = gtav.export(mk_world(coll), tmp, "vis", visual=visual, double_sided=False,
                  log=lambda _t: None)
    same = a.chunk_triangles == b.chunk_triangles == len(coll)
    pa, pb = (json.load(open(os.path.join(tmp, m, "index.json")))["phys_chunks"]
              for m in ("plain", "vis"))
    files_same = sorted(pa) == sorted(pb) and all(
        open(os.path.join(tmp, "plain", "phys", pa[k]["file"]), "rb").read()
        == open(os.path.join(tmp, "vis", "phys", pb[k]["file"]), "rb").read() for k in pa)
check("physics chunks keep every collision triangle", same and n < len(coll))
check("... byte-identical to an export without the visual", files_same)

if "--perf" in sys.argv:
    rng = np.random.default_rng(1)

    def soup(nx, ny, step, z, jitter):
        xs, ys = np.meshgrid(np.arange(nx) * step, np.arange(ny) * step, indexing="ij")
        x, y = xs.ravel(), ys.ravel()
        zz = z + 0.02 * np.sin(x * 0.05) + jitter
        a = np.stack([x, y, zz], 1)
        b = np.stack([x + step, y, zz], 1)
        c = np.stack([x, y + step, zz], 1)
        d = np.stack([x + step, y + step, zz], 1)
        return np.concatenate([np.stack([a, b, c], 1), np.stack([b, d, c], 1)])

    coll = soup(317, 317, 1.6, 0.0, 0.0)  # ~200k tris, 507 m
    art = soup(388, 388, 1.0, 0.03, 0.0)  # ~300k tris, 388 m
    pos = art.reshape(-1, 3)
    geom = YdrGeometry(positions=pos, uvs=np.zeros((len(pos), 2)), normals=None,
                       indices=np.arange(len(pos), dtype=np.int64).reshape(-1, 3),
                       shader=0, texture="road")
    big = YdrDrawable(bounds_min=pos.min(0), bounds_max=pos.max(0),
                      geometries=[geom], source="perf_road_sect.ydr")
    t0 = time.perf_counter()
    cov = gtav._filler_covered(coll * S, [big], MATS, S)
    t1 = time.perf_counter()
    print("perf: _filler_covered %d coll + %d art tris: %.2f s, %d covered"
          % (len(coll), len(art), t1 - t0, int(cov.sum())))
    t0 = time.perf_counter()
    lines = []
    gtav.build_ydr_visual(mk_world(coll), [big], MATS,
                        palette_prefix="models/gtav/dt1_02", log=lines.append)
    print("perf: build_ydr_visual total: %.2f s" % (time.perf_counter() - t0))
    for l in lines:
        print("     " + l.strip())

print("ALL OK" if not FAILED else "FAILED: %d check(s)" % len(FAILED))
sys.exit(1 if FAILED else 0)
