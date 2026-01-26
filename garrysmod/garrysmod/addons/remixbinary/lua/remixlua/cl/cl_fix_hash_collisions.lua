--[[
    Solid Color Texture Fixer for RTX Remix
    
    When multiple materials use textures with identical pixel content (like solid colors),
    RTX Remix computes the same hash for them, causing them to be treated as the same
    texture. This script fixes that by giving each solid-color material a unique texture.
    
    How it works:
    1. C++ detects solid-color textures as they load and tracks them
    2. Lua periodically queries C++ for newly detected solid-color materials
    3. For each solid-color material, Lua creates a unique procedural texture
    4. The $basetexture is swapped to the unique texture via mat:SetTexture()
    5. RTX Remix now computes unique hashes for each material
    
    This happens automatically as textures load - no hardcoded lists needed!
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

-- Timer identifiers
local TIMER_NAME = "SolidColorFixerContinuous"

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

-- Query C++ for newly detected solid-color materials and fix them
local function FixNewSolidColorMaterials(showOutput)
    if not enable_addon:GetBool() then return 0 end
    
    -- Check if RemixMaterial API is available
    if not RemixMaterial or not RemixMaterial.GetSolidColorMaterials then
        if showOutput then
            MsgC(Color(255, 200, 100), "[RTX SolidFix] RemixMaterial.GetSolidColorMaterials not available yet.\n")
        end
        return 0
    end
    
    local previousFixed = fixCount
    
    -- Query C++ for solid-color materials that need fixing
    local materials = RemixMaterial.GetSolidColorMaterials()
    
    if not materials or #materials == 0 then
        return 0
    end
    
    -- Fix each solid-color material
    local newFixed = 0
    for _, matName in ipairs(materials) do
        if FixSolidColorMaterial(matName) then
            newFixed = newFixed + 1
        end
    end
    
    if showOutput and newFixed > 0 then
        MsgC(Color(100, 255, 100), "[RTX SolidFix] Fixed ", newFixed, " solid-color textures\n")
    end
    
    return newFixed
end

-- Start continuous checking (runs frequently to catch newly loaded textures)
local function StartContinuousChecking()
    if timer.Exists(TIMER_NAME) then 
        timer.Remove(TIMER_NAME)
    end
    
    -- Run every 0.5 seconds to quickly fix newly loaded textures
    timer.Create(TIMER_NAME, 0.5, 0, function()
        FixNewSolidColorMaterials(false)
    end)
end

-- Stop continuous checking
local function StopContinuousChecking()
    if timer.Exists(TIMER_NAME) then
        timer.Remove(TIMER_NAME)
    end
end

-- Handle cvar changes
cvars.AddChangeCallback("rtx_fix_hash_collisions", function(_, _, new)
    if new == "1" then
        FixNewSolidColorMaterials(true)
        StartContinuousChecking()
    else
        StopContinuousChecking()
    end
end)

-- Initial run with delay
hook.Add("InitPostEntity", "FixSolidColorsOnMapLoad", function()
    if enable_addon:GetBool() then
        -- Wait a bit for initial textures to load
        timer.Simple(1, function()
            FixNewSolidColorMaterials(true)
            StartContinuousChecking()
        end)
    end
end)

-- Reset on map change
hook.Add("ShutDown", "CleanupSolidColorFixer", function()
    StopContinuousChecking()
    fixCount = 0
    uniqueTextures = {}
end)

-- Console commands
concommand.Add("rtx_fix_hash_collisions_process", function()
    local fixed = FixNewSolidColorMaterials(true)
    if fixed > 0 then
        notification.AddLegacy("Fixed " .. fixed .. " solid-color textures", NOTIFY_GENERIC, 3)
    else
        notification.AddLegacy("No new solid-color textures to fix", NOTIFY_GENERIC, 3)
    end
end, nil, "Manually trigger solid-color texture fixing")

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
MsgC(Color(200, 200, 200), "  Automatically detects and fixes solid-color textures as they load.\n")
MsgC(Color(200, 200, 200), "  Commands: rtx_fix_hash_collisions_stats, rtx_fix_hash_collisions_check <mat>\n")
