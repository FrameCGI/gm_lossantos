import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from exporter.gtav import crypto, rpf
from exporter.gtav.resource import ResourceError
from exporter.gtav.ybn import parse_ybn

KEYS = Path(__file__).resolve().parents[1] / 'exporter' / 'gtav' / 'keys'
stats = {'files': 0, 'same': 0, 'diff': 0, 'tris': 0, 'maxerr': 0.0, 'empty': 0}


def walk(a, flt, cdir, inside=False):
    mine = inside or not flt or flt in a.name
    for i, e in enumerate(a.entries):
        if i == 0 or e.is_dir:
            continue
        if e.name.lower().endswith('.rpf'):
            try:
                c = a.open_child(e)
            except rpf.RpfError:
                continue
            walk(c, flt, cdir, mine)
            c.close()
            continue
        if not mine or e.kind != rpf.RESOURCE or not e.name.lower().endswith('.ybn'):
            continue
        stats['files'] += 1
        raw = (Path(cdir) / (e.name + '.tri')).read_bytes()
        # u64 count, f64 tris, u8 mats
        nt = int(np.frombuffer(raw[:8], '<u8')[0])
        ctri = np.frombuffer(raw[8:8 + nt * 72], '<f8').reshape(nt, 3, 3)
        cmat = np.frombuffer(raw[8 + nt * 72:8 + nt * 72 + nt], np.uint8)
        try:
            w = parse_ybn(a.read(e), rpf.size_from_flags(e.system_flags), e.name)
            ptri, pmat = w.triangles, w.material
        except ResourceError:  # python raises on 0 triangles
            ptri, pmat = np.zeros((0, 3, 3)), np.zeros(0, np.uint8)
            stats['empty'] += 1
        ok = ptri.shape == ctri.shape and np.array_equal(pmat, cmat)
        err = float(np.abs(ptri - ctri).max()) if ok and nt else 0.0
        if ok and err <= 1e-6:
            stats['same'] += 1
        else:
            stats['diff'] += 1
            if stats['diff'] <= 5:
                print('DIFF', e.path, ptri.shape, ctri.shape, err)
        stats['tris'] += nt
        stats['maxerr'] = max(stats['maxerr'], err)


def main():
    a = rpf.RpfArchive(sys.argv[1], crypto.NgCipher.load(KEYS))
    walk(a, sys.argv[2], sys.argv[3])
    print(stats)


if __name__ == '__main__':
    main()
