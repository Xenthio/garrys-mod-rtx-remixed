if not (BRANCH == "x86-64" or BRANCH == "chromium") then return end
if not CLIENT then return end

-- Configuration
local cv_enabled = CreateClientConVar("rtx_projectedtexture_wrapper_enabled", "1", true, false, "Enable RTX wrapping for ProjectedTexture() calls")
local cv_debug = CreateClientConVar("rtx_projectedtexture_wrapper_debug", "0", true, false, "Debug logging for projected texture wrapper")
local cv_brightness_scale = CreateClientConVar("rtx_projectedtexture_wrapper_brightness_scale", "10", true, false, "Brightness scaling for projected textures")
local cv_radius_scale = CreateClientConVar("rtx_projectedtexture_wrapper_radius_scale", "0.005", true, false, "Radius scaling for projected textures")
local cv_skip_change_detection = CreateClientConVar("rtx_projectedtexture_wrapper_skip_change_detection", "1", true, false, "Skip change detection and update every frame (lowest latency)")
local cv_offset_x = CreateClientConVar("rtx_projectedtexture_wrapper_offset_x", "50", true, false, "Position offset X in local space (forward/back relative to entity)")
local cv_offset_y = CreateClientConVar("rtx_projectedtexture_wrapper_offset_y", "0", true, false, "Position offset Y in local space (left/right relative to entity)")
local cv_offset_z = CreateClientConVar("rtx_projectedtexture_wrapper_offset_z", "0", true, false, "Position offset Z in local space (up/down relative to entity)")

-- Tracking: projectedTexture object -> { rtxLightId, props }
local wrappedProjectedTextures = {}
local textureIdCounter = 0

local function DebugPrint(...)
    if cv_debug:GetBool() then
        print("[RTX ProjectedTexture]", ...)
    end
end

local function vec3(x, y, z)
    return { x = x, y = y, z = z }
end

-- Store original ProjectedTexture function
local OriginalProjectedTexture = ProjectedTexture

-- Create or update RTX light from projected texture properties
local function UpdateRTXFromProjectedTexture(projTex, textureId)
    if not cv_enabled:GetBool() then return end
    if not istable(RemixLight) then return end
    if not projTex then return end
    
    -- Get all properties from the projected texture
    local pos = projTex:GetPos()
    local angles = projTex:GetAngles()
    local color = projTex:GetColor()
    local brightness = projTex:GetBrightness()
    local fov = projTex:GetHorizontalFOV()
    local farZ = projTex:GetFarZ()
    
    -- Check if valid
    if not pos or not angles or not color then return end
    
    -- Apply position offset in local space (relative to entity orientation)
    local offsetX = cv_offset_x:GetFloat()
    local offsetY = cv_offset_y:GetFloat()
    local offsetZ = cv_offset_z:GetFloat()
    
    if offsetX ~= 0 or offsetY ~= 0 or offsetZ ~= 0 then
        -- Transform local offset to world space using entity angles
        local forward = angles:Forward()
        local right = angles:Right()
        local up = angles:Up()
        
        pos = pos + forward * offsetX + right * offsetY + up * offsetZ
    end
    
    -- Apply scaling
    local brightnessScale = cv_brightness_scale:GetFloat()
    local radiusScale = cv_radius_scale:GetFloat()
    
    -- Calculate radiance
    local scale = (brightness * brightnessScale) / 100.0
    local base = {
        hash = tonumber(util.CRC(string.format("projected_texture_%d", textureId))) or (textureId + 200000),
        radiance = vec3(
            color.r * scale,
            color.g * scale,
            color.b * scale
        ),
        isDynamic = true,
    }
    
    -- ProjectedTexture uses FOV (full cone angle)
    -- RTX uses half-angle from center to edge
    local coneAngle = fov and (fov / 2) or 45
    
    -- Use FarZ as radius if available, otherwise default
    local radius = (farZ or 1500) * radiusScale
    
    local forward = angles:Forward()
    local sphere = {
        position = vec3(pos.x, pos.y, pos.z),
        radius = radius,
        volumetricRadianceScale = 1.0,
        shaping = {
            direction = vec3(forward.x, forward.y, forward.z),
            coneAngleDegrees = coneAngle,
            coneSoftness = 0.2,
            focusExponent = 1.0,
        }
    }
    
    -- Check if we need to create or update
    local data = wrappedProjectedTextures[projTex]
    
    if not data or not data.rtxLightId or data.rtxLightId == 0 then
        -- Create new RTX light (always direct for lowest latency)
        local rtxLightId = nil
        if RemixLight.CreateSphere then
            rtxLightId = RemixLight.CreateSphere(base, sphere, textureId + 200000)
        end
        
        if rtxLightId and rtxLightId ~= 0 then
            wrappedProjectedTextures[projTex] = {
                rtxLightId = rtxLightId,
                textureId = textureId,
                props = { pos = pos, angles = angles, color = color, brightness = brightness, fov = fov, farZ = farZ }
            }
            DebugPrint("Created RTX spotlight", rtxLightId, "for ProjectedTexture", textureId)
        end
    else
        -- Update existing RTX light
        local skipChangeDetection = cv_skip_change_detection:GetBool()
        
        local needsUpdate = skipChangeDetection
        
        if not skipChangeDetection then
            -- Check if properties changed significantly
            local oldProps = data.props
            if oldProps.pos:DistToSqr(pos) > 1 then needsUpdate = true end
            if math.abs((oldProps.angles.p or 0) - (angles.p or 0)) > 0.5 then needsUpdate = true end
            if math.abs((oldProps.angles.y or 0) - (angles.y or 0)) > 0.5 then needsUpdate = true end
            if oldProps.color.r ~= color.r or oldProps.color.g ~= color.g or oldProps.color.b ~= color.b then needsUpdate = true end
            if math.abs((oldProps.brightness or 0) - brightness) > 0.1 then needsUpdate = true end
            if math.abs((oldProps.fov or 0) - (fov or 0)) > 1 then needsUpdate = true end
        end
        
        if needsUpdate then
            -- Update directly
            if RemixLight.UpdateSphere then
                RemixLight.UpdateSphere(base, sphere, data.rtxLightId)
            end
            
            data.props = { pos = pos, angles = angles, color = color, brightness = brightness, fov = fov, farZ = farZ }
            if not skipChangeDetection then
                DebugPrint("Updated RTX spotlight", data.rtxLightId, "for ProjectedTexture", textureId)
            end
        end
    end
