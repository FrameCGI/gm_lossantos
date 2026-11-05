if CLIENT then return end

util.AddNetworkString("GTAV_Teleport")
util.AddNetworkString("GTAV_Respawn")

GTAV.Server = GTAV.Server or {}
local S = GTAV.Server

S.Frame = S.Frame or Vector(0, 0, 0)          -- current floating-origin cell
S.ActiveChunks = S.ActiveChunks or {}          -- ["cx cy"] = ground entity
S.TrackedProps = S.TrackedProps or {}
S.ChunkPoolFree = S.ChunkPoolFree or {}
S.ShiftAbs = S.ShiftAbs or {}                  -- entity -> absolute pos, per re-base
S.initialized = S.initialized or false
S.nextChunkTick = 0
S.nextShiftTick = 0

local TICK = 0.1                -- streaming/shift interval
local FOOT_GRACE = 0.15         -- seconds to wait for ground before walking again
local ZERO = Vector(0, 0, 0)

local function dbg()
	local cv = GetConVar("gtav_sv_debug")
	return cv and cv:GetBool()
end

local function log(msg)
	if dbg() then print("[GTAV] " .. msg) end
end


local shiftPhys = {}
local shiftTarget = {}
local shiftVel = {}
local shiftAngVel = {}

local function collectPhysicsObjects(ent)
	local count = ent.GetPhysicsObjectCount and ent:GetPhysicsObjectCount() or 0
	if count > 1 then
		local n = 0
		for i = 0, count - 1 do
			local phys = ent:GetPhysicsObjectNum(i)
			if IsValid(phys) then
				n = n + 1
				shiftPhys[n] = phys
			end
		end
		return n
	end

	local body = ent:GetPhysicsObject()
	if not IsValid(body) then return 0 end
	shiftPhys[1] = body
	return 1
end

-- absolute target so a double move cant happen
local function MoveEntity(ent, absPos)
	local target = absPos - S.Frame
	local remaining = target - ent:GetPos()

	-- snapshot motion first, SetPos can halt it
	local count = collectPhysicsObjects(ent)
	for i = 1, count do
		local phys = shiftPhys[i]
		if IsValid(phys) then
			shiftTarget[i] = phys:GetPos() + remaining
			shiftVel[i] = phys:GetVelocity()
			shiftAngVel[i] = phys.GetAngleVelocity and phys:GetAngleVelocity() or nil
		else
			shiftTarget[i] = nil
			shiftVel[i] = nil
			shiftAngVel[i] = nil
		end
	end

	ent:SetPos(target)

	local moved = 0
	for i = 1, count do
		local phys = shiftPhys[i]
		local physTarget = shiftTarget[i]
		local vel = shiftVel[i]
		local angVel = shiftAngVel[i]
		shiftPhys[i] = nil
		shiftTarget[i] = nil
		shiftVel[i] = nil
		shiftAngVel[i] = nil

		if IsValid(phys) and physTarget then
			local didMove = phys:GetPos():DistToSqr(physTarget) > 0.25
			if didMove then
				phys:SetPos(physTarget)
				moved = moved + 1
			end

			if vel and phys:GetVelocity():DistToSqr(vel) > 1 then
				phys:SetVelocity(vel)
			end
			if angVel and phys.GetAngleVelocity then
				local cur = phys:GetAngleVelocity()
				if not cur or cur:DistToSqr(angVel) > 0.01 then
					phys:SetAngleVelocity(angVel)
				end
			end

			if didMove then phys:Wake() end
		end
	end
	return moved
end

-- simfphys/Glide, the car moves not the player
local function AttachmentRoot(ply)
	local veh = ply.GetVehicle and ply:GetVehicle() or NULL
	if IsValid(veh) then
		local parent = veh:GetParent()
		if IsValid(parent) and parent ~= ply and not parent:IsWorld() then
			return parent, true
		end
		return veh, false
	end

	local parent = ply:GetParent()
	if IsValid(parent) and not parent:IsWorld() and not parent.IsGTAVGround then
		return parent, true
	end

	return nil, false
