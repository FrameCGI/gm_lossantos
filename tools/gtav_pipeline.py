"""GTA5 -> GMod export/fix/validate pipeline. One command for the whole chain."""
from __future__ import annotations

import argparse
import csv
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
TEMP = Path(r'C:\Users\Frame\AppData\Local\Temp\opencode')
GTA = Path(r'D:\Games\Grand Theft Auto V Legacy')
GMOD = Path(r'C:\Program Files (x86)\Steam\steamapps\common\GarrysMod')
ADDON = GMOD / 'garrysmod' / 'addons' / 'lossantos'
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from exporter.gtav.paths import CONTENT_MATERIALS
MATERIALS = CONTENT_MATERIALS
ROOT = GMOD / 'garrysmod' / 'data' / 'lossantos' / 'lossantos'
STAGE = TEMP / 'gtav' / 'city_stage_full_fix1'
SELFTEST = REPO / 'tools' / 'selftest.py'
MASK_REDO = TEMP / 'mask_redo.py'
MASK_CALIB = TEMP / 'mask_calib.py'
MANIFEST = TEMP / 'vtf_src' / 'manifest_redo.json'

NOTES = [
    'capture-delta attribution (20261101_152640 vs 20261101_011655, wp_13 '
    'meanAbs 1.97) - RESOLVED, not a regression. Camera is byte-identical '
    'across the two runs (stats.csv x/y/z/eyePitch 18/eyeYaw 180 all equal), '
    'so every pixel delta is real content. Verified row->distance calibration '
    '(tools/capture_diff.py --calib, eye z 2000, ground ~0, vFOV 58.7): '
    'horizon row 190; rows 293-601 = lod1 (6.5k-16.3k units), 241-293 = lod2, '
    '216-241 = lod3, 190-216 = lod4. The wp_13 hotspot box (1408,320) spans '
    'rows 200-500 = ALL FOUR levels, but the changed content is rows 324-461 '
    '= LOD1 ONLY. Far-LOD decimation is therefore NOT implicated and nothing '
    'needs pulling back. Cause = the ROADS EXPORT (detail_all 01:22-01:59: '
    'tile rebuild + 1182 materials written 01:43-01:59) which landed AFTER '
    'the 011655 capture at 01:16:55 - that reference frame predates the roads '
    'work, so its delta against it IS the roads mesh+materials. '
    '20261101_011655 is NOT a valid visual reference for lod1+ content.',
    'wp_13 delta is a content GAIN, not a loss. Directional detail test '
    '(tools/capture_diff.py, holes = "ref detailed AND new flat", gains = the '
    'reverse): whole-frame mean detail 0.0610 -> 0.0619 (ratio 1.014), HOLE '
    'px 0.08%, GAIN px 0.01%. In the hotspot box: detail 0.1030 -> 0.0928 '
    '(0.90x) but holes only 0.09% of px, and the new frame is visibly RICHER '
    '- a road band along the shore, an extra palm tree, a red surface patch, '
    'and a larger/more resolved white building cluster. Do not read a '
    'run-vs-stale-baseline diff as geometry loss without running the '
    'directional test first: an eyeball "structures missing" verdict was wrong '
    'here.',
    'camera-basis trap in screen projection (cost hours): Source has Y as '
    'LEFT, so right = (sin y, -cos y, 0). Using (-sin y, cos y, 0) makes '
    'cross(right, fwd) return an up with NEGATIVE z, which mirrors the screen '
    'vertically and sends every ray downward - far LOD bands then gather zero '
    'tiles and the row/level mapping silently inverts. Self-check: the basis '
    'must imply horizon row ~175-190 at pitch 18 (exporter/gtav/lodproj.py '
    'asserts up.z > 0 and prints it).',
    'no geometry duplication in the 01:22 rebuild. lod0 tris inflated +130% '
    'vs baseline and several submeshes are exactly 50.0% "duplicates", which '
    'reads like double emission - but write_tile(double_sided) INTENTIONALLY '
    'emits [originals][mirrors] for $nocull materials, and a mirror has '
    'identical vertex POSITIONS. Splitting by normals (negated = mirror, '
    'identical = real dup) gives 1747/1747 and 869/873 mirrors, only 4-8 true '
    'dups per tile (0.08%), zero degenerate tris. Any dup detector must '
    'canonicalise corner order on position AND normal together, or every '
    'mirror is misread as duplication.',
    'material_flags.json is a no-op for rendering. It is a line-for-line port '
    'of the live Lua rules (bit0 glass = name has glass/window/shop OR VMT '
    '$translucent; bit1 nocull = glass|railing|fence|branch|leaf|hedge|ivy OR '
    'VMT $nocull), so a run with the sidecar and a run without resolve '
    'identical flags. Written 15:26:29 (only run that has it) - its timestamp '
    'is a red herring for capture deltas.',
    'run-dir picking must not sort by NAME (cost a crash loop). Probe run '
    'dirs sit alongside the timestamped sweeps and are named full_perf / '
    'full_probe / full_sheets3, so sorted() puts them AFTER every real run - '
    'and full_sheets3 has a stripped stats.csv with no eyePitch/eyeYaw at '
    'all, so "newest run" code that indexes those columns raises KeyError on '
    'exactly the run it picked. Rank by (has wp captures, has camera cols, '
    'timestamped name, mtime) instead - see tools/capture_diff.py pick_run(). '
    'Partial runs are also real: a run dir can hold fewer waypoint rows than '
    'the one being indexed.',
    'stale-page trap (root cause of "regen changed nothing"): pages/v_* '
    'bundle every tile of an 8x8 sector and SHADOW the loose files '
    '(sh_binary.lua PageRead). Three LOD regens were invisible to the game '
    'until pack build repacked the pages. Automated: stage_detail chains '
    'stage_pack; lod.py prints a NOTE after rewriting tiles. pack build is '
    'incremental (page_fresh), so running it unconditionally is cheap.',
    'lod.py nrm_step leak fixed 11-01: regen_one passed --coarse-nrm[0] (2) '
    'into lod1, but lod1 runs the FINE merge whose normal bucket default is '
    '4 (~14deg) - the override widened it to ~30deg and would silently '
    'decimate near-field detail on any lod1 regen. The override now applies '
    'only to coarse levels. Current on-disk lod1 was built with ns=4 (byte-'
    'verified against a rebuild: 96335 tris exactly), so no regen is owed.',
    'index slots: keys whose lod2-4 point at s_* (SLOD district models) are '
    'left alone by regen_one on purpose (family ownership); their t_*_0_2..4 '
    'files on disk are orphans (1206 in the Del Perro region) and are never '
    'rendered. Not stale data.',
]

