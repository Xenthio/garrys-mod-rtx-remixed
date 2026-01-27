-- Refract Material Fixer for RTX Remix
-- Refract shader materials often don't have $basetexture, which means RTX Remix
-- can't create a replacement material hash. This module sets $basetexture to the
-- $normalmap or $dudvmap so RTX Remix has a texture to use for material replacement.
--
-- This module provides ProcessMaterial() - called by the unified MaterialPipeline.
-- All material discovery is handled by RTXMaterialPipeline.

if not (BRANCH == "x86-64" or BRANCH == "chromium") then return end

-- Global module table
RTXFixRefract = RTXFixRefract or {}

-- ConVars for configuration
local enable_addon = CreateConVar("rtx_fixrefract_enabled", "1", FCVAR_ARCHIVE, "Enable/disable the Refract Material Fixer")
local debug_mode = CreateConVar("rtx_fixrefract_debug", "0", FCVAR_ARCHIVE, "Enable debugging output")

-- Keep track of modified materials to avoid reprocessing
local modifiedMaterials = {}
local materialsFixed = 0

-- Debug print function
local function DebugPrint(...)
    if debug_mode:GetBool() then
        MsgC(Color(200, 255, 200), "[RTX FixRefract] ", Color(255, 255, 255), ...)
        MsgC(Color(255, 255, 255), "\n")
    end
end

-- Function to check if a string contains a substring (case insensitive)
local function ContainsIgnoreCase(str, substr)
    if not str or not substr then return false end
    return string.find(string.lower(str), string.lower(substr)) ~= nil
end

-- Function to check if a texture path is valid (not empty, not error, not UNDEFINED)
local function IsValidTexturePath(path)
    if not path or path == "" then return false end
    if ContainsIgnoreCase(path, "undefined") then return false end
    if ContainsIgnoreCase(path, "error") then return false end
    if ContainsIgnoreCase(path, "rtx/ignore") then return false end
    return true
end

-- Public function to process a single material (called by MaterialPipeline)
function RTXFixRefract.ProcessMaterial(matName)
    if not matName or matName == "" then 
        return false 
    end
    
    -- Skip already processed materials
    if modifiedMaterials[matName] ~= nil then
        return modifiedMaterials[matName]
    end
    
    local mat = Material(matName)
    if not mat or mat:IsError() then 
        modifiedMaterials[matName] = false
        return false 
    end
    
    -- Get the shader name
    local shaderName = mat:GetShader()
    
    -- Only process Refract shaders or materials without $basetexture that have refract properties
    local isRefract = shaderName and ContainsIgnoreCase(shaderName, "refract")
    
    -- Also check if the material name suggests it's a refract material (sometimes shader name is wrong)
    if not isRefract then
        -- Check for refract-related properties
        local refractAmount = mat:GetFloat("$refractamount")
        if refractAmount and refractAmount > 0 then
            isRefract = true
            DebugPrint(string.format("Material '%s' detected as Refract via $refractamount", matName))
        end
    end
    
    if not isRefract then
        modifiedMaterials[matName] = false
        return false
    end
    
    -- Check if this material already has a valid $basetexture
    local baseTexture = mat:GetString("$basetexture")
    if IsValidTexturePath(baseTexture) then
        DebugPrint(string.format("Material '%s' already has valid $basetexture: %s", matName, baseTexture))
        modifiedMaterials[matName] = false
        return false
    end
    
    -- Try to find a fallback texture
    local fallbackTexture = nil
    local fallbackSource = nil
    
    -- Try $refracttinttexture FIRST (this is the actual color texture for the glass)
    local refractTint = mat:GetString("$refracttinttexture")
    if IsValidTexturePath(refractTint) then
        fallbackTexture = refractTint
        fallbackSource = "$refracttinttexture"
    end
    
    -- Try $normalmap second (common for refract)
    if not fallbackTexture then
        local normalMap = mat:GetString("$normalmap")
        if IsValidTexturePath(normalMap) then
            fallbackTexture = normalMap
            fallbackSource = "$normalmap"
        end
    end
    
    -- Try $dudvmap if no normalmap
    if not fallbackTexture then
        local dudvMap = mat:GetString("$dudvmap")
        if IsValidTexturePath(dudvMap) then
            fallbackTexture = dudvMap
            fallbackSource = "$dudvmap"
        end
    end
    
    -- Try $envmapmask as last resort
    if not fallbackTexture then
        local envmapMask = mat:GetString("$envmapmask")
        if IsValidTexturePath(envmapMask) then
            fallbackTexture = envmapMask
            fallbackSource = "$envmapmask"
        end
    end
    
    if not fallbackTexture then
        DebugPrint(string.format("Material '%s' is Refract but has no usable fallback texture", matName))
        modifiedMaterials[matName] = false
        return false
    end
    
    -- Set the $basetexture to the fallback
    mat:SetTexture("$basetexture", fallbackTexture)
    
    if debug_mode:GetBool() then
        MsgC(Color(100, 255, 100), string.format("[RTX FixRefract] Fixed '%s': set $basetexture to %s (from %s)\n", matName, fallbackTexture, fallbackSource))
    end
    
    materialsFixed = materialsFixed + 1
    modifiedMaterials[matName] = true
    
    return true
end

-- Get statistics
function RTXFixRefract.GetStats()
    return {
        fixed = materialsFixed,
        cached = table.Count(modifiedMaterials)
    }
end

-- Clear cache (useful on map change)
function RTXFixRefract.ClearCache()
    modifiedMaterials = {}
    materialsFixed = 0
end

-- Check if enabled
function RTXFixRefract.IsEnabled()
    return enable_addon:GetBool()
end

-- Add command to show stats
concommand.Add("rtx_fixrefract_stats", function()
    local stats = RTXFixRefract.GetStats()
    MsgC(Color(100, 200, 255), "[RTX FixRefract] Statistics:\n")
    MsgC(Color(200, 200, 200), string.format("  Materials fixed: %d\n", stats.fixed))
    MsgC(Color(200, 200, 200), string.format("  Cached entries: %d\n", stats.cached))
end, nil, "Show Refract material fix statistics")

-- Startup message
MsgC(Color(100, 255, 100), "[RTX FixRefract] Refract Material Fixer loaded (processing module).\n")
MsgC(Color(200, 200, 200), "  Provides RTXFixRefract.ProcessMaterial() for MaterialPipeline.\n")
