if CLIENT then return end

util.AddNetworkString("GTAV_Teleport")
util.AddNetworkString("GTAV_Respawn")

GTAV.Server = GTAV.Server or {}
local S = GTAV.Server

S.DefaultCell = S.DefaultCell or Vector(0, 0, 0)   -- anything never labelled lives here
S.ActiveChunks = S.ActiveChunks or {}          -- ["cx cy|cell"] = ground entity
S.Held = S.Held or {}                          -- player -> what theyre carrying
S.TrackedProps = S.TrackedProps or {}
S.ChunkPoolFree = S.ChunkPoolFree or {}
S.ShiftAbs = S.ShiftAbs or {}                  -- entity -> absolute pos, per migrate
S.initialized = S.initialized or false
S.nextChunkTick = 0
S.nextShiftTick = 0

local TICK = 0.1                -- streaming/shift interval
local FOOT_GRACE = 0.15         -- seconds to wait for ground before walking again
local ZERO = Vector(0, 0, 0)
local MERGE_DIST = 3000         -- this close and you get put in the same cell
local BOUNDS = 15000            -- past this a local pos is about to leave the map

local function cellOf(ent)
	return ent.gtavCell or S.DefaultCell
end

local function cellKeyOf(ent)
	return ent.WorldCellKey or GTAV.GetCellKey(S.DefaultCell)
end

local function absOf(ent)
	return ent:GetPos() + cellOf(ent)
end

local function chunkKey(cx, cy, ckey)
	return cx .. " " .. cy .. "|" .. ckey
end

local function inRange(v, r)
	return math.abs(v.x) <= r and math.abs(v.y) <= r and math.abs(v.z) <= r
end

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
local function MoveEntity(ent, absPos, cell)
	local target = absPos - cell
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
					(body + cellOf(ent)).x, (body + cellOf(ent)).y))
			end
		end
	end
end

local function labelCell(ent, cell)
	if ent.IsGTAVGround then return end
	GTAV.SetEntityCell(ent, cell)
	for _, child in ipairs(ent:GetChildren()) do labelCell(child, cell) end
	if ent:IsPlayer() then
		for _, wep in ipairs(ent:GetWeapons()) do GTAV.SetEntityCell(wep, cell) end
	end
end

-- player count per cell, skip lets a mover not count itself
local function occupants(skip)
	local occ = {}
	for _, ply in ipairs(player.GetAll()) do
		if not (skip and skip[ply]) then
			local key = cellKeyOf(ply)
			occ[key] = (occ[key] or 0) + 1
		end
	end
	return occ
end

-- everything that has to move with this entity
local function gather(ent, set)
	set = set or {}
	local function add(e)
		if not IsValid(e) or set[e] or e:IsWorld() or e.IsGTAVGround then return end
		set[e] = true
		if constraint.HasConstraints(e) then
			for _, other in pairs(constraint.GetAllConstrainedEntities(e)) do add(other) end
		end
		if e.GetDriver then add(e:GetDriver()) end
		for _, child in ipairs(e:GetChildren()) do
			if child:IsPlayer() or child.GetDriver then add(child) end
		end
	end
	add(ent)
	if ent:IsPlayer() then
		add((AttachmentRoot(ent)))
		add(S.Held[ent])
	end
	return set
end

