import argparse
import csv
import re
import sys
from pathlib import Path

import numpy as np

SELFTEST_ROOT = Path(r'C:\Program Files (x86)\Steam\steamapps\common\GarrysMod'
                     r'\garrysmod\data\gtav\selftest')
PIPELINE_RUNS = Path(r'C:\Users\Frame\AppData\Local\Temp\opencode\pipeline_runs')
VISUAL_BASELINE = PIPELINE_RUNS / 'VISUAL_BASELINE.txt'
W, H = 1600, 900
VFOV = 58.7

MEANABS_FLOOR = 0.35
HOLE_PCT_GATE = 0.50  # % of pixels allowed to be holes before FAIL
DETAIL_RATIO_LO = 0.80  # new/old mean detail, below = collapsed
DETAIL_RATIO_HI = 1.60  # above = suspicious (usually a baseline artifact)


def load_gray(run: Path, img: str):
    from PIL import Image
    p = run / img
    if not p.is_file():
        return None
    return np.asarray(Image.open(p).convert('L'), np.float32) / 255.0


def detail(a: np.ndarray) -> np.ndarray:
    g = (np.abs(np.diff(a, axis=0, prepend=a[:1]))
         + np.abs(np.diff(a, axis=1, prepend=a[:, :1])))
    k = 5
    pad = np.pad(g, k // 2, mode='edge')
    s = np.zeros_like(g)
    for dy in range(k):
        for dx in range(k):
            s += pad[dy:dy + g.shape[0], dx:dx + g.shape[1]]
    m = s.max()
    return s / m if m > 0 else s


DEFAULT_PITCH = 18.0
DEFAULT_YAW = 180.0


def cam_basis(row: dict):
    tan_v = np.tan(np.radians(VFOV) / 2)
    y = np.radians(float(row.get('eyeYaw') or DEFAULT_YAW))
    p = np.radians(float(row.get('eyePitch') or DEFAULT_PITCH))
    fwd = np.array([np.cos(y) * np.cos(p), np.sin(y) * np.cos(p), -np.sin(p)])
    right = np.array([np.sin(y), -np.cos(y), 0.0])  # Y is LEFT in Source
    up = np.cross(right, fwd)
    if up[2] <= 0:
        return fwd, right, up, float('nan')
    horizon = (0.5 - (-fwd[2] / up[2]) / (2 * tan_v)) * H
    return fwd, right, up, horizon


def row_distance_table(row: dict):
    tan_v = np.tan(np.radians(VFOV) / 2)
    fwd, right, up, horizon = cam_basis(row)
    if not np.isfinite(horizon):
        return horizon, []
    eye_z = float(row['z'])
    out = []
    for r in (190, 216, 241, 293, 320, 384, 447, 601, 700):
        ny = (0.5 - r / H) * 2 * tan_v
        dz = fwd[2] + ny * up[2]
        t = (0.0 - eye_z) / dz if dz < 0 else float('inf')
        out.append((r, t))
    return horizon, out


def pick_row(run: Path, wp: int) -> dict:
    rows = list(csv.DictReader(open(run / 'stats.csv')))
    if not rows:
        raise SystemExit(f'{run}: stats.csv has no rows')
    return rows[wp - 1] if len(rows) >= wp else rows[-1]


STAMPED = re.compile(r'(?:^|_)(\d{8}_\d{6})$')


def run_info(p: Path):
    try:
        rows = list(csv.DictReader(open(p / 'stats.csv')))
    except Exception:
        rows = []
    cols = rows[0].keys() if rows else ()
    m = STAMPED.search(p.name)
    return {
        'cam': 'eyePitch' in cols and 'eyeYaw' in cols,
        'caps': any(p.glob('wp_*.png')),
        'stamped': bool(m),
        'stamp': m.group(1) if m else '',  # chronological key for real sweeps
        'mtime': p.stat().st_mtime,
    }


def pick_run(need_captures=False, exclude=()):
    skip_paths, skip_names = set(), set()
    for e in exclude:
        q = Path(e)
        skip_names.add(q.name)
        try:
            skip_paths.add(q.resolve())
        except OSError:
            pass
    runs = [p for p in SELFTEST_ROOT.iterdir() if (p / 'stats.csv').is_file()]
    scored = []
    for p in runs:
        if p.resolve() in skip_paths or p.name in skip_names:
            continue
        i = run_info(p)
        if need_captures and not i['caps']:
            continue
        scored.append(((int(i['caps']), int(i['cam']), int(i['stamped']),
                        i['stamp'], i['mtime']), p))
    if not scored:
        return None
    scored.sort(key=lambda kv: kv[0])
    return scored[-1][1]


def diff_one(old_a, new_a, box=None):
    if box:
        x, y, w, h = box
        sl = (slice(y, y + h), slice(x, x + w))
        old_a, new_a = old_a[sl], new_a[sl]
    do, dn = detail(old_a), detail(new_a)
    thr_strong = np.percentile(do, 75)
    thr_flat = np.percentile(dn, 25)
    holes = (do > thr_strong) & (dn < thr_flat)
    gains = (dn > thr_strong) & (do < thr_flat)
    changed = np.abs(do - dn) > 0.15
    return {
        'detail_old': float(do.mean()),
        'detail_new': float(dn.mean()),
        'ratio': float(dn.mean() / max(do.mean(), 1e-9)),
        'hole_pct': float(100 * holes.mean()),
        'gain_pct': float(100 * gains.mean()),
        'changed_pct': float(100 * changed.mean()),
        'meanAbs': float(np.abs(new_a - old_a).mean() * 255),
        'bright_old': float(old_a.mean()),
        'bright_new': float(new_a.mean()),
        'holes': holes,
        'gains': gains,
        'changed': changed,
    }


def save_map(d, old_run, new_run, box, tag):
    from PIL import Image
    out = PIPELINE_RUNS / 'capture_diff'
    out.mkdir(parents=True, exist_ok=True)
    img = np.zeros(d['holes'].shape + (3,), np.uint8)
    img[..., 0] = np.where(d['holes'], 255, 0)
    img[..., 1] = np.where(d['gains'], 255, 0)
    img[..., 2] = np.where(d['changed'] & ~d['holes'] & ~d['gains'], 255, 0)
    name = f'{old_run.name}_vs_{new_run.name}_{tag}.png'
    Image.fromarray(img).save(out / name)
    return out / name


def resolve(p: str) -> Path:
    if p == 'baseline':
        if not VISUAL_BASELINE.is_file():
            raise SystemExit('no visual baseline; run with '
                             '--accept-visual-baseline first')
        return Path(VISUAL_BASELINE.read_text(encoding='utf-8').strip())
    q = Path(p)
    return q if q.is_dir() else SELFTEST_ROOT / p


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument('old', nargs='?', default=None)
    ap.add_argument('new', nargs='?', default=None)
    ap.add_argument('--box', nargs=4, type=int, default=None,
                    metavar=('X', 'Y', 'W', 'H'))
    ap.add_argument('--calib', action='store_true',
                    help='print the row->distance calibration only')
    ap.add_argument('--accept-visual-baseline', action='store_true')
    ap.add_argument('--run', default=None,
                    help='pin the run dir used by --calib / '
                         '--accept-visual-baseline (default: newest '
                         'capture run with camera columns)')
    ap.add_argument('--wp', type=int, default=13)
    ap.add_argument('--newest', action='store_true',
                    help='use the newest capture run as <new> '
                         '(so "capture_diff.py baseline --newest" compares the '
                         'sticky baseline against whatever was captured last)')
    a = ap.parse_args()

    if a.calib:
        run = Path(a.run) if a.run else pick_run()
        if run is None or not (run / 'stats.csv').is_file():
            print('RESULT VIS OK=0 ERR=no selftest runs')
            return 1
        row = pick_row(run, a.wp)
        horizon, tbl = row_distance_table(row)
        print(f'run {run.name}  wp{a.wp}  eye z {row.get("z", "?")}')
        print(f'  camera pitch {row.get("eyePitch", DEFAULT_PITCH)} '
              f'yaw {row.get("eyeYaw", DEFAULT_YAW)}'
              + ('  (defaults: run has no camera cols)'
                 if 'eyeYaw' not in row else ''))
        print(f'  horizon row {horizon:.0f}   (healthy ~175-190 at pitch 18)')
        for r, t in tbl:
            print(f'  row {r:4d} -> {t:9.0f} units')
        print('  LOD bands: lod1 rows 293-601, lod2 241-293, lod3 216-241, '
              'lod4 190-216')
        return 0

    if a.accept_visual_baseline:
        run = Path(a.run) if a.run else pick_run(need_captures=True)
        if run is None or not (run / 'stats.csv').is_file():
            print('RESULT VIS OK=0 ERR=no selftest run with wp_*.png captures')
            return 1
        PIPELINE_RUNS.mkdir(parents=True, exist_ok=True)
        VISUAL_BASELINE.write_text(str(run), encoding='utf-8')
        print(f'RESULT VIS OK=1 VISUAL_BASELINE_SET={run}')
        return 0

    new = a.new
    if a.newest:
        exclude = []
        if a.old:
            try:
                exclude.append(resolve(a.old))
            except SystemExit:
                pass
        run = pick_run(need_captures=True, exclude=exclude)
        if run is None:
            print('RESULT VIS OK=0 ERR=no selftest run with wp_*.png captures')
            return 1
        new = str(run)
    if not a.old or not new:
        ap.error('need <old_run> <new_run>, or --calib / '
                 '--accept-visual-baseline / --newest')

    old_run, new_run = resolve(a.old), resolve(new)
    horizon, tbl = row_distance_table(pick_row(new_run, a.wp))

    imgs = sorted(p.name for p in new_run.glob('wp_*.png'))
    if not imgs:
        print('RESULT VIS OK=0 ERR=no wp_*.png captures')
        return 1

    ok = True
    print(f'== visual: {new_run.name} vs {old_run.name}')
    print(f'   horizon row {horizon:.0f}  (healthy ~175-190)')
    if not (150 <= horizon <= 220):
        print('   WARN horizon off: camera basis is likely mirrored')
    for img in imgs:
        o, n = load_gray(old_run, img), load_gray(new_run, img)
        if o is None or n is None:
            print(f'RESULT VIS WP={img} OK=0 ERR=missing capture')
            ok = False
            continue
        d = diff_one(o, n, a.box)
        verdict = 'OK'
        if d['hole_pct'] > HOLE_PCT_GATE:
            verdict = 'FAIL'
            ok = False
        elif not (DETAIL_RATIO_LO <= d['ratio'] <= DETAIL_RATIO_HI):
            verdict = 'WARN'
        wp_ok = verdict != 'FAIL'
        print(f'RESULT VIS WP={img} OK={1 if wp_ok else 0} VERDICT={verdict} '
              f'meanAbs={d["meanAbs"]:.2f} detailRatio={d["ratio"]:.3f} '
              f'holePct={d["hole_pct"]:.2f} gainPct={d["gain_pct"]:.2f} '
              f'changedPct={d["changed_pct"]:.2f}')
        if verdict != 'OK':
            p = save_map(d, old_run, new_run, a.box, img[:-4])
            print(f'   map -> {p}')
    print(f'RESULT VIS OK={1 if ok else 0} RUNS={old_run.name}->{new_run.name}')
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
