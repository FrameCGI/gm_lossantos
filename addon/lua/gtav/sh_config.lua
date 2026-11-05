GTAV = GTAV or {}
GTAV.Config = GTAV.Config or {}
local Config = GTAV.Config

Config.CELL_SIZE = 8192
Config.CELL_KEEP = 0.75 -- rebase past this * CELL_SIZE
Config.KEEP_RADIUS = 6144 -- CELL_SIZE * CELL_KEEP on load
Config.VISUAL_TILE_SIZE = 4096
Config.PHYS_CHUNK_SIZE = 1024
Config.STREAM_BUDGET = 0.006     -- 6ms per frame for mesh building
Config.BUILDS_PER_FRAME = 24     -- hard cap on tiles decoded in one frame
Config.MAX_PHYS_TRIS = 2000
Config.PHYS_UNLOAD_TIME = 10.0
Config.ZFAR = 160000
Config.STREAM_RADIUS = 160000  -- max 2D distance for active visual tiles
Config.MAX_TILES = 512
Config.FOG = {
	enabled = false,
	color = {170, 181, 194},
	near = 0,
	far = 0,
}

Config.LOD_DISTANCES = {
	[0] = 4096,   -- LOD0: High detail, near
	[1] = 16384,  -- LOD1: Medium detail
	[2] = 32768,  -- LOD2: Low detail
	[3] = 65536,  -- LOD3: Very low detail
	[4] = 160000, -- LOD4: Impostor/horizon
}

Config.GROUND_QUERY_RADIUS = 1400
Config.PROP_PRECACHE_TIME = 0.5
-- max active collision entities
Config.MAX_ACTIVE_PHYS_ENTITIES = 256

GTAV.MapData = GTAV.MapData or {}
GTAV.MapData.current = nil
GTAV.MapData.tiles = {}
GTAV.MapData.materials = {}

-- utility functions

function GTAV.SnapCell(pos)
	local cs = Config.CELL_SIZE
	return Vector(
		math.floor(pos.x / cs + 0.5) * cs,
		math.floor(pos.y / cs + 0.5) * cs,
		math.floor(pos.z / cs + 0.5) * cs
	)
end

function GTAV.GetCellKey(pos)
	local cs = Config.CELL_SIZE
	return string.format("%d %d %d",
		math.floor(pos.x / cs + 0.5),
		math.floor(pos.y / cs + 0.5),
		math.floor(pos.z / cs + 0.5))
end

function GTAV.GetTileKey(pos)
	local ts = Config.VISUAL_TILE_SIZE
	return string.format("%d %d", math.floor(pos.x / ts), math.floor(pos.y / ts))
end

function GTAV.GetPhysChunkKey(pos)
	local ps = Config.PHYS_CHUNK_SIZE
	return string.format("%d %d", math.floor(pos.x / ps), math.floor(pos.y / ps))
end

function GTAV.GetPhysChunkCoord(pos)
	local ps = Config.PHYS_CHUNK_SIZE
	return math.floor(pos.x / ps), math.floor(pos.y / ps)
end

function GTAV.GetAbsolutePos(ent)
	local cell = ent:GetNW2Vector("WorldCell", Vector(0, 0, 0))
	return ent:GetPos() + cell
end

function GTAV.GetEntityCell(ent)
	return ent:GetNW2Vector("WorldCell", Vector(0, 0, 0))
end

function GTAV.SetEntityCell(ent, cell)
	ent:SetNW2Vector("WorldCell", cell)
	local key = GTAV.GetCellKey(cell)
	if ent.WorldCellKey ~= key then
		ent.WorldCellKey = key
		-- or ShouldCollide never gets asked
		ent:SetCustomCollisionCheck(true)
		ent:CollisionRulesChanged()
	end
	return key
end

function GTAV.ShouldCollide(entA, entB)
	local a = entA.WorldCellKey
	local b = entB.WorldCellKey
	if a and b and a ~= b then return false end
