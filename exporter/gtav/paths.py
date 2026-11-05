import os
from pathlib import Path

GMOD = Path(os.environ.get('GTAV_GMOD', r'C:\Program Files (x86)\Steam\steamapps\common\GarrysMod\garrysmod'))
ADDON = GMOD / 'addons' / 'lossantos'
DATA = GMOD / 'data' / 'lossantos'
CONTENT = Path(os.environ.get('GTAV_CONTENT', r'C:\Users\Frame\Downloads\openworld_work\content'))
CONTENT_MATERIALS = CONTENT / 'materials'
CONTENT_SOUND = CONTENT / 'sound'
RADIO_SOUND = CONTENT_SOUND / 'lossantos_radio'
RADIO_LOGOS = CONTENT_MATERIALS / 'lossantos_radio'
GTA_MATERIALS = CONTENT_MATERIALS / 'models' / 'gtav'
CONTENT_GMA = DATA / 'content'
ADDON_RADIO_MANIFESTS = ADDON / 'sound' / 'lossantos_radio'
