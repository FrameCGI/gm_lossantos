import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from exporter.gtav import crypto, rpf, rpfbin

KEYS = Path(__file__).resolve().parents[1] / 'exporter' / 'gtav' / 'keys'
MASK = (1 << 64) - 1


def fnv1a64(data):
    h = 1469598103934665603
    for b in data:
        h = ((h ^ b) * 1099511628211) & MASK
    return h


def walk(a, out, stats):
    for i, e in enumerate(a.entries):
        if i == 0 or e.is_dir:
            continue
        if e.name.lower().endswith('.rpf'):
            child = a.open_child(e)
            walk(child, out, stats)
            child.close()
            continue
        enc = int(e.kind == rpf.BINARY and e.encrypted)
        try:
            data = rpfbin.read_binary(a, e) if enc else a.read(e)
        except Exception:
            out.write('%s\tERROR\t-\t%d\t-\n' % (e.path, enc))
            continue
        stats[0] += 1
        stats[1] += len(data)
        out.write('%s\t%d\t%016x\t%d\t%s\n' % (e.path, len(data), fnv1a64(data), enc, data[:4].hex()))


def main():
    arc, dump = sys.argv[1], sys.argv[2]
    a = rpf.RpfArchive(arc, crypto.NgCipher.load(KEYS))
    stats = [0, 0]
    t0 = time.time()
    with open(dump, 'w', encoding='utf-8', newline='\n') as out:
        walk(a, out, stats)
    print('python: files %d out %.1f MB in %.1f s' % (stats[0], stats[1] / 2**20, time.time() - t0))


if __name__ == '__main__':
    main()
