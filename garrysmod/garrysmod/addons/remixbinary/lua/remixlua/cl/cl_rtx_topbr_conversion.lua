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
CreateClientConVar("rtx_topbr_autodiscover", "1", true, false, "Auto-discover companion textures (_normal, _mask, _spec) not explicitly referenced in VMT")
CreateClientConVar("rtx_topbr_parse_commented_properties", "1", true, false, "Parse commented-out VMT properties (e.g., //$envmap) - useful for maps where they were disabled for vanilla Source")
CreateClientConVar("rtx_topbr_continuous", "1", true, false, "Continuous processing interval in seconds (0 = disabled, >0 = process every N seconds)")
CreateClientConVar("rtx_topbr_batch_size", "3", true, false, "Number of materials to process per batch (reduces stutter)")

-- Module table
RTXToPBR = RTXToPBR or {}

-- Initialization state
local isInitialized = false
local autoProcessTimer = nil
local continuousProcessTimer = nil

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
        -- Sync ConVars to C++
        processor.SetDebugOutput(GetConVarBoolSafe("rtx_topbr_debug", false))
        if processor.SetMetallicGeneration then
            processor.SetMetallicGeneration(GetConVarBoolSafe("rtx_topbr_metallic", false))
        end
        if processor.SetAutoDiscover then
            processor.SetAutoDiscover(GetConVarBoolSafe("rtx_topbr_autodiscover", true))
        end
        if processor.SetParseCommentedProperties then
            processor.SetParseCommentedProperties(GetConVarBoolSafe("rtx_topbr_parse_commented_properties", true))
        end
        return true
    end
    
    -- Initialize the C++ converter (fallback if not already done)
    local success = processor.Initialize()
    if not success then
        MsgC(Color(255, 100, 100), "[RTX ToPBR] Failed to initialize LegacyTextureProcessor\n")
        return false
    end
    
    -- Sync ConVars to C++
    processor.SetDebugOutput(GetConVarBoolSafe("rtx_topbr_debug", false))
    if processor.SetMetallicGeneration then
        processor.SetMetallicGeneration(GetConVarBoolSafe("rtx_topbr_metallic", false))
    end
    if processor.SetAutoDiscover then
        processor.SetAutoDiscover(GetConVarBoolSafe("rtx_topbr_autodiscover", true))
    end
    if processor.SetParseCommentedProperties then
        processor.SetParseCommentedProperties(GetConVarBoolSafe("rtx_topbr_parse_commented_properties", true))
    end
    
    isInitialized = true
    MsgC(Color(100, 255, 100), "[RTX ToPBR] Initialized successfully\n")
    return true
end

-- Helper to get the processor (LegacyTextureProcessor or VTFConverter)
local function GetProcessor()
    return LegacyTextureProcessor or VTFConverter
end

--[[
    Process all tracked materials for PBR conversion (BACKGROUND - non-blocking)
    Returns the number of NEW materials queued for processing
]]--
function RTXToPBR.ProcessAllMaterials(silent)
    if not GetConVarBoolSafe("rtx_topbr_enabled", true) then
        if not silent then
            MsgC(Color(255, 200, 100), "[RTX ToPBR] Conversion disabled (rtx_topbr_enabled = 0)\n")
        end
        return 0
    end
    
    if not RTXToPBR.Initialize() then
        return 0
    end
    
    local processor = GetProcessor()
    
    -- Use background processing if available (new API)
    if processor.QueueMaterialsForProcessing then
        local queuedCount = processor.QueueMaterialsForProcessing()
        
        if queuedCount > 0 then
            if not silent then
                MsgC(Color(100, 255, 100), string.format("[RTX ToPBR] Queued %d new materials for background processing\n", queuedCount))
            end
        else
            if not silent then
                MsgC(Color(200, 200, 200), "[RTX ToPBR] No new materials to process\n")
            end
        end
        
        return queuedCount
    end
    
    -- Fallback to old synchronous API
    if not silent then
        MsgC(Color(100, 200, 255), "[RTX ToPBR] Processing tracked materials...\n")
    end
    
    local count = processor.ProcessAllMaterials()
    
    if count > 0 then
        if not silent then
            MsgC(Color(100, 255, 100), string.format("[RTX ToPBR] Processed %d materials with PBR properties\n", count))
        end
        
        -- Write USDA if there are new changes
        if processor.WriteUSDAIfNeeded then
            processor.WriteUSDAIfNeeded()
            if not silent and GetConVarBoolSafe("rtx_topbr_debug", false) then
                MsgC(Color(150, 150, 150), "[RTX ToPBR] USDA updated with new materials\n")
            end
        end
    else
        if not silent then
            MsgC(Color(200, 200, 200), "[RTX ToPBR] No new materials to process\n")
        end
    end
    
    return count
