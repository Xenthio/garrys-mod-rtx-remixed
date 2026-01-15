if not (BRANCH == "x86-64" or BRANCH == "chromium") then return end

local brightness_multiplier = CreateClientConVar("rtx_api_map_lights_brightness", "1.0", true, false, "Brightness multiplier for converted lights")
local min_size = CreateClientConVar("rtx_api_map_lights_min_size", "0", true, false, "Minimum size for RTX lights")
local max_size = CreateClientConVar("rtx_api_map_lights_max_size", "4", true, false, "Maximum size for RTX lights")
local visual_mode = CreateClientConVar("rtx_api_map_lights_visual", "0", true, false, "Show visible models for lights")
local debug_mode = CreateClientConVar("rtx_api_map_lights_debug", "0", true, false, "Enable debug messages")
local env_max_brightness = CreateClientConVar("rtx_api_map_lights_env_max_brightness", "3", true, false, "Max brightness (0-100 scale) for directional lights; 0 disables clamping")
local env_dir_flip = CreateClientConVar("rtx_api_map_lights_env_dir_flip", "0", true, false, "Flip directional vector for light_environment if needed")
local creation_delay = CreateClientConVar("rtx_api_map_lights_creation_delay", "0.0", true, false, "Delay between light creation in seconds")

-- Per-type runtime controls
local point_radius_mult = CreateClientConVar("rtx_api_map_lights_point_radius_mult", "1.0", true, false, "Radius multiplier for point lights")
local spot_radius_mult = CreateClientConVar("rtx_api_map_lights_spot_radius_mult", "1.5", true, false, "Radius multiplier for spot lights")
local env_angular_mult = CreateClientConVar("rtx_api_map_lights_env_angular_mult", "1.0", true, false, "Angular diameter multiplier for directional lights")

local point_brightness_mult = CreateClientConVar("rtx_api_map_lights_point_brightness_mult", "1.0", true, false, "Brightness multiplier for point lights")
local spot_brightness_mult = CreateClientConVar("rtx_api_map_lights_spot_brightness_mult", "1.0", true, false, "Brightness multiplier for spot lights")
local env_brightness_mult = CreateClientConVar("rtx_api_map_lights_env_brightness_mult", "1.0", true, false, "Brightness multiplier for directional lights")

local point_volumetric_mult = CreateClientConVar("rtx_api_map_lights_point_volumetric_mult", "1.0", true, false, "Volumetric scale multiplier for point lights")
local spot_volumetric_mult = CreateClientConVar("rtx_api_map_lights_spot_volumetric_mult", "1.0", true, false, "Volumetric scale multiplier for spot lights")
local env_volumetric_mult = CreateClientConVar("rtx_api_map_lights_env_volumetric_mult", "1.0", true, false, "Volumetric scale multiplier for directional lights")

local point_size_mult = CreateClientConVar("rtx_api_map_lights_point_size_mult", "1.0", true, false, "Size scaling multiplier for point lights")
local spot_size_mult = CreateClientConVar("rtx_api_map_lights_spot_size_mult", "1.0", true, false, "Size scaling multiplier for spot lights")
local env_size_mult = CreateClientConVar("rtx_api_map_lights_env_size_mult", "1.0", true, false, "Size scaling multiplier for directional lights")

local creation_batch_size = CreateClientConVar("rtx_api_map_lights_batch_size", "1", true, false, "Number of lights to create in each batch")
local creation_batch_delay = CreateClientConVar("rtx_api_map_lights_batch_delay", "0.0", true, false, "Delay between batches in seconds")
local pos_jitter = CreateClientConVar("rtx_api_map_lights_position_jitter", "1", true, false, "Add a small random offset to light positions to prevent conflicts")
local pos_jitter_amount = CreateClientConVar("rtx_api_map_lights_position_jitter_amount", "0.1", true, false, "Amount of random position offset")

-- Trace mask constants (use GMod globals if available, otherwise define them)
local TRACE_MASK_SOLID = MASK_SOLID or 0x200400B -- CONTENTS_SOLID | CONTENTS_MOVEABLE | CONTENTS_WINDOW | CONTENTS_MONSTER | CONTENTS_GRATE
local rect_rotation_x = CreateClientConVar("rtx_api_map_lights_rect_rotation_x", "0", true, false, "X rotation offset for rectangle and disk lights")
local rect_rotation_y = CreateClientConVar("rtx_api_map_lights_rect_rotation_y", "0", true, false, "Y rotation offset for rectangle and disk lights")
local rect_rotation_z = CreateClientConVar("rtx_api_map_lights_rect_rotation_z", "0", true, false, "Z rotation offset for rectangle and disk lights")

-- env_projectedtexture specific tuning
local projtex_radius_from_fov = CreateClientConVar("rtx_api_map_lights_projtex_radius_from_fov", "1", true, false, "Scale spotlight radius from FOV for env_projectedtexture")
local projtex_fov_baseline = CreateClientConVar("rtx_api_map_lights_projtex_fov_baseline", "45", true, false, "Baseline FOV that maps to radius scale = 1.0")
local projtex_fov_half_angle = CreateClientConVar("rtx_api_map_lights_projtex_fov_half_angle", "1", true, false, "Treat env_projectedtexture FOV as full FOV; convert to half-angle for cone")
local projtex_dir_basis = CreateClientConVar("rtx_api_map_lights_projtex_dir_basis", "0", true, false, "Angles basis for env_projectedtexture: 0=F,1=-F,2=U,3=-U,4=R,5=-R")
local projtex_invert_pitch = CreateClientConVar("rtx_api_map_lights_projtex_invert_pitch", "1", true, false, "Invert pitch sign parsed from angles for env_projectedtexture")

-- Auto-spawn controls
local autospawn = CreateClientConVar("rtx_api_map_lights_autospawn", "1", true, false, "Automatically convert map lights on map start")
local autospawn_delay = CreateClientConVar("rtx_api_map_lights_autospawn_delay", "1.5", true, false, "Delay (seconds) before auto-processing after map start")

-- Debug helpers
local debug_vis = CreateClientConVar("rtx_api_map_lights_debug_vis", "0", true, false, "Draw debug direction for spotlights")
local debug_hud = CreateClientConVar("rtx_api_map_lights_debug_hud", "0", true, false, "Show HUD overlay with light positions and info")
local debug_hud_max_distance = CreateClientConVar("rtx_api_map_lights_debug_hud_max_distance", "2048", true, false, "Maximum distance to show lights in HUD")
local debug_hud_show_disabled = CreateClientConVar("rtx_api_map_lights_debug_hud_show_disabled", "0", true, false, "Show disabled/dark lights in HUD")
local spot_dir_basis = CreateClientConVar("rtx_api_map_lights_dir_basis", "0", true, false, "Angles basis if no target: 0=F,1=-F,2=U,3=-U,4=R,5=-R")
local debug_beam_mat = Material("cable/physbeam")

-- Optional queue include to throttle RemixLight operations
if file.Exists("remixlua/cl/remixapi/cl_remix_light_queue.lua", "LUA") then
    include("remixlua/cl/remixapi/cl_remix_light_queue.lua")
end

-- Track created entities so we can clean them up
local createdLights = {}
local last_entity_id = 0
local createdLightPositions = {}
local lastSpawnedMap = ""
-- Per-kind registries for quick runtime updates
local lightsByKind = { point = {}, spot = {}, env = {} } -- values: lightId -> true
local idToIndex = {} -- lightId -> index in createdLights
local lightsByName = {} -- lower(targetname) -> { [lightId] = true }

-- Light entity classes we want to detect
local lightClasses = {
    ["light"] = true,
    ["light_spot"] = true,
    ["light_dynamic"] = true,
    ["light_environment"] = true,
    ["env_projectedtexture"] = true,
}

-- Mapping from Source light types to RTX light types
local rtxLightTypes = {
    ["light"] = 0, -- Sphere light
    ["light_dynamic"] = 0, -- Sphere light
    ["light_spot"] = 0, -- Disk light
    ["env_projectedtexture"] = 0, -- Treated as spotlight (shaped sphere)
    ["light_environment"] = 3, -- Distant (directional) light
}

-- Visual models to use for different light types
local lightModels = {
    ["light"] = "models/hunter/misc/sphere025x025.mdl",
    ["light_spot"] = "models/hunter/misc/sphere025x025.mdl",
    ["light_dynamic"] = "models/hunter/misc/sphere025x025.mdl",
    ["light_environment"] = "models/hunter/misc/sphere025x025.mdl",
    ["env_projectedtexture"] = "models/hunter/misc/sphere025x025.mdl",
}

-- Print debug messages if debug mode is enabled
local function DebugPrint(...)
    if debug_mode:GetBool() then
        print("[Light2RTX Debug]", ...)
    end
end

-- Convert a string "x y z" to a Vector
local function StringToVector(str)
    if type(str) == "Vector" then 
        return str -- Already a Vector
    elseif type(str) == "string" then
        local x, y, z = string.match(str, "([%-%.%d]+)%s+([%-%.%d]+)%s+([%-%.%d]+)")
        if x and y and z then
            return Vector(tonumber(x), tonumber(y), tonumber(z))
        end
    end
    return Vector(0, 0, 0)
end

