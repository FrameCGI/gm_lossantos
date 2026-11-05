import sys as _sys
from pathlib import Path as _P
_sys.path.insert(0, str(_P(__file__).resolve().parents[1]))
from exporter.gtav import paths as _paths
import sys
from pathlib import Path

DEFAULT = str(_paths.GTA_MATERIALS)  # exporter/gtav/paths.py

LINE = '    "$vertexcolor" "1"\n'


def patch_text(text):
    for raw in text.splitlines():
        s = raw.strip().lower()
        if s.startswith('"$vertexcolor"') or s.startswith('$vertexcolor'):
            return None

    lines = text.splitlines(keepends=True)
    out = []
    inserted = False
    brace_seen = False
    for raw in lines:
        out.append(raw)
        if inserted:
            continue
        s = raw.strip().lower()
        if not brace_seen:
            if s == '{':
                brace_seen = True
            continue
        if s.startswith('"$basetexture"') or s.startswith('$basetexture'):
            out.append(LINE)
            inserted = True
    if not inserted:
        if brace_seen:
            for i, raw in enumerate(out):
                if raw.strip() == '{':
                    out.insert(i + 1, LINE)
                    inserted = True
                    break
        else:
            return None  # not a KeyValues VMT
    return ''.join(out)


def main():
    root = Path(sys.argv[1] if len(sys.argv) > 1 else DEFAULT)
    if not root.is_dir():
        print(f'not a directory: {root}')
        return 1

    patched = same = odd = 0
    for p in root.rglob('*.vmt'):
        try:
            text = p.read_text(encoding='utf-8', errors='surrogateescape')
        except OSError:
            odd += 1
            continue
        new = patch_text(text)
        if new is None:
            if any(l.strip().lower().startswith(('$vertexcolor', '"$vertexcolor"'))
                   for l in text.splitlines()):
                same += 1
            else:
                odd += 1
            continue
        p.write_text(new, encoding='utf-8', errors='surrogateescape')
        patched += 1

    print(f'{root}')
    print(f'  patched {patched}, already had it {same}, left alone {odd}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
