--[[
    RTX Remix Pseudoplayer Material Auto-Categorization
    
    Automatically detects and categorizes pseudoplayer proxy materials as Player Model textures.
    The pseudoplayer addon creates proxied materials for first-person body/shadow rendering.
]]--

if not RemixMaterial or not RemixCategoryManager then
    Error("[RemixPseudoplayerCategory] RemixMaterial or RemixCategoryManager not available!\n")
    return
end

local MODULE_NAME = "RemixPseudoplayerCategory"

-- ConVar to enable/disable auto-categorization
CreateClientConVar("rtx_auto_categorize_pseudoplayer", "1", true, false, 
    "Automatically categorize pseudoplayer materials as Player Model (1 = enabled, 0 = disabled)")
CreateClientConVar("rtx_auto_categorize_pseudoweapon", "1", true, false, 
    "Automatically categorize pseudoweapon materials as Player Model (1 = enabled, 0 = disabled)")

-- Track which materials we've already processed
local processedMaterials = {}

-- Track last player model to detect changes
local lastPlayerModel = ""

-- Track last weapon to detect changes
local lastWeapon = ""
local categorizedWeapons = {}  -- Track all weapons we've already categorized

--[[
    Detect if a material is a pseudoplayer material
    Pseudoplayer materials typically have unique names or are created dynamically
]]--
local function IsPseudoplayerMaterial(matName)
    if not matName then return false end
    
    -- Check for common pseudoplayer material patterns
    -- These materials are typically player model materials that get proxied
    local lower = string.lower(matName)
    
    -- Check if it's a player model material being used by pseudoplayer
    if string.find(lower, "models/player") or 
       string.find(lower, "models/humans") or
       string.find(lower, "pseudoplayer") then
        return true
    end
    
    return false
end

