-- Refract Material Fixer for RTX Remix
-- Refract shader materials often don't have $basetexture, which means RTX Remix
-- can't create a replacement material hash. This addon sets $basetexture to the
-- $normalmap or $dudvmap so RTX Remix has a texture to use for material replacement.

if not (BRANCH == "x86-64" or BRANCH == "chromium") then return end

-- ConVars for configuration
local enable_addon = CreateConVar("rtx_fixrefract_enabled", "1", FCVAR_ARCHIVE, "Enable/disable the Refract Material Fixer addon")
local debug_mode = CreateConVar("rtx_fixrefract_debug", "0", FCVAR_ARCHIVE, "Enable debugging output")
local apply_delay = CreateConVar("rtx_fixrefract_delay", "0.5", FCVAR_ARCHIVE, "Delay before initial processing (seconds)")

-- Keep track of modified materials to avoid reprocessing
local modifiedMaterials = {}
local materialsProcessed = 0
local materialsFixed = 0

-- Timer identifier for continuous checking
local TIMER_NAME = "RefractMaterialFixerContinuous"

-- Debug print function
local function DebugPrint(...)
    if debug_mode:GetBool() then
        MsgC(Color(200, 255, 200), "[RTX FixRefract] ", Color(255, 255, 255), ...)
        MsgC(Color(255, 255, 255), "\n")
    end
end

-- Flag to track whether we're running via command (show all output) or automatically (silent)
local runningFromCommand = false

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
    
    if runningFromCommand or debug_mode:GetBool() then
        MsgC(Color(100, 255, 100), string.format("[RTX FixRefract] Fixed '%s': set $basetexture to %s (from %s)\n", matName, fallbackTexture, fallbackSource))
    end
    
    materialsProcessed = materialsProcessed + 1
    materialsFixed = materialsFixed + 1
    modifiedMaterials[matName] = true
    
    return true
end

-- Function to process all loaded entities
local function ProcessLoadedEntities()
    -- Process entities in the world
    for _, ent in ipairs(ents.GetAll()) do
        if IsValid(ent) then
            -- Process entity's materials
            if ent:GetMaterials() then
                for _, matName in ipairs(ent:GetMaterials()) do
                    ProcessMaterial(matName)
                end
            end

            -- Process model
            local modelName = ent:GetModel()
            if modelName then
                ProcessMaterial(modelName)
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

-- Main function to fix refract materials
local function FixRefractMaterials(showOutput)
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
            MsgC(Color(100, 255, 100), string.format("[RTX FixRefract] Fixed %d Refract materials in %dms\n", newFixed, processingTime))
        else
            MsgC(Color(200, 200, 200), string.format("[RTX FixRefract] No new Refract materials to fix (checked in %dms)\n", processingTime))
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
        FixRefractMaterials(false)
    end)
end

-- Stop the continuous checking
local function StopContinuousChecking()
    if timer.Exists(TIMER_NAME) then
        timer.Remove(TIMER_NAME)
    end
end

-- Function to handle when the enable cvar changes
cvars.AddChangeCallback("rtx_fixrefract_enabled", function(_, _, new)
    if new == "1" then
        StartContinuousChecking()
    else
        StopContinuousChecking()
    end
end)

-- Initial run with delay
hook.Add("InitPostEntity", "FixRefractMaterialsOnMapLoad", function()
    if enable_addon:GetBool() then
        timer.Simple(apply_delay:GetFloat(), function()
            local fixed = FixRefractMaterials(true)
            StartContinuousChecking()
        end)
    end
end)

-- Reset variables on map change
hook.Add("ShutDown", "CleanupRefractMaterialFixer", function()
    StopContinuousChecking()
    modifiedMaterials = {}
    materialsProcessed = 0
    materialsFixed = 0
end)

-- Add console command to manually trigger fixing
concommand.Add("rtx_fixrefract_process", function()
    local fixed = FixRefractMaterials(true)
    notification.AddLegacy("Fixed " .. fixed .. " Refract materials", NOTIFY_GENERIC, 3)
end, nil, "Fix Refract materials by setting $basetexture from $normalmap/$dudvmap")

-- Add command to show stats
concommand.Add("rtx_fixrefract_stats", function()
    MsgC(Color(100, 200, 255), "[RTX FixRefract] Statistics:\n")
    MsgC(Color(200, 200, 200), string.format("  Materials checked: %d\n", materialsProcessed))
    MsgC(Color(200, 200, 200), string.format("  Materials fixed: %d\n", materialsFixed))
    MsgC(Color(200, 200, 200), string.format("  Cached entries: %d\n", table.Count(modifiedMaterials)))
end, nil, "Show Refract material fix statistics")

-- Startup message
MsgC(Color(100, 255, 100), "[RTX FixRefract] Refract Material Fixer loaded.\n")
MsgC(Color(200, 200, 200), "  Will set $basetexture for Refract materials from $refracttinttexture/$normalmap/$dudvmap.\n")
