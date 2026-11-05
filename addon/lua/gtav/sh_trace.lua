GTAV.Trace = GTAV.Trace or {}
-- untouched engine traces
GTAV.Trace.RealTraceLine = GTAV.Trace.RealTraceLine or util.TraceLine
GTAV.Trace.RealTraceHull = GTAV.Trace.RealTraceHull or util.TraceHull

if SERVER then
	GTAV.Server = GTAV.Server or {}

	function GTAV.Trace.ToAbsolute(ent, localPos)
		return localPos + ent:GetNW2Vector("WorldCell", Vector(0, 0, 0))
	end
end
