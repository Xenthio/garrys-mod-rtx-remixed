if not CLIENT then return end

RTXPatcher = nil
include("patcher/sh_core.lua")
include("patcher/cl_intercepts.lua")

local oldMW_ATTS = MW_ATTS
local oldMwUtils = mw_utils
local oldWeapons = weapons
local oldScriptedEnts = scripted_ents
local oldCreateClientConVar = CreateClientConVar
local oldGetConVar = GetConVar
local oldIsValid = IsValid
local oldCreateMaterial = CreateMaterial
local oldConcommand = concommand
local oldRender = render
local storedConVars = {}
local materialOverrideCalls = {}

local function createFakeConVar(default)
    return {
        GetBool = function() return tostring(default) ~= "0" end,
        GetString = function() return tostring(default) end,
        GetFloat = function() return tonumber(default) or 0 end,
        GetInt = function() return tonumber(default) or 0 end,
    }
end

CreateClientConVar = function(name, default)
    if storedConVars[name] then
        return storedConVars[name]
    end

    local convar = createFakeConVar(default)
    storedConVars[name] = convar
    return convar
end

GetConVar = function(name)
    return storedConVars[name]
end

IsValid = function(value)
    return value ~= nil and value ~= false and value.__invalid ~= true
end

CreateMaterial = function(name, shader, params)
    return {
        name = name,
        shader = shader,
        params = params
    }
end

concommand = {
    Add = function() end
}

render = {
    MaterialOverrideByIndex = function(slot, material)
        materialOverrideCalls[#materialOverrideCalls + 1] = {
            slot = slot,
            material = material
        }
    end,
    ModelMaterialOverride = function() end
}

MW_ATTS = {
    att_sight_reticle = {
        Base = "att_sight",
        DoReticleStencil = function() return "base" end
    },
    att_fake_reflex = {
        Base = "att_sight_reticle",
        DoReticleStencil = function() return "child" end
    },
    att_fake_optic = {
        Base = "att_optic",
        Optic = {
            LensBodygroup = "lens"
        },
        Reticle = {
            Attachment = "reticle",
            Size = 800
        },
        DoReticleStencil = function() return "optic-reticle" end,
        Render = function() return "optic-render" end
    }
}

mw_utils = {
    IsAttachmentBasedOn = function(current, base)
        return current == base or (current == "att_fake_reflex" and base == "att_sight_reticle")
            or (current == "att_fake_optic" and base == "att_optic")
    end
}

weapons = {
    GetStored = function(className)
        if className == "mg_base" then return { ClassName = "mg_base" } end
        return nil
    end
}

scripted_ents = {
    GetStored = function(className)
        if className == "mg_viewmodel" then
            return {
                t = {
                    Draw = function() return "draw" end,
                    ViewBlur = function() return "blur" end
                }
            }
        end
        return nil
    end,
    Get = function() return nil end
}

