if not CLIENT then return end
if not (BRANCH == "x86-64" or BRANCH == "chromium") then return end

RTXRemoveDetail = RTXRemoveDetail or {}

local enable_addon = CreateConVar("rtx_rdt_enabled", "1", FCVAR_ARCHIVE,
    "Completely disable Source detail textures")
local debug_mode = CreateConVar("rtx_rdt_debug", "0", FCVAR_ARCHIVE,
    "Enable detail texture disabler debug output")

-- $detail is used by the standard world/model shaders. The numbered variants
-- are used by shaders such as WorldVertexTransition and 4-way blends.
local detailTextureParameters = {
    "$detail",
    "$detail1",
    "$detail2",
}

local modifiedMaterials = {}
local detailTexturesRemoved = 0

local function DebugPrint(...)
    if not debug_mode:GetBool() then return end

    MsgC(Color(100, 200, 255), "[RTX RDT] ", Color(255, 255, 255), ...)
    MsgC(Color(255, 255, 255), "\n")
end

local function DisableMaterialDetail(matName)
    local mat = Material(matName)
    if not mat or mat:IsError() then return false end

    local removedParameters = {}

    for _, parameter in ipairs(detailTextureParameters) do
        -- GetTexture returns nil when the shader parameter does not exist.
        -- This remains reliable after Source has resolved the VMT path into an
        -- ITexture, unlike treating the parameter as a string.
        if mat:GetTexture(parameter) then
            mat:SetUndefined(parameter)
            table.insert(removedParameters, parameter)
        end
    end

    if #removedParameters == 0 then return false end

    -- The presence of a detail texture selects a static shader combination.
    -- Recompute once so Source drops that combination and no longer binds or
    -- samples the detail texture. Setting $detailscale to zero would still
    -- sample one texel and could visibly tint the material.
    mat:Recompute()

    DebugPrint(string.format("Disabled %s on '%s'",
        table.concat(removedParameters, ", "), matName))
    return true
end

function RTXRemoveDetail.ProcessMaterial(matName)
    if not matName or matName == "" then return false end

    if modifiedMaterials[matName] ~= nil then
        return modifiedMaterials[matName]
    end

    local modified = DisableMaterialDetail(matName)
    modifiedMaterials[matName] = modified

    if modified then
        detailTexturesRemoved = detailTexturesRemoved + 1
    end

    return modified
end

function RTXRemoveDetail.GetStats()
    return {
        removed = detailTexturesRemoved,
        cached = table.Count(modifiedMaterials),
    }
end

function RTXRemoveDetail.ClearCache()
    modifiedMaterials = {}
    detailTexturesRemoved = 0
end

function RTXRemoveDetail.IsEnabled()
    return enable_addon:GetBool()
end

-- Useful after mat_reloadallmaterials or while testing an already-loaded map.
function RTXRemoveDetail.ForceReapply()
    local count = 0

    for matName, hadDetail in pairs(modifiedMaterials) do
        if hadDetail and DisableMaterialDetail(matName) then
            count = count + 1
        end
    end

    return count
end

concommand.Add("rtx_rdt_reapply", function()
    local count = RTXRemoveDetail.ForceReapply()
    print(string.format("[RTX RDT] Reapplied detail disabling to %d materials", count))
end, nil, "Reapply detail texture disabling after a material reload")

concommand.Add("rtx_rdt_stats", function()
    local stats = RTXRemoveDetail.GetStats()
    MsgC(Color(100, 200, 255), "[RTX RDT] Statistics:\n")
    MsgC(Color(200, 200, 200), string.format("  Materials with detail disabled: %d\n", stats.removed))
    MsgC(Color(200, 200, 200), string.format("  Cached materials: %d\n", stats.cached))
end, nil, "Show detail texture disabler statistics")

MsgC(Color(100, 255, 100), "[RTX RDT] Detail Texture Disabler loaded.\n")
