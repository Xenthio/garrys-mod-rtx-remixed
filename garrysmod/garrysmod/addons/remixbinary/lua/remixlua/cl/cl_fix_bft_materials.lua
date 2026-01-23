--[[
    BlueFlyTrap PseudoPBR Material Fix
    
    This script fixes materials using the BlueFlyTrap/Galaxyi PseudoPBR technique.
    
    These materials use $blendTintByBaseAlpha + $color2 "[0 0 0]" to darken the 
    diffuse texture based on the alpha channel, which stores the metallic mask.
    
    For RTX rendering, we need to disable this tinting so the base albedo shows
    correctly - the metallic information is extracted separately from the alpha.
    
    Additionally, BFT uses channel overlay materials (_ch, _ch_r, _ch_g, _ch_b)
    for colored glow effects. These additive layers cause white overlaps in RTX
    and should be hidden.
]]

-- =========================================================================
-- Channel Overlay Detection
-- =========================================================================
-- BFT channel overlays use $additive + $selfillum for RGB glow effects.
-- They appear as white overlaps in RTX and should be hidden.

local function IsChannelOverlay(matPath)
    local pathLower = string.lower(matPath)
    
    -- Check for channel suffixes
    if string.EndsWith(pathLower, "_ch") or
       string.EndsWith(pathLower, "_ch_r") or
       string.EndsWith(pathLower, "_ch_g") or
       string.EndsWith(pathLower, "_ch_b") then
        return true
    end
    
    return false
end

local function HideChannelOverlay(mat)
    if not mat or mat:IsError() then return end
    
    -- Make the material fully transparent/invisible
    mat:SetInt("$additive", 0)
    mat:SetInt("$selfillum", 0)
    mat:SetVector("$color", Vector(0, 0, 0))
    mat:SetVector("$color2", Vector(0, 0, 0))
    mat:SetFloat("$alpha", 0)
    
    -- Try to make it not render at all
    mat:SetInt("$translucent", 1)
    mat:SetInt("$no_draw", 1)
    
    if GetConVar("developer"):GetInt() > 0 then
        print("[RTX-BFT] Hidden channel overlay: " .. mat:GetName())
    end
end

-- =========================================================================
-- BFT Tinting Fix
-- =========================================================================

local function FixBFTMaterial(mat)
    if not mat or mat:IsError() then return end
    
    -- Check for BFT pattern: $blendTintByBaseAlpha with dark $color2
    local blendTint = mat:GetInt("$blendtintbybasealpha")
    if blendTint ~= 1 then return end
    
    -- Get $color2 - if it's very dark (typically [0 0 0]), this is BFT
    local color2 = mat:GetVector("$color2")
    if not color2 then return end
    
    -- Check if color2 is very dark (sum of RGB < 0.3 - matches C++ threshold)
    local brightness = color2.x + color2.y + color2.z
    if brightness > 0.3 then return end
    
    -- This is a BFT material - disable the tinting
    mat:SetInt("$blendtintbybasealpha", 0)
    -- Optionally reset color2 to white so it doesn't affect anything
    mat:SetVector("$color2", Vector(1, 1, 1))
    
    if GetConVar("developer"):GetInt() > 0 then
        print("[RTX-BFT] Fixed material: " .. mat:GetName())
    end
end

-- =========================================================================
-- Combined Material Processing
-- =========================================================================

local function ProcessBFTMaterial(matPath)
    local mat = Material(matPath)
    if mat:IsError() then return end
    
    -- Check if this is a channel overlay that should be hidden
    if IsChannelOverlay(matPath) then
        -- Verify it has BFT channel characteristics
        local additive = mat:GetInt("$additive")
        local selfillum = mat:GetInt("$selfillum")
        
        if additive == 1 or selfillum == 1 then
            HideChannelOverlay(mat)
            return
        end
    end
    
    -- Try to fix BFT tinting
    FixBFTMaterial(mat)
end

-- Fix materials as entities are created
hook.Add("OnEntityCreated", "RTX_FixBFTMaterials", function(ent)
    if not IsValid(ent) then return end
    
    timer.Simple(0, function()
        if not IsValid(ent) then return end
        
        -- Get all materials on the entity
        local materials = ent:GetMaterials()
        for _, matPath in ipairs(materials) do
            ProcessBFTMaterial(matPath)
        end
    end)
end)

-- Console command to manually fix a material
concommand.Add("rtx_fix_bft_material", function(ply, cmd, args)
    if #args < 1 then
        print("Usage: rtx_fix_bft_material <material_path>")
        return
    end
    
    ProcessBFTMaterial(args[1])
    print("Processed BFT material: " .. args[1])
end, nil, "Fix a BlueFlyTrap PseudoPBR material for RTX rendering")

-- Console command to hide channel overlays specifically
concommand.Add("rtx_hide_bft_channel", function(ply, cmd, args)
    if #args < 1 then
        print("Usage: rtx_hide_bft_channel <material_path>")
        return
    end
    
    local mat = Material(args[1])
    if mat:IsError() then
        print("Material not found: " .. args[1])
        return
    end
    
    HideChannelOverlay(mat)
    print("Hidden BFT channel overlay: " .. args[1])
end, nil, "Hide a BlueFlyTrap channel overlay material for RTX rendering")

print("[RTX] BlueFlyTrap PseudoPBR material fix loaded (includes channel overlay hiding)")