DEFERRED = [
    'lighting + skydome (out of scope for this pass; road dark exposure is '
    'faithful GTA albedo (im_road_027b lum 28 vs sidewalk 128) lifted by '
    'GTA\'s in-game lighting - not a material error)',
    'cyan glow blob: CLOSED as not-exporter-data. Evidence - the field is '
    'camera/view-pinned (survives --pitch 20 while the scene shifts +404px; '
    'stats.csv eyePitch 20 confirmed on all wps), identical across 8700u '
    'travel + 120km altitude (corr 0.92-0.99), survives global $color red '
    'tint as an additive layer, invisible to every Lua draw wrap, survives '
    'cl_drawhud=0, AND survives cl_drawviewmodel=0 (corr 0.987-0.997, same '
    'centroid ~1029,689 - physgun viewmodel eliminated). Not skybox, water, '
    'playermodel, or any VMT key (all 26359 VMTs VertexLitGeneric, zero '
    '$envmap/$phong/$bumpmap). Engine/HUD-side. Evidence: '
    'triage/verify/blob_vmoff_cmp.png',
    'melting DIDIER SACHS storefront sign (wp_05): letters are a separate '
    'PROP mesh - none of the 42 tile materials/textures under the sign hold '
    'the letter art (raycast attributes letters to bt1_04_rsn_rls_mainwall_'
    '0001 = plain stone; brand/adscombo/dsachs atlases all ruled out). '
    'Melting = alpha-erosion of the sign prop\'s soft-fade texture. Needs '
    'prop-level (ydr) texture identification. Non-critical (one sign).',
    '13 keys whose lod1 slot points at s_*',
    'black shoreline band / sky scribble cutouts / fog-wall striping '
    '(deferred visuals)',
    'radio / NPCs',
    'road slab ovals in wide intersections look like GTA\'s own baked '
    'ground-blend patch art (dc_parking_blends / im_ground_overlay family); '
    'needs a same-location GTA screenshot to fully confirm. Road lane '
    'markings / crosswalks / kerbs / medians now come from road-pack '
    'drawables (detail_all processes blocks+roads; see world.CAT_MEDIAN) - '
    're-run the detail stage to rebuild tiles with them.',
    'teleport hitch (~2.8-3.2 s worst frame on waypoint jumps): engine-side '
    'texture/mesh upload + world rebase, not streamer work - the tracked '
    'phases sum to only ~59 ms on the worst tick (spike_attrib over '
    'stats.csv). Far-LOD decimation (lod3/lod4 position-only merge, '
    'exporter.gtav.lod --coarse-nrm 2,2,0,0) lightens what gets uploaded but '
    'the floor is engine cost. Harness already excludes the PNG-capture '
    'frame, so this is not selftest overhead.',
    'steady frame floor (~23 ms = engine 13.3 + render 5.2): uncorrelated '
    'with resident tris (r=0.34) and unchanged by decimation - engine/render '
    'bound, outside the chosen perf scope (far-LOD decimation + spike '
    'smoothing).',
]


