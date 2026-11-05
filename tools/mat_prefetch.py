import argparse
import json
import os
import re
import struct
import sys
from collections import defaultdict

GVT_MAGIC = b'GVT1'
PAGE_MAGIC = b'GVPG'
SECTOR_SHIFT = 3  # 8x8 tiles per page
SECTOR = 1 << SECTOR_SHIFT
LODS = 5
T_CHUNK = 250


def gvt_names(buf, off=0):
    if buf[off:off + 4] != GVT_MAGIC:
        return None
    submesh_count, mat_count = struct.unpack_from('<II', buf, off + 16)
    if mat_count > 4096 or submesh_count > 8192:
        return None
    pos = off + 32 + 16 * submesh_count
    end = pos + 64 * mat_count
    if end > len(buf):
        return None
    names = []
    block = buf[pos:end]
    for i in range(mat_count):
        raw = block[i * 64:(i + 1) * 64]
        z = raw.find(b'\0')
        if z >= 0:
            raw = raw[:z]
        name = raw.decode('ascii', 'replace')
        if name:
            names.append(name)
    return names


def page_entries(buf):
    if buf[:4] != PAGE_MAGIC:
        return None
    count, = struct.unpack_from('<I', buf, 8)
    if count == 0 or count > 65536:
        return None
    out = {}
    for i in range(count):
        name_off, blob_off, blob_len = struct.unpack_from('<III', buf, 16 + i * 16)
        z = buf.find(b'\0', name_off)
        name = buf[name_off:z].decode('ascii', 'replace')
        out[name] = (blob_off, blob_len)
    return out


TILE_KEY = re.compile(r'^(-?\d+) (-?\d+)$')


def load_tiles(index_path):
    with open(index_path, 'rb') as f:
        idx = json.load(f)
    tiles = {}
    for key, lods in idx['tiles'].items():
        m = TILE_KEY.match(key)
        if not m:
            continue
        tx, ty = int(m.group(1)), int(m.group(2))
        per = {}
        for lod in range(LODS):
            info = lods.get('lod%d' % lod)
            if info and info.get('file'):
                per[lod] = info['file']
        tiles[(tx, ty)] = per
    nmat = len(idx.get('materials') or {})
    return tiles, nmat


def resolve_file(per, lod):
    if lod in per:
        return per[lod]
    for alt in range(lod - 1, -1, -1):
        if alt in per:
            return per[alt]
    for alt in range(lod + 1, LODS):
        if alt in per:
            return per[alt]
    return None


class BlobSource:
    def __init__(self, map_dir):
        self.map_dir = map_dir
        self.pages = {}  # file name -> (page path, off, len)
        self.page_files = []
        pdir = os.path.join(map_dir, 'pages')
        if os.path.isdir(pdir):
            self.page_files = sorted(
                os.path.join(pdir, n) for n in os.listdir(pdir)
                if n.startswith('v_') and n.endswith('.bin'))

    def index_pages(self, log=lambda s: None):
        for path in self.page_files:
            with open(path, 'rb') as f:
                head = f.read(16)
                if head[:4] != PAGE_MAGIC:
                    continue
                count, = struct.unpack_from('<I', head, 8)
                if count == 0 or count > 65536:
                    continue
                entries = f.read(count * 16)
                names_start = 16 + count * 16
                first_blob = None
                rows = []
                for i in range(count):
                    name_off, blob_off, blob_len = struct.unpack_from('<III', entries, i * 16)
                    rows.append((name_off, blob_off, blob_len))
                    if first_blob is None or blob_off < first_blob:
                        first_blob = blob_off
                names_len = (first_blob or names_start) - names_start
                if names_len <= 0 or names_len > 4 * 1024 * 1024:
                    continue
                names_blob = f.read(names_len)
            for name_off, blob_off, blob_len in rows:
                rel = name_off - names_start
                if 0 <= rel < names_len:
                    z = names_blob.find(b'\0', rel)
                    name = names_blob[rel:z].decode('ascii', 'replace')
                    if name:
                        self.pages[name] = (path, blob_off, blob_len)
        log('pages: %d page files, %d packed blobs' %
            (len(self.page_files), len(self.pages)))

    def scan(self, wanted, on_file):
        by_page = defaultdict(list)
        loose = []
        for fn in wanted:
            ent = self.pages.get(fn)
            if ent:
                by_page[ent[0]].append(fn)
            else:
                loose.append(fn)
        done = 0
        total = len(wanted)
        for path, fns in by_page.items():
            with open(path, 'rb') as f:
                buf = f.read()
            for fn in fns:
                _, off, ln = self.pages[fn]
                names = gvt_names(buf, off)
                if names is None:
                    names = gvt_names(buf[off:off + ln])
                on_file(fn, names)
                done += 1
            if done % 20000 < len(fns):
                print('  scanned %d / %d files' % (done, total))
        for fn in loose:
            names = None
            p = os.path.join(self.map_dir, 'tiles', fn)
            if os.path.isfile(p):
                with open(p, 'rb') as f:
                    head = f.read(1 << 17)
                names = gvt_names(head)
                if names is None:
                    with open(p, 'rb') as f:
                        names = gvt_names(f.read())
            on_file(fn, names)
            done += 1
            if done % 20000 == 0:
                print('  scanned %d / %d files' % (done, total))


