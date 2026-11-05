local mapDir = arg[1]
local dumpPath = arg[2]
assert(mapDir and dumpPath, "usage: index_test.lua <map_dir> <dump_path>")

-- json hides the .dat files, forces the fallback
local forceJson = (arg[3] == "json")


local DATA_ROOT = mapDir

local FileStub = {}
FileStub.__index = FileStub

function FileStub:ReadLine(maxLength)
	local line = self.handle:read("*l")
	if not line then return nil end
	if maxLength and #line > maxLength then
		self.overlong = (self.overlong or 0) + 1
	end
	-- keep the terminator like gmod
	return line .. "\n"
end

function FileStub:Close()
	self.handle:close()
end

file = {}
function file.Open(path, mode, where)
	assert(where == "DATA", "only the DATA realm is stubbed")
	if forceJson and string.sub(path, -4) == ".dat" then return nil end
	local full = DATA_ROOT .. "/" .. path:gsub("^lossantos/[%w_]+/", "")
	local h = io.open(full, (mode == "rb") and "rb" or "r")
	if not h then return nil end
	return setmetatable({ handle = h, path = full }, FileStub)
end

function file.Read(path, where)
	assert(where == "DATA", "only the DATA realm is stubbed")
	local full = DATA_ROOT .. "/" .. path:gsub("^lossantos/[%w_]+/", "")
	local h = io.open(full, "rb")
	if not h then return nil end
	local data = h:read("*a")
	h:close()
	return data
end

function file.Exists(path, where)
	local full = DATA_ROOT .. "/" .. path:gsub("^lossantos/[%w_]+/", "")
	local h = io.open(full, "rb")
	if h then h:close() return true end
	return false
end

function table.Count(t)
	local n = 0
	for _ in pairs(t) do n = n + 1 end
	return n
end

function ErrorNoHalt(msg)
	io.stderr:write(msg)
end

function Vector(x, y, z)
	return { x = x, y = y, z = z }
end

