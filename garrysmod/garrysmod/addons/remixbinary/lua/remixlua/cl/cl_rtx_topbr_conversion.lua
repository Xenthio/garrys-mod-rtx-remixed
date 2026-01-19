-- RTX Remix ToPBR Material Property Extractor
-- Extracts Source Engine material properties (normals, phong, envmap masks) and exports them
-- for use in creating PBR replacement materials in RTX Remix.
--
-- NOTE: This module EXTRACTS and REPORTS material properties. Automatic runtime PBR 
-- conversion is not currently possible without:
-- 1. Converting VTF textures to DDS format at runtime
-- 2. Uploading texture data via RemixAPI CreateTexture
-- These features would require significant C++ implementation.
--
-- What this module DOES do:
-- - Extracts $bumpmap, $phongexponent, $envmapmask from Source materials
-- - Converts phong exponent to suggested roughness values
-- - Exports material data to JSON files for use with Remix Toolkit
-- - Provides console commands for inspecting materials

if not (BRANCH == "x86-64" or BRANCH == "chromium") then return end

-- ConVars for configuration
CreateClientConVar("rtx_topbr_enabled", "1", true, false, "Enable ToPBR property extraction")
CreateClientConVar("rtx_topbr_debug", "0", true, false, "Enable debug output")
CreateClientConVar("rtx_topbr_export_path", "rtx_pbr_export", true, false, "Subdirectory in data/ for exported material info")

-- Module table
RTXToPBR = RTXToPBR or {}

-- Cache of extracted materials
local extractedMaterials = {}

-- Statistics
local stats = {
    scanned = 0,
    withNormals = 0,
    withPhong = 0,
    withEnvmapMask = 0,
    exported = 0
}

--[[
    Safe ConVar access helpers
]]--
local function GetConVarBoolSafe(name, default)
    local cv = GetConVar(name)
    if cv then return cv:GetBool() end
    return default or false
end

local function GetConVarStringSafe(name, default)
    local cv = GetConVar(name)
    if cv then return cv:GetString() end
    return default or ""
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
    Convert phong exponent to roughness
    Source Engine phong exponent typically ranges from 1-150+
    Lower exponent = more diffuse/rough, Higher exponent = more specular/smooth
    
    Formula: roughness = 1.0 - (log(exponent) / log(maxExponent))
    Clamped to 0.05 - 0.95 range to avoid extreme values
]]--
function RTXToPBR.PhongExponentToRoughness(exponent)
    if not exponent or exponent <= 0 then
        return 0.5 -- Default roughness
    end
    
    -- Clamp exponent to reasonable range
    exponent = math.Clamp(exponent, 1, 256)
    
    -- Logarithmic conversion - higher exponent = lower roughness (more shiny)
    local maxExponent = 150 -- Typical max in Source
    local roughness = 1.0 - (math.log(exponent) / math.log(maxExponent))
    
    -- Clamp to usable PBR range
    return math.Clamp(roughness, 0.05, 0.95)
end

--[[
    Convert phong boost/fresnel to metallic hint
    This is a rough approximation - not physically accurate
]]--
function RTXToPBR.PhongBoostToMetallicHint(boost, fresnelRanges)
    if not boost then boost = 1.0 end
    
    local metallic = 0.0
    
    if boost > 2.0 then
        metallic = math.Clamp((boost - 2.0) / 8.0, 0, 0.5)
    end
    
    if fresnelRanges and fresnelRanges.z then
        if fresnelRanges.z > 0.8 then
            metallic = math.max(metallic, 0.3)
        end
    end
    
    return metallic
end

--[[
    Sanitize material name to prevent path traversal
]]--
local function SanitizeMaterialName(materialName)
    if not materialName or type(materialName) ~= "string" then
        return nil
    end
    
    local sanitized = materialName
    sanitized = string.gsub(sanitized, "%.%.", "")
    sanitized = string.gsub(sanitized, "//+", "/")
    sanitized = string.gsub(sanitized, "\\", "/")
    sanitized = string.gsub(sanitized, "^/+", "")
    
    if string.find(sanitized, "^%.") then
        return nil
    end
    
    return sanitized
end

