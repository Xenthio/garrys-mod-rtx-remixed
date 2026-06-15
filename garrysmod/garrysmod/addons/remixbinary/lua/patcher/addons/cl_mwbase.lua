if not CLIENT then return end
if not RTXPatcher then return end

local Patcher = RTXPatcher

local function createClientConVar(name, default, help)
    if CreateClientConVar then
        local convar = CreateClientConVar(name, default, true, false, help)
        if convar then return convar end
    end

    if GetConVar then
        local convar = GetConVar(name)
        if convar then return convar end
    end

    return {
        GetBool = function() return tostring(default) ~= "0" end,
        GetFloat = function() return tonumber(default) or 0 end,
        GetString = function() return tostring(default) end,
        GetInt = function() return tonumber(default) or 0 end
    }
end

local cvEnabled = createClientConVar("rtx_patcher_mwbase", "1", "Enable MWBase RTX Remix compatibility patches")
local cvReticleAdsOnly = createClientConVar("rtx_patcher_mwbase_reticle_ads_only", "1", "Hide MWBase reticles outside ADS")
local cvReticleAdsThreshold = createClientConVar("rtx_patcher_mwbase_reticle_ads_threshold", "0.65", "ADS threshold for MWBase reticle fallback")
local cvReticleDepth = createClientConVar("rtx_patcher_mwbase_reticle_depth", "100", "Forward offset for MWBase reticle quads")
local cvReticleScale = createClientConVar("rtx_patcher_mwbase_reticle_scale", "1", "Scale multiplier for MWBase reticle quads")
local cvCustomization = createClientConVar("rtx_patcher_mwbase_customization", "1", "Enable MWBase customization render patch")

Patcher.MWBase = Patcher.MWBase or {}
local MWBase = Patcher.MWBase

local function resolveStoredEntity(className)
    if not scripted_ents then return nil end

    local stored = scripted_ents.GetStored and scripted_ents.GetStored(className)
    if type(stored) == "table" and type(stored.t) == "table" then return stored.t end
    if type(stored) == "table" then return stored end

    local direct = scripted_ents.Get and scripted_ents.Get(className)
    if type(direct) == "table" and type(direct.t) == "table" then return direct.t end
    if type(direct) == "table" then return direct end

    return nil
end

function MWBase.ResolveViewModelTable()
    local viewmodel = resolveStoredEntity("mg_viewmodel")
    if type(viewmodel) == "table" and type(viewmodel.Draw) == "function" then
        return viewmodel
    end
    return nil
end

function MWBase.IsReady()
    if type(MW_ATTS) ~= "table" then return false, "MW_ATTS missing" end
    if type(MW_ATTS.att_sight_reticle) ~= "table" then return false, "att_sight_reticle missing" end
    if type(MW_ATTS.att_sight_reticle.DoReticleStencil) ~= "function" then return false, "DoReticleStencil missing" end
    if not weapons or type(weapons.GetStored) ~= "function" or not weapons.GetStored("mg_base") then return false, "mg_base missing" end

    local viewmodel = MWBase.ResolveViewModelTable()
    if not viewmodel then return false, "mg_viewmodel missing" end

    return true, {
        viewmodel = viewmodel
    }
end

local function isBasedOnSightReticle(className, attachment)
    if className == "att_sight_reticle" then return true end
    if mw_utils and type(mw_utils.IsAttachmentBasedOn) == "function" then
        local ok, based = pcall(mw_utils.IsAttachmentBasedOn, className, "att_sight_reticle")
        if ok and based then return true end
    end
    return type(attachment) == "table" and type(attachment.DoReticleStencil) == "function" and attachment.Reticle ~= nil
end

function MWBase.PatchReticleAttachment(className, attachment)
    if type(attachment) ~= "table" or type(attachment.DoReticleStencil) ~= "function" then return false end
    if not isBasedOnSightReticle(className, attachment) then return false end
    return Patcher.InterceptFunction(attachment, "DoReticleStencil", {
        patchId = "mwbase",
        id = "reticle_" .. tostring(className),
        around = function(original, ...)
            return MWBase.DrawReticleStencil(original, ...)
        end
    })
end

function MWBase.PatchAllReticleAttachments()
    for className, attachment in pairs(MW_ATTS or {}) do
        MWBase.PatchReticleAttachment(className, attachment)
    end
end

