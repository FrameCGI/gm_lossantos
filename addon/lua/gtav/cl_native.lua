GTAV = GTAV or {}
GTAV.Native = GTAV.Native or {}
local N = GTAV.Native

N.Available = false
N.Version = nil
N.FastVectors = false
N.LoadError = nil


local MODULE_PATHS = {
	"lua/bin/gmcl_gtav_win64.dll",   -- client realm
	"lua/bin/gmsv_gtav_win64.dll",   -- server realm (listen server)
}

local loadlib = package and package.loadlib or nil

-- probe by hand, require errors say nothing
print("[GTAV] native probe:")
local openFn, closeFn
for _, path in ipairs(MODULE_PATHS) do
	local exists = file.Exists(path, "GAME")
	local size = exists and file.Size(path, "GAME") or -1
	print(string.format("[GTAV]   file %s exists=%s size=%d",
		path, exists and "yes" or "no", size))

	if exists and loadlib then
		for _, sym in ipairs({ "gmod13_open", "gmod13_close", "gmod_open", "gmod_close" }) do
			local fn, err = loadlib(path, sym)
			if fn then
				print(string.format("[GTAV]   %s %s -> resolved", path, sym))
				if sym == "gmod13_open" then openFn = fn end
				if sym == "gmod13_close" then closeFn = fn end
			else
				print(string.format("[GTAV]   %s %s -> %s", path, sym, tostring(err)))
			end
		end
	elseif exists then
		print("[GTAV]   package.loadlib is unavailable in this realm, no raw probe")
	end
end

local ok, err = false, "not installed"
-- no dll, no require, it prints a red error
if file.Exists(MODULE_PATHS[1], "GAME") then
	ok, err = pcall(require, "gtav")
end

local mod = rawget(_G, "gtav")

-- require can fail on a fine dll, open it by hand
-- require lies sometimes so just call it by hand. this fucking sucks but okay it works ig
if not (ok and type(mod) == "table" and mod.version) and openFn then
	print("[GTAV] require() failed, trying gmod13_open directly")
	local rok, a, b = pcall(openFn)
	if not rok then
		print("[GTAV] gmod13_open raised: " .. tostring(a))
	elseif a == nil and b ~= nil then
		print("[GTAV] gmod13_open reported: " .. tostring(b))
	end
	mod = rawget(_G, "gtav")
end

if ok and not (type(mod) == "table" and mod.version) then
	err = "module loaded but registered no 'gtav' table"
end

if type(mod) == "table" and mod.version then
	N.Module = mod
	N.Available = true
	N.EntryOpen = openFn ~= nil
	N.EntryClose = closeFn ~= nil

	local vok, ver = pcall(mod.version)
	if vok then N.Version = ver end

	local fok, fast = pcall(mod.fast_vectors)
	if fok then N.FastVectors = fast and true or false end

	print(string.format("[GTAV] native module %s (%s vectors)",
		tostring(N.Version or "?"),
		N.FastVectors and "fast" or "slow"))
else
	N.LoadError = tostring(err or "module did not register an 'gtav' table")
	print("[GTAV] native module unavailable: " .. N.LoadError)
	print("[GTAV] falling back to the Lua builder (slower, same output)")
end


-- mesh.Begin outside render hooks is undocumented but it works
CreateClientConVar("gtav_cl_mesh_builder", "1", false, false,
	"Native mesh path: 0 = Mesh:BuildFromTriangles (safe), 1 = mesh.Begin (faster)")

function N.ApplyBuilderMode()
	if not N.Available or not N.Module.builder_mode then
		N.BuilderFast = false
		return false
	end
	if N.BuilderBlocked then
		pcall(N.Module.builder_mode, 0)
		N.BuilderFast = false
		return false
	end
	local cv = GetConVar("gtav_cl_mesh_builder")
	local want = (cv and cv:GetBool()) and 1 or 0
	local okB, eff = pcall(N.Module.builder_mode, want)
	N.BuilderFast = okB and eff and true or false
	return N.BuilderFast
end

if N.Available then
	N.ApplyBuilderMode()
	cvars.AddChangeCallback("gtav_cl_mesh_builder", function()
		N.BuilderBlocked = nil
		local on = N.ApplyBuilderMode()
		print(string.format("[GTAV] mesh builder -> %s",
			on and "mesh.Begin (fast)" or "Mesh:BuildFromTriangles (safe)"))
	end, "GTAV_MeshBuilder")
