--[[
    Physgun Color Sync for RTX Remix
    
    Syncs the player's physgun color (cl_weaponcolor) with RTX Remix's
    legacy emissive color settings for the physgun beam/glow materials.
]]

if not CLIENT then return end

local PhysgunColorSync = {}

-- Physgun-related materials that should have their emissive color synced
-- The main physgun sheet texture that controls the glow color
PhysgunColorSync.PhysgunMaterials = {
    "models/weapons/v_physcannon/v_superphyscannon_sheet",  -- Main physgun texture
    -- Additional effect materials (may or may not be tracked)
    "effects/tool_tracer",
    "sprites/physbeam",
    "sprites/physcannon_bluecore2b",
    "sprites/physcannon_bluecore1b", 
    "sprites/physg_glow1",
    "sprites/physg_glow2",
}

-- Cache of material name -> hash
PhysgunColorSync.MaterialHashes = {}

-- Last applied color (to avoid redundant updates)
PhysgunColorSync.LastColor = nil

-- ConVar for enabling/disabling this feature
PhysgunColorSync.EnabledConVar = CreateClientConVar(
    "remix_physgun_color_sync", 
    "1", 
    true, 
    false, 
    "Enable syncing physgun color to RTX Remix emissive color"
)

-- ConVar for emissive intensity multiplier
PhysgunColorSync.IntensityConVar = CreateClientConVar(
    "remix_physgun_intensity", 
    "5.0", 
    true, 
    false, 
    "Emissive intensity multiplier for physgun glow (0.1-50)"
)

-- Initialize: discover material hashes
function PhysgunColorSync.Initialize()
    if not RemixMaterial then
        timer.Simple(1, PhysgunColorSync.Initialize)
        return
    end
    
    print("[PhysgunColorSync] Initializing...")
    
    -- Force-track physgun materials and get their hashes
    for _, matName in ipairs(PhysgunColorSync.PhysgunMaterials) do
        local mat = Material(matName)
        if mat and not mat:IsError() then
            -- Force track the material
            if RemixMaterial.ForceTrackTexture then
                RemixMaterial.ForceTrackTexture(matName)
            end
        end
    end
    
    -- Immediately try to get hashes and apply color
    PhysgunColorSync.RefreshHashes()
    PhysgunColorSync.ApplyColor()
end

