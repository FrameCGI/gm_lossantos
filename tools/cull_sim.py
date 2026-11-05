"""Offline replica of the lossantos draw cull - decide cuts before shipping them."""

from __future__ import annotations

import argparse
import csv
import json
import math
import struct
from pathlib import Path

GMOD = Path(r"C:\Program Files (x86)\Steam\steamapps\common\GarrysMod")
ROOT = GMOD / "garrysmod" / "data" / "lossantos" / "lossantos"

LOD_DISTANCES = {0: 4096, 1: 16384, 2: 32768, 3: 65536, 4: 160000}

Z_FALLBACK = 128000  # tileInView's box Z half-extent without bounds
FOV = 90  # fov_desired default
DEFAULT_STREAM_RADIUS = 90000


def load_index():
    with open(ROOT / "index.json", encoding="utf-8") as fh:
        idx = json.load(fh)
    with open(ROOT / "tile_bounds.json", encoding="utf-8") as fh:
        zb = json.load(fh)["bounds"]
    return idx, zb


def build_groups(idx, zb, grid):
    span = grid * 1024
    groups = {}
    for key in idx["tiles"]:
        tx, ty = (int(v) for v in key.split())
        g = groups.setdefault(
            (tx // grid, ty // grid),
            {"tx": (tx // grid) * grid, "ty": (ty // grid) * grid,
             "fp": span, "z0": None, "z1": None},
        )
        b = zb.get(f"{tx} {ty}")
        if b:
            z0, z1 = float(b[0]), float(b[1])
            g["z0"] = z0 if g["z0"] is None else min(g["z0"], z0)
            g["z1"] = z1 if g["z1"] is None else max(g["z1"], z1)
    return list(groups.values())


def near_dist2(eye, t):
    ex, ey, ez = eye
    x0, y0 = t["tx"] * 1024, t["ty"] * 1024
    x1, y1 = x0 + t["fp"], y0 + t["fp"]
    dx = max(x0 - ex, 0.0, ex - x1)
    dy = max(y0 - ey, 0.0, ey - y1)
    z0, z1 = t["z0"], t["z1"]
    dz = 0.0
    if z0 is not None:
        if ez < z0:
            dz = z0 - ez
        elif z1 is not None and ez > z1:
            dz = ez - z1
    return dx * dx + dy * dy + dz * dz


def desired_lod(dist, bias=0.0):
    lod = 4
    for i in range(0, 5):
        if dist <= LOD_DISTANCES[i]:
            lod = i
            break
    return max(0, min(4, math.floor(lod + bias + 0.5)))


def cull_dist2(t, lod):
    maxd = LOD_DISTANCES[min(lod + 1, 4)] + t["fp"]
    return maxd * maxd


def frustum_planes(eye, pitch, yaw, th, tv):
    cy, sy = math.cos(yaw), math.sin(yaw)
    cp, sp = math.cos(pitch), math.sin(pitch)
    f = (cy * cp, sy * cp, -sp)
    r = (sy, -cy, 0.0)
    u = (sp * cy, sp * sy, cp)

    def cross(a, b):
        return (a[1] * b[2] - a[2] * b[1],
                a[2] * b[0] - a[0] * b[2],
                a[0] * b[1] - a[1] * b[0])

    def add(n):
        if n[0] * f[0] + n[1] * f[1] + n[2] * f[2] > 0:
            n = (-n[0], -n[1], -n[2])
        return n

    f_r_th = (f[0] + r[0] * th, f[1] + r[1] * th, f[2] + r[2] * th)
    f_r_nth = (f[0] - r[0] * th, f[1] - r[1] * th, f[2] - r[2] * th)
    f_u_tv = (f[0] + u[0] * tv, f[1] + u[1] * tv, f[2] + u[2] * tv)
    f_u_ntv = (f[0] - u[0] * tv, f[1] - u[1] * tv, f[2] - u[2] * tv)
    return [add(cross(u, f_r_th)), add(cross(u, f_r_nth)),
            add(cross(r, f_u_tv)), add(cross(r, f_u_ntv))], f


def box_outside(t, planes, eye):
    ex, ey, ez = eye
    x0, y0 = t["tx"] * 1024, t["ty"] * 1024
    x1, y1 = x0 + t["fp"], y0 + t["fp"]
    if t["z0"] is None:
        z0, z1 = -Z_FALLBACK, Z_FALLBACK
    else:
        z0, z1 = t["z0"], t["z1"] if t["z1"] is not None else Z_FALLBACK
    for n in planes:
        mnx = x0 if n[0] >= 0 else x1
        mny = y0 if n[1] >= 0 else y1
        mnz = z0 if n[2] >= 0 else z1
        if n[0] * (mnx - ex) + n[1] * (mny - ey) + n[2] * (mnz - ez) > 0:
            return True
    return False


def min_view_z(t, fwd, eye):
    ex, ey, ez = eye
    x0, y0 = t["tx"] * 1024, t["ty"] * 1024
    x1, y1 = x0 + t["fp"], y0 + t["fp"]
    if t["z0"] is None:
        z0, z1 = -Z_FALLBACK, Z_FALLBACK
    else:
        z0, z1 = t["z0"], t["z1"] if t["z1"] is not None else Z_FALLBACK
    s = 0.0
    for c0, c1, e, fc in ((x0, x1, ex, fwd[0]), (y0, y1, ey, fwd[1]),
                          (z0, z1, ez, fwd[2])):
        s += (c0 - e) * fc if fc >= 0 else (c1 - e) * fc
    return s


def png_aspect(run: Path):
    for png in sorted(run.glob("wp_*.png")):
        with open(png, "rb") as fh:
            head = fh.read(24)
        if head[:8] == b"\x89PNG\r\n\x1a\n":
            w, h = struct.unpack(">II", head[16:24])
            return w / max(1, h)
    return 16 / 9


def load_cams(run: Path):
    with open(run / "stats.csv", encoding="utf-8") as fh:
        rows = list(csv.DictReader(fh))
    cams = []
    for r in rows:
        cams.append({
            "wp": int(float(r["wp"])),
            "eye": (float(r["x"]), float(r["y"]), float(r["z"])),
            "pitch": math.radians(float(r["eyePitch"])),
            "yaw": math.radians(float(r["eyeYaw"])),
            "active": int(float(r["active"])),
            "drawn": int(float(r["drawn"])),
        })
    return cams


def fov_half_angles(aspect, fov=FOV):
    tan_half = math.tan(math.radians(fov) * 0.5)
    h_half = math.atan(max(tan_half, tan_half * 0.75 * aspect))
    v_half = math.atan(max(tan_half / aspect, tan_half * 0.75))
    return h_half, v_half


def variant_planes(cam, aspect, mh, mv, fov=FOV):
    h_half, v_half = fov_half_angles(aspect, fov)
    th = math.tan(h_half + mh)
    tv = math.tan(v_half + mv)
    return frustum_planes(cam["eye"], cam["pitch"], cam["yaw"], th, tv)


VARIANTS = {
    "base":      (0.14, 0.20, 8, None),
    "tight":     (0.035, 0.05, 8, None),
    "exact":     (0.0, 0.0, 8, None),
    "fog88k":    (0.035, 0.05, 8, 88000.0),
    "fog60k":    (0.035, 0.05, 8, 60000.0),
    "fog45k":    (0.035, 0.05, 8, 45000.0),
    "fog35k":    (0.035, 0.05, 8, 35000.0),
    "grid16":    (0.14, 0.20, 16, None),
    "grid16tight": (0.035, 0.05, 16, None),
    "grid16tightfog88k": (0.035, 0.05, 16, 88000.0),
    "tightfog60k": (0.035, 0.05, 8, 60000.0),
}


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--run", required=True, help="selftest run dir (stats.csv)")
    ap.add_argument("--variants", default="base,tight,fog88k,fog45k,grid16tight",
                    help="comma list of " + ",".join(VARIANTS))
    ap.add_argument("--fov", type=float, default=FOV)
    args = ap.parse_args()

    run = Path(args.run)
    names = [v.strip() for v in args.variants.split(",") if v.strip()]
    for n in names:
        if n not in VARIANTS:
            raise SystemExit(f"unknown variant {n!r}; known: {','.join(VARIANTS)}")

    idx, zb = load_index()
    cams = load_cams(run)
    aspect = png_aspect(run)
    radius = float((idx.get("config", {}) or {}).get("stream_radius")
                   or DEFAULT_STREAM_RADIUS)

    grids = sorted({VARIANTS[n][2] for n in names} | {8})
    groups_by_grid = {g: build_groups(idx, zb, g) for g in grids}

    wanted_cache = {}

    def wanted(grid, cam):
        key = (grid, cam["wp"])
        if key not in wanted_cache:
            gs = groups_by_grid[grid]
            wanted_cache[key] = [
                t for t in gs if near_dist2(cam["eye"], t) <= radius * radius]
        return wanted_cache[key]

    resident = {}  # wp -> set of group ids for grid 8 (report only)
    acc = set()
    for cam in cams:
        for t in wanted(8, cam):
            acc.add((t["tx"], t["ty"]))
        resident[cam["wp"]] = set(acc)

    print(f"run {run.name}  aspect {aspect:.3f}  fov {args.fov:g}  "
          f"stream radius {radius:.0f}")
    print(f"groups: " + "  ".join(
        f"grid{g}={len(groups_by_grid[g])}" for g in grids))

    hdr = (f"{'wp':>3} {'measDrawn':>9} {'density':>7} "
           + " ".join(f"{n:>11}" for n in names))
    print(hdr)
    print("-" * len(hdr))

    for cam in cams:
        res8 = [t for t in groups_by_grid[8]
                if (t["tx"], t["ty"]) in resident[cam["wp"]]]
        base_planes, fwd = variant_planes(cam, aspect, *VARIANTS["base"][:2],
                                          fov=args.fov)

        def passes(t, planes, mh_mv_grid_fog):
            _, _, _, fogz = mh_mv_grid_fog
            if box_outside(t, planes, cam["eye"]):
                return False
            if fogz is not None and min_view_z(t, fwd, cam["eye"]) > fogz:
                return False
            d2 = near_dist2(cam["eye"], t)
            lod = desired_lod(math.sqrt(d2))
            return d2 <= cull_dist2(t, lod)

        base_groups = sum(1 for t in res8 if passes(t, base_planes,
                                                    VARIANTS["base"]))
        density = cam["drawn"] / base_groups if base_groups else 0.0

        cells = []
        for n in names:
            mh, mv, grid, fogz = VARIANTS[n]
            planes, _ = variant_planes(cam, aspect, mh, mv, fov=args.fov)
            src = res8
            if grid != 8:
                acc16, src = set(), []
                for c2 in cams:
                    if c2["wp"] > cam["wp"]:
                        break
                    for t in wanted(grid, c2):
                        key = (t["tx"], t["ty"])
                        if key not in acc16:
                            acc16.add(key)
                            src.append(t)
            ng = sum(1 for t in src if passes(t, planes, (mh, mv, grid, fogz)))
            cells.append(f"{ng:>5}/{int(ng * density):>5}")

        print(f"{cam['wp']:>3} {cam['drawn']:>9} {density:>7.1f} "
              + " ".join(f"{c:>11}" for c in cells))

    print()
    print("cells: groups/est-draws   (est-draws = groups x calibrated density)")
    if any(VARIANTS[n][2] != 8 for n in names):
        print("note: grid16 est-draws use grid8 density and OVERSTATE the cut -"
              " a grid16 group holds more submeshes; trust its group count")
    print(f"measured active vs resident model (grid8, accumulated): "
          + "  ".join(f"wp{c['wp']} {c['active']}/{len(resident[c['wp']])}"
                      for c in cams))


if __name__ == "__main__":
    main()
