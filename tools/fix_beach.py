import argparse
import csv
import os
import struct
import sys
import time
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from exporter.gtav.lod import read_tile, write_tile

TILE_MAGIC = 0x31545647
TILE_UNITS = 1024.0
UNITS_PER_M = 39.37

SAND_MAT = "models/gtav/vb_36/im_ground_sand_01"

def is_beach_pal(name):
    low = name.lower()
    return "beach" in low and low.endswith("_pal")


SAND_ART = {
    "models/gtav/bh1_06/bh1_06_rsn_os_marblewall_0003",
    "models/gtav/vb_36/vb_36_beach1_pal",
    "models/gtav/vb_36/vb_36_beach7_pal",
    "models/gtav/vb_36/vb_36_beach370_pal",
    "models/gtav/vb_36/vb_36_beach371_pal",
}
SAND_AMBIG = {
    "models/gtav/dt1_02/im_dt1_02_wall_01",
    "models/gtav/dt1_02/im_dt1_02_wall_02",
}
SAND_SOURCES = SAND_ART | SAND_AMBIG


def is_sand_source(name):
    return name in SAND_SOURCES or is_beach_pal(name)

MARKER_WORDS = ("beach", "sand", "waterstain", "seaweed", "uwrock",
                "erodedbank", "breaker")


def is_marker_name(name):
    low = name.lower()
    if any(w in low for w in MARKER_WORDS):
        return True
    return "surf" in low and "surface" not in low


HEIGHT_EXCLUDE = ("beachconcrete", "beach_concrete", "surfboard")  # made city fill look like beach


def is_height_marker_name(name):
    low = name.lower()
    return is_marker_name(name) and not any(k in low for k in HEIGHT_EXCLUDE)


def read_names(path):
    with open(path, "rb") as f:
        head = f.read(32)  # 32 byte tile header
        magic, ver, flags, vc, ic, sc, mc, stride, res = struct.unpack_from(
            "<IHHIIIIII", head, 0)
        if magic != TILE_MAGIC:
            raise ValueError(f"bad tile {path}")
        f.seek(32 + 16 * sc)
        blob = f.read(64 * mc)
    return [blob[64 * i:64 * (i + 1)].split(b"\0")[0].decode()
            for i in range(mc)]


def pos_keys(tri_pos):
    c = np.round(tri_pos, 2)
    order = np.lexsort((c[:, :, 2], c[:, :, 1], c[:, :, 0]), axis=1)
    ps = np.take_along_axis(c, order[:, :, None], axis=1).reshape(len(c), -1)
    return ps.astype(np.int64)


def fill_uv_mask(tri_pos, tri_uv, tri_nrm):
    tn = tri_nrm.mean(axis=1)
    pick = np.argmax(np.abs(tn), axis=1)
    cu = np.where(pick[:, None] == 0, tri_pos[:, :, 1], tri_pos[:, :, 0])
    cv = np.where(pick[:, None] == 2, tri_pos[:, :, 1], tri_pos[:, :, 2])
    u = tri_uv[:, :, 0]
    v = tri_uv[:, :, 1]
    num = (cu * u + cv * v).sum(axis=1)
    den = (u * u + v * v).sum(axis=1)
    ok = den > 1e-12
    r = np.where(ok, num / np.maximum(den, 1e-30), 0.0)
    res = np.maximum(np.abs(u * r[:, None] - cu).max(axis=1),
                     np.abs(v * r[:, None] - cv).max(axis=1))
    tol = np.maximum(1e-3, 1e-4 * np.abs(r))
    return ok & (res < tol)


