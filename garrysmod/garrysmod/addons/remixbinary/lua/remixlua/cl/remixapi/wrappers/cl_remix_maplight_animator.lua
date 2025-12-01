if not CLIENT then return end

local cv_enable = CreateClientConVar("rtx_maplight_anim", "1", true, false, "Enable client map light animator")
local cv_debug = CreateClientConVar("rtx_maplight_anim_debug", "0", true, false, "Debug logging for map light animator")

local function dprint(...)
	if cv_debug:GetBool() then print("[RTX-MapLightAnimator]", ...) end
end

-- Desired state cache by targetname (lowercased)
local desired = {} -- name -> { has = true, enabled = boolean, mul = number }

local function applyDesiredToEntries(name)
	if not Light2RTX or not Light2RTX.GetEntriesByTargetName then return false end
	local lname = string.lower(name)
	local st = desired[lname]
	if not st or not st.has then return false end
	local entries = Light2RTX.GetEntriesByTargetName(lname)
	local applied = false
	for _, entry in ipairs(entries) do
		entry.animEnabled = st.enabled and true or false
		entry.animMul = tonumber(st.mul) or (st.enabled and 1.0 or 0.0)
		if Light2RTX.UpdateEntry then Light2RTX.UpdateEntry(entry) end
		applied = true
	end
	return applied
end

-- Periodically try to apply latched states for names that didn't exist yet when we received I/O
timer.Create("rtx_maplights_anim_flush", 0.5, 0, function()
	if not cv_enable:GetBool() then return end
	if not Light2RTX then return end
	for name, st in pairs(desired) do
		if st and st.has then
			local ok = applyDesiredToEntries(name)
			if ok then dprint("Applied latched state to", name) end
		end
	end
end)

local function setDesired(name, enabled, mul)
	local lname = string.lower(name)
	desired[lname] = desired[lname] or { has = true, enabled = true, mul = 1.0 }
	desired[lname].has = true
	if enabled ~= nil then desired[lname].enabled = enabled and true or false end
	if mul ~= nil then desired[lname].mul = tonumber(mul) or desired[lname].mul end
end

local function applyImmediateOrLatch(name, fn)
	if not cv_enable:GetBool() then return end
	if Light2RTX and Light2RTX.GetEntriesByTargetName then
		fn()
	else
		-- Light2RTX not ready yet; store desired state and the flush timer will apply it
	end
end

local function parseBrightnessPayload(payload)
	local v = tonumber(payload)
	if not v then return nil end
	-- Accept 0..1, 0..10, 0..100, or 0..255 inputs and normalize to 0..1 multiplier
	if v > 10 then v = v / 100.0 end
	v = math.Clamp(v, 0, 10)
	if v > 1.0 then v = v / 10.0 end
	return v
end

local function handleEvent(targetname, classname, etype, payload)
	if not isstring(targetname) or targetname == "" then return end
	local lname = string.lower(targetname)
	etype = string.lower(etype or "")
	if etype == "on" then
		setDesired(lname, true, 1.0)
		applyImmediateOrLatch(lname, function()
			if Light2RTX.SetEnabledByTargetName then Light2RTX.SetEnabledByTargetName(lname, true) end
		end)
	elseif etype == "off" then
		setDesired(lname, false, 0.0)
		applyImmediateOrLatch(lname, function()
			if Light2RTX.SetEnabledByTargetName then Light2RTX.SetEnabledByTargetName(lname, false) end
		end)
	elseif etype == "toggle" then
		-- Toggle: if we have a desired entry, flip it; otherwise forward to Light2RTX
		local cur = desired[lname]
		if cur and cur.has then
			local newOn = not cur.enabled
			setDesired(lname, newOn, newOn and 1.0 or 0.0)
		end
		applyImmediateOrLatch(lname, function()
			if Light2RTX.ToggleByTargetName then Light2RTX.ToggleByTargetName(lname) end
		end)
	elseif etype == "brightness" then
		local mul = parseBrightnessPayload(payload)
		if not mul then return end
		setDesired(lname, mul > 0, mul)
		applyImmediateOrLatch(lname, function()
			if Light2RTX.SetBrightnessMulByTargetName then Light2RTX.SetBrightnessMulByTargetName(lname, mul) end
		end)
	elseif etype == "pattern" then
		-- Basic support: treat numeric payload as brightness percent; otherwise known tokens
		local mul = parseBrightnessPayload(payload)
		if mul then
			setDesired(lname, mul > 0, mul)
			applyImmediateOrLatch(lname, function()
				if Light2RTX.SetBrightnessMulByTargetName then Light2RTX.SetBrightnessMulByTargetName(lname, mul) end
			end)
			return
		end
		local s = string.lower(tostring(payload or ""))
		local map = { ["0"] = 1.0, ["1"] = 0.0, ["2"] = 0.5, ["3"] = 0.25, ["4"] = 0.75, ["a"] = 1.0, ["b"] = 0.75, ["c"] = 0.5, ["d"] = 0.25 }
		local m = map[s]
		if m ~= nil then
			setDesired(lname, m > 0, m)
			applyImmediateOrLatch(lname, function()
				if Light2RTX.SetBrightnessMulByTargetName then Light2RTX.SetBrightnessMulByTargetName(lname, m) end
			end)
		else
			-- Fallback: toggle
			applyImmediateOrLatch(lname, function()
				if Light2RTX.ToggleByTargetName then Light2RTX.ToggleByTargetName(lname) end
			end)
		end
	end
end

net.Receive("rtx_maplight_io", function()
	if not cv_enable:GetBool() then return end
	local targetname = net.ReadString()
	local classname = net.ReadString()
	local etype = net.ReadString()
	local payload = net.ReadString()
	dprint("event", targetname, classname, etype, payload)
	handleEvent(targetname, classname, etype, payload)
end)


