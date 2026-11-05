import os
import struct

VTF_BGRA8888 = 12
VTF_DXT1 = 13
VTF_DXT3 = 14
VTF_DXT5 = 15

HEADER = 80

DEFAULT_CAP = 512  # max width/height


def mip_size(fmt, w, h):
    if fmt == VTF_BGRA8888:
        return w * h * 4
    if fmt == VTF_DXT1:
        return max(1, (w + 3) // 4) * max(1, (h + 3) // 4) * 8
    if fmt in (VTF_DXT3, VTF_DXT5):
        return max(1, (w + 3) // 4) * max(1, (h + 3) // 4) * 16
    return None


def parse(path):
    try:
        with open(path, 'rb') as f:
            head = f.read(HEADER)
            if len(head) < HEADER or head[:4] != b'VTF\0':
                return None
            major, minor, hdr = struct.unpack_from('<III', head, 4)
            if (major, minor) != (7, 2) or hdr != HEADER:
                return None
            w, h = struct.unpack_from('<HH', head, 16)
            fmt, = struct.unpack_from('<I', head, 52)
            mipcount = head[56]
            lo_fmt, = struct.unpack_from('<I', head, 57)
            if not w or not h or not mipcount:
                return None
            if lo_fmt != 0xFFFFFFFF:
                return None  # a thumbnail would sit before the mip data
            size = os.path.getsize(path)
            data = size - HEADER
            sizes = []
            mw, mh = w, h
            for _ in range(mipcount):
                s = mip_size(fmt, mw, mh)
                if s is None:
                    return None
                sizes.append(s)
                mw, mh = max(1, mw >> 1), max(1, mh >> 1)
            if sum(sizes) != data:
                return None
            return w, h, fmt, mipcount, sizes
    except OSError:
        return None


def plan(info, cap=DEFAULT_CAP):
    w, h, fmt, mipcount, sizes = info
    drops = 0
    rw, rh = w, h
    while max(rw, rh) > cap and mipcount - drops > 1:
        rw, rh = max(1, rw >> 1), max(1, rh >> 1)
        drops += 1
    return drops, rw, rh


def shrink(path, info, cap=DEFAULT_CAP):
    w, h, fmt, mipcount, sizes = info
    drops, rw, rh = plan(info, cap)
    if drops == 0:
        return None

    with open(path, 'rb') as f:
        blob = f.read()
    data = blob[HEADER:]
    keep = sum(sizes[drops:])  # bytes of the surviving small mips
    data = data[:keep]  # smallest-first on disk: keep the head

    head = bytearray(blob[:HEADER])
    struct.pack_into('<HH', head, 16, rw, rh)
    head[56] = mipcount - drops
    tmp = path + '.shrink.tmp'
    with open(tmp, 'wb') as f:
        f.write(bytes(head))
        f.write(data)
    os.replace(tmp, path)
    return (w, h), (rw, rh)


def shrink_file(path, cap=DEFAULT_CAP):
    info = parse(path)
    if info is None:
        return None
    return shrink(path, info, cap)
