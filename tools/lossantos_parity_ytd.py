import sys
import time
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from exporter.gtav import crypto, rpf
from exporter.gtav.ytd import decode_rgba, parse_ytd

KEYS = Path(__file__).resolve().parents[1] / 'exporter' / 'gtav' / 'keys'


def wsum64(b):
    a = np.frombuffer(b, np.uint8).astype(np.uint64)
    idx = np.arange(len(a), dtype=np.uint64)
    with np.errstate(over='ignore'):
        return int((a * (idx * np.uint64(2654435761) + np.uint64(1))).sum(dtype=np.uint64))


def walk(a, flt, out, inside=False):
    mine = inside or not flt or flt in a.name
    for i, e in enumerate(a.entries):
        if i == 0 or e.is_dir:
            continue
        if e.name.lower().endswith('.rpf'):
            c = a.open_child(e)
            walk(c, flt, out, mine)
            c.close()
            continue
        if not mine or e.kind != rpf.RESOURCE or not e.name.lower().endswith('.ytd'):
            continue
        for t in parse_ytd(a.read(e), rpf.size_from_flags(e.system_flags), e.name):
            img = decode_rgba(t)
            h = '-' if img is None else '%016x' % wsum64(np.ascontiguousarray(img).tobytes())
            out.write('%s:%s\t%d\t%d\t%d\t%d\t%s\n' % (e.path, t.name, t.width, t.height, t.format,
                                                       t.levels, h))


def main():
    t0 = time.time()
    a = rpf.RpfArchive(sys.argv[1], crypto.NgCipher.load(KEYS))
    with open(sys.argv[3], 'w', encoding='utf-8', newline='\n') as out:
        walk(a, sys.argv[2], out)
    print('python ytd dump in %.1f s' % (time.time() - t0))


if __name__ == '__main__':
    main()
