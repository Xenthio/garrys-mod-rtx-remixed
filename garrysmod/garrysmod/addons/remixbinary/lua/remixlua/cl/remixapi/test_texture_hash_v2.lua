if not CLIENT then return end

print("\n[Texture Hash Test V2] Loading...")

-- This test script demonstrates the NEW hook-based texture hash retrieval

concommand.Add("rtx_test_gethash_v2", function(ply, cmd, args)
    if not RemixMaterial or not RemixMaterial.GetTextureHash then
        print("[Test V2] ERROR: RemixMaterial.GetTextureHash not available!")
        return
    end
    
    local materialName = args[1] or "concrete/concretefloor001a"
    
    print(string.format("\n[Test V2] Testing material: %s", materialName))
    print("[Test V2] This uses the D3D9 SetTexture hook to track textures")
    
    -- NOTE: For the hook to capture the texture, the material must be RENDERED first!
    -- The hook captures textures as Source Engine binds them for rendering.
    
    -- Try to get the hash (will only work if material was recently rendered)
    local hash, hashStr = RemixMaterial.GetTextureHash(materialName)
    
    if hash and hash > 0 then
        -- Use the string version if available for display to avoid precision loss
        local displayHash = hashStr or string.format("0x%X", hash)
        
        print(string.format("[Test V2] ✓ SUCCESS! Hash: %s", displayHash))
        print(string.format("[Test V2]   This hash was captured from D3D9 SetTexture hook"))
        
        _G.LAST_TEXTURE_HASH = hash
        _G.LAST_TEXTURE_MATERIAL = materialName
    else
        print("[Test V2] ✗ Material not in cache")
        print("[Test V2]   The material hasn't been rendered yet!")
        print("\n[Test V2] To fix this:")
        print("  1. Look around the map until you see a surface with this material")
        print("  2. The hook will automatically capture it when Source renders it")
        print("  3. Run this command again")
        print("\n[Test V2] OR use rtx_force_track to explicitly track it")
    end
end)

-- Force track a material by rendering it
concommand.Add("rtx_force_track", function(ply, cmd, args)
    if not RemixMaterial or not RemixMaterial.TrackMaterial then
        print("[Test V2] ERROR: RemixMaterial.TrackMaterial not available!")
        return
    end
    
    local materialName = args[1] or "concrete/concretefloor001a"
    
    print(string.format("\n[Test V2] Force tracking: %s", materialName))
    
    -- This will set the material as "current" and touch it to trigger rendering
    RemixMaterial.TrackMaterial(materialName)
    
    -- Wait a frame, then try to get the hash
    timer.Simple(0.1, function()
        local hash, hashStr = RemixMaterial.GetTextureHash(materialName)
        if hash and hash > 0 then
            local displayHash = hashStr or string.format("0x%X", hash)
            print(string.format("[Test V2] ✓ Tracked! Hash: %s", displayHash))
        else
            print("[Test V2] ✗ Still not in cache - material may need to be fully rendered")
            print("[Test V2]   Try looking at a surface with this material")
        end
    end)
end)

-- Show cache stats
concommand.Add("rtx_cache_stats", function()
    if not RemixMaterial or not RemixMaterial.GetTextureHash then
        print("[Test V2] ERROR: RemixMaterial not available!")
        return
    end
    
    -- Try to get a hash to trigger cache size logging
    RemixMaterial.GetTextureHash("__dummy__")
    print("[Test V2] Check console for cache size")
end)

print("[Texture Hash Test V2] Commands loaded:")
print("  rtx_test_gethash_v2 <material_name>  - Get hash (only works if rendered)")
print("  rtx_force_track <material_name>      - Try to force material tracking")
print("  rtx_cache_stats                      - Show cache statistics")
print("\nNOTE: Materials must be RENDERED before their textures are captured!")
print("Look around the map to populate the cache, then use rtx_test_gethash_v2")