def log2(run: Path, name: str, text: str):
    (run / name).write_text(text, encoding='utf-8', errors='replace')


def sh(cmd, run: Path, log_name: str, timeout=None):
    print(f'  $ {" ".join(str(c) for c in cmd)}', flush=True)
    res = subprocess.run([str(c) for c in cmd], cwd=str(REPO),
                         capture_output=True, text=True, timeout=timeout)
    text = (res.stdout or '') + (res.stderr or '')
    log2(run, log_name, text)
    return res.returncode, text


def stage_redo(run: Path) -> bool:
    if not MANIFEST.is_file():
        print('redo: hunting GTA sources (long)...', flush=True)
        code, _ = sh([sys.executable, '-X', 'utf8', MASK_REDO, 'hunt'],
                     run, 'redo_hunt.log', timeout=8 * 3600)
        if code != 0:
            print('RESULT STAGE=redo_hunt OK=0')
            return False
    print('redo: re-deriving decode-derived textures...', flush=True)
    code, text = sh([sys.executable, '-X', 'utf8', MASK_REDO, 'apply'],
                    run, 'redo_apply.log', timeout=8 * 3600)
    ok = code == 0 and 'missing sources 0' in text
    missing = re.search(r'missing sources (\d+)', text)
    print(f'RESULT STAGE=redo OK={1 if ok else 0} '
          f'MISSING={missing.group(1) if missing else "?"}')
    return ok


def stage_calib(run: Path) -> bool:
    code, text = sh([sys.executable, '-X', 'utf8', MASK_CALIB], run,
                    'calib.log', timeout=3600)
    ok = code == 0
    print(f'RESULT STAGE=calib OK={1 if ok else 0}')
    return ok


def stage_pack(run: Path) -> bool:
    code, text = sh([sys.executable, '-X', 'utf8', '-m', 'exporter.gtav.pack',
                     'build', ROOT, '--workers', '8'],
                    run, 'pack.log', timeout=4 * 3600)
    ok = code == 0 and 'packed ' in text
    packed = re.search(r'packed (\d+) files \(([\d.]+) GB\) into (\d+) pages',
                       text)
    print(f'RESULT STAGE=pack OK={1 if ok else 0} '
          f'FILES={packed.group(1) if packed else 0} '
          f'GB={packed.group(2) if packed else 0} '
          f'PAGES={packed.group(3) if packed else 0}')
    return ok


def stage_detail(run: Path, blocks=None) -> bool:
    cmd = [sys.executable, '-X', 'utf8', '-m', 'exporter.gtav.detail_all',
           '--stage', STAGE, '--root', ROOT, '--materials',
           str(MATERIALS), '--map-name', 'lossantos', '--redo']
    if blocks:
        for b in blocks:
            cmd += ['--only', b]
    code, text = sh(cmd, run, 'detail.log', timeout=24 * 3600)
    ok = code == 0 and 'OK=1' in text
    print(f'RESULT STAGE=detail OK={1 if ok else 0}')
    if ok:
        ok = stage_pack(run)
    return ok


