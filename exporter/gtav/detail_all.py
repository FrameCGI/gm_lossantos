"""Full-detail (.ydr) pass over the WHOLE city, then LODs, SLODs and pages."""

from __future__ import annotations

import argparse
import re
import subprocess
import time
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

from . import city, crypto, detail_batch, lod, pack, slod
from . import materials as materials_mod

MERGED = '.merged_into_root'  # resume marker


# cli.py substitution lines
FALLBACK_RE = re.compile(r'^\s*(?:! no (\S+) in this archive; using'
                         r'|- dropped (\S+) geometry'
                         r'|= no (\S+) in this archive; using \S+_lod\s*$)')
REPO = Path(__file__).resolve().parents[2]


def _export(job):
    gta, stage, materials, outer_name, inner, stem = job
    log = stage / f'{stem}_detail.export.log'
    t0 = time.time()
    with open(log, 'w', encoding='utf-8', errors='replace') as fh:
        rc = subprocess.run(detail_batch.export_cmd(gta, stage, materials, outer_name,
                                                    inner, stem),
                            cwd=str(REPO), stdout=fh, stderr=subprocess.STDOUT).returncode
    return stem, rc, time.time() - t0


def _now_borrowable(stage, stem, materials):
    log = stage / f'{stem}_detail.export.log'
    try:
        lines = log.read_text(encoding='utf-8', errors='replace').splitlines()
    except OSError:
        return False
    for ln in lines:
        m = FALLBACK_RE.match(ln)
        if m:
            key = m.group(1) or m.group(2) or m.group(3)
            if materials_mod.find_installed(materials, key):
                return True
    return False


def run_parallel(todo, gta, stage, root, materials, jobs):
    failed = []
    need = {t[3] for t in todo if not (stage / f'{t[3]}_detail' / 'index.json').is_file()}
    print(f'detail_all: parallel x{jobs}: {len(need)} exports '
          f'({len(todo) - len(need)} cached), merges overlapped in block order', flush=True)

    def export_and_merge(items, label):
        bad = []
        t0 = time.time()
        with ThreadPoolExecutor(max_workers=jobs) as pool:
            futs = {s: pool.submit(_export, (gta, stage, materials, o, i, s))
                    for o, i, _r, s in items if s in need or label != 'export'}
            # merges stay serial, in block order
            for n, (outer_name, inner, _rpf, stem) in enumerate(items, 1):
                tag = f'[{label} {n}/{len(items)}] {stem}:'
                f = futs.get(stem)
                if f is not None:
                    _s, rc, secs = f.result()
                    if rc != 0:
                        bad.append(stem)
                        print(f'{tag} export FAILED rc {rc} - see {stem}_detail.export.log',
                              flush=True)
                        continue
                    what = f'export {secs:.0f}s'
                else:
                    what = 'cached export'
                ts = time.time()
                ok = detail_batch.merge_block(stage, root, outer_name, inner, stem)
                if ok:
                    (stage / f'{stem}_detail' / MERGED).write_text('')
                else:
                    bad.append(stem)
                left = (time.time() - t0) / n * (len(items) - n)
                print(f'{tag} {"ok" if ok else "merge FAILED"} ({what}, merge '
                      f'{time.time() - ts:.1f}s, ~{left / 60:.0f} min left)', flush=True)
        return bad

    bad = export_and_merge(todo, 'export')
    again = [t for t in todo if t[3] not in bad and _now_borrowable(stage, t[3], materials)]
    if again:
        print(f'detail_all: re-exporting {len(again)} blocks whose substitutions are '
              f'now installed', flush=True)
        bad += export_and_merge(again, 're-export')
    failed += bad
    return failed


