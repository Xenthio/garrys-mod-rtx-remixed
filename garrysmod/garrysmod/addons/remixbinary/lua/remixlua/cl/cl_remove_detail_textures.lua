-- Detail Texture Remover
-- This module removes detail textures from materials for RTX Remix.
--
-- This module provides ProcessMaterial() - called by the unified MaterialPipeline.
-- All material discovery is handled by RTXMaterialPipeline.

-- Wait for NikNaks to load if it hasn't already
if not NikNaks then 
    require("niknaks") 
end

-- Global module table
RTXRemoveDetail = RTXRemoveDetail or {}

local enable_addon = CreateConVar("rtx_rdt_enabled", "1", FCVAR_ARCHIVE, "Enable/disable the Detail Texture Remover")
local debug_mode = CreateConVar("rtx_rdt_debug", "0", FCVAR_ARCHIVE, "Enable debugging output")

-- The replacement texture
local replacementTexture = "rtx/ignore"

-- Keep track of modified materials to avoid reprocessing
local modifiedMaterials = {}
local detailTexturesRemoved = 0

-- Debug print function
local function DebugPrint(...)
    if debug_mode:GetBool() then
        print("[RTX RDT]", ...)
    end
end

-- Public function to process a single material (called by MaterialPipeline)
function RTXRemoveDetail.ProcessMaterial(matName)
    if not matName or matName == "" then 
        return false 
    end
    
    -- Skip already processed materials
    if modifiedMaterials[matName] ~= nil then
        return modifiedMaterials[matName]
    end
    
    local mat = Material(matName)
    if not mat or mat:IsError() then 
        modifiedMaterials[matName] = false
        return false 
    end
    
    local modified = false
    
    -- Check if this material has detail textures
    local detailTexture = mat:GetString("$detail")
    if detailTexture and detailTexture ~= "" then
        mat:SetTexture("$detail", replacementTexture)
        modified = true
        DebugPrint("Removed detail texture from: " .. matName)
    end
    
    -- Check for $detailscale
    local detailscale = mat:GetVector("$detailscale")
    if detailscale then
        mat:SetVector("$detailscale", Vector(0, 0, 0))
        modified = true
    end
    
    if modified then
        detailTexturesRemoved = detailTexturesRemoved + 1
        modifiedMaterials[matName] = true
        return true
    else
        modifiedMaterials[matName] = false
    end
    
    return false
end

-- Get statistics
function RTXRemoveDetail.GetStats()
    return {
        removed = detailTexturesRemoved,
        cached = table.Count(modifiedMaterials)
    }
end

-- Clear cache (useful on map change)
function RTXRemoveDetail.ClearCache()
    modifiedMaterials = {}
    detailTexturesRemoved = 0
end

-- Check if enabled
function RTXRemoveDetail.IsEnabled()
    return enable_addon:GetBool()
end

-- Function to force reapply to already processed materials (utility function)
function RTXRemoveDetail.ForceReapply()
    local count = 0
    for matName, hadDetail in pairs(modifiedMaterials) do
        if hadDetail then
            local mat = Material(matName)
            if mat and not mat:IsError() then
                mat:SetTexture("$detail", replacementTexture)
                local detailscale = mat:GetVector("$detailscale")
                if detailscale then
                    mat:SetVector("$detailscale", Vector(0, 0, 0))
                end
                count = count + 1
            end
        end
    end
    return count
end

-- Add command to force reapply texture replacements
concommand.Add("rtx_rdt_reapply", function()
    local count = RTXRemoveDetail.ForceReapply()
    print("[RTX RDT] Force reapplied to " .. count .. " materials")
    notification.AddLegacy("Reapplied to " .. count .. " materials", NOTIFY_GENERIC, 3)
end, nil, "Force reapply detail texture removal")

-- Add command to show stats
concommand.Add("rtx_rdt_stats", function()
    local stats = RTXRemoveDetail.GetStats()
    MsgC(Color(100, 200, 255), "[RTX RDT] Statistics:\n")
    MsgC(Color(200, 200, 200), string.format("  Detail textures removed: %d\n", stats.removed))
    MsgC(Color(200, 200, 200), string.format("  Cached entries: %d\n", stats.cached))
end, nil, "Show detail texture removal statistics")

-- Startup message
MsgC(Color(100, 255, 100), "[RTX RDT] Detail Texture Remover loaded (processing module).\n")
MsgC(Color(200, 200, 200), "  Provides RTXRemoveDetail.ProcessMaterial() for MaterialPipeline.\n")