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
    Error("[RemixCategoryManager] RemixMaterial API not available!\n")
    return
end

RemixCategoryManager = RemixCategoryManager or {}

-- ConVars for configuration
CreateClientConVar("remix_auto_categorize", "1", true, false, "Automatically categorize world textures when a map loads (1 = enabled, 0 = disabled)")
CreateClientConVar("remix_auto_categorize_delay", "5", true, false, "Delay in seconds before auto-categorization runs (default: 5)")

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
    -- For opaque world geometry (walls, floors, etc.) - needs Decal for proper blending
    WORLD_GEOMETRY = RemixCategoryManager.CATEGORY.DECAL_STATIC,  -- 0x1000
    
    -- For transparent world geometry (glass, windows, fences)
    WORLD_GEOMETRY_TRANSPARENT = RemixCategoryManager.CATEGORY.DECAL_STATIC,  -- 0x1000
    
    -- For sky textures
    SKY = RemixCategoryManager.CATEGORY.SKY,  -- 0x4
    
    -- For water
    WATER = RemixCategoryManager.CATEGORY.ANIMATED_WATER,  -- 0x40000
    
    -- For terrain (displacement surfaces) - mark as DECAL for proper blending
    TERRAIN = RemixCategoryManager.CATEGORY.DECAL_STATIC,  -- 0x1000
    
    -- For map decals (overlays, bullet holes, blood, etc.)
    MAP_DECAL = RemixCategoryManager.CATEGORY.DECAL_STATIC,  -- 0x1000
    
    -- For self-illuminated/emissive materials (lights, glows, LED panels)
    EMISSIVE = RemixCategoryManager.CATEGORY.LEGACY_EMISSIVE,  -- 0x1000000
    
    -- For player model textures (pseudoplayer, third-person view)
    PLAYER_MODEL = RemixCategoryManager.CATEGORY.THIRD_PERSON_PLAYER_MODEL,  -- 0x80000
}

-- Local cache of material name -> hash mappings
local materialHashCache = {}

-- Local cache of texture names that have been processed
local processedTextures = {}

