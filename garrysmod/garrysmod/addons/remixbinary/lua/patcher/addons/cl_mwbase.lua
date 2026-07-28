if not CLIENT then return end
if not RTXPatcher then return end

local Patcher = RTXPatcher

local projectedReticleOverlayBounds
local submitExplicitReticleOverlay

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
local cvReticleRasterOverlay = createClientConVar("rtx_patcher_mwbase_reticle_raster_overlay", "1", "Mark MWBase reticle textures as post-RTX raster overlays")
local cvReticleRasterOverlayDebug = createClientConVar("rtx_patcher_mwbase_reticle_raster_overlay_debug", "0", "Print MWBase reticle raster overlay diagnostics")
local cvReticleExplicitOverlay = createClientConVar("rtx_patcher_mwbase_reticle_explicit_overlay", "1", "Submit MWBase reticles as explicit post-RTX raster overlay quads")
local cvReticleExplicitOverlayScale = createClientConVar("rtx_patcher_mwbase_reticle_explicit_overlay_scale", "1", "Screen-space scale for explicit MWBase reticle overlay quads")
local cvReticleExplicitOverlayClip = createClientConVar("rtx_patcher_mwbase_reticle_explicit_overlay_clip", "1", "Clip explicit MWBase reticle overlay quads to an ellipse")
local cvReticleExplicitOverlayClipScale = createClientConVar("rtx_patcher_mwbase_reticle_explicit_overlay_clip_scale", "1", "Ellipse clip scale for explicit MWBase reticle overlay quads")
local cvReticleExplicitOverlayClipScaleX = createClientConVar("rtx_patcher_mwbase_reticle_explicit_overlay_clip_scale_x", "1", "Horizontal ellipse clip scale for explicit MWBase reticle overlay quads")
local cvReticleExplicitOverlayClipScaleY = createClientConVar("rtx_patcher_mwbase_reticle_explicit_overlay_clip_scale_y", "1", "Vertical ellipse clip scale for explicit MWBase reticle overlay quads")
local cvReticleExplicitOverlayClipOffsetX = createClientConVar("rtx_patcher_mwbase_reticle_explicit_overlay_clip_offset_x", "0", "Horizontal ellipse clip offset as a fraction of clip width")
local cvReticleExplicitOverlayClipOffsetY = createClientConVar("rtx_patcher_mwbase_reticle_explicit_overlay_clip_offset_y", "0", "Vertical ellipse clip offset as a fraction of clip height")
local cvReticleExplicitOverlayClipSource = createClientConVar("rtx_patcher_mwbase_reticle_explicit_overlay_clip_source", "reticle", "Clip source for explicit MWBase reticle overlays: reticle or model")
local cvReticleExplicitOverlayStencil = createClientConVar("rtx_patcher_mwbase_reticle_explicit_overlay_stencil", "1", "Mask explicit MWBase reticle overlays with the active Source stencil")
local cvReticleExplicitOverlayStencilClear = createClientConVar("rtx_patcher_mwbase_reticle_explicit_overlay_stencil_clear", "1", "Clear the active Source stencil after drawing stencil-masked explicit MWBase reticle overlays")
local cvReticleExplicitOverlayStencilRefMode = createClientConVar("rtx_patcher_mwbase_reticle_explicit_overlay_stencil_ref_mode", "auto", "Explicit MWBase reticle overlay stencil reference mode: auto, reticle, or lens")
local cvOptics = createClientConVar("rtx_patcher_mwbase_optics", "1", "Enable MWBase optic render patch")
local cvOpticSimpleRender = createClientConVar("rtx_patcher_mwbase_optic_simple_render", "1", "Use a simplified Remix-friendly MWBase optic render path")
local cvOpticAdsThreshold = createClientConVar("rtx_patcher_mwbase_optic_ads_threshold", "0.9", "ADS threshold for simplified MWBase optic rendering")
local cvOpticLensOverride = createClientConVar("rtx_patcher_mwbase_optic_lens_override", "1", "Hide MWBase optic lens materials that render opaque in Remix")
local cvOpticLensDebug = createClientConVar("rtx_patcher_mwbase_optic_lens_debug", "0", "Print MWBase optic lens material override diagnostics")
local cvOpticReticle = createClientConVar("rtx_patcher_mwbase_optic_reticle", "1", "Draw MWBase optic reticles in the simplified optic path")
local cvOpticReticleDepth = createClientConVar("rtx_patcher_mwbase_optic_reticle_depth", "2", "Forward offset for simplified MWBase optic reticles")
local cvOpticReticleScale = createClientConVar("rtx_patcher_mwbase_optic_reticle_scale", "0.1", "Scale multiplier for simplified MWBase optic reticles")
local cvOpticReticleMaxSize = createClientConVar("rtx_patcher_mwbase_optic_reticle_max_size", "4", "Maximum world quad size for simplified MWBase optic reticles")
local cvOpticViewSurface = createClientConVar("rtx_patcher_mwbase_optic_view_surface", "0", "Submit a raytraced render-view camera for MWBase magnified optics")
local cvOpticViewSurfaceDebug = createClientConVar("rtx_patcher_mwbase_optic_view_surface_debug", "0", "Print MWBase optic render-view diagnostics")
local cvOpticViewSurfaceUseOpticFov = createClientConVar("rtx_patcher_mwbase_optic_view_surface_use_optic_fov", "1", "Use MWBase optic FOV values for render-view optic cameras")
local cvOpticViewSurfaceFov = createClientConVar("rtx_patcher_mwbase_optic_view_surface_fov", "15", "Fallback vertical FOV for MWBase render-view optic cameras")
local cvOpticViewSurfaceFovScale = createClientConVar("rtx_patcher_mwbase_optic_view_surface_fov_scale", "1", "Scale applied to the MWBase render-view optic camera FOV")
local cvOpticViewSurfaceAspect = createClientConVar("rtx_patcher_mwbase_optic_view_surface_aspect", "1", "Aspect ratio for MWBase render-view optic camera")
local cvOpticViewSurfaceNear = createClientConVar("rtx_patcher_mwbase_optic_view_surface_near", "1", "Near plane for MWBase render-view optic camera")
local cvOpticViewSurfaceFar = createClientConVar("rtx_patcher_mwbase_optic_view_surface_far", "16384", "Far plane for MWBase render-view optic camera")
local cvOpticViewSurfaceReticleOrigin = createClientConVar("rtx_patcher_mwbase_optic_view_surface_reticle_origin", "0", "Start MWBase render-view optic cameras from the optic reticle attachment")
local cvOpticViewSurfaceReticleOriginForward = createClientConVar("rtx_patcher_mwbase_optic_view_surface_reticle_origin_forward", "8", "Forward offset from the MWBase optic reticle attachment for render-view optic cameras")
local cvOpticViewSurfaceOffsetForward = createClientConVar("rtx_patcher_mwbase_optic_view_surface_offset_forward", "0", "Forward offset for MWBase render-view optic camera")
local cvOpticViewSurfaceOffsetRight = createClientConVar("rtx_patcher_mwbase_optic_view_surface_offset_right", "0", "Right offset for MWBase render-view optic camera")
local cvOpticViewSurfaceOffsetUp = createClientConVar("rtx_patcher_mwbase_optic_view_surface_offset_up", "0", "Up offset for MWBase render-view optic camera")
local cvCustomization = createClientConVar("rtx_patcher_mwbase_customization", "1", "Enable MWBase customization render patch")

