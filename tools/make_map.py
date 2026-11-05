"""Make an GTAV wrapper map: clone the template BSP and repoint it."""

from __future__ import annotations

import argparse
import shutil
import struct
import sys
from pathlib import Path

GMOD = Path(r'C:\Program Files (x86)\Steam\steamapps\common\GarrysMod\garrysmod')
MAPS = Path(__file__).resolve().parents[1] / 'native' / 'inst' / 'payload'
TEMPLATE_BSP = 'gm_lossantos.bsp'
TEMPLATE_VMF = 'gm_lossantos.vmf'
KEY = 'gtav_map'

LIGHT_EDITS = [
    ('"pitch" "0"', '"pitch" "-55"'),
    ('"SunSpreadAngle" "0"', '"SunSpreadAngle" "2"'),
    ('"_light" "255 255 255 200"', '"_light" "255 246 224 145"'),
    ('"_ambient" "255 255 255 20"', '"_ambient" "186 202 226 42"'),
]

IDENT = b'VBSP'
LUMP_COUNT = 64
HEADER_SIZE = 8 + LUMP_COUNT * 16 + 4  # ident, version, lumps[64], mapRevision
LUMP_ENTITIES = 0


def parse_header(data: bytes):
    if data[:4] != IDENT:
        raise SystemExit('not a VBSP file')
    if len(data) < HEADER_SIZE:
        raise SystemExit('truncated BSP header')

    version = struct.unpack_from('<i', data, 4)[0]
    lumps = []
    for i in range(LUMP_COUNT):
        off, length, ver, fourcc = struct.unpack_from('<IIi4s', data, 8 + i * 16)
        lumps.append([off, length, ver, fourcc])
    revision = struct.unpack_from('<i', data, 8 + LUMP_COUNT * 16)[0]

    for i, (off, length, _ver, _fc) in enumerate(lumps):
        if length and off + length > len(data):
            raise SystemExit(f'lump {i} runs past end of file')
    return version, lumps, revision


def entity_text(data: bytes, lumps) -> tuple[bytes, bytes, int]:
    off, length, _v, _f = lumps[LUMP_ENTITIES]
    raw = data[off:off + length]
    end = raw.find(b'\0')
    if end < 0:
        return raw, b'', off
    return raw[:end], raw[end:], off


def align4(n: int) -> int:
    return (n + 3) & ~3


LUMP_GAME = 35
LUMP_PAKFILE = 40


def game_lump_entries(data: bytes, off: int, length: int):
    if length < 4:
        return []
    count = struct.unpack_from('<i', data, off)[0]
    if count < 0 or 4 + count * 16 > length:
        return []
    out = []
    for i in range(count):
        p = off + 4 + i * 16
        out.append(struct.unpack_from('<IHHii', data, p))
    return out


def patch_game_lump_offsets(body: bytearray, off: int, length: int,
                            delta: int, ent_off: int) -> int:
    if not delta or length < 4:
        return 0
    count = struct.unpack_from('<i', body, off)[0]
    if count < 0 or 4 + count * 16 > length:
        return 0
    patched = 0
    for i in range(count):
        p = off + 4 + i * 16
        fofs = struct.unpack_from('<i', body, p + 8)[0]
        if fofs > ent_off:
            struct.pack_into('<i', body, p + 8, fofs + delta)
            patched += 1
    return patched


def rebuild(data: bytes, lumps, new_ent: bytes, ent_off: int) -> bytes:
    old_len = lumps[LUMP_ENTITIES][1]
    old_region = align4(old_len)
    new_region = align4(len(new_ent))
    delta = new_region - old_region

    if len(new_ent) > new_region:
        raise SystemExit('entity lump longer than its aligned region')
    if ent_off + old_region > len(data):
        raise SystemExit('entity region runs past end of file')

    body = bytearray(data)
    body[ent_off:ent_off + old_region] = new_ent + b'\0' * (new_region - len(new_ent))

    hdr = bytearray(data[:HEADER_SIZE])
    for i, (off, length, ver, fourcc) in enumerate(lumps):
        if i == LUMP_ENTITIES:
            length = len(new_ent)  # content only, padding is not counted
        elif off > ent_off:
            off += delta
        struct.pack_into('<IIi4s', hdr, 8 + i * 16, off, length, ver, fourcc)

    gl_off, gl_len = lumps[LUMP_GAME][0], lumps[LUMP_GAME][1]
    if gl_len and gl_off > ent_off:
        patched = patch_game_lump_offsets(body, gl_off + delta, gl_len,
                                          delta, ent_off)
        if patched:
            print(f'      : shifted {patched} game lump offset(s) by {delta}')

    return bytes(hdr) + bytes(body[HEADER_SIZE:])


