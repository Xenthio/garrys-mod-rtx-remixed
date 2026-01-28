--[[
================================================================================
    Solid Color Material Hash Collision Fixer for RTX Remix
    
    RTX Remix hashes textures to identify them. When multiple materials use
    solid-color textures (e.g., white, black, gray), they all produce the same
    hash, causing hash collisions. This breaks material replacement in RTX Remix.
    
    This module works with the C++ HashCollisionFixer to:
    1. Use C++ detection of solid-color textures (via VTF parsing)
    2. Create unique replacement textures with embedded material names
    3. Apply the unique textures to prevent hash collisions
    
    The fix embeds a small unique pattern into each texture based on the
    material name, ensuring each solid-color material gets a unique hash
    while appearing visually identical.
    
    Pipeline Stage: Lua Phase 1 (Pre-D3D9)
    Called by: RTXMaterialPipeline via RunLuaFixers()
================================================================================
]]

if not CLIENT then return end
if not (BRANCH == "x86-64" or BRANCH == "chromium") then return end

-- Global module table
RTXFixSolidColor = RTXFixSolidColor or {}

-- =============================================================================
-- CONVARS
-- =============================================================================

local enable_addon = CreateConVar("rtx_fixsolidcolor_enabled", "1", FCVAR_ARCHIVE,
    "Enable/disable the solid color hash collision fixer")
local debug_mode = CreateConVar("rtx_fixsolidcolor_debug", "0", FCVAR_ARCHIVE,
    "Enable debugging output for solid color fixer")

-- =============================================================================
-- INTERNAL STATE
-- =============================================================================

local State = {
    -- Track processed materials
    processedMaterials = {},  -- [matName] = true/false (success/fail)
    fixedCount = 0,
    
    -- Cache of created unique textures
    -- [colorKey] = { texture = ITexture, materials = { matName1, matName2, ... } }
    uniqueTextures = {},
    
    -- Material name to unique texture mapping
    materialToTexture = {},
}

-- =============================================================================
-- UTILITY FUNCTIONS
-- =============================================================================

local function DebugPrint(...)
    if debug_mode:GetBool() then
        MsgC(Color(255, 200, 100), "[RTX SolidColorFix] ", Color(255, 255, 255), ...)
        MsgC(Color(255, 255, 255), "\n")
    end
end

local function InfoPrint(...)
    MsgC(Color(255, 200, 100), "[RTX SolidColorFix] ", Color(255, 255, 255), ...)
    MsgC(Color(255, 255, 255), "\n")
end

-- Generate a simple hash from a string (for creating unique patterns)
local function SimpleHash(str)
    local hash = 5381
    for i = 1, #str do
        hash = ((hash * 33) + string.byte(str, i)) % 0xFFFFFFFF
    end
    return hash
end

-- Convert RGBA values to a color key string
local function ColorKey(r, g, b, a)
    return string.format("%02X%02X%02X%02X", r, g, b, a)
end

-- =============================================================================
-- TEXTURE CREATION
-- =============================================================================

