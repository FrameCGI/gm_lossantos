import argparse
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
SRC = REPO / 'shaders'
SC = Path(r'C:\Users\Frame\Downloads\openworld_work\shadercompile\ShaderCompile.exe')
FXC = Path(r'C:\Program Files (x86)\Steam\steamapps\common\GarrysMod\garrysmod\shaders\fxc')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--only', help='file name prefix filter')
    a = ap.parse_args()
    if not SC.is_file():
        sys.exit(f'ShaderCompile missing: {SC}')
    files = sorted(SRC.glob('*.hlsl'))
    if a.only:
        files = [f for f in files if f.name.startswith(a.only)]
    FXC.mkdir(parents=True, exist_ok=True)
    bad = 0
    for f in files:
        stem = f.stem  # gtav_water_ps30
        ver = stem.rsplit('_', 1)[1][2:]  # 30 / 20b
        with tempfile.TemporaryDirectory() as td:
            work = Path(td)
            shutil.copy(f, work / f.name)
            r = subprocess.run([str(SC), '/O', '3', '-ver', ver, '-shaderpath', str(work),
                                f.name], cwd=str(work), capture_output=True, text=True)
            out = list((work / 'shaders' / 'fxc').glob('*.vcs'))
            if r.returncode != 0 or not out:
                bad += 1
                print(f'FAIL {f.name}\n{r.stdout[-2000:]}\n{r.stderr[-2000:]}')
                continue
            for v in out:
                shutil.copy(v, FXC / v.name)
                print(f'ok   {f.name} -> {FXC / v.name} ({v.stat().st_size} B)')
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