end

--[[
    Check if background processing is active
]]--
function RTXToPBR.IsProcessingInBackground()
    local processor = GetProcessor()
    if processor and processor.IsProcessingInBackground then
        return processor.IsProcessingInBackground()
    end
    return false
end

--[[
    Get the number of materials still in the processing queue
]]--
function RTXToPBR.GetQueuedCount()
    local processor = GetProcessor()
    if processor and processor.GetQueuedMaterialCount then
        return processor.GetQueuedMaterialCount()
    end
    return 0
end

--[[
    Get the number of materials processed in the last/current background run
]]--
function RTXToPBR.GetLastProcessedCount()
    local processor = GetProcessor()
    if processor and processor.GetLastProcessedCount then
        return processor.GetLastProcessedCount()
    end
    return 0
end

--[[
    Get material from what the player is looking at (trace)
]]--
function RTXToPBR.GetMaterialFromTrace()
    local ply = LocalPlayer()
    if not IsValid(ply) then return nil, nil end
    
    local tr = ply:GetEyeTrace()
    if not tr.Hit then return nil, nil end
    
    -- Try to get material from entity first (models)
    if IsValid(tr.Entity) and tr.Entity:GetClass() ~= "worldspawn" then
        local ent = tr.Entity
        local materials = ent:GetMaterials()
        if materials and #materials > 0 then
            -- Return first material and entity info
            return materials[1], {
                type = "entity",
                entity = ent,
                class = ent:GetClass(),
                model = ent:GetModel(),
                allMaterials = materials
            }
        end
    end
    
    -- Fall back to brush/world material
    if tr.HitTexture and tr.HitTexture ~= "" and tr.HitTexture ~= "**empty**" then
        return tr.HitTexture, {
            type = "brush",
            hitPos = tr.HitPos,
            hitNormal = tr.HitNormal
        }
    end
    
    return nil, nil
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
    MsgC(Color(100, 200, 255), string.rep("=", 70) .. "\n")
    
    if not props then
        MsgC(Color(255, 100, 100), "  Failed to load material\n")
        return nil
    end
    
    -- =========================================================================
    -- Detected Format (most important info first!)
    -- =========================================================================
    local formatColor = Color(200, 200, 200)
    local formatStr = props.detectedFormat or "Source Engine (Standard)"
    
    if props.isExoPBR then
        formatColor = Color(100, 255, 200)
    elseif props.isGPBR then
        formatColor = Color(200, 100, 255)
    elseif props.isBFTPseudoPBR then
        formatColor = Color(255, 200, 100)
    end
    
    MsgC(formatColor, string.format("  ★ Detected Format: %s\n", formatStr))
    MsgC(Color(150, 150, 150), string.format("  Shader: %s\n", props.shaderName or "(unknown)"))
    
    -- =========================================================================
    -- Base Textures
    -- =========================================================================
    MsgC(Color(100, 200, 255), "\n  --- Textures ---\n")
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
    
    -- Phong exponent texture
    if props.hasPhongExponentTexture then
        MsgC(Color(100, 255, 100), string.format("  ✓ Phong Exponent Texture: %s\n", props.phongExponentTexture))
    end
    
    -- Envmap mask
    if props.hasEnvMapMask then
        MsgC(Color(100, 255, 100), string.format("  ✓ Envmap Mask: %s\n", props.envMapMask))
    end
    
    -- =========================================================================
    -- Format-specific info
    -- =========================================================================
    if props.isExoPBR then
        MsgC(Color(100, 255, 200), "\n  --- ExoPBR Format ---\n")
        if props.hasARMTexture then
            MsgC(Color(100, 255, 100), string.format("  ✓ ARM Texture: %s\n", props.armTexture))
        end
        if props.hasExoNormal then
            MsgC(Color(100, 255, 100), string.format("  ✓ ExoPBR Normal: %s\n", props.exoNormal))
        end
        if props.hasEmissionTexture then
            MsgC(Color(100, 255, 100), string.format("  ✓ Emission: %s\n", props.emissionTexture))
        end
    end
    
    if props.isGPBR then
        MsgC(Color(200, 100, 255), "\n  --- GPBR (Strata) Format ---\n")
        if props.hasMRAOTexture then
            MsgC(Color(100, 255, 100), string.format("  ✓ MRAO Texture: %s\n", props.mraoTexture))
        end
        if props.hasGPBREmission then
            MsgC(Color(100, 255, 100), string.format("  ✓ Emission: %s\n", props.gpbrEmission))
        end
    end
    
    if props.isBFTPseudoPBR then
        MsgC(Color(255, 200, 100), "\n  --- PseudoPBR (BlueFlyTrap/MWB) ---\n")
        if props.hasBFTExponentTexture then
            MsgC(Color(100, 255, 100), string.format("  ✓ Exponent Texture (roughness): %s\n", props.bftExponentTexture))
        end
        if props.isBFTMetallicLayer then
            MsgC(Color(255, 200, 100), "  ! Metallic Layer ($translucent + $phongalbedotint)\n")
        end
        if props.isBFTDiffuseLayer then
            MsgC(Color(255, 200, 100), "  ! Diffuse Layer (uses $blendTintByBaseAlpha)\n")
        end
        if props.hasBFTBlendTintByBaseAlpha then
            MsgC(Color(255, 150, 100), "  ! $blendTintByBaseAlpha detected (alpha = metallic mask)\n")
        end
        if props.hasBFTColor2 and props.bftColor2 then
            MsgC(Color(200, 200, 200), string.format("  $color2: [%.2f %.2f %.2f]\n", 
                props.bftColor2.r or 0, props.bftColor2.g or 0, props.bftColor2.b or 0))
        end
    end
    
    -- =========================================================================
    -- Auto-discovered textures
    -- =========================================================================
    local hasDiscovered = props.hasDiscoveredNormal or props.hasDiscoveredHeight or 
                          props.hasDiscoveredMask or props.hasDiscoveredAO
    if hasDiscovered then
        MsgC(Color(100, 200, 255), "\n  --- Auto-Discovered Textures ---\n")
        if props.hasDiscoveredNormal then
            MsgC(Color(100, 255, 100), string.format("  ✓ Normal: %s\n", props.discoveredNormal))
        end
        if props.hasDiscoveredHeight then
            MsgC(Color(100, 255, 100), string.format("  ✓ Height: %s\n", props.discoveredHeight))
        end
        if props.hasDiscoveredMask then
            MsgC(Color(100, 255, 100), string.format("  ✓ Mask/Spec: %s\n", props.discoveredMask))
        end
        if props.hasDiscoveredAO then
            MsgC(Color(100, 255, 100), string.format("  ✓ AO: %s\n", props.discoveredAO))
        end
    end
    
    -- =========================================================================
    -- Roughness Sources
    -- =========================================================================
    MsgC(Color(100, 200, 255), "\n  --- Roughness Sources ---\n")
    local roughnessSource = "Default (0.5)"
    
    if props.hasPhongExponentTexture then
        roughnessSource = "Phong Exponent Texture (best)"
    elseif props.normalMapAlphaEnvMapMask and props.hasBumpMap then
        roughnessSource = "Normal Map Alpha ($normalmapalphaenvmapmask)"
    elseif props.hasBaseAlphaEnvMapMask then
        roughnessSource = "Base Texture Alpha ($basealphaenvmapmask)"
    elseif props.hasBaseMapAlphaPhongMask then
        roughnessSource = "Base Texture Alpha ($basemapalphaphongmask)"
    elseif props.hasEnvMapMask then
        roughnessSource = "Envmap Mask Texture"
    elseif props.hasDiscoveredMask then
        roughnessSource = "Auto-discovered Mask/Spec"
    elseif props.hasPhong then
        roughnessSource = string.format("Phong Exponent (%.1f)", props.phongExponent or 0)
    end
    
    MsgC(Color(200, 200, 200), string.format("  Source: %s\n", roughnessSource))
    
    -- =========================================================================
    -- Calculated PBR Values
    -- =========================================================================
    MsgC(Color(100, 200, 255), "\n  --- Calculated PBR Values ---\n")
    MsgC(Color(200, 200, 200), string.format("  Roughness: %.2f\n", props.roughness or 0.5))
    MsgC(Color(200, 200, 200), string.format("  Metallic: %.2f\n", props.metallic or 0))
    
    if props.hasBaseTextureBrightness then
        MsgC(Color(150, 150, 150), string.format("  Base Texture Brightness: %.2f\n", props.baseTextureBrightness or 0))
    end
    
    -- =========================================================================
    -- Envmap Properties
    -- =========================================================================
    if props.hasEnvMap or props.hasEnvMapTint then
        MsgC(Color(100, 200, 255), "\n  --- Environment Map ---\n")
        if props.hasEnvMap then
            MsgC(Color(100, 255, 100), "  ✓ Has Envmap\n")
        end
        if props.hasEnvMapTint and props.envMapTint then
            MsgC(Color(200, 200, 200), string.format("  $envmaptint: [%.2f %.2f %.2f]\n", 
                props.envMapTint.r or 0, props.envMapTint.g or 0, props.envMapTint.b or 0))
        end
        if props.hasEnvMapContrast then
            MsgC(Color(200, 200, 200), string.format("  $envmapcontrast: %.2f\n", props.envMapContrast or 1))
        end
        if props.hasEnvMapSaturation then
            MsgC(Color(200, 200, 200), string.format("  $envmapsaturation: %.2f\n", props.envMapSaturation or 1))
        end
    end
    
    -- =========================================================================
    -- Phong Properties
    -- =========================================================================
    if props.hasPhong or props.hasPhongFresnelRanges or props.hasRimLight then
        MsgC(Color(100, 200, 255), "\n  --- Phong/Specular ---\n")
        if props.hasPhong then
            MsgC(Color(200, 200, 200), string.format("  Exponent: %.1f\n", props.phongExponent or 0))
        end
        if props.phongBoost and props.phongBoost > 0 then
            MsgC(Color(200, 200, 200), string.format("  Boost: %.2f\n", props.phongBoost))
        end
        if props.hasPhongFresnelRanges and props.phongFresnelRanges then
            MsgC(Color(200, 200, 200), string.format("  Fresnel: [%.2f %.2f %.2f]\n", 
                props.phongFresnelRanges[1] or 0, props.phongFresnelRanges[2] or 0, props.phongFresnelRanges[3] or 0))
        end
        if props.hasRimLight then
            MsgC(Color(200, 200, 200), string.format("  Rim Light: exp=%.1f boost=%.2f\n", 
                props.rimLightExponent or 0, props.rimLightBoost or 0))
        end
    end
    
    -- =========================================================================
    -- Material Flags
    -- =========================================================================
    local hasFlags = props.isSelfIllum or props.isTranslucent or props.isGlass
    if hasFlags then
        MsgC(Color(100, 200, 255), "\n  --- Flags ---\n")
        if props.isSelfIllum then
            MsgC(Color(255, 200, 100), "  ! Self-illuminated (emissive)\n")
            if props.hasSelfIllumMask then
                MsgC(Color(150, 150, 150), string.format("    Mask: %s\n", props.selfIllumMask))
            end
        end
        if props.isTranslucent then
            MsgC(Color(255, 200, 100), "  ! Translucent material\n")
        end
        if props.isGlass then
            MsgC(Color(100, 200, 255), "  ✓ GLASS MATERIAL (RTX Translucent shader, IOR 1.5)\n")
        end
    end
    
    -- =========================================================================
    -- Surface Properties
    -- =========================================================================
    if props.surfaceProp and props.surfaceProp ~= "" then
        MsgC(Color(100, 200, 255), "\n  --- Surface ---\n")
        MsgC(Color(150, 150, 150), string.format("  Surface Prop: %s\n", props.surfaceProp))
    end
    
    -- Parallax
    if props.hasParallaxMap then
        MsgC(Color(100, 200, 255), "\n  --- Parallax ---\n")
        MsgC(Color(100, 255, 100), string.format("  ✓ Parallax Map: %s\n", props.parallaxMap))
    end
    
    MsgC(Color(100, 200, 255), string.rep("=", 70) .. "\n\n")
    
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

