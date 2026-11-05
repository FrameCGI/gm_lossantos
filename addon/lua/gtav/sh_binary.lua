GTAV.Binary = GTAV.Binary or {}
GTAV.Binary.PhysCache = GTAV.Binary.PhysCache or {}      -- raw chunk (absolute verts)
GTAV.Binary.PhysMeshCache = GTAV.Binary.PhysMeshCache or {} -- chunk-local MeshVertex list


local TILE_MAGIC = 0x31545647 -- "GVT1"
local CHUNK_MAGIC = 0x31435647 -- "GVC1"

-- little endian f32 from raw bytes
local POW2 = {}
for e = 0, 255 do POW2[e] = 2 ^ (e - 127) end

local function F32(b0, b1, b2, b3)
	local e = (b3 % 128) * 2 + math.floor(b2 / 128)
	local m = (b2 % 128) * 65536 + b1 * 256 + b0
	if e == 0 then
		local v = m * 1.401298464324817e-45 -- subnormal
		if b3 >= 128 then return -v end
		return v
	elseif e == 255 then
		return 0 -- inf/nan
	end
	local v = (1 + m / 8388608) * POW2[e]
	if b3 >= 128 then return -v end
	return v
end


local function U16(s, o)
	local a, b = s:byte(o, o + 1)
	if not b then return nil end
	return a + b * 256
end

local function U32(s, o)
	local a, b, c, d = s:byte(o, o + 3)
	if not d then return nil end
	return a + b * 256 + c * 65536 + d * 16777216
end


-- 0   magic "GVPG"
-- 4   version u16
-- 6   flags u16
-- 8   entry count
-- 12  reserved
-- 16  entries, 16 bytes each
--       0   name offset, absolute
--       4   blob offset, absolute
--       8   blob length
--       12  reserved
-- ..  name table, NUL terminated
-- ..  payloads back to back
local PAGE_MAGIC = 0x47505647 -- "GVPG"
local PAGE_SHIFT = 3          -- 8x8 tiles / chunks per page

GTAV.Binary.PageCache = GTAV.Binary.PageCache or {}

local function pageName(kind, coordX, coordY)
	return string.format("%s_%d_%d.bin",
		kind, math.floor(coordX / (2 ^ PAGE_SHIFT)), math.floor(coordY / (2 ^ PAGE_SHIFT)))
end

local function openPage(path)
	local f = file.Open(path, "rb", "DATA")
	if not f then return nil end

	local head = f:Read(16)
	if not head or #head < 16 or U32(head, 1) ~= PAGE_MAGIC then
		f:Close()
		return nil
	end

	local count = U32(head, 9)
	if not count or count <= 0 or count > 65536 then
		f:Close()
		return nil
	end

	local entries = f:Read(count * 16)
	if not entries or #entries < count * 16 then
		f:Close()
		return nil
	end

	local namesStart = 16 + count * 16
	local firstBlob = nil
	local parsed = {}
	local order = {}
	for i = 1, count do
		local o = (i - 1) * 16
		local nameOff = U32(entries, o + 1)
		local blobOff = U32(entries, o + 5)
		local blobLen = U32(entries, o + 9)
		if not (nameOff and blobOff and blobLen) then
			f:Close()
			return nil
		end
		order[i] = {nameOff = nameOff, blobOff = blobOff, blobLen = blobLen}
		if not firstBlob or blobOff < firstBlob then firstBlob = blobOff end
	end

	local namesLen = (firstBlob or namesStart) - namesStart
	if namesLen <= 0 or namesLen > 4 * 1024 * 1024 then
		f:Close()
		return nil
	end
	local namesBlob = f:Read(namesLen)
	f:Close()
	if not namesBlob or #namesBlob < namesLen then return nil end

	for i = 1, count do
		local rel = order[i].nameOff - namesStart
		if rel >= 0 and rel < namesLen then
			local name = string.match(namesBlob, "([^%z]*)", rel + 1)
			if name and name ~= "" then
				parsed[name] = {off = order[i].blobOff, len = order[i].blobLen}
			end
		end
	end

	return parsed
