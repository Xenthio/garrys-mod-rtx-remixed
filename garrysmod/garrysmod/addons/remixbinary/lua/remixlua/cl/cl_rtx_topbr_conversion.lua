-- RTX Remix ToPBR Conversion
-- Automatically forwards Source Engine material properties (normals, phong, envmap masks) to RTX Remix
-- for primitive PBR approximation.

if not (BRANCH == "x86-64" or BRANCH == "chromium") then return end

if not RemixMaterial then
    return -- Silently skip if API not available
end

-- ConVars for configuration
CreateClientConVar("rtx_topbr_enabled", "1", true, false, "Enable automatic ToPBR conversion (1 = enabled, 0 = disabled)")
CreateClientConVar("rtx_topbr_normals", "1", true, false, "Forward $bumpmap as normal textures (1 = enabled, 0 = disabled)")
CreateClientConVar("rtx_topbr_roughness", "1", true, false, "Convert $phongexponent to roughness (1 = enabled, 0 = disabled)")
CreateClientConVar("rtx_topbr_metallic", "0", true, false, "Attempt metallic approximation from $envmapmask (1 = enabled, 0 = disabled)")
CreateClientConVar("rtx_topbr_default_roughness", "0.5", true, false, "Default roughness for materials without phong (0.0-1.0)")
CreateClientConVar("rtx_topbr_default_metallic", "0.0", true, false, "Default metallic value (0.0-1.0)")
CreateClientConVar("rtx_topbr_debug", "0", true, false, "Enable debug output for ToPBR conversion")
CreateClientConVar("rtx_topbr_delay", "5", true, false, "Delay in seconds before auto-conversion runs after map load")

-- Module table
RTXToPBR = RTXToPBR or {}

-- Cache of processed materials to avoid reprocessing
local processedMaterials = {}

-- Statistics
local stats = {
    total = 0,
    normals = 0,
    roughness = 0,
    metallic = 0,
    failed = 0
}

--[[
    Safe ConVar access helpers with nil guards
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
    Convert phong exponent to roughness
    Source Engine phong exponent typically ranges from 1-150+
    Lower exponent = more diffuse/rough, Higher exponent = more specular/smooth
    
    Formula: roughness = 1.0 - (log(exponent) / log(maxExponent))
    Clamped to 0.05 - 0.95 range to avoid extreme values
]]--
function RTXToPBR.PhongExponentToRoughness(exponent)
    if not exponent or exponent <= 0 then
        return GetConVarFloatSafe("rtx_topbr_default_roughness", 0.5)
    end
    
    -- Clamp exponent to reasonable range
    exponent = math.Clamp(exponent, 1, 256)
    
    -- Logarithmic conversion - higher exponent = lower roughness (more shiny)
    -- Using natural log for smoother curve
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
    
    -- High phong boost with fresnel suggests metallic-like surface
    local metallic = 0.0
    
    if boost > 2.0 then
        metallic = math.Clamp((boost - 2.0) / 8.0, 0, 0.5)
    end
    
    -- Fresnel ranges can indicate metallic behavior
    -- $phongfresnelranges "[x y z]" where z (high angle) being high suggests metallic
    if fresnelRanges and fresnelRanges.z then
        if fresnelRanges.z > 0.8 then
            metallic = math.max(metallic, 0.3)
        end
    end
    
    return metallic
end

--[[
    Sanitize material name to prevent path traversal attacks
]]--
local function SanitizeMaterialName(materialName)
    if not materialName or type(materialName) ~= "string" then
        return nil
    end
    
    -- Remove any path traversal attempts
    local sanitized = materialName
    sanitized = string.gsub(sanitized, "%.%.", "")  -- Remove ..
    sanitized = string.gsub(sanitized, "//+", "/")  -- Collapse multiple slashes
    sanitized = string.gsub(sanitized, "\\", "/")   -- Normalize slashes
    sanitized = string.gsub(sanitized, "^/+", "")   -- Remove leading slashes
    
    -- Ensure it doesn't escape materials folder
    if string.find(sanitized, "^/") or string.find(sanitized, "^%.") then
        return nil
    end
    
    return sanitized