class BeachBand:

    def __init__(self, gx0, gy0, nx, ny, cell, radius_units, zradius_units,
                 margin, margin_tight):
        self.gx0, self.gy0, self.nx, self.ny = gx0, gy0, nx, ny
        self.cell = float(cell)
        self.radius_units = radius_units
        self.zradius_units = zradius_units
        self.rc = int(np.ceil(radius_units / cell))
        self.rc_z = int(np.ceil(zradius_units / cell))
        self.margin = margin
        self.margin_tight = margin_tight
        self.cnt = np.zeros((ny, nx), np.int64)
        self.hcnt = np.zeros((ny, nx), np.int64)
        self.zsum = np.zeros((ny, nx), np.float64)

    def add(self, x0, y0, x1, y1, z, height=True):
        cx0 = np.floor(x0 / self.cell).astype(np.int64) - self.gx0
        cx1 = np.floor(x1 / self.cell).astype(np.int64) - self.gx0
        cy0 = np.floor(y0 / self.cell).astype(np.int64) - self.gy0
        cy1 = np.floor(y1 / self.cell).astype(np.int64) - self.gy0
        inside = ((cx1 >= 0) & (cx0 < self.nx) & (cy1 >= 0) & (cy0 < self.ny))
        if not inside.any():
            return
        cx0 = np.clip(cx0[inside], 0, self.nx - 1)
        cx1 = np.clip(cx1[inside], 0, self.nx - 1)
        cy0 = np.clip(cy0[inside], 0, self.ny - 1)
        cy1 = np.clip(cy1[inside], 0, self.ny - 1)
        z = z[inside]
        hmask = (z > 0) if height else np.zeros(z.shape, bool)
        dx = cx1 - cx0
        dy = cy1 - cy0
        big = (dx > 7) | (dy > 7)
        if big.any():
            for fx, fy in ((0.0, 0.0), (1.0, 0.0), (0.0, 1.0), (1.0, 1.0),
                           (0.5, 0.5)):
                sx = np.clip((cx0 + fx * (cx1 - cx0)).astype(np.int64),
                             0, self.nx - 1)
                sy = np.clip((cy0 + fy * (cy1 - cy0)).astype(np.int64),
                             0, self.ny - 1)
                np.add.at(self.cnt, (sy[big], sx[big]), 1)
                bh = big & hmask
                if bh.any():
                    np.add.at(self.hcnt, (sy[bh], sx[bh]), 1)
                    np.add.at(self.zsum, (sy[bh], sx[bh]), z[bh])
        small = ~big
        for ddx in range(8):
            for ddy in range(8):
                sel = small & (dx == ddx) & (dy == ddy)
                if not sel.any():
                    continue
                n = int(sel.sum())
                X = cx0[sel][:, None, None] + np.arange(ddx + 1)[None, :, None]
                Y = cy0[sel][:, None, None] + np.arange(ddy + 1)[None, None, :]
                X = np.broadcast_to(X, (n, ddx + 1, ddy + 1))
                Y = np.broadcast_to(Y, (n, ddx + 1, ddy + 1))
                Z = np.broadcast_to(z[sel][:, None, None], X.shape)
                np.add.at(self.cnt, (Y.ravel(), X.ravel()), 1)
                H = np.broadcast_to(hmask[sel][:, None, None], X.shape)
                if H.any():
                    np.add.at(self.hcnt, (Y[H], X[H]), 1)
                    np.add.at(self.zsum, (Y[H], X[H]), Z[H])

    def finish(self):
        marker = self.cnt > 0
        self.band = self._box_sum(marker.astype(np.float64), self.rc) > 0
        cnt_box = self._box_sum(self.hcnt.astype(np.float64), self.rc_z)
        z_box = self._box_sum(self.zsum, self.rc_z)
        z_tight = np.where(cnt_box > 0, z_box / np.maximum(cnt_box, 1.0), np.nan)
        cnt_wide = self._box_sum(self.hcnt.astype(np.float64), self.rc)
        z_wide = self._box_sum(self.zsum, self.rc)
        z_w = np.where(cnt_wide > 0, z_wide / np.maximum(cnt_wide, 1.0), np.nan)
        self.zmean = np.where(np.isnan(z_tight), z_w, z_tight).astype(np.float64)

    @staticmethod
    def _box_sum(a, r):
        h, w = a.shape
        p = np.zeros((h + 1, w + 1), np.float64)
        p[1:, 1:] = np.cumsum(np.cumsum(a.astype(np.float64), 0), 1)
        y0 = np.clip(np.arange(h) - r, 0, h)
        y1 = np.clip(np.arange(h) + r + 1, 0, h)
        x0 = np.clip(np.arange(w) - r, 0, w)
        x1 = np.clip(np.arange(w) + r + 1, 0, w)
        return (p[np.ix_(y1, x1)] - p[np.ix_(y0, x1)]
                - p[np.ix_(y1, x0)] + p[np.ix_(y0, x0)])

    def state(self):
        return {
            "gx0": self.gx0, "gy0": self.gy0, "nx": self.nx, "ny": self.ny,
            "cell": self.cell, "margin": self.margin,
            "margin_tight": self.margin_tight,
            "bak_dir": getattr(self, "bak_dir", ""),
            "band": self.band, "zmean": self.zmean,
        }


