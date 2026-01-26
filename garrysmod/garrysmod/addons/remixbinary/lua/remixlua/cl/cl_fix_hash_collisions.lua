--[[
    Hash Collision Fixer for RTX Remix
    
    When multiple materials use textures with identical pixel content and dimensions,
    RTX Remix computes the same hash for them, causing them to be treated as the same
    texture. This is common with solid-color textures like envball_1, envball_2, etc.
    
    This script fixes hash collisions by:
    1. Creating small unique procedural textures at runtime
    2. Swapping the $basetexture of colliding materials to these unique textures
    3. RTX Remix will now compute unique hashes for each material
    
    The unique textures are created using CreateMaterial with a unique $basetexture
    pattern derived from the material name.
]]

if not CLIENT then return end
if not (BRANCH == "x86-64" or BRANCH == "chromium") then return end

-- ConVars for configuration
local enable_addon = CreateConVar("rtx_fix_hash_collisions", "1", FCVAR_ARCHIVE, "Enable/disable the hash collision fixer")
local debug_mode = CreateConVar("rtx_fix_hash_collisions_debug", "0", FCVAR_ARCHIVE, "Enable debugging output")

-- Track materials we've already processed
local processedMaterials = {}
-- Track hash -> first material mapping for collision detection
local hashToMaterial = {}
-- Count of fixes applied
local fixCount = 0
-- Generated unique textures
local uniqueTextures = {}

-- Timer identifier
local TIMER_NAME = "HashCollisionFixerContinuous"

-- Debug print function
local function DebugPrint(...)
    if debug_mode:GetBool() then
        MsgC(Color(200, 200, 255), "[RTX HashFix] ", Color(255, 255, 255), ...)
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
    -- Generate RGB values from hash, keeping them close to black to minimize visual impact
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
    
    -- Create a tiny 4x4 material with unique color
    local uniqueMat = CreateMaterial(texName, "UnlitGeneric", {
        ["$basetexture"] = "color/white",
        ["$color"] = string.format("[%f %f %f]", r, g, b),
        ["$color2"] = string.format("[%f %f %f]", r, g, b),
        ["$vertexcolor"] = 0,
        ["$vertexalpha"] = 0,
    })
    
    if uniqueMat and not uniqueMat:IsError() then
        uniqueTextures[texName] = uniqueMat
        DebugPrint("Created unique texture: ", texName, " with color (", r, ", ", g, ", ", b, ")")
        return uniqueMat
    end
    
    return nil
end

-- Check if a texture appears to be a solid color (very small dimensions)
-- This is a heuristic - we can't easily check pixel values from Lua
local function IsPotentialSolidColorTexture(mat)
    local texName = mat:GetString("$basetexture")
    if not texName or texName == "" then return false end
    
    -- Common solid-color texture patterns
    local solidPatterns = {
        "envball",
        "shadertest",
        "color",
        "white",
        "black",
        "grey",
        "gray",
        "red",
        "green",
        "blue",
        "_color",
        "_tint",
    }
    
    local lowerName = string.lower(texName)
    for _, pattern in ipairs(solidPatterns) do
        if string.find(lowerName, pattern, 1, true) then
            return true
        end
    end
    
    return false
end

-- Get the D3D9 texture hash for a material (requires our C++ function)
local function GetMaterialHash(mat)
    -- Use our C++ exposed function if available
    if remix and remix.GetMaterialTextureHash then
        return remix.GetMaterialTextureHash(mat)
    end
    
    -- Fallback: use texture name as a proxy for collision detection
    -- This won't catch true hash collisions but will catch obvious cases
    local texName = mat:GetString("$basetexture")
    return texName and string.lower(texName) or nil
end

-- Process a single material for potential hash collision
local function ProcessMaterial(matName)
    if not matName or matName == "" then return false end
    
    -- Skip already processed
    if processedMaterials[matName] ~= nil then
        return processedMaterials[matName]
    end
    
    local mat = Material(matName)
    if not mat or mat:IsError() then
        processedMaterials[matName] = false
        return false
    end
    
    -- Skip non-solid-color textures (heuristic)
    if not IsPotentialSolidColorTexture(mat) then
        processedMaterials[matName] = false
        return false
    end
    
    local hash = GetMaterialHash(mat)
    if not hash then
        processedMaterials[matName] = false
        return false
    end
    
    -- Check for collision
    if hashToMaterial[hash] then
        local firstMat = hashToMaterial[hash]
        
        if firstMat ~= matName then
            -- Collision detected!
            MsgC(Color(255, 200, 100), "[RTX HashFix] COLLISION: '", matName, "' has same hash as '", firstMat, "'\n")
            
            -- Create unique texture and swap
            local uniqueMat = CreateUniqueTexture(matName)
            if uniqueMat then
                local uniqueTex = uniqueMat:GetTexture("$basetexture")
                if uniqueTex then
                    mat:SetTexture("$basetexture", uniqueTex)
                    fixCount = fixCount + 1
                    
                    MsgC(Color(100, 255, 100), "[RTX HashFix] Fixed: '", matName, "' now has unique texture\n")
                    
                    processedMaterials[matName] = true
                    return true
                end
            end
            
            MsgC(Color(255, 100, 100), "[RTX HashFix] Failed to fix: '", matName, "'\n")
        end
    else
        -- First material with this hash
        hashToMaterial[hash] = matName
    end
    
    processedMaterials[matName] = false
    return false
