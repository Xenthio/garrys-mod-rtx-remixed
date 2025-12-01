if not (BRANCH == "x86-64" or BRANCH == "chromium") then return end
if not CLIENT then return end

-- Configuration
local cv_enabled = CreateClientConVar("rtx_light_wrapper_enabled", "1", true, false, "Enable automatic RTX light wrapping for legacy light entities")
local cv_debug = CreateClientConVar("rtx_light_wrapper_debug", "0", true, false, "Debug logging for light wrapper")
local cv_skip_change_detection = CreateClientConVar("rtx_light_wrapper_skip_change_detection", "0", true, false, "Skip change detection and update every frame (highest latency but most responsive)")
local cv_update_rate = CreateClientConVar("rtx_light_wrapper_update_rate", "0.001", true, false, "How often to check for entity property changes when change detection is enabled (seconds)")
local cv_brightness_scale = CreateClientConVar("rtx_light_wrapper_brightness_scale", "1", true, false, "Brightness scaling factor (Source uses 0-255, RTX uses radiance)")
local cv_radius_power = CreateClientConVar("rtx_light_wrapper_radius_power", "0.1", true, false, "Power/exponent for radius curve (1.0=linear, <1.0=compress large sizes, >1.0=amplify large sizes)")
local cv_radius_scale_point = CreateClientConVar("rtx_light_wrapper_radius_scale_point", "1.50", true, false, "Radius scaling factor for point lights")
local cv_radius_scale_spot = CreateClientConVar("rtx_light_wrapper_radius_scale_spot", "5", true, false, "Radius scaling factor for spotlights")
local cv_offset_x = CreateClientConVar("rtx_light_wrapper_offset_x", "0", true, false, "Position offset X in local space (forward/back relative to entity)")
local cv_offset_y = CreateClientConVar("rtx_light_wrapper_offset_y", "0", true, false, "Position offset Y in local space (left/right relative to entity)")
local cv_offset_z = CreateClientConVar("rtx_light_wrapper_offset_z", "0", true, false, "Position offset Z in local space (up/down relative to entity)")

-- Light entity classes we want to wrap
local WRAPPABLE_CLASSES = {
    ["gmod_light"] = true,           -- Standard sandbox light tool
    ["gmod_lamp"] = true,            -- Standard sandbox lamp tool (spotlight)
}

-- Tracking table: entity -> { lightId, lastUpdate, cachedProps }
local wrappedLights = {}

local function DebugPrint(...)
    if cv_debug:GetBool() then
        print("[RTX Light Wrapper]", ...)
    end
end

local function vec3(x, y, z)
    return { x = x, y = y, z = z }
end