def verify_clone(orig: bytes, clone: bytes, name: str) -> None:
    v0, l0, r0 = parse_header(orig)
    v1, l1, r1 = parse_header(clone)
    if v0 != v1 or r0 != r1:
        raise SystemExit('header version/revision changed')

    ent_off = l0[LUMP_ENTITIES][0]
    content_delta = l1[LUMP_ENTITIES][1] - l0[LUMP_ENTITIES][1]
    region_delta = (align4(l1[LUMP_ENTITIES][1]) - align4(l0[LUMP_ENTITIES][1]))

    for i, ((o0, n0, _, _), (o1, n1, _, _)) in enumerate(zip(l0, l1)):
        if n1 and o1 % 4:
            raise SystemExit(f'lump {i} offset {o1} is not 4-byte aligned')
        if i == LUMP_ENTITIES:
            if n1 != n0 + content_delta:
                raise SystemExit(f'entity lump length wrong: {n1} != {n0 + content_delta}')
            if o1 != o0:
                raise SystemExit('entity lump moved')
            continue
        if n1 != n0:
            raise SystemExit(f'lump {i} length changed')
        want = o0 + region_delta if o0 > ent_off else o0
        if o1 != want:
            raise SystemExit(f'lump {i} offset wrong: {o1} != {want}')

    if len(clone) % 4:
        raise SystemExit(f'file size {len(clone)} is not 4-byte aligned')

    for i, (off, length, _, _) in enumerate(l1):
        if i in (LUMP_ENTITIES, LUMP_GAME) or length == 0:
            continue
        o0 = l0[i][0]
        if orig[o0:o0 + length] != clone[off:off + length]:
            raise SystemExit(f'lump {i} payload changed')

    g0, gn0 = l0[LUMP_GAME][0], l0[LUMP_GAME][1]
    g1, gn1 = l1[LUMP_GAME][0], l1[LUMP_GAME][1]
    if gn0 != gn1:
        raise SystemExit('game lump length changed')
    e0 = game_lump_entries(orig, g0, gn0)
    e1 = game_lump_entries(clone, g1, gn1)
    if len(e0) != len(e1):
        raise SystemExit(f'game lump entry count changed: {len(e1)} != {len(e0)}')
    for (i0, f0, vr0, ofs0, ln0), (i1, f1, vr1, ofs1, ln1) in zip(e0, e1):
        if (i0, f0, vr0, ln0) != (i1, f1, vr1, ln1):
            raise SystemExit('game lump entry metadata changed')
        want = ofs0 + region_delta if ofs0 > ent_off else ofs0
        if ofs1 != want:
            raise SystemExit(f'game lump fileofs wrong: {ofs1} != {want}')
        if not (g1 <= ofs1 < g1 + gn1):
            raise SystemExit(f'game lump fileofs {ofs1} is outside its lump')
    table = 4 + len(e1) * 16
    if orig[g0 + table:g0 + gn0] != clone[g1 + table:g1 + gn1]:
        raise SystemExit('game lump data changed')
    if e0:
        print(f'verify: {len(e0)} game lump fileofs shifted correctly')

    pk, pn = l1[LUMP_PAKFILE][0], l1[LUMP_PAKFILE][1]
    if pn:
        raw = clone[pk:pk + pn]
        sig = raw.rfind(b'PK\x05\x06')
        if sig < 0:
            raise SystemExit('pakfile has no end-of-central-directory record')
        _dsk, _cds, _nth, _tot, cdsize, cdoff, _cmt = struct.unpack_from(
            '<HHHHIIH', raw, sig + 4)
        if cdoff != sig - cdsize:
            raise SystemExit(f'pakfile cdoff {cdoff} is not archive-relative')
        print(f'verify: pakfile zip is archive-relative ({pn} bytes)')

    text, _pad, _off = entity_text(clone, l1)
    if f'"{KEY}" "{name}"'.encode() not in text:
        raise SystemExit(f'{KEY} is not "{name}" after patch')
    if b'"targetname" "gtav_info"' not in text:
        raise SystemExit('info_target gtav_info missing')
    if b'"classname" "info_target"' not in text:
        raise SystemExit('info_target missing')
    print(f'verify: {content_delta:+d} bytes of text, {region_delta:+d} bytes of '
          f'region, all 63 other lumps intact and 4-byte aligned')
    print(f'verify: entity lump has {KEY}="{name}" on gtav_info')


