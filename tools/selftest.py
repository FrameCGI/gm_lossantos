from __future__ import annotations

import argparse
import json
import shutil
import subprocess
import sys
import time
import winreg
from pathlib import Path

DEFAULT_GMOD = Path(r"C:\Program Files (x86)\Steam\steamapps\common\GarrysMod")

# dx dy dz pitch yaw, from spawn
WAYPOINTS = [
    (0, 0, 0, 0, 0),
    (0, 0, 500, 0, 0),
    (3000, 0, 400, 0, 0),
    (8192, 0, 400, 0, 0),
    (8400, 300, 400, 0, 0),
    (-8192, -8192, 400, 0, 0),
    (0, 0, 20000, 0, 0),
    (0, 0, 120000, 0, 0),
    (25000, 25000, 3700, 0, 0),
    (60000, 60000, 12000, 0, 0),
    # del perro shoreline, water check
    (-70181.1, -202.3, -276.4, 12, 180),
    (-70181.1, -202.3, -276.4, 8, 0),
    (-81569.1, -202.3, 523.6, 18, 180),
    (-70181.1, -202.3, -276.4, 10, -90),
]

SHOW = [
    'wp', 'x', 'y', 'z', 'active', 'tris', 'drawn', 'bad', 'badidx',
    'lostTris', 'queue', 'builds', 'bsTotal', 'groupWorstMs', 'jump2D', 'drift',
    'settleTicks',
]


def config_line(cfg, name):
    try:
        for ln in cfg.read_text(encoding='utf-8', errors='replace').splitlines():
            if ln.split(' ', 1)[0].strip('"') == name:
                return ln
    except OSError:
        pass
    return None


def restore_config_line(cfg, name, original):
    try:
        lines = cfg.read_text(encoding='utf-8', errors='replace').splitlines()
    except OSError:
        return
    out, placed = [], False
    for ln in lines:
        if ln.split(' ', 1)[0].strip('"') == name:
            if original is not None and not placed:
                out.append(original)
                placed = True
            continue
        out.append(ln)
    if original is not None and not placed:
        out.append(original)
    if out != lines:
        cfg.write_text('\n'.join(out) + '\n', encoding='utf-8')


def steam_exe() -> str:
    with winreg.OpenKey(winreg.HKEY_CURRENT_USER, r'Software\Valve\Steam') as key:
        return winreg.QueryValueEx(key, 'SteamExe')[0]


def gmod_running() -> bool:
    out = subprocess.run(['tasklist'], capture_output=True, text=True).stdout.lower()
    return 'gmod.exe' in out


def wait_closed(seconds: int = 60) -> bool:
    for _ in range(seconds):
        if not gmod_running():
            return True
        time.sleep(1)
    return not gmod_running()


def read_index(gmod: Path, name: str) -> dict:
    p = gmod / 'data' / 'lossantos' / name / 'index.json'
    if not p.is_file():
        sys.exit(f"no map index at {p}")
    with open(p, encoding='utf-8') as f:
        return json.load(f)


def build_arm(idx: dict, run: str, pitch=None, waypoints=None) -> dict:
    spawn = idx.get('spawn') or [0, 0, 0]
    sx, sy, sz = (float(spawn[0]), float(spawn[1]), float(spawn[2]))
    src = WAYPOINTS if waypoints is None else waypoints
    wps = []
    for wp in src:
        dx, dy, dz = float(wp[0]), float(wp[1]), float(wp[2])
        row = [sx + dx, sy + dy, sz + dz]
        p = float(wp[3]) if len(wp) > 3 else pitch
        y = float(wp[4]) if len(wp) > 4 else None
        if p is not None:
            row.append(float(p))
            row.append(float(y) if y is not None else 0.0)
        wps.append(row)
    return {
        'run': run,
        'waypoints': wps,
        'note': 'offsets from spawn; see tools/selftest.py',
    }