end

function GTAV.Binary.PageRead(kind, coordX, coordY, fileName)
	if not GTAV.MapData.name then return nil end
	if not coordX or not coordY or not fileName then return nil end

	local path = "lossantos/" .. tostring(GTAV.MapData.name) ..
		"/pages/" .. pageName(kind, coordX, coordY)
	local cache = GTAV.Binary.PageCache[path]
	if cache == false then return nil end
	if not cache then
		cache = openPage(path)
		GTAV.Binary.PageCache[path] = cache or false
		if not cache then return nil end
	end

	local entry = cache[fileName]
	if not entry then return nil end

	local f = file.Open(path, "rb", "DATA")
	if not f then return nil end
	f:Seek(entry.off)
	local blob = f:Read(entry.len)
	f:Close()
	if not blob or #blob ~= entry.len then return nil end
	return blob
end

-- page first, then the loose file
function GTAV.Binary.ReadWorldFile(filePath, kind, coordX, coordY)
	local fileName = string.match(filePath, "([^/\\]+)$")
	if fileName then
		local blob = GTAV.Binary.PageRead(kind, coordX, coordY, fileName)
		if blob then return blob end
	end
	return file.Read(filePath, "DATA")
end


-- 0   magic "GVT1"
-- 4   version u16
-- 6   flags u16, bit 0 lightmap uv, bit 1 normals
-- 8   vertex count
-- 12  index count
-- 16  submesh count
-- 20  material count
-- 24  vertex stride (28)
-- 28  reserved
-- 32  submeshes, 16 bytes each
--       0   material index
--       4   index offset
--       8   index count
--       12  vertex offset
-- ..  materials, 64 bytes each, NUL padded
-- ..  vertices, stride bytes each
--       0   x y z f32
--       12  u v f32
--       20  lightmap u v u16
--       24  normal i8 x y z + pad, 20 if no lightmap
-- ..  indices, u16, 0-based, clockwise
function GTAV.Binary.DecodeTile(blob, filePath)
	local total = #blob
	if total < 32 then return nil end

	if U32(blob, 1) ~= TILE_MAGIC then
		ErrorNoHalt("[GTAV] Invalid tile magic in " .. tostring(filePath) .. "\n")
		return nil
	end

	local version = U16(blob, 5)
	local flags = U16(blob, 7)
	local vertexCount = U32(blob, 9) or 0
	local indexCount = U32(blob, 13) or 0
	local submeshCount = U32(blob, 17) or 0
	local materialCount = U32(blob, 21) or 0
	local vertexStride = U32(blob, 25) or 0

	local hdrVerts, hdrIndices = vertexCount, indexCount

	-- corrupt header
	if vertexStride < 20 or vertexStride > 256 then return nil end
	if submeshCount > 8192 or materialCount > 8192 then return nil end
	if vertexCount > 262144 or indexCount > 1048576 then return nil end

	local hasLightmapUV = bit.band(flags, 1) ~= 0
	local hasNormals = bit.band(flags, 2) ~= 0

	local pos = 33
	local submeshes = {}
	for i = 1, submeshCount do
		local a = U32(blob, pos)
		local b = U32(blob, pos + 4)
		local c = U32(blob, pos + 8)
		local d = U32(blob, pos + 12)
		if not d then return nil end
		submeshes[i] = {
			materialIndex = a,
			indexOffset = b,
			indexCount = c,
			vertexOffset = d,
		}
		pos = pos + 16
	end

	local materials = {}
	for i = 1, materialCount do
		if pos + 63 > total then return nil end
		materials[i] = string.match(blob, "([^%z]*)", pos) or ""
		pos = pos + 64
	end

	local vertices = {}
	local vbase = pos
	local lastByte = vbase + vertexCount * vertexStride - 1
	if lastByte > total then
		vertexCount = math.max(0, math.floor((total - vbase) / vertexStride))
	end

	for i = 1, vertexCount do
		local o = vbase + (i - 1) * vertexStride
		-- ugly but one byte() call per vertex
		local b0, b1, b2, b3, b4, b5, b6, b7, b8, b9, b10, b11,
			  b12, b13, b14, b15, b16, b17, b18, b19,
			  b20, b21, b22, b23, b24, b25, b26 = blob:byte(o, o + math.min(26, vertexStride - 1))
		if not b0 or not b19 then break end

		local vert = {
			pos = Vector(F32(b0, b1, b2, b3), F32(b4, b5, b6, b7), F32(b8, b9, b10, b11)),
			uv = { x = F32(b12, b13, b14, b15), y = F32(b16, b17, b18, b19) },
		}
		if hasLightmapUV and b20 and b21 and b22 and b23 then
			vert.lm_uv = {
				x = (b20 + b21 * 256) / 65535.0,
				y = (b22 + b23 * 256) / 65535.0,
			}
		end
		if hasNormals then
			local nx, ny, nz
			if hasLightmapUV then
				nx, ny, nz = b24, b25, b26
			else
				nx, ny, nz = b20, b21, b22
			end
			if nx and ny and nz then
				if nx > 127 then nx = nx - 256 end
				if ny > 127 then ny = ny - 256 end
				if nz > 127 then nz = nz - 256 end
				vert.normal = Vector(nx / 127.0, ny / 127.0, nz / 127.0)
			end
		end
		vertices[i] = vert
	end

	local indices = {}
	local ibase = vbase + vertexCount * vertexStride
	-- +1 or the last triangle goes missing
	local usable = math.floor((total - ibase + 1) / 2)
	if usable < 0 then usable = 0 end
	if indexCount > usable then indexCount = usable end
	for i = 1, indexCount do
		local o = ibase + (i - 1) * 2
		local lo, hi = blob:byte(o, o + 1)
		if not hi then break end
		indices[i] = lo + hi * 256 + 1 -- 1-based for Lua
	end

	local nIdx = #indices
	local nVert = #vertices

	if nIdx ~= hdrIndices or nVert ~= hdrVerts then
		GTAV.Binary.BadTiles = (GTAV.Binary.BadTiles or 0) + 1
		if GTAV.Binary.BadTiles <= 5 then
			ErrorNoHalt(string.format(
				"[GTAV] %s: header says %d verts / %d indices, decoded %d / %d" ..
				" (submeshes %d, materials %d, stride %d, %d bytes)\n",
				tostring(filePath), hdrVerts, hdrIndices, nVert, nIdx,
				submeshCount, materialCount, vertexStride, total))
		end
	end

	for i = 1, #submeshes do
		local s = submeshes[i]
		local first = s.indexOffset or 0
		local count = s.indexCount or 0
		if first < 0 or count < 0 or first > nIdx then
			count = 0
		elseif first + count > nIdx then
			count = nIdx - first
		end
		s.indexCount = count
		local vfirst = s.vertexOffset or 0
		if vfirst < 0 or vfirst > nVert then s.vertexOffset = 0 end
	end

	return {
		version = version,
		flags = flags,
		hasLightmapUV = hasLightmapUV,
		hasNormals = hasNormals,
		vertexCount = nVert,
		indexCount = nIdx,
		submeshes = submeshes,
		materials = materials,
		vertices = vertices,
		indices = indices,
	}