end

--[[
    Extract material properties from a Source Engine material
    Returns table with extracted PBR-relevant data
]]--
function RTXToPBR.ExtractMaterialProperties(materialName)
    -- Sanitize material name to prevent path traversal
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
        bumpmap = nil,           -- Normal map texture path
        phongExponent = nil,     -- For roughness calculation
        phongBoost = nil,        -- For metallic hint
        phongFresnelRanges = nil,-- For metallic hint
        envmapMask = nil,        -- For roughness/metallic mask
        baseTexture = nil,       -- Albedo texture path
        selfillum = false,       -- Emissive flag
        translucent = false,     -- Transparency flag
    }
    
    -- Method 1: Try GetTexture for bumpmap
    local bumpTex = mat:GetTexture("$bumpmap")
    if bumpTex and not bumpTex:IsError() then
        props.bumpmap = bumpTex:GetName()
    end
    
    -- Method 2: Try GetString for bumpmap (sometimes stored as string path)
    if not props.bumpmap then
        local bumpStr = mat:GetString("$bumpmap")
        if bumpStr and bumpStr ~= "" then
            props.bumpmap = bumpStr
        end
    end
    
    -- Get base texture for reference
    local baseTex = mat:GetTexture("$basetexture")
    if baseTex and not baseTex:IsError() then
        props.baseTexture = baseTex:GetName()
    end
    
    -- Get envmap mask texture
    local envmapMaskTex = mat:GetTexture("$envmapmask")
    if envmapMaskTex and not envmapMaskTex:IsError() then
        props.envmapMask = envmapMaskTex:GetName()
    end
    
    -- Method 2: Try GetString for envmapmask
    if not props.envmapMask then
        local envmapMaskStr = mat:GetString("$envmapmask")
        if envmapMaskStr and envmapMaskStr ~= "" then
            props.envmapMask = envmapMaskStr
        end
    end
    
    -- Get phong exponent (controls specular sharpness)
    props.phongExponent = mat:GetFloat("$phongexponent")
    if not props.phongExponent or props.phongExponent <= 0 then
        -- Try GetInt as fallback
        local exponentInt = mat:GetInt("$phongexponent")
        if exponentInt and exponentInt > 0 then
            props.phongExponent = exponentInt
        end
    end
    
    -- Get phong boost (controls specular intensity)
    props.phongBoost = mat:GetFloat("$phongboost")
    
    -- Get phong fresnel ranges
    local fresnelRanges = mat:GetVector("$phongfresnelranges")
    if fresnelRanges then
        props.phongFresnelRanges = fresnelRanges
    end
    
    -- Check for selfillum (already handled by category manager, but good to know)
    local selfillum = mat:GetInt("$selfillum")
    props.selfillum = (selfillum and selfillum == 1)
    
    -- Check for translucency
    local translucent = mat:GetInt("$translucent")
    props.translucent = (translucent and translucent == 1)
    
    -- VMT file fallback for missing parameters (safeName is already sanitized)
    local vmtPath = "materials/" .. safeName
    if not string.EndsWith(vmtPath, ".vmt") then
        vmtPath = vmtPath .. ".vmt"
    end
    
    local content = file.Read(vmtPath, "GAME")
    if content then
        local lowerContent = content:lower()
        
        -- Parse $bumpmap if not found via API
        if not props.bumpmap then
            local bumpMatch = string.match(lowerContent, '%$bumpmap%s+["\']?([^"\'\n\r]+)["\']?')
            if bumpMatch then
                props.bumpmap = string.Trim(bumpMatch)
            end
        end
        
        -- Parse $phongexponent if not found via API
        if not props.phongExponent then
            local exponentMatch = string.match(lowerContent, '%$phongexponent%s+["\']?([%d%.]+)["\']?')
            if exponentMatch then
                props.phongExponent = tonumber(exponentMatch)
            end
        end
        
        -- Parse $envmapmask if not found via API
        if not props.envmapMask then
            local maskMatch = string.match(lowerContent, '%$envmapmask%s+["\']?([^"\'\n\r]+)["\']?')
            if maskMatch then
                props.envmapMask = string.Trim(maskMatch)
            end
        end
    end
    
    return props