def show(folder: Path) -> None:
    stats = folder / 'stats.csv'
    holes = folder / 'holes.csv'
    if not stats.is_file():
        print('no stats.csv was written')
        return

    rows = [line.rstrip('\n').split(',') for line in stats.read_text(encoding='utf-8').splitlines() if line]
    head, body = rows[0], rows[1:]
    want = [c for c in SHOW if c in head]
    ix = [head.index(c) for c in want]

    print()
    print('  ' + ' '.join(f'{c:>10}' for c in want))
    for r in body:
        vals = [r[i] if i < len(r) else '?' for i in ix]

        def fmt(v: str) -> str:
            try:
                f = float(v)
                return f'{f:>10.0f}' if abs(f) >= 1 or f == 0 else f'{f:>10.3f}'
            except ValueError:
                return f'{v:>10}'

        print('  ' + ' '.join(fmt(v) for v in vals))

    if 'lodCounts' in head:
        li = head.index('lodCounts')
        ltri = head.index('lodTris') if 'lodTris' in head else None
        wpi = head.index('wp') if 'wp' in head else None
        print()
        print('  active tiles per LOD 0..4 (0 = full detail, 4 = most decimated):')
        for i, r in enumerate(body):
            wpv = r[wpi] if (wpi is not None and wpi < len(r)) else str(i + 1)
            cnt = r[li] if li < len(r) else '?'
            extra = ''
            if ltri is not None and ltri < len(r):
                extra = f"   tris {r[ltri]}"
            warn = ''
            try:
                nums = [int(p) for p in cnt.split('/')]
                tot = sum(nums)
                coarse = sum(nums[3:]) if len(nums) >= 4 else 0
                if tot and coarse / tot > 0.25:
                    warn = f"   <-- {100.0 * coarse / tot:.0f}% decimated"
            except ValueError:
                pass
            print(f"    wp {wpv:>3}: {cnt}{extra}{warn}")

    if holes.is_file():
        hrows = [line.rstrip('\n').split(',') for line in holes.read_text(encoding='utf-8').splitlines() if line]
        hhead = hrows[0]
        if 'misses' in hhead and 'total' in hhead:
            mi, ti = hhead.index('misses'), hhead.index('total')
            gi = hhead.index('hits') if 'hits' in hhead else None
            ui = hhead.index('unbuilt') if 'unbuilt' in hhead else None
            ei = hhead.index('gents') if 'gents' in hhead else None
            bi = hhead.index('built') if 'built' in hhead else None
            print()
            print('  collision coverage vs the streamer\'s own wanted set')
            print('  (every phys chunk within GROUND_QUERY_RADIUS that has data):')
            wi = hhead.index('wp') if 'wp' in hhead else 0
            for r in hrows[1:]:
                if ti < len(r):
                    extra = ''
                    if gi is not None:
                        extra = f"   covered {r[gi]:>4}"
                    if ui is not None:
                        extra += f"  unbuilt {r[ui]:>4}"
                    if ei is not None and bi is not None:
                        extra += f"  gents {r[ei]:>4} built {r[bi]:>4}"
                    wai = hhead.index('wide') if 'wide' in hhead else None
                    wti = hhead.index('wideTotal') if 'wideTotal' in hhead else None
                    if wai is not None and wti is not None:
                        extra += f"   (wide {r[wai]}/{r[wti]})"
                    print(f"    wp {r[wi]:>3}: {r[mi]:>6} / {r[ti]}{extra}")

    for name in ('DONE.txt', 'FAIL.txt'):
        p = folder / name
        if p.is_file():
            print()
            print(f'  {name}: {p.read_text(encoding="utf-8").strip()}')


ERROR_SIGNATURES = ('[ERROR]', 'SCRIPT ERROR', 'attempt to call',
                    'attempt to index', 'attempt to perform')


