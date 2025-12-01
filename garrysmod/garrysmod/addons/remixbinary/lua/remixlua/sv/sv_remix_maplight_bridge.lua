if not SERVER then return end

-- Bridge Source I/O for map light entities to clients so they can animate RTX lights

CreateConVar("rtx_maplight_bridge", "1", FCVAR_ARCHIVE, "Enable server->client bridge for map light I/O")
local cv_debug = CreateConVar("rtx_maplight_bridge_debug", "0", FCVAR_ARCHIVE, "Debug logging for map light I/O bridge")

util.AddNetworkString("rtx_maplight_io")

local function dprint(...)
	if cv_debug:GetBool() then
		print("[RTX-MapLightBridge]", ...)
	end
end

local handledClasses = {
	["light"] = true,
	["light_spot"] = true,
	["light_dynamic"] = true,
	["light_environment"] = true,
}

local function normalizeInputName(name)
	if not isstring(name) then return "" end
	return string.lower(name)
end

hook.Add("AcceptInput", "rtx_maplight_bridge_accept", function(ent, inputName, activator, caller, value)
	if GetConVar("rtx_maplight_bridge"):GetBool() ~= true then return end
	if not IsValid(ent) then return end
	local classname = string.lower(ent:GetClass() or "")
	if not handledClasses[classname] then return end
	local targetname = ent:GetName() or ""
	if targetname == "" then return end
	local inName = normalizeInputName(inputName)
	-- Recognized inputs only
	local typ = nil
	if inName == "turnon" or inName == "enable" then
		typ = "on"
	elseif inName == "turnoff" or inName == "disable" then
		typ = "off"
	elseif inName == "toggle" then
		typ = "toggle"
	elseif inName == "setpattern" or inName == "fadetopattern" or inName == "setlightstyle" or inName == "setdefaultstyle" then
		typ = "pattern"
	elseif inName == "setbrightness" or inName == "setintensity" or inName == "setlightvalue" or inName == "brightness" then
		typ = "brightness"
	else
		return
	end

	local payload = tostring(value or "")
	dprint(string.format("%s[%s] %s value='%s'", targetname, classname, typ, payload))
	net.Start("rtx_maplight_io")
	net.WriteString(targetname)
	net.WriteString(classname)
	net.WriteString(typ)
	net.WriteString(payload)
	net.Broadcast()
end)


