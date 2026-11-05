"""Emit the line-oriented index tables the addon streams."""

from __future__ import annotations

import argparse
import json
import os

MAGIC_TILES = 'GVIDX1'
MAGIC_PHYS = 'GVPHY1'

SCALARS = (
    'map', 'tile_size', 'cell_size', 'phys_chunk_size', 'stream_radius',
    'max_tiles', 'merge_grid',
)

LOD_SLOTS = ('lod0', 'lod1', 'lod2', 'lod3', 'lod4')
ABSENT = '-'


def _clean(value: str) -> str:
    if value is None:
        return ABSENT
    value = str(value).replace('\t', ' ').replace('\r', ' ').replace('\n', ' ')
    return value if value else ABSENT


def _num(value) -> str:
    if value is None:
        return ABSENT
    if isinstance(value, bool):
        return '1' if value else '0'
    if isinstance(value, int):
        return str(value)
    return repr(float(value))


def tiles_text(index: dict) -> str:
    # K  scalar
    # F  fog
    # S  spawn
    # J  source json
    # M  material
    # T  tile, one file per lod
    out = [MAGIC_TILES]

    for name in SCALARS:
        if name in index and index[name] is not None:
            out.append('K\t%s\t%s' % (name, _clean(index[name])))

    fog = index.get('fog') or {}
    out.append('F\t%s\t%s\t%s\t%s\t%s\t%s' % (
        '1' if fog.get('enabled', True) else '0',
        _num(fog.get('near', fog.get('start'))),
        _num(fog.get('far', fog.get('end'))),
        _num((fog.get('color') or [255, 255, 255])[0]),
        _num((fog.get('color') or [255, 255, 255])[1]),
        _num((fog.get('color') or [255, 255, 255])[2]),
    ))

    spawn = index.get('spawn') or [0, 0, 0]
    angles = index.get('spawn_angles') or [0, 0, 0]
    out.append('S\t%s\t%s\t%s\t%s\t%s\t%s' % (
        _num(spawn[0]), _num(spawn[1]), _num(spawn[2]),
        _num(angles[0]), _num(angles[1]), _num(angles[2]),
    ))

    if index.get('source') is not None:
        out.append('J\t' + json.dumps(index['source'], separators=(',', ':')))

    materials = index.get('materials') or {}
    for mat_id in sorted(materials, key=lambda k: int(k) if str(k).isdigit() else 0):
        out.append('M\t%s\t%s' % (_clean(mat_id), _clean(materials[mat_id])))

    tiles = index.get('tiles') or {}
    for key in sorted(tiles, key=_tile_sort_key):
        lods = tiles[key] or {}
        files = []
        for slot in LOD_SLOTS:
            entry = lods.get(slot)
            files.append(_clean(entry.get('file')) if isinstance(entry, dict) else ABSENT)
        out.append('T\t%s\t%s' % (_clean(key), '\t'.join(files)))

    return '\n'.join(out) + '\n'


def phys_text(index: dict) -> str:
    # P  chunk
    out = [MAGIC_PHYS]
    chunks = index.get('phys_chunks') or {}
    for key in sorted(chunks, key=_tile_sort_key):
        entry = chunks[key] or {}
        name = entry.get('file') if isinstance(entry, dict) else entry
        out.append('P\t%s\t%s' % (_clean(key), _clean(name)))
    return '\n'.join(out) + '\n'


def _tile_sort_key(key: str):
    parts = str(key).split()
    try:
        return (int(parts[0]), int(parts[1]))
    except (ValueError, IndexError):
        return (0, 0)


def write_index_dat(index: dict, out_dir: str) -> dict:
    tiles = tiles_text(index)
    phys = phys_text(index)
    with open(os.path.join(out_dir, 'index_tiles.dat'), 'w', encoding='utf-8',
              newline='\n') as f:
        f.write(tiles)
    with open(os.path.join(out_dir, 'index_phys.dat'), 'w', encoding='utf-8',
              newline='\n') as f:
        f.write(phys)
    return {
        'tile_lines': tiles.count('\n'),
        'phys_lines': phys.count('\n'),
    }


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('map_dir', help='directory holding index.json')
    args = ap.parse_args()

    idx_path = os.path.join(args.map_dir, 'index.json')
    with open(idx_path, encoding='utf-8') as f:
        index = json.load(f)

    stats = write_index_dat(index, args.map_dir)
    print('index_tiles.dat : %d tiles, %d materials'
          % (len(index.get('tiles') or {}), len(index.get('materials') or {})))
    print('index_phys.dat  : %d chunks' % len(index.get('phys_chunks') or {}))
    print('lines           : %d tile records, %d phys records'
          % (stats['tile_lines'], stats['phys_lines']))
    for name in ('index_tiles.dat', 'index_phys.dat'):
        path = os.path.join(args.map_dir, name)
        print('%-16s: %.2f MB' % (name, os.path.getsize(path) / 1e6))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
