-- Note to anyone reading: Try to do things on the client if you can!!!
if (SERVER) then
	-- Initialize NikNaks
	require("niknaks")

	-- Create debug ConVar for server-side debugging
	local cv_debug = CreateConVar("rtx_rt_debug_sv", "0", FCVAR_ARCHIVE, "Enable debug messages for RT States on server")

	-- Helper function for debug printing
	local function DebugPrint(message)
		if cv_debug:GetBool() then
			print("[gmRTX-SV] " .. message)
		end
	end

	local function LoadServerSubAddons()
		-- First, send ALL necessary files to clients (shared AND client files)
		local allowCSLua = GetConVar("sv_allowcslua")
		if allowCSLua and allowCSLua:GetBool() then
			DebugPrint("sv_allowcslua is enabled, sending shared and client files to clients")
			
			-- Send shared files to clients (CRITICAL for multiplayer)
			local sharedFiles, _ = file.Find("remixlua/sh/*.lua", "LUA")
			if sharedFiles then
				DebugPrint("Sending " .. #sharedFiles .. " shared files to clients")
				for _, fileName in ipairs(sharedFiles) do
					local filePath = "remixlua/sh/" .. fileName
					AddCSLuaFile(filePath)
					DebugPrint("Sent shared file: " .. filePath)
				end
			end
			
			-- Send client files to clients
			local clientFolders = {"remixlua/cl/", "remixlua/cl/remixapi/"}
			for _, folder in ipairs(clientFolders) do
				local files, _ = file.Find(folder .. "*.lua", "LUA")
				if files then
					DebugPrint("Sending " .. #files .. " client files from " .. folder)
					for _, fileName in ipairs(files) do
						local filePath = folder .. fileName
						AddCSLuaFile(filePath)
						DebugPrint("Sent client file: " .. filePath)
					end
				end
			end
		else
			DebugPrint("sv_allowcslua is disabled, not sending files to clients")
		end

		-- Load server-side files on the server
		local foldersToLoad = {}
		
		-- Always load shared files on server
		table.insert(foldersToLoad, "remixlua/sh/")
		
		-- Always load server files 
		table.insert(foldersToLoad, "remixlua/sv/")
		
		for _, folder in ipairs(foldersToLoad) do
			local files, _ = file.Find(folder .. "*.lua", "LUA")
			
			if files then
				DebugPrint("Found " .. #files .. " files in " .. folder)
				
				for _, fileName in ipairs(files) do
					local filePath = folder .. fileName
					local success, err = pcall(include, filePath)
					
					if not success then
						DebugPrint("Warning: Failed to load sub-addon: " .. filePath .. " - Error: " .. tostring(err))
					else
						DebugPrint("Successfully loaded sub-addon: " .. filePath)
					end
				end
			else
				DebugPrint("No files found in " .. folder)
			end
		end
	end

	-- Load sub-addons immediately when server starts
	print("[gmRTX] - Initialising Server") 
	LoadServerSubAddons()

	function RTXLoadServer( ply )  
		-- This still runs when players spawn for any per-player initialization
		DebugPrint("Player " .. tostring(ply) .. " spawned")
	end 
	hook.Add( "PlayerInitialSpawn", "RTXReadyServer", RTXLoadServer)  

end

if SERVER then
    util.AddNetworkString("remix_spawn_rt_light")
    net.Receive("remix_spawn_rt_light", function(len, ply)
        if not IsValid(ply) then return end
        local pos = net.ReadVector()
        local ent = ents.Create("remix_rt_light")
        if not IsValid(ent) then return end
        ent:SetPos(pos)
        ent:SetAngles(Angle(0, ply:EyeAngles().y, 0))
        ent:Spawn()
        ent:Activate()
    end)

    util.AddNetworkString("remix_rt_light_apply")
    net.Receive("remix_rt_light_apply", function(len, ply)
        if not IsValid(ply) or not ply:IsAdmin() then return end
        local ent = net.ReadEntity()
        if not IsValid(ent) or ent:GetClass() ~= "remix_rt_light" then return end
        local t = net.ReadTable()
        if not istable(t) then return end

        local function clampf(v, minv, maxv)
            if type(v) ~= "number" then return nil end
            return math.Clamp(v, minv, maxv)
        end
        local function clampb(v) return v and true or false end
        local function clampstr(s)
            if type(s) ~= "string" then return "" end
            return string.sub(s, 1, 256)
        end
        local function clampvec(tbl)
            if istable(tbl) and tbl.x ~= nil and tbl.y ~= nil and tbl.z ~= nil then
                local x = tonumber(tbl.x) or 0
                local y = tonumber(tbl.y) or 0
                local z = tonumber(tbl.z) or 0
                return Vector(x, y, z)
            end
            return nil
        end

        if isstring(t.rtx_light_type) then
            local lt = string.lower(t.rtx_light_type)
            if lt == "sphere" or lt == "rect" or lt == "disk" or lt == "cylinder" or lt == "distant" or lt == "dome" then
                ent:SetNWString("rtx_light_type", lt)
            end
        end

        local v
        v = clampf(t.rtx_light_radius, 1, 200);        if v then ent:SetNWFloat("rtx_light_radius", v) end
        v = clampf(t.rtx_light_brightness, 0, 10);     if v then ent:SetNWFloat("rtx_light_brightness", v) end
        v = clampf(t.rtx_light_volumetric, 0, 5);      if v then ent:SetNWFloat("rtx_light_volumetric", v) end
        if t.rtx_light_shape_enabled ~= nil then ent:SetNWBool("rtx_light_shape_enabled", clampb(t.rtx_light_shape_enabled)) end
        v = clampf(t.rtx_light_shape_cone, 0, 180);    if v then ent:SetNWFloat("rtx_light_shape_cone", v) end
        v = clampf(t.rtx_light_shape_softness, 0, 1);  if v then ent:SetNWFloat("rtx_light_shape_softness", v) end
        v = clampf(t.rtx_light_shape_focus, 0, 10);    if v then ent:SetNWFloat("rtx_light_shape_focus", v) end
        v = clampf(t.rtx_light_xsize, 1, 400);         if v then ent:SetNWFloat("rtx_light_xsize", v) end
        v = clampf(t.rtx_light_ysize, 1, 400);         if v then ent:SetNWFloat("rtx_light_ysize", v) end
        v = clampf(t.rtx_light_xradius, 1, 200);       if v then ent:SetNWFloat("rtx_light_xradius", v) end
        v = clampf(t.rtx_light_yradius, 1, 200);       if v then ent:SetNWFloat("rtx_light_yradius", v) end
        v = clampf(t.rtx_light_axis_len, 1, 400);      if v then ent:SetNWFloat("rtx_light_axis_len", v) end
        v = clampf(t.rtx_light_distant_angle, 0, 10);  if v then ent:SetNWFloat("rtx_light_distant_angle", v) end
        local s = clampstr(t.rtx_light_dome_tex);       if s and s ~= "" then ent:SetNWString("rtx_light_dome_tex", s) end
        local col = clampvec(t.rtx_light_col);          if col then ent:SetNWVector("rtx_light_col", col) end
    end)
end