local function park(ply)
	if not IsValid(ply) then return end
	ply:SetMoveType(MOVETYPE_NOCLIP)
	ply:SetVelocity(vector_origin)
	ply:SetLocalVelocity(vector_origin)
	if ply.SetGravity then ply:SetGravity(0) end
end

concommand.Add("gtav_st_goto", function(ply, _, args)
	if not IsValid(ply) or not ply:IsAdmin() or #args < 3 then return end
	local x, y, z = tonumber(args[1]), tonumber(args[2]), tonumber(args[3])
	if not x or not y or not z then return end

	local target = Vector(x, y, z)
	local S = GTAV and GTAV.Server

	-- absolute, plain SetPos lands a frame off
	if S and S.Teleport then
		S.Teleport(ply, target)
	else
		ErrorNoHalt("[GTAV selftest] gtav_st_goto: no placement API, refusing\n")
		return
	end

	park(ply)
	-- twice, SetPos can leave a tick of momentum
	timer.Simple(0, function()
		if IsValid(ply) then
			park(ply)
		end
	end)

	if SetGlobal2Vector then
		SetGlobal2Vector("gtav_st_last", target)
		SetGlobal2Vector("gtav_st_landed", GTAV.GetAbsolutePos(ply))
	end
end)

concommand.Add("gtav_st_park", function(ply)
	if not IsValid(ply) or not ply:IsAdmin() then return end
	park(ply)
end)