--[[
    Extract material properties from a Source Engine material
    Returns table with extracted PBR-relevant data
]]--
function RTXToPBR.ExtractMaterialProperties(materialName)
    local safeName = SanitizeMaterialName(materialName)
    if not safeName then
        return nil
    end
    
    local mat = Material(safeName)
    if not mat or mat:IsError() then
        return nil
    end
    
    local props = {
        materialName = safeName,
        baseTexture = nil,
        bumpmap = nil,
        phongExponent = nil,
        phongBoost = nil,
        phongFresnelRanges = nil,
        envmapMask = nil,
        selfillum = false,
        translucent = false,
        -- Calculated PBR values
        suggestedRoughness = 0.5,
        suggestedMetallic = 0.0,
    }
    
    -- Get base texture
    local baseTex = mat:GetTexture("$basetexture")
    if baseTex and not baseTex:IsError() then
        props.baseTexture = baseTex:GetName()
    end
    
    -- Get bumpmap (normal map)
    local bumpTex = mat:GetTexture("$bumpmap")
    if bumpTex and not bumpTex:IsError() then
        props.bumpmap = bumpTex:GetName()
    end
    if not props.bumpmap then
        local bumpStr = mat:GetString("$bumpmap")
        if bumpStr and bumpStr ~= "" then
            props.bumpmap = bumpStr
        end
    end
    
    -- Get envmap mask
    local envmapMaskTex = mat:GetTexture("$envmapmask")
    if envmapMaskTex and not envmapMaskTex:IsError() then
        props.envmapMask = envmapMaskTex:GetName()
    end
    if not props.envmapMask then
        local envmapMaskStr = mat:GetString("$envmapmask")
        if envmapMaskStr and envmapMaskStr ~= "" then
            props.envmapMask = envmapMaskStr
        end
    end
    
    -- Get phong exponent
    props.phongExponent = mat:GetFloat("$phongexponent")
    if not props.phongExponent or props.phongExponent <= 0 then
        local exponentInt = mat:GetInt("$phongexponent")
        if exponentInt and exponentInt > 0 then
            props.phongExponent = exponentInt
        end
    end
    
    -- Get phong boost
    props.phongBoost = mat:GetFloat("$phongboost")
    
    -- Get phong fresnel ranges
    local fresnelRanges = mat:GetVector("$phongfresnelranges")
    if fresnelRanges then
        props.phongFresnelRanges = {x = fresnelRanges.x, y = fresnelRanges.y, z = fresnelRanges.z}
    end
    
    -- Check flags
    local selfillum = mat:GetInt("$selfillum")
    props.selfillum = (selfillum and selfillum == 1)
    
    local translucent = mat:GetInt("$translucent")
    props.translucent = (translucent and translucent == 1)
    
    -- VMT file fallback
    local vmtPath = "materials/" .. safeName
    if not string.EndsWith(vmtPath, ".vmt") then
        vmtPath = vmtPath .. ".vmt"
    end
    
    local content = file.Read(vmtPath, "GAME")
    if content then
        local lowerContent = content:lower()
        
        if not props.bumpmap then
            local bumpMatch = string.match(lowerContent, '%$bumpmap%s+["\']?([^"\'\n\r]+)["\']?')
            if bumpMatch then
                props.bumpmap = string.Trim(bumpMatch)
            end
        end
        
        if not props.phongExponent then
            local exponentMatch = string.match(lowerContent, '%$phongexponent%s+["\']?([%d%.]+)["\']?')
            if exponentMatch then
                props.phongExponent = tonumber(exponentMatch)
            end
        end
        
        if not props.envmapMask then
            local maskMatch = string.match(lowerContent, '%$envmapmask%s+["\']?([^"\'\n\r]+)["\']?')
            if maskMatch then
                props.envmapMask = string.Trim(maskMatch)
            end
        end
    end
    
    -- Calculate suggested PBR values
    if props.phongExponent then
        props.suggestedRoughness = RTXToPBR.PhongExponentToRoughness(props.phongExponent)
    end
    
    props.suggestedMetallic = RTXToPBR.PhongBoostToMetallicHint(props.phongBoost, props.phongFresnelRanges)
    
    return props
end

--[[
    Build texture path info for Remix
]]--
function RTXToPBR.GetRemixTexturePaths(props)
    local paths = {}
    
    if props.baseTexture then
        paths.albedo = "materials/" .. props.baseTexture .. ".dds"
    end
    
    if props.bumpmap then
        paths.normal = "materials/" .. props.bumpmap .. ".dds"
    end
    
    if props.envmapMask then
        paths.roughnessMask = "materials/" .. props.envmapMask .. ".dds"
    end
    
    return paths
end