def stage_test(run: Path) -> bool:
    stamp = time.strftime('%Y%m%d_%H%M%S')
    code, text = sh([sys.executable, '-X', 'utf8', SELFTEST, '--map',
                     'lossantos', '--run', f'pipeline_{stamp}'],
                    run, 'selftest.log', timeout=3600)
    console = GMOD / 'garrysmod' / 'console.log'
    if console.is_file():
        shutil.copy2(console, run / 'console.log')
        ctext = console.read_text(encoding='utf-8', errors='replace')
    else:
        ctext = text
    vtf_fail = ctext.count('Failed to load VTF')
    run_dir = (GMOD / 'garrysmod' / 'data' / 'gtav' / 'selftest'
               / f'pipeline_{stamp}')
    bad = badidx = lost = holes = -1
    done = run_dir / 'done.txt'
    if done.is_file():
        m = re.search(r'bad (\d+)', done.read_text())
        bad = int(m.group(1)) if m else -1
        m = re.search(r'badidx (\d+)', done.read_text())
        badidx = int(m.group(1)) if m else -1
        m = re.search(r'lostTris (\d+)', done.read_text())
        lost = int(m.group(1)) if m else -1
    hcsv = run_dir / 'holes.csv'
    if hcsv.is_file():
        rows = [ln.split(',') for ln in hcsv.read_text(
            encoding='utf-8').splitlines() if ln]
        head = rows[0]
        if 'misses' in head:
            mi = head.index('misses')
            holes = sum(int(r[mi]) for r in rows[1:] if len(r) > mi)
        else:
            holes = -1
    ok = (code == 0 and vtf_fail == 0 and bad == 0 and badidx == 0
          and lost == 0 and holes == 0)
    print(f'RESULT STAGE=test OK={1 if ok else 0} VTFFAIL={vtf_fail} '
          f'BAD={bad} BADIDX={badidx} LOST={lost} HOLES={holes} '
          f'RUNDIR={run_dir}')
    return ok


PERF_BASELINE = TEMP / 'pipeline_runs' / 'PERF_BASELINE.txt'
SELFTEST_ROOT = GMOD / 'garrysmod' / 'data' / 'gtav' / 'selftest'


def newest_selftest_run() -> Path | None:
    if not SELFTEST_ROOT.is_dir():
        return None
    runs = []
    for p in SELFTEST_ROOT.iterdir():
        s = p / 'stats.csv'
        if not (p.is_dir() and s.is_file()):
            continue
        try:
            head = s.open(encoding='utf-8', errors='replace').readline()
        except OSError:
            continue
        if 'frameAvgMs' not in head:
            continue
        runs.append((s.stat().st_mtime, p))
    return max(runs, key=lambda t: t[0])[1] if runs else None


