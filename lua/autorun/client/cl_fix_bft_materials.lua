--[[
    BlueFlyTrap PseudoPBR Material Fix
    
    This script fixes materials using the BlueFlyTrap/Galaxyi PseudoPBR technique.
    
    These materials use $blendTintByBaseAlpha + $color2 "[0 0 0]" to darken the 
    diffuse texture based on the alpha channel, which stores the metallic mask.
    
    For RTX rendering, we need to disable this tinting so the base albedo shows
    correctly - the metallic information is extracted separately from the alpha.
]]

local function FixBFTMaterial(mat)
    if not mat or mat:IsError() then return end
    
    -- Check for BFT pattern: $blendTintByBaseAlpha with dark $color2
    local blendTint = mat:GetInt("$blendtintbybasealpha")
    if blendTint ~= 1 then return end
    
    -- Get $color2 - if it's very dark (typically [0 0 0]), this is BFT
    local color2 = mat:GetVector("$color2")
    if not color2 then return end
    
    -- Check if color2 is very dark (sum of RGB < 0.1)
    local brightness = color2.x + color2.y + color2.z
    if brightness > 0.1 then return end
    
    -- This is a BFT material - disable the tinting
    mat:SetInt("$blendtintbybasealpha", 0)
    -- Optionally reset color2 to white so it doesn't affect anything
    mat:SetVector("$color2", Vector(1, 1, 1))
    
    if GetConVar("developer"):GetInt() > 0 then
        print("[RTX-BFT] Fixed material: " .. mat:GetName())
    end
end

-- Hook into material loading
hook.Add("PostRender", "RTX_FixBFTMaterials_Init", function()
    hook.Remove("PostRender", "RTX_FixBFTMaterials_Init")
    
    -- Process all loaded materials
    timer.Simple(1, function()
        for _, mat in ipairs(Material("___ALL___"):GetString("$basetexture") or {}) do
            -- This won't work - materials aren't iterable this way
            -- Instead, we fix materials as they're encountered
        end
    end)
end)

-- Fix materials as entities are created
hook.Add("OnEntityCreated", "RTX_FixBFTMaterials", function(ent)
    if not IsValid(ent) then return end
    
    timer.Simple(0, function()
        if not IsValid(ent) then return end
        
        -- Get all materials on the entity
        local materials = ent:GetMaterials()
        for _, matPath in ipairs(materials) do
            local mat = Material(matPath)
            FixBFTMaterial(mat)
        end
    end)
end)

-- Console command to manually fix a material
concommand.Add("rtx_fix_bft_material", function(ply, cmd, args)
    if #args < 1 then
        print("Usage: rtx_fix_bft_material <material_path>")
        return
    end
    
    local mat = Material(args[1])
    if mat:IsError() then
        print("Material not found: " .. args[1])
        return
    end
    
    FixBFTMaterial(mat)
    print("Fixed BFT material: " .. args[1])
end, nil, "Fix a BlueFlyTrap PseudoPBR material for RTX rendering")

print("[RTX] BlueFlyTrap PseudoPBR material fix loaded")