--[[
    Inspect a material and print its properties
]]--
function RTXToPBR.InspectMaterial(materialName)
    local props = RTXToPBR.ExtractMaterialProperties(materialName)
    
    MsgC(Color(100, 200, 255), string.format("\n[RTX ToPBR] Material: %s\n", materialName))
    MsgC(Color(100, 200, 255), string.rep("=", 60) .. "\n")
    
    if not props then
        MsgC(Color(255, 100, 100), "  Failed to load material\n")
        return nil
    end
    
    -- Base texture
    MsgC(Color(200, 200, 200), string.format("  Base Texture: %s\n", props.baseTexture or "(none)"))
    
    -- Bumpmap/Normal
    if props.bumpmap then
        MsgC(Color(100, 255, 100), string.format("  ✓ Bumpmap: %s\n", props.bumpmap))
        MsgC(Color(150, 150, 150), string.format("    → Convert to: materials/%s.dds (normal map)\n", props.bumpmap))
    else
        MsgC(Color(255, 200, 100), "  ✗ Bumpmap: (none)\n")
    end
    
    -- Phong/Roughness
    if props.phongExponent then
        MsgC(Color(100, 255, 100), string.format("  ✓ Phong Exponent: %.1f\n", props.phongExponent))
        MsgC(Color(150, 150, 150), string.format("    → Suggested Roughness: %.2f\n", props.suggestedRoughness))
    else
        MsgC(Color(255, 200, 100), "  ✗ Phong Exponent: (none) → Default Roughness: 0.50\n")
    end
    
    if props.phongBoost then
        MsgC(Color(200, 200, 200), string.format("  Phong Boost: %.2f\n", props.phongBoost))
    end
    
    -- Envmap mask
    if props.envmapMask then
        MsgC(Color(100, 255, 100), string.format("  ✓ Envmap Mask: %s\n", props.envmapMask))
        MsgC(Color(150, 150, 150), string.format("    → Can be used as roughness texture (inverted)\n"))
        MsgC(Color(150, 150, 150), string.format("    → Metallic hint: %.2f\n", props.suggestedMetallic))
    else
        MsgC(Color(255, 200, 100), "  ✗ Envmap Mask: (none)\n")
    end
    
    -- Flags
    if props.selfillum then
        MsgC(Color(255, 200, 100), "  ! Self-illuminated (emissive)\n")
    end
    if props.translucent then
        MsgC(Color(255, 200, 100), "  ! Translucent\n")
    end
    
    -- Texture hash if available
    if RemixMaterial and RemixMaterial.GetTextureHash then
        local hash, hashStr = RemixMaterial.GetTextureHash(materialName)
        if hash and hash ~= 0 then
            MsgC(Color(100, 255, 100), string.format("  Texture Hash: %s\n", hashStr))
        else
            MsgC(Color(255, 100, 100), "  Texture Hash: (not tracked - render the material first)\n")
        end
    end
    
    MsgC(Color(100, 200, 255), string.rep("=", 60) .. "\n\n")
    
    return props
end

