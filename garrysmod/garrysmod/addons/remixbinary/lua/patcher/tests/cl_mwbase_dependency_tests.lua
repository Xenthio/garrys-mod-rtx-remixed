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
local storedConVars = {}

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
end)

MW_ATTS = oldMW_ATTS
mw_utils = oldMwUtils
weapons = oldWeapons
scripted_ents = oldScriptedEnts
CreateClientConVar = oldCreateClientConVar
GetConVar = oldGetConVar

if not ok then
    error(err, 0)
end

print("[RTXPatcherTests] PASS")