--[[
    Check if a material is self-illuminated (emissive)
    @param materialName string - The material name
    @return boolean - True if material has $selfillum enabled
]]--
function RemixCategoryManager.IsMaterialEmissive(materialName)
    local mat = Material(materialName)
    if not mat or mat:IsError() then
        return false
    end
    
    -- Method 1: Check raw KeyValues (Most reliable for VMT parameters)
    local keyValues = mat:GetKeyValues()
    if keyValues then
        -- Check $selfillum (case-insensitive scan)
        for k, v in pairs(keyValues) do
            if k:lower() == "$selfillum" then
                local val = tonumber(v)
                if val and val >= 1 then return true end
            end
        end
    end

    -- Method 2: Check standard GetInt (compiled params)
    local selfillum = mat:GetInt("$selfillum")
    if selfillum and selfillum == 1 then return true end
    
    -- Method 3: Check GetFloat (sometimes stored as float)
    local selfillumFloat = mat:GetFloat("$selfillum")
    if selfillumFloat and selfillumFloat >= 1 then return true end

    -- Method 4: Check Shader Name (UnlitGeneric is always emissive)
    local shader = mat:GetShader()
    if shader and shader:lower() == "unlitgeneric" then return true end
    
    -- Method 5: Check $emissive parameter (used in some shaders)
    local emissive = mat:GetVector("$emissive")
    if emissive and (emissive.x > 0 or emissive.y > 0 or emissive.z > 0) then
        return true
    end
    
    -- Method 6: Check for $selfillummask texture
    -- If a dedicated mask texture is defined, it's definitely emissive
    local mask = mat:GetTexture("$selfillummask")
    if mask and not mask:IsError() then
        return true
    end
    
    -- Method 7: Check for $illumposition (usually implies lights/emissive)
    -- This is often used for volumetric lights or sprites
    if mat:GetVector("$illumposition") then
        return true
    end

    -- Method 8: Raw VMT File Parse (The "Nuclear Option")
    -- If the engine hides the parameter (e.g. in DX7 fallback shaders), read the file directly
    local vmtPath = "materials/" .. materialName
    if not string.EndsWith(vmtPath, ".vmt") then
        vmtPath = vmtPath .. ".vmt"
    end
    
    local content = file.Read(vmtPath, "GAME")
    if content then
        -- Simple pattern match for "$selfillum" followed by "1"
        -- Normalize to lowercase
        local lowerContent = content:lower()
        
        -- Escape the $ symbol with % because it's a magic character in Lua patterns
        -- Pattern: optional quote, literal $selfillum, optional quote, whitespace, optional quote, 1, optional quote
        if lowerContent:find('["\']?%$selfillum["\']?%s+["\']?1["\']?') then
            return true
        end
    end

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
    local hashStr, hashNum = RemixCategoryManager.GetMaterialHash(materialName)
    if hashStr then
        -- Hash already available
        MsgC(Color(0, 255, 150), string.format("[RemixCategoryManager] Setting category 0x%X for material '%s' (hash %s)\n", 
            categoryFlags, materialName, hashStr))
        
        local success = RemixCategoryManager.SetHashCategory(hashStr, categoryFlags)
        if callback then callback(success, hashStr) end
        return success
    else
        -- Hash not available yet - track and retry after rendering
        -- MsgC(Color(255, 200, 100), "[RemixCategoryManager] Tracking material for hash: " .. materialName .. "\n")
        RemixMaterial.TrackMaterial(materialName)
        
        -- Wait for material to be rendered and hash to be available
        timer.Simple(0.15, function()
            local hashStr2, hashNum2 = RemixCategoryManager.GetMaterialHash(materialName)
            if hashStr2 then
                MsgC(Color(0, 255, 150), string.format("[RemixCategoryManager] Setting category 0x%X for material '%s' (hash %s)\n", 
                    categoryFlags, materialName, hashStr2))
                
                local success = RemixCategoryManager.SetHashCategory(hashStr2, categoryFlags)
                if callback then callback(success, hashStr2) end
            else
                MsgC(Color(255, 150, 0), "[RemixCategoryManager] Warning: Could not get hash for material after tracking: " .. materialName .. "\n")
                if callback then callback(false, nil) end
            end
        end)
        
        return false -- Not immediate
    end
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
                                -- Displacement materials often have two base textures ($basetexture and $basetexture2)
                                -- We need to extract and categorize both
                                local texturesToAdd = {}
                                
                                -- Try to get $basetexture
                                local baseTex = material:GetTexture("$basetexture")
                                if baseTex and baseTex.GetName then
                                    local texName = baseTex:GetName()
                                    if texName and texName ~= "" then
                                        table.insert(texturesToAdd, texName)
                                    end
                                end
                                
                                -- Try to get $basetexture2
                                local baseTex2 = material:GetTexture("$basetexture2")
                                if baseTex2 and baseTex2.GetName then
                                    local texName2 = baseTex2:GetName()
                                    if texName2 and texName2 ~= "" then
                                        table.insert(texturesToAdd, texName2)
                                    end
                                end
                                
                                if dispFaceCount <= 3 then
                                    MsgC(Color(255, 255, 0), string.format("[RemixCategoryManager] Displacement textures: %s\n", 
                                        table.concat(texturesToAdd, ", ")))
                                end
                                
                                -- Force-track and add all found textures
                                for _, texName in ipairs(texturesToAdd) do
                                    if not seenDisplacements[texName] then
                                        seenDisplacements[texName] = true
                                        dispCount = dispCount + 1
                                        
                                        -- Force the texture to be tracked by D3D9
                                        local trackedMatName = RemixCategoryManager.ForceTrackTexture(texName)
                                        
                                        if trackedMatName then
                                            -- Check if not already in main texture list
                                            local found = false
                                            for _, existingTex in ipairs(textures) do
                                                if existingTex == trackedMatName then
                                                    found = true
                                                    break
                                                end
                                            end
                                            
                                            if not found then
                                                table.insert(textures, trackedMatName)
                                            end
                                        end
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
            
            -- Skip if already processed
            if not processedTextures[lowerName] then
                processedTextures[lowerName] = true
                
                -- Set category for this material
                if RemixCategoryManager.SetMaterialCategory(materialName, categoryFlags) then
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
                                -- Displacement materials often have two base textures ($basetexture and $basetexture2)
                                local texturesToAdd = {}
                                
                                -- Try to get $basetexture
                                local baseTex = material:GetTexture("$basetexture")
                                if baseTex and baseTex.GetName then
                                    local texName = baseTex:GetName()
                                    if texName and texName ~= "" then
                                        table.insert(texturesToAdd, texName)
                                    end
                                end
                                
                                -- Try to get $basetexture2
                                local baseTex2 = material:GetTexture("$basetexture2")
                                if baseTex2 and baseTex2.GetName then
                                    local texName2 = baseTex2:GetName()
                                    if texName2 and texName2 ~= "" then
                                        table.insert(texturesToAdd, texName2)
                                    end
                                end
                                
                                -- Force-track and add all found textures
                                for _, texName in ipairs(texturesToAdd) do
                                    if not displacementTextures[texName] then
                                        displacementTextures[texName] = true  -- Mark as displacement
                                        dispCount = dispCount + 1
                                        
                                        -- Force the texture to be tracked by D3D9
                                        local trackedMatName = RemixCategoryManager.ForceTrackTexture(texName)
                                        
                                        if trackedMatName then
                                            -- Also mark the tracked material name as displacement
                                            displacementTextures[trackedMatName] = true
                                            
                                            -- Check if not already in main texture list
                                            local found = false
                                            for _, existingTex in ipairs(textures) do
                                                if existingTex == trackedMatName then
                                                    found = true
                                                    break
                                                end
                                            end
                                            
                                            if not found then
                                                table.insert(textures, trackedMatName)
                                            end
                                        end
                                    end
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
            MsgC(Color(100, 255, 100), string.format("[RemixCategoryManager] Added %d displacement textures\n", dispCount))
        else
            MsgC(Color(200, 200, 200), "[RemixCategoryManager] No displacement textures found\n")
        end
    else
        MsgC(Color(255, 200, 100), "[RemixCategoryManager] Could not get leafs from BSP\n")
    end
    
    -- Process Static Props (models placed in Hammer)
    -- These often contain emissive materials that aren't used on world brushes
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
        water = 0,
        sky = 0,
        terrain = 0,
        displacements = 0,
        decals = 0,
        emissive = 0,
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
                processedTextures[lowerName] = true
                
                -- Determine category based on texture properties and name patterns
                local category = nil
                
                -- Sky textures (check first, highest priority for exclusion)
                if string.find(lowerName, "sky") or string.find(lowerName, "skybox") then
                    category = RemixCategoryManager.PRESET.SKY
                    stats.sky = stats.sky + 1
                
                -- Emissive/Self-illuminated materials (check VMT parameter $selfillum)
                -- This is more reliable than keyword matching
                elseif RemixCategoryManager.IsMaterialEmissive(materialName) then
                    category = RemixCategoryManager.PRESET.EMISSIVE
                    stats.emissive = stats.emissive + 1
                
                -- Map decals (overlays, bullet holes, blood, etc.)
                elseif string.find(lowerName, "^decals/") or 
                       string.find(lowerName, "/decals/") or
                       string.find(lowerName, "overlay") or
                       string.find(lowerName, "bulleth") or
                       string.find(lowerName, "blood") or
                       string.find(lowerName, "scorch") then
                    category = RemixCategoryManager.CATEGORY.DECAL_STATIC
                    stats.decals = stats.decals + 1
                
                -- Displacement textures (terrain) - also marked as DECAL_STATIC
                elseif displacementTextures[texName] then
                    category = RemixCategoryManager.PRESET.TERRAIN  -- This is DECAL_STATIC (0x1000)
                    stats.displacements = stats.displacements + 1
                
                -- Sky textures
                elseif string.find(lowerName, "sky") or string.find(lowerName, "skybox") then
                    category = RemixCategoryManager.PRESET.SKY
                    stats.sky = stats.sky + 1
                    
                -- Water textures
                elseif string.find(lowerName, "water") or string.find(lowerName, "slime") then
                    category = RemixCategoryManager.PRESET.WATER
                    stats.water = stats.water + 1
                    
                -- Transparent textures (glass, fences, etc.)
                elseif string.find(lowerName, "glass") or 
                       string.find(lowerName, "window") or
                       string.find(lowerName, "fence") or
                       string.find(lowerName, "grate") or
                       string.find(lowerName, "chain") then
                    category = RemixCategoryManager.PRESET.WORLD_GEOMETRY_TRANSPARENT
                    stats.transparent = stats.transparent + 1
                    
                -- Terrain textures
                elseif string.find(lowerName, "dirt") or
                       string.find(lowerName, "grass") or
                       string.find(lowerName, "ground") or
                       string.find(lowerName, "terrain") then
                    category = RemixCategoryManager.PRESET.TERRAIN
                    stats.terrain = stats.terrain + 1
                    
                -- Default: solid world geometry
                else
                    category = RemixCategoryManager.PRESET.WORLD_GEOMETRY
                    stats.solid = stats.solid + 1
                end
                
                if category then
                    RemixCategoryManager.SetMaterialCategory(materialName, category)
                end
            else
                stats.skipped = stats.skipped + 1
            end
        end
    end
    
    MsgC(Color(100, 255, 100), "[RemixCategoryManager] Smart-mark complete:\n")
    MsgC(Color(200, 200, 200), string.format("  Total: %d, Solid: %d, Emissive: %d, Transparent: %d, Water: %d, Sky: %d, Terrain: %d, Decals: %d, Skipped: %d\n",
        stats.total, stats.solid, stats.emissive, stats.transparent, stats.water, stats.sky, stats.terrain, stats.decals, stats.skipped))
    
    return stats