def stage_perf(run: Path, baseline=None, accept=False) -> bool:
    import numpy as np

    sys.path.insert(0, str(REPO / 'tools'))
    import perf_analyze

    target = newest_selftest_run()
    if target is None:
        print('RESULT STAGE=perf OK=0 ERR=no selftest run with stats.csv')
        return False

    if accept:
        PERF_BASELINE.parent.mkdir(parents=True, exist_ok=True)
        PERF_BASELINE.write_text(str(target), encoding='utf-8')
        print(f'RESULT STAGE=perf OK=1 BASELINE_SET={target}')
        return True

    if baseline is None and PERF_BASELINE.is_file():
        baseline = PERF_BASELINE.read_text(encoding='utf-8').strip()
    if not baseline:
        print('RESULT STAGE=perf OK=0 ERR=no baseline (run with '
              '--accept-perf-baseline first)')
        return False

    base = Path(baseline)
    if not (base / 'stats.csv').is_file():
        cand = SELFTEST_ROOT / base.name
        base = cand if (cand / 'stats.csv').is_file() else base
    if not (base / 'stats.csv').is_file():
        print(f'RESULT STAGE=perf OK=0 ERR=baseline {baseline} has no stats.csv')
        return False
    head = (base / 'stats.csv').open(
        encoding='utf-8', errors='replace').readline()
    if 'frameAvgMs' not in head:
        print(f'RESULT STAGE=perf OK=0 ERR=baseline {base} has no frame '
              f'telemetry columns')
        return False

    bm = perf_analyze.summarize(base)
    nm = perf_analyze.summarize(target)
    out = [f'== perf: {target}', f'== baseline: {base}', '']
    import io
    import contextlib
    buf = io.StringIO()
    with contextlib.redirect_stdout(buf):
        perf_analyze.show(nm)
        print()
        perf_analyze.show(bm)
        print()
        perf_analyze.diff(bm, nm)
    out.append(buf.getvalue())
    log2(run, 'perf.txt', '\n'.join(out))
    print(buf.getvalue(), end='')

    def delta(k):
        b, n = bm.get(k), nm.get(k)
        if not isinstance(b, float) or not isinstance(n, float) or not b:
            return 0.0
        if not (np.isfinite(b) and np.isfinite(n)):
            return 0.0
        return (n - b) / b * 100.0

    core = ('steady_ms', 'frameMaxMs_worst', 'maxDtBuildMs_worst')
    dead = [k for k in core
            if not isinstance(nm.get(k), float) or not np.isfinite(nm[k])]
    if dead:
        print(f'RESULT STAGE=perf OK=0 ERR=no-telemetry ({",".join(dead)} '
              f'not finite in {target})')
        return False

    reg = [k for k in core if delta(k) > 25.0]
    far_before = sum(bm.get(f'lod{i}_tris_tick', 0.0) for i in (3, 4))
    far_after = sum(nm.get(f'lod{i}_tris_tick', 0.0) for i in (3, 4))
    ok = not reg
    print(f'RESULT STAGE=perf OK={1 if ok else 0} '
          f'STEADY={nm.get("steady_ms", float("nan")):.1f}ms '
          f'(base {bm.get("steady_ms", float("nan")):.1f}) '
          f'FRAMEMAX={nm.get("frameMaxMs_worst", float("nan")):.0f}ms '
          f'(base {bm.get("frameMaxMs_worst", float("nan")):.0f}) '
          f'BUILDMAX={nm.get("maxDtBuildMs_worst", float("nan")):.0f}ms '
          f'(base {bm.get("maxDtBuildMs_worst", float("nan")):.0f}) '
          f'FARTRIS={far_after:.0f} (base {far_before:.0f}) '
          f'REGRESSED={",".join(reg) if reg else "none"} '
          f'RUNDIR={target}')
    return ok


VISUAL_BASELINE = TEMP / 'pipeline_runs' / 'VISUAL_BASELINE.txt'
CAPTURE_DIFF = REPO / 'tools' / 'capture_diff.py'


def stage_visual(run: Path, baseline=None, accept=False) -> bool:
    if not CAPTURE_DIFF.is_file():
        print(f'RESULT STAGE=visual OK=0 ERR={CAPTURE_DIFF} missing')
        return False

    if accept:
        r = subprocess.run([sys.executable, '-X', 'utf8', str(CAPTURE_DIFF),
                            '--accept-visual-baseline'],
                           capture_output=True, text=True, errors='replace')
        out = (r.stdout or '') + (r.stderr or '')
        log2(run, 'visual.txt', out)
        print(out, end='')
        return r.returncode == 0

    if baseline is None and VISUAL_BASELINE.is_file():
        baseline = VISUAL_BASELINE.read_text(encoding='utf-8').strip()
    if not baseline:
        print('RESULT STAGE=visual OK=0 ERR=no visual baseline (run with '
              '--accept-visual-baseline first)')
        return False

    r = subprocess.run([sys.executable, '-X', 'utf8', str(CAPTURE_DIFF),
                        baseline, '--newest'],
                       capture_output=True, text=True, errors='replace')
    out = (r.stdout or '') + (r.stderr or '')
    log2(run, 'visual.txt', out)
    print(out, end='')
    ok = r.returncode == 0 and 'RESULT VIS OK=1' in out
    worst = None
    for line in out.splitlines():
        if not line.startswith('RESULT VIS WP='):
            continue
        m = re.search(r'holePct=([\d.]+)', line)
        if m and (worst is None or float(m.group(1)) > worst[0]):
            worst = (float(m.group(1)), line.split('WP=')[1].split('.')[0])
    print(f'RESULT STAGE=visual OK={1 if ok else 0} '
          f'VISUAL_BASELINE={Path(baseline).name} '
          f'WORST={worst[1] if worst else "none"} '
          f'WORST_HOLE_PCT={worst[0] if worst else 0:.2f} '
          f'RUNDIR={run}')
    return ok


