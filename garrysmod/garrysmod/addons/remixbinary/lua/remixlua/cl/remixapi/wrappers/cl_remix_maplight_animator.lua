if not CLIENT then return end

local cv_enable = CreateClientConVar("rtx_maplight_anim", "1", true, false, "Enable client map light animator")
local cv_debug = CreateClientConVar("rtx_maplight_anim_debug", "0", true, false, "Debug logging for map light animator")
local cv_pattern_fps = CreateClientConVar("rtx_maplight_anim_pattern_fps", "10", true, false, "Frames per second for lightstyle pattern animations")

local function dprint(...)
	if cv_debug:GetBool() then print("[RTX-MapLightAnimator]", ...) end
end

-- Source Engine default lightstyle patterns (indices 0-12)
local g_DefaultLightstyles = {
	[0] = "m",                                                 -- normal
	[1] = "mmnmmommommnonmmonqnmmo",                          -- FLICKER (first variety)
	[2] = "abcdefghijklmnopqrstuvwxyzyxwvutsrqponmlkjihgfedcba", -- SLOW STRONG PULSE
	[3] = "mmmmmaaaaammmmmaaaaaabcdefgabcdefg",                -- CANDLE (first variety)
	[4] = "mamamamamama",                                      -- FAST STROBE
	[5] = "jklmnopqrstuvwxyzyxwvutsrqponmlkj",                -- GENTLE PULSE 1
	[6] = "nmonqnmomnmomomno",                                -- FLICKER (second variety)
	[7] = "mmmaaaabcdefgmmmmaaaammmaamm",                     -- CANDLE (second variety)
	[8] = "mmmaaammmaaammmabcdefaaaammmmabcdefmmmaaaa",       -- CANDLE (third variety)
	[9] = "aaaaaaaazzzzzzzz",                                 -- SLOW STROBE
	[10] = "mmamammmmammamamaaamammma",                       -- FLUORESCENT FLICKER
	[11] = "abcdefghijklmnopqrrqponmlkjihgfedcba",            -- SLOW PULSE NOT FADE TO BLACK
	[12] = "mmnnmmnnnmmnn",                                   -- UNDERWATER LIGHT MUTATION
}

