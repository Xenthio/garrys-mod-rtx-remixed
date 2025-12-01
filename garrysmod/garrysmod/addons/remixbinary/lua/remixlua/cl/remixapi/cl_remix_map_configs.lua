if not (BRANCH == "x86-64" or BRANCH == "chromium") then return end
if SERVER then return end

local RemixMapConfigs = {}

-- Configuration
local CONFIG_DIR = "remix_map_configs"
local CONFIG_VERSION = 1  -- Increment when config format changes
local DEBUG_MODE = CreateClientConVar("rtx_conf_map_configs_debug", "0", true, false, "Enable debug output for map configs")
local AUTO_LOAD = CreateClientConVar("rtx_conf_map_configs_autoload", "1", true, false, "Automatically load map configs on map start")

-- Dynamic arrays populated from default.txt
local TRACKED_CONFIGS = {}
local TRACKED_SOURCE_COMMANDS = {}

-- Function to load tracked parameters from default.txt
local function LoadTrackedParameters()
    local defaultPath = CONFIG_DIR .. "/default.txt"
    
    -- Clear existing arrays
    TRACKED_CONFIGS = {}
    TRACKED_SOURCE_COMMANDS = {}
    
    if not file.Exists(defaultPath, "DATA") then
        DebugPrint("No default.txt found, using empty tracking lists")
        return
    end
    
    local configText = file.Read(defaultPath, "DATA")
    if not configText then
        DebugPrint("Failed to read default.txt for tracking parameters")
        return
    end
    
    DebugPrint("Loading tracked parameters from default.txt...")
    
    -- Parse default.txt and extract parameter names
    local rtxCount = 0
    local srcCount = 0
    
    for line in string.gmatch(configText, "[^\r\n]+") do
        line = string.Trim(line)
        
        -- Skip comments and empty lines
        if line ~= "" and not string.StartWith(line, "#") then
            -- Parse key = value
            local key, value = string.match(line, "^(%S+)%s*=%s*(.+)$")
            if key and value then
                -- Handle RTX config variables
                if string.StartWith(key, "rtx:") then
                    local rtxKey = string.sub(key, 5) -- Remove "rtx:" prefix
                    table.insert(TRACKED_CONFIGS, rtxKey)
                    rtxCount = rtxCount + 1
                    DebugPrint("Added RTX parameter: " .. rtxKey)
                -- Handle Source engine commands
                elseif string.StartWith(key, "src:") then
                    local cvarName = string.sub(key, 5) -- Remove "src:" prefix
                    table.insert(TRACKED_SOURCE_COMMANDS, cvarName)
                    srcCount = srcCount + 1
                    DebugPrint("Added Source parameter: " .. cvarName)
                -- Handle legacy format (backwards compatibility)
                else
                    table.insert(TRACKED_CONFIGS, key)
                    rtxCount = rtxCount + 1
                    DebugPrint("Added legacy parameter: " .. key)
                end
            end
        end
    end
    
    DebugPrint("Loaded " .. rtxCount .. " RTX parameters and " .. srcCount .. " Source parameters from default.txt")
end

-- Load tracked parameters on addon initialization
LoadTrackedParameters()

-- Current map name
local currentMap = ""

-- Utility functions
local function DebugPrint(msg)
    if DEBUG_MODE:GetBool() then
        print("[gmRTX - Remix API] " .. msg)
    end
end

local function GetCurrentMap()
    return game.GetMap() or ""
end

local function GetConfigPath(mapName)
    return CONFIG_DIR .. "/" .. mapName .. ".txt"
end

-- File I/O helper functions
local function EnsureConfigDir()
    if not file.Exists(CONFIG_DIR, "DATA") then
        file.CreateDir(CONFIG_DIR)
        DebugPrint("Created config directory: " .. CONFIG_DIR)
    end
end