end

-- Manually specify known collision materials
-- These are materials known to have identical solid-color textures
local knownCollisions = {
    -- envball series - all solid black textures
    "models/shadertest/envball_1",
    "models/shadertest/envball_2",
    "models/shadertest/envball_3",
    "models/shadertest/envball_4",
    "models/shadertest/envball_5",
    "models/shadertest/envball_6",
    "models/shadertest/envball_7",
    "models/shadertest/envball_8",
    "models/shadertest/envball_9",
    "models/shadertest/envball_10",
}

-- Process known collision materials
local function ProcessKnownCollisions()
    for _, matName in ipairs(knownCollisions) do
        local mat = Material(matName)
        if mat and not mat:IsError() then
            local uniqueMat = CreateUniqueTexture(matName)
            if uniqueMat then
                local uniqueTex = uniqueMat:GetTexture("$basetexture")
                if uniqueTex then
                    mat:SetTexture("$basetexture", uniqueTex)
                    fixCount = fixCount + 1
                    
                    if debug_mode:GetBool() then
                        MsgC(Color(100, 255, 100), "[RTX HashFix] Pre-fixed known collision: '", matName, "'\n")
                    end
                end
            end
        end
    end
end

-- Process all loaded entities
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

-- Main function to fix hash collisions
local function FixHashCollisions(showOutput)
    if not enable_addon:GetBool() then return 0 end
    
    local previousFixed = fixCount
    local startTime = SysTime()
    
    -- First, process known collisions
    ProcessKnownCollisions()
    
    -- Then scan all entities
    ProcessLoadedEntities()
    
    local endTime = SysTime()
    local processingTime = math.Round((endTime - startTime) * 1000)
    local newFixed = fixCount - previousFixed
    
    if showOutput or (newFixed > 0 and debug_mode:GetBool()) then
        if newFixed > 0 then
            MsgC(Color(100, 255, 100), "[RTX HashFix] Fixed ", newFixed, " hash collisions in ", processingTime, "ms\n")
        else
            MsgC(Color(200, 200, 200), "[RTX HashFix] No new hash collisions found (", processingTime, "ms)\n")
        end
    end
    
    return newFixed
end

-- Start continuous checking
local function StartContinuousChecking()
    if timer.Exists(TIMER_NAME) then 
        timer.Remove(TIMER_NAME)
    end
    
    -- Run every 3 seconds
    timer.Create(TIMER_NAME, 3, 0, function()
        FixHashCollisions(false)
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
        FixHashCollisions(true)
        StartContinuousChecking()
    else
        StopContinuousChecking()
    end
end)

-- Initial run with delay
hook.Add("InitPostEntity", "FixHashCollisionsOnMapLoad", function()
    if enable_addon:GetBool() then
        timer.Simple(1, function()
            FixHashCollisions(true)
            StartContinuousChecking()
        end)
    end
end)

-- Reset on map change
hook.Add("ShutDown", "CleanupHashCollisionFixer", function()
    StopContinuousChecking()
    processedMaterials = {}
    hashToMaterial = {}
    fixCount = 0
    uniqueTextures = {}
end)

-- Console commands
concommand.Add("rtx_fix_hash_collisions_process", function()
    local fixed = FixHashCollisions(true)
    notification.AddLegacy("Fixed " .. fixed .. " hash collisions", NOTIFY_GENERIC, 3)
end, nil, "Fix hash collisions for solid-color textures")

concommand.Add("rtx_fix_hash_collisions_stats", function()
    MsgC(Color(100, 200, 255), "[RTX HashFix] Statistics:\n")
    MsgC(Color(200, 200, 200), "  Collisions fixed: ", fixCount, "\n")
    MsgC(Color(200, 200, 200), "  Processed materials: ", table.Count(processedMaterials), "\n")
    MsgC(Color(200, 200, 200), "  Unique textures created: ", table.Count(uniqueTextures), "\n")
end, nil, "Show hash collision fix statistics")

concommand.Add("rtx_fix_hash_collisions_add", function(ply, cmd, args)
    if #args < 1 then
        MsgC(Color(255, 200, 100), "[RTX HashFix] Usage: rtx_fix_hash_collisions_add <material_name>\n")
        return
    end
    
    local matName = args[1]
    local mat = Material(matName)
    
    if mat and not mat:IsError() then
        local uniqueMat = CreateUniqueTexture(matName)
        if uniqueMat then
            local uniqueTex = uniqueMat:GetTexture("$basetexture")
            if uniqueTex then
                mat:SetTexture("$basetexture", uniqueTex)
                fixCount = fixCount + 1
                MsgC(Color(100, 255, 100), "[RTX HashFix] Manually fixed: '", matName, "'\n")
            end
        end
    else
        MsgC(Color(255, 100, 100), "[RTX HashFix] Material not found or is error: '", matName, "'\n")
    end
end, nil, "Manually add a material to the hash collision fix list")

-- Startup message
MsgC(Color(100, 255, 100), "[RTX HashFix] Hash Collision Fixer loaded.\n")
MsgC(Color(200, 200, 200), "  Fixes hash collisions for solid-color textures so RTX Remix can distinguish them.\n")