--[[
    Scan and categorize pseudoplayer materials
    @param force boolean - If true, categorize even if ConVar is disabled
]]--
local function CategorizePseudoplayerMaterials(force)
    if not force and not GetConVar("rtx_auto_categorize_pseudoplayer"):GetBool() then
        return
    end
    
    local ply = LocalPlayer()
    if not IsValid(ply) then return end
    
    -- Get player model materials
    local playerModel = ply:GetModel()
    if not playerModel or playerModel == "" then return end
    
    -- Load the player model to get its materials
    local ent = ClientsideModel(playerModel)
    if not IsValid(ent) then return end
    
    local materials = ent:GetMaterials()
    ent:Remove()
    
    if not materials or #materials == 0 then return end
    
    local categorized = 0
    local categoryFlags = RemixCategoryManager.CATEGORY.THIRD_PERSON_PLAYER_MODEL -- 0x80000
    
    MsgC(Color(150, 200, 255), string.format("[%s] Scanning for pseudoplayer materials...\n", MODULE_NAME))
    
    -- Check if unique_hashes is enabled
    local uniqueHashesEnabled = GetConVar("rtx_pseudoplayer_unique_hashes")
    local useProxyMaterials = uniqueHashesEnabled and uniqueHashesEnabled:GetBool() or false
    
    -- Method 1: Categorize original materials (ONLY when unique_hashes is disabled)
    if not useProxyMaterials then
        MsgC(Color(200, 200, 255), string.format("[%s] Categorizing original materials (unique_hashes disabled)\n", MODULE_NAME))
        for _, matName in ipairs(materials) do
        if not processedMaterials[matName] then
            RemixMaterial.TrackMaterial(matName)
            
            timer.Simple(0.2, function()
                local success = RemixCategoryManager.SetMaterialCategory(matName, categoryFlags)
                
                if success then
                    processedMaterials[matName] = true
                    categorized = categorized + 1
                    
                    MsgC(Color(100, 255, 100), string.format("[%s] ✓ Marked '%s' as Player Model\n", 
                        MODULE_NAME, matName))
                end
            end)
        end
        end  -- Close for loop
    else
        MsgC(Color(200, 200, 255), string.format("[%s] Categorizing proxy materials (unique_hashes enabled)\n", MODULE_NAME))
    end
    
    -- Method 2: Detect and categorize proxy materials (ONLY when unique_hashes is enabled)
    -- These are created by the pseudoplayer addon with names like "pseudoplayermaterial1", "pseudoplayermaterial2", etc.
    if useProxyMaterials then
        timer.Simple(0.5, function()
            -- Use FindTexturesByName to search the D3D9 cache for proxy materials
            local proxyMaterials = RemixMaterial.FindTexturesByName("pseudoplayermaterial")
        
        if proxyMaterials and #proxyMaterials > 0 then
            MsgC(Color(200, 200, 255), string.format("[%s] Found %d proxy materials in D3D9 cache\n", 
                MODULE_NAME, #proxyMaterials))
            
            for _, entry in ipairs(proxyMaterials) do
                local proxyMatName = entry.name
                
                -- Skip temp materials and alpha materials - only categorize the final ones
                if not string.find(proxyMatName, "temp") and not string.find(proxyMatName, "alpha") then
                    if not processedMaterials[proxyMatName] then
                        MsgC(Color(200, 200, 255), string.format("[%s] Found proxy material: %s\n", 
                            MODULE_NAME, proxyMatName))
                        
                        timer.Simple(0.2, function()
                            local success = RemixCategoryManager.SetMaterialCategory(proxyMatName, categoryFlags)
                            
                            if success then
                                processedMaterials[proxyMatName] = true
                                categorized = categorized + 1
                                
                                MsgC(Color(100, 255, 100), string.format("[%s] ✓ Marked proxy '%s' as Player Model\n", 
                                    MODULE_NAME, proxyMatName))
                            else
                                MsgC(Color(255, 150, 100), string.format("[%s] ⚠ Could not categorize proxy '%s'\n", 
                                    MODULE_NAME, proxyMatName))
                            end
                        end)
                    end
                end
            end
        else
            MsgC(Color(200, 200, 200), string.format("[%s] No proxy materials found\n", 
                MODULE_NAME))
        end
        end)
    end
end

--[[
    Scan and categorize pseudoweapon materials
    @param force boolean - If true, categorize even if ConVar is disabled
]]--
local function CategorizePseudoweaponMaterials(force)
    if not force and not GetConVar("rtx_auto_categorize_pseudoweapon"):GetBool() then
        return
    end
    
    local categoryFlags = RemixCategoryManager.CATEGORY.THIRD_PERSON_PLAYER_MODEL -- 0x80000
    
    MsgC(Color(150, 200, 255), string.format("[%s] Scanning for pseudoweapon materials...\n", MODULE_NAME))
    
    -- Check if unique_hashes is enabled
    local uniqueHashesEnabled = GetConVar("rtx_pseudoweapon_unique_hashes")
    local useProxyMaterials = uniqueHashesEnabled and uniqueHashesEnabled:GetBool() or false
    
    if useProxyMaterials then
        MsgC(Color(200, 200, 255), string.format("[%s] Categorizing weapon proxy materials (unique_hashes enabled)\n", MODULE_NAME))
        
        -- Wait for materials to be created
        timer.Simple(0.5, function()
            -- Use FindTexturesByName to search the D3D9 cache for proxy materials
            local proxyMaterials = RemixMaterial.FindTexturesByName("pseudoweaponmaterial")
            
            if proxyMaterials and #proxyMaterials > 0 then
                MsgC(Color(200, 200, 255), string.format("[%s] Found %d weapon proxy materials in D3D9 cache\n", 
                    MODULE_NAME, #proxyMaterials))
                
                for _, entry in ipairs(proxyMaterials) do
                    local proxyMatName = entry.name
                    
                    -- Skip temp materials and alpha materials - only categorize the final ones
                    if not string.find(proxyMatName, "temp") and not string.find(proxyMatName, "alpha") then
                        if not processedMaterials[proxyMatName] then
                            timer.Simple(0.2, function()
                                -- Check if this hash is already categorized (check inside timer, after materials are ready)
                                local hashStr, hashNum = RemixCategoryManager.GetMaterialHash(proxyMatName)
                                if hashStr then
                                    local existingCategory = RemixMaterial.GetHashCategory(hashStr)
                                    if existingCategory and existingCategory == categoryFlags then
                                        -- Already categorized with the correct flags, skip
                                        processedMaterials[proxyMatName] = true  -- Mark as processed to avoid rechecking
                                        return
                                    end
                                end
                                
                                local success = RemixCategoryManager.SetMaterialCategory(proxyMatName, categoryFlags)
                                
                                if success then
                                    processedMaterials[proxyMatName] = true
                                    
                                    MsgC(Color(100, 255, 100), string.format("[%s] ✓ Marked weapon proxy '%s' as Player Model\n", 
                                        MODULE_NAME, proxyMatName))
                                else
                                    MsgC(Color(255, 150, 100), string.format("[%s] ⚠ Could not categorize weapon proxy '%s'\n", 
                                        MODULE_NAME, proxyMatName))
                                end
                            end)
                        end
                    end
                end
            else
                MsgC(Color(200, 200, 200), string.format("[%s] No weapon proxy materials found\n", 
                    MODULE_NAME))
            end
        end)
    else
        MsgC(Color(200, 200, 200), string.format("[%s] Pseudoweapon unique_hashes disabled, skipping categorization\n", MODULE_NAME))
    end
end

--[[
    Hook into player model changes
]]--
local function OnPlayerModelChanged()
    -- Wait longer for the pseudoplayer to initialize and render new materials
    timer.Simple(2, function()
        -- No need to clear cache - unique suffixes prevent collisions
        CategorizePseudoplayerMaterials()
    end)
end

-- Run on initial spawn
hook.Add("InitPostEntity", MODULE_NAME .. "_Init", function()
    timer.Simple(3, function()
        -- Set initial player model and weapon
        if IsValid(LocalPlayer()) then
            lastPlayerModel = LocalPlayer():GetModel()
            
            local weapon = LocalPlayer():GetActiveWeapon()
            if IsValid(weapon) then
                lastWeapon = weapon:GetClass()
            end
        end
        
        CategorizePseudoplayerMaterials()
        CategorizePseudoweaponMaterials()
    end)
end)

-- Run when player model changes
hook.Add("PlayerSetModel", MODULE_NAME .. "_ModelChange", OnPlayerModelChanged)

-- Detect model and weapon changes, categorize immediately
timer.Create(MODULE_NAME .. "_ModelChangeDetector", 0.5, 0, function()
    local ply = LocalPlayer()
    if not IsValid(ply) or not ply:Alive() then
        return
    end
    
    -- Check for player model changes
    if GetConVar("rtx_auto_categorize_pseudoplayer"):GetBool() then
        local currentModel = ply:GetModel()
        if currentModel ~= lastPlayerModel and lastPlayerModel ~= "" then
            -- Model changed! Wait a moment for pseudoplayer to update, then categorize
            MsgC(Color(150, 200, 255), string.format("[%s] Player model changed, auto-categorizing...\n", MODULE_NAME))
            
            timer.Simple(0.5, function()
                -- No need to clear cache - unique suffixes prevent collisions
                CategorizePseudoplayerMaterials()
            end)
        end
        
        lastPlayerModel = currentModel
    end
    
    -- Check for weapon changes
    if GetConVar("rtx_auto_categorize_pseudoweapon"):GetBool() then
        local weapon = ply:GetActiveWeapon()
        if IsValid(weapon) then
            local currentWeapon = weapon:GetClass()
            if currentWeapon ~= lastWeapon and lastWeapon ~= "" then
                -- Check if we've already categorized this weapon
                if categorizedWeapons[currentWeapon] then
                    -- Already categorized, skip silently
                    lastWeapon = currentWeapon
                    return
                end
                
                -- Weapon changed! Mark as categorized immediately to prevent duplicate scans
                categorizedWeapons[currentWeapon] = true
                
                -- Wait a moment for pseudoweapon to update, then categorize
                MsgC(Color(150, 200, 255), string.format("[%s] Weapon changed to %s, auto-categorizing...\n", MODULE_NAME, currentWeapon))
                
                timer.Simple(0.5, function()
                    CategorizePseudoweaponMaterials()
                end)
            end
            
            lastWeapon = currentWeapon
        end
    end
end)

-- Console command for manual player categorization
concommand.Add("rtx_categorize_pseudoplayer", function()
    -- Clear the D3D9 texture cache to remove old texture variants
    MsgC(Color(200, 200, 255), "[RemixPseudoplayerCategory] Clearing D3D9 texture cache...\n")
    RemixMaterial.ClearTextureCache()
    
    processedMaterials = {} -- Clear cache to reprocess
    CategorizePseudoplayerMaterials(true) -- Force categorization (will clear old hashes automatically)
end, nil, "Manually categorize pseudoplayer materials as Player Model textures")

-- Console command for manual weapon categorization
concommand.Add("rtx_categorize_pseudoweapon", function()
    -- Clear the D3D9 texture cache to remove old texture variants
    MsgC(Color(200, 200, 255), "[RemixPseudoplayerCategory] Clearing D3D9 texture cache...\n")
    RemixMaterial.ClearTextureCache()
    
    processedMaterials = {} -- Clear cache to reprocess
    CategorizePseudoweaponMaterials(true) -- Force categorization (will clear old hashes automatically)
end, nil, "Manually categorize pseudoweapon materials as Player Model textures")