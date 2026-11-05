"""Summarise a selftest run's performance telemetry, optionally vs a baseline."""
import argparse
import csv
import json
import sys
import warnings
from pathlib import Path

import numpy as np


def summarize(run: Path) -> dict:
    rows = list(csv.DictReader(open(run / 'stats.csv', newline='',
                                    encoding='utf-8')))
    if not rows:
        raise SystemExit(f'{run}: stats.csv missing or empty')

    def col(name):
        out = []
        for r in rows:
            try:
                out.append(float(r.get(name) or 'nan'))
            except ValueError:
                out.append(np.nan)
        return np.array(out)

    favg = col('frameAvgMs')
    steady = favg[favg < 50]
    hot = favg[favg >= 50]

    lod = {}
    for r in rows:
        s = r.get('lodTris') or ''
        if '/' not in s:
            continue
        try:
            parts = np.array([float(x) for x in s.split('/')])
        except ValueError:
            continue
        for i, v in enumerate(parts):
            lod[i] = lod.get(i, 0.0) + v
    n_lod = max(1, len(rows))

    with warnings.catch_warnings():
        warnings.simplefilter('ignore', RuntimeWarning)
        m = {
            'rows': len(rows),
            'frameAvgMs_mean': float(np.nanmean(favg)),
            'frameAvgMs_median': float(np.nanmedian(favg)),
            'frameP95Ms_mean': float(np.nanmean(col('frameP95Ms'))),
            'frameMaxMs_worst': float(np.nanmax(col('frameMaxMs'))),
            'steady_ms': float(np.nanmean(steady)) if len(steady) else float('nan'),
            'steady_fps': (float(1000 / np.nanmean(steady))
                           if len(steady) and np.nanmean(steady) > 0 else 0.0),
            'settle_ms': float(np.nanmean(hot)) if len(hot) else float('nan'),
            'maxDtMs_worst': float(np.nanmax(col('maxDtMs'))),
            'maxDtBuildMs_worst': float(np.nanmax(col('maxDtBuildMs'))),
            'maxDtWarmMs_worst': float(np.nanmax(col('maxDtWarmMs'))),
            'maxDtWrapMs_worst': float(np.nanmax(col('maxDtWrapMs'))),
            'tris_mean': float(np.nanmean(col('tris'))),
            'submeshes_mean': float(np.nanmean(col('submeshes'))),
            'renderCalls_rate': float(np.nanmean(np.diff(col('renderCalls')))),
        }
    for i in sorted(lod):
        m[f'lod{i}_tris_tick'] = lod[i] / n_lod
    m['_frameAvg_by_wp'] = {}
    for i, r in enumerate(rows):
        try:
            m['_frameAvg_by_wp'][str(r.get('wp') or i)] = float(favg[i])
        except (ValueError, IndexError):
            continue
    return m


HIGHER_BETTER = {'steady_fps', 'renderCalls_rate'}
UNITS = {'steady_fps': 'fps', 'steady_ms': 'ms', 'settle_ms': 'ms'}


def show(m: dict):
    print(f'rows {m["rows"]}')
    print(f'  frame avg/median mean : {m["frameAvgMs_mean"]:7.1f} / '
          f'{m["frameAvgMs_median"]:7.1f} ms')
    print(f'  steady floor          : {m["steady_ms"]:7.1f} ms '
          f'({m["steady_fps"]:.0f} fps)')
    settle = m["settle_ms"]
    print(f'  hot-row mean (>=50ms) : '
          f'{settle:7.1f} ms' if np.isfinite(settle) else
          '  hot-row mean (>=50ms) :      n/a (no row >= 50 ms)')
    print(f'  worst frame / maxDt   : {m["frameMaxMs_worst"]:7.1f} / '
          f'{m["maxDtMs_worst"]:7.1f} ms')
    print(f'  worst build/warm/wrap : {m["maxDtBuildMs_worst"]:7.1f} / '
          f'{m["maxDtWarmMs_worst"]:7.1f} / {m["maxDtWrapMs_worst"]:7.1f} ms')
    print(f'  resident tris/submesh : {m["tris_mean"]:7.0f} / '
          f'{m["submeshes_mean"]:7.0f}')
    for i in range(5):
        k = f'lod{i}_tris_tick'
        if k in m:
            print(f'  lod{i} tris/tick        : {m[k]:7.0f}')


def diff(base: dict, new: dict):
    keys = [k for k in new if isinstance(new.get(k), float)
            and k in base and isinstance(base.get(k), float)]
    print(f'\n{"metric":22s} {"baseline":>10} {"new":>10} {"delta":>10}')
    for k in keys:
        b, n = base[k], new[k]
        if not (np.isfinite(b) and np.isfinite(n)):
            continue
        d = n - b
        pct = (d / b * 100) if b else 0.0
        good = (d >= 0) if k in HIGHER_BETTER else (d <= 0)
        mark = 'ok' if good else 'WORSE'
        print(f'{k:22s} {b:10.1f} {n:10.1f} {pct:+9.1f}%  {mark}')

    bp = base.get('_frameAvg_by_wp') or {}
    np_ = new.get('_frameAvg_by_wp') or {}
    shared = [w for w in np_ if w in bp]
    if shared:
        def wkey(w):
            try:
                return (0, int(w))
            except ValueError:
                return (1, w)
        shared.sort(key=wkey)
        worse = []
        print('\nper-waypoint frameAvgMs (paired, ms):')
        for w in shared:
            b, n = bp[w], np_[w]
            if not (np.isfinite(b) and np.isfinite(n)):
                continue
            d = n - b
            flag = '  <-- WORSE' if d > 0.5 else ''
            if d > 0.5:
                worse.append(w)
            print(f'  wp {w:>3s}: {b:7.1f} -> {n:7.1f}  ({d:+6.1f}){flag}')
        both = [w for w in shared
                if bp[w] < 50 and np_[w] < 50
                and np.isfinite(bp[w]) and np.isfinite(np_[w])]
        if both:
            bm = float(np.mean([bp[w] for w in both]))
            nm = float(np.mean([np_[w] for w in both]))
            print(f'steady paired (<50ms in both, {len(both)} wps): '
                  f'{bm:.1f} -> {nm:.1f} ms ({nm - bm:+.1f})')
        crossed = [w for w in shared if (bp[w] < 50) != (np_[w] < 50)]
        if crossed:
            print(f'steady_ms membership differs at wp '
                  f'{", ".join(str(w) for w in crossed)} - the steady_ms '
                  f'rows above are NOT comparable across these runs')


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('run', help='selftest run dir (contains stats.csv)')
    ap.add_argument('--baseline', help='baseline run dir to compare against')
    ap.add_argument('--json', help='write the metrics as JSON to this path')
    args = ap.parse_args()

    run = Path(args.run)
    m = summarize(run)
    print(f'== perf summary: {run}')
    show(m)

    if args.baseline:
        b = Path(args.baseline)
        print(f'\n== baseline: {b}')
        bm = summarize(b)
        show(bm)
        diff(bm, m)

    if args.json:
        Path(args.json).write_text(json.dumps(m, indent=2), encoding='utf-8')
        print(f'\nwrote {args.json}')


if __name__ == '__main__':
    main()
