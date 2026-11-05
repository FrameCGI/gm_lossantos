"""Pack a streamed world into GTA V-style bundled sector pages."""

from __future__ import annotations

import argparse
import os
import re
import struct
import sys
import time
from collections import defaultdict
from concurrent.futures import ProcessPoolExecutor
from pathlib import Path

from . import flags_sidecar

PAGE_MAGIC = b'GVPG'
PAGE_VERSION = 1
PAGE_SHIFT = 3  # 8x8 per page
PAGE_AXIS = 1 << PAGE_SHIFT

NAME_RE = re.compile(r'^[a-z]_(-?\d+)_(-?\d+)')

KINDS = (('v', 'tiles'), ('p', 'phys'))  # kind letter, source dir


def page_coords(x: int, y: int):
    # >> floors negatives like lua math.floor
    return (x >> PAGE_SHIFT, y >> PAGE_SHIFT)


def parse_name(name: str):
    m = NAME_RE.match(name)
    if not m:
        return None
    return int(m.group(1)), int(m.group(2))


def build_page(out_path: Path, files):
    names = [f.name for f in files]
    blobs = [f.read_bytes() for f in files]
    count = len(files)

    names_start = 16 + count * 16
    name_table = bytearray()
    name_offs = []
    for n in names:
        name_offs.append(names_start + len(name_table))
        name_table += n.encode('ascii') + b'\0'

    blob_start = names_start + len(name_table)
    blob_offs = []
    pos = blob_start
    for b in blobs:
        blob_offs.append(pos)
        pos += len(b)

    tmp = out_path.with_suffix(out_path.suffix + '.tmp')
    with open(tmp, 'wb') as f:
        # 0   magic "GVPG"
        # 4   version
        # 6   flags
        # 8   entry count
        # 12  reserved
        # 16  entries, 16 bytes each
        # ..  name table, then payloads
        f.write(PAGE_MAGIC)
        f.write(struct.pack('<HHII', PAGE_VERSION, 0, count, 0))
        # name off, blob off, blob len, reserved
        for i in range(count):
            f.write(struct.pack('<IIII', name_offs[i], blob_offs[i],
                                len(blobs[i]), 0))
        f.write(name_table)
        for b in blobs:
            f.write(b)
    tmp.replace(out_path)
    return count, pos


def build_one(job):
    out_path, files = job
    return build_page(out_path, files)


def page_fresh(out_path: Path, files) -> bool:
    try:
        out_m = out_path.stat().st_mtime
    except OSError:
        return False
    for f in files:
        if f.stat().st_mtime > out_m:
            return False
    return True


def read_page(path: Path):
    raw = path.read_bytes()
    if len(raw) < 16 or raw[:4] != PAGE_MAGIC:
        raise ValueError(f'{path}: bad page magic')
    version, flags, count, _reserved = struct.unpack_from('<HHII', raw, 4)
    if version != PAGE_VERSION:
        raise ValueError(f'{path}: unsupported page version {version}')
    if count <= 0:
        raise ValueError(f'{path}: empty page')

    names_start = 16 + count * 16
    if len(raw) < names_start:
        raise ValueError(f'{path}: truncated entry table')

    entries = []
    first_blob = None
    for i in range(count):
        name_off, blob_off, blob_len, _ = struct.unpack_from('<IIII', raw, 16 + i * 16)
        entries.append((name_off, blob_off, blob_len))
        if first_blob is None or blob_off < first_blob:
            first_blob = blob_off
    if first_blob is None or first_blob < names_start:
        raise ValueError(f'{path}: name table overlaps the entry table')

    out = {}
    for name_off, blob_off, blob_len in entries:
        if not (names_start <= name_off < first_blob):
            raise ValueError(f'{path}: name offset {name_off} outside the name table')
        end = raw.find(b'\0', name_off)
        if end < 0:
            raise ValueError(f'{path}: unterminated name at {name_off}')
        name = raw[name_off:end].decode('ascii')
        if blob_off + blob_len > len(raw):
            raise ValueError(f'{path}: payload {name} runs past the end of the page')
        if name in out:
            raise ValueError(f'{path}: duplicate entry {name}')
        out[name] = raw[blob_off:blob_off + blob_len]
    return out


def collect(map_dir: Path, kind: str, sub: str, log=print):
    src = map_dir / sub
    if not src.is_dir():
        return {}
    groups = defaultdict(list)
    skipped = []
    for f in sorted(src.iterdir()):
        if not f.is_file():
            continue
        coords = parse_name(f.name)
        if coords is None:
            skipped.append(f.name)
            continue
        groups[page_coords(*coords)].append(f)
    if skipped:
        log(f'  ! {len(skipped)} {sub} files with unrecognised names were left out: '
            f'{skipped[:5]}')
    return groups