Patcher.MWBase = Patcher.MWBase or {}
local MWBase = Patcher.MWBase
local markReticleMaterialForRasterOverlay

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

local reticleStencilMaskMaterial
local function getReticleStencilMaskMaterial()
    if reticleStencilMaskMaterial then return reticleStencilMaskMaterial end
    if not CreateMaterial then return nil end

    reticleStencilMaskMaterial = CreateMaterial("rtx_patcher_mwbase_stencil_mask", "UnlitGeneric", {
        ["$basetexture"] = "color/white",
        ["$model"] = "1",
        ["$translucent"] = "0",
        ["$alpha"] = "1",
    })

    return reticleStencilMaskMaterial
end

local function drawReticleStencilMaskModel(model)
    local stencilMaskMaterial = getReticleStencilMaskMaterial()
    local canOverrideMaterial = render and type(render.ModelMaterialOverride) == "function" and stencilMaskMaterial ~= nil
    local canDisableColorWrite = render and type(render.OverrideColorWriteEnable) == "function"
    local canSuppressBlend = render and type(render.SetBlend) == "function"

    if canOverrideMaterial then
        render.ModelMaterialOverride(stencilMaskMaterial)
    end

    if canDisableColorWrite then
        render.OverrideColorWriteEnable(true, false)
    elseif canSuppressBlend then
        render.SetBlend(0)
    end

    local ok, err = pcall(function()
        model:DrawModel()
    end)

    if canDisableColorWrite then
        render.OverrideColorWriteEnable(false, true)
    elseif canSuppressBlend then
        render.SetBlend(1)
    end

    if canOverrideMaterial then
        render.ModelMaterialOverride(nil, nil)
    end

    if not ok then
        error(err, 0)
    end
end

local function normalizedMaterialName(name)
    local materialName = string.lower(tostring(name or ""))
    if string.Trim then
        return string.Trim(materialName)
    end
    return string.match(materialName, "^%s*(.-)%s*$") or materialName
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