-- Extract light properties from entity
local function GetEntityLightProps(ent)
    if not IsValid(ent) then return nil end
    
    local classname = ent:GetClass()
    
    local props = {
        pos = ent:GetPos(),
        angles = ent:GetAngles(),
        color = ent:GetColor(),
        enabled = not ent:IsDormant() and ent:GetNoDraw() == false,
    }
    
    -- Special handling for gmod_light (sandbox light tool)
    if classname == "gmod_light" then
        -- Check if light is on
        local isOn = true
        if ent.GetOn then
            isOn = ent:GetOn()
        elseif ent.on ~= nil then
            isOn = ent.on
        end
        props.enabled = isOn and props.enabled
        
        DebugPrint("gmod_light", ent:EntIndex(), "- GetOn():", isOn, "NoDraw:", ent:GetNoDraw(), "Final enabled:", props.enabled)
        
        -- Get brightness (gmod_light uses -10 to 20 scale)
        if ent.GetBrightness then
            local brightness = ent:GetBrightness()
            -- Convert from gmod_light scale to a reasonable radiance value
            -- Scale: brightness 2 = normal, so we use that as baseline
            props.brightness = math.max(0, brightness * 50) -- Adjust multiplier as needed
        else
            props.brightness = 100 -- Default
        end
        
        -- Get radius/size
        if ent.GetLightSize then
            props.radius = ent:GetLightSize()
        elseif ent.Size then
            props.radius = ent.Size
        else
            props.radius = 256 -- Default gmod_light size
        end
    elseif classname == "gmod_lamp" then
        -- Special handling for gmod_lamp (sandbox lamp/spotlight tool)
        
        -- Check if lamp is on
        local isOn = true
        if ent.GetOn then
            isOn = ent:GetOn()
        elseif ent.on ~= nil then
            isOn = ent.on
        end
        props.enabled = isOn and props.enabled
        
        DebugPrint("gmod_lamp", ent:EntIndex(), "- GetOn():", isOn, "NoDraw:", ent:GetNoDraw(), "Final enabled:", props.enabled)
        
        -- Get brightness (gmod_lamp uses 0-8 scale, 4 is default)
        if ent.GetBrightness then
            local brightness = ent:GetBrightness()
            -- Convert from gmod_lamp scale to radiance
            -- Scale: brightness 4 = normal
            props.brightness = math.max(0, brightness * 25)
        else
            props.brightness = 100 -- Default
        end
        
        -- Get distance/radius
        if ent.GetDistance then
            props.radius = ent:GetDistance()
        elseif ent.distance then
            props.radius = ent.distance
        else
            props.radius = 1024 -- Default gmod_lamp distance
        end
        
        -- Get FOV for cone angle
        if ent.GetLightFOV then
            local fov = ent:GetLightFOV()
            -- FOV is full cone angle, convert to half angle
            props.coneAngle = fov / 2
        elseif ent.fov then
            props.coneAngle = ent.fov / 2
        else
            props.coneAngle = 45 -- Default (90 FOV / 2)
        end
        
        -- Set default softness for spotlight
        props.coneSoftness = 0.2
    else
        -- Standard handling for other light types
        
        -- Try to get light-specific networked vars if they exist
        if ent.GetLightColor then
            local col = ent:GetLightColor()
            if col then props.color = col end
        end
        
        if ent.GetBrightness then
            props.brightness = ent:GetBrightness()
        else
            -- Estimate from color intensity
            local c = props.color
            props.brightness = (c.r + c.g + c.b) / 3
        end
        
        if ent.GetDistance then
            props.radius = ent:GetDistance()
        elseif ent.GetRadius then
            props.radius = ent:GetRadius()
        else
            props.radius = 200 -- Default
        end
        
        -- Spotlight properties
        if ent.GetConeAngle then
            props.coneAngle = ent:GetConeAngle()
        elseif ent.GetHorizontalFOV then
            props.coneAngle = ent:GetHorizontalFOV()
        end
        
        if ent.GetConeSoftness then
            props.coneSoftness = ent:GetConeSoftness()
        end
        
        -- Apply brightness scaling for non-gmod_light entities
        props.brightness = props.brightness * cv_brightness_scale:GetFloat()
    end
    
    -- Apply radius power curve to control growth rate
    local radiusPower = cv_radius_power:GetFloat()
    if radiusPower ~= 1.0 and props.radius > 0 then
        props.radius = math.pow(props.radius, radiusPower)
    end
    
    -- Apply radius scaling (all entity types)
    -- Use different scales for point lights vs spotlights
    if props.coneAngle then
        -- This is a spotlight
        props.radius = props.radius * cv_radius_scale_spot:GetFloat()
    else
        -- This is a point light
        props.radius = props.radius * cv_radius_scale_point:GetFloat()
    end
    
    return props
end

-- Check if properties have changed significantly
local function PropsChanged(oldProps, newProps)
    if not oldProps or not newProps then return true end
    
    -- Position changed?
    if oldProps.pos:DistToSqr(newProps.pos) > 1 then return true end
    
    -- Angles changed?
    if math.abs(oldProps.angles.p - newProps.angles.p) > 1 or
       math.abs(oldProps.angles.y - newProps.angles.y) > 1 or
       math.abs(oldProps.angles.r - newProps.angles.r) > 1 then
        return true
    end
    
    -- Color changed?
    if oldProps.color.r ~= newProps.color.r or
       oldProps.color.g ~= newProps.color.g or
       oldProps.color.b ~= newProps.color.b then
        return true
    end
    
    -- Brightness changed?
    if math.abs((oldProps.brightness or 0) - (newProps.brightness or 0)) > 0.01 then
        return true
    end
    
    -- Radius changed?
    if math.abs((oldProps.radius or 0) - (newProps.radius or 0)) > 1 then
        return true
    end
    
    -- Enabled state changed?
    if oldProps.enabled ~= newProps.enabled then
        return true
    end
    
    return false