-- Atomic file write: write to temp file then rename to avoid corruption
local function SafeFileWrite(filePath, content)
    local tempPath = filePath .. ".tmp"
    
    -- Write to temp file
    file.Write(tempPath, content)
    
    -- Verify temp file was written
    if not file.Exists(tempPath, "DATA") then
        return false, "Failed to write temporary file"
    end
    
    -- Read back to verify content
    local verification = file.Read(tempPath, "DATA")
    if verification ~= content then
        file.Delete(tempPath)
        return false, "Content verification failed"
    end
    
    -- Delete old file if it exists
    if file.Exists(filePath, "DATA") then
        file.Delete(filePath)
    end
    
    -- Rename temp to final (Garry's Mod file API doesn't have rename, so we copy content)
    file.Write(filePath, content)
    
    -- Clean up temp file
    file.Delete(tempPath)
    
    -- Final verification
    if not file.Exists(filePath, "DATA") then
        return false, "Failed to create final file"
    end
    
    return true, nil
end

-- File I/O functions
local function SaveMapConfig(mapName)
    EnsureConfigDir()
    
    -- Reload tracked parameters from default.txt
    LoadTrackedParameters()
    
    -- GetConfigVariable now reads directly from config file and defaults
    DebugPrint("Getting current config values...")
    
    local configData = {}
    local configLines = {}
    
    -- Add header with version
    table.insert(configLines, "# RTX Remix and Source Engine configuration for map: " .. mapName)
    table.insert(configLines, "# " .. os.date("%Y-%m-%d %H:%M:%S"))
    table.insert(configLines, "# Config Version: " .. CONFIG_VERSION)
    table.insert(configLines, "")
    table.insert(configLines, "version = " .. CONFIG_VERSION)
    table.insert(configLines, "")
    table.insert(configLines, "# Note: These are current config values. Adjust RTX settings in-game,")
    table.insert(configLines, "# modify Source engine cvars, then save again or modify this file directly.")
    table.insert(configLines, "")
    
    local savedCount = 0
    
    -- Save RTX tracked config variables
    if RemixConfig and #TRACKED_CONFIGS > 0 then
        table.insert(configLines, "# RTX Remix Settings")
        for _, configKey in ipairs(TRACKED_CONFIGS) do
            local value = RemixConfig.GetConfigVariable(configKey)
            if value and value ~= "" then
                configData[configKey] = value
                table.insert(configLines, "rtx:" .. configKey .. " = " .. value)
                DebugPrint("Saved RTX " .. configKey .. " = " .. value)
                savedCount = savedCount + 1
            else
                DebugPrint("Skipped RTX " .. configKey .. " (no value)")
            end
        end
        table.insert(configLines, "")
    end
    
    -- Save Source engine tracked commands
    if #TRACKED_SOURCE_COMMANDS > 0 then
        table.insert(configLines, "# Source Engine Settings")
        for _, cvarName in ipairs(TRACKED_SOURCE_COMMANDS) do
            local cvar = GetConVar(cvarName)
            if cvar then
                local value = cvar:GetString()
                if value and value ~= "" then
                    configData[cvarName] = value
                    table.insert(configLines, "src:" .. cvarName .. " = " .. value)
                    DebugPrint("Saved Source " .. cvarName .. " = " .. value)
                    savedCount = savedCount + 1
                else
                    DebugPrint("Skipped Source " .. cvarName .. " (no value)")
                end
            else
                DebugPrint("Skipped Source " .. cvarName .. " (cvar not found)")
            end
        end
        table.insert(configLines, "")
    end
    
    -- Write to file atomically
    local configText = table.concat(configLines, "\n")
    local filePath = GetConfigPath(mapName)
    
    local success, err = SafeFileWrite(filePath, configText)
    
    if not success then
        print("[gmRTX - Remix API] Error: Failed to save config file: " .. tostring(err))
        return false
    end
    
    DebugPrint("Saved config for map '" .. mapName .. "' to " .. filePath)
    print("[gmRTX - Remix API] Saved " .. savedCount .. " RTX and Source settings for map: " .. mapName)
    
    if savedCount == 0 then
        print("[gmRTX - Remix API] Warning: No config values were saved. Try adjusting RTX settings or Source cvars first.")
    end
    
    return savedCount > 0
end

