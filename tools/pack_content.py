"""Pack the converted content into .gma archives the addon mounts (plan L3)."""
import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from exporter.gtav import paths

GMAD = paths.GMOD.parent / 'bin' / 'win64' / 'gmad.exe'
ADDONS = paths.GMOD / 'addons'
OUTDIR = paths.CONTENT_GMA  # data/lossantos/content (game.MountGMA)
STAGE = paths.CONTENT.parent / 'gma_stage'
PREFIX = 'lossantos_content_'
PACK_EXT = ('.vmt', '.vtf', '.png', '.ogg')  # all inside gmad's whitelist


def gmod_running():
    out = subprocess.run(['tasklist', '/FI', 'IMAGENAME eq gmod.exe'],
                         capture_output=True, text=True).stdout
    return 'gmod.exe' in out.lower()


def migrate():
    moves = [
        (paths.ADDON / 'materials' / 'models' / 'gtav', paths.GTA_MATERIALS),
        (paths.ADDON / 'materials' / 'lossantos_radio', paths.RADIO_LOGOS),
    ]
    for src, dst in moves:
        if not src.exists():
            print('  skip (not loose):', src)
            continue
        if dst.exists():
            sys.exit(f'refusing: both {src} and {dst} exist - merge by hand')
        dst.parent.mkdir(parents=True, exist_ok=True)
        os.rename(src, dst)
        print('  moved', src, '->', dst)
    src_root = paths.ADDON / 'sound' / 'lossantos_radio'
    if src_root.is_dir():
        paths.RADIO_SOUND.mkdir(parents=True, exist_ok=True)
        for st in sorted(p for p in src_root.iterdir() if p.is_dir()):
            dst = paths.RADIO_SOUND / st.name
            if any(f.suffix == '.ogg' for f in st.rglob('*.ogg')):
                if dst.exists():
                    sys.exit(f'refusing: both {st} and {dst} exist - merge by hand')
                os.rename(st, dst)
                print('  moved', st, '->', dst)
    sync_manifests()


def sync_manifests():
    n = 0
    if not paths.RADIO_SOUND.is_dir():
        return
    for man in sorted(paths.RADIO_SOUND.glob('*/manifest.json')):
        out = paths.ADDON_RADIO_MANIFESTS / man.parent.name / 'manifest.json'
        out.parent.mkdir(parents=True, exist_ok=True)
        if not out.exists() or out.read_bytes() != man.read_bytes():
            shutil.copy2(man, out)
            n += 1
    print(f'  radio manifests: {n} copied loose into the addon')


def files_under(base, rel_root):
    out = []
    for dirpath, dirnames, filenames in os.walk(base):
        dirnames.sort()
        for fn in sorted(filenames):
            if fn.lower().endswith(PACK_EXT):
                p = os.path.join(dirpath, fn)
                st = os.stat(p)
                out.append((os.path.relpath(p, paths.CONTENT).replace(os.sep, '/'),
                            st.st_size, st.st_mtime_ns))
    return out


def plan(limit):
    groups = []  # (kind, unit name, files)
    if paths.GTA_MATERIALS.is_dir():
        for region in sorted(p for p in paths.GTA_MATERIALS.iterdir() if p.is_dir()):
            groups.append(('mat', region.name, files_under(region, None)))
    if paths.RADIO_LOGOS.is_dir():
        groups.append(('radio', '_logos', files_under(paths.RADIO_LOGOS, None)))
    if paths.RADIO_SOUND.is_dir():
        for st in sorted(p for p in paths.RADIO_SOUND.iterdir() if p.is_dir()):
            groups.append(('radio', st.name, files_under(st, None)))

    chunks = []
    for kind in ('mat', 'radio'):
        cur, size, n = [], 0, 1
        for k, _unit, files in groups:
            if k != kind or not files:
                continue
            fsize = sum(f[1] for f in files)
            if cur and size + fsize > limit:
                chunks.append((f'{PREFIX}{kind}_{n:02d}', cur))
                cur, size, n = [], 0, n + 1
            cur += files
            size += fsize
        if cur:
            chunks.append((f'{PREFIX}{kind}_{n:02d}', cur))
    return chunks


