-- forcedynamic_enabler.lua
-- Manages mat_forcedynamic persistence per map

local FILE_NAME = "mapfixes_maps.txt"
local enabledMaps = {}

-- Load the list of enabled maps from file
local function LoadEnabledMaps()
    if file.Exists(FILE_NAME, "DATA") then
        local content = file.Read(FILE_NAME, "DATA")
        if content and content ~= "" then
            local loadedData = util.JSONToTable(content)
            if type(loadedData) == "table" then
                enabledMaps = loadedData
            else
                MsgC(Color(255, 100, 100), "[Render Fixes] Error loading data: Invalid format in "..FILE_NAME.."\n")
            end
        end
    else
        -- Create the file if it doesn't exist
        file.Write(FILE_NAME, util.TableToJSON({}))
    end
end

-- Save the list of enabled maps to file
local function SaveEnabledMaps()
    local success = file.Write(FILE_NAME, util.TableToJSON(enabledMaps))
    if not success then
         MsgC(Color(255, 100, 100), "[Render Fixes] Error saving data to "..FILE_NAME.."\n")
    end
end

-- Function to enable forcedynamic for the current map
local function EnableForCurrentMap()
    local currentMap = game.GetMap()
    if not enabledMaps[currentMap] then
        enabledMaps[currentMap] = true
        SaveEnabledMaps()
        MsgC(Color(100, 255, 100), "[Render Fixes] Enabled automatic Map Fixes for map: ", currentMap, "\n")
        -- Add notification
        notification.AddLegacy("[Render Fixes] Enabled and saved for map: " .. currentMap, NOTIFY_GENERIC, 5)
    else
        MsgC(Color(255, 255, 100), "[Render Fixes] automatic Map Fixes already enabled for map: ", currentMap, "\n")
        -- Optional: Notify that it was already enabled
        notification.AddLegacy("[Render Fixes] Already enabled for map: " .. currentMap, NOTIFY_HINT, 3)
    end

    -- Attempt to set the CVar
    RunConsoleCommand("mat_forcedynamic", "1")
    RunConsoleCommand("prop_disable_distance_fade", "1")
end

-- Function to disable forcedynamic for the current map
local function DisableForCurrentMap()
    local currentMap = game.GetMap()
    if enabledMaps[currentMap] then
        enabledMaps[currentMap] = nil -- Remove from the table
        SaveEnabledMaps()
        MsgC(Color(100, 255, 100), "[Render Fixes] Disabled automatic Map Fixes for map: ", currentMap, "\n")
        notification.AddLegacy("[Render Fixes] Disabled and removed for map: " .. currentMap, NOTIFY_GENERIC, 5) -- Added notification
    else
        MsgC(Color(255, 255, 100), "[Render Fixes] automatic Map Fixes was not enabled for map: ", currentMap, "\n")
        notification.AddLegacy("[Render Fixes] Was not enabled for map: " .. currentMap, NOTIFY_HINT, 3) -- Added notification
    end
    -- Attempt to disable the CVar
    RunConsoleCommand("mat_forcedynamic", "0")
    RunConsoleCommand("prop_disable_distance_fade", "0")
end

-- Check on map load if forcedynamic should be enabled
local function CheckMapOnLoad()
    local currentMap = game.GetMap()
    if enabledMaps[currentMap] then
        MsgC(Color(100, 255, 100), "[Render Fixes] Map ", currentMap, " found in list. Enabling mat_forcedynamic.\n")
        RunConsoleCommand("mat_forcedynamic", "1")
         -- Optional notification on auto-enable
         notification.AddLegacy("[Render Fixes] Auto-enabled for map: " .. currentMap, NOTIFY_GENERIC, 3)
    else
        -- Ensure it's off if not explicitly enabled for this map by the script
        -- Only do this if the CVar actually exists to avoid errors
        if GetConVar("mat_forcedynamic") then
             RunConsoleCommand("mat_forcedynamic", "0")
             RunConsoleCommand("prop_disable_distance_fade", "1")
        end
    end
end

-- Register console commands
concommand.Add("rtx_mf_enable_current_map", EnableForCurrentMap, nil, "Enables mat_forcedynamic 1 for the current map and remembers it.")
concommand.Add("rtx_mf_disable_current_map", DisableForCurrentMap, nil, "Disables mat_forcedynamic 0 for the current map and forgets it.")

-- Hook into map load (InitPostEntity runs after entities are initialized)
-- Using a timer to delay slightly, ensuring the game state is fully ready
hook.Add("InitPostEntity", "ForceDynamicCheckMap", function()
    timer.Simple(0.1, CheckMapOnLoad)
end)

-- Load the map list when the script first runs
LoadEnabledMaps()

MsgC(Color(150, 200, 255), "[Render Fixes] Addon loaded. Use 'rtx_mf_enable_current_map' and 'rtx_mf_disable_current_map'.\n") 