end


function N.InitMap(mapName)
	if not N.Available then return false end
	if not mapName or mapName == "" then return false end
	if N.MapName == mapName then return true end

	local okInit, res = pcall(N.Module.pages_init, mapName)
	if not okInit or not res then
		N.LoadError = "pages_init failed for " .. tostring(mapName)
		print("[GTAV] native: " .. N.LoadError)
		return false
	end

	N.MapName = mapName
	-- old map builds must not land here
	N.BuildGroupCancel()
	if GTAV.Client then
		GTAV.Client.AsyncTask = nil
		GTAV.Client.ReadyTask = nil
		GTAV.Client.CommitTask = nil
	end
	print(string.format("[GTAV] native pages: %s",
		tostring(N.Module.pages_mapped and N.Module.pages_mapped() or "?")))
	N.ShadowInit()
	N.ApplyLighting()
	N.PrefetchInit()
	return true
end

CreateClientConVar("gtav_cl_file_prefetch", "0", false, false,
	"Read material files on a background thread ahead of Material() (1 = on; measured no gain, off by default)")

function N.PrefetchInit()
	N.PrefetchOn = false
	if not (N.Available and N.Module and N.Module.prefetch_init) then return false end
	local cv = GetConVar("gtav_cl_file_prefetch")
	if cv and not cv:GetBool() then return false end
	local ok, started, info = pcall(N.Module.prefetch_init, "lossantos")
	if not ok then
		print("[GTAV] native: prefetch_init error: " .. tostring(started))
		return false
	end
	print("[GTAV] native: file prefetch " ..
		(started and ("on, root " .. tostring(info)) or ("off: " .. tostring(info))))
	N.PrefetchOn = started and true or false
	return N.PrefetchOn
end

function N.Prefetch(names)
	if not N.PrefetchOn then return 0 end
	local ok, n = pcall(N.Module.prefetch_add, names)
	return ok and n or 0
end

function N.PrefetchStats()
	if not (N.PrefetchOn and N.Module.prefetch_stats) then return nil end
	local ok, s = pcall(N.Module.prefetch_stats)
	return ok and s or nil
end

function N.EnsureMap()
	if not N.Available or N.MapName then return N.MapName ~= nil end
	local name = GTAV.MapData and GTAV.MapData.name
	if not name then return false end
	return N.InitMap(name)
end

function N.SetPageBudget(pages)
	if N.Available and N.Module.pages_budget then
		pcall(N.Module.pages_budget, math.max(1, math.floor(pages or 24)))
	end
end


function N.ShadowInit()
	if not (N.Available and N.Module and N.Module.shadow_init) then
		return false
	end
	local ok, loaded, info = pcall(N.Module.shadow_init)
	if not ok then
		print("[GTAV] native: shadow_init error: " .. tostring(loaded))
		return false
	end
	print("[GTAV] native: " .. tostring(info))
	return loaded and true or false
end