function MWBase.DrawReticleStencil(original, self, model, reticle, weapon)
    if not reticle or not weapon or not model or not IsValid(model) then
        return original(self, model, reticle, weapon)
    end

    if cvReticleAdsOnly:GetBool() then
        local aimDelta = type(weapon.GetAimDelta) == "function" and weapon:GetAimDelta() or 0
        if aimDelta < cvReticleAdsThreshold:GetFloat() then
            model:DrawModel()
            return
        end
    end

    if IsValid(GetViewEntity()) then
        if CurTime() < GetViewEntity():GetNWFloat("MW19_EMPEffect", CurTime()) then
            model:DrawModel()
            return
        end
    end

    render.SetStencilWriteMask(0xFF)
    render.SetStencilTestMask(0xFF)
    render.SetStencilReferenceValue(0)
    render.SetStencilCompareFunction(STENCIL_ALWAYS)
    render.SetStencilPassOperation(STENCIL_REPLACE)
    render.SetStencilFailOperation(STENCIL_KEEP)
    render.SetStencilZFailOperation(STENCIL_KEEP)
    render.SetStencilEnable(true)
    render.SetStencilReferenceValue((MWBASE_STENCIL_REFVALUE or 0) + 1)

    if self.Reticle and self.Reticle.Squash ~= nil and type(weapon.GetAimDelta) == "function" then
        model:ManipulateBoneScale(0, Vector(Lerp(weapon:GetAimDelta(), 1, self.Reticle.Squash), 1, 1))
    end

    model:DrawModel()
    render.SetStencilCompareFunction(STENCIL_EQUAL)

    if not mw_utils or type(mw_utils.GetFastAttachment) ~= "function" or not self.m_Model then
        render.SetStencilEnable(false)
        render.ClearStencil()
        return original(self, model, reticle, weapon)
    end

    local att = mw_utils.GetFastAttachment(self.m_Model, reticle.Attachment)
    if not att then
        render.SetStencilEnable(false)
        render.ClearStencil()
        return
    end

    local viewmodel = type(weapon.GetViewModel) == "function" and weapon:GetViewModel() or nil
    local fovMult = IsValid(viewmodel) and viewmodel.m_AdsFovMult or 1
    local size = reticle.Size * fovMult * cvReticleScale:GetFloat()
    local color = reticle.Color

    render.SetMaterial(reticle.Material)

    local offset = att.Ang:Forward() * cvReticleDepth:GetFloat()
    if reticle.Offset ~= nil then
        offset = offset + att.Ang:Right() * reticle.Offset.x
        offset = offset + att.Ang:Up() * reticle.Offset.y
    end

    render.DrawQuadEasy(att.Pos + offset, att.Ang:Forward():GetNegated(), size * 0.01, size * 0.01, color, -att.Ang.r + 180)

    render.SetStencilEnable(false)
    render.ClearStencil()
end

function MWBase.ApplyCustomizationPatch(context)
    if not cvCustomization:GetBool() then return true end
    if not context or not context.viewmodel then return false, "missing viewmodel context" end

    local unpackValues = unpack or table.unpack
    local okDraw, drawErr = Patcher.InterceptFunction(context.viewmodel, "Draw", {
        patchId = "mwbase",
        id = "customization_draw",
        around = function(original, self, flags)
            if not IsValid(MW_CUSTOMIZEMENU) then
                return original(self, flags)
            end

            local menu = MW_CUSTOMIZEMENU
            MW_CUSTOMIZEMENU = nil

            local results = { pcall(original, self, flags) }
            MW_CUSTOMIZEMENU = menu

            local ok = table.remove(results, 1)
            if not ok then
                error(results[1], 0)
            end

            return unpackValues(results)
        end
    })

    if not okDraw then return false, drawErr end

    if type(context.viewmodel.ViewBlur) == "function" then
        local okBlur, blurErr = Patcher.InterceptFunction(context.viewmodel, "ViewBlur", {
            patchId = "mwbase",
            id = "customization_blur",
            around = function(original, self, ...)
                local weapon = type(self.GetOwner) == "function" and self:GetOwner() or nil
                if IsValid(MW_CUSTOMIZEMENU) and IsValid(weapon) and type(weapon.HasFlag) == "function" and weapon:HasFlag("Customizing") then
                    self.LerpBlur = 0
                    return
                end

                return original(self, ...)
            end
        })

        if not okBlur then return false, blurErr end
    end

    return true
end

function MWBase.GetModeSummary()
    return {
        enabled = cvEnabled:GetBool(),
        reticleMode = cvReticleAdsOnly:GetBool() and "ADS-only" or "hipfire-preserve",
        reticleAdsThreshold = cvReticleAdsThreshold:GetFloat(),
        reticleDepth = cvReticleDepth:GetFloat(),
        reticleScale = cvReticleScale:GetFloat(),
        customization = cvCustomization:GetBool()
    }
end

Patcher.RegisterPatch({
    id = "mwbase",
    label = "Modern Warfare Base",
    side = "client",
    enabled = function() return cvEnabled:GetBool() end,
    depends = MWBase.IsReady,
    apply = function(_, _, context)
        MWBase.PatchAllReticleAttachments()
        Patcher.InterceptHook("OnBuildCustomizedGun", "mwbase_patch_reticle_instances", function(weapon)
            if not IsValid(weapon) or type(weapon.GetAllAttachmentsInUse) ~= "function" then return end
            for _, attachment in pairs(weapon:GetAllAttachmentsInUse() or {}) do
                if type(attachment) == "table" and attachment.ClassName then
                    MWBase.PatchReticleAttachment("instance_" .. attachment.ClassName .. "_" .. tostring(attachment), attachment)
                end
            end
        end, { patchId = "mwbase" })
        MWBase.ApplyCustomizationPatch(context)
        Patcher.State.mwbase = MWBase.GetModeSummary()
    end,
    retryInterval = 0.5,
    maxAttempts = 120
})

return MWBase