end

-- Remove RTX light for a projected texture
local function RemoveRTXForProjectedTexture(projTex)
    local data = wrappedProjectedTextures[projTex]
    if not data then return end
    
    if data.rtxLightId then
        if istable(RemixLight) and RemixLight.DestroyLight then
            RemixLight.DestroyLight(data.rtxLightId)
        end
        DebugPrint("Destroyed RTX spotlight", data.rtxLightId, "for ProjectedTexture", data.textureId)
    end
    
    wrappedProjectedTextures[projTex] = nil
end

-- Hook into ProjectedTexture function
function ProjectedTexture()
    -- Call original function
    local projTex = OriginalProjectedTexture()
    
    if not projTex then return projTex end
    
    -- Assign unique ID
    textureIdCounter = textureIdCounter + 1
    local textureId = textureIdCounter
    
    -- Store reference to track this texture
    wrappedProjectedTextures[projTex] = {
        textureId = textureId,
        props = {}
    }
    
    DebugPrint("ProjectedTexture created with ID", textureId)
    
    return projTex
end

-- Hook-based update for instant response
hook.Add("Think", "RTXProjectedTexture_Update", function()
    if not cv_enabled:GetBool() then return end
    if not istable(RemixLight) then return end
    
    local toRemove = {}
    
    for projTex, data in pairs(wrappedProjectedTextures) do
        -- Check if ProjectedTexture was removed using IsValid()
        if not IsValid(projTex) then
            table.insert(toRemove, projTex)
            DebugPrint("Detected removed ProjectedTexture", data.textureId)
        else
            -- Still valid - update position/angles every frame (vehicles move)
            UpdateRTXFromProjectedTexture(projTex, data.textureId)
        end
    end
    
    -- Clean up removed textures
    for _, projTex in ipairs(toRemove) do
        RemoveRTXForProjectedTexture(projTex)
    end
end)

-- Cleanup on map change
hook.Add("OnReloaded", "RTXProjectedTexture_Cleanup", function()
    for projTex, data in pairs(wrappedProjectedTextures) do
        if data.rtxLightId then
            if istable(RemixLight) and RemixLight.DestroyLight then
                RemixLight.DestroyLight(data.rtxLightId)
            end
        end
    end
    wrappedProjectedTextures = {}
    textureIdCounter = 0
end)

