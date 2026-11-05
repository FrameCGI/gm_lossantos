import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from exporter.gtav import pack

MAP = Path(r'C:\Program Files (x86)\Steam\steamapps\common\GarrysMod'
           r'\garrysmod\data\lossantos\lossantos')


def decode(blob):
    total = len(blob)
    magic, = struct.unpack_from('<I', blob, 0)
    if magic != 0x31545647:  # "GVT1"
        raise ValueError('bad magic')
    version, flags = struct.unpack_from('<HH', blob, 4)
    vc, ic, sc, mc, stride = struct.unpack_from('<IIIII', blob, 8)
    hdrV, hdrI = vc, ic
    clamped = 0

    pos = 32  # 32 byte header, then submeshes
    subs = []
    for i in range(sc):
        subs.append(struct.unpack_from('<IIII', blob, pos + i * 16))
    pos += sc * 16

    mats = []
    for i in range(mc):
        raw = blob[pos + i * 64: pos + i * 64 + 64]
        mats.append(raw.split(b'\0')[0].decode('ascii'))
    pos += mc * 64

    vbase = pos
    if vc > 0 and vbase + vc * stride - 1 > total:
        vc = (total - vbase) // stride
        clamped = 1

    has_lm = (flags & 1) != 0
    has_nrm = (flags & 2) != 0
    verts = []
    for i in range(vc):
        v = blob[vbase + i * stride: vbase + (i + 1) * stride]
        x, y, z, u, vv = struct.unpack_from('<fffff', v, 0)
        nx = ny = nz = 0.0
        if has_nrm:
            o = 24 if has_lm else 20  # normal sits after lightmap uv
            if o + 3 <= stride:
                b = struct.unpack_from('<bbb', v, o)
                nx, ny, nz = b[0] / 127.0, b[1] / 127.0, b[2] / 127.0
        verts.append((x, y, z, u, vv, nx, ny, nz))
    pos = vbase + vc * stride

    usable = (total - pos) // 2 if total > pos else 0
    if ic > usable:
        ic = usable
        clamped = 1
    idx = list(struct.unpack_from('<%dH' % ic, blob, pos)) if ic else []

    subs = list(subs)
    for i in range(sc):
        first, count = subs[i][1], subs[i][2]
        if first > ic:
            count = 0
        elif first + count > ic:
            count = ic - first
            clamped = 1
        if subs[i][3] > vc:
            subs[i] = (subs[i][0], first, count, 0)
            clamped = 1
        else:
            subs[i] = (subs[i][0], first, count, subs[i][3])

    return dict(version=version, flags=flags, vc=vc, ic=ic, sc=sc, mc=mc,
                stride=stride, clamped=clamped, mats=mats, subs=subs,
                verts=verts, idx=idx, hdrV=hdrV, hdrI=hdrI)


def dump(blob, name):
    t = decode(blob)
    print('tile %s bytes=%d' % (name, len(blob)))
    print('hdr version=%d flags=%d vc=%d ic=%d sc=%d mc=%d stride=%d clamped=%d'
          % (t['version'], t['flags'], t['vc'], t['ic'], t['sc'], t['mc'],
             t['stride'], t['clamped']))
    for i, m in enumerate(t['mats']):
        print('mat[%d]=%s' % (i, m))
    for i, (mi, ioff, icnt, voff) in enumerate(t['subs']):
        print('sub[%d] mat=%d ioff=%d icnt=%d voff=%d' % (i, mi, ioff, icnt, voff))

    show = min(3, t['vc'])
    for i in list(range(show)) + list(range(t['vc'] - show, t['vc'])):
        x, y, z, u, v, nx, ny, nz = t['verts'][i]
        print('vert[%d] %.6f %.6f %.6f | %.6f %.6f | %.6f %.6f %.6f'
              % (i, x, y, z, u, v, nx, ny, nz))

    print('sum_idx=%d' % sum(t['idx']))

    sumbits = 0
    mn = [1e30] * 3
    mx = [-1e30] * 3
    for x, y, z, *_ in t['verts']:
        for k, p in enumerate((x, y, z)):
            sumbits += struct.unpack('<I', struct.pack('<f', p))[0]
            if p < mn[k]:
                mn[k] = p
            if p > mx[k]:
                mx[k] = p
    print('sum_posbits=%d' % sumbits)
    if t['vc']:
        print('bounds %.3f %.3f %.3f -> %.3f %.3f %.3f'
              % (mn[0], mn[1], mn[2], mx[0], mx[1], mx[2]))

    bad = 0
    for mi, ioff, icnt, voff in t['subs']:
        if ioff + icnt > t['ic']:
            bad += 1
        if voff > t['vc']:
            bad += 1
    print('range_errors=%d' % bad)


def scan(radius, eye_x, eye_y):
    tile_size = 1024.0
    page_span = 8.0 * tile_size
    import math
    span = int(radius / page_span) + 2
    epx = int(math.floor(eye_x / page_span))
    epy = int(math.floor(eye_y / page_span))

    pages = blobs = verts = idxs = subs = 0
    failures = clamped = range_err = 0

    for px in range(epx - span, epx + span + 1):
        for py in range(epy - span, epy + span + 1):
            cx0, cy0 = px * page_span, py * page_span
            nx = min(max(eye_x, cx0), cx0 + page_span)
            ny = min(max(eye_y, cy0), cy0 + page_span)
            dx, dy = eye_x - nx, eye_y - ny
            if dx * dx + dy * dy > radius * radius:
                continue
            page = MAP / 'pages' / ('v_%d_%d.bin' % (px, py))
            if not page.exists():
                continue
            pages += 1
            for name, blob in pack.read_page(page).items():
                try:
                    t = decode(blob)
                except Exception as exc:
                    failures += 1
                    if failures <= 5:
                        print('FAIL %s: %s' % (name, exc))
                    continue
                blobs += 1
                verts += t['vc']
                idxs += t['ic']
                subs += t['sc']
                if t['clamped']:
                    clamped += 1
                    if clamped <= 5:
                        print('CLAMPED %s' % name)
                for mi, ioff, icnt, voff in t['subs']:
                    if ioff + icnt > t['ic']:
                        range_err += 1

    print('pages=%d blobs=%d verts=%d idx=%d subs=%d'
          % (pages, blobs, verts, idxs, subs))
    print('decode_failures=%d clamped=%d range_errors=%d'
          % (failures, clamped, range_err))
    return 1 if (failures or range_err) else 0


def main():
    if sys.argv[1] == 'scan':
        return scan(float(sys.argv[2]), float(sys.argv[3]), float(sys.argv[4]))
    tx, ty, name = int(sys.argv[2]), int(sys.argv[3]), sys.argv[4]
    page = MAP / 'pages' / ('v_%d_%d.bin' % (tx >> 3, ty >> 3))
    blob = pack.read_page(page).get(name)
    if blob is None:
        print('no entry %s in %s' % (name, page))
        return 1
    dump(blob, name)
    return 0


if __name__ == '__main__':
    sys.exit(main())
