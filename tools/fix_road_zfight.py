import argparse, os, sys, time
from pathlib import Path
import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from exporter.gtav.lod import read_tile, write_tile

GROUND_FILLER = {
    "models/gtav/bh1_17/im_road_027b",
    "models/gtav/bh1_17/dc_sidewalk004",
}
ROAD_ART = lambda n: (
    n.startswith("models/gtav/roads/")
    or n in ("models/gtav/dt1_02/im_road_012",)
    or "im_road_0" in n or "dc_road_0" in n or "im_kerbs" in n
    or "im_road_edge" in n or "im_roadblend" in n
    or "im_roadmarkings" in n or "im_drains" in n
    or "im_sidewalk" in n or "dc_sidewalk" in n
)


def canon_keys(tri_pos, tri_nrm):
    c = np.round(tri_pos, 2)
    order = np.lexsort((c[:, :, 2], c[:, :, 1], c[:, :, 0]), axis=1)
    ps = np.take_along_axis(c, order[:, :, None], axis=1).reshape(len(c), -1)
    ns = np.take_along_axis(tri_nrm, order[:, :, None], axis=1).reshape(len(c), -1)
    ns = np.round(ns * 127).astype(np.int64)
    return ps.astype(np.int64), ns


def dedup_mask(tri_pos, tri_nrm):
    pk, nk = canon_keys(tri_pos, tri_nrm)
    key = np.concatenate([pk, nk], axis=1)
    _, first = np.unique(key, axis=0, return_index=True)
    keep = np.zeros(len(key), bool)
    keep[first] = True
    return keep


def _point_in_tri_2d(p, a, b, c):
    v0 = c - a
    v1 = b - a
    v2 = p - a
    d00 = (v0 * v0).sum(1); d01 = (v0 * v1).sum(1); d11 = (v1 * v1).sum(1)
    d20 = (v2 * v0).sum(1); d21 = (v2 * v1).sum(1)
    den = d00 * d11 - d01 * d01
    ok = np.abs(den) > 1e-9
    u = np.zeros(len(a)); v = np.zeros(len(a))
    u[ok] = (d11[ok] * d20[ok] - d01[ok] * d21[ok]) / den[ok]
    v[ok] = (d00[ok] * d21[ok] - d01[ok] * d20[ok]) / den[ok]
    return ok & (u >= -1e-6) & (v >= -1e-6) & (u + v <= 1 + 1e-6)


def ground_covered_mask(tri_pos, mat_name, names, z_eps=1.0):
    ntri = len(tri_pos)
    drop = np.zeros(ntri, bool)
    filler_ids = {i for i, n in enumerate(names) if n in GROUND_FILLER}
    art_ids = {i for i, n in enumerate(names)
               if n not in GROUND_FILLER and ROAD_ART(n)}
    if not filler_ids or not art_ids:
        return drop

    is_filler = np.isin(mat_name, list(filler_ids))
    is_art = np.isin(mat_name, list(art_ids))
    fi = np.flatnonzero(is_filler)
    ai = np.flatnonzero(is_art)
    if len(fi) == 0 or len(ai) == 0:
        return drop

    art_xy = tri_pos[ai]  # (A,3,3)
    art_z = art_xy[:, :, 2].mean(1)
    art_a = art_xy[:, 0, :2]; art_b = art_xy[:, 1, :2]; art_c = art_xy[:, 2, :2]
    art_min = np.minimum.reduce([art_a, art_b, art_c])
    art_max = np.maximum.reduce([art_a, art_b, art_c])

    f_xy = tri_pos[fi]
    f_z = f_xy[:, :, 2].mean(1)
    f_cent = f_xy.mean(1, keepdims=False)[:, :2]
    f_min = np.minimum.reduce([f_xy[:, 0, :2], f_xy[:, 1, :2], f_xy[:, 2, :2]])
    f_max = np.maximum.reduce([f_xy[:, 0, :2], f_xy[:, 1, :2], f_xy[:, 2, :2]])

    CH = 4096
    covered = np.zeros(len(fi), bool)
    for s in range(0, len(ai), CH):
        amin = art_min[s:s+CH]; amax = art_max[s:s+CH]; az = art_z[s:s+CH]
        A = art_a[s:s+CH]; B = art_b[s:s+CH]; C = art_c[s:s+CH]
        # bbox overlap
        ov = ((f_min[:, None, 0] <= amax[None, :, 0]) &
              (f_max[:, None, 0] >= amin[None, :, 0]) &
              (f_min[:, None, 1] <= amax[None, :, 1]) &
              (f_max[:, None, 1] >= amin[None, :, 1]) &
              (np.abs(f_z[:, None] - az[None, :]) <= z_eps))
        rows, cols = np.nonzero(ov)
        if len(rows):
            inside = _point_in_tri_2d(f_cent[rows], A[cols], B[cols], C[cols])
            covered[rows[inside]] = True
    drop[fi[covered]] = True
    return drop