--[[
    Scan all materials in the current map
]]--
function RTXToPBR.ScanMapMaterials()
    extractedMaterials = {}
    stats = { scanned = 0, withNormals = 0, withPhong = 0, withEnvmapMask = 0, exported = 0 }
    
    if not NikNaks or not NikNaks.CurrentMap then
        MsgC(Color(255, 100, 100), "[RTX ToPBR] NikNaks not available\n")
        return {}
    end
    
    local bsp = NikNaks.CurrentMap
    local textures = {}
    
    local ok, bspTextures = pcall(function() return bsp:GetTextures() end)
    if ok and bspTextures then
        textures = bspTextures
    end
    
    MsgC(Color(100, 200, 255), string.format("[RTX ToPBR] Scanning %d BSP textures...\n", #textures))
    
    for _, texName in ipairs(textures) do
        if texName then
            local materialName = texName
            materialName = string.gsub(materialName, "^materials/", "")
            materialName = string.gsub(materialName, "%.vmt$", "")
            
            local props = RTXToPBR.ExtractMaterialProperties(materialName)
            if props then
                stats.scanned = stats.scanned + 1
                
                if props.bumpmap then
                    stats.withNormals = stats.withNormals + 1
                end
                if props.phongExponent then
                    stats.withPhong = stats.withPhong + 1
                end
                if props.envmapMask then
                    stats.withEnvmapMask = stats.withEnvmapMask + 1
                end
                
                -- Only store materials that have PBR-relevant data
                if props.bumpmap or props.phongExponent or props.envmapMask then
                    extractedMaterials[materialName] = props
                end
            end
        end
    end
    
    MsgC(Color(100, 255, 100), string.format("[RTX ToPBR] Scan complete!\n"))
    MsgC(Color(200, 200, 200), string.format("  Total materials: %d\n", stats.scanned))
    MsgC(Color(200, 200, 200), string.format("  With normal maps: %d\n", stats.withNormals))
    MsgC(Color(200, 200, 200), string.format("  With phong data: %d\n", stats.withPhong))
    MsgC(Color(200, 200, 200), string.format("  With envmap masks: %d\n", stats.withEnvmapMask))
    MsgC(Color(200, 200, 200), string.format("  PBR-relevant materials: %d\n", table.Count(extractedMaterials)))
    
    return extractedMaterials
end

--[[
    Export extracted materials to a JSON file
]]--
function RTXToPBR.ExportToJSON(filename)
    if table.Count(extractedMaterials) == 0 then
        MsgC(Color(255, 200, 100), "[RTX ToPBR] No materials to export. Run 'rtx_topbr_scan' first.\n")
        return false
    end
    
    local exportPath = GetConVarStringSafe("rtx_topbr_export_path", "rtx_pbr_export")
    
    -- Create directory if needed
    if not file.IsDir(exportPath, "DATA") then
        file.CreateDir(exportPath)
    end
    
    -- Build export data
    local exportData = {
        exportedAt = os.date("%Y-%m-%d %H:%M:%S"),
        mapName = game.GetMap(),
        stats = stats,
        materials = {}
    }
    
    for matName, props in pairs(extractedMaterials) do
        local matData = {
            name = matName,
            baseTexture = props.baseTexture,
            normalMap = props.bumpmap,
            phongExponent = props.phongExponent,
            phongBoost = props.phongBoost,
            envmapMask = props.envmapMask,
            suggestedRoughness = props.suggestedRoughness,
            suggestedMetallic = props.suggestedMetallic,
            isEmissive = props.selfillum,
            isTranslucent = props.translucent,
            remixPaths = RTXToPBR.GetRemixTexturePaths(props)
        }
        
        -- Get texture hash if available
        if RemixMaterial and RemixMaterial.GetTextureHash then
            local hash, hashStr = RemixMaterial.GetTextureHash(matName)
            if hash and hash ~= 0 then
                matData.textureHash = hashStr
            end
        end
        
        table.insert(exportData.materials, matData)
    end
    
    -- Write JSON
    local jsonStr = util.TableToJSON(exportData, true)
    local fullPath = exportPath .. "/" .. (filename or (game.GetMap() .. "_pbr.json"))
    
    file.Write(fullPath, jsonStr)
    
    stats.exported = table.Count(extractedMaterials)
    
    MsgC(Color(100, 255, 100), string.format("[RTX ToPBR] Exported %d materials to data/%s\n", 
        stats.exported, fullPath))
    
    return true
end

--[[
    Export a simple list of textures that need conversion
]]--
function RTXToPBR.ExportTextureList(filename)
    if table.Count(extractedMaterials) == 0 then
        MsgC(Color(255, 200, 100), "[RTX ToPBR] No materials to export. Run 'rtx_topbr_scan' first.\n")
        return false
    end
    
    local exportPath = GetConVarStringSafe("rtx_topbr_export_path", "rtx_pbr_export")
    
    if not file.IsDir(exportPath, "DATA") then
        file.CreateDir(exportPath)
    end
    
    local lines = {
        "# RTX Remix PBR Texture Conversion List",
        "# Generated: " .. os.date("%Y-%m-%d %H:%M:%S"),
        "# Map: " .. game.GetMap(),
        "#",
        "# Format: sourceTexture → remixPath | suggestedRoughness",
        "#",
        ""
    }
    
    -- Sort by material name
    local sortedMats = {}
    for matName, props in pairs(extractedMaterials) do
        table.insert(sortedMats, {name = matName, props = props})
    end
    table.sort(sortedMats, function(a, b) return a.name < b.name end)
    
    table.insert(lines, "=== NORMAL MAPS (convert VTF → DDS) ===")
    for _, mat in ipairs(sortedMats) do
        if mat.props.bumpmap then
            table.insert(lines, string.format("materials/%s.vtf → materials/%s.dds", 
                mat.props.bumpmap, mat.props.bumpmap))
        end
    end
    
    table.insert(lines, "")
    table.insert(lines, "=== ROUGHNESS VALUES (from phong exponent) ===")
    for _, mat in ipairs(sortedMats) do
        if mat.props.phongExponent then
            table.insert(lines, string.format("%s: phong=%.0f → roughness=%.2f", 
                mat.name, mat.props.phongExponent, mat.props.suggestedRoughness))
        end
    end
    
    table.insert(lines, "")
    table.insert(lines, "=== ENVMAP MASKS (can be used for roughness texture) ===")
    for _, mat in ipairs(sortedMats) do
        if mat.props.envmapMask then
            table.insert(lines, string.format("materials/%s.vtf → materials/%s.dds (invert for roughness)", 
                mat.props.envmapMask, mat.props.envmapMask))
        end
    end
    
    local fullPath = exportPath .. "/" .. (filename or (game.GetMap() .. "_textures.txt"))
    file.Write(fullPath, table.concat(lines, "\n"))
    
    MsgC(Color(100, 255, 100), string.format("[RTX ToPBR] Exported texture list to data/%s\n", fullPath))
    
    return true
end

--[[
    Get statistics
]]--
function RTXToPBR.GetStats()
    return table.Copy(stats)
end

--[[
    Clear cache
]]--
function RTXToPBR.ClearCache()
    extractedMaterials = {}
    stats = { scanned = 0, withNormals = 0, withPhong = 0, withEnvmapMask = 0, exported = 0 }
    MsgC(Color(100, 255, 100), "[RTX ToPBR] Cache cleared\n")
end

-- Console Commands
concommand.Add("rtx_topbr_scan", function()
    RTXToPBR.ScanMapMaterials()
end, nil, "Scan current map for PBR-convertible materials")

concommand.Add("rtx_topbr_inspect", function(ply, cmd, args)
    if not args[1] then
        MsgC(Color(255, 200, 100), "Usage: rtx_topbr_inspect <material_name>\n")
        MsgC(Color(255, 200, 100), "Example: rtx_topbr_inspect concrete/concretefloor001a\n")
        return
    end
    
    local safeName = SanitizeMaterialName(args[1])
    if not safeName then
        MsgC(Color(255, 100, 100), "[RTX ToPBR] Invalid material name\n")
        return
    end
    
    RTXToPBR.InspectMaterial(safeName)
end, nil, "Inspect a material's PBR properties")

concommand.Add("rtx_topbr_export", function(ply, cmd, args)
    RTXToPBR.ExportToJSON(args[1])
end, nil, "Export scanned materials to JSON (data/rtx_pbr_export/)")

concommand.Add("rtx_topbr_export_list", function(ply, cmd, args)
    RTXToPBR.ExportTextureList(args[1])
end, nil, "Export texture conversion list (data/rtx_pbr_export/)")

concommand.Add("rtx_topbr_stats", function()
    local s = RTXToPBR.GetStats()
    MsgC(Color(100, 200, 255), "[RTX ToPBR] Statistics:\n")
    MsgC(Color(200, 200, 200), string.format("  Scanned: %d\n", s.scanned))
    MsgC(Color(200, 200, 200), string.format("  With normals: %d\n", s.withNormals))
    MsgC(Color(200, 200, 200), string.format("  With phong: %d\n", s.withPhong))
    MsgC(Color(200, 200, 200), string.format("  With envmap masks: %d\n", s.withEnvmapMask))
    MsgC(Color(200, 200, 200), string.format("  Exported: %d\n", s.exported))
end, nil, "Show scan/export statistics")

concommand.Add("rtx_topbr_clear", function()
    RTXToPBR.ClearCache()
end, nil, "Clear material cache")

concommand.Add("rtx_topbr_help", function()
    MsgC(Color(100, 200, 255), "\n[RTX ToPBR] Help\n")
    MsgC(Color(100, 200, 255), string.rep("=", 60) .. "\n")
    MsgC(Color(255, 255, 255), [[
This module extracts Source Engine material properties and exports
them for use in creating PBR replacement materials in RTX Remix.

Commands:
  rtx_topbr_scan        - Scan current map for materials
  rtx_topbr_inspect     - Inspect a specific material
  rtx_topbr_export      - Export to JSON file
  rtx_topbr_export_list - Export texture conversion list
  rtx_topbr_stats       - Show statistics
  rtx_topbr_clear       - Clear cache

Workflow:
  1. Load a map
  2. Run 'rtx_topbr_scan' to extract material properties
  3. Run 'rtx_topbr_export' to save to JSON
  4. Use the exported data in Remix Toolkit to create replacements

Note: This module EXTRACTS data only. To apply PBR materials at runtime,
you need to convert VTF textures to DDS and place them in the Remix mods
folder with proper material definitions.

]])
    MsgC(Color(100, 200, 255), string.rep("=", 60) .. "\n\n")
end, nil, "Show help information")

-- Startup message
MsgC(Color(100, 255, 100), "[RTX ToPBR] Material Property Extractor loaded.\n")
MsgC(Color(200, 200, 200), "  Use 'rtx_topbr_help' for usage information.\n")
MsgC(Color(200, 200, 200), "  Use 'rtx_topbr_scan' to scan map materials.\n")

return RTXToPBR
