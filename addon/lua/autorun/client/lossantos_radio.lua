if jit.arch ~= "x64" then return end  -- lossantos_install_check.lua tells the player

LosSantosRadio = LosSantosRadio or {}
local R = LosSantosRadio
R.Stations = R.Stations or {}
R.Order = R.Order or {}

local ROOT = "sound/lossantos_radio/"
local MAT_DIR = "lossantos_radio/"

CreateClientConVar("lossantos_radio_volume", "0.6", true, false, "Radio volume (0-1)", 0, 1)

-- hud.ytd logo sheets, 128 px cells
R.AtlasSheets = {
	main = { file = "gtav_radio_stations_texture_512.png", cols = 4, rows = 4 },
	s02 = { file = "gta_radio_stations_texture02_512.png", cols = 2, rows = 1 },
}
-- {column, row [, sheet]}
R.AtlasCells = {
	radio_06_country = { 0, 0 }, radio_08_mexican = { 1, 0 },
	radio_04_punk = { 2, 0 }, radio_01_class_rock = { 3, 0 },
	radio_02_pop = { 0, 1 }, radio_07_dance_01 = { 1, 1 },
	radio_11_talk_02 = { 2, 1 }, radio_16_silverlake = { 3, 1 },
	radio_09_hiphop_old = { 0, 2 }, radio_18_90s_rock = { 1, 2 },
	radio_03_hiphop_new = { 2, 2 }, radio_12_reggae = { 3, 2 },
	radio_13_jazz = { 0, 3 }, radio_14_dance_02 = { 1, 3 },
	radio_15_motown = { 2, 3 },
	off = { 3, 3 },
	radio_17_funk = { 0, 0, "s02" }, radio_05_talk_01 = { 1, 0, "s02" },
}

R.SharedBanks = { radio_adverts = true, radio_news = true }


local function prettify(stem)
	stem = stem:gsub("_", " ")
	return (stem:gsub("(%a)([%w']*)", function(a, b) return a:upper() .. b end))
end

local function classify(stem)
	if stem:match("^id_%d+") then return "id" end
	if stem:match("^mono_solo_%d+") then return "solo" end
	if stem == "general" then return "dj" end
	if stem == "intro" or stem == "time" or stem == "to" then return "skip" end
	return "song"
end

local function sortByPath(list)
	table.sort(list, function(a, b) return a.path < b.path end)
end

local function loadShared()
	R.Shared = { adverts = {}, news = {} }
	for bank, key in pairs({ radio_adverts = "adverts", radio_news = "news" }) do
		local txt = file.Read(ROOT .. bank .. "/manifest.json", "GAME")
		local m = txt and util.JSONToTable(txt)
		for stem, clips in pairs(m and m.files or {}) do
			-- skip the long tv show audio
			if not stem:match("^mono_tv_") then
				for _, c in ipairs(clips) do
					local dur = tonumber(c.seconds) or 0
					if dur > 3 and dur < 240 then
						table.insert(R.Shared[key], { path = ROOT .. bank .. "/" .. c.path, dur = dur })
					end
				end
			end
		end
		sortByPath(R.Shared[key])
	end
end

