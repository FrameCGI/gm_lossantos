from __future__ import annotations

import json
import os
import struct
from concurrent.futures import ProcessPoolExecutor

from .world import TILE_MAGIC

HEADER = 32


def tile_z_bounds(path):
    try:
        with open(path, 'rb') as f:
            data = f.read()
    except OSError:
        return None
    if len(data) < HEADER:
        return None
    magic, _ver, _flags, nv, _nidx, nsub, nmat, stride = struct.unpack_from(
        '<IHHIIIII', data, 0)
    if magic != TILE_MAGIC or stride != 28 or nv == 0 or nv > 8_000_000:
        return None
    off = HEADER + nsub * 16 + nmat * 64
    if off + nv * 28 > len(data):
        return None
    lo = hi = None
    step = 4096
    for start in range(0, nv, step):
        block = memoryview(data)[off + start * 28:
                                 off + min(nv, start + step) * 28]
        n = min(step, nv - start)
        # z is the 3rd float of the vertex
        pos = [struct.unpack_from('<f', block, i * 28 + 8)[0] for i in range(n)]
        b0, b1 = min(pos), max(pos)
        lo = b0 if lo is None or b0 < lo else lo
        hi = b1 if hi is None or b1 > hi else hi
    if lo is None or lo != lo or hi != hi:  # NaN guard
        return None
    return (float(lo), float(hi))


def _bounds_job(args):
    name, path = args
    b = tile_z_bounds(path)
    return name, b


def generate(map_dir, log=print, workers=None):
    with open(os.path.join(map_dir, 'index.json'), encoding='utf-8') as f:
        idx = json.load(f)
    tiles = idx.get('tiles') or {}

    files = {}
    for key, entry in tiles.items():
        if not isinstance(entry, dict):
            continue
        for slot in entry.values():
            if isinstance(slot, dict):
                name = slot.get('file')
                if name:
                    files[name] = True

    jobs = [(name, os.path.join(map_dir, 'tiles', name)) for name in files]
    file_bounds = {}
    skipped = 0
    done = 0
    with ProcessPoolExecutor(max_workers=workers) as pool:
        for name, b in pool.map(_bounds_job, jobs, chunksize=256):
            done += 1
            if b is None:
                skipped += 1
            else:
                file_bounds[name] = b
            if done % 20000 == 0:
                log(f'  ... {done}/{len(jobs)} tile files')

    bounds = {}
    for key, entry in tiles.items():
        if not isinstance(entry, dict):
            continue
        z0 = z1 = None
        for slot in entry.values():
            if not isinstance(slot, dict):
                continue
            b = file_bounds.get(slot.get('file'))
            if not b:
                continue
            z0 = b[0] if z0 is None or b[0] < z0 else z0
            z1 = b[1] if z1 is None or b[1] > z1 else z1
        if z0 is not None:
            bounds[key] = [round(z0, 2), round(z1, 2)]

    return bounds, len(files), skipped


def write_sidecar(map_dir, bounds):
    try:
        path = os.path.join(map_dir, 'tile_bounds.json')
        tmp = path + '.tmp'
        with open(tmp, 'w', encoding='utf-8') as f:
            json.dump({'schema': 1, 'bounds': bounds}, f,
                      separators=(',', ':'))
        os.replace(tmp, path)
        # line twin, the runtime prefers this one
        dat = os.path.join(map_dir, 'tile_bounds.dat')
        with open(dat + '.tmp', 'w', encoding='utf-8', newline='\n') as f:
            f.write('GVTB1\n')
            for k, (z0, z1) in bounds.items():
                f.write(f'{k}\t{z0}\t{z1}\n')
        os.replace(dat + '.tmp', dat)
        return path
    except OSError:
        return None


def load(map_dir):
    try:
        with open(os.path.join(map_dir, 'tile_bounds.json'),
                  encoding='utf-8') as f:
            data = json.load(f)
        bounds = data.get('bounds') if isinstance(data, dict) else None
        return bounds if isinstance(bounds, dict) else {}
    except (OSError, ValueError):
        return {}