end

--[[
    Console command to mark all world textures
]]--
concommand.Add("remix_mark_world_textures", function(ply, cmd, args)
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
concommand.Add("remix_smart_mark_world", function(ply, cmd, args)
    RemixCategoryManager.SmartMarkWorldTextures()
end, nil, "Intelligently mark world textures based on their properties")

--[[
    Console command to clear all category mappings
]]--
concommand.Add("remix_clear_categories", function(ply, cmd, args)
    RemixCategoryManager.ClearAllCategories()
    MsgC(Color(100, 255, 100), "[RemixCategoryManager] All category mappings cleared\n")
end, nil, "Clear all hash-to-category mappings")

--[[
    Console command to search for texture hashes by name
]]--
concommand.Add("remix_find_texture_hash", function(ply, cmd, args)
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
concommand.Add("remix_set_material_category", function(ply, cmd, args)
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
concommand.Add("remix_check_emissive", function(ply, cmd, args)
    if not args[1] then
        MsgC(Color(255, 200, 100), "Usage: remix_check_emissive <material_name>\n")
        MsgC(Color(255, 200, 100), "Example: remix_check_emissive models/props_c17/industrialbellbottomon01\n")
        return
    end
    
    local materialName = args[1]
    local isEmissive = RemixCategoryManager.IsMaterialEmissive(materialName)
    
    if isEmissive then
        MsgC(Color(100, 255, 100), string.format("[RemixCategoryManager] ✓ '%s' is EMISSIVE\n", materialName))
    else
        MsgC(Color(255, 100, 100), string.format("[RemixCategoryManager] ✗ '%s' is NOT emissive\n", materialName))
    end

    -- Show detailed debug info
    local mat = Material(materialName)
    if mat and not mat:IsError() then
        MsgC(Color(200, 200, 200), "  Debug Info:\n")
        MsgC(Color(200, 200, 200), string.format("  - Shader: %s\n", tostring(mat:GetShader())))
        
        local intVal = mat:GetInt("$selfillum")
        MsgC(Color(200, 200, 200), string.format("  - GetInt($selfillum): %s\n", tostring(intVal or "nil")))
        
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
            -- Print other relevant keys
            for k, v in pairs(keyValues) do
                if k:lower():find("illum") or k:lower():find("emiss") then
                    MsgC(Color(200, 200, 200), string.format("    - %s: %s\n", k, tostring(v)))
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
                
                -- Check for value 1
                if lowerContent:find('["\']?%$selfillum["\']?%s+["\']?1["\']?') then
                    MsgC(Color(100, 255, 100), "    - Found '$selfillum 1' pattern match!\n")
                else
                    MsgC(Color(255, 200, 100), "    - '$selfillum' found but not set to 1 (or pattern failed)\n")
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


-- Auto-initialize on player spawn (client-side)
hook.Add("HUDPaint", "RemixCategoryManager_AutoInit", function()
    -- Only run once
    hook.Remove("HUDPaint", "RemixCategoryManager_AutoInit")
    
    -- Check if auto-categorization is enabled
    if not GetConVar("remix_auto_categorize"):GetBool() then
        MsgC(Color(200, 200, 200), "[RemixCategoryManager] Auto-categorization disabled (remix_auto_categorize = 0)\n")
        MsgC(Color(255, 200, 100), "[RemixCategoryManager] Tip: Use 'remix_smart_mark_world' to manually categorize textures\n")
        return
    end
    
    -- Get delay from ConVar (default 5 seconds after player spawns)
    local delay = GetConVar("remix_auto_categorize_delay"):GetFloat()
    
    MsgC(Color(200, 200, 200), string.format("[RemixCategoryManager] Will auto-categorize world textures in %.1f seconds...\n", delay))
    
    -- Wait for world to render and materials to be tracked
    timer.Simple(delay, function()
        MsgC(Color(100, 200, 255), "[RemixCategoryManager] Auto-marking world textures...\n")
        MsgC(Color(255, 200, 100), "[RemixCategoryManager] Note: Materials are tracked as they render. More will be categorized as you explore.\n")
        MsgC(Color(255, 200, 100), "[RemixCategoryManager] Tip: Use 'remix_smart_mark_world' to manually trigger categorization\n")
        -- Use smart marking by default
        RemixCategoryManager.SmartMarkWorldTextures()
    end)
end)

MsgC(Color(100, 255, 100), "[RemixCategoryManager] Loaded successfully!\n")
MsgC(Color(200, 200, 200), "[RemixCategoryManager] Commands:\n")
MsgC(Color(200, 200, 200), "  remix_mark_world_textures    - Mark all world textures with DECAL_STATIC\n")
MsgC(Color(200, 200, 200), "  remix_smart_mark_world       - Intelligently categorize world textures\n")
MsgC(Color(200, 200, 200), "  remix_clear_categories       - Clear all category mappings\n")
MsgC(Color(200, 200, 200), "  remix_find_texture_hash      - Search for texture by name\n")
MsgC(Color(200, 200, 200), "  remix_set_material_category  - Set category for a material\n")
MsgC(Color(200, 200, 200), "  remix_check_emissive         - Check if material has $selfillum\n")
MsgC(Color(200, 200, 200), "[RemixCategoryManager] ConVars:\n")
MsgC(Color(200, 200, 200), "  remix_auto_categorize (0/1), remix_auto_categorize_delay (seconds)\n")

return RemixCategoryManager
