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
local cvOptics = createClientConVar("rtx_patcher_mwbase_optics", "1", "Enable MWBase optic render patch")
local cvOpticSimpleRender = createClientConVar("rtx_patcher_mwbase_optic_simple_render", "1", "Use a simplified Remix-friendly MWBase optic render path")
local cvOpticAdsThreshold = createClientConVar("rtx_patcher_mwbase_optic_ads_threshold", "0.9", "ADS threshold for simplified MWBase optic rendering")
local cvOpticLensOverride = createClientConVar("rtx_patcher_mwbase_optic_lens_override", "1", "Hide MWBase optic lens materials that render opaque in Remix")
local cvOpticReticle = createClientConVar("rtx_patcher_mwbase_optic_reticle", "1", "Draw MWBase optic reticles in the simplified optic path")
local cvOpticReticleDepth = createClientConVar("rtx_patcher_mwbase_optic_reticle_depth", "2", "Forward offset for simplified MWBase optic reticles")
local cvOpticReticleScale = createClientConVar("rtx_patcher_mwbase_optic_reticle_scale", "0.1", "Scale multiplier for simplified MWBase optic reticles")
local cvOpticReticleMaxSize = createClientConVar("rtx_patcher_mwbase_optic_reticle_max_size", "4", "Maximum world quad size for simplified MWBase optic reticles")
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

local function isBasedOnAttachment(className, baseClassName)
    if className == baseClassName then return true end
    if mw_utils and type(mw_utils.IsAttachmentBasedOn) == "function" then
        local ok, based = pcall(mw_utils.IsAttachmentBasedOn, className, baseClassName)
        if ok and based then return true end
    end
    return false
end

local function isBasedOnSightReticle(className, attachment)
    if isBasedOnAttachment(className, "att_sight_reticle") then return true end
    return type(attachment) == "table" and type(attachment.DoReticleStencil) == "function" and attachment.Reticle ~= nil
end

local function isOpticAttachment(className, attachment)
    if isBasedOnAttachment(className, "att_optic") then return true end
    return type(attachment) == "table" and type(attachment.Optic) == "table"
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

function MWBase.PatchOpticAttachment(className, attachment)
    if not cvOptics:GetBool() then return false end
    if type(attachment) ~= "table" or type(attachment.Render) ~= "function" then return false end
    if not isOpticAttachment(className, attachment) then return false end
    return Patcher.InterceptFunction(attachment, "Render", {
        patchId = "mwbase",
        id = "optic_" .. tostring(className),
        around = function(original, ...)
            return MWBase.DrawOpticRender(original, ...)
        end
    })
end

function MWBase.PatchAllReticleAttachments()
    for className, attachment in pairs(MW_ATTS or {}) do
        MWBase.PatchReticleAttachment(className, attachment)
    end
end

function MWBase.PatchAllOpticAttachments()
    for className, attachment in pairs(MW_ATTS or {}) do
        MWBase.PatchOpticAttachment(className, attachment)
    end
end

local function getWeaponValue(weapon, methodName, fallback)
    if weapon and type(weapon[methodName]) == "function" then
        return weapon[methodName](weapon)
    end
    return fallback
end

local transparentLensMaterial
local function getTransparentLensMaterial()
    if transparentLensMaterial then return transparentLensMaterial end
    if not CreateMaterial then return nil end

    transparentLensMaterial = CreateMaterial("rtx_patcher_mwbase_transparent_lens", "UnlitGeneric", {
        ["$basetexture"] = "color/white",
        ["$translucent"] = "1",
        ["$alpha"] = "0",
        ["$vertexalpha"] = "1",
        ["$color"] = "[0 0 0]",
    })

    return transparentLensMaterial
end

local function normalizedMaterialName(name)
    return string.Trim(string.lower(tostring(name or "")))
end

local function isOpticLensMaterialName(name)
    local materialName = normalizedMaterialName(name)
    if materialName == "" then return false end
    if string.find(materialName, "reticle", 1, true) then return false end
    if string.find(materialName, "icon", 1, true) then return false end

    return string.find(materialName, "lens", 1, true) ~= nil
        or string.find(materialName, "lense", 1, true) ~= nil
        or string.find(materialName, "scope_lens", 1, true) ~= nil
        or string.find(materialName, "4x_lens", 1, true) ~= nil
end

function MWBase.OverrideOpticLensMaterials(attachment, enabled)
    if type(attachment) ~= "table" or not attachment.m_Model or not IsValid(attachment.m_Model) then return end
    if type(attachment.m_Model.GetMaterials) ~= "function" or type(attachment.m_Model.SetSubMaterial) ~= "function" then return end

    attachment._RTXPatcherLensSlots = attachment._RTXPatcherLensSlots or {}

    if not enabled then
        for slot, previousMaterial in pairs(attachment._RTXPatcherLensSlots) do
            attachment.m_Model:SetSubMaterial(slot, previousMaterial or "")
        end
        return
    end

    local transparentMaterial = getTransparentLensMaterial()
    if not transparentMaterial then return end

    for index, materialName in ipairs(attachment.m_Model:GetMaterials() or {}) do
        if isOpticLensMaterialName(materialName) then
            local slot = index - 1
            if attachment._RTXPatcherLensSlots[slot] == nil then
                attachment._RTXPatcherLensSlots[slot] = type(attachment.m_Model.GetSubMaterial) == "function"
                    and attachment.m_Model:GetSubMaterial(slot)
                    or ""
            end
            attachment.m_Model:SetSubMaterial(slot, "!rtx_patcher_mwbase_transparent_lens")
        end
    end