function R.Discover()
	R.Stations, R.Order = {}, {}
	loadShared()
	local _, dirs = file.Find(ROOT .. "*", "GAME")
	for _, d in ipairs(dirs or {}) do
		local txt = not R.SharedBanks[d] and file.Read(ROOT .. d .. "/manifest.json", "GAME")
		local m = txt and util.JSONToTable(txt)
		if m and m.files then
			local st = { id = d, name = m.name or prettify(d), kind = m.kind or "music",
						 songs = {}, ids = {}, dj = {}, solo = {} }
			local titles = m.titles or {}
			for stem, clips in pairs(m.files) do
				local kind = classify(stem)
				local t = titles[stem]
				for _, c in ipairs(clips) do
					local e = { path = ROOT .. d .. "/" .. c.path, dur = tonumber(c.seconds) or 0 }
					if kind == "song" and st.kind ~= "mix" then
						e.title = (t and t.title) or prettify(stem)
						e.artist = t and t.artist ~= "" and t.artist or nil
					end
					if e.dur > 0.5 then
						if kind == "song" and e.dur > 60 then st.songs[#st.songs + 1] = e
						elseif kind == "id" then st.ids[#st.ids + 1] = e
						elseif kind == "dj" then st.dj[#st.dj + 1] = e
						elseif kind == "solo" then st.solo[#st.solo + 1] = e end
					end
				end
			end
			if #st.songs > 0 then
				for _, list in pairs({ st.songs, st.ids, st.dj, st.solo }) do sortByPath(list) end
				R.Stations[d] = st
				R.Order[#R.Order + 1] = d
			end
		end
	end
	table.sort(R.Order)
	R.BuildSchedules()
	print(string.format("[lossantos radio] %d station(s): %s", #R.Order, table.concat(R.Order, ", ")))
end


-- seeded, leaves math.random alone
local function rng(seed)
	local s = seed % 2147483647
	if s <= 0 then s = s + 2147483646 end
	return function(n)
		s = (s * 48271) % 2147483647
		return (s % n) + 1
	end
end

local function seedOf(str)
	local h = 5381
	for i = 1, #str do h = (h * 33 + str:byte(i)) % 2147483647 end
	return h
end

function R.BuildSchedules()
	for id, st in pairs(R.Stations) do
		local rand = rng(seedOf(id))
		local songs = {}
		for i, s in ipairs(st.songs) do songs[i] = s end
		-- mix and talk stay in file order
		if st.kind == "music" then
			for i = #songs, 2, -1 do
				local j = rand(i)
				songs[i], songs[j] = songs[j], songs[i]
			end
		end
		local ads, news = R.Shared.adverts, R.Shared.news
		local seq, t = {}, 0
		local function add(e)
			seq[#seq + 1] = { e = e, start = t }
			t = t + e.dur
		end
		for _, song in ipairs(songs) do
			if st.kind == "music" then
				-- 1-2 ads, 3 news, 4-9 id, 10-15 dj, 16-17 solo
				local roll = rand(20)
				if roll <= 2 and #ads > 0 then         -- ad break: id, 1-2 ads
					if #st.ids > 0 then add(st.ids[rand(#st.ids)]) end
					add(ads[rand(#ads)])
					if rand(2) == 1 then add(ads[rand(#ads)]) end
				elseif roll == 3 and #news > 0 then
					add(news[rand(#news)])
				elseif roll <= 9 and #st.ids > 0 then add(st.ids[rand(#st.ids)])
				elseif roll <= 15 and #st.dj > 0 then add(st.dj[rand(#st.dj)])
				elseif roll <= 17 and #st.solo > 0 then add(st.solo[rand(#st.solo)]) end
			elseif st.kind == "talk" then              -- between shows: id + ads
				if #st.ids > 0 then add(st.ids[rand(#st.ids)]) end
				for _ = 1, (#ads > 0 and 2 or 0) do add(ads[rand(#ads)]) end
			end
			add(song)
		end
		st.seq, st.cycle = seq, t
		st.offset = seedOf(id .. "#") % math.max(1, math.floor(t))   -- stations not in lockstep
	end
end

-- real clock, tuning in lands mid song
local clockBase = os.time() - SysTime()
local function radioNow() return clockBase + SysTime() end

local function locate(st)
	local pos = (radioNow() + st.offset) % st.cycle
	local lo, hi = 1, #st.seq
	while lo < hi do
		local mid = math.floor((lo + hi + 1) / 2)
		if st.seq[mid].start <= pos then lo = mid else hi = mid - 1 end
	end
	return lo, pos - st.seq[lo].start
end


R.Current = R.Current or nil      -- station id or nil
R.Channel = R.Channel or nil
R.Seg = nil
R.Token = 0                       -- invalidates stale PlayFile callbacks

local function stopChannel()
	if IsValid(R.Channel) then R.Channel:Stop() end
	R.Channel = nil
end

local function volume()
	return GetConVar("lossantos_radio_volume"):GetFloat()
end

local function playSegment(st, idx, offset)
	R.Token = R.Token + 1
	local token = R.Token
	local seg = st.seq[idx]
	R.Seg = { st = st.id, idx = idx, e = seg.e, endsAt = SysTime() + (seg.e.dur - offset) }
	sound.PlayFile(seg.e.path, "noplay", function(ch, errId, errName)
		if token ~= R.Token then
			if IsValid(ch) then ch:Stop() end
			return
		end
		if not IsValid(ch) then
			print("[lossantos radio] cannot play " .. seg.e.path .. ": " .. tostring(errName))
			return
		end
		stopChannel()
		R.Channel = ch
		ch:SetVolume(volume())
		if offset > 0.05 then ch:SetTime(offset) end
		ch:Play()
	end)
end

function R.Tune(id, silent)
	local st = id and R.Stations and R.Stations[id]
	stopChannel()
	R.Token = R.Token + 1
	R.Seg = nil
	R.Current = st and id or nil
	if st then
		local idx, off = locate(st)
		playSegment(st, idx, off)
	end
	if not silent then R.Banner = { t = SysTime() } end
end

local function cycleStation(dir)
	if not R.Order or #R.Order == 0 then return end
	local list = { false }                          -- false = Radio Off
	for _, id in ipairs(R.Order) do list[#list + 1] = id end
	local cur = 1
	for i, v in ipairs(list) do if v == (R.Current or false) then cur = i end end
	local nxt = list[((cur - 1 + dir) % #list) + 1]
	R.Tune(nxt or nil)
end

function R.Next() cycleStation(1) end
function R.Prev() cycleStation(-1) end

hook.Add("Think", "LosSantosRadio_Advance", function()
	if not R.Current or not R.Seg then return end
	if SysTime() >= R.Seg.endsAt - 0.02 then
		local st = R.Stations[R.Current]
		if st then
			local idx = (R.Seg.idx % #st.seq) + 1
			playSegment(st, idx, 0)
		end
	end
	if IsValid(R.Channel) then R.Channel:SetVolume(volume()) end
end)


surface.CreateFont("LosSantosRadioStation", { font = "Roboto", size = ScreenScale(11), weight = 700 })
surface.CreateFont("LosSantosRadioTrack", { font = "Roboto", size = ScreenScale(7), weight = 500 })

local atlasMats = {}
local function atlas(key)
	if atlasMats[key] == nil then
		local sh = R.AtlasSheets[key]
		local m = sh and Material(MAT_DIR .. sh.file, "smooth mips")
		atlasMats[key] = (m and not m:IsError()) and m or false
	end
	return atlasMats[key] or nil
end

local function drawLogo(id, x, y, size, col)
	local cell = R.AtlasCells[id or "off"]
	if not cell then return false end
	local key = cell[3] or "main"
	local sh, m = R.AtlasSheets[key], atlas(key)
	if not (sh and m) then return false end
	surface.SetMaterial(m)
	surface.SetDrawColor(col)
	local cw, ch = 1 / sh.cols, 1 / sh.rows
	local u0, v0 = cell[1] * cw, cell[2] * ch
	local hu, hv = 0.5 / (128 * sh.cols), 0.5 / (128 * sh.rows)   -- half-texel inset
	surface.DrawTexturedRectUV(x - size / 2, y - size / 2, size, size,
		u0 + hu, v0 + hv, u0 + cw - hu, v0 + ch - hv)
	return true
end

local function drawWedge(cx, cy, r0, r1, a0, a1, col)
	draw.NoTexture()
	surface.SetDrawColor(col)
	local steps = math.max(2, math.ceil((a1 - a0) / math.rad(6)))
	for i = 0, steps - 1 do
		local s = a0 + (a1 - a0) * i / steps
		local e = a0 + (a1 - a0) * (i + 1) / steps
		surface.DrawPoly({
			{ x = cx + math.cos(s) * r0, y = cy + math.sin(s) * r0 },
			{ x = cx + math.cos(s) * r1, y = cy + math.sin(s) * r1 },
			{ x = cx + math.cos(e) * r1, y = cy + math.sin(e) * r1 },
			{ x = cx + math.cos(e) * r0, y = cy + math.sin(e) * r0 },
		})
	end
end

R.Wheel = R.Wheel or { open = false }

function R.OpenWheel()
	if R.Wheel.open then return end
	R.Wheel = { open = true, sel = nil }
	gui.EnableScreenClicker(true)
	input.SetCursorPos(ScrW() / 2, ScrH() / 2)
end

function R.CloseWheel(apply)
	if not R.Wheel.open then return end
	local sel = R.Wheel.sel
	R.Wheel.open = false
	gui.EnableScreenClicker(false)
	if apply and sel ~= nil then
		R.Tune(sel ~= false and sel or nil)
	end
end

local function wheelEntries()
	local list = { false }
	for _, id in ipairs(R.Order or {}) do list[#list + 1] = id end
	return list
end

local function nowPlaying()
	if not R.Seg or not R.Current or not R.Seg.e then return nil end
	return R.Seg.e.title, R.Seg.e.artist
end

hook.Add("HUDPaint", "LosSantosRadio_HUD", function()
	local W, H = ScrW(), ScrH()

	if R.Wheel.open then
		local list = wheelEntries()
		local n = #list
		local cx, cy = W / 2, H / 2
		local r1 = math.min(W, H) * 0.36
		local r0 = r1 * 0.42
		local mx, my = input.GetCursorPos()
		local dx, dy = mx - cx, my - cy
		local span = 2 * math.pi / n
		local start = -math.pi / 2 - span / 2
		if dx * dx + dy * dy > (r0 * 0.6) ^ 2 then
			local a = (math.atan2(dy, dx) - start) % (2 * math.pi)
			R.Wheel.sel = list[math.floor(a / span) + 1]
		end
		for i, id in ipairs(list) do
			local a0 = start + (i - 1) * span + 0.012
			local a1 = start + i * span - 0.012
			local selected = (R.Wheel.sel == id)
			drawWedge(cx, cy, r0, r1, a0, a1,
				selected and Color(255, 255, 255, 70) or Color(0, 0, 0, 165))
			local am = (a0 + a1) / 2
			local lx = cx + math.cos(am) * (r0 + r1) / 2
			local ly = cy + math.sin(am) * (r0 + r1) / 2
			local size = math.min((r1 - r0) * 0.62, (r0 + r1) / 2 * span * 0.8)
			local col = selected and Color(255, 255, 255) or Color(150, 150, 150)
			if not drawLogo(id or "off", lx, ly, size, col) then
				local st = id and R.Stations[id]
				draw.SimpleText(st and st.name or "Radio Off", "LosSantosRadioTrack", lx, ly, col,
					TEXT_ALIGN_CENTER, TEXT_ALIGN_CENTER)
			end
		end
		local sel = R.Wheel.sel
		local st = sel and R.Stations[sel]
		draw.SimpleText(st and st.name or (sel == false and "Radio Off" or ""), "LosSantosRadioStation",
			cx, cy - 8, color_white, TEXT_ALIGN_CENTER, TEXT_ALIGN_CENTER)
		if sel and sel == R.Current then
			local title, artist = nowPlaying()
			if title then
				draw.SimpleText(title, "LosSantosRadioTrack", cx, cy + 22,
					Color(230, 230, 230), TEXT_ALIGN_CENTER, TEXT_ALIGN_CENTER)
			end
			if artist then
				draw.SimpleText(artist, "LosSantosRadioTrack", cx, cy + 22 + ScreenScale(7),
					Color(180, 180, 180), TEXT_ALIGN_CENTER, TEXT_ALIGN_CENTER)
			end
		end
		return
	end

	local b = R.Banner
	if b then
		local age = SysTime() - b.t
		if age > 4 then R.Banner = nil return end
		local alpha = 255 * math.Clamp(4 - age, 0, 1)
		local st = R.Current and R.Stations[R.Current]
		local y = H * 0.06
		draw.SimpleTextOutlined(st and st.name or "Radio Off", "LosSantosRadioStation", W / 2, y,
			Color(255, 255, 255, alpha), TEXT_ALIGN_CENTER, TEXT_ALIGN_CENTER, 1,
			Color(0, 0, 0, alpha * 0.8))
		local title, artist
		if st then title, artist = nowPlaying() end
		if title then
			draw.SimpleTextOutlined(title, "LosSantosRadioTrack", W / 2, y + ScreenScale(11),
				Color(230, 230, 230, alpha), TEXT_ALIGN_CENTER, TEXT_ALIGN_CENTER, 1,
				Color(0, 0, 0, alpha * 0.8))
		end
		if artist then
			draw.SimpleTextOutlined(artist, "LosSantosRadioTrack", W / 2, y + ScreenScale(11) + ScreenScale(7),
				Color(190, 190, 190, alpha), TEXT_ALIGN_CENTER, TEXT_ALIGN_CENTER, 1,
				Color(0, 0, 0, alpha * 0.8))
		end
	end
end)

local function bindDown(bind)
	local key = input.LookupBinding(bind)
	if not key then return false end
	local code = input.GetKeyCode(key)
	return code and code > 0 and input.IsButtonDown(code) or false
end

local SELECT_BINDS = { "invnext", "invprev", "lastinv" }
hook.Add("PlayerBindPress", "LosSantosRadio_WeaponSelect", function(_, bind, pressed)
	if not pressed or not bind then return end
	local b = bind:lower()
	if b:match("^slot%d") then R.SelectPending = SysTime() return end
	for _, s in ipairs(SELECT_BINDS) do
		if b:find(s, 1, true) then R.SelectPending = SysTime() return end
	end
end)

local prev = {}
local lastWep = nil
hook.Add("Think", "LosSantosRadio_WalkmanInput", function()
	local ply = LocalPlayer()
	local wep = IsValid(ply) and ply:GetActiveWeapon()
	if wep ~= lastWep then
		lastWep = wep
		R.SelectPending = nil
		-- all held after a switch, the selecting click
		prev = { attack = true, attack2 = true, reload = true }
	end
	if R.SelectPending and SysTime() - R.SelectPending > 3.5 then R.SelectPending = nil end
	local holding = IsValid(wep) and wep:GetClass() == "weapon_lossantos_walkman"
	local blocked = gui.IsGameUIVisible() or IsValid(vgui.GetKeyboardFocus())
	local now = {
		attack = holding and not blocked and bindDown("+attack"),
		attack2 = holding and not blocked and bindDown("+attack2"),
		reload = holding and not blocked and bindDown("+reload"),
	}
	if now.reload and not prev.reload then R.OpenWheel() R.Wheel.byReload = true end
	if not now.reload and prev.reload and R.Wheel.byReload then
		R.Wheel.byReload = false
		R.CloseWheel(true)
	end
	if not R.Wheel.open then
		local attackEdge = now.attack and not prev.attack
		local attack2Edge = now.attack2 and not prev.attack2
		if R.SelectPending and (attackEdge or attack2Edge) then
			R.SelectPending = nil                     -- the selector's click, not ours
		else
			if attackEdge then R.Next() end
			if attack2Edge then R.Prev() end
		end
	end
	prev = now
end)


concommand.Add("+lossantos_radio_wheel", function() R.OpenWheel() end)
concommand.Add("-lossantos_radio_wheel", function() R.CloseWheel(true) end)
concommand.Add("lossantos_radio_next", function() R.Next() end)
concommand.Add("lossantos_radio_prev", function() R.Prev() end)
concommand.Add("lossantos_radio_off", function() R.Tune(nil) end)
concommand.Add("lossantos_radio_rescan", function() R.Discover() end)

hook.Add("InitPostEntity", "LosSantosRadio_Init", function()
	R.Discover()
end)

if IsValid(LocalPlayer()) then R.Discover() end