end

--[[
    Build the texture path for Remix
    Remix expects .dds files in the rtx-remix/mods folder structure
    We return the Source Engine texture path - Remix will need to resolve this
]]--
function RTXToPBR.BuildRemixTexturePath(sourceTexturePath)
    if not sourceTexturePath or sourceTexturePath == "" then
        return nil
    end
    
    -- Clean up the path
    local path = sourceTexturePath
    path = string.gsub(path, "^materials/", "")
    path = string.gsub(path, "\\", "/")
    
    -- Remix will look for .dds files
    -- The path structure should be: .../rtx-remix/mods/gameTextures/materials/...
    -- We just return the materials-relative path; Remix resolves the rest
    return "materials/" .. path .. ".dds"
end

--[[
    Apply PBR conversion to a material
    Creates or updates a Remix material with converted PBR properties
]]--
function RTXToPBR.ConvertMaterial(materialName)
    if not GetConVarBoolSafe("rtx_topbr_enabled", true) then
        return false
    end
    
    -- Skip if already processed
    local lowerName = materialName:lower()
    if processedMaterials[lowerName] then
        return false
    end
    
    -- Extract properties
    local props = RTXToPBR.ExtractMaterialProperties(materialName)
    if not props then
        DebugPrint("Failed to extract properties from: ", materialName)
        stats.failed = stats.failed + 1
        processedMaterials[lowerName] = { error = true }
        return false
    end
    
    -- Check if there's anything to convert
    local hasNormal = GetConVarBoolSafe("rtx_topbr_normals", true) and props.bumpmap
    local hasPhong = GetConVarBoolSafe("rtx_topbr_roughness", true) and props.phongExponent
    local hasEnvmapMask = GetConVarBoolSafe("rtx_topbr_metallic", false) and props.envmapMask
    
    if not hasNormal and not hasPhong and not hasEnvmapMask then
        -- Nothing to convert, skip silently
        processedMaterials[lowerName] = { skipped = true }
        return false
    end
    
    -- Get the texture hash for this material
    local hash, hashStr = RemixMaterial.GetTextureHash(materialName)
    if not hash or hash == 0 then
        -- Material hasn't been rendered yet, queue for later
        DebugPrint("Material not yet rendered (no hash): ", materialName)
        return false
    end
    
    -- Build material info for Remix
    local materialInfo = {
        hash = hash
    }
    
    local opaqueInfo = {}
    local convertedSomething = false
    
    -- Forward normal map
    if hasNormal then
        local normalPath = RTXToPBR.BuildRemixTexturePath(props.bumpmap)
        if normalPath then
            materialInfo.normalTexture = normalPath
            stats.normals = stats.normals + 1
            convertedSomething = true
            DebugPrint(string.format("Normal map for '%s': %s", materialName, normalPath))
        end
    end
    
    -- Convert phong to roughness
    if hasPhong then
        local roughness = RTXToPBR.PhongExponentToRoughness(props.phongExponent)
        opaqueInfo.roughnessConstant = roughness
        stats.roughness = stats.roughness + 1
        convertedSomething = true
        DebugPrint(string.format("Roughness for '%s': %.2f (from phong exp %.0f)", 
            materialName, roughness, props.phongExponent))
    else
        -- Apply default roughness
        opaqueInfo.roughnessConstant = GetConVarFloatSafe("rtx_topbr_default_roughness", 0.5)
    end
    
    -- Metallic approximation from envmap mask or phong boost
    if hasEnvmapMask then
        -- If we have an envmap mask, we could use it as a roughness or metallic texture
        -- For now, we just set a metallic hint based on presence of envmap
        local metallicHint = RTXToPBR.PhongBoostToMetallicHint(props.phongBoost, props.phongFresnelRanges)
        if metallicHint > 0.1 then
            opaqueInfo.metallicConstant = metallicHint
            stats.metallic = stats.metallic + 1
            convertedSomething = true
            DebugPrint(string.format("Metallic hint for '%s': %.2f", materialName, metallicHint))
        end
        
        -- Note: We could also forward the envmapmask texture as a roughness texture (inverted)
        -- but that requires actual texture conversion which is beyond simple forwarding
        -- local roughnessTexPath = RTXToPBR.BuildRemixTexturePath(props.envmapMask)
        -- opaqueInfo.roughnessTexture = roughnessTexPath
    else
        -- Apply default metallic
        opaqueInfo.metallicConstant = GetConVarFloatSafe("rtx_topbr_default_metallic", 0.0)
    end
    
    if not convertedSomething then
        processedMaterials[lowerName] = { skipped = true }
        return false
    end
    
    -- Create the opaque PBR material in Remix
    local success = false
    if RemixMaterial.CreateOpaqueMaterial then
        local matId = RemixMaterial.CreateOpaqueMaterial(
            "topbr_" .. materialName,
            materialInfo,
            opaqueInfo
        )
        success = (matId and matId ~= 0)
    end
    
    if success then
        processedMaterials[lowerName] = {
            hash = hash,
            hasNormal = hasNormal,
            hasRoughness = hasPhong,
            hasMetallic = hasEnvmapMask
        }
        stats.total = stats.total + 1
        
        if GetConVarBoolSafe("rtx_topbr_debug", false) then
            MsgC(Color(100, 255, 100), string.format("[RTX ToPBR] Converted: %s (hash: %s)\n", 
                materialName, hashStr or "unknown"))
        end
        
        return true
    else
        DebugPrint("Failed to create Remix material for: ", materialName)
        stats.failed = stats.failed + 1
        processedMaterials[lowerName] = { error = true }
        return false
    end
