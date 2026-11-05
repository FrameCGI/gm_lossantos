if jit.arch ~= "x64" then return end  -- lossantos_install_check.lua tells the player

GTAV = GTAV or {}
GTAV.Content = GTAV.Content or {}
local CT = GTAV.Content
if CT.mounted then return end         -- once per Lua state (auto-refresh)

-- not addons/, gmod ignores local gmas there
local DIR = "lossantos/content/"

local function list()
	local names = {}
	local txt = file.Read(DIR .. "chunks.txt", "DATA")
	if txt then
		for line in string.gmatch(txt, "[^\r\n]+") do
			if line:match("^lossantos_content_[%w_]+%.gma$") then names[#names + 1] = line end
		end
	end
	if #names == 0 then
		local found = file.Find(DIR .. "lossantos_content_*.gma", "DATA")
		for _, n in ipairs(found or {}) do names[#names + 1] = n end
		table.sort(names)
	end
	return names
end

local t0 = SysTime()
local ok, files, failed = 0, 0, {}
for _, name in ipairs(list()) do
	local good, mounted = game.MountGMA("data/" .. DIR .. name)
	if good then
		ok = ok + 1
		files = files + (istable(mounted) and #mounted or 0)
	else
		failed[#failed + 1] = name
	end
end
CT.mounted = true
CT.chunks, CT.files, CT.failed = ok, files, failed
print(string.format("[GTAV] content: mounted %d archive(s), %d files in %.0f ms%s",
	ok, files, (SysTime() - t0) * 1000,
	#failed > 0 and (" - FAILED: " .. table.concat(failed, ", ")) or ""))
