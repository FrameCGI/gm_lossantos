Vector = function(x, y, z) return {x = x, y = y, z = z} end
CreateClientConVar = function() return {GetInt = function() return 0 end} end
GetConVar = function() return nil end
if not table.Count then
	table.Count = function(t)
		local n = 0
		for _ in pairs(t) do n = n + 1 end
		return n
	end
end

GTAV = {
	Config = {VISUAL_TILE_SIZE = 1024, CELL_SIZE = 8192, LOD_DISTANCES = {
		[0] = 4096, [1] = 16384, [2] = 32768, [3] = 65536, [4] = 160000}},
	MapData = {current = {merge_grid = 8}, tiles = {}},
	Client = {},
	Merge = {},
}

-- luajit merge_test.lua [index_tiles.lua] [cl_merge.lua]
local INDEX_LUA = arg[1] or
	"C:/Users/Frame/AppData/Local/Temp/opencode/index_tiles.lua"
local MERGE_LUA = arg[2] or
	"C:/Program Files (x86)/Steam/steamapps/common/GarrysMod/garrysmod"
	.. "/addons/lossantos/lua/gtav/cl_merge.lua"

INDEX_DATA_PATH = INDEX_LUA:gsub("%.lua$", ".dat")
dofile(INDEX_LUA)
dofile(MERGE_LUA)

local M = GTAV.Merge
local ts = GTAV.Config.VISUAL_TILE_SIZE

local totalTiles = table.Count(GTAV.MapData.tiles)
local list = M.EnsureList()

-- every tile in exactly one group
local seen = {}
local dupes = 0
local covered = 0

for i = 1, #list do
	local g = list[i]
	for j = 1, #g.members do
		local m = g.members[j]
		if seen[m.key] then
			dupes = dupes + 1
		else
			seen[m.key] = true
			covered = covered + 1
		end
	end
end

local missing = 0
for key in pairs(GTAV.MapData.tiles) do
	if not seen[key] then missing = missing + 1 end
end

print(string.format("index tiles   : %d", totalTiles))
print(string.format("merge groups  : %d  (grid %d)", #list, M.Grid()))
print(string.format("covered tiles : %d", covered))
print(string.format("duplicated    : %d", dupes))
print(string.format("missing       : %d", missing))

local buckets = {}
local minS, maxS = 1e9, 0
local totalMembers = 0
for i = 1, #list do
	local n = #list[i].members
	totalMembers = totalMembers + n
	if n < minS then minS = n end
	if n > maxS then maxS = n end
	local b = buckets[n] or 0
	buckets[n] = b + 1
end
print(string.format("members/group : min %d  max %d  avg %.1f",
	minS, maxS, totalMembers / math.max(1, #list)))

local keys = {}
for k in pairs(buckets) do keys[#keys + 1] = k end
table.sort(keys)
io.write("distribution  :")
for i = 1, math.min(#keys, 12) do
	io.write(string.format("  %dx%d", keys[i], buckets[keys[i]]))
end
if #keys > 12 then io.write("  ...") end
print("")

-- every member needs a file at every lod
local unresolved = 0
local resolved = 0
for lod = 0, 4 do
	local okLod = 0
	for i = 1, #list do
		local files = M.MemberFiles(list[i], lod)
		if #files == #list[i].members then
			okLod = okLod + 1
		else
			unresolved = unresolved + (#list[i].members - #files)
		end
		resolved = resolved + #files
	end
	print(string.format("LOD%d          : %d/%d groups complete", lod, okLod, #list))
end
print(string.format("file lookups  : %d resolved, %d unresolved", resolved, unresolved))

local ok = (dupes == 0 and missing == 0)
print("")
if ok then
	print("PASS - every tile is in exactly one merge group")
else
	print("FAIL - the grouping loses or duplicates tiles")
end
os.exit(ok and 0 or 1)