end

function GTAV.Binary.ReadTile(filePath, tileX, tileY)
	local blob = GTAV.Binary.ReadWorldFile(filePath, "v", tileX, tileY)
	if not blob then return nil end
	return GTAV.Binary.DecodeTile(blob, filePath)
end


-- 0   magic "GVC1"
-- 4   version u16
-- 6   flags u16, reserved
-- 8   triangle count
-- 12  vertex count
-- 16  reserved
-- 20  vertices, 3 f32 each, absolute
-- ..  triangles, 3 u16 each, 0-based, clockwise
function GTAV.Binary.DecodePhysChunk(blob, filePath)
	local total = #blob
	if total < 20 then return nil end

	if U32(blob, 1) ~= CHUNK_MAGIC then
		ErrorNoHalt("[GTAV] Invalid phys chunk magic in " .. tostring(filePath) .. "\n")
		return nil
	end

	local version = U16(blob, 5)
	local flags = U16(blob, 7)
	local triCount = U32(blob, 9) or 0
	local vertCount = U32(blob, 13) or 0
	-- 17..20 reserved

	if vertCount > 262144 or triCount > 262144 then return nil end
	if 20 + vertCount * 12 + triCount * 6 > total then return nil end

	local vertices = {}
	local off = 21
	for i = 1, vertCount do
		local b0, b1, b2, b3, b4, b5, b6, b7, b8, b9, b10, b11 = blob:byte(off, off + 11)
		if not b11 then break end
		vertices[i] = Vector(F32(b0, b1, b2, b3), F32(b4, b5, b6, b7), F32(b8, b9, b10, b11))
		off = off + 12
	end

	local indices = {}
	for i = 1, triCount * 3 do
		local lo, hi = blob:byte(off, off + 1)
		if not hi then break end
		indices[i] = lo + hi * 256 + 1
		off = off + 2
	end

	-- bounds check once
	for i = 1, #indices do
		if indices[i] > vertCount then
			ErrorNoHalt(string.format("[GTAV] Phys chunk index out of bounds in %s\n", tostring(filePath)))
			return nil
		end
	end

	return {
		version = version,
		flags = flags,
		triangleCount = #indices / 3,
		vertexCount = #vertices,
		vertices = vertices,
		indices = indices,
	}