function N.ApplyLighting()
	if not (N.Available and N.Module) then return end
	local cvBaked = GetConVar("gtav_cl_baked_lighting")
	local cvAmb = GetConVar("gtav_cl_sun_ambient")
	local cvSun = GetConVar("gtav_cl_sun_intensity")
	local cvExpo = GetConVar("gtav_cl_light_exposure")
	local baked, amb, sun, expo, missing = true, 2.0, 1.0, 2.5, {}
	-- nil checks, or a missing convar just looks default
	if cvBaked ~= nil then baked = cvBaked:GetBool() else missing[#missing + 1] = "baked" end
	if cvAmb ~= nil then amb = cvAmb:GetFloat() else missing[#missing + 1] = "ambient" end
	if cvSun ~= nil then sun = cvSun:GetFloat() else missing[#missing + 1] = "sun" end
	if cvExpo ~= nil then expo = cvExpo:GetFloat() else missing[#missing + 1] = "exposure" end
	if N.Module.baked_lighting and N.Module.light_config then
		local okB, retB = pcall(N.Module.baked_lighting, baked)
		local okL, retA, retS, retE = pcall(N.Module.light_config, amb, sun, expo)
		print(string.format(
			"[GTAV] lighting: gate %s (ok %s) | ambient %.2f sun %.2f exposure %.2f (ok %s) | cv '%s'/'%s'/'%s'%s",
			tostring(retB), tostring(okB),
			tonumber(retA) or -1, tonumber(retS) or -1, tonumber(retE) or -1, tostring(okL),
			cvBaked and cvBaked:GetString() or "nil",
			cvAmb and cvAmb:GetString() or "nil",
			cvSun and cvSun:GetString() or "nil",
			#missing > 0 and (" MISSING " .. table.concat(missing, ",")) or ""))
	end
end

if N.Available then
	local function lightingChanged(name, old, new)
		GTAV.LightStamp = (GTAV.LightStamp or 0) + 1
		print(string.format("[GTAV] lighting change: %s %s -> %s",
			tostring(name), tostring(old), tostring(new)))
		N.ApplyLighting()   -- logs the module's own gate/light state
	end
	cvars.AddChangeCallback("gtav_cl_baked_lighting", lightingChanged,
		"GTAV_BakedLighting")
	cvars.AddChangeCallback("gtav_cl_sun_ambient", lightingChanged,
		"GTAV_SunAmbient")
	cvars.AddChangeCallback("gtav_cl_sun_intensity", lightingChanged,
		"GTAV_SunIntensity")
	cvars.AddChangeCallback("gtav_cl_light_exposure", lightingChanged,
		"GTAV_LightExposure")
end


local flagTable = nil
local flagMT = nil

function N.FlagTable()
	if flagTable then return flagTable end

	flagTable = {}
	flagMT = {
		__index = function(t, name)
			local flags = nil
			if GTAV.Client and GTAV.Client.GetMaterialFlags then
				local okF, f = pcall(GTAV.Client.GetMaterialFlags, name)
				if okF then flags = f end
			end
			flags = flags or {glass = false, nocull = false}
			rawset(t, name, flags)
			return flags
		end,
	}
	setmetatable(flagTable, flagMT)
	return flagTable
end

function N.ResetFlags()
	flagTable = nil
end


function N.BuildTile(file, tx, ty, tileSize)
	if not N.Available then return nil, "no native module" end
	if not N.EnsureMap() then return nil, "no map pages" end

	local okB, res, errB = pcall(N.Module.build_tile, file, tx, ty, tileSize,
								 N.FlagTable())
	if not okB then return nil, tostring(res) end
	if not res then return nil, tostring(errB or "build_tile failed") end

	local getMat = GTAV.Client and GTAV.Client.GetMaterial
	local out = {}
	for i = 1, #res do
		local e = res[i]
		local mat = nil
		if getMat then
			local okM, m = pcall(getMat, e.material)
			if okM then mat = m end
		end
		out[#out + 1] = {
			mesh = e.mesh,
			mat = mat,
			glass = e.glass and true or false,
			tris = e.tris,
			mins = e.mins,
			maxs = e.maxs,
		}
	end

	if #out == 0 then return nil, "no drawable submeshes" end
	return out
end

function N.TileInfo(file, tx, ty)
	if not N.Available then return nil end
	if not N.EnsureMap() then return nil end
	local okI, res = pcall(N.Module.tile_info, file, tx, ty)
	if not okI then return nil end
	return res
end

function N.AfterBuild(subs)
	do
		local _why, path, _push, _build, fastFails = N.MeshInfo()
		if path then N.LastPath = path end
		N.LastStats = N.BuildStats()

		local fails = tonumber(fastFails) or 0
		if N.BuilderFast and not N.BuilderBlocked and fails > (N.FastFailsSeen or 0) then
			N.BuilderBlocked = true
			N.ApplyBuilderMode()
			ErrorNoHalt(string.format(
				"[GTAV] mesh.Begin was rejected (%d chunk(s) fell back) - " ..
				"using Mesh:BuildFromTriangles for the rest of this session; " ..
				"gtav_cl_mesh_builder 1 forces the fast path again\n", fails))
		end
		N.FastFailsSeen = fails

		local s = N.LastStats
		if s then
			N.MeshRetry = (N.MeshRetry or 0) + (s.meshRetry or 0)
			N.MeshFail = (N.MeshFail or 0) + (s.meshFail or 0)
			N.MeshFailTris = (N.MeshFailTris or 0) + (s.meshFailTris or 0)
			if (s.meshFail or 0) > 0 and not N.MeshFailLogged then
				N.MeshFailLogged = true
				ErrorNoHalt(string.format(
					"[GTAV] mesh builder refused %d triangles - the world has holes there\n",
					s.meshFailTris or 0))
			end

			-- cumulative in the module, read the delta
			local bad = s.meshBadIdx or 0
			local seen = N.MeshBadIdxSeen or 0
			if bad > seen then
				N.MeshBadIdx = (N.MeshBadIdx or 0) + (bad - seen)
				N.MeshBadIdxSeen = bad
				if not N.MeshBadIdxLogged then
					N.MeshBadIdxLogged = true
					ErrorNoHalt(string.format(
						"[GTAV] %d mesh corners had out-of-range indices - those triangles are degenerate, the payload is corrupt\n",
						bad))
				end
			elseif bad < seen then
				N.MeshBadIdxSeen = bad
			end
		end
	end

end

function N.WrapSubs(subs, lazy)
	local getMat = GTAV.Client and GTAV.Client.GetMaterial
	local out = {}
	for i = 1, #subs do
		local e = subs[i]
		local mat = nil
		if getMat and not lazy then
			local okM, m = pcall(getMat, e.material)
			if okM then mat = m end
		end
		out[#out + 1] = {
			mesh = e.mesh,
			mat = mat,
			material = e.material,
			glass = e.glass and true or false,
			tris = e.tris,
			mins = e.mins,
			maxs = e.maxs,
		}
	end
	return out
end

function N.WrapSubsBudgeted(subs, state, budgetMs)
	local getMat = GTAV.Client and GTAV.Client.GetMaterial
	if not getMat then
		state.cur = #subs + 1
		return true
	end
	if not state.cur then state.cur = 1 end
	local t0 = SysTime()
	local made = 0
	while state.cur <= #subs do
		if made > 0 and (SysTime() - t0) * 1000 >= budgetMs then break end
		local e = subs[state.cur]
		if not e.mat then
			local cl = GTAV.Client
			local warm = cl and ((cl.IsMaterialWarm and cl.IsMaterialWarm(e.material))
				or (cl.MaterialCache and cl.MaterialCache[e.material]))
			-- cold load budget, cache hits always go
			if not warm
				and cl and cl.MatLoadBudgetLeft and cl.MatLoadBudgetLeft() <= 0 then
				break
			end
			local okM, m = pcall(getMat, e.material)
			if okM then e.mat = m end
			made = made + 1
		end
		state.cur = state.cur + 1
	end
	return state.cur > #subs
end

function N.BuildGroup(files, originTx, originTy, tileSize)
	if not N.Available then return nil, "no native module" end
	if not N.EnsureMap() then return nil, "no map pages" end
	if not files or #files == 0 then return nil, "empty tile list" end
	if not N.Module.build_group then
		return nil, "module has no build_group (stale DLL)"
	end

	local okB, subs, merged = pcall(N.Module.build_group, files,
		originTx, originTy, tileSize, N.FlagTable())
	if not okB then return nil, tostring(subs) end
	if not subs then return nil, merged end

	N.AfterBuild(subs)
	local out = N.WrapSubs(subs)
	if #out == 0 then return nil, "no drawable material in the group" end
	return out, merged
end

function N.AsyncAvailable()
	return N.Available and N.Module ~= nil and N.Module.build_async_start ~= nil
end

-- preFiles = next group, names only
function N.BuildGroupPrep(files, originTx, originTy, tileSize, preFiles)
	if not N.AsyncAvailable() then return nil, "no async builder (stale DLL)" end
	if not N.EnsureMap() then return nil, "no map pages" end
	if not files or #files == 0 then return nil, "empty tile list" end

	local okS, res, err = pcall(N.Module.build_async_start, files,
		originTx, originTy, tileSize, preFiles)
	if not okS then return nil, tostring(res) end
	if not res then return nil, tostring(err or "start refused") end
	return true
end

-- idle queued working ready failed
function N.BuildGroupPoll(flagBudgetMs)
	if not N.AsyncAvailable() then return "idle" end
	local okP, state, err = pcall(N.Module.build_async_poll, N.FlagTable(),
		flagBudgetMs or 0)
	if not okP then return "failed", tostring(state) end
	return state or "idle", err
end

-- one slice, returns subs, done or nil, why
function N.BuildGroupCommit(budgetMs)
	if not N.AsyncAvailable() then return nil, "no async builder (stale DLL)" end
	local okC, subs, done = pcall(N.Module.build_async_commit, budgetMs or 0)
	if not okC then return nil, tostring(subs) end
	if not subs then return nil, tostring(done or "commit refused") end

	local out = N.WrapSubs(subs, true)
	if done then N.AfterBuild(subs) end
	return out, done and true or false
end

function N.BuildGroupCancel()
	if not N.AsyncAvailable() then return end
	pcall(N.Module.build_async_cancel)
end


function N.MeshInfo()
	if not N.Available or not N.Module.mesh_info then return nil end
	local ok, why, path, pushMs, buildMs, fastFails = pcall(N.Module.mesh_info)
	if not ok then return nil end
	return why, path, pushMs, buildMs, fastFails
end

function N.BuildStats()
	if not N.Available or not N.Module.build_stats then return nil end
	local ok, s = pcall(N.Module.build_stats)
	if not ok or type(s) ~= "table" then return nil end
	return s
end

function N.Status()
	if not N.Available then
		return "native: unavailable (" .. tostring(N.LoadError) .. ")"
	end
	local _why, path = N.MeshInfo()
	return string.format(
		"native: %s | vectors=%s | mesh=%s (%s) | map=%s | pages_mapped=%s",
		tostring(N.Version),
		N.FastVectors and "fast" or "slow",
		tostring(path or "?"),
		(N.Module.builder_mode and N.BuilderFast) and "fast path ON"
			or N.BuilderBlocked and "fast path blocked after a failed chunk"
			or "fast path off - gtav_cl_mesh_builder 1 to enable",
		tostring(N.MapName or "none"),
		tostring(N.Module.pages_mapped and N.Module.pages_mapped() or "?"))
end

function N.StatusDetail()
	local why, _path, pushMs, buildMs = N.MeshInfo()
	if not why then return nil end
	return string.format("native mesh: %s | last build %.2fms (prep %.2fms)",
		why, (buildMs or 0), (pushMs or 0))
end

if N.Available then
	concommand.Add("gtav_native_status", function()
		print("[GTAV] " .. N.Status())
		local detail = N.StatusDetail()
		if detail then print("[GTAV] " .. detail) end
		local bs = N.BuildStats()
		if bs then
			print(string.format(
				"[GTAV] last group: %d tiles  %d draws  %d tris | read %.1f  dec %.1f  merge %.1f  mesh %.1f  = %.1fms",
				bs.tiles or 0, bs.draws or 0, bs.tris or 0,
				bs.readMs or 0, bs.decodeMs or 0, bs.accMs or 0,
				bs.meshMs or 0, bs.totalMs or 0))
		end
	end)
end


local function pickTiles(count)
	local list = {}
	local tiles = GTAV.MapData and GTAV.MapData.tiles
	if not tiles then return list end

	local ply = LocalPlayer()
	local eye = (IsValid(ply) and ply:GetPos()) or vector_origin
	local eyeAbs = eye
	if GTAV.GetAbsolutePos and IsValid(ply) then
		local okA, a = pcall(GTAV.GetAbsolutePos, ply)
		if okA and a then eyeAbs = a end
	end

	local ts = GTAV.Config.VISUAL_TILE_SIZE or 1024
	for key, lods in pairs(tiles) do
		local info = lods["lod0"] or lods["lod1"] or lods["lod2"] or
					 lods["lod3"] or lods["lod4"]
		if info and info.file then
			local tx, ty = string.match(key, "^(%-?%d+)%s+(%-?%d+)$")
			if tx then
				tx, ty = tonumber(tx), tonumber(ty)
				local dx = tx * ts + ts * 0.5 - eyeAbs.x
				local dy = ty * ts + ts * 0.5 - eyeAbs.y
				list[#list + 1] = {
					tx = tx, ty = ty,
					file = info.file,
					lod = lods["lod0"] and 0 or (lods["lod1"] and 1 or
						  (lods["lod2"] and 2 or (lods["lod3"] and 3 or 4))),
					d2 = dx * dx + dy * dy,
				}
			end
		end
		if #list > 4000 then break end
	end

	table.sort(list, function(a, b) return a.d2 < b.d2 end)
	local out = {}
	for i = 1, math.min(count, #list) do out[i] = list[i] end
	return out
end

local function tileSubmeshCount(file, tx, ty)
	local info = N.TileInfo(file, tx, ty)
	return info
end

function N.SmokeTest(countArg)
		local count = math.max(1, math.floor(tonumber(countArg) or 3))
		local ts = GTAV.Config.VISUAL_TILE_SIZE or 1024

		print("[GTAV] ---- native smoke test -------------------------------")
		print("[GTAV] " .. N.Status())
		local detail = N.StatusDetail()
		if detail then print("[GTAV] " .. detail) end

		local picks = pickTiles(count)
		if #picks == 0 then
			print("[GTAV] no tiles in the active index to test against")
			return
		end

		local function fmt(n)
			return string.format("%.2f ms", (n or 0) * 1000)
		end

		for i = 1, #picks do
			local p = picks[i]
			local info = tileSubmeshCount(p.file, p.tx, p.ty)
			print(string.format("[GTAV] tile %s (LOD%d) at %d %d",
				p.file, p.lod, p.tx, p.ty))
			if info then
				print(string.format(
					"[GTAV]   payload %d bytes | %d verts | %d indices | %d submeshes | %d materials | stride %d | clamped %d",
					info.bytes or 0, info.verts or 0, info.indices or 0,
					info.submeshes or 0, info.materials or 0,
					info.stride or 0, info.clamped or 0))
			else
				print("[GTAV]   tile_info returned nil")
			end

			-- native build.
			local t0 = SysTime()
			local nres, nerr = N.BuildTile(p.file, p.tx, p.ty, ts)
			local nTime = SysTime() - t0
			local _why, nPath, nPush, nBuild = N.MeshInfo()

			if nres then
				local nTris = 0
				local nMeshes = #nres
				for k = 1, #nres do nTris = nTris + (nres[k].tris or 0) end
				print(string.format(
					"[GTAV]   native  %s -> %d meshes, %d triangles (mesh=%s, native %.2fms)",
					fmt(nTime), nMeshes, nTris,
					tostring(nPath or "?"), (nBuild or 0)))
			else
				print(string.format("[GTAV]   native  failed: %s",
					tostring(nerr)))
			end

			-- lua build to compare
			if GTAV.Client and GTAV.Client.BuildTileMesh then
				local t1 = SysTime()
				local okL, tile = pcall(GTAV.Client.BuildTileMesh,
					{key = "test " .. i, tx = p.tx, ty = p.ty, lod = p.lod})
				local lTime = SysTime() - t1
				if okL and tile then
					local lTris = tile.triCount or 0
					print(string.format(
						"[GTAV]   lua     %s -> %d meshes, %d verts",
						fmt(lTime), tile.submeshCount or 0, lTris))
					if nres then
						local ratio = (lTime > 0) and (lTime / math.max(nTime, 1e-6)) or 0
						print(string.format(
							"[GTAV]   speedup x%.1f, draw calls %d -> %d",
							ratio, tile.submeshCount or 0, #nres))
					end
					if tile.submeshes then
						for k = 1, #tile.submeshes do
							local m = tile.submeshes[k].mesh
							if m and m.Destroy then pcall(m.Destroy, m) end
						end
					end
				else
					print(string.format("[GTAV]   lua     failed: %s",
						tostring(tile)))
				end
			end

			if nres then
				for k = 1, #nres do
					local m = nres[k].mesh
					if m and m.Destroy then pcall(m.Destroy, m) end
				end
			end
		end

		print("[GTAV] -----------------------------------------------")
end

if N.Available then
	concommand.Add("gtav_native_test", function(ply, cmd, args)
		N.SmokeTest(args and args[1])
	end)
end

hook.Add("InitPostEntity", "GTAV_NativeSmoke", function()
	timer.Simple(3, function()
		if not GTAV.IsActive or not GTAV.IsActive() then return end
		N.SmokeTest(2)
	end)
end)