end

--[[
    Scan all tracked materials and attempt conversion
]]--
function RTXToPBR.ConvertAllTrackedMaterials()
    if not GetConVarBoolSafe("rtx_topbr_enabled", true) then
        MsgC(Color(255, 200, 100), "[RTX ToPBR] Conversion disabled (rtx_topbr_enabled = 0)\n")
        return
    end
    
    if not RemixMaterial or not RemixMaterial.GetCachedMaterials then
        MsgC(Color(255, 100, 100), "[RTX ToPBR] Remix API not available\n")
        return
    end
    
    MsgC(Color(100, 200, 255), "[RTX ToPBR] Scanning tracked materials for PBR conversion...\n")
    
    local materials = RemixMaterial.GetCachedMaterials()
    if not materials or #materials == 0 then
        MsgC(Color(255, 200, 100), "[RTX ToPBR] No tracked materials found\n")
        return
    end
    
    local converted = 0
    local skipped = 0
    local failed = 0
    
    for _, matName in ipairs(materials) do
        local result = RTXToPBR.ConvertMaterial(matName)
        if result then
            converted = converted + 1
        else
            local cached = processedMaterials[matName:lower()]
            if cached then
                if cached.error then
                    failed = failed + 1
                else
                    skipped = skipped + 1
                end
            else
                skipped = skipped + 1
            end
        end
    end
    
    MsgC(Color(100, 255, 100), string.format(
        "[RTX ToPBR] Conversion complete: %d converted, %d skipped, %d failed\n",
        converted, skipped, failed))
    MsgC(Color(200, 200, 200), string.format(
        "[RTX ToPBR] Totals: %d normals, %d roughness, %d metallic hints\n",
        stats.normals, stats.roughness, stats.metallic))
end