_B = None


def _init_worker(state):
    global _B
    _B = state


def tri_mat_of(ntri, subs):
    mat_of = np.zeros(ntri, np.int64)
    for (m, off, count, voff) in subs:
        if count:
            mat_of[off // 3:(off + count) // 3] = int(m)
    return mat_of


def sand_mask(tri_pos, tri_uv, tri_nrm, mat_of, names):
    ntri = len(tri_pos)
    src_ids = [i for i, n in enumerate(names) if is_sand_source(n)]
    ambig_ids = [i for i, n in enumerate(names) if n in SAND_AMBIG]
    is_src = np.isin(mat_of, src_ids)
    is_ambig = np.isin(mat_of, ambig_ids)
    sand = np.zeros(ntri, bool)
    fill = np.zeros(ntri, bool)
    if not is_src.any():
        return sand, fill

    up = tri_nrm.mean(axis=1)[:, 2] > 0.5
    fill = fill_uv_mask(tri_pos, tri_uv, tri_nrm)
    cent = tri_pos.mean(axis=1)

    b = _B
    cx = np.floor(cent[:, 0] / b["cell"]).astype(np.int64) - b["gx0"]
    cy = np.floor(cent[:, 1] / b["cell"]).astype(np.int64) - b["gy0"]
    valid = (cx >= 0) & (cx < b["nx"]) & (cy >= 0) & (cy < b["ny"])
    inb = np.zeros(ntri, bool)
    zmean = np.full(ntri, np.nan)
    inb[valid] = b["band"][cy[valid], cx[valid]]
    zmean[valid] = b["zmean"][cy[valid], cx[valid]]

    margin = np.where(is_ambig, b["margin_tight"], b["margin"])
    zok = np.abs(cent[:, 2] - zmean) <= margin
    sand = is_src & up & inb & zok

    cand = np.flatnonzero(is_src)
    if sand.any() and len(cand):
        keys = pos_keys(tri_pos[cand])
        _, inv = np.unique(keys, axis=0, return_inverse=True)
        hit = np.unique(inv[sand[cand]])
        sand[cand[np.isin(inv, hit)]] = True
    return sand, fill


def process(path, dry=False, dump=False):
    pos, uv, nrm, idx, subs, names = read_tile(path)
    ntri = len(idx) // 3
    if ntri == 0:
        return None
    tri_idx = idx.reshape(-1, 3)
    tri_pos = pos[tri_idx]
    tri_uv = uv[tri_idx]
    tri_nrm = nrm[tri_idx]
    mat_of = tri_mat_of(ntri, subs)

    sand, fill = sand_mask(tri_pos, tri_uv, tri_nrm, mat_of, names)
    n_sand = int(sand.sum())
    out = {"changed": n_sand > 0, "tris": ntri, "sand": n_sand}
    if dry:
        cnt = {}
        for i, n in enumerate(names):
            if not is_sand_source(n):
                continue
            m = mat_of == i
            if m.any():
                cnt[n] = (int(m.sum()), int(sand[m].sum()),
                          int((m & fill).sum()),
                          int((m & fill & sand).sum()))
        out["names"] = cnt
    if dump:
        cand = np.isin(mat_of, [i for i, n in enumerate(names)
                                if is_sand_source(n)])
        idxs = np.flatnonzero(cand)
        if len(idxs) > 400:
            idxs = idxs[:: len(idxs) // 400 + 1]
        cent = tri_pos.mean(axis=1)
        out["pts"] = [(float(cent[i, 0]), float(cent[i, 1]),
                       float(cent[i, 2]), bool(sand[i]),
                       names[int(mat_of[i])]) for i in idxs]
    if n_sand == 0 or dry:
        return out

    bak = _B.get("bak_dir")
    if bak:
        os.makedirs(bak, exist_ok=True)
        dst = Path(bak) / path.name
        if not dst.exists():
            import shutil
            shutil.copy2(path, dst)

    names = list(names)
    if SAND_MAT not in names:
        names.append(SAND_MAT)
    sand_id = names.index(SAND_MAT)
    mat2 = mat_of.copy()
    mat2[sand] = sand_id

    order = np.argsort(mat2, kind="stable")
    new_idx = tri_idx[order].reshape(-1)
    mk = mat2[order]
    cuts = np.flatnonzero(np.diff(mk)) + 1
    runs = np.split(np.arange(len(mk)), cuts)
    new_subs = [(int(mk[r[0]]), int(r[0]) * 3, int(len(r)) * 3, 0)
                for r in runs]
    write_tile(path, pos, uv, nrm, new_idx, new_subs, names)
    return out


def _worker(job):
    p, dry = job
    try:
        return process(Path(p), dry=dry)
    except Exception as e:
        return {"err": f"{Path(p).name}: {e}"}


def _scan(job):
    p = job
    try:
        names = read_names(Path(p))
        return (p,
                any(is_marker_name(n) for n in names),
                any(is_sand_source(n) for n in names))
    except Exception as e:
        return (p, False, False)


def rasterize(path, band):
    pos, uv, nrm, idx, subs, names = read_tile(path)
    ntri = len(idx) // 3
    if ntri == 0:
        return 0, {}
    tri_idx = idx.reshape(-1, 3)
    mat_of = tri_mat_of(ntri, subs)
    n = 0
    by_name = {}
    for (m, off, count, voff) in subs:
        if not count:
            continue
        name = names[int(m)] if int(m) < len(names) else ""
        if not is_marker_name(name):
            continue
        rows = tri_idx[off // 3:(off + count) // 3]
        tri = pos[rows]
        zc = tri[:, :, 2].mean(1)
        band.add(tri[:, :, 0].min(1), tri[:, :, 1].min(1),
                 tri[:, :, 0].max(1), tri[:, :, 1].max(1), zc,
                 height=is_height_marker_name(name))
        n += len(rows)
        st = by_name.setdefault(name, [0, 0.0, 1e30, -1e30])
        st[0] += len(rows)
        st[1] += float(zc.sum())
        st[2] = min(st[2], float(zc.min()))
        st[3] = max(st[3], float(zc.max()))
    return n, by_name


def tile_key(p):
    parts = p.stem.split("_")
    return int(parts[1]), int(parts[2])


def _print_names(by_name):
    rows = sorted(by_name.items(), key=lambda kv: -kv[1][1])
    print("  sand by name (cand -> sanded | fill family -> sanded):")
    for k, (c, s, cf, sf) in rows:
        print(f"    {k[:52]:52s} {c:6d} -> {s:6d}  | "
              f"fill {cf:6d} -> {sf:6d}")


def dump_preview(path, pts, band):
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    csv_path = Path(path).with_suffix(".csv")
    with open(csv_path, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["x", "y", "z", "sand", "name"])
        for x, y, z, s, n in pts:
            w.writerow([f"{x:.1f}", f"{y:.1f}", f"{z:.1f}", int(s), n])
    print(f"  points -> {csv_path} ({len(pts)} rows)")

    fig, ax = plt.subplots(figsize=(13, 13))
    ext = (band.gx0 * band.cell, (band.gx0 + band.nx) * band.cell,
           band.gy0 * band.cell, (band.gy0 + band.ny) * band.cell)
    ax.imshow(band.band.astype(float), origin="lower", extent=ext,
              cmap="Blues", alpha=0.25, vmin=0, vmax=1.5,
              interpolation="nearest", label="band")
    ax.imshow((band.cnt > 0).astype(float), origin="lower", extent=ext,
              cmap="Blues", alpha=0.45, vmin=0, vmax=1.5,
              interpolation="nearest")
    keep = [(x, y) for x, y, z, s, n in pts if not s]
    sand = [(x, y) for x, y, z, s, n in pts if s]
    if keep:
        k = np.array(keep)
        ax.scatter(k[:, 0], k[:, 1], s=2, c="#b0b0b0", label="kept")
    if sand:
        s = np.array(sand)
        ax.scatter(s[:, 0], s[:, 1], s=3, c="#e08a20", label="sanded")
    ax.set_aspect("equal")
    ax.legend(loc="upper right")
    ax.set_title(f"fix_beach sand mask (band {band.radius_units:.0f}u, "
                 f"z {band.margin:.0f}/{band.margin_tight:.0f} "
                 f"@ r{band.zradius_units:.0f}u)")
    fig.savefig(path, dpi=90)
    print(f"  preview -> {path}")


def main():
    ap = argparse.ArgumentParser(
        description="Retexture grey-grid/palette-strip beach terrain to sand")
    ap.add_argument("map_dir")
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--window", nargs=4, type=int, metavar=("TX0", "TX1",
                                                            "TY0", "TY1"),
                    help="tile-key half-open box; default = whole map")
    ap.add_argument("--radius", type=float, default=75.0,
                    help="beach band dilation around markers, metres")
    ap.add_argument("--zradius", type=float, default=25.0,
                    help="marker HEIGHT lookup radius, metres (tight on "
                         "purpose: local marker neighbourhood)")
    ap.add_argument("--cell", type=float, default=256.0,
                    help="marker grid cell, world units")
    ap.add_argument("--zmargin", type=float, default=250.0,
                    help="|z - local marker z| for beach art names, units")
    ap.add_argument("--zmargin-tight", type=float, default=100.0,
                    help="same for the ambiguous city-fill names")
    ap.add_argument("--no-backup", action="store_true",
                    help="do not copy changed tiles to tiles_bak_fixbeach")
    ap.add_argument("--dump-pts", type=str, default="",
                    help="dry-run only: write sand mask preview PNG (+CSV)")
    ap.add_argument("--workers", type=int, default=0)
    ap.add_argument("--limit", type=int, default=0)
    a = ap.parse_args()

    tiles = Path(a.map_dir) / "tiles"
    files = sorted(tiles.glob("t_*.bin")) + sorted(tiles.glob("s_*.bin"))
    radius_units = a.radius * UNITS_PER_M
    margin_tiles = int(radius_units // TILE_UNITS) + 2

    if a.window:
        tx0, tx1, ty0, ty1 = a.window
        scan_files = [p for p in files
                      if tx0 - margin_tiles <= tile_key(p)[0] < tx1 + margin_tiles
                      and ty0 - margin_tiles <= tile_key(p)[1] < ty1 + margin_tiles]
        apply_files = [p for p in files
                       if tx0 <= tile_key(p)[0] < tx1
                       and ty0 <= tile_key(p)[1] < ty1]
    else:
        scan_files = files
        apply_files = files
    if a.limit:
        apply_files = apply_files[:a.limit]
        scan_files = scan_files[:a.limit]
    print(f"{len(files)} tile files under {tiles}")
    print(f"  scan {len(scan_files)} (window + {margin_tiles} tile margin), "
          f"apply {len(apply_files)}")

    workers = a.workers or max(1, (os.cpu_count() or 4) - 1)
    t0 = time.time()

    marker_files, sand_files = [], []
    if workers > 1:
        from concurrent.futures import ProcessPoolExecutor
        with ProcessPoolExecutor(max_workers=workers) as ex:
            for p, has_m, has_s in ex.map(_scan, [str(p) for p in scan_files],
                                          chunksize=64):
                if has_m:
                    marker_files.append(p)
                if has_s:
                    sand_files.append(p)
    else:
        for p in scan_files:
            _, has_m, has_s = _scan(str(p))
            if has_m:
                marker_files.append(str(p))
            if has_s:
                sand_files.append(str(p))
    apply_set = {str(p) for p in apply_files}
    sand_files = [p for p in sand_files if p in apply_set]
    print(f"  pass A {time.time()-t0:.0f}s: {len(marker_files)} marker tiles, "
          f"{len(sand_files)} sand-source tiles to process", flush=True)

    keys = [tile_key(Path(p)) for p in scan_files]
    if not keys:
        print("no tiles in scope")
        return
    mminx = min(k[0] for k in keys)
    mmaxx = max(k[0] for k in keys)
    mminy = min(k[1] for k in keys)
    mmaxy = max(k[1] for k in keys)
    cell = a.cell
    gx0 = int(np.floor(mminx * TILE_UNITS / cell))
    gy0 = int(np.floor(mminy * TILE_UNITS / cell))
    nx = int(np.ceil((mmaxx + 1) * TILE_UNITS / cell)) - gx0
    ny = int(np.ceil((mmaxy + 1) * TILE_UNITS / cell)) - gy0
    zradius_units = a.zradius * UNITS_PER_M
    band = BeachBand(gx0, gy0, nx, ny, cell, radius_units, zradius_units,
                     a.zmargin, a.zmargin_tight)
    band.bak_dir = "" if a.no_backup else str(Path(a.map_dir) /
                                              "tiles_bak_fixbeach")
    if band.bak_dir:
        os.makedirs(band.bak_dir, exist_ok=True)
    nmark = 0
    markers = {}
    for i, p in enumerate(marker_files, 1):
        n, by_name = rasterize(Path(p), band)
        nmark += n
        for k, st in by_name.items():
            cur = markers.get(k)
            if cur is None:
                markers[k] = list(st)
            else:
                cur[0] += st[0]
                cur[1] += st[1]
                cur[2] = min(cur[2], st[2])
                cur[3] = max(cur[3], st[3])
        if i % 500 == 0:
            print(f"  rasterize {i}/{len(marker_files)}", flush=True)
    band.finish()
    print(f"  band: grid {nx}x{ny} cells, {int(band.cnt.sum())} marker hits "
          f"({nmark} marker tris), band {band.band.mean():.1%} of cells "
          f"[{time.time()-t0:.0f}s]", flush=True)
    top = sorted(markers.items(), key=lambda kv: -kv[1][0])[:12]
    print("  markers (name: tris, mean z, z range):")
    for k, (v, zs, zlo, zhi) in top:
        print(f"    {k.split('/')[-1][:44]:44s} {v:7d}  z~{zs / v:6.0f} "
              f"[{zlo:.0f},{zhi:.0f}]")

    if not sand_files:
        print("nothing to sand")
        return

    if a.dump_pts:
        pts = []
        by_name = {}
        for i, p in enumerate(sand_files, 1):
            _init_worker(band.state())
            r = process(Path(p), dry=True, dump=True)
            if r and r.get("pts"):
                pts.extend(r["pts"])
            if r and r.get("names"):
                for k, (c, s, cf, sf) in r["names"].items():
                    pc, ps, pcf, psf = by_name.get(k, (0, 0, 0, 0))
                    by_name[k] = (pc + c, ps + s, pcf + cf, psf + sf)
        dump_preview(a.dump_pts, pts, band)
        _print_names(by_name)
        print(f"DONE dry dump {len(sand_files)} files, "
              f"{sum(1 for _x, _y, _z, s, _n in pts if s)} sanded samples, "
              f"{time.time()-t0:.0f}s")
        return

    changed = tot_tris = tot_sand = 0
    by_name = {}
    jobs = [(p, a.dry_run) for p in sand_files]
    if workers > 1 and not a.dry_run and not a.dump_pts:
        from concurrent.futures import ProcessPoolExecutor
        with ProcessPoolExecutor(max_workers=workers,
                                 initializer=_init_worker,
                                 initargs=(band.state(),)) as ex:
            for i, r in enumerate(ex.map(_worker, jobs, chunksize=8), 1):
                if isinstance(r, dict) and "err" in r:
                    print(f"  ERR {r['err']}")
                    continue
                if r and r.get("changed"):
                    changed += 1
                    tot_tris += r["tris"]
                    tot_sand += r["sand"]
                if r and r.get("names"):
                    for k, (c, s) in r["names"].items():
                        pc, ps = by_name.get(k, (0, 0))
                        by_name[k] = (pc + c, ps + s)
                if i % 500 == 0:
                    print(f"  {i}/{len(jobs)}  {time.time()-t0:.0f}s  "
                          f"sanded {tot_sand}", flush=True)
    else:
        _init_worker(band.state())
        for i, (p, dry) in enumerate(jobs, 1):
            try:
                r = process(Path(p), dry=dry)
            except Exception as e:
                print(f"  ERR {Path(p).name}: {e}")
                continue
            if r and r.get("changed"):
                changed += 1
                tot_tris += r["tris"]
                tot_sand += r["sand"]
            if r and r.get("names"):
                for k, (c, s, cf, sf) in r["names"].items():
                    pc, ps, pcf, psf = by_name.get(k, (0, 0, 0, 0))
                    by_name[k] = (pc + c, ps + s, pcf + cf, psf + sf)
            if i % 500 == 0:
                print(f"  {i}/{len(jobs)}  {time.time()-t0:.0f}s  "
                      f"sanded {tot_sand}", flush=True)

    verb = "would sand" if a.dry_run else "sanded"
    print(f"DONE {len(jobs)} sand-source tiles, {changed} changed, "
          f"{time.time()-t0:.0f}s")
    print(f"  triangles in changed tiles: {tot_tris}, {verb} {tot_sand}")
    if a.dry_run and by_name:
        _print_names(by_name)
    if not a.dry_run:
        print("  NOTE: repack pages or the game keeps reading the old ones:")
        print("    python -X utf8 tools\\pack build")


if __name__ == "__main__":
    main()
