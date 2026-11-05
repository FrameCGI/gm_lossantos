import sys as _sys
from pathlib import Path as _P
_sys.path.insert(0, str(_P(__file__).resolve().parents[1]))
from exporter.gtav import paths as _paths
import sys
from pathlib import Path

DEFAULT = str(_paths.GTA_MATERIALS)  # exporter/gtav/paths.py

UNLIT_FLAGS = ['    "$model" "1"', '    "$vertexcolor" "1"']
VERTEXALPHA_FLAG = '    "$vertexalpha" "1"'


def has_param(text, name):
    key = f'"${name}"'
    for raw in text.splitlines():
        s = raw.strip().lower()
        if s.startswith(key.lower()) or s.startswith(f'${name}'.lower()):
            return True
    return False


def param_on(text, name):
    key = f'${name}'.lower()
    for raw in text.splitlines():
        parts = raw.strip().replace('"', ' ').split()
        if len(parts) >= 2 and parts[0].lower() == key:
            return parts[1] not in ('0', '0.0')
    return False


def wants_vertexalpha(text):
    return param_on(text, 'translucent') or param_on(text, 'additive')


def is_param_line(raw, name):
    parts = raw.strip().replace('"', ' ').split()
    return bool(parts) and parts[0].lower() == f'${name}'.lower()


def patch_text(text, to):
    lines = text.splitlines(keepends=True)
    want_shader = '"UnlitGeneric"' if to == 'unlit' else '"VertexLitGeneric"'

    out = []
    shader_done = False
    brace_idx = None
    for raw in lines:
        s = raw.strip().lower()
        if not shader_done and s in ('"unlitgeneric"', '"vertexlitgeneric"',
                                     'unlitgeneric', 'vertexlitgeneric'):
            nl = '\r\n' if raw.endswith('\r\n') else '\n'
            out.append(want_shader + nl)
            shader_done = True
            continue
        out.append(raw)
        if brace_idx is None and s == '{':
            brace_idx = len(out) - 1

    if not shader_done or brace_idx is None:
        return None, 'odd'  # not a shader VMT we understand

    if to == 'unlit':
        insert_at = brace_idx + 1
        for flag in UNLIT_FLAGS:
            name = flag.split('"')[1].lstrip('$')
            if not has_param(''.join(out), name):
                out.insert(insert_at, flag + '\n')
                insert_at += 1
        if wants_vertexalpha(''.join(out)):
            if not has_param(''.join(out), 'vertexalpha'):
                out.insert(insert_at, VERTEXALPHA_FLAG + '\n')
        else:
            out = [raw for raw in out if not is_param_line(raw, 'vertexalpha')]

    return ''.join(out), 'ok'


def main():
    to = 'unlit'
    root = Path(DEFAULT)
    dry = False
    args = sys.argv[1:]
    i = 0
    while i < len(args):
        if args[i] == '--dry-run':
            dry = True
            i += 1
        elif args[i] == '--to' and i + 1 < len(args):
            to = args[i + 1].lower()
            i += 2
        else:
            root = Path(args[i])
            i += 1
    if to not in ('unlit', 'vlg'):
        print('--to must be unlit or vlg')
        return 1
    if not root.is_dir():
        print(f'not a directory: {root}')
        return 1

    patched = same = odd = va_kept = va_dropped = 0
    for p in root.rglob('*.vmt'):
        try:
            text = p.read_text(encoding='utf-8', errors='surrogateescape')
        except OSError:
            odd += 1
            continue
        new, status = patch_text(text, to)
        if new is None:
            odd += 1
            continue
        if to == 'unlit':
            if has_param(new, 'vertexalpha'):
                va_kept += 1
            elif has_param(text, 'vertexalpha'):
                va_dropped += 1
        if new != text:
            if not dry:
                p.write_text(new, encoding='utf-8', errors='surrogateescape')
            patched += 1
        else:
            same += 1

    print(f'{root}')
    print(f'  -> {to}{" (dry run)" if dry else ""}: patched {patched}, already {same}, left alone {odd}')
    if to == 'unlit':
        print(f'     $vertexalpha kept on {va_kept} translucent/additive, dropped from {va_dropped} opaque')
    return 0


if __name__ == '__main__':
    sys.exit(main())