local ok, err = pcall(function()
    include("patcher/addons/cl_mwbase.lua")

    local patch = RTXPatcher.Patches.mwbase
    if not patch then
        error("MWBase patch did not register")
    end

    local ready, context = patch.depends()
    if not ready then
        error("MWBase fake dependency did not resolve")
    end

    if not context.viewmodel or type(context.viewmodel.Draw) ~= "function" then
        error("MWBase viewmodel context missing Draw")
    end

    if not RTXPatcher.State.applied.mwbase then
        error("MWBase patch did not apply against fake dependencies")
    end

    if MW_ATTS.att_sight_reticle.DoReticleStencil() ~= "base" then
        error("base reticle wrapper did not call original in fallback mode")
    end

    if MW_ATTS.att_fake_reflex.DoReticleStencil() ~= "child" then
        error("child reticle wrapper did not call original in fallback mode")
    end

    local wrappedBase = false
    local wrappedChild = false
    local wrappedOptic = false
    local wrappedCustomizationDraw = false
    local wrappedCustomizationBlur = false
    for _, state in pairs(RTXPatcher.State.functionIntercepts) do
        if state.id == "reticle_att_sight_reticle" then wrappedBase = true end
        if state.id == "reticle_att_fake_reflex" then wrappedChild = true end
        if state.id == "optic_att_fake_optic" then wrappedOptic = true end
        if state.id == "customization_draw" then wrappedCustomizationDraw = true end
        if state.id == "customization_blur" then wrappedCustomizationBlur = true end
    end

    if not wrappedBase then error("base reticle attachment was not intercepted") end
    if not wrappedChild then error("child reticle attachment was not intercepted") end
    if not wrappedOptic then error("optic attachment render was not intercepted") end
    if not wrappedCustomizationDraw then error("customization draw was not intercepted") end
    if not wrappedCustomizationBlur then error("customization blur was not intercepted") end

    if not RTXPatcher.State.mwbase or RTXPatcher.State.mwbase.reticleMode ~= "ADS-only" then
        error("MWBase mode summary was not recorded")
    end

    if RTXPatcher.State.mwbase.reticleDepth ~= 100 then
        error("MWBase reticle depth default should match original attachment depth")
    end

    if RTXPatcher.State.mwbase.reticleScale ~= 1 then
        error("MWBase reticle scale default should preserve original attachment size")
    end

    if RTXPatcher.State.mwbase.optics ~= true then
        error("MWBase optic patch mode was not recorded")
    end

    if RTXPatcher.State.mwbase.opticRenderMode ~= "simple" then
        error("MWBase optic render mode should default to simple")
    end

    if RTXPatcher.State.mwbase.opticLensOverride ~= true then
        error("MWBase optic lens override mode was not recorded")
    end

    local function makeFakeModel(materials)
        return {
            materials = materials,
            subMaterials = {},
            drawCount = 0,
            GetMaterials = function(self) return self.materials end,
            GetSubMaterial = function(self, slot) return self.subMaterials[slot] or "" end,
            SetSubMaterial = function(self, slot, material) self.subMaterials[slot] = material end,
            DrawModel = function(self) self.drawCount = self.drawCount + 1 end
        }
    end

    local opticModel = makeFakeModel({
        "viper/mw/attachments/attachment_vm_4x_west_body",
        "lens/4x_lens ",
        "reticle_ui_stencil"
    })
    local hideModel = makeFakeModel({
        "viper/MW/attachments/attachment_vm_4x_west_lens"
    })
    local opticAttachment = {
        Optic = {
            LensBodygroup = "lens"
        },
        m_Model = opticModel,
        hideModel = hideModel
    }

    RTXPatcher.MWBase.OverrideOpticLensMaterials(opticAttachment, true)

    if opticModel.subMaterials[1] ~= "!rtx_patcher_mwbase_transparent_lens" then
        error("lens/4x_lens material on optic model was not overridden")
    end

    if opticModel.subMaterials[2] ~= nil then
        error("reticle material should not be treated as an optic lens")
    end

    if hideModel.subMaterials[0] ~= "!rtx_patcher_mwbase_transparent_lens" then
        error("lens material on optic hide model was not overridden")
    end

    RTXPatcher.MWBase.OverrideOpticLensMaterials(opticAttachment, false)

    if opticModel.subMaterials[1] ~= "" then
        error("optic model lens material was not restored")
    end

    if hideModel.subMaterials[0] ~= "" then
        error("hide model lens material was not restored")
    end

    materialOverrideCalls = {}
    RTXPatcher.MWBase.DrawModelWithOpticLensOverrides(opticModel, true)

    if opticModel.drawCount ~= 1 then
        error("optic model was not drawn through lens override path")
    end

    if #materialOverrideCalls < 2 then
        error("draw-time material override was not applied and cleared")
    end

    if materialOverrideCalls[1].slot ~= 1 or not materialOverrideCalls[1].material then
        error("draw-time material override did not target lens/4x_lens slot")
    end

    if materialOverrideCalls[2].slot ~= 1 or materialOverrideCalls[2].material ~= nil then
        error("draw-time material override was not cleared")
    end
end)

MW_ATTS = oldMW_ATTS
mw_utils = oldMwUtils
weapons = oldWeapons
scripted_ents = oldScriptedEnts
CreateClientConVar = oldCreateClientConVar
GetConVar = oldGetConVar
IsValid = oldIsValid
CreateMaterial = oldCreateMaterial
concommand = oldConcommand
render = oldRender

if not ok then
    error(err, 0)
end

print("[RTXPatcherTests] PASS")