--[[
    Start continuous processing
]]--
function RTXToPBR.StartContinuousProcessing(interval)
    interval = interval or GetConVarFloatSafe("rtx_topbr_continuous", 0)
    
    if interval <= 0 then
        MsgC(Color(255, 200, 100), "[RTX ToPBR] Invalid interval. Use a value > 0 (seconds)\n")
        return false
    end
    
    -- Stop any existing continuous timer
    RTXToPBR.StopContinuousProcessing()
    
    -- Create repeating timer
    timer.Create("RTXToPBR_Continuous", interval, 0, function()
        if not GetConVarBoolSafe("rtx_topbr_enabled", true) then
            return
        end
        
        -- Process silently (no console spam) - uses background thread
        local count = RTXToPBR.ProcessAllMaterials(true)
        
        -- Only log if debug is enabled and materials were queued
        if count > 0 and GetConVarBoolSafe("rtx_topbr_debug", false) then
            MsgC(Color(150, 150, 150), string.format("[RTX ToPBR] Continuous: queued %d new materials\n", count))
        end
    end)
    
    MsgC(Color(100, 255, 100), string.format("[RTX ToPBR] Continuous processing started (every %.1f seconds)\n", interval))
    return true
end

--[[
    Stop continuous processing
]]--
function RTXToPBR.StopContinuousProcessing()
    if timer.Exists("RTXToPBR_Continuous") then
        timer.Remove("RTXToPBR_Continuous")
        MsgC(Color(100, 200, 255), "[RTX ToPBR] Continuous processing stopped\n")
        return true
    end
    return false
