-- RTX Remix ToPBR Material Converter
-- Automatically converts Source Engine material properties to PBR materials in RTX Remix.
--
-- This module uses C++ LegacyTextureProcessor module to:
-- - Read VTF texture files from Source Engine filesystem
-- - Extract pixel data and convert to Remix-compatible format
-- - Upload textures via RemixAPI CreateTexture
-- - Create PBR materials with proper roughness/metallic values
--
-- Console Commands:
-- - rtx_topbr_process - Process all tracked materials
-- - rtx_topbr_inspect <material> - Inspect a material's PBR properties
-- - rtx_topbr_stats - Show conversion statistics
-- - rtx_topbr_clear - Clear conversion cache
-- - rtx_topbr_debug <0/1> - Enable debug output
-- - rtx_topbr_help - Show help

if not (BRANCH == "x86-64" or BRANCH == "chromium") then return end

-- ConVars for configuration
CreateClientConVar("rtx_topbr_enabled", "1", true, false, "Enable automatic ToPBR conversion")
CreateClientConVar("rtx_topbr_auto", "1", true, false, "Auto-process materials on map load")
CreateClientConVar("rtx_topbr_debug", "0", true, false, "Enable debug output")
CreateClientConVar("rtx_topbr_delay", "5", true, false, "Delay before auto-processing (seconds)")
CreateClientConVar("rtx_topbr_metallic", "0", true, false, "Enable experimental metallic generation from base texture brightness (may cause black materials)")

-- Module table
RTXToPBR = RTXToPBR or {}

-- Initialization state
local isInitialized = false
local autoProcessTimer = nil

--[[
    Safe ConVar access helpers
]]--
local function GetConVarBoolSafe(name, default)
    local cv = GetConVar(name)
    if cv then return cv:GetBool() end
    return default or false
end

local function GetConVarFloatSafe(name, default)
    local cv = GetConVar(name)
    if cv then return cv:GetFloat() end
    return default or 0
end

--[[
    Debug print helper
]]--
local function DebugPrint(...)
    if GetConVarBoolSafe("rtx_topbr_debug", false) then
        MsgC(Color(200, 200, 255), "[RTX ToPBR] ", Color(255, 255, 255), ...)
        MsgC(Color(255, 255, 255), "\n")
    end
end

--[[
    Initialize the LegacyTextureProcessor C++ module
]]--
function RTXToPBR.Initialize()
    if isInitialized then
        return true
    end
    
    -- Check if LegacyTextureProcessor is available (from C++ module)
    -- Also check for backwards compatible VTFConverter alias
    local processor = LegacyTextureProcessor or VTFConverter
    if not processor then
        MsgC(Color(255, 100, 100), "[RTX ToPBR] LegacyTextureProcessor not available - C++ module not loaded\n")
        return false
    end
    
    -- Check if already initialized by C++ (during RemixAPI init)
    if processor.IsInitialized and processor.IsInitialized() then
        isInitialized = true
        MsgC(Color(100, 255, 100), "[RTX ToPBR] LegacyTextureProcessor already initialized by C++\n")
        -- Set debug output based on ConVar
        processor.SetDebugOutput(GetConVarBoolSafe("rtx_topbr_debug", false))
        return true
    end
    
    -- Initialize the C++ converter (fallback if not already done)
    local success = processor.Initialize()
    if not success then
        MsgC(Color(255, 100, 100), "[RTX ToPBR] Failed to initialize LegacyTextureProcessor\n")
        return false
    end
    
    -- Set debug output based on ConVar
    processor.SetDebugOutput(GetConVarBoolSafe("rtx_topbr_debug", false))
    
    isInitialized = true
    MsgC(Color(100, 255, 100), "[RTX ToPBR] Initialized successfully\n")
    return true
end

-- Helper to get the processor (LegacyTextureProcessor or VTFConverter)
local function GetProcessor()
    return LegacyTextureProcessor or VTFConverter
end

--[[
    Process all tracked materials for PBR conversion
]]--
function RTXToPBR.ProcessAllMaterials()
    if not GetConVarBoolSafe("rtx_topbr_enabled", true) then
        MsgC(Color(255, 200, 100), "[RTX ToPBR] Conversion disabled (rtx_topbr_enabled = 0)\n")
        return 0
    end
    
    if not RTXToPBR.Initialize() then
        return 0
    end
    
    MsgC(Color(100, 200, 255), "[RTX ToPBR] Processing tracked materials...\n")
    
    local processor = GetProcessor()
    local count = processor.ProcessAllMaterials()
    
    if count > 0 then
        MsgC(Color(100, 255, 100), string.format("[RTX ToPBR] Processed %d materials with PBR properties\n", count))
    else
        MsgC(Color(200, 200, 200), "[RTX ToPBR] No new materials to process\n")
    end
    
    return count
end

