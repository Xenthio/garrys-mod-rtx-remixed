if not CLIENT then return end

if CLIENT then
    require((BRANCH == "x86-64" or BRANCH == "chromium" ) and "RTXFixesBinary" or "RTXFixesBinary")
end



-- Initialize NikNaks
require("niknaks")

-- Create debug ConVar here so it's available for DebugPrint
local cv_debug = CreateClientConVar("rtx_rt_debug", "0", true, false, "Enable debug messages for RT States")

-- Helper function for debug printing (moved up before sub-addon loading)
-- Made global so sub-addons can use it
function DebugPrint(message)
    if cv_debug:GetBool() then
        print(message)
    end
end

-- Master toggle for applying a preset of render cvars
local cv_custom_render = CreateClientConVar("rtx_custom_render", "0", true, false, "Toggle Remix custom render preset")

local function ApplyCustomRenderPreset(enable)
    -- r_drawworld and r_drawopaqueworld stay at 1 so the engine still collects
    -- surface data needed for overlay and decal rendering.  Shader_DrawChains is
    -- patched out instead to prevent double-drawing of world surface polygons.
    RunConsoleCommand("r_drawstaticprops", enable and "0" or "1")
    RunConsoleCommand("r_DrawDisp", enable and "0" or "1")
    RunConsoleCommand("r_DrawDetailProps", enable and "0" or "1")
    RunConsoleCommand("rtx_mwr_enable", enable and "1" or "0")
    RunConsoleCommand("rtx_spr_enable", enable and "1" or "0")
    RunConsoleCommand("rtx_dpr_enable", enable and "1" or "0")

    -- Patch out Shader_DrawChains so world surfaces aren't drawn by the engine,
    -- but overlays (OverlayMgr()->RenderOverlays) and decals (DecalSurfaceDraw)
    -- still render because they run after Shader_DrawChains in Shader_WorldEnd.
    RunConsoleCommand("rtx_patch_skip_world_draw", enable and "1" or "0")

    -- Engine patches: when custom rendering is on, only keep frustum culling + BSP culling patches
    -- When off, enable all patches since engine rendering needs them for RTX
    RunConsoleCommand("rtx_patch_frustumcull_engine", "1")
    RunConsoleCommand("rtx_patch_cullnode", "1")
    RunConsoleCommand("rtx_patch_brush_backfaces", enable and "0" or "1")
    RunConsoleCommand("rtx_patch_world_backfaces1", enable and "0" or "1")
    RunConsoleCommand("rtx_patch_world_backfaces2", enable and "0" or "1")
    RunConsoleCommand("rtx_patch_frustumcull_client", enable and "0" or "1")
    RunConsoleCommand("rtx_patch_forcenovis", enable and "0" or "1")
end

cvars.AddChangeCallback("rtx_custom_render", function(name, oldValue, newValue)
    local enable = tonumber(newValue) == 1
    ApplyCustomRenderPreset(enable)
end, "gmrtx_custom_render_preset_cb")

-- Apply initial state on load
ApplyCustomRenderPreset(cv_custom_render:GetBool())

local function LoadSubAddons()
    local foldersToLoad = {}
    
    -- Always load shared files first
    table.insert(foldersToLoad, "remixlua/sh/")
    
    -- Load client files when on client
    if CLIENT then
        table.insert(foldersToLoad, "remixlua/cl/")
        table.insert(foldersToLoad, "remixlua/cl/remixapi/")
        table.insert(foldersToLoad, "remixlua/cl/remixapi/wrappers/")
        table.insert(foldersToLoad, "remixlua/cl/customrender/")
    end
    
    -- Load server files when on server
    if SERVER then
        table.insert(foldersToLoad, "remixlua/sv/")
    end
    
    for _, folder in ipairs(foldersToLoad) do
        local files, _ = file.Find(folder .. "*.lua", "LUA")
        
        if files then
            DebugPrint("[gmRTX] Found " .. #files .. " files in " .. folder)
            
            for _, fileName in ipairs(files) do
                local filePath = folder .. fileName
                local success, err = pcall(include, filePath)
                
                if not success then
                    DebugPrint("[gmRTX] Warning: Failed to load sub-addon: " .. filePath .. " - Error: " .. tostring(err))
                else
                    DebugPrint("[gmRTX] Successfully loaded sub-addon: " .. filePath)
                end
            end
        else
            DebugPrint("[gmRTX] No files found in " .. folder)
        end
    end
end

-- Load all sub-addons
LoadSubAddons()

-- ============================
-- Garbage Collection Tuning
-- ============================
-- Tune GC to minimize frametime spikes during rendering
local function ConfigureGC()
    -- Check if any renderer is actively building
    local anyBuilding = false
    if RemixRenderCore then
        if RemixRenderCore._worldBuildState and RemixRenderCore._worldBuildState.active then
            anyBuilding = true
        end
        if RemixRenderCore._dispBuildState and RemixRenderCore._dispBuildState.active then
            anyBuilding = true
        end
        if RemixRenderCore._sprBuildState and RemixRenderCore._sprBuildState.active then
            anyBuilding = true
        end
    end
    
    if anyBuilding then
        -- During builds: more aggressive collection to prevent buildup
        collectgarbage("setpause", 100)    -- Default pause
        collectgarbage("setstepmul", 400)  -- More aggressive stepping
    else
        -- During rendering: lazy collection to avoid hitches
        collectgarbage("setpause", 120)    -- Wait longer before GC
        collectgarbage("setstepmul", 200)  -- Less aggressive stepping
    end
end

-- Run GC tuning periodically (not every frame to reduce overhead)
local lastGCTune = 0
hook.Add("Think", "RemixGCTuning", function()
    local now = SysTime()
    if now - lastGCTune > 1 then  -- Tune every second
        ConfigureGC()
        lastGCTune = now
    end
end)

-- Sync engine patches with ConVar values periodically // Kinda cursed but it works :3 
local lastPatchSync = 0
hook.Add("Think", "RemixPatchSync", function()
    if not RTX_SyncPatches then return end
    local now = SysTime()
    if now - lastPatchSync > 0.5 then
        RTX_SyncPatches()
        lastPatchSync = now
    end
end)