end

-- distance squared (avoids sqrt)
function GTAV.Dist2Sqr(a, b)
	local dx = a.x - b.x
	local dy = a.y - b.y
	local dz = a.z - b.z
	return dx * dx + dy * dy + dz * dz
end

-- 2D distance squared (ignores Z)
function GTAV.Dist2DSqr(a, b)
	local dx = a.x - b.x
	local dy = a.y - b.y
	return dx * dx + dy * dy
end

function GTAV.InRadius2D(pos, center, radius)
	local dx = pos.x - center.x
	local dy = pos.y - center.y
	return dx * dx + dy * dy <= radius * radius
end


-- index_tiles.dat, tab separated, first line GVIDX1
-- K  name value
-- F  on near far r g b
-- S  x y z pitch yaw roll
-- J  json
-- M  id material
-- T  key lod0 lod1 lod2 lod3 lod4, "-" = none
-- index_phys.dat, first line GVPHY1
-- P  key file
local DAT_MAGIC = "GVIDX1"
local PHYS_MAGIC = "GVPHY1"

-- ReadLine keeps the terminator on some gmod builds
local function datLine(f)
	local line = f:ReadLine(2048)
	if not line then return nil end
	local n = #line
	while n > 0 do
		local c = string.sub(line, n, n)
		if c ~= "\r" and c ~= "\n" then break end
		n = n - 1
	end
	if n ~= #line then line = string.sub(line, 1, n) end
	if line == "" then return nil end
	return line
end

local function readIndexDat(mapName)
	local base = "lossantos/" .. mapName .. "/"
	local f = file.Open(base .. "index_tiles.dat", "rb", "DATA")
	if not f then return nil, "no index_tiles.dat" end

	if datLine(f) ~= DAT_MAGIC then
		f:Close()
		return nil, "index_tiles.dat has the wrong magic"
	end

	local data = { materials = {}, tiles = {} }
	local ntiles = 0

	while true do
		local line = datLine(f)
		if not line then break end

		local kind = string.sub(line, 1, 1)
		if kind == "K" then
			local name, value = string.match(line, "^K\t([^\t]+)\t(.*)$")
			if name then
				if name == "map" then
					data.map = value
				else
					data[name] = tonumber(value) or value
				end
			end
		elseif kind == "F" then
			local on, near, far, r, g, b = string.match(line,
				"^F\t([^\t]+)\t([^\t]+)\t([^\t]+)\t([^\t]+)\t([^\t]+)\t([^\t]+)$")
			if on then
				data.fog = {
					enabled = (on == "1"),
					near = tonumber(near) or 0,
					far = tonumber(far) or 0,
					color = { tonumber(r) or 255, tonumber(g) or 255,
							  tonumber(b) or 255 },
				}
			end
		elseif kind == "S" then
			local x, y, z, p, yaw, roll = string.match(line,
				"^S\t([^\t]+)\t([^\t]+)\t([^\t]+)\t([^\t]+)\t([^\t]+)\t([^\t]+)$")
			if x then
				data.spawn = { tonumber(x) or 0, tonumber(y) or 0, tonumber(z) or 0 }
				data.spawn_angles = {
					tonumber(p) or 0, tonumber(yaw) or 0, tonumber(roll) or 0,
				}
			end
		elseif kind == "J" then
			local okJ, obj = pcall(util.JSONToTable, string.sub(line, 3))
			if okJ then data.source = obj end
		elseif kind == "M" then
			local id, name = string.match(line, "^M\t([^\t]+)\t(.*)$")
			if id then data.materials[id] = name end
		elseif kind == "T" then
			local key, f0, f1, f2, f3, f4 = string.match(line,
				"^T\t([^\t]+)\t([^\t]*)\t([^\t]*)\t([^\t]*)\t([^\t]*)\t([^\t]*)$")
			if key then
				local names = { f0, f1, f2, f3, f4 }
				local lods = {}
				for i = 1, 5 do
					local n = names[i]
					if n and n ~= "" and n ~= "-" then
						lods["lod" .. (i - 1)] = { file = n }
					end
				end
				data.tiles[key] = lods
				ntiles = ntiles + 1
			end
		end
	end
	f:Close()

	local pf = file.Open(base .. "index_phys.dat", "rb", "DATA")
	if pf then
		if datLine(pf) == PHYS_MAGIC then
			data.phys_chunks = {}
			while true do
				local line = datLine(pf)
				if not line then break end
				local key, name = string.match(line, "^P\t([^\t]+)\t(.*)$")
				if key then data.phys_chunks[key] = { file = name } end
			end
		else
			print("[GTAV] index_phys.dat has the wrong magic - no collision")
		end
		pf:Close()
	else
		data.phys_chunks = {}
	end

	return data, string.format("index_tiles.dat (%d tiles, %d chunks, %d materials)",
		ntiles, table.Count(data.phys_chunks), table.Count(data.materials))
