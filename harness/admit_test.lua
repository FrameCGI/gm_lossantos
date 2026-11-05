-- luajit admit_test.lua [index_lua] [addon_lua_dir]
local INDEX_LUA = arg[1] or
	"C:/Users/Frame/AppData/Local/Temp/opencode/index_tiles.lua"
local ADDON = arg[2] or
	"C:/Program Files (x86)/Steam/steamapps/common/GarrysMod/garrysmod"
	.. "/addons/lossantos/lua/gtav"


local VEC = {}
VEC.__index = VEC
function VEC:LengthSqr() return self.x * self.x + self.y * self.y + self.z * self.z end
function VEC:Length() return math.sqrt(self:LengthSqr()) end
function VEC:Length2D() return math.sqrt(self.x * self.x + self.y * self.y) end
function VEC.__sub(a, b)
	return setmetatable({x = a.x - b.x, y = a.y - b.y, z = a.z - b.z}, VEC)
end
function VEC.__add(a, b)
	return setmetatable({x = a.x + b.x, y = a.y + b.y, z = a.z + b.z}, VEC)
end

function Vector(x, y, z)
	return setmetatable({x = x or 0, y = y or 0, z = z or 0}, VEC)
end
vector_origin = Vector(0, 0, 0)

-- conVars the test drives.
local CVARS = {
	gtav_cl_tile_budget = 24000,
	gtav_cl_merge_grid = 0,
	gtav_cl_lod_bias = 0,
	gtav_stream_radius = 0,
	gtav_fog = 1,
	gtav_fog_start = 0,
	gtav_fog_end = 0,
	gtav_skybox_world = 0,
}
local function convar(name)
	return {
		GetInt = function() return math.floor(CVARS[name] or 0) end,
		GetFloat = function() return CVARS[name] or 0 end,
		GetBool = function() return (CVARS[name] or 0) ~= 0 end,
		GetString = function() return tostring(CVARS[name] or 0) end,
	}
end
function GetConVar(name) return convar(name) end
function CreateClientConVar(name, default)
	if CVARS[name] == nil then CVARS[name] = tonumber(default) or 0 end
	return convar(name)
end

hook = {}
function hook.Add() end
function hook.Remove() end
concommand = {}
function concommand.Add() end
timer = {}
function timer.Create() end
function timer.Remove() end
function timer.Simple() end
function Matrix() return {} end
function IsValid(o) return o ~= nil and o.IsValid ~= nil end
function ErrorNoHalt(...) print(...) end
function MsgC(...) print(...) end
function SysTime() return NOW or 0 end
function CurTime() return NOW or 0 end
function RealTime() return NOW or 0 end
function RealFrameTime() return 0.015 end
function EyePos() return Vector(0, 0, 0) end
function EyeAngles() return {Forward = function() return Vector(1, 0, 0) end} end
function LocalPlayer() return nil end
function CreateMaterial() return {} end
function Material() return {} end
game = {GetMap = function() return "gm_merge_test" end,
		GetIPAddress = function() return "127.0.0.1" end}
ents = setmetatable({}, {__index = function() return function() return nil end end})
net = setmetatable({}, {__index = function() return function() end end})
cvars = setmetatable({}, {__index = function() return function() end end})
input = setmetatable({}, {__index = function() return function() end end})
vgui = setmetatable({}, {__index = function() return function() end end})
gui = setmetatable({}, {__index = function() return function() end end})
draw = setmetatable({}, {__index = function() return function() end end})
sound = setmetatable({}, {__index = function() return function() end end})
scrnw, scrnh = 1920, 1080
function ScrW() return scrnw end
function ScrH() return scrnh end
render = setmetatable({}, {__index = function() return function() end end})
cam = setmetatable({}, {__index = function() return function() end end})
surface = setmetatable({}, {__index = function() return function() end end})
util = setmetatable({}, {__index = function() return function() return nil end end})
file = setmetatable({}, {__index = function() return function() return nil end end})
CLIENT = true
SERVER = false

