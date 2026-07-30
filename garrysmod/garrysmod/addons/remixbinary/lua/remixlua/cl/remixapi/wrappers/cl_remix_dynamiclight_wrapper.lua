if not (BRANCH == "x86-64" or BRANCH == "chromium") then return end
if not CLIENT then return end

-- Configuration
local cv_enabled = CreateClientConVar("rtx_dynamiclight_wrapper_enabled", "1", true, false, "Enable RTX wrapping for DynamicLight() calls")
local cv_debug = CreateClientConVar("rtx_dynamiclight_wrapper_debug", "0", true, false, "Debug logging for dynamic light wrapper")
local cv_brightness_scale = CreateClientConVar("rtx_dynamiclight_wrapper_brightness_scale", "50", true, false, "Brightness scaling for dynamic lights")
local cv_radius_scale = CreateClientConVar("rtx_dynamiclight_wrapper_radius_scale", "0.01", true, false, "Radius scaling for dynamic lights")
local cv_decay_scale = CreateClientConVar("rtx_dynamiclight_wrapper_decay_scale", "2", true, false, "Perceptual decay multiplier for dynamic lights")

-- Tracking: lightId -> {
--   rtxLightId, lastUpdate, expiresAt, decayStartedAt, fade,
--   firstRenderPending, props
-- }
local wrappedDynamicLights = {}

local function DebugPrint(...)
    if cv_debug:GetBool() then
        print("[RTX DynamicLight]", ...)
    end
end

local function vec3(x, y, z)
    return { x = x, y = y, z = z }
end

local function IsWrapperActive()
    local lightUpdater = GetConVar("rtx_lightupdater")
    return cv_enabled:GetBool() and not (lightUpdater and lightUpdater:GetBool())
end

local function DestroyWrappedDynamicLight(lightId, reason)
    local data = wrappedDynamicLights[lightId]
    if not data then return false end

    -- Remove tracking first so cleanup timers and entity-wrapper ownership
    -- changes cannot destroy the same Remix light twice.
    wrappedDynamicLights[lightId] = nil
    if data.rtxLightId and istable(RemixLight) and RemixLight.DestroyLight then
        RemixLight.DestroyLight(data.rtxLightId)
    end

    DebugPrint(
        "Destroyed RTX light",
        data.rtxLightId or 0,
        "for DynamicLight",
        lightId,
        reason or "")
    return true
end

local function DestroyAllWrappedDynamicLights(reason)
    local lightIds = {}
    for lightId in pairs(wrappedDynamicLights) do
        table.insert(lightIds, lightId)
    end

    for _, lightId in ipairs(lightIds) do
        DestroyWrappedDynamicLight(lightId, reason)
    end
end

-- Preserve the engine function across Lua auto-refresh. Older versions of
-- this file captured the already-wrapped global each time they reloaded,
-- building a proxy chain that could submit duplicate lights with the same ID.
local function ResolveOriginalDynamicLight()
    local stored = rawget(_G, "RTXOriginalDynamicLight")
    if isfunction(stored) then return stored end

    local candidate = DynamicLight
    local visited = {}

    -- Unwrap any copies of this wrapper already stacked in the current Lua
    -- state. The engine function is a C closure and has no Lua upvalues.
    while isfunction(candidate) and not visited[candidate] do
        visited[candidate] = true
        local wrappedOriginal = nil

        if debug and debug.getupvalue then
            for index = 1, 32 do
                local name, value = debug.getupvalue(candidate, index)
                if not name then break end
                if name == "OriginalDynamicLight" and isfunction(value) then
                    wrappedOriginal = value
                    break
                end
            end
        end

        if not wrappedOriginal then break end
        candidate = wrappedOriginal
    end

    _G.RTXOriginalDynamicLight = candidate
    return candidate
end

local OriginalDynamicLight = ResolveOriginalDynamicLight()

-- DynamicLight IDs are commonly reused for successive muzzle flashes. Treat
-- tightly grouped submissions as updates to one flash, but restart the fade
-- when a later submission extends the lifetime and begins another pulse.
local function IsNewDecayPulse(data, currentTime, expiresAt, size, decay)
    if not data or decay <= 0 or size <= 0 then return false, 0, 0 end

    local previousExpiresAt = data.expiresAt or expiresAt
    local lifetimeExtension = expiresAt - previousExpiresAt
    if lifetimeExtension <= 0.001 then return false, 0, lifetimeExtension end

    local previousUpdate = data.lastUpdate or currentTime
    local submissionGap = math.max(0, currentTime - previousUpdate)
    local decayDuration = size / decay
    local pulseGap = math.Clamp(decayDuration * 0.4, 0.025, 0.05)
    local decayStartedAt = data.decayStartedAt or previousUpdate
    local phaseElapsed = math.max(0, currentTime - decayStartedAt)
    local fadedOut = (data.fade or 1) <= 0.005 or phaseElapsed >= decayDuration

    return fadedOut or
        (submissionGap >= pulseGap and phaseElapsed >= pulseGap),
        submissionGap,
        lifetimeExtension