end

-- fallback, JSONToTable truncates big files
local function readIndexJson(mapName)
	local path = "lossantos/" .. mapName .. "/index.json"
	local json = file.Read(path, "DATA")
	if not json then return nil, "no index.json" end

	local bytes = #json
	local data = util.JSONToTable(json)
	if not data then
		return nil, string.format("index.json did not parse (%d bytes)", bytes)
	end

	return data, string.format(
		"index.json (%d bytes -> %d tiles, %d chunks, %d materials)",
		bytes, table.Count(data.tiles or {}),
		table.Count(data.phys_chunks or {}), table.Count(data.materials or {}))
end

function GTAV.LoadMapIndex(mapName)
	local data, how = readIndexDat(mapName)
	if not data then
		local why = how
		data, how = readIndexJson(mapName)
		if not data then
			ErrorNoHalt(string.format(
				"[GTAV] Failed to load map index for %s: %s / %s\n",
				tostring(mapName), tostring(why), tostring(how)))
			return false
		end
		print("[GTAV] map index via JSON fallback: " .. tostring(how))
	end
	print("[GTAV] map index: " .. tostring(how))

	GTAV.MapData.current = data
	GTAV.MapData.name = data.map or mapName
	GTAV.MapData.tiles = data.tiles or {}
	GTAV.MapData.materials = data.materials or {}
	GTAV.MapData.phys_chunks = data.phys_chunks or {}

	if data.tile_size then Config.VISUAL_TILE_SIZE = data.tile_size end
	if data.cell_size then Config.CELL_SIZE = data.cell_size end
	if data.phys_chunk_size then Config.PHYS_CHUNK_SIZE = data.phys_chunk_size end
	if data.stream_radius then Config.STREAM_RADIUS = data.stream_radius end
	if data.max_tiles then Config.MAX_TILES = data.max_tiles end
	if data.fog then
		Config.FOG.enabled = data.fog.enabled ~= false
		if data.fog.color then Config.FOG.color = data.fog.color end
		Config.FOG.near = tonumber(data.fog.near) or tonumber(data.fog.start) or 0
		Config.FOG.far = tonumber(data.fog.far) or tonumber(data.fog["end"]) or 0
	end
	if Config.FOG.enabled then
		if Config.FOG.far <= 0 then
			Config.FOG.far = Config.STREAM_RADIUS * 0.97
		end
		if Config.FOG.near <= 0 then
			Config.FOG.near = Config.FOG.far * 0.48
		end
	end
	if data.spawn then GTAV.MapData.spawn = Vector(data.spawn[1], data.spawn[2], data.spawn[3]) end
	GTAV.MapData.spawn_angles = data.spawn_angles

	Config.KEEP_RADIUS = Config.CELL_SIZE * Config.CELL_KEEP

	GTAV.Binary = GTAV.Binary or {}
	GTAV.Binary.PhysMeshCache = {}
	GTAV.Binary.PhysCache = {}
	GTAV.Binary.PageCache = {}

	print(string.format("[GTAV] Loaded map index: %s (cell %.0f, keep %.0f, tiles %d, chunks %d)",
		tostring(GTAV.MapData.name), Config.CELL_SIZE, Config.KEEP_RADIUS,
		table.Count(GTAV.MapData.tiles), table.Count(GTAV.MapData.phys_chunks)))
	return true