-- nobody stays behind so the whole cell comes along, cheap and nothing rebuilds
function S.MigrateCell(old, new)
	local oldKey, newKey = GTAV.GetCellKey(old), GTAV.GetCellKey(new)
	local wasDefault = oldKey == GTAV.GetCellKey(S.DefaultCell)
	local t0 = SysTime()

	-- snapshot before anything moves, dont touch this
	local absOfOld = S.ShiftAbs
	for k in pairs(absOfOld) do absOfOld[k] = nil end
	for _, ent in ipairs(ents.GetAll()) do
		if IsValid(ent) and not ent:IsWorld() and not ent.IsGTAVGround and cellKeyOf(ent) == oldKey then
			absOfOld[ent] = ent:GetPos() + old
		end
	end

	local keys = {}
	for key, ent in pairs(S.ActiveChunks) do
		if IsValid(ent) and ent.CellKey == oldKey then keys[#keys + 1] = key end
	end
	local groundMoved = 0
	for _, key in ipairs(keys) do
		local ent = S.ActiveChunks[key]
		local nk = chunkKey(ent:GetPieceX(), ent:GetPieceY(), newKey)
		S.ActiveChunks[key] = nil
		if S.ActiveChunks[nk] or not inRange(ent.AbsOrigin - new, BOUNDS) then
			ent:Remove()
		else
			-- no SetEntityCell here, CollisionRulesChanged hitches
			ent.CellKey = newKey
			ent.gtavCell = new
			ent.WorldCellKey = newKey
			ent:SetNW2Vector("WorldCell", new)
			ent:MoveTo(ent.AbsOrigin - new)
			S.ActiveChunks[nk] = ent
			groundMoved = groundMoved + 1
		end
	end
	local t1 = SysTime()

	if wasDefault then S.DefaultCell = Vector(new.x, new.y, new.z) end

	for ent, abs in pairs(absOfOld) do
		if ent:IsPlayer() and IsValid(ent) then
			labelCell(ent, new)
			local root, scripted = AttachmentRoot(ent)
			if scripted and IsValid(root) then
				MoveEntity(root, absOfOld[root] or (root:GetPos() + old), new)
			elseif not ent:InVehicle() then
				ent:SetPos(abs - new)
			end
		end
	end
	local t2 = SysTime()

	local propsMoved = 0
	local multiMoved = 0
	for ent, abs in pairs(absOfOld) do
		if IsValid(ent) and not ent:IsPlayer() and not IsValid(ent:GetParent())
			and (not ent:IsDormant() or ent:IsVehicle()
				or ent.IsSimfphyscar or ent.IsGlideVehicle) then
			if inRange(abs - new, BOUNDS) then
				local physMoved = MoveEntity(ent, abs, new)
				local isVehicle = ent:IsVehicle()
					or ent.IsSimfphyscar or ent.IsGlideVehicle
				if isVehicle then CheckVehicle(ent) end
				if physMoved > 1 or isVehicle then
					multiMoved = multiMoved + 1
					log(string.format("shifted %s (%d physics objects, abs %.0f %.0f)",
						ent:GetClass(), physMoved, abs.x, abs.y))
				end
				if ent.WorldCellKey then labelCell(ent, new) end
				propsMoved = propsMoved + 1
			else
				-- too far to come along, stays where it is
				labelCell(ent, old)
			end
		end
	end
	local t3 = SysTime()

	log(string.format("cell %s -> %s (%.2fms: ground %d/%.2fms, players %.2fms, props %d multi %d/%.2fms)",
		oldKey, newKey, (t3 - t0) * 1000,
		groundMoved, (t1 - t0) * 1000,
		(t2 - t1) * 1000,
		propsMoved, multiMoved, (t3 - t2) * 1000))
end

-- move a set of entities into a cell, abs position stays the same
local function moveSet(set, cell)
	local abs = {}
	for ent in pairs(set) do abs[ent] = absOf(ent) end
	for ent, pos in pairs(abs) do
		if IsValid(ent) then
			labelCell(ent, cell)
			if not IsValid(ent:GetParent()) then
				if ent:IsPlayer() then
					if not ent:InVehicle() then ent:SetPos(pos - cell) end
				else
					MoveEntity(ent, pos, cell)
				end
			end
		end
	end
end

-- per player, this is the whole multiplayer fix
function S.Rebase(ply, cell)
	local oldKey, newKey = cellKeyOf(ply), GTAV.GetCellKey(cell)
	if oldKey == newKey then return false end

	local set = gather(ply)
	local occ = occupants(set)
	if not occ[oldKey] and not occ[newKey] then
		S.MigrateCell(cellOf(ply), cell)
		-- the migrate only walks whats in the old cell
		for ent in pairs(set) do
			if IsValid(ent) and cellKeyOf(ent) ~= newKey then
				moveSet({[ent] = true}, cell)
			end
		end
	else
		moveSet(set, cell)
	end
	return true
end

function S.Place(ent, abs)
	local l = abs - cellOf(ent)
	ent:SetPos(l)
	return l
end


local function canHaveGround(cx, cy)
	return GTAV.GetPhysChunkInfo(cx, cy) ~= nil
end

local SPAWN_BUDGET = 0.004    -- 4ms of ground building per tick
local SPAWN_PER_TICK = 3      -- hard cap, whatever the budget says

-- ground box leads moving players
local LEAD_SECONDS = 1.5
local LEAD_MAX = 4096

function S.SpawnChunk(cx, cy, cell)
	local ckey = GTAV.GetCellKey(cell)
	local key = chunkKey(cx, cy, ckey)
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

	-- no model = client treats it as not solid and prediction falls through
	-- singleplayer never noticed. infmap uses a couch
	ent:SetModel("models/hunter/blocks/cube025x025x025.mdl")
	ent:SetPieceX(cx)
	ent:SetPieceY(cy)
	ent.AbsOrigin = absOrigin
	ent.gtavBuilt = false
	ent.CellKey = ckey
	ent.gtavCell = Vector(cell.x, cell.y, cell.z)
	ent.WorldCellKey = ckey
	ent:SetNW2Vector("WorldCell", cell)
	ent:SetPos(absOrigin - cell)
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

local function underChunkKey(abs, ckey)
	local ps = GTAV.Config.PHYS_CHUNK_SIZE
	return chunkKey(math.floor(abs.x / ps), math.floor(abs.y / ps), ckey)
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
			local cell = cellOf(ply)
			local ckey = cellKeyOf(ply)
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
						local key = chunkKey(cx, cy, ckey)
						local d = math.abs(cx - px) + math.abs(cy - py)
						local w = wanted[key]
						if w then
							if d < w.dist then w.dist = d end
						else
							wanted[key] = {cx = cx, cy = cy, cell = cell, dist = d}
						end
					end
				end
			end

			local ready = S.ActiveChunks[underChunkKey(abs, ckey)] ~= nil
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
		S.SpawnChunk(w.cx, w.cy, w.cell)
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

-- whoever made it decides the cell
local function assignCell(ent)
	if not IsValid(ent) or ent.gtavCell or ent.IsGTAVGround or ent:IsPlayer() or ent:IsWorld() then return end
	local owner = ent:GetOwner()
	if not IsValid(owner) then owner = ent:GetCreator() end
	if not IsValid(owner) then owner = ent:GetParent() end
	if IsValid(owner) and owner.gtavCell then
		labelCell(ent, owner.gtavCell)
	end
end

-- nobody home in its cell, hand it to whoever walked up to it
local function adoptProp(ent, abs, occ)
	if occ[cellKeyOf(ent)] then return end
	local best, bestD = nil, MERGE_DIST * MERGE_DIST
	for _, ply in ipairs(player.GetAll()) do
		if ply:Alive() then
			local d = absOf(ply):DistToSqr(abs)
			if d < bestD and inRange(abs - cellOf(ply), GTAV.Config.KEEP_RADIUS) then
				best, bestD = ply, d
			end
		end
	end
	if best then moveSet(gather(ent), cellOf(best)) end
end

function S.TrackProps(now)
	local occ = occupants()
	local cells = 0
	for _ in pairs(occ) do cells = cells + 1 end
	local multi = cells > 1
	for _, ent in pairs(S.ActiveChunks) do
		if not occ[ent.CellKey] then multi = true end
	end
	for _, ent in ipairs(ents.GetAll()) do
		if not S.TrackedProps[ent] and Trackable(ent) then
			S.TrackedProps[ent] = true
			assignCell(ent)
			GTAV.SetEntityCell(ent, cellOf(ent))
		end
	end

	for ent in pairs(S.TrackedProps) do
		if not IsValid(ent) then
			S.TrackedProps[ent] = nil
		else
			local abs = absOf(ent)
			local phys = ent:GetPhysicsObject()

			adoptProp(ent, abs, occ)
			if not occ[cellKeyOf(ent)] then multi = true end
			if not inRange(ent:GetPos(), GTAV.Config.KEEP_RADIUS + 2048) then
				moveSet(gather(ent), GTAV.SnapCell(abs))
			end

			local key = underChunkKey(abs, cellKeyOf(ent))
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
	GTAV.MultiCell = multi
end


function S.PlacePlayerAtSpawn(ply)
	local spawn = GTAV.MapData.spawn or Vector(0, 0, 500)
	local cell = GTAV.SnapCell(spawn)

	-- one spawn point and no info_player_starts, spread out or you spawn stuck
	local n = #player.GetAll() - 1
	if n > 0 then
		local i = ply:EntIndex() * 2.4
		local r = 56 * math.sqrt(n)
		spawn = spawn + Vector(math.cos(i) * r, math.sin(i) * r, 0)
	end

	S.Rebase(ply, cell)
	labelCell(ply, cell)
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

-- two friends can land in different cells on the same spot, walk through each other, cant shoot
function S.MergePlayers()
	local all = player.GetAll()
	local keep = GTAV.Config.KEEP_RADIUS - 256
	for i = 1, #all do
		local a = all[i]
		for j = i + 1, #all do
			local b = all[j]
			if a:Alive() and b:Alive() and cellKeyOf(a) ~= cellKeyOf(b) then
				local pa, pb = absOf(a), absOf(b)
				if pa:DistToSqr(pb) < MERGE_DIST * MERGE_DIST then
					if inRange(pb - cellOf(a), keep) then
						S.Rebase(b, cellOf(a))
					elseif inRange(pa - cellOf(b), keep) then
						S.Rebase(a, cellOf(b))
					end
				end
			end
		end
	end
end

function S.ShiftPlayers(now)
	local keep = GTAV.Config.KEEP_RADIUS
	for _, ply in ipairs(player.GetAll()) do
		if IsValid(ply) and ply:Alive() and now >= (ply.gtavShiftBlock or 0) then
			local abs = absOf(ply)
			-- safety net, there is no floor
			if abs.z < -500000 then
				S.ResetPlayer(ply)
			elseif not inRange(ply:GetPos(), keep) then
				S.Rebase(ply, GTAV.SnapCell(abs))
			end
		end
	end
	S.MergePlayers()
end

function S.Teleport(ply, abs)
	local cell = GTAV.SnapCell(abs)

	local root, scripted
	local vehTarget
	-- before the re-base, absOf(ply) goes stale after
	if IsValid(ply) then
		root, scripted = AttachmentRoot(ply)
		if IsValid(root) then
			vehTarget = absOf(root) + (abs - absOf(ply))
		end
	end

	if IsValid(ply) then
		S.Rebase(ply, cell)
		labelCell(ply, cell)
		if not scripted then
			S.Place(ply, abs)
			ply:SetLocalVelocity(ZERO)
		end
		ply.gtavFootHeld = nil
		ply.gtavFootReady = nil
		ply.gtavHoldStart = nil

		if vehTarget then MoveEntity(root, vehTarget, cell) end
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
	print(string.format("[GTAV] chunks=%d cells=%d", table.Count(S.ActiveChunks), table.Count(occupants())))
	for _, ply in ipairs(player.GetAll()) do
		local abs = absOf(ply)
		print(string.format("  %s cell=%s abs=%.0f %.0f %.0f mt=%d", ply:Nick(), cellKeyOf(ply),
			abs.x, abs.y, abs.z, ply:GetMoveType()))
	end
end

function S.Think()
	if not GTAV.MapData.current then return end
	local now = CurTime()

	if now >= S.nextShiftTick then
		S.nextShiftTick = now + TICK
		S.ShiftPlayers(now)
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
	S.DefaultCell = GTAV.SnapCell(spawn)
	print("[GTAV] Default cell: " .. GTAV.GetCellKey(S.DefaultCell))

	hook.Add("PlayerSpawn", "GTAV_PlayerSpawn", S.OnPlayerSpawn)
	hook.Add("EntityRemoved", "GTAV_EntityRemoved", S.OnEntityRemoved)
	hook.Add("Think", "GTAV_Stream", S.Think)

	-- things in other cells cant touch you
	local function crossCell(a, b)
		if not IsValid(a) or not IsValid(b) then return false end
		local ka, kb = a.WorldCellKey, b.WorldCellKey
		return ka and kb and ka ~= kb
	end
	local function denyCross(a, b)
		if crossCell(a, b) then return false end
	end
	hook.Add("EntityTakeDamage", "GTAV_CellDamage", function(target, dmg)
		if crossCell(target, dmg:GetInflictor()) or crossCell(target, dmg:GetAttacker()) then return true end
	end)
	hook.Add("PlayerCanPickupWeapon", "GTAV_CellPickup", denyCross)
	hook.Add("PlayerCanPickupItem", "GTAV_CellPickup", denyCross)
	hook.Add("GravGunPickupAllowed", "GTAV_CellPickup", denyCross)
	hook.Add("PhysgunPickup", "GTAV_CellPickup", denyCross)
	hook.Add("PlayerUse", "GTAV_CellPickup", denyCross)

	-- carried stuff crosses cells with you
	local function pickup(ply, ent) S.Held[ply] = ent end
	local function drop(ply) S.Held[ply] = nil end
	hook.Add("OnPhysgunPickup", "GTAV_Held", pickup)
	hook.Add("PhysgunDrop", "GTAV_Held", drop)
	hook.Add("GravGunOnPickedUp", "GTAV_Held", pickup)
	hook.Add("GravGunOnDropped", "GTAV_Held", drop)
	hook.Add("OnPlayerPhysicsPickup", "GTAV_Held", pickup)
	hook.Add("OnPlayerPhysicsDrop", "GTAV_Held", drop)

	hook.Add("OnEntityCreated", "GTAV_AssignCell", function(ent)
		timer.Simple(0, function() assignCell(ent) end)
	end)
	local function spawned(ply, a, b)
		local ent = isentity(a) and a or b
		if IsValid(ent) and ply.gtavCell then labelCell(ent, ply.gtavCell) end
	end
	hook.Add("PlayerSpawnedProp", "GTAV_AssignCell", spawned)
	hook.Add("PlayerSpawnedEffect", "GTAV_AssignCell", spawned)
	hook.Add("PlayerSpawnedRagdoll", "GTAV_AssignCell", spawned)
	hook.Add("PlayerSpawnedNPC", "GTAV_AssignCell", spawned)
	hook.Add("PlayerSpawnedSENT", "GTAV_AssignCell", spawned)
	hook.Add("PlayerSpawnedVehicle", "GTAV_AssignCell", spawned)
	hook.Add("PlayerSpawnedSWEP", "GTAV_AssignCell", spawned)

	concommand.Add("gtav_tp", function(ply, cmd, args)
		if IsValid(ply) and not ply:IsAdmin() then return end
		local x, y, z = tonumber(args[1]), tonumber(args[2]), tonumber(args[3])
		if not x or not y then
			print("[GTAV] usage: gtav_tp <x> <y> [z]  (absolute world coordinates)")
			return
		end
		local targets = IsValid(ply) and {ply} or player.GetAll()
		for _, p in ipairs(targets) do
			local abs = Vector(x, y, z or ((absOf(p).z)))
			S.Teleport(p, abs)
			print(string.format("[GTAV] teleported %s to %s (cell %s)", p:Nick(), tostring(abs),
				cellKeyOf(p)))
		end
	end)

	net.Receive("GTAV_Teleport", function(len, ply)
		local x = net.ReadFloat()
		local y = net.ReadFloat()
		local z = net.ReadFloat()
		local hasZ = net.ReadBool()
		if not IsValid(ply) or not ply:IsAdmin() then return end
		local abs = Vector(x, y, hasZ and z or absOf(ply).z)
		S.Teleport(ply, abs)
		print(string.format("[GTAV] %s teleported to %s (cell %s)", ply:Nick(), tostring(abs),
			cellKeyOf(ply)))
	end)

	net.Receive("GTAV_Respawn", function(len, ply)
		if not IsValid(ply) or CurTime() < (ply.gtavNextRespawn or 0) then return end
		ply.gtavNextRespawn = CurTime() + 2
		S.ResetPlayer(ply)
	end)

	concommand.Add("gtav_frame", function(ply)
		print(string.format("[GTAV] default cell %s chunks=%d", GTAV.GetCellKey(S.DefaultCell),
			table.Count(S.ActiveChunks)))
		for _, p in ipairs(player.GetAll()) do
			print(string.format("  %s cell=%s local=%.0f %.0f %.0f abs=%.0f %.0f %.0f mt=%d",
				p:Nick(), cellKeyOf(p), p:GetPos().x, p:GetPos().y, p:GetPos().z,
				absOf(p).x, absOf(p).y, absOf(p).z, p:GetMoveType()))
		end
	end)

	concommand.Add("gtav_shift", function(ply, cmd, args)
		if not GTAV.MapData.current then return end
		if not IsValid(ply) or not ply:IsAdmin() then return end
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

		local abs = absOf(ply)
		local t0 = SysTime()
		S.Rebase(ply, cellOf(ply) + dir * GTAV.Config.CELL_SIZE)
		ply.gtavShiftBlock = CurTime() + 5
		local root, scripted = AttachmentRoot(ply)
		if not (scripted and IsValid(root)) then
			S.Place(ply, abs)
			ply:SetLocalVelocity(ZERO)
		end
		ply.gtavFootHeld = nil
		ply.gtavFootReady = nil
		S.ThinkChunks()
		print(string.format("[GTAV] gtav_shift %s: cell %s, %.2fms", tostring(dir), cellKeyOf(ply),
			(SysTime() - t0) * 1000))
	end)

	concommand.Add("gtav_freecam", function(ply)
		if not IsValid(ply) or not ply:IsAdmin() then return end
		ply:SetMoveType(MOVETYPE_NOCLIP)
		ply.gtavFootHeld = nil
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

	local traceLine = GTAV.Trace and GTAV.Trace.RealTraceLine
	if not traceLine then return end

	for _, ply in ipairs(player.GetAll()) do
		local abs = absOf(ply)
		local tr = traceLine({
			start = ply:GetPos() + Vector(0, 0, 8),
			endpos = ply:GetPos() - Vector(0, 0, 600),
			filter = ply,
			mask = MASK_SOLID,
		})

		local hitClass = IsValid(tr.Entity) and tr.Entity:GetClass() or "none"
		print(string.format("[GTAV] probe %s: hit=%s ent=%s absZ=%.1f localZ=%.1f chunks=%d mt=%d",
			ply:Nick(), tostring(tr.Hit), hitClass, abs.z, ply:GetPos().z,
			table.Count(S.ActiveChunks), ply:GetMoveType()))
	end
end)
