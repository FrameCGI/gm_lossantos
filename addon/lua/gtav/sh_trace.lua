GTAV.Trace = GTAV.Trace or {}
-- untouched engine traces
GTAV.Trace.RealTraceLine = GTAV.Trace.RealTraceLine or util.TraceLine
GTAV.Trace.RealTraceHull = GTAV.Trace.RealTraceHull or util.TraceHull
GTAV.Trace.RealTraceEntity = GTAV.Trace.RealTraceEntity or util.TraceEntity

-- bullets ask ShouldCollide but util traces dont, so eye traces and the toolgun
-- hit stuff in other cells. infmap does this too. only runs with 2+ cells going
local function traceCell(data, ent)
	if CLIENT then
		local me = LocalPlayer()
		return IsValid(me) and me.WorldCellKey or nil
	end
	if isentity(ent) and IsValid(ent) then return ent.WorldCellKey end
	local f = data.filter
	if isentity(f) and IsValid(f) then return f.WorldCellKey end
	if istable(f) and isentity(f[1]) and IsValid(f[1]) then return f[1].WorldCellKey end
end

local function inCell(real)
	return function(data, ent)
		if not GTAV.MultiCell then return real(data, ent) end
		local key = traceCell(data, ent)
		if not key then return real(data, ent) end

		local copy = {}
		for k, v in pairs(data) do copy[k] = v end
		local old = data.filter
		copy.filter = function(e)
			local k = e.WorldCellKey
			if k and k ~= key then return false end
			if old == nil then return true end
			if isfunction(old) then return old(e) end
			if isentity(old) then return e ~= old end
			for i = 1, #old do
				if old[i] == e then return false end
			end
			return true
		end
		return real(copy, ent)
	end
end

util.TraceLine = inCell(GTAV.Trace.RealTraceLine)
util.TraceHull = inCell(GTAV.Trace.RealTraceHull)
util.TraceEntity = inCell(GTAV.Trace.RealTraceEntity)

if SERVER then
	GTAV.Server = GTAV.Server or {}

	function GTAV.Trace.ToAbsolute(ent, localPos)
		return localPos + ent:GetNW2Vector("WorldCell", Vector(0, 0, 0))
	end
end
