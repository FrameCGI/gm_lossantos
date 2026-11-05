"""Full re-export of the streamed map into a SEPARATE root, then (on request)"""
import argparse
import os
import shutil
import subprocess
import sys
import time
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
GMOD = Path(r'C:\Program Files (x86)\Steam\steamapps\common\GarrysMod\garrysmod')
DATA = GMOD / 'data' / 'lossantos'
LIVE = DATA / 'lossantos'
NEW = DATA / 'lossantos_regen'
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from exporter.gtav.paths import CONTENT_MATERIALS
MATS = CONTENT_MATERIALS
GTA = Path(r'D:\Games\Grand Theft Auto V Legacy')
OLD_STAGE = Path(os.environ['TEMP']) / 'opencode' / 'gtav' / 'city_stage_full_fix1'
NEW_STAGE = Path(r'C:\Users\Frame\Downloads\openworld_work\stage_regen1')
PY = [sys.executable, '-X', 'utf8']
EXCLUDE_DIRS = ['regrid_tmp', 'tiles_bak_regrid', 'tiles_bak_fixbeach', '.regen']
JOBS = max(1, min(6, (os.cpu_count() or 4) // 2))  # parallel block exports

STEPS = ['stage', 'root', 'detail', 'sweep', 'lod', 'slod', 'regrid', 'verify',
         'beach', 'water', 'sidecars', 'pack', 'shadow', 'content']


def log(msg):
    line = time.strftime('%H:%M:%S ') + msg
    print(line, flush=True)
    (NEW / '.regen').mkdir(parents=True, exist_ok=True)
    with open(NEW / '.regen' / 'log.txt', 'a', encoding='utf-8') as fh:
        fh.write(line + '\n')


def run(cmd, cwd=REPO, ok_codes=(0,)):
    log('$ ' + ' '.join(str(c) for c in cmd))
    t0 = time.time()
    with open(NEW / '.regen' / 'log.txt', 'a', encoding='utf-8') as fh:
        r = subprocess.run([str(c) for c in cmd], cwd=str(cwd), stdout=fh,
                           stderr=subprocess.STDOUT)
    log(f'  -> exit {r.returncode} ({(time.time() - t0) / 60:.1f} min)')
    if r.returncode not in ok_codes:
        raise SystemExit(f'step failed: {cmd[:4]} exit {r.returncode}')


def robocopy(src, dst, extra):
    run(['robocopy', src, dst, '/E', '/NFL', '/NDL', '/NJH', '/NP', '/R:1', '/W:1',
         '/MT:16'] + extra, ok_codes=tuple(range(8)))


def gmod_running():
    out = subprocess.run(['tasklist', '/FI', 'IMAGENAME eq gmod.exe'],
                         capture_output=True, text=True).stdout
    return 'gmod.exe' in out.lower()


def step_stage():
    robocopy(OLD_STAGE, NEW_STAGE, ['/XD', '*_detail'])


def step_root():
    robocopy(LIVE, NEW, ['/XD'] + EXCLUDE_DIRS)


def step_detail():
    run(PY + ['-m', 'exporter.gtav.detail_all', '--gta', GTA, '--stage', NEW_STAGE,
              '--root', NEW, '--materials', MATS, '--no-lod', '--no-slod', '--no-pack',
              '--jobs', str(JOBS)])


def step_sweep():
    run(PY + ['-m', 'exporter.gtav.detail', '--stage', NEW_STAGE, '--root', NEW, '--sweep'])


def step_lod():
    run(PY + ['-m', 'exporter.gtav.lod', '--root', NEW])


def step_slod():
    run(PY + ['-m', 'exporter.gtav.slod', '--gta', GTA, '--root', NEW,
              '--map-name', 'lossantos', '--materials', MATS])


def step_regrid():
    run(PY + ['tools/regrid_tiles.py', '--root', NEW, '--apply'])


def step_verify():
    run(PY + ['tools/regrid_verify.py', '--strict', NEW])


def step_beach():
    run(PY + ['tools/fix_beach.py', '--no-backup', NEW])


def step_water():
    run(PY + ['-m', 'exporter.gtav.water', '--root', NEW, '--materials', MATS])


def step_sidecars():
    run(PY + ['tools/material_flags.py', '--root', NEW])
    run(PY + ['tools/tile_bounds.py', '--root', NEW])
    run(PY + ['tools/mat_prefetch.py', '--map-dir', NEW, '--out', NEW / 'tile_materials.dat'])
    run(PY + ['tools/patch_vmt_shader.py', '--to', 'unlit'])
    run(PY + ['tools/fix_beach_vmt.py'])
    run(PY + ['tools/mat_canon.py', '--root', NEW])


def step_pack():
    run(PY + ['-m', 'exporter.gtav.pack', 'build', NEW])


def step_content():
    run(PY + ['tools/pack_content.py'])


def step_shadow():
    run([REPO / 'native' / 'build' / 'gtav_bake.exe', NEW, NEW / 'sun_shadow.gvsd',
         '16', '256', '225', '52'])


def cmd_run(a):
    if gmod_running():
        log('note: GMod is running - fine for the export, but the swap needs it closed')
    start = STEPS.index(a.from_step) if a.from_step else 0
    for i, s in enumerate(STEPS):
        marker = NEW / '.regen' / f'{s}.done'
        if i < start and marker.exists():
            continue
        if marker.exists() and not a.from_step:
            log(f'[{s}] already done')
            continue
        if a.until and STEPS.index(s) > STEPS.index(a.until):
            break
        log(f'[{s}] start')
        globals()['step_' + s]()
        marker.parent.mkdir(parents=True, exist_ok=True)
        marker.write_text(time.strftime('%Y-%m-%d %H:%M:%S'))
        log(f'[{s}] done')
    log('regen: all requested steps done')


def cmd_status(_a):
    for s in STEPS:
        m = NEW / '.regen' / f'{s}.done'
        print('%-9s %s' % (s, m.read_text() if m.exists() else '-'))


def cmd_swap(_a):
    if gmod_running():
        raise SystemExit('close Garry\'s Mod first')
    missing = [s for s in STEPS if not (NEW / '.regen' / f'{s}.done').exists()]
    if missing:
        raise SystemExit(f'not all steps done: {missing}')
    backup = DATA / ('lossantos_prev_' + time.strftime('%Y%m%d_%H%M%S'))
    LIVE.rename(backup)
    NEW.rename(LIVE)
    print(f'swapped: live -> {backup.name}, regen -> lossantos')


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest='cmd', required=True)
    r = sub.add_parser('run')
    r.add_argument('--from', dest='from_step', choices=STEPS)
    r.add_argument('--until', choices=STEPS)
    sub.add_parser('status')
    sub.add_parser('swap')
    a = ap.parse_args()
    {'run': cmd_run, 'status': cmd_status, 'swap': cmd_swap}[a.cmd](a)


if __name__ == '__main__':
    main()
