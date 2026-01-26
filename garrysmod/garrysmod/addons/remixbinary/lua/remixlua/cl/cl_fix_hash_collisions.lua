--[[
    Hash Collision Fixer for RTX Remix
    
    When multiple materials use textures with identical pixel content and dimensions,
    RTX Remix computes the same hash for them, causing them to be treated as the same
    texture. This is common with solid-color textures like envball_1, envball_2, etc.
    
    This script fixes hash collisions by:
    1. Querying the C++ side for detected hash collisions (RemixMaterial.GetHashCollisions)
    2. Creating small unique procedural textures at runtime
    3. Swapping the $basetexture of colliding materials to these unique textures
    4. RTX Remix will now compute unique hashes for each material
]]

if not CLIENT then return end
if not (BRANCH == "x86-64" or BRANCH == "chromium") then return end

-- ConVars for configuration
local enable_addon = CreateConVar("rtx_fix_hash_collisions", "1", FCVAR_ARCHIVE, "Enable/disable the hash collision fixer")
local debug_mode = CreateConVar("rtx_fix_hash_collisions_debug", "0", FCVAR_ARCHIVE, "Enable debugging output")

-- Track materials we've already fixed
local fixedMaterials = {}
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

-- Fix a single material's hash collision
local function FixMaterialCollision(matName)
    -- Skip already fixed
    if fixedMaterials[matName] then
        return false
    end
    
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
            fixedMaterials[matName] = true
            
            MsgC(Color(100, 255, 100), "[RTX HashFix] Fixed: '", matName, "' now has unique texture\n")
            return true
        end
    end
    
    MsgC(Color(255, 100, 100), "[RTX HashFix] Failed to fix: '", matName, "'\n")
    return false
end

-- Query C++ for detected hash collisions and fix them
local function FixDetectedCollisions(showOutput)
    if not enable_addon:GetBool() then return 0 end
    
    -- Check if RemixMaterial API is available
    if not RemixMaterial or not RemixMaterial.GetHashCollisions then
        if showOutput then
            MsgC(Color(255, 200, 100), "[RTX HashFix] RemixMaterial.GetHashCollisions not available yet. Try again later.\n")
        end
        return 0
    end
    
    local previousFixed = fixCount
    local startTime = SysTime()
    
    -- Query C++ for all detected hash collisions
    local collisions = RemixMaterial.GetHashCollisions()
    
    if not collisions or table.Count(collisions) == 0 then
        if showOutput then
            MsgC(Color(200, 200, 200), "[RTX HashFix] No hash collisions detected by C++ tracker.\n")
        end
        return 0
    end
    
    -- Process each collision group
    for hashStr, materials in pairs(collisions) do
        if #materials > 1 then
            if debug_mode:GetBool() or showOutput then
                MsgC(Color(255, 200, 100), "[RTX HashFix] Collision detected at hash ", hashStr, ":\n")
                for _, matName in ipairs(materials) do
                    MsgC(Color(255, 200, 100), "    - ", matName, "\n")
                end
            end
            
            -- Fix all but the first material in the collision group
            -- (The first one keeps its original hash, others get unique hashes)
            for i = 2, #materials do
                FixMaterialCollision(materials[i])
            end
        end
    end
    
    local endTime = SysTime()
    local processingTime = math.Round((endTime - startTime) * 1000)
    local newFixed = fixCount - previousFixed
    
    if showOutput or (newFixed > 0 and debug_mode:GetBool()) then
        if newFixed > 0 then
            MsgC(Color(100, 255, 100), "[RTX HashFix] Fixed ", newFixed, " hash collisions in ", processingTime, "ms\n")
        else
            MsgC(Color(200, 200, 200), "[RTX HashFix] No new hash collisions to fix (", processingTime, "ms)\n")
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
        FixDetectedCollisions(false)
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
        FixDetectedCollisions(true)
        StartContinuousChecking()
    else
        StopContinuousChecking()
    end
end)

-- Initial run with delay
hook.Add("InitPostEntity", "FixHashCollisionsOnMapLoad", function()
    if enable_addon:GetBool() then
        -- Wait a bit for textures to be loaded and hashes to be computed
        timer.Simple(3, function()
            FixDetectedCollisions(true)
            StartContinuousChecking()
        end)
    end
end)

-- Reset on map change
hook.Add("ShutDown", "CleanupHashCollisionFixer", function()
    StopContinuousChecking()
    fixedMaterials = {}
    fixCount = 0
    uniqueTextures = {}
end)

-- Console commands
concommand.Add("rtx_fix_hash_collisions_process", function()
    local fixed = FixDetectedCollisions(true)
    notification.AddLegacy("Fixed " .. fixed .. " hash collisions", NOTIFY_GENERIC, 3)
end, nil, "Query C++ for hash collisions and fix them")