def signature(files):
    h = hashlib.sha1()
    for rel, size, mt in files:
        h.update(f'{rel}|{size}|{mt}\n'.encode())
    return h.hexdigest()


def pack_chunk(name, files):
    stage = STAGE / name
    if stage.exists():
        shutil.rmtree(stage)
    stage.mkdir(parents=True)
    for rel, _s, _m in files:
        dst = stage / rel
        dst.parent.mkdir(parents=True, exist_ok=True)
        os.link(paths.CONTENT / rel, dst)
    (stage / 'addon.json').write_text(json.dumps({
        'title': 'GTAV converted content (' + name + ')',
        'type': 'servercontent',
        'tags': ['scenic', 'realism'],
        'ignore': [],
    }, indent=1), encoding='utf-8')
    out = OUTDIR / (name + '.gma')
    tmp = STAGE / (name + '.gma')
    if tmp.exists():
        tmp.unlink()
    t0 = time.time()
    r = subprocess.run([str(GMAD), 'create', '-folder', str(stage), '-out', str(tmp), '-quiet'],
                       capture_output=True, text=True)
    if r.returncode != 0 or not tmp.is_file():
        print(r.stdout[-1500:], r.stderr[-1500:])
        sys.exit(f'gmad failed for {name} (exit {r.returncode})')
    os.replace(tmp, out)
    shutil.rmtree(stage)
    return out, time.time() - t0


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--migrate', action='store_true', help='move loose content out of the addon first')
    ap.add_argument('--status', action='store_true')
    ap.add_argument('--limit-mb', type=int, default=1500)
    ap.add_argument('--force', action='store_true', help='repack every chunk')
    a = ap.parse_args()

    if not GMAD.is_file():
        sys.exit(f'gmad.exe not found: {GMAD}')
    chunks = plan(a.limit_mb * 2**20)
    if a.status:
        for name, files in chunks:
            sig_ok = (STAGE / (name + '.sig')).is_file() and \
                (STAGE / (name + '.sig')).read_text() == signature(files)
            gma = OUTDIR / (name + '.gma')
            print('%-32s %6d files %7.0f MB  %s' % (name, len(files), sum(f[1] for f in files) / 2**20,
                  'up to date' if sig_ok and gma.is_file() else 'NEEDS PACK'))
        return 0
    if gmod_running():
        sys.exit('close Garry\'s Mod first (it holds the .gma files open)')
    if a.migrate:
        migrate()
        chunks = plan(a.limit_mb * 2**20)

    STAGE.mkdir(parents=True, exist_ok=True)
    OUTDIR.mkdir(parents=True, exist_ok=True)
    want = {name for name, _ in chunks}
    for old in OUTDIR.glob(PREFIX + '*.gma'):
        if old.stem not in want:
            old.unlink()
            print('  removed obsolete', old.name)
    for stray in ADDONS.glob(PREFIX + '*.gma*'):  # GMod would move these into cache/workshop
        stray.unlink()
        print('  removed stray', stray.name, 'from addons/')
    total = time.time()
    for name, files in chunks:
        sig = signature(files)
        sig_file = STAGE / (name + '.sig')
        gma = OUTDIR / (name + '.gma')
        if not a.force and gma.is_file() and sig_file.is_file() and sig_file.read_text() == sig:
            print('  %-32s up to date (%d files)' % (name, len(files)))
            continue
        out, secs = pack_chunk(name, files)
        sig_file.write_text(sig)
        print('  %-32s packed %6d files -> %.0f MB in %.0f s' % (
            name, len(files), out.stat().st_size / 2**20, secs), flush=True)
    (OUTDIR / 'chunks.txt').write_text(''.join(n + '.gma\n' for n, _ in chunks), encoding='utf-8')
    sync_manifests()
    print('pack_content: %d chunk(s), %.0f s' % (len(chunks), time.time() - total))
    return 0


if __name__ == '__main__':
    sys.exit(main())