if not table.Count then
	table.Count = function(t)
		local n = 0
		for _ in pairs(t) do n = n + 1 end
		return n
	end
end

function math.Clamp(v, lo, hi)
	if v < lo then return lo end
	if v > hi then return hi end
	return v
end
function math.Round(n, p)
	local m = 10 ^ (p or 0)
	return math.floor(n * m + 0.5) / m
end
function math.Remap(v, a, b, c, d) return c + (v - a) * (d - c) / (b - a) end
function math.TimeFraction(a, b, t) return (t - a) / (b - a) end
function math.Approach(cur, target, inc)
	if cur < target then
		cur = cur + inc
		if cur > target then return target end
	else
		cur = cur - inc
		if cur < target then return target end
	end
	return cur
end


GTAV = {Config = {}, MapData = {tiles = {}}, Client = {}, Merge = {}}
dofile(ADDON .. "/sh_config.lua")
dofile(ADDON .. "/cl_merge.lua")
dofile(ADDON .. "/cl_stream.lua")

INDEX_DATA_PATH = INDEX_LUA:gsub("%.lua$", ".dat")
dofile(INDEX_LUA)

local C = GTAV.Client
local M = GTAV.Merge
local cfg = GTAV.Config

-- what LoadMapIndex sets for los santos
cfg.VISUAL_TILE_SIZE = 1024
cfg.CELL_SIZE = 8192
cfg.PHYS_CHUNK_SIZE = 1024
cfg.STREAM_RADIUS = 90000
cfg.MAX_TILES = 24000
cfg.KEEP_RADIUS = cfg.CELL_SIZE * cfg.CELL_KEEP

io.write("[admit] index: " .. table.Count(GTAV.MapData.tiles) .. " tiles\n")

