if not CLIENT then return end
local RenderCore = include("remixlua/cl/customrender/render_core.lua") or RemixRenderCore

-- Custom 2D Skybox Renderer
-- Draws a 2D skybox
-- Author: CR

local cv_enable = CreateClientConVar("rtx_sky2d_enable", "1", true, false, "Enable custom 2D skybox rendering")
local cv_override = CreateClientConVar("rtx_sky2d_name", "", true, false, "Override skybox name (leave blank to use sv_skyname)")
local cv_brightness = CreateClientConVar("rtx_sky2d_brightness", "1.0", true, false, "Brightness multiplier for custom 2D skybox")
local cv_useDepthRange = CreateClientConVar("rtx_sky2d_use_depthrange", "1", true, false, "Use DepthRange(near,far) during 2D sky draw for RTX detection")
local cv_depthNear = CreateClientConVar("rtx_sky2d_depthnear", "0.999", true, false, "DepthRange near (0..1) when enabled")
local cv_depthFar  = CreateClientConVar("rtx_sky2d_depthfar",  "1.0",  true, false, "DepthRange far (0..1) when enabled")
-- Depth behavior and sizing
local cv_zTest  = CreateClientConVar("rtx_sky2d_ztest", "1", true, false, "Enable depth test during 2D sky draw")
local cv_zWrite = CreateClientConVar("rtx_sky2d_zwrites", "1", true, false, "Enable depth writes during 2D sky draw")
local cv_clearDepth = CreateClientConVar("rtx_sky2d_clear_depth", "0", true, false, "Clear depth buffer before drawing 2D sky")
local cv_size = CreateClientConVar("rtx_sky2d_size", "1", true, false, "Half-extent of the sky cube in units")
local cv_camNear = CreateClientConVar("rtx_sky2d_cam_znear", "1", true, false, "Camera z-near for 2D sky pass")
local cv_camFar  = CreateClientConVar("rtx_sky2d_cam_zfar", "65536", true, false, "Camera z-far for 2D sky pass")
local cv_disableCull = CreateClientConVar("rtx_sky2d_disable_cull", "0", true, false, "Disable backface culling when drawing sky quads")
local cv_swapUD = CreateClientConVar("rtx_sky2d_swap_ud", "0", true, false, "Swap up/down sky faces to match Source orientation")
local cv_swapLR = CreateClientConVar("rtx_sky2d_swap_lr", "1", true, false, "Swap left/right sky faces to match Source orientation")
local cv_debug = CreateClientConVar("rtx_sky2d_debug", "0", true, false, "Debug prints for 2D skybox renderer")

local function DebugPrint(...)
    if cv_debug:GetBool() then
        print("[2D Skybox]", ...)
    end
end

-- Cache materials per sky name
local skyCache = {}
local lastDrawFrame = -1

-- Cached ConVar values (updated on change callbacks)
local cachedConVars = {
    size = 16384,
    brightness = 1.0,
    disableCull = false,
    clearDepth = false,
    zTest = false,
    zWrite = false,
    useDepthRange = false,
    depthNear = 0.999,
    depthFar = 1.0,
    swapLR = false,
    swapUD = false
}

-- Update cache from ConVars
local function UpdateConVarCache()
    cachedConVars.size = math.max(1024, math.floor(cv_size:GetFloat() or 16384))
    cachedConVars.brightness = math.max(0.0, cv_brightness:GetFloat())
    cachedConVars.disableCull = cv_disableCull:GetBool()
    cachedConVars.clearDepth = cv_clearDepth:GetBool()
    cachedConVars.zTest = cv_zTest:GetBool()
    cachedConVars.zWrite = cv_zWrite:GetBool()
    cachedConVars.useDepthRange = cv_useDepthRange:GetBool()
    cachedConVars.depthNear = math.Clamp(cv_depthNear:GetFloat() or 0.999, 0, 1)
    cachedConVars.depthFar = math.Clamp((cv_depthFar and cv_depthFar:GetFloat()) or 1, 0, 1)
    if cachedConVars.depthFar < cachedConVars.depthNear then
        cachedConVars.depthFar = cachedConVars.depthNear
    end
    cachedConVars.swapLR = cv_swapLR:GetBool()
    cachedConVars.swapUD = cv_swapUD:GetBool()
end

-- Initialize cache
UpdateConVarCache()

-- Register change callbacks to invalidate cache
for name, _ in pairs({
    rtx_sky2d_size = true,
    rtx_sky2d_brightness = true,
    rtx_sky2d_disable_cull = true,
    rtx_sky2d_clear_depth = true,
    rtx_sky2d_ztest = true,
    rtx_sky2d_zwrites = true,
    rtx_sky2d_use_depthrange = true,
    rtx_sky2d_depthnear = true,
    rtx_sky2d_depthfar = true,
    rtx_sky2d_swap_lr = true,
    rtx_sky2d_swap_ud = true
}) do
    cvars.AddChangeCallback(name, UpdateConVarCache, "RTX2DSky_CacheUpdate")
end

local function getSkyName()
    local o = string.Trim(cv_override:GetString() or "")
    if o ~= "" then return o end
    local c = GetConVar("sv_skyname")
    local n = c and c:GetString() or "painted"
    return n ~= "" and n or "painted"
end

