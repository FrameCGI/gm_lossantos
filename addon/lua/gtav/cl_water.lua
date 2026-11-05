GTAV = GTAV or {}
GTAV.Water = GTAV.Water or {}
local W = GTAV.Water

local OCEAN = "models/gtav/water/ocean"

CreateClientConVar("gtav_cl_water_shader", "1", true, false,
	"GTA-style water shader (0 = the flat exported water material)")
CreateClientConVar("gtav_cl_water_spec", "1.0", true, false, "Water sun specular intensity", 0, 8)
CreateClientConVar("gtav_cl_water_body", "0.78", true, false,
	"Water body opacity over the refracted seabed (0 = clear, 1 = solid colour)", 0, 1)

local function lin(c) return (c / 255) ^ 2.2 end

local mat, why
local function build()
	local need = {
		"shaders/fxc/gtav_water_vs30.vcs", "shaders/fxc/gtav_water_ps30.vcs",
		"materials/gtav/water/waterbump2.vtf", "materials/gtav/water/waternoise.vtf",
	}
	for _, f in ipairs(need) do
		if not file.Exists(f, "GAME") then
			why = "missing " .. f
			print("[GTAV] water shader off: " .. why)
			return false
		end
	end
	mat = CreateMaterial("gtav_water_shader", "screenspace_general", {
		["$pixshader"] = "gtav_water_ps30",
		["$vertexshader"] = "gtav_water_vs30",
		["$basetexture"] = "gtav/water/waterbump2",
		["$texture1"] = "gtav/water/waternoise",
		["$texture2"] = "_rt_FullFrameFB",
		["$linearread_basetexture"] = 1,
		["$linearread_texture1"] = 1,
		["$depthtest"] = 1,
		["$writedepth"] = 0,
		["$cull"] = 0,
		["$c0_x"] = 0, ["$c0_y"] = 0, ["$c0_z"] = 0, ["$c0_w"] = 1,
		["$c1_x"] = 1, ["$c1_y"] = 1, ["$c1_z"] = 1, ["$c1_w"] = 1,
		["$c2_x"] = 0, ["$c2_y"] = 0, ["$c2_z"] = 0, ["$c2_w"] = 0.78,
		["$c3_x"] = 0, ["$c3_y"] = 0, ["$c3_z"] = 0, ["$c3_w"] = 0,
	})
	if not mat or mat:IsError() then
		why = "CreateMaterial failed"
		mat = nil
		return false
	end
	print("[GTAV] water shader on (gtav_water_vs30/ps30)")
	return true
end

local built = false
function W.Material()
	local cv = GetConVar("gtav_cl_water_shader")
	if cv and not cv:GetBool() then return nil end
	if not built then built = true build() end
	return mat
end

function W.IsWater(sub)
	local w = sub.isWater
	if w == nil then
		w = sub.mat and sub.mat.GetName and sub.mat:GetName() == OCEAN or false
		sub.isWater = w
	end
	return w
end

local lastFrame = -1
function W.Prepare()
	local fn = FrameNumber()
	if fn == lastFrame then return end
	lastFrame = fn

	local sx, sy, sz = 0.3, 0.2, 0.93
	local N = GTAV.Native
	if N and N.Available and N.Module and N.Module.sun_info then
		local ok, az, el = pcall(N.Module.sun_info)
		if ok and type(az) == "number" and type(el) == "number" then
			sx, sy, sz = math.cos(el) * math.cos(az), math.cos(el) * math.sin(az), math.sin(el)
		end
	end
	local fs, fe = render.GetFogDistances()
	local fr, fg, fb = render.GetFogColor()
	if not fe or fe <= 0 then fs, fe = 0, 1e9 end
	if not fr then
		local c = (GTAV.Config.FOG and GTAV.Config.FOG.color) or { 170, 181, 194 }
		fr, fg, fb = c[1], c[2], c[3]
	end

	-- c0  time, sun x y z
	-- c1  fog r g b, specular
	-- c2  deep body r g b, body opacity
	-- c3  fog start, fog end
	mat:SetFloat("$c0_x", CurTime())
	mat:SetFloat("$c0_y", sx) mat:SetFloat("$c0_z", sy) mat:SetFloat("$c0_w", sz)
	mat:SetFloat("$c1_x", lin(fr)) mat:SetFloat("$c1_y", lin(fg)) mat:SetFloat("$c1_z", lin(fb))
	mat:SetFloat("$c1_w", GetConVar("gtav_cl_water_spec"):GetFloat())
	mat:SetFloat("$c2_x", 0.012) mat:SetFloat("$c2_y", 0.045) mat:SetFloat("$c2_z", 0.065)
	mat:SetFloat("$c2_w", GetConVar("gtav_cl_water_body"):GetFloat())
	mat:SetFloat("$c3_x", fs) mat:SetFloat("$c3_y", fe)

	-- refraction source
	render.UpdateScreenEffectTexture()
end

concommand.Add("gtav_water_status", function()
	print(string.format("[GTAV] water shader: %s%s", mat and "built" or "not built",
		why and (" (" .. why .. ")") or ""))
end)
