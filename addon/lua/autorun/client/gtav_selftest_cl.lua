if CLIENT then
	local ARM_PATH = "gtav/selftest_arm.json"

	local ST = {
		running = false,
		run = "run",
		waypoints = {},
		index = 0,
		phase = "idle",     -- idle | travel | settle | capture | done
		settle = 0,
		settledFor = 0,
		rows = {},
		holeRows = {},
		startedAt = 0,
		failReason = nil,
		frameMs = {},
		lastFrameT = nil,
		lastFrameNo = nil,
		frameMax = 0,
		worstFrame = nil,
	}

	-- STARTED.txt  run began
	-- stats.csv    one row per waypoint
	-- holes.csv    collision per waypoint
	-- wp_NN.png    capture per waypoint
	-- DONE.txt     clean finish
	-- FAIL.txt     or this, with the reason
	local function outDir()
		return "gtav/selftest/" .. ST.run
	end

	local function say(msg)
		print("[GTAV selftest] " .. msg)
	end


	local function collectStats()
		local C = GTAV and GTAV.Client
		local N = GTAV and GTAV.Native
		if not C then return {} end

		local ds = C.DrawStats or {}
		local perf = C.Perf or {}
		local st = C.Stats or {}
		local wf = ST.worstFrame
		local av = ST.framePhaseSum
		local avN = av and av.n > 0 and av.n or 1
		local function avgOf(k) return av and av[k] / avN or 0 end
		local bs = {}
		if N and N.BuildStats then
			local okB, res = pcall(N.BuildStats)
			if okB and type(res) == "table" then bs = res end
		end

		local active, corners, subs = 0, 0, 0

		local lodN = {0, 0, 0, 0, 0}
		local lodTris = {0, 0, 0, 0, 0}

		for _, tile in pairs(C.ActiveTiles or {}) do
			active = active + 1
			corners = corners + (tile.triCount or 0) -- triCount is corners
			subs = subs + (tile.submeshCount or 0)

			local l = tonumber(tile.lod)
			if l and l >= 0 and l <= 4 then
				lodN[l + 1] = lodN[l + 1] + 1
				lodTris[l + 1] = lodTris[l + 1] + math.floor((tile.triCount or 0) / 3)
			end
		end

		local ply = LocalPlayer()
		local eye = C.LastEyePos or (IsValid(ply) and ply:GetPos()) or vector_origin
		local cell = C.RenderCell or vector_origin
		local ang = (IsValid(ply) and ply:EyeAngles()) or Angle(0, 0, 0)

		local latN = perf.latN or 0
		local latAvgFrames = latN > 0 and (perf.latSum or 0) / latN or 0
		local latMaxFrames = perf.latMax or 0
		perf.latSum, perf.latN, perf.latMax = 0, 0, 0

		return {
			t = SysTime() - ST.startedAt,
			x = eye.x, y = eye.y, z = eye.z,
			eyePitch = ang.pitch, eyeYaw = ang.yaw,
			cellX = cell.x, cellY = cell.y, cellZ = cell.z,

			active = active,
			tris = math.floor(corners / 3),
			corners = corners,
			submeshes = subs,
			lodCounts = table.concat(lodN, "/"),
			lodTris = table.concat(lodTris, "/"),
			queue = C.QueueLen and C.QueueLen() or 0,
			builds = st.builds or 0,
			buildMs = (st.buildTime or 0) * 1000,
			renderMs = (st.renderTime or 0) * 1000,

			drawOrder = ds.order or 0,
			drawSubs = ds.subs or 0,
			drawn = ds.drawn or 0,
			culled = ds.culled or 0,
			behind = ds.behind or 0,
			bad = ds.bad or 0,
			glass = ds.glass or 0,
			subBehind = ds.subBehind or 0,
			subFogged = ds.subFogged or 0,
			glassCulled = ds.glassCulled or 0,
			glassMs = st.glassMs or 0,
			renderCalls = st.renderCalls or 0,
			renderSkip = tostring(C.Stats and C.Stats.renderSkip or "-"),

			lostTris = (N and N.MeshFailTris) or 0,
			lostMats = (N and N.MeshFail) or 0,
			retry = (N and N.MeshRetry) or 0,
			split = bs.meshSplit or 0,
			badidx = (N and N.MeshBadIdx) or 0,

			nativeGroups = C.NativeGroups or 0,
			luaGroups = C.LuaGroups or 0,
			fallbackGroups = C.FallbackGroups or 0,
			path = tostring(C.MeshPath or (N and N.LastPath) or "-"),

			groupWorstMs = C.WorstGroupMs or 0,
			debtMs = (C.BuildDebt or 0) * 1000,
			skipped = C.BuildSkipped or 0,
			hold = perf.holdFrames or 0,
			drift = perf.maxDrift or 0,

			bsTiles = bs.tiles or 0,
			bsDraws = bs.draws or 0,
			bsTris = bs.tris or 0,
			bsRead = bs.readMs or 0,
			bsDecode = bs.decodeMs or 0,
			bsAcc = bs.accMs or 0,
			bsMesh = bs.meshMs or 0,
			bsTotal = bs.totalMs or 0,
			bsFlagNames = bs.flagNames or 0,
			bsFlagMs = bs.flagMs or 0,
			bsHandshakes = bs.handshakes or 0,
			flagCacheN = bs.flagCacheN or 0,
			latAvgFrames = latAvgFrames,
			latMaxFrames = latMaxFrames,
			flagNamesTot = bs.flagNamesTot or 0,
			flagMsTot = bs.flagMsTot or 0,
			prepTot = bs.prepTot or 0,

			maxDtMs = wf and wf.dt or 0,
			maxDtBuildMs = wf and wf.build or 0,
			maxDtRenderMs = wf and wf.render or 0,
			maxDtWantedMs = wf and wf.wanted or 0,
			maxDtPollMs = wf and wf.poll or 0,
			maxDtWarmMs = wf and wf.warm or 0,
			maxDtCommitMs = wf and wf.commit or 0,
			maxDtWrapMs = wf and wf.wrap or 0,
			maxDtPrepMs = wf and wf.prep or 0,
			maxDtSyncMs = wf and wf.sync or 0,
			maxDtGcMs = wf and wf.gc or 0,
			maxDtGroundMs = wf and wf.ground or 0,
			wantedWorstMs = perf.worstWanted or 0,
			pollWorstMs = perf.worstPoll or 0,
			warmWorstMs = perf.worstWarm or 0,
			commitWorstMs = perf.worstCommit or 0,
			wrapWorstMs = perf.worstWrap or 0,
			prepWorstMs = perf.worstPrep or 0,
			syncWorstMs = perf.worstSync or 0,
			gcWorstMs = perf.worstGC or 0,
			groundWorstMs = perf.worstGround or 0,

			avgDtMs = avgOf("dt"),
			avgBuildMs = avgOf("build"),
			avgRenderMs = avgOf("render"),
			avgGroundMs = avgOf("ground"),
			avgEngineMs = avgOf("dt") - avgOf("build") - avgOf("render")
				- avgOf("ground"),
			avgWantedMs = avgOf("wanted"),
			avgPollMs = avgOf("poll"),
			avgWarmMs = avgOf("warm"),
			avgCommitMs = avgOf("commit"),
			avgWrapMs = avgOf("wrap"),
			avgPrepMs = avgOf("prep"),
			avgSyncMs = avgOf("sync"),
			avgGcMs = avgOf("gc"),
		}
	end


	local function groundState(origin)
		local ps = GTAV.Config.PHYS_CHUNK_SIZE or 1024
		local radius = GTAV.Config.GROUND_QUERY_RADIUS or 1400


