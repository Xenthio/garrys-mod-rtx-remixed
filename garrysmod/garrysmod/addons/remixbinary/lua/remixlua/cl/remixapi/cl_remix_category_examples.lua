--[[
    RTX Remix Category Manager - Examples and Documentation
    
    This file provides examples and documentation for using the RemixCategoryManager
    to control texture categories in RTX Remix.
]]--

--[[
    === OVERVIEW ===
    
    The RemixCategoryManager allows you to control how textures are rendered in RTX Remix
    by assigning category flags to texture hashes. This is particularly useful for:
    
    1. Marking world geometry as Decal for proper blending
    2. Identifying transparent surfaces (glass, fences)
    3. Marking terrain, water, and sky textures
    4. Controlling lighting and rendering behaviors per-texture
    
    === BASIC USAGE ===
]]--

-- Example 1: Mark all world textures as DECAL_STATIC
concommand.Add("example_mark_all_world", function()
    local marked = RemixCategoryManager.MarkWorldTextures(RemixCategoryManager.PRESET.WORLD_GEOMETRY)
    print(string.format("Marked %d world textures as DECAL_STATIC", marked))
end)

-- Example 2: Smart-mark world textures (automatically categorizes based on texture names)
concommand.Add("example_smart_mark", function()
    local stats = RemixCategoryManager.SmartMarkWorldTextures()
    print("Smart marking complete!")
    PrintTable(stats)
end)

-- Example 3: Set category for a specific material
concommand.Add("example_mark_single_material", function()
    local materialName = "materials/concrete/concretefloor001a.vmt"
    local success = RemixCategoryManager.SetMaterialCategory(
        materialName,
        RemixCategoryManager.CATEGORY.DECAL_STATIC
    )
    print("Material marked:", success)
end)

-- Example 4: Combine multiple category flags
concommand.Add("example_combined_flags", function()
    local materialName = "materials/glass/glasswindow001a.vmt"
    
    -- Mark as decal + transparent + no opacity micromap
    local flags = bit.bor(
        RemixCategoryManager.CATEGORY.DECAL_STATIC,
        RemixCategoryManager.CATEGORY.ALPHA_BLEND_TO_CUTOUT,
        RemixCategoryManager.CATEGORY.IGNORE_OPACITY_MICROMAP
    )
    
    RemixCategoryManager.SetMaterialCategory(materialName, flags)
    print(string.format("Glass material marked with combined flags: 0x%X", flags))
end)

-- Example 5: Mark materials by pattern
concommand.Add("example_mark_by_pattern", function()
    local count = 0
    
    -- Get all cached materials
    local materials = RemixMaterial.GetCachedMaterials()
    
    for _, materialName in ipairs(materials) do
        -- Mark all concrete materials as DECAL_STATIC + TERRAIN
        if string.find(string.lower(materialName), "concrete") then
            local flags = bit.bor(
                RemixCategoryManager.CATEGORY.DECAL_STATIC,
                RemixCategoryManager.CATEGORY.TERRAIN
            )
            
            if RemixCategoryManager.SetMaterialCategory(materialName, flags) then
                count = count + 1
            end
        end
    end
    
    print(string.format("Marked %d concrete materials", count))
end)

-- Example 6: Custom BSP parsing
concommand.Add("example_custom_bsp_parse", function()
    if not NikNaks or not NikNaks.CurrentMap then
        print("NikNaks not available!")
        return
    end
    
    local bsp = NikNaks.CurrentMap
    local textures = bsp:GetTextures()
    
    local metalCount = 0
    local woodCount = 0
    
    for i, texName in ipairs(textures) do
        if texName then
            texName = string.lower(texName)
            local materialName = "materials/" .. texName
            if not string.find(materialName, "%.vmt$") then
                materialName = materialName .. ".vmt"
            end
            
            -- Mark metal surfaces
            if string.find(texName, "metal") then
                RemixCategoryManager.SetMaterialCategory(
                    materialName,
                    RemixCategoryManager.PRESET.WORLD_GEOMETRY
                )
                metalCount = metalCount + 1
            -- Mark wood surfaces
            elseif string.find(texName, "wood") then
                RemixCategoryManager.SetMaterialCategory(
                    materialName,
                    RemixCategoryManager.PRESET.WORLD_GEOMETRY
                )
                woodCount = woodCount + 1
            end
        end
    end
    
    print(string.format("Marked %d metal and %d wood textures", metalCount, woodCount))
end)

-- Example 7: Working with texture hashes directly
concommand.Add("example_hash_direct", function()
    local materialName = "materials/concrete/concretefloor001a.vmt"
    
    -- Get the texture hash
    local hashStr, hashNum = RemixCategoryManager.GetMaterialHash(materialName)
    
    if hashStr then
        print(string.format("Material: %s", materialName))
        print(string.format("Hash (string): %s", hashStr))
        print(string.format("Hash (number): %.0f", hashNum))
        
        -- Set category using hash directly
        RemixMaterial.SetHashCategory(hashStr, RemixCategoryManager.CATEGORY.DECAL_STATIC)
        
        -- Check if it was set
        local category = RemixMaterial.GetHashCategory(hashStr)
        if category then
            print(string.format("Category set to: 0x%X", category))
        end
    else
        print("Could not get hash for material")
    end
end)

