--[[
    Solid Color Texture Fixer for RTX Remix
    
    When multiple materials use textures with identical pixel content (like solid colors),
    RTX Remix computes the same hash for them, causing them to be treated as the same
    texture. This script fixes that by giving each solid-color material a unique texture.
    
    How it works:
    1. C++ detects solid-color textures as they load and queues them
    2. Lua periodically calls RemixMaterial.ProcessPendingSolidColors()
    3. C++ fires "RTX_SolidColorDetected" hook for each pending material
    4. Lua's hook handler creates a unique texture and swaps $basetexture
    5. RTX Remix now computes unique hashes for each material
    
    Hook: RTX_SolidColorDetected(materialName)
    - Called when C++ detects a solid-color texture
    - Return true from your hook to prevent the default fix
]]

if not CLIENT then return end
if not (BRANCH == "x86-64" or BRANCH == "chromium") then return end

-- ConVars for configuration
local enable_addon = CreateConVar("rtx_fix_hash_collisions", "1", FCVAR_ARCHIVE, "Enable/disable the solid color texture fixer")
local debug_mode = CreateConVar("rtx_fix_hash_collisions_debug", "0", FCVAR_ARCHIVE, "Enable debugging output")

-- Count of fixes applied
local fixCount = 0
-- Generated unique textures
local uniqueTextures = {}

-- Timer for processing pending materials
local TIMER_NAME = "RTX_SolidColorProcessor"

-- Debug print function
local function DebugPrint(...)
    if debug_mode:GetBool() then
        MsgC(Color(200, 200, 255), "[RTX SolidFix] ", Color(255, 255, 255), ...)
        MsgC(Color(255, 255, 255), "\n")
    end
end

-- Generate a simple hash from a string
local function StringHash(str)
    local hash = 5381
    for i = 1, #str do
        hash = ((hash * 33) + string.byte(str, i)) % 4294967296
    end
    return hash
end

-- Create a unique color based on material name
local function GetUniqueColor(materialName)
    local hash = StringHash(materialName)
    -- Generate RGB values from hash
    -- Keep values low to minimize visual impact on dark textures
    -- but different enough to produce different Remix hashes
    local r = (hash % 16) / 255  -- 0-15 range
    local g = ((hash / 16) % 16) / 255
    local b = ((hash / 256) % 16) / 255
    return r, g, b
end

-- Create a unique procedural texture for a material
local function CreateUniqueTexture(materialName)
    local texName = "rtx_unique_" .. string.gsub(materialName, "[^%w]", "_")
    
    -- Check if we already created this texture
    if uniqueTextures[texName] then
        return uniqueTextures[texName]
    end
    
    local r, g, b = GetUniqueColor(materialName)
    
    -- Create a tiny material with unique color
    local uniqueMat = CreateMaterial(texName, "UnlitGeneric", {
        ["$basetexture"] = "color/white",
        ["$color"] = string.format("[%f %f %f]", r, g, b),
        ["$color2"] = string.format("[%f %f %f]", r, g, b),
        ["$vertexcolor"] = 0,
        ["$vertexalpha"] = 0,
    })
    
    if uniqueMat and not uniqueMat:IsError() then
        uniqueTextures[texName] = uniqueMat
        DebugPrint("Created unique texture: ", texName, " with color (", 
            math.Round(r*255), ", ", math.Round(g*255), ", ", math.Round(b*255), ")")
        return uniqueMat
    end
    
    return nil
end

-- Fix a single solid-color material
local function FixSolidColorMaterial(matName)
    local mat = Material(matName)
    if not mat or mat:IsError() then
        DebugPrint("Skipping (error material): ", matName)
        return false
    end
    
    -- Create unique texture and swap
    local uniqueMat = CreateUniqueTexture(matName)
    if uniqueMat then
        local uniqueTex = uniqueMat:GetTexture("$basetexture")
        if uniqueTex then
            mat:SetTexture("$basetexture", uniqueTex)
            fixCount = fixCount + 1
            
            -- Tell C++ we've fixed this material
            if RemixMaterial and RemixMaterial.MarkMaterialFixed then
                RemixMaterial.MarkMaterialFixed(matName)
            end
            
            if debug_mode:GetBool() then
                MsgC(Color(100, 255, 100), "[RTX SolidFix] Fixed: '", matName, "'\n")
            end
            return true
        end
    end
    
    MsgC(Color(255, 100, 100), "[RTX SolidFix] Failed to fix: '", matName, "'\n")
    return false
end

-- Hook handler for when C++ detects a solid-color texture
hook.Add("RTX_SolidColorDetected", "RTX_SolidColorFixer", function(materialName)
    if not enable_addon:GetBool() then return end
    
    DebugPrint("Solid color detected: ", materialName)
    
    -- Fix the material
    FixSolidColorMaterial(materialName)
end)

-- Process pending solid-color materials (calls C++ which fires hooks)
local function ProcessPending()
    if not enable_addon:GetBool() then return end
    if not RemixMaterial or not RemixMaterial.ProcessPendingSolidColors then return end
    
    -- This calls hook.Call("RTX_SolidColorDetected", nil, materialName) for each pending material
    local count = RemixMaterial.ProcessPendingSolidColors()
    
    if count > 0 and debug_mode:GetBool() then
        DebugPrint("Processed ", count, " pending solid-color materials")
    end
end