-- Robustly parse Source entity angle fields. Returns Angle or nil, and a debug source tag
local function ParseEntityAngles(ent)
    local function applyEnvPitchOverride(ent, ang, tag)
        if ent and ent.classname == "light_environment" then
            local pOverride = tonumber(ent.pitch or ent._pitch)
            if pOverride then
                ang.p = -(pOverride)
                tag = tostring(tag or "") .. "+envpitch"
            end
        end
        return ang, tag
    end
    local angField = ent.angles or ent._angles
    -- Support string angles: "pitch yaw roll" or just "yaw"
    if type(angField) == "string" then
        local ax, ay, az = string.match(angField, "([%-%.%d]+)%s+([%-%.%d]+)%s+([%-%.%d]+)")
        if ax and ay and az then
            -- Source convention: +pitch looks down -> negate when building Angle
            local a = Angle(-(tonumber(ax) or 0), tonumber(ay) or 0, tonumber(az) or 0)
            -- Check for standalone pitch/yaw fields - they override the angles field when present
            local standalonePitch = tonumber(ent.pitch or ent._pitch)
            local standaloneYaw = tonumber(ent.angle or ent._angle or ent.yaw or ent._yaw)
            local overridden = false
            if standalonePitch and standalonePitch ~= 0 then
                a.p = -standalonePitch
                overridden = true
            end
            if standaloneYaw and standaloneYaw ~= 0 and a.y == 0 then
                a.y = standaloneYaw
                overridden = true
            end
            if overridden then
                return a, "angles_xyz+standalone_override"
            end
            return applyEnvPitchOverride(ent, a, "angles_xyz")
        end
        local yawOnly = tonumber(angField)
        if yawOnly then
            local pitch = tonumber(ent.pitch or ent._pitch or 0)
            return Angle(-pitch, yawOnly, 0), "angles_yaw"
        end
    elseif type(angField) == "number" then
        -- Some entities may store yaw-only in 'angles' as a number
        local pitch = tonumber(ent.pitch or ent._pitch or 0)
        return Angle(-pitch, angField, 0), "angles_yaw_num"
    elseif (isvector and isvector(angField)) or type(angField) == "Vector" then
        -- Some BSP libs may expose angles as a Vector
        local a = Angle(-(angField.x or 0), angField.y or 0, angField.z or 0)
        -- Check for standalone pitch/yaw fields - they override the angles field when present
        local standalonePitch = tonumber(ent.pitch or ent._pitch)
        local standaloneYaw = tonumber(ent.angle or ent._angle or ent.yaw or ent._yaw)
        local overridden = false
        if standalonePitch and standalonePitch ~= 0 then
            a.p = -standalonePitch
            overridden = true
        end
        if standaloneYaw and standaloneYaw ~= 0 then
            a.y = standaloneYaw
            overridden = true
        end
        if overridden then
            return a, "angles_vec+standalone_override"
        end
        return applyEnvPitchOverride(ent, a, "angles_vec")
    elseif istable(angField) and angField.x and angField.y and angField.z then
        local a = Angle(-(angField.x or 0), angField.y or 0, angField.z or 0)
        -- Check for standalone pitch/yaw fields - they override the angles field when present
        local standalonePitch = tonumber(ent.pitch or ent._pitch)
        local standaloneYaw = tonumber(ent.angle or ent._angle or ent.yaw or ent._yaw)
        local overridden = false
        if standalonePitch and standalonePitch ~= 0 then
            a.p = -standalonePitch
            overridden = true
        end
        if standaloneYaw and standaloneYaw ~= 0 then
            a.y = standaloneYaw
            overridden = true
        end
        if overridden then
            return a, "angles_tbl+standalone_override"
        end
        return applyEnvPitchOverride(ent, a, "angles_tbl")
    elseif istable(angField) then
        -- Support array-like tables: {pitch, yaw, roll}
        local ax, ay, az = tonumber(angField[1] or 0), tonumber(angField[2] or 0), tonumber(angField[3] or 0)
        if ax ~= 0 or ay ~= 0 or az ~= 0 then
            local a = Angle(-ax, ay, az)
            -- Check for standalone pitch/yaw fields - they override the angles field when present
            local standalonePitch = tonumber(ent.pitch or ent._pitch)
            local standaloneYaw = tonumber(ent.angle or ent._angle or ent.yaw or ent._yaw)
            local overridden = false
            if standalonePitch and standalonePitch ~= 0 then
                a.p = -standalonePitch
                overridden = true
            end
            if standaloneYaw and standaloneYaw ~= 0 then
                a.y = standaloneYaw
                overridden = true
            end
            if overridden then
                return a, "angles_tbl_idx+standalone_override"
            end
            return applyEnvPitchOverride(ent, a, "angles_tbl_idx")
        end
        -- All zeros in array, check standalone fields
        local standalonePitch = tonumber(ent.pitch or ent._pitch)
        local standaloneYaw = tonumber(ent.angle or ent._angle or ent.yaw or ent._yaw)
        if standalonePitch or standaloneYaw then
            local a = Angle(-(standalonePitch or 0), standaloneYaw or 0, 0)
            return a, "angles_tbl_idx_zero+standalone"
        end
    elseif (isangle and isangle(angField)) then
        -- GLua Angle userdata
        local a = Angle(-(angField.p or 0), angField.y or 0, angField.r or 0)
        -- Check for standalone pitch/yaw fields - they override the angles field when present
        local standalonePitch = tonumber(ent.pitch or ent._pitch)
        local standaloneYaw = tonumber(ent.angle or ent._angle or ent.yaw or ent._yaw)
        local overridden = false
        if standalonePitch and standalonePitch ~= 0 then
            a.p = -standalonePitch
            overridden = true
        end
        if standaloneYaw and standaloneYaw ~= 0 then
            a.y = standaloneYaw
            overridden = true
        end
        if overridden then
            return a, "angles_glua+standalone_override"
        end
        return applyEnvPitchOverride(ent, a, "angles_glua")
    elseif type(angField) == "Angle" then
        -- Angle type without isangle available
        local a = Angle(-(angField.p or 0), angField.y or 0, angField.r or 0)
        -- Check for standalone pitch/yaw fields - they override the angles field when present
        local standalonePitch = tonumber(ent.pitch or ent._pitch)
        local standaloneYaw = tonumber(ent.angle or ent._angle or ent.yaw or ent._yaw)
        local overridden = false
        if standalonePitch and standalonePitch ~= 0 then
            a.p = -standalonePitch
            overridden = true
        end
        if standaloneYaw and standaloneYaw ~= 0 then
            a.y = standaloneYaw
            overridden = true
        end
        if overridden then
            return a, "angles_type_Angle+standalone_override"
        end
        return applyEnvPitchOverride(ent, a, "angles_type_Angle")
    elseif type(angField) == "userdata" then
        -- Generic userdata exposing p/y/r or pitch/yaw/roll
        local p = angField.p or angField.pitch
        local y = angField.y or angField.yaw
        local r = angField.r or angField.roll
        if p ~= nil or y ~= nil or r ~= nil then
            local a = Angle(-(tonumber(p) or 0), tonumber(y) or 0, tonumber(r) or 0)
            return applyEnvPitchOverride(ent, a, "angles_userdata")
        end
        -- Try parsing its string representation (e.g., "-16.000 45.000 0.000")
        local s = tostring(angField)
        if type(s) == "string" then
            local ax, ay, az = string.match(s, "([%-%.%d]+)%s+([%-%.%d]+)%s+([%-%.%d]+)")
            if ax and ay and az then
                local a = Angle(-(tonumber(ax) or 0), tonumber(ay) or 0, tonumber(az) or 0)
                return applyEnvPitchOverride(ent, a, "angles_userdata_str")
            end
        end
    end
    -- Generic fallback: if we have any angles field at all, try parsing its string form
    if angField ~= nil then
        local s = tostring(angField)
        if type(s) == "string" then
            local ax, ay, az = string.match(s, "([%-%.%d]+)%s+([%-%.%d]+)%s+([%-%.%d]+)")
            if ax and ay and az then
                local a = Angle(-(tonumber(ax) or 0), tonumber(ay) or 0, tonumber(az) or 0)
                -- If angles field is all zeros, check for standalone pitch/yaw fields
                if a.p == 0 and a.y == 0 and a.r == 0 then
                    local standalonePitch = tonumber(ent.pitch or ent._pitch)
                    local standaloneYaw = tonumber(ent.angle or ent._angle or ent.yaw or ent._yaw)
                    if standalonePitch and standalonePitch ~= 0 then
                        a.p = -standalonePitch
                        return a, "angles_any_str_zero+pitch_override"
                    elseif standaloneYaw and standaloneYaw ~= 0 then
                        a.y = standaloneYaw
                        return a, "angles_any_str_zero+yaw_override"
                    end
                end
                return applyEnvPitchOverride(ent, a, "angles_any_str")
            end
        end
    end
    -- Fallback to separate fields
    local hasPitch = ent.pitch or ent._pitch
    local yawRaw = ent.angle or ent._angle or ent.yaw or ent._yaw
    local hasYaw = yawRaw ~= nil
    if hasPitch or hasYaw then
        local pitch = tonumber(ent.pitch or ent._pitch or 0)
        local yaw = tonumber(yawRaw or 0)
        local tag = (yawRaw ~= nil and "pitch_yaw") or "pitch_only"
        return Angle(-pitch, yaw, 0), tag
    end
    return nil, "none"
end

-- Convert sRGB (0-255) to linear space for physically accurate lighting
local function srgbToLinear(c)
    c = c / 255.0
    return (c <= 0.04045) and (c / 12.92) or math.pow((c + 0.055) / 1.055, 2.4)
end

-- Helper function to estimate appropriate light size based on brightness
-- Returns: finalSize, baseSizeBeforeMultipliers
local function estimateLightSize(brightness, entitySize, lightType)
    -- Base size - scaled down to work with per-type multipliers
    local baseSize = entitySize or 5
    
    -- Scale slightly by brightness (reduced influence)
    baseSize = baseSize * (1 + brightness / 1000)
    
    -- Store this value BEFORE type multiplier (for runtime updates)
    local baseSizeBeforeMultipliers = baseSize
    
    -- Apply per-type size multiplier
    local typeMult = 1.0
    if lightType == "light" or lightType == "light_dynamic" then
        typeMult = point_size_mult:GetFloat()
    elseif lightType == "light_spot" or lightType == "env_projectedtexture" then
        typeMult = spot_size_mult:GetFloat()
    elseif lightType == "light_environment" then
        typeMult = env_size_mult:GetFloat()
    end
    baseSize = baseSize * typeMult
    
    -- Enforce minimum and maximum size
    return math.Clamp(baseSize, min_size:GetFloat(), max_size:GetFloat()), baseSizeBeforeMultipliers
end