def cmd_build(map_dir: Path, log=print, workers: int = 0,
              force: bool = False) -> int:
    pages_dir = map_dir / 'pages'
    pages_dir.mkdir(exist_ok=True)
    workers = workers or max(1, (os.cpu_count() or 4) - 1)

    grand_files = 0
    grand_bytes = 0
    grand_pages = 0
    t0 = time.time()
    for kind, sub in KINDS:
        groups = collect(map_dir, kind, sub, log)
        if not groups:
            log(f'{sub}: nothing to pack')
            continue
        n_files = sum(len(v) for v in groups.values())
        log(f'{sub}: {n_files} files -> {len(groups)} pages')
        jobs = []
        fresh = 0
        for (px, py), files in sorted(groups.items()):
            out = pages_dir / f'{kind}_{px}_{py}.bin'
            if not force and page_fresh(out, files):
                fresh += 1
                continue
            jobs.append((out, files))
        with ProcessPoolExecutor(max_workers=workers) as ex:
            for count, size in ex.map(build_one, jobs, chunksize=4):
                grand_files += count
                grand_bytes += size
                grand_pages += 1
        log(f'  wrote {len(jobs)} pages under {pages_dir}'
            + (f' ({fresh} up to date)' if fresh else ''))
    log(f'packed {grand_files} files ({grand_bytes / 1e9:.2f} GB) '
        f'into {grand_pages} pages in {time.time() - t0:.0f}s, '
        f'workers={workers}')
    flags_sidecar.write_sidecar(map_dir, log=log)
    return 0


def verify_one(job):
    page, src = job
    msgs = []
    try:
        entries = read_page(page)
    except ValueError as exc:
        msgs.append(f'  FAIL {exc}')
        return 0, 0, 1, msgs
    compared = missing = bad = 0
    for name, blob in entries.items():
        loose = src / name
        if not loose.is_file():
            missing += 1
            continue
        if loose.read_bytes() != blob:
            msgs.append(f'  FAIL {page.name}: {name} differs from the loose file')
            bad += 1
        compared += 1
    return compared, missing, bad, msgs


def cmd_verify(map_dir: Path, log=print, workers: int = 0) -> int:
    pages_dir = map_dir / 'pages'
    if not pages_dir.is_dir():
        log('no pages/ directory - nothing packed yet')
        return 1

    pages = sorted(pages_dir.glob('*.bin'))
    if not pages:
        log('pages/ is empty')
        return 1
    workers = workers or max(1, (os.cpu_count() or 4) - 1)

    bad = 0
    compared = 0
    missing = 0
    jobs = []
    for page in pages:
        kind = page.name[0]
        sub = 'tiles' if kind == 'v' else 'phys'
        jobs.append((page, map_dir / sub))
    with ProcessPoolExecutor(max_workers=workers) as ex:
        for c, m, b, msgs in ex.map(verify_one, jobs, chunksize=4):
            compared += c
            missing += m
            bad += b
            for msg in msgs:
                log(msg)

    log(f'verified {len(pages)} pages: {compared} payloads match the loose files, '
        f'{missing} already pruned, {bad} errors')
    return 1 if bad else 0


def cmd_prune(map_dir: Path, dry_run: bool, log=print) -> int:
    if cmd_verify(map_dir, log) != 0:
        log('prune refused: the pages do not match the loose files')
        return 1

    pages_dir = map_dir / 'pages'
    removed = 0
    freed = 0
    for kind, sub in KINDS:
        src = map_dir / sub
        if not src.is_dir():
            continue
        groups = collect(map_dir, kind, sub, log)
        for _key, files in groups.items():
            for f in files:
                freed += f.stat().st_size
                removed += 1
                if not dry_run:
                    f.unlink()
        if not dry_run:
            try:
                src.rmdir()
            except OSError:
                log(f'  ! {src} not empty, left in place')

    verb = 'would remove' if dry_run else 'removed'
    log(f'{verb} {removed} loose files ({freed / 1e9:.2f} GB); '
        f'the pages are now the only copy')
    return 0


def cmd_info(map_dir: Path, log=print) -> int:
    pages_dir = map_dir / 'pages'
    for kind, sub in KINDS:
        src = map_dir / sub
        n_files = sum(1 for f in src.iterdir() if f.is_file()) if src.is_dir() else 0
        bytes_ = sum(f.stat().st_size for f in src.iterdir() if f.is_file()) if src.is_dir() else 0
        pages = sorted(pages_dir.glob(f'{kind}_*.bin')) if pages_dir.is_dir() else []
        pbytes = sum(p.stat().st_size for p in pages)
        log(f'{sub}: {n_files} loose files ({bytes_ / 1e9:.2f} GB) | '
            f'{len(pages)} pages ({pbytes / 1e9:.2f} GB)')
    return 0


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('command', choices=('build', 'verify', 'prune', 'info'))
    ap.add_argument('map_dir', type=Path,
                    help='data/lossantos/<map> directory')
    ap.add_argument('--dry-run', action='store_true',
                    help='prune: report what would be deleted')
    ap.add_argument('--workers', type=int, default=0,
                    help='parallel workers (default: cpu count - 1)')
    ap.add_argument('--force', action='store_true',
                    help='build: rebuild pages even when they are up to date')
    args = ap.parse_args(argv)

    map_dir = args.map_dir
    if not map_dir.is_dir():
        print(f'not a directory: {map_dir}', file=sys.stderr)
        return 2

    if args.command == 'build':
        return cmd_build(map_dir, workers=args.workers, force=args.force)
    if args.command == 'verify':
        return cmd_verify(map_dir, workers=args.workers)
    if args.command == 'prune':
        return cmd_prune(map_dir, args.dry_run)
    return cmd_info(map_dir)


if __name__ == '__main__':
    raise SystemExit(main())
