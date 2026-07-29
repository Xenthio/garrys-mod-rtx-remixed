if not (BRANCH == "x86-64" or BRANCH == "chromium") then return end
--[[
    RTX Remix Category Manager
    
    This library provides tools to control texture categories in RTX Remix based on runtime
    texture hash detection. It includes:
    - Category flag constants matching the Remix API
    - Hash-to-category mapping management
    - BSP parsing integration with niknaks for automatic world geometry detection
    - Utilities for marking world geometry as Decal for proper blending
]]--

if not RemixMaterial then
    return -- Silently skip if API not available
end

RemixCategoryManager = RemixCategoryManager or {}

-- ConVars for configuration
CreateClientConVar("rtx_auto_categorize", "1", true, false, "Automatically categorize world textures when a map loads (1 = enabled, 0 = disabled)")
CreateClientConVar("rtx_auto_categorize_delay", "3", true, false, "Delay in seconds before auto-categorization runs (default: 3)")
CreateClientConVar("rtx_auto_categorize_world", "1", true, false, "Auto-categorize world geometry from BSP as decals (1 = enabled, 0 = disabled)")
CreateClientConVar("rtx_auto_categorize_particles", "1", true, false, "Auto-categorize particle effects (1 = enabled, 0 = disabled)")
CreateClientConVar("rtx_auto_categorize_decals", "1", true, false, "Auto-categorize overlay decals with $decal parameter (1 = enabled, 0 = disabled)")
CreateClientConVar("rtx_auto_categorize_emissive", "0", true, false, "Auto-categorize legacy emissive materials (1 = enabled, 0 = disabled)")
CreateClientConVar("rtx_require_emissive_mask", "1", true, false, "Require $selfillummask or alpha channel for emissive materials (1 = strict, 0 = allow any $selfillum)")
CreateClientConVar("rtx_debug_categorization", "0", true, false, "Show debug messages for auto-categorization (1 = enabled, 0 = disabled)")

-- Cache ConVar references at module level to avoid repeated GetConVar calls
local cvar_auto_categorize_decals = GetConVar("rtx_auto_categorize_decals")
local cvar_auto_categorize_emissive = GetConVar("rtx_auto_categorize_emissive")
local cvar_auto_categorize_particles = GetConVar("rtx_auto_categorize_particles")
local cvar_debug_categorization = GetConVar("rtx_debug_categorization")

-- Remix Instance Category Flags
RemixCategoryManager.CATEGORY = {
    WORLD_UI                  = bit.lshift(1, 0),  -- 0x1
    WORLD_MATTE               = bit.lshift(1, 1),  -- 0x2
    SKY                       = bit.lshift(1, 2),  -- 0x4
    IGNORE                    = bit.lshift(1, 3),  -- 0x8
    IGNORE_LIGHTS             = bit.lshift(1, 4),  -- 0x10
    IGNORE_ANTI_CULLING       = bit.lshift(1, 5),  -- 0x20
    IGNORE_MOTION_BLUR        = bit.lshift(1, 6),  -- 0x40
    IGNORE_OPACITY_MICROMAP   = bit.lshift(1, 7),  -- 0x80
    IGNORE_ALPHA_CHANNEL      = bit.lshift(1, 8),  -- 0x100 
    HIDDEN                    = bit.lshift(1, 9),  -- 0x200
    PARTICLE                  = bit.lshift(1, 10), -- 0x400 
    BEAM                      = bit.lshift(1, 11), -- 0x800
    DECAL_STATIC              = bit.lshift(1, 12), -- 0x1000
    DECAL_DYNAMIC             = bit.lshift(1, 13), -- 0x2000
    DECAL_SINGLE_OFFSET       = bit.lshift(1, 14), -- 0x4000
    DECAL_NO_OFFSET           = bit.lshift(1, 15), -- 0x8000
    ALPHA_BLEND_TO_CUTOUT     = bit.lshift(1, 16), -- 0x10000
    TERRAIN                   = bit.lshift(1, 17), -- 0x20000
    ANIMATED_WATER            = bit.lshift(1, 18), -- 0x40000
    THIRD_PERSON_PLAYER_MODEL = bit.lshift(1, 19), -- 0x80000
    THIRD_PERSON_PLAYER_BODY  = bit.lshift(1, 20), -- 0x100000
    IGNORE_BAKED_LIGHTING     = bit.lshift(1, 21), -- 0x200000
    IGNORE_TRANSPARENCY_LAYER = bit.lshift(1, 22), -- 0x400000
    PARTICLE_EMITTER          = bit.lshift(1, 23), -- 0x800000
    LEGACY_EMISSIVE           = bit.lshift(1, 24), -- 0x1000000
}