end

-- Create RTX light from entity
local function CreateRTXLightFromEntity(ent)
    if not IsValid(ent) then return nil end
    if not istable(RemixLight) then
        DebugPrint("RemixLight API not available")
        return nil
    end
    
    local props = GetEntityLightProps(ent)
    if not props then return nil end
    
    -- Apply position offset in local space (relative to entity orientation)
    local pos = props.pos
    local offsetX = cv_offset_x:GetFloat()
    local offsetY = cv_offset_y:GetFloat()
    local offsetZ = cv_offset_z:GetFloat()
    
    if offsetX ~= 0 or offsetY ~= 0 or offsetZ ~= 0 then
        -- Transform local offset to world space using entity angles
        local forward = props.angles:Forward()
        local right = props.angles:Right()
        local up = props.angles:Up()
        
        pos = pos + forward * offsetX + right * offsetY + up * offsetZ
    end
    
    -- If light is disabled, set radiance to zero
    local scale = props.enabled and (props.brightness / 100.0) or 0
    local base = {
        hash = tonumber(util.CRC(string.format("wrapped_light_%d", ent:EntIndex()))) or ent:EntIndex(),
        radiance = vec3(
            props.color.r * scale,
            props.color.g * scale,
            props.color.b * scale
        ),
    }
    
    local sphere = {
        position = vec3(pos.x, pos.y, pos.z),
        radius = props.radius,
        volumetricRadianceScale = 1.0,
    }
    
    -- Add shaping for spotlights
    if props.coneAngle then
        local forward = props.angles:Forward()
        sphere.shaping = {
            direction = vec3(forward.x, forward.y, forward.z),
            coneAngleDegrees = props.coneAngle or 45,
            coneSoftness = props.coneSoftness or 0.2,
            focusExponent = 1.0,
        }
    end
    
    -- Create the light
    local lightId = nil
    if RemixLight.CreateSphere then
        lightId = RemixLight.CreateSphere(base, sphere, ent:EntIndex())
    end
    
    if lightId and lightId ~= 0 then
        DebugPrint("Created RTX light", lightId, "for entity", ent:EntIndex(), ent:GetClass())
        return lightId, props
    end
    
    return nil
end

-- Update RTX light from entity
local function UpdateRTXLightFromEntity(ent, lightId, oldProps)
    if not IsValid(ent) then return false end
    if not istable(RemixLight) then return false end
    if not lightId or lightId == 0 then return false end
    
    local props = GetEntityLightProps(ent)
    if not props then return false end
    
    local skipChangeDetection = cv_skip_change_detection:GetBool()
    
    -- Check if update is needed (skip if forced update mode)
    if not skipChangeDetection and not PropsChanged(oldProps, props) then
        return true -- No change needed, but not an error
    end
    
    -- Apply position offset in local space (relative to entity orientation)
    local pos = props.pos
    local offsetX = cv_offset_x:GetFloat()
    local offsetY = cv_offset_y:GetFloat()
    local offsetZ = cv_offset_z:GetFloat()
    
    if offsetX ~= 0 or offsetY ~= 0 or offsetZ ~= 0 then
        -- Transform local offset to world space using entity angles
        local forward = props.angles:Forward()
        local right = props.angles:Right()
        local up = props.angles:Up()
        
        pos = pos + forward * offsetX + right * offsetY + up * offsetZ
    end
    
    -- If light is disabled, set radiance to zero
    local scale = props.enabled and (props.brightness / 100.0) or 0
    
    -- Debug output for state changes
    if oldProps and oldProps.enabled ~= props.enabled then
        DebugPrint("Entity", ent:EntIndex(), "toggled", props.enabled and "ON" or "OFF")
    end
    
    local base = {
        hash = tonumber(util.CRC(string.format("wrapped_light_%d", ent:EntIndex()))) or ent:EntIndex(),
        radiance = vec3(
            props.color.r * scale,
            props.color.g * scale,
            props.color.b * scale
        ),
    }
    
    local sphere = {
        position = vec3(pos.x, pos.y, pos.z),
        radius = props.radius,
        volumetricRadianceScale = 1.0,
    }
    
    -- Add shaping for spotlights
    if props.coneAngle then
        local forward = props.angles:Forward()
        sphere.shaping = {
            direction = vec3(forward.x, forward.y, forward.z),
            coneAngleDegrees = props.coneAngle or 45,
            coneSoftness = props.coneSoftness or 0.2,
            focusExponent = 1.0,
        }
    end
    
    -- Update the light
    if RemixLight.UpdateSphere then
        RemixLight.UpdateSphere(base, sphere, lightId)
    end
    
    if not skipChangeDetection then
        DebugPrint("Updated RTX light", lightId, "for entity", ent:EntIndex(), "enabled:", props.enabled)
    end
    return true
