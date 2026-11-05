local bad = 0
for i = 1, #arg do
	local f, err = loadfile(arg[i])
	if f then
		print(string.format("ok   %s", arg[i]))
	else
		bad = bad + 1
		print(string.format("FAIL %s -> %s", arg[i], tostring(err)))
	end
end
print(string.format("%d checked, %d failed", #arg, bad))
os.exit(bad == 0 and 0 or 1)