def scan_lua_errors(log):
    if not log.is_file():
        return [], []
    try:
        text = log.read_text(errors='replace')
    except OSError:
        return [], []
    counts = {}
    for ln in text.splitlines():
        s = ln.strip()
        if any(sig in s for sig in ERROR_SIGNATURES):
            counts[s] = counts.get(s, 0) + 1
    rows = sorted(((n, s) for s, n in counts.items()), reverse=True)
    ours = [(n, s) for n, s in rows if 'gtav' in s]
    foreign = [(n, s) for n, s in rows if 'gtav' not in s]
    return ours, foreign


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument('--gmod', default=str(DEFAULT_GMOD))
    ap.add_argument('--map', default='gm_lossantos')
    ap.add_argument('--run', default=time.strftime('%Y%m%d_%H%M%S'))
    ap.add_argument('--timeout', type=int, default=1800)
    ap.add_argument('--width', type=int, default=1600)
    ap.add_argument('--height', type=int, default=900)
    ap.add_argument('--extra', default='', help='extra launch options')
    ap.add_argument('--convars', default='',
                    help='client convars to set at run start, e.g. '
                         '"gtav_cl_merge_grid=16;gtav_cl_gc_budget_ms=0". Written '
                         'into the arm file and applied by the harness after '
                         'the addons load (+commands on the launch line run '
                         'too early and are dropped).')
    ap.add_argument('--pitch', type=float, default=None,
                    help='look direction for every capture, in degrees: '
                         '90 is straight down at the ground, -90 straight up. '
                         'Unset leaves the camera alone.')
    ap.add_argument('--waypoints', default=None,
                    help='JSON file of [dx,dy,dz] or [dx,dy,dz,pitch,yaw] '
                         'spawn-relative offsets, replacing the built-in path')
    ap.add_argument('--open', action='store_true', help='open the output folder at the end')
    ap.add_argument('--dry-run', action='store_true',
                    help='write the arm file and print the plan, but do not launch GMod')
    a = ap.parse_args()

    gmod = Path(a.gmod)
    game = gmod / 'garrysmod'
    if not (gmod / 'gmod.exe').is_file():
        sys.exit(f'not a Garry\'s Mod install: {gmod}')

    for _ in range(30):
        if not gmod_running():
            break
        time.sleep(1)
    if gmod_running():
        sys.exit("Garry's Mod is running: close it first")

    idx = read_index(game, a.map)
    out = game / 'data' / 'gtav' / 'selftest' / a.run
    if out.exists():
        shutil.rmtree(out)
    out.mkdir(parents=True)

    wps = None
    if a.waypoints:
        wps = json.loads(Path(a.waypoints).read_text(encoding='utf-8'))
        if not isinstance(wps, list) or not wps:
            raise SystemExit(f'--waypoints {a.waypoints}: expected a non-empty '
                             f'JSON list of [dx,dy,dz] offsets')
    arm = build_arm(idx, a.run, pitch=a.pitch, waypoints=wps)
    if a.convars:
        cv = {}
        for pair in a.convars.replace(',', ';').split(';'):
            pair = pair.strip()
            if not pair:
                continue
            if '=' not in pair:
                sys.exit(f'--convars: expected k=v, got {pair!r}')
            k, v = pair.split('=', 1)
            cv[k.strip()] = v.strip()
        arm['convars'] = cv
    arm_path = game / 'data' / 'gtav' / 'selftest_arm.json'
    arm_path.write_text(json.dumps(arm, indent=1), encoding='utf-8')

    log = game / 'console.log'
    log.unlink(missing_ok=True)

    print(f"run {a.run} on {a.map}")
    print(f"  {len(arm['waypoints'])} waypoints, spawn {idx.get('spawn')}")
    print(f"  output -> {out}")
    for i, wp in enumerate(arm['waypoints'], 1):
        look = f"   look {wp[3]:+.0f}" if len(wp) > 3 else ""
        print(f"    {i:>2}. {wp[0]:>11.0f} {wp[1]:>11.0f} {wp[2]:>11.0f}{look}")

    if a.dry_run:
        print(f"\narm file written to {arm_path} - not launching")
        return 0

    cfg_path = game / 'cfg' / 'config.cfg'
    launch_cvars = ['engine_no_focus_sleep'] + [
        tok[1:] for tok in a.extra.split() if tok.startswith('+') and len(tok) > 1]
    saved_lines = {name: config_line(cfg_path, name) for name in launch_cvars}

    subprocess.Popen([
        steam_exe(), '-applaunch', '4000',
        '-windowed', '-w', str(a.width), '-h', str(a.height),
        '-novid', '-condebug',
        '+engine_no_focus_sleep', '0',
        '+gamemode', 'sandbox', '+map', a.map,
    ] + a.extra.split())

    start = time.time()
    seen = 0  # harness lines printed
    last_out = time.time()
    result = 'timed out'

    while True:
        if (out / 'DONE.txt').is_file():
            result = 'done'
            break
        if (out / 'FAIL.txt').is_file():
            result = 'the run reported a failure'
            break

        last_fail = out.parent / 'LAST_FAIL.txt'
        if last_fail.is_file():
            result = 'the harness could not start: ' + \
                last_fail.read_text(encoding='utf-8').strip().replace('\n', ' | ')
            try:
                last_fail.unlink()
            except OSError:
                pass
            break

        now = time.time()
        if now - start > a.timeout:
            result = f'timed out after {a.timeout}s'
            break
        if now - start > 120 and not gmod_running():
            result = "Garry's Mod closed before the run finished"
            break

        if log.is_file():
            try:
                lines = log.read_text(errors='replace').splitlines()
            except OSError:
                lines = []
            fresh = [ln for ln in lines if '[GTAV selftest]' in ln]
            if len(fresh) > seen:
                for ln in fresh[seen:]:
                    print('    ' + ln.strip())
                seen = len(fresh)
                last_out = now
            elif (out / 'STARTED.txt').is_file() and now - last_out > 90:
                print(f'    ... no harness output for {int(now - last_out)}s '
                      f'(started, but stalled)')
                last_out = now

        time.sleep(3)

    if gmod_running():
        print('waiting for Garry\'s Mod to close...')
        if not wait_closed(60):
            subprocess.run(['taskkill', '/F', '/IM', 'gmod.exe'], capture_output=True)
            wait_closed(15)
    for name, line in saved_lines.items():
        restore_config_line(cfg_path, name, line)

    print(f'\n{result}')
    if not (out / 'DONE.txt').is_file():
        started = out / 'STARTED.txt'
        if started.is_file():
            print(f"  the harness DID start: {started.read_text(encoding='utf-8').strip()!r}")
        else:
            print('  the harness never started - the arm file was either not read,')
            print('  no gtav map was loaded, or the map failed to load.')
            print(f'  see {log}')
    show(out)

    if log.is_file():
        try:
            (out / 'console.log').write_bytes(log.read_bytes())
        except OSError as e:
            print(f'  console.log NOT archived: {e}')

    ours, foreign = scan_lua_errors(log)
    if ours:
        total = sum(n for n, _ in ours)
        print(f'\n  LUA ERRORS from gtav ({total} lines, {len(ours)} distinct)'
              ' - the frame loop did not run as written:')
        for n, s in ours[:6]:
            print(f'    {n:>6}x  {s[:160]}')
        if len(ours) > 6:
            print(f'    ... and {len(ours) - 6} more distinct')
    else:
        print('\n  lua errors (gtav): none')
    if foreign:
        print(f'  lua errors (other addons, not judged): '
              f'{sum(n for n, _ in foreign)} lines, {len(foreign)} distinct')

    if a.open:
        subprocess.run(['explorer', str(out)])

    failed = ((out / 'FAIL.txt').is_file() or not (out / 'DONE.txt').is_file()
              or bool(ours))
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
