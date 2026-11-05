import struct
import sys
import zlib
from collections import Counter
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from exporter.gtav import paths


def read_gma(path):
    raw = Path(path).read_bytes()
    assert raw[:4] == b'GMAD' and raw[4] == 3, 'bad magic/version'
    pos = 5 + 8 + 8
    assert raw[pos] == 0, 'required content list not empty'
    pos += 1
    def cstr():
        nonlocal pos
        end = raw.index(b'\0', pos)
        s = raw[pos:end].decode('utf-8')
        pos = end + 1
        return s
    name, desc, author = cstr(), cstr(), cstr()
    pos += 4
    entries = []
    while True:
        n, = struct.unpack_from('<I', raw, pos)
        pos += 4
        if n == 0:
            break
        p = cstr()
        size, crc = struct.unpack_from('<qI', raw, pos)
        pos += 12
        entries.append((p, size, crc))
    files = {}
    bad_crc = 0
    for p, size, crc in entries:
        data = raw[pos:pos + size]
        pos += size
        if zlib.crc32(data) & 0xFFFFFFFF != crc:
            bad_crc += 1
        files[p] = data
    tail, = struct.unpack_from('<I', raw, pos)  # crc of the whole gma
    whole = zlib.crc32(raw[:pos]) & 0xFFFFFFFF
    assert pos + 4 == len(raw), 'trailing bytes'
    return name, files, bad_crc, tail == whole


def vtf_head(b):
    # 16 w h, 20 flags, 52 format, 56 mips
    w, h = struct.unpack_from('<HH', b, 16)
    flags, = struct.unpack_from('<I', b, 20)
    fmt, = struct.unpack_from('<I', b, 52)
    return (w, h, fmt, b[56], flags)


def main():
    gma = sys.argv[1]
    root = Path(sys.argv[2]) if len(sys.argv) > 2 else paths.CONTENT
    name, files, bad_crc, tail_ok = read_gma(gma)
    print(f'{name}: {len(files)} files, per-file CRC errors {bad_crc}, trailing CRC {"ok" if tail_ok else "BAD"}')
    st = Counter()
    vmt_diff, vtf_diff = [], []
    for p, data in sorted(files.items()):
        loose = root / p
        kind = p.rsplit('.', 1)[1]
        if not loose.is_file():
            st[kind + ' new (no python copy)'] += 1
            continue
        old = loose.read_bytes()
        if old == data:
            st[kind + ' identical'] += 1
            continue
        if kind == 'vmt':
            st['vmt differ'] += 1
            vmt_diff.append((p, old.decode('utf-8', 'replace'), data.decode('utf-8', 'replace')))
        else:
            ha, hb = vtf_head(old), vtf_head(data)
            if ha != hb:
                st['vtf header differs'] += 1
                vtf_diff.append((p, ha, hb))
            elif len(old) != len(data):
                st['vtf size differs'] += 1
            else:
                st['vtf payload differs (same header)'] += 1
    for k, v in sorted(st.items()):
        print(f'  {k:40s} {v}')
    for p, a, b in vmt_diff[:4]:
        print('VMT', p)
        print('  python:', a.replace('\n', ' | '))
        print('  C     :', b.replace('\n', ' | '))
    for p, a, b in vtf_diff[:8]:
        print('VTF', p, 'python', a, 'C', b)


if __name__ == '__main__':
    main()
