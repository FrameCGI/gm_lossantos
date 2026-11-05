if SERVER then return end

GTAV.BuildTag = "gtav-2026-11-02.13-tclight"
print("[GTAV] client streaming build " .. GTAV.BuildTag)

GTAV.Client = GTAV.Client or {}
local C = GTAV.Client

C.ActiveTiles = C.ActiveTiles or {}   -- ["tx ty"] = tile
C.BuildQueue = C.BuildQueue or {}     -- ordered list of {key, tx, ty, lod, dist}
C.BuildHead = C.BuildHead or 1       -- index of the next queue entry to build

function C.QueueLen()
	return math.max(0, #C.BuildQueue - (C.BuildHead or 1) + 1)
end
C.MaterialCache = C.MaterialCache or {}
C.DrawStats = C.DrawStats or {}
C.MaterialFlags = C.MaterialFlags or {}
C.WrapTasks = C.WrapTasks or {}
C.WrapSet = C.WrapSet or {}
C.InstallPending = C.InstallPending or {}
C.InstallSet = C.InstallSet or {}
C.LastEyePos = nil
C.LastEyeCell = Vector(0, 0, 0)
C.LastPlayerCellKey = nil
C.LastGroundCount = 0
C.NextGroundCount = 0

C.Perf = C.Perf or {
	frameMs = 0,
	groundMs = 0,
	groundMoves = 0,
	builtThisFrame = 0,
	lastCellKey = nil,
	watchUntil = 0,
	holdFrames = 0,
	lastSpike = 0,
}

C.Stats = {
	tilesLoaded = 0,
	tilesDrawn = 0,
	renderCalls = 0,
	renderSkip = nil,
	buildTime = 0,
	renderTime = 0,
	builds = 0,
}

local EVICT_AFTER = 3.0 -- seconds a tile may stay loaded while unwanted

local function tileNearDistSqr(eye, tx, ty, ts, fp, z0, z1)
	if not fp then fp = ts end
	local x0, y0 = tx * ts, ty * ts
	local x1, y1 = x0 + fp, y0 + fp
	local dx = 0
	if eye.x < x0 then
		dx = x0 - eye.x
	elseif eye.x > x1 then
		dx = eye.x - x1
	end
	local dy = 0
	if eye.y < y0 then
		dy = y0 - eye.y
	elseif eye.y > y1 then
		dy = eye.y - y1
	end
	local dz = 0
	if z0 then
		if eye.z < z0 then
			dz = z0 - eye.z
		elseif z1 and eye.z > z1 then
			dz = eye.z - z1
		end
	end
	return dx * dx + dy * dy + dz * dz
end

local function footprint(tile, tileSize)
	local fp = tile._fp
	if fp then return fp end
	fp = tileSize * (tile.span or 1)
	tile._fp = fp
	return fp
end

local function ensureTileList()
	if GTAV.Merge and GTAV.Merge.EnsureList then
		GTAV.Merge.EnsureList()
		return
	end
	C.TileCells = C.TileCells or {}
end

CreateClientConVar("gtav_cl_tile_budget", "0", false, false,
	"Max tiles held within the stream radius (0 = map default; the merge grid decides how many draw sets that is)")
CreateClientConVar("gtav_cl_async_build", "1", false, false,
	"Build groups on the native worker thread (0 = old synchronous build; A/B and kill switch)")
CreateClientConVar("gtav_cl_flags_budget_ms", "12", false, false,
	"Per-frame budget for the async build's material-flags handshake (0 = answer everything in one frame)")
CreateClientConVar("gtav_cl_commit_budget_ms", "16", false, false,
	"Per-frame budget for a group's mesh commit (0 = emit the whole group in one frame)")
CreateClientConVar("gtav_cl_wrap_budget_ms", "8", false, false,
	"Per-frame budget for attaching IMaterials to a committed group (0 = attach all at once)")
CreateClientConVar("gtav_cl_warm_budget_ms", "8", false, false,
	"Per-frame budget for the urgent material warm queue (tripled while the streamer is otherwise idle)")
CreateClientConVar("gtav_cl_installs_per_frame", "3", false, false,
	"Max finished tiles swapped in per frame (0 = unlimited; spreads first-draw texture uploads)")
CreateClientConVar("gtav_cl_mat_loads_per_frame", "4", false, false,
	"Max cold engine Material() loads per frame (GTA V MAX_LOAD_PER_FRAME=4; lower = smoother flight, tiles warm a little later). 0 = unlimited")
CreateClientConVar("gtav_cl_mat_prefetch", "1", false, false,
	"Warm queued tiles' materials ahead of time from tile_materials.dat (0 = only the build handshake warms; kill switch)")
CreateClientConVar("gtav_cl_mat_prefetch_budget_ms", "6", false, false,
	"Max speculative texture prefetch per frame, and only from what the warm queue left over (0 = off)")
CreateClientConVar("gtav_cl_mat_prefetch_groups", "4", false, false,
	"How many builds ahead of the build queue to warm materials for (pipeline depth; 0 = off)")
CreateClientConVar("gtav_cl_frustum_cull", "1", false, false,
	"Cull tiles outside the camera frustum (0 = old 230-degree cone test; kill switch)")
CreateClientConVar("gtav_cl_fog_cull", "1", false, false,
	"Skip tiles whose every pixel is past the fog end (0 = draw fully fogged tiles again; kill switch)")
CreateClientConVar("gtav_cl_far_cull", "1", false, false,
	"Skip tiles entirely past the engine far plane (r_farz), which the GPU clips away every frame anyway (0 = submit them again; kill switch)")
CreateClientConVar("gtav_cl_glass_enable", "1", false, false,
	"Draw the glass pass at all (0 = skip it; A/B probe for pricing the ocean)")
CreateClientConVar("gtav_cl_additive_enable", "1", false, false,
	"Draw the additive pass ($additive neon/glow submeshes) at all (0 = skip; A/B probe)")
CreateClientConVar("gtav_cl_gc_budget_ms", "1.5", false, false,
	"Per-frame budget for incremental garbage collection (0 = engine default GC)")
CreateClientConVar("gtav_cl_draw_enable", "1", false, false,
	"Submit the world's draws (0 = measurement probe: skip SetMaterial+Mesh:Draw, change nothing else)")
CreateClientConVar("gtav_cl_force_depthwrite", "1", false, false,
	"Force depth writes on for the opaque world pass (0 = material state decides; $vertexalpha materials then write no depth)")
CreateClientConVar("gtav_fog", "1", true, false, "Distance haze (0 = off)")
CreateClientConVar("gtav_fog_start", "0", true, false, "Fog start (0 = map default)")
CreateClientConVar("gtav_fog_end", "0", true, false, "Fog end (0 = map default)")
CreateClientConVar("gtav_stream_radius", "0", false, false, "Visual stream radius (0 = map default)")
CreateClientConVar("gtav_skybox_world", "0", true, false, "Draw the world in the skybox pass (1 = old behaviour)")
CreateClientConVar("gtav_cl_lod_bias", "0", false, false, "LOD bias (-1 to 1)")
CreateClientConVar("gtav_cl_baked_lighting", "1", false, false,
	"Baked sun lighting: per-vertex sun shading + shadows from sun_shadow.gvsd (0 = plain unshaded world; A/B switch)")
CreateClientConVar("gtav_cl_sun_ambient", "2.0", false, false,
	"Baked lighting: scale on the timecycle sky/bounce/directional ambient (1 = timecycle; 2 = default shade depth)")
CreateClientConVar("gtav_cl_sun_intensity", "1.0", false, false,
	"Baked lighting: scale on the timecycle sun (1 = timecycle)")
CreateClientConVar("gtav_cl_light_exposure", "2.5", false, false,
	"Baked lighting: filmic input a fully sunlit up-facing surface maps to (higher = flatter, brighter shade)")
CreateClientConVar("gtav_cl_skydome", "1", false, false,
	"Generated skydome + sun disc from the baked sun direction (0 = engine skybox)")
CreateClientConVar("gtav_cl_post", "1", false, false,
	"Post chain: sun shafts, bloom, vignette and the colour grade (0 = raw framebuffer)")
CreateClientConVar("gtav_cl_post_bloom", "1", false, false,
	"Bloom pass (composite intensity 0.72 = the game's bloom term)")
CreateClientConVar("gtav_cl_post_rays", "1", false, false,
	"Sun shafts from the baked sun position")
CreateClientConVar("gtav_cl_post_grade", "0", false, false,
	"Colour grade (DrawColorModify). Off: the timecycle's midday grade is neutral (desaturation 1, colour correct 1); the filmic curve is baked into the vertex lighting")
CreateClientConVar("gtav_cl_post_vignette", "0", false, false,
	"Screen vignette. Off: the timecycle's midday vignetting intensity is 0")
CreateClientConVar("gtav_cl_post_bloom_darken", "0.75", false, false,
	"Bloom bright-pass threshold in LDR (timecycle bright pass 1.8 HDR -> only near-white pixels)")
CreateClientConVar("gtav_cl_post_bloom_mult", "0.25", false, false,
	"Bloom composite strength (timecycle postfx_intensity_bloom 0.1 at 13:00: subtle)")
CreateClientConVar("gtav_cl_post_rays_mult", "0.1", false, false,
	"Sun shaft strength (timecycle light_ray_mult 0.5 at 13:00 vs 8 at 10:00: faint at midday)")

local SKY_R = 12000   -- 60000 got clipped, stay under r_farz
local SKY_SEGS, SKY_RINGS = 48, 24
local SKY_DISC_R = math.tan(math.rad(3)) * SKY_R   -- ~3 degree sun disc

local skyDomeActive = false
local skySunAz, skySunEl = nil, nil
local skyDomeMesh, skySunMesh = nil, nil
local skyBuiltAz, skyBuiltEl = nil, nil
local skyMtx = Matrix()
local skyDomeMat, skySunMat = nil, nil
local skyDomeDead = nil
local skyBuildPending = false  -- deferred build scheduled, not yet run

local function refreshSkyDome()
	local want = false
	local cv = GetConVar("gtav_cl_skydome")
	if (cv == nil or cv:GetBool()) and GTAV.IsActive() then
		local N = GTAV.Native
		if N and N.Available and N.Module and N.Module.sun_info then
			local ok, az, el = pcall(N.Module.sun_info)
			if ok and type(az) == "number" and type(el) == "number" and el > 0.001 then
				want = true
				skySunAz, skySunEl = az, el
			end
		end
	end
	skyDomeActive = want
end

local function buildSkyMeshes()
	local az, el = skySunAz or 0, skySunEl or 0
	local sd = Vector(math.cos(el) * math.cos(az),
					  math.cos(el) * math.sin(az),
					  math.sin(el))

	local fog = GTAV.Config.FOG
	local hz = (fog and fog.color) or {170, 181, 194}
	local hr, hg, hb = hz[1] or 170, hz[2] or 181, hz[3] or 194
	local zr, zg, zb = 66, 106, 176     -- clear-day zenith

	local pos, col = {}, {}
	for j = 0, SKY_RINGS do
		local phi = -math.pi * 0.5 + math.pi * j / SKY_RINGS
		local cp, sp = math.cos(phi), math.sin(phi)
		pos[j], col[j] = {}, {}
		for i = 0, SKY_SEGS - 1 do
			local th = 2 * math.pi * i / SKY_SEGS
			local dx, dy, dz = cp * math.cos(th), cp * math.sin(th), sp
			pos[j][i] = Vector(dx * SKY_R, dy * SKY_R, dz * SKY_R)

			local t = dz * 2        -- smoothstep(dz / 0.5)
			if t < 0 then t = 0 elseif t > 1 then t = 1 end
			local s = t * t * (3 - 2 * t)

			local d = dx * sd.x + dy * sd.y + dz * sd.z
			if d < 0 then d = 0 end
			local g = d ^ 8 * 0.30 + d ^ 64 * 0.55 + d ^ 256 * 0.65

			local r = hr + (zr - hr) * s + g * 255
			local gg = hg + (zg - hg) * s + g * 232
			local b = hb + (zb - hb) * s + g * 196
			col[j][i] = {
				math.floor(math.Clamp(r, 0, 255) + 0.5),
				math.floor(math.Clamp(gg, 0, 255) + 0.5),
				math.floor(math.Clamp(b, 0, 255) + 0.5),
			}
		end
	end

	skyDomeMesh = Mesh()
	mesh.Begin(skyDomeMesh, MATERIAL_TRIANGLES, SKY_RINGS * SKY_SEGS * 4 * 3)   -- vertex count not prims
	local function emit(p, c)
		mesh.Position(p)
		mesh.Color(c[1], c[2], c[3], 255)
		mesh.AdvanceVertex()
	end
	for j = 0, SKY_RINGS - 1 do
		for i = 0, SKY_SEGS - 1 do
			local i2 = (i + 1) % SKY_SEGS
			local p00, p01 = pos[j][i], pos[j][i2]
			local p10, p11 = pos[j + 1][i], pos[j + 1][i2]
			local c00, c01 = col[j][i], col[j][i2]
			local c10, c11 = col[j + 1][i], col[j + 1][i2]

			-- both windings, dome vmt has no nocull
			emit(p00, c00) emit(p10, c10) emit(p11, c11)
			emit(p00, c00) emit(p11, c11) emit(p01, c01)
			emit(p00, c00) emit(p11, c11) emit(p10, c10)
			emit(p00, c00) emit(p01, c01) emit(p11, c11)
		end
	end
	mesh.End()

	local up = (math.abs(sd.z) > 0.99) and Vector(0, 1, 0) or Vector(0, 0, 1)
	local u = sd:Cross(up):GetNormalized()
	local v = sd:Cross(u):GetNormalized()
	local cc = Vector(sd.x * SKY_R * 0.998, sd.y * SKY_R * 0.998, sd.z * SKY_R * 0.998)

	local ring = 24
	skySunMesh = Mesh()
	mesh.Begin(skySunMesh, MATERIAL_TRIANGLES, ring * 3)   -- 24 tris, 72 verts
	for k = 0, ring - 1 do
		local a1 = 2 * math.pi * k / ring
		local a2 = 2 * math.pi * (k + 1) / ring
		local p1 = cc + u * (math.cos(a1) * SKY_DISC_R) + v * (math.sin(a1) * SKY_DISC_R)
		local p2 = cc + u * (math.cos(a2) * SKY_DISC_R) + v * (math.sin(a2) * SKY_DISC_R)
		mesh.Position(cc)
		mesh.Color(255, 244, 224, 255)
		mesh.AdvanceVertex()
		mesh.Position(p1)
		mesh.Color(255, 244, 224, 0)
		mesh.AdvanceVertex()
		mesh.Position(p2)
		mesh.Color(255, 244, 224, 0)
		mesh.AdvanceVertex()
	end
	mesh.End()

	skyDomeMat = Material("gtav/sky_dome")
	skySunMat = Material("gtav/sky_sun")
end

local function drawSkyMeshes()
	render.SetMaterial(skyDomeMat)
	skyDomeMesh:Draw()
	if skySunMesh ~= nil then
		render.SetMaterial(skySunMat)
		skySunMesh:Draw()
	end
end

local function drawSkyDome()
	if skyDomeDead then return end
	refreshSkyDome()
	if not skyDomeActive then return end

	if skyDomeMesh == nil or skyBuiltAz ~= skySunAz or skyBuiltEl ~= skySunEl then
		if not skyBuildPending then
			skyBuildPending = true
			timer.Simple(0, function()
				skyBuildPending = false
				if skyDomeDead then return end
				local ok, err = pcall(buildSkyMeshes)
				if not ok then
					skyDomeDead = tostring(err)
					ErrorNoHalt("[GTAV] skydome disabled (build): " .. skyDomeDead .. "\n")
					return
				end
				skyBuiltAz, skyBuiltEl = skySunAz, skySunEl
				print(string.format("[GTAV] skydome: R %d, sun azim %.1f deg elev %.1f deg",
					SKY_R, math.deg(skySunAz or 0), math.deg(skySunEl or 0)))
			end)
		end
		return
	end


	skyMtx:SetTranslation(EyePos())
	cam.PushModelMatrix(skyMtx)

	render.OverrideDepthEnable(true, false)

	local ok, err = pcall(drawSkyMeshes)

	render.OverrideDepthEnable(false, false)
	cam.PopModelMatrix()

	if not ok then
		skyDomeDead = tostring(err)
		skyDomeActive = false
		ErrorNoHalt("[GTAV] skydome disabled (draw): " .. skyDomeDead .. "\n")
	end
end


local postVignette, postVignetteTried = nil, false

local function postConvar(name, default)
	local cv = GetConVar(name)
	if cv == nil then return default end
	return cv:GetBool()
end

local function drawPostVignette()
	if not postVignetteTried then
		postVignetteTried = true
		local ok, m = pcall(Material, "gtav/vignette.png")
		if ok and m and not m:IsError() then postVignette = m end
	end
	if not postVignette then return end
	render.SetMaterial(postVignette)
	render.DrawScreenQuad()
end

CreateClientConVar("gtav_cl_post_aa", "1", true, false, "Post anti-aliasing (edge filter shader)")
CreateClientConVar("gtav_cl_post_aa_threshold", "0.12", false, false,
	"AA: local luma contrast below which a pixel is left untouched", 0, 1)
CreateClientConVar("gtav_cl_post_aa_strength", "1", false, false, "AA blend strength", 0, 1)

local postAAMat, postAATried = nil, false
local function drawPostAA()
	if not postAATried then
		postAATried = true
		if file.Exists("shaders/fxc/gtav_aa_ps20b.vcs", "GAME") then
			postAAMat = CreateMaterial("gtav_post_aa", "screenspace_general", {
				["$pixshader"] = "gtav_aa_ps20b",
				["$basetexture"] = "_rt_FullFrameFB",
				["$linearread_basetexture"] = 1,
				["$linearwrite"] = 1,
				["$ignorez"] = 1,
				["$vertextransform"] = 1,   -- quad lands offset without this
				["$c0_x"] = 0, ["$c0_y"] = 0, ["$c0_z"] = 0.12, ["$c0_w"] = 1,
			})
			if postAAMat and postAAMat:IsError() then postAAMat = nil end
		end
		print("[GTAV] post AA " .. (postAAMat and "on (gtav_aa_ps20b)" or "off (shader missing)"))
	end
	if not postAAMat then return end
	postAAMat:SetFloat("$c0_x", 1 / ScrW())
	postAAMat:SetFloat("$c0_y", 1 / ScrH())
	postAAMat:SetFloat("$c0_z", GetConVar("gtav_cl_post_aa_threshold"):GetFloat())
	postAAMat:SetFloat("$c0_w", GetConVar("gtav_cl_post_aa_strength"):GetFloat())
	render.UpdateScreenEffectTexture()
	render.SetMaterial(postAAMat)
	render.DrawScreenQuad()
end

hook.Add("RenderScreenspaceEffects", "GTAVPostFX", function()
	if not postConvar("gtav_cl_post", true) then return end
	if not GTAV.MapData or not GTAV.MapData.current then return end

	if postConvar("gtav_cl_post_aa", true) then drawPostAA() end

	if postConvar("gtav_cl_post_rays", true) and skyDomeActive and
	   skySunAz and skySunEl then
		local f = DrawSunbeams or DrawSunBeams
		if type(f) == "function" then
			local sd = Vector(math.cos(skySunEl) * math.cos(skySunAz),
							  math.cos(skySunEl) * math.sin(skySunAz),
							  math.sin(skySunEl))
			local fade = (sd:Dot(EyeVector()) - 0.8) * 5
			if fade > 0 then
				local ok, scr = pcall(function()
					return (EyePos() + sd * 4096):ToScreen()
				end)
				if ok and scr and scr.x and scr.y then
					local cvR = GetConVar("gtav_cl_post_rays_mult")
					local rm = cvR and cvR:GetFloat() or 0.1
					f(0.92, rm * fade, 0.04,   -- darken, mult, size, sun x y
					  scr.x / ScrW(), scr.y / ScrH())
				end
			end
		end
	end

	if postConvar("gtav_cl_post_bloom", true) then
		local f = DrawBloom
		if type(f) == "function" then
			local cvD = GetConVar("gtav_cl_post_bloom_darken")
			local cvM = GetConVar("gtav_cl_post_bloom_mult")
			f(cvD and cvD:GetFloat() or 0.75, cvM and cvM:GetFloat() or 0.25,
			  9, 9, 2, 1, 1, 1, 1)   -- size x y, passes, mult, rgb
		end
	end

	if postConvar("gtav_cl_post_vignette", false) then
		drawPostVignette()
	end

	if postConvar("gtav_cl_post_grade", false) then
		local f = DrawColorModify
		if type(f) == "function" then
			f({
				["$pp_colour_addr"] = 0,
				["$pp_colour_addg"] = 0,
				["$pp_colour_addb"] = 0,
				["$pp_colour_brightness"] = 0.015,
				["$pp_colour_contrast"] = 1.06,
				["$pp_colour_colour"] = 0.82,
				["$pp_colour_mulr"] = 0,
				["$pp_colour_mulg"] = 0,
				["$pp_colour_mulb"] = 0,
			})
		end
	end
end)

local fogAmbient = nil
local fogAmbientNext = 0

local function fogDistances()
	if not GTAV.IsActive() then return nil end
	local cvFog = GetConVar("gtav_fog")
	if cvFog and not cvFog:GetBool() then return nil end
	local fog = GTAV.Config.FOG
	if not fog or not fog.enabled then return nil end

	local startDist = fog.near or 0
	local endDist = fog.far or GTAV.Config.STREAM_RADIUS
	local s = GetConVar("gtav_fog_start")
	local e = GetConVar("gtav_fog_end")
	if s and s:GetFloat() > 0 then startDist = s:GetFloat() end
	if e and e:GetFloat() > 0 then endDist = e:GetFloat() end

	local eff = C.EffRadius
	if eff and eff > 0 and eff < endDist then
		endDist = eff
		if startDist > endDist * 0.75 then
			startDist = endDist * 0.55
		end
	end
	return startDist, endDist
end

local function GTAVFog()
	refreshSkyDome()

	local startDist, endDist = fogDistances()
	if not endDist then return end
	local fog = GTAV.Config.FOG

	local color = fog.color or {170, 181, 194}
	local r, g, b = color[1] or 170, color[2] or 181, color[3] or 194

	local now = CurTime()
	if skyDomeActive then
		fogAmbient = 1   -- frozen, has to match the dome horizon
	elseif now >= fogAmbientNext then
		fogAmbientNext = now + 0.5
		local ply = LocalPlayer()
		if IsValid(ply) then
			local ok, amb = pcall(render.GetLightColor, ply:GetPos())
			if ok and amb and amb.x and amb.y and amb.z then
				local lum = (amb.x * 0.2126 + amb.y * 0.7152 + amb.z * 0.0722)
				fogAmbient = math.Clamp(0.55 + lum * 0.75, 0.35, 1.35)
			end
		end
	end
	if fogAmbient then
		r = math.Clamp(r * fogAmbient, 0, 255)
		g = math.Clamp(g * fogAmbient, 0, 255)
		b = math.Clamp(b * fogAmbient, 0, 255)
	end

	render.FogMode(MATERIAL_FOG_LINEAR)
	render.FogColor(r, g, b)
	render.FogStart(startDist)
	render.FogEnd(endDist)
	render.FogMaxDensity(1)
	return true
end

hook.Add("SetupWorldFog", "GTAV_Fog", GTAVFog)
hook.Add("SetupSkyboxFog", "GTAV_SkyFog", GTAVFog)

CreateClientConVar("gtav_cl_zfar", "0", false, false,
	"Far clip plane (0 = auto: just past the fog end / stream radius; -1 = engine default)")

function C.WantedZFar()
	local cv = GetConVar("gtav_cl_zfar")
	local v = cv and cv:GetFloat() or 0
	if v < 0 then return nil end
	if v > 0 then return v end
	local _, endDist = fogDistances()
	endDist = endDist or GTAV.Config.STREAM_RADIUS or 90000
	return endDist + 2048
end

function C.CalcViewFarPlane(ply, origin, angles, fov, znear, zfar)
	C.EngineZFar = zfar
	local want = C.WantedZFar()
	if not want or not zfar or want <= zfar or not GAMEMODE or not GAMEMODE.CalcView then
		C.ActiveZFar = nil
		return
	end
	local ok, view = pcall(GAMEMODE.CalcView, GAMEMODE, ply, origin, angles, fov, znear, want)
	if not ok or type(view) ~= "table" then
		C.ActiveZFar = nil
		return
	end
	view.zfar = want
	C.ActiveZFar = want
	return view
end

hook.Add("PreDrawSkyBox", "GTAV_SkyDome", function()
	return skyDomeActive and skyDomeMesh ~= nil
end)

function C.Initialize()
	if not GTAV.IsActive() then return end
	if C.initialized then return end
	C.initialized = true

	print("[GTAV] Client initializing...")

	hook.Add("Think", "GTAV_StreamMeshes", C.StreamMeshes)
	hook.Add("Think", "GTAV_BuildGround", C.BuildGround)
	hook.Add("Think", "GTAV_SyncClientCells", C.SyncClientCells)
	hook.Add("PreDrawOpaqueRenderables", "GTAV_RenderWorld", C.RenderWorld)
	hook.Add("PostDrawTranslucentRenderables", "GTAV_RenderGlass", C.RenderGlass)
	hook.Add("CalcView", "GTAV_FarPlane", C.CalcViewFarPlane)
	hook.Add("Think", "GTAV_CrossCellEntities", C.CrossCellEntities)
	hook.Add("PostDrawTranslucentRenderables", "GTAV_DebugDraw", C.DebugDraw)

	local function teleportAbs(x, y, z)
		C.SkipHoldUntil = CurTime() + 1.0
		net.Start("GTAV_Teleport")
		net.WriteFloat(x)
		net.WriteFloat(y)
		net.WriteFloat(z or 0)
		net.WriteBool(z ~= nil)
		net.SendToServer()
	end

	concommand.Add("gtav_tp", function(ply, cmd, args)
		local x, y = tonumber(args[1]), tonumber(args[2])
		local z = tonumber(args[3])
		if not x or not y then
			print("[GTAV] usage: gtav_tp <x> <y> [z]  (absolute coordinates)")
			return
		end
		teleportAbs(x, y, z)
	end)

	local GTA_SCALE = 39.3700787   -- 1 m in source units
	concommand.Add("gtav_gtatp", function(ply, cmd, args)
		local x, y = tonumber(args[1]), tonumber(args[2])
		local z = tonumber(args[3])
		if not x or not y then
			print("[GTAV] usage: gtav_gtatp <x> <y> [z]  (GTA V metres)")
			return
		end
		teleportAbs(x * GTA_SCALE, y * GTA_SCALE, z and (z * GTA_SCALE) or nil)
	end)

	concommand.Add("gtav_respawn", function()
		C.SkipHoldUntil = CurTime() + 1.0
		net.Start("GTAV_Respawn")
		net.SendToServer()
	end)

	concommand.Add("gtav_frame", function()
		local ply = LocalPlayer()
		if not IsValid(ply) then return end
		local cell = ply:GetNW2Vector("WorldCell", Vector(0, 0, 0))
		local l = ply:GetPos()
		local a = l + cell
		print(string.format("[GTAV] cell %s  local %.0f %.0f %.0f  abs %.0f %.0f %.0f  gta %.1f %.1f %.1f  mt=%d",
			GTAV.GetCellKey(cell), l.x, l.y, l.z, a.x, a.y, a.z,
			a.x / 39.3700787, a.y / 39.3700787, a.z / 39.3700787,
			ply:GetMoveType()))
	end)

	print("[GTAV] Client initialized")
end


local function desiredLOD(dist, bias)
	local lodD = GTAV.Config.LOD_DISTANCES
	local lod = 4
	for i = 0, 4 do
		if dist <= lodD[i] then lod = i break end
	end
	return math.Clamp(math.floor(lod + bias + 0.5), 0, 4)
end

local function lodChanged(cur, want, dist)
	if want == cur then return false end
	local lodD = GTAV.Config.LOD_DISTANCES
	local boundary = lodD[math.min(cur, want)] or 4096
	local band = math.max(512, boundary * 0.25)
	if want > cur then return dist > boundary + band end   -- getting coarser
	return dist < boundary - band                          -- getting finer
end

local BACK_PENALTY = 6000      -- added distance for a tile behind the camera
local LOOKAHEAD_MAX = 20000    -- clamp for the velocity prediction

function C.UpdateTileQueue(eyeAbs, settling)
	ensureTileList()

	local cfg = GTAV.Config
	local bias = GetConVar("gtav_cl_lod_bias"):GetFloat()
	local lightStamp = GTAV.LightStamp or 0
	local maxDist = math.min(cfg.LOD_DISTANCES[4],
							 cfg.STREAM_RADIUS or cfg.LOD_DISTANCES[4])
	local radiusOverride = GetConVar("gtav_stream_radius")
	if radiusOverride and radiusOverride:GetInt() > 0 then
		maxDist = radiusOverride:GetInt()
	end
	local budgetCvar = GetConVar("gtav_cl_tile_budget")
	local maxTiles = (budgetCvar and budgetCvar:GetInt()) or 0
	if maxTiles <= 0 then
		maxTiles = cfg.MAX_TILES or 24000
	end

	local ply = LocalPlayer()
	local vel = (IsValid(ply) and ply:GetVelocity()) or vector_origin
	local lookX, lookY = vel.x, vel.y
	local lookLen = math.sqrt(lookX * lookX + lookY * lookY)
	if lookLen > LOOKAHEAD_MAX then
		local k = LOOKAHEAD_MAX / lookLen
		lookX, lookY = lookX * k, lookY * k
		lookLen = LOOKAHEAD_MAX
	end

	local fwdX, fwdY = 0, 0
	if IsValid(ply) then
		local ang = ply:GetAngles()
		local f = ang:Forward()
		local fl = math.sqrt(f.x * f.x + f.y * f.y)
		if fl > 0.15 then
			fwdX, fwdY = f.x / fl, f.y / fl
		end
	end

	local now = CurTime()
	local wantList = {}
	local wanted = {}

	local function consider(t)
		local fp = footprint(t, GTAV.Config.VISUAL_TILE_SIZE)
		local near2 = tileNearDistSqr(eyeAbs, t.tx, t.ty,
									  GTAV.Config.VISUAL_TILE_SIZE, fp,
									  t.z0, t.z1)
		if near2 > maxDist * maxDist then return end
		local dist = math.sqrt(GTAV.Dist2DSqr(eyeAbs, t.center))
		local lodDist = math.sqrt(near2)
		local lod = desiredLOD(lodDist, bias)

		local active = C.ActiveTiles[t.key]
		local cost = #(t.members or {})
		if cost < 1 then cost = 1 end

		local prio = dist
		if dist > 1 then
			local dx = (t.center.x - eyeAbs.x) / dist
			local dy = (t.center.y - eyeAbs.y) / dist
			prio = prio - (dx * lookX + dy * lookY)
			if fwdX ~= 0 or fwdY ~= 0 then
				if dx * fwdX + dy * fwdY < -0.15 then
					prio = prio + BACK_PENALTY
				end
			end
		end

		-- new      not resident
		-- keep     resident and current
		-- defer    wrong lod but a crossing is settling
		-- rebuild  lod, bias or lighting changed
		local mode = "new"
		if active then
			local relight = active.lightStamp ~= lightStamp
			if active.lod == lod and not relight then
				mode = "keep"
			elseif settling then
				mode = "defer"
			elseif active.lodBias ~= bias
				or relight
				or lodChanged(active.lod, lod, lodDist) then
				mode = "rebuild"
			else
				mode = "keep"
			end
		end

		wantList[#wantList + 1] = {
			g = t, key = t.key, tx = t.tx, ty = t.ty, span = t.span,
			center = t.center, members = t.members,
			z0 = t.z0, z1 = t.z1,
			lod = lod, lodBias = bias, lightStamp = lightStamp,
			dist = dist, lodDist = lodDist, prio = prio,
			cost = cost, mode = mode,
		}
	end

	local cs = cfg.CELL_SIZE
	local ex = math.floor(eyeAbs.x / cs)
	local ey = math.floor(eyeAbs.y / cs)
	local reach = math.ceil(maxDist / cs)
	for cx = ex - reach, ex + reach do
		for cy = ey - reach, ey + reach do
			local bucket = C.TileCells[cx .. " " .. cy]
			if bucket then
				for i = 1, #bucket do
					consider(bucket[i])
				end
			end
		end
	end

	local BIN = 256

	local function countingSort(list, field, bin)
		local bins, maxBin = {}, 0
		for i = 1, #list do
			local v = list[i][field]
			local b = math.floor(v / bin) + 1
			if b < 1 then b = 1 end
			local bucket = bins[b]
			if not bucket then
				bucket = {}
				bins[b] = bucket
			end
			bucket[#bucket + 1] = list[i]
			if b > maxBin then maxBin = b end
		end
		local out = {}
		for b = 1, maxBin do
			local bucket = bins[b]
			if bucket then
				for i = 1, #bucket do
					out[#out + 1] = bucket[i]
				end
			end
		end
		return out
	end

	local DIST_BIN = 32
	local byDist = countingSort(wantList, "dist", DIST_BIN)

	local covered, keep = 0, 0
	local curBin = nil
	for i = 1, #byDist do
		local e = byDist[i]
		local b = math.floor(e.dist / DIST_BIN) + 1
		-- only cut between dist bins
		if keep > 0 and b ~= curBin and covered + e.cost > maxTiles then break end
		covered = covered + e.cost
		keep = i
		curBin = b
	end

	local flight = {}
	if C.AsyncTask then flight[C.AsyncTask.key] = true end
	if C.ReadyTask then flight[C.ReadyTask.key] = true end
	if C.CommitTask then flight[C.CommitTask.key] = true end
	do
		local wt = C.WrapTasks
		if wt then
			for w = 1, #wt do flight[wt[w].key] = true end
		end
	end
	do
		local is = C.InstallSet
		if is then
			for k in pairs(is) do flight[k] = true end
		end
	end

	local queue = {}
	C.EffRadius = 0
	for i = 1, keep do
		local e = byDist[i]
		wanted[e.key] = true

		local reach = e.dist + (e.span or 1) * cfg.VISUAL_TILE_SIZE * 0.75
		if reach > C.EffRadius then C.EffRadius = reach end

		if (e.mode == "new" or e.mode == "rebuild") and not flight[e.key] then
			queue[#queue + 1] = e
		end
	end

	C.BuildQueue = countingSort(queue, "prio", BIN)
	C.BuildHead = 1

	C.RefreshMatPrefetch()

	local now = CurTime()
	if now >= (C.NextAdmitLog or 0) then
		C.NextAdmitLog = now + 5
		local modes = {}
		for i = 1, keep do
			local m = byDist[i].mode or "?"
			modes[m] = (modes[m] or 0) + 1
		end
		local line = string.format(
			"[GTAV] admit: want %d groups -> kept %d (%d tiles of budget %d) | resident %d | radius %d of %d | " ..
			"budget cvar %d | modes new %d rebuild %d keep %d defer %d",
			#wantList, keep, covered, maxTiles,
			table.Count(C.ActiveTiles), C.EffRadius or 0, maxDist,
			budgetCvar and budgetCvar:GetInt() or 0,
			modes.new or 0, modes.rebuild or 0,
			modes.keep or 0, modes.defer or 0)
		if line ~= C.LastAdmitReport then
			C.LastAdmitReport = line
			print(line)
		end
	end

	local evictKey, evictTile
	local evictJumpSqr = (cfg.CELL_SIZE * 0.5) ^ 2
	local nearRing2 = (cfg.LOD_DISTANCES[1] or 16384) ^ 2
	for key, tile in pairs(C.ActiveTiles) do
		if wanted[key] then
			tile.lastWanted = now
			tile.lastWantedEye = eyeAbs
		else
			tile.lastWanted = tile.lastWanted or now

			if not tile.lastWantedEye then
				tile.lastWantedEye = eyeAbs
			elseif (eyeAbs - tile.lastWantedEye):LengthSqr() > evictJumpSqr then
				tile.lastWantedEye = eyeAbs
				tile.lastWanted = now
			elseif not settling then
				local grace = EVICT_AFTER
				if tile.lod >= 3 then grace = EVICT_AFTER * 2 end
				if GTAV.Dist2DSqr(eyeAbs, tile.center) < nearRing2 then
					grace = 0.5
				end
				if now - tile.lastWanted > grace then
					if not evictTile or tile.lastWanted < evictTile.lastWanted then
						evictKey, evictTile = key, tile
					end
				end
			end
		end
	end
	if evictTile then
		C.FreeTile(evictTile)
		C.ActiveTiles[evictKey] = nil
	end

	C.DrawOrderDirty = true
end

function C.FreeTile(tile)
	C.Perf.freedThisFrame = (C.Perf.freedThisFrame or 0) + 1
	local t0 = SysTime()
	for _, sub in ipairs(tile.submeshes or {}) do
		if sub.mesh then sub.mesh:Destroy() end
	end
	for _, mesh in ipairs(tile.meshes or {}) do
		if mesh then mesh:Destroy() end
	end
	C.Perf.freeMs = (C.Perf.freeMs or 0) + (SysTime() - t0) * 1000
	tile.submeshes = nil
	tile.meshes = nil
end

function C.RefreshDrawOrder()
	local list = {}
	for _, tile in pairs(C.ActiveTiles) do
		list[#list + 1] = tile
	end

	local BIN = 512
	local bins, maxBin = {}, 0
	local eyeAbs = C.LastEyePos
	for i = 1, #list do
		local t = list[i]
		local d
		if eyeAbs then
			d = math.sqrt(tileNearDistSqr(eyeAbs, t.tx, t.ty,
										  GTAV.Config.VISUAL_TILE_SIZE,
										  footprint(t, GTAV.Config.VISUAL_TILE_SIZE),
										  t.z0, t.z1))
		else
			d = 0
		end
		t.sortDist = d
		local b = math.floor(d / BIN) + 1
		if b < 1 then b = 1 end
		local bucket = bins[b]
		if not bucket then
			bucket = {}
			bins[b] = bucket
		end
		bucket[#bucket + 1] = t
		if b > maxBin then maxBin = b end
	end

	local ordered = {}
	for b = 1, maxBin do
		local bucket = bins[b]
		if bucket then
			for i = 1, #bucket do
				ordered[#ordered + 1] = bucket[i]
			end
		end
	end
	C.DrawOrder = ordered

	local glass, additive = {}, {}
	for i = #ordered, 1, -1 do
		local t = ordered[i]
		local subs = t.submeshes
		if subs then
			local hasGlass, hasAdd = false, false
			for s = 1, #subs do
				local sub = subs[s]
				if sub.additive then
					hasAdd = true
				elseif sub.glass then
					hasGlass = true
				end
				if hasGlass and hasAdd then break end
			end
			if hasGlass then glass[#glass + 1] = t end
			if hasAdd then additive[#additive + 1] = t end
		end
	end
	C.GlassOrder = glass
	C.AdditiveOrder = additive
	C.DrawOrderDirty = false
end

function C.Deactivate()
	for key, tile in pairs(C.ActiveTiles) do
		C.FreeTile(tile)
		C.ActiveTiles[key] = nil
	end
	C.BuildQueue = {}
	C.BuildHead = 1
	C.LastQueueTime = nil
	C.LastQueueEye = nil
	C.DrawOrder = nil
	C.GlassOrder = nil
	C.AdditiveOrder = nil
	C.DrawOrderDirty = true
	C.GroupList = nil
	C.GroupListIndex = nil
	C.LastEyePos = nil
	C.LastPlayerCellKey = nil
	C.NextGroundBuild = 0
	C.LastGroundCount = 0
	C.BuiltGroundCount = 0
	C.initialized = false
end


local STABLE_JUMP = 3000
local STABLE_JUMP_SQR = STABLE_JUMP * STABLE_JUMP
local MAX_HOLD = 0.75

C.Stable = C.Stable or {}
C.RenderCell = C.RenderCell or nil

-- dont touch this
local function Stable(slot, eye, cell)
	local s = C.Stable[slot]
	local abs = eye + cell

	if CurTime() < (C.SkipHoldUntil or 0) then
		C.Stable[slot] = { abs = abs, cell = cell, eye = eye }
		return abs, cell
	end

	-- origin moved first, netvar late
	if s and s.cell == cell and (eye - s.eye):LengthSqr() > STABLE_JUMP_SQR then
		local moved = s.eye - eye
		if (moved - GTAV.SnapCell(moved)):LengthSqr() < 64 * 64 then
			local now = CurTime()
			s.since = s.since or now
			if now - s.since < MAX_HOLD then
				return s.abs, s.cell
			end
		end
		C.Stable[slot] = nil
		return abs, cell
	end

	-- netvar moved first, origin late
	if s and s.cell ~= cell
		and (abs - s.abs):LengthSqr() > STABLE_JUMP_SQR
		and (eye + s.cell - s.abs):LengthSqr() < STABLE_JUMP_SQR then
		local now = CurTime()
		s.since = s.since or now
		if now - s.since < MAX_HOLD then
			return eye + s.cell, s.cell
		end
		if slot == "draw" then
			print(string.format(
				"[GTAV] frame stuck: net %s draw %s local %.0f %.0f eye %.0f %.0f abs %.0f %.0f (origin never caught up, taking netvar)",
				GTAV.GetCellKey(cell), GTAV.GetCellKey(s.cell),
				eye.x, eye.y, eye.x, eye.y, abs.x, abs.y))
		end
	end
	C.Stable[slot] = { abs = abs, cell = cell, eye = eye }
	return abs, cell
end

function C.StreamMeshes()
	if not GTAV.MapData.current then return end

	local ply = LocalPlayer()
	if not IsValid(ply) then return end

	C.Perf.freedThisFrame = 0
	C.Perf.freeMs = 0

	local startTime = SysTime()
	local budget = GTAV.Config.STREAM_BUDGET

	local ph = C.Perf.framePhase
	if not ph then
		ph = {wanted = 0, poll = 0, warm = 0, commit = 0, wrap = 0, prep = 0,
			  sync = 0, gc = 0, total = 0}
		C.Perf.framePhase = ph
	end
	ph.wanted, ph.poll, ph.warm, ph.commit, ph.wrap, ph.prep, ph.sync, ph.gc,
		ph.total = 0, 0, 0, 0, 0, 0, 0, 0, 0

	local eye = ply:GetPos()
	local cell = ply:GetNW2Vector("WorldCell", Vector(0, 0, 0))

	local eyeAbs = Stable("cam", eye, cell)
	local _, drawCell = Stable("draw", eye, cell)

	C.RenderCell = drawCell
	C.LastEyePos = eyeAbs
	C.LastEyeCell = cell
	C.EyeMismatch = drawCell ~= cell

	do
		local cellKey = GTAV.GetCellKey(cell)
		if C.Perf.lastCellKey and C.Perf.lastCellKey ~= cellKey then
			C.Perf.watchUntil = CurTime() + 0.5
			C.Perf.pendingReason = C.Perf.lastCellKey .. " -> " .. cellKey
			C.Perf.worstMs = 0
			C.Perf.holdFrames = 0
			C.Perf.winBuilds = 0
			C.Perf.winBuildMs = 0
			C.Perf.maxGroundMs = 0
			C.Perf.maxGroundMoves = 0
			C.Perf.maxRenderMs = 0
			C.WorstGroupMs = 0
			C.Perf.worstWanted = 0
			C.Perf.worstPoll = 0
			C.Perf.worstWarm = 0
			C.Perf.worstCommit = 0
			C.Perf.worstWrap = 0
			C.Perf.worstPrep = 0
			C.Perf.worstSync = 0
			C.Perf.worstGC = 0
			C.Perf.worstGround = 0
			C.Perf.crossAbs = C.Perf.lastAbs
			C.Perf.maxDrift = 0

			print(string.format(
				"[GTAV] cross %s -> %s | pos %.0f %.0f eye %.0f %.0f | draw %s cam %s",
				C.Perf.lastCellKey, cellKey,
				ply:GetPos().x, ply:GetPos().y, eye.x, eye.y,
				GTAV.GetCellKey(drawCell), GTAV.GetCellKey(eyeAbs - eye)))
		end
		C.Perf.lastCellKey = cellKey
	end

	if C.EyeMismatch and C.Perf.watchUntil > CurTime() then
		C.Perf.holdFrames = (C.Perf.holdFrames or 0) + 1
	end

	local now = CurTime()
	local eyeMoved = not C.LastQueueEye or
		(eyeAbs - C.LastQueueEye):LengthSqr() > (GTAV.Config.VISUAL_TILE_SIZE * 0.35) ^ 2
	if not C.LastQueueTime or now - C.LastQueueTime > 0.15 or eyeMoved then
		local t0 = SysTime()
		C.UpdateTileQueue(eyeAbs, C.Perf.watchUntil > now)
		ph.wanted = (SysTime() - t0) * 1000
		C.LastQueueTime = now
		C.LastQueueEye = eyeAbs
	end

	do
		local warmBudget = 8
		local cv = GetConVar("gtav_cl_warm_budget_ms")
		if cv then warmBudget = cv:GetFloat() end
		if warmBudget < 1 then warmBudget = 1 end
		local backlog = #C.WarmQueue - (C.WarmHead or 1) + 1
		if backlog > 32 and not C.CommitTask and not C.WrapTasks[1]
			and (C.BuildDebt or 0) <= 0 and not C.InstallPending[1] then
			warmBudget = warmBudget * 3
		end
		local tW = SysTime()
		C.DrainMaterialWarm(warmBudget)
		do
			local cvP = GetConVar("gtav_cl_mat_prefetch_budget_ms")
			local preBudget = (cvP and cvP:GetFloat()) or 6
			if preBudget > 0 and GetConVar("gtav_cl_mat_prefetch"):GetBool() then
				C.MatManifestTick(3)
				local left = warmBudget - (SysTime() - tW) * 1000
				if left > preBudget then left = preBudget end
				if left > 0 then C.DrainMatPrefetch(left) end
			end
		end
		ph.warm = (SysTime() - tW) * 1000
		if ph.warm > (C.Perf.worstWarm or 0) then C.Perf.worstWarm = ph.warm end
	end

	do
		local gcBudget = 1.5
		local cv = GetConVar("gtav_cl_gc_budget_ms")
		if cv then gcBudget = cv:GetFloat() end
		ph.gc = C.GcPump(gcBudget)
		if ph.gc > (C.Perf.worstGC or 0) then C.Perf.worstGC = ph.gc end
	end

	local maxBuilds = GTAV.Config.BUILDS_PER_FRAME or 24
	C.BuildDebt = C.BuildDebt or 0
	if C.BuildDebt > 0 then
		C.BuildDebt = math.max(0, C.BuildDebt - budget * 2)
		if C.BuildDebt > budget * 6 then C.BuildDebt = budget * 6 end
		C.BuildSkipped = (C.BuildSkipped or 0) + 1
	end

	local built = 0
	local loopStart = SysTime()

	local function installNow(task, tile)
		local old = C.ActiveTiles[task.key]
		if old then C.FreeTile(old) end
		tile.lodBias = task.lodBias
		tile.lightStamp = task.lightStamp
		C.ActiveTiles[task.key] = tile
		C.InstallSet[task.key] = nil
		C.DrawOrderDirty = true
		C.Stats.tilesLoaded = C.Stats.tilesLoaded + 1
		C.Stats.builds = C.Stats.builds + 1
		if task.startFrame then
			local lat = FrameNumber() - task.startFrame
			C.Perf.latSum = (C.Perf.latSum or 0) + lat
			C.Perf.latN = (C.Perf.latN or 0) + 1
			if lat > (C.Perf.latMax or 0) then C.Perf.latMax = lat end
		end
	end

	local installsThisFrame = 0
	local function installPerFrame()
		local cap = 3
		local cv = GetConVar("gtav_cl_installs_per_frame")
		if cv then cap = cv:GetInt() end
		return cap
	end
	local function installTile(task, tile)
		local cap = installPerFrame()
		if cap > 0 and installsThisFrame >= cap then
			C.InstallPending[#C.InstallPending + 1] = {task = task, tile = tile}
			C.InstallSet[task.key] = true
			return
		end
		installsThisFrame = installsThisFrame + 1
		installNow(task, tile)
	end
	do
		local cap = installPerFrame()
		if cap <= 0 then cap = #C.InstallPending + 1 end
		while C.InstallPending[1] and installsThisFrame < cap do
			local e = table.remove(C.InstallPending, 1)
			installsThisFrame = installsThisFrame + 1
			installNow(e.task, e.tile)
		end
	end

	local function wrapAdd(task)
		if C.WrapSet[task.key] then return end
		C.WrapSet[task.key] = task
		C.WrapTasks[#C.WrapTasks + 1] = task
	end

	-- AsyncTask       on the worker
	-- ReadyTask       built, waiting for the commit buffer
	-- CommitTask      budgeted mesh emit
	-- WrapTasks       attaching imaterials
	-- InstallPending  done, waiting for an install slot
	local asyncOn = false
	do
		local cv = GetConVar("gtav_cl_async_build")
		local N = GTAV.Native
		if (not cv or cv:GetBool()) and N and N.AsyncAvailable then
			asyncOn = N.AsyncAvailable() and true or false
		end
	end
	C.BuildAsyncOn = asyncOn

	local flagBudget = 8
	do
		local cv = GetConVar("gtav_cl_flags_budget_ms")
		if cv then flagBudget = cv:GetFloat() end
	end

	local function rebuildSync(task, why)
		C.AsyncFails = (C.AsyncFails or 0) + 1
		if C.AsyncFails <= 5 then
			print(string.format(
				"[GTAV] async build failed (%s) - rebuilding %s synchronously",
				tostring(why), tostring(task.key)))
		end
		local tS = SysTime()
		local tile = C.BuildGroupMesh(task)
		ph.sync = ph.sync + (SysTime() - tS) * 1000
		if tile then
			installTile(task, tile)
			built = built + 1
			return true
		end
		return false
	end

	if not asyncOn and (C.AsyncTask or C.ReadyTask or C.CommitTask) then
		GTAV.Native.BuildGroupCancel()
		local tasks = {}
		if C.CommitTask then tasks[#tasks + 1] = C.CommitTask end
		if C.ReadyTask then tasks[#tasks + 1] = C.ReadyTask end
		if C.AsyncTask then tasks[#tasks + 1] = C.AsyncTask end
		C.CommitTask = nil
		C.ReadyTask = nil
		C.AsyncTask = nil
		for i = 1, #tasks do
			rebuildSync(tasks[i], "async build switched off")
		end
		C.BuildDebt = math.max(0, (SysTime() - loopStart) - budget)
	end

	if asyncOn and C.AsyncTask then
		local task = C.AsyncTask
		local tP = SysTime()
		local slice = flagBudget
		if not C.ReadyTask and not C.CommitTask then slice = flagBudget * 3 end
		local state, why = GTAV.Native.BuildGroupPoll(slice)
		ph.poll = (SysTime() - tP) * 1000
		if state == "ready" then
			C.AsyncTask = nil
			C.ReadyTask = task
		elseif state == "failed" then
			C.AsyncTask = nil
			rebuildSync(task, why)
		end
	elseif asyncOn and (C.CommitTask or C.ReadyTask) then
		local tP = SysTime()
		GTAV.Native.BuildGroupPoll(flagBudget)
		ph.poll = (SysTime() - tP) * 1000
	end

	if asyncOn then
		for pass = 1, 2 do
			if C.ReadyTask and not C.CommitTask then
				C.BuildGroupCommitStart(C.ReadyTask)
				C.CommitTask = C.ReadyTask
				C.ReadyTask = nil
			end
			if not C.CommitTask then break end
			local task = C.CommitTask
			local tC = SysTime()
			local tile, st, wrapMs = C.BuildGroupFinish(task)
			local tMs = (SysTime() - tC) * 1000
			wrapMs = wrapMs or 0
			ph.commit = ph.commit + (tMs - wrapMs)
			ph.wrap = ph.wrap + wrapMs
			task.commitMs = (task.commitMs or 0) + tMs
			if tile then
				installTile(task, tile)
				C.CommitTask = nil
				if task.commitMs > (C.WorstGroupMs or 0) then
					C.WorstGroupMs = task.commitMs
				end
			elseif st == "warming" then
				C.CommitTask = nil
				wrapAdd(task)
			elseif st ~= "more" then
				C.CommitTask = nil
				rebuildSync(task, st)
				break
			else
				break
			end
		end
		C.BuildDebt = math.max(0, (SysTime() - loopStart) - budget)
	end

	if C.WrapTasks[1] then
		local wrapBudget = 8
		do
			local cv = GetConVar("gtav_cl_wrap_budget_ms")
			if cv then wrapBudget = cv:GetFloat() end
		end
		if wrapBudget < 1 then wrapBudget = 1 end
		local tW = SysTime()
		local i = 1
		local calls = 0
		while i <= #C.WrapTasks do
			local task = C.WrapTasks[i]
			local remain = wrapBudget - (SysTime() - tW) * 1000
			if remain <= 0 and calls > 0 then break end
			calls = calls + 1
			if remain < 1 then remain = 1 end
			local tC = SysTime()
			local tile, st = C.BuildGroupFinish(task, remain)
			local tMs = (SysTime() - tC) * 1000
			ph.wrap = ph.wrap + tMs
			task.commitMs = (task.commitMs or 0) + tMs
			if tile then
				table.remove(C.WrapTasks, i)
				C.WrapSet[task.key] = nil
				installTile(task, tile)
				if task.commitMs > (C.WorstGroupMs or 0) then
					C.WorstGroupMs = task.commitMs
				end
			elseif st == "warming" or st == "more" then
				i = i + 1
			else
				table.remove(C.WrapTasks, i)
				C.WrapSet[task.key] = nil
				rebuildSync(task, st)
			end
		end
	end

	while C.BuildHead <= #C.BuildQueue do
		-- always one build, debt only stops the rest
		if built > 0 and
			(C.BuildDebt > 0 or SysTime() - startTime >= budget
			 or built >= maxBuilds) then break end

		if asyncOn and (C.AsyncTask or C.ReadyTask) then break end   -- one job slot

		local task = C.BuildQueue[C.BuildHead]
		C.BuildHead = C.BuildHead + 1
		if C.BuildHead > #C.BuildQueue then
			C.BuildQueue = {}
			C.BuildHead = 1
		end

		local act = C.ActiveTiles[task.key]
		local stale = (act and act.lod == task.lod
						   and act.lightStamp == task.lightStamp) or
					  (C.CommitTask and C.CommitTask.key == task.key) or
					  (C.WrapSet and C.WrapSet[task.key] ~= nil) or
					  (C.InstallSet and C.InstallSet[task.key] == true)

		if not stale then
			local g0 = SysTime()
			local tile = nil
			local started = false
			task.startFrame = FrameNumber()
			if asyncOn then
				local preTask = nil
				local last = C.BuildHead + 31
				if last > #C.BuildQueue then last = #C.BuildQueue end
				for j = C.BuildHead, last do
					local cand = C.BuildQueue[j]
					if cand and cand.key ~= task.key then
						local cact = C.ActiveTiles[cand.key]
						local cstale = (cact and cact.lod == cand.lod
											and cact.lightStamp == cand.lightStamp) or
							(C.CommitTask and C.CommitTask.key == cand.key) or
							(C.WrapSet and C.WrapSet[cand.key] ~= nil) or
							(C.InstallSet and C.InstallSet[cand.key] == true)
						if not cstale then
							preTask = cand
							break
						end
					end
				end
				local ok = C.BuildGroupPrep(task, preTask)
				if ok then
					C.AsyncTask = task
					started = true
					built = built + 1
				end
			end
			if not started then
				tile = task and C.BuildGroupMesh(task)
			end
			local gMs = (SysTime() - g0) * 1000
			if gMs > (C.WorstGroupMs or 0) then C.WorstGroupMs = gMs end
			if started then
				ph.prep = ph.prep + gMs
			else
				ph.sync = ph.sync + gMs
			end
			if tile then
				installTile(task, tile)
				built = built + 1
			end
			C.BuildDebt = math.max(0, (SysTime() - loopStart) - budget)
		end
	end

	C.Perf.builtThisFrame = built
	C.Stats.buildTime = SysTime() - startTime
	C.Perf.lastBuildMs = C.Stats.buildTime * 1000
	C.Perf.lastAbs = C.LastEyePos

	ph.total = C.Perf.lastBuildMs
	if ph.wanted > (C.Perf.worstWanted or 0) then C.Perf.worstWanted = ph.wanted end
	if ph.poll   > (C.Perf.worstPoll or 0)   then C.Perf.worstPoll   = ph.poll   end
	if ph.commit > (C.Perf.worstCommit or 0) then C.Perf.worstCommit = ph.commit end
	if ph.wrap   > (C.Perf.worstWrap or 0)   then C.Perf.worstWrap   = ph.wrap   end
	if ph.prep   > (C.Perf.worstPrep or 0)   then C.Perf.worstPrep   = ph.prep   end
	if ph.sync   > (C.Perf.worstSync or 0)   then C.Perf.worstSync   = ph.sync   end

	if C.Perf.watchUntil > 0 then
		C.Perf.winBuilds = (C.Perf.winBuilds or 0) + built
		C.Perf.winBuildMs = (C.Perf.winBuildMs or 0) + C.Stats.buildTime * 1000
		if C.Perf.crossAbs then
			local drift = (C.LastEyePos - C.Perf.crossAbs):Length2D()
			if drift > (C.Perf.maxDrift or 0) then C.Perf.maxDrift = drift end
		end
	end
end

function C.BuildTileMesh(task)
	local tx, ty, lod = task.tx, task.ty, task.lod
	local tileSize = GTAV.Config.VISUAL_TILE_SIZE

	local info = GTAV.GetTileInfo(tx, ty, lod)
	if not info and task.file then
		info = {file = task.file}
	end
	if not info then
		C.TileInfoMiss = (C.TileInfoMiss or 0) + 1
		if C.TileInfoMiss <= 5 then
			print(string.format("[GTAV] no index LOD%d for tile %d %d", lod, tx, ty))
		end
		return nil
	end

	local path = "lossantos/" .. tostring(GTAV.MapData.name or "gm_lossantos") ..
		"/tiles/" .. info.file

	if not task.origin and GTAV.Native and GTAV.Native.Available then
		local t0 = SysTime()
		local built, why = GTAV.Native.BuildTile(info.file, tx, ty, tileSize)
		local tBuild = SysTime()
		if built then
			local triCount = 0
			for i = 1, #built do
				triCount = triCount + (built[i].tris or 0) * 3
			end
			if tBuild - t0 > 0.04 then
				print(string.format(
					"[GTAV] slow native tile %s (LOD%d): %.1fms -> %d draws",
					info.file, lod, (tBuild - t0) * 1000, #built))
			end
			C.NativeBuilds = (C.NativeBuilds or 0) + 1
			return {
				key = task.key,
				tx = tx, ty = ty, lod = lod,
				origin = Vector(tx * tileSize, ty * tileSize, 0),
				center = Vector(tx * tileSize + tileSize * 0.5,
								ty * tileSize + tileSize * 0.5, 0),
				submeshes = built,
				submeshCount = #built,
				triCount = triCount,
				lastWanted = CurTime(),
			}
		end
		if why then
			C.NativeMiss = (C.NativeMiss or 0) + 1
			if C.NativeMiss <= 5 then
				print(string.format("[GTAV] native declined %s: %s (Lua fallback)",
					info.file, tostring(why)))
			end
		end
	end

	local t0 = SysTime()
	local data = GTAV.Binary.ReadTile(path, tx, ty)
	local tRead = SysTime()
	if not data then
		C.TileReadMiss = (C.TileReadMiss or 0) + 1
		if C.TileReadMiss <= 5 then
			print(string.format("[GTAV] could not read tile %s", path))
		end
		return nil
	end

	local origin = task.origin or Vector(tx * tileSize, ty * tileSize, 0)

	local srcVerts = data.vertices
	local shared = {}
	for i = 1, #srcVerts do
		local sv = srcVerts[i]
		shared[i] = {
			pos = sv.pos - origin,
			u = sv.uv.x,
			v = sv.uv.y,
			normal = sv.normal,
		}
	end

	local submeshes = {}
	local matMs = 0
	local meshMs = 0
	local triCount = 0

	local srcIdx = data.indices
	local nIdx = #srcIdx
	local nShared = #shared

	for i, sub in ipairs(data.submeshes) do
		local matName = data.materials[sub.materialIndex + 1] or "error"
		local tm = SysTime()
		local mat = C.GetMaterial(matName)
		local mflags = C.GetMaterialFlags(matName)
			or {glass = false, nocull = false, additive = false}
		matMs = matMs + (SysTime() - tm)

		local idxStart = (sub.indexOffset or 0) + 1
		local idxCount = sub.indexCount or 0
		local vertOffset = sub.vertexOffset or 0

		if idxStart < 1 then
			idxCount = idxCount + (idxStart - 1)
			idxStart = 1
		end
		if idxStart + idxCount - 1 > nIdx then
			idxCount = math.max(0, nIdx - idxStart + 1)
			C.TileRangeClamp = (C.TileRangeClamp or 0) + 1
			if C.TileRangeClamp <= 5 then
				print(string.format(
					"[GTAV] clamped submesh %d/%d of %s: idx %d+%d past %d indices" ..
					" (decoded verts %d shared %d materials %d)",
					i, #data.submeshes, tostring(info.file),
					(sub.indexOffset or 0), (sub.indexCount or 0), nIdx,
					data.vertexCount or 0, nShared, #data.materials))
			end
		end
		if vertOffset < 0 or vertOffset > nShared then vertOffset = 0 end

		-- double sided export, drop the mirrored half
		if mflags.nocull and idxCount > 3 and idxCount % 6 == 0 then
			local half = idxCount / 2
			local paired = true
			for t = 0, half / 3 - 1 do
				local i0 = idxStart + t * 3
				local j0 = i0 + half
				local ia1, ia2, ia3 = srcIdx[i0], srcIdx[i0 + 1], srcIdx[i0 + 2]
				local ib1, ib2, ib3 = srcIdx[j0], srcIdx[j0 + 1], srcIdx[j0 + 2]
				local a1 = ia1 and shared[ia1 + vertOffset]
				local a2 = ia2 and shared[ia2 + vertOffset]
				local a3 = ia3 and shared[ia3 + vertOffset]
				local b1 = ib1 and shared[ib1 + vertOffset]
				local b2 = ib2 and shared[ib2 + vertOffset]
				local b3 = ib3 and shared[ib3 + vertOffset]
				if not (a1 and a2 and a3 and b1 and b2 and b3) then
					paired = false
					break
				end
				local p1, p2, p3 = a1.pos, a2.pos, a3.pos
				local q1, q2, q3 = b1.pos, b2.pos, b3.pos
				local forward = p1.x == q1.x and p1.y == q1.y and p1.z == q1.z
					and p2.x == q3.x and p2.y == q3.y and p2.z == q3.z
					and p3.x == q2.x and p3.y == q2.y and p3.z == q2.z
				local same = p1.x == q1.x and p1.y == q1.y and p1.z == q1.z
					and p2.x == q2.x and p2.y == q2.y and p2.z == q2.z
					and p3.x == q3.x and p3.y == q3.y and p3.z == q3.z
				if not (forward or same) then
					paired = false
					break
				end
			end
			if paired then
				idxCount = half
				C.MirrorDropped = (C.MirrorDropped or 0) + half
			end
		end

		local verts = {}
		local mnx, mny, mnz = math.huge, math.huge, math.huge
		local mxx, mxy, mxz = -math.huge, -math.huge, -math.huge
		for k = idxStart, idxStart + idxCount - 1 do
			local iv = srcIdx[k]
			local v = iv and shared[iv + vertOffset]
			if v then
				verts[#verts + 1] = v
				local p = v.pos
				if p.x < mnx then mnx = p.x end
				if p.y < mny then mny = p.y end
				if p.z < mnz then mnz = p.z end
				if p.x > mxx then mxx = p.x end
				if p.y > mxy then mxy = p.y end
				if p.z > mxz then mxz = p.z end
			end
		end

		if #verts >= 3 then
			local mesh = Mesh()
			local tb = SysTime()
			mesh:BuildFromTriangles(verts)
			meshMs = meshMs + (SysTime() - tb)
			triCount = triCount + #verts
			submeshes[#submeshes + 1] = {
				mesh = mesh, mat = mat, glass = mflags.glass,
				additive = mflags.additive or false,
				mins = Vector(mnx, mny, mnz),
				maxs = Vector(mxx, mxy, mxz),
			}
		end
	end

	local total = SysTime() - t0
	if total > 0.04 then
		print(string.format(
			"[GTAV] slow tile build %s (LOD%d): %.1fms = read %.1f + mats %.1f + mesh %.1f (%d verts -> %d shared)",
			info.file, lod, total * 1000, (tRead - t0) * 1000, matMs * 1000, meshMs * 1000,
			data.vertexCount or 0, #shared))
	end

	if #submeshes == 0 then return nil end

	return {
		key = task.key,
		tx = tx, ty = ty, lod = lod,
		origin = origin,
		center = Vector(tx * tileSize + tileSize * 0.5, ty * tileSize + tileSize * 0.5, 0),
		submeshes = submeshes,
		submeshCount = #submeshes,
		triCount = triCount,
		lastWanted = CurTime(),
	}
end

local function buildFallbackMaterial(name)
	local safeName = string.gsub(string.lower(name), "[^a-z0-9]", "_")
	local m = CreateMaterial("GTAV_Fallback_" .. safeName, "VertexLitGeneric", {
		["$basetexture"] = "models/debug/debugwhite",
	})

	local h = 0
	for i = 1, #name do
		h = (h * 31 + string.byte(name, i)) % 360
	end
	local color = HSVToColor(h, 0.45, 0.85)
	m:SetVector("$color", Vector(color.r / 255, color.g / 255, color.b / 255))
	return m
end

C.MatProbe = C.MatProbe or {}

CreateClientConVar("gtav_cl_queue_switch", "1", false, false,
	"Run the material system single-threaded (mat_queue_mode 0) only while cold materials load - removes the queued renderer's texture-creation stalls (0 = never touch mat_queue_mode)")

local QS = C.QueueSwitch or {}
C.QueueSwitch = QS
local QS_FILE = "gtav/queue_mode_restore.txt"   -- user mode, read back next session
local function qsMode()
	local cv = GetConVar("mat_queue_mode")
	return cv and cv:GetInt() or nil
end
local function qsRestore()
	if not QS.active then return end
	RunConsoleCommand("mat_queue_mode", tostring(QS.userMode or -1))
	if (QS.restores or 0) < 30 then
		print(string.format("[GTAV] queue switch restore: mat_queue_mode -> %d after %d frames",
			QS.userMode or -1, FrameNumber() - (QS.since or 0)))
	end
	QS.active = false
	QS.restores = (QS.restores or 0) + 1
	file.Delete(QS_FILE)
end
do  -- a previous session that ended while switched
	local saved = tonumber(file.Read(QS_FILE, "DATA") or "")
	if saved then
		if qsMode() == 0 and saved ~= 0 then RunConsoleCommand("mat_queue_mode", tostring(saved)) end
		file.Delete(QS_FILE)
	end
end

function C.QueueSwitchGate()
	local cv = GetConVar("gtav_cl_queue_switch")
	if QS.dead or not (cv and cv:GetBool()) then return true end
	QS.lastDemand = SysTime()
	if not QS.active then
		local m = qsMode()
		if m == nil or m == 0 then return true end      -- already single-threaded
		QS.userMode, QS.active, QS.since = m, true, FrameNumber()
		QS.switches = (QS.switches or 0) + 1
		file.CreateDir("gtav")
		file.Write(QS_FILE, tostring(m))
		RunConsoleCommand("mat_queue_mode", "0")
		if QS.switches <= 30 then
			print(string.format("[GTAV] queue switch #%d: mat_queue_mode %d -> 0", QS.switches, m))
		end
		return false
	end
	return FrameNumber() >= QS.since + 2 and qsMode() == 0
end

hook.Add("Think", "GTAV_QueueSwitch", function()
	if not QS.active then return end
	if FrameNumber() > QS.since + 30 and qsMode() ~= 0 then
		QS.dead = "mat_queue_mode did not change"
		print("[GTAV] queue switch disabled: " .. QS.dead)
		QS.active = false
		file.Delete(QS_FILE)
		return
	end
	local cv = GetConVar("gtav_cl_queue_switch")
	if not (cv and cv:GetBool()) or SysTime() - (QS.lastDemand or 0) > 0.75 then qsRestore() end
end)
hook.Add("ShutDown", "GTAV_QueueSwitch", qsRestore)

function C.MatLoadBudgetLeft()
	local fn = FrameNumber()
	if C.MatColdFrame ~= fn then
		C.MatColdFrame = fn
		C.MatColdThisFrame = 0
	end
	if not C.QueueSwitchGate() then return 0 end
	local cap = 2
	local cv = GetConVar("gtav_cl_mat_loads_per_frame")
	if cv then cap = cv:GetInt() end
	if cap <= 0 then return math.huge end
	return cap - (C.MatColdThisFrame or 0)
end

function C.GetMaterial(name)
	if type(name) ~= "string" or name == "" then
		C.BadMaterialNames = (C.BadMaterialNames or 0) + 1
		name = "___unnamed_" .. tostring(C.BadMaterialNames)
	end

	local cached = C.MaterialCache[name]
	if cached then return cached end

	if C.MatCanon == nil then C.CanonEnsure() end
	local canon = C.MatCanon[name]
	if canon and canon ~= name then
		if C.MaterialCache[canon] then
			C.MatProbe.aliasHits = (C.MatProbe.aliasHits or 0) + 1
		end
		local m = C.GetMaterial(canon)
		if m then C.MaterialCache[name] = m end
		return m
	end

	if C.SpellingCache == nil then C.SpellingEnsure() end
	local known = C.SpellingCache[name]

	local mat = nil

	local candidates
	if known then
		candidates = {known}
	else
		candidates = {name}
		if string.sub(name, 1, 7) ~= "models/" then
			candidates[#candidates + 1] = "models/gtav/" .. name
		end
	end

	local mp = C.MatProbe
	local msM, msE, msF = 0, 0, 0
	local won = nil
	for i = 1, #candidates do
		local tM = SysTime()
		local okM, res = pcall(Material, candidates[i])
		msM = msM + (SysTime() - tM) * 1000
		if okM and res then
			local tE = SysTime()
			local okE, isErr = pcall(res.IsError, res)
			msE = msE + (SysTime() - tE) * 1000
			if okE and not isErr then
				mat = res
				won = candidates[i]
				break
			end
		end
	end

	if won and won ~= name and known ~= won then
		C.SpellingCache[name] = won
		C.SpellingDirty = true
	elseif not won and known then
		C.SpellingCache[name] = nil
		C.SpellingDirty = true
	end

	if not mat then
		local tF = SysTime()
		local okF, fb = pcall(buildFallbackMaterial, name)
		msF = (SysTime() - tF) * 1000
		mp.fallbacks = (mp.fallbacks or 0) + 1
		if okF and fb then mat = fb end
	end

	if not mat then
		local okW, white = pcall(Material, "models/debug/debugwhite")
		if okW and white then mat = white end
	end

	local tot = msM + msE + msF
	do
		local fn = FrameNumber()
		if C.MatColdFrame ~= fn then
			C.MatColdFrame = fn
			C.MatColdThisFrame = 0
		end
		C.MatColdThisFrame = (C.MatColdThisFrame or 0) + 1
	end
	mp.calls = (mp.calls or 0) + 1
	mp.ms = (mp.ms or 0) + tot
	mp.msMaterial = (mp.msMaterial or 0) + msM
	mp.msIsError = (mp.msIsError or 0) + msE
	mp.msFallback = (mp.msFallback or 0) + msF
	if tot >= 100 then
		mp.slowN = (mp.slowN or 0) + 1
		if tot > (mp.slowWorst or 0) then
			mp.slowWorst = tot
			mp.slowWorstName = name
		end
	end
	C.MatLastSplit = {msM, msE, msF}

	if not mat then return nil end
	C.MaterialCache[name] = mat
	return mat
end

C.WarmQueue = C.WarmQueue or {}
C.WarmHead = C.WarmHead or 1
C.WarmSeen = C.WarmSeen or {}

function C.QueueMaterialWarm(name)
	if type(name) ~= "string" or name == "" or C.WarmSeen[name] then return end
	C.WarmSeen[name] = true
	C.WarmQueue[#C.WarmQueue + 1] = name
	local N = GTAV.Native
	if N and N.PrefetchOn then N.Prefetch(name) end
end

function C.DrainMaterialWarm(budgetMs)
	local made, t0 = 0, SysTime()
	while C.WarmHead <= #C.WarmQueue do
		if made > 0 and (SysTime() - t0) * 1000 >= budgetMs then break end
		local name = C.WarmQueue[C.WarmHead]
		local warm = C.IsMaterialWarm(name)
		if not warm then
			local left = C.MatLoadBudgetLeft()
			if C.WrapTasks and C.WrapTasks[1] then left = left - 2 end   -- hold 2 loads back for wraps
			if left <= 0 then
				break
			end
		end
		C.WarmQueue[C.WarmHead] = false
		C.WarmHead = C.WarmHead + 1
		if warm then
			C.GetMaterial(name)   -- alias of a warm canonical, free
		else
			local tN = SysTime()
			local kbBefore = collectgarbage("count")
			C.GetMaterial(name)
			local nMs = (SysTime() - tN) * 1000
			local kbAfter = collectgarbage("count")
			if nMs > (C.Perf.warmNameWorst or 0) then
				C.Perf.warmNameWorst = nMs
				C.Perf.warmNameWorstName = name
			end
			if nMs >= 100 then
				C.WarmNameSlowCount = (C.WarmNameSlowCount or 0) + 1
			end
			if nMs >= 100 and (C.WarmNameWarned or 0) < 20 then
				C.WarmNameWarned = (C.WarmNameWarned or 0) + 1
				local sp = C.MatLastSplit or {0, 0, 0}
				print(string.format(
					"[GTAV] slow material load %.1fms: %s (heap %.1f -> %.1f MB) (mat %.1f isErr %.1f fb %.1f) slow %d of %d",
					nMs, tostring(name), kbBefore / 1024, kbAfter / 1024,
					sp[1] or 0, sp[2] or 0, sp[3] or 0,
					C.WarmNameSlowCount, (C.MatProbe.slowN or 0)))
			end
			made = made + 1
		end
	end
	if C.WarmHead > #C.WarmQueue and #C.WarmQueue > 256 then
		C.WarmQueue = {}
		C.WarmHead = 1
	end
	return made
end


C.MatMan = C.MatMan or nil          -- manifest once loaded, false = absent
C.MatManLoad = C.MatManLoad or nil  -- in-progress load state
C.MatSetMemo = C.MatSetMemo or {}   -- "group@lod" -> {set ids}

-- tile_materials.dat, tab separated rows
-- GVMAT1          magic
-- N name          0 based id by order
-- T id csv        name set, repeats append
-- S sx sy lod id  set per sector
-- K key val       sector size
local function matDatLine(f)
	local line = f:ReadLine(4096)
	if not line then return nil end
	local n = #line
	while n > 0 do
		local c = string.sub(line, n, n)
		if c ~= "\r" and c ~= "\n" then break end
		n = n - 1
	end
	if n ~= #line then line = string.sub(line, 1, n) end
	if line == "" then return nil end
	return line
end

function C.MatManifestTick(budgetMs)
	if C.MatMan ~= nil then return end
	local st = C.MatManLoad
	if not st then
		local path = "lossantos/" .. tostring(GTAV.MapData.name)
			.. "/tile_materials.dat"
		local okF, f = pcall(file.Open, path, "rb", "DATA")
		if not okF or not f then
			C.MatMan = false
			return
		end
		if matDatLine(f) ~= "GVMAT1" then
			f:Close()
			C.MatMan = false
			print("[GTAV] tile_materials.dat: bad magic - texture prefetch off")
			return
		end
		st = {f = f, names = {}, sets = {}, sect = {}, sector = 8, slots = 0}
		C.MatManLoad = st
	end

	local t0 = SysTime()
	while (SysTime() - t0) * 1000 < budgetMs do
		local line = matDatLine(st.f)
		if not line then
			st.f:Close()
			C.MatMan = {
				names = st.names, sets = st.sets, sect = st.sect,
				sector = st.sector, setState = {},
			}
			C.MatManLoad = nil
			C.MatSetMemo = {}
			print(string.format(
				"[GTAV] tile_materials.dat: %d names, %d sets, %d slots (prefetch on)",
				#st.names, #st.sets, st.slots))
			return
		end
		local kind = string.sub(line, 1, 1)
		if kind == "N" then
			st.names[#st.names + 1] = string.sub(line, 3)
		elseif kind == "T" then
			local id, csv = string.match(line, "^T\t(%d+)\t(.*)$")
			if id then
				id = tonumber(id) + 1
				local prev = st.sets[id]
				st.sets[id] = prev and (prev .. "," .. csv) or csv
			end
		elseif kind == "S" then
			local sx, sy, lod, sid = string.match(line,
				"^S\t(%-?%d+)\t(%-?%d+)\t(%d+)\t(%d+)$")
			if sx then
				local ck = sx .. " " .. sy
				local slots = st.sect[ck]
				if not slots then
					slots = {}
					st.sect[ck] = slots
				end
				slots[tonumber(lod)] = tonumber(sid) + 1
				st.slots = st.slots + 1
			end
		elseif kind == "K" then
			local key, val = string.match(line, "^K\t([^\t]+)\t(.*)$")
			if key == "sector" then st.sector = tonumber(val) or 8 end
		end
	end
end

function C.GroupMatSets(key, tx, ty, span, lod)
	local ck = key .. "@" .. lod
	local memo = C.MatSetMemo[ck]
	if memo ~= nil then return memo end
	local man = C.MatMan
	if not man then return nil end
	local out, seen = {}, {}
	local sec = man.sector
	local sx0 = math.floor(tx / sec)
	local sx1 = math.floor((tx + span - 1) / sec)
	local sy0 = math.floor(ty / sec)
	local sy1 = math.floor((ty + span - 1) / sec)
	for sy = sy0, sy1 do
		for sx = sx0, sx1 do
			local slots = man.sect[sx .. " " .. sy]
			local sid = slots and slots[lod]
			if sid and not seen[sid] then
				seen[sid] = true
				out[#out + 1] = sid
			end
		end
	end
	C.MatSetMemo[ck] = out
	return out
end

local function matSetState(man, sid)
	local st = man.setState[sid]
	if st then return st end
	local csv = man.sets[sid]
	if not csv then return nil end
	local names = {}
	for num in string.gmatch(csv, "%d+") do
		local n = man.names[tonumber(num) + 1]
		if n then names[#names + 1] = n end
	end
	st = {names = names, head = 1}
	man.setState[sid] = st
	return st
end

function C.RefreshMatPrefetch()
	local cv = GetConVar("gtav_cl_mat_prefetch")
	if cv and cv:GetBool() and C.MatMan == nil then
		C.MatManifestTick(2)
	end
	if not (cv and cv:GetBool()) or not C.MatMan then
		C.PrefetchItems = nil
		C.PrefetchLeft = 0
		return
	end
	local maxGroups = 4
	local cvN = GetConVar("gtav_cl_mat_prefetch_groups")
	if cvN then maxGroups = cvN:GetInt() end
	if maxGroups < 1 then
		C.PrefetchItems = nil
		C.PrefetchLeft = 0
		return
	end
	local q = C.BuildQueue
	local items = {}
	local n = 0
	if q then
		for i = C.BuildHead or 1, #q do
			if n >= maxGroups then break end
			n = n + 1
			items[n] = q[i]
		end
	end
	C.PrefetchItems = items
	C.PrefetchLeft = n
	C.FilePrefetchAhead(q)

	local now = SysTime()
	if now >= (C.MatStatsLogAt or 0) then
		C.MatStatsLogAt = now + 15
		local mp = C.MatProbe or {}
		print(string.format(
			"[GTAV] material loads: cold %d (%.0f ms total, %d slow >=100ms) alias hits %d",
			mp.calls or 0, mp.ms or 0, mp.slowN or 0, mp.aliasHits or 0))
	end
end

local FILE_PREFETCH_GROUPS = 32     -- groups ahead
local FILE_PREFETCH_NEW_SETS = 16   -- new sets per refresh
function C.FilePrefetchAhead(q)
	local N = GTAV.Native
	local man = C.MatMan
	if not (N and N.PrefetchOn and man and q) then return end
	local newSets = 0
	local seen = 0
	for i = C.BuildHead or 1, #q do
		if seen >= FILE_PREFETCH_GROUPS or newSets >= FILE_PREFETCH_NEW_SETS then break end
		local e = q[i]
		seen = seen + 1
		local sets = e and C.GroupMatSets(e.key, e.tx, e.ty, e.span, e.lod)
		if sets then
			for s = 1, #sets do
				local st = matSetState(man, sets[s])
				if st and not st.fileSent then
					st.fileSent = true
					N.Prefetch(st.names)
					newSets = newSets + 1
					if newSets >= FILE_PREFETCH_NEW_SETS then break end
				end
			end
		end
	end
	local now = SysTime()
	if now >= (C.FilePrefetchLogAt or 0) then
		C.FilePrefetchLogAt = now + 15
		local s = N.PrefetchStats()
		if s then
			print(string.format(
				"[GTAV] file prefetch: queued %d done %d files %d missing %d dropped %d pending %d (%.0f ms worker)",
				s.queued or 0, s.done or 0, s.files or 0, s.missing or 0,
				s.dropped or 0, s.pending or 0, s.ms or 0))
		end
	end
end

function C.DrainMatPrefetch(maxMs)
	local items = C.PrefetchItems
	if not items or #items == 0 or maxMs <= 0 then return 0 end
	local man = C.MatMan
	if not man then return 0 end

	local made, t0 = 0, SysTime()
	for i = 1, #items do
		local e = items[i]
		local sets = C.GroupMatSets(e.key, e.tx, e.ty, e.span, e.lod)
		local pending = false
		if sets then
			for s = 1, #sets do
				local st = matSetState(man, sets[s])
				if st then
					local list = st.names
					while st.head <= #list do
						local name = list[st.head]
						if C.IsMaterialWarm(name) then
							st.head = st.head + 1
						else
							local left = C.MatLoadBudgetLeft()
							if C.WrapTasks and C.WrapTasks[1] then left = left - 2 end
							if left <= 0
								or (SysTime() - t0) * 1000 >= maxMs then
								pending = true
								break
							end
							local tN = SysTime()
							C.GetMaterial(name)
							local nMs = (SysTime() - tN) * 1000
							if nMs > (C.Perf.warmNameWorst or 0) then
								C.Perf.warmNameWorst = nMs
								C.Perf.warmNameWorstName = name
							end
							if nMs >= 100 then
								C.WarmNameSlowCount = (C.WarmNameSlowCount or 0) + 1
								if (C.WarmNameWarned or 0) < 20 then
									C.WarmNameWarned = (C.WarmNameWarned or 0) + 1
									print(string.format(
										"[GTAV] slow material load %.1fms (prefetch): %s slow %d of %d",
										nMs, tostring(name), C.WarmNameSlowCount,
										(C.MatProbe.slowN or 0)))
								end
							end
							made = made + 1
							st.head = st.head + 1
						end
					end
					if st.head <= #list then pending = true end
				end
				if pending then break end
			end
		end
		if pending then break end
		C.PrefetchLeft = (C.PrefetchLeft or 1) - 1
	end
	return made
end

function C.SpellingFlush()
	if not C.SpellingDirty then return end
	C.SpellingDirty = false
	local map = tostring(GTAV.MapData.name or game.GetMap())
	local tab = {schema = 1, resolved = C.SpellingCache or {}}
	local okJ, txt = pcall(util.TableToJSON, tab)
	if not okJ or not txt then return end
	pcall(file.CreateDir, "lossantos/" .. map)
	pcall(file.Write, "lossantos/" .. map .. "/mat_spelling.json", txt)
end

local function spellingLoadImpl()
	if C.SpellingCache ~= nil then return end
	C.SpellingCache = {}
	local path = "lossantos/" .. tostring(GTAV.MapData.name or game.GetMap())
		.. "/mat_spelling.json"
	local ok, txt = pcall(file.Read, path, "DATA")
	if ok and type(txt) == "string" and txt ~= "" then
		local okJ, tab = pcall(util.JSONToTable, txt)
		if okJ and type(tab) == "table" and type(tab.resolved) == "table" then
			C.SpellingCache = tab.resolved
		end
	end
end

C.SpellingEnsure = spellingLoadImpl

CreateClientConVar("gtav_cl_mat_canon", "1", false, false,
	"Share one IMaterial between byte-identical duplicate materials (mat_canon.txt; 0 = off)")
cvars.AddChangeCallback("gtav_cl_mat_canon", function()
	C.MatCanon = nil
end, "GTAV_MatCanon")

function C.CanonEnsure()
	if C.MatCanon ~= nil then return end
	C.MatCanon = {}
	local cv = GetConVar("gtav_cl_mat_canon")
	if cv and not cv:GetBool() then return end
	local path = "lossantos/" .. tostring(GTAV.MapData.name or game.GetMap())
		.. "/mat_canon.txt"
	local ok, txt = pcall(file.Read, path, "DATA")
	if not ok or type(txt) ~= "string" or txt == "" then return end
	local n = 0
	for alias, canon in string.gmatch(txt, "([^\t\n]+)\t([^\n]+)") do
		C.MatCanon[alias] = canon
		n = n + 1
	end
	print(string.format("[GTAV] mat_canon.txt: %d duplicate materials share a canonical IMaterial", n))
end

function C.IsMaterialWarm(name)
	if C.MaterialCache[name] then return true end
	if C.MatCanon == nil then C.CanonEnsure() end
	local c = C.MatCanon[name]
	return c ~= nil and C.MaterialCache[c] ~= nil
end

function C.GcPump(budgetMs)
	if budgetMs <= 0 then
		if C.GcStopped then
			collectgarbage("restart")
			C.GcStopped = false
		end
		return 0
	end
	if not C.GcStopped then
		C.GcPostKB = nil
	end
	collectgarbage("stop")   -- every call, engine can restart it
	C.GcStopped = true

	local kb = collectgarbage("count")
	local GC_TRIGGER_KB = 4096
	local post = C.GcPostKB
	if post and kb <= post + GC_TRIGGER_KB then
		C.Perf.gcSteps = 0
		return 0
	end

	local t0 = SysTime()
	local steps = 0
	while (SysTime() - t0) * 1000 < budgetMs do
		local tS = SysTime()
		local done = collectgarbage("step", 16)
		local sMs = (SysTime() - tS) * 1000
		steps = steps + 1
		if sMs > (C.Perf.gcStepWorstMs or 0) then C.Perf.gcStepWorstMs = sMs end
		if done then
			C.GcPostKB = collectgarbage("count")
			break
		end
	end
	C.Perf.gcSteps = steps
	return (SysTime() - t0) * 1000
end

local function readVmtText(name)
	local candidates = {name}
	if string.sub(name, 1, 7) ~= "models/" then
		candidates[#candidates + 1] = "models/gtav/" .. name
	end
	for i = 1, #candidates do
		local ok, text = pcall(file.Read,
							   "materials/" .. candidates[i] .. ".vmt", "GAME")
		if ok and type(text) == "string" and text ~= "" then
			return string.lower(text)
		end
	end
	return nil
end

local function vmtFlag(text, key)
	if not text or text == "" then return false end
	local pat = "[%$%%%-]" .. key .. "[^%w][\"%s]*%s*\"?([%w%+%.%-]*)"
	local pos = 1
	while true do
		local s, e, val = string.find(text, pat, pos)
		if not s then return false end
		if val ~= "" and val ~= "0" then return true end
		pos = e + 1
	end
end

-- magic line, then key tab a [tab b]
local function readSidecarDat(path, magic)
	local f = file.Open(path, "rb", "DATA")
	if not f then return nil end
	local first = matDatLine(f)
	if first ~= magic then
		f:Close()
		return nil
	end
	local tab = {}
	while true do
		local line = matDatLine(f)
		if not line then break end
		local a = string.find(line, "\t", 1, true)
		if a then
			local key = string.sub(line, 1, a - 1)
			local b = string.find(line, "\t", a + 1, true)
			if b then
				tab[key] = {tonumber(string.sub(line, a + 1, b - 1)), tonumber(string.sub(line, b + 1))}
			else
				tab[key] = {tonumber(string.sub(line, a + 1))}
			end
		end
	end
	f:Close()
	return tab
end

local function flagsSidecar()
	if C.FlagsSidecar ~= nil then return C.FlagsSidecar end
	local base = "lossantos/" .. tostring(game.GetMap())
	local tab = nil
	local dat = readSidecarDat(base .. "/material_flags.dat", "GVMF1")
	if dat then
		tab = {}
		for name, v in pairs(dat) do tab[name] = v[1] end
	else
		local ok, txt = pcall(file.Read, base .. "/material_flags.json", "DATA")
		if ok and type(txt) == "string" and txt ~= "" then
			local okJ, parsed = pcall(util.JSONToTable, txt)
			if okJ and type(parsed) == "table" then tab = parsed end
		end
	end
	C.FlagsSidecar = tab or false -- false = absent, never re-read
	return C.FlagsSidecar
end

function C.TileZBounds()
	if C.TileBounds ~= nil then return C.TileBounds end
	local base = "lossantos/" .. tostring(game.GetMap())
	local tab = readSidecarDat(base .. "/tile_bounds.dat", "GVTB1")
	local src = "tile_bounds.dat"
	if not tab then
		src = "tile_bounds.json"
		local ok, txt = pcall(file.Read, base .. "/tile_bounds.json", "DATA")
		if ok and type(txt) == "string" and txt ~= "" then
			local okJ, parsed = pcall(util.JSONToTable, txt)
			if okJ and type(parsed) == "table" then tab = parsed.bounds or parsed end
		end
	end
	C.TileBounds = tab or false -- false = absent, never re-read
	local n = 0
	if tab then for _ in pairs(tab) do n = n + 1 end end
	print(string.format("[GTAV] %s: %s (%d entries)", src, tab and "loaded" or "ABSENT/unparsed", n))
	return C.TileBounds
end

function C.GetMaterialFlags(name)
	local flags = C.MaterialFlags[name]
	if flags then return flags end

	do
		local side = flagsSidecar()
		if side then
			local tab = side.flags or side
			local bits = tab[name]
			if bits == nil then bits = tab[string.lower(name)] end
			if bits ~= nil then
				bits = tonumber(bits) or 0
				flags = {
					glass = (bits % 2) >= 1,      -- bit 0
					nocull = (bits % 4) >= 2,     -- bit 1
					additive = (bits % 8) >= 4,   -- bit 2
				}
				C.MaterialFlags[name] = flags
				C.QueueMaterialWarm(name)
				return flags
			end
		end
	end

	-- no sidecar entry, guess from the name
	local lower = string.lower(name)
	local glass = string.find(lower, "glass", 1, true) ~= nil
		or string.find(lower, "window", 1, true) ~= nil
		or string.find(lower, "shop", 1, true) ~= nil
	local nocull = glass
		or string.find(lower, "railing", 1, true) ~= nil
		or string.find(lower, "fence", 1, true) ~= nil
		or string.find(lower, "branch", 1, true) ~= nil
		or string.find(lower, "leaf", 1, true) ~= nil
		or string.find(lower, "hedge", 1, true) ~= nil
		or string.find(lower, "ivy", 1, true) ~= nil

	local text = readVmtText(lower)
	if vmtFlag(text, "nocull") then nocull = true end
	if vmtFlag(text, "translucent") then glass = true end
	local additive = vmtFlag(text, "additive")

	flags = {glass = glass, nocull = nocull, additive = additive}
	C.MaterialFlags[name] = flags
	C.QueueMaterialWarm(name)
	return flags
end


local renderMatrix = Matrix()

local viewPlanes = nil       -- outward normals, nil = cull nothing
local viewPlaneN = nil
local viewPlaneCount = 0
local viewEyeX, viewEyeY, viewEyeZ = 0, 0, 0
local viewEyePrev = nil      -- last frame's eye, for the teleport guard
local Z_FALLBACK = 128000    -- box Z half-extent when a tile has no Z bounds
local viewFwdX, viewFwdY, viewFwdZ = 0, 0, 1  -- view axis for the fog/far caps
local viewFogEnd = nil       -- fog saturation distance, nil = no fog cap
local viewFarZ = nil         -- engine far plane, nil = no far cap

local SUB_TEST_FOG = 1        -- the tile box straddles the fog end
local SUB_TEST_FRUSTUM = 2    -- the tile box straddles a frustum plane

local function tileInView(tile, tileSize)
	if not viewPlanes then return true end
	local fp = footprint(tile, tileSize)
	local x0 = tile.tx * tileSize
	local y0 = tile.ty * tileSize
	local x1, y1 = x0 + fp, y0 + fp
	local z0, z1 = tile.z0, tile.z1
	if not z0 then
		z0, z1 = -Z_FALLBACK, Z_FALLBACK
	elseif not z1 then
		z1 = Z_FALLBACK
	end
	local ex, ey, ez = viewEyeX, viewEyeY, viewEyeZ
	for p = 1, #viewPlanes do
		local n = viewPlanes[p]
		local mnx = n.x >= 0 and x0 or x1
		local mny = n.y >= 0 and y0 or y1
		local mnz = n.z >= 0 and z0 or z1
		if n.x * (mnx - ex) + n.y * (mny - ey) + n.z * (mnz - ez) > 0 then
			return false
		end
	end
	return true
end

local function tileBeyondViewZ(tile, tileSize, limit)
	local fp = footprint(tile, tileSize)
	local x0 = tile.tx * tileSize
	local y0 = tile.ty * tileSize
	local x1, y1 = x0 + fp, y0 + fp
	local z0, z1 = tile.z0, tile.z1
	if not z0 then
		z0, z1 = -Z_FALLBACK, Z_FALLBACK
	elseif not z1 then
		z1 = Z_FALLBACK
	end
	local dx = viewFwdX >= 0 and (x0 - viewEyeX) or (x1 - viewEyeX)
	local dy = viewFwdY >= 0 and (y0 - viewEyeY) or (y1 - viewEyeY)
	local dz = viewFwdZ >= 0 and (z0 - viewEyeZ) or (z1 - viewEyeZ)
	return viewFwdX * dx + viewFwdY * dy + viewFwdZ * dz > limit
end

local function tileBeyondFog(tile, tileSize)
	local fogEnd = viewFogEnd
	if not fogEnd then return false end
	return tileBeyondViewZ(tile, tileSize, fogEnd)
end

local function tileBeyondFar(tile, tileSize)
	if C.Sky3dPass then return false end   -- 1/16 scale camera, different zfar
	local farz = viewFarZ
	if not farz then return false end
	return tileBeyondViewZ(tile, tileSize, farz)
end

-- 0 draw, 1 fogged, 2 outside frustum
local function subBoxCull(sub, ox, oy, mask)
	local mn, mx = sub.mins, sub.maxs
	if not mn or not mx then return 0 end
	local x0 = mn.x + ox
	local y0 = mn.y + oy
	local z0 = mn.z
	local x1 = mx.x + ox
	local y1 = mx.y + oy
	local z1 = mx.z
	local ex, ey, ez = viewEyeX, viewEyeY, viewEyeZ

	local fogEnd = viewFogEnd
	if mask % 2 == SUB_TEST_FOG and fogEnd then
		local dx = viewFwdX >= 0 and (x0 - ex) or (x1 - ex)
		local dy = viewFwdY >= 0 and (y0 - ey) or (y1 - ey)
		local dz = viewFwdZ >= 0 and (z0 - ez) or (z1 - ez)
		if viewFwdX * dx + viewFwdY * dy + viewFwdZ * dz > fogEnd then
			return 1
		end
	end

	if mask >= SUB_TEST_FRUSTUM then
		local pn, np = viewPlaneN, viewPlaneCount
		if not pn then return 0 end
		for p = 1, np do
			local i3 = p * 3
			local nx, ny, nz = pn[i3 - 2], pn[i3 - 1], pn[i3]
			local mnx = nx >= 0 and x0 or x1
			local mny = ny >= 0 and y0 or y1
			local mnz = nz >= 0 and z0 or z1
			if nx * (mnx - ex) + ny * (mny - ey) + nz * (mnz - ez) > 0 then
				return 2
			end
		end
	end
	return 0
end

local function tileSubTestMask(tile, tileSize)
	local mask = 0
	local fp = footprint(tile, tileSize)
	local x0 = tile.tx * tileSize
	local y0 = tile.ty * tileSize
	local x1, y1 = x0 + fp, y0 + fp
	local z0, z1 = tile.z0, tile.z1
	if not z0 then
		z0, z1 = -Z_FALLBACK, Z_FALLBACK
	elseif not z1 then
		z1 = Z_FALLBACK
	end
	local ex, ey, ez = viewEyeX, viewEyeY, viewEyeZ

	local planes = viewPlanes
	if planes then
		for p = 1, #planes do
			local n = planes[p]
			local mxx = n.x >= 0 and x1 or x0
			local mxy = n.y >= 0 and y1 or y0
			local mxz = n.z >= 0 and z1 or z0
			if n.x * (mxx - ex) + n.y * (mxy - ey) + n.z * (mxz - ez) > 0 then
				mask = mask + SUB_TEST_FRUSTUM
				break
			end
		end
	end

	local fogEnd = viewFogEnd
	if fogEnd then
		local fx = viewFwdX >= 0 and (x1 - ex) or (x0 - ex)
		local fy = viewFwdY >= 0 and (y1 - ey) or (y0 - ey)
		local fz = viewFwdZ >= 0 and (z1 - ez) or (z0 - ez)
		if viewFwdX * fx + viewFwdY * fy + viewFwdZ * fz > fogEnd then
			mask = mask + SUB_TEST_FOG
		end
	end

	return mask
end

local cvRfarz = nil
local farzPrinted = false
local function farPlaneZ()
	if C.ActiveZFar then
		if farzPrinted ~= "active" then
			farzPrinted = "active"
			print(string.format("[GTAV] far plane: %d (engine default %s) -> tile far cull at %d",
				C.ActiveZFar, tostring(C.EngineZFar), C.ActiveZFar))
		end
		return C.ActiveZFar
	end
	if cvRfarz == nil then cvRfarz = GetConVar("r_farz") end
	local v = cvRfarz and cvRfarz:GetFloat() or 0
	local eng = C.EngineZFar
	local fallback = (eng and eng > 0) and eng or 30000
	if farzPrinted ~= "engine" then
		farzPrinted = "engine"
		print(string.format("[GTAV] far plane: r_farz=%s engine=%s -> tile far cull at %d",
			tostring(v), tostring(eng), v > 0 and v or fallback))
	end
	if v > 0 then return v end
	return fallback
end

local function refreshViewCone()
	viewPlanes = nil
	viewPlaneN = nil
	viewPlaneCount = 0
	viewFogEnd = nil
	viewFarZ = nil
	local ang = EyeAngles()
	if not ang then return end
	local eye = C.LastEyePos
	if not eye then return end
	-- teleport, cull nothing this pass
	if viewEyePrev and (eye - viewEyePrev):LengthSqr() > 131072 * 131072 then
		viewEyePrev = eye
		return
	end
	viewEyePrev = eye
	viewEyeX, viewEyeY, viewEyeZ = eye.x, eye.y, eye.z

	local fw = ang:Forward()
	viewFwdX, viewFwdY, viewFwdZ = fw.x, fw.y, fw.z

	local cvFarCull = GetConVar("gtav_cl_far_cull")
	if not cvFarCull or cvFarCull:GetBool() then
		viewFarZ = farPlaneZ()
	end

	local _, fogEnd = fogDistances()
	local cvFogCull = GetConVar("gtav_cl_fog_cull")
	if fogEnd and (not cvFogCull or cvFogCull:GetBool()) then
		viewFogEnd = fogEnd
	end

	local cv = GetConVar("gtav_cl_frustum_cull")
	if cv and not cv:GetBool() then return end

	local fov = 90
	local ply = LocalPlayer()
	if IsValid(ply) then fov = ply:GetFOV() end
	if not fov or fov <= 0 then
		local fcv = GetConVar("fov_desired")
		fov = fcv and fcv:GetFloat() or 90
	end
	if fov < 45 then fov = 45 elseif fov > 120 then fov = 120 end
	local aspect = ScrW() / math.max(1, ScrH())
	local tanHalf = math.tan(math.rad(fov) * 0.5)
	-- fov is plain or 4:3 hor+, take the wider
	local hHalf = math.atan(math.max(tanHalf, tanHalf * 0.75 * aspect))
	local vHalf = math.atan(math.max(tanHalf / aspect, tanHalf * 0.75))
	local th = math.tan(hHalf + 0.035)   -- ~2 deg margin
	local tv = math.tan(vHalf + 0.05)    -- ~2.9 deg margin

	local f, r, u = ang:Forward(), ang:Right(), ang:Up()

	local planes = {}
	local pn = {}
	local function addPlane(n)
		if n.x * f.x + n.y * f.y + n.z * f.z > 0 then
			n = Vector(-n.x, -n.y, -n.z)
		end
		planes[#planes + 1] = n
		local k = #planes * 3
		pn[k - 2], pn[k - 1], pn[k] = n.x, n.y, n.z
	end
	addPlane(u:Cross(f + r * th))   -- right
	addPlane(u:Cross(f - r * th))   -- left
	addPlane(r:Cross(f + u * tv))   -- top
	addPlane(r:Cross(f - u * tv))   -- bottom
	viewPlanes = planes
	viewPlaneN = pn
	viewPlaneCount = #planes
end

local function tileCullDistSqr(tile, tileSize)
	local lodD = GTAV.Config.LOD_DISTANCES
	local lod = tonumber(tile.lod) or 0
	local maxDist = (lodD[math.min(lod + 1, 4)] or GTAV.Config.ZFAR)
		+ footprint(tile, tileSize)
	return maxDist * maxDist
end

local drawWorldPass, flushDrawStats -- defined below, called from RenderWorld

function C.RenderWorld(depth, skybox, sky3d)
	if depth then
		C.Stats.renderSkip = "depth"
		return
	end
	if sky3d and not GetConVar("gtav_skybox_world"):GetBool() then
		C.Stats.renderSkip = "sky3d"
		return
	end
	if not GTAV.MapData.current then
		C.Stats.renderSkip = "nomap"
		return
	end

	C.Stats.renderSkip = nil
	C.Stats.renderCalls = C.Stats.renderCalls + 1
	C.Sky3dPass = (sky3d == true)

	drawSkyDome()

	local myCell = C.RenderCell or C.LastEyeCell or Vector(0, 0, 0)
	local eyeAbs = C.LastEyePos or Vector(0, 0, 0)
	local tileSize = GTAV.Config.VISUAL_TILE_SIZE

	local startTime = SysTime()

	if C.DrawOrderDirty or not C.DrawOrder
		or (#C.DrawOrder == 0 and next(C.ActiveTiles) ~= nil) then
		local okR, errR = pcall(C.RefreshDrawOrder)
		if not okR then
			C.RenderError = "draworder: " .. tostring(errR)
			ErrorNoHalt("[GTAV] draw order rebuild failed: " .. tostring(errR) .. "\n")
			C.DrawOrderDirty = false
		end
	end
	refreshViewCone()

	local ds = C.DrawStats
	ds.order, ds.subs, ds.drawn = 0, 0, 0
	ds.culled, ds.farCulled, ds.behind, ds.fogged, ds.bad, ds.glass = 0, 0, 0, 0, 0, 0
	ds.subBehind, ds.subFogged, ds.glassCulled = 0, 0, 0
	ds.additive, ds.additiveCulled = 0, 0

	local ok, err = pcall(drawWorldPass, myCell, eyeAbs, tileSize, ds)
	flushDrawStats(startTime, ok, err)
end

function drawWorldPass(myCell, eyeAbs, tileSize, ds)
	local drawOn = true
	local cvDraw = GetConVar("gtav_cl_draw_enable")
	if cvDraw then drawOn = cvDraw:GetBool() end

	-- vertexalpha materials write no depth
	local cvDepth = GetConVar("gtav_cl_force_depthwrite")
	if not cvDepth or cvDepth:GetBool() then
		render.OverrideDepthEnable(true, true)
		C.DepthOverridden = true
	end

	local order = C.DrawOrder or {}
	ds.order = #order
	for i = 1, #order do
		local tile = order[i]
		local subs = tile and tile.submeshes
		if not subs or not tile.origin then
			ds.bad = ds.bad + 1
		elseif tileNearDistSqr(eyeAbs, tile.tx, tile.ty, tileSize,
			footprint(tile, tileSize), tile.z0, tile.z1)
			> tileCullDistSqr(tile, tileSize) then
			ds.culled = ds.culled + 1
		elseif tileBeyondFar(tile, tileSize) then
			ds.farCulled = ds.farCulled + 1
		elseif not tileInView(tile, tileSize) then
			ds.behind = ds.behind + 1
		elseif tileBeyondFog(tile, tileSize) then
			ds.fogged = ds.fogged + 1
		else
			local mask = tileSubTestMask(tile, tileSize)
			renderMatrix:SetTranslation(tile.origin - myCell)
			cam.PushModelMatrix(renderMatrix)
			C.MatrixPushed = true

			local ox, oy = tile.origin.x, tile.origin.y
			for s = 1, #subs do
				local sub = subs[s]
				ds.subs = ds.subs + 1
				if sub.additive then   -- additive wins over glass
					ds.additive = ds.additive + 1
				elseif sub.glass then
					ds.glass = ds.glass + 1
				elseif not sub.mat or not sub.mesh then
					ds.bad = ds.bad + 1
				else
					local cull = 0
					if mask ~= 0 then
						cull = subBoxCull(sub, ox, oy, mask)
					end
					if cull == 1 then
						ds.subFogged = ds.subFogged + 1
					elseif cull == 2 then
						ds.subBehind = ds.subBehind + 1
					else
						if drawOn then
							render.SetMaterial(sub.mat)
							sub.mesh:Draw()
						end
						ds.drawn = ds.drawn + 1
					end
				end
			end

			cam.PopModelMatrix()
			C.MatrixPushed = false
		end
	end
end

function flushDrawStats(startTime, ok, err)
	if C.MatrixPushed then
		pcall(cam.PopModelMatrix)
		C.MatrixPushed = false
	end
	if C.DepthOverridden then
		render.OverrideDepthEnable(false, false)
		C.DepthOverridden = false
	end

	local ds = C.DrawStats
	C.Stats.tilesDrawn = ds.drawn or 0
	C.Stats.tilesFarCulled = ds.farCulled or 0
	C.Stats.renderTime = SysTime() - startTime
	C.Stats.drawOrder = ds.order or 0

	if not ok then
		C.RenderError = tostring(err)
		ErrorNoHalt("[GTAV] render pass failed: " .. tostring(err) .. "\n")
	end

	if C.Perf.watchUntil > 0 and C.Stats.renderTime * 1000 > (C.Perf.maxRenderMs or 0) then
		C.Perf.maxRenderMs = C.Stats.renderTime * 1000
	end
end

function C.RenderGlass(depth, skybox, sky3d)
	if depth or sky3d then return end
	if not GTAV.MapData.current then return end

	local myCell = C.RenderCell or C.LastEyeCell or Vector(0, 0, 0)
	local eyeAbs = C.LastEyePos or Vector(0, 0, 0)
	local tileSize = GTAV.Config.VISUAL_TILE_SIZE

	if C.DrawOrderDirty or not C.GlassOrder or not C.AdditiveOrder then
		C.RefreshDrawOrder()
	end
	refreshViewCone()

	local drawOn = true
	local cvDraw = GetConVar("gtav_cl_draw_enable")
	if cvDraw then drawOn = cvDraw:GetBool() end

	local cvGlass = GetConVar("gtav_cl_glass_enable")
	if not cvGlass or cvGlass:GetBool() then
		local t0 = SysTime()
		local order = C.GlassOrder or {}
		local waterMat = GTAV.Water and GTAV.Water.Material()
		for i = 1, #order do
			local tile = order[i]
			if tileNearDistSqr(eyeAbs, tile.tx, tile.ty, tileSize,
				footprint(tile, tileSize), tile.z0, tile.z1) <= tileCullDistSqr(tile, tileSize)
				and not tileBeyondFar(tile, tileSize)
				and tileInView(tile, tileSize)
				and not tileBeyondFog(tile, tileSize) then
				local translation = tile.origin - myCell
				renderMatrix:SetTranslation(translation)
				cam.PushModelMatrix(renderMatrix)
				C.MatrixPushed = true
				local subs = tile.submeshes
				local ds = C.DrawStats
				local ox, oy = tile.origin.x, tile.origin.y
				local mask = tileSubTestMask(tile, tileSize)
				for s = 1, #subs do
					local sub = subs[s]
					if sub.glass and not sub.additive and sub.mat and sub.mesh then
						if mask ~= 0 and subBoxCull(sub, ox, oy, mask) ~= 0 then
							ds.glassCulled = (ds.glassCulled or 0) + 1
						elseif drawOn then
							local wmat = waterMat and GTAV.Water.IsWater(sub) and waterMat
							if wmat then
								GTAV.Water.Prepare()
								render.SetMaterial(wmat)
							else
								render.SetMaterial(sub.mat)
							end
							sub.mesh:Draw()
						end
					end
				end
				cam.PopModelMatrix()
				C.MatrixPushed = false
			end
		end

		local dtMs = (SysTime() - t0) * 1000
		C.GlassMsEma = (C.GlassMsEma or 0) * 0.9 + dtMs * 0.1
		C.Stats.glassMs = C.GlassMsEma
	end

	local cvAdd = GetConVar("gtav_cl_additive_enable")
	if not cvAdd or cvAdd:GetBool() then
		local t0 = SysTime()
		local order = C.AdditiveOrder or {}
		if #order > 0 then
			render.OverrideDepthEnable(true, false)
			for i = 1, #order do
				local tile = order[i]
				if tileNearDistSqr(eyeAbs, tile.tx, tile.ty, tileSize,
					footprint(tile, tileSize), tile.z0, tile.z1) <= tileCullDistSqr(tile, tileSize)
					and not tileBeyondFar(tile, tileSize)
					and tileInView(tile, tileSize)
					and not tileBeyondFog(tile, tileSize) then
					local translation = tile.origin - myCell
					renderMatrix:SetTranslation(translation)
					cam.PushModelMatrix(renderMatrix)
					C.MatrixPushed = true
					local subs = tile.submeshes
					local ds = C.DrawStats
					local ox, oy = tile.origin.x, tile.origin.y
					local mask = tileSubTestMask(tile, tileSize)
					for s = 1, #subs do
						local sub = subs[s]
						if sub.additive and sub.mat and sub.mesh then
							if mask ~= 0 and subBoxCull(sub, ox, oy, mask) ~= 0 then
								ds.additiveCulled = (ds.additiveCulled or 0) + 1
							elseif drawOn then
								render.SetMaterial(sub.mat)
								sub.mesh:Draw()
							end
						end
					end
					cam.PopModelMatrix()
					C.MatrixPushed = false
				end
			end
			render.OverrideDepthEnable(false, false)
		end
		local dtMs = (SysTime() - t0) * 1000
		C.AdditiveMsEma = (C.AdditiveMsEma or 0) * 0.9 + dtMs * 0.1
		C.Stats.additiveMs = C.AdditiveMsEma
	end
end


local groundBuildWarned = false

function C.SyncGroundPhys(ent)
	local phys = ent:GetPhysicsObject()
	if not IsValid(phys) then return false end

	local target = ent:TargetPos()
	local moved = false
	if phys:GetPos():DistToSqr(target) > 0.25 then
		phys:SetPos(target)
		moved = true
	end
	ent.gtavPhysPos = target
	return moved
end

function C.BuildGround()
	if not GTAV.MapData.current then return end

	local started = SysTime()
	C.LastGroundCount = 0
	C.BuiltGroundCount = 0
	local built = 0
	for _, ent in ipairs(ents.FindByClass("gtav_ground")) do
		C.LastGroundCount = C.LastGroundCount + 1

		if ent.gtavBuilt then
			C.BuiltGroundCount = C.BuiltGroundCount + 1
			C.SyncGroundPhys(ent)
		elseif built == 0 then
			if not ent.Build then
				if not groundBuildWarned then
					groundBuildWarned = true
					ErrorNoHalt("[GTAV] gtav_ground entities have no Build method - the class is registered but not bound through baseclass (see sh_ground.lua RegisterGround)\n")
				end
				break
			elseif ent:Build() then
				built = 1
			end
		end
	end
	C.Perf.builtGround = built
	C.Perf.groundBuildMs = (SysTime() - started) * 1000
	if C.Perf.groundBuildMs > (C.Perf.worstGround or 0) then
		C.Perf.worstGround = C.Perf.groundBuildMs
	end
end

-- before prediction or you drop after a cell jump
hook.Add("CreateMove", "GTAV_GroundPhys", function()
	if not GTAV.MapData.current then return end

	local started = SysTime()
	local moved = 0
	for _, ent in ipairs(ents.FindByClass("gtav_ground")) do
		if ent.gtavBuilt and C.SyncGroundPhys(ent) then moved = moved + 1 end
	end
	C.Perf.groundMs = SysTime() - started
	C.Perf.groundMoves = moved
	if C.Perf.groundMs * 1000 > (C.Perf.worstGround or 0) then
		C.Perf.worstGround = C.Perf.groundMs * 1000
	end
	if C.Perf.watchUntil > 0 then
		if C.Perf.groundMs * 1000 > (C.Perf.maxGroundMs or 0) then
			C.Perf.maxGroundMs = C.Perf.groundMs * 1000
		end
		if moved > (C.Perf.maxGroundMoves or 0) then
			C.Perf.maxGroundMoves = moved
		end
	end
end)

local function snapshot()
	local perf = C.Perf
	return {
		frameMs = perf.worstMs or 0,
		buildMs = perf.winBuildMs or 0,
		built = perf.winBuilds or 0,
		groundMs = perf.maxGroundMs or 0,
		groundMoves = perf.maxGroundMoves or 0,
		renderMs = perf.maxRenderMs or 0,
		hold = perf.holdFrames or 0,
		drift = perf.maxDrift or 0,
		tiles = table.Count(C.ActiveTiles),
		queue = C.QueueLen(),
		ground = (C.LastGroundCount or 0) .. "/" .. (C.BuiltGroundCount or 0),
		groupMs = C.WorstGroupMs or 0,
	}
end

local function hitchReport(reason, s)
	print(string.format(
		"[GTAV] %s: frame %.1fms | build %.2fms (%d, worst group %.1fms) | ground %.2fms (%d moved) | render %.2fms | hold %df | drift %.0f | tiles %d queue %d ground %s",
		reason, s.frameMs, s.buildMs, s.built, s.groupMs,
		s.groundMs, s.groundMoves, s.renderMs, s.hold, s.drift,
		s.tiles, s.queue, s.ground))
end

function C.PerfTick()
	if not GTAV.MapData.current then return end
	local cv = GetConVar("gtav_hitch_log")
	if cv and not cv:GetBool() then return end

	local ft = FrameTime() * 1000
	local perf = C.Perf
	perf.frameMs = ft

	local wallNow = SysTime()
	local wallMs = perf.lastTickT and (wallNow - perf.lastTickT) * 1000 or 0
	perf.lastTickT = wallNow
	perf.wallMs = wallMs
	local spikeMs = ft > wallMs and ft or wallMs   -- frametime lies during engine stalls

	local mp = C.MatProbe
	if mp then
		perf.matMsFrame = (mp.ms or 0) - (perf.lastMatMs or 0)
		perf.matCallsFrame = (mp.calls or 0) - (perf.lastMatCalls or 0)
		perf.lastMatMs = mp.ms or 0
		perf.lastMatCalls = mp.calls or 0
	else
		perf.matMsFrame, perf.matCallsFrame = 0, 0
	end

	local now = CurTime()
	if perf.watchUntil > 0 then
		if now < perf.watchUntil then
			if ft > (perf.worstMs or 0) then perf.worstMs = ft end
		else
			perf.watchUntil = 0
			hitchReport("cell " .. (perf.pendingReason or "?"), snapshot())
			perf.worstMs = 0
		end
	end

	-- 2s cooldown, 250ms+ always prints
	if spikeMs > 100 and (spikeMs >= 250 or now - (perf.lastSpike or 0) > 2) then
		perf.lastSpike = now
		local ph = perf.framePhase or {}
		print(string.format(
			"[GTAV] frame spike %.1fms (frame %.1f wall %.1f) | build %.2fms (%d this frame) | mat %.1fms (%d loads) | gc %.1fms (%d steps worst %.1f) | free %.1fms (%d) | ground %.2fms | ground built %d | tiles %d queue %d | mem %.0fMB",
			spikeMs, ft, wallMs,
			perf.lastBuildMs or 0, perf.builtThisFrame or 0,
			perf.matMsFrame or 0, perf.matCallsFrame or 0,
			ph.gc or 0, perf.gcSteps or 0, perf.gcStepWorstMs or 0,
			perf.freeMs or 0, perf.freedThisFrame or 0,
			(perf.groundMs or 0) * 1000, perf.builtGround or 0,
			table.Count(C.ActiveTiles), C.QueueLen(),
			collectgarbage("count") / 1024))
		if perf.watchUntil > 0 and spikeMs > (perf.worstMs or 0) then
			perf.worstMs = spikeMs
		end
	end
end

hook.Add("Think", "GTAV_HitchLog", C.PerfTick)

local nextState = 0
hook.Add("Think", "GTAV_StateLog", function()
	if not GTAV.MapData.current then return end
	if not GetConVar("gtav_debug"):GetBool() then return end
	local now = CurTime()
	if now < nextState then return end
	nextState = now + 1

	local ply = LocalPlayer()
	if not IsValid(ply) or not C.LastEyePos then return end
	local cell = ply:GetNW2Vector("WorldCell", Vector(0, 0, 0))
	local draw = C.RenderCell or cell
	local pos = ply:GetPos()
	local eye = pos
	local abs = C.LastEyePos

	print(string.format(
		"[GTAV] state: net %s draw %s | local %.0f %.0f %.0f eye %.0f %.0f %.0f abs %.0f %.0f %.0f | hold %s",
		GTAV.GetCellKey(cell), GTAV.GetCellKey(draw),
		pos.x, pos.y, pos.z, eye.x, eye.y, eye.z, abs.x, abs.y, abs.z,
		(draw - cell):LengthSqr() > 1 and "yes" or "no"))

	local ps = GTAV.Config.PHYS_CHUNK_SIZE
	local cx, cy = math.floor(abs.x / ps), math.floor(abs.y / ps)
	for _, ent in ipairs(ents.FindByClass("gtav_ground")) do
		if ent.GetPieceX and ent:GetPieceX() == cx and ent:GetPieceY() == cy then
			local phys = ent:GetPhysicsObject()
			local target = ent:TargetPos()
			print(string.format(
				"  ground %d %d built=%s ent %.0f %.0f target %.0f %.0f phys %s | d %.0f",
				cx, cy, tostring(ent.gtavBuilt == true),
				ent:GetPos().x, ent:GetPos().y, target.x, target.y,
				IsValid(phys) and string.format("%.0f %.0f", phys:GetPos().x, phys:GetPos().y) or "none",
				(target - Vector(pos.x, pos.y, 0)):Length2D()))
			return
		end
	end
	print(string.format("  ground %d %d is not loaded", cx, cy))
end)

hook.Add("OnEntityCreated", "GTAV_GroundCreated", function(ent)
	if not IsValid(ent) or ent:GetClass() ~= "gtav_ground" then return end
	C.NextGroundBuild = 0
end)


function C.SyncClientCells()
	if not GTAV.MapData.current then return end
	local now = CurTime()
	if C.NextCellSync and now < C.NextCellSync then return end
	C.NextCellSync = now + 0.2

	local me = LocalPlayer()
	if not IsValid(me) then return end

	local function sync(ent)
		local cell = ent:GetNW2Vector("WorldCell", Vector(0, 0, 0))
		local key = GTAV.GetCellKey(cell)
		if ent.WorldCellKey ~= key then
			ent.WorldCellKey = key
			ent:SetCustomCollisionCheck(true)
			ent:CollisionRulesChanged()
		end
	end

	sync(me)
	for _, ply in ipairs(player.GetAll()) do
		if ply ~= me then sync(ply) end
	end
end

function C.CrossCellEntities()
	if not GTAV.MapData.current then return end

	local now = CurTime()
	if C.NextCellScan and now < C.NextCellScan then return end
	C.NextCellScan = now + 0.1

	local me = LocalPlayer()
	if not IsValid(me) then return end

	-- draw frame not the netvar
	local myCell = C.RenderCell or me:GetNW2Vector("WorldCell", Vector(0, 0, 0))

	for _, ent in ipairs(ents.GetAll()) do
		if ent ~= me and not ent:IsWorld() and not ent.IsGTAVGround then
			local entCell = ent:GetNW2Vector("WorldCell", myCell)
			local diff = entCell - myCell

			if diff.x ~= 0 or diff.y ~= 0 or diff.z ~= 0 then
				if ent.RenderOffset ~= diff then
					if not ent.gtavOrigRenderOverride then
						ent.gtavOrigRenderOverride = ent.RenderOverride
					end
					ent.RenderOffset = diff
					ent.RenderOverride = C.ShiftedRender
					ent:SetRenderBounds(Vector(-16384, -16384, -16384), Vector(16384, 16384, 16384))
				end
			elseif ent.RenderOffset then
				ent.RenderOffset = nil
				ent.RenderOverride = ent.gtavOrigRenderOverride
				ent.gtavOrigRenderOverride = nil
				ent:SetRenderBounds(Vector(-100, -100, -100), Vector(100, 100, 100))
			end
		end
	end
end

function C.ShiftedRender(ent, flags)
	local offset = ent.RenderOffset
	if offset then
		local m = Matrix()
		m:SetTranslation(offset)
		cam.PushModelMatrix(m)
	end
	ent:DrawModel(flags)
	if offset then cam.PopModelMatrix() end
end


function C.DebugDraw(depth, skybox, sky3d)
	if depth or sky3d then return end
	if not GetConVar("gtav_debug"):GetBool() then return end
	if not GTAV.MapData.current then return end

	local ply = LocalPlayer()
	if not IsValid(ply) then return end
	local myCell = C.RenderCell or ply:GetNW2Vector("WorldCell", Vector(0, 0, 0))

	if GetConVar("gtav_debug_cells"):GetBool() then
		local cs = GTAV.Config.CELL_SIZE
		local o = myCell
		render.SetColorMaterial()
		for x = -cs, cs, cs do
			for y = -cs, cs, cs do
				local origin = Vector(x, y, 0) - o
				render.DrawWireframeBox(origin, angle_zero, Vector(0, 0, 0),
					Vector(cs, cs, 100), Color(0, 255, 0, 60), true)
			end
		end
	end

	if GetConVar("gtav_debug_lod"):GetBool() then
		local lodColors = {
			[0] = Color(0, 255, 0, 100),
			[1] = Color(255, 255, 0, 100),
			[2] = Color(255, 165, 0, 100),
			[3] = Color(255, 0, 0, 100),
			[4] = Color(128, 0, 128, 100),
		}
		local ts = GTAV.Config.VISUAL_TILE_SIZE
		render.SetColorMaterial()
		for _, tile in pairs(C.ActiveTiles) do
			local color = lodColors[tile.lod] or Color(255, 255, 255, 100)
			render.DrawWireframeBox(tile.origin - myCell, angle_zero,
				Vector(0, 0, 0), Vector(ts, ts, 50), color, true)
		end
	end

	if GetConVar("gtav_debug_phys"):GetBool() then
		render.SetColorMaterial()
		for _, ent in ipairs(ents.FindByClass("gtav_ground")) do
			local p = ent.TargetPos and ent:TargetPos() or ent:GetPos()
			render.DrawWireframeBox(p, angle_zero, Vector(0, 0, 0),
				Vector(GTAV.Config.PHYS_CHUNK_SIZE, GTAV.Config.PHYS_CHUNK_SIZE, 100),
				Color(255, 255, 0, 40), true)
		end
	end

	if GetConVar("gtav_debug_phys_mesh"):GetBool() then
		render.SetColorMaterial()
		local col = Color(0, 200, 255, 180)
		local budget = 3000 -- triangle EDGES per frame
		local eye = EyePos()

		local near = {}
		for _, ent in ipairs(ents.FindByClass("gtav_ground")) do
			if ent.gtavBuilt then
				local p = ent.TargetPos and ent:TargetPos() or ent:GetPos()
				near[#near + 1] = { ent = ent, pos = p, d = p:DistToSqr(eye) }
			end
		end
		table.sort(near, function(a, b) return a.d < b.d end)

		local used = 0
		for i = 1, #near do
			if used >= budget then break end
			local e = near[i].ent
			local tris = GTAV.Binary.GetPhysMesh(e:GetPieceX(), e:GetPieceY())
			if tris then
				local base = near[i].pos
				for t = 1, #tris - 2, 3 do
					if used >= budget then break end
					local a = tris[t]
					local b = tris[t + 1]
					local c = tris[t + 2]
					if a and b and c then
						render.DrawLine(base + a, base + b, col, true)
						render.DrawLine(base + b, base + c, col, true)
						render.DrawLine(base + c, base + a, col, true)
						used = used + 3
					end
				end
			end
		end
	end

	cam.Start2D()
	local y = 100
	local function stat(text)
		draw.SimpleText(text, "DermaDefault", 10, y, Color(255, 255, 255, 255),
			TEXT_ALIGN_LEFT, TEXT_ALIGN_TOP)
		y = y + 18
	end
	stat("GTAV")
	stat(string.format("  Tiles active: %d  queued: %d  builds: %d",
		table.Count(C.ActiveTiles), C.QueueLen(), C.Stats.builds))
	local ds = C.DrawStats
	stat(string.format("  Draw: calls %d  skip %s  order %d  drawn %d  culled %d  behind %d  bad %d  glass %d  add %d",
		C.Stats.renderCalls or 0, tostring(C.Stats.renderSkip or "-"),
		ds.order or 0, C.Stats.tilesDrawn or 0, ds.culled or 0, ds.behind or 0,
		ds.bad or 0, ds.glass or 0, ds.additive or 0))
	if C.RenderError then
		stat("  ERR " .. tostring(C.RenderError))
	end
	stat(string.format("  Ground: %d chunks, %d built", C.LastGroundCount, C.BuiltGroundCount or 0))
	stat(string.format("  Cell/eye: %s", C.EyeMismatch and "holding old frame (origin lagging)" or "ok"))
	stat(string.format("  Build %.2fms  Render %.2fms", C.Stats.buildTime * 1000, C.Stats.renderTime * 1000))
	local bs = GTAV.Native and GTAV.Native.LastStats
	if bs then
		stat(string.format("  Group: %d tiles  %d draws  %d tris | read %.1f  dec %.1f  merge %.1f  mesh %.1f  = %.1fms",
			bs.tiles or 0, bs.draws or 0, bs.tris or 0,
			bs.readMs or 0, bs.decodeMs or 0, bs.accMs or 0, bs.meshMs or 0,
			bs.totalMs or 0))
		local N = GTAV.Native
		stat(string.format("  Mesh: lost %d tris total (%d materials)  retry %d  split %d  badidx %d",
			N.MeshFailTris or 0, N.MeshFail or 0, N.MeshRetry or 0,
			bs.meshSplit or 0, N.MeshBadIdx or 0))
	end
	stat(string.format("  Group worst %.1fms  debt %.1fms  skipped %d  path %s",
		C.WorstGroupMs or 0, (C.BuildDebt or 0) * 1000, C.BuildSkipped or 0,
		tostring(C.MeshPath or (GTAV.Native and GTAV.Native.LastPath) or "?")))
	stat(string.format("  Hold %df  Drift %.0f", C.Perf.holdFrames or 0, C.Perf.maxDrift or 0))
	stat(string.format("  Cell: %s", GTAV.GetCellKey(myCell)))
	stat(string.format("  Local: %.0f %.0f %.0f", ply:GetPos().x, ply:GetPos().y, ply:GetPos().z))
	local absEye = C.LastEyePos or ply:GetPos()
	stat(string.format("  Abs: %.0f %.0f %.0f", absEye.x, absEye.y, absEye.z))
	cam.End2D()
end

hook.Add("InitPostEntity", "GTAV_ClientInit", C.Initialize)

hook.Add("ShutDown", "GTAV_SpellingFlush", C.SpellingFlush)
hook.Add("Think", "GTAV_SpellingFlushTick", function()
	if not C.SpellingDirty then return end
	local now = RealTime()
	if now < (C.SpellingNext or 0) then return end
	C.SpellingNext = now + 60
	C.SpellingFlush()
end)
