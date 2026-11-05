import sys
from pathlib import Path

try:
    import lupa.luajit21 as lj
except ImportError:
    sys.exit('pip install --user lupa')

REPO = Path(__file__).resolve().parents[1]
files = [Path(a) for a in sys.argv[1:]] or sorted((REPO / 'addon' / 'lua').rglob('*.lua'))
rt = lj.LuaRuntime()
check = rt.eval('function(s, n) local f, e = loadstring(s, "=" .. n); return e end')
bad = 0
for f in files:
    err = check(f.read_text(encoding='utf-8'), f.name)
    if err:
        bad += 1
        print('FAIL', err)
print('%d file(s), %d error(s)' % (len(files), bad))
sys.exit(1 if bad else 0)