-- Start the processing timer
local function StartProcessing()
    if timer.Exists(TIMER_NAME) then
        timer.Remove(TIMER_NAME)
    end
    
    -- Process every 0.1 seconds (10 times per second)
    timer.Create(TIMER_NAME, 0.1, 0, ProcessPending)
end

-- Stop the processing timer
local function StopProcessing()
    if timer.Exists(TIMER_NAME) then
        timer.Remove(TIMER_NAME)
    end
end

-- Handle cvar changes
cvars.AddChangeCallback("rtx_fix_hash_collisions", function(_, _, new)
    if new == "1" then
        StartProcessing()
    else
        StopProcessing()
    end
end)

-- Initial run with delay
hook.Add("InitPostEntity", "FixSolidColorsOnMapLoad", function()
    if enable_addon:GetBool() then
        -- Wait a bit for initial textures to load, then start processing
        timer.Simple(1, function()
            StartProcessing()
        end)
    end
end)

-- Reset on map change
hook.Add("ShutDown", "CleanupSolidColorFixer", function()
    StopProcessing()
    fixCount = 0
    uniqueTextures = {}
end)

-- Console commands
concommand.Add("rtx_fix_hash_collisions_process", function()
    ProcessPending()
    notification.AddLegacy("Processed pending solid-color textures", NOTIFY_GENERIC, 3)
end, nil, "Manually trigger solid-color texture processing")

concommand.Add("rtx_fix_hash_collisions_stats", function()
    MsgC(Color(100, 200, 255), "[RTX SolidFix] Statistics:\n")
    MsgC(Color(200, 200, 200), "  Total fixed: ", fixCount, "\n")
    MsgC(Color(200, 200, 200), "  Unique textures created: ", table.Count(uniqueTextures), "\n")
    
    -- Show pending solid-color materials from C++
    if RemixMaterial and RemixMaterial.GetSolidColorMaterials then
        local pending = RemixMaterial.GetSolidColorMaterials()
        MsgC(Color(200, 200, 200), "  Pending (unfixed): ", #pending, "\n")
        
        if #pending > 0 and #pending <= 20 then
            MsgC(Color(255, 200, 100), "  Pending materials:\n")
            for _, matName in ipairs(pending) do
                MsgC(Color(200, 200, 200), "    - ", matName, "\n")
            end
        end
    end
end, nil, "Show solid-color fix statistics")

concommand.Add("rtx_fix_hash_collisions_list", function()
    if not RemixMaterial or not RemixMaterial.GetSolidColorMaterials then
        MsgC(Color(255, 200, 100), "[RTX SolidFix] RemixMaterial.GetSolidColorMaterials not available.\n")
        return
    end
    
    local pending = RemixMaterial.GetSolidColorMaterials()
    
    MsgC(Color(100, 200, 255), "[RTX SolidFix] Solid-color materials pending fix: ", #pending, "\n")
    for _, matName in ipairs(pending) do
        MsgC(Color(200, 200, 200), "  - ", matName, "\n")
    end
end, nil, "List solid-color materials pending fix")

concommand.Add("rtx_fix_hash_collisions_check", function(ply, cmd, args)
    if #args < 1 then
        MsgC(Color(255, 200, 100), "[RTX SolidFix] Usage: rtx_fix_hash_collisions_check <material_name>\n")
        return
    end
    
    local matName = args[1]
    
    if not RemixMaterial then
        MsgC(Color(255, 200, 100), "[RTX SolidFix] RemixMaterial not available.\n")
        return
    end
    
    -- Check if it's a solid color
    if RemixMaterial.IsSolidColor then
        local isSolid, color = RemixMaterial.IsSolidColor(matName)
        if isSolid then
            MsgC(Color(100, 255, 100), "[RTX SolidFix] '", matName, "' IS a solid color")
            if color then
                MsgC(Color(100, 255, 100), " (RGB: ", color.r, ", ", color.g, ", ", color.b, ")")
            end
            MsgC(Color(100, 255, 100), "\n")
        else
            MsgC(Color(200, 200, 200), "[RTX SolidFix] '", matName, "' is NOT a solid color (or not in cache)\n")
        end
    end
    
    -- Check if it's been fixed
    if RemixMaterial.IsMaterialFixed then
        local isFixed = RemixMaterial.IsMaterialFixed(matName)
        MsgC(Color(200, 200, 200), "[RTX SolidFix] Fixed status: ", isFixed and "YES" or "NO", "\n")
    end
end, nil, "Check if a material is a solid color and its fix status")

concommand.Add("rtx_fix_hash_collisions_add", function(ply, cmd, args)
    if #args < 1 then
        MsgC(Color(255, 200, 100), "[RTX SolidFix] Usage: rtx_fix_hash_collisions_add <material_name>\n")
        return
    end
    
    local matName = args[1]
    
    if FixSolidColorMaterial(matName) then
        MsgC(Color(100, 255, 100), "[RTX SolidFix] Manually fixed: '", matName, "'\n")
    end
end, nil, "Manually fix a material (force it to have a unique texture)")

-- Startup message
MsgC(Color(100, 255, 100), "[RTX SolidFix] Solid Color Texture Fixer loaded.\n")
MsgC(Color(200, 200, 200), "  Hook: RTX_SolidColorDetected(materialName) - fired when solid colors are detected\n")
MsgC(Color(200, 200, 200), "  Commands: rtx_fix_hash_collisions_stats, rtx_fix_hash_collisions_check <mat>\n")