-- Helper function to estimate light brightness and color from BSP entity data
local function getLightProperties(entity)
    local color = Color(255, 255, 255)
    local brightness = 100
    local entitySize = nil
    local lightType = rtxLightTypes[entity.classname] or rtxLightTypes["default"]
    
    -- Special properties for each light type
    local lightProps = {
        rectWidth = 100,
        rectHeight = 100,
        angularDiameter = 0.5,
        coneAngle = 120,
        coneSoftness = 0.2,
        shapingEnabled = false
    }
    
    -- Extract color information from the _light property if available
    -- Format: "R G B I" or sometimes just "R G B" (defaults intensity to 255)
    if entity._light then
        -- Try parsing 4 values first (R G B I)
        local r, g, b, i = string.match(entity._light or "", "([%+%-]?%d+)%s+([%+%-]?%d+)%s+([%+%-]?%d+)%s+([%+%-]?%d+)")
        if r and g and b and i then
            r, g, b, i = tonumber(r), tonumber(g), tonumber(b), tonumber(i)
            -- Clamp negative values to 0 (some maps have malformed data)
            if r < 0 then r = 0 end
            if g < 0 then g = 0 end
            if b < 0 then b = 0 end
            if i < 0 then i = 255 end
            color = Color(r, g, b)
            brightness = i
        else
            -- Try parsing 3 values (R G B), default intensity to 200
            r, g, b = string.match(entity._light or "", "([%+%-]?%d+)%s+([%+%-]?%d+)%s+([%+%-]?%d+)")
            if r and g and b then
                r, g, b = tonumber(r), tonumber(g), tonumber(b)
                -- Clamp negative values
                if r < 0 then r = 0 end
                if g < 0 then g = 0 end
                if b < 0 then b = 0 end
                color = Color(r, g, b)
                brightness = 200  -- Default intensity when not specified (matches Source Engine's vrad)
            end
        end
    end
    
    -- Check _lightHDR for sentinel value (indicates "use SDR values")
    -- Sentinel formats: "-1 -1 -1 1" or any negative RGB with negative intensity
    if entity._lightHDR then
        local hr, hg, hb, hi = string.match(entity._lightHDR or "", "([%+%-]?%d+)%s+([%+%-]?%d+)%s+([%+%-]?%d+)%s+([%+%-]?%d+)")
        if hr and hg and hb and hi then
            hr, hg, hb, hi = tonumber(hr), tonumber(hg), tonumber(hb), tonumber(hi)
            local hasNegativeRGB = (hr < 0 or hg < 0 or hb < 0)
            local hasNegativeIntensity = (hi and hi < 0)
            -- If NOT a sentinel (no negative values), use HDR values instead
            if not ((hr < 0 and hg < 0 and hb < 0) or (hasNegativeRGB and hasNegativeIntensity)) then
                -- Valid HDR color, use it
                if hr >= 0 and hg >= 0 and hb >= 0 and hi >= 0 then
                    color = Color(hr, hg, hb)
                    brightness = hi
                end
            end
            -- Otherwise it's a sentinel, keep using SDR values from above
        end
    end
    
    -- Get size from entity properties and falloff distances
    -- Source Engine uses these to control light attenuation:
    -- _fifty_percent_distance (_distance): Distance where light is 50% bright
    -- _zero_percent_distance: Distance where light reaches 0%
    
    -- Only apply falloff-based calculations if these fields are actually present
    local fiftyPercent = tonumber(entity._fifty_percent_distance or entity._distance or entity.distance)
    local zeroPercent = tonumber(entity._zero_percent_distance)
    
    if fiftyPercent and zeroPercent and fiftyPercent > 0 and zeroPercent > fiftyPercent then
        -- Hybrid approach: Use fifty_percent for radius hint
        -- Since RTX uses physical PBR (inverse-square falloff), we approximate Source's arbitrary curves
        
        -- Calculate radius from fifty_percent with a baseline offset to prevent tiny lights
        -- Use a more conservative divisor and add a minimum base size
        local radiusFromFalloff = (fiftyPercent / 40.0) + 2.0  -- Base size of 2.0 + scaled component
        entitySize = radiusFromFalloff
        
        -- Apply a very modest brightness adjustment using square root scaling
        -- This prevents extreme brightness for lights with large falloff distances
        -- Square root provides diminishing returns: 100->1.0x, 400->2.0x, 900->3.0x
        local baseline = 100.0  -- Source's typical default falloff distance
        local reachScale = math.sqrt(fiftyPercent / baseline)
        -- Cap the multiplier to reasonable values
        reachScale = math.Clamp(reachScale, 0.3, 2.0)
        brightness = brightness * reachScale
    end
    
    -- Estimate appropriate size
    local size, baseSizeBeforeMultipliers = estimateLightSize(brightness, entitySize, entity.classname)
    
    -- Store the base size for runtime updates
    lightProps.baseSizeBeforeMultipliers = baseSizeBeforeMultipliers
    
    -- Special handling for certain light types
    if entity.classname == "light_environment" then
        -- Size adjustment for environment lights
        size = size * 1.5
        -- Read sun spread/diameter if available, else default to ~solar disc size
        local spread = tonumber(entity.sunspreadangle or entity._sunspreadangle or 0.53)
        lightProps.angularDiameter = spread or 0.53
        -- Store HDR brightness scale for use in intensity calculation
        lightProps._lightscaleHDR = tonumber(entity._lightscaleHDR or entity.lightscaleHDR or 1.0)
        -- Derive directional angles (reuse robust parser)
        local a, src = ParseEntityAngles(entity)
        if debug_mode:GetBool() then
            local function tv(v)
                local t = type(v)
                if t == "table" then return "table" end
                if t == "Vector" or (isvector and isvector(v)) then
                    return string.format("Vector(%.2f,%.2f,%.2f)", v.x or 0, v.y or 0, v.z or 0)
                end
                return tostring(v)
            end
            print("[Light2RTX Debug] env raw angle fields:",
                "angles=", tv(entity.angles),
                "_angles=", tv(entity._angles),
                "angle=", tv(entity.angle),
                "_angle=", tv(entity._angle),
                "yaw=", tv(entity.yaw),
                "_yaw=", tv(entity._yaw),
                "pitch=", tv(entity.pitch),
                "_pitch=", tv(entity._pitch))
        end
        if a then
            lightProps.angles = a
            local dir = a:Forward()
            if env_dir_flip:GetBool() then dir = -dir end
            lightProps.direction = dir
            lightProps.debugSource = (src or "?") .. "+light_environment"
        end
    elseif entity.classname == "light_spot" then
        -- Dump raw fields to diagnose angle parsing (always show for light_spot with potential zero angles issue)
        local function tv(v)
            local t = type(v)
            if t == "table" then return "table" end
            if t == "Vector" or (isvector and isvector(v)) then
                return string.format("Vector(%.2f,%.2f,%.2f)", v.x or 0, v.y or 0, v.z or 0)
            end
            return tostring(v)
        end
        local showDebug = debug_mode:GetBool()
        -- Always show debug if angles is "0 0 0" but pitch field exists
        local anglesStr = tostring(entity.angles or entity._angles or "")
        local hasSeparatePitch = (entity.pitch ~= nil or entity._pitch ~= nil)
        if anglesStr:match("^0%s+0%s+0") and hasSeparatePitch then
            showDebug = true
        end
        if showDebug then
            print("[Light2RTX Debug] spot raw angle fields:",
                "angles=", tv(entity.angles),
                "_angles=", tv(entity._angles),
                "angle=", tv(entity.angle),
                "_angle=", tv(entity._angle),
                "yaw=", tv(entity.yaw),
                "_yaw=", tv(entity._yaw),
                "pitch=", tv(entity.pitch),
                "_pitch=", tv(entity._pitch))
        end
        -- For spotlights, extract cone parameters
        local coneAngle = tonumber(entity.cone or entity._cone or 45)
        lightProps.coneAngle = coneAngle
        lightProps.coneSoftness = 0.2 -- Default softness
        
        -- Adjust rect dimensions based on cone angle
        -- Wider cone = wider rectangle
        local aspectRatio = 1.0 -- Default aspect ratio for the rectangle
        lightProps.rectWidth = size * aspectRatio
        lightProps.rectHeight = size
        
        -- Derive spotlight angles and direction consistently (Source: +pitch looks down)
        local a, src = ParseEntityAngles(entity)
        if a then
            lightProps.angles = a
            -- Provide a direction immediately; later logic will respect this if no target exists
            lightProps.direction = a:Forward()
            lightProps.shapingEnabled = true
            lightProps.debugSource = src .. "+getLightProperties"
            if showDebug then
                print(string.format("[Light2RTX Debug] spot parsed: pitch=%.2f yaw=%.2f roll=%.2f src=%s dir=(%.2f,%.2f,%.2f)", 
                    a.p, a.y, a.r, src, lightProps.direction.x, lightProps.direction.y, lightProps.direction.z))
            end
        end
    elseif entity.classname == "env_projectedtexture" then
        if debug_mode:GetBool() then
            local function tv(v)
                local t = type(v)
                if t == "table" then return "table" end
                if t == "Vector" or (isvector and isvector(v)) then
                    return string.format("Vector(%.2f,%.2f,%.2f)", v.x or 0, v.y or 0, v.z or 0)
                end
                return tostring(v)
            end
            print("[Light2RTX Debug] env_projectedtexture raw fields:",
                "lightcolor=", tv(entity.lightcolor),
                "_light=", tv(entity._light),
                "lightfov=", tv(entity.lightfov),
                "angles=", tv(entity.angles))
        end
        -- Color/brightness: prefer lightcolor (R G B [A]) then fallback to _light (R G B I)
        local lr, lg, lb, la = string.match(tostring(entity.lightcolor or ""), "(%d+)%s+(%d+)%s+(%d+)%s*([%d%.%-]*)")
        if lr and lg and lb then
            lr, lg, lb = tonumber(lr), tonumber(lg), tonumber(lb)
            color = Color(lr, lg, lb)
            
            -- Normalize env_projectedtexture brightness:
            --  - Small values (0..10) are treated as 0..100
            --  - Typical values (0..255) map to 0..100 via /2.55
            --  - HDR values (>255, e.g. 10000) map to 0..100 via /100
            local iv = tonumber(la)
            if iv then
                if iv <= 10 then
                    brightness = math.max(0, iv * 10)
                elseif iv <= 255 then
                    brightness = math.max(0, iv / 2.55)
                else
                    brightness = math.max(0, iv / 100.0)
                end
            end
        elseif entity._light then
            local r, g, b, i = string.match(entity._light or "", "(%d+)%s+(%d+)%s+(%d+)%s+(%d+)")
            if r and g and b then
                r, g, b = tonumber(r), tonumber(g), tonumber(b)
                color = Color(r, g, b)
                if i then
                    local iv = tonumber(i)
                    if iv then
                        if iv <= 255 then brightness = math.max(0, iv / 2.55) else brightness = math.max(0, iv / 100.0) end
                    end
                end
            end
        end
        -- Optional global/intensity scale property ("Light Strength" in Hammer)
        local scale = tonumber(
            entity.lightstrength or entity._lightstrength or
            entity.lightbrightnessscale or entity._lightbrightnessscale or
            entity.brightnessscale or entity._brightnessscale or
            entity.brightness or entity._brightness or 1) or 1
        -- Apply even when scale is 0 to allow disabling the light
        brightness = brightness * math.max(0.0, scale)
        -- FOV -> cone angle (support multiple common keys)
        local fov = tonumber(entity.fov or entity.lightfov or entity._lightfov or 45) or 45
        -- Optionally convert full FOV to half-angle, which is a common convention for cone apertures
        local useAngle = fov
        if projtex_fov_half_angle:GetBool() then
            useAngle = fov * 0.5
        end
        lightProps.coneAngle = useAngle
        lightProps.coneSoftness = 0.2
        -- Rectangle proxy dimensions roughly scaled by size estimate
        local aspectRatio = 1.0
        lightProps.rectWidth = size * aspectRatio
        lightProps.rectHeight = size
        -- Optionally scale base size by FOV so cone gets wider/narrower instead of tilting direction
        if projtex_radius_from_fov:GetBool() then
            local base = math.max(1.0, projtex_fov_baseline:GetFloat() or 45)
            local scaleFromFov = (fov / base)
            size = size * math.max(0.1, scaleFromFov)
        end
        -- Derive direction from angles
        local a, src = ParseEntityAngles(entity)
        if a then
            -- Optional pitch inversion for env_projectedtexture to match Hammer view gizmo
            if projtex_invert_pitch:GetBool() then
                a.p = -(a.p or 0)
            end
            -- Choose a basis vector from angles for projection direction
            local basis = projtex_dir_basis:GetInt()
            local dir = a:Forward()
            if basis == 0 then dir = a:Forward()
            elseif basis == 1 then dir = -a:Forward()
            elseif basis == 2 then dir = a:Up()
            elseif basis == 3 then dir = -a:Up()
            elseif basis == 4 then dir = a:Right()
            elseif basis == 5 then dir = -a:Right()
            end
            lightProps.angles = a
            lightProps.direction = dir
            lightProps.shapingEnabled = true
            lightProps.debugSource = (src or "?") .. "+env_projectedtexture"
        end
    end
    
    return color, brightness, size, lightType, lightProps
end

-- Check if a position is within map bounds and clamp it if needed
local function validateAndClampPosition(pos, classname)
    if not NikNaks or not NikNaks.CurrentMap then return pos end
    
    local bsp = NikNaks.CurrentMap
    
    -- Try to get map bounds from BSP
    local mins, maxs
    
    -- Method 1: Try GetModels() to get world geometry bounds (model 0 = worldspawn)
    if bsp.GetModels then
        local models = bsp:GetModels()
        if models and models[0] then
            local worldModel = models[0]
            if worldModel.min and worldModel.max then
                mins = worldModel.min
                maxs = worldModel.max
                DebugPrint(string.format("Got map bounds from model: mins=(%.1f, %.1f, %.1f) maxs=(%.1f, %.1f, %.1f)", 
                    mins.x, mins.y, mins.z, maxs.x, maxs.y, maxs.z))
            end
        end
    end
    
    -- Method 2: Try GetModel(0) directly
    if not mins and bsp.GetModel then
        local worldModel = bsp:GetModel(0)
        if worldModel and worldModel.min and worldModel.max then
            mins = worldModel.min
            maxs = worldModel.max
            DebugPrint(string.format("Got map bounds from GetModel(0): mins=(%.1f, %.1f, %.1f) maxs=(%.1f, %.1f, %.1f)", 
                mins.x, mins.y, mins.z, maxs.x, maxs.y, maxs.z))
        end
    end
    
    -- Method 3: Calculate bounds from all brush entities
    if not mins and bsp.GetEntities then
        local calcMins = Vector(999999, 999999, 999999)
        local calcMaxs = Vector(-999999, -999999, -999999)
        local foundAny = false
        
        for _, ent in pairs(bsp:GetEntities()) do
            if ent.origin then
                local entPos = StringToVector(ent.origin)
                if entPos ~= Vector(0, 0, 0) then
                    calcMins.x = math.min(calcMins.x, entPos.x)
                    calcMins.y = math.min(calcMins.y, entPos.y)
                    calcMins.z = math.min(calcMins.z, entPos.z)
                    calcMaxs.x = math.max(calcMaxs.x, entPos.x)
                    calcMaxs.y = math.max(calcMaxs.y, entPos.y)
                    calcMaxs.z = math.max(calcMaxs.z, entPos.z)
                    foundAny = true
                end
            end
        end
        
        if foundAny then
            -- Add padding around calculated bounds
            local padding = 512
            mins = Vector(calcMins.x - padding, calcMins.y - padding, calcMins.z - padding)
            maxs = Vector(calcMaxs.x + padding, calcMaxs.y + padding, calcMaxs.z + padding)
            DebugPrint(string.format("Calculated map bounds from entities: mins=(%.1f, %.1f, %.1f) maxs=(%.1f, %.1f, %.1f)", 
                mins.x, mins.y, mins.z, maxs.x, maxs.y, maxs.z))
        end
    end
    
    -- Method 4: Use a reasonable default fallback (very large bounds)
    if not mins or not maxs then
        mins = Vector(-16384, -16384, -16384)
        maxs = Vector(16384, 16384, 16384)
        DebugPrint("Using default map bounds for clamping")
    end
    
    -- Check if position is out of bounds
    local clamped = Vector(pos.x, pos.y, pos.z)
    local wasOutOfBounds = false
    
    if pos.x < mins.x then clamped.x = mins.x + 10; wasOutOfBounds = true end
    if pos.y < mins.y then clamped.y = mins.y + 10; wasOutOfBounds = true end
    if pos.z < mins.z then clamped.z = mins.z + 10; wasOutOfBounds = true end
    if pos.x > maxs.x then clamped.x = maxs.x - 10; wasOutOfBounds = true end
    if pos.y > maxs.y then clamped.y = maxs.y - 10; wasOutOfBounds = true end
    if pos.z > maxs.z then clamped.z = maxs.z - 10; wasOutOfBounds = true end
    
    if wasOutOfBounds then
        print(string.format("[Light2RTX] WARNING: %s at (%.1f, %.1f, %.1f) is out of bounds! Clamped to (%.1f, %.1f, %.1f)", 
            classname or "light", pos.x, pos.y, pos.z, clamped.x, clamped.y, clamped.z))
    end
    
    return clamped
end

-- Check if a position is inside solid geometry and try to move it to a valid position
-- If lightDir is provided (for spotlights), prefer moving in that direction
local function validatePositionAgainstGeometry(pos, classname, lightDir)
    local isInSolid = false
    local checkMethod = "none"
    
    -- Method 1: Check BSP geometry using NikNaks
    if NikNaks and NikNaks.CurrentMap and NikNaks.CurrentMap.PointContents then
        local contents = NikNaks.CurrentMap:PointContents(pos)
        local CONTENTS_SOLID = 1
        if contents and bit.band(contents, CONTENTS_SOLID) ~= 0 then
            isInSolid = true
            checkMethod = "BSP"
        end
    end
    
    -- Method 2: Check for collision with world + static props using hull trace
    -- TraceHull is better at detecting if we're inside geometry
    if not isInSolid then
        local hullSize = 4 -- Small hull to detect tight spaces
        local trace = util.TraceHull({
            start = pos,
            endpos = pos, -- Zero-length trace to check current position
            mins = Vector(-hullSize, -hullSize, -hullSize),
            maxs = Vector(hullSize, hullSize, hullSize),
            mask = TRACE_MASK_SOLID, -- Check everything solid (world + props)
            -- No filter - we want to detect ALL solid geometry including props
        })
        
        if debug_mode:GetBool() then
            local hitEnt = "nil"
            if trace.Entity and IsValid(trace.Entity) then
                hitEnt = trace.Entity:GetClass()
            elseif trace.Entity then
                hitEnt = "world"
            end
            DebugPrint(string.format("Hull trace at (%.1f,%.1f,%.1f): StartSolid=%s AllSolid=%s Hit=%s HitEnt=%s", 
                pos.x, pos.y, pos.z, tostring(trace.StartSolid), tostring(trace.AllSolid), tostring(trace.Hit), hitEnt))
        end
        
        if trace.StartSolid or trace.AllSolid then
            isInSolid = true
            checkMethod = "trace_hull"
        end
    end
    
    -- Method 3: Check if light is inside a prop_static bounding box
    -- This catches non-solid props (like lamp fixtures) that don't have collision
    if not isInSolid and NikNaks and NikNaks.CurrentMap then
        local bsp = NikNaks.CurrentMap
        
        -- Get static props from BSP using NikNaks API
        if bsp.GetStaticProps then
            local props = bsp:GetStaticProps()
            local propCount = #props
            local nearbyProps = 0
            
            for _, prop in pairs(props) do
                -- StaticProp has Origin (capital O) and PropType fields
                local propOrigin = prop.Origin
                local propModel = prop.PropType
                
                if propOrigin and propModel then
                    -- Check if prop is nearby (within 100 units) before doing expensive checks
                    local dist = pos:Distance(propOrigin)
                    if dist < 100 then
                        nearbyProps = nearbyProps + 1
                        
                        -- Get model bounds using NikNaks StaticProp:GetModelBounds() method
                        -- This automatically includes scale
                        local mins, maxs = prop:GetModelBounds()
                        
                        -- Fallback if GetModelBounds fails
                        if not mins or not maxs then
                            mins = Vector(-32, -32, -32)
                            maxs = Vector(32, 32, 32)
                        end
                        
                        -- Transform bounds to world space
                        local worldMins = propOrigin + mins
                        local worldMaxs = propOrigin + maxs
                        
                        if debug_mode:GetBool() then
                            DebugPrint(string.format("Checking prop at distance %.1f: %s", dist, propModel))
                            DebugPrint(string.format("  Light: (%.1f,%.1f,%.1f), Prop: (%.1f,%.1f,%.1f)", 
                                pos.x, pos.y, pos.z, propOrigin.x, propOrigin.y, propOrigin.z))
                            DebugPrint(string.format("  BBox: mins=(%.1f,%.1f,%.1f) maxs=(%.1f,%.1f,%.1f)",
                                worldMins.x, worldMins.y, worldMins.z, worldMaxs.x, worldMaxs.y, worldMaxs.z))
                        end
                        
                        -- Check if light is inside bounding box
                        if pos.x >= worldMins.x and pos.x <= worldMaxs.x and
                           pos.y >= worldMins.y and pos.y <= worldMaxs.y and
                           pos.z >= worldMins.z and pos.z <= worldMaxs.z then
                            isInSolid = true
                            checkMethod = "inside_prop_bbox"
                            if debug_mode:GetBool() then
                                DebugPrint(string.format(">>> Light IS inside this prop!"))
                            end
                            break
                        end
                    end
                end
            end
            
            if debug_mode:GetBool() then
                DebugPrint(string.format("Prop check: found %d total props, %d nearby", propCount, nearbyProps))
            end
        else
            if debug_mode:GetBool() then
                DebugPrint("Prop check: NikNaks doesn't support GetStaticProps()")
            end
        end
    end
    
    -- Method 4: Check if there's geometry very close in all directions (enclosed space)
    if not isInSolid then
        local checkDist = 8
        local hitCount = 0
        local directions = {
            Vector(1, 0, 0), Vector(-1, 0, 0),
            Vector(0, 1, 0), Vector(0, -1, 0),
            Vector(0, 0, 1), Vector(0, 0, -1),
        }
        
        for _, dir in ipairs(directions) do
            local trace = util.TraceLine({
                start = pos,
                endpos = pos + dir * checkDist,
                mask = TRACE_MASK_SOLID, -- Check all solid geometry
            })
            
            if trace.Hit and trace.Fraction < 1.0 then
                hitCount = hitCount + 1
            end
        end
        
        -- If we hit geometry in most/all directions, we're likely enclosed
        if hitCount >= 5 then
            isInSolid = true
            checkMethod = "enclosed"
            if debug_mode:GetBool() then
                DebugPrint(string.format("Light at (%.1f,%.1f,%.1f) detected as enclosed (%d/6 directions blocked)", 
                    pos.x, pos.y, pos.z, hitCount))
            end
        end
    end
    
    if not isInSolid then
        -- Position is valid, no adjustment needed
        return pos
    end
    
    print(string.format("[Light2RTX] WARNING: %s at (%.1f, %.1f, %.1f) is inside solid geometry! (detected via %s)", 
        classname or "light", pos.x, pos.y, pos.z, checkMethod))
    
    -- Try to find a valid position by tracing in multiple directions
    -- Use shorter distances for spotlights (keep them close to fixture)
    local testDistances = lightDir and { 8, 16, 32, 64, 96 } or { 32, 64, 128, 256 }
    
    -- If we have a light direction (spotlight), try ALL distances in that direction first
    if lightDir and lightDir:Length() > 0 then
        local dir = lightDir:GetNormalized()
        for _, dist in ipairs(testDistances) do
            local testPos = pos + (dir * dist)
            
            -- Check both BSP and props at test position using same logic as detection
            local isValid = true
            
            -- Check BSP if available
            if NikNaks and NikNaks.CurrentMap and NikNaks.CurrentMap.PointContents then
                local testContents = NikNaks.CurrentMap:PointContents(testPos)
                local CONTENTS_SOLID = 1
                if testContents and bit.band(testContents, CONTENTS_SOLID) ~= 0 then
                    isValid = false
                end
            end
            
            -- Check via hull trace
            if isValid then
                local hullSize = 4
                local trace = util.TraceHull({
                    start = testPos,
                    endpos = testPos,
                    mins = Vector(-hullSize, -hullSize, -hullSize),
                    maxs = Vector(hullSize, hullSize, hullSize),
                    mask = TRACE_MASK_SOLID,
                })
                if trace.StartSolid or trace.AllSolid then
                    isValid = false
                end
            end
            
            -- Check if still inside a prop bounding box
            if isValid and NikNaks and NikNaks.CurrentMap then
                local bsp = NikNaks.CurrentMap
                if bsp.GetStaticProps then
                    local props = bsp:GetStaticProps()
                    for _, prop in pairs(props) do
                        local propOrigin = prop.Origin
                        local propModel = prop.PropType
                        if propOrigin and propModel then
                            local propDist = testPos:Distance(propOrigin)
                            if propDist < 150 then
                                local mins, maxs = prop:GetModelBounds()
                                if not mins or not maxs then
                                    mins = Vector(-32, -32, -32)
                                    maxs = Vector(32, 32, 32)
                                end
                                local worldMins = propOrigin + mins
                                local worldMaxs = propOrigin + maxs
                                
                                if testPos.x >= worldMins.x and testPos.x <= worldMaxs.x and
                                   testPos.y >= worldMins.y and testPos.y <= worldMaxs.y and
                                   testPos.z >= worldMins.z and testPos.z <= worldMaxs.z then
                                    isValid = false
                                    break
                                end
                            end
                        end
                    end
                end
            end
            
            -- CRITICAL: Check if path from stuck position to candidate is clear
            -- This prevents pushing lights through walls to the other side
            if isValid then
                local pathTrace = util.TraceLine({
                    start = pos,
                    endpos = testPos,
                    mask = TRACE_MASK_SOLID,
                })
                -- If we hit something before reaching the destination, path is blocked
                if pathTrace.Hit and pathTrace.Fraction < 0.99 then
                    isValid = false
                end
            end
            
            if isValid then
                -- Found valid position in light direction!
                print(string.format("[Light2RTX] Moved %s to valid position (%.1f, %.1f, %.1f) - %d units in light direction", 
                    classname or "light", testPos.x, testPos.y, testPos.z, dist))
                return testPos
            end
        end
    end
    
    -- Fallback: try standard directions if light direction didn't work
    local testDirections = {
        Vector(0, 0, 1),    -- Up
        Vector(0, 0, -1),   -- Down
        Vector(1, 0, 0),    -- Right
        Vector(-1, 0, 0),   -- Left
        Vector(0, 1, 0),    -- Forward
        Vector(0, -1, 0),   -- Back
        Vector(1, 1, 0):GetNormalized(),   -- Diagonal
        Vector(-1, 1, 0):GetNormalized(),
        Vector(1, -1, 0):GetNormalized(),
        Vector(-1, -1, 0):GetNormalized(),
    }
    
    -- Try each direction at various distances
    for _, dist in ipairs(testDistances) do
        for _, dir in ipairs(testDirections) do
            local testPos = pos + (dir * dist)
            
            -- Check both BSP and props at test position using same logic as detection
            local isValid = true
            
            -- Check BSP if available
            if NikNaks and NikNaks.CurrentMap and NikNaks.CurrentMap.PointContents then
                local testContents = NikNaks.CurrentMap:PointContents(testPos)
                local CONTENTS_SOLID = 1
                if testContents and bit.band(testContents, CONTENTS_SOLID) ~= 0 then
                    isValid = false
                end
            end
            
            -- Check via hull trace
            if isValid then
                local hullSize = 4
                local trace = util.TraceHull({
                    start = testPos,
                    endpos = testPos,
                    mins = Vector(-hullSize, -hullSize, -hullSize),
                    maxs = Vector(hullSize, hullSize, hullSize),
                    mask = TRACE_MASK_SOLID, -- Check all solid geometry
                })
                if trace.StartSolid or trace.AllSolid then
                    isValid = false
                end
            end
            
            -- Check if still inside a prop bounding box
            if isValid and NikNaks and NikNaks.CurrentMap then
                local bsp = NikNaks.CurrentMap
                if bsp.GetStaticProps then
                    local props = bsp:GetStaticProps()
                    for _, prop in pairs(props) do
                        local propOrigin = prop.Origin
                        local propModel = prop.PropType
                        if propOrigin and propModel then
                            local dist = testPos:Distance(propOrigin)
                            if dist < 100 then
                                local mins, maxs = prop:GetModelBounds()
                                if not mins or not maxs then
                                    mins = Vector(-32, -32, -32)
                                    maxs = Vector(32, 32, 32)
                                end
                                local worldMins = propOrigin + mins
                                local worldMaxs = propOrigin + maxs
                                
                                -- Check if test position is inside this prop's bbox
                                if testPos.x >= worldMins.x and testPos.x <= worldMaxs.x and
                                   testPos.y >= worldMins.y and testPos.y <= worldMaxs.y and
                                   testPos.z >= worldMins.z and testPos.z <= worldMaxs.z then
                                    isValid = false
                                    break
                                end
                            end
                        end
                    end
                end
            end
            
            -- CRITICAL: Check if path from stuck position to candidate is clear
            -- This prevents pushing lights through walls to the other side
            if isValid then
                local pathTrace = util.TraceLine({
                    start = pos,
                    endpos = testPos,
                    mask = TRACE_MASK_SOLID,
                })
                -- If we hit something before reaching the destination, path is blocked
                if pathTrace.Hit and pathTrace.Fraction < 0.99 then
                    isValid = false
                end
            end
            
            -- Check if enclosed (surrounded by geometry)
            if isValid then
                local checkDist = 8
                local hitCount = 0
                local checkDirs = {
                    Vector(1,0,0), Vector(-1,0,0),
                    Vector(0,1,0), Vector(0,-1,0),
                    Vector(0,0,1), Vector(0,0,-1),
                }
                for _, d in ipairs(checkDirs) do
                    local tr = util.TraceLine({
                        start = testPos,
                        endpos = testPos + d * checkDist,
                        mask = TRACE_MASK_SOLID, -- Check all solid geometry
                    })
                    if tr.Hit and tr.Fraction < 1.0 then
                        hitCount = hitCount + 1
                    end
                end
                if hitCount >= 5 then
                    isValid = false
                end
            end
            
            if isValid then
                -- Found a valid position!
                print(string.format("[Light2RTX] Moved %s to valid position (%.1f, %.1f, %.1f) - %d units %s", 
                    classname or "light", testPos.x, testPos.y, testPos.z, dist, 
                    dir.z > 0.5 and "up" or dir.z < -0.5 and "down" or "away"))
                return testPos
            end
        end
    end
    
    -- Could not find a valid position, return original with warning
    print(string.format("[Light2RTX] ERROR: Could not find valid position for %s! Using original position.", 
        classname or "light"))
    return pos
end

-- Find lights in the BSP data
local function findLightsInBSP()
    if not NikNaks or not NikNaks.CurrentMap then 
        print("[Light2RTX] NikNaks or current map data not available!")
        return {} 
    end
    
    local lights = {}
    local bsp = NikNaks.CurrentMap
    
    -- Build a map of targetname -> position for aiming spotlights at targets
    local nameToPos = {}
    for _, e in pairs(bsp:GetEntities()) do
        local tn = e.targetname or e._targetname
        if tn then
            local p = StringToVector(e.origin)
            nameToPos[tn] = p
        end
    end
    
    -- Check entities in the BSP
    for _, ent in pairs(bsp:GetEntities()) do
        if ent.classname and lightClasses[ent.classname] then
            -- ADD DIAGNOSTIC: Show all keys for light entities
            if debug_mode:GetBool() then
                print("[Light2RTX Debug] ===== All keys for " .. ent.classname .. " =====")
                for k, v in pairs(ent) do
                    print(string.format("  [%s] = %s (type: %s)", tostring(k), tostring(v), type(v)))
                end
                print("[Light2RTX Debug] ===== End keys =====")
            end
            
            -- Get position - convert to Vector if it's a string
            local pos = StringToVector(ent.origin)
            
            local invalidPos = (pos == Vector(0, 0, 0) and ent.origin)
            if invalidPos then
                DebugPrint("Could not parse position from:", ent.origin)
            else
                -- Validate and clamp position to map bounds
                pos = validateAndClampPosition(pos, ent.classname)
                
                local color, brightness, size, lightType, lightProps = getLightProperties(ent)
                
                -- Derive direction for spotlights from target or angles when available
                if ent.classname == "light_spot" or ent.classname == "env_projectedtexture" then
                    local tgt = ent.target or ent._target
                    if tgt and nameToPos[tgt] then
                        local dirVec = (nameToPos[tgt] - pos):GetNormalized()
                        lightProps.direction = dirVec
                        lightProps.shapingEnabled = true
                        lightProps.debugSource = "target"
                elseif not lightProps.direction then
                    -- Fallback: parse angles only if not already set by getLightProperties
                    local a, src = ParseEntityAngles(ent)
                    if a then
                        if ent.classname == "env_projectedtexture" and projtex_invert_pitch:GetBool() then
                            a.p = -(a.p or 0)
                        end
                        local basis = (ent.classname == "env_projectedtexture") and projtex_dir_basis:GetInt() or spot_dir_basis:GetInt()
                        local dir = a:Forward()
                        if basis == 0 then dir = a:Forward()
                        elseif basis == 1 then dir = -a:Forward()
                        elseif basis == 2 then dir = a:Up()
                        elseif basis == 3 then dir = -a:Up()
                        elseif basis == 4 then dir = a:Right()
                        elseif basis == 5 then dir = -a:Right()
                        end
                        lightProps.direction = dir
                        lightProps.shapingEnabled = true
                        lightProps.angles = a
                        lightProps.debugSource = src
                    end
                end
                end
                
                -- Check if position is inside solid geometry and try to fix it
                -- Pass light direction for spotlights so they move in the right direction
                local lightDir = (ent.classname == "light_spot" or ent.classname == "env_projectedtexture") and lightProps.direction or nil
                pos = validatePositionAgainstGeometry(pos, ent.classname, lightDir)
                
                -- Check if light should start disabled (spawnflags bit 1 = "Initially dark")
                local spawnflags = tonumber(ent.spawnflags or ent._spawnflags or 0) or 0
                local initiallyDark = (bit.band(spawnflags, 1) == 1)
                
                -- Read lightstyle/appearance pattern (style field)
                local style = ent.style or ent._style
                
                table.insert(lights, {
                    pos = pos,
                    color = color,
                    brightness = brightness,
                    size = size,
                    classname = ent.classname,
                    lightType = lightType,
                    lightProps = lightProps,
                    angles = lightProps.angles, -- Store angles if available
                    targetname = ent.targetname or ent._targetname,
                    initiallyDark = initiallyDark,
                    style = style
                })
                
                DebugPrint(string.format("Found light: %s (RTX Type: %d) at %.2f,%.2f,%.2f - Color: %d,%d,%d - Brightness: %.1f - Size: %.1f%s", 
                    ent.classname, lightType, pos.x, pos.y, pos.z, color.r, color.g, color.b, brightness, size, initiallyDark and " [Initially Dark]" or ""))
            end
        end
    end
    
    return lights
end

-- Create a unique entity ID for each light
local function getUniqueEntityID()
    last_entity_id = last_entity_id + 1
    -- Combine time-based component with counter and a random element
    local timeComponent = math.floor(CurTime() * 100) * 100000
    local randomComponent = math.random(1000, 9999)
    -- Ensure unique ID across client sessions
    return last_entity_id + timeComponent + randomComponent
end

-- Get the appropriate model for a light type
local function getLightModel(classname)
    return lightModels[classname] or lightModels["default"]
end

-- Create visual prop for a light
local function createVisualProp(pos, color, classname)
    local prop = ents.CreateClientProp(getLightModel(classname))
    if not IsValid(prop) then return nil end
    
    prop:SetPos(pos)
    prop:SetAngles(Angle(0, 0, 0))
    prop:SetColor(color)
    prop:SetRenderMode(RENDERMODE_TRANSALPHA)
    prop:SetModelScale(0.5, 0)
    
    -- Make it slightly transparent
    local c = prop:GetColor()
    prop:SetColor(Color(c.r, c.g, c.b, 200))
    
    -- Mark as a light prop for selection
    prop.RTXLight = true
    
    -- Make the prop pickupable with physgun
    -- Clientside entities aren't normally affected by the physics gun, but we'll
    -- track this flag and handle it ourselves
    prop:SetMoveType(MOVETYPE_VPHYSICS)
    prop:PhysicsInit(SOLID_VPHYSICS)
    
    return prop
end

-- Create a Remix light using the newer RemixLight Lua API (sphere for now)
local function createRemixLight(pos, color, brightness, size, lightType, lightProps, angles, visualProp, classname, targetname, initiallyDark)
    -- Generate a unique position key with some tolerance (0.1 units)
    local posKey = string.format("%.1f_%.1f_%.1f", pos.x, pos.y, pos.z)
    if createdLightPositions[posKey] then
        print("[Light2RTX] Skipping duplicate light at " .. posKey)
        return nil
    end
    createdLightPositions[posKey] = true

    if not istable(RemixLight) then
        print("[Light2RTX] RemixLight API not available")
        createdLightPositions[posKey] = nil
        return nil
    end

    local entityId = getUniqueEntityID()

    -- Base light definition: compute radiance from color and brightness (0-255)
    local appliedBrightness = tonumber(brightness) or 255
    
    -- Per-type brightness multiplier
    local kind = (classname == "light_environment") and "env"
        or ((classname == "light_spot" or classname == "env_projectedtexture") and "spot" or "point")
    local typeBrightnessMult = (kind == "env") and env_brightness_mult:GetFloat()
        or ((kind == "spot") and spot_brightness_mult:GetFloat() or point_brightness_mult:GetFloat())
    
    -- Compute intensity using Source engine's formula
    -- Source: intensity = (color_linear) * (brightness / 255.0) * lightscale
    -- where color_linear = pow(color/255, 2.2) * 255 (but srgbToLinear already does this)
    -- brightness is the 4th value in _light field (0-255 range for LDR, >255 for bright lights)
    local baseScale = 1.0
    local brightBoost = 1.0
    
    if kind == "env" then
        -- For light_environment, apply a baseline scale then multiply by mapper's HDR Brightness Scale
        -- Use square root scaling to compress brightness range (prevents extreme lights from dominating)
        local hdrScale = tonumber(lightProps._lightscaleHDR or 1.0)
        
        -- Normalize brightness to a baseline, then apply sqrt to compress range
        -- Baseline of 200: sqrt(200/200) = 1.0x, sqrt(560/200) = 1.67x, sqrt(1000/200) = 2.24x
        -- Clamp minimum to 0.9 to prevent very dim lights (50) from being over-bright
        local brightnessNormalized = math.max(0.9, math.sqrt(appliedBrightness / 200.0))
        local envBaseline = 0.2  -- Base multiplier (tuned for brightness ~200)
        
        baseScale = envBaseline * brightnessNormalized * hdrScale
        -- No brightBoost for environment lights - HDR scale handles intensity variation
    else
        -- Point/spot lights need ~100x boost to compensate for missing radiosity calculations
        baseScale = 100.0
        
        -- Apply extra boost for high brightness values (>255) to compensate for lack of HDR tone mapping
        if appliedBrightness > 255 then
            -- Scale: 256-2000 -> 2x-20x boost, >2000 -> clamp at 20x
            brightBoost = math.min(20.0, 1.0 + (appliedBrightness - 255) / 92.0)
        end
    end
    
    local intensity = (appliedBrightness / 255.0) * baseScale * typeBrightnessMult * brightBoost
    
    -- Force intensity to 0 if light should start disabled
    if initiallyDark then
        intensity = 0.0
    end
    
    local base = {
        hash = tonumber(util.CRC(string.format("maplight_%s", posKey))) or entityId,
        radiance = { 
            x = srgbToLinear(color.r) * intensity, 
            y = srgbToLinear(color.g) * intensity, 
            z = srgbToLinear(color.b) * intensity 
        },
        isDynamic = true,
    }

    -- Direction from angles if present
    -- Default to pointing down; override with explicit direction when known
    local dir = Vector(0, 0, -1)
    -- If a spotlight direction was derived, prefer it
    if lightProps and lightProps.direction then
        local d = lightProps.direction
        dir = Vector(d.x, d.y, d.z)
    elseif angles then
        local baseAngles = angles
        baseAngles.x = baseAngles.x + rect_rotation_x:GetFloat()
        baseAngles.y = baseAngles.y + rect_rotation_y:GetFloat()
        baseAngles.z = baseAngles.z + rect_rotation_z:GetFloat()
        local basis = spot_dir_basis:GetInt()
        if basis == 0 then dir = baseAngles:Forward()
        elseif basis == 1 then dir = -baseAngles:Forward()
        elseif basis == 2 then dir = baseAngles:Up()
        elseif basis == 3 then dir = -baseAngles:Up()
        elseif basis == 4 then dir = baseAngles:Right()
        elseif basis == 5 then dir = -baseAngles:Right()
        else dir = baseAngles:Forward() end
    end

    -- Special-case directional: use Distant lights
    local lightId = nil
    if classname == "light_environment" then
        local baseAngular = (lightProps and tonumber(lightProps.angularDiameter)) or 0.53
        local distant = {
            direction = { x = dir.x, y = dir.y, z = dir.z },
            angularDiameterDegrees = baseAngular * (env_angular_mult:GetFloat() or 1.0) * (env_size_mult:GetFloat() or 1.0),
            volumetricRadianceScale = env_volumetric_mult:GetFloat() or 1.0,
        }
        if RemixLight.CreateDistant then
            lightId = RemixLight.CreateDistant(base, distant, entityId)
        end
    else
        -- Sphere info as a reasonable default representation
        local baseRadius = tonumber(size) or 5
        -- If size is 0 or very small, use default
        if baseRadius < 0.1 then baseRadius = 5 end
        local rmult = (kind == "spot") and (spot_radius_mult:GetFloat() or 1.0) or (point_radius_mult:GetFloat() or 1.0)
        local smult = (kind == "spot") and (spot_size_mult:GetFloat() or 1.0) or (point_size_mult:GetFloat() or 1.0)
        local vmult = (kind == "spot") and (spot_volumetric_mult:GetFloat() or 1.0) or (point_volumetric_mult:GetFloat() or 1.0)
        
        -- Apply size multiplier to radius, then clamp like updateEntryRuntime does
        local radiusWithSizeMult = baseRadius * smult
        radiusWithSizeMult = math.Clamp(radiusWithSizeMult, min_size:GetFloat(), max_size:GetFloat())
        
        local sphere = {
            position = { x = pos.x, y = pos.y, z = pos.z },
            radius = radiusWithSizeMult * rmult,
            volumetricRadianceScale = vmult,
        }
        if lightProps and lightProps.shapingEnabled then
            sphere.shaping = {
                direction = { x = dir.x, y = dir.y, z = dir.z },
                coneAngleDegrees = tonumber(lightProps.coneAngle) or 45.0,
                coneSoftness = tonumber(lightProps.coneSoftness) or 0.2,
                focusExponent = 1.0,
            }
            DebugPrint(string.format("Create spot dir=(%.2f, %.2f, %.2f) src=%s", dir.x, dir.y, dir.z, tostring(lightProps.debugSource)))
        end

        -- Create the light (synchronous) and get its id
        if RemixLight.CreateSphere then
            lightId = RemixLight.CreateSphere(base, sphere, entityId)
        end
    end

    if not lightId or lightId == 0 then
        print("[Light2RTX] Failed to create Remix light")
        createdLightPositions[posKey] = nil
        return nil
    end

    -- Link optional visual prop
    if IsValid(visualProp) then
        visualProp.RTXLight = true
    end

    local entry = {
        id = lightId,
        entityId = entityId,
        type = (classname == "light_environment") and "distant" or "sphere",
        pos = pos,
        color = color,
        size = size,
        shapingEnabled = lightProps and lightProps.shapingEnabled or false,
        classname = classname,
        visualProp = visualProp,
        kind = kind,
        baseBrightness = appliedBrightness,
        baseAngular = (classname == "light_environment") and ((lightProps and tonumber(lightProps.angularDiameter)) or 0.53) or nil,
        baseRadius = (classname ~= "light_environment") and (tonumber(size) or 200) or nil,
        baseSizeBeforeMultipliers = (classname ~= "light_environment") and (lightProps and tonumber(lightProps.baseSizeBeforeMultipliers)) or nil,
        -- Store HDR scale for environment lights (used in runtime updates)
        hdrScale = (classname == "light_environment") and (lightProps and tonumber(lightProps._lightscaleHDR)) or nil,
        -- Debug/inspection fields
        angles = angles,
        direction = dir,
        -- Preserve spot shaping parameters for safe updates
        coneAngleDegrees = (lightProps and tonumber(lightProps.coneAngle)) or nil,
        coneSoftness = (lightProps and tonumber(lightProps.coneSoftness)) or nil,
        -- Animation / linkage
        targetname = targetname,
        animMul = (initiallyDark and 0.0) or 1.0,  -- Respect "Initially dark" spawnflag
        animEnabled = not initiallyDark,  -- Start disabled if initially dark
    }
    return entry
end

local function addPositionOffset(pos)
    if not pos_jitter:GetBool() then return pos end
    
    local jitterAmount = pos_jitter_amount:GetFloat() or 0.1
    local offset = Vector(
        math.Rand(-jitterAmount, jitterAmount),
        math.Rand(-jitterAmount, jitterAmount),
        math.Rand(-jitterAmount, jitterAmount)
    )
    return pos + offset
end

-- Create RTX lights for all the lights we found
local function batchCreateRTXLights()
    -- Reset entity ID counter and position tracking
    last_entity_id = 0
    table.Empty(createdLightPositions)
    
    -- Get lights from BSP
    local bspLights = findLightsInBSP()
    print("[Light2RTX] Found " .. #bspLights .. " lights in BSP data")
    
    -- Skip duplicate positions in advance by using a position map
    local uniqueLightsByPosition = {}
    for _, light in ipairs(bspLights) do
        -- Add small jitter to prevent exact overlaps
        light.pos = addPositionOffset(light.pos)
        
        -- Create a unique position key
        local posKey = string.format("%.1f_%.1f_%.1f", light.pos.x, light.pos.y, light.pos.z)
        
        -- Only keep one light per position
        uniqueLightsByPosition[posKey] = light
    end
    
    -- Convert back to an array
    local uniqueLights = {}
    for _, light in pairs(uniqueLightsByPosition) do
        table.insert(uniqueLights, light)
    end
    
    print("[Light2RTX] After deduplication: " .. #uniqueLights .. " unique light positions")
    
    -- Batch processing variables
    local BATCH_SIZE = creation_batch_size:GetInt()
    local BATCH_DELAY = creation_batch_delay:GetFloat()
    local CREATION_INTERVAL = creation_delay:GetFloat()
    
    -- Split into batches
    local batches = {}
    for i = 1, #uniqueLights, BATCH_SIZE do
        local endIndex = math.min(i + BATCH_SIZE - 1, #uniqueLights)
        local batch = {}
        for j = i, endIndex do
            table.insert(batch, uniqueLights[j])
        end
        table.insert(batches, batch)
    end
    
    print("[Light2RTX] Split into " .. #batches .. " batches of up to " .. BATCH_SIZE .. " lights each")
    
    -- Process batches one at a time
    local function processBatch(batchIndex)
        if batchIndex > #batches then
            print("[Light2RTX] All batches complete. " .. #createdLights .. " lights created.")
            return
        end
        
        local batch = batches[batchIndex]
        local lightsCreated = 0
        
        -- Process lights in this batch with intervals
        for i, light in ipairs(batch) do
            timer.Simple((i-1) * CREATION_INTERVAL, function()
                -- Create visual prop if enabled
                local visualProp = nil
                if visual_mode:GetBool() then
                    visualProp = createVisualProp(light.pos, light.color, light.classname)
                    if light.angles and IsValid(visualProp) then
                        visualProp:SetAngles(light.angles)
                    end
                end
                
                -- Create the Remix light using new API
                local entry = createRemixLight(
                    light.pos,
                    light.color,
                    light.brightness,
                    light.size,
                    light.lightType or 0,
                    light.lightProps,
                    light.angles,
                    visualProp,
                    light.classname,
                    light.targetname,
                    light.initiallyDark  -- Respect "Initially dark" spawnflag from BSP
                )
                
                if entry and entry.id then
                    table.insert(createdLights, entry)
                    local idx = #createdLights
                    idToIndex[entry.id] = idx
                    if entry.kind and lightsByKind[entry.kind] then
                        lightsByKind[entry.kind][entry.id] = true
                    end
                    
                    -- Assign targetname for pattern support
                    local targetname = entry.targetname
                    if (not targetname or targetname == "") and light.style then
                        -- Create synthetic targetname for unnamed lights with patterns
                        targetname = string.format("_rtx_light_%d", entry.id)
                        entry.targetname = targetname
                    end
                    
                    if targetname and targetname ~= "" then
                        local lname = string.lower(targetname)
                        lightsByName[lname] = lightsByName[lname] or {}
                        lightsByName[lname][entry.id] = true
                    end
                    lightsCreated = lightsCreated + 1
                    
                    -- Start lightstyle pattern animation if specified
                    if light.style and targetname and targetname ~= "" then
                        Light2RTX.StartPattern(targetname, light.style)
                        DebugPrint(string.format("Started pattern %s for light '%s' (id=%d)", tostring(light.style), targetname, entry.id))
                    end
                    
                    -- Last light in batch
                    if i == #batch then
                        -- Process next batch after delay
                        timer.Simple(BATCH_DELAY, function()
                            processBatch(batchIndex + 1)
                        end)
                    end
                else
                    -- Last light in batch but creation failed
                    if i == #batch then
                        -- Process next batch after delay
                        timer.Simple(BATCH_DELAY, function()
                            processBatch(batchIndex + 1)
                        end)
                    end
                end
            end)
        end
    end
    
    -- Start with the first batch
    processBatch(1)
    
    print("[Light2RTX] Started batch processing for " .. #uniqueLights .. " lights")
end

-- Remove all created lights
local function clearRTXLights()
    for _, entry in ipairs(createdLights) do
        if entry.visualProp and IsValid(entry.visualProp) then
            entry.visualProp:Remove()
        end
        if istable(RemixLight) and RemixLight.DestroyLight and entry.id then
            RemixLight.DestroyLight(entry.id)
        end
    end
    createdLights = {}
    -- reset registries
    lightsByKind = { point = {}, spot = {}, env = {} }
    idToIndex = {}
    lightsByName = {}
    print("[Light2RTX] Cleared all RTX lights")
end

-- Recompute and push updates for a single light entry based on current CVars
local function updateEntryRuntime(entry)
    if not entry or not entry.id then return end
    -- Determine kind reliably
    local kind = entry.kind or ((entry.classname == "light_environment") and "env" or ((entry.classname == "light_spot" or entry.classname == "env_projectedtexture") and "spot" or "point"))
    -- Brightness scale from stored baseBrightness (0-255) and current per-kind multiplier
    local baseBright = tonumber(entry.baseBrightness) or 255
    local bmult = 1.0
    if kind == "env" then
        bmult = env_brightness_mult:GetFloat()
    elseif kind == "spot" then
        bmult = spot_brightness_mult:GetFloat()
    else
        bmult = point_brightness_mult:GetFloat()
    end
    local amult = tonumber(entry.animMul or 1.0) or 1.0
    
    -- Compute intensity using Source engine's formula
    -- Source: intensity = (color_linear) * (brightness / 255.0) * lightscale
    -- where color_linear = pow(color/255, 2.2) * 255 (but srgbToLinear already does this)
    -- brightness is the 4th value in _light field (0-255 range for LDR, >255 for bright lights)
    local baseScale = 1.0
    local brightBoost = 1.0
    
    if kind == "env" then
        -- For light_environment, apply same formula as creation: baseline * sqrt(brightness) * hdrScale
        local hdrScale = tonumber(entry.hdrScale or 1.0)
        local brightnessNormalized = math.max(0.9, math.sqrt(baseBright / 200.0))
        local envBaseline = 0.2
        baseScale = envBaseline * brightnessNormalized * hdrScale
        -- No brightBoost for environment lights
    else
        -- Point/spot lights need ~100x boost to compensate for missing radiosity calculations
        baseScale = 100.0
        
        -- Apply extra boost for high brightness values (>255) to compensate for lack of HDR tone mapping
        if baseBright > 255 then
            -- Scale: 256-2000 -> 2x-20x boost, >2000 -> clamp at 20x
            brightBoost = math.min(20.0, 1.0 + (baseBright - 255) / 92.0)
        end
    end
    
    local intensity = (baseBright / 255.0) * baseScale * bmult * amult * brightBoost
    
    -- Force radiance to 0 if light is disabled (for compatibility with HDRI editor)
    if not entry.animEnabled or amult <= 0.0 then
        intensity = 0.0
    end
    
    local base = {
        hash = tonumber(util.CRC("upd_" .. tostring(entry.id))) or entry.entityId,
        radiance = { 
            x = srgbToLinear(entry.color.r) * intensity, 
            y = srgbToLinear(entry.color.g) * intensity, 
            z = srgbToLinear(entry.color.b) * intensity 
        },
        isDynamic = true,
    }
    -- Helper to compute direction for distant/spot from stored angles if available
    local function computeDir()
        local dir = entry.direction or Vector(0, 0, -1)
        if entry.angles then
            if entry.classname == "light_environment" then
                dir = entry.angles:Forward()
                if env_dir_flip:GetBool() then dir = -dir end
            elseif entry.shapingEnabled then
                -- For spotlights keep stored basis simple to avoid jitter; prefer stored direction
                dir = entry.direction or entry.angles:Forward()
            end
        end
        return dir
    end
    if entry.type == "distant" or entry.classname == "light_environment" then
        local baseAngular = tonumber(entry.baseAngular) or 0.53
        local distant = {
            direction = (function() local d = computeDir(); return { x = d.x, y = d.y, z = d.z } end)(),
            angularDiameterDegrees = baseAngular * (env_angular_mult:GetFloat() or 1.0) * (env_size_mult:GetFloat() or 1.0),
            volumetricRadianceScale = env_volumetric_mult:GetFloat() or 1.0,
        }
        if istable(RemixLight) and RemixLight.UpdateDistant then
            RemixLight.UpdateDistant(base, distant, entry.id)
        end
    else
        local rmult = (kind == "spot") and (spot_radius_mult:GetFloat() or 1.0) or (point_radius_mult:GetFloat() or 1.0)
        local smult = (kind == "spot") and (spot_size_mult:GetFloat() or 1.0) or (point_size_mult:GetFloat() or 1.0)
        local vmult = (kind == "spot") and (spot_volumetric_mult:GetFloat() or 1.0) or (point_volumetric_mult:GetFloat() or 1.0)
        
        -- Use baseSizeBeforeMultipliers if available to apply type size multiplier at runtime
        local baseSize = tonumber(entry.baseSizeBeforeMultipliers) or tonumber(entry.baseRadius) or tonumber(entry.size) or 5
        -- If size is 0 or very small, use default (matches creation logic)
        if baseSize < 0.1 then baseSize = 5 end
        local radiusWithSizeMult = baseSize * smult
        
        -- Debug: Print values for lights that seem stuck
        if debug_mode:GetBool() and radiusWithSizeMult < 2 then
            DebugPrint(string.format("Light %s: baseSize=%.2f, smult=%.2f, result=%.2f, hasBaseBeforeMult=%s", 
                entry.classname or "?", baseSize, smult, radiusWithSizeMult, tostring(entry.baseSizeBeforeMultipliers ~= nil)))
        end
        
        -- Apply min/max clamping just like during creation
        radiusWithSizeMult = math.Clamp(radiusWithSizeMult, min_size:GetFloat(), max_size:GetFloat())
        
        local sphere = {
            position = { x = entry.pos.x, y = entry.pos.y, z = entry.pos.z },
            radius = radiusWithSizeMult * rmult,
            volumetricRadianceScale = vmult,
        }
        if entry.shapingEnabled then
            local d = computeDir()
            sphere.shaping = {
                direction = { x = d.x, y = d.y, z = d.z },
                coneAngleDegrees = entry.coneAngleDegrees or 45.0,
                coneSoftness = entry.coneSoftness or 0.2,
                focusExponent = 1.0,
            }
        end
        if istable(RemixLight) and RemixLight.UpdateSphere then
            RemixLight.UpdateSphere(base, sphere, entry.id)
        end
    end
end

-- Update all lights of a given kind: "point", "spot", or "env"
local function updateAllOfKind(kind)
    local map = lightsByKind[kind]
    if not map then return end
    for lightId, _ in pairs(map) do
        local idx = idToIndex[lightId]
        local entry = idx and createdLights[idx] or nil
        if entry then updateEntryRuntime(entry) end
    end
    DebugPrint("Updated all lights of kind:", tostring(kind))
end

local function refreshAllLights()
    updateAllOfKind("point")
    updateAllOfKind("spot")
    updateAllOfKind("env")
end

-- CVar change callbacks to trigger runtime updates
if cvars and cvars.AddChangeCallback then
    cvars.AddChangeCallback("rtx_api_map_lights_point_brightness_mult", function() updateAllOfKind("point") end, "rtx_maplights_point_bmult")
    cvars.AddChangeCallback("rtx_api_map_lights_spot_brightness_mult", function() updateAllOfKind("spot") end, "rtx_maplights_spot_bmult")
    cvars.AddChangeCallback("rtx_api_map_lights_env_brightness_mult", function() updateAllOfKind("env") end, "rtx_maplights_env_bmult")
    cvars.AddChangeCallback("rtx_api_map_lights_point_radius_mult", function() updateAllOfKind("point") end, "rtx_maplights_point_rmult")
    cvars.AddChangeCallback("rtx_api_map_lights_spot_radius_mult", function() updateAllOfKind("spot") end, "rtx_maplights_spot_rmult")
    cvars.AddChangeCallback("rtx_api_map_lights_env_angular_mult", function() updateAllOfKind("env") end, "rtx_maplights_env_amult")
    cvars.AddChangeCallback("rtx_api_map_lights_point_volumetric_mult", function() updateAllOfKind("point") end, "rtx_maplights_point_vmult")
    cvars.AddChangeCallback("rtx_api_map_lights_spot_volumetric_mult", function() updateAllOfKind("spot") end, "rtx_maplights_spot_vmult")
    cvars.AddChangeCallback("rtx_api_map_lights_env_volumetric_mult", function() updateAllOfKind("env") end, "rtx_maplights_env_vmult")
    cvars.AddChangeCallback("rtx_api_map_lights_point_size_mult", function() updateAllOfKind("point") end, "rtx_maplights_point_smult")
    cvars.AddChangeCallback("rtx_api_map_lights_spot_size_mult", function() updateAllOfKind("spot") end, "rtx_maplights_spot_smult")
    cvars.AddChangeCallback("rtx_api_map_lights_env_size_mult", function() updateAllOfKind("env") end, "rtx_maplights_env_smult")
    cvars.AddChangeCallback("rtx_api_map_lights_min_size", function() refreshAllLights() end, "rtx_maplights_min_size")
    cvars.AddChangeCallback("rtx_api_map_lights_max_size", function() refreshAllLights() end, "rtx_maplights_max_size")
    -- Flip callback: re-evaluate env directions from stored angles when toggled
    cvars.AddChangeCallback("rtx_api_map_lights_env_dir_flip", function() updateAllOfKind("env") end, "rtx_maplights_env_flip")
end

-- Toggle light visibility
local function toggleVisualMode()
    local newValue = not visual_mode:GetBool()
    RunConsoleCommand("rtx_api_map_lights_visual", newValue and "1" or "0")
    
    -- If turning off, remove all visual props
    if not newValue then
        for _, entry in ipairs(createdLights) do
            if entry.visualProp and IsValid(entry.visualProp) then
                entry.visualProp:Remove()
                entry.visualProp = nil
            end
        end
    else
        -- If turning on, create visual props for existing lights
        for _, entry in ipairs(createdLights) do
            if (not entry.visualProp) or (entry.visualProp and not IsValid(entry.visualProp)) then
                local visualProp = createVisualProp(entry.pos, entry.color, "default")
                if IsValid(visualProp) then
                    entry.visualProp = visualProp
                end
            end
        end
    end
    
    print("[Light2RTX] Visual mode " .. (newValue and "enabled" or "disabled"))
end

-- PhysGun functionality
local heldProps = {}

-- Enable drawing of beams
hook.Add("PostDrawOpaqueRenderables", "rtx_api_map_lights_DrawPhysBeams", function()
    local wep = LocalPlayer():GetActiveWeapon()
    if not IsValid(wep) or wep:GetClass() ~= "weapon_physgun" then 
        return 
    end
    
    -- Draw beams for all held props
    for prop, _ in pairs(heldProps) do
        if IsValid(prop) then
            local attachment = wep:GetAttachment(1) -- Get the physgun beam attachment
            if attachment then
                -- Draw a beam from the physgun to the held entity
                local startPos = attachment.Pos
                local endPos = prop:GetPos()
                
                render.SetMaterial(Material("cable/physbeam"))
                render.DrawBeam(startPos, endPos, 1, 0, 10, Color(0, 255, 255, 200))
            end
        end
    end
end)

-- Hook for picking up clientside props with the physgun
hook.Add("PhysgunPickup", "rtx_api_map_lights_PhysgunPickup", function(ply, ent)
    if ent.RTXLight then
        heldProps[ent] = true
        DebugPrint("Light prop picked up with physgun")
        return true
    end
end)

-- Hook for dropping clientside props with the physgun
hook.Add("PhysgunDrop", "rtx_api_map_lights_PhysgunDrop", function(ply, ent)
    if ent.RTXLight then
        heldProps[ent] = nil
        DebugPrint("Light prop dropped with physgun")
        return true
    end
end)

-- Handle moving the held props with the physgun
hook.Add("Think", "rtx_api_map_lights_PhysgunThink", function()
    local ply = LocalPlayer()
    local wep = ply:GetActiveWeapon()
    
    if not IsValid(wep) or wep:GetClass() ~= "weapon_physgun" then
        -- If player isn't holding a physgun, clear held props
        heldProps = {}
        return
    end
    
    if not input.IsMouseDown(MOUSE_LEFT) then
        -- If mouse isn't held down, clear held props
        heldProps = {}
        return
    end
    
    -- Handle moving the held props
    for prop, _ in pairs(heldProps) do
        if IsValid(prop) then
            -- Get aim trace
            local eyeTrace = ply:GetEyeTrace()
            
            -- Get physgun attachment position
            local attachment = wep:GetAttachment(1)
            local startPos = attachment and attachment.Pos or ply:EyePos()
            
            -- Calculate new position
            local newPos = eyeTrace.HitPos
            
            -- Move the prop
            prop:SetPos(newPos)
            
            -- Update the connected Remix light
            if prop.RTXLight then
                -- Find the entry that owns this prop
                for _, entry in ipairs(createdLights) do
                    if entry.visualProp == prop and entry.type == "sphere" and entry.id then
                        entry.pos = newPos
                        if istable(RemixLight) and RemixLight.UpdateSphereFields then
                            RemixLight.UpdateSphereFields(entry.id, { position = { x = newPos.x, y = newPos.y, z = newPos.z } })
                        elseif istable(RemixLight) and RemixLight.UpdateSphere then
                            -- Fallback: build minimal base+info from cached entry, only include shaping if enabled
                            local base = { hash = tonumber(util.CRC("upd_" .. tostring(entry.id))) or entry.entityId, radiance = { x = entry.color.r, y = entry.color.g, z = entry.color.b }, isDynamic = true }
                            local info = { position = { x = newPos.x, y = newPos.y, z = newPos.z }, radius = entry.size or 200, volumetricRadianceScale = 1.0 }
                            if entry.shapingEnabled then
                                info.shaping = { direction = { x = 0, y = 0, z = -1 }, coneAngleDegrees = 45.0, coneSoftness = 0.2, focusExponent = 1.0 }
                            end
                            RemixLight.UpdateSphere(base, info, entry.id)
                        end
                        break
                    end
                end
            end
        end
    end
end)

local function resetLightTracking()
    -- Clear all created lights
    clearRTXLights()
    
    -- Reset tracking variables
    table.Empty(createdLightPositions)
    last_entity_id = 0
    
    -- Reset Lua-side entity tracking
    for k in pairs(lightsBeingProcessed or {}) do
        lightsBeingProcessed[k] = nil
    end
    
    print("[Light2RTX] Light tracking data has been reset")
end

-- Add console commands
concommand.Add("rtx_api_map_lights_process", function()
    batchCreateRTXLights()
end)

concommand.Add("rtx_api_map_lights_clear", function()
    clearRTXLights()
end)

concommand.Add("rtx_api_map_lights_toggle_visual", function()
    toggleVisualMode()
end)

concommand.Add("rtx_api_map_lights_reset", function()
    resetLightTracking()
end)

concommand.Add("rtx_api_map_lights_refresh", function()
    refreshAllLights()
    print("[Light2RTX] Refreshed all lights with current multipliers")
end)

-- Add context menu for lights
properties.Add("rtx_light_edit", {
    MenuLabel = "Edit RTX Light",
    Order = 500,
    MenuIcon = "icon16/lightbulb.png",
    
    Filter = function(self, ent, ply)
        if not IsValid(ent) then return false end
        return ent.RTXLight and IsValid(ent.lightEntity)
    end,
    
    Action = function(self, ent)
        if IsValid(ent.lightEntity) then
            ent.lightEntity:OpenPropertyMenu()
        end
    end
})

-- Add highlight for light entities when using physgun
hook.Add("PreDrawHalos", "rtx_api_map_lights_Highlight", function()
    if not visual_mode:GetBool() then return end
    
    local ply = LocalPlayer()
    local wep = ply:GetActiveWeapon()
    
    if IsValid(wep) and wep:GetClass() == "weapon_physgun" then
        -- Find all RTX light props
        local lightProps = {}
        for _, entry in ipairs(createdLights) do
            if entry.visualProp and IsValid(entry.visualProp) then
                table.insert(lightProps, entry.visualProp)
            end
        end
        
        -- Add a halo around light props
        halo.Add(lightProps, Color(0, 255, 255), 2, 2, 1, true, true)
        
        -- Add a more prominent halo around held props
        local heldLightProps = {}
        for prop, _ in pairs(heldProps) do
            if IsValid(prop) then
                table.insert(heldLightProps, prop)
            end
        end
        
        if #heldLightProps > 0 then
            halo.Add(heldLightProps, Color(255, 255, 0), 4, 4, 2, true, true)
        end
    end
end)

-- Optional debug direction lines for spotlights and directional lights
hook.Add("PostDrawTranslucentRenderables", "rtx_api_map_lights_DebugDir", function(depth, sky)
    if not debug_vis:GetBool() then return end
    -- Use a textured beam material (fixed-function safe)
    render.SetMaterial(debug_beam_mat)
    for _, entry in ipairs(createdLights) do
        if entry.pos then
            local isSpot = (entry.classname == "light_spot" or entry.classname == "env_projectedtexture") and entry.shapingEnabled
            local isDistant = entry.type == "distant" or entry.classname == "light_environment"
            if isSpot or isDistant then
                local startPos = entry.pos
                local dir = Vector(0, 0, -1)
                -- Prefer stored direction when present
                if entry.direction and isvector(entry.direction) then
                    dir = entry.direction
                elseif entry.angles then
                    dir = entry.angles:Forward()
                end
                local length = isDistant and 1024 or 128
                local endPos = startPos + dir * length
                local col = isDistant and Color(0, 200, 255, 220) or Color(255, 0, 0, 220)
                local beamWidth = 1
                if isDistant then beamWidth = 2 end
                render.DrawBeam(startPos, endPos, beamWidth, 0, 1, col)
            end
        end
    end
end)

-- HUD overlay showing light positions and info
hook.Add("HUDPaint", "rtx_api_map_lights_DebugHUD", function()
    if not debug_hud:GetBool() then return end
    if #createdLights == 0 then return end
    
    local ply = LocalPlayer()
    if not IsValid(ply) then return end
    
    local plyPos = ply:EyePos()
    local maxDist = debug_hud_max_distance:GetFloat()
    local showDisabled = debug_hud_show_disabled:GetBool()
    
    -- Font setup
    surface.SetFont("DermaDefault")
    
    local visibleLights = 0
    
    for _, entry in ipairs(createdLights) do
        if not entry.pos then continue end
        
        -- Check if light is enabled
        local isEnabled = (entry.animMul or 1.0) > 0.01
        if not isEnabled and not showDisabled then continue end
        
        -- Distance check
        local dist = plyPos:Distance(entry.pos)
        if dist > maxDist then continue end
        
        -- Convert world position to screen position
        local scrPos = entry.pos:ToScreen()
        if not scrPos.visible then continue end
        
        visibleLights = visibleLights + 1
        
        -- Determine light type and color
        local lightType = "Point"
        local typeColor = Color(255, 255, 0, 255)
        
        if entry.classname == "light_environment" then
            lightType = "Sun"
            typeColor = Color(255, 200, 100, 255)
        elseif entry.classname == "light_spot" then
            lightType = "Spot"
            typeColor = Color(255, 100, 100, 255)
        elseif entry.classname == "env_projectedtexture" then
            lightType = "Proj"
            typeColor = Color(255, 0, 255, 255)
        end
        
        -- Fade based on distance
        local alpha = math.Clamp(1 - (dist / maxDist), 0.3, 1)
        typeColor.a = alpha * 255
        
        -- Draw background box
        local text = string.format("%s", lightType)
        local textW, textH = surface.GetTextSize(text)
        local boxPadding = 4
        local boxW = textW + boxPadding * 2
        local boxH = textH + boxPadding * 2
        
        -- Background with light color tint
        local bgColor = Color(0, 0, 0, alpha * 180)
        surface.SetDrawColor(bgColor)
        surface.DrawRect(scrPos.x - boxW / 2, scrPos.y - boxH / 2, boxW, boxH)
        
        -- Border with light color
        local borderColor = Color(entry.color.r, entry.color.g, entry.color.b, alpha * 255)
        surface.SetDrawColor(borderColor)
        surface.DrawOutlinedRect(scrPos.x - boxW / 2, scrPos.y - boxH / 2, boxW, boxH)
        
        -- Draw light type text
        draw.SimpleText(text, "DermaDefault", scrPos.x, scrPos.y, typeColor, TEXT_ALIGN_CENTER, TEXT_ALIGN_CENTER)
        
        -- Draw additional info below if close enough
        if dist < maxDist * 0.5 then
            local infoY = scrPos.y + boxH / 2 + 2
            
            -- Distance
            local distText = string.format("%dm", math.floor(dist / 39.37)) -- Convert to meters (Source units to meters)
            local distColor = Color(200, 200, 200, alpha * 200)
            draw.SimpleText(distText, "DermaDefault", scrPos.x, infoY, distColor, TEXT_ALIGN_CENTER, TEXT_ALIGN_TOP)
            
            -- Targetname if available
            if entry.targetname and entry.targetname ~= "" then
                local nameText = string.format("%s", entry.targetname)
                local nameColor = Color(100, 200, 255, alpha * 200)
                draw.SimpleText(nameText, "DermaDefault", scrPos.x, infoY + textH, nameColor, TEXT_ALIGN_CENTER, TEXT_ALIGN_TOP)
            end
            
            -- Show disabled status
            if not isEnabled then
                local disabledText = "[OFF]"
                local disabledColor = Color(255, 100, 100, alpha * 200)
                draw.SimpleText(disabledText, "DermaDefault", scrPos.x, scrPos.y - boxH / 2 - textH, disabledColor, TEXT_ALIGN_CENTER, TEXT_ALIGN_BOTTOM)
            end
        end
        
        -- Draw line from player to light (if very close)
        if dist < maxDist * 0.25 then
            local centerX, centerY = scrPos.x, scrPos.y
            local plyScreenPos = plyPos:ToScreen()
            if plyScreenPos.visible then
                surface.SetDrawColor(borderColor.r, borderColor.g, borderColor.b, alpha * 100)
                surface.DrawLine(plyScreenPos.x, plyScreenPos.y, centerX, centerY)
            end
        end
    end
    
    -- Draw legend in top-right corner
    local legendX = ScrW() - 150
    local legendY = 100
    local legendBg = Color(0, 0, 0, 200)
    
    surface.SetDrawColor(legendBg)
    surface.DrawRect(legendX - 5, legendY - 5, 145, 110)
    
    draw.SimpleText("Map Lights HUD", "DermaDefaultBold", legendX, legendY, Color(255, 255, 255), TEXT_ALIGN_LEFT, TEXT_ALIGN_TOP)
    draw.SimpleText(string.format("Visible: %d / %d", visibleLights, #createdLights), "DermaDefault", legendX, legendY + 15, Color(200, 200, 200), TEXT_ALIGN_LEFT, TEXT_ALIGN_TOP)
    draw.SimpleText(string.format("Max Dist: %dm", math.floor(maxDist / 39.37)), "DermaDefault", legendX, legendY + 30, Color(200, 200, 200), TEXT_ALIGN_LEFT, TEXT_ALIGN_TOP)
    
    -- Legend items
    draw.SimpleText("Point", "DermaDefault", legendX, legendY + 50, Color(255, 255, 0), TEXT_ALIGN_LEFT, TEXT_ALIGN_TOP)
    draw.SimpleText("Spot", "DermaDefault", legendX, legendY + 65, Color(255, 100, 100), TEXT_ALIGN_LEFT, TEXT_ALIGN_TOP)
    draw.SimpleText("Sun", "DermaDefault", legendX, legendY + 80, Color(255, 200, 100), TEXT_ALIGN_LEFT, TEXT_ALIGN_TOP)
    draw.SimpleText("Proj", "DermaDefault", legendX, legendY + 95, Color(255, 0, 255), TEXT_ALIGN_LEFT, TEXT_ALIGN_TOP)
end)

hook.Add("Initialize", "rtx_api_map_lights_Reset", function()
    resetLightTracking()
end)

hook.Add("InitPostEntity", "rtx_api_map_lights_Reset", function()
    resetLightTracking()
end)

-- Auto process once per map after a short delay (to ensure player/entities are ready)
hook.Add("InitPostEntity", "rtx_api_map_lights_AutoProcess", function()
    if not autospawn:GetBool() then return end
    local map = game.GetMap() or ""
    if map == "" or map == lastSpawnedMap then return end
    local delay = math.max(0, autospawn_delay:GetFloat())
    timer.Simple(delay, function()
        if not autospawn:GetBool() then return end
        local lp = LocalPlayer and LocalPlayer() or nil
        if not IsValid(lp) then return end
        lastSpawnedMap = map
        batchCreateRTXLights()
    end)
end)

-- Make functions accessible to other scripts
Light2RTX = {
    Process = batchCreateRTXLights,
    Clear = clearRTXLights,
    ToggleVisual = toggleVisualMode,
    Refresh = refreshAllLights
}

-- Expose a minimal API for animators to target lights by targetname
function Light2RTX.GetEntriesByTargetName(name)
    local result = {}
    if not name or name == "" then return result end
    local map = lightsByName[string.lower(name)]
    if not map then return result end
    for lightId, _ in pairs(map) do
        local idx = idToIndex[lightId]
        local entry = idx and createdLights[idx] or nil
        if entry then table.insert(result, entry) end
    end
    return result
end

function Light2RTX.UpdateEntry(entry)
    if entry then
        updateEntryRuntime(entry)
    end
end

function Light2RTX.SetEnabledByTargetName(name, enabled)
    local on = enabled and true or false
    for _, entry in ipairs(Light2RTX.GetEntriesByTargetName(name)) do
        entry.animEnabled = on
        entry.animMul = on and 1.0 or 0.0
        updateEntryRuntime(entry)
    end
end

function Light2RTX.ToggleByTargetName(name)
    for _, entry in ipairs(Light2RTX.GetEntriesByTargetName(name)) do
        entry.animEnabled = not entry.animEnabled
        entry.animMul = entry.animEnabled and 1.0 or 0.0
        updateEntryRuntime(entry)
    end
end

function Light2RTX.SetBrightnessMulByTargetName(name, mul)
    local m = tonumber(mul) or 1.0
    m = math.max(0.0, m)
    for _, entry in ipairs(Light2RTX.GetEntriesByTargetName(name)) do
        entry.animEnabled = m > 0
        entry.animMul = m
        updateEntryRuntime(entry)
    end
end

function Light2RTX.FadeBrightnessByTargetName(name, targetMul, duration)
    local t = tonumber(targetMul) or 1.0
    local d = math.max(0.0, tonumber(duration) or 0.0)
    local entries = Light2RTX.GetEntriesByTargetName(name)
    if d <= 0 then
        for _, entry in ipairs(entries) do
            entry.animEnabled = t > 0
            entry.animMul = t
            updateEntryRuntime(entry)
        end
        return
    end
    local steps = math.max(1, math.floor(d * 30))
    local interval = d / steps
    for _, entry in ipairs(entries) do
        local startMul = tonumber(entry.animMul or 1.0) or 1.0
        local timerName = "rtx_maplights_fade_" .. tostring(entry.id)
        if timer.Exists(timerName) then timer.Remove(timerName) end
        local i = 0
        timer.Create(timerName, interval, steps, function()
            if not entry then return end
            i = i + 1
            local alpha = i / steps
            entry.animMul = startMul + (t - startMul) * alpha
            entry.animEnabled = entry.animMul > 0
            updateEntryRuntime(entry)
        end)
    end
end

-- Get all lights of a specific classname (for HDRI Editor and other external tools)
function Light2RTX.GetEntriesByClassname(classname)
    local result = {}
    if not classname or classname == "" then return result end
    for _, entry in ipairs(createdLights) do
        if entry.classname == classname then
            table.insert(result, entry)
        end
    end
    return result
end

-- Forward pattern control to the animator (will be available when animator loads)
function Light2RTX.StartPattern(name, style)
    -- Defer to hook - the animator will handle this
    hook.Run("RTXMapLight_StartPattern", name, style)
end

print("[Light2RTX] Loaded! Use 'rtx_api_map_lights_process' to convert map lights to RTX lights")
print("[Light2RTX] Use 'rtx_api_map_lights_clear' to remove all created lights")
print("[Light2RTX] Use 'rtx_api_map_lights_toggle_visual' to toggle visual mode for moving lights with physgun")