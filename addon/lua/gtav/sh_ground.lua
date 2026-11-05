local ENT = {}

ENT.Type = "anim"
ENT.Base = "base_anim"
ENT.PrintName = "GTAV Ground Chunk"
ENT.Author = "gm_lossantos"
ENT.Spawnable = false
ENT.AdminSpawnable = false
ENT.AdminOnly = true
ENT.DisableDuplicator = true
ENT.DoNotDuplicate = true
ENT.PhysgunDisabled = true
ENT.m_tblToolsAllowed = {}

ENT.IsGTAVGround = true

local ZERO = Vector(0, 0, 0)

function ENT:SetupDataTables()
	self:NetworkVar("Int", 0, "PieceX")
	self:NetworkVar("Int", 1, "PieceY")
end

function ENT:CanTool() return false end
function ENT:CanProperty() return false end
function ENT:GravGunPickupAllowed() return false end
function ENT:GravGunPunt() return false end

function ENT:ChunkAbsOrigin()
	local ps = GTAV.Config.PHYS_CHUNK_SIZE
	return Vector(self:GetPieceX() * ps, self:GetPieceY() * ps, 0)
end

-- from netvars, GetPos lags on the client
function ENT:TargetPos()
	return self:ChunkAbsOrigin() - self:GetNW2Vector("WorldCell", ZERO)
end

function ENT:Build()
	if self.gtavBuilt then return true end
	if not GTAV.MapData.current then return false end

	local cx, cy = self:GetPieceX(), self:GetPieceY()

	-- netvars not in yet, retry for a while
	if self:GetPos():DistToSqr(self:TargetPos()) > 64 then
		self.gtavBuildTries = (self.gtavBuildTries or 0) + 1
		if self.gtavBuildTries < 120 then return false end
	end

	local mesh = GTAV.Binary.GetPhysMesh(cx, cy)
	if not mesh then return false end

	self.gtavBuilt = true

	self:SetCustomCollisionCheck(true)
	self:PhysicsFromMesh(mesh)
	self:SetSolid(SOLID_VPHYSICS)
	self:SetMoveType(MOVETYPE_NONE)
	self:EnableCustomCollisions(true)
	self:DrawShadow(false)

	local phys = self:GetPhysicsObject()
	if IsValid(phys) then
		phys:EnableMotion(false)
		phys:EnableGravity(false)
		local target = self:TargetPos()
		phys:SetPos(target)
		phys:SetAngles(angle_zero)
		self.gtavPhysPos = target
	else
		ErrorNoHalt(string.format("[GTAV] ground %d %d has NO physics object after PhysicsFromMesh\n", cx, cy))
	end

	self:CollisionRulesChanged()

	if SERVER then
		local cv = GetConVar("gtav_sv_debug")
		if cv and cv:GetBool() then
			print(string.format("[GTAV] Ground %d %d built (server): %d verts (%d tris) abs %s",
				cx, cy, #mesh, math.floor(#mesh / 3), tostring(self:ChunkAbsOrigin())))
		end
	end

	return true
end

function ENT:Initialize()
	if SERVER then
		self:SetNoDraw(true)
	end
end

function ENT:Spawn()
	if SERVER then
		self:SetNoDraw(true)
		if not self:Build() then
			timer.Simple(0, function()
				if IsValid(self) then self:Build() end
			end)
		end
	end
end

function ENT:UpdateTransmitState()
	return TRANSMIT_ALWAYS
end

GTAV.GroundBuildQueue = GTAV.GroundBuildQueue or {}

function ENT:Think()
	if CLIENT then
		if not self.gtavBuilt and not self.gtavBuildQueued then
			self.gtavBuildQueued = true
			local q = GTAV.GroundBuildQueue
			q[#q + 1] = self
		end
		self:SetNextClientThink(CurTime() + 0.25)
		return true
	end
	return false
end

function ENT:Draw() end

function ENT:MoveTo(pos)
	self:SetPos(pos)
	local phys = self:GetPhysicsObject()
	if IsValid(phys) then
		-- every extra SetPos is a collision tree update
		if phys:GetPos():DistToSqr(pos) > 0.25 then
			phys:SetPos(pos)
		end
	end
	self.gtavPhysPos = pos
end


local CLASS = "gtav_ground"

local function BindClass()
	if not baseclass or not baseclass.Set then return end
	local stored = scripted_ents.GetStored(CLASS)
	if not stored then return end

	local tab = stored.t
	if stored.Base and scripted_ents.GetStored(stored.Base) then
		tab = scripted_ents.Get(CLASS) -- base is resolvable: bind the merged class
	end
	baseclass.Set(CLASS, tab)
end

local function RegisterGround()
	if not scripted_ents then return false end
	if not scripted_ents.GetStored(CLASS) then
		scripted_ents.Register(ENT, CLASS)
	end
	BindClass()
	return scripted_ents.GetStored("base_anim") ~= nil
end

-- base_anim registers late, this sucks but it works
if not RegisterGround() then
	local tries = 0
	local function Retry()
		tries = tries + 1
		if RegisterGround() or tries >= 40 then
			hook.Remove("Initialize", "GTAVGroundRegister")
			hook.Remove("InitPostEntity", "GTAVGroundRegister")
			if timer.Remove then timer.Remove("GTAVGroundRegister") end
		end
	end
	hook.Add("Initialize", "GTAVGroundRegister", Retry)
	hook.Add("InitPostEntity", "GTAVGroundRegister", Retry)
	timer.Create("GTAVGroundRegister", 0.25, 0, Retry)
end

if CLIENT then
	local buildQ = GTAV.GroundBuildQueue
	local buildHead = 1

	hook.Add("Think", "GTAVGroundBuildPump", function()
		while buildHead <= #buildQ do
			local ent = buildQ[buildHead]
			buildQ[buildHead] = false
			buildHead = buildHead + 1

			if IsValid(ent) and not ent.gtavBuilt then
				ent.gtavBuildQueued = nil
				if ent:Build() then
					ent:SetRenderBounds(Vector(-1, -1, -1), Vector(1, 1, 1))
				end
				return -- exactly one chunk per frame, cheap or not
			end
		end
		buildQ = {}
		GTAV.GroundBuildQueue = buildQ
		buildHead = 1
	end)
end