def write_vmf(name: str) -> Path:
    src = MAPS / TEMPLATE_VMF
    if not src.is_file():
        return None

    text = src.read_text(encoding='utf-8', errors='replace')
    needle = f'"{KEY}" "'
    hits = [i for i in range(len(text)) if text.startswith(needle, i)]
    if len(hits) != 1:
        raise SystemExit(f'expected exactly one {KEY} in {src.name}, found {len(hits)}')

    start = hits[0] + len(needle)
    end = text.index('"', start)
    was = text[start:end]
    dst = MAPS / f'{name}.vmf'
    dst.write_text(text[:start] + name + text[end:], encoding='utf-8', newline='\r\n')
    print(f'vmf   : {dst.name}  ({KEY}: {was} -> {name})')
    return dst


def apply_edits(text: bytes, edits) -> bytes:
    out = text
    for old_s, new_s in edits:
        old, new = old_s.encode(), new_s.encode()
        n_old, n_new = out.count(old), out.count(new)
        if n_old == 1:
            out = out.replace(old, new)
        elif n_old == 0 and n_new == 1:
            continue  # already applied
        else:
            raise SystemExit(
                f'entity lump has {n_old} of {old_s!r} and {n_new} of {new_s!r}; '
                f'expected exactly one of either')
    return out


def main(argv=None):
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('name', help='map name, e.g. lossantos_blockout')
    ap.add_argument('--template', default=TEMPLATE_BSP,
                    help=f'BSP to clone (default {TEMPLATE_BSP})')
    args = ap.parse_args(argv)

    name = args.name
    if not name or any(c in name for c in ' \t/\\'):
        raise SystemExit(f'bad map name: {name!r}')

    src = MAPS / args.template
    if not src.is_file():
        raise SystemExit(f'template not found: {src}')
    orig = src.read_bytes()
    print(f'tmpl  : {src.name}  ({len(orig) / 1024:.0f} KB)')

    version, lumps, _rev = parse_header(orig)
    text, pad, ent_off = entity_text(orig, lumps)
    print(f'      : BSP version {version}, entity lump '
          f'{lumps[LUMP_ENTITIES][1]} bytes at {ent_off}, {len(pad)} trailing pad')

    lit = apply_edits(text, LIGHT_EDITS)
    if lit != text:
        print(f'light : applied {len(LIGHT_EDITS)} lighting edits '
              f'(sun pitch -55, brightness 200 -> 145)')
    text = lit

    old = f'"{KEY}" "'.encode()
    hits = [i for i in range(len(text)) if text.startswith(old, i)]
    if len(hits) != 1:
        raise SystemExit(f'expected exactly one {KEY} in the entity lump, '
                         f'found {len(hits)}')
    start = hits[0] + len(old)
    end = text.index(b'"', start)
    was = text[start:end].decode()

    new_ent = text[:start] + name.encode() + text[end:] + pad
    clone = rebuild(orig, lumps, new_ent, ent_off)

    dst = MAPS / f'{name}.bsp'
    dst.write_bytes(clone)
    print(f'bsp   : {dst.name}  ({len(clone) / 1024:.0f} KB, {KEY}: {was} -> {name})')

    verify_clone(orig, clone, name)
    write_vmf(name)

    extra = GMOD / 'maps' / f'{name}.bsp'
    extra.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(dst, extra)
    print(f'copy  : {extra}')

    data_dir = GMOD / 'data' / 'lossantos' / name
    state = 'found' if data_dir.is_dir() else 'MISSING'
    print(f'data  : {data_dir}  [{state}]')
    print(f'\ndone - "map {name}" loads data/lossantos/{name}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