-- Refresh the hash cache
function PhysgunColorSync.RefreshHashes()
    if not RemixMaterial or not RemixMaterial.GetTextureHash then return end
    
    local foundNewHash = false
    
    for _, matName in ipairs(PhysgunColorSync.PhysgunMaterials) do
        -- Skip if we already have this hash
        if PhysgunColorSync.MaterialHashes[matName] then continue end
        
        -- GetTextureHash returns TWO values: number (lossy) and string (accurate)
        -- We MUST use the string version to preserve full 64-bit precision
        local hashNum, hashStr = RemixMaterial.GetTextureHash(matName)
        
        if hashStr and hashStr ~= "0x0" and hashStr ~= "0x0000000000000000" then
            PhysgunColorSync.MaterialHashes[matName] = hashStr
            print(string.format("[PhysgunColorSync] Found hash for '%s': %s", matName, hashStr))
            foundNewHash = true
        end
    end
    
    local count = table.Count(PhysgunColorSync.MaterialHashes)
    if foundNewHash then
        print(string.format("[PhysgunColorSync] Now tracking %d/%d physgun material hashes", 
            count, #PhysgunColorSync.PhysgunMaterials))
    end
    
    return foundNewHash
end

-- Get the current weapon color from the ConVar
function PhysgunColorSync.GetWeaponColor()
    local colorStr = GetConVar("cl_weaponcolor"):GetString()
    local parts = string.Explode(" ", colorStr)
    
    local r = tonumber(parts[1]) or 0.3
    local g = tonumber(parts[2]) or 1.8
    local b = tonumber(parts[3]) or 2.1
    
    -- Clamp values to reasonable range (GMod allows > 1.0 for HDR-like effects)
    r = math.Clamp(r, 0, 10)
    g = math.Clamp(g, 0, 10)
    b = math.Clamp(b, 0, 10)
    
    return r, g, b
end

-- Apply the current weapon color to all physgun materials
function PhysgunColorSync.ApplyColor()
    if not PhysgunColorSync.EnabledConVar:GetBool() then return end
    if not RemixConfig or not RemixConfig.SetConfigVariable then 
        -- Remix not ready yet
        return 
    end
    
    local r, g, b = PhysgunColorSync.GetWeaponColor()
    
    -- Normalize if any component > 1 (convert HDR-style to 0-1 range while preserving hue)
    local maxVal = math.max(r, g, b, 1)
    local normR = r / maxVal
    local normG = g / maxVal
    local normB = b / maxVal
    
    -- Check if color changed
    local colorKey = string.format("%.3f,%.3f,%.3f", normR, normG, normB)
    if PhysgunColorSync.LastColor == colorKey and table.Count(PhysgunColorSync.MaterialHashes) > 0 then 
        -- Only skip if we already applied AND we have hashes
        return 
    end
    PhysgunColorSync.LastColor = colorKey
    
    -- Build the color string for Remix
    -- Format: HASH:R,G,B;HASH:R,G,B;...
    local colorEntries = {}
    local intensityEntries = {}
    local intensity = PhysgunColorSync.IntensityConVar:GetFloat()
    intensity = math.Clamp(intensity, 0.1, 50)
    
    for matName, hash in pairs(PhysgunColorSync.MaterialHashes) do
        -- Ensure hash is a string and remove 0x prefix if present
        local hashStr = tostring(hash)
        local hashClean = hashStr:gsub("^0x", ""):gsub("^0X", ""):upper()
        table.insert(colorEntries, string.format("%s:%.6f,%.6f,%.6f", hashClean, normR, normG, normB))
        table.insert(intensityEntries, string.format("%s:%.6f", hashClean, intensity))
    end
    
    if #colorEntries == 0 then
        -- No hashes yet, try to refresh
        PhysgunColorSync.RefreshHashes()
        return
    end
    
    local colorString = table.concat(colorEntries, ";")
    local intensityString = table.concat(intensityEntries, ",")
    
    -- Debug: print exactly what we're sending
    print("[PhysgunColorSync] Sending to Remix:")
    print("  rtx.legacyEmissiveColorsString = " .. colorString)
    print("  rtx.legacyEmissiveIntensitiesString = " .. intensityString)
    
    -- Apply to Remix
    local success1 = RemixConfig.SetConfigVariable("rtx.legacyEmissiveColorsString", colorString)
    local success2 = RemixConfig.SetConfigVariable("rtx.legacyEmissiveIntensitiesString", intensityString)
    
    if success1 and success2 then
        print(string.format("[PhysgunColorSync] Applied color (%.2f, %.2f, %.2f) intensity %.1f to %d materials", 
            normR, normG, normB, intensity, #colorEntries))
    else
        print(string.format("[PhysgunColorSync] FAILED: colors=%s, intensities=%s", tostring(success1), tostring(success2)))
    end
end

-- Watch for ConVar changes
cvars.AddChangeCallback("cl_weaponcolor", function(name, old, new)
    timer.Simple(0, PhysgunColorSync.ApplyColor)
end, "PhysgunColorSync")

cvars.AddChangeCallback("remix_physgun_color_sync", function(name, old, new)
    if new == "1" then
        PhysgunColorSync.ApplyColor()
    end
end, "PhysgunColorSync_Enable")

cvars.AddChangeCallback("remix_physgun_intensity", function(name, old, new)
    PhysgunColorSync.LastColor = nil -- Force update
    PhysgunColorSync.ApplyColor()
end, "PhysgunColorSync_Intensity")

-- Console commands
concommand.Add("remix_physgun_refresh", function()
    PhysgunColorSync.MaterialHashes = {} -- Clear all hashes to force re-scan
    PhysgunColorSync.RefreshHashes()
    PhysgunColorSync.LastColor = nil
    PhysgunColorSync.ApplyColor()
    
    -- Restart the checks
    PeriodicCheckCount = 0
    ThinkCheckCount = 0
    timer.Simple(0.5, PeriodicHashCheck)
    
    print("[PhysgunColorSync] Refreshed physgun material hashes and applied color")
end, nil, "Refresh physgun material hashes and reapply color")

concommand.Add("remix_physgun_debug", function()
    print("[PhysgunColorSync] Debug Info:")
    print("  Enabled: " .. tostring(PhysgunColorSync.EnabledConVar:GetBool()))
    print("  Intensity: " .. PhysgunColorSync.IntensityConVar:GetFloat())
    
    local r, g, b = PhysgunColorSync.GetWeaponColor()
    print(string.format("  Weapon Color (raw): %.3f, %.3f, %.3f", r, g, b))
    
    local maxVal = math.max(r, g, b, 1)
    print(string.format("  Weapon Color (normalized): %.3f, %.3f, %.3f", r/maxVal, g/maxVal, b/maxVal))
    
    print("  Tracked Materials:")
    for matName, hash in pairs(PhysgunColorSync.MaterialHashes) do
        print(string.format("    %s -> %s", matName, hash))
    end
    
    local count = table.Count(PhysgunColorSync.MaterialHashes)
    if count == 0 then
        print("    (none - try 'remix_physgun_refresh' after using the physgun)")
    end
end, nil, "Show physgun color sync debug info")

-- Persistent check to ensure hashes are acquired and color is applied
local PeriodicCheckCount = 0
local MaxPeriodicChecks = 30 -- Check for up to 60 seconds (30 * 2s intervals)
local function PeriodicHashCheck()
    PeriodicCheckCount = PeriodicCheckCount + 1
    
    local count = table.Count(PhysgunColorSync.MaterialHashes)
    local expectedCount = #PhysgunColorSync.PhysgunMaterials
    
    -- Always try to refresh hashes if we don't have them all
    if count < expectedCount then
        PhysgunColorSync.RefreshHashes()
    end
    
    -- Always try to apply color (it will skip if nothing changed)
    PhysgunColorSync.ApplyColor()
    
    -- Stop early if we found all materials AND successfully applied color
    if count >= expectedCount and PhysgunColorSync.LastColor ~= nil then
        print(string.format("[PhysgunColorSync] All %d materials found and color applied - stopping periodic checks", count))
        return
    end
    
    -- Keep checking periodically if we haven't found everything or haven't applied yet
    if PeriodicCheckCount < MaxPeriodicChecks then
        timer.Simple(2, PeriodicHashCheck)
    else
        print(string.format("[PhysgunColorSync] Reached max periodic checks (%d/%d materials found)", count, expectedCount))
    end
end

-- Hook when player switches to physgun - materials will definitely be loaded then
hook.Add("PlayerSwitchWeapon", "PhysgunColorSync_WeaponSwitch", function(ply, oldWep, newWep)
    if not IsValid(newWep) then return end
    if newWep:GetClass() == "weapon_physgun" then
        timer.Simple(0.5, function()
            PhysgunColorSync.RefreshHashes()
            PhysgunColorSync.ApplyColor()
        end)
    end
end)

-- Also try when player spawns (in case they spawn with physgun)
hook.Add("PlayerSpawn", "PhysgunColorSync_Spawn", function(ply)
    if ply == LocalPlayer() then
        timer.Simple(0.5, function()
            PhysgunColorSync.RefreshHashes()
            PhysgunColorSync.ApplyColor()
        end)
    end
end)

-- Hook for when game is fully loaded (more reliable than InitPostEntity)
local function SetupHUDPaintHook()
    hook.Add("HUDPaint", "PhysgunColorSync_FirstFrame", function()
        -- Run once on first HUDPaint, then remove self
        hook.Remove("HUDPaint", "PhysgunColorSync_FirstFrame")
        
        -- Clear cache to force re-application on map load
        PhysgunColorSync.LastColor = nil
        
        timer.Simple(1, function()
            PhysgunColorSync.Initialize()
            PeriodicCheckCount = 0
            timer.Simple(0.5, PeriodicHashCheck)
        end)
    end)
end

-- Set up the hook initially
SetupHUDPaintHook()

-- Think hook to continuously check for materials during early gameplay
local ThinkCheckCount = 0
local MaxThinkChecks = 300 -- Check for 10 seconds (300 frames at 30fps, 150 at 60fps)
local function SetupThinkHook()
    -- Remove old hook if exists
    hook.Remove("Think", "PhysgunColorSync_EarlyCheck")
    ThinkCheckCount = 0
    
    hook.Add("Think", "PhysgunColorSync_EarlyCheck", function()
        ThinkCheckCount = ThinkCheckCount + 1
        
        -- Check if we already have all materials
        local count = table.Count(PhysgunColorSync.MaterialHashes)
        local expectedCount = #PhysgunColorSync.PhysgunMaterials
        
        if count >= expectedCount then
            -- Found everything, stop checking
            hook.Remove("Think", "PhysgunColorSync_EarlyCheck")
            print("[PhysgunColorSync] All materials found - stopping Think hook checks")
            return
        end
        
        -- Only check every 30 frames to avoid spam
        if ThinkCheckCount % 30 == 0 then
            local foundNew = PhysgunColorSync.RefreshHashes()
            
            -- If we found new hashes, apply color immediately
            if foundNew then
                PhysgunColorSync.ApplyColor()
            end
        end
        
        -- Remove hook after max checks
        if ThinkCheckCount >= MaxThinkChecks then
            hook.Remove("Think", "PhysgunColorSync_EarlyCheck")
            print(string.format("[PhysgunColorSync] Think hook timeout (%d/%d materials found)", count, expectedCount))
        end
    end)
end

-- Set up the Think hook initially
SetupThinkHook()

-- Initialize when the player spawns
hook.Add("InitPostEntity", "PhysgunColorSync_Init", function()
    -- Reset state on map load - force fresh application
    PeriodicCheckCount = 0
    PhysgunColorSync.LastColor = nil  -- Clear cache to force re-application
    
    -- Re-add hooks for this map load
    SetupHUDPaintHook()
    SetupThinkHook()
    
    timer.Simple(0.5, function()
        PhysgunColorSync.Initialize()
        timer.Simple(0.5, PeriodicHashCheck)
    end)
end)

-- Also try to initialize if already in-game
if LocalPlayer and IsValid(LocalPlayer()) then
    PeriodicCheckCount = 0
    PhysgunColorSync.LastColor = nil  -- Clear cache to force re-application
    timer.Simple(0.5, function()
        PhysgunColorSync.Initialize()
        timer.Simple(0.5, PeriodicHashCheck)
    end)
end

print("[PhysgunColorSync] Loaded - use 'remix_physgun_debug' for info")