--[[
    Scan BSP materials from NikNaks
]]--
function RTXToPBR.ConvertBSPMaterials()
    if not GetConVarBoolSafe("rtx_topbr_enabled", true) then
        return
    end
    
    if not NikNaks or not NikNaks.CurrentMap then
        DebugPrint("NikNaks not available")
        return
    end
    
    local bsp = NikNaks.CurrentMap
    local textures = {}
    
    -- Get textures from BSP
    local ok, bspTextures = pcall(function() return bsp:GetTextures() end)
    if ok and bspTextures then
        textures = bspTextures
    end
    
    MsgC(Color(100, 200, 255), string.format("[RTX ToPBR] Processing %d BSP textures...\n", #textures))
    
    for _, texName in ipairs(textures) do
        if texName then
            -- Normalize material name
            local materialName = texName
            materialName = string.gsub(materialName, "^materials/", "")
            materialName = string.gsub(materialName, "%.vmt$", "")
            
            -- Track the material first to get its hash
            RemixMaterial.TrackMaterial(materialName)
        end
    end
    
    -- Wait a moment for materials to be tracked, then convert
    timer.Simple(1, function()
        RTXToPBR.ConvertAllTrackedMaterials()
    end)
end

--[[
    Get conversion statistics
]]--
function RTXToPBR.GetStats()
    return {
        total = stats.total,
        normals = stats.normals,
        roughness = stats.roughness,
        metallic = stats.metallic,
        failed = stats.failed,
        processed = table.Count(processedMaterials)
    }
end

--[[
    Clear conversion cache (allows re-processing)
]]--
function RTXToPBR.ClearCache()
    processedMaterials = {}
    stats = {
        total = 0,
        normals = 0,
        roughness = 0,
        metallic = 0,
        failed = 0
    }
    MsgC(Color(100, 255, 100), "[RTX ToPBR] Cache cleared\n")
end

--[[
    Debug: Inspect a material's properties
]]--
function RTXToPBR.InspectMaterial(materialName)
    local props = RTXToPBR.ExtractMaterialProperties(materialName)
    
    MsgC(Color(100, 200, 255), string.format("[RTX ToPBR] Inspecting: %s\n", materialName))
    
    if not props then
        MsgC(Color(255, 100, 100), "  - Failed to load material\n")
        return
    end
    
    MsgC(Color(200, 200, 200), string.format("  Base Texture: %s\n", props.baseTexture or "(none)"))
    
    if props.bumpmap then
        MsgC(Color(100, 255, 100), string.format("  ✓ Bumpmap: %s\n", props.bumpmap))
        local remixPath = RTXToPBR.BuildRemixTexturePath(props.bumpmap)
        MsgC(Color(200, 200, 200), string.format("    -> Remix path: %s\n", remixPath or "(failed)"))
    else
        MsgC(Color(255, 200, 100), "  ✗ Bumpmap: (none)\n")
    end
    
    if props.phongExponent then
        local roughness = RTXToPBR.PhongExponentToRoughness(props.phongExponent)
        MsgC(Color(100, 255, 100), string.format("  ✓ Phong Exponent: %.1f -> Roughness: %.2f\n", 
            props.phongExponent, roughness))
    else
        MsgC(Color(255, 200, 100), string.format("  ✗ Phong Exponent: (none) -> Default Roughness: %.2f\n",
            GetConVarFloatSafe("rtx_topbr_default_roughness", 0.5)))
    end
    
    if props.phongBoost then
        MsgC(Color(200, 200, 200), string.format("  Phong Boost: %.2f\n", props.phongBoost))
    end
    
    if props.envmapMask then
        MsgC(Color(100, 255, 100), string.format("  ✓ Envmap Mask: %s\n", props.envmapMask))
        local metallicHint = RTXToPBR.PhongBoostToMetallicHint(props.phongBoost, props.phongFresnelRanges)
        MsgC(Color(200, 200, 200), string.format("    -> Metallic hint: %.2f\n", metallicHint))
    else
        MsgC(Color(255, 200, 100), "  ✗ Envmap Mask: (none)\n")
    end
    
    if props.selfillum then
        MsgC(Color(255, 200, 100), "  ! Self-illuminated (handled by category manager)\n")
    end
    
    if props.translucent then
        MsgC(Color(255, 200, 100), "  ! Translucent material\n")
    end
    
    -- Get texture hash
    local hash, hashStr = RemixMaterial.GetTextureHash(materialName)
    if hash and hash ~= 0 then
        MsgC(Color(100, 255, 100), string.format("  Texture Hash: %s\n", hashStr))
    else
        MsgC(Color(255, 100, 100), "  Texture Hash: (not tracked - material not rendered)\n")
    end
end

-- Console Commands
concommand.Add("rtx_topbr_convert", function()
    RTXToPBR.ConvertAllTrackedMaterials()
end, nil, "Run ToPBR conversion on all tracked materials")

concommand.Add("rtx_topbr_convert_bsp", function()
    RTXToPBR.ConvertBSPMaterials()
end, nil, "Run ToPBR conversion on BSP materials (uses NikNaks)")

concommand.Add("rtx_topbr_inspect", function(ply, cmd, args)
    if not args[1] then
        MsgC(Color(255, 200, 100), "Usage: rtx_topbr_inspect <material_name>\n")
        MsgC(Color(255, 200, 100), "Example: rtx_topbr_inspect models/props_c17/oildrum001\n")
        return
    end
    
    -- Sanitize input material name
    local safeName = SanitizeMaterialName(args[1])
    if not safeName then
        MsgC(Color(255, 100, 100), "[RTX ToPBR] Invalid material name\n")
        return
    end
    
    RTXToPBR.InspectMaterial(safeName)
end, nil, "Inspect a material's PBR-convertible properties")

concommand.Add("rtx_topbr_stats", function()
    local s = RTXToPBR.GetStats()
    MsgC(Color(100, 200, 255), "[RTX ToPBR] Statistics:\n")
    MsgC(Color(200, 200, 200), string.format("  Total converted: %d\n", s.total))
    MsgC(Color(200, 200, 200), string.format("  Normal maps forwarded: %d\n", s.normals))
    MsgC(Color(200, 200, 200), string.format("  Roughness values set: %d\n", s.roughness))
    MsgC(Color(200, 200, 200), string.format("  Metallic hints set: %d\n", s.metallic))
    MsgC(Color(200, 200, 200), string.format("  Failed conversions: %d\n", s.failed))
    MsgC(Color(200, 200, 200), string.format("  Processed (cached): %d\n", s.processed))
end, nil, "Show ToPBR conversion statistics")

concommand.Add("rtx_topbr_clear", function()
    RTXToPBR.ClearCache()
end, nil, "Clear ToPBR conversion cache (allows re-processing)")

-- Auto-run on map load (if enabled)
hook.Add("InitPostEntity", "RTXToPBR_AutoConvert", function()
    -- Clear cache from previous map
    RTXToPBR.ClearCache()
    
    if not GetConVarBoolSafe("rtx_topbr_enabled", true) then
        return
    end
    
    local delay = GetConVarFloatSafe("rtx_topbr_delay", 5)
    
    timer.Simple(delay, function()
        if not GetConVarBoolSafe("rtx_topbr_enabled", true) then
            return
        end
        
        MsgC(Color(100, 200, 255), "[RTX ToPBR] Auto-conversion starting...\n")
        RTXToPBR.ConvertBSPMaterials()
    end)
end)

-- Also clear on map cleanup (before new map loads)
hook.Add("PostCleanupMap", "RTXToPBR_MapCleanup", function()
    RTXToPBR.ClearCache()
end)

MsgC(Color(100, 255, 100), "[RTX ToPBR] Module loaded. Use 'rtx_topbr_convert' or enable auto-conversion.\n")

return RTXToPBR