local ply = LocalPlayer()
		local worldCell = (ply and GTAV.GetEntityCell)
			and GTAV.GetEntityCell(ply) or Vector(0, 0, 0)
		-- origin is local, chunks are absolute
		local ax, ay = origin.x + worldCell.x, origin.y + worldCell.y

		-- no traces, the bsp shell eats every ray
		local owned, built = {}, 0
		local gents = 0
		for _, ent in ipairs(ents.FindByClass("gtav_ground")) do
			if ent.GetPieceX and ent.GetPieceY then
				gents = gents + 1
				local ok = ent.gtavBuilt and true or false
				owned[ent:GetPieceX() .. " " .. ent:GetPieceY()] = ok -- false = ent but no mesh yet
				if ok then built = built + 1 end
			end
		end


		-- mirrors the ThinkChunks box
		local wanted, hits, misses, unbuilt = 0, 0, 0, 0
		local missList = {}

		local x0 = math.floor((ax - radius) / ps)
		local x1 = math.floor((ax + radius) / ps)
		local y0 = math.floor((ay - radius) / ps)
		local y1 = math.floor((ay + radius) / ps)

		for cx = x0, x1 do
			for cy = y0, y1 do
				if GTAV.GetPhysChunkInfo(cx, cy) ~= nil then
					wanted = wanted + 1
					local st = owned[cx .. " " .. cy]
					if st == true then
						hits = hits + 1
					elseif st == false then
						unbuilt = unbuilt + 1
					else
						misses = misses + 1
						if #missList < 64 then
							missList[#missList + 1] = string.format(
								"chunk %d %d (abs %.0f %.0f)", cx, cy,
								cx * ps, cy * ps)
						end
					end
				end
			end
		end

		local wideHalf = math.floor((radius * 3) / ps)
		local wide, wideTotal = 0, 0
		for cx = x0 - wideHalf, x1 + wideHalf do
			for cy = y0 - wideHalf, y1 + wideHalf do
				wideTotal = wideTotal + 1
				if owned[cx .. " " .. cy] ~= nil then wide = wide + 1 end
			end
		end

		return {
			x = origin.x, y = origin.y, z = origin.z,
			hits = hits, misses = misses, unbuilt = unbuilt,
			gents = gents, built = built,
			total = wanted,
			complete = (misses == 0 and unbuilt == 0),
			-- informational only, cannot fail
			wide = wide, wideTotal = wideTotal,
			radius = radius, chunk = ps,
			samples = table.concat(missList, ";"),
		}
	end


	local function csvEscape(v)
		local s = tostring(v)
		if s:find("[,\r\n\"]") then
			s = '"' .. s:gsub('"', '""') .. '"'
		end
		return s
	end

	local function writeCsv(name, rows)
		if #rows == 0 then return end
		local keys = {}
		for k in pairs(rows[1]) do keys[#keys + 1] = k end
		table.sort(keys)

		local buf = { table.concat(keys, ",") }
		for i = 1, #rows do
			local line = {}
			for j = 1, #keys do
				line[j] = csvEscape(rows[i][keys[j]] or 0)
			end
			buf[#buf + 1] = table.concat(line, ",")
		end
		file.Write(outDir() .. "/" .. name, table.concat(buf, "\n") .. "\n")
	end


	local function finish(reason)
		if not ST.running then return end
		ST.running = false
		ST.phase = "done"

		writeCsv("stats.csv", ST.rows)
		writeCsv("holes.csv", ST.holeRows)

		-- wrong place = failed run
		if not reason then
			local tol = (GetConVar("gtav_st_postol") and GetConVar("gtav_st_postol"):GetInt()) or 512
			local worst = ST.maxPosErr or 0
			if worst > tol then
				reason = string.format(
					"placement mismatch: the body landed %.0f units from its waypoint (tolerance %d)",
					worst, tol)
			end
		end

		if not reason then
			local bad, badidx, lost, holes = 0, 0, 0, 0
			for i = 1, #ST.rows do
				local r = ST.rows[i]
				bad = bad + (r.bad or 0)
				badidx = badidx + (r.badidx or 0)
				lost = lost + (r.lostTris or 0)
			end
			for i = 1, #ST.holeRows do
				holes = holes + (ST.holeRows[i].misses or 0)
				holes = holes + (ST.holeRows[i].unbuilt or 0)
			end
			ST.chain = { bad = bad, badidx = badidx, lost = lost, holes = holes }
			if bad > 0 or badidx > 0 or lost > 0 or holes > 0 then
				reason = string.format(
					"corruption: bad %d, badidx %d, lostTris %d, holes %d",
					bad, badidx, lost, holes)
			end
		end

		if reason then
			ST.failReason = reason
			file.Write(outDir() .. "/FAIL.txt",
				reason .. "\ncompleted " .. (#ST.rows) .. " of " .. (#ST.waypoints) .. " waypoints\n")
			say("FAILED: " .. reason)
		else
			local c = ST.chain or {}
			file.Write(outDir() .. "/DONE.txt",
				string.format("run %s\n%d waypoints\n%ds wall\nmaxPosErr %.1f\n"
					.. "bad %d\nbadidx %d\nlostTris %d\nholes %d\n",
					ST.run, #ST.rows, math.floor(SysTime() - ST.startedAt),
					ST.maxPosErr or 0,
					c.bad or 0, c.badidx or 0, c.lost or 0, c.holes or 0))
			say(string.format("done: %d waypoints in %ds -> %s",
				#ST.rows, math.floor(SysTime() - ST.startedAt), outDir()))
		end

		-- batch job, quit so pack build can run
		timer.Simple(1.5, function()
			say("quitting")
			RunConsoleCommand("quit")
		end)
	end

	local function beginWaypoint(i)
		-- waypoint {x, y, z [, pitch, yaw]}
		local wp = ST.waypoints[i]
		if not wp then return finish(nil) end

		local target = Vector(wp[1], wp[2], wp[3])

		if wp[4] ~= nil then
			ST.lookPitch = tonumber(wp[4])
			ST.lookYaw = tonumber(wp[5]) or 0
			local ply = LocalPlayer()
			if IsValid(ply) then
				ply:SetEyeAngles(Angle(ST.lookPitch, ST.lookYaw, 0))
			end
		else
			ST.lookPitch = nil
		end

		local prev = ST.target
		ST.jump2D = prev and (target - prev):Length2D() or 0

		ST.index = i
		ST.phase = "travel"
		ST.settle = 0
		ST.settledFor = 0
		ST.target = target

		ST.frameMs = {}
		ST.lastFrameT = nil
		ST.lastFrameNo = nil
		ST.frameMax = 0
		ST.worstFrame = nil
		ST.framePhaseSum = nil

		say(string.format("waypoint %d/%d -> %.0f %.0f %.0f",
			i, #ST.waypoints, target.x, target.y, target.z))
		RunConsoleCommand("gtav_st_goto",
			tostring(target.x), tostring(target.y), tostring(target.z))
	end

	local function streamerSettled()
		local C = GTAV and GTAV.Client

		local ply = LocalPlayer()
		if ply then
			local g = groundState(ply:GetPos())
			if not g.complete then return false end
		end

		if not C then return true end
		local q = C.QueueLen and C.QueueLen() or 0
		local built = (C.Perf and C.Perf.builtThisFrame) or 0
		if q ~= 0 or built ~= 0 then return false end

		-- in flight builds arent in the queue
		if C.AsyncTask or C.ReadyTask or C.CommitTask then return false end
		if C.WrapTasks and C.WrapTasks[1] then return false end

		if C.EyeMismatch then return false end
		return true
	end

	local function step()
		if not ST.running then return end

		if ST.phase == "travel" then
			ST.travelTicks = (ST.travelTicks or 0) + 1
			local got = GetGlobal2Vector and GetGlobal2Vector("gtav_st_last", vector_origin) or nil
			if got and got:DistToSqr(ST.target) > 25 and ST.travelTicks < 60 then return end
			ST.travelTicks = 0
			ST.phase = "settle"
			return
		end

		if ST.phase == "settle" then
			if ST.lookPitch ~= nil then
				local ply = LocalPlayer()
				if IsValid(ply) then
					ply:SetEyeAngles(Angle(ST.lookPitch, ST.lookYaw or 0, 0))
				end
			end
			ST.settle = ST.settle + 1
			if streamerSettled() then
				ST.settledFor = ST.settledFor + 1
			else
				ST.settledFor = 0
			end

			local hold = (GetConVar("gtav_st_settle") and GetConVar("gtav_st_settle"):GetInt()) or 30
			local timeout = (GetConVar("gtav_st_timeout") and GetConVar("gtav_st_timeout"):GetInt()) or 1200

			if ST.settledFor >= hold then
				ST.phase = "capture"
				ST.wantCapture = true
			elseif ST.settle >= timeout then
				say(string.format("waypoint %d never settled (queue=%d) - capturing anyway",
					ST.index, GTAV.Client and GTAV.Client.QueueLen and GTAV.Client.QueueLen() or -1))
				ST.phase = "capture"
				ST.wantCapture = true
				ST.neverSettled = true
			end
			return
		end
	end


	local function sampleFrame()
		local now = SysTime()
		local fn = FrameNumber and FrameNumber() or nil
		-- PostRender can fire twice a frame
		if fn then
			if fn == ST.lastFrameNo then return end
			ST.lastFrameNo = fn
		end
		if ST.lastFrameT then
			local dt = (now - ST.lastFrameT) * 1000
			if dt > 0 then
				ST.frameMs[#ST.frameMs + 1] = dt
				do
					local C = GTAV and GTAV.Client
					local p = C and C.Perf or {}
					local ph = p.framePhase or {}
					local av = ST.framePhaseSum
					if not av then
						av = {n = 0, dt = 0, build = 0, render = 0,
							  ground = 0, wanted = 0, poll = 0, warm = 0,
							  commit = 0, wrap = 0, prep = 0, sync = 0, gc = 0}
						ST.framePhaseSum = av
					end
					av.n = av.n + 1
					av.dt = av.dt + dt
					av.build = av.build
						+ (((C and C.Stats and C.Stats.buildTime) or 0) * 1000)
					av.render = av.render
						+ (((C and C.Stats and C.Stats.renderTime) or 0) * 1000)
					av.ground = av.ground
						+ (((p.groundBuildMs or 0) + (p.groundMs or 0) * 1000))
					av.wanted = av.wanted + (ph.wanted or 0)
					av.poll = av.poll + (ph.poll or 0)
					av.warm = av.warm + (ph.warm or 0)
					av.commit = av.commit + (ph.commit or 0)
					av.wrap = av.wrap + (ph.wrap or 0)
					av.prep = av.prep + (ph.prep or 0)
					av.sync = av.sync + (ph.sync or 0)
					av.gc = av.gc + (ph.gc or 0)
				end
				if dt > (ST.frameMax or 0) then
					ST.frameMax = dt
					local C = GTAV and GTAV.Client
					local p = C and C.Perf or {}
					local ph = p.framePhase or {}
					ST.worstFrame = {
						dt = dt,
						build = ((C and C.Stats and C.Stats.buildTime) or 0) * 1000,
						render = ((C and C.Stats and C.Stats.renderTime) or 0) * 1000,
						wanted = ph.wanted or 0,
						poll = ph.poll or 0,
						warm = ph.warm or 0,
						commit = ph.commit or 0,
						wrap = ph.wrap or 0,
						prep = ph.prep or 0,
						sync = ph.sync or 0,
						gc = ph.gc or 0,
						ground = ((p.groundBuildMs or 0)
								  + (p.groundMs or 0) * 1000),
					}
				end
			end
		end
		ST.lastFrameT = now
	end

	hook.Add("PostRender", "GTAVSelfTestCapture", function()
		if not ST.running then return end
		sampleFrame()
		if not ST.wantCapture then return end
		ST.wantCapture = false

		local wp = ST.waypoints[ST.index]
		local row = collectStats()
		row.wp = ST.index
		row.wx = wp and wp[1] or 0
		row.wy = wp and wp[2] or 0
		row.wz = wp and wp[3] or 0
		row.settled = ST.neverSettled and 0 or 1
		row.settleTicks = ST.settle

		local landed = GetGlobal2Vector and GetGlobal2Vector("gtav_st_landed", nil) or nil
		row.posErr = landed and landed:Distance(Vector(row.wx, row.wy, row.wz)) or -1
		ST.maxPosErr = math.max(ST.maxPosErr or 0, math.max(row.posErr, 0))

		row.jump2D = ST.jump2D or 0

		do
			local samples = ST.frameMs
			local n = #samples
			row.frameN = n
			row.frameAvgMs = 0
			row.frameP95Ms = 0
			row.frameMaxMs = 0
			if n > 0 then
				local sorted = {}
				for i = 1, n do sorted[i] = samples[i] end
				table.sort(sorted)
				local sum = 0
				for i = 1, n do sum = sum + sorted[i] end
				local k = math.ceil(n * 0.95)
				if k < 1 then k = 1 end
				row.frameAvgMs = sum / n
				row.frameP95Ms = sorted[k]
				row.frameMaxMs = sorted[n]
			end
		end

		ST.neverSettled = false
		ST.rows[#ST.rows + 1] = row

		local hole = groundState(LocalPlayer():GetPos())
		hole.wp = ST.index
		ST.holeRows[#ST.holeRows + 1] = hole

		say(string.format(
			"  wp %d: active %d tris %d drawn %d bad %d badidx %d lost %d coll %d/%d (unbuilt %d, gents %d built %d) posErr %.0f",
			ST.index, row.active, row.tris, row.drawn, row.bad,
			row.badidx, row.lostTris, hole.hits, hole.total,
			hole.unbuilt, hole.gents, hole.built, row.posErr))
		say(string.format(
			"       wanted set = chunks within %d units (%d-chunk grid); wide box %d/%d owned",
			hole.radius, hole.chunk, hole.wide, hole.wideTotal))
		say(string.format("       lod tiles 0..4 = %s | lod tris = %s",
			row.lodCounts, row.lodTris))
		say(string.format("       frames %d: avg %.2fms p95 %.2fms max %.2fms | renderPass %.2fms | mesh %s",
			row.frameN, row.frameAvgMs, row.frameP95Ms, row.frameMaxMs,
			row.renderMs, row.path))
		say(string.format(
			"       floor split (avg %d frames): streamer %.2f (warm %.2f commit %.2f gc %.2f) + render %.2f + ground %.2f + engine/GPU %.2f",
			row.frameN, row.avgBuildMs, row.avgWarmMs, row.avgCommitMs,
			row.avgGcMs, row.avgRenderMs, row.avgGroundMs,
			math.max(0, row.avgEngineMs)))
		if row.maxDtMs and row.maxDtMs > 0 then
			say(string.format(
				"       worst frame %.1fms = streamer %.1f (wanted %.1f poll %.1f warm %.1f commit %.1f wrap %.1f prep %.1f sync %.1f gc %.1f) + render %.1f + engine/GPU %.1f",
				row.maxDtMs, row.maxDtBuildMs, row.maxDtWantedMs, row.maxDtPollMs,
				row.maxDtWarmMs, row.maxDtCommitMs, row.maxDtWrapMs,
				row.maxDtPrepMs, row.maxDtSyncMs, row.maxDtGcMs,
				row.maxDtRenderMs,
				math.max(0, row.maxDtMs - row.maxDtBuildMs - row.maxDtRenderMs)))
			say(string.format(
				"       phase worsts: wanted %.1f poll %.1f warm %.1f commit %.1f wrap %.1f prep %.1f sync %.1f gc %.1f",
				row.wantedWorstMs, row.pollWorstMs, row.warmWorstMs,
				row.commitWorstMs, row.wrapWorstMs,
				row.prepWorstMs, row.syncWorstMs, row.gcWorstMs))
		end

		local ok, err = pcall(function()
			local png = render.Capture({
				format = "png",
				x = 0, y = 0, w = ScrW(), h = ScrH(),
			})
			if png and #png > 0 then
				file.Write(string.format("%s/wp_%02d.png", outDir(), ST.index), png)
			end
		end)
		if not ok then
			say("capture failed: " .. tostring(err))
		end

		if ST.index >= #ST.waypoints then
			finish(nil)
		else
			beginWaypoint(ST.index + 1)
		end
	end)

	hook.Add("Think", "GTAVSelfTestStep", step)

	-- viewmodel sits on top of the holes
	hook.Add("PreDrawViewModel", "GTAVSelfTestNoViewModel", function()
		if ST.running then return true end
	end)


	local function begin()
		if ST.running then return end

		if not file.Exists(ARM_PATH, "DATA") then return end
		local raw = file.Read(ARM_PATH, "DATA")

		local arm = util.JSONToTable(raw or "")
		if not arm then
			say("arm file was not valid JSON")
			file.Delete(ARM_PATH, "DATA")
			return
		end

		local wps = arm.waypoints
		if type(wps) ~= "table" or #wps == 0 then
			say("arm file has no waypoints")
			file.Delete(ARM_PATH, "DATA")
			return
		end

		if not GTAV or not GTAV.MapData or not GTAV.MapData.current then
			say("no gtav map loaded yet - leaving the arm file in place")
			return
		end

		if type(arm.convars) == "table" then
			for k, v in SortedPairs(arm.convars) do
				local cv = GetConVar(k)
				if not cv then
					say("boot: unknown convar " .. tostring(k) .. " - ignored")
				else
					local n = tonumber(v)
					local okSet, errSet = pcall(function()
						if n and n == math.floor(n) then
							cv:SetInt(n)
						elseif n then
							cv:SetFloat(n)
						else
							cv:SetString(tostring(v))
						end
					end)
					if okSet then
						say(string.format("boot: convar %s = %s (now %s)",
							tostring(k), tostring(v),
							tostring(cv:GetString())))
					else
						say(string.format("boot: convar %s NOT applied: %s",
							tostring(k), tostring(errSet)))
					end
				end
			end
		end

		-- consumed now, a crash must not loop
		file.Delete(ARM_PATH, "DATA")

		ST.running = true
		ST.run = tostring(arm.run or os.date("%Y%m%d_%H%M%S"))
		ST.waypoints = wps
		ST.index = 0
		ST.rows = {}
		ST.holeRows = {}
		ST.startedAt = SysTime()
		ST.phase = "travel"

		file.CreateDir("gtav")
		file.CreateDir("gtav/selftest")
		file.CreateDir(outDir())

		file.Write(outDir() .. "/STARTED.txt",
			string.format("run %s on %s\n%d waypoints\n%s\n",
				ST.run, tostring(GTAV.MapData.name), #wps,
				os.date("!%Y-%m-%dT%H:%M:%SZ")))

		say(string.format("starting run %s on %s: %d waypoints -> %s",
			ST.run, tostring(GTAV.MapData.name), #wps, outDir()))
		beginWaypoint(1)
	end

	concommand.Add("gtav_st_client_begin", function()
		timer.Simple(0.5, begin)
	end)

	concommand.Add("gtav_selftest", function()
		timer.Simple(0, begin)
	end)


	local BOOT_LIMIT = 180        -- seconds before an armed run gives up
	local bootWaited = 0

	timer.Create("GTAVSelfTestBoot", 1, 0, function()
		if ST.running then
			timer.Remove("GTAVSelfTestBoot")
			return
		end

		if not file.Exists(ARM_PATH, "DATA") then return end

		local ready = IsValid(LocalPlayer())
			and GTAV and GTAV.MapData and GTAV.MapData.current

		if ready then
			timer.Remove("GTAVSelfTestBoot")
			say("boot: arm file found, parking the player and starting")
			RunConsoleCommand("gtav_st_park")
			timer.Simple(1, begin)
			return
		end

		bootWaited = bootWaited + 1
		if bootWaited >= BOOT_LIMIT then
			timer.Remove("GTAVSelfTestBoot")
			say(string.format(
				"boot: arm file present but no gtav map after %ds - giving up",
				BOOT_LIMIT))
			file.CreateDir("gtav")
			file.CreateDir("gtav/selftest")
			file.Write("gtav/selftest/LAST_FAIL.txt",
				string.format("no gtav map loaded after %ds\n%s\n",
					BOOT_LIMIT, os.date("!%Y-%m-%dT%H:%M:%SZ")))
		end
	end)

	CreateClientConVar("gtav_st_settle", "30", false, false, "Ticks of quiet before a waypoint is measured")
	CreateClientConVar("gtav_st_timeout", "1200", false, false, "Ticks to wait for the streamer before capturing anyway")
	CreateClientConVar("gtav_st_postol", "512", false, false, "Max units a body may land from its waypoint before the run fails")
end