local function getSkyMaterials(name)
    local entry = skyCache[name]
    if entry then return entry end

    local sides = { "rt", "lf", "bk", "ft", "up", "dn" }
    local mats = {}
    for _, s in ipairs(sides) do
        local path = "skybox/" .. name .. s
        -- Use RenderCore material cache if available
        local mat = (RenderCore and RenderCore.GetMaterial) and RenderCore.GetMaterial(path) or Material(path)
        if mat and mat.SetInt then
            -- Keep $ignorez in sync with z-test setting so we can force-visibility when needed
            pcall(function()
                mat:SetInt("$ignorez", cv_zTest:GetBool() and 0 or 1)
            end)
        end
        mats[s] = mat
    end
    skyCache[name] = mats
    DebugPrint("Cached sky materials for:", name)
    return mats
end

local function drawFace(mat, pos, normal, size, rot)
    if not mat then return end
    render.SetMaterial(mat)
    render.DrawQuadEasy(pos, normal, size * 2, size * 2, color_white, rot or 0)
end

local function Draw2DSky()
    if not cv_enable:GetBool() then return end
    -- Avoid drawing multiple times per frame if multiple hooks call us
    -- Skip entirely during offscreen RT renders (e.g., rearview RenderView)
    if RenderCore and RenderCore.IsOffscreen and RenderCore.IsOffscreen() then
        return
    end
    local fn = FrameNumber()
    if lastDrawFrame == fn then return end
    lastDrawFrame = fn
    -- Only draw our sky when engine world is hidden to avoid double sky
    local cv_world = GetConVar("r_drawworld")
    local cv_opaque = GetConVar("r_drawopaqueworld")
    local cv_capture = GetConVar("rtx_capture_mode")
    local engineWorldOn = true
    if cv_world and cv_world:GetInt() == 0 then engineWorldOn = false end
    if cv_opaque and cv_opaque:GetInt() == 0 then engineWorldOn = false end
    if cv_capture and cv_capture:GetInt() == 1 then engineWorldOn = false end
    if engineWorldOn then return end

    local name = getSkyName()
    local mats = getSkyMaterials(name)

    -- Rendering parameters (use cached values)
    local origin = EyePos()
    local size = cachedConVars.size
    local br = cachedConVars.brightness
    
    render.SuppressEngineLighting(true)
    if cachedConVars.disableCull then
        render.CullMode(MATERIAL_CULLMODE_NONE)
    end
    -- Control depth test/write and optional clear
    if cachedConVars.clearDepth then
        render.ClearDepth()
    end
    render.OverrideDepthEnable(cachedConVars.zTest, cachedConVars.zWrite)
    if cachedConVars.useDepthRange then
        render.DepthRange(cachedConVars.depthNear, cachedConVars.depthFar)
    end
    render.SetColorModulation(br, br, br)

    -- Hardcoded rotation: entire skybox rotated 180° around Z axis (horizontal flip)
    local axisX = Vector(1, 0, 0)
    local axisY = Vector(0, 1, 0)
    local axisZ = Vector(0, 0, 1)

    -- Sides (180° rotation swaps front<->back and right<->left)
    -- Right/Left with optional swap
    local rtMat = mats["rt"]
    local lfMat = mats["lf"]
    if cachedConVars.swapLR then
        rtMat, lfMat = lfMat, rtMat
    end
    -- Right material now on left position: plane at -X, facing inward (+X)
    drawFace(rtMat, origin - axisX * size,  axisX, size, 180)
    -- Left material now on right position: plane at +X, facing inward (-X)
    drawFace(lfMat, origin + axisX * size, -axisX, size, 180)
    -- Back material now on front position: plane at +Y, facing inward (-Y)
    drawFace(mats["bk"], origin + axisY * size, -axisY, size, 180)
    -- Front material now on back position: plane at -Y, facing inward (+Y)
    drawFace(mats["ft"], origin - axisY * size,  axisY, size, 180)
    -- Up/Down with optional swap (also rotated 180° to match horizontal rotation)
    local upMat = mats["up"]
    local dnMat = mats["dn"]
    if cachedConVars.swapUD then
        upMat, dnMat = dnMat, upMat
    end
    -- Up (up): plane at +Z, facing inward (-Z), rotated 360° (180° base + 180° global)
    drawFace(upMat, origin + axisZ * size, -axisZ, size, 0)
    -- Down (dn): plane at -Z, facing inward (+Z), rotated 360° (180° base + 180° global)
    drawFace(dnMat, origin - axisZ * size,  axisZ, size, 0)

    -- Restore render state
    render.SetColorModulation(1, 1, 1)
    if cachedConVars.useDepthRange then
        render.DepthRange(0, 1)
    end
    render.OverrideDepthEnable(false, false)
    if cachedConVars.disableCull then
        render.CullMode(MATERIAL_CULLMODE_CCW)
    end
    render.SuppressEngineLighting(false)
end

-- Draw very early in the frame so it acts as background
-- Note: Frame number guard in Draw2DSky() prevents multiple draws per frame even with multiple hooks
-- Using PreDrawOpaqueRenderables as primary hook for reliability
RenderCore.Register("PreDrawOpaqueRenderables", "RTX2DSky_Draw", { fn = function(bDrawingDepth, bDrawingSkybox)
    if bDrawingDepth then return end
    -- Only draw in world pass, not during skybox depth pass
    Draw2DSky()
end, prio = -10000 })

-- Clear cache on shutdown/map change
RenderCore.Register("ShutDown", "RTX2DSky_Cleanup", function()
    skyCache = {}
end)

print("[Custom 2D Skybox] Loaded.")
