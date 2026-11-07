-- clients only get what the server sends them
if SERVER then
	AddCSLuaFile("gtav/sh_config.lua")
	AddCSLuaFile("gtav/sh_binary.lua")
	AddCSLuaFile("gtav/sh_trace.lua")
	AddCSLuaFile("gtav/sh_maploader.lua")
	AddCSLuaFile("gtav/sh_ground.lua")
	AddCSLuaFile("gtav/cl_native.lua")
	AddCSLuaFile("gtav/cl_merge.lua")
	AddCSLuaFile("gtav/cl_water.lua")
	AddCSLuaFile("gtav/cl_stream.lua")
end

include("gtav/sh_config.lua")
include("gtav/sh_binary.lua")
include("gtav/sh_trace.lua")
include("gtav/sh_maploader.lua")
include("gtav/sh_ground.lua")

print("[GTAV] Shared modules loaded")