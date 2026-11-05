import argparse
import shutil
import subprocess
import sys
import time
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO))
from exporter.gtav import city, crypto, detail_batch

GTA = Path(r'D:\Games\Grand Theft Auto V Legacy')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('work')
    ap.add_argument('stems', nargs='+')
    ap.add_argument('--jobs', type=int, default=3)
    args = ap.parse_args()
    work = Path(args.work)
    (work / 'stage').mkdir(parents=True, exist_ok=True)
    blocks, roads = city.discover(GTA, crypto.NgCipher.load(REPO / 'exporter' / 'gtav' / 'keys'))
    where = {Path(r).stem: (o, i) for o, i, r in blocks + roads}

    def one(stem):
        outer, inner = where[stem]
        mats = work / f'mats_{stem}'
        if mats.exists():
            shutil.rmtree(mats)
        mats.mkdir()
        cmd = detail_batch.export_cmd(GTA, work / 'stage', str(mats), outer, inner, stem)
        t0 = time.time()
        with open(work / f'{stem}.log', 'w', encoding='utf-8') as log:
            rc = subprocess.run(cmd, cwd=str(REPO), stdout=log, stderr=subprocess.STDOUT).returncode
        return stem, rc, time.time() - t0

    with ThreadPoolExecutor(args.jobs) as pool:
        for stem, rc, dt in pool.map(one, args.stems):
            print(f'{stem}: exit {rc} in {dt:.1f} s', flush=True)


if __name__ == '__main__':
    main()
