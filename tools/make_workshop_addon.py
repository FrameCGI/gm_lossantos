import shutil
import subprocess
import sys
from pathlib import Path

from PIL import Image

REPO = Path(__file__).resolve().parents[1]
WORK = REPO.parent / 'openworld_work'
OUT = WORK / 'workshop_addon'
GMA = WORK / 'gm_lossantos_base.gma'
ICON = WORK / 'gm_lossantos_workshop.jpg'
GMAD = Path(r'C:\Program Files (x86)\Steam\steamapps\common\GarrysMod\bin\gmad.exe')
ALLOWED = {'.lua', '.vmt', '.vtf', '.png'}


def main():
    if OUT.exists():
        shutil.rmtree(OUT)
    listed = subprocess.run(['git', 'ls-files', '--cached', '--others', '--exclude-standard',
                             'addon/lua', 'addon/materials'],
                            cwd=REPO, capture_output=True, text=True, check=True).stdout
    for rel in listed.splitlines():
        if 'selftest' in rel:
            continue
        dst = OUT / Path(rel).relative_to('addon')
        dst.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy(REPO / rel, dst)
    shutil.copy(REPO / 'addon' / 'addon.json', OUT / 'addon.json')

    files = [p for p in OUT.rglob('*') if p.is_file() and p.name != 'addon.json']
    bad = [p for p in files if p.suffix.lower() not in ALLOWED]
    if bad:
        raise SystemExit(f'not a base addon file: {bad}')

    r = subprocess.run([str(GMAD), 'create', '-folder', str(OUT), '-out', str(GMA)],
                       capture_output=True, text=True)
    print(r.stdout[-600:], r.stderr[-300:])
    if r.returncode != 0 or not GMA.is_file():
        raise SystemExit(f'gmad failed (exit {r.returncode})')
    Image.open(REPO / 'native' / 'inst' / 'payload' / 'gm_lossantos.png').convert('RGB') \
        .resize((512, 512), Image.LANCZOS).save(ICON, quality=92)
    print(f'{len(files)} files, {GMA.stat().st_size / 1024:.0f} KB -> {GMA}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
