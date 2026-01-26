--[[
    Hash Collision Fixer for RTX Remix
    
    When multiple materials use textures with identical pixel content (like solid colors),
    RTX Remix computes the same hash for them, causing them to be treated as the same
    texture. This script fixes that by giving colliding materials unique textures.
    
    How it works:
    1. C++ tracks texture hashes and detects when multiple materials share the same hash
    2. Lua periodically queries C++ for hash collisions via RemixMaterial.GetHashCollisions()
    3. For each collision group, all but the first material get a unique texture swap
    4. RTX Remix now computes unique hashes for each material
    
    Hook: RTX_HashCollisionDetected(materialName, hash, collisionGroup)
    - Called when a hash collision is detected and needs fixing
    - Return true from your hook to prevent the default fix
    
    NOTE: C++ solid-color detection via LockRect has been disabled because it causes
    crashes with DXVK/RTX Remix. We now rely purely on hash collision detection.
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

-- Hook handler for when C++ detects a solid-color texture (DEPRECATED - no longer called)
-- Kept for backwards compatibility with custom hooks
hook.Add("RTX_SolidColorDetected", "RTX_SolidColorFixer", function(materialName)
    if not enable_addon:GetBool() then return end
    
    DebugPrint("Solid color detected (legacy): ", materialName)
    
    -- Fix the material
    FixSolidColorMaterial(materialName)
end)

-- Hook handler for hash collisions
hook.Add("RTX_HashCollisionDetected", "RTX_HashCollisionFixer", function(materialName, hash, collisionGroup)
    if not enable_addon:GetBool() then return end
    
    DebugPrint("Hash collision detected: ", materialName, " (hash: ", string.format("0x%X", hash), ")")
    
    -- Fix the material
    FixSolidColorMaterial(materialName)
end)

-- Track which collisions we've already processed
local processedCollisions = {}

-- Process hash collisions from C++
local function ProcessHashCollisions()
    if not enable_addon:GetBool() then return end
    if not RemixMaterial or not RemixMaterial.GetHashCollisions then return end
    
    -- Get collision groups from C++
    local collisions = RemixMaterial.GetHashCollisions()
    if not collisions then return end
    
    local fixedThisRound = 0
    
    for hash, materials in pairs(collisions) do
        -- Skip the first material in each collision group (it's the "original")
        -- Fix all others
        for i = 2, #materials do
            local matName = materials[i]
            
            -- Check if we already processed this
            if not processedCollisions[matName] then
                processedCollisions[matName] = true
                
                -- Fire the hook - allows custom handlers to intercept
                local prevented = hook.Call("RTX_HashCollisionDetected", nil, matName, hash, materials)
                
                if not prevented then
                    if FixSolidColorMaterial(matName) then
                        fixedThisRound = fixedThisRound + 1
                    end
                end
            end
        end
    end
    
    if fixedThisRound > 0 and debug_mode:GetBool() then
        DebugPrint("Fixed ", fixedThisRound, " colliding materials this round")
    end
end

-- Process pending solid-color materials (calls C++ which fires hooks)
-- DEPRECATED: C++ solid-color detection is disabled due to DXVK crashes
local function ProcessPending()
    if not enable_addon:GetBool() then return end
    
    -- Process hash collisions (new approach)
    ProcessHashCollisions()
    
    -- Legacy: try to process solid colors if the API exists
    if RemixMaterial and RemixMaterial.ProcessPendingSolidColors then
        local count = RemixMaterial.ProcessPendingSolidColors()
        
        if count > 0 and debug_mode:GetBool() then
            DebugPrint("Processed ", count, " pending solid-color materials (legacy)")
        end
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
    MsgC(Color(100, 200, 255), "[RTX HashFix] Statistics:\n")
    MsgC(Color(200, 200, 200), "  Total fixed: ", fixCount, "\n")
    MsgC(Color(200, 200, 200), "  Unique textures created: ", table.Count(uniqueTextures), "\n")
    MsgC(Color(200, 200, 200), "  Collisions processed: ", table.Count(processedCollisions), "\n")
    
    -- Show hash collisions from C++
    if RemixMaterial and RemixMaterial.GetHashCollisions then
        local collisions = RemixMaterial.GetHashCollisions()
        local collisionCount = 0
        local totalColliding = 0
        
        for hash, materials in pairs(collisions) do
            collisionCount = collisionCount + 1
            totalColliding = totalColliding + #materials
        end
        
        MsgC(Color(200, 200, 200), "  Hash collision groups: ", collisionCount, "\n")
        MsgC(Color(200, 200, 200), "  Total colliding materials: ", totalColliding, "\n")
        
        if collisionCount > 0 and collisionCount <= 10 then
            MsgC(Color(255, 200, 100), "\n  Collision groups:\n")
            for hash, materials in pairs(collisions) do
                MsgC(Color(200, 200, 200), "    Hash 0x", string.format("%X", hash), ":\n")
                for _, matName in ipairs(materials) do
                    local fixed = processedCollisions[matName] and " [FIXED]" or ""
                    MsgC(Color(200, 200, 200), "      - ", matName, fixed, "\n")
                end
            end
        end
    end
end, nil, "Show hash collision fix statistics")

concommand.Add("rtx_fix_hash_collisions_list", function()
    if not RemixMaterial or not RemixMaterial.GetHashCollisions then
        MsgC(Color(255, 200, 100), "[RTX HashFix] RemixMaterial.GetHashCollisions not available.\n")
        return
    end
    
    local collisions = RemixMaterial.GetHashCollisions()
    
    local collisionCount = 0
    for _ in pairs(collisions) do
        collisionCount = collisionCount + 1
    end
    
    MsgC(Color(100, 200, 255), "[RTX HashFix] Hash collision groups: ", collisionCount, "\n")
    
    for hash, materials in pairs(collisions) do
        MsgC(Color(255, 200, 100), "  Hash 0x", string.format("%X", hash), " (", #materials, " materials):\n")
        for i, matName in ipairs(materials) do
            local status = ""
            if i == 1 then
                status = " [ORIGINAL]"
            elseif processedCollisions[matName] then
                status = " [FIXED]"
            else
                status = " [PENDING]"
            end
            MsgC(Color(200, 200, 200), "    - ", matName, status, "\n")
        end
    end
end, nil, "List hash collision groups")

concommand.Add("rtx_fix_hash_collisions_check", function(ply, cmd, args)
    if #args < 1 then
        MsgC(Color(255, 200, 100), "[RTX HashFix] Usage: rtx_fix_hash_collisions_check <material_name>\n")
        return
    end
    
    local matName = args[1]
    
    if not RemixMaterial then
        MsgC(Color(255, 200, 100), "[RTX HashFix] RemixMaterial not available.\n")
        return
    end
    
    -- Check if it's been fixed locally
    if processedCollisions[matName] then
        MsgC(Color(100, 255, 100), "[RTX HashFix] '", matName, "' has been FIXED by Lua.\n")
    else
        MsgC(Color(200, 200, 200), "[RTX HashFix] '", matName, "' has NOT been fixed by Lua.\n")
    end
    
    -- Check C++ fixed status
    if RemixMaterial.IsMaterialFixed then
        local isFixed = RemixMaterial.IsMaterialFixed(matName)
        MsgC(Color(200, 200, 200), "[RTX HashFix] C++ fixed status: ", isFixed and "YES" or "NO", "\n")
    end
    
    -- Check for collisions involving this material
    if RemixMaterial.GetMaterialCollisions then
        local collisions = RemixMaterial.GetMaterialCollisions(matName)
        if collisions and #collisions > 0 then
            MsgC(Color(255, 200, 100), "[RTX HashFix] '", matName, "' collides with:\n")
            for _, other in ipairs(collisions) do
                MsgC(Color(200, 200, 200), "    - ", other, "\n")
            end
        else
            MsgC(Color(200, 200, 200), "[RTX HashFix] '", matName, "' has no hash collisions.\n")
        end
    end
end, nil, "Check if a material has hash collisions and its fix status")

concommand.Add("rtx_fix_hash_collisions_add", function(ply, cmd, args)
    if #args < 1 then
        MsgC(Color(255, 200, 100), "[RTX HashFix] Usage: rtx_fix_hash_collisions_add <material_name>\n")
        return
    end
    
    local matName = args[1]
    
    if FixSolidColorMaterial(matName) then
        processedCollisions[matName] = true
        MsgC(Color(100, 255, 100), "[RTX HashFix] Manually fixed: '", matName, "'\n")
    end
end, nil, "Manually fix a material (force it to have a unique texture)")

-- Startup message
MsgC(Color(100, 255, 100), "[RTX HashFix] Hash Collision Fixer loaded.\n")
MsgC(Color(200, 200, 200), "  Hook: RTX_HashCollisionDetected(materialName, hash, collisionGroup)\n")
MsgC(Color(200, 200, 200), "  Commands: rtx_fix_hash_collisions_stats, rtx_fix_hash_collisions_list\n")
