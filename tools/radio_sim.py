import json
from pathlib import Path

import lupa.luajit21 as lj

ADDON = Path(r'C:\Program Files (x86)\Steam\steamapps\common\GarrysMod\garrysmod\addons\lossantos')
SRC = Path(r'C:\Users\Frame\Downloads\openworld_stream\addon\lua\autorun\client\lossantos_radio.lua')
rt = lj.LuaRuntime(unpack_returned_tuples=True)
g = rt.globals()


def file_read(path, _where):
    p = ADDON / path
    return p.read_text(encoding='utf-8') if p.is_file() else None


def file_find(pattern, _where):
    d = ADDON / pattern.rstrip('*')
    return rt.table(), rt.table(*sorted(x.name for x in d.iterdir() if x.is_dir()))


def json_to_table(s):
    def conv(o):
        if isinstance(o, dict):
            return rt.table_from({k: conv(v) for k, v in o.items()})
        if isinstance(o, list):
            return rt.table(*[conv(v) for v in o])
        return o
    return conv(json.loads(s))


# fake gmod globals
rt.execute('''
local noop = function() end
local any = setmetatable({}, { __index = function() return noop end })
hook = { Add = noop }; concommand = any; surface = any; draw = any; input = any; gui = any
vgui = any; sound = any
function CreateClientConVar() end
function ScreenScale(x) return x end
function IsValid(x) return x ~= nil and x ~= false end
function LocalPlayer() return nil end
function SysTime() return os.clock() end
function GetConVar() return { GetFloat = function() return 0.6 end } end
file = {}; util = {}
''')
g.file.Read = file_read
g.file.Find = file_find
g.util.JSONToTable = json_to_table
rt.execute(SRC.read_text(encoding='utf-8'))
rt.execute('LosSantosRadio.Discover()')
R = g.LosSantosRadio
print('shared adverts', len(R.Shared.adverts), 'news', len(R.Shared.news))
for i in range(1, len(R.Order) + 1):
    sid = R.Order[i]
    st = R.Stations[sid]
    kinds = {}
    for j in range(1, len(st.seq) + 1):
        p = st.seq[j].e.path
        k = 'ad' if 'radio_adverts' in p else 'news' if 'radio_news' in p else (
            'song' if st.seq[j].e.title else 'other')
        kinds[k] = kinds.get(k, 0) + 1
    first = [st.seq[j].e.title for j in range(1, len(st.seq) + 1) if st.seq[j].e.title][:2]
    print('%-22s %-24s %-5s songs %2d  seq %3d  %5.1f h  %s  %s' % (
        sid, st.name, st.kind, len(st.songs), len(st.seq), st.cycle / 3600,
        dict(sorted(kinds.items())), first))
    has_logo = R.AtlasCells[sid] is not None
    if not has_logo:
        print('   ! no logo cell')