end

-- Create or update RTX light from dynamic light properties
local function UpdateRTXFromDynamicLight(lightId, dlight)
    if not IsWrapperActive() then return end
    if not istable(RemixLight) then return end
    if not dlight then return end

    -- gmod_light drives a Source DynamicLight using its EntIndex, but the
    -- dedicated entity wrapper already owns a persistent Remix light for it.
    -- Keep the Source DynamicLight intact while suppressing the duplicate
    -- Remix light (and remove one created during the wrapper's startup delay).
    local sourceEntity = Entity(lightId)
    if istable(_G.RTXLightWrapper) and
       IsValid(sourceEntity) and
       _G.RTXLightWrapper.IsWrapped(sourceEntity) then
        DestroyWrappedDynamicLight(lightId, "owned by RTXLightWrapper")
        return
    end
    
    -- Extract properties
    local pos = dlight.Pos or Vector(0, 0, 0)
    local r = dlight.r or 255
    local g = dlight.g or 255
    local b = dlight.b or 255
    local brightness = dlight.brightness or 1
    local size = dlight.Size or 256
    local decay = tonumber(dlight.Decay) or 0
    local currentTime = CurTime()
    -- DynamicLight.DieTime is an absolute CurTime value. Keep the old timeout
    -- only as a defensive fallback for malformed third-party light tables.
    local expiresAt = tonumber(dlight.DieTime) or (currentTime + 0.5)

    -- Read the existing light before constructing radiance. Repeated Source
    -- submissions refresh properties and DieTime, but do not restart the
    -- radius decay that began when this wrapped light was created.
    local data = wrappedDynamicLights[lightId]
    local restartDecay, submissionGap, lifetimeExtension =
        IsNewDecayPulse(data, currentTime, expiresAt, size, decay)

    if restartDecay then
        data.decayStartedAt = currentTime
        data.fade = 1
        data.firstRenderPending = true
    end
    
    -- Apply scaling
    local brightnessScale = cv_brightness_scale:GetFloat()
    local radiusScale = cv_radius_scale:GetFloat()
    
    -- Calculate radiance. Property refreshes must retain the fade already
    -- applied by UpdateDecayingLights instead of restoring full brightness.
    local appliedFade = data and (data.fade or 1) or 1
    local scale = ((brightness * brightnessScale) / 100.0) * appliedFade
    local base = {
        hash = tonumber(util.CRC(string.format("dynamic_light_%d", lightId))) or (lightId + 100000),
        radiance = vec3(
            r * scale,
            g * scale,
            b * scale
        ),
        isDynamic = true,
    }
    
    local sphere = {
        position = vec3(pos.x, pos.y, pos.z),
        radius = size * radiusScale,
        volumetricRadianceScale = 1.0,
    }
    
    -- Check if we need to create or update
    if not data or not data.rtxLightId or data.rtxLightId == 0 then
        -- Create new RTX light
        local rtxLightId = nil
        if RemixLight.CreateSphere then
            rtxLightId = RemixLight.CreateSphere(base, sphere, lightId + 100000)
        end
        
        if rtxLightId and rtxLightId ~= 0 then
            wrappedDynamicLights[lightId] = {
                rtxLightId = rtxLightId,
                lastUpdate = currentTime,
                expiresAt = expiresAt,
                decayStartedAt = currentTime,
                fade = 1,
                firstRenderPending = true,
                props = {
                    pos = pos,
                    r = r,
                    g = g,
                    b = b,
                    brightness = brightness,
                    size = size,
                    decay = decay,
                }
            }
            DebugPrint(
                "Created RTX light",
                rtxLightId,
                "for DynamicLight",
                lightId,
                "brightness:", brightness,
                "size:", size,
                "decay:", decay,
                "expires:", expiresAt)
        else
            DebugPrint(
                "CreateSphere failed for DynamicLight",
                lightId,
                "result:", tostring(rtxLightId),
                "API available:", RemixLight.CreateSphere ~= nil)
        end
    else
        -- Update existing RTX light
        local oldProps = data.props
        
        -- Only update if properties changed significantly
        local changed = restartDecay
        if oldProps.pos:DistToSqr(pos) > 1 then changed = true end
        if oldProps.r ~= r or oldProps.g ~= g or oldProps.b ~= b then changed = true end
        if math.abs(oldProps.brightness - brightness) > 0.01 then changed = true end
        if math.abs(oldProps.size - size) > 1 then changed = true end
        if math.abs((oldProps.decay or 0) - decay) > 0.01 then changed = true end
        
        if changed then
            if RemixLight.UpdateSphere then
                RemixLight.UpdateSphere(base, sphere, data.rtxLightId)
            end
            
            DebugPrint("Updated RTX light", data.rtxLightId, "for DynamicLight", lightId)
        end

        if restartDecay then
            DebugPrint(
                "Restarted RTX light pulse",
                data.rtxLightId,
                "for DynamicLight",
                lightId,
                "submission gap:", string.format("%.4f", submissionGap),
                "lifetime extension:", string.format("%.4f", lifetimeExtension))
        end
        
        -- Refresh properties and lifetime without restarting the decay phase.
        -- A fresh light gets a new phase naturally after the old one expires
        -- and is removed from wrappedDynamicLights.
        data.props = {
            pos = pos,
            r = r,
            g = g,
            b = b,
            brightness = brightness,
            size = size,
            decay = decay,
        }
        data.lastUpdate = currentTime
        data.expiresAt = expiresAt
    end
end

-- Apply Source's radius decay as a radiance fade. Remix sphere radius is a
-- physical emitter size rather than Source's illumination range, so shrinking
-- the Remix sphere does not reproduce the intended muzzle-flash fade.
local function UpdateDecayingLights()
    if not IsWrapperActive() then return end
    if not istable(RemixLight) or not RemixLight.UpdateSphere then return end

    local currentTime = CurTime()
    local brightnessScale = cv_brightness_scale:GetFloat()
    local radiusScale = cv_radius_scale:GetFloat()
    local decayScale = math.max(0, cv_decay_scale:GetFloat())

    for lightId, data in pairs(wrappedDynamicLights) do
        local props = data.props
        local decay = props and math.max(0, props.decay or 0) or 0
        local size = props and math.max(0, props.size or 0) or 0

        if not data.firstRenderPending and decay > 0 and size > 0 then
            local decayStartedAt = data.decayStartedAt or data.lastUpdate or currentTime
            local elapsed = math.max(0, currentTime - decayStartedAt)
            -- Source reduces the light's illumination range, which goes dark
            -- perceptually faster than a linear Remix radiance reduction.
            -- Accelerate the radiance fade so short burst intervals contain a
            -- real zero-light gap.
            local remainingSize = math.max(0, size - elapsed * decay * decayScale)
            local fade = math.Clamp(remainingSize / size, 0, 1)

            if math.abs((data.fade or 1) - fade) > 0.005 then
                local scale = (props.brightness * brightnessScale / 100.0) * fade
                local base = {
                    hash = tonumber(util.CRC(string.format("dynamic_light_%d", lightId))) or (lightId + 100000),
                    radiance = vec3(
                        props.r * scale,
                        props.g * scale,
                        props.b * scale
                    ),
                    isDynamic = true,
                }
                local sphere = {
                    position = vec3(props.pos.x, props.pos.y, props.pos.z),
                    radius = size * radiusScale,
                    volumetricRadianceScale = 1.0,
                }

                RemixLight.UpdateSphere(base, sphere, data.rtxLightId)
                data.fade = fade
                DebugPrint(
                    "Faded RTX light",
                    data.rtxLightId,
                    "for DynamicLight",
                    lightId,
                    "fade:", string.format("%.3f", fade),
                    "elapsed:", string.format("%.4f", elapsed))
            end
        end
    end
end

-- Cleanup expired dynamic lights
local function CleanupExpiredLights()
    if not IsWrapperActive() then
        DestroyAllWrappedDynamicLights("wrapper inactive")
        return
    end
    
    local currentTime = CurTime()
    local expired = {}
    
    -- Match Source's absolute DynamicLight.DieTime instead of using a fixed
    -- inactivity window.
    for lightId, data in pairs(wrappedDynamicLights) do
        local expiresAt = data.expiresAt or ((data.lastUpdate or currentTime) + 0.5)
        if not data.firstRenderPending and currentTime >= expiresAt then
            table.insert(expired, lightId)
        end
    end
    
    -- Remove expired lights
    for _, lightId in ipairs(expired) do
        DestroyWrappedDynamicLight(lightId, "expired")
    end
end

local function MarkSubmittedLightsRendered()
    for _, data in pairs(wrappedDynamicLights) do
        data.firstRenderPending = false
    end
end

-- Hook into DynamicLight function
function DynamicLight(index, fallbackIndex)
    -- Call original function
    local dlight = OriginalDynamicLight(index, fallbackIndex)
    
    if not dlight then return dlight end
    
    -- Store original metatable methods
    local originalIndex = dlight.__index
    local mt = getmetatable(dlight)
    
    -- Track when properties are set
    local lightId = index or fallbackIndex or 0
    local propsUpdated = false
    
    -- Wrap the dynamic light object to intercept property changes
    local wrappedDlight = setmetatable({}, {
        __index = function(t, k)
            return rawget(t, k) or (dlight and dlight[k])
        end,
        __newindex = function(t, k, v)
            rawset(t, k, v)
            if dlight then
                dlight[k] = v
            end
            
            -- Mark that properties were updated
            if k == "Pos" or k == "r" or k == "g" or k == "b" or k == "brightness" or k == "Size" then
                propsUpdated = true
            end
            
            -- When DieTime is set, we know all properties are set - create/update RTX light
            if k == "DieTime" and propsUpdated then
                UpdateRTXFromDynamicLight(lightId, t)
            end
        end
    })
    
    return wrappedDlight
end

-- Remove the old periodic timer during Lua auto-refresh, then check lifetime
-- every rendered frame after entity Think calls have refreshed their lights.
timer.Remove("RTXDynamicLightCleanup")
hook.Add("PreRender", "RTXDynamicLightCleanup", function()
    UpdateDecayingLights()
    CleanupExpiredLights()
    MarkSubmittedLightsRendered()
end)

-- Cleanup on map change
hook.Add("OnReloaded", "RTXDynamicLight_Cleanup", function()
    DestroyAllWrappedDynamicLights("Lua reloaded")
end)

hook.Add("ShutDown", "RTXDynamicLight_Cleanup", function()
    DestroyAllWrappedDynamicLights("shutdown")
end)

-- Console commands
concommand.Add("rtx_dynamiclight_list", function()
    print("[RTX DynamicLight] Currently wrapped dynamic lights:")
    local count = 0
    for lightId, data in pairs(wrappedDynamicLights) do
        local props = data.props
        print(string.format("  - DynamicLight %d -> RTX Light %d (r=%d g=%d b=%d, brightness=%.1f, size=%.0f)",
            lightId, data.rtxLightId, props.r, props.g, props.b, props.brightness, props.size))
        count = count + 1
    end
    print("Total: " .. count .. " dynamic lights")
end, nil, "List all wrapped dynamic lights")

concommand.Add("rtx_dynamiclight_clear", function()
    local count = table.Count(wrappedDynamicLights)
    DestroyAllWrappedDynamicLights("console clear")
    print("[RTX DynamicLight] Cleared " .. count .. " dynamic lights")
end, nil, "Clear all wrapped dynamic lights")

concommand.Add("rtx_dynamiclight_status", function()
    local lightUpdater = GetConVar("rtx_lightupdater")
    print(string.format(
        "[RTX DynamicLight] enabled=%s lightupdater=%s wrapperActive=%s remixApi=%s createSphere=%s updateSphere=%s decayScale=%.2f wrapped=%d",
        tostring(cv_enabled:GetBool()),
        tostring(lightUpdater and lightUpdater:GetBool() or false),
        tostring(IsWrapperActive()),
        tostring(istable(RemixLight)),
        tostring(istable(RemixLight) and RemixLight.CreateSphere ~= nil),
        tostring(istable(RemixLight) and RemixLight.UpdateSphere ~= nil),
        cv_decay_scale:GetFloat(),
        table.Count(wrappedDynamicLights)))
end, nil, "Print DynamicLight wrapper and Remix API status")

-- Public API
local RTXDynamicLightWrapper = {}

function RTXDynamicLightWrapper.GetWrappedCount()
    return table.Count(wrappedDynamicLights)
end

function RTXDynamicLightWrapper.GetWrappedLight(lightId)
    local data = wrappedDynamicLights[lightId]
    return data and data.rtxLightId or nil
end

function RTXDynamicLightWrapper.RemoveWrappedLight(lightId)
    return DestroyWrappedDynamicLight(lightId, "removed by owner")
end

_G.RTXDynamicLightWrapper = RTXDynamicLightWrapper