local function LoadConfigFromFile(filePath, configName)
    if not file.Exists(filePath, "DATA") then
        return false, 0
    end
    
    local configText = file.Read(filePath, "DATA")
    if not configText then
        DebugPrint("Failed to read config file: " .. filePath)
        return false, 0
    end
    
    DebugPrint("Loading config: " .. configName)
    
    -- Check config version
    local fileVersion = 0
    for line in string.gmatch(configText, "[^\r\n]+") do
        local key, value = string.match(line, "^(%S+)%s*=%s*(.+)$")
        if key == "version" then
            fileVersion = tonumber(value) or 0
            break
        end
    end
    
    if fileVersion > CONFIG_VERSION then
        print("[gmRTX - Remix API] Warning: Config file version (" .. fileVersion .. ") is newer than supported version (" .. CONFIG_VERSION .. ")")
        print("[gmRTX - Remix API] Some settings may not be applied correctly. Consider updating the addon.")
    elseif fileVersion < CONFIG_VERSION and fileVersion > 0 then
        DebugPrint("Config file version (" .. fileVersion .. ") is older than current version (" .. CONFIG_VERSION .. "), will attempt to load")
    end
    
    -- Parse config file and apply settings
    local loadedCount = 0
    for line in string.gmatch(configText, "[^\r\n]+") do
        line = string.Trim(line)
        
        -- Skip comments and empty lines
        if line ~= "" and not string.StartWith(line, "#") then
            -- Parse key = value
            local key, value = string.match(line, "^(%S+)%s*=%s*(.+)$")
            if key and value then
                -- Skip version field (already processed)
                if key == "version" then
                    -- Skip
                else
                    -- Remove any trailing comments
                    value = string.match(value, "^([^#]+)") or value
                    value = string.Trim(value)
                    
                    -- Handle RTX config variables
                    if string.StartWith(key, "rtx:") then
                        local rtxKey = string.sub(key, 5) -- Remove "rtx:" prefix
                        if RemixConfig and RemixConfig.SetConfigVariable(rtxKey, value) then
                            DebugPrint("Loaded RTX " .. rtxKey .. " = " .. value)
                            loadedCount = loadedCount + 1
                        else
                            DebugPrint("Failed to set RTX " .. rtxKey .. " = " .. value)
                        end
                    -- Handle Source engine commands
                    elseif string.StartWith(key, "src:") then
                        local cvarName = string.sub(key, 5) -- Remove "src:" prefix
                        RunConsoleCommand(cvarName, value)
                        DebugPrint("Loaded Source " .. cvarName .. " = " .. value)
                        loadedCount = loadedCount + 1
                    -- Handle legacy format (backwards compatibility)
                    else
                        if RemixConfig and RemixConfig.SetConfigVariable(key, value) then
                            DebugPrint("Loaded (legacy) " .. key .. " = " .. value)
                            loadedCount = loadedCount + 1
                        else
                            DebugPrint("Failed to set (legacy) " .. key .. " = " .. value)
                        end
                    end
                end
            end
        end
    end
    
    return true, loadedCount
end

local function LoadMapConfig(mapName)
    -- Reload tracked parameters from default.txt
    LoadTrackedParameters()
    
    local filePath = GetConfigPath(mapName)
    local defaultPath = CONFIG_DIR .. "/default.txt"
    local loadedCount = 0
    local configSource = ""
    
    -- First try to load map-specific config
    if file.Exists(filePath, "DATA") then
        local success, count = LoadConfigFromFile(filePath, "map config for " .. mapName)
        if success then
            loadedCount = count
            configSource = "map-specific config"
        end
    end
    
    -- If no map-specific config or it failed, try default.txt
    if loadedCount == 0 then
        DebugPrint("No map-specific config found, trying default.txt")
        
        if file.Exists(defaultPath, "DATA") then
            local success, count = LoadConfigFromFile(defaultPath, "default config")
            if success then
                loadedCount = count
                configSource = "default config"
            end
        end
    end
    
    if loadedCount > 0 then
        print("[gmRTX - Remix API] Loaded " .. loadedCount .. " RTX and Source settings from " .. configSource .. " for map: " .. mapName)
        return true
    else
        DebugPrint("No config file found for map: " .. mapName .. " (tried map-specific and default.txt)")
        return false
    end
