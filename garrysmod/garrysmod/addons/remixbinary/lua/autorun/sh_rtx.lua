
-- Load shared files immediately in shared autorun
local function LoadSharedFiles()
	local files, _ = file.Find("remixlua/sh/*.lua", "LUA")
	
	if files then
		print("[gmRTX-SH] Loading " .. #files .. " shared files...")
		
		for _, fileName in ipairs(files) do
			local filePath = "remixlua/sh/" .. fileName
			local success, err = pcall(include, filePath)
			
			if not success then
				print("[gmRTX-SH] Warning: Failed to load shared file: " .. filePath .. " - Error: " .. tostring(err))
			else
				print("[gmRTX-SH] Successfully loaded shared file: " .. filePath)
			end
		end
	else
		print("[gmRTX-SH] No shared files found")
	end
end

-- Load shared files immediately when this autorun executes (both client and server)
LoadSharedFiles()

if (SERVER) then
	util.AddNetworkString( "RTXPlayerSpawnedFully" )
    util.AddNetworkString( "remix_spawn_rt_light" )
end

hook.Add( "PlayerInitialSpawn", "RTXFullLoadSetup", function( ply )
	hook.Add( "SetupMove", ply, function( self, mvply, _, cmd )
		if self == mvply and not cmd:IsForced() then
			hook.Run( "RTXPlayerFullLoad", self )
			hook.Remove( "SetupMove", self )
			if (SERVER) then
				net.Start( "RTXPlayerSpawnedFully" )
				net.Send( mvply )
			end
		end
	end )
end )