if jit.arch ~= "x64" then return end

if scripted_ents and not scripted_ents.GetStored("gtav_ground") then
	include("gtav/sh_ground.lua")
end

include("gtav/cl_native.lua")
include("gtav/cl_merge.lua")
include("gtav/cl_water.lua")
include("gtav/cl_stream.lua")

print("[GTAV] Client modules loaded")
