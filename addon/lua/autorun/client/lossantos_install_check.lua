local MESSAGE
if jit.arch ~= "x64" then
	MESSAGE = "gm_lossantos needs 64-bit Garry's Mod. In Steam: Garry's Mod > Properties > Betas > x86-64."
elseif not file.Exists("lossantos/installed.txt", "DATA") then
	MESSAGE = "This is only the base addon. Run the gm_lossantos installer to install the rest of the addon."
end
if not MESSAGE then return end
print("[gm_lossantos] " .. MESSAGE)

hook.Add("InitPostEntity", "GTAVInstallCheck", function()
	timer.Simple(5, function()
		chat.AddText(Color(90, 170, 255), "[gm_lossantos] ", color_white, MESSAGE)
	end)
end)