hook.Add("ShutDown", "RTXProjectedTexture_Cleanup", function()
    for projTex, data in pairs(wrappedProjectedTextures) do
        if data.rtxLightId then
            if istable(RemixLight) and RemixLight.DestroyLight then
                RemixLight.DestroyLight(data.rtxLightId)
            end
        end
    end
    wrappedProjectedTextures = {}
end)

-- Console commands
concommand.Add("rtx_projectedtexture_list", function()
    print("[RTX ProjectedTexture] Currently wrapped projected textures:")
    local count = 0
    local invalidCount = 0
    
    for projTex, data in pairs(wrappedProjectedTextures) do
        local props = data.props
        
        if IsValid(projTex) then
            local pos = props.pos or Vector(0,0,0)
            local angles = props.angles or Angle(0,0,0)
            print(string.format("  - ProjectedTexture %d -> RTX Light %d (pos: %.0f %.0f %.0f, ang: %.0f %.0f %.0f, fov: %.0f) [VALID]",
                data.textureId, data.rtxLightId or 0, pos.x, pos.y, pos.z, angles.p, angles.y, angles.r, props.fov or 0))
            count = count + 1
        else
            print(string.format("  - ProjectedTexture %d -> RTX Light %d [INVALID]", data.textureId, data.rtxLightId or 0))
            invalidCount = invalidCount + 1
        end
    end
    
    print(string.format("Total: %d active, %d invalid (will be cleaned up)", count, invalidCount))
end, nil, "List all wrapped projected textures")

concommand.Add("rtx_projectedtexture_clear", function()
    local count = table.Count(wrappedProjectedTextures)
    for projTex, data in pairs(wrappedProjectedTextures) do
        if data.rtxLightId then
            if istable(RemixLight) and RemixLight.DestroyLight then
                RemixLight.DestroyLight(data.rtxLightId)
            end
        end
    end
    wrappedProjectedTextures = {}
    print("[RTX ProjectedTexture] Cleared " .. count .. " projected textures")
end, nil, "Clear all wrapped projected textures")

concommand.Add("rtx_projectedtexture_cleanup", function()
    print("[RTX ProjectedTexture] Running manual cleanup...")
    local toRemove = {}
    
    for projTex, data in pairs(wrappedProjectedTextures) do
        if not IsValid(projTex) then
            table.insert(toRemove, projTex)
            print(string.format("  Removing invalid ProjectedTexture %d", data.textureId))
        end
    end
    
    for _, projTex in ipairs(toRemove) do
        RemoveRTXForProjectedTexture(projTex)
    end
    
    print(string.format("[RTX ProjectedTexture] Cleaned up %d invalid projected textures", #toRemove))
    print(string.format("[RTX ProjectedTexture] Remaining: %d", table.Count(wrappedProjectedTextures)))
end, nil, "Clean up invalid projected textures")

concommand.Add("rtx_projectedtexture_force_clear_all", function()
    local count = table.Count(wrappedProjectedTextures)
    for projTex, data in pairs(wrappedProjectedTextures) do
        RemoveRTXForProjectedTexture(projTex)
    end
    wrappedProjectedTextures = {}
    print("[RTX ProjectedTexture] Force cleared all " .. count .. " projected texture lights")
end, nil, "Force clear ALL projected texture lights (debug)")

concommand.Add("rtx_projectedtexture_reset_offsets", function()
    RunConsoleCommand("rtx_projectedtexture_wrapper_offset_x", "50")
    RunConsoleCommand("rtx_projectedtexture_wrapper_offset_y", "0")
    RunConsoleCommand("rtx_projectedtexture_wrapper_offset_z", "0")
    print("[RTX ProjectedTexture] Reset position offsets to 0, 0, 0")
end, nil, "Reset all position offsets to zero")

-- Public API
local RTXProjectedTextureWrapper = {}

function RTXProjectedTextureWrapper.GetWrappedCount()
    return table.Count(wrappedProjectedTextures)
end

function RTXProjectedTextureWrapper.GetWrappedLight(projTex)
    local data = wrappedProjectedTextures[projTex]
    return data and data.rtxLightId or nil
end

_G.RTXProjectedTextureWrapper = RTXProjectedTextureWrapper