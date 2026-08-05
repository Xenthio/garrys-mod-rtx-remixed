if not (BRANCH == "x86-64" or BRANCH == "chromium") then return end
if not CLIENT then return end

-- RTX Remix Hash Lookup Tool
-- Allows reverse lookup: paste a Remix hash to find the Source Engine material

print("[RTX Hash Lookup] Loading...")

-- Console command to find materials by hash
concommand.Add("rtx_find_hash", function(ply, cmd, args)
    if not RemixMaterial or not RemixMaterial.FindMaterialByHash then
        MsgC(Color(255, 100, 100), "[RTX Hash Lookup] ERROR: RemixMaterial.FindMaterialByHash not available!\n")
        MsgC(Color(255, 200, 100), "[RTX Hash Lookup] Make sure the RTX binary module is loaded\n")
        return
    end
    
    if not args[1] then
        MsgC(Color(255, 200, 100), "Usage: rtx_find_hash <hash>\n")
        MsgC(Color(200, 200, 200), "Examples:\n")
        MsgC(Color(200, 200, 200), "  rtx_find_hash 0xABCD1234\n")
        MsgC(Color(200, 200, 200), "  rtx_find_hash ABCD1234\n")
        MsgC(Color(200, 200, 200), "  rtx_find_hash 12345678\n")
        return
    end
    
    local hashInput = args[1]
    
    MsgC(Color(100, 200, 255), string.format("\n[RTX Hash Lookup] Searching for hash: %s\n", hashInput))
    MsgC(Color(200, 200, 200), "[RTX Hash Lookup] This may take a moment...\n\n")
    
    -- Call the C++ function
    local materials = RemixMaterial.FindMaterialByHash(hashInput)
    
    if materials and #materials > 0 then
        MsgC(Color(100, 255, 100), string.format("\n[RTX Hash Lookup] ✓ Found %d material(s):\n", #materials))
        for i, matName in ipairs(materials) do
            MsgC(Color(255, 255, 255), string.format("  %d. %s\n", i, matName))
        end
        
        -- If only one result, offer to mark it as emissive
        if #materials == 1 then
            MsgC(Color(255, 200, 100), "\n[RTX Hash Lookup] Tip: Use 'rtx_mark_emissive " .. materials[1] .. "' to mark as emissive\n")
        end
    else
        MsgC(Color(255, 100, 100), "\n[RTX Hash Lookup] ✗ No materials found with this hash\n")
        MsgC(Color(200, 200, 200), "[RTX Hash Lookup] Possible reasons:\n")
        MsgC(Color(200, 200, 200), "  • The texture hasn't been rendered yet (look around more)\n")
        MsgC(Color(200, 200, 200), "  • The hash doesn't match any Source Engine materials\n")
        MsgC(Color(200, 200, 200), "  • The texture is from a model or prop (not tracked yet)\n")
    end
end, nil, "Find Source Engine materials by RTX Remix texture hash")

-- Console command to quickly mark a material as emissive
concommand.Add("rtx_mark_emissive", function(ply, cmd, args)
    if not RemixCategoryManager then
        MsgC(Color(255, 100, 100), "[RTX Hash Lookup] ERROR: RemixCategoryManager not loaded!\n")
        return
    end
    
    if not args[1] then
        MsgC(Color(255, 200, 100), "Usage: rtx_mark_emissive <material_name>\n")
        MsgC(Color(200, 200, 200), "Example: rtx_mark_emissive concrete/concretefloor001a\n")
        return
    end
    
    local materialName = args[1]
    local categoryFlags = RemixCategoryManager.CATEGORY.LEGACY_EMISSIVE
    
    MsgC(Color(100, 200, 255), string.format("[RTX Hash Lookup] Marking '%s' as LEGACY_EMISSIVE (0x%X)...\n", 
        materialName, categoryFlags))
    
    -- Same force-albedo wiring as the automatic BSP/tracked-material scans:
    -- without this, UnlitGeneric materials without an alpha channel stay dark
    -- since Remix has nothing to mask emission with.
    local forceAlbedoCallback = RemixCategoryManager.MakeForceAlbedoCallback(materialName, categoryFlags)
    
    RemixCategoryManager.SetMaterialCategory(materialName, categoryFlags, function(success, hash)
        if success then
            MsgC(Color(100, 255, 100), string.format("[RTX Hash Lookup] ✓ Successfully marked '%s' (hash %s) as emissive!\n", 
                materialName, hash or "unknown"))
            if forceAlbedoCallback then
                forceAlbedoCallback(success, hash)
            end
        else
            MsgC(Color(255, 100, 100), string.format("[RTX Hash Lookup] ✗ Failed to mark '%s' as emissive\n", materialName))
        end
    end)
end, nil, "Mark a material as LEGACY_EMISSIVE")

-- Console command to mark a material with any category
concommand.Add("rtx_mark_category", function(ply, cmd, args)
    if not RemixCategoryManager then
        MsgC(Color(255, 100, 100), "[RTX Hash Lookup] ERROR: RemixCategoryManager not loaded!\n")
        return
    end
    
    if not args[1] or not args[2] then
        MsgC(Color(255, 200, 100), "Usage: rtx_mark_category <material_name> <category_hex>\n")
        MsgC(Color(200, 200, 200), "Examples:\n")
        MsgC(Color(200, 200, 200), "  rtx_mark_category lights/white001 0x1000000  (LEGACY_EMISSIVE)\n")
        MsgC(Color(200, 200, 200), "  rtx_mark_category concrete/floor 0x1000      (DECAL_STATIC)\n")
        MsgC(Color(200, 200, 200), "  rtx_mark_category water/water 0x40000        (ANIMATED_WATER)\n")
        return
    end
    
    local materialName = args[1]
    local categoryFlags = tonumber(args[2])
    
    if not categoryFlags then
        MsgC(Color(255, 100, 100), "[RTX Hash Lookup] ERROR: Invalid category flags: " .. args[2] .. "\n")
        return
    end
    
    MsgC(Color(100, 200, 255), string.format("[RTX Hash Lookup] Marking '%s' with category 0x%X...\n", 
        materialName, categoryFlags))
    
    -- Same force-albedo wiring as the automatic BSP/tracked-material scans - only
    -- takes effect when categoryFlags includes LEGACY_EMISSIVE on a qualifying
    -- unlit-shader material.
    local forceAlbedoCallback = RemixCategoryManager.MakeForceAlbedoCallback(materialName, categoryFlags)
    
    RemixCategoryManager.SetMaterialCategory(materialName, categoryFlags, function(success, hash)
        if success then
            MsgC(Color(100, 255, 100), string.format("[RTX Hash Lookup] ✓ Successfully marked '%s' (hash %s) with category 0x%X!\n", 
                materialName, hash or "unknown", categoryFlags))
            if forceAlbedoCallback then
                forceAlbedoCallback(success, hash)
            end
        else
            MsgC(Color(255, 100, 100), string.format("[RTX Hash Lookup] ✗ Failed to mark '%s'\n", materialName))
        end
    end)
end, nil, "Mark a material with a specific category flag")

MsgC(Color(100, 255, 100), "[RTX Hash Lookup] Loaded successfully!\n")
MsgC(Color(200, 200, 200), "[RTX Hash Lookup] Commands:\n")
MsgC(Color(200, 200, 200), "  rtx_find_hash <hash>             - Find material by RTX Remix hash\n")
MsgC(Color(200, 200, 200), "  rtx_mark_emissive <material>     - Mark material as LEGACY_EMISSIVE\n")
MsgC(Color(200, 200, 200), "  rtx_mark_category <mat> <flags>  - Mark material with custom category\n")

