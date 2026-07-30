if not (BRANCH == "x86-64" or BRANCH == "chromium") then return end
if not CLIENT then return end

-- Configuration
local cv_enabled = CreateClientConVar("rtx_dynamiclight_wrapper_enabled", "1", true, false, "Enable RTX wrapping for DynamicLight() calls")
local cv_debug = CreateClientConVar("rtx_dynamiclight_wrapper_debug", "0", true, false, "Debug logging for dynamic light wrapper")
local cv_brightness_scale = CreateClientConVar("rtx_dynamiclight_wrapper_brightness_scale", "50", true, false, "Brightness scaling for dynamic lights")
local cv_radius_scale = CreateClientConVar("rtx_dynamiclight_wrapper_radius_scale", "0.01", true, false, "Radius scaling for dynamic lights")

-- Tracking: lightId -> { rtxLightId, lastUpdate, expiresAt, props }
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

-- Store original DynamicLight function
local OriginalDynamicLight = DynamicLight

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
    local currentTime = CurTime()
    -- DynamicLight.DieTime is an absolute CurTime value. Keep the old timeout
    -- only as a defensive fallback for malformed third-party light tables.
    local expiresAt = tonumber(dlight.DieTime) or (currentTime + 0.5)
    
    -- Apply scaling
    local brightnessScale = cv_brightness_scale:GetFloat()
    local radiusScale = cv_radius_scale:GetFloat()
    
    -- Calculate radiance
    local scale = (brightness * brightnessScale) / 100.0
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
    local data = wrappedDynamicLights[lightId]
    
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
                props = { pos = pos, r = r, g = g, b = b, brightness = brightness, size = size }
            }
            DebugPrint("Created RTX light", rtxLightId, "for DynamicLight", lightId)
        end
    else
        -- Update existing RTX light
        local oldProps = data.props
        
        -- Only update if properties changed significantly
        local changed = false
        if oldProps.pos:DistToSqr(pos) > 1 then changed = true end
        if oldProps.r ~= r or oldProps.g ~= g or oldProps.b ~= b then changed = true end
        if math.abs(oldProps.brightness - brightness) > 0.01 then changed = true end
        if math.abs(oldProps.size - size) > 1 then changed = true end
        
        if changed then
            if RemixLight.UpdateSphere then
                RemixLight.UpdateSphere(base, sphere, data.rtxLightId)
            end
            
            data.props = { pos = pos, r = r, g = g, b = b, brightness = brightness, size = size }
            DebugPrint("Updated RTX light", data.rtxLightId, "for DynamicLight", lightId)
        end
        
        -- Refresh lifetime even when the visual properties did not change.
        data.lastUpdate = currentTime
        data.expiresAt = expiresAt
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
        if currentTime >= expiresAt then
            table.insert(expired, lightId)
        end
    end
    
    -- Remove expired lights
    for _, lightId in ipairs(expired) do
        DestroyWrappedDynamicLight(lightId, "expired")
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
    CleanupExpiredLights()
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