def process(path, dry=False):
    pos, uv, nrm, idx, subs, names = read_tile(path)
    ntri = len(idx) // 3
    if ntri == 0:
        return None
    tri_idx = idx.reshape(-1, 3)
    tri_pos = pos[tri_idx]
    tri_nrm = nrm[tri_idx]

    # material id per triangle
    mat_of = np.zeros(ntri, np.int64)
    for (m, off, count, voff) in subs:
        mat_of[off // 3:(off + count) // 3] = int(m)

    keep_dedup = dedup_mask(tri_pos, tri_nrm)
    drop_cov = ground_covered_mask(tri_pos, mat_of, names)
    keep = keep_dedup & ~drop_cov

    n_dup = int((~keep_dedup).sum())
    n_cov = int((drop_cov & keep_dedup).sum())
    kept = int(keep.sum())
    if kept == ntri:
        return {"changed": False, "tris": ntri}
    if dry:
        return {"changed": True, "tris": ntri, "kept": kept,
                "dup": n_dup, "covered": n_cov}

    tri_keep = tri_idx[keep]
    mk = mat_of[keep]
    order = np.argsort(mk, kind="stable")
    tri_keep = tri_keep[order]
    mk = mk[order]
    new_idx = tri_keep.reshape(-1)
    cuts = np.flatnonzero(np.diff(mk)) + 1
    runs = np.split(np.arange(len(mk)), cuts)
    new_subs = []
    voff = 0
    for r in runs:
        off3 = int(r[0]) * 3
        cnt3 = int(len(r)) * 3
        new_subs.append((int(mk[r[0]]), off3, cnt3, 0))
    write_tile(path, pos, uv, nrm, new_idx, new_subs, names)
    return {"changed": True, "tris": ntri, "kept": kept, "dup": n_dup, "covered": n_cov}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("map_dir")
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--limit", type=int, default=0)
    ap.add_argument("--workers", type=int, default=0)
    a = ap.parse_args()
    tiles = Path(a.map_dir) / "tiles"
    files = sorted(tiles.glob("t_*_0*.bin"))  # lod0 + far lods share the prefix
    if a.limit:
        files = files[:a.limit]
    print(f"{len(files)} tile files under {tiles}", flush=True)
    t0 = time.time()

    jobs = [(str(p), a.dry_run) for p in files]
    changed = 0
    tot_tris = tot_kept = tot_dup = tot_cov = 0
    workers = a.workers or max(1, (os.cpu_count() or 4) - 1)

    def _acc(r):
        nonlocal changed, tot_tris, tot_kept, tot_dup, tot_cov
        if r and r.get("changed"):
            changed += 1
            tot_tris += r["tris"]; tot_kept += r["kept"]
            tot_dup += r.get("dup", 0); tot_cov += r.get("covered", 0)

    if workers > 1 and not a.dry_run:
        from concurrent.futures import ProcessPoolExecutor
        with ProcessPoolExecutor(max_workers=workers) as ex:
            for i, r in enumerate(ex.map(_worker, jobs, chunksize=16), 1):
                if isinstance(r, dict) and "err" in r:
                    print(f"  ERR {r['err']}"); continue
                _acc(r)
                if i % 2000 == 0:
                    print(f"  {i}/{len(files)}  {time.time()-t0:.0f}s "
                          f"kept {tot_kept}", flush=True)
    else:
        for i, (p, dry) in enumerate(jobs, 1):
            try:
                r = process(Path(p), dry=dry)
            except Exception as e:
                print(f"  ERR {Path(p).name}: {e}"); continue
            _acc(r)
            if i % 500 == 0:
                print(f"  {i}/{len(files)}  {time.time()-t0:.0f}s", flush=True)

    print(f"DONE {len(files)} files, {changed} changed, {time.time()-t0:.0f}s")
    print(f"  tris {tot_tris} -> {tot_kept}  (dropped {tot_tris-tot_kept}: "
          f"dup~{tot_dup} covered~{tot_cov})")


def _worker(job):
    p, dry = job
    try:
        return process(Path(p), dry=dry)
    except Exception as e:
        return {"err": f"{Path(p).name}: {e}"}


if __name__ == "__main__":
    main()