end

--[[
    Check if continuous processing is active
]]--
function RTXToPBR.IsContinuousProcessing()
    return timer.Exists("RTXToPBR_Continuous")
end

-- Console Commands
concommand.Add("rtx_topbr_process", function()
    RTXToPBR.ProcessAllMaterials()
end, nil, "Process all tracked materials for PBR conversion")

concommand.Add("rtx_topbr_inspect", function(ply, cmd, args)
    if not args[1] or args[1] == "" then
        -- No material specified - try to get from what player is looking at
        local matName, traceInfo = RTXToPBR.GetMaterialFromTrace()
        
        if not matName then
            MsgC(Color(255, 200, 100), "Usage: rtx_topbr_inspect [material_name]\n")
            MsgC(Color(255, 200, 100), "  If no material specified, looks at what you're aiming at.\n")
            MsgC(Color(255, 200, 100), "Example: rtx_topbr_inspect concrete/concretefloor001a\n")
            return
        end
        
        -- Show trace info
        if traceInfo then
            if traceInfo.type == "entity" then
                MsgC(Color(100, 200, 255), string.format("[RTX ToPBR] Looking at entity: %s\n", traceInfo.class or "unknown"))
                if traceInfo.model then
                    MsgC(Color(150, 150, 150), string.format("  Model: %s\n", traceInfo.model))
                end
                if traceInfo.allMaterials and #traceInfo.allMaterials > 1 then
                    MsgC(Color(150, 150, 150), string.format("  Entity has %d materials. Showing first one.\n", #traceInfo.allMaterials))
                    MsgC(Color(150, 150, 150), "  All materials:\n")
                    for i, mat in ipairs(traceInfo.allMaterials) do
                        MsgC(Color(120, 120, 120), string.format("    [%d] %s\n", i, mat))
                    end
                end
            elseif traceInfo.type == "brush" then
                MsgC(Color(100, 200, 255), "[RTX ToPBR] Looking at brush/world surface\n")
            end
        end
        
        RTXToPBR.InspectMaterial(matName)
    else
        RTXToPBR.InspectMaterial(args[1])
    end
end, nil, "Inspect a material's PBR properties. If no material specified, inspects what you're looking at.")

concommand.Add("rtx_topbr_stats", function()
    local stats = RTXToPBR.GetStats()
    MsgC(Color(100, 200, 255), "[RTX ToPBR] Conversion Statistics:\n")
    MsgC(Color(200, 200, 200), string.format("  Materials processed: %d\n", stats.materialsProcessed or 0))
    MsgC(Color(200, 200, 200), string.format("  Textures uploaded: %d\n", stats.texturesUploaded or 0))
    MsgC(Color(200, 200, 200), string.format("  Materials with normals: %d\n", stats.materialsWithNormals or 0))
    MsgC(Color(200, 200, 200), string.format("  Materials with roughness: %d\n", stats.materialsWithRoughness or 0))
    MsgC(Color(200, 200, 200), string.format("  Failed conversions: %d\n", stats.failedConversions or 0))
    
    -- Background processing status
    local isProcessing = RTXToPBR.IsProcessingInBackground()
    local queuedCount = RTXToPBR.GetQueuedCount()
    local lastProcessed = RTXToPBR.GetLastProcessedCount()
    
    MsgC(Color(100, 200, 255), "\n  Background Processing:\n")
    if isProcessing then
        MsgC(Color(100, 255, 100), string.format("  ✓ ACTIVE - %d materials in queue, %d processed this run\n", queuedCount, lastProcessed))
    else
        MsgC(Color(200, 200, 200), string.format("  Idle - %d processed in last run\n", lastProcessed))
    end
    
    -- Continuous processing status
    if RTXToPBR.IsContinuousProcessing() then
        local interval = GetConVarFloatSafe("rtx_topbr_continuous", 0)
        MsgC(Color(100, 255, 100), string.format("  ✓ Continuous mode: every %.1fs\n", interval))
    end
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

concommand.Add("rtx_topbr_autodiscover", function(ply, cmd, args)
    local enabled = args[1] == "1" or args[1] == "true"
    local processor = GetProcessor()
    if processor and processor.SetAutoDiscover then
        processor.SetAutoDiscover(enabled)
        RunConsoleCommand("rtx_topbr_autodiscover", enabled and "1" or "0")
    else
        MsgC(Color(255, 100, 100), "[RTX ToPBR] Auto-discover not available (module not loaded)\n")
    end
end, nil, "Enable/disable auto-discovery of companion textures (_normal, _mask, _spec)")

concommand.Add("rtx_topbr_parse_commented_properties", function(ply, cmd, args)
    local enabled = args[1] == "1" or args[1] == "true"
    local processor = GetProcessor()
    if processor and processor.SetParseCommentedProperties then
        processor.SetParseCommentedProperties(enabled)
        RunConsoleCommand("rtx_topbr_parse_commented_properties", enabled and "1" or "0")
        MsgC(Color(100, 255, 100), string.format("[RTX ToPBR] Parse commented properties %s\n", enabled and "enabled" or "disabled"))
        if enabled then
            MsgC(Color(255, 200, 100), "  WARNING: Commented-out VMT properties (like //\"$envmap\") will now be parsed.\n")
            MsgC(Color(255, 200, 100), "  This may cause unexpected results if properties were commented out for a reason.\n")
            MsgC(Color(255, 200, 100), "  Clear cache and reprocess: rtx_topbr_clear && rtx_topbr_process\n")
        end
    else
        MsgC(Color(255, 100, 100), "[RTX ToPBR] Parse commented properties not available (module not loaded)\n")
    end
end, nil, "Enable/disable parsing of commented-out VMT properties (WARNING: may cause unexpected results)")

concommand.Add("rtx_topbr_continuous", function(ply, cmd, args)
    if not args[1] or args[1] == "" then
        -- Show current status
        if RTXToPBR.IsContinuousProcessing() then
            local interval = GetConVarFloatSafe("rtx_topbr_continuous", 0)
            MsgC(Color(100, 255, 100), string.format("[RTX ToPBR] Continuous processing is ACTIVE (%.1fs interval)\n", interval))
        else
            MsgC(Color(200, 200, 200), "[RTX ToPBR] Continuous processing is INACTIVE\n")
        end
        MsgC(Color(200, 200, 200), "Usage: rtx_topbr_continuous <interval>\n")
        MsgC(Color(200, 200, 200), "  interval = 0: stop continuous processing\n")
        MsgC(Color(200, 200, 200), "  interval > 0: start/restart with interval in seconds\n")
        return
    end
    
    local interval = tonumber(args[1])
    if not interval then
        MsgC(Color(255, 100, 100), "[RTX ToPBR] Invalid interval (must be a number)\n")
        return
    end
    
    RunConsoleCommand("rtx_topbr_continuous", tostring(interval))
    
    if interval <= 0 then
        RTXToPBR.StopContinuousProcessing()
    else
        RTXToPBR.StartContinuousProcessing(interval)
    end
end, nil, "Start/stop continuous material processing. Usage: rtx_topbr_continuous <seconds> (0 = stop)")

concommand.Add("rtx_topbr_help", function()
    MsgC(Color(100, 200, 255), "\n[RTX ToPBR] Runtime PBR Material Converter\n")
    MsgC(Color(100, 200, 255), string.rep("=", 70) .. "\n")
    MsgC(Color(255, 255, 255), [[
This module automatically converts Source Engine materials to PBR
materials in RTX Remix at runtime.

It uses C++ code to:
- Read VTF textures from Source Engine filesystem
- Convert texture data to Remix-compatible format
- Upload textures via Remix API
- Create PBR materials with calculated roughness/metallic

SUPPORTED PBR FORMATS:
- ExoPBR (screenspace_general_8tex shader with ARM textures)
- GPBR/Strata ("PBR" shader with MRAO textures)
- PseudoPBR (BlueFlyTrap/MWB phong-based PBR encoding)
- Standard Source Engine materials (phong, envmap, etc.)

Commands:
  rtx_topbr_inspect [material] - Inspect material's PBR properties
                                 (no argument = look at what you're aiming at)
  rtx_topbr_process           - Process all tracked materials now
  rtx_topbr_continuous <sec>  - Start/stop continuous processing
                                (0 = stop, >0 = process every N seconds)
  rtx_topbr_stats             - Show conversion statistics
  rtx_topbr_clear             - Clear conversion cache
  rtx_topbr_debug 1/0         - Enable/disable debug output
  rtx_topbr_metallic 1/0      - Enable/disable experimental metallic
                                (WARNING: may cause black materials)
  rtx_topbr_autodiscover 1/0  - Enable/disable auto-discovery of 
                                companion textures (_normal, _mask, _spec)
  rtx_topbr_parse_commented_properties 1/0
                              - Enable/disable parsing of commented-out
                                VMT properties (e.g., //"$envmap")
                                Useful for maps where envmap was disabled
                                for vanilla Source but benefits RTX
  rtx_topbr_help              - Show this help

ConVars:
  rtx_topbr_enabled    - Enable/disable conversion (default: 1)
  rtx_topbr_auto       - Auto-process on map load (default: 1)
  rtx_topbr_delay      - Delay before auto-processing (default: 5)
  rtx_topbr_continuous - Continuous processing interval in seconds
                         (default: 0 = disabled)
  rtx_topbr_batch_size - Materials per batch, prevents stutter (default: 3)
  rtx_topbr_debug      - Debug output (default: 0)
  rtx_topbr_metallic   - Experimental metallic generation (default: 0)
  rtx_topbr_autodiscover - Auto-discover companion textures (default: 1)

Continuous Processing:
  Set rtx_topbr_continuous to automatically process new materials as they
  appear. Materials are processed in small batches (rtx_topbr_batch_size) to
  avoid stuttering. The USDA file is only written when there are new changes.
  Useful for multiplayer servers or maps with lots of dynamic content.
  
  Example: rtx_topbr_continuous 5   (process every 5 seconds)
           rtx_topbr_batch_size 3   (3 materials per batch, reduces stutter)

Roughness Priority (phong materials are handled first):
1. $phongexponenttexture (best quality)
2. $basemapalphaphongmask / $normalmapalphaenvmapmask
3. $envmapmask texture
4. Auto-discovered _mask/_spec textures
5. Base texture alpha with envmap

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
        
        -- Start continuous processing if configured
        local continuousInterval = GetConVarFloatSafe("rtx_topbr_continuous", 0)
        if continuousInterval > 0 then
            RTXToPBR.StartContinuousProcessing(continuousInterval)
        end
    end)
end)

-- Clear cache on map cleanup
hook.Add("PostCleanupMap", "RTXToPBR_MapCleanup", function()
    if timer.Exists("RTXToPBR_AutoProcess") then
        timer.Remove("RTXToPBR_AutoProcess")
    end
    -- Stop continuous processing on map cleanup
    RTXToPBR.StopContinuousProcessing()
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
