--[[
    Hash Collision Fixer for RTX Remix
    
    When multiple materials use textures with identical pixel content (like solid colors),
    RTX Remix computes the same hash for them, causing them to be treated as the same
    texture. This script fixes that by giving solid-color materials unique textures.
    
    How it works:
    1. C++ reads VTF files directly (via VTFParser) to detect solid-color textures
    2. Lua calls HashCollisionFixer.CheckMaterial() for materials it encounters
    3. Solid-color materials are given unique textures via mat:SetTexture()
    4. RTX Remix now computes unique hashes for each material
    
    Hook: RTX_SolidColorDetected(materialName, r, g, b, a)
    - Called when a solid-color texture is detected
    - Return true from your hook to prevent the default fix
    
    NOTE: VTF-based detection reads texture files directly, bypassing D3D9/DXVK entirely.
    This is safe and doesn't cause crashes.
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
-- Materials we've already checked
local checkedMaterials = {}

-- Timer for processing materials
local TIMER_NAME = "RTX_SolidColorProcessor"
local SCAN_INTERVAL = 0.5 -- Check every 0.5 seconds

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
            if HashCollisionFixer and HashCollisionFixer.MarkMaterialFixed then
                HashCollisionFixer.MarkMaterialFixed(matName)
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
hook.Add("RTX_SolidColorDetected", "RTX_SolidColorFixer", function(materialName, r, g, b, a)
    if not enable_addon:GetBool() then return end
    
    DebugPrint("Solid color detected: ", materialName, " (RGBA: ", r, ",", g, ",", b, ",", a, ")")
    
    -- Fix the material
    FixSolidColorMaterial(materialName)
end)

-- Check a material for solid-color texture using VTF parser
local function CheckMaterialForSolidColor(matName)
    if checkedMaterials[matName] then return false end
    checkedMaterials[matName] = true
    
    -- Skip if already fixed
    if HashCollisionFixer and HashCollisionFixer.IsMaterialFixed then
        if HashCollisionFixer.IsMaterialFixed(matName) then
            return false
        end
    end
    
    -- Get the material
    local mat = Material(matName)
    if not mat or mat:IsError() then return false end
    
    -- Get $basetexture path
    local baseTex = mat:GetTexture("$basetexture")
    if not baseTex then return false end
    
    local texPath = baseTex:GetName()
    if not texPath or texPath == "" then return false end
    
    -- Check with C++ VTF parser
    if HashCollisionFixer and HashCollisionFixer.CheckMaterial then
        local isSolid = HashCollisionFixer.CheckMaterial(matName, texPath, debug_mode:GetBool())
        
        if isSolid then
            DebugPrint("VTF solid-color detected: ", matName, " (", texPath, ")")
            
            -- Get the color
            local r, g, b, a = 0, 0, 0, 255
            if HashCollisionFixer.GetMaterialColor then
                r, g, b, a = HashCollisionFixer.GetMaterialColor(matName)
            end
            
            -- Fire hook for custom handlers
            local prevented = hook.Call("RTX_SolidColorDetected", nil, matName, r, g, b, a)
            
            if not prevented then
                FixSolidColorMaterial(matName)
            end
            
            return true
        end
    end
    
    return false
end

-- Scan materials for solid colors
local function ScanMaterials()
    if not enable_addon:GetBool() then return end
    
    -- First, check if there are any materials needing fix from C++
    if HashCollisionFixer and HashCollisionFixer.GetMaterialsNeedingFix then
        local materials = HashCollisionFixer.GetMaterialsNeedingFix()
        
        for _, matName in ipairs(materials) do
            if not checkedMaterials[matName] then
                checkedMaterials[matName] = true
                
                -- Get color
                local r, g, b, a = 0, 0, 0, 255
                if HashCollisionFixer.GetMaterialColor then
                    r, g, b, a = HashCollisionFixer.GetMaterialColor(matName)
                end
                
                -- Fire hook
                local prevented = hook.Call("RTX_SolidColorDetected", nil, matName, r, g, b, a)
                
                if not prevented then
                    FixSolidColorMaterial(matName)
                end
            end
        end
    end
end

-- Start the processing timer
local function StartProcessing()
    if timer.Exists(TIMER_NAME) then
        timer.Remove(TIMER_NAME)
    end
    
    -- Process periodically
    timer.Create(TIMER_NAME, SCAN_INTERVAL, 0, ScanMaterials)
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
    checkedMaterials = {}
    
    -- Reset C++ state
    if HashCollisionFixer and HashCollisionFixer.Reset then
        HashCollisionFixer.Reset()
    end
end)

-- Console commands
concommand.Add("rtx_fix_hash_collisions_process", function()
    ScanMaterials()
    notification.AddLegacy("Processed solid-color textures", NOTIFY_GENERIC, 3)
end, nil, "Manually trigger solid-color texture processing")

concommand.Add("rtx_fix_hash_collisions_stats", function()
    MsgC(Color(100, 200, 255), "[RTX SolidFix] Statistics:\n")
    MsgC(Color(200, 200, 200), "  Total fixed (Lua): ", fixCount, "\n")
    MsgC(Color(200, 200, 200), "  Unique textures created: ", table.Count(uniqueTextures), "\n")
    MsgC(Color(200, 200, 200), "  Materials checked: ", table.Count(checkedMaterials), "\n")
    
    -- Show C++ stats if available
    if HashCollisionFixer and HashCollisionFixer.GetStats then
        local stats = HashCollisionFixer.GetStats()
        MsgC(Color(200, 200, 200), "  C++ detected: ", stats.totalDetected, "\n")
        MsgC(Color(200, 200, 200), "  C++ fixed: ", stats.totalFixed, "\n")
        MsgC(Color(200, 200, 200), "  C++ pending: ", stats.pending, "\n")
    end
end, nil, "Show solid-color fix statistics")

concommand.Add("rtx_fix_hash_collisions_list", function()
    if not HashCollisionFixer or not HashCollisionFixer.GetMaterialsNeedingFix then
        MsgC(Color(255, 200, 100), "[RTX SolidFix] HashCollisionFixer not available.\n")
        return
    end
    
    local materials = HashCollisionFixer.GetMaterialsNeedingFix()
    
    MsgC(Color(100, 200, 255), "[RTX SolidFix] Materials needing fix: ", #materials, "\n")
    
    for i, matName in ipairs(materials) do
        local r, g, b, a = 0, 0, 0, 255
        if HashCollisionFixer.GetMaterialColor then
            r, g, b, a = HashCollisionFixer.GetMaterialColor(matName)
        end
        MsgC(Color(200, 200, 200), "  ", i, ". ", matName, " (RGBA: ", r, ",", g, ",", b, ",", a, ")\n")
    end
end, nil, "List solid-color materials needing fix")

concommand.Add("rtx_fix_hash_collisions_check", function(ply, cmd, args)
    if #args < 1 then
        MsgC(Color(255, 200, 100), "[RTX SolidFix] Usage: rtx_fix_hash_collisions_check <material_name>\n")
        return
    end
    
    local matName = args[1]
    
    -- Check local state
    if checkedMaterials[matName] then
        MsgC(Color(100, 255, 100), "[RTX SolidFix] '", matName, "' has been checked by Lua.\n")
    else
        MsgC(Color(200, 200, 200), "[RTX SolidFix] '", matName, "' has NOT been checked by Lua.\n")
    end
    
    -- Check C++ state
    if HashCollisionFixer then
        if HashCollisionFixer.IsMaterialFixed then
            local isFixed = HashCollisionFixer.IsMaterialFixed(matName)
            MsgC(Color(200, 200, 200), "[RTX SolidFix] C++ fixed status: ", isFixed and "YES" or "NO", "\n")
        end
        
        if HashCollisionFixer.IsSolidColorMaterial then
            local isSolid = HashCollisionFixer.IsSolidColorMaterial(matName)
            MsgC(Color(200, 200, 200), "[RTX SolidFix] Is solid-color: ", isSolid and "YES" or "NO", "\n")
            
            if isSolid and HashCollisionFixer.GetMaterialColor then
                local r, g, b, a = HashCollisionFixer.GetMaterialColor(matName)
                if r then
                    MsgC(Color(200, 200, 200), "[RTX SolidFix] Color: RGBA(", r, ",", g, ",", b, ",", a, ")\n")
                end
            end
        end
    end
end, nil, "Check if a material is solid-color and its fix status")

concommand.Add("rtx_fix_hash_collisions_add", function(ply, cmd, args)
    if #args < 1 then
        MsgC(Color(255, 200, 100), "[RTX SolidFix] Usage: rtx_fix_hash_collisions_add <material_name>\n")
        return
    end
    
    local matName = args[1]
    
    if FixSolidColorMaterial(matName) then
        checkedMaterials[matName] = true
        MsgC(Color(100, 255, 100), "[RTX SolidFix] Manually fixed: '", matName, "'\n")
    end
end, nil, "Manually fix a material (force it to have a unique texture)")

concommand.Add("rtx_fix_hash_collisions_scan", function(ply, cmd, args)
    local texPath = args[1]
    
    if not texPath then
        MsgC(Color(255, 200, 100), "[RTX SolidFix] Usage: rtx_fix_hash_collisions_scan <texture_path>\n")
        MsgC(Color(200, 200, 200), "  Example: rtx_fix_hash_collisions_scan Models/ShaderTest/envball_1\n")
        return
    end
    
    if not HashCollisionFixer or not HashCollisionFixer.CheckSolidColor then
        MsgC(Color(255, 200, 100), "[RTX SolidFix] HashCollisionFixer.CheckSolidColor not available.\n")
        return
    end
    
    local isSolid, rOrErr, g, b, a = HashCollisionFixer.CheckSolidColor(texPath, true)
    
    if type(rOrErr) == "string" then
        MsgC(Color(255, 100, 100), "[RTX SolidFix] Error: ", rOrErr, "\n")
    elseif isSolid then
        MsgC(Color(100, 255, 100), "[RTX SolidFix] '", texPath, "' IS solid-color: RGBA(", rOrErr, ",", g, ",", b, ",", a, ")\n")
    else
        MsgC(Color(200, 200, 200), "[RTX SolidFix] '", texPath, "' is NOT solid-color.\n")
    end
end, nil, "Scan a VTF texture to check if it's solid-color")

-- Startup message
MsgC(Color(100, 255, 100), "[RTX SolidFix] Solid-Color Texture Fixer loaded.\n")
MsgC(Color(200, 200, 200), "  Uses VTF parsing to detect solid-color textures (safe with DXVK).\n")
MsgC(Color(200, 200, 200), "  Hook: RTX_SolidColorDetected(materialName, r, g, b, a)\n")
MsgC(Color(200, 200, 200), "  Commands: rtx_fix_hash_collisions_stats, rtx_fix_hash_collisions_scan\n")