local function debugOpticLens(...)
    if not cvOpticLensDebug:GetBool() then return end

    local parts = {}
    for i = 1, select("#", ...) do
        parts[#parts + 1] = tostring(select(i, ...))
    end
    print("[RTXPatcher][MWBase OpticLens] " .. table.concat(parts, " "))
end

local function debugReticleOverlay(...)
    if not cvReticleRasterOverlayDebug:GetBool() then return end

    local parts = {}
    for i = 1, select("#", ...) do
        parts[#parts + 1] = tostring(select(i, ...))
    end

    print("[RTXPatcher][MWBase ReticleOverlay] " .. table.concat(parts, " "))
end

local function modelCanListMaterials(model)
    return model and IsValid(model) and type(model.GetMaterials) == "function"
end

local function addOpticModel(models, seen, label, model)
    if not modelCanListMaterials(model) or seen[model] then return end
    seen[model] = true
    models[#models + 1] = {
        label = label,
        model = model
    }
end

local function collectOpticModels(attachment)
    local models = {}
    local seen = {}

    if type(attachment) ~= "table" then return models end

    addOpticModel(models, seen, "m_Model", attachment.m_Model)
    addOpticModel(models, seen, "hideModel", attachment.hideModel)

    if attachment.m_Model and type(attachment.m_Model.GetChildren) == "function" then
        for index, child in ipairs(attachment.m_Model:GetChildren() or {}) do
            addOpticModel(models, seen, "m_Model child " .. tostring(index), child)
        end
    end

    if attachment.hideModel and type(attachment.hideModel.GetChildren) == "function" then
        for index, child in ipairs(attachment.hideModel:GetChildren() or {}) do
            addOpticModel(models, seen, "hideModel child " .. tostring(index), child)
        end
    end

    return models
end

local function collectOpticLensSlots(model)
    local slots = {}

    if not modelCanListMaterials(model) then return slots end

    for index, materialName in ipairs(model:GetMaterials() or {}) do
        if isOpticLensMaterialName(materialName) then
            slots[#slots + 1] = {
                slot = index - 1,
                materialName = materialName
            }
        end
    end

    return slots
end

local function restoreOpticLensMaterials(attachment)
    if type(attachment) ~= "table" or type(attachment._RTXPatcherLensSlots) ~= "table" then return 0 end

    local restored = 0
    for model, state in pairs(attachment._RTXPatcherLensSlots) do
        if model and IsValid(model) and type(model.SetSubMaterial) == "function" and type(state) == "table" then
            for slot, previousMaterial in pairs(state.slots or {}) do
                model:SetSubMaterial(slot, previousMaterial or "")
                restored = restored + 1
            end
            debugOpticLens("restored", state.label or "model", restored, "slot(s)")
        end
    end

    attachment._RTXPatcherLensSlots = nil
    return restored
end

local function overrideOpticLensModel(attachment, label, model)
    if not modelCanListMaterials(model) or type(model.SetSubMaterial) ~= "function" then return 0 end

    local transparentMaterial = getTransparentLensMaterial()
    if not transparentMaterial then return 0 end

    local slots = collectOpticLensSlots(model)
    if #slots == 0 then return 0 end

    attachment._RTXPatcherLensSlots = attachment._RTXPatcherLensSlots or {}
    local state = attachment._RTXPatcherLensSlots[model]
    if not state then
        state = {
            label = label,
            slots = {}
        }
        attachment._RTXPatcherLensSlots[model] = state
    end

    for _, entry in ipairs(slots) do
        if state.slots[entry.slot] == nil then
            state.slots[entry.slot] = type(model.GetSubMaterial) == "function"
                and model:GetSubMaterial(entry.slot)
                or ""
        end
        model:SetSubMaterial(entry.slot, "!rtx_patcher_mwbase_transparent_lens")
        debugOpticLens("override", label, "slot", entry.slot, entry.materialName)
    end

    return #slots
end

function MWBase.OverrideOpticLensMaterials(attachment, enabled)
    if not enabled then
        return restoreOpticLensMaterials(attachment)
    end

    local overridden = 0
    for _, entry in ipairs(collectOpticModels(attachment)) do
        overridden = overridden + overrideOpticLensModel(attachment, entry.label, entry.model)
    end

    if Patcher.State then
        Patcher.State.mwbaseOpticLens = {
            enabled = true,
            overridden = overridden
        }
    end

    return overridden
end

function MWBase.DrawModelWithOpticLensOverrides(model, enabled)
    if not model or not IsValid(model) or type(model.DrawModel) ~= "function" then return end

    if not enabled or not render or type(render.MaterialOverrideByIndex) ~= "function" then
        model:DrawModel()
        return
    end

    local transparentMaterial = getTransparentLensMaterial()
    if not transparentMaterial then
        model:DrawModel()
        return
    end

    local slots = collectOpticLensSlots(model)
    if #slots == 0 then
        model:DrawModel()
        return
    end

    for _, entry in ipairs(slots) do
        render.MaterialOverrideByIndex(entry.slot, transparentMaterial)
        debugOpticLens("draw override", "slot", entry.slot, entry.materialName)
    end

    model:DrawModel()

    for _, entry in ipairs(slots) do
        render.MaterialOverrideByIndex(entry.slot, nil)
    end

    if render.ModelMaterialOverride then
        render.ModelMaterialOverride(nil, nil)
    end
end

function MWBase.PrintOpticLensMaterials(attachment)
    if type(attachment) ~= "table" then
        print("[RTXPatcher][MWBase OpticLens] no optic attachment to inspect")
        return
    end

    local modelCount = 0
    for _, entry in ipairs(collectOpticModels(attachment)) do
        modelCount = modelCount + 1
        print("[RTXPatcher][MWBase OpticLens] " .. entry.label)

        for index, materialName in ipairs(entry.model:GetMaterials() or {}) do
            local slot = index - 1
            local matched = isOpticLensMaterialName(materialName) and "MATCH" or "skip"
            local subMaterial = type(entry.model.GetSubMaterial) == "function" and entry.model:GetSubMaterial(slot) or ""
            print(string.format("  [%d] %s | sub=%s | %s", slot, tostring(materialName), tostring(subMaterial), matched))
        end
    end

    if modelCount == 0 then
        print("[RTXPatcher][MWBase OpticLens] optic has no inspectable client models")
    end
end

function MWBase.FindActiveOpticAttachment()
    local player = LocalPlayer and LocalPlayer() or nil
    if not IsValid(player) or type(player.GetActiveWeapon) ~= "function" then return nil, "LocalPlayer unavailable" end

    local weapon = player:GetActiveWeapon()
    if not IsValid(weapon) then return nil, "active weapon unavailable" end

    if type(weapon.GetSight) == "function" then
        local sight = weapon:GetSight()
        if type(sight) == "table" and isOpticAttachment(sight.ClassName or "", sight) then
            return sight, nil, weapon
        end
    end

    if type(weapon.GetAllAttachmentsInUse) == "function" then
        for _, attachment in pairs(weapon:GetAllAttachmentsInUse() or {}) do
            if type(attachment) == "table" and isOpticAttachment(attachment.ClassName or "", attachment) then
                return attachment, nil, weapon
            end
        end
    end

    return nil, "no active MWBase optic attachment found"
end

if concommand and concommand.Add and not (_G and _G.RTXPatcher_MWBaseOpticLensDumpCommandAdded) then
    concommand.Add("rtx_patcher_mwbase_dump_optic_lens", function()
        local attachment, reason = MWBase.FindActiveOpticAttachment()
        if not attachment then
            print("[RTXPatcher][MWBase OpticLens] " .. tostring(reason))
            return
        end

        MWBase.PrintOpticLensMaterials(attachment)
    end)

    if _G then
        _G.RTXPatcher_MWBaseOpticLensDumpCommandAdded = true
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

    local worldPos = att.Pos + offset
    local bounds = projectedReticleOverlayBounds(worldPos, att.Ang, quadSize)
    if submitExplicitReticleOverlay(reticle.Material, bounds, color, "optic") then
        return
    end

    if markReticleMaterialForRasterOverlay then
        markReticleMaterialForRasterOverlay(reticle.Material)
    end

    render.DrawQuadEasy(worldPos, att.Ang:Forward():GetNegated(), quadSize, quadSize, color, -att.Ang.r + 180)
end

local function debugOpticViewSurface(...)
    if not cvOpticViewSurfaceDebug:GetBool() then return end

    local parts = {}
    for i = 1, select("#", ...) do
        parts[#parts + 1] = tostring(select(i, ...))
    end
    print("[RTXPatcher][MWBase ViewSurface] " .. table.concat(parts, " "))
end

local function getOpticViewSurfaceCategory()
    if type(RemixCategoryManager) ~= "table" then return nil end
    if type(RemixCategoryManager.PRESET) == "table" and RemixCategoryManager.PRESET.VIEW_SURFACE then
        return RemixCategoryManager.PRESET.VIEW_SURFACE
    end
    if type(RemixCategoryManager.CATEGORY) == "table" then
        if RemixCategoryManager.CATEGORY.VIEW_SURFACE then
            return RemixCategoryManager.CATEGORY.VIEW_SURFACE
        end
        return RemixCategoryManager.CATEGORY.RAYTRACED_RENDER_TARGET
    end
    return nil
end

local function getRasterOverlayCategory()
    if type(RemixCategoryManager) == "table" then
        if type(RemixCategoryManager.PRESET) == "table" and RemixCategoryManager.PRESET.RASTER_OVERLAY then
            return RemixCategoryManager.PRESET.RASTER_OVERLAY
        end
        if type(RemixCategoryManager.CATEGORY) == "table" and RemixCategoryManager.CATEGORY.RASTER_OVERLAY then
            return RemixCategoryManager.CATEGORY.RASTER_OVERLAY
        end
    end

    return 0x4000000
end

local function combineCategoryFlags(existing, category)
    existing = tonumber(existing) or 0
    category = tonumber(category) or 0
    if category == 0 then return existing end

    if bit and type(bit.band) == "function" then
        if bit.band(existing, category) ~= 0 then return existing end
        if type(bit.bor) == "function" then
            return bit.bor(existing, category)
        end
    end

    local quotient = math.floor(existing / category)
    if quotient % 2 >= 1 then return existing end
    return existing + category
end

local function addMaterialCandidate(candidates, seen, name)
    local candidate = tostring(name or "")
    if candidate == "" or seen[candidate] then return end
    seen[candidate] = true
    candidates[#candidates + 1] = candidate
end

local function getMaterialCandidates(materialName)
    local candidates = {}
    local seen = {}
    local rawName = tostring(materialName or "")
    local normalized = normalizedMaterialName(rawName)
    local stripped = string.gsub(normalized, "\\", "/")

    if string.sub(stripped, 1, 10) == "materials/" then
        stripped = string.sub(stripped, 11)
    end

    if string.sub(stripped, -4) == ".vmt" then
        stripped = string.sub(stripped, 1, -5)
    end

    addMaterialCandidate(candidates, seen, rawName)
    addMaterialCandidate(candidates, seen, normalized)
    addMaterialCandidate(candidates, seen, stripped)

    return candidates
end

local function mergeViewSurfaceHashCategory(hashStr, category)
    if type(RemixMaterial) ~= "table" or type(RemixMaterial.SetHashCategory) ~= "function" then
        return false
    end

    local existing = 0
    if type(RemixMaterial.GetHashCategory) == "function" then
        local ok, result = pcall(RemixMaterial.GetHashCategory, hashStr)
        if ok then
            existing = tonumber(result) or 0
        end
    end

    local combined = combineCategoryFlags(existing, category)
    if combined == existing and existing ~= 0 then
        return true
    end

    local ok, result = pcall(RemixMaterial.SetHashCategory, hashStr, combined)
    return ok and result ~= false
end

local function mergeRasterOverlayHashCategory(hashStr, category)
    if type(RemixMaterial) ~= "table" or type(RemixMaterial.SetHashCategory) ~= "function" then
        return false
    end

    local existing = 0
    if type(RemixMaterial.GetHashCategory) == "function" then
        local ok, result = pcall(RemixMaterial.GetHashCategory, hashStr)
        if ok then
            existing = tonumber(result) or 0
        end
    end

    local combined = combineCategoryFlags(existing, category)
    if combined == existing and existing ~= 0 then
        return true
    end

    local ok, result = pcall(RemixMaterial.SetHashCategory, hashStr, combined)
    return ok and result ~= false
end

function MWBase.SetViewSurfaceCategoryForMaterial(materialName, state)
    local category = getOpticViewSurfaceCategory()
    if not category or type(RemixMaterial) ~= "table" then return false end

    local touched = false
    local queued = false
    local candidates = getMaterialCandidates(materialName)

    for _, candidate in ipairs(candidates) do
        if type(RemixMaterial.GetAllTextureHashes) == "function" then
            local ok, hashes = pcall(RemixMaterial.GetAllTextureHashes, candidate)
            if ok and type(hashes) == "table" and #hashes > 0 then
                for _, hashStr in ipairs(hashes) do
                    if mergeViewSurfaceHashCategory(hashStr, category) then
                        touched = true
                        if state then
                            state.immediate = (state.immediate or 0) + 1
                        end
                        debugOpticViewSurface("hash", hashStr, "material", candidate)
                    end
                end
            end
        end

        if type(RemixCategoryManager) == "table" and type(RemixCategoryManager.SetMaterialCategory) == "function" then
            local callback = function(success, hashStr)
                if success and hashStr then
                    mergeViewSurfaceHashCategory(hashStr, category)
                end
            end
            local ok = pcall(RemixCategoryManager.SetMaterialCategory, candidate, category, callback)
            if ok then
                queued = true
            end
        elseif type(RemixMaterial.TrackMaterial) == "function" then
            local ok = pcall(RemixMaterial.TrackMaterial, candidate)
            queued = queued or ok
        end
    end

    if queued and not touched and state then
        state.pending = (state.pending or 0) + 1
    end

    return touched or queued
end

function MWBase.SetRasterOverlayCategoryForMaterial(materialName, state)
    local category = getRasterOverlayCategory()
    if not category or type(RemixMaterial) ~= "table" then return false end

    local touched = false
    local queued = false
    local candidates = getMaterialCandidates(materialName)

    for _, candidate in ipairs(candidates) do
        if type(RemixMaterial.GetAllTextureHashes) == "function" then
            local ok, hashes = pcall(RemixMaterial.GetAllTextureHashes, candidate)
            if ok and type(hashes) == "table" and #hashes > 0 then
                for _, hashStr in ipairs(hashes) do
                    if mergeRasterOverlayHashCategory(hashStr, category) then
                        touched = true
                        if state then
                            state.immediate = (state.immediate or 0) + 1
                        end
                        debugReticleOverlay("hash", hashStr, "material", candidate)
                    end
                end
            end
        end

        if type(RemixCategoryManager) == "table" and type(RemixCategoryManager.SetMaterialCategory) == "function" then
            local callback = function(success, hashStr)
                if success and hashStr then
                    mergeRasterOverlayHashCategory(hashStr, category)
                end
            end
            local ok = pcall(RemixCategoryManager.SetMaterialCategory, candidate, category, callback)
            if ok then
                queued = true
            end
        elseif type(RemixMaterial.TrackMaterial) == "function" then
            local ok = pcall(RemixMaterial.TrackMaterial, candidate)
            queued = queued or ok
        end
    end

    if queued and not touched and state then
        state.pending = (state.pending or 0) + 1
    end

    return touched or queued
end

local reticleOverlayMarked = {}
local reticleOverlayLastTry = {}

local function materialNameFromMaterial(material)
    if type(material) == "string" then return material end
    if not material then return nil end

    if type(material.GetName) == "function" then
        local ok, name = pcall(material.GetName, material)
        if ok and name and tostring(name) ~= "" then
            return tostring(name)
        end
    end

    return nil
end

local function screenPointFromWorld(pos)
    if not pos or type(pos.ToScreen) ~= "function" then return nil end

    local screen = pos:ToScreen()
    if type(screen) ~= "table" or screen.visible == false then return nil end

    local x = tonumber(screen.x)
    local y = tonumber(screen.y)
    if not x or not y then return nil end

    return x, y
end

local function distance2D(ax, ay, bx, by)
    local dx = ax - bx
    local dy = ay - by
    return math.sqrt(dx * dx + dy * dy)
end

local function reticleExplicitOverlayClipSource()
    local source = cvReticleExplicitOverlayClipSource:GetString() or "reticle"
    source = string.lower(tostring(source):gsub("^%s+", ""):gsub("%s+$", ""))

    if source == "model" or source == "entity" or source == "1" then
        return "model"
    end

    return "reticle"
end

function MWBase.GetReticleOverlayStencilReference(attachment)
    local baseRef = MWBASE_STENCIL_REFVALUE or 0
    local mode = cvReticleExplicitOverlayStencilRefMode:GetString() or "auto"
    mode = string.lower(tostring(mode):gsub("^%s+", ""):gsub("%s+$", ""))

    if mode == "reticle" or mode == "shape" or mode == "1" then
        return baseRef + 1
    end

    if mode == "lens" or mode == "optic" or mode == "2" then
        return baseRef + 2
    end

    if type(attachment) == "table" and type(attachment.Optic) == "table" then
        return baseRef + 2
    end

    return baseRef + 1
end

local function getEntityLocalBounds(ent)
    if not ent then return nil end

    local getters = {
        { "GetRenderBounds" },
        { "OBBMins", "OBBMaxs" },
        { "GetModelBounds" }
    }

    for _, names in ipairs(getters) do
        local first = ent[names[1]]
        if type(first) == "function" then
            if names[2] then
                local second = ent[names[2]]
                if type(second) == "function" then
                    local okMin, mins = pcall(first, ent)
                    local okMax, maxs = pcall(second, ent)
                    if okMin and okMax and mins and maxs then
                        return mins, maxs
                    end
                end
            else
                local ok, mins, maxs = pcall(first, ent)
                if ok and mins and maxs then
                    return mins, maxs
                end
            end
        end
    end

    return nil
end

local function projectedEntityOverlayBounds(ent)
    if not ent or type(ent.LocalToWorld) ~= "function" then return nil end

    local mins, maxs = getEntityLocalBounds(ent)
    if not mins or not maxs then return nil end

    local minX, minY, maxX, maxY = nil, nil, nil, nil
    local xs = { mins.x, maxs.x }
    local ys = { mins.y, maxs.y }
    local zs = { mins.z, maxs.z }

    for _, x in ipairs(xs) do
        for _, y in ipairs(ys) do
            for _, z in ipairs(zs) do
                local screenX, screenY = screenPointFromWorld(ent:LocalToWorld(Vector(x, y, z)))
                if screenX and screenY then
                    minX = minX and math.min(minX, screenX) or screenX
                    minY = minY and math.min(minY, screenY) or screenY
                    maxX = maxX and math.max(maxX, screenX) or screenX
                    maxY = maxY and math.max(maxY, screenY) or screenY
                end
            end
        end
    end

    if not minX or not minY or not maxX or not maxY then return nil end

    local width = maxX - minX
    local height = maxY - minY
    if width <= 0 or height <= 0 then return nil end

    return {
        x = minX + width * 0.5,
        y = minY + height * 0.5,
        w = width,
        h = height
    }
end

projectedReticleOverlayBounds = function(worldPos, ang, worldSize)
    local size = tonumber(worldSize) or 0
    if size <= 0 or not worldPos or not ang then return nil end

    local centerX, centerY = screenPointFromWorld(worldPos)
    if not centerX then return nil end

    local halfSize = size * 0.5
    local right = ang:Right()
    local up = ang:Up()

    local rx0, ry0 = screenPointFromWorld(worldPos - right * halfSize)
    local rx1, ry1 = screenPointFromWorld(worldPos + right * halfSize)
    local ux0, uy0 = screenPointFromWorld(worldPos - up * halfSize)
    local ux1, uy1 = screenPointFromWorld(worldPos + up * halfSize)
    if not rx0 or not rx1 or not ux0 or not ux1 then return nil end

    local width = distance2D(rx0, ry0, rx1, ry1)
    local height = distance2D(ux0, uy0, ux1, uy1)
    if width <= 0 or height <= 0 then return nil end

    return {
        x = centerX,
        y = centerY,
        w = width,
        h = height
    }
end

submitExplicitReticleOverlay = function(material, bounds, color, label)
    if not cvReticleExplicitOverlay:GetBool() then return false end
    if type(RemixRasterOverlay) ~= "table" or type(RemixRasterOverlay.DrawQuad) ~= "function" then return false end
    if type(bounds) ~= "table" then return false end

    local materialName = materialNameFromMaterial(material)
    if not materialName or materialName == "" then return false end

    local x = tonumber(bounds.x)
    local y = tonumber(bounds.y)
    local width = tonumber(bounds.w) or 0
    local height = tonumber(bounds.h) or 0
    if not x or not y or width <= 0 or height <= 0 then return false end

    local scale = math.max(0.01, cvReticleExplicitOverlayScale:GetFloat())
    width = math.max(1, width * scale)
    height = math.max(1, height * scale)

    local info = {
        material = materialName,
        x = x - width * 0.5,
        y = y - height * 0.5,
        w = width,
        h = height,
        color = color or color_white
    }

    local useStencilMask = bounds.stencil == true and cvReticleExplicitOverlayStencil:GetBool()
    if cvReticleExplicitOverlayClip:GetBool() and not useStencilMask then
        local clipSource = reticleExplicitOverlayClipSource()
        local clipBounds = clipSource == "model" and type(bounds.clip) == "table" and bounds.clip or nil
        local clipX = tonumber(clipBounds and clipBounds.x) or x
        local clipY = tonumber(clipBounds and clipBounds.y) or y
        local baseClipW = tonumber(clipBounds and clipBounds.w) or width
        local baseClipH = tonumber(clipBounds and clipBounds.h) or height
        local clipScale = math.max(0.01, cvReticleExplicitOverlayClipScale:GetFloat())
        local clipScaleX = math.max(0.01, cvReticleExplicitOverlayClipScaleX:GetFloat())
        local clipScaleY = math.max(0.01, cvReticleExplicitOverlayClipScaleY:GetFloat())
        local clipW = math.max(1, baseClipW * clipScale * clipScaleX)
        local clipH = math.max(1, baseClipH * clipScale * clipScaleY)
        clipX = clipX + baseClipW * cvReticleExplicitOverlayClipOffsetX:GetFloat()
        clipY = clipY + baseClipH * cvReticleExplicitOverlayClipOffsetY:GetFloat()
        info.clip = {
            mode = "ellipse",
            x = clipX - clipW * 0.5,
            y = clipY - clipH * 0.5,
            w = clipW,
            h = clipH
        }
        debugReticleOverlay("clip", tostring(label or ""), clipSource, "center", clipX, clipY, "size", clipW, clipH)
    end

    if useStencilMask then
        local stencilReference = tonumber(bounds.stencilReference)
        local stencilReadMask = tonumber(bounds.stencilReadMask) or 0xFF
        info.stencil = {
            current = true,
            clear = cvReticleExplicitOverlayStencilClear:GetBool(),
            readMask = stencilReadMask
        }
        if stencilReference then
            info.stencil.reference = stencilReference
        end
        debugReticleOverlay("stencil", tostring(label or ""), "clear", info.stencil.clear, "ref", tostring(stencilReference), "mask", stencilReadMask)
    end

    local ok, submitted, reason = pcall(RemixRasterOverlay.DrawQuad, info)

    if not ok then
        debugReticleOverlay("explicit_error", tostring(label or ""), materialName, submitted)
        return false
    end

    if submitted ~= true then
        debugReticleOverlay("explicit_miss", tostring(label or ""), materialName, tostring(reason))
        return false
    end

    debugReticleOverlay("explicit", tostring(label or ""), materialName, "size", width, height)
    return true
end

markReticleMaterialForRasterOverlay = function(material, force)
    if not cvReticleRasterOverlay:GetBool() then return false end

    local materialName = materialNameFromMaterial(material)
    if not materialName or materialName == "" then return false end

    local key = normalizedMaterialName(materialName)
    if key == "" then return false end

    if reticleOverlayMarked[key] and not force then
        return true
    end

    local now = RealTime and RealTime() or 0
    if not force and reticleOverlayLastTry[key] and now - reticleOverlayLastTry[key] < 0.5 then
        return false
    end
    reticleOverlayLastTry[key] = now

    local state = Patcher.State.mwbaseReticleOverlay or {
        materials = {},
        marked = 0,
        immediate = 0,
        pending = 0,
    }
    Patcher.State.mwbaseReticleOverlay = state
    state.enabled = true
    state.category = getRasterOverlayCategory()
    state.lastMaterial = materialName

    if MWBase.SetRasterOverlayCategoryForMaterial(materialName, state) then
        local isNewMaterial = not state.materials[key]
        state.materials[key] = true
        reticleOverlayMarked[key] = true
        if isNewMaterial then
            state.marked = (state.marked or 0) + 1
        end
        debugReticleOverlay("material", materialName)
        return true
    end

    return false
end

local function formatCategoryFlags(flags)
    flags = tonumber(flags) or 0
    if flags == 0 then return "none" end

    local names = {}
    local categoryNames = {
        { flag = 0x4, name = "SKY" },
        { flag = 0x8, name = "IGNORE" },
        { flag = 0x200, name = "HIDDEN" },
        { flag = 0x400, name = "PARTICLE" },
        { flag = 0x800, name = "BEAM" },
        { flag = 0x1000, name = "DECAL_STATIC" },
        { flag = 0x40000, name = "ANIMATED_WATER" },
        { flag = 0x1000000, name = "LEGACY_EMISSIVE" },
        { flag = 0x2000000, name = "VIEW_SURFACE" },
        { flag = 0x4000000, name = "RASTER_OVERLAY" },
    }

    for _, entry in ipairs(categoryNames) do
        if bit and type(bit.band) == "function" then
            if bit.band(flags, entry.flag) ~= 0 then
                names[#names + 1] = entry.name
            end
        elseif math.floor(flags / entry.flag) % 2 >= 1 then
            names[#names + 1] = entry.name
        end
    end

    if #names == 0 then return string.format("0x%X", flags) end
    return table.concat(names, " | ")
end

local function printReticleOverlayMaterial(label, reticle, force)
    if type(reticle) ~= "table" or not reticle.Material then return false end

    if force and markReticleMaterialForRasterOverlay then
        markReticleMaterialForRasterOverlay(reticle.Material, true)
    end

    local materialName = materialNameFromMaterial(reticle.Material)
    print(string.format("[RTXPatcher][MWBase ReticleOverlay] %s material %s", label, tostring(materialName or reticle.Material)))

    if not materialName or type(RemixMaterial) ~= "table" or type(RemixMaterial.GetAllTextureHashes) ~= "function" then
        print("  no hashes available")
        return true
    end

    local printedHash = false
    for _, candidate in ipairs(getMaterialCandidates(materialName)) do
        local ok, hashes = pcall(RemixMaterial.GetAllTextureHashes, candidate)
        if ok and type(hashes) == "table" and #hashes > 0 then
            for _, hashStr in ipairs(hashes) do
                local category = nil
                if type(RemixMaterial.GetHashCategory) == "function" then
                    local okCategory, result = pcall(RemixMaterial.GetHashCategory, hashStr)
                    if okCategory then category = result end
                end
                print(string.format("  %s -> %s category=%s", candidate, tostring(hashStr), formatCategoryFlags(category)))
                printedHash = true
            end
        end
    end

    if not printedHash then
        print("  no hashes found yet; render the reticle once, then dump again")
    end

    return true
end

function MWBase.PrintReticleOverlayState(attachment, force)
    if type(attachment) ~= "table" then
        print("[RTXPatcher][MWBase ReticleOverlay] no optic attachment to inspect")
        return
    end

    local printed = false
    printed = printReticleOverlayMaterial("Reticle", attachment.Reticle, force) or printed
    printed = printReticleOverlayMaterial("ReticleHybrid", attachment.ReticleHybrid, force) or printed

    if not printed then
        print("[RTXPatcher][MWBase ReticleOverlay] active optic has no reticle materials")
    end

    local state = Patcher.State and Patcher.State.mwbaseReticleOverlay or nil
    if state then
        print(string.format(
            "[RTXPatcher][MWBase ReticleOverlay] enabled=%s category=%s marked=%s immediate=%s pending=%s last=%s",
            tostring(cvReticleRasterOverlay:GetBool()),
            tostring(state.category),
            tostring(state.marked or 0),
            tostring(state.immediate or 0),
            tostring(state.pending or 0),
            tostring(state.lastMaterial or "")
        ))
    end
end

local RETICLE_OVERLAY_DUMP_COMMAND_VERSION = 1
if concommand and concommand.Add and (not _G or _G.RTXPatcher_MWBaseReticleOverlayDumpCommandVersion ~= RETICLE_OVERLAY_DUMP_COMMAND_VERSION) then
    concommand.Add("rtx_patcher_mwbase_dump_reticle_overlay", function(_, _, args)
        local attachment, reason = MWBase.FindActiveOpticAttachment()
        if not attachment then
            print("[RTXPatcher][MWBase ReticleOverlay] " .. tostring(reason))
            return
        end

        local force = args and (args[1] == "1" or args[1] == "force")
        MWBase.PrintReticleOverlayState(attachment, force)
    end)

    if _G then
        _G.RTXPatcher_MWBaseReticleOverlayDumpCommandVersion = RETICLE_OVERLAY_DUMP_COMMAND_VERSION
    end
end

function MWBase.MarkOpticViewSurfaceMaterials(attachment, force)
    if type(attachment) ~= "table" then return false end

    local state = attachment._RTXPatcherViewSurfaceMaterials
    if type(state) == "table" and state.initialized and not force then
        return (state.marked or 0) > 0 or (state.immediate or 0) > 0 or (state.pending or 0) > 0
    end

    state = state or {
        materials = {},
        marked = 0,
        immediate = 0,
        pending = 0,
    }
    state.materials = state.materials or {}
    state.lastRetry = CurTime and CurTime() or nil

    local lensCount = 0
    for _, modelEntry in ipairs(collectOpticModels(attachment)) do
        for _, lensEntry in ipairs(collectOpticLensSlots(modelEntry.model)) do
            lensCount = lensCount + 1
            local key = normalizedMaterialName(lensEntry.materialName)
            if key ~= "" and (force or not state.materials[key]) then
                if MWBase.SetViewSurfaceCategoryForMaterial(lensEntry.materialName, state) then
                    local isNewMaterial = not state.materials[key]
                    state.materials[key] = true
                    if isNewMaterial then
                        state.marked = (state.marked or 0) + 1
                    end
                    debugOpticViewSurface("material", lensEntry.materialName, "slot", lensEntry.slot, "model", modelEntry.label)
                end
            end
        end
    end

    state.initialized = lensCount > 0 and ((state.marked or 0) > 0 or (state.immediate or 0) > 0 or (state.pending or 0) > 0)
    state.lensCount = lensCount
    attachment._RTXPatcherViewSurfaceMaterials = state

    return (state.marked or 0) > 0 or (state.immediate or 0) > 0 or (state.pending or 0) > 0
end

local function getOpticViewSurfaceFov(attachment)
    local manualFov = cvOpticViewSurfaceFov:GetFloat()
    local nativeFov = nil

    if type(attachment) == "table" and type(attachment.Optic) == "table" then
        nativeFov = tonumber(attachment.Optic.FOV)
    end

    local baseFov = manualFov
    local source = "manual"

    if cvOpticViewSurfaceUseOpticFov:GetBool() and nativeFov and nativeFov > 0 then
        baseFov = nativeFov
        source = "optic"
    elseif not baseFov or baseFov <= 0 then
        baseFov = nativeFov and nativeFov > 0 and nativeFov or 15
        source = nativeFov and nativeFov > 0 and "optic-fallback" or "fallback"
    end

    return math.max(1, baseFov * cvOpticViewSurfaceFovScale:GetFloat()), source, nativeFov, manualFov
end

local function getOpticReticleAttachment(attachment)
    if not cvOpticViewSurfaceReticleOrigin:GetBool() then return nil end
    if type(attachment) ~= "table" or type(attachment.Reticle) ~= "table" then return nil end
    if not attachment.m_Model or not IsValid(attachment.m_Model) then return nil end
    if type(mw_utils) ~= "table" or type(mw_utils.GetFastAttachment) ~= "function" then return nil end

    local attachmentName = attachment.Reticle.Attachment
    if not attachmentName then return nil end

    local ok, reticleAttachment = pcall(mw_utils.GetFastAttachment, attachment.m_Model, attachmentName)
    if not ok or type(reticleAttachment) ~= "table" then return nil end
    if not reticleAttachment.Pos or not reticleAttachment.Ang then return nil end
    if type(reticleAttachment.Ang.Forward) ~= "function" then return nil end

    return reticleAttachment, tostring(attachmentName)
end

local function getOpticViewSurfacePose(attachment)
    if not EyeAngles or not EyePos then return nil end

    local ang = EyeAngles()
    local pos = EyePos()
    if not ang or not pos or type(ang.Forward) ~= "function" or type(ang.Up) ~= "function" or type(ang.Right) ~= "function" then
        return nil
    end

    local forward = ang:Forward()
    local up = ang:Up()
    local right = ang:Right()
    local originSource = "eye"
    local originAttachment = nil
    local reticleForwardOffset = 0

    local reticleAttachment, reticleAttachmentName = getOpticReticleAttachment(attachment)
    if reticleAttachment then
        local reticleForward = reticleAttachment.Ang:Forward()
        reticleForwardOffset = cvOpticViewSurfaceReticleOriginForward:GetFloat()
        pos = reticleAttachment.Pos + reticleForward * reticleForwardOffset
        originSource = "reticle"
        originAttachment = reticleAttachmentName
    end

    pos = pos + forward * cvOpticViewSurfaceOffsetForward:GetFloat()
    pos = pos + right * cvOpticViewSurfaceOffsetRight:GetFloat()
    pos = pos + up * cvOpticViewSurfaceOffsetUp:GetFloat()

    return pos, forward, up, right, originSource, originAttachment, reticleForwardOffset
end

function MWBase.SubmitOpticViewSurfaceCamera(attachment, weapon, forceMaterialRetry)
    if not cvOpticViewSurface:GetBool() then
        if Patcher.State then
            Patcher.State.mwbaseOpticViewSurface = { enabled = false }
        end
        return false
    end
    if type(RemixCamera) ~= "table" or type(RemixCamera.SetupParameterizedCamera) ~= "function" then
        debugOpticViewSurface("RemixCamera.SetupParameterizedCamera unavailable")
        return false
    end

    local cameraType = RemixCamera.TYPE_RENDER_VIEW or RemixCamera.TYPE_RENDER_TO_TEXTURE
    if cameraType == nil then
        debugOpticViewSurface("render-view camera type unavailable")
        return false
    end

    local marked = MWBase.MarkOpticViewSurfaceMaterials(attachment, forceMaterialRetry)
    if not marked then
        debugOpticViewSurface("no lens materials marked")
        return false
    end

    local pos, forward, up, right, originSource, originAttachment, reticleForwardOffset = getOpticViewSurfacePose(attachment)
    if not pos then return false end

    local fov, fovSource, nativeFov, manualFov = getOpticViewSurfaceFov(attachment)
    local aspect = math.max(0.01, cvOpticViewSurfaceAspect:GetFloat())
    local nearPlane = math.max(0.01, cvOpticViewSurfaceNear:GetFloat())
    local farPlane = math.max(nearPlane + 1, cvOpticViewSurfaceFar:GetFloat())

    local ok, result = pcall(RemixCamera.SetupParameterizedCamera, {
        type = cameraType,
        position = pos,
        forward = forward,
        up = up,
        right = right,
        fovYInDegrees = fov,
        aspect = aspect,
        nearPlane = nearPlane,
        farPlane = farPlane,
    })

    local submitted = ok and result ~= false
    local viewSurfaceState = attachment and attachment._RTXPatcherViewSurfaceMaterials or nil
    if Patcher.State then
        Patcher.State.mwbaseOpticViewSurface = {
            enabled = true,
            submitted = submitted,
            aiming = true,
            fov = fov,
            fovSource = fovSource,
            nativeFov = nativeFov,
            manualFov = manualFov,
            originSource = originSource,
            originAttachment = originAttachment,
            reticleForwardOffset = reticleForwardOffset,
            aspect = aspect,
            nearPlane = nearPlane,
            farPlane = farPlane,
            marked = viewSurfaceState and viewSurfaceState.marked or 0,
            immediate = viewSurfaceState and viewSurfaceState.immediate or 0,
            pending = viewSurfaceState and viewSurfaceState.pending or 0,
            frame = FrameNumber and FrameNumber() or nil,
        }
    end

    debugOpticViewSurface("camera", "submitted", submitted, "fov", fov, "source", fovSource, "origin", originSource, "origin_attachment", originAttachment, "origin_forward", reticleForwardOffset)

    if not submitted then
        debugOpticViewSurface("camera submit failed", ok and tostring(result) or tostring(result))
    end

    return submitted
end

function MWBase.SubmitActiveOpticViewSurfaceCamera(forceMaterialRetry)
    if not cvEnabled:GetBool() or not cvOpticViewSurface:GetBool() then return false end

    local attachment, reason, weapon = MWBase.FindActiveOpticAttachment()
    if not attachment then
        debugOpticViewSurface(reason)
        return false
    end

    if not MWBase.IsOpticAiming(weapon) then
        if Patcher.State then
            Patcher.State.mwbaseOpticViewSurface = { enabled = true, submitted = false, aiming = false }
        end
        return false
    end

    local frame = FrameNumber and FrameNumber() or nil
    local state = Patcher.State and Patcher.State.mwbaseOpticViewSurface or nil
    if not forceMaterialRetry and frame ~= nil and type(state) == "table" and state.frame == frame then
        return state.submitted == true
    end

    return MWBase.SubmitOpticViewSurfaceCamera(attachment, weapon, forceMaterialRetry)
end

function MWBase.PrintOpticViewSurfaceState(attachment)
    local state = Patcher.State and Patcher.State.mwbaseOpticViewSurface or nil
    if type(state) == "table" then
        print(string.format(
            "[RTXPatcher][MWBase ViewSurface] enabled=%s submitted=%s aiming=%s marked=%s immediate=%s pending=%s fov=%s source=%s native=%s manual=%s origin=%s origin_attachment=%s origin_forward=%s frame=%s",
            tostring(state.enabled),
            tostring(state.submitted),
            tostring(state.aiming),
            tostring(state.marked),
            tostring(state.immediate),
            tostring(state.pending),
            tostring(state.fov),
            tostring(state.fovSource),
            tostring(state.nativeFov),
            tostring(state.manualFov),
            tostring(state.originSource),
            tostring(state.originAttachment),
            tostring(state.reticleForwardOffset),
            tostring(state.frame)
        ))
    else
        print("[RTXPatcher][MWBase ViewSurface] no submitted state")
    end

    if type(attachment) ~= "table" then
        print("[RTXPatcher][MWBase ViewSurface] no optic attachment to inspect")
        return
    end

    local category = getOpticViewSurfaceCategory()
    print("[RTXPatcher][MWBase ViewSurface] category=" .. tostring(category))

    for _, modelEntry in ipairs(collectOpticModels(attachment)) do
        for _, lensEntry in ipairs(collectOpticLensSlots(modelEntry.model)) do
            print(string.format(
                "[RTXPatcher][MWBase ViewSurface] %s slot %d material %s",
                tostring(modelEntry.label),
                lensEntry.slot,
                tostring(lensEntry.materialName)
            ))

            for _, candidate in ipairs(getMaterialCandidates(lensEntry.materialName)) do
                local hashes = nil
                if type(RemixMaterial) == "table" and type(RemixMaterial.GetAllTextureHashes) == "function" then
                    local ok, result = pcall(RemixMaterial.GetAllTextureHashes, candidate)
                    if ok then hashes = result end
                end

                if type(hashes) ~= "table" or #hashes == 0 then
                    print("  " .. candidate .. " -> no hashes")
                else
                    for _, hashStr in ipairs(hashes) do
                        local flags = nil
                        if type(RemixMaterial.GetHashCategory) == "function" then
                            local ok, result = pcall(RemixMaterial.GetHashCategory, hashStr)
                            if ok then flags = result end
                        end
                        print(string.format("  %s -> %s category=%s", candidate, tostring(hashStr), tostring(flags)))
                    end
                end
            end
        end
    end
end

local VIEW_SURFACE_DUMP_COMMAND_VERSION = 4
if concommand and concommand.Add and (not _G or _G.RTXPatcher_MWBaseViewSurfaceDumpCommandVersion ~= VIEW_SURFACE_DUMP_COMMAND_VERSION) then
    concommand.Add("rtx_patcher_mwbase_dump_view_surface", function()
        local attachment, reason = MWBase.FindActiveOpticAttachment()
        if not attachment then
            print("[RTXPatcher][MWBase ViewSurface] " .. tostring(reason))
            return
        end

        local forcedMark = MWBase.MarkOpticViewSurfaceMaterials(attachment, true)
        print("[RTXPatcher][MWBase ViewSurface] forced_mark=" .. tostring(forcedMark))
        MWBase.PrintOpticViewSurfaceState(attachment)
    end)

    if _G then
        _G.RTXPatcher_MWBaseViewSurfaceDumpCommandVersion = VIEW_SURFACE_DUMP_COMMAND_VERSION
    end
end

local VIEW_SURFACE_THINK_HOOK_VERSION = 1
if hook and hook.Add and (not _G or _G.RTXPatcher_MWBaseViewSurfaceThinkHookVersion ~= VIEW_SURFACE_THINK_HOOK_VERSION) then
    hook.Add("Think", "RTXPatcher_MWBaseViewSurfaceCamera", function()
        MWBase.SubmitActiveOpticViewSurfaceCamera(false)
    end)

    if _G then
        _G.RTXPatcher_MWBaseViewSurfaceThinkHookVersion = VIEW_SURFACE_THINK_HOOK_VERSION
    end
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

    local viewSurfaceActive = MWBase.SubmitOpticViewSurfaceCamera(self, weapon)
    MWBase.SetOpticLensBodygroup(self, not viewSurfaceActive)
    local overrideLens = cvOpticLensOverride:GetBool() and not viewSurfaceActive
    MWBase.OverrideOpticLensMaterials(self, overrideLens)
    self.m_bRemovedRT = false
    MWBase.DrawModelWithOpticLensOverrides(self.m_Model, overrideLens)
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

    local stencilReference = MWBase.GetReticleOverlayStencilReference(self)
    render.SetStencilWriteMask(0xFF)
    render.SetStencilTestMask(0xFF)
    render.SetStencilReferenceValue(0)
    render.SetStencilCompareFunction(STENCIL_ALWAYS)
    render.SetStencilPassOperation(STENCIL_REPLACE)
    render.SetStencilFailOperation(STENCIL_KEEP)
    render.SetStencilZFailOperation(STENCIL_KEEP)
    render.SetStencilEnable(true)
    render.SetStencilReferenceValue(stencilReference)

    if self.Reticle and self.Reticle.Squash ~= nil and type(weapon.GetAimDelta) == "function" then
        model:ManipulateBoneScale(0, Vector(Lerp(weapon:GetAimDelta(), 1, self.Reticle.Squash), 1, 1))
    end

    drawReticleStencilMaskModel(model)
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

    local offset = att.Ang:Forward() * cvReticleDepth:GetFloat()
    if reticle.Offset ~= nil then
        offset = offset + att.Ang:Right() * reticle.Offset.x
        offset = offset + att.Ang:Up() * reticle.Offset.y
    end

    local worldPos = att.Pos + offset
    local bounds = projectedReticleOverlayBounds(worldPos, att.Ang, size * 0.01)
    if bounds and reticleExplicitOverlayClipSource() == "model" then
        bounds.clip = projectedEntityOverlayBounds(model)
    end
    if bounds then
        bounds.stencil = true
        bounds.stencilReference = stencilReference
        bounds.stencilReadMask = 0xFF
    end
    local submittedUsesDeferredStencilClear = bounds
        and bounds.stencil == true
        and cvReticleExplicitOverlayStencil:GetBool()
        and cvReticleExplicitOverlayStencilClear:GetBool()

    if submitExplicitReticleOverlay(reticle.Material, bounds, color, "stencil") then
        render.SetStencilEnable(false)
        if not submittedUsesDeferredStencilClear then
            render.ClearStencil()
        end
        return
    end

    if markReticleMaterialForRasterOverlay then
        markReticleMaterialForRasterOverlay(reticle.Material)
    end

    render.SetMaterial(reticle.Material)
    render.DrawQuadEasy(worldPos, att.Ang:Forward():GetNegated(), size * 0.01, size * 0.01, color, -att.Ang.r + 180)

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
        reticleRasterOverlay = cvReticleRasterOverlay:GetBool(),
        reticleRasterOverlayDebug = cvReticleRasterOverlayDebug:GetBool(),
        reticleExplicitOverlay = cvReticleExplicitOverlay:GetBool(),
        reticleExplicitOverlayScale = cvReticleExplicitOverlayScale:GetFloat(),
        reticleExplicitOverlayClip = cvReticleExplicitOverlayClip:GetBool(),
        reticleExplicitOverlayClipScale = cvReticleExplicitOverlayClipScale:GetFloat(),
        reticleExplicitOverlayClipScaleX = cvReticleExplicitOverlayClipScaleX:GetFloat(),
        reticleExplicitOverlayClipScaleY = cvReticleExplicitOverlayClipScaleY:GetFloat(),
        reticleExplicitOverlayClipOffsetX = cvReticleExplicitOverlayClipOffsetX:GetFloat(),
        reticleExplicitOverlayClipOffsetY = cvReticleExplicitOverlayClipOffsetY:GetFloat(),
        reticleExplicitOverlayClipSource = reticleExplicitOverlayClipSource(),
        reticleExplicitOverlayStencil = cvReticleExplicitOverlayStencil:GetBool(),
        reticleExplicitOverlayStencilClear = cvReticleExplicitOverlayStencilClear:GetBool(),
        reticleExplicitOverlayStencilRefMode = cvReticleExplicitOverlayStencilRefMode:GetString(),
        optics = cvOptics:GetBool(),
        opticRenderMode = cvOpticSimpleRender:GetBool() and "simple" or "original",
        opticAdsThreshold = cvOpticAdsThreshold:GetFloat(),
        opticLensOverride = cvOpticLensOverride:GetBool(),
        opticLensDebug = cvOpticLensDebug:GetBool(),
        opticReticle = cvOpticReticle:GetBool(),
        opticReticleDepth = cvOpticReticleDepth:GetFloat(),
        opticReticleScale = cvOpticReticleScale:GetFloat(),
        opticReticleMaxSize = cvOpticReticleMaxSize:GetFloat(),
        opticViewSurface = cvOpticViewSurface:GetBool(),
        opticViewSurfaceDebug = cvOpticViewSurfaceDebug:GetBool(),
        opticViewSurfaceUseOpticFov = cvOpticViewSurfaceUseOpticFov:GetBool(),
        opticViewSurfaceFov = cvOpticViewSurfaceFov:GetFloat(),
        opticViewSurfaceFovScale = cvOpticViewSurfaceFovScale:GetFloat(),
        opticViewSurfaceAspect = cvOpticViewSurfaceAspect:GetFloat(),
        opticViewSurfaceNear = cvOpticViewSurfaceNear:GetFloat(),
        opticViewSurfaceFar = cvOpticViewSurfaceFar:GetFloat(),
        opticViewSurfaceReticleOrigin = cvOpticViewSurfaceReticleOrigin:GetBool(),
        opticViewSurfaceReticleOriginForward = cvOpticViewSurfaceReticleOriginForward:GetFloat(),
        opticViewSurfaceOffsetForward = cvOpticViewSurfaceOffsetForward:GetFloat(),
        opticViewSurfaceOffsetRight = cvOpticViewSurfaceOffsetRight:GetFloat(),
        opticViewSurfaceOffsetUp = cvOpticViewSurfaceOffsetUp:GetFloat(),
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
