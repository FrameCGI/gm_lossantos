import sys as _sys
from pathlib import Path as _P
_sys.path.insert(0, str(_P(__file__).resolve().parents[1]))
from exporter.gtav import paths as _paths
import argparse
import json
import os
import shutil
import subprocess
import sys
import tempfile
import wave
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO))
sys.path.insert(0, str(REPO / 'tools'))
from exporter.gtav import crypto, rpf
import gtav_keys

GTA = Path(r'D:\Games\Grand Theft Auto V Legacy')
KEYS = REPO / 'exporter' / 'gtav' / 'keys'
VGM = Path(r'C:\Users\Frame\Downloads\openworld_work\vgmstream\vgmstream-cli.exe')
OUT_ROOT = _paths.RADIO_SOUND
OGG_QUALITY = '5'  # libvorbis q5 ~ 160 kbps stereo


def wav_seconds(path):
    with wave.open(str(path)) as w:
        return w.getnframes() / float(w.getframerate())


def subsong_count(awc_path):
    r = subprocess.run([str(VGM), '-m', str(awc_path)], capture_output=True, text=True)
    for line in r.stdout.splitlines():
        if line.lower().startswith('stream count:'):
            try:
                return int(line.split(':', 1)[1].split()[0])
            except ValueError:
                pass
    return 1


def encode(wav, ogg):
    ogg.parent.mkdir(parents=True, exist_ok=True)
    tmp = ogg.with_suffix('.tmp.ogg')
    subprocess.run(['ffmpeg', '-y', '-loglevel', 'error', '-i', str(wav), '-c:a', 'libvorbis',
                    '-q:a', OGG_QUALITY, str(tmp)], check=True)
    os.replace(tmp, ogg)


def convert_one(name, data, work, out_dir):
    stem = name[:-4]
    d = Path(tempfile.mkdtemp(dir=work))
    try:
        awc = d / name
        awc.write_bytes(data)
        shutil.copy(work / '.awckey', d / '.awckey')
        n = subsong_count(awc)
        results = []
        for s in range(1, n + 1):
            rel = Path(stem + '.ogg') if n == 1 else Path(stem) / ('%03d.ogg' % s)
            ogg = out_dir / rel
            wav = d / ('%s_%03d.wav' % (stem, s))
            r = subprocess.run([str(VGM), '-s', str(s), '-o', str(wav), str(awc)],
                               capture_output=True, text=True)
            if r.returncode != 0 or not wav.is_file():
                print('  ! %s subsong %d: %s' % (name, s, (r.stderr or r.stdout).strip()[-120:]),
                      flush=True)
                continue
            secs = wav_seconds(wav)
            if not ogg.is_file():
                encode(wav, ogg)
            wav.unlink()
            results.append((rel.as_posix(), round(secs, 3)))
        return results
    finally:
        shutil.rmtree(d, ignore_errors=True)


def repack():
    import subprocess
    r = subprocess.run([sys.executable, '-X', 'utf8', str(Path(__file__).with_name('pack_content.py'))])
    if r.returncode != 0:
        print('note: content not repacked - close GMod and run tools/pack_content.py')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--station', help='e.g. RADIO_02_POP')
    ap.add_argument('--list', action='store_true')
    ap.add_argument('--jobs', type=int, default=4)
    ap.add_argument('--no-pack', action='store_true',
                    help='do not rebuild the content archives afterwards')
    a = ap.parse_args()

    sfx = GTA / 'x64' / 'audio' / 'sfx'
    if a.list or not a.station:
        for p in sorted(sfx.glob('RADIO_*.rpf')):
            print(p.stem)
        return 0
    if not VGM.is_file():
        raise SystemExit('vgmstream missing: run tools/fetch_vgmstream.py')
    keyfile = KEYS / 'gtav_awc_key.dat'
    if not keyfile.is_file():
        raise SystemExit('AWC key missing: run tools/gtav_keys.py --write')

    station = a.station.upper()
    out_dir = OUT_ROOT / station.lower()
    out_dir.mkdir(parents=True, exist_ok=True)
    work = Path(tempfile.mkdtemp(prefix='lossantos_radio_'))
    try:
        (work / '.awckey').write_bytes(gtav_keys.vgmstream_awckey(keyfile.read_bytes()))
        arch = rpf.RpfArchive(sfx / (station + '.rpf'), crypto.NgCipher.load(KEYS))
        entries = sorted((e for e in arch.walk()
                          if not e.is_dir and e.name.lower().endswith('.awc')),
                         key=lambda e: e.name)
        print('%s: %d awc files -> %s' % (station, len(entries), out_dir), flush=True)
        manifest = {'station': station.lower(), 'files': {}}

        with ThreadPoolExecutor(max_workers=a.jobs) as pool:
            futures = []
            for e in entries:
                data = arch.read(e)
                futures.append(pool.submit(
                    lambda n=e.name.lower(), d=data: (n[:-4], convert_one(n, d, work, out_dir))))
            for f in futures:
                stem, res = f.result()
                manifest['files'][stem] = [{'path': p, 'seconds': s} for p, s in res]
                print('  %-40s %d clip(s)' % (stem, len(res)), flush=True)
        (out_dir / 'manifest.json').write_text(json.dumps(manifest, indent=1), encoding='utf-8')
        clips = sum(len(v) for v in manifest['files'].values())
        print('%s: %d clips written, manifest.json' % (station, clips), flush=True)
    finally:
        shutil.rmtree(work, ignore_errors=True)
    if not a.no_pack:
        repack()
    return 0


if __name__ == '__main__':
    sys.exit(main())