M.EnsureList()
io.write("[admit] merge grid " .. M.Grid() .. ": " .. #C.GroupList .. " groups\n")


local function sortedByDist(ex, ey)
	local eye = Vector(ex, ey, 0)
	local all = {}
	for i = 1, #C.GroupList do
		local g = C.GroupList[i]
		all[#all + 1] = {
			key = g.key,
			dist = math.sqrt(GTAV.Dist2DSqr(eye, g.center)),
			cost = #(g.members or {}),
		}
	end
	table.sort(all, function(a, b) return a.dist < b.dist end)
	return all
end

local function streamRadius()
	local maxDist = math.min(cfg.LOD_DISTANCES[4],
							 cfg.STREAM_RADIUS or cfg.LOD_DISTANCES[4])
	local o = CVARS.gtav_stream_radius
	if o and o > 0 then maxDist = o end
	return maxDist
end

local function drain()
	for i = C.BuildHead, #C.BuildQueue do
		local task = C.BuildQueue[i]
		if task then
			C.ActiveTiles[task.key] = {
				key = task.key, tx = task.tx, ty = task.ty, span = task.span,
				lod = task.lod, center = task.center, members = task.members,
				submeshes = {}, lastWanted = NOW,
			}
		end
	end
	C.BuildQueue = {}
	C.BuildHead = 1
end


local failures = 0

local function check(name, ok, detail)
	io.write(string.format("  %-4s %s%s\n", ok and "ok" or "FAIL", name,
		detail and ("  -- " .. detail) or ""))
	if not ok then failures = failures + 1 end
end

local function runCase(label, ex, ey, budget, warm)
	CVARS.gtav_cl_tile_budget = budget
	NOW = 100

	C.ActiveTiles = {}
	C.BuildQueue = {}
	C.BuildHead = 1
	C.EffRadius = 0

	if warm then
		-- furthest first, worst case
		local all = sortedByDist(ex, ey)
		local covered = 0
		for i = #all, 1, -1 do
			if covered + all[i].cost > budget then break end
			covered = covered + all[i].cost
			local g = nil
			for j = 1, #C.GroupList do
				if C.GroupList[j].key == all[i].key then g = C.GroupList[j] break end
			end
			if g then
				C.ActiveTiles[g.key] = {
					key = g.key, tx = g.tx, ty = g.ty, span = g.span,
					lod = 4, center = g.center, members = g.members,
					submeshes = {}, lastWanted = NOW,
				}
			end
		end
	end

	-- eviction is one mesh per refresh, takes a while
	local STABLE_NEED = 12
	local passes, prev, stable = 0, -1, 0
	while passes < 900 and stable < STABLE_NEED do
		passes = passes + 1
		C.UpdateTileQueue(Vector(ex, ey, 0), false)
		local hadWork = #C.BuildQueue > 0
		drain()
		NOW = NOW + 1

		local n = 0
		for _ in pairs(C.ActiveTiles) do n = n + 1 end
		if n == prev and not hadWork then
			stable = stable + 1
		else
			stable = 0
		end
		prev = n
	end

	local kept = C.ActiveTiles
	local nkept = 0
	for _ in pairs(kept) do nkept = nkept + 1 end

	local all = sortedByDist(ex, ey)
	local radius = streamRadius()
	local inRadius = 0
	for i = 1, #all do
		if all[i].dist <= radius then inRadius = inRadius + 1 end
	end

	check(label .. ": kept " .. nkept .. " of " .. inRadius .. " groups in radius",
		  nkept > 0, "settled in " .. passes .. " passes")

	local DIST_BIN = 32     -- mirrors cl_stream.lua
	local covered = 0
	for _, t in pairs(kept) do
		covered = covered + math.max(1, #(t.members or {}))
	end

	-- cut lands between bins, may overshoot one
	local cutBin, bandCost = 0, 0
	for i = 1, #all do
		local b = math.floor(all[i].dist / DIST_BIN) + 1
		if kept[all[i].key] and b > cutBin then cutBin = b end
	end
	for i = 1, #all do
		if math.floor(all[i].dist / DIST_BIN) + 1 == cutBin then
			bandCost = bandCost + all[i].cost
		end
	end

	check(label .. ": budget " .. budget .. " -> covered " .. covered ..
		  " (band allowance " .. bandCost .. ")",
		  covered <= budget + bandCost,
		  covered > budget + bandCost and
			("over budget by " .. (covered - budget)) or nil)

	-- the big one, kept set is a distance prefix
	local holes, farthest = 0, 0
	for i = 1, #all do
		if kept[all[i].key] then farthest = i end
	end
	for i = 1, farthest do
		if not kept[all[i].key] then holes = holes + 1 end
	end
	check(label .. ": disc is hole-free out to group #" .. farthest,
		  holes == 0,
		  holes > 0 and (holes .. " missing groups inside the kept radius") or nil)

	local tol = cfg.VISUAL_TILE_SIZE * M.Grid()
	local strays = 0
	for i = 1, #all do
		if kept[all[i].key] and all[i].dist > radius + tol then
			strays = strays + 1
		end
	end
	check(label .. ": no groups kept outside the radius", strays == 0)
end


io.write("\n-- cold start: nothing resident --\n")
runCase("cold  500", -20318.9, -14645.7, 500, false)
runCase("cold 2k ", -20318.9, -14645.7, 2000, false)
runCase("cold 8k ", -20318.9, -14645.7, 8000, false)
runCase("cold 24k", -20318.9, -14645.7, 24000, false)

io.write("\n-- warm: far groups arrived first and hold budget --\n")
runCase("warm  500", -20318.9, -14645.7, 500, true)
runCase("warm 2k ", -20318.9, -14645.7, 2000, true)
runCase("warm 24k", -20318.9, -14645.7, 24000, true)

io.write("\n-- other places in the city --\n")
runCase("downtown 8k", -3267.5, -21256.7, 8000, false)
runCase("airport 8k", 12000.0, -32000.0, 8000, false)
runCase("northco 8k", -8000.0, 24000.0, 8000, false)

io.write("\n")
if failures == 0 then
	print("PASS - admission always keeps a complete disc")
else
	print("FAIL - " .. failures .. " check(s) failed")
end
os.exit(failures == 0 and 0 or 1)