--[[
    === AVAILABLE CATEGORY FLAGS ===
    
    RemixCategoryManager.CATEGORY contains all available flags:
    
    - WORLD_UI                  : UI elements in the world
    - WORLD_MATTE               : Matte objects (no reflections)
    - SKY                       : Sky textures
    - IGNORE                    : Completely ignore this geometry
    - IGNORE_LIGHTS             : Don't light this geometry
    - IGNORE_ANTI_CULLING       : Disable anti-culling
    - IGNORE_MOTION_BLUR        : Disable motion blur
    - IGNORE_OPACITY_MICROMAP   : Disable opacity micromap optimization
    - HIDDEN                    : Hide this geometry
    - PARTICLE                  : Particle system
    - BEAM                      : Beam effects
    - DECAL_STATIC              : Static decal (for world geometry)
    - DECAL_DYNAMIC             : Dynamic decal
    - DECAL_SINGLE_OFFSET       : Decal with single offset
    - DECAL_NO_OFFSET           : Decal with no offset
    - ALPHA_BLEND_TO_CUTOUT     : Convert alpha blend to alpha test
    - TERRAIN                   : Terrain texture
    - ANIMATED_WATER            : Animated water surface
    - THIRD_PERSON_PLAYER_MODEL : Third person player model
    - THIRD_PERSON_PLAYER_BODY  : Third person player body
    - IGNORE_BAKED_LIGHTING     : Ignore baked lighting
    - IGNORE_ALPHA_CHANNEL      : Ignore alpha channel
    - IGNORE_TRANSPARENCY_LAYER : Ignore transparency
    - PARTICLE_EMITTER          : Particle emitter
    - LEGACY_EMISSIVE           : Legacy emissive materials
    
    === PRESETS ===
    
    RemixCategoryManager.PRESET contains common combinations:
    
    - WORLD_GEOMETRY            : For opaque world geometry (walls, floors)
    - WORLD_GEOMETRY_TRANSPARENT: For transparent world geometry (glass, fences)
    - TERRAIN                   : For terrain textures
    - WATER                     : For water surfaces
    - SKY                       : For skybox textures
    
    === COMMON USE CASES ===
]]--

-- Use case 1: Fix world geometry blending issues
-- Problem: Opaque world geometry doesn't blend properly with lighting
-- Solution: Mark as DECAL_STATIC
function FixWorldGeometryBlending()
    RemixCategoryManager.MarkWorldTextures(RemixCategoryManager.CATEGORY.DECAL_STATIC)
end

-- Use case 2: Identify transparent surfaces
-- Problem: Glass and fences need special handling
-- Solution: Use smart marking or mark manually
function FixTransparentSurfaces()
    local materials = RemixMaterial.GetCachedMaterials()
    
    for _, mat in ipairs(materials) do
        local name = string.lower(mat)
        if string.find(name, "glass") or string.find(name, "window") then
            local flags = bit.bor(
                RemixCategoryManager.CATEGORY.DECAL_STATIC,
                RemixCategoryManager.CATEGORY.ALPHA_BLEND_TO_CUTOUT
            )
            RemixCategoryManager.SetMaterialCategory(mat, flags)
        end
    end
end

-- Use case 3: Per-map custom categories
-- Problem: Some maps need special handling
-- Solution: Create map-specific config
hook.Add("InitPostEntity", "RemixCategoryManager_CustomPerMap", function()
    local mapName = game.GetMap()
    
    if mapName == "gm_construct" then
        -- Specific handling for gm_construct
        timer.Simple(2, function()
            -- Mark dark room as terrain
            RemixCategoryManager.SetMaterialCategory(
                "materials/concrete/concretefloor006a.vmt",
                RemixCategoryManager.PRESET.TERRAIN
            )
        end)
    elseif mapName == "gm_flatgrass" then
        -- Specific handling for gm_flatgrass
        timer.Simple(2, function()
            -- Mark grass as terrain
            RemixCategoryManager.SetMaterialCategory(
                "materials/nature/grass.vmt",
                RemixCategoryManager.PRESET.TERRAIN
            )
        end)
    end
end)

--[[
    === TIPS AND BEST PRACTICES ===
    
    1. Use smart marking first:
       remix_smart_mark_world
       
    2. Track materials before getting hashes:
       RemixMaterial.TrackMaterial(materialName)
       
    3. Wait for materials to load:
       Use timer.Simple(2, ...) after map load
       
    4. Combine flags with bit.bor():
       local flags = bit.bor(CATEGORY.DECAL_STATIC, CATEGORY.TERRAIN)
       
    5. Cache material hashes:
       GetMaterialHash() caches automatically
       
    6. Use presets when possible:
       PRESET.WORLD_GEOMETRY instead of manual flags
       
    7. Clear categories on map change:
       remix_clear_categories
       
    8. Check if hash exists before setting:
       local hash = GetMaterialHash(mat)
       if hash then SetHashCategory(hash, flags) end
]]--

print("[RemixCategoryManager] Examples loaded. Try: example_mark_all_world, example_smart_mark, etc.")