end

-- Destroy RTX light
local function DestroyRTXLight(lightId)
    if not lightId or lightId == 0 then return end
    
    if istable(RemixLight) and RemixLight.DestroyLight then
        RemixLight.DestroyLight(lightId)
    end
    
    DebugPrint("Destroyed RTX light", lightId)
end

-- Wrap an entity with RTX light
local function WrapEntity(ent)
    if not cv_enabled:GetBool() then return end
    if not IsValid(ent) then return end
    if wrappedLights[ent] then return end -- Already wrapped
    
    local lightId, props = CreateRTXLightFromEntity(ent)
    if lightId then
        wrappedLights[ent] = {
            lightId = lightId,
            lastUpdate = CurTime(),
            cachedProps = props,
        }
    end
end

-- Unwrap an entity
local function UnwrapEntity(ent)
    local data = wrappedLights[ent]
    if not data then return end
    
    DestroyRTXLight(data.lightId)
    wrappedLights[ent] = nil
    DebugPrint("Unwrapped entity", IsValid(ent) and ent:EntIndex() or "invalid")
end

-- Scan for wrappable entities
local function ScanForLights()
    if not cv_enabled:GetBool() then return end
    
    for _, ent in ipairs(ents.GetAll()) do
        if IsValid(ent) and WRAPPABLE_CLASSES[ent:GetClass()] then
            if not wrappedLights[ent] then
                WrapEntity(ent)
            end
        end
    end
end

-- Update all wrapped lights
local function UpdateWrappedLights()
    if not cv_enabled:GetBool() then
        -- Clean up if disabled
        for ent, data in pairs(wrappedLights) do
            UnwrapEntity(ent)
        end
        return
    end
    
    local skipChangeDetection = cv_skip_change_detection:GetBool()
    local updateInterval = cv_update_rate:GetFloat()
    local currentTime = CurTime()
    
    for ent, data in pairs(wrappedLights) do
        if not IsValid(ent) then
            -- Entity removed, clean up
            UnwrapEntity(ent)
        elseif skipChangeDetection or (currentTime - data.lastUpdate >= updateInterval) then
            -- Update every frame (skip mode) or time-based check
            local props = GetEntityLightProps(ent)
            if props then
                if skipChangeDetection or PropsChanged(data.cachedProps, props) then
                    UpdateRTXLightFromEntity(ent, data.lightId, data.cachedProps)
                    data.cachedProps = props
                end
                data.lastUpdate = currentTime
            end
        end
    end
end

-- Hook: Entity created
hook.Add("OnEntityCreated", "RTXLightWrapper_EntityCreated", function(ent)
    if not cv_enabled:GetBool() then return end
    
    -- Delay slightly to ensure entity is fully initialized
    timer.Simple(0.1, function()
        if IsValid(ent) and WRAPPABLE_CLASSES[ent:GetClass()] then
            WrapEntity(ent)
        end
    end)
end)

