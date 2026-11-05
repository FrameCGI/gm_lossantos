import struct
import sys
import time
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from exporter.gtav import crypto, rpf
from exporter.gtav.ydr import parse_ydr
from exporter.gtav.ymap import parse_ymap, parse_ytyp

KEYS = Path(__file__).resolve().parents[1] / 'exporter' / 'gtav' / 'keys'


def wsum64(b):
    a = np.frombuffer(b, np.uint8).astype(np.uint64)
    idx = np.arange(len(a), dtype=np.uint64)
    with np.errstate(over='ignore'):
        return int((a * (idx * np.uint64(2654435761) + np.uint64(1))).sum(dtype=np.uint64))


def fhex(vals):
    return ','.join('%08x' % struct.unpack('<I', struct.pack('<f', float(v)))[0] for v in vals)


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
        n = e.name.lower()
        if not mine or e.kind != rpf.RESOURCE or not n.endswith(('.ydr', '.ymap', '.ytyp')):
            continue
        data, sys_size = a.read(e), rpf.size_from_flags(e.system_flags)
        if n.endswith('.ydr'):
            d = parse_ydr(data, sys_size, e.name)
            for gi, g in enumerate(d.geometries):
                nrm = '-' if g.normals is None else '%016x' % wsum64(np.ascontiguousarray(g.normals, np.float32).tobytes())
                out.write('%s#%d\t%d\t%d\t%d\t%s\t%016x\t%016x\t%s\t%016x\n' % (
                    e.path, gi, len(g.positions), len(g.indices), g.shader, g.texture or '-',
                    wsum64(np.ascontiguousarray(g.positions, np.float32).tobytes()),
                    wsum64(np.ascontiguousarray(g.uvs, np.float32).tobytes()), nrm,
                    wsum64(np.ascontiguousarray(g.indices, np.int64).tobytes())))
            out.write('%s#tex\t%d\n' % (e.path, len(d.textures)))
        elif n.endswith('.ymap'):
            for k, en in enumerate(parse_ymap(data, sys_size, e.name)):
                out.write('%s#%d\t%08x\t%d\t%d\t%s\t%s\t%s,%s\n' % (
                    e.path, k, en.archetype, en.flags, en.lod_level, fhex(en.position),
                    fhex(en.rotation), fhex([en.scale_xy]), fhex([en.scale_z])))
        else:
            for k, at in enumerate(parse_ytyp(data, sys_size, e.name)):
                out.write('%s#%d\t%08x\t%08x\t%s\t%s\t%s\n' % (
                    e.path, k, at.name, at.asset_name, fhex(at.bb_min), fhex(at.bb_max), fhex([at.lod_dist])))


def main():
    t0 = time.time()
    a = rpf.RpfArchive(sys.argv[1], crypto.NgCipher.load(KEYS))
    with open(sys.argv[3], 'w', encoding='utf-8', newline='\n') as out:
        try:
            walk(a, sys.argv[2], out)
        except rpf.RpfError as exc:
            print('python stopped:', exc)
    print('python geom dump in %.1f s' % (time.time() - t0))


if __name__ == '__main__':
    main()
