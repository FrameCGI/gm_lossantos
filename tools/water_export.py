import argparse
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO))
from exporter.gtav import crypto, rpf
from exporter.gtav.ytd import decode_rgba, parse_ytd, write_vtf

GTA = Path(r'D:\Games\Grand Theft Auto V Legacy')
KEYS = REPO / 'exporter' / 'gtav' / 'keys'
OUT = REPO / 'addon' / 'materials' / 'gtav' / 'water'
WANT = ('waterbump2', 'waternoise', 'oceanwaves', 'shorewaves')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--png', help='also write PNG previews here')
    a = ap.parse_args()
    c = crypto.NgCipher.load(KEYS)
    up = rpf.RpfArchive(GTA / 'update' / 'update.rpf', c)
    e = next(x for x in up.walk()
             if str(x.path).lower().endswith('x64/textures/graphics.ytd'))
    texs = {t.name.lower(): t for t in parse_ytd(up.read(e), rpf.size_from_flags(e.system_flags),
                                                 e.name)}
    OUT.mkdir(parents=True, exist_ok=True)
    for name in WANT:
        t = texs.get(name)
        if t is None:
            print('  ! missing', name)
            continue
        ok = write_vtf(OUT / (name + '.vtf'), t)
        if not ok:
            rgba = decode_rgba(t)
            ok = rgba is not None and write_vtf(OUT / (name + '.vtf'), t, rgba_map=rgba)
        print('  %-11s %4dx%-4d %-5s -> %s' % (name, t.width, t.height, t.format_name,
                                               'ok' if ok else 'FAILED'))
        if a.png:
            from PIL import Image
            rgba = decode_rgba(t)
            if rgba is not None:
                Path(a.png).mkdir(parents=True, exist_ok=True)
                img = rgba if isinstance(rgba, Image.Image) else Image.fromarray(rgba, 'RGBA')
                img.save(Path(a.png) / (name + '.png'))
    up.close()
    return 0


if __name__ == '__main__':
    sys.exit(main())
