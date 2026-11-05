import io
import json
import sys
from pathlib import Path

import numpy as np
from PIL import Image

REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO))
sys.path.insert(0, str(REPO / 'tools'))
from exporter.gtav import paths
from lossantos_parity_content import read_gma


def main():
    out = Path(sys.argv[1])
    files, bad = {}, 0
    for g in sorted(out.glob('lossantos_content_radio_*.gma')):
        name, f, bad_crc, tail_ok = read_gma(g)
        print(f'{g.name}: {len(f)} files, CRC errors {bad_crc}, trailing CRC {"ok" if tail_ok else "BAD"}')
        bad += bad_crc + (not tail_ok)
        files.update(f)
    live_root = paths.ADDON_RADIO_MANIFESTS
    worst = []
    for man in sorted((out / 'manifests').glob('*/manifest.json')):
        st = man.parent.name
        c = json.loads(man.read_text(encoding='utf-8'))
        lp = live_root / st / 'manifest.json'
        if not lp.is_file():
            print(f'{st}: no live manifest')
            continue
        p = json.loads(lp.read_text(encoding='utf-8'))
        diffs = []
        for k in ('station', 'name', 'kind'):
            if c.get(k) != p.get(k):
                diffs.append(f'{k}: py {p.get(k)!r} C {c.get(k)!r}')
        cf, pf = c.get('files', {}), p.get('files', {})
        if set(cf) != set(pf):
            diffs.append(f'files: only py {sorted(set(pf) - set(cf))[:5]} only C {sorted(set(cf) - set(pf))[:5]}')
        nsec = 0
        for stem in set(cf) & set(pf):
            a = [(x['path'], x['seconds']) for x in pf[stem]]
            b = [(x['path'], x['seconds']) for x in cf[stem]]
            if [x[0] for x in a] != [x[0] for x in b]:
                diffs.append(f'paths differ for {stem}')
            elif a != b:
                nsec += 1
        if nsec:
            diffs.append(f'seconds differ in {nsec} files')
        ct, pt = c.get('titles', {}), p.get('titles', {})
        if ct != pt:
            ks = sorted(set(ct) | set(pt))
            d = [k for k in ks if ct.get(k) != pt.get(k)]
            diffs.append(f'titles differ for {len(d)}: ' + '; '.join(f'{k}: py {pt.get(k)} C {ct.get(k)}' for k in d[:3]))
        # ogg sizes
        n_ogg = miss = 0
        ratios = []
        for stem, clips in cf.items():
            for x in clips:
                gp = f'sound/lossantos_radio/{st}/{x["path"]}'
                n_ogg += 1
                if gp not in files:
                    miss += 1
                    continue
                pyf = paths.CONTENT / gp
                if pyf.is_file():
                    ratios.append(len(files[gp]) / pyf.stat().st_size)
        if miss:
            diffs.append(f'{miss} oggs missing from the GMA')
        r = np.array(ratios) if ratios else np.array([1.0])
        worst.append((r.min(), st))
        print(f'{st:24s} {"OK" if not diffs else "DIFF"}  clips {n_ogg}, ogg size C/py median {np.median(r):.4f} '
              f'min {r.min():.4f} max {r.max():.4f}')
        for d in diffs:
            print('    ' + d)
    # logos
    pngs = {k: v for k, v in files.items() if k.endswith('.png')}
    same = 0
    for k, v in sorted(pngs.items()):
        pyf = paths.CONTENT / k
        if not pyf.is_file():
            print('  logo without python copy:', k)
            continue
        a = np.asarray(Image.open(pyf).convert('RGBA'))
        b = np.asarray(Image.open(io.BytesIO(v)).convert('RGBA'))
        if a.shape == b.shape and (a == b).all():
            same += 1
        else:
            print('  logo differs:', k, a.shape, b.shape)
    print(f'logos: {same}/{len(pngs)} pixel-identical')
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