end

function GTAV.Binary.ReadPhysChunk(filePath, chunkX, chunkY)
	local cached = GTAV.Binary.PhysCache[filePath]
	if cached then return cached end

	local blob = GTAV.Binary.ReadWorldFile(filePath, "p", chunkX, chunkY)
	if not blob then return nil end

	local result = GTAV.Binary.DecodePhysChunk(blob, filePath)
	if not result then return nil end

	GTAV.Binary.PhysCache[filePath] = result
	return result
end

function GTAV.Binary.GetPhysMesh(chunkX, chunkY)
	local key = chunkX .. " " .. chunkY
	local cached = GTAV.Binary.PhysMeshCache[key]
	if cached ~= nil then return cached or nil end

	local info = GTAV.GetPhysChunkInfo(chunkX, chunkY)
	if not info then
		GTAV.Binary.PhysMeshCache[key] = false
		return nil
	end

	local path = "lossantos/" .. tostring(GTAV.MapData.name or "gm_lossantos") ..
		"/phys/" .. info.file
	local data = GTAV.Binary.ReadPhysChunk(path, chunkX, chunkY)
	if not data or data.triangleCount == 0 then
		GTAV.Binary.PhysMeshCache[key] = false
		return nil
	end

	local ps = GTAV.Config.PHYS_CHUNK_SIZE
	local origin = Vector(chunkX * ps, chunkY * ps, 0)

	local mesh = {}
	local verts, idx = data.vertices, data.indices
	for i = 1, #idx do
		mesh[i] = verts[idx[i]] - origin
	end

	GTAV.Binary.PhysMeshCache[key] = mesh
	return mesh
end

GTAV.Binary.ReadQueue = GTAV.Binary.ReadQueue or {}

function GTAV.Binary.QueueTileRead(filePath, callback, tileX, tileY)
	table.insert(GTAV.Binary.ReadQueue,
		{path = filePath, callback = callback, tx = tileX, ty = tileY})
end

function GTAV.Binary.ProcessReadQueue(maxTime)
	maxTime = maxTime or 0.002
	local startTime = SysTime()

	while #GTAV.Binary.ReadQueue > 0 and (SysTime() - startTime) < maxTime do
		local task = table.remove(GTAV.Binary.ReadQueue, 1)
		local data = GTAV.Binary.ReadTile(task.path, task.tx, task.ty)
		if task.callback then
			task.callback(data)
		end
	end
end