--[[
    Inspect a specific material's PBR properties
]]--
function RTXToPBR.InspectMaterial(materialName)
    if not materialName or materialName == "" then
        MsgC(Color(255, 200, 100), "Usage: RTXToPBR.InspectMaterial(materialName)\n")
        return nil
    end
    
    if not RTXToPBR.Initialize() then
        return nil
    end
    
    local processor = GetProcessor()
    local props = processor.InspectMaterial(materialName)
    
    MsgC(Color(100, 200, 255), string.format("\n[RTX ToPBR] Material: %s\n", materialName))
    MsgC(Color(100, 200, 255), string.rep("=", 60) .. "\n")
    
    if not props then
        MsgC(Color(255, 100, 100), "  Failed to load material\n")
        return nil
    end
    
    -- Base texture
    MsgC(Color(200, 200, 200), string.format("  Base Texture: %s\n", props.baseTexture or "(none)"))
    
    -- Bumpmap/Normal
    if props.hasBumpMap then
        local ssbumpStr = ""
        if props.isSSBump then
            ssbumpStr = " [SSBump→Normal]"
        end
        MsgC(Color(100, 255, 100), string.format("  ✓ Bumpmap: %s%s\n", props.bumpMap, ssbumpStr))
    else
        MsgC(Color(255, 200, 100), "  ✗ Bumpmap: (none)\n")
    end
    
    -- Phong/Roughness
    if props.hasPhong then
        MsgC(Color(100, 255, 100), string.format("  ✓ Phong Exponent: %.1f\n", props.phongExponent))
        MsgC(Color(150, 150, 150), string.format("    → Calculated Roughness: %.2f\n", props.roughness))
    else
        MsgC(Color(255, 200, 100), "  ✗ Phong Exponent: (none) → Default Roughness: 0.50\n")
    end
    
    if props.phongBoost and props.phongBoost > 0 then
        MsgC(Color(200, 200, 200), string.format("  Phong Boost: %.2f\n", props.phongBoost))
    end
    
    -- Envmap mask
    if props.hasEnvMapMask then
        MsgC(Color(100, 255, 100), string.format("  ✓ Envmap Mask: %s\n", props.envMapMask))
        MsgC(Color(150, 150, 150), string.format("    → Metallic hint: %.2f\n", props.metallic))
    else
        MsgC(Color(255, 200, 100), "  ✗ Envmap Mask: (none)\n")
    end
    
    -- Calculated values
    MsgC(Color(100, 200, 255), "  --- Calculated PBR Values ---\n")
    MsgC(Color(200, 200, 200), string.format("  Roughness: %.2f\n", props.roughness))
    MsgC(Color(200, 200, 200), string.format("  Metallic: %.2f\n", props.metallic))
    
    -- Flags
    if props.isSelfIllum then
        MsgC(Color(255, 200, 100), "  ! Self-illuminated (emissive)\n")
    end
    if props.isTranslucent then
        MsgC(Color(255, 200, 100), "  ! Translucent material\n")
    end
    if props.isGlass then
        MsgC(Color(100, 200, 255), "  ✓ GLASS MATERIAL (will use RTX Translucent shader with IOR 1.5)\n")
    end
    if props.shaderName and props.shaderName ~= "" then
        MsgC(Color(150, 150, 150), string.format("  Shader: %s\n", props.shaderName))
    end
    if props.surfaceProp and props.surfaceProp ~= "" then
        MsgC(Color(150, 150, 150), string.format("  Surface Prop: %s\n", props.surfaceProp))
    end
    
    MsgC(Color(100, 200, 255), string.rep("=", 60) .. "\n\n")
    
    return props
end

--[[
    Get conversion statistics
]]--
function RTXToPBR.GetStats()
    local processor = GetProcessor()
    if not processor then
        return {
            materialsProcessed = 0,
            texturesUploaded = 0,
            materialsWithNormals = 0,
            materialsWithRoughness = 0,
            failedConversions = 0
        }
    end
    
    return processor.GetStats()
end

--[[
    Clear conversion cache
]]--
function RTXToPBR.ClearCache()
    local processor = GetProcessor()
    if processor then
        processor.ClearCache()
    end
    MsgC(Color(100, 255, 100), "[RTX ToPBR] Cache cleared\n")
end

--[[
    Set debug output
]]--
function RTXToPBR.SetDebugOutput(enabled)
    local processor = GetProcessor()
    if processor then
        processor.SetDebugOutput(enabled)
    end
end

-- Console Commands
concommand.Add("rtx_topbr_process", function()
    RTXToPBR.ProcessAllMaterials()
end, nil, "Process all tracked materials for PBR conversion")

concommand.Add("rtx_topbr_inspect", function(ply, cmd, args)
    if not args[1] then
        MsgC(Color(255, 200, 100), "Usage: rtx_topbr_inspect <material_name>\n")
        MsgC(Color(255, 200, 100), "Example: rtx_topbr_inspect concrete/concretefloor001a\n")
        return
    end
    
    RTXToPBR.InspectMaterial(args[1])
end, nil, "Inspect a material's PBR properties")