end


if SERVER then
	-- not archived, a stale value pins the map
	CreateConVar("gtav_map", "", FCVAR_REPLICATED,
		"Override which lossantos/<name> data set to load (empty = BSP name)")
end

function GTAV.DeriveMapName()
	local override = GetConVar("gtav_map")
	if override then
		local name = override:GetString()
		if name and name ~= "" then return name end
	end
	return (string.gsub(game.GetMap(), "^sgm_", ""))
end

function GTAV.HasMapData(name)
	if not name or name == "" then return false end
	local base = "lossantos/" .. name .. "/"
	return file.Exists(base .. "index_tiles.dat", "DATA")
		or file.Exists(base .. "index.json", "DATA")
end

function GTAV.IsActive()
	return GTAV.MapData.current ~= nil
end

function GTAV.Deactivate()
	if not GTAV.MapData.current then return end
	GTAV.MapData.current = nil
	GTAV.MapData.name = nil
	GTAV.MapData.spawn_angles = nil
	GTAV.MapData.tiles = {}
	GTAV.MapData.materials = {}
	GTAV.MapData.phys_chunks = {}

	if CLIENT then
		if GTAV.Client and GTAV.Client.Deactivate then
			GTAV.Client.Deactivate()
		end
	elseif SERVER then
		local S = GTAV.Server
		if S then
			S.initialized = false
			S.ActiveChunks = {}
			S.TrackedProps = {}
			S.ChunkPoolFree = {}
			S.Frame = Vector(0, 0, 0)
			S.nextChunkTick = 0
			S.nextShiftTick = 0
		end
	end
end

-- string lod keys, JSONToTable reindexes numeric ones
function GTAV.GetTileInfo(tileX, tileY, lod)
	local key = tileX .. " " .. tileY
	local tileData = GTAV.MapData.tiles[key]
	if not tileData then return nil end
	return tileData["lod" .. tostring(lod or 0)]
end

function GTAV.HasTile(tileX, tileY)
	return GTAV.MapData.tiles[tileX .. " " .. tileY] ~= nil
end

-- get phys chunk info from index
function GTAV.GetPhysChunkInfo(chunkX, chunkY)
	return GTAV.MapData.phys_chunks[chunkX .. " " .. chunkY]
end

if CLIENT then
	CreateClientConVar("gtav_debug", "0", false, false, "Enable GTAV debug rendering")
	CreateClientConVar("gtav_debug_cells", "0", false, false, "Show cell boundaries")
	CreateClientConVar("gtav_debug_tiles", "0", false, false, "Show tile boundaries")
	CreateClientConVar("gtav_debug_phys", "0", false, false, "Show physics chunks")
	CreateClientConVar("gtav_debug_phys_mesh", "0", false, false,
		"Show the actual collision triangle meshes (not just chunk bounds)")
	CreateClientConVar("gtav_debug_lod", "0", false, false, "Show LOD levels")
	CreateClientConVar("gtav_hitch_log", "1", true, false, "Print one line per cell crossing with the worst frame time in its 0.5s window")
end

if SERVER then
	CreateConVar("gtav_sv_debug", "0", FCVAR_ARCHIVE, "Enable GTAV server debug")
	CreateConVar("gtav_sv_max_phys_ents", tostring(Config.MAX_ACTIVE_PHYS_ENTITIES), FCVAR_ARCHIVE, "Max active physics entities")
end
