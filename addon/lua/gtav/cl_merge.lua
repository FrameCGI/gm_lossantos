GTAV = GTAV or {}
GTAV.Client = GTAV.Client or {}
GTAV.Merge = GTAV.Merge or {}

local C = GTAV.Client
local M = GTAV.Merge

CreateClientConVar("gtav_cl_merge_grid", "0", false, false,
	"Tiles merged into one draw set per axis (0 = map default, 1 = no merge)")

function M.Grid()
	local cv = GetConVar("gtav_cl_merge_grid")
	local g = cv and cv:GetInt() or 0
	if g <= 0 then
		local idx = GTAV.MapData and GTAV.MapData.current
		g = (idx and tonumber(idx.merge_grid)) or 8
	end
	if g < 1 then g = 1 end
	if g > 32 then g = 32 end
	return g
end


-- grid is in the key because i got burned by this. dont change it
function M.EnsureList()
	local grid = M.Grid()
	if C.GroupListIndex == GTAV.MapData.current and C.GroupList
		and C.MergeGridUsed == grid then
		return C.GroupList
	end
	if C.GroupList and C.MergeGridUsed and C.MergeGridUsed ~= grid then
		M.FlushForGridChange()
	end
	local ts = GTAV.Config.VISUAL_TILE_SIZE
	local cs = GTAV.Config.CELL_SIZE
	local span = grid * ts

	local list = {}
	local cells = {}
	local byKey = {}

	local zb = C.TileZBounds and C.TileZBounds() or nil

	for key in pairs(GTAV.MapData.tiles) do
		local sx, sy = string.match(key, "^(%-?%d+) (%-?%d+)$")
		local tx, ty = tonumber(sx), tonumber(sy)
		if tx and ty then
			local gx = math.floor(tx / grid)
			local gy = math.floor(ty / grid)
			-- grid in the key, group 0 0 differs per grid
			local gkey = gx .. " " .. gy .. "@" .. grid

			local g = byKey[gkey]
			if not g then
				g = {
					key = gkey,
					gx = gx,
					gy = gy,
					tx = gx * grid,
					ty = gy * grid,
					span = grid,
					center = Vector(gx * span + span * 0.5,
									gy * span + span * 0.5, 0),
					members = {},
				}
				byKey[gkey] = g
				list[#list + 1] = g

				local cellKey = math.floor(g.center.x / cs) .. " " ..
								math.floor(g.center.y / cs)
				local bucket = cells[cellKey]
				if not bucket then
					bucket = {}
					cells[cellKey] = bucket
				end
				bucket[#bucket + 1] = g
			end

			g.members[#g.members + 1] = {tx = tx, ty = ty, key = key}
			if zb then
				local b = zb[key]
				local z0 = b and tonumber(b[1])
				local z1 = b and tonumber(b[2])
				if z0 and z1 then
					if not g.z0 or z0 < g.z0 then g.z0 = z0 end
					if not g.z1 or z1 > g.z1 then g.z1 = z1 end
				end
			end
		end
	end

	C.GroupList = list
	C.TileCells = cells
	C.GroupListIndex = GTAV.MapData.current
	C.MergeGridUsed = grid

	print(string.format("[GTAV] merge grid %d: %d tiles -> %d draw groups",
		grid, table.Count(GTAV.MapData.tiles), #list))

	return list
end

function M.FlushForGridChange()
	for key, tile in pairs(C.ActiveTiles) do
		C.FreeTile(tile)
		C.ActiveTiles[key] = nil
	end

	local pend = C.InstallPending or {}
	for i = 1, #pend do
		local e = pend[i]
		if e and e.tile then C.FreeTile(e.tile) end
	end
	C.InstallPending = {}
	C.InstallSet = {}

	local wrap = C.WrapTasks or {}
	for i = 1, #wrap do
		local subs = wrap[i] and wrap[i].subs
		if subs then
			for s = 1, #subs do
				local m = subs[s].mesh
				if m and m.Destroy then pcall(m.Destroy, m) end
			end
		end
	end
	C.WrapTasks = {}
	C.WrapSet = {}

	C.BuildQueue = {}
	C.BuildHead = 1
	C.LastQueueTime = nil
	C.LastQueueEye = nil
	C.GroupList = nil
	C.GroupListIndex = nil
	C.TileCells = nil
	C.DrawOrder = nil
	C.GlassOrder = nil
	C.DrawOrderDirty = true
end

cvars.AddChangeCallback("gtav_cl_merge_grid", function()
	if not (GTAV and GTAV.MapData and GTAV.MapData.current) then
		return
	end
	if C.MergeGridUsed and C.MergeGridUsed == M.Grid() then return end
	print("[GTAV] merge grid changed - re-streaming the world")
	M.FlushForGridChange()
end, "GTAVMergeGrid")


function M.MemberFiles(g, lod)
	local out = {}
	for i = 1, #g.members do
		local m = g.members[i]
		local lods = GTAV.MapData.tiles[m.key]
		local info = lods and lods["lod" .. tostring(lod)]

		-- nearest lod slot, down then up
		if not info then
			for alt = lod - 1, 0, -1 do
				info = lods and lods["lod" .. tostring(alt)]
				if info then break end
			end
		end
		if not info then
			for alt = lod + 1, 4 do
				info = lods and lods["lod" .. tostring(alt)]
				if info then break end
			end
		end

		if info and info.file then
			out[#out + 1] = {file = info.file, tx = m.tx, ty = m.ty}
		end
	end
	return out
end

function M.Build(g, lod)
	local ts = GTAV.Config.VISUAL_TILE_SIZE
	local files = M.MemberFiles(g, lod)
	if #files == 0 then return nil, "no file for LOD" .. tostring(lod) end

	if GTAV.Native and GTAV.Native.Available then
		local okB, subs, merged = pcall(GTAV.Native.BuildGroup,
			files, g.tx, g.ty, ts)
		if okB and subs and #subs > 0 then
			C.NativeGroups = (C.NativeGroups or 0) + 1
			C.NativeGroupTiles = (C.NativeGroupTiles or 0) + (merged or #files)
			return subs
		end

		-- lua fallback is way slower, log why
		local why
		if okB then
			why = tostring(merged or "build_group returned nothing")
		else
			why = "error: " .. tostring(subs)
		end
		C.FallbackReasons = C.FallbackReasons or {}
		C.FallbackGroups = (C.FallbackGroups or 0) + 1
		local seen = (C.FallbackReasons[why] or 0) + 1
		C.FallbackReasons[why] = seen
		if seen <= 3 then
			ErrorNoHalt(string.format(
				"[GTAV] native group build failed: %s - falling back to the slow Lua builder for %d tiles\n",
				why, #files))
		end
	else
		C.NoNativeGroups = (C.NoNativeGroups or 0) + 1
	end

	local origin = Vector(g.tx * ts, g.ty * ts, 0)
	local out = {}
	for i = 1, #files do
		local f = files[i]
		local tile = C.BuildTileMesh({
			key = g.key .. "#" .. i,
			tx = f.tx,
			ty = f.ty,
			lod = lod,
			file = f.file,          -- already resolved by memberFiles()
			origin = origin,        -- make the vertices group-relative
		})
		if tile and tile.submeshes then
			for s = 1, #tile.submeshes do
				out[#out + 1] = tile.submeshes[s]
			end
		end
	end

	if #out == 0 then return nil, "nothing built" end
	C.LuaGroups = (C.LuaGroups or 0) + 1
	return out
end

function M.Free(submeshes)
	if not submeshes then return end
	for i = 1, #submeshes do
		local m = submeshes[i].mesh
		if m and m.Destroy then pcall(m.Destroy, m) end
	end
end

local function groupTile(task, subs)
	local ts = GTAV.Config.VISUAL_TILE_SIZE
	local tris = 0
	for i = 1, #subs do
		tris = tris + (subs[i].tris or 0)
	end

	return {
		key = task.key,
		tx = task.tx,
		ty = task.ty,
		span = task.span or 1,
		lod = task.lod,
		z0 = task.z0,
		z1 = task.z1,
		origin = Vector(task.tx * ts, task.ty * ts, 0),
		center = task.center,
		submeshes = subs,
		submeshCount = #subs,
		triCount = tris,
		lastWanted = CurTime(),
	}
end

function C.BuildGroupMesh(task)
	local ts = GTAV.Config.VISUAL_TILE_SIZE
	local subs, why = M.Build(task, task.lod)
	if not subs then
		C.BuildMiss = (C.BuildMiss or 0) + 1
		if C.BuildMiss <= 5 then
			print(string.format("[GTAV] group %s LOD%d failed: %s",
				tostring(task.key), task.lod, tostring(why)))
		end
		return nil
	end

	return groupTile(task, subs)
end

function C.BuildGroupPrep(task, preTask)
	local N = GTAV.Native
	if not (N and N.Available and N.BuildGroupPrep) then
		return nil, "no async builder"
	end
	local ts = GTAV.Config.VISUAL_TILE_SIZE
	local files = M.MemberFiles(task, task.lod)
	if #files == 0 then
		return nil, "no file for LOD" .. tostring(task.lod)
	end

	local preFiles = nil
	if preTask then
		local okP, list = pcall(M.MemberFiles, preTask, preTask.lod)
		if okP and list and #list > 0 then preFiles = list end
	end

	local ok, err = N.BuildGroupPrep(files, task.tx, task.ty, ts, preFiles)
	if not ok then return nil, err end
	return files
end

function C.BuildGroupCommitStart(task)
	task.subs = {}
	return true
end

function C.BuildGroupFinish(task, wrapBudget)
	local N = GTAV.Native

	-- phase 1 mesh slices
	if not task.emitDone then
		local budget = 10
		do
			local cv = GetConVar("gtav_cl_commit_budget_ms")
			if cv then budget = cv:GetFloat() end
		end

		local out, done = N.BuildGroupCommit(budget)
		if not out then
			C.BuildMiss = (C.BuildMiss or 0) + 1
			if C.BuildMiss <= 5 then
				print(string.format("[GTAV] group %s LOD%d commit failed: %s",
					tostring(task.key), task.lod, tostring(done)))
			end
			return nil, done
		end

		local subs = task.subs or {}
		task.subs = subs
		for i = 1, #out do subs[#subs + 1] = out[i] end

		if not done then return nil, "more", 0 end
		task.emitDone = true
	end

	-- phase 2 material wrap
	local wBudget = wrapBudget
	if not wBudget then
		wBudget = 8
		do
			local cv = GetConVar("gtav_cl_wrap_budget_ms")
			if cv then wBudget = cv:GetFloat() end
		end
	end
	if wBudget < 1 then wBudget = 1 end

	task.wrapState = task.wrapState or {cur = 1}
	local tw = SysTime()
	local wrapped = N.WrapSubsBudgeted(task.subs, task.wrapState, wBudget)
	local wrapMs = (SysTime() - tw) * 1000
	task.wrapMs = (task.wrapMs or 0) + wrapMs
	if not wrapped then
		return nil, "warming", wrapMs
	end

	local subs = task.subs or {}
	if #subs == 0 then
		C.BuildMiss = (C.BuildMiss or 0) + 1
		if C.BuildMiss <= 5 then
			print(string.format("[GTAV] group %s LOD%d failed: no drawable material in the group",
				tostring(task.key), task.lod))
		end
		return nil, "no drawable material in the group"
	end

	C.AsyncGroups = (C.AsyncGroups or 0) + 1
	return groupTile(task, subs), "done", wrapMs
end