def stage_report(run: Path, results) -> bool:
    lines = ['# GTA5 -> GMod pipeline report', '',
             f'run: {run}', f'when: {time.strftime("%Y-%m-%d %H:%M:%S")}', '']
    for name, ok in results:
        lines.append(f'* {"OK" if ok else "FAIL"} - {name}')
    lines += ['', '## Resolved forensics (do not re-derive)', '']
    for n in NOTES:
        lines.append(f'* {n}')
    lines += ['', '## Deferred (reported, not fixed)', '']
    for d in DEFERRED:
        lines.append(f'* {d}')
    extra = [run / n for n in ('perf.txt', 'visual.txt')
             if (run / n).is_file()]
    for f in sorted(run.glob('*.log')) + extra:
        tail = f.read_text(encoding='utf-8', errors='replace')[-800:]
        lines += ['', f'## tail of {f.name}', '', '```', tail.strip(), '```']
    log2(run, 'REPORT.md', '\n'.join(lines) + '\n')
    print(f'RESULT STAGE=report OK=1 FILE={run / "REPORT.md"}')
    return True


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('stage', nargs='*', default=None,
                    choices=['all', 'redo', 'calib', 'detail', 'pack', 'test',
                             'visual', 'perf', 'report'],
                    help='stage(s) to run, default all. Several may be given '
                         '(e.g. "visual perf") so one report records both '
                         'verdicts.')
    ap.add_argument('--blocks', nargs='*', help='detail: only these block stems')
    ap.add_argument('--run-dir', help='reuse an existing run directory')
    ap.add_argument('--perf-baseline',
                    help='perf: baseline selftest run dir (default: sticky '
                         'baseline in pipeline_runs/PERF_BASELINE.txt)')
    ap.add_argument('--accept-perf-baseline', action='store_true',
                    help='perf: record the newest selftest run as the perf '
                         'baseline instead of comparing')
    ap.add_argument('--visual-baseline',
                    help='visual: baseline selftest run dir (default: sticky '
                         'baseline in pipeline_runs/VISUAL_BASELINE.txt)')
    ap.add_argument('--accept-visual-baseline', action='store_true',
                    help='visual: record the newest capture run as the visual '
                         'baseline instead of comparing')
    args = ap.parse_args()

    run = Path(args.run_dir) if args.run_dir else (
        TEMP / 'pipeline_runs' / time.strftime('%Y%m%d_%H%M%S'))
    run.mkdir(parents=True, exist_ok=True)
    print(f'pipeline run dir: {run}', flush=True)

    results = []
    stages = list(args.stage) if args.stage else ['all']
    is_all = stages == ['all']
    if is_all:
        stages = ['redo', 'calib', 'pack', 'test', 'visual', 'perf', 'report']
    for name in stages:
        print(f'== stage {name}', flush=True)
        if name == 'redo':
            ok = stage_redo(run)
        elif name == 'calib':
            ok = stage_calib(run)
        elif name == 'detail':
            ok = stage_detail(run, args.blocks)
        elif name == 'pack':
            ok = stage_pack(run)
        elif name == 'test':
            ok = stage_test(run)
        elif name == 'visual':
            ok = stage_visual(run, args.visual_baseline,
                              args.accept_visual_baseline)
        elif name == 'perf':
            ok = stage_perf(run, args.perf_baseline, args.accept_perf_baseline)
        elif name == 'report':
            continue  # written once at the end
        else:
            print(f'pipeline: unknown stage {name}', flush=True)
            return 1
        results.append((name, ok))
        if not ok and is_all:
            print(f'pipeline: stage {name} failed - stopping', flush=True)
            stage_report(run, results)
            return 1
    stage_report(run, results)
    return 0 if all(ok for _, ok in results) else 1


if __name__ == '__main__':
    raise SystemExit(main())