-- Hook: Think - Update lights periodically
hook.Add("Think", "RTXLightWrapper_UpdateLights", function()
    UpdateWrappedLights()
end)

-- Hook: Cleanup on map change
hook.Add("OnReloaded", "RTXLightWrapper_Cleanup", function()
    for ent, data in pairs(wrappedLights) do
        UnwrapEntity(ent)
    end
    wrappedLights = {}
end)

hook.Add("ShutDown", "RTXLightWrapper_Cleanup", function()
    for ent, data in pairs(wrappedLights) do
        UnwrapEntity(ent)
    end
    wrappedLights = {}
end)

-- Hook: Initial scan after map load
hook.Add("InitPostEntity", "RTXLightWrapper_InitialScan", function()
    if not cv_enabled:GetBool() then return end
    
    timer.Simple(2, function()
        DebugPrint("Performing initial entity scan...")
        ScanForLights()
        local count = table.Count(wrappedLights)
        if count > 0 then
            print("[RTX Light Wrapper] Wrapped " .. count .. " light entities")
        end
    end)
end)

-- Console commands
concommand.Add("rtx_light_wrapper_scan", function()
    ScanForLights()
    print("[RTX Light Wrapper] Scan complete. Wrapped " .. table.Count(wrappedLights) .. " entities")
end, nil, "Scan for wrappable light entities and create RTX lights")

concommand.Add("rtx_light_wrapper_clear", function()
    local count = table.Count(wrappedLights)
    for ent, data in pairs(wrappedLights) do
        UnwrapEntity(ent)
    end
    wrappedLights = {}
    print("[RTX Light Wrapper] Cleared " .. count .. " wrapped lights")
end, nil, "Clear all wrapped RTX lights")

concommand.Add("rtx_light_wrapper_list", function()
    print("[RTX Light Wrapper] Currently wrapped entities:")
    local count = 0
    for ent, data in pairs(wrappedLights) do
        if IsValid(ent) then
            print(string.format("  - Entity %d (%s) -> RTX Light %d", 
                ent:EntIndex(), ent:GetClass(), data.lightId))
            count = count + 1
        end
    end
    print("Total: " .. count .. " entities")
end, nil, "List all wrapped light entities")

concommand.Add("rtx_light_wrapper_force_update", function()
    local count = 0
    for ent, data in pairs(wrappedLights) do
        if IsValid(ent) then
            UpdateRTXLightFromEntity(ent, data.lightId, data.cachedProps)
            data.cachedProps = GetEntityLightProps(ent)
            count = count + 1
        end
    end
    print("[RTX Light Wrapper] Force updated " .. count .. " lights")
end, nil, "Force update all wrapped lights immediately")

concommand.Add("rtx_light_wrapper_reset_offsets", function()
    RunConsoleCommand("rtx_light_wrapper_offset_x", "0")
    RunConsoleCommand("rtx_light_wrapper_offset_y", "0")
    RunConsoleCommand("rtx_light_wrapper_offset_z", "0")
    print("[RTX Light Wrapper] Reset position offsets to 0, 0, 0")
end, nil, "Reset all position offsets to zero")

-- Public API
local RTXLightWrapper = {}

function RTXLightWrapper.WrapEntity(ent)
    WrapEntity(ent)
end

function RTXLightWrapper.UnwrapEntity(ent)
    UnwrapEntity(ent)
end

function RTXLightWrapper.IsWrapped(ent)
    return wrappedLights[ent] ~= nil
end

function RTXLightWrapper.GetWrappedLightID(ent)
    local data = wrappedLights[ent]
    return data and data.lightId or nil
end

function RTXLightWrapper.RegisterClass(className)
    if not WRAPPABLE_CLASSES[className] then
        WRAPPABLE_CLASSES[className] = true
        DebugPrint("Registered class for wrapping:", className)
    end
end

function RTXLightWrapper.UnregisterClass(className)
    WRAPPABLE_CLASSES[className] = nil
    DebugPrint("Unregistered class from wrapping:", className)
end

function RTXLightWrapper.GetWrappedCount()
    return table.Count(wrappedLights)
end

_G.RTXLightWrapper = RTXLightWrapper