end

-- Public API
function RemixMapConfigs.SaveCurrentMapConfig()
    local mapName = GetCurrentMap()
    if mapName == "" then
        print("[gmRTX - Remix API] Error: No map loaded")
        return false
    end
    
    return SaveMapConfig(mapName)
end

function RemixMapConfigs.LoadCurrentMapConfig()
    local mapName = GetCurrentMap()
    if mapName == "" then
        print("[gmRTX - Remix API] Error: No map loaded")
        return false
    end
    
    return LoadMapConfig(mapName)
end

function RemixMapConfigs.SaveMapConfig(mapName)
    if not mapName or mapName == "" then
        print("[gmRTX - Remix API] Error: Invalid map name")
        return false
    end
    
    return SaveMapConfig(mapName)
end

function RemixMapConfigs.LoadMapConfig(mapName)
    if not mapName or mapName == "" then
        print("[gmRTX - Remix API] Error: Invalid map name")
        return false
    end
    
    return LoadMapConfig(mapName)
end

function RemixMapConfigs.ListConfigs()
    local files, _ = file.Find(CONFIG_DIR .. "/*.txt", "DATA")
    
    if not files or #files == 0 then
        print("[gmRTX - Remix API] No saved map configs found")
        return {}
    end
    
    print("[gmRTX - Remix API] Saved map configs:")
    local mapNames = {}
    local hasDefault = false
    
    for _, fileName in ipairs(files) do
        local mapName = string.StripExtension(fileName)
        table.insert(mapNames, mapName)
        
        if mapName == "default" then
            hasDefault = true
            print("  - " .. mapName .. " (fallback config for maps without specific configs)")
        else
            print("  - " .. mapName)
        end
    end
    
    if not hasDefault then
        print("")
        print("  No default.txt found. Use 'rtx_conf_default_save' to create fallback settings.")
    end
    
    return mapNames
end

function RemixMapConfigs.DeleteMapConfig(mapName)
    if not mapName or mapName == "" then
        print("[gmRTX - Remix API] Error: Invalid map name")
        return false
    end
    
    local filePath = GetConfigPath(mapName)
    
    if not file.Exists(filePath, "DATA") then
        print("[gmRTX - Remix API] No config found for map: " .. mapName)
        return false
    end
    
    file.Delete(filePath)
    print("[gmRTX - Remix API] Deleted config for map: " .. mapName)
    return true
end

-- Auto-loading functions
local function OnMapStart()
    -- Check if auto-loading is enabled
    if not AUTO_LOAD:GetBool() then
        DebugPrint("Auto-loading disabled, skipping config load")
        return
    end
    
    -- Small delay to ensure everything is loaded
    timer.Simple(1, function()
        local mapName = GetCurrentMap()
        if mapName and mapName ~= "" then
            DebugPrint("Auto-loading config for map: " .. mapName)
            RemixMapConfigs.LoadCurrentMapConfig()
        end
    end)
end

-- Hook into map events
hook.Add("InitPostEntity", "RemixMapConfigs_MapStart", OnMapStart)

-- Console commands
concommand.Add("rtx_conf_save_map_config", function()
    RemixMapConfigs.SaveCurrentMapConfig()
end, nil, "Save current RTX and Source engine settings for this map")

concommand.Add("rtx_conf_load_map_config", function()
    RemixMapConfigs.LoadCurrentMapConfig()
end, nil, "Load RTX and Source engine settings for this map")

concommand.Add("rtx_conf_list_map_configs", function()
    RemixMapConfigs.ListConfigs()
end, nil, "List all saved map configs")

concommand.Add("rtx_conf_delete_map_config", function(ply, cmd, args)
    if #args < 1 then
        print("Usage: remix_delete_map_config <mapname>")
        return
    end
    
    RemixMapConfigs.DeleteMapConfig(args[1])
end, nil, "Delete config for specified map")

