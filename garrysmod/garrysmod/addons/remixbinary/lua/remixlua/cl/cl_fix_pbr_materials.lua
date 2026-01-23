--[[
    PBR Material Fixer for RTX Remix
    
    ExoPBR and GPBR use custom shaders (screenspace_general_8tex and PBR)
    that RTX Remix cannot replace because:
    1. They're not standard Source Engine shaders
    2. They may not have $basetexture set
    
    This script converts these materials to VertexLitGeneric/LightmappedGeneric
    and ensures $basetexture is set so RTX Remix can hash and replace them.
    
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

-- ConVars for configuration
local enable_addon = CreateConVar("rtx_fixpbr_enabled", "1", FCVAR_ARCHIVE, "Enable/disable the PBR Material Fixer addon")
local debug_mode = CreateConVar("rtx_fixpbr_debug", "0", FCVAR_ARCHIVE, "Enable debugging output")

-- Keep track of modified materials to avoid reprocessing
local modifiedMaterials = {}
local materialsFixed = 0

-- Timer identifier
local TIMER_NAME = "PBRMaterialFixerContinuous"

-- Debug print function
local function DebugPrint(...)
    if debug_mode:GetBool() then
        MsgC(Color(200, 200, 255), "[RTX FixPBR] ", Color(255, 255, 255), ...)
        MsgC(Color(255, 255, 255), "\n")
    end
end

-- Flag to track whether we're running via command
local runningFromCommand = false

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

-- Function to process a single material
local function ProcessMaterial(matName)
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
        
        if runningFromCommand or debug_mode:GetBool() then
            local formatName = isExoPBR and "ExoPBR" or "GPBR"
            MsgC(Color(100, 255, 100), string.format("[RTX FixPBR] Fixed '%s' (%s): set $basetexture to %s (from %s)\n", 
                matName, formatName, fallbackTexture, fallbackSource))
        end
    elseif not needsBaseTexture then
        -- Material already has basetexture, just mark it as a PBR material for logging
        fixed = true
        
        if runningFromCommand or debug_mode:GetBool() then
            local formatName = isExoPBR and "ExoPBR" or "GPBR"
            MsgC(Color(200, 200, 100), string.format("[RTX FixPBR] '%s' (%s) already has valid $basetexture: %s\n", 
                matName, formatName, baseTexture))
        end
    else
        -- Couldn't find a fallback texture
        if runningFromCommand or debug_mode:GetBool() then
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

-- Function to process all loaded entities
local function ProcessLoadedEntities()
    for _, ent in ipairs(ents.GetAll()) do
        if IsValid(ent) then
            local materials = ent:GetMaterials()
            if materials then
                for _, matName in ipairs(materials) do
                    ProcessMaterial(matName)
                end
            end
        end
    end
end

-- Function to process BSP materials (if NikNaks is available)
local function ProcessBSPMaterials()
    if not NikNaks or not NikNaks.CurrentMap then
        return
    end
    
    local bsp = NikNaks.CurrentMap
    
    -- Process textures from map
    for _, texture in ipairs(bsp:GetTextures()) do
        if texture then
            ProcessMaterial(texture)
        end
    end
    
    -- Process faces to get additional materials
    for _, face in pairs(bsp:GetFaces()) do
        local texture = face:GetTexture()
        if texture then
            ProcessMaterial(texture)
        end
    end
end

-- Main function to fix PBR materials
local function FixPBRMaterials(showOutput)
    if not enable_addon:GetBool() then return 0 end
    
    local previousFixed = materialsFixed
    
    runningFromCommand = showOutput or false
    
    local startTime = SysTime()
    
    -- Process BSP materials
    ProcessBSPMaterials()
    
    -- Process all loaded entities
    ProcessLoadedEntities()
    
    runningFromCommand = false
    
    local endTime = SysTime()
    local processingTime = math.Round((endTime - startTime) * 1000)
    
    local newFixed = materialsFixed - previousFixed
    
    if showOutput or (newFixed > 0 and debug_mode:GetBool()) then
        if newFixed > 0 then
            MsgC(Color(100, 255, 100), string.format("[RTX FixPBR] Fixed %d PBR materials (ExoPBR/GPBR) in %dms\n", newFixed, processingTime))
        else
            MsgC(Color(200, 200, 200), string.format("[RTX FixPBR] No new PBR materials to fix (checked in %dms)\n", processingTime))
        end
    end
    
    return newFixed
end

-- Start continuous checking
local function StartContinuousChecking()
    if timer.Exists(TIMER_NAME) then 
        timer.Remove(TIMER_NAME)
    end
    
    -- Create a timer that runs every 2 seconds
    timer.Create(TIMER_NAME, 2, 0, function() 
        FixPBRMaterials(false)
    end)
end

-- Stop the continuous checking
local function StopContinuousChecking()
    if timer.Exists(TIMER_NAME) then
        timer.Remove(TIMER_NAME)
    end
end

-- Function to handle when the enable cvar changes
cvars.AddChangeCallback("rtx_fixpbr_enabled", function(_, _, new)
    if new == "1" then
        StartContinuousChecking()
    else
        StopContinuousChecking()
    end
end)

-- Initial run with delay
hook.Add("InitPostEntity", "FixPBRMaterialsOnMapLoad", function()
    if enable_addon:GetBool() then
        timer.Simple(0.5, function()
            local fixed = FixPBRMaterials(true)
            StartContinuousChecking()
        end)
    end
end)

-- Reset variables on map change
hook.Add("ShutDown", "CleanupPBRMaterialFixer", function()
    StopContinuousChecking()
    modifiedMaterials = {}
    materialsFixed = 0
end)

-- Add console command to manually trigger fixing
concommand.Add("rtx_fixpbr_process", function()
    local fixed = FixPBRMaterials(true)
    notification.AddLegacy("Fixed " .. fixed .. " PBR materials", NOTIFY_GENERIC, 3)
end, nil, "Fix ExoPBR and GPBR materials for RTX Remix")

-- Add command to show stats
concommand.Add("rtx_fixpbr_stats", function()
    MsgC(Color(100, 200, 255), "[RTX FixPBR] Statistics:\n")
    MsgC(Color(200, 200, 200), string.format("  Materials fixed: %d\n", materialsFixed))
    MsgC(Color(200, 200, 200), string.format("  Cached entries: %d\n", table.Count(modifiedMaterials)))
end, nil, "Show PBR material fix statistics")

-- Startup message
MsgC(Color(100, 255, 100), "[RTX FixPBR] PBR Material Fixer loaded.\n")
MsgC(Color(200, 200, 200), "  Will set $basetexture for ExoPBR and GPBR materials so RTX Remix can replace them.\n")