def sector_of(tx, ty):
    return (tx >> SECTOR_SHIFT, ty >> SECTOR_SHIFT)


def write_manifest(path, slot_sets):
    name_id = {}
    names = []
    set_id = {}
    set_ids = []  # id -> [name ids]
    slots = []
    for key in sorted(slot_sets):
        sx, sy, lod = key
        sids = []
        for n in sorted(slot_sets[key]):
            i = name_id.get(n)
            if i is None:
                i = len(names)
                name_id[n] = i
                names.append(n)
            sids.append(i)
        sid = set_id.get(tuple(sids))
        if sid is None:
            sid = len(set_ids)
            set_id[tuple(sids)] = sid
            set_ids.append(sids)
        slots.append((sx, sy, lod, sid))

    with open(path, 'w', encoding='utf-8', newline='\n') as f:
        f.write('GVMAT1\n')
        f.write('K\tsector\t%d\n' % SECTOR)
        f.write('K\tnames\t%d\n' % len(names))
        f.write('K\tsets\t%d\n' % len(set_ids))
        f.write('K\tslots\t%d\n' % len(slots))
        for n in names:
            f.write('N\t%s\n' % n)
        for sid, sids in enumerate(set_ids):
            for start in range(0, len(sids), T_CHUNK):
                chunk = sids[start:start + T_CHUNK]
                f.write('T\t%d\t%s\n' % (sid, ','.join(str(i) for i in chunk)))
        for sx, sy, lod, sid in slots:
            f.write('S\t%d\t%d\t%d\t%d\n' % (sx, sy, lod, sid))
    return len(names), len(set_ids), len(slots)


def build(map_dir, out_path, probe):
    tiles, nmat = load_tiles(os.path.join(map_dir, 'index.json'))
    print('index: %d tiles, %d index materials' % (len(tiles), nmat))

    src = BlobSource(map_dir)
    src.index_pages(log=print)

    wanted = set()
    for per in tiles.values():
        for lod in range(LODS):
            fn = resolve_file(per, lod)
            if fn:
                wanted.add(fn)
    print('distinct tile files referenced: %d' % len(wanted))

    if probe:
        step = max(1, len(src.page_files) // probe)
        sample = set(src.page_files[::step])
        wanted = set()
        for fn, (path, off, ln) in src.pages.items():
            if path in sample:
                wanted.add(fn)
        sectors = set()
        for path in sample:
            m = re.match(r'v_(-?\d+)_(-?\d+)\.bin$', os.path.basename(path))
            if m:
                sectors.add((int(m.group(1)), int(m.group(2))))
        tiles = {k: v for k, v in tiles.items() if sector_of(*k) in sectors}
        print('probe: %d sectors, %d blobs, %d tiles' %
              (len(sectors), len(wanted), len(tiles)))

    file_names = {}
    missing = []

    def on_file(fn, names):
        if names is None:
            missing.append(fn)
        else:
            file_names[fn] = names

    src.scan(wanted, on_file)
    print('scanned %d files, %d unreadable' % (len(wanted), len(missing)))

    slot_sets = defaultdict(set)
    for (tx, ty), per in tiles.items():
        sec = sector_of(tx, ty)
        for lod in range(LODS):
            fn = resolve_file(per, lod)
            names = fn and file_names.get(fn)
            if names:
                slot_sets[(sec[0], sec[1], lod)].update(names)

    slots = len(slot_sets)
    total_refs = sum(len(v) for v in slot_sets.values())
    uniq_names = set()
    for v in slot_sets.values():
        uniq_names |= v
    sets = set(frozenset(v) for v in slot_sets.values())
    print('slots (sector x lod): %d, name refs %d (avg %.1f/slot)' %
          (slots, total_refs, total_refs / max(1, slots)))
    for lod in range(LODS):
        n = sum(1 for (x, y, l), v in slot_sets.items() if l == lod)
        r = sum(len(v) for (x, y, l), v in slot_sets.items() if l == lod)
        print('  lod%d: %d slots, %d refs' % (lod, n, r))
    print('distinct names: %d (index has %d)' % (len(uniq_names), nmat))
    print('interned sets: %d (collapse %d -> %d)' % (len(sets), slots, len(sets)))
    biggest = sorted((len(v) for v in slot_sets.values()), reverse=True)[:5]
    print('largest slot lists:', biggest)

    if probe:
        return 0
    n_names, n_sets, n_slots = write_manifest(out_path, slot_sets)
    size = os.path.getsize(out_path)
    print('wrote %s: %d names, %d sets, %d slots, %.1f MB' %
          (out_path, n_names, n_sets, n_slots, size / 1048576.0))
    return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--map-dir', default=None,
                    help='data/lossantos/<map> directory')
    ap.add_argument('--out', default=None, help='output .dat path')
    ap.add_argument('--probe', type=int, default=0, metavar='N',
                    help='scan only N sample sectors; stats only, no output')
    args = ap.parse_args()

    if not args.map_dir:
        print('usage: mat_prefetch.py --map-dir <dir> [--probe N]')
        return 2
    out = args.out or os.path.join(args.map_dir, 'tile_materials.dat')
    rc = build(args.map_dir, out, args.probe)
    return rc


if __name__ == '__main__':
    sys.exit(main())