-- tiny json reader for the fallback and J record
local function jsonParse(s)
	local pos = 1
	local skip
	local parseValue

	local function peek()
		return s:sub(pos, pos)
	end

	skip = function()
		while pos <= #s do
			local c = s:sub(pos, pos)
			if c == " " or c == "\t" or c == "\n" or c == "\r" then
				pos = pos + 1
			else
				break
			end
		end
	end

	local function parseString()
		pos = pos + 1
		local parts = {}
		while pos <= #s do
			local c = s:sub(pos, pos)
			if c == '"' then
				pos = pos + 1
				return table.concat(parts)
			elseif c == "\\" then
				local n = s:sub(pos + 1, pos + 1)
				local map = { n = "\n", t = "\t", r = "\r", b = "\b", f = "\f",
							  ['"'] = '"', ["\\"] = "\\", ["/"] = "/" }
				parts[#parts + 1] = map[n] or n
				pos = pos + 2
			else
				parts[#parts + 1] = c
				pos = pos + 1
			end
		end
		error("unterminated string")
	end

	parseValue = function()
		skip()
		local c = peek()
		if c == "{" then
			pos = pos + 1
			local obj = {}
			skip()
			if peek() == "}" then pos = pos + 1 return obj end
			while true do
				skip()
				local key = parseString()
				skip()
				assert(peek() == ":", "expected :")
				pos = pos + 1
				obj[key] = parseValue()
				skip()
				local d = peek()
				pos = pos + 1
				if d == "}" then return obj end
				assert(d == ",", "expected , or }")
			end
		elseif c == "[" then
			pos = pos + 1
			local arr = {}
			skip()
			if peek() == "]" then pos = pos + 1 return arr end
			while true do
				arr[#arr + 1] = parseValue()
				skip()
				local d = peek()
				pos = pos + 1
				if d == "]" then return arr end
				assert(d == ",", "expected , or ]")
			end
		elseif c == '"' then
			return parseString()
		elseif s:sub(pos, pos + 3) == "true" then
			pos = pos + 4
			return true
		elseif s:sub(pos, pos + 4) == "false" then
			pos = pos + 5
			return false
		elseif s:sub(pos, pos + 3) == "null" then
			pos = pos + 4
			return nil
		else
			local num = s:match("^-?%d+%.?%d*[eE]?[+-]?%d*", pos)
			assert(num and #num > 0, "bad value at " .. pos)
			pos = pos + #num
			return tonumber(num)
		end
	end

	return parseValue()
end

util = { JSONToTable = jsonParse }


local addon = "C:/Program Files (x86)/Steam/steamapps/common/GarrysMod/" ..
			  "garrysmod/addons/lossantos/lua/gtav/sh_config.lua"
local chunk, err = loadfile(addon)
assert(chunk, "could not compile sh_config.lua: " .. tostring(err))
chunk()

assert(GTAV and GTAV.LoadMapIndex, "LoadMapIndex did not load")

local t0 = os.clock()
local ok = GTAV.LoadMapIndex("lossantos")
local elapsed = os.clock() - t0

print(string.format("[index_test] LoadMapIndex -> %s in %.2fs",
	tostring(ok), elapsed))
assert(ok, "LoadMapIndex failed")

local M = GTAV.MapData


-- canonical dump, gets diffed against index.json
local out = assert(io.open(dumpPath, "wb"))
local function emit(fmt, ...)
	out:write(string.format(fmt, ...) .. "\n")
end

emit("SCALARS")
emit("tile_size %s", tostring(M.current.tile_size))
emit("cell_size %s", tostring(M.current.cell_size))
emit("phys_chunk_size %s", tostring(M.current.phys_chunk_size))
emit("stream_radius %s", tostring(M.current.stream_radius))
emit("max_tiles %s", tostring(M.current.max_tiles))
emit("merge_grid %s", tostring(M.current.merge_grid))
emit("map %s", tostring(M.current.map))

emit("FOG %s %s %s %s %s %s",
	tostring(M.current.fog.enabled),
	tostring(M.current.fog.near), tostring(M.current.fog.far),
	tostring(M.current.fog.color[1]), tostring(M.current.fog.color[2]),
	tostring(M.current.fog.color[3]))

emit("SPAWN %s %s %s", tostring(M.spawn.x), tostring(M.spawn.y),
	tostring(M.spawn.z))
emit("ANGLES %s %s %s", tostring(M.spawn_angles[1]),
	tostring(M.spawn_angles[2]), tostring(M.spawn_angles[3]))

emit("SOURCE %s", tostring(M.current.source and M.current.source.chunks or "nil"))

emit("MATERIALS %d", table.Count(M.materials))
local mids = {}
for k in pairs(M.materials) do mids[#mids + 1] = k end
table.sort(mids, function(a, b) return tonumber(a) < tonumber(b) end)
for i = 1, #mids do
	emit("M\t%s\t%s", mids[i], M.materials[mids[i]])
end

emit("TILES %d", table.Count(M.tiles))
local keys = {}
for k in pairs(M.tiles) do keys[#keys + 1] = k end
table.sort(keys, function(a, b)
	local ax, ay = string.match(a, "^(%-?%d+) (%-?%d+)$")
	local bx, by = string.match(b, "^(%-?%d+) (%-?%d+)$")
	if ax == bx then return tonumber(ay) < tonumber(by) end
	return tonumber(ax) < tonumber(bx)
end)
for i = 1, #keys do
	local lods = M.tiles[keys[i]]
	local f = {}
	for lod = 0, 4 do
		local e = lods["lod" .. lod]
		f[lod + 1] = (e and e.file) or "-"
	end
	emit("T\t%s\t%s\t%s\t%s\t%s\t%s", keys[i], f[1], f[2], f[3], f[4], f[5])
end

emit("PHYS %d", table.Count(M.phys_chunks))
local pkeys = {}
for k in pairs(M.phys_chunks) do pkeys[#pkeys + 1] = k end
table.sort(pkeys, function(a, b)
	local ax, ay = string.match(a, "^(%-?%d+) (%-?%d+)$")
	local bx, by = string.match(b, "^(%-?%d+) (%-?%d+)$")
	if ax == bx then return tonumber(ay) < tonumber(by) end
	return tonumber(ax) < tonumber(bx)
end)
for i = 1, #pkeys do
	emit("P\t%s\t%s", pkeys[i], M.phys_chunks[pkeys[i]].file)
end

out:close()

print(string.format(
	"[index_test] dumped %d tiles, %d phys chunks, %d materials",
	table.Count(M.tiles), table.Count(M.phys_chunks), table.Count(M.materials)))
print("[index_test] OK")