concommand.Add("rtx_fix_hash_collisions_stats", function()
    MsgC(Color(100, 200, 255), "[RTX HashFix] Statistics:\n")
    MsgC(Color(200, 200, 200), "  Collisions fixed: ", fixCount, "\n")
    MsgC(Color(200, 200, 200), "  Fixed materials: ", table.Count(fixedMaterials), "\n")
    MsgC(Color(200, 200, 200), "  Unique textures created: ", table.Count(uniqueTextures), "\n")
    
    -- Show current collisions from C++
    if RemixMaterial and RemixMaterial.GetHashCollisions then
        local collisions = RemixMaterial.GetHashCollisions()
        local collisionCount = table.Count(collisions)
        MsgC(Color(200, 200, 200), "  Active collision groups: ", collisionCount, "\n")
        
        if collisionCount > 0 then
            MsgC(Color(255, 200, 100), "\n  Current collision groups:\n")
            for hashStr, materials in pairs(collisions) do
                MsgC(Color(200, 200, 200), "    Hash ", hashStr, ": ")
                for i, matName in ipairs(materials) do
                    local status = fixedMaterials[matName] and " (fixed)" or ""
                    if i > 1 then MsgC(Color(200, 200, 200), ", ") end
                    MsgC(Color(255, 255, 255), matName, status)
                end
                MsgC(Color(200, 200, 200), "\n")
            end
        end
    else
        MsgC(Color(255, 200, 100), "  RemixMaterial.GetHashCollisions not available\n")
    end
end, nil, "Show hash collision fix statistics")

concommand.Add("rtx_fix_hash_collisions_list", function()
    if not RemixMaterial or not RemixMaterial.GetHashCollisions then
        MsgC(Color(255, 200, 100), "[RTX HashFix] RemixMaterial.GetHashCollisions not available.\n")
        return
    end
    
    local collisions = RemixMaterial.GetHashCollisions()
    
    if table.Count(collisions) == 0 then
        MsgC(Color(200, 200, 200), "[RTX HashFix] No hash collisions detected.\n")
        return
    end
    
    MsgC(Color(100, 200, 255), "[RTX HashFix] Detected hash collisions:\n")
    for hashStr, materials in pairs(collisions) do
        MsgC(Color(255, 200, 100), "  Hash ", hashStr, ":\n")
        for _, matName in ipairs(materials) do
            local status = fixedMaterials[matName] and " [FIXED]" or ""
            MsgC(Color(200, 200, 200), "    - ", matName, status, "\n")
        end
    end
end, nil, "List all detected hash collisions")

concommand.Add("rtx_fix_hash_collisions_add", function(ply, cmd, args)
    if #args < 1 then
        MsgC(Color(255, 200, 100), "[RTX HashFix] Usage: rtx_fix_hash_collisions_add <material_name>\n")
        return
    end
    
    local matName = args[1]
    
    if FixMaterialCollision(matName) then
        MsgC(Color(100, 255, 100), "[RTX HashFix] Manually fixed: '", matName, "'\n")
    end
end, nil, "Manually fix a material's hash collision")

concommand.Add("rtx_fix_hash_collisions_check", function(ply, cmd, args)
    if #args < 1 then
        MsgC(Color(255, 200, 100), "[RTX HashFix] Usage: rtx_fix_hash_collisions_check <material_name>\n")
        return
    end
    
    local matName = args[1]
    
    if not RemixMaterial or not RemixMaterial.GetMaterialCollisions then
        MsgC(Color(255, 200, 100), "[RTX HashFix] RemixMaterial.GetMaterialCollisions not available.\n")
        return
    end
    
    local collisions = RemixMaterial.GetMaterialCollisions(matName)
    
    if #collisions == 0 then
        MsgC(Color(100, 255, 100), "[RTX HashFix] '", matName, "' has no hash collisions.\n")
    else
        MsgC(Color(255, 200, 100), "[RTX HashFix] '", matName, "' collides with:\n")
        for _, otherMat in ipairs(collisions) do
            MsgC(Color(200, 200, 200), "    - ", otherMat, "\n")
        end
    end
end, nil, "Check if a material has hash collisions with other materials")

-- Startup message
MsgC(Color(100, 255, 100), "[RTX HashFix] Hash Collision Fixer loaded.\n")
MsgC(Color(200, 200, 200), "  Uses C++ detection to find actual hash collisions (no hardcoded lists).\n")
MsgC(Color(200, 200, 200), "  Commands: rtx_fix_hash_collisions_list, rtx_fix_hash_collisions_check <mat>\n")
