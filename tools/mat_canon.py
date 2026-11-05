import argparse
import hashlib
import os
import sys
from collections import defaultdict

GMOD = r'C:\Program Files (x86)\Steam\steamapps\common\GarrysMod\garrysmod'
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from exporter.gtav.paths import CONTENT_MATERIALS
MATS = str(CONTENT_MATERIALS)


def vmt_params(path):
    params = {}
    for ln in open(path, encoding='utf-8', errors='replace'):
        parts = ln.strip().replace('"', ' ').split()
        if len(parts) >= 2 and parts[0].startswith('$'):
            params[parts[0].lower()] = parts[1].lower()
        elif len(parts) == 1 and parts[0] not in ('{', '}'):
            params['__shader'] = parts[0].lower()
    return params


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--map', default='lossantos')
    ap.add_argument('--root', help='write <root>/mat_canon.txt instead of data/lossantos/<map>/')
    ap.add_argument('--dry-run', action='store_true')
    a = ap.parse_args()

    root = os.path.join(MATS, 'models', 'gtav')
    groups = defaultdict(list)
    hcache = {}
    total = 0
    for d in sorted(os.listdir(root)):
        dp = os.path.join(root, d)
        if not os.path.isdir(dp):
            continue
        for f in os.listdir(dp):
            if not f.lower().endswith('.vmt'):
                continue
            total += 1
            name = 'models/gtav/%s/%s' % (d, f[:-4])
            p = vmt_params(os.path.join(dp, f))
            base = p.pop('$basetexture', None)
            p.pop('$surfaceprop', None)
            if not base:
                continue
            vtf = os.path.join(MATS, base.replace('/', os.sep) + '.vtf')
            h = hcache.get(vtf)
            if h is None:
                try:
                    with open(vtf, 'rb') as fh:
                        h = hashlib.sha1(fh.read()).hexdigest()
                except OSError:
                    continue
                hcache[vtf] = h
            groups[(h, tuple(sorted(p.items())))].append(name)

    lines = []
    for names in groups.values():
        names.sort()
        for n in names[1:]:
            lines.append('%s\t%s' % (n, names[0]))
    lines.sort()
    print('materials %d, distinct %d, aliases %d (%.0f%%)' % (
        total, len(groups), len(lines), 100.0 * len(lines) / max(total, 1)))
    out = (os.path.join(a.root, 'mat_canon.txt') if a.root
           else os.path.join(GMOD, 'data', 'lossantos', a.map, 'mat_canon.txt'))
    if a.dry_run:
        print('dry run - not writing', out)
        return 0
    tmp = out + '.tmp'
    with open(tmp, 'w', encoding='utf-8', newline='\n') as fh:
        fh.write('\n'.join(lines) + '\n')
    os.replace(tmp, out)
    print('wrote', out)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