-- Create a unique texture for a solid-color material
-- The texture is 4x4 pixels (minimum for proper hashing) with the solid color
-- plus a tiny invisible variation in one pixel based on material hash
local function CreateUniqueTexture(matName, r, g, b, a)
    -- Generate a unique name based on material
    local uniqueName = "rtx_solidcolor_" .. string.gsub(matName, "[^%w]", "_")
    
    -- Check if we already created this texture
    if State.materialToTexture[matName] then
        return State.materialToTexture[matName]
    end
    
    -- Generate hash from material name to create variation
    local hash = SimpleHash(matName)
    
    -- Create a 4x4 render target
    local texSize = 4
    local rtName = uniqueName .. "_rt"
    local rt = GetRenderTargetEx(rtName, texSize, texSize, 
        RT_SIZE_LITERAL, MATERIAL_RT_DEPTH_NONE, 0, 0, IMAGE_FORMAT_RGBA8888)
    
    if not rt then
        DebugPrint("Failed to create render target for: ", matName)
        return nil
    end
    
    -- Create temporary materials for rendering
    local fillMat = CreateMaterial(uniqueName .. "_fill", "UnlitGeneric", {
        ["$basetexture"] = "color/white",
        ["$vertexcolor"] = 1,
        ["$vertexalpha"] = 1,
        ["$ignorez"] = 1,
        ["$nolod"] = 1,
    })
    
    -- Render the solid color with a tiny variation
    render.PushRenderTarget(rt)
    cam.Start2D()
    
    -- Clear with the base solid color
    render.Clear(r, g, b, a)
    
    -- Add a tiny invisible variation to one pixel based on the hash
    -- This ensures each material gets a unique hash while looking identical
    -- We modify the LSB of the red channel for one pixel
    local varR = bit.band(r + bit.band(hash, 1), 255)
    local varG = bit.band(g + bit.band(bit.rshift(hash, 1), 1), 255)
    local varB = bit.band(b + bit.band(bit.rshift(hash, 2), 1), 255)
    
    -- Only apply variation if it won't be noticeable (within 1 unit)
    if math.abs(varR - r) <= 1 and math.abs(varG - g) <= 1 and math.abs(varB - b) <= 1 then
        -- Draw a single pixel with the variation at position (0,0)
        surface.SetDrawColor(varR, varG, varB, a)
        surface.DrawRect(0, 0, 1, 1)
    end
    
    -- Add additional variation using other hash bits in other corners
    -- This ensures more uniqueness without being visible
    local hash2 = bit.rshift(hash, 8)
    local var2R = bit.band(r + bit.band(hash2, 1), 255)
    local var2G = bit.band(g + bit.band(bit.rshift(hash2, 1), 1), 255)
    
    if math.abs(var2R - r) <= 1 and math.abs(var2G - g) <= 1 then
        surface.SetDrawColor(var2R, var2G, b, a)
        surface.DrawRect(texSize - 1, texSize - 1, 1, 1)
    end
    
    cam.End2D()
    render.PopRenderTarget()
    
    -- Store the reference
    State.materialToTexture[matName] = rt
    
    DebugPrint(string.format("Created unique texture for '%s' (color: %d,%d,%d,%d, hash: 0x%08X)", 
        matName, r, g, b, a, hash))
    
    return rt
end

-- =============================================================================
-- MAIN PROCESSING FUNCTION
-- =============================================================================

-- Process a single material through the solid color fixer
-- Called by RTXMaterialPipeline for each material
function RTXFixSolidColor.ProcessMaterial(matName)
    if not matName or matName == "" then 
        return false 
    end
    
    -- Check if enabled
    if not enable_addon:GetBool() then
        return false
    end
    
    -- Skip already processed materials
    if State.processedMaterials[matName] ~= nil then
        return State.processedMaterials[matName]
    end
    
    -- Check if C++ HashCollisionFixer detected this as a solid-color material
    if not HashCollisionFixer then
        -- C++ module not loaded yet
        State.processedMaterials[matName] = false
        return false
    end
    
    -- Get the material
    local mat = Material(matName)
    if not mat or mat:IsError() then
        State.processedMaterials[matName] = false
        return false
    end
    
    -- Get the $basetexture path
    local baseTexture = mat:GetString("$basetexture")
    if not baseTexture or baseTexture == "" then
        State.processedMaterials[matName] = false
        return false
    end
    
    -- Use C++ to check if this is a solid-color texture
    local isSolid, r, g, b, a = HashCollisionFixer.CheckSolidColor(baseTexture, debug_mode:GetBool())
    
    if not isSolid then
        State.processedMaterials[matName] = false
        return false
    end
    
    -- Default alpha if not returned
    r = r or 255
    g = g or 255
    b = b or 255
    a = a or 255
    
    DebugPrint(string.format("Detected solid-color material: '%s' (color: %d,%d,%d,%d)", 
        matName, r, g, b, a))
    
    -- Create a unique texture for this material
    local uniqueTex = CreateUniqueTexture(matName, r, g, b, a)
    
    if not uniqueTex then
        DebugPrint("Failed to create unique texture for: ", matName)
        State.processedMaterials[matName] = false
        return false
    end
    
    -- Apply the unique texture to the material
    mat:SetTexture("$basetexture", uniqueTex)
    
    -- Mark as fixed in C++ tracker
    HashCollisionFixer.MarkMaterialFixed(matName)
    
    State.processedMaterials[matName] = true
    State.fixedCount = State.fixedCount + 1
    
    if debug_mode:GetBool() then
        InfoPrint(string.format("Fixed solid-color hash collision: '%s'", matName))
    end
    
    return true
end

-- =============================================================================
-- BATCH PROCESSING
-- =============================================================================