end

function MWBase.IsOpticAiming(weapon)
    local aimDelta = getWeaponValue(weapon, "GetAimDelta", 0)
    local aimModeDelta = getWeaponValue(weapon, "GetAimModeDelta", 0)
    local tacStanceDelta = getWeaponValue(weapon, "GetTacStanceDelta", 0)

    return aimDelta > cvOpticAdsThreshold:GetFloat()
        and aimModeDelta < 0.5
        and tacStanceDelta < 0.5
end

function MWBase.SetOpticLensBodygroup(attachment, open)
    if type(attachment) ~= "table" or type(attachment.Optic) ~= "table" then return end
    if not attachment.m_Model or not IsValid(attachment.m_Model) then return end
    if type(attachment.m_Model.FindBodygroupByName) ~= "function" or type(attachment.m_Model.SetBodygroup) ~= "function" then return end

    local lensBodygroup = attachment.Optic.LensBodygroup
    if lensBodygroup == nil then return end

    local index = attachment.m_Model:FindBodygroupByName(lensBodygroup)
    if not index or index < 0 then return end

    attachment.m_Model:SetBodygroup(index, open and 0 or 1)
end

function MWBase.DrawOpticReticle(attachment, reticle, weapon)
    if not cvOpticReticle:GetBool() then return end
    if not attachment or not reticle then return end
    if not reticle.Material or not reticle.Size then return end
    if not mw_utils or type(mw_utils.GetFastAttachment) ~= "function" or not attachment.m_Model then return end

    local att = mw_utils.GetFastAttachment(attachment.m_Model, reticle.Attachment)
    if not att then return end

    local viewmodel = weapon and type(weapon.GetViewModel) == "function" and weapon:GetViewModel() or nil
    local fovMult = IsValid(viewmodel) and viewmodel.m_AdsFovMult or 1
    local quadSize = reticle.Size * fovMult * cvOpticReticleScale:GetFloat() * 0.01
    local maxSize = cvOpticReticleMaxSize:GetFloat()
    if maxSize > 0 then
        quadSize = math.min(quadSize, maxSize)
    end

    local color = reticle.Color or color_white
    render.SetMaterial(reticle.Material)

    local offset = att.Ang:Forward() * cvOpticReticleDepth:GetFloat()
    if reticle.Offset ~= nil then
        offset = offset + att.Ang:Right() * reticle.Offset.x
        offset = offset + att.Ang:Up() * reticle.Offset.y
    end

    render.DrawQuadEasy(att.Pos + offset, att.Ang:Forward():GetNegated(), quadSize, quadSize, color, -att.Ang.r + 180)
end

function MWBase.DrawOpticRender(original, self, weapon, model)
    if not cvOptics:GetBool() or not cvOpticSimpleRender:GetBool() then
        MWBase.OverrideOpticLensMaterials(self, false)
        return original(self, weapon, model)
    end

    if type(self) ~= "table" or type(self.Optic) ~= "table" or not self.m_Model or not IsValid(self.m_Model) then
        return original(self, weapon, model)
    end

    if type(self.m_Model.DrawModel) ~= "function" then
        return original(self, weapon, model)
    end

    if not MWBase.IsOpticAiming(weapon) then
        MWBase.OverrideOpticLensMaterials(self, false)
        return original(self, weapon, model)
    end

    MWBase.SetOpticLensBodygroup(self, true)
    MWBase.OverrideOpticLensMaterials(self, cvOpticLensOverride:GetBool())
    self.m_bRemovedRT = false
    self.m_Model:DrawModel()
    MWBase.DrawOpticReticle(self, self.Reticle, weapon)
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
        optics = cvOptics:GetBool(),
        opticRenderMode = cvOpticSimpleRender:GetBool() and "simple" or "original",
        opticAdsThreshold = cvOpticAdsThreshold:GetFloat(),
        opticLensOverride = cvOpticLensOverride:GetBool(),
        opticReticle = cvOpticReticle:GetBool(),
        opticReticleDepth = cvOpticReticleDepth:GetFloat(),
        opticReticleScale = cvOpticReticleScale:GetFloat(),
        opticReticleMaxSize = cvOpticReticleMaxSize:GetFloat(),
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
        MWBase.PatchAllOpticAttachments()
        Patcher.InterceptHook("OnBuildCustomizedGun", "mwbase_patch_reticle_instances", function(weapon)
            if not IsValid(weapon) or type(weapon.GetAllAttachmentsInUse) ~= "function" then return end
            for _, attachment in pairs(weapon:GetAllAttachmentsInUse() or {}) do
                if type(attachment) == "table" and attachment.ClassName then
                    MWBase.PatchReticleAttachment("instance_" .. attachment.ClassName .. "_" .. tostring(attachment), attachment)
                    MWBase.PatchOpticAttachment("instance_" .. attachment.ClassName .. "_" .. tostring(attachment), attachment)
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