concommand.Add("rtx_topbr_stats", function()
    local stats = RTXToPBR.GetStats()
    MsgC(Color(100, 200, 255), "[RTX ToPBR] Conversion Statistics:\n")
    MsgC(Color(200, 200, 200), string.format("  Materials processed: %d\n", stats.materialsProcessed or 0))
    MsgC(Color(200, 200, 200), string.format("  Textures uploaded: %d\n", stats.texturesUploaded or 0))
    MsgC(Color(200, 200, 200), string.format("  Materials with normals: %d\n", stats.materialsWithNormals or 0))
    MsgC(Color(200, 200, 200), string.format("  Materials with roughness: %d\n", stats.materialsWithRoughness or 0))
    MsgC(Color(200, 200, 200), string.format("  Failed conversions: %d\n", stats.failedConversions or 0))
end, nil, "Show PBR conversion statistics")

concommand.Add("rtx_topbr_clear", function()
    RTXToPBR.ClearCache()
end, nil, "Clear PBR conversion cache")

concommand.Add("rtx_topbr_debug", function(ply, cmd, args)
    local enabled = args[1] == "1" or args[1] == "true"
    RTXToPBR.SetDebugOutput(enabled)
    RunConsoleCommand("rtx_topbr_debug", enabled and "1" or "0")
    MsgC(Color(100, 255, 100), string.format("[RTX ToPBR] Debug output %s\n", enabled and "enabled" or "disabled"))
end, nil, "Enable/disable debug output")

concommand.Add("rtx_topbr_metallic", function(ply, cmd, args)
    local enabled = args[1] == "1" or args[1] == "true"
    local processor = GetProcessor()
    if processor and processor.SetMetallicGeneration then
        processor.SetMetallicGeneration(enabled)
        RunConsoleCommand("rtx_topbr_metallic", enabled and "1" or "0")
    else
        MsgC(Color(255, 100, 100), "[RTX ToPBR] Metallic generation not available (module not loaded)\n")
    end
end, nil, "Enable/disable experimental metallic generation (WARNING: may cause dark materials to appear black)")

concommand.Add("rtx_topbr_help", function()
    MsgC(Color(100, 200, 255), "\n[RTX ToPBR] Runtime PBR Material Converter\n")
    MsgC(Color(100, 200, 255), string.rep("=", 60) .. "\n")
    MsgC(Color(255, 255, 255), [[
This module automatically converts Source Engine materials to PBR
materials in RTX Remix at runtime.

It uses C++ code to:
- Read VTF textures from Source Engine filesystem
- Convert texture data to Remix-compatible format
- Upload textures via Remix API
- Create PBR materials with calculated roughness/metallic

Commands:
  rtx_topbr_process    - Process all tracked materials now
  rtx_topbr_inspect    - Inspect a specific material
  rtx_topbr_stats      - Show conversion statistics
  rtx_topbr_clear      - Clear conversion cache
  rtx_topbr_debug 1/0  - Enable/disable debug output
  rtx_topbr_metallic 1/0 - Enable/disable experimental metallic
                          generation (WARNING: may cause black materials)
  rtx_topbr_help       - Show this help

ConVars:
  rtx_topbr_enabled   - Enable/disable conversion (default: 1)
  rtx_topbr_auto      - Auto-process on map load (default: 1)
  rtx_topbr_delay     - Delay before auto-processing (default: 5)
  rtx_topbr_debug     - Debug output (default: 0)
  rtx_topbr_metallic  - Experimental metallic generation (default: 0)

Note: Dark envmap materials (chrome balls, etc.) use low roughness
for reflections by default. The experimental metallic mode attempts
to make them metallic but may cause them to appear black since
PBR metallic surfaces reflect their base color.

]])
    MsgC(Color(100, 200, 255), string.rep("=", 60) .. "\n\n")
end, nil, "Show ToPBR help information")

-- Auto-process on map load
hook.Add("InitPostEntity", "RTXToPBR_AutoProcess", function()
    if not GetConVarBoolSafe("rtx_topbr_enabled", true) then
        return
    end
    
    if not GetConVarBoolSafe("rtx_topbr_auto", true) then
        return
    end
    
    local delay = GetConVarFloatSafe("rtx_topbr_delay", 5)
    
    -- Clear any existing timer
    if autoProcessTimer then
        timer.Remove("RTXToPBR_AutoProcess")
    end
    
    -- Schedule auto-processing
    timer.Create("RTXToPBR_AutoProcess", delay, 1, function()
        MsgC(Color(100, 200, 255), "[RTX ToPBR] Running auto-process...\n")
        RTXToPBR.ProcessAllMaterials()
    end)
end)

-- Clear cache on map cleanup
hook.Add("PostCleanupMap", "RTXToPBR_MapCleanup", function()
    if timer.Exists("RTXToPBR_AutoProcess") then
        timer.Remove("RTXToPBR_AutoProcess")
    end
end)

-- Startup message
MsgC(Color(100, 255, 100), "[RTX ToPBR] Runtime PBR Converter loaded.\n")
if LegacyTextureProcessor or VTFConverter then
    MsgC(Color(200, 200, 200), "  C++ LegacyTextureProcessor module available - runtime conversion enabled.\n")
else
    MsgC(Color(255, 200, 100), "  C++ LegacyTextureProcessor module not loaded - waiting for binary module.\n")
end
MsgC(Color(200, 200, 200), "  Use 'rtx_topbr_help' for usage information.\n")

return RTXToPBR