-- Convert lightstyle character to brightness multiplier (a=bright, z=dark)
-- Source uses: 'a' = 0 (dimmest), 'z' = 25 (brightest), 'm' = 12 (normal)
-- We normalize: 'a' = 1.0 (brightest), 'm' = ~0.5, 'z' = 0.0 (dimmest)
local function lightstyleCharToMul(char)
	local byte = string.byte(char)
	if byte >= 97 and byte <= 122 then -- a-z
		-- Map a=1.0 down to z=0.0 (inverted from Source's convention for our brightness multiplier)
		return (122 - byte) / 25.0
	end
	return 0.5 -- default to 'm' = middle brightness
end

-- Active pattern animators by targetname
local activePatterns = {} -- name -> { pattern = string, index = number, lastUpdate = number }

-- Desired state cache by targetname (lowercased)
local desired = {} -- name -> { has = true, enabled = boolean, mul = number, pattern = string }

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

-- Pattern animation think - updates lights with active flicker patterns
local lastThinkDebug = 0
hook.Add("Think", "rtx_maplight_pattern_animator", function()
	if not cv_enable:GetBool() then return end
	if not Light2RTX or not Light2RTX.GetEntriesByTargetName then return end
	
	local fps = cv_pattern_fps:GetFloat()
	if fps <= 0 then fps = 10 end
	local frameTime = 1.0 / fps
	local now = CurTime()
	
	-- Debug output every 5 seconds
	if cv_debug:GetBool() and now - lastThinkDebug > 5 then
		local count = 0
		for _ in pairs(activePatterns) do count = count + 1 end
		if count > 0 then
			dprint(string.format("Pattern animator running: %d active patterns", count))
		end
		lastThinkDebug = now
	end
	
	for name, anim in pairs(activePatterns) do
		if anim and anim.pattern then
			-- Check if it's time for next frame
			if now - anim.lastUpdate >= frameTime then
				anim.lastUpdate = now
				
				-- Get current character from pattern
				local char = string.sub(anim.pattern, anim.index, anim.index)
				if char == "" then
					anim.index = 1
					char = string.sub(anim.pattern, 1, 1)
				end
				
				-- Convert to brightness multiplier
				local mul = lightstyleCharToMul(char)
				
				-- Apply to all lights with this targetname
				local entries = Light2RTX.GetEntriesByTargetName(name)
				if cv_debug:GetBool() and #entries > 0 then
					dprint(string.format("Pattern '%s' char '%s' -> mul=%.2f, updating %d lights", name, char, mul, #entries))
				end
				for _, entry in ipairs(entries) do
					entry.animMul = mul
					if Light2RTX.UpdateEntry then Light2RTX.UpdateEntry(entry) end
				end
				
				-- Advance to next character
				anim.index = anim.index + 1
				if anim.index > #anim.pattern then
					anim.index = 1
				end
			end
		end
	end
end)

local function setDesired(name, enabled, mul, pattern)
	local lname = string.lower(name)
	desired[lname] = desired[lname] or { has = true, enabled = true, mul = 1.0 }
	desired[lname].has = true
	if enabled ~= nil then desired[lname].enabled = enabled and true or false end
	if mul ~= nil then desired[lname].mul = tonumber(mul) or desired[lname].mul end
	if pattern ~= nil then desired[lname].pattern = pattern end
end

-- Start or update a pattern animator for a light
local function startPattern(name, pattern)
	local lname = string.lower(name)
	if not pattern or pattern == "" or pattern == "m" then
		-- Static light, stop any existing pattern
		activePatterns[lname] = nil
		return
	end
	
	-- Convert numeric index to pattern string if needed
	local patternStr = pattern
	local idx = tonumber(pattern)
	if idx and g_DefaultLightstyles[idx] then
		patternStr = g_DefaultLightstyles[idx]
	end
	
	-- Ensure we have a string pattern
	if type(patternStr) ~= "string" or patternStr == "" or patternStr == "m" then
		activePatterns[lname] = nil
		return
	end
	
	activePatterns[lname] = {
		pattern = patternStr,
		index = 1,
		lastUpdate = CurTime()
	}
	print(string.format("[RTX-MapLightAnimator] Started pattern for '%s': %s (length=%d)", name, patternStr, #patternStr))
end

-- Stop pattern animator for a light
local function stopPattern(name)
	local lname = string.lower(name)
	activePatterns[lname] = nil
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
	elseif etype == "pattern" or etype == "setpattern" then
		-- Pattern can be: numeric index (0-12), pattern string, or single character brightness
		local s = tostring(payload or "")
		
		-- Try as numeric index first (0-12 for default patterns)
		local idx = tonumber(s)
		if idx and g_DefaultLightstyles[idx] then
			local pattern = g_DefaultLightstyles[idx]
			setDesired(lname, true, nil, pattern)
			startPattern(lname, pattern)
			return
		end
		
		-- Try as brightness multiplier
		local mul = parseBrightnessPayload(s)
		if mul then
			stopPattern(lname)
			setDesired(lname, mul > 0, mul, "m")
			applyImmediateOrLatch(lname, function()
				if Light2RTX.SetBrightnessMulByTargetName then Light2RTX.SetBrightnessMulByTargetName(lname, mul) end
			end)
			return
		end
		
		-- Treat as custom pattern string (if it looks like one)
		if #s > 0 and string.match(s, "^[a-z]+$") then
			setDesired(lname, true, nil, s)
			startPattern(lname, s)
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

-- Hook to receive pattern start requests from map lights loader
hook.Add("RTXMapLight_StartPattern", "rtx_maplight_animator_pattern_hook", function(name, style)
	if not cv_enable:GetBool() then return end
	if not name or not style then return end
	
	-- startPattern will handle numeric index conversion
	startPattern(name, style)
end)


