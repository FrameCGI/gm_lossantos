from __future__ import annotations

import argparse
import subprocess
import sys
from pathlib import Path

from . import city, crypto, detail

# shared road atlases in x64g
ROAD_ATLASES = (
    'globalroads', 'cityeastrd', 'citynorthrd', 'citywestrd',
    'beverlyhillsrd', 'bhmedianshare', 'downtownrd', 'freewayrd',
    'hollywoodrd', 'koreatownrd', 'portrd', 'sanpedrord', 'santamonicard',
    'southcentralrd', 'sunsetrd', 'venicebeachrd', 'fwyroads', 'chrddetail',
    'ch1', 'ch2', 'ch3roadnorth', 'ch3roadsouth',
)
ROAD_TEX_YTDS = [f'x64g.rpf:levels/gta5/generic/gtxd.rpf/{name}.ytd'
                 for name in ROAD_ATLASES]


def export_cmd(gta: Path, stage: Path, materials: str, outer_name: str, inner: str,
               stem: str):
    cmd = [sys.executable, '-m', 'exporter.gtav.cli', 'export',
           str(gta / outer_name),
           '--rpf', inner,
           '--merge-hi',
           '--textures', 'losantos',
           '--ydr', 'all',
           '--materials', materials,
           '--tile-size', '1024',
           '--name', f'{stem}_detail',
           '--out', str(stage),
           '--clear']
    for spec in ROAD_TEX_YTDS:
        cmd += ['--tex-ytd', spec]
    return cmd


def merge_block(stage: Path, root: Path, outer_name: str, inner: str, stem: str) -> bool:
    replace = city.staging_name(outer_name, inner)
    if not (stage / replace / 'index.json').is_file():
        print(f'  ! {stem}: no collision-only stage {replace}; assuming detail only',
              flush=True)
    try:
        detail.main(['--stage', str(stage), '--root', str(root),
                     '--replace', replace, '--detail', f'{stem}_detail'])
    except Exception as exc:
        print(f'  ! {stem}: merge failed: {exc}', flush=True)
        return False
    return True


def run_block(gta: Path, stage: Path, root: Path, materials: str,
              outer_name: str, inner: str, stem: str) -> bool:
    detail_name = f'{stem}_detail'
    if (stage / detail_name / 'index.json').is_file():
        print(f'{stem}: cached detail', flush=True)
    else:
        cmd = export_cmd(gta, stage, materials, outer_name, inner, stem)
        print(f'{stem}: exporting detail', flush=True)
        result = subprocess.run(cmd, cwd=str(Path(__file__).resolve().parents[2]))
        if result.returncode != 0:
            print(f'  ! {stem}: export failed ({result.returncode})', flush=True)
            return False
    return merge_block(stage, root, outer_name, inner, stem)


def run(todo, gta: Path, stage: Path, root: Path, materials: str) -> int:
    total = len(todo)
    done = 0
    for n, (outer_name, inner, _rpf_name, stem) in enumerate(todo, 1):
        print(f'[{n}/{total}] ', end='', flush=True)
        if run_block(gta, stage, root, materials, outer_name, inner, stem):
            done += 1
    print(f'detail batch: {done}/{total} blocks/road packs at full detail',
          flush=True)
    return 0 if done == total else 1


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument('--gta', default=r'D:\Games\Grand Theft Auto V Legacy')
    ap.add_argument('--stage', required=True)
    ap.add_argument('--root', required=True)
    ap.add_argument('--materials', required=True)
    ap.add_argument('--district', default='_cityw/beverly_01')
    ap.add_argument('--skip', action='append', default=[])
    ap.add_argument('--only', action='append', default=[])
    args = ap.parse_args(argv)
    gta = Path(args.gta)
    stage = Path(args.stage)
    root = Path(args.root)
    cipher = crypto.NgCipher.load(Path(__file__).with_name('keys'))
    blocks, roads = city.discover(gta, cipher)
    todo = []
    for outer_name, inner, rpf_name in blocks + roads:
        rel = inner[len('levels/gta5/'):]
        if not rel.startswith(args.district + '/'):
            continue
        stem = Path(rpf_name).stem
        if stem in args.skip:
            continue
        if args.only and stem not in args.only:
            continue
        todo.append((outer_name, inner, rpf_name, stem))
    print(f'detail batch: {len(todo)} blocks/road packs in {args.district}',
          flush=True)
    return run(todo, gta, stage, root, args.materials)


if __name__ == '__main__':
    raise SystemExit(main())