concommand.Add("rtx_conf_default_save", function()
    EnsureConfigDir()
    
    -- Reload tracked parameters from current default.txt (if it exists)
    LoadTrackedParameters()
    
    local defaultPath = CONFIG_DIR .. "/default.txt"
    local configLines = {}
    
    -- Add header with version
    table.insert(configLines, "# RTX Remix and Source Engine default configuration")
    table.insert(configLines, "# " .. os.date("%Y-%m-%d %H:%M:%S"))
    table.insert(configLines, "# Config Version: " .. CONFIG_VERSION)
    table.insert(configLines, "")
    table.insert(configLines, "version = " .. CONFIG_VERSION)
    table.insert(configLines, "")
    table.insert(configLines, "# This file will be loaded for any map that doesn't have its own config.")
    table.insert(configLines, "# Adjust RTX settings in-game and Source cvars, then run this command to save as defaults.")
    table.insert(configLines, "")
    
    local savedCount = 0
    
    -- Save RTX tracked config variables
    if RemixConfig and #TRACKED_CONFIGS > 0 then
        table.insert(configLines, "# RTX Remix Settings")
        for _, configKey in ipairs(TRACKED_CONFIGS) do
            local value = RemixConfig.GetConfigVariable(configKey)
            if value and value ~= "" then
                table.insert(configLines, "rtx:" .. configKey .. " = " .. value)
                savedCount = savedCount + 1
            end
        end
        table.insert(configLines, "")
    end
    
    -- Save Source engine tracked commands
    if #TRACKED_SOURCE_COMMANDS > 0 then
        table.insert(configLines, "# Source Engine Settings")
        for _, cvarName in ipairs(TRACKED_SOURCE_COMMANDS) do
            local cvar = GetConVar(cvarName)
            if cvar then
                local value = cvar:GetString()
                if value and value ~= "" then
                    table.insert(configLines, "src:" .. cvarName .. " = " .. value)
                    savedCount = savedCount + 1
                end
            end
        end
        table.insert(configLines, "")
    end
    
    -- Write to file atomically
    local configText = table.concat(configLines, "\n")
    local success, err = SafeFileWrite(defaultPath, configText)
    
    if not success then
        print("[gmRTX - Remix API] Error: Failed to save default config: " .. tostring(err))
        return
    end
    
    print("[gmRTX - Remix API] Saved " .. savedCount .. " RTX and Source settings as default config")
    print("[gmRTX - Remix API] Default config saved to: " .. defaultPath)
end, nil, "Save current RTX and Source engine settings as default config for all maps")

concommand.Add("rtx_conf_load_default", function()
    local defaultPath = CONFIG_DIR .. "/default.txt"
    
    if not file.Exists(defaultPath, "DATA") then
        print("[gmRTX - Remix API] No default config file found. Use rtx_conf_default_save to create one.")
        return
    end
    
    local success, count = LoadConfigFromFile(defaultPath, "default config")
    if success then
        print("[gmRTX - Remix API] Loaded " .. count .. " RTX and Source settings from default config")
    else
        print("[gmRTX - Remix API] Failed to load default config")
    end
end, nil, "Load the default RTX and Source engine config immediately")

concommand.Add("rtx_conf_toggle_autoload", function()
    local currentValue = AUTO_LOAD:GetBool()
    AUTO_LOAD:SetBool(not currentValue)
    
    if AUTO_LOAD:GetBool() then
        print("[gmRTX - Remix API] Auto-loading enabled - configs will load automatically on map start")
    else
        print("[gmRTX - Remix API] Auto-loading disabled - use rtx_conf_load_map_config to load manually")
    end
end, nil, "Toggle automatic loading of map configs on map start")

concommand.Add("rtx_conf_reload_tracked_params", function()
    LoadTrackedParameters()
    print("[gmRTX - Remix API] Reloaded tracked parameters from default.txt")
    print("[gmRTX - Remix API] Now tracking " .. #TRACKED_CONFIGS .. " RTX parameters and " .. #TRACKED_SOURCE_COMMANDS .. " Source parameters")
end, nil, "Reload the list of tracked parameters from default.txt")

-- Make API globally available
_G.RemixMapConfigs = RemixMapConfigs