end

local VEHICLE_DESYNC = 512

local function CheckVehicle(ent)
	local count = ent:GetPhysicsObjectCount()
	if not count or count < 2 then return end
	local body = ent:GetPos()
	for i = 0, count - 1 do
		local phys = ent:GetPhysicsObjectNum(i)
		if IsValid(phys) then
			local d2 = phys:GetPos():DistToSqr(body)
			if d2 > VEHICLE_DESYNC * VEHICLE_DESYNC then
				log(string.format(
					"VEHICLE DESYNC %s #%d object %d is %.0f from the body (abs %.0f %.0f)",
					ent:GetClass(), ent:EntIndex(), i, math.sqrt(d2),
					(body + S.Frame).x, (body + S.Frame).y))
			end
		end
	end
end

function S.SetFrame(newFrame)
	newFrame = Vector(newFrame.x, newFrame.y, newFrame.z)
	local old = S.Frame or ZERO
	local delta = newFrame - old
	if delta.x == 0 and delta.y == 0 and delta.z == 0 then
		S.Frame = newFrame
		return false
	end

	S.Frame = newFrame
	local t0 = SysTime()

	-- snapshot before anything moves, dont touch this
	local absOfOld = S.ShiftAbs
	for k in pairs(absOfOld) do absOfOld[k] = nil end
	for _, ent in ipairs(ents.GetAll()) do
		if IsValid(ent) and not ent:IsWorld() and not ent.IsGTAVGround then
			absOfOld[ent] = ent:GetPos() + old
		end
	end
	local snap = function(ent)
		return absOfOld[ent] or (ent:GetPos() + old)
	end

	local groundMoved = 0
	for _, ent in pairs(S.ActiveChunks) do
		if IsValid(ent) and ent.AbsOrigin then
			-- no SetEntityCell here, CollisionRulesChanged hitches
			ent:SetNW2Vector("WorldCell", newFrame)
			ent:MoveTo(ent.AbsOrigin - newFrame)
			groundMoved = groundMoved + 1
		end
	end
	local t1 = SysTime()

	for _, ply in ipairs(player.GetAll()) do
		if IsValid(ply) then
			GTAV.SetEntityCell(ply, newFrame)
			local root, scripted = AttachmentRoot(ply)
			if scripted and IsValid(root) then
				MoveEntity(root, snap(root))
			else
				ply:SetPos(snap(ply) - newFrame)
			end
		end
	end
	local t2 = SysTime()

	local propsMoved = 0
	local multiMoved = 0
	for _, ent in ipairs(ents.GetAll()) do
		if IsValid(ent) and not ent:IsPlayer() and not ent:IsWorld()
			and not ent.IsGTAVGround and not IsValid(ent:GetParent())
			and (not ent:IsDormant() or ent:IsVehicle()
				or ent.IsSimfphyscar or ent.IsGlideVehicle) then
			local physMoved = MoveEntity(ent, snap(ent))
			local isVehicle = ent:IsVehicle()
				or ent.IsSimfphyscar or ent.IsGlideVehicle
			if isVehicle then CheckVehicle(ent) end
			if physMoved > 1 or isVehicle then
				multiMoved = multiMoved + 1
				log(string.format("shifted %s (%d physics objects, abs %.0f %.0f)",
					ent:GetClass(), physMoved, (ent:GetPos() + newFrame).x,
					(ent:GetPos() + newFrame).y))
			end
			if ent.WorldCellKey then
				GTAV.SetEntityCell(ent, newFrame)
			end
			propsMoved = propsMoved + 1
		end
	end
	local t3 = SysTime()

	log(string.format("frame %s -> %s (%.2fms: ground %d/%.2fms, players %.2fms, props %d multi %d/%.2fms)",
		GTAV.GetCellKey(old), GTAV.GetCellKey(newFrame),
		(t3 - t0) * 1000,
		groundMoved, (t1 - t0) * 1000,
		(t2 - t1) * 1000,
		propsMoved, multiMoved, (t3 - t2) * 1000))
	return true
end

function S.Place(ent, abs)
	ent:SetPos(abs - S.Frame)
	return abs - S.Frame
end

local function absOf(ent)
	return ent:GetPos() + (S.Frame or ZERO)
end


local function canHaveGround(cx, cy)
	return GTAV.GetPhysChunkInfo(cx, cy) ~= nil
end

local SPAWN_BUDGET = 0.004    -- 4ms of ground building per tick
local SPAWN_PER_TICK = 3      -- hard cap, whatever the budget says

-- ground box leads moving players
local LEAD_SECONDS = 1.5
local LEAD_MAX = 4096

function S.SpawnChunk(cx, cy)
	local key = cx .. " " .. cy
	if S.ActiveChunks[key] then return S.ActiveChunks[key] end
	if not canHaveGround(cx, cy) then return nil end

	local mesh = GTAV.Binary.GetPhysMesh(cx, cy)
	if not mesh then return nil end

	local ps = GTAV.Config.PHYS_CHUNK_SIZE
	local absOrigin = Vector(cx * ps, cy * ps, 0)

	local ent = table.remove(S.ChunkPoolFree)
	if not IsValid(ent) then
		ent = ents.Create("gtav_ground")
	end
	if not IsValid(ent) then return nil end

	ent:SetPieceX(cx)
	ent:SetPieceY(cy)
	ent.AbsOrigin = absOrigin
	ent.gtavBuilt = false
	ent:SetNW2Vector("WorldCell", S.Frame)
	ent:SetPos(absOrigin - S.Frame)
	ent:SetAngles(angle_zero)
	ent:Spawn()
	if not ent.gtavBuilt then ent:Build() end
	ent:Activate()

	S.ActiveChunks[key] = ent
	return ent
end

function S.UnloadChunk(key)
	local ent = S.ActiveChunks[key]
	S.ActiveChunks[key] = nil
	if IsValid(ent) then
		ent:Remove()
	end
end

local function underChunkKey(abs)
	local ps = GTAV.Config.PHYS_CHUNK_SIZE
	return math.floor(abs.x / ps) .. " " .. math.floor(abs.y / ps)
end

local function HoldPlayer(ply, ready, now)
	if ply:InVehicle() or ply:GetObserverMode() ~= OBS_MODE_NONE then return end

	if not ready then
		ply.gtavHoldStart = ply.gtavHoldStart or now
		if now - ply.gtavHoldStart < 3 then
			ply.gtavFootReady = nil
			if ply.gtavFootHeld ~= true then
				ply.gtavFootHeld = true
				if ply:GetMoveType() ~= MOVETYPE_NOCLIP then
					ply:SetMoveType(MOVETYPE_NONE)
				end
				ply:SetLocalVelocity(ZERO)
			end
			return
		end
		ready = true
	end
	ply.gtavHoldStart = nil

	ply.gtavFootReady = ply.gtavFootReady or now
	if ply.gtavFootHeld and (now - ply.gtavFootReady) > FOOT_GRACE then
		ply.gtavFootHeld = nil
		if ply:GetMoveType() == MOVETYPE_NONE then
			ply:SetMoveType(MOVETYPE_WALK)
		end
		ply:SetLocalVelocity(ZERO)
	end
end

function S.ThinkChunks()
	if not GTAV.MapData.current then return end
	local now = CurTime()

	local radius = GTAV.Config.GROUND_QUERY_RADIUS
	local ps = GTAV.Config.PHYS_CHUNK_SIZE
	local wanted = {}
	local anyPlayer = false

	for _, ply in ipairs(player.GetAll()) do
		if IsValid(ply) and ply:Alive() then
			anyPlayer = true
			local abs = absOf(ply)
			local vel = ply:GetVelocity()
			local lead = radius + math.min(vel:Length() * LEAD_SECONDS, LEAD_MAX)
			local px = math.floor((abs.x + vel.x * LEAD_SECONDS) / ps)
			local py = math.floor((abs.y + vel.y * LEAD_SECONDS) / ps)

			local x0 = math.floor((abs.x - lead) / ps)
			local x1 = math.floor((abs.x + lead) / ps)
			local y0 = math.floor((abs.y - lead) / ps)
			local y1 = math.floor((abs.y + lead) / ps)

			for cx = x0, x1 do
				for cy = y0, y1 do
					if canHaveGround(cx, cy) then
						local key = cx .. " " .. cy
						local d = math.abs(cx - px) + math.abs(cy - py)
						local w = wanted[key]
						if w then
							if d < w.dist then w.dist = d end
						else
							wanted[key] = {cx = cx, cy = cy, dist = d}
						end
					end
				end
			end

			local ukey = underChunkKey(abs)
			local ready = S.ActiveChunks[ukey] ~= nil
			if not canHaveGround(math.floor(abs.x / ps), math.floor(abs.y / ps)) then
				ready = true -- no data here: nothing to wait for
			end
			HoldPlayer(ply, ready, now)
		end
	end

	if not anyPlayer then return end

	local missing = {}
	for key, w in pairs(wanted) do
		if not S.ActiveChunks[key] then
			missing[#missing + 1] = w
		end
	end
	table.sort(missing, function(a, b) return a.dist < b.dist end)

	local t0 = SysTime()
	local spawned = 0
	for i = 1, #missing do
		if spawned > 0 and
			(SysTime() - t0 >= SPAWN_BUDGET or spawned >= SPAWN_PER_TICK) then
			break
		end
		local w = missing[i]
		S.SpawnChunk(w.cx, w.cy)
		spawned = spawned + 1
	end
	if spawned > 1 or (SysTime() - t0) * 1000 > 20 then
		S.WorstSpawnMs = math.max(S.WorstSpawnMs or 0, (SysTime() - t0) * 1000)
	end

	local unloadTime = GTAV.Config.PHYS_UNLOAD_TIME
	local expired
	for key, ent in pairs(S.ActiveChunks) do
		if not wanted[key] then
			ent.gtavUnusedFor = (ent.gtavUnusedFor or 0) + TICK
			if ent.gtavUnusedFor > unloadTime then
				expired = expired or {}
				expired[#expired + 1] = { key = key, for_ = ent.gtavUnusedFor }
			end
		else
			ent.gtavUnusedFor = 0
		end
	end
	-- 2 per tick, teleports expire everything at once
	if expired then
		table.sort(expired, function(a, b) return a.for_ > b.for_ end)
		for i = 1, math.min(#expired, 2) do
			S.UnloadChunk(expired[i].key)
		end
	end

	-- hard cap
	local maxEnts = GetConVar("gtav_sv_max_phys_ents"):GetInt()
	if table.Count(S.ActiveChunks) > maxEnts then
		local sorted = {}
		for key, ent in pairs(S.ActiveChunks) do
			sorted[#sorted + 1] = {key = key, dist = (wanted[key] and wanted[key].dist) or 9999}
		end
		table.sort(sorted, function(a, b) return a.dist > b.dist end)
		for i = 1, #sorted - maxEnts do
			if not wanted[sorted[i].key] then
				S.UnloadChunk(sorted[i].key)
			end
		end
	end
end


local function Trackable(ent)
	if not IsValid(ent) then return false end
	if ent:IsPlayer() or ent:IsWorld() or ent:IsNPC() or ent:IsVehicle() then return false end
	if ent.IsGTAVGround then return false end
	if ent:GetMoveType() ~= MOVETYPE_VPHYSICS then return false end
	local phys = ent:GetPhysicsObject()
	return IsValid(phys) and phys:IsMotionEnabled()
end

local function HasOccupant(ent)
	local driver = ent.GetDriver and ent:GetDriver()
	if IsValid(driver) then return true end

	local children = ent.GetChildren and ent:GetChildren()
	if children then
		for i = 1, #children do
			local child = children[i]
			if IsValid(child) and child.GetDriver then
				if IsValid(child:GetDriver()) then return true end
			end
		end
	end

	return false
end

function S.TrackProps(now)
	for _, ent in ipairs(ents.GetAll()) do
		if not S.TrackedProps[ent] and Trackable(ent) then
			S.TrackedProps[ent] = true
			GTAV.SetEntityCell(ent, S.Frame)
		end
	end

	for ent in pairs(S.TrackedProps) do
		if not IsValid(ent) then
			S.TrackedProps[ent] = nil
		else
			local abs = absOf(ent)
			local phys = ent:GetPhysicsObject()


			local key = underChunkKey(abs)
			local cx, cy = GTAV.GetPhysChunkCoord(abs)
			if canHaveGround(cx, cy) and not S.ActiveChunks[key] then
				-- never freeze an occupied car
				if HasOccupant(ent) then
					if ent.gtavFrozenForGround then
						ent.gtavFrozenForGround = nil
						if IsValid(phys) then
							phys:EnableMotion(true)
							phys:Wake()
						end
						log("unfroze " .. ent:GetClass() .. " (occupied)")
					end
				elseif IsValid(phys) and phys:IsMotionEnabled()
					and phys:GetVelocity():LengthSqr() < 200 * 200 then
					-- slow props only, this zeroes velocity
					phys:EnableMotion(false)
					ent.gtavFrozenForGround = true
					log("froze " .. ent:GetClass() .. " waiting for ground")
				end
			elseif ent.gtavFrozenForGround then
				ent.gtavFrozenForGround = nil
				if IsValid(phys) then
					phys:EnableMotion(true)
					phys:Wake()
				end
				log("unfroze " .. ent:GetClass())
			end
		end
	end
end


function S.PlacePlayerAtSpawn(ply)
	local spawn = GTAV.MapData.spawn or Vector(0, 0, 500)
	local frame = GTAV.SnapCell(spawn)

	if frame ~= S.Frame then S.SetFrame(frame) end

	GTAV.SetEntityCell(ply, S.Frame)
	S.Place(ply, spawn)

	local ang = GTAV.MapData.spawn_angles
	if ang then
		ply:SetEyeAngles(Angle(ang[1] or 0, ang[2] or 0, ang[3] or 0))
	end

	ply:SetLocalVelocity(ZERO)
	ply.gtavFootHeld = nil
	ply.gtavFootReady = nil
	ply.gtavHoldStart = nil

	if ply:GetMoveType() == MOVETYPE_NONE then
		ply:SetMoveType(MOVETYPE_WALK)
	end
end

function S.ResetPlayer(ply)
	if not IsValid(ply) or not GTAV.MapData.current then return end
	S.PlacePlayerAtSpawn(ply)
	S.ThinkChunks()
	log("placed " .. ply:Nick() .. " at abs " .. tostring(GTAV.MapData.spawn))
end

function S.ShiftFrame(now)
	if now < (S.ShiftBlock or 0) then return end

	local ref
	for _, ply in ipairs(player.GetAll()) do
		if IsValid(ply) and ply:Alive() then
			if ply:GetMoveType() ~= MOVETYPE_NOCLIP then
				ref = ply
				break
			end
			ref = ref or ply
		end
	end
	if not ref then return end

	local abs = absOf(ref)

	-- safety net, there is no floor
	if abs.z < -500000 then
		S.ResetPlayer(ref)
		return
	end

	local keep = GTAV.Config.KEEP_RADIUS
	local l = ref:GetPos()
	if math.abs(l.x) > keep or math.abs(l.y) > keep or math.abs(l.z) > keep then
		S.SetFrame(GTAV.SnapCell(abs))
	end
end

function S.Teleport(ply, abs)
	local frame = GTAV.SnapCell(abs)

	local root, scripted
	local vehTarget
	-- before the re-base, absOf(ply) goes stale after
	if IsValid(ply) then
		root, scripted = AttachmentRoot(ply)
		if IsValid(root) then
			vehTarget = absOf(root) + (abs - absOf(ply))
		end
	end

	if frame ~= S.Frame then S.SetFrame(frame) end

	if IsValid(ply) then
		GTAV.SetEntityCell(ply, S.Frame)
		if not scripted then
			S.Place(ply, abs)
			ply:SetLocalVelocity(ZERO)
		end
		ply.gtavFootHeld = nil
		ply.gtavFootReady = nil
		ply.gtavHoldStart = nil

		if vehTarget then MoveEntity(root, vehTarget) end
	end
	S.ThinkChunks()
end


function S.OnPlayerSpawn(ply)
	timer.Simple(0.1, function()
		if IsValid(ply) and GTAV.MapData.current then
			S.ResetPlayer(ply)
		end
	end)
end

function S.OnEntityRemoved(ent)
	S.TrackedProps[ent] = nil
	if ent.IsGTAVGround then
		for key, e in pairs(S.ActiveChunks) do
			if e == ent then S.ActiveChunks[key] = nil end
		end
	end
end

S.lastStats = S.lastStats or 0

function S.Stats()
	local now = CurTime()
	if now - S.lastStats < 5 then return end
	S.lastStats = now
	local ply = player.GetAll()[1]
	local abs = IsValid(ply) and absOf(ply) or ZERO
	print(string.format("[GTAV] frame=%s chunks=%d playerAbs=%.0f %.0f %.0f mt=%d",
		GTAV.GetCellKey(S.Frame), table.Count(S.ActiveChunks),
		abs.x, abs.y, abs.z,
		IsValid(ply) and ply:GetMoveType() or -1))
end

function S.Think()
	if not GTAV.MapData.current then return end
	local now = CurTime()

	if now >= S.nextShiftTick then
		S.nextShiftTick = now + TICK
		S.ShiftFrame(now)
	end

	if now >= S.nextChunkTick then
		S.nextChunkTick = now + TICK
		S.TrackProps(now)
		S.ThinkChunks()
		if dbg() then S.Stats() end
	end
end

function S.Initialize()
	if S.initialized then return end

	if not GTAV.IsActive() then
		local name = GTAV.DeriveMapName()
		if not GTAV.HasMapData(name) or not GTAV.LoadMapIndex(name) then
			print("[GTAV] server inactive: no map data for " .. game.GetMap())
			return
		end
	end

	S.initialized = true

	print("[GTAV] Server initializing...")

	local spawn = GTAV.MapData.spawn or Vector(0, 0, 200)
	S.Frame = GTAV.SnapCell(spawn)
	print("[GTAV] Frame origin: " .. GTAV.GetCellKey(S.Frame))

	hook.Add("ShouldCollide", "GTAV_CellCollision", GTAV.ShouldCollide)
	hook.Add("PlayerSpawn", "GTAV_PlayerSpawn", S.OnPlayerSpawn)
	hook.Add("EntityRemoved", "GTAV_EntityRemoved", S.OnEntityRemoved)
	hook.Add("Think", "GTAV_Stream", S.Think)

	concommand.Add("gtav_tp", function(ply, cmd, args)
		local x, y, z = tonumber(args[1]), tonumber(args[2]), tonumber(args[3])
		if not x or not y then
			print("[GTAV] usage: gtav_tp <x> <y> [z]  (absolute world coordinates)")
			return
		end
		local targets = IsValid(ply) and {ply} or player.GetAll()
		for _, p in ipairs(targets) do
			local abs = Vector(x, y, z or ((absOf(p).z)))
			S.Teleport(p, abs)
			print(string.format("[GTAV] teleported %s to %s (frame %s)", p:Nick(), tostring(abs),
				GTAV.GetCellKey(S.Frame)))
		end
	end)

	net.Receive("GTAV_Teleport", function(len, ply)
		local x = net.ReadFloat()
		local y = net.ReadFloat()
		local z = net.ReadFloat()
		local hasZ = net.ReadBool()
		if not IsValid(ply) then return end
		local abs = Vector(x, y, hasZ and z or absOf(ply).z)
		S.Teleport(ply, abs)
		print(string.format("[GTAV] %s teleported to %s (frame %s)", ply:Nick(), tostring(abs),
			GTAV.GetCellKey(S.Frame)))
	end)

	net.Receive("GTAV_Respawn", function(len, ply)
		if IsValid(ply) then S.ResetPlayer(ply) end
	end)

	concommand.Add("gtav_frame", function(ply)
		local f = S.Frame
		print(string.format("[GTAV] frame %s (%.0f %.0f %.0f) chunks=%d",
			GTAV.GetCellKey(f), f.x, f.y, f.z, table.Count(S.ActiveChunks)))
		for _, p in ipairs(player.GetAll()) do
			print(string.format("  %s local=%.0f %.0f %.0f abs=%.0f %.0f %.0f mt=%d",
				p:Nick(), p:GetPos().x, p:GetPos().y, p:GetPos().z,
				absOf(p).x, absOf(p).y, absOf(p).z, p:GetMoveType()))
		end
	end)

	concommand.Add("gtav_shift", function(ply, cmd, args)
		if not GTAV.MapData.current then return end
		local axis = string.lower(args[1] or "x")
		local sign = 1
		if axis:sub(1, 1) == "-" then
			sign = -1
			axis = axis:sub(2)
		end
		local dir = Vector(sign, 0, 0)
		if axis == "y" then
			dir = Vector(0, sign, 0)
		elseif axis == "z" then
			dir = Vector(0, 0, sign)
		end

		local abs = {}
		for _, p in ipairs(player.GetAll()) do abs[p] = absOf(p) end

		local t0 = SysTime()
		S.SetFrame(S.Frame + dir * GTAV.Config.CELL_SIZE)
		S.ShiftBlock = CurTime() + 5
		for _, p in ipairs(player.GetAll()) do
			GTAV.SetEntityCell(p, S.Frame)
			local root, scripted = AttachmentRoot(p)
			if scripted and IsValid(root) then
				MoveEntity(root, absOf(root))
			else
				S.Place(p, abs[p])
				p:SetLocalVelocity(ZERO)
			end
			p.gtavFootHeld = nil
			p.gtavFootReady = nil
		end
		S.ThinkChunks()
		print(string.format("[GTAV] gtav_shift %s: frame %s, %d players, %.2fms",
			tostring(dir), GTAV.GetCellKey(S.Frame), table.Count(abs),
			(SysTime() - t0) * 1000))
	end)

	concommand.Add("gtav_freecam", function(ply)
		for _, p in ipairs(player.GetAll()) do
			p:SetMoveType(MOVETYPE_NOCLIP)
			p.gtavFootHeld = nil
		end
	end)

	timer.Simple(0.5, function()
		for _, p in ipairs(player.GetAll()) do S.ResetPlayer(p) end
	end)

	print("[GTAV] Server initialized")
end

hook.Add("InitPostEntity", "GTAV_ServerInit", S.Initialize)

-- ground probe, gtav_sv_debug 1
-- ent=gtav_ground  collision works
-- ent=worldspawn   landed on the bsp, chunks not colliding
-- hit=false        nothing below
local lastProbeTime = 0
hook.Add("Think", "GTAV_GroundProbe", function()
	if not GTAV.MapData.current then return end
	if not dbg() then return end
	if CurTime() - lastProbeTime < 2 then return end
	lastProbeTime = CurTime()

	local ply = player.GetAll()[1]
	if not IsValid(ply) then return end

	local traceLine = GTAV.Trace and GTAV.Trace.RealTraceLine
	if not traceLine then return end

	local abs = absOf(ply)
	local tr = traceLine({
		start = ply:GetPos() + Vector(0, 0, 8),
		endpos = ply:GetPos() - Vector(0, 0, 600),
		filter = ply,
		mask = MASK_SOLID,
	})

	local hitClass = IsValid(tr.Entity) and tr.Entity:GetClass() or "none"
	print(string.format("[GTAV] probe: hit=%s ent=%s absZ=%.1f localZ=%.1f chunks=%d mt=%d",
		tostring(tr.Hit), hitClass, abs.z, ply:GetPos().z,
		table.Count(S.ActiveChunks), ply:GetMoveType()))
end)