def main(argv=None):
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--gta', default=r'D:\Games\Grand Theft Auto V Legacy')
    ap.add_argument('--stage', required=True,
                    help='staging directory holding one folder per block')
    ap.add_argument('--root', required=True,
                    help='data/lossantos/<map> directory')
    ap.add_argument('--materials', required=True,
                    help='addon materials folder the .ydr textures install into')
    ap.add_argument('--map-name', default='lossantos')
    ap.add_argument('--district', action='append', default=[],
                    help='only these district prefixes (repeatable), '
                         'e.g. _cityw/beverly_01')
    ap.add_argument('--skip', action='append', default=[])
    ap.add_argument('--only', action='append', default=[])
    ap.add_argument('--redo', action='store_true',
                    help='re-merge blocks that already carry the resume marker')
    ap.add_argument('--no-detail', action='store_true',
                    help='skip the .ydr pass and only refresh LOD/SLOD/pages')
    ap.add_argument('--no-lod', action='store_true')
    ap.add_argument('--no-slod', action='store_true')
    ap.add_argument('--no-pack', action='store_true')
    ap.add_argument('--jobs', type=int, default=1,
                    help='parallel block exports (merges stay serial, in block order); '
                         '1 = the old one-at-a-time loop')
    args = ap.parse_args(argv)

    gta = Path(args.gta)
    stage = Path(args.stage)
    root = Path(args.root)
    materials = args.materials

    failed = []
    if not args.no_detail:
        cipher = crypto.NgCipher.load(Path(__file__).with_name('keys'))
        blocks, roads = city.discover(gta, cipher)

        todo = []
        skipped_done = 0
        for outer_name, inner, rpf_name in blocks + roads:
            rel = inner[len('levels/gta5/'):]
            if args.district and not any(rel.startswith(d + '/')
                                         for d in args.district):
                continue
            stem = Path(rpf_name).stem
            if stem in args.skip:
                continue
            if args.only and stem not in args.only:
                continue
            if not args.redo and (stage / f'{stem}_detail' / MERGED).is_file():
                skipped_done += 1
                continue
            todo.append((outer_name, inner, rpf_name, stem))

        print(f'detail_all: {len(todo)} blocks/road packs to do, '
              f'{skipped_done} already merged', flush=True)
        if skipped_done and not todo:
            print('detail_all: nothing to do', flush=True)

        started = time.time()
        total = len(todo)
        if args.jobs > 1:
            failed += run_parallel(todo, gta, stage, root, materials, args.jobs)
        else:
            for n, (outer_name, inner, _rpf_name, stem) in enumerate(todo, 1):
                t0 = time.time()
                print(f'[{n}/{total}] ', end='', flush=True)
                ok = detail_batch.run_block(gta, stage, root, materials,
                                            outer_name, inner, stem)
                if ok:
                    (stage / f'{stem}_detail' / MERGED).write_text('')
                else:
                    failed.append(stem)
                rate = (time.time() - started) / n
                left = rate * (total - n)
                print(f'    {stem}: {"ok" if ok else "FAILED"} '
                      f'({time.time() - t0:.1f}s, ~{left / 60:.0f} min left)',
                      flush=True)

    if not args.no_lod:
        print('detail_all: regenerating LOD1-4', flush=True)
        if lod.main(['--root', str(root)]):
            print('detail_all: LOD regen FAILED', flush=True)
            return 1

    if not args.no_slod:
        print('detail_all: wiring district SLODs', flush=True)
        slod.main(['--gta', str(gta), '--root', str(root),
                   '--map-name', args.map_name, '--materials', materials])

    if not args.no_pack:
        print('detail_all: rebuilding page packs', flush=True)
        pack.main(['build', str(root)])

    if failed:
        print(f'detail_all: {len(failed)} block(s) FAILED: '
              f'{", ".join(failed[:20])}', flush=True)
    print(f'RESULT BLOCKS_FAILED={len(failed)} OK={1 if not failed else 0}',
          flush=True)
    print('detail_all: done', flush=True)
    return 1 if failed else 0


if __name__ == '__main__':
    raise SystemExit(main())