-- Common category flag combinations
RemixCategoryManager.PRESET = {
    -- For opaque world geometry (walls, floors, etc.) from BSP - mark as Decal for proper blending
    WORLD_GEOMETRY = RemixCategoryManager.CATEGORY.DECAL_STATIC,  -- 0x1000
    
    -- For transparent world geometry (glass, windows, fences) - still needs Decal for BSP geometry
    WORLD_GEOMETRY_TRANSPARENT = RemixCategoryManager.CATEGORY.DECAL_STATIC,  -- 0x1000
    
    -- For sky textures
    SKY = RemixCategoryManager.CATEGORY.SKY,  -- 0x4
    
    -- For water
    WATER = RemixCategoryManager.CATEGORY.ANIMATED_WATER,  -- 0x40000
    
    -- For terrain (displacement surfaces) - mark as DECAL for proper blending
    TERRAIN = RemixCategoryManager.CATEGORY.DECAL_STATIC,  -- 0x1000
    
    -- For actual map decals (overlays, bullet holes, blood, etc.) - materials with $decal parameter
    MAP_DECAL = RemixCategoryManager.CATEGORY.DECAL_STATIC,  -- 0x1000
    
    -- For self-illuminated/emissive materials (lights, glows, LED panels)
    EMISSIVE = RemixCategoryManager.CATEGORY.LEGACY_EMISSIVE,  -- 0x1000000
    
    -- For player model textures (pseudoplayer, third-person view)
    PLAYER_MODEL = RemixCategoryManager.CATEGORY.THIRD_PERSON_PLAYER_MODEL,  -- 0x80000
    
    -- For props and models (NO decal flag - they're not part of the world geometry)
    PROP_STATIC = 0,  -- No special flags
}

-- Local cache of material name -> hash mappings
local materialHashCache = {}

-- Local cache of texture names that have been processed
local processedTextures = {}

-- Persistent pending categorizations: entries are {materialName, categoryFlags, materialsToTry, callback}
-- Drained each frame by the Think hook until the hash becomes available (material rendered)
local pendingCategorizations = {}

-- Map token to guard against stale timers firing after map change
local currentMapToken = game.GetMap() or ""

-- $no_fullbright is stored in Source's material flags as bit 1
-- (MATERIAL_VAR_NO_DEBUG_OVERRIDE). Prefer the raw VMT so an explicit opt-out
-- remains visible even if the compiled material does not expose the key.
local NO_FULLBRIGHT_FLAG = 0x2
local VERTEXCOLOR_FLAG = 0x10
local VERTEXALPHA_FLAG = 0x20

local function ReadRawVMT(materialName)
    local vmtPath = "materials/" .. materialName
    if not string.EndsWith(vmtPath, ".vmt") then
        vmtPath = vmtPath .. ".vmt"
    end
    return file.Read(vmtPath, "GAME"), vmtPath
end

local function HasNoFullbrightOptOut(materialName, mat, rawContent)
    local content = rawContent
    if content == nil then
        content = ReadRawVMT(materialName)
    end

    if content then
        for line in content:gmatch("[^\r\n]+") do
            local commentStart = line:find("//", 1, true)
            if commentStart then
                line = line:sub(1, commentStart - 1)
            end

            -- Removing quotes produces two ordinary tokens for both
            -- "$no_fullbright" 1 and "$no_fullbright" "1".
            local normalized = line:lower():gsub("[\"']", " ")
            local key, value = normalized:match("^%s*(%S+)%s+(%S+)")
            if key == "$no_fullbright" and tonumber(value) == 1 then
                return true
            end
        end
    end

    if not mat or mat:IsError() then return false end

    local keyValues = mat:GetKeyValues()
    if keyValues then
        for key, value in pairs(keyValues) do
            if type(key) == "string" and
               key:lower() == "$no_fullbright" and
               tonumber(value) == 1 then
                return true
            end
        end
    end

    if mat:GetInt("$no_fullbright") == 1 then
        return true
    end

    local flags = mat:GetInt("$flags") or 0
    return bit.band(flags, NO_FULLBRIGHT_FLAG) ~= 0
end

local function HasEnabledMaterialFlag(mat, rawContent, flagName, flagMask)
    if rawContent then
        for line in rawContent:gmatch("[^\r\n]+") do
            local commentStart = line:find("//", 1, true)
            if commentStart then
                line = line:sub(1, commentStart - 1)
            end

            local normalized = line:lower():gsub("[\"']", " ")
            local key, value = normalized:match("^%s*(%S+)%s+(%S+)")
            if key == flagName and tonumber(value) == 1 then
                return true
            end
        end
    end

    if not mat or mat:IsError() then return false end

    local keyValues = mat:GetKeyValues()
    if keyValues then
        for key, value in pairs(keyValues) do
            local lowerKey = type(key) == "string" and key:lower() or ""
            if lowerKey == flagName and tonumber(value) == 1 then
                return true
            end
        end
    end

    if mat:GetInt(flagName) == 1 then
        return true
    end

    local flags = mat:GetInt("$flags") or 0
    return bit.band(flags, flagMask) ~= 0
end

local function HasVertexColorOrAlphaOptOut(materialName, mat, rawContent)
    local content = rawContent
    if content == nil then
        content = ReadRawVMT(materialName)
    end

    return HasEnabledMaterialFlag(
               mat, content, "$vertexalpha", VERTEXALPHA_FLAG) or
           HasEnabledMaterialFlag(
               mat, content, "$vertexcolor", VERTEXCOLOR_FLAG)
end

local function HasUnlitEmissionOptOut(materialName, mat, rawContent)
    return HasNoFullbrightOptOut(materialName, mat, rawContent) or
           HasVertexColorOrAlphaOptOut(materialName, mat, rawContent)
end

--[[
    Check if a material is self-illuminated (emissive) with proper validation
    @param materialName string - The material name
    @return boolean - True if material has $selfillum enabled AND proper masking
]]--
function RemixCategoryManager.IsMaterialEmissive(materialName)
    local mat = Material(materialName)
    if not mat or mat:IsError() then
        return false
    end

    local content, vmtPath = ReadRawVMT(materialName)

    -- Source's unlit shaders render their full albedo without receiving scene
    -- lighting. Treat every variant (including DX-suffixed shader names) as
    -- emissive unless the VMT explicitly opts out with $no_fullbright,
    -- $vertexalpha, or $vertexcolor.
    local shader = mat:GetShader()
    if shader then
        local lowerShader = shader:lower()
        if lowerShader:find("^unlitgeneric") or
           lowerShader:find("^unlittwotexture") then
            return not HasUnlitEmissionOptOut(materialName, mat, content)
        end
    end
    
    local requireMask = GetConVar("rtx_require_emissive_mask"):GetBool()
    local hasSelfillum = false
    local hasMask = false
    
    -- Raw VMT File Parse
    local vmtParsedSuccessfully = false
    if content then
        vmtParsedSuccessfully = true
        local lines = string.Explode("\n", content)
        local foundSelfillumActive = false
        local foundSelfillumCommented = false
        
        for _, line in ipairs(lines) do
            local lowerLine = line:lower()
            local trimmedLine = string.Trim(line)  -- Trim original line, not lowercased (for better comment detection)
            local trimmedLowerLine = trimmedLine:lower()
            
            -- Check if line has $selfillum
            if lowerLine:find('%$selfillum') then
                -- Check if it's commented out (check for // at start after trimming whitespace)
                if string.StartsWith(trimmedLine, "//") or trimmedLine:find("^%s*//") then
                    foundSelfillumCommented = true
                -- Check if it's active and set to 1
                elseif trimmedLowerLine:find('["\']?%$selfillum["\']?%s+["\']?1["\']?') then
                    foundSelfillumActive = true
                end
            end
            
            -- Check for $selfillummask (only if not commented)
            if not (string.StartsWith(trimmedLine, "//") or trimmedLine:find("^%s*//")) then
                if lowerLine:find('%$selfillummask') then
                    hasMask = true
                end
            end
        end
        
        -- If we found $selfillum in the file (commented or not), use that as truth
        if foundSelfillumCommented or foundSelfillumActive then
            hasSelfillum = foundSelfillumActive  -- Only true if uncommented
            -- Debug output
            if foundSelfillumCommented and not foundSelfillumActive then
                -- Material has ONLY commented $selfillum - should NOT be emissive
            end
        end
    end
    
    -- Only check other methods if VMT file parse didn't find $selfillum parameter at all
    -- (This handles materials where the parameter might be set programmatically or in other ways)
    if not vmtParsedSuccessfully or (not hasSelfillum and content and not content:lower():find('%$selfillum')) then
        -- Method 1: Check raw KeyValues for $selfillum
        local keyValues = mat:GetKeyValues()
        if keyValues then
            for k, v in pairs(keyValues) do
                local lowerKey = k:lower()
                if lowerKey == "$selfillum" then
                    local val = tonumber(v)
                    if val and val >= 1 then
                        hasSelfillum = true
                    end
                end
                -- Check for $selfillummask
                if lowerKey == "$selfillummask" then
                    hasMask = true
                end
            end
        end

        -- Method 2: Check standard GetInt (compiled params)
        if not hasSelfillum then
            local selfillum = mat:GetInt("$selfillum")
            if selfillum and selfillum == 1 then
                hasSelfillum = true
            end
        end
        
        -- Method 3: Check GetFloat (sometimes stored as float)
        if not hasSelfillum then
            local selfillumFloat = mat:GetFloat("$selfillum")
            if selfillumFloat and selfillumFloat >= 1 then
                hasSelfillum = true
            end
        end
        
        -- Method 4: Check for $selfillummask texture
        if not hasMask then
            local mask = mat:GetTexture("$selfillummask")
            if mask and not mask:IsError() then
                hasMask = true
            end
        end
    end
    
    -- Method 5: Check $emissive parameter (used in some shaders - doesn't need mask)
    local emissive = mat:GetVector("$emissive")
    if emissive and (emissive.x > 0 or emissive.y > 0 or emissive.z > 0) then
        return true
    end
    
    -- $illumposition is deliberately not an emissive signal. It controls the
    -- origin used to light a model, and shaders may expose a default vector even
    -- when the VMT does not define it. Treating its presence as emission made
    -- ordinary VertexLitGeneric and DecalModulate materials glow.

    -- If $selfillum is found on any other shader, check if mask is required
    if hasSelfillum then
        if requireMask then
            -- Strict mode: require $selfillummask or alpha rendering mode
            if hasMask then
                return true
            end
            
            -- Check if material has alpha rendering enabled
            -- ($translucent or $alphatest indicates alpha channel is used for masking)
            local translucent = mat:GetInt("$translucent")
            local alphatest = mat:GetInt("$alphatest")
            if (translucent and translucent == 1) or (alphatest and alphatest == 1) then
                return true
            end
            
            -- No valid mask found - return false to avoid fullbright materials
            return false
        else
            -- Permissive mode: any $selfillum counts
            return true
        end
    end

    return false
end

--[[
    Check if a material should be excluded from world geometry categorization
    @param materialName string - The material name
    @return boolean - True if material should NOT be treated as world geometry (no DECAL_STATIC)
]]--
function RemixCategoryManager.ShouldExcludeFromWorldGeometry(materialName)
    local mat = Material(materialName)
    if not mat or mat:IsError() then
        return false
    end
    
    -- IMPORTANT: If material has $decal flag, NEVER exclude it
    -- Decals with $translucent are still valid decals (graffiti, blood, etc.)
    if RemixCategoryManager.IsMaterialDecal(materialName) then
        return false
    end
    
    -- Check for $translucent - these are alpha-blended surfaces (foliage, etc.)
    local keyValues = mat:GetKeyValues()
    if keyValues then
        for k, v in pairs(keyValues) do
            local lowerKey = k:lower()
            
            -- Check $translucent
            if lowerKey == "$translucent" then
                local val = tonumber(v)
                if val and val >= 1 then
                    return true  -- Translucent materials shouldn't be world geometry decals
                end
            end
            
            -- Check $surfaceprop for "no_decal"
            if lowerKey == "$surfaceprop" then
                local strVal = tostring(v):lower()
                if strVal:find("no_decal") then
                    return true  -- Explicitly marked to not receive decals
                end
            end
        end
    end
    
    -- Check GetInt for $translucent
    local translucent = mat:GetInt("$translucent")
    if translucent and translucent == 1 then
        return true
    end
    
    -- Check GetString for $surfaceprop
    local surfaceprop = mat:GetString("$surfaceprop")
    if surfaceprop and surfaceprop:lower():find("no_decal") then
        return true
    end
    
    -- Raw VMT file parse for reliability
    local vmtPath = "materials/" .. materialName
    if not string.EndsWith(vmtPath, ".vmt") then
        vmtPath = vmtPath .. ".vmt"
    end
    
    local content = file.Read(vmtPath, "GAME")
    if content then
        local lines = string.Explode("\n", content)
        
        for _, line in ipairs(lines) do
            local lowerLine = line:lower()
            local trimmedLine = string.Trim(lowerLine)
            
            -- Skip commented lines
            if not string.StartsWith(trimmedLine, "//") then
                -- Check for $translucent 1
                if lowerLine:find('["\']?%$translucent["\']?%s+["\']?1["\']?') then
                    return true
                end
                
                -- Check for $surfaceprop "no_decal"
                if lowerLine:find('%$surfaceprop') and lowerLine:find('no_decal') then
                    return true
                end
            end
        end
    end
    
    return false
end

--[[
    Check if a material is a decal (has $decal parameter)
    @param materialName string - The material name
    @return boolean - True if material has $decal enabled
]]--
function RemixCategoryManager.IsMaterialDecal(materialName)
    local mat = Material(materialName)
    if not mat or mat:IsError() then
        return false
    end
    
    -- Method 1: Check raw KeyValues (Most reliable for VMT parameters)
    local keyValues = mat:GetKeyValues()
    if keyValues then
        -- Check $decal (case-insensitive scan)
        for k, v in pairs(keyValues) do
            if k:lower() == "$decal" then
                local val = tonumber(v)
                if val and val >= 1 then return true end
            end
        end
    end
    
    -- Method 2: Check standard GetInt (compiled params)
    local decal = mat:GetInt("$decal")
    if decal and decal == 1 then return true end
    
    -- Method 3: Check GetFloat (sometimes stored as float)
    local decalFloat = mat:GetFloat("$decal")
    if decalFloat and decalFloat >= 1 then return true end
    
    -- Method 4: Check decal shaders (prefix match for DX suffixes like DecalModulate_dx6)
    local shader = mat:GetShader()
    if shader then
        local s = shader:lower()
        if s:find("^decalmodulate") or 
           (s:find("^decal") and not s:find("modulate")) then
            return true
        end
    end
    
    -- Method 5: Path-based detection (matches C++ heuristics, exclude light materials)
    local lowerName = materialName:lower()
    if (lowerName:find("^decals/") or
        lowerName:find("/decals/") or
        lowerName:find("overlay") or
        lowerName:find("bulleth") or
        lowerName:find("_blood") or
        lowerName:find("blood_") or
        lowerName:find("/blood") or
        lowerName:find("scorch")) and
       not lowerName:find("light") and
       not lowerName:find("/lights/") and
       not lowerName:find("^lights/") then
        return true
    end
    
    -- Method 6: Raw VMT File Parse
    local vmtPath = "materials/" .. materialName
    if not string.EndsWith(vmtPath, ".vmt") then
        vmtPath = vmtPath .. ".vmt"
    end
    
    local content = file.Read(vmtPath, "GAME")
    if content then
        local lines = string.Explode("\n", content)
        
        for _, line in ipairs(lines) do
            local lowerLine = line:lower()
            
            local trimmedLine = string.Trim(lowerLine)
            if not string.StartsWith(trimmedLine, "//") then
                if lowerLine:find('["\']?%$decal["\']?%s+["\']?1["\']?') then
                    return true
                end
            end
        end
    end
    
    return false
end

--[[
    Check if a material is likely a particle/sprite
    @param materialName string - The material name
    @return boolean - True if material is likely a particle
]]--
function RemixCategoryManager.IsMaterialParticle(materialName)
    local lowerName = materialName:lower()
    
    -- Check common particle paths (both singular and plural forms)
    if lowerName:find("^particles/") or 
       lowerName:find("^particle/") or
       lowerName:find("^effects/") or 
       lowerName:find("^sprites/") or
       lowerName:find("/particles/") or
       lowerName:find("/particle/") or
       lowerName:find("/effects/") or
       lowerName:find("/sprites/") then
        return true
    end
    
    local mat = Material(materialName)
    if not mat or mat:IsError() then return false end
    
    local shader = mat:GetShader()
    if shader then
        local s = shader:lower()
        
        -- Prefix match to handle DX suffixes (e.g. Sprite_dx6, Modulate_dx6)
        if s:find("^sprite") or 
           s:find("^cable") or 
           s:find("^modulate") then
            return true
        end
        
        -- Vertex-colored UnlitGeneric materials are particle materials. Do not
        -- require $vertexalpha; many opaque/additive particles only modulate RGB.
        if s:find("^unlitgeneric") then
            local content = ReadRawVMT(materialName)
            if HasEnabledMaterialFlag(
                mat, content, "$vertexcolor", VERTEXCOLOR_FLAG) then
                return true
            end
        end
    end
    
    return false
end

--[[
    Check whether Source renders this material with an unlit shader.
    Prefix matching covers runtime names such as UnlitGeneric_dx9.
    @param materialName string
    @return boolean
]]--
function RemixCategoryManager.IsUnlitShaderEmissive(materialName)
    local mat = Material(materialName)
    if not mat or mat:IsError() then return false end

    local shader = mat:GetShader()
    if not shader then return false end

    local lowerShader = shader:lower()
    local isUnlit = lowerShader:find("^unlitgeneric") ~= nil or
                    lowerShader:find("^unlittwotexture") ~= nil
    return isUnlit and
           not HasUnlitEmissionOptOut(materialName, mat)
end

--[[
    Check if a material is an UnlitTwoTexture panel that drives its overlay via a
    scanline texture ($texture2 or %tooltexture referencing "scanline").
    These materials are inherently fullbright but have no alpha mask, so Remix would
    suppress their emission unless force-albedo mode is enabled for their hash.
    @param materialName string
    @return boolean
]]--
function RemixCategoryManager.IsUnlitScanlineEmissive(materialName)
    local mat = Material(materialName)
    if not mat or mat:IsError() then return false end
    
    local shader = mat:GetShader()
    if not shader or not shader:lower():find("^unlittwotexture") then return false end
    
    -- Primary: check $texture2 via material API
    local tex2 = mat:GetTexture("$texture2")
    if tex2 and not tex2:IsError() and tex2:GetName():lower():find("scanline") then
        return true
    end
    
    -- Fallback: scan raw VMT for $texture2 or %tooltexture pointing at a scanline texture
    local vmtPath = "materials/" .. materialName
    if not string.EndsWith(vmtPath, ".vmt") then vmtPath = vmtPath .. ".vmt" end
    local content = file.Read(vmtPath, "GAME")
    if content then
        local lc = content:lower()
        if lc:find('"?%$texture2"?%s+"[^"]*scanline') or
           lc:find('"?%%tooltexture"?%s+"[^"]*scanline') then
            return true
        end
    end
    
    return false
end

--[[
    Check if a material is an UnlitGeneric that has $selfillum enabled.
    @param materialName string
    @return boolean
]]--
function RemixCategoryManager.IsUnlitSelfillumEmissive(materialName)
    local mat = Material(materialName)
    if not mat or mat:IsError() then return false end
    
    local shader = mat:GetShader()
    if not shader or not shader:lower():find("^unlitgeneric") then return false end
    
    -- Check via material API first (fastest)
    local selfillum = mat:GetInt("$selfillum")
    if selfillum and selfillum == 1 then return true end
    
    -- Fallback: raw VMT parse
    local vmtPath = "materials/" .. materialName
    if not string.EndsWith(vmtPath, ".vmt") then vmtPath = vmtPath .. ".vmt" end
    local content = file.Read(vmtPath, "GAME")
    if content then
        local lc = content:lower()
        if lc:find('["\']?%$selfillum["\']?%s+["\']?1["\']?') then
            return true
        end
    end
    
    return false
end

--[[
    Returns true when an emissive material needs force-albedo mode because Remix
    would otherwise suppress emission or interpret alpha as an emissive mask.
    Source treats the full albedo of UnlitGeneric and UnlitTwoTexture as unlit,
    so both shaders use force-albedo emission unless a supported VMT flag opts out.
    @param materialName string
    @return boolean
]]--
function RemixCategoryManager.NeedsForceAlbedoEmission(materialName)
    return RemixCategoryManager.IsUnlitShaderEmissive(materialName)
end

-- Build the callback used by immediate and deferred hash resolution. Re-check
-- the surviving category because C++ may reject a contested global emissive hash.
local function MakeForceAlbedoCallback(materialName, categoryFlags)
    if not RemixMaterial.AddForceAlbedoHash or
       bit.band(categoryFlags or 0, RemixCategoryManager.CATEGORY.LEGACY_EMISSIVE) == 0 or
       not RemixCategoryManager.NeedsForceAlbedoEmission(materialName) then
        return nil
    end

    return function(success, hashStr)
        if not success then return end

        local survivingFlags = RemixMaterial.GetHashCategory and
                               RemixMaterial.GetHashCategory(hashStr)
        if survivingFlags and
           bit.band(survivingFlags, RemixCategoryManager.CATEGORY.LEGACY_EMISSIVE) ~= 0 then
            RemixMaterial.AddForceAlbedoHash(hashStr)
        end
    end
end

--[[
    Categorize all currently tracked materials
    This catches dynamic materials (weapons, effects) that aren't in the BSP
    @return table - Statistics
]]--
function RemixCategoryManager.CategorizeAllTrackedMaterials()
    if not RemixMaterial or not RemixMaterial.GetCachedMaterials then
        return { error = "Remix API not available" }
    end
    
    MsgC(Color(100, 200, 255), "[RemixCategoryManager] Scanning all tracked materials...\n")
    
    local materials = RemixMaterial.GetCachedMaterials()
    if not materials then return { count = 0 } end
    
    local stats = {
        total = #materials,
        particles = 0,
        emissive = 0,
        decals = 0,
        newly_categorized = 0
    }
    
    -- Check which categories are enabled
    local enableDecals = GetConVar("rtx_auto_categorize_decals"):GetBool()
    local enableEmissive = GetConVar("rtx_auto_categorize_emissive"):GetBool()
    
    for _, matName in ipairs(materials) do
        local lowerName = matName:lower()
        
        if not processedTextures[lowerName] then
            processedTextures[lowerName] = true
            local category = nil
            
            
            -- Unlit materials must retain emissive priority even when their path
            -- or VMT also resembles a decal.
            if enableEmissive and RemixCategoryManager.IsMaterialEmissive(matName) then
                category = RemixCategoryManager.PRESET.EMISSIVE
                stats.emissive = stats.emissive + 1

            -- Check for decals (important - catches map overlay decals!)
            elseif enableDecals and RemixCategoryManager.IsMaterialDecal(matName) then
                category = RemixCategoryManager.CATEGORY.DECAL_STATIC
                stats.decals = stats.decals + 1
                
                -- Debug first few
                if stats.decals <= 3 then
                    MsgC(Color(100, 255, 100), string.format("[RemixCategoryManager] Found overlay decal: %s\n", matName))
                end
            end
            
            if category then
                local forceAlbedoCallback =
                    MakeForceAlbedoCallback(matName, category)

                -- Skip only when every hash already contains every requested
                -- bit. A particle/decal-only hash still needs EMISSIVE added.
                local allHashes = RemixMaterial.GetAllTextureHashes and RemixMaterial.GetAllTextureHashes(matName)
                if allHashes and #allHashes > 0 then
                    local alreadyCategorized = true
                    for _, hashStr in ipairs(allHashes) do
                        local existing = RemixMaterial.GetHashCategory and RemixMaterial.GetHashCategory(hashStr)
                        if bit.band(existing or 0, category) ~= category then
                            alreadyCategorized = false
                            break
                        end
                    end
                    
                    if not alreadyCategorized then
                        if RemixCategoryManager.SetMaterialCategory(
                            matName, category, forceAlbedoCallback) then
                            stats.newly_categorized = stats.newly_categorized + 1
                        end
                    elseif forceAlbedoCallback then
                        for _, hashStr in ipairs(allHashes) do
                            forceAlbedoCallback(true, hashStr)
                        end
                    end
                else
                    -- No hashes yet, categorize anyway (will retry later)
                    if RemixCategoryManager.SetMaterialCategory(
                        matName, category, forceAlbedoCallback) then
                        stats.newly_categorized = stats.newly_categorized + 1
                    end
                end
            end
        end
    end
    
    MsgC(Color(100, 255, 100), string.format("[RemixCategoryManager] Tracked scan: %d total, %d decals, %d emissive found (particles/water/sky handled by C++)\n",
        stats.total, stats.decals, stats.emissive))
        
    return stats
end

--[[
    Auto-categorize a single material (called by MaterialPipeline)
    @param materialName string - The material name to categorize
    @return boolean - True if the material was categorized
]]--
function RemixCategoryManager.AutoCategorizeMaterial(materialName)
    if not RemixMaterial then
        return false
    end
    
    local lowerName = materialName:lower()
    
    -- Check if already processed
    if processedTextures[lowerName] then
        return false
    end
    
    -- Check which categories are enabled
    local enableDecals = cvar_auto_categorize_decals:GetBool()
    local enableEmissive = cvar_auto_categorize_emissive:GetBool()
    local enableParticles = cvar_auto_categorize_particles:GetBool()
    local debug = cvar_debug_categorization:GetBool()
    
    local category = nil
    local categoryName = nil
    
    -- Wrap in pcall to catch C++ exceptions
    -- pcall returns: success, result1, result2, ...
    local success, pcallCategory, pcallCategoryName = pcall(function()
        -- Unlit shaders are always emissive. Preserve a particle/decal bit when
        -- applicable instead of allowing those older priority checks to hide it.
        if enableEmissive and
           RemixCategoryManager.IsUnlitShaderEmissive(materialName) then
            local flags = RemixCategoryManager.PRESET.EMISSIVE
            local label = "EMISSIVE"

            if enableParticles and
               RemixCategoryManager.IsMaterialParticle(materialName) then
                flags = bit.bor(flags, RemixCategoryManager.CATEGORY.PARTICLE)
                label = "PARTICLE | EMISSIVE"
            elseif enableDecals and
                   RemixCategoryManager.IsMaterialDecal(materialName) then
                flags = bit.bor(flags, RemixCategoryManager.CATEGORY.DECAL_STATIC)
                label = "DECAL | EMISSIVE"
            end

            return flags, label
        end

        -- Check for particles (including sprites)
        if enableParticles and RemixCategoryManager.IsMaterialParticle(materialName) then
            return RemixCategoryManager.CATEGORY.PARTICLE, "PARTICLE"
        
        -- Check for decals
        elseif enableDecals and RemixCategoryManager.IsMaterialDecal(materialName) then
            return RemixCategoryManager.CATEGORY.DECAL_STATIC, "DECAL"
        
        -- Check for emissive
        elseif enableEmissive and RemixCategoryManager.IsMaterialEmissive(materialName) then
            return RemixCategoryManager.PRESET.EMISSIVE, "EMISSIVE"
        end
        
        return nil, nil
    end)
    
    if not success then
        -- C++ exception occurred, mark as processed to avoid retries
        processedTextures[lowerName] = true
        if debug then
            MsgC(Color(255, 100, 100), string.format("[RemixCategoryManager] Exception checking material: %s\n", materialName))
        end
        return false
    end
    
    -- Use the properly captured return values from pcall
    category = pcallCategory
    categoryName = pcallCategoryName
    
    if category then
        -- Mark as processed
        processedTextures[lowerName] = true

        local forceAlbedoCallback =
            MakeForceAlbedoCallback(materialName, category)
        
        -- Check if already categorized (with pcall protection)
        local hashSuccess, allHashes = pcall(function()
            if RemixMaterial.GetAllTextureHashes then
                return RemixMaterial.GetAllTextureHashes(materialName)
            end
            return nil
        end)
        
        if hashSuccess and allHashes and #allHashes > 0 then
            local alreadyCategorized = true
            for _, hashStr in ipairs(allHashes) do
                local catSuccess, existing = pcall(function()
                    if RemixMaterial.GetHashCategory then
                        return RemixMaterial.GetHashCategory(hashStr)
                    end
                    return nil
                end)
                if not catSuccess or
                   bit.band(existing or 0, category) ~= category then
                    alreadyCategorized = false
                    break
                end
            end
            
            if alreadyCategorized then
                if forceAlbedoCallback then
                    for _, hashStr in ipairs(allHashes) do
                        forceAlbedoCallback(true, hashStr)
                    end
                end
                if debug then
                    MsgC(Color(200, 200, 200), string.format("[RemixCategoryManager] %s already categorized: %s\n", categoryName or "UNKNOWN", materialName))
                end
                return false
            end
        end
        
        -- Apply category (with pcall protection)
        local setSuccess, setResult = pcall(function()
            return RemixCategoryManager.SetMaterialCategory(materialName, category, forceAlbedoCallback)
        end)
        
        if setSuccess and setResult then
            if debug then
                MsgC(Color(100, 255, 100), string.format("[RemixCategoryManager] Categorized as %s: %s\n", categoryName or "UNKNOWN", materialName))
            end
            return true
        end
    end
    
    -- Mark as processed even if not categorized (so we don't re-check)
    processedTextures[lowerName] = true
    return false
end

function RemixCategoryManager.ForceTrackTexture(textureName)
    -- Create a simple material that uses this texture
    local matName = "rtx_force_track_" .. textureName:gsub("[^%w]", "_")
    
    local mat = Material(textureName)
    if not mat or mat:IsError() then
        -- Try creating a material wrapper
        local ok, result = pcall(function()
            return CreateMaterial(matName, "UnlitGeneric", {
                ["$basetexture"] = textureName,
                ["$vertexcolor"] = 1,
                ["$vertexalpha"] = 1,
            })
        end)
        
        if not ok or not result then
            return false
        end
        mat = result
    end
    
    -- Set as current material for tracking
    RemixMaterial.TrackMaterial(matName)
    
    -- Render it to a tiny off-screen surface to force D3D9 to bind it
    render.PushRenderTarget(render.GetScreenEffectTexture(0))
    cam.Start2D()
        surface.SetDrawColor(255, 255, 255, 255)
        surface.SetMaterial(mat)
        surface.DrawTexturedRect(0, 0, 1, 1)
    cam.End2D()
    render.PopRenderTarget()
    
    -- The created material name is what will be tracked, not the texture name
    return matName
end

--[[
    Set category flags for a texture hash
    @param textureHash string|number - The texture hash (can be hex string like "0x..." or number)
    @param categoryFlags number - The category flags to set (use CATEGORY constants)
    @return boolean - Success
]]--
function RemixCategoryManager.SetHashCategory(textureHash, categoryFlags)
    return RemixMaterial.SetHashCategory(textureHash, categoryFlags)
end

--[[
    Remove category mapping for a texture hash
    @param textureHash string|number - The texture hash
    @return boolean - Success
]]--
function RemixCategoryManager.RemoveHashCategory(textureHash)
    return RemixMaterial.RemoveHashCategory(textureHash)
end

--[[
    Clear all hash-to-category mappings
    @return boolean - Success
]]--
function RemixCategoryManager.ClearAllCategories()
    processedTextures = {}
    materialHashCache = {}
    return RemixMaterial.ClearHashCategories()
end

--[[
    Get category flags for a texture hash
    @param textureHash string|number - The texture hash
    @return number|nil - The category flags, or nil if not found
]]--
function RemixCategoryManager.GetHashCategory(textureHash)
    return RemixMaterial.GetHashCategory(textureHash)
end

--[[
    Track a material and return its texture hash
    @param materialName string - The Source Engine material name
    @return string|nil, number|nil - Hash as string, hash as number (nil if not found)
]]--
function RemixCategoryManager.GetMaterialHash(materialName)
    -- Check cache first
    if materialHashCache[materialName] then
        return materialHashCache[materialName].str, materialHashCache[materialName].num
    end
    
    -- Track the material to ensure it's loaded
    RemixMaterial.TrackMaterial(materialName)
    
    -- Get the hash (returns both number and string)
    local hashNum, hashStr = RemixMaterial.GetTextureHash(materialName)
    
    if hashNum and hashNum ~= 0 then
        -- Cache it
        materialHashCache[materialName] = {
            str = hashStr,
            num = hashNum
        }
        return hashStr, hashNum
    end
    
    return nil, nil
end

--[[
    Set category for a material by name
    @param materialName string - The Source Engine material name
    @param categoryFlags number - The category flags to set
    @param callback function - Optional callback(success, hash) when done
    @return boolean - Success (immediate if cached, false if needs tracking)
]]--
function RemixCategoryManager.SetMaterialCategory(materialName, categoryFlags, callback)
    -- Try both the base material name and the _stage1 variant (for displacement materials)
    local materialsToTry = {materialName}
    if not string.match(materialName, "_stage1$") then
        table.insert(materialsToTry, materialName .. "_stage1")
    end
    
    local foundAny = false
    local categorizedHashes = {}
    
    for _, tryName in ipairs(materialsToTry) do
        -- Get ALL hashes for this material (there can be multiple texture variants)
        local allHashes = RemixMaterial.GetAllTextureHashes and RemixMaterial.GetAllTextureHashes(tryName)
        
        if allHashes and #allHashes > 0 then
            for _, hashStr in ipairs(allHashes) do
                -- Skip if we've already handled this hash in this call
                if not categorizedHashes[hashStr] then
                    categorizedHashes[hashStr] = true
                    -- Categories are a bitmask. Preserve any prior particle/decal
                    -- classification while adding newly detected properties such
                    -- as the mandatory emissive bit for unlit shaders.
                    local existing = RemixMaterial.GetHashCategory and
                                     RemixMaterial.GetHashCategory(hashStr) or 0
                    local mergedFlags = bit.bor(existing, categoryFlags)
                    if mergedFlags ~= existing then
                        MsgC(Color(0, 255, 150), string.format("[RemixCategoryManager] Setting category 0x%X for material '%s' (hash %s)\n", 
                            mergedFlags, tryName, hashStr))
                        local success = RemixCategoryManager.SetHashCategory(hashStr, mergedFlags)
                        if callback then callback(success, hashStr) end
                    else
                        -- The requested bits already exist, but side-effects such
                        -- as force-albedo registration may still need restoring.
                        if callback then callback(true, hashStr) end
                    end
                    foundAny = true
                end
            end
        end
    end
    
    if foundAny then
        -- Hashes were available immediately. Still push a watch entry into the pending
        -- queue so any additional variants that appear within the next few seconds
        -- (e.g. a second _stage1 displacement instance) also get categorized.
        table.insert(pendingCategorizations, {
            materialName   = materialName,
            categoryFlags  = categoryFlags,
            materialsToTry = materialsToTry,
            callback       = callback,
            firstApplied   = CurTime(),
        })
        return true
    end
    
    -- Hash not available yet - add to persistent retry queue so it gets applied the
    -- first frame the material is rendered (no matter how late that happens).
    -- processedTextures is NOT set here; it will be set when the queue entry succeeds.
    RemixMaterial.TrackMaterial(materialName)
    table.insert(pendingCategorizations, {
        materialName = materialName,
        categoryFlags = categoryFlags,
        materialsToTry = materialsToTry,
        callback = callback,
    })
    
    return false -- Not immediate
end

--[[
    Parse BSP and mark all world textures with a category
    @param categoryFlags number - The category flags to apply (default: WORLD_GEOMETRY)
    @return number - Number of textures processed
]]--
function RemixCategoryManager.MarkWorldTextures(categoryFlags)
    categoryFlags = categoryFlags or RemixCategoryManager.PRESET.WORLD_GEOMETRY
    
    if not NikNaks or not NikNaks.CurrentMap then
        MsgC(Color(255, 100, 100), "[RemixCategoryManager] Error: NikNaks library not available!\n")
        MsgC(Color(255, 200, 100), "[RemixCategoryManager] Install NikNaks addon: https://github.com/Nak2/NikNaks\n")
        return 0
    end
    
    local bsp = NikNaks.CurrentMap
    if not bsp then
        MsgC(Color(255, 100, 100), "[RemixCategoryManager] Error: Could not load current map BSP!\n")
        return 0
    end
    
    MsgC(Color(100, 200, 255), "[RemixCategoryManager] Parsing BSP for world textures...\n")
    MsgC(Color(200, 200, 200), "[RemixCategoryManager] Map name: " .. game.GetMap() .. "\n")
    
    local ok, textures = pcall(function() return bsp:GetTextures() end)
    if not ok then
        MsgC(Color(255, 100, 100), "[RemixCategoryManager] Error calling GetTextures: " .. tostring(textures) .. "\n")
        return 0
    end
    
    MsgC(Color(200, 200, 200), string.format("[RemixCategoryManager] Got %d textures from GetTextures()\n", textures and #textures or 0))
    
    -- If GetTextures returns nothing, try getting from faces instead
    if not textures or #textures == 0 then
        MsgC(Color(255, 200, 100), "[RemixCategoryManager] GetTextures() returned empty, trying GetFaces() instead...\n")
        
        local faces = bsp:GetFaces()
        if faces then
            MsgC(Color(200, 200, 200), string.format("[RemixCategoryManager] Got %d faces from BSP\n", #faces))
            
            -- Extract unique texture names from faces
            local textureSet = {}
            for _, face in pairs(faces) do
                if face then
                    -- Try GetMaterial first (returns IMaterial)
                    if face.GetMaterial then
                        local ok2, material = pcall(function() return face:GetMaterial() end)
                        if ok2 and material and material.GetName then
                            local matName = material:GetName()
                            if matName and matName ~= "" then
                                textureSet[matName] = true
                            end
                        end
                    end
                    -- Also try GetTexture (might return texture object or string)
                    if face.GetTexture then
                        local ok3, texture = pcall(function() return face:GetTexture() end)
                        if ok3 and texture then
                            -- Could be string or object
                            if type(texture) == "string" then
                                textureSet[texture] = true
                            elseif type(texture) == "table" and texture.name then
                                textureSet[texture.name] = true
                            end
                        end
                    end
                end
            end
            
            -- Convert set to array
            textures = {}
            for texName, _ in pairs(textureSet) do
                table.insert(textures, texName)
            end
            
            MsgC(Color(200, 200, 200), string.format("[RemixCategoryManager] Extracted %d unique textures from faces\n", #textures))
        end
    end
    
    -- Also get displacement textures (terrain surfaces)
    MsgC(Color(200, 200, 200), "[RemixCategoryManager] Checking for displacements...\n")
    MsgC(Color(255, 255, 0), "[RemixCategoryManager] DEBUG: Displacement code version 2.0\n")
    
    local okLeafs, allLeafs = pcall(function() return bsp:GetLeafs() end)
    MsgC(Color(255, 255, 0), string.format("[RemixCategoryManager] GetLeafs result: ok=%s, leafs=%s\n", 
        tostring(okLeafs), allLeafs and table.Count(allLeafs) or "nil"))
    
    if okLeafs and allLeafs then
        textures = textures or {}
        local dispCount = 0
        local seenDisplacements = {}
        local leafCount = 0
        local faceCount = 0
        local dispFaceCount = 0
        
        for _, leaf in pairs(allLeafs) do
            leafCount = leafCount + 1
            if leaf then
                local okFaces, leafFaces = pcall(function() return leaf:GetFaces(true) end) -- include displacements
                if okFaces and leafFaces then
                    for _, face in pairs(leafFaces) do
                        faceCount = faceCount + 1
                        if face and face.IsDisplacement and face:IsDisplacement() then
                            dispFaceCount = dispFaceCount + 1
                            -- Get material from this displacement face
                            local ok_mat, material = pcall(function() return face:GetMaterial() end)
                            if dispFaceCount <= 3 then  -- Only log first 3 to avoid spam
                                MsgC(Color(255, 255, 0), string.format("[RemixCategoryManager] Disp face %d: GetMaterial ok=%s, material=%s\n",
                                    dispFaceCount, tostring(ok_mat), tostring(material)))
                            end
                            if ok_mat and material then
                                -- Get the ACTUAL material name being rendered by the engine
                                -- This is what gets tracked by D3D9 (e.g., "maps/.../blend_blacktop_01_wvt_patch")
                                local matName = material:GetName()
                                
                                -- Normalize the material name. Source Engine generates runtime WVT patch
                                -- materials for blend displacements with names like
                                -- "maps/<mapname>/nature/blend_foo_wvt_patch". The D3D9 tracker stores
                                -- them under the base material name without that prefix/suffix, so we
                                -- strip both here to match what GetAllTextureHashes expects.
                                local normalizedMatName = matName
                                normalizedMatName = string.gsub(normalizedMatName, "^materials/", "")
                                normalizedMatName = string.gsub(normalizedMatName, "%.vmt$", "")
                                normalizedMatName = string.gsub(normalizedMatName, "^maps/[^/]+/", "")
                                normalizedMatName = string.gsub(normalizedMatName, "_wvt_patch$", "")
                                if matName and matName ~= "" and not seenDisplacements[normalizedMatName] then
                                    seenDisplacements[normalizedMatName] = true
                                    dispCount = dispCount + 1
                                    
                                    if dispFaceCount <= 3 then
                                        MsgC(Color(255, 255, 0), string.format("[RemixCategoryManager] Displacement material: '%s'\n", normalizedMatName))
                                        
                                        -- Also show the base textures for debug
                                        local baseTex = material:GetTexture("$basetexture")
                                        local baseTex2 = material:GetTexture("$basetexture2")
                                        if baseTex and baseTex.GetName then
                                            MsgC(Color(200, 200, 200), string.format("    $basetexture: %s\n", baseTex:GetName()))
                                        end
                                        if baseTex2 and baseTex2.GetName then
                                            MsgC(Color(200, 200, 200), string.format("    $basetexture2: %s\n", baseTex2:GetName()))
                                        end
                                    end
                                    
                                    -- Track the material (it will be tracked automatically when rendered, but this ensures it's in our list)
                                    RemixMaterial.TrackMaterial(normalizedMatName)
                                    
                                    -- Check if not already in main texture list
                                    local found = false
                                    for _, existingTex in ipairs(textures) do
                                        if existingTex == normalizedMatName then
                                            found = true
                                            break
                                        end
                                    end
                                    
                                    if not found then
                                        table.insert(textures, normalizedMatName)
                                    end
                                end
                            end
                        end
                    end
                end
            end
        end
        
        MsgC(Color(255, 255, 0), string.format("[RemixCategoryManager] Traversed %d leafs, %d faces, found %d displacement faces\n", 
            leafCount, faceCount, dispFaceCount))
        
        if dispCount > 0 then
            MsgC(Color(100, 255, 100), string.format("[RemixCategoryManager] Added %d displacement textures\n", dispCount))
        else
            MsgC(Color(200, 200, 200), "[RemixCategoryManager] No displacement textures found\n")
        end
    else
        MsgC(Color(255, 200, 100), "[RemixCategoryManager] Could not get leafs from BSP\n")
    end
    
    local count = 0
    local marked = 0
    
    if not textures or #textures == 0 then
        MsgC(Color(255, 100, 100), "[RemixCategoryManager] Error: No textures found!\n")
        return 0
    end
    
    for i, texName in ipairs(textures) do
        if texName then
            count = count + 1
            
            -- Normalize material name - remove materials/ prefix and .vmt extension if present
            -- The C++ tracking expects just the material path like "dev/dev_measurecrate01"
            local materialName = texName
            materialName = string.gsub(materialName, "^materials/", "")
            materialName = string.gsub(materialName, "%.vmt$", "")
            
            local lowerName = string.lower(materialName)
            
            -- Skip if already processed (either applied immediately or queued for retry)
            if not processedTextures[lowerName] then
                -- Set category for this material.
                -- If the hash is available now, SetMaterialCategory returns true and we mark
                -- processedTextures immediately. If the hash is 0 (material not yet rendered),
                -- SetMaterialCategory queues the entry in pendingCategorizations and returns
                -- false - processedTextures stays unset so the queue can mark it on success.
                if RemixCategoryManager.SetMaterialCategory(materialName, categoryFlags) then
                    processedTextures[lowerName] = true
                    marked = marked + 1
                end
            end
        end
    end
    
    MsgC(Color(100, 255, 100), string.format("[RemixCategoryManager] Processed %d BSP textures, marked %d new textures with category 0x%X\n",
        count, marked, categoryFlags))
    
    return marked
end

--[[
    Apply categories to world textures based on their properties
    Uses heuristics to determine appropriate categories (e.g., glass vs solid walls)
    @return table - Statistics about processed textures
]]--
function RemixCategoryManager.SmartMarkWorldTextures()
    if not NikNaks or not NikNaks.CurrentMap then
        MsgC(Color(255, 100, 100), "[RemixCategoryManager] Error: NikNaks library not available!\n")
        return { error = "NikNaks not available" }
    end
    
    local bsp = NikNaks.CurrentMap
    if not bsp then
        MsgC(Color(255, 100, 100), "[RemixCategoryManager] Error: Could not load current map BSP!\n")
        return { error = "BSP not loaded" }
    end
    
    MsgC(Color(100, 200, 255), "[RemixCategoryManager] Smart-marking world textures...\n")
    MsgC(Color(200, 200, 200), "[RemixCategoryManager] BSP object: " .. tostring(bsp) .. "\n")
    MsgC(Color(200, 200, 200), "[RemixCategoryManager] Map name: " .. game.GetMap() .. "\n")
    
    -- Check if GetTextures exists
    if not bsp.GetTextures then
        MsgC(Color(255, 100, 100), "[RemixCategoryManager] Error: BSP has no GetTextures method!\n")
        return { error = "GetTextures not available" }
    end
    
    local ok, textures = pcall(function() return bsp:GetTextures() end)
    if not ok then
        MsgC(Color(255, 100, 100), "[RemixCategoryManager] Error calling GetTextures: " .. tostring(textures) .. "\n")
        return { error = "GetTextures failed: " .. tostring(textures) }
    end
    
    MsgC(Color(200, 200, 200), string.format("[RemixCategoryManager] Got %d textures from GetTextures()\n", textures and #textures or 0))
    
    -- If GetTextures returns nothing, try getting from faces instead
    if not textures or #textures == 0 then
        MsgC(Color(255, 200, 100), "[RemixCategoryManager] GetTextures() returned empty, trying GetFaces() instead...\n")
        
        local faces = bsp:GetFaces()
        if faces then
            MsgC(Color(200, 200, 200), string.format("[RemixCategoryManager] Got %d faces from BSP\n", #faces))
            
            -- Extract unique texture names from faces
            local textureSet = {}
            for _, face in pairs(faces) do
                if face then
                    -- Try GetMaterial first (returns IMaterial)
                    if face.GetMaterial then
                        local ok2, material = pcall(function() return face:GetMaterial() end)
                        if ok2 and material and material.GetName then
                            local matName = material:GetName()
                            if matName and matName ~= "" then
                                textureSet[matName] = true
                            end
                        end
                    end
                    -- Also try GetTexture (might return texture object or string)
                    if face.GetTexture then
                        local ok3, texture = pcall(function() return face:GetTexture() end)
                        if ok3 and texture then
                            -- Could be string or object
                            if type(texture) == "string" then
                                textureSet[texture] = true
                            elseif type(texture) == "table" and texture.name then
                                textureSet[texture.name] = true
                            end
                        end
                    end
                end
            end
            
            -- Convert set to array
            textures = {}
            for texName, _ in pairs(textureSet) do
                table.insert(textures, texName)
            end
            
            MsgC(Color(200, 200, 200), string.format("[RemixCategoryManager] Extracted %d unique textures from faces\n", #textures))
        end
    end
    
    -- Also get displacement textures (terrain surfaces)
    local displacementTextures = {}  -- Track which textures are from displacements
    MsgC(Color(200, 200, 200), "[RemixCategoryManager] Checking for displacements...\n")
    
    local okLeafs, allLeafs = pcall(function() return bsp:GetLeafs() end)
    MsgC(Color(200, 200, 200), string.format("[RemixCategoryManager] GetLeafs result: ok=%s, leafs=%s\n", 
        tostring(okLeafs), allLeafs and table.Count(allLeafs) or "nil"))
    
    if okLeafs and allLeafs then
        textures = textures or {}
        local dispCount = 0
        local leafCount = 0
        local faceCount = 0
        local dispFaceCount = 0
        
        for _, leaf in pairs(allLeafs) do
            leafCount = leafCount + 1
            if leaf then
                local okFaces, leafFaces = pcall(function() return leaf:GetFaces(true) end) -- include displacements
                if okFaces and leafFaces then
                    for _, face in pairs(leafFaces) do
                        faceCount = faceCount + 1
                        if face and face.IsDisplacement and face:IsDisplacement() then
                            dispFaceCount = dispFaceCount + 1
                            -- Get material from this displacement face
                            local ok_mat, material = pcall(function() return face:GetMaterial() end)
                            if ok_mat and material then
                                -- Get the ACTUAL material name being rendered by the engine
                                -- This is what gets tracked by D3D9 (e.g., "maps/.../blend_blacktop_01_wvt_patch")
                                local matName = material:GetName()
                                
                                if matName and matName ~= "" and not displacementTextures[matName] then
                                    displacementTextures[matName] = true
                                    dispCount = dispCount + 1
                                    
                                    -- Store normalized versions for matching
                                    local normalizedMatName = matName
                                    normalizedMatName = string.gsub(normalizedMatName, "^materials/", "")
                                    normalizedMatName = string.gsub(normalizedMatName, "%.vmt$", "")
                                    normalizedMatName = string.gsub(normalizedMatName, "\\", "/")  -- Normalize slashes
                                    displacementTextures[normalizedMatName] = true
                                    displacementTextures[string.lower(normalizedMatName)] = true
                                    
                                    -- Debug: Log first few
                                    if dispCount <= 5 then
                                        MsgC(Color(200, 200, 255), string.format("[RemixCategoryManager] Displacement #%d: material='%s'\n", dispCount, normalizedMatName))
                                        
                                        -- Also show the base textures for debug
                                        local baseTex = material:GetTexture("$basetexture")
                                        local baseTex2 = material:GetTexture("$basetexture2")
                                        if baseTex and baseTex.GetName then
                                            MsgC(Color(200, 200, 200), string.format("    $basetexture: %s\n", baseTex:GetName()))
                                        end
                                        if baseTex2 and baseTex2.GetName then
                                            MsgC(Color(200, 200, 200), string.format("    $basetexture2: %s\n", baseTex2:GetName()))
                                        end
                                    end
                                    
                                    -- Track the material (will be auto-tracked when rendered)
                                    RemixMaterial.TrackMaterial(normalizedMatName)
                                end
                            end
                        end
                    end
                end
            end
        end
        
        MsgC(Color(200, 200, 200), string.format("[RemixCategoryManager] Traversed %d leafs, %d faces, found %d displacement faces\n", 
            leafCount, faceCount, dispFaceCount))
        
        if dispCount > 0 then
            MsgC(Color(100, 255, 100), string.format("[RemixCategoryManager] Found %d unique displacement materials\n", dispCount))
            
            -- Debug: List all displacement material names (limited output)
            local debugCount = 0
            local debugList = {}
            for matName, _ in pairs(displacementTextures) do
                -- Only show actual material names (not texture names)
                if not string.find(matName, "^rtx_force_track") then
                    table.insert(debugList, matName)
                end
            end
            
            table.sort(debugList)
            for _, matName in ipairs(debugList) do
                if debugCount < 10 then
                    MsgC(Color(200, 200, 200), string.format("  - %s\n", matName))
                    debugCount = debugCount + 1
                end
            end
            
            if #debugList > 10 then
                MsgC(Color(200, 200, 200), string.format("  ... and %d more\n", #debugList - 10))
            end
        else
            MsgC(Color(200, 200, 200), "[RemixCategoryManager] No displacement materials found\n")
        end
    else
        MsgC(Color(255, 200, 100), "[RemixCategoryManager] Could not get leafs from BSP\n")
    end
    
    -- Track which materials are from BSP world geometry (these get DECAL_STATIC)
    local bspWorldTextures = {}
    for _, texName in ipairs(textures or {}) do
        bspWorldTextures[texName] = true
    end
    
    -- Track which materials are from displacements (these also get DECAL_STATIC)
    local bspDisplacementTextures = displacementTextures
    
    -- Add displacement materials to the main textures list for processing
    -- (They might not be in the BSP GetTextures() list)
    textures = textures or {}
    local addedDispCount = 0
    for dispMat, _ in pairs(displacementTextures) do
        -- Only add if not a texture name (avoid duplicates)
        if not string.find(dispMat, "^rtx_force_track") then
            local found = false
            for _, existing in ipairs(textures) do
                if existing == dispMat then
                    found = true
                    break
                end
            end
            
            if not found then
                table.insert(textures, dispMat)
                addedDispCount = addedDispCount + 1
            end
        end
    end
    
    if addedDispCount > 0 then
        MsgC(Color(100, 255, 100), string.format("[RemixCategoryManager] Added %d displacement materials to processing queue\n", addedDispCount))
    end
    
    -- Process Static Props (models placed in Hammer)
    -- These should NOT get DECAL_STATIC unless they have $decal parameter
    local propTextures = {}  -- Track prop materials separately
    MsgC(Color(200, 200, 200), "[RemixCategoryManager] Checking static props...\n")
    if bsp.GetStaticProps then
        local ok_props, staticProps = pcall(function() return bsp:GetStaticProps() end)
        if ok_props and staticProps then
            local propCount = 0
            local propMatCount = 0
            local processedModels = {}
            
            for _, prop in pairs(staticProps) do
                local modelName = prop.PropType
                if modelName and not processedModels[modelName] then
                    processedModels[modelName] = true
                    propCount = propCount + 1
                    
                    -- Get materials for this model
                    local matNames = {}
                    
                    if util.GetModelMaterials then
                        matNames = util.GetModelMaterials(modelName) or {}
                    elseif NikNaks and NikNaks.ModelMaterials then
                        local materials = NikNaks.ModelMaterials(modelName)
                        if materials then
                            for _, mat in pairs(materials) do
                                if mat and mat.GetName then
                                    table.insert(matNames, mat:GetName())
                                end
                            end
                        end
                    end
                    
                    for _, matName in ipairs(matNames) do
                        -- Mark as prop texture
                        propTextures[matName] = true
                        
                        -- Check if already in list to avoid duplicates
                        local found = false
                        for _, existing in ipairs(textures) do
                            if existing == matName then 
                                found = true 
                                break 
                            end
                        end
                        
                        if not found then
                            table.insert(textures, matName)
                            propMatCount = propMatCount + 1
                        end
                    end
                end
            end
            MsgC(Color(100, 255, 100), string.format("[RemixCategoryManager] Scanned %d unique static prop models, added %d new textures\n", propCount, propMatCount))
        else
             MsgC(Color(255, 200, 100), "[RemixCategoryManager] GetStaticProps returned error or nil\n")
        end
    else
        MsgC(Color(255, 200, 100), "[RemixCategoryManager] BSP does not support GetStaticProps\n")
    end
    
    local stats = {
        total = 0,
        solid = 0,
        transparent = 0,
        sky = 0,
        terrain = 0,
        displacements = 0,
        decals = 0,
        emissive = 0,
        props = 0,  -- Materials from static props (not categorized unless special)
        skipped = 0
    }
    
    if not textures then
        return { error = "Could not get textures from BSP" }
    end
    
    for i, texName in ipairs(textures) do
        if texName then
            stats.total = stats.total + 1
            
            -- Normalize material name - remove materials/ prefix and .vmt extension if present
            -- The C++ tracking expects just the material path like "dev/dev_measurecrate01"
            local materialName = texName
            materialName = string.gsub(materialName, "^materials/", "")
            materialName = string.gsub(materialName, "%.vmt$", "")
            
            local lowerName = string.lower(materialName)
            
            if not processedTextures[lowerName] then
                -- Determine if this is from BSP world geometry or a static prop
                local isFromBSP = bspWorldTextures[texName] or false
                local isFromProp = propTextures[texName] or false
                -- Check multiple variants for displacement detection
                local isFromDisplacement = bspDisplacementTextures[texName] or 
                                          bspDisplacementTextures[materialName] or 
                                          bspDisplacementTextures[lowerName] or
                                          false
                
                -- Determine category based on texture properties and source
                local category = nil
                
                -- Emissive/Self-illuminated materials (check first for priority)
                -- This is more reliable than keyword matching
                if RemixCategoryManager.IsMaterialEmissive(materialName) then
                    category = RemixCategoryManager.PRESET.EMISSIVE
                    stats.emissive = stats.emissive + 1
                
                -- Materials with $decal parameter (actual decals)
                elseif RemixCategoryManager.IsMaterialDecal(materialName) then
                    category = RemixCategoryManager.CATEGORY.DECAL_STATIC
                    stats.decals = stats.decals + 1
                    
                    -- Debug: Log first few decal categorizations
                    if stats.decals <= 3 then
                        MsgC(Color(100, 255, 100), string.format("[RemixCategoryManager] Categorizing decal: %s\n", materialName))
                    end
                
                -- Displacement textures (terrain) - always marked as DECAL_STATIC
                elseif isFromDisplacement then
                    category = RemixCategoryManager.PRESET.TERRAIN  -- This is DECAL_STATIC (0x1000)
                    stats.displacements = stats.displacements + 1
                    
                    -- Debug: Log first few displacement categorizations
                    if stats.displacements <= 3 then
                        MsgC(Color(100, 255, 100), string.format("[RemixCategoryManager] Categorizing displacement: %s\n", materialName))
                    end
                
                -- BSP world geometry textures (faces from brushes) - mark as DECAL_STATIC
                -- This is ONLY for actual world brushes, not props
                elseif isFromBSP and not isFromProp then
                    -- Always register with the C++ reverse hash map regardless of whether
                    -- this material receives a Remix category.  This protects translucent
                    -- world brushes (and any other excluded materials) from hash collisions
                    -- with particle/sprite effects at runtime without forcing DECAL_STATIC.
                    if RemixMaterial and RemixMaterial.RegisterBSPWorldMaterial then
                        RemixMaterial.RegisterBSPWorldMaterial(materialName)
                    end

                    -- Check if material should be excluded (translucent, no_decal, etc.)
                    if RemixCategoryManager.ShouldExcludeFromWorldGeometry(materialName) then
                        -- Leave uncategorized; hash-collision protection is handled by the
                        -- reverse map registered above.
                        stats.skipped = stats.skipped + 1
                    
                    -- Check for transparent world geometry (glass, fences, etc.)
                    elseif string.find(lowerName, "glass") or 
                       string.find(lowerName, "window") or
                       string.find(lowerName, "fence") or
                       string.find(lowerName, "grate") or
                       string.find(lowerName, "chain") then
                        category = RemixCategoryManager.PRESET.WORLD_GEOMETRY_TRANSPARENT
                        stats.transparent = stats.transparent + 1
                    
                    -- Check for terrain-like textures
                    elseif string.find(lowerName, "dirt") or
                           string.find(lowerName, "grass") or
                           string.find(lowerName, "ground") or
                           string.find(lowerName, "terrain") then
                        category = RemixCategoryManager.PRESET.TERRAIN
                        stats.terrain = stats.terrain + 1
                    
                    -- Default: solid world geometry (DECAL_STATIC)
                    else
                        category = RemixCategoryManager.PRESET.WORLD_GEOMETRY
                        stats.solid = stats.solid + 1
                    end
                
                -- Static prop materials - NO DECAL_STATIC unless they have $decal parameter
                -- These are already handled by emissive/decal checks above
                -- Leave uncategorized (category = nil) so they render as normal models
                elseif isFromProp then
                    stats.props = stats.props + 1
                end
                
                if category then
                    local forceAlbedoCallback =
                        MakeForceAlbedoCallback(materialName, category)
                    
                    -- If SetMaterialCategory succeeds immediately (hash available), mark as
                    -- processed now. If it returns false the entry is in pendingCategorizations
                    -- and the Think hook will mark processedTextures on success.
                    if RemixCategoryManager.SetMaterialCategory(materialName, category, forceAlbedoCallback) then
                        processedTextures[lowerName] = true
                    end
                else
                    -- No category assigned (excluded/prop/skipped) - mark as processed so we
                    -- don't re-evaluate this material on subsequent SmartMark calls.
                    processedTextures[lowerName] = true
                end
                
                -- If this is a displacement material, also ensure it's tracked
                if isFromDisplacement and category then
                    RemixMaterial.TrackMaterial(materialName)
                end
            else
                stats.skipped = stats.skipped + 1
            end
        end
    end
    
    MsgC(Color(100, 255, 100), "[RemixCategoryManager] Smart-mark complete:\n")
    MsgC(Color(200, 200, 200), string.format("  Total: %d, Solid: %d, Emissive: %d, Transparent: %d, Sky: %d, Terrain: %d, Decals: %d, Props: %d, Skipped: %d\n",
        stats.total, stats.solid, stats.emissive, stats.transparent, stats.sky, stats.terrain, stats.decals, stats.props, stats.skipped))
    
    -- Also scan all tracked/rendered materials to catch overlay decals, effects, etc.
    MsgC(Color(200, 200, 200), "[RemixCategoryManager] Scanning tracked materials for decals, particles, etc...\n")
    local trackedStats = RemixCategoryManager.CategorizeAllTrackedMaterials()
    
    return stats
end

--[[
    Console command to mark all world textures
]]--
concommand.Add("rtx_mark_world_textures", function(ply, cmd, args)
    local categoryFlags = RemixCategoryManager.PRESET.WORLD_GEOMETRY
    
    if args[1] then
        categoryFlags = tonumber(args[1])
        if not categoryFlags then
            MsgC(Color(255, 100, 100), "[RemixCategoryManager] Invalid category flags: " .. args[1] .. "\n")
            return
        end
    end
    
    RemixCategoryManager.MarkWorldTextures(categoryFlags)
end, nil, "Mark all world geometry textures with a category (default: DECAL_STATIC)")

--[[
    Console command to smart-mark world textures
]]--
concommand.Add("rtx_smart_mark_world", function(ply, cmd, args)
    -- Optionally clear the processed cache to reprocess everything
    if args[1] == "force" then
        processedTextures = {}
        MsgC(Color(255, 200, 100), "[RemixCategoryManager] Cleared processed texture cache\n")
    end
    
    RemixCategoryManager.SmartMarkWorldTextures()
end, nil, "Intelligently mark world textures based on their properties (use 'force' to reprocess all)")

--[[
    Console command to clear all category mappings
]]--
concommand.Add("rtx_clear_categories", function(ply, cmd, args)
    RemixCategoryManager.ClearAllCategories()
    MsgC(Color(100, 255, 100), "[RemixCategoryManager] All category mappings cleared\n")
end, nil, "Clear all hash-to-category mappings")

--[[
    Console command to get texture hash of the surface the player is currently looking at.
    Useful for cross-referencing what Remix reports vs what the Lua tracker has.
]]--
concommand.Add("rtx_look_hash", function(ply, cmd, args)
    local trace = LocalPlayer():GetEyeTrace()
    if not trace.Hit then
        MsgC(Color(255, 100, 100), "[RTX Look Hash] Not looking at a surface\n")
        return
    end

    local texName = trace.HitTexture
    if not texName or texName == "" then
        MsgC(Color(255, 100, 100), "[RTX Look Hash] Could not read surface texture\n")
        return
    end

    -- GMod returns "**displacement**" for displacement surfaces instead of the material name.
    -- Fall back to a NikNaks BSP trace to get the actual face and its material.
    if texName == "**displacement**" then
        if not NikNaks or not NikNaks.CurrentMap then
            MsgC(Color(255, 100, 100), "[RTX Look Hash] Hit a displacement but NikNaks is not available\n")
            return
        end
        local eyePos = LocalPlayer():EyePos()
        local endPos = trace.HitPos + LocalPlayer():GetAimVector() * 5
        local bspTrace = NikNaks.CurrentMap:SurfaceTraceLine(eyePos, endPos)
        if not bspTrace.Hit or not bspTrace.Face then
            MsgC(Color(255, 100, 100), "[RTX Look Hash] NikNaks BSP trace did not hit a face\n")
            return
        end
        local mat = bspTrace.Face:GetMaterial()
        if not mat then
            MsgC(Color(255, 100, 100), "[RTX Look Hash] Could not get material from BSP face\n")
            return
        end
        texName = mat:GetName()
        MsgC(Color(200, 200, 200), "[RTX Look Hash] (displacement — resolved via NikNaks BSP trace)\n")
    end

    -- Normalize to the form the tracker uses (no materials/ prefix, no .vmt, no maps/<map>/ prefix, no _wvt_patch)
    local matName = texName
    matName = matName:gsub("^materials/", "")
    matName = matName:gsub("%.vmt$", "")
    matName = matName:gsub("^maps/[^/]+/", "")
    matName = matName:gsub("_wvt_patch$", "")

    MsgC(Color(100, 200, 255), "[RTX Look Hash] Surface: ")
    MsgC(Color(255, 255, 255), matName .. "\n")
    MsgC(Color(200, 200, 200), string.format("  Hit pos: (%.1f, %.1f, %.1f)  Entity: %s\n",
        trace.HitPos.x, trace.HitPos.y, trace.HitPos.z,
        IsValid(trace.Entity) and trace.Entity:GetClass() or "world"))

    if not RemixMaterial then
        MsgC(Color(255, 100, 100), "[RTX Look Hash] RemixMaterial API not available\n")
        return
    end

    -- Human-readable category flag names
    local categoryNames = {
        [0x1]       = "WORLD_UI",
        [0x2]       = "WORLD_MATTE",
        [0x4]       = "SKY",
        [0x8]       = "IGNORE",
        [0x400]     = "PARTICLE",
        [0x800]     = "BEAM",
        [0x1000]    = "DECAL_STATIC",
        [0x2000]    = "DECAL_DYNAMIC",
        [0x40000]   = "ANIMATED_WATER",
        [0x1000000] = "LEGACY_EMISSIVE",
    }
    local function categoryStr(flags)
        if not flags or flags == 0 then return "none" end
        local names = {}
        for flag, name in pairs(categoryNames) do
            if bit.band(flags, flag) ~= 0 then
                table.insert(names, name)
            end
        end
        return #names > 0 and table.concat(names, " | ") or string.format("0x%X", flags)
    end

    local function printHashes(label, name)
        local hashes = RemixMaterial.GetAllTextureHashes and RemixMaterial.GetAllTextureHashes(name)
        if not hashes or #hashes == 0 then return false end
        MsgC(Color(100, 255, 100), string.format("  %s (%d hash%s):\n", label, #hashes, #hashes == 1 and "" or "es"))
        for _, hashStr in ipairs(hashes) do
            local cat = RemixMaterial.GetHashCategory and RemixMaterial.GetHashCategory(hashStr)
            MsgC(Color(200, 255, 200), string.format("    %s", hashStr))
            MsgC(Color(180, 180, 180), string.format("  category: %s\n", categoryStr(cat)))
        end
        return true
    end

    local foundAny = false
    foundAny = printHashes(matName, matName) or foundAny
    foundAny = printHashes(matName .. " (_stage1)", matName .. "_stage1") or foundAny

    if not foundAny then
        MsgC(Color(255, 200, 100), "  No hashes found — material may not have been rendered yet\n")
        -- Show whether it's in the pending queue
        local inQueue = false
        for _, pending in ipairs(pendingCategorizations) do
            if pending.materialName:lower() == matName:lower() then
                inQueue = true
                break
            end
        end
        if inQueue then
            MsgC(Color(255, 200, 100), "  (queued in pendingCategorizations — will apply when rendered)\n")
        end
    end
end, nil, "Get the texture hash of the surface the player is looking at")

--[[
    Console command to manually set a category for a texture hash.
    Usage: rtx_set_hash_category <hash> <category_flags_hex>
    The hash can be the Remix-reported hex string (with or without 0x prefix).
    Category flags: DECAL_STATIC=0x1000  PARTICLE=0x400  LEGACY_EMISSIVE=0x1000000
                    SKY=0x4  IGNORE=0x8  ANIMATED_WATER=0x40000
    Example: rtx_set_hash_category 44EBDE4ABC8229DD 0x1000
]]--
concommand.Add("rtx_set_hash_category", function(ply, cmd, args)
    if not args[1] or not args[2] then
        MsgC(Color(255, 200, 100), "Usage: rtx_set_hash_category <hash> <category_flags_hex>\n")
        MsgC(Color(255, 200, 100), "  hash:  hex string, e.g. 44EBDE4ABC8229DD or 0x44EBDE4ABC8229DD\n")
        MsgC(Color(255, 200, 100), "  flags: 0x1000=DECAL_STATIC  0x400=PARTICLE  0x1000000=EMISSIVE\n")
        MsgC(Color(255, 200, 100), "         0x4=SKY  0x8=IGNORE  0x40000=ANIMATED_WATER\n")
        return
    end

    -- Accept hashes with or without 0x prefix
    local hashStr = args[1]:upper():gsub("^0X", "")
    hashStr = "0x" .. hashStr

    local flags = tonumber(args[2])
    if not flags then
        MsgC(Color(255, 100, 100), string.format("[RTX Set Hash] Invalid flags '%s' — must be a hex number like 0x1000\n", args[2]))
        return
    end

    if not RemixMaterial then
        MsgC(Color(255, 100, 100), "[RTX Set Hash] RemixMaterial API not available\n")
        return
    end

    local success = RemixCategoryManager.SetHashCategory(hashStr, flags)

    -- Human-readable flag names (reuse the table from rtx_look_hash)
    local names = {}
    local flagMap = {
        [0x4]       = "SKY",
        [0x8]       = "IGNORE",
        [0x400]     = "PARTICLE",
        [0x1000]    = "DECAL_STATIC",
        [0x40000]   = "ANIMATED_WATER",
        [0x1000000] = "LEGACY_EMISSIVE",
    }
    for flag, name in pairs(flagMap) do
        if bit.band(flags, flag) ~= 0 then table.insert(names, name) end
    end
    local flagLabel = #names > 0 and table.concat(names, " | ") or string.format("0x%X", flags)

    if success ~= false then
        MsgC(Color(100, 255, 100), string.format("[RTX Set Hash] Set %s → %s\n", hashStr, flagLabel))
    else
        MsgC(Color(255, 100, 100), string.format("[RTX Set Hash] SetHashCategory returned false for %s\n", hashStr))
    end
end, nil, "Manually set category flags for a texture hash (usage: rtx_set_hash_category <hash> <flags_hex>)")

--[[
    Console command to search for texture hashes by name
]]--
concommand.Add("rtx_find_texture_hash", function(ply, cmd, args)
    if not args[1] then
        MsgC(Color(255, 200, 100), "Usage: remix_find_texture_hash <texture_name>\n")
        MsgC(Color(255, 200, 100), "Example: remix_find_texture_hash grass1\n")
        return
    end
    
    local searchName = string.lower(args[1])
    local found = RemixMaterial.FindTexturesByName(searchName)
    
    if found and #found > 0 then
        MsgC(Color(100, 255, 100), string.format("Found %d materials matching '%s':\n", #found, searchName))
        for _, entry in ipairs(found) do
            -- Get the actual hash from D3D9 tracker (if material was rendered)
            local hash, hashStr = RemixMaterial.GetTextureHash(entry.name)
            if hash and hash > 0 then
                MsgC(Color(200, 200, 200), string.format("  %s -> %s\n", entry.name, hashStr or string.format("0x%X", hash)))
            else
                MsgC(Color(150, 150, 150), string.format("  %s -> NOT RENDERED YET\n", entry.name))
            end
        end
    else
        MsgC(Color(255, 100, 100), string.format("No materials found matching '%s'\n", searchName))
    end
end, nil, "Search for texture hashes by partial name match")

--[[
    Console command to set category for a specific material
]]--
concommand.Add("rtx_set_material_category", function(ply, cmd, args)
    if not args[1] or not args[2] then
        MsgC(Color(255, 200, 100), "Usage: remix_set_material_category <material_name> <category_flags_hex>\n")
        MsgC(Color(255, 200, 100), "Example: remix_set_material_category materials/concrete/concrete.vmt 0x800\n")
        return
    end
    
    local materialName = args[1]
    local categoryFlags = tonumber(args[2])
    
    if not categoryFlags then
        MsgC(Color(255, 100, 100), "[RemixCategoryManager] Invalid category flags: " .. args[2] .. "\n")
        return
    end
    
    RemixCategoryManager.SetMaterialCategory(materialName, categoryFlags)
end, nil, "Set category for a specific material")

--[[
    Console command to check if a material is emissive
]]--
concommand.Add("rtx_check_emissive", function(ply, cmd, args)
    if not args[1] then
        MsgC(Color(255, 200, 100), "Usage: remix_check_emissive <material_name>\n")
        MsgC(Color(255, 200, 100), "Example: remix_check_emissive models/props_c17/industrialbellbottomon01\n")
        return
    end
    
    local materialName = args[1]
    local isEmissive = RemixCategoryManager.IsMaterialEmissive(materialName)
    
    local requireMask = GetConVar("rtx_require_emissive_mask"):GetBool()
    
    if isEmissive then
        MsgC(Color(100, 255, 100), string.format("[RemixCategoryManager] ✓ '%s' is EMISSIVE\n", materialName))
    else
        MsgC(Color(255, 100, 100), string.format("[RemixCategoryManager] ✗ '%s' is NOT emissive", materialName))
        if requireMask then
            MsgC(Color(255, 200, 100), " (strict mode: requires mask)\n")
        else
            MsgC(Color(255, 200, 100), "\n")
        end
    end
    
    MsgC(Color(200, 200, 200), string.format("  Mask requirement: %s (remix_require_emissive_mask)\n", 
        requireMask and "STRICT" or "PERMISSIVE"))

    -- Show detailed debug info
    local mat = Material(materialName)
    if mat and not mat:IsError() then
        MsgC(Color(200, 200, 200), "  Debug Info:\n")
        MsgC(Color(200, 200, 200), string.format("  - Shader: %s\n", tostring(mat:GetShader())))
        
        local intVal = mat:GetInt("$selfillum")
        if intVal and intVal == 1 then
            MsgC(Color(100, 255, 100), string.format("  - GetInt($selfillum): %s ✓\n", tostring(intVal)))
        else
            MsgC(Color(200, 200, 200), string.format("  - GetInt($selfillum): %s\n", tostring(intVal or "nil")))
        end
        
        -- Check for mask
        local mask = mat:GetTexture("$selfillummask")
        if mask and not mask:IsError() then
            MsgC(Color(100, 255, 100), string.format("  - $selfillummask: %s ✓\n", mask:GetName()))
        else
            MsgC(Color(255, 200, 100), "  - $selfillummask: (none) ⚠\n")
        end
        
        -- Check for alpha channel indicators
        local translucent = mat:GetInt("$translucent")
        local alphatest = mat:GetInt("$alphatest")
        if (translucent and translucent == 1) or (alphatest and alphatest == 1) then
            MsgC(Color(100, 255, 100), string.format("  - Alpha masking: $translucent=%s $alphatest=%s ✓\n", 
                tostring(translucent), tostring(alphatest)))
        else
            MsgC(Color(255, 200, 100), string.format("  - Alpha masking: $translucent=%s $alphatest=%s ⚠\n", 
                tostring(translucent or 0), tostring(alphatest or 0)))
        end
        
        local floatVal = mat:GetFloat("$selfillum")
        MsgC(Color(200, 200, 200), string.format("  - GetFloat($selfillum): %s\n", tostring(floatVal or "nil")))
        
        local vec = mat:GetVector("$emissive")
        if vec then MsgC(Color(200, 200, 200), string.format("  - GetVector($emissive): [%.2f, %.2f, %.2f]\n", vec.x, vec.y, vec.z)) end
        
        local keyValues = mat:GetKeyValues()
        if keyValues then
            MsgC(Color(200, 200, 200), "  - KeyValues (VMT raw):\n")
            if keyValues["$selfillum"] then
                MsgC(Color(100, 255, 100), string.format("    - $selfillum: %s\n", tostring(keyValues["$selfillum"])))
            else
                MsgC(Color(255, 100, 100), "    - $selfillum: (missing)\n")
            end
            if keyValues["$selfillummask"] then
                MsgC(Color(100, 255, 100), string.format("    - $selfillummask: %s\n", tostring(keyValues["$selfillummask"])))
            end
            -- Print other relevant keys
            for k, v in pairs(keyValues) do
                if k:lower():find("illum") or k:lower():find("emiss") or k:lower():find("alpha") or k:lower():find("transluc") then
                    if not (k:lower() == "$selfillum" or k:lower() == "$selfillummask") then
                        MsgC(Color(200, 200, 200), string.format("    - %s: %s\n", k, tostring(v)))
                    end
                end
            end
        else
             MsgC(Color(255, 100, 100), "  - KeyValues: nil (Failed to read VMT)\n")
        end
        
        -- Check file directly
        local vmtPath = "materials/" .. materialName
        if not string.EndsWith(vmtPath, ".vmt") then vmtPath = vmtPath .. ".vmt" end
        local content = file.Read(vmtPath, "GAME")
        if content then
            MsgC(Color(200, 200, 200), "  - Raw File Parse:\n")
            local lowerContent = content:lower()
            
            if lowerContent:find("%$selfillum") then
                MsgC(Color(100, 255, 100), "    - Found '$selfillum' string in file!\n")
                
                -- Parse line-by-line to check if it's commented
                local lines = string.Explode("\n", content)
                local foundActive = false
                local foundCommented = false
                
                for _, line in ipairs(lines) do
                    local lowerLine = line:lower()
                    if lowerLine:find("%$selfillum") then
                        local trimmedLine = string.Trim(lowerLine)
                        if string.StartsWith(trimmedLine, "//") then
                            foundCommented = true
                            MsgC(Color(255, 200, 100), "    - Found COMMENTED OUT: " .. string.Trim(line) .. "\n")
                        elseif lowerLine:find('["\']?%$selfillum["\']?%s+["\']?1["\']?') then
                            foundActive = true
                            MsgC(Color(100, 255, 100), "    - Found ACTIVE '$selfillum 1': " .. string.Trim(line) .. "\n")
                        end
                    end
                end
                
                if not foundActive and not foundCommented then
                    MsgC(Color(255, 200, 100), "    - '$selfillum' found but not set to 1 (or pattern failed)\n")
                elseif not foundActive and foundCommented then
                    MsgC(Color(255, 150, 100), "    - '$selfillum' is COMMENTED OUT (not emissive)\n")
                end
            else
                MsgC(Color(200, 200, 200), "    - '$selfillum' string NOT found in file.\n")
            end
        else
            MsgC(Color(255, 100, 100), "  - Raw File Parse: File not found (" .. vmtPath .. ")\n")
        end
    else
        MsgC(Color(255, 100, 100), "  - Invalid Material\n")
    end
end, nil, "Check if a material has $selfillum enabled")

--[[
    Console command to debug emissive detection
]]--
concommand.Add("rtx_debug_emissive", function(ply, cmd, args)
    if not args[1] then
        MsgC(Color(255, 200, 100), "Usage: remix_debug_emissive <material_name>\n")
        MsgC(Color(255, 200, 100), "Example: remix_debug_emissive decals/decalstain005a\n")
        return
    end
    
    local materialName = args[1]
    MsgC(Color(100, 200, 255), string.format("[Emissive Debug] Checking: %s\n", materialName))
    
    local mat = Material(materialName)
    if not mat or mat:IsError() then
        MsgC(Color(255, 100, 100), "  ✗ Material is invalid or error\n")
        return
    end
    
    local reasons = {}
    
    -- Check Shader
    local shader = mat:GetShader()
    MsgC(Color(200, 200, 200), string.format("  Shader: %s\n", tostring(shader)))
    local lowerShader = shader and shader:lower() or ""
    local isUnlit = lowerShader:find("^unlitgeneric") ~= nil or
                    lowerShader:find("^unlittwotexture") ~= nil
    local hasNoFullbright = HasNoFullbrightOptOut(materialName, mat)
    local hasVertexOptOut = HasVertexColorOrAlphaOptOut(materialName, mat)
    MsgC(Color(200, 200, 200), string.format(
        "  $no_fullbright opt-out: %s\n", tostring(hasNoFullbright)))
    MsgC(Color(200, 200, 200), string.format(
        "  $vertexalpha/$vertexcolor opt-out: %s\n", tostring(hasVertexOptOut)))
    if isUnlit and (hasNoFullbright or hasVertexOptOut) then
        if hasNoFullbright then
            table.insert(reasons, "$no_fullbright 1 suppresses unlit emission")
        end
        if hasVertexOptOut then
            table.insert(reasons,
                "$vertexalpha 1 or $vertexcolor 1 suppresses unlit emission")
        end
    elseif isUnlit then
        table.insert(reasons, "Unlit shader")
    end
    
    -- Check GetInt
    local selfillumInt = mat:GetInt("$selfillum")
    MsgC(Color(200, 200, 200), string.format("  GetInt($selfillum): %s\n", tostring(selfillumInt)))
    if selfillumInt and selfillumInt == 1 then
        table.insert(reasons, "GetInt($selfillum) = 1")
    end
    
    -- Show what IsMaterialEmissive returns
    local luaResult = RemixCategoryManager.IsMaterialEmissive(materialName)
    MsgC(Color(200, 200, 200), string.format("\n  IsMaterialEmissive() returns: %s\n", tostring(luaResult)))
    
    if #reasons > 0 then
        MsgC(Color(255, 150, 100), "  Reasons detected:\n")
        for _, reason in ipairs(reasons) do
            MsgC(Color(255, 150, 100), string.format("    - %s\n", reason))
        end
    end
end, nil, "Debug why a material is being marked as emissive")

--[[
    Console command to check if a material is a decal
]]--
concommand.Add("rtx_check_decal", function(ply, cmd, args)
    if not args[1] then
        MsgC(Color(255, 200, 100), "Usage: remix_check_decal <material_name>\n")
        MsgC(Color(255, 200, 100), "Example: remix_check_decal decals/decalgraffiti036a\n")
        return
    end
    
    local materialName = args[1]
    local isDecal = RemixCategoryManager.IsMaterialDecal(materialName)
    
    if isDecal then
        MsgC(Color(100, 255, 100), string.format("[RemixCategoryManager] ✓ '%s' is a DECAL\n", materialName))
    else
        MsgC(Color(255, 100, 100), string.format("[RemixCategoryManager] ✗ '%s' is NOT a decal\n", materialName))
    end

    -- Show detailed debug info
    local mat = Material(materialName)
    if mat and not mat:IsError() then
        MsgC(Color(200, 200, 200), "  Debug Info:\n")
        MsgC(Color(200, 200, 200), string.format("  - Shader: %s\n", tostring(mat:GetShader())))
        
        local intVal = mat:GetInt("$decal")
        MsgC(Color(200, 200, 200), string.format("  - GetInt($decal): %s\n", tostring(intVal or "nil")))
        
        local floatVal = mat:GetFloat("$decal")
        MsgC(Color(200, 200, 200), string.format("  - GetFloat($decal): %s\n", tostring(floatVal or "nil")))
        
        local keyValues = mat:GetKeyValues()
        if keyValues then
            MsgC(Color(200, 200, 200), "  - KeyValues (VMT raw):\n")
            if keyValues["$decal"] then
                MsgC(Color(100, 255, 100), string.format("    - $decal: %s\n", tostring(keyValues["$decal"])))
            else
                MsgC(Color(255, 100, 100), "    - $decal: (missing)\n")
            end
        else
             MsgC(Color(255, 100, 100), "  - KeyValues: nil (Failed to read VMT)\n")
        end
        
        -- Check file directly
        local vmtPath = "materials/" .. materialName
        if not string.EndsWith(vmtPath, ".vmt") then vmtPath = vmtPath .. ".vmt" end
        local content = file.Read(vmtPath, "GAME")
        if content then
            MsgC(Color(200, 200, 200), "  - Raw File Parse:\n")
            local lowerContent = content:lower()
            
            if lowerContent:find("%$decal") then
                MsgC(Color(100, 255, 100), "    - Found '$decal' string in file!\n")
                
                -- Parse line-by-line to check if it's commented
                local lines = string.Explode("\n", content)
                local foundActive = false
                local foundCommented = false
                
                for _, line in ipairs(lines) do
                    local lowerLine = line:lower()
                    if lowerLine:find("%$decal") then
                        local trimmedLine = string.Trim(lowerLine)
                        if string.StartsWith(trimmedLine, "//") then
                            foundCommented = true
                            MsgC(Color(255, 200, 100), "    - Found COMMENTED OUT: " .. string.Trim(line) .. "\n")
                        elseif lowerLine:find('["\']?%$decal["\']?%s+["\']?1["\']?') then
                            foundActive = true
                            MsgC(Color(100, 255, 100), "    - Found ACTIVE '$decal 1': " .. string.Trim(line) .. "\n")
                        end
                    end
                end
                
                if not foundActive and not foundCommented then
                    MsgC(Color(255, 200, 100), "    - '$decal' found but not set to 1 (or pattern failed)\n")
                elseif not foundActive and foundCommented then
                    MsgC(Color(255, 150, 100), "    - '$decal' is COMMENTED OUT (not a decal)\n")
                end
            else
                MsgC(Color(200, 200, 200), "    - '$decal' string NOT found in file.\n")
            end
        else
            MsgC(Color(255, 100, 100), "  - Raw File Parse: File not found (" .. vmtPath .. ")\n")
        end
    else
        MsgC(Color(255, 100, 100), "  - Invalid Material\n")
    end
end, nil, "Check if a material has $decal enabled")

--[[
    Console command to check if a material should be excluded from world geometry
]]--
concommand.Add("rtx_check_world_exclude", function(ply, cmd, args)
    if not args[1] then
        MsgC(Color(255, 200, 100), "Usage: remix_check_world_exclude <material_name>\n")
        MsgC(Color(255, 200, 100), "Example: remix_check_world_exclude nature/miltree007\n")
        return
    end
    
    local materialName = args[1]
    local shouldExclude = RemixCategoryManager.ShouldExcludeFromWorldGeometry(materialName)
    
    if shouldExclude then
        MsgC(Color(255, 200, 100), string.format("[RemixCategoryManager] ✓ '%s' EXCLUDED from world geometry (no DECAL_STATIC)\n", materialName))
    else
        MsgC(Color(100, 255, 100), string.format("[RemixCategoryManager] ✗ '%s' can be world geometry\n", materialName))
    end

    -- Show detailed debug info
    local mat = Material(materialName)
    if mat and not mat:IsError() then
        MsgC(Color(200, 200, 200), "  Debug Info:\n")
        MsgC(Color(200, 200, 200), string.format("  - Shader: %s\n", tostring(mat:GetShader())))
        
        local translucent = mat:GetInt("$translucent")
        if translucent and translucent == 1 then
            MsgC(Color(255, 200, 100), "  - GetInt($translucent): 1 (EXCLUDED)\n")
        else
            MsgC(Color(200, 200, 200), string.format("  - GetInt($translucent): %s\n", tostring(translucent or "nil")))
        end
        
        local surfaceprop = mat:GetString("$surfaceprop")
        if surfaceprop and surfaceprop:lower():find("no_decal") then
            MsgC(Color(255, 200, 100), string.format("  - GetString($surfaceprop): %s (EXCLUDED)\n", surfaceprop))
        else
            MsgC(Color(200, 200, 200), string.format("  - GetString($surfaceprop): %s\n", tostring(surfaceprop or "nil")))
        end
        
        local keyValues = mat:GetKeyValues()
        if keyValues then
            MsgC(Color(200, 200, 200), "  - KeyValues (VMT raw):\n")
            for k, v in pairs(keyValues) do
                local lowerKey = k:lower()
                if lowerKey == "$translucent" or lowerKey == "$surfaceprop" then
                    local val = tostring(v)
                    if (lowerKey == "$translucent" and tonumber(v) == 1) or 
                       (lowerKey == "$surfaceprop" and val:lower():find("no_decal")) then
                        MsgC(Color(255, 200, 100), string.format("    - %s: %s (EXCLUDED)\n", k, val))
                    else
                        MsgC(Color(200, 200, 200), string.format("    - %s: %s\n", k, val))
                    end
                end
            end
        else
             MsgC(Color(255, 100, 100), "  - KeyValues: nil (Failed to read VMT)\n")
        end
        
        -- Check file directly
        local vmtPath = "materials/" .. materialName
        if not string.EndsWith(vmtPath, ".vmt") then vmtPath = vmtPath .. ".vmt" end
        local content = file.Read(vmtPath, "GAME")
        if content then
            MsgC(Color(200, 200, 200), "  - Raw File Parse:\n")
            
            local lines = string.Explode("\n", content)
            for _, line in ipairs(lines) do
                local lowerLine = line:lower()
                local trimmedLine = string.Trim(lowerLine)
                
                if not string.StartsWith(trimmedLine, "//") then
                    if lowerLine:find("%$translucent") or lowerLine:find("%$surfaceprop") then
                        local isExcluded = (lowerLine:find('["\']?%$translucent["\']?%s+["\']?1["\']?') or 
                                          (lowerLine:find('%$surfaceprop') and lowerLine:find('no_decal')))
                        
                        if isExcluded then
                            MsgC(Color(255, 200, 100), "    - " .. string.Trim(line) .. " (EXCLUDED)\n")
                        else
                            MsgC(Color(200, 200, 200), "    - " .. string.Trim(line) .. "\n")
                        end
                    end
                end
            end
        else
            MsgC(Color(255, 100, 100), "  - Raw File Parse: File not found (" .. vmtPath .. ")\n")
        end
    else
        MsgC(Color(255, 100, 100), "  - Invalid Material\n")
    end
end, nil, "Check if a material should be excluded from world geometry (translucent, no_decal)")

--[[
    Console command to categorize all tracked materials
]]--
concommand.Add("rtx_categorize_tracked", function(ply, cmd, args)
    RemixCategoryManager.CategorizeAllTrackedMaterials()
end, nil, "Categorize all materials currently tracked by the renderer")


-- Function to initialize C++ module flags from ConVars
local function InitializeCppModuleFlags()
    if not RemixMaterial then return end
    
    -- Set master auto-categorization flag
    if RemixMaterial.SetAutoCategorization then
        local masterEnabled = GetConVar("rtx_auto_categorize"):GetBool()
        RemixMaterial.SetAutoCategorization(masterEnabled)
    end
    
    -- Set particle categorization flag
    if RemixMaterial.SetParticleCategorization then
        local particlesEnabled = GetConVar("rtx_auto_categorize_particles"):GetBool()
        RemixMaterial.SetParticleCategorization(particlesEnabled)
    end
    
    -- Set decal categorization flag
    if RemixMaterial.SetDecalCategorization then
        local decalsEnabled = GetConVar("rtx_auto_categorize_decals"):GetBool()
        RemixMaterial.SetDecalCategorization(decalsEnabled)
    end
    
    -- Set emissive categorization flag
    if RemixMaterial.SetEmissiveCategorization then
        local emissiveEnabled = GetConVar("rtx_auto_categorize_emissive"):GetBool()
        RemixMaterial.SetEmissiveCategorization(emissiveEnabled)
    end
    
    -- Set debug output flag
    if RemixMaterial.SetDebugOutput then
        local debugEnabled = GetConVar("rtx_debug_categorization"):GetBool()
        RemixMaterial.SetDebugOutput(debugEnabled)
    end
end

-- Auto-initialize on player spawn (client-side)
-- NEW APPROACH: Parse BSP once, send world texture list to C++ for real-time categorization
-- C++ will automatically categorize textures as they render (single-pass, no delays!)
local autoInitRan = false
local function AutoInitFunction()
    -- Only run once per map load
    if autoInitRan then return end
    autoInitRan = true
    hook.Remove("HUDPaint", "RemixCategoryManager_AutoInit")
    
    -- Check if auto-categorization is enabled
    if not GetConVar("rtx_auto_categorize"):GetBool() then
        MsgC(Color(200, 200, 200), "[RemixCategoryManager] Auto-categorization disabled (remix_auto_categorize = 0)\n")
        MsgC(Color(255, 200, 100), "[RemixCategoryManager] Tip: Use 'remix_smart_mark_world' to manually categorize textures\n")
        return
    end
    
    -- Get delay from ConVar (default 5 seconds to ensure NikNaks is ready)
    local delay = GetConVar("rtx_auto_categorize_delay"):GetFloat()
    
    MsgC(Color(200, 200, 200), string.format("[RemixCategoryManager] Will parse BSP and send world textures to C++ in %.1f seconds...\n", delay))
    
    -- Wait for NikNaks and textures to be ready, then run full categorization
    timer.Simple(delay, function()
        if not GetConVar("rtx_auto_categorize"):GetBool() then
            MsgC(Color(255, 200, 100), "[RemixCategoryManager] Auto-categorization disabled, skipping\n")
            return
        end
        
        MsgC(Color(100, 200, 255), "[RemixCategoryManager] Running auto-categorization (SmartMarkWorldTextures)...\n")
        
        -- Clear processed cache so everything gets categorized fresh
        processedTextures = {}
        
        -- Run the same full categorization that "Scan Now" uses
        RemixCategoryManager.SmartMarkWorldTextures()
    end)
end

--[[
    Drain the pending categorization queue.
    Called every frame via Think. For each entry, attempt to get the texture hash now
    that the material may have been rendered. On success, apply the category and mark
    processedTextures. Entries with still-unavailable hashes stay in the queue.
]]--
-- How long to keep watching a material after its first hash is found, to catch
-- additional variants that render a few frames later (e.g. _stage1 blend displacements
-- with multiple UV instances sharing the same material but different hashes).
local PENDING_WATCH_SECONDS = 10

local function DrainPendingCategorizations()
    if #pendingCategorizations == 0 then return end

    local remaining = {}
    local now = CurTime()
    for _, pending in ipairs(pendingCategorizations) do
        local foundAnyHash = false

        -- callbackFiredForHashes tracks which hashes in this pending entry have already
        -- had their callback invoked, so the callback fires exactly once per hash even
        -- across multiple Think ticks while the entry stays in the watch window.
        if not pending.callbackFiredForHashes then
            pending.callbackFiredForHashes = {}
        end
        if not pending.categoryAttemptedForHashes then
            pending.categoryAttemptedForHashes = {}
        end

        for _, tryName in ipairs(pending.materialsToTry) do
            local allHashes = RemixMaterial.GetAllTextureHashes and RemixMaterial.GetAllTextureHashes(tryName)
            if allHashes and #allHashes > 0 then
                for _, hashStr in ipairs(allHashes) do
                    local existing = RemixMaterial.GetHashCategory and
                                     RemixMaterial.GetHashCategory(hashStr) or 0
                    local mergedFlags = bit.bor(existing, pending.categoryFlags)
                    if mergedFlags ~= existing and
                       not pending.categoryAttemptedForHashes[hashStr] then
                        pending.categoryAttemptedForHashes[hashStr] = true
                        local success =
                            RemixCategoryManager.SetHashCategory(hashStr, mergedFlags)
                        if pending.callback and not pending.callbackFiredForHashes[hashStr] then
                            pending.callbackFiredForHashes[hashStr] = true
                            pending.callback(success, hashStr)
                        end
                        -- Reset the watch window each time we categorize a new hash so
                        -- that displacement tiles which render progressively (e.g. the
                        -- player walks across a large blend-displacement) each extend
                        -- the window rather than being cut off by the initial timer.
                        pending.firstApplied = now
                    elseif pending.callback and
                           not pending.callbackFiredForHashes[hashStr] then
                        -- The requested bits already exist, or reconciliation
                        -- rejected them. Invoke the side-effect callback once;
                        -- it re-checks which flags actually survived.
                        pending.callbackFiredForHashes[hashStr] = true
                        pending.callback(true, hashStr)
                    end
                end
                processedTextures[pending.materialName:lower()] = true
                foundAnyHash = true
                -- Do NOT break — all materialsToTry must be checked so that _stage1
                -- displacement variants are also categorized even when the base name
                -- already has hashes.
            end
        end

        if not foundAnyHash then
            -- No hash available yet — keep waiting
            table.insert(remaining, pending)
        elseif now - (pending.firstApplied or now) < PENDING_WATCH_SECONDS then
            -- Hash found but still within the watch window: keep the entry so any
            -- additional variants that appear over the next few seconds also get caught.
            if not pending.firstApplied then pending.firstApplied = now end
            table.insert(remaining, pending)
        end
        -- else: PENDING_WATCH_SECONDS has elapsed since the last new hash was categorized
    end

    pendingCategorizations = remaining
end

hook.Add("Think", "RemixCategoryManager_DrainPending", DrainPendingCategorizations)

-- Initialize C++ flags on every map load and trigger auto-categorization directly
hook.Add("InitPostEntity", "RemixCategoryManager_InitFlags", function()
    InitializeCppModuleFlags()
    MsgC(Color(100, 200, 255), "[RemixCategoryManager] C++ module flags initialized from ConVars\n")
    
    -- Always clear stale caches on map change regardless of auto-categorize setting
    processedTextures = {}
    materialHashCache = {}
    pendingCategorizations = {}
    currentMapToken = game.GetMap() or ""

    -- Drop previous-map texture associations and synchronize the C++ pipeline
    -- worker before starting this map's scan.
    if RemixMaterial and RemixMaterial.ClearTextureCache then
        RemixMaterial.ClearTextureCache()
    end

    -- Clear the C++ BSP world material registry so previous-map entries don't
    -- persist into the new map's BSP scan.
    if RemixMaterial and RemixMaterial.ClearBSPWorldMaterials then
        RemixMaterial.ClearBSPWorldMaterials()
    end

    -- Clear force-albedo hashes from the previous map so stale hashes don't
    -- bleed into the new map's rtx.legacyEmissiveForceAlbedoString.
    if RemixMaterial and RemixMaterial.ClearForceAlbedoHashes then
        RemixMaterial.ClearForceAlbedoHashes()
    end
    
    -- Reset guard flag so auto-categorization runs on each new map load
    autoInitRan = false
    
    hook.Remove("HUDPaint", "RemixCategoryManager_AutoInit")
    AutoInitFunction()
end)

hook.Add("HUDPaint", "RemixCategoryManager_AutoInit", AutoInitFunction)

-- Console command: remix_mark_decal (manually mark a material as decal by name)
concommand.Add("rtx_mark_decal", function(ply, cmd, args)
    if #args < 1 then
        MsgC(Color(255, 100, 100), "Usage: remix_mark_decal <material_name>\n")
        return
    end
    
    local materialName = args[1]
    local mat = Material(materialName)
    
    if not mat or mat:IsError() then
        MsgC(Color(255, 100, 100), string.format("[RemixCategoryManager] Error: Material '%s' not found\n", materialName))
        return
    end
    
    -- Get all hashes for this material
    if not RemixMaterial or not RemixMaterial.GetHashesForMaterial then
        MsgC(Color(255, 100, 100), "[RemixCategoryManager] RemixMaterial API not available\n")
        return
    end
    
    local hashes = RemixMaterial.GetHashesForMaterial(materialName)
    if not hashes or #hashes == 0 then
        MsgC(Color(255, 100, 100), string.format("[RemixCategoryManager] No hashes found for '%s'\n", materialName))
        return
    end
    
    -- Mark all hashes as decal
    local count = 0
    for _, hashStr in ipairs(hashes) do
        if RemixCategoryManager.SetMaterialCategory then
            RemixCategoryManager.SetMaterialCategory(materialName, RemixCategoryManager.CATEGORY_DECAL, hashStr)
            count = count + 1
        end
    end
    
    MsgC(Color(100, 255, 100), string.format("[RemixCategoryManager] Marked %d hash(es) for '%s' as DECAL\n", count, materialName))
end, nil, "Manually mark a material as decal by name")

-- Console command: remix_send_world_textures (manual trigger for testing)
concommand.Add("rtx_send_world_textures", function()
    MsgC(Color(100, 200, 255), "[RemixCategoryManager] Manually parsing BSP...\n")
    
    if not NikNaks or not NikNaks.CurrentMap then
        MsgC(Color(255, 100, 100), "[RemixCategoryManager] Error: NikNaks not available!\n")
        return
    end
    
    local bsp = NikNaks.CurrentMap
    if not bsp or not bsp.GetTextures then
        MsgC(Color(255, 100, 100), "[RemixCategoryManager] Error: BSP not loaded!\n")
        return
    end
    
    local textures = {}
    local ok, bspTextures = pcall(function() return bsp:GetTextures() end)
    
    if ok and bspTextures then
        textures = bspTextures
    end
    
    MsgC(Color(200, 200, 200), string.format("[RemixCategoryManager] Found %d textures in BSP\n", #textures))
    
    if RemixMaterial and RemixMaterial.SetWorldTextureList and #textures > 0 then
        RemixMaterial.SetWorldTextureList(textures)
        MsgC(Color(100, 255, 100), "[RemixCategoryManager] Sent to C++ module!\n")
    else
        MsgC(Color(255, 100, 100), "[RemixCategoryManager] Error: RemixMaterial.SetWorldTextureList not available or no textures\n")
    end
end, nil, "Manually parse BSP and send world textures to C++ for categorization")

-- Console command: remix_retry_pending
concommand.Add("rtx_retry_pending", function()
    if not RemixMaterial or not RemixMaterial.RetryPendingCategories then
        MsgC(Color(255, 100, 100), "[RemixCategoryManager] RemixMaterial.RetryPendingCategories not available\n")
        return
    end
    
    local pending = RemixMaterial.GetPendingCount and RemixMaterial.GetPendingCount() or 0
    MsgC(Color(200, 200, 200), string.format("[RemixCategoryManager] %d textures pending categorization\n", pending))
    
    if pending > 0 then
        local success = RemixMaterial.RetryPendingCategories()
        local remaining = RemixMaterial.GetPendingCount and RemixMaterial.GetPendingCount() or 0
        MsgC(Color(100, 255, 100), string.format("[RemixCategoryManager] Categorized %d, %d still pending\n", success, remaining))
    end
end, nil, "Retry categorizing textures that returned hash=0")

-- Console command: remix_rescan_materials
concommand.Add("rtx_rescan_materials", function()
    if not RemixMaterial or not RemixMaterial.RescanAllMaterials then
        MsgC(Color(255, 100, 100), "[RemixCategoryManager] RemixMaterial.RescanAllMaterials not available\n")
        return
    end
    
    MsgC(Color(200, 200, 200), "[RemixCategoryManager] Rescanning all cached materials for emissive...\n")
    local count = RemixMaterial.RescanAllMaterials()
    MsgC(Color(100, 255, 100), string.format("[RemixCategoryManager] Categorized %d materials\n", count))
end, nil, "Rescan all cached materials and apply categories (emissive, etc.)")

-- Console command: remix_dump_all_hashes
-- Console command: remix_dump_all_hashes
concommand.Add("rtx_dump_all_hashes", function(ply, cmd, args)
    if not RemixMaterial or not RemixMaterial.DumpAllTextureHashes then
        MsgC(Color(255, 100, 100), "[RemixCategoryManager] RemixMaterial.DumpAllTextureHashes not available\n")
        return
    end
    
    local searchHash = args[1]
    if searchHash then
        searchHash = string.upper(searchHash):gsub("^0X", "")
        MsgC(Color(100, 200, 255), string.format("[RemixCategoryManager] Searching for hash containing: %s\n", searchHash))
    else
        MsgC(Color(100, 200, 255), "[RemixCategoryManager] Dumping all tracked texture hashes...\n")
    end
    
    local dump = RemixMaterial.DumpAllTextureHashes()
    local found = 0
    local total = 0
    
    for _, entry in ipairs(dump) do
        total = total + 1
        local shouldPrint = not searchHash
        
        if searchHash then
            local hashUpper = string.upper(entry.hash):gsub("^0X", "")
            if hashUpper:find(searchHash, 1, true) then
                shouldPrint = true
                found = found + 1
            end
        end
        
        if shouldPrint then
            if entry.hash == "0x0" then
                MsgC(Color(255, 150, 100), string.format("  [%4d] %-60s | texture=%s | hash=%s (PENDING)\n", 
                    total, entry.material, entry.texture, entry.hash))
            else
                MsgC(Color(200, 200, 200), string.format("  [%4d] %-60s | texture=%s | hash=%s\n", 
                    total, entry.material, entry.texture, entry.hash))
            end
        end
    end
    
    if searchHash then
        if found > 0 then
            MsgC(Color(100, 255, 100), string.format("[RemixCategoryManager] Found %d/%d materials with hash containing '%s'\n", found, total, searchHash))
        else
            MsgC(Color(255, 100, 100), string.format("[RemixCategoryManager] No materials found with hash containing '%s' (total: %d)\n", searchHash, total))
        end
    else
        MsgC(Color(100, 255, 100), string.format("[RemixCategoryManager] Total tracked textures: %d\n", total))
    end
end, nil, "Dump all tracked textures with their hashes (optional: remix_dump_all_hashes <partial_hash>)")

-- Console command: remix_check_shared_hash
concommand.Add("rtx_check_shared_hash", function(ply, cmd, args)
    if not args[1] then
        MsgC(Color(255, 200, 100), "Usage: remix_check_shared_hash <hash>\n")
        MsgC(Color(255, 200, 100), "Example: remix_check_shared_hash F0F66D57DE6A8885\n")
        return
    end
    
    local hash = args[1]
    local materials = RemixMaterial.FindMaterialByHash(hash)
    
    if not materials or #materials == 0 then
        MsgC(Color(255, 100, 100), string.format("No materials found with hash %s\n", hash))
        return
    end
    
    MsgC(Color(100, 200, 255), string.format("[Hash Check] Found %d material(s) sharing hash %s\n", #materials, hash))
    MsgC(Color(200, 200, 200), "[Hash Check] Checking base textures...\n\n")
    
    local baseTextures = {}
    local errorCount = 0
    
    for i, matName in ipairs(materials) do
        local mat = Material(matName)
        if mat and not mat:IsError() then
            -- Get $basetexture parameter
            local baseTex = mat:GetTexture("$basetexture")
            if baseTex and not baseTex:IsError() then
                local texName = baseTex:GetName()
                baseTextures[texName] = (baseTextures[texName] or 0) + 1
                
                if i <= 5 then  -- Show first 5
                    MsgC(Color(200, 200, 200), string.format("  %2d. %-50s -> %s\n", i, matName, texName))
                end
            else
                MsgC(Color(255, 150, 100), string.format("  %2d. %-50s -> ERROR TEXTURE\n", i, matName))
                errorCount = errorCount + 1
            end
        else
            MsgC(Color(255, 100, 100), string.format("  %2d. %-50s -> ERROR MATERIAL\n", i, matName))
            errorCount = errorCount + 1
        end
    end
    
    if #materials > 5 then
        MsgC(Color(200, 200, 200), string.format("  ... and %d more\n", #materials - 5))
    end
    
    -- Summary
    MsgC(Color(100, 200, 255), "\n[Hash Check] Summary:\n")
    MsgC(Color(200, 200, 200), string.format("  Total materials: %d\n", #materials))
    MsgC(Color(200, 200, 200), string.format("  Error materials: %d\n", errorCount))
    MsgC(Color(200, 200, 200), string.format("  Unique base textures: %d\n", table.Count(baseTextures)))
    
    if table.Count(baseTextures) > 0 then
        MsgC(Color(200, 200, 200), "\n  Base textures used:\n")
        for texName, count in pairs(baseTextures) do
            MsgC(Color(200, 200, 200), string.format("    - %s (used by %d material%s)\n", 
                texName, count, count > 1 and "s" or ""))
        end
    end
    
    if table.Count(baseTextures) == 1 then
        MsgC(Color(100, 255, 100), "\n✓ All materials share the same base texture (LEGITIMATE)\n")
    elseif errorCount == #materials then
        MsgC(Color(255, 100, 100), "\n✗ All materials are errors (PLACEHOLDER/ERROR TEXTURE)\n")
    elseif table.Count(baseTextures) > 1 then
        MsgC(Color(255, 200, 100), "\n⚠ Materials use different base textures but same hash (TEXTURE ATLAS or UV VARIANTS)\n")
    end
end, nil, "Check why multiple materials share the same hash")

-- Console command: remix_test_displacement_categorization
concommand.Add("rtx_test_disp_cat", function(ply, cmd, args)
    local matName = args[1] or "concrete/blend_blacktop_01"
    
    MsgC(Color(100, 200, 255), string.format("[Test] Testing categorization for: %s\n", matName))
    
    -- Try base material
    local hash1, hash1Str = RemixCategoryManager.GetMaterialHash(matName)
    if hash1 and hash1 ~= 0 then
        MsgC(Color(100, 255, 100), string.format("  Base material '%s': hash=%s\n", matName, hash1Str))
    else
        MsgC(Color(255, 150, 100), string.format("  Base material '%s': NO HASH\n", matName))
    end
    
    -- Try _stage1 variant
    local matNameStage1 = matName .. "_stage1"
    local hash2, hash2Str = RemixCategoryManager.GetMaterialHash(matNameStage1)
    if hash2 and hash2 ~= 0 then
        MsgC(Color(100, 255, 100), string.format("  Stage1 material '%s': hash=%s\n", matNameStage1, hash2Str))
    else
        MsgC(Color(255, 150, 100), string.format("  Stage1 material '%s': NO HASH\n", matNameStage1))
    end
    
    -- Now try to categorize
    MsgC(Color(100, 200, 255), string.format("[Test] Attempting to categorize...\n"))
    RemixCategoryManager.SetMaterialCategory(matName, RemixCategoryManager.CATEGORY.DECAL_STATIC)
end, nil, "Test displacement categorization (usage: remix_test_disp_cat <material_name>)")

-- Add ConVar callback to update C++ module when master toggle changes
cvars.AddChangeCallback("rtx_auto_categorize", function(convar, oldValue, newValue)
    if RemixMaterial and RemixMaterial.SetAutoCategorization then
        local enabled = tonumber(newValue) == 1
        RemixMaterial.SetAutoCategorization(enabled)
        MsgC(Color(100, 200, 255), string.format("[RemixCategoryManager] Auto-categorization %s\n", 
            enabled and "enabled" or "disabled"))
        
        -- If enabling, rescan all tracked materials to catch what was missed
        if enabled and RemixMaterial.RescanAllMaterials then
            timer.Simple(0.1, function()
                if GetConVar("rtx_debug_categorization"):GetBool() then
                    MsgC(Color(100, 200, 255), "[RemixCategoryManager] Rescanning all tracked materials...\n")
                end
                local count = RemixMaterial.RescanAllMaterials()
                if GetConVar("rtx_debug_categorization"):GetBool() then
                    MsgC(Color(100, 255, 100), string.format("[RemixCategoryManager] Rescan complete: %d materials checked\n", count))
                end
            end)
        end
    end
end, "RemixCategoryManager_MasterToggle")

-- Add ConVar callback to update C++ module when particle toggle changes
cvars.AddChangeCallback("rtx_auto_categorize_particles", function(convar, oldValue, newValue)
    if RemixMaterial and RemixMaterial.SetParticleCategorization then
        local enabled = tonumber(newValue) == 1
        RemixMaterial.SetParticleCategorization(enabled)
        MsgC(Color(100, 200, 255), string.format("[RemixCategoryManager] Particle categorization %s\n", 
            enabled and "enabled" or "disabled"))
        
        -- If enabling and master is enabled, rescan
        if enabled and GetConVar("rtx_auto_categorize"):GetBool() and RemixMaterial.RescanAllMaterials then
            timer.Simple(0.1, function()
                local count = RemixMaterial.RescanAllMaterials()
                if GetConVar("rtx_debug_categorization"):GetBool() then
                    MsgC(Color(100, 255, 100), string.format("[RemixCategoryManager] Rescanned %d materials for particles\n", count))
                end
            end)
        end
    end
end, "RemixCategoryManager_ParticleToggle")

-- Add ConVar callback to update C++ module when decal toggle changes
cvars.AddChangeCallback("rtx_auto_categorize_decals", function(convar, oldValue, newValue)
    if RemixMaterial and RemixMaterial.SetDecalCategorization then
        local enabled = tonumber(newValue) == 1
        RemixMaterial.SetDecalCategorization(enabled)
        MsgC(Color(100, 200, 255), string.format("[RemixCategoryManager] Decal categorization %s\n", 
            enabled and "enabled" or "disabled"))
        
        -- If enabling and master is enabled, rescan
        if enabled and GetConVar("rtx_auto_categorize"):GetBool() and RemixMaterial.RescanAllMaterials then
            timer.Simple(0.1, function()
                local count = RemixMaterial.RescanAllMaterials()
                if GetConVar("rtx_debug_categorization"):GetBool() then
                    MsgC(Color(100, 255, 100), string.format("[RemixCategoryManager] Rescanned %d materials for decals\n", count))
                end
            end)
        end
    end
end, "RemixCategoryManager_DecalToggle")

-- Add ConVar callback to update C++ module when emissive toggle changes
cvars.AddChangeCallback("rtx_auto_categorize_emissive", function(convar, oldValue, newValue)
    if RemixMaterial and RemixMaterial.SetEmissiveCategorization then
        local enabled = tonumber(newValue) == 1
        RemixMaterial.SetEmissiveCategorization(enabled)
        MsgC(Color(100, 200, 255), string.format("[RemixCategoryManager] Emissive categorization %s\n", 
            enabled and "enabled" or "disabled"))
        
        -- If enabling and master is enabled, rescan
        if enabled and GetConVar("rtx_auto_categorize"):GetBool() and RemixMaterial.RescanAllMaterials then
            timer.Simple(0.1, function()
                local count = RemixMaterial.RescanAllMaterials()
                if GetConVar("rtx_debug_categorization"):GetBool() then
                    MsgC(Color(100, 255, 100), string.format("[RemixCategoryManager] Rescanned %d materials for emissives\n", count))
                end
            end)
        end
    end
end, "RemixCategoryManager_EmissiveToggle")

-- Add ConVar callback to update C++ module when debug toggle changes
cvars.AddChangeCallback("rtx_debug_categorization", function(convar, oldValue, newValue)
    if RemixMaterial and RemixMaterial.SetDebugOutput then
        local enabled = tonumber(newValue) == 1
        RemixMaterial.SetDebugOutput(enabled)
        MsgC(Color(100, 200, 255), string.format("[RemixCategoryManager] Debug output %s\n", 
            enabled and "enabled" or "disabled"))
    end
end, "RemixCategoryManager_DebugToggle")

return RemixCategoryManager
