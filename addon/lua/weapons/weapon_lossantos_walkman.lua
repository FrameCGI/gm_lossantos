SWEP.PrintName = "Walkman"
SWEP.Author = "Frame"
SWEP.Category = "GTAV"
SWEP.Spawnable = true
SWEP.AdminOnly = false

SWEP.ViewModel = "models/weapons/c_arms.mdl"
SWEP.UseHands = true
SWEP.WorldModel = ""
SWEP.ViewModelFOV = 54

SWEP.Slot = 5
SWEP.SlotPos = 3
SWEP.DrawAmmo = false
SWEP.DrawCrosshair = false

SWEP.Primary.ClipSize = -1
SWEP.Primary.DefaultClip = -1
SWEP.Primary.Automatic = false
SWEP.Primary.Ammo = "none"
SWEP.Secondary.ClipSize = -1
SWEP.Secondary.DefaultClip = -1
SWEP.Secondary.Automatic = false
SWEP.Secondary.Ammo = "none"

function SWEP:Initialize()
	self:SetHoldType("normal")
end

-- input lives in lossantos_radio.lua
function SWEP:PrimaryAttack() self:SetNextPrimaryFire(CurTime() + 0.2) end
function SWEP:SecondaryAttack() self:SetNextSecondaryFire(CurTime() + 0.2) end
function SWEP:Reload() end

if CLIENT then
	function SWEP:DrawWorldModel() end
end
