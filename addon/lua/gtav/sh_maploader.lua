local function TryActivate(mapName, why)
	if not GTAV.HasMapData(mapName) then return false end
	if not GTAV.LoadMapIndex(mapName) then return false end
	print("[GTAV] active map: " .. mapName .. " (" .. why .. ")")
	return true
end

local candidate = GTAV.DeriveMapName()
if not TryActivate(candidate, "bsp name") then
	print(string.format(
		"[GTAV] inactive: no lossantos/%s/index.json for map %s - standalone",
		candidate, game.GetMap()))
end

if SERVER then
	-- changelevel can keep this Lua state
	hook.Add("InitPostEntity", "GTAV_MapGuard", function()
		local name = GTAV.DeriveMapName()

		if not GTAV.HasMapData(name) then
			if GTAV.IsActive() then
				print("[GTAV] deactivating: " .. name .. " has no gtav data")
				GTAV.Deactivate()
			end
			return
		end

		if not GTAV.IsActive() or GTAV.MapData.name ~= name then
			TryActivate(name, "map guard")
			if GTAV.IsActive() and GTAV.Server
				and GTAV.Server.Initialize then
				GTAV.Server.Initialize()
			end
		end
	end)
end

if SERVER then
	local function BroadcastMap()
		local name = GTAV.IsActive() and GTAV.MapData.name or ""
		for _, ply in ipairs(player.GetAll()) do
			net.Start("GTAV_MapData")
			net.WriteString(name)
			net.Send(ply)
		end
	end

	hook.Add("InitPostEntity", "GTAV_MapInit", function()
		timer.Simple(0.5, function()
			local gtavMapName = GTAV.DeriveMapName()
			-- info_target can name another data set
			for _, ent in ipairs(ents.FindByClass("info_target")) do
				if ent:GetName() == "gtav_info" then
					local gtavMap = ent:GetKeyValues()["gtav_map"]
					if gtavMap then gtavMapName = gtavMap end
				end
			end

			if GTAV.IsActive() and GTAV.MapData.name == gtavMapName then
				return
			end

			if not GTAV.HasMapData(gtavMapName) then
				print("[GTAV] No map index found for: " .. gtavMapName .. " - running in standalone mode")
				return
			end

			TryActivate(gtavMapName, "map init")
			if not GTAV.IsActive() then return end

			print("[GTAV] Map initialized: " .. gtavMapName)
			if GTAV.Server and GTAV.Server.Initialize then
				GTAV.Server.Initialize()
			end
			for _, ply in ipairs(player.GetAll()) do
				if GTAV.Server and GTAV.Server.ResetPlayer then
					GTAV.Server.ResetPlayer(ply)
				end
			end

			BroadcastMap()
		end)
	end)

	concommand.Add("gtav_loadmap", function(ply, cmd, args)
		if IsValid(ply) and not ply:IsAdmin() then return end
		local mapName = args[1] or GTAV.DeriveMapName()
		if not TryActivate(mapName, "gtav_loadmap") then
			print("[GTAV] Failed to load map: " .. mapName)
			return
		end
		print("[GTAV] Loaded map: " .. mapName)

		local S = GTAV.Server
		if not S then return end

		for key in pairs(S.ActiveChunks or {}) do
			S.UnloadChunk(key)
		end
		S.ActiveChunks = {}
		S.TrackedProps = {}
		S.ChunkPoolFree = {}
		S.initialized = false

		if S.Initialize then S.Initialize() end

		for _, p in ipairs(player.GetAll()) do
			if S.ResetPlayer then S.ResetPlayer(p) end
		end

		BroadcastMap()
	end)
end

if CLIENT then
	hook.Add("InitPostEntity", "GTAV_ClientMapInit", function()
		timer.Simple(1, function()
			net.Start("GTAV_RequestMap")
			net.SendToServer()
		end)
	end)

	net.Receive("GTAV_MapData", function()
		local mapName = net.ReadString()
		if mapName == "" then
			if GTAV.IsActive() then GTAV.Deactivate() end
			return
		end
		if GTAV.IsActive() and GTAV.MapData.name == mapName then return end

		if GTAV.IsActive() then GTAV.Deactivate() end

		if GTAV.LoadMapIndex(mapName) then
			print("[GTAV] Client loaded map: " .. mapName)
			GTAV.Client.TileList = nil -- force tile list rebuild
			if GTAV.Client.Initialize then GTAV.Client.Initialize() end
		end
	end)
end

-- network strings
if SERVER then
	util.AddNetworkString("GTAV_RequestMap")
	util.AddNetworkString("GTAV_MapData")

	net.Receive("GTAV_RequestMap", function(len, ply)
		-- empty = not a gtav map
		local mapName = GTAV.IsActive() and GTAV.MapData.name or ""
		net.Start("GTAV_MapData")
		net.WriteString(mapName)
		net.Send(ply)
	end)
end