-- Process all materials that the C++ side has flagged as needing fixes
function RTXFixSolidColor.ProcessPendingMaterials()
    if not HashCollisionFixer then
        return 0
    end
    
    local materials = HashCollisionFixer.GetMaterialsNeedingFix()
    if not materials or #materials == 0 then
        return 0
    end
    
    local fixed = 0
    for _, matName in ipairs(materials) do
        if RTXFixSolidColor.ProcessMaterial(matName) then
            fixed = fixed + 1
        end
    end
    
    if fixed > 0 and debug_mode:GetBool() then
        InfoPrint(string.format("Batch processed %d solid-color materials", fixed))
    end
    
    return fixed
end

-- =============================================================================
-- PUBLIC API
-- =============================================================================

-- Get statistics
function RTXFixSolidColor.GetStats()
    local stats = {
        fixed = State.fixedCount,
        cached = table.Count(State.processedMaterials),
        uniqueTextures = table.Count(State.materialToTexture),
    }
    
    -- Add C++ stats if available
    if HashCollisionFixer and HashCollisionFixer.GetStats then
        local cppStats = HashCollisionFixer.GetStats()
        stats.cppDetected = cppStats.totalDetected or 0
        stats.cppFixed = cppStats.totalFixed or 0
        stats.cppPending = cppStats.pending or 0
    end
    
    return stats
end

-- Clear cache (useful on map change)
function RTXFixSolidColor.ClearCache()
    State.processedMaterials = {}
    State.uniqueTextures = {}
    State.materialToTexture = {}
    State.fixedCount = 0
    
    -- Clear C++ cache if available
    if HashCollisionFixer and HashCollisionFixer.Reset then
        HashCollisionFixer.Reset()
    end
end

-- Check if enabled
function RTXFixSolidColor.IsEnabled()
    return enable_addon:GetBool()
end

-- Check if C++ module is available
function RTXFixSolidColor.IsAvailable()
    return HashCollisionFixer ~= nil and HashCollisionFixer.CheckSolidColor ~= nil
end

-- =============================================================================
-- CONSOLE COMMANDS
-- =============================================================================

concommand.Add("rtx_fixsolidcolor_stats", function()
    local stats = RTXFixSolidColor.GetStats()
    print("\n=== RTX Solid Color Fixer Statistics ===")
    print("C++ Module Available: " .. (RTXFixSolidColor.IsAvailable() and "YES" or "NO"))
    print("Lua Materials Fixed: " .. stats.fixed)
    print("Cached Entries: " .. stats.cached)
    print("Unique Textures Created: " .. stats.uniqueTextures)
    if stats.cppDetected then
        print("\n--- C++ HashCollisionFixer ---")
        print("Total Detected: " .. stats.cppDetected)
        print("Total Fixed: " .. stats.cppFixed)
        print("Pending: " .. stats.cppPending)
    end
    print("============================================\n")
end, nil, "Show solid color fixer statistics")

concommand.Add("rtx_fixsolidcolor_process", function()
    if not RTXFixSolidColor.IsAvailable() then
        print("[RTX SolidColorFix] C++ HashCollisionFixer not available")
        return
    end
    
    print("[RTX SolidColorFix] Processing pending solid-color materials...")
    local count = RTXFixSolidColor.ProcessPendingMaterials()
    print("[RTX SolidColorFix] Fixed " .. count .. " materials")
end, nil, "Process pending solid-color materials")

concommand.Add("rtx_fixsolidcolor_test", function(ply, cmd, args)
    local texPath = args[1]
    if not texPath then
        print("Usage: rtx_fixsolidcolor_test <texture_path>")
        print("Example: rtx_fixsolidcolor_test vgui/white")
        return
    end
    
    if not RTXFixSolidColor.IsAvailable() then
        print("[RTX SolidColorFix] C++ HashCollisionFixer not available")
        return
    end
    
    print("[RTX SolidColorFix] Testing texture: " .. texPath)
    local isSolid, r, g, b, a = HashCollisionFixer.CheckSolidColor(texPath, true)
    
    if isSolid then
        print(string.format("  Result: SOLID COLOR (RGBA: %d, %d, %d, %d)", r or 0, g or 0, b or 0, a or 255))
    else
        print("  Result: NOT a solid color (or error)")
    end
end, nil, "Test if a texture is a solid color")

concommand.Add("rtx_fixsolidcolor_clear", function()
    RTXFixSolidColor.ClearCache()
    print("[RTX SolidColorFix] Cache cleared")
end, nil, "Clear the solid color fixer cache")

-- =============================================================================
-- STARTUP
-- =============================================================================

print("[RTX SolidColorFix] Solid Color Hash Collision Fixer loaded")
print("  - Waits for C++ HashCollisionFixer module")
print("  - Use rtx_fixsolidcolor_stats for info")
