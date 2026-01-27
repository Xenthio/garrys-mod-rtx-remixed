--[[
    PBR Material Fixer for RTX Remix
    
    ExoPBR and GPBR use custom shaders (screenspace_general_8tex and PBR)
    that RTX Remix cannot replace because:
    1. They're not standard Source Engine shaders
    2. They may not have $basetexture set
    
    This module provides ProcessMaterial() - called by the unified MaterialPipeline.
    All material discovery is handled by RTXMaterialPipeline.
    
    ExoPBR uses:
      - screenspace_general_8tex shader
      - $texture1 = ARM map (R=AO, G=Roughness, B=Metallic)
      - $texture2 = Normal map
      - $texture3 = Emission
      
    GPBR (Strata Source) uses:
      - PBR shader
      - $mraotexture = MRAO map (R=Metallic, G=Roughness, B=AO)
      - $basetexture = Albedo
      - $bumpmap = Normal map
      - $emissiontexture = Emission
]]

if not (BRANCH == "x86-64" or BRANCH == "chromium") then return end

-- Global module table
RTXFixPBR = RTXFixPBR or {}

-- ConVars for configuration
local enable_addon = CreateConVar("rtx_fixpbr_enabled", "1", FCVAR_ARCHIVE, "Enable/disable the PBR Material Fixer")
local debug_mode = CreateConVar("rtx_fixpbr_debug", "0", FCVAR_ARCHIVE, "Enable debugging output")

-- Keep track of modified materials to avoid reprocessing
local modifiedMaterials = {}
local materialsFixed = 0

-- Function to check if a texture path is valid
local function IsValidTexturePath(path)
    if not path or path == "" then return false end
    if string.find(string.lower(path), "undefined") then return false end
    if string.find(string.lower(path), "error") then return false end
    return true
end

-- Function to detect and fix ExoPBR materials
local function IsExoPBR(mat)
    local shader = mat:GetShader()
    if not shader then return false end
    
    shader = string.lower(shader)
    if shader ~= "screenspace_general_8tex" then return false end
    
    -- Check for ExoPBR proxy marker in the material
    -- This is detected by presence of $texture1 (ARM) or $texture2 (normal)
    local tex1 = mat:GetString("$texture1")
    local tex2 = mat:GetString("$texture2")
    
    return IsValidTexturePath(tex1) or IsValidTexturePath(tex2)
end

-- Function to detect GPBR materials
local function IsGPBR(mat)
    local shader = mat:GetShader()
    if not shader then return false end
    
    shader = string.lower(shader)
    return shader == "pbr"
end

-- Public function to process a single material (called by MaterialPipeline)
function RTXFixPBR.ProcessMaterial(matName)
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
    
    local isExoPBR = IsExoPBR(mat)
    local isGPBR = IsGPBR(mat)
    
    if not isExoPBR and not isGPBR then
        modifiedMaterials[matName] = false
        return false
    end
    
    -- Check if this material already has a valid $basetexture
    local baseTexture = mat:GetString("$basetexture")
    local needsBaseTexture = not IsValidTexturePath(baseTexture)
    
    -- Find a fallback texture for $basetexture
    local fallbackTexture = nil
    local fallbackSource = nil
    
    if needsBaseTexture then
        if isExoPBR then
            -- ExoPBR: Try $texture1 (ARM) first, then $texture2 (normal)
            local tex1 = mat:GetString("$texture1")
            if IsValidTexturePath(tex1) then
                fallbackTexture = tex1
                fallbackSource = "$texture1 (ARM)"
            else
                local tex2 = mat:GetString("$texture2")
                if IsValidTexturePath(tex2) then
                    fallbackTexture = tex2
                    fallbackSource = "$texture2 (Normal)"
                end
            end
        elseif isGPBR then
            -- GPBR: Try $mraotexture, then $bumpmap
            local mrao = mat:GetString("$mraotexture")
            if IsValidTexturePath(mrao) then
                fallbackTexture = mrao
                fallbackSource = "$mraotexture (MRAO)"
            else
                local bump = mat:GetString("$bumpmap")
                if IsValidTexturePath(bump) then
                    fallbackTexture = bump
                    fallbackSource = "$bumpmap"
                end
            end
        end
    end
    
    -- Apply fixes
    local fixed = false
    
    -- Set $basetexture if needed
    if needsBaseTexture and fallbackTexture then
        mat:SetTexture("$basetexture", fallbackTexture)
        fixed = true
        
        if debug_mode:GetBool() then
            local formatName = isExoPBR and "ExoPBR" or "GPBR"
            MsgC(Color(100, 255, 100), string.format("[RTX FixPBR] Fixed '%s' (%s): set $basetexture to %s (from %s)\n", 
                matName, formatName, fallbackTexture, fallbackSource))
        end
    elseif not needsBaseTexture then
        -- Material already has basetexture, just mark it as a PBR material for logging
        fixed = true
        
        if debug_mode:GetBool() then
            local formatName = isExoPBR and "ExoPBR" or "GPBR"
            MsgC(Color(200, 200, 100), string.format("[RTX FixPBR] '%s' (%s) already has valid $basetexture: %s\n", 
                matName, formatName, baseTexture))
        end
    else
        -- Couldn't find a fallback texture
        if debug_mode:GetBool() then
            local formatName = isExoPBR and "ExoPBR" or "GPBR"
            MsgC(Color(255, 200, 100), string.format("[RTX FixPBR] Warning: '%s' (%s) has no usable fallback texture\n", 
                matName, formatName))
        end
    end
    
    if fixed then
        materialsFixed = materialsFixed + 1
    end
    
    modifiedMaterials[matName] = fixed
    return fixed
end

-- Get statistics
function RTXFixPBR.GetStats()
    return {
        fixed = materialsFixed,
        cached = table.Count(modifiedMaterials)
    }
end

-- Clear cache (useful on map change)
function RTXFixPBR.ClearCache()
    modifiedMaterials = {}
    materialsFixed = 0
end

-- Check if enabled
function RTXFixPBR.IsEnabled()
    return enable_addon:GetBool()
end

-- Add command to show stats
concommand.Add("rtx_fixpbr_stats", function()
    local stats = RTXFixPBR.GetStats()
    MsgC(Color(100, 200, 255), "[RTX FixPBR] Statistics:\n")
    MsgC(Color(200, 200, 200), string.format("  Materials fixed: %d\n", stats.fixed))
    MsgC(Color(200, 200, 200), string.format("  Cached entries: %d\n", stats.cached))
end, nil, "Show PBR material fix statistics")

-- Startup message
MsgC(Color(100, 255, 100), "[RTX FixPBR] PBR Material Fixer loaded (processing module).\n")
MsgC(Color(200, 200, 200), "  Provides RTXFixPBR.ProcessMaterial() for MaterialPipeline.\n")
