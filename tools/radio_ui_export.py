import difflib
import json
import re
import sys
from pathlib import Path

from PIL import Image

REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO))
from exporter.gtav import crypto, rpf, rpfbin
from exporter.gtav.ymap import name_hash
from exporter.gtav.ytd import decode_rgba, parse_ytd

GTA = Path(r'D:\Games\Grand Theft Auto V Legacy')
KEYS = REPO / 'exporter' / 'gtav' / 'keys'
from exporter.gtav import paths
TEX_OUT = paths.RADIO_LOGOS
SOUND = paths.RADIO_SOUND


def export_textures(c):
    up = rpf.RpfArchive(GTA / 'update' / 'update.rpf', c)
    sg = up.open_child(next(e for e in up.walk() if e.name.lower() == 'scaleform_generic.rpf'))
    e = next(x for x in sg.walk() if x.name.lower() == 'hud.ytd')
    TEX_OUT.mkdir(parents=True, exist_ok=True)
    n = 0
    for t in parse_ytd(sg.read(e), rpf.size_from_flags(e.system_flags), e.name):
        if 'radio' not in t.name.lower():
            continue
        rgba = decode_rgba(t)
        img = rgba if isinstance(rgba, Image.Image) else Image.fromarray(rgba, 'RGBA')
        img.save(TEX_OUT / (t.name.lower() + '.png'))
        n += 1
    print('radio textures:', n, '->', TEX_OUT)


NUM_WORDS = {'one': '1', 'two': '2', 'three': '3', 'four': '4', 'five': '5', 'six': '6',
             'seven': '7', 'eight': '8', 'nine': '9', 'ten': '10'}
MATCH_MIN = 0.75
TITLE_OVERRIDES = {'smokin_and_ridin': 'Smoke And Ride'}


def _norm(text):
    t = text.lower().replace('&', ' and ').replace('_', ' ')
    t = re.sub(r'\bwanna\b', 'want to', t)
    words = re.findall(r'[a-z0-9]+', t)
    out = []
    for w in words:
        w = NUM_WORDS.get(w, w)
        if w in ('and', 'n'):
            continue
        if w == 'u':
            w = 'you'
        out.append(w)
    return ''.join(out)


def _gxt(lang, name):
    return rpfbin.parse_gxt2(rpfbin.read_binary(
        lang, next(x for x in lang.walk() if x.name.lower() == name)))


def _tracks(trackid):
    out = []
    for n in range(30000):
        s, a = trackid.get(name_hash('%dS' % n)), trackid.get(name_hash('%dA' % n))
        if s and a:
            out.append((n, s, a))
    return out


_CREDIT = re.compile(r'\s*(\(.*?\)|\[.*?\]|\b(feat|ft|featuring)\b\.?.*$)', re.I)


def _variants(title, artist):
    bare = _CREDIT.sub('', title).strip() or title
    art = _CREDIT.sub('', artist).strip() or artist
    out = {_norm(title), _norm(bare), _norm(bare + ' ' + art), _norm(title + ' ' + art)}
    for piece in re.split(r'[/,]', bare):
        p = _norm(piece)
        if len(p) >= 4:
            out.add(p)
    return out


def _score(k, v):
    r = difflib.SequenceMatcher(None, k, v).ratio()
    if len(k) >= 8 and v.startswith(k):
        r = max(r, 0.9)
    return r


def _match_titles(stems, tracks):
    cand = []
    tn = [(n, s, a, _variants(s, a)) for n, s, a in tracks]
    for stem in stems:
        if stem in TITLE_OVERRIDES:
            want = _norm(TITLE_OVERRIDES[stem])
            cand += [(1.0, stem, n, s, a) for n, s, a, vs in tn if want in vs]
            continue
        k = _norm(stem)
        for n, s, a, vs in tn:
            r = max(_score(k, v) for v in vs)
            if r >= MATCH_MIN:
                cand.append((r, stem, n, s, a))
    cand.sort(reverse=True)
    used_s, used_t, out = set(), set(), {}
    for r, stem, n, s, a in cand:
        if stem in used_s or n in used_t:
            continue
        used_s.add(stem)
        used_t.add(n)
        out[stem] = {'id': n, 'title': s, 'artist': a, 'score': round(r, 2)}
    return out


SHARED_BANKS = ('radio_adverts', 'radio_news')


def _station_kind(files):
    stems = set(files)
    if not any(re.match(r'id_\d+$', s) for s in stems):
        return 'mix'
    if 'general' not in stems and any(s.startswith('mono_') and not s.startswith('mono_solo')
                                      for s in stems):
        return 'talk'
    return 'music'


def _show_titles(stems):
    small = {'a', 'an', 'of', 'the', 'and', 'you', 'to', 'in', 'on', 'my', 'her', 'his'}
    words = {s: (s[5:] if s.startswith('mono_') else s).split('_') for s in stems}
    out = {}
    for s, w in words.items():
        w = list(w)
        others = {v[0] for k, v in words.items() if k != s}
        if len(w[0]) > 3 and w[0][1:] in others:
            w[0] = w[0][1:]
        title = ' '.join(x.upper() if len(x) <= 3 and x.isalpha() and x not in small
                         else x.capitalize() for x in w)
        out[s] = {'title': title, 'show': True}
    return out


def station_names(c):
    outer = rpf.RpfArchive(GTA / 'x64b.rpf', c)
    lang = outer.open_child(next(x for x in outer.walk() if x.name.lower() == 'american_rel.rpf'))
    gxt = _gxt(lang, 'global.gxt2')
    tracks = _tracks(_gxt(lang, 'trackid.gxt2'))
    print('trackid.gxt2: %d title/artist pairs' % len(tracks))
    for man in sorted(SOUND.glob('*/manifest.json')):
        m = json.loads(man.read_text(encoding='utf-8'))
        st = m.get('station') or man.parent.name
        if st in SHARED_BANKS:
            continue
        name = gxt.get(name_hash(st))
        if name:
            m['name'] = name
        files = m.get('files', {})
        songs = [k for k, v in files.items()
                 if len(v) == 1 and v[0].get('seconds', 0) > 60
                 and not re.match(r'(id|mono_solo)_\d+$', k)]  # not id_rather_be_with_you
        m['kind'] = _station_kind(files)
        if m['kind'] == 'music':
            titles = _match_titles(songs, tracks)
        elif m['kind'] == 'talk':
            titles = _show_titles(songs)
        else:
            titles = {}  # mix: per-song timing not decoded yet
        m['titles'] = titles
        man.write_text(json.dumps(m, indent=1), encoding='utf-8')
        print('  %-24s %-22s titles %d/%d' % (st, name or '(no label)', len(titles), len(songs)))
        for stem in sorted(set(songs) - set(titles)):
            print('      unmatched: %s' % stem)
        for stem, t in sorted(titles.items()):
            if t.get('score', 1) < 0.95:
                print('      check %.2f: %-30s -> %s / %s' % (t['score'], stem, t['title'], t['artist']))


def repack():
    import subprocess
    r = subprocess.run([sys.executable, '-X', 'utf8', str(Path(__file__).with_name('pack_content.py'))])
    if r.returncode != 0:
        print('note: content not repacked - close GMod and run tools/pack_content.py')


def main():
    c = crypto.NgCipher.load(KEYS)
    export_textures(c)
    station_names(c)
    if '--no-pack' not in sys.argv:
        repack()
    return 0


if __name__ == '__main__':
    sys.exit(main())
