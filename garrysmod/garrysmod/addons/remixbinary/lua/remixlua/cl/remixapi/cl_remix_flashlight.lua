if not (BRANCH == "x86-64" or BRANCH == "chromium") then return end
if not CLIENT then return end

-- RTX Remix Flashlight Replacement
-- Replaces the Source engine flashlight with an RTX spotlight that follows the player's view

-- Configuration ConVars
local cv_enabled = CreateClientConVar("rtx_flashlight_enabled", "1", true, false, "Enable RTX flashlight replacement")
local cv_brightness = CreateClientConVar("rtx_flashlight_brightness", "1000", true, false, "Flashlight brightness (0-100)")
local cv_radius = CreateClientConVar("rtx_flashlight_radius", "20", true, false, "Flashlight sphere radius")
local cv_cone_angle = CreateClientConVar("rtx_flashlight_cone_angle", "25", true, false, "Flashlight cone angle in degrees")
local cv_cone_softness = CreateClientConVar("rtx_flashlight_cone_softness", "0.45", true, false, "Flashlight cone edge softness (0-1)")
local cv_color_r = CreateClientConVar("rtx_flashlight_color_r", "255", true, false, "Flashlight red color (0-255)")
local cv_color_g = CreateClientConVar("rtx_flashlight_color_g", "255", true, false, "Flashlight green color (0-255)")
local cv_color_b = CreateClientConVar("rtx_flashlight_color_b", "255", true, false, "Flashlight blue color (0-255)")
local cv_offset_forward = CreateClientConVar("rtx_flashlight_offset_forward", "-5", true, false, "Forward offset from player eyes")
local cv_offset_right = CreateClientConVar("rtx_flashlight_offset_right", "0", true, false, "Right offset from player eyes")
local cv_offset_up = CreateClientConVar("rtx_flashlight_offset_up", "0", true, false, "Up offset from player eyes")
local cv_volumetric = CreateClientConVar("rtx_flashlight_volumetric", "0.0", true, false, "Volumetric intensity multiplier")
local cv_debug = CreateClientConVar("rtx_flashlight_debug", "0", true, false, "Show debug info")

-- State tracking
local flashlightActive = false
local flashlightLightId = nil

-- Multiplayer: Track all player flashlights
local playerFlashlights = {} -- [player] = { active = bool, lightId = number, color = Color() }

local function DebugPrint(...)
    if cv_debug:GetBool() then
        print("[RTX Flashlight]", ...)
    end
end

-- Reusable tables to avoid GC stutter
local _vec_pos = { x = 0, y = 0, z = 0 }
local _vec_dir = { x = 0, y = 0, z = 0 }
local _vec_rad = { x = 0, y = 0, z = 0 }
local _shaping = { direction = _vec_dir, coneAngleDegrees = 0, coneSoftness = 0, focusExponent = 1.0 }
local _sphere = { position = _vec_pos, radius = 0, shaping = _shaping, volumetricRadianceScale = 0 }
local _base = { hash = 0, radiance = _vec_rad, isDynamic = true, ignoreViewModel = true }

local function vec3(x, y, z)
    return { x = x, y = y, z = z }
end

-- Create the RTX flashlight light for any player
local function CreateFlashlight(ply, colorOverride)
    if not cv_enabled:GetBool() then return false end
    if not istable(RemixLight) then
        DebugPrint("RemixLight API not available")
        return false
    end
    
    ply = ply or LocalPlayer()
    if not IsValid(ply) then return false end
    
    -- Get position and direction
    local eyePos = ply:EyePos()
    local eyeAngles = ply:EyeAngles()
    local forward = eyeAngles:Forward()
    local right = eyeAngles:Right()
    local up = eyeAngles:Up()
    
    -- Apply offset
    local offsetPos = eyePos
        + forward * cv_offset_forward:GetFloat()
        + right * cv_offset_right:GetFloat()
        + up * cv_offset_up:GetFloat()
    
    -- Get color and brightness
    -- Use color override if provided (from network), otherwise use ConVars for local player
    local brightness = cv_brightness:GetFloat()
    local scale = brightness / 1000.0
    local r, g, b
    if colorOverride then
        r = colorOverride.r * scale
        g = colorOverride.g * scale
        b = colorOverride.b * scale
    else
        r = math.Clamp(cv_color_r:GetInt(), 0, 255) * scale
        g = math.Clamp(cv_color_g:GetInt(), 0, 255) * scale
        b = math.Clamp(cv_color_b:GetInt(), 0, 255) * scale
    end
    
    -- Create light definition
    local hash = tonumber(util.CRC("rtx_flashlight_" .. ply:EntIndex())) or 99999
    local base = {
        hash = hash,
        radiance = vec3(r, g, b),
        isDynamic = true,
        ignoreViewModel = true,  -- Don't light view models
    }
    
    local sphere = {
        position = vec3(offsetPos.x, offsetPos.y, offsetPos.z),
        radius = cv_radius:GetFloat(),
        shaping = {
            direction = vec3(forward.x, forward.y, forward.z),
            coneAngleDegrees = cv_cone_angle:GetFloat(),
            coneSoftness = cv_cone_softness:GetFloat(),
            focusExponent = 1.0,
        },
        volumetricRadianceScale = cv_volumetric:GetFloat(),
    }
    
    -- Create directly
    local lightId = nil
    if RemixLight.CreateSphere then
        lightId = RemixLight.CreateSphere(base, sphere, ply:EntIndex())
    end
    
    if lightId and lightId ~= 0 then
        -- Track for local player separately
        if ply == LocalPlayer() then
            flashlightLightId = lightId
            flashlightActive = true
        end
        
        -- Store the color used
        local storedColor = colorOverride or Color(
            cv_color_r:GetInt(),
            cv_color_g:GetInt(),
            cv_color_b:GetInt()
        )
        
        -- Track in multiplayer table
        playerFlashlights[ply] = {
            active = true,
            lightId = lightId,
            color = storedColor,
            hash = hash
        }
        
        DebugPrint("Flashlight created for player", ply:Nick(), "with ID:", lightId)
        return true
    else
        DebugPrint("Failed to create flashlight")
        return false
    end
end

-- Cached settings (only read ConVars when they change)
local cachedSettings = {
    brightness = 15,
    radius = 80,
    coneAngle = 35,
    coneSoftness = 0.3,
    volumetric = 2.0,
    offsetForward = 10,
    offsetRight = 5,
    offsetUp = -3,
    lastUpdate = 0,
}

local SETTINGS_CACHE_TIME = 0.1 -- Update cache every 0.1 seconds

local function UpdateCachedSettings()
    local currentTime = CurTime()
    if currentTime - cachedSettings.lastUpdate < SETTINGS_CACHE_TIME then return end
    
    cachedSettings.brightness = cv_brightness:GetFloat()
    cachedSettings.radius = cv_radius:GetFloat()
    cachedSettings.coneAngle = cv_cone_angle:GetFloat()
    cachedSettings.coneSoftness = cv_cone_softness:GetFloat()
    cachedSettings.volumetric = cv_volumetric:GetFloat()
    cachedSettings.offsetForward = cv_offset_forward:GetFloat()
    cachedSettings.offsetRight = cv_offset_right:GetFloat()
    cachedSettings.offsetUp = cv_offset_up:GetFloat()
    cachedSettings.lastUpdate = currentTime
end

-- Update the RTX flashlight position and direction for a specific player
local function UpdateFlashlight(ply, posOverride, angOverride)
    ply = ply or LocalPlayer()
    
    local flashData = playerFlashlights[ply]
    if not flashData or not flashData.active or not flashData.lightId then return end
    if not istable(RemixLight) then return end
    if not IsValid(ply) then 
        -- Player is invalid, destroy the light
        DestroyFlashlight(ply)
        return 
    end
    
    -- Get current position and direction
    -- Use overrides if provided (for local player camera lock), otherwise calculate from entity
    local eyePos, eyeAngles
    if posOverride and angOverride then
        eyePos = posOverride
        eyeAngles = angOverride
    else
        eyePos = ply:EyePos()
        eyeAngles = ply:EyeAngles()
    end

    local forward = eyeAngles:Forward()
    local right = eyeAngles:Right()
    local up = eyeAngles:Up()
    
    -- Apply offset using cached settings
    local offsetPos = eyePos
        + forward * cachedSettings.offsetForward
        + right * cachedSettings.offsetRight
        + up * cachedSettings.offsetUp
    
    -- Get color and brightness
    -- Use stored color for this player, with current brightness settings
    local scale = cachedSettings.brightness / 1000.0
    local playerColor = flashData.color or Color(255, 240, 200)
    local r = playerColor.r * scale
    local g = playerColor.g * scale
    local b = playerColor.b * scale
    
    -- Update reusable tables
    _vec_rad.x = r
    _vec_rad.y = g
    _vec_rad.z = b
    
    _base.hash = flashData.hash or (tonumber(util.CRC("rtx_flashlight_" .. ply:EntIndex())) or 99999)
    -- _base.radiance is already _vec_rad
    _base.isDynamic = true -- Flashlight moves every frame, must be dynamic to avoid cache thrashing/ghosting
    
    _vec_pos.x = offsetPos.x
    _vec_pos.y = offsetPos.y
    _vec_pos.z = offsetPos.z
    
    _vec_dir.x = forward.x
    _vec_dir.y = forward.y
    _vec_dir.z = forward.z
    
    _shaping.coneAngleDegrees = cachedSettings.coneAngle
    _shaping.coneSoftness = cachedSettings.coneSoftness
    -- _shaping.direction is already _vec_dir
    
    _sphere.radius = cachedSettings.radius
    _sphere.volumetricRadianceScale = cachedSettings.volumetric
    -- _sphere.position is already _vec_pos
    -- _sphere.shaping is already _shaping
    
    -- Update directly
    if RemixLight.UpdateSphere then
        RemixLight.UpdateSphere(_base, _sphere, flashData.lightId)
    end
end

-- Destroy the RTX flashlight for a specific player
local function DestroyFlashlight(ply)
    ply = ply or LocalPlayer()
    
    local flashData = playerFlashlights[ply]
    if not flashData or not flashData.lightId then return end
    
    local lightIdToDestroy = flashData.lightId
    
    -- Set inactive FIRST to stop any pending updates
    flashData.active = false
    
    -- Update local player tracking
    if ply == LocalPlayer() then
        flashlightActive = false
        flashlightLightId = nil
    end
    
    -- Destroy directly
    if istable(RemixLight) and RemixLight.DestroyLight then
        RemixLight.DestroyLight(lightIdToDestroy)
    end
    
    playerFlashlights[ply] = nil
    
    DebugPrint("Flashlight destroyed for player", IsValid(ply) and ply:Nick() or "invalid", "ID:", lightIdToDestroy)
end

-- Toggle flashlight on/off for local player
local function ToggleFlashlight()
    if not cv_enabled:GetBool() then
        print("[RTX Flashlight] Disabled - enable with rtx_flashlight_enabled 1")
        return
    end
    
    -- Send toggle request to server for multiplayer sync with color data
    net.Start("rtx_flashlight_toggle")
    net.WriteUInt(cv_color_r:GetInt(), 8)
    net.WriteUInt(cv_color_g:GetInt(), 8)
    net.WriteUInt(cv_color_b:GetInt(), 8)
    net.SendToServer()
    
    -- Play sound locally (will be synced with server response)
    LocalPlayer():EmitSound("items/flashlight1.wav", 50, 100, 1, CHAN_ITEM)
end

-- RenderScene hook for low-latency updates (runs every frame during rendering)
hook.Add("Think", "RTXFlashlight_Update", function(origin, angles, fov)
    -- Update cached settings periodically
    UpdateCachedSettings()
    
    local localPly = LocalPlayer()
    local useCamera = IsValid(localPly) and not localPly:ShouldDrawLocalPlayer()

    -- Update all active player flashlights
    for ply, flashData in pairs(playerFlashlights) do
        if IsValid(ply) and flashData.active then
            -- For local player in first person, use exact camera transform to prevent jitter
            if ply == localPly and useCamera then
                UpdateFlashlight(ply, origin, angles)
            else
                UpdateFlashlight(ply)
            end
        else
            -- Clean up invalid players
            if not IsValid(ply) then
                playerFlashlights[ply] = nil
            end
        end
    end
end)

-- Network: Receive flashlight state updates from server
net.Receive("rtx_flashlight_state", function()
    local ply = net.ReadEntity()
    local state = net.ReadBool()
    local r = net.ReadUInt(8)
    local g = net.ReadUInt(8)
    local b = net.ReadUInt(8)
    
    if not IsValid(ply) then return end
    
    if state then
        -- Turn on flashlight for this player with their color
        CreateFlashlight(ply, Color(r, g, b))
    else
        -- Turn off flashlight for this player
        DestroyFlashlight(ply)
    end
end)

-- Network: Receive color update from server (real-time adjustment)
net.Receive("rtx_flashlight_update_color", function()
    local ply = net.ReadEntity()
    local r = net.ReadUInt(8)
    local g = net.ReadUInt(8)
    local b = net.ReadUInt(8)
    
    if not IsValid(ply) then return end
    
    local flashData = playerFlashlights[ply]
    if flashData and flashData.active then
        -- Update stored color
        flashData.color = Color(r, g, b)
        -- Light will update on next frame via RenderScene hook
    end
end)

-- Cleanup on map change
hook.Add("OnReloaded", "RTXFlashlight_Cleanup", function()
    -- Clean up all player flashlights
    for ply, _ in pairs(playerFlashlights) do
        DestroyFlashlight(ply)
    end
    playerFlashlights = {}
end)

hook.Add("ShutDown", "RTXFlashlight_Cleanup", function()
    -- Clean up all player flashlights
    for ply, _ in pairs(playerFlashlights) do
        DestroyFlashlight(ply)
    end
    playerFlashlights = {}
end)

-- Console commands
concommand.Add("rtx_flashlight_toggle", function()
    ToggleFlashlight()
end, nil, "Toggle RTX flashlight on/off (bind this to a key)")

concommand.Add("rtx_flashlight_on", function()
    if not cv_enabled:GetBool() then
        print("[RTX Flashlight] Disabled - enable with rtx_flashlight_enabled 1")
        return
    end
    
    if not flashlightActive then
        net.Start("rtx_flashlight_toggle")
        net.WriteUInt(cv_color_r:GetInt(), 8)
        net.WriteUInt(cv_color_g:GetInt(), 8)
        net.WriteUInt(cv_color_b:GetInt(), 8)
        net.SendToServer()
        LocalPlayer():EmitSound("items/flashlight1.wav", 50, 100, 1, CHAN_ITEM)
    end
end, nil, "Turn RTX flashlight on")

concommand.Add("rtx_flashlight_off", function()
    if flashlightActive then
        net.Start("rtx_flashlight_toggle")
        net.WriteUInt(cv_color_r:GetInt(), 8)
        net.WriteUInt(cv_color_g:GetInt(), 8)
        net.WriteUInt(cv_color_b:GetInt(), 8)
        net.SendToServer()
        LocalPlayer():EmitSound("items/flashlight1.wav", 50, 100, 1, CHAN_ITEM)
    end
end, nil, "Turn RTX flashlight off")

concommand.Add("rtx_flashlight_reload", function()
    if flashlightActive then
        DestroyFlashlight()
        timer.Simple(0.1, function()
            CreateFlashlight()
        end)
        print("[RTX Flashlight] Reloaded")
    else
        print("[RTX Flashlight] Not active")
    end
end, nil, "Reload the flashlight")

-- Debug visualization
hook.Add("PostDrawTranslucentRenderables", "RTXFlashlight_Debug", function(depth, sky)
    if not cv_debug:GetBool() then return end
    if not flashlightActive then return end
    
    local ply = LocalPlayer()
    if not IsValid(ply) then return end
    
    local eyePos = ply:EyePos()
    local eyeAngles = ply:EyeAngles()
    local forward = eyeAngles:Forward()
    local right = eyeAngles:Right()
    local up = eyeAngles:Up()
    
    local offsetPos = eyePos
        + forward * cv_offset_forward:GetFloat()
        + right * cv_offset_right:GetFloat()
        + up * cv_offset_up:GetFloat()
    
    -- Draw debug beam
    render.SetMaterial(Material("cable/physbeam"))
    render.DrawBeam(offsetPos, offsetPos + forward * 512, 2, 0, 1, Color(255, 255, 0, 200))
    
    -- Draw position marker
    render.DrawWireframeSphere(offsetPos, cv_radius:GetFloat() * 0.1, 8, 8, Color(255, 255, 0, 150))
end)

-- Tool menu integration
hook.Add("PopulateToolMenu", "RTXFlashlight_Menu", function()
    spawnmenu.AddToolMenuOption("Utilities", "RTX Remix - API Lights", "Flashlight", "Flashlight", "", "", function(panel)
        panel:ClearControls()
        
        panel:Help("Light Properties")
        panel:NumSlider("Brightness", "rtx_flashlight_brightness", 0, 100000, 1)
        panel:NumSlider("Radius", "rtx_flashlight_radius", 1, 200, 0)
        panel:NumSlider("Cone Angle", "rtx_flashlight_cone_angle", 5, 90, 0)
        panel:NumSlider("Cone Softness", "rtx_flashlight_cone_softness", 0, 1, 2)
        panel:NumSlider("Volumetric Scale", "rtx_flashlight_volumetric", 0, 5, 1)
        
        panel:Help("Color")
        
        -- Color picker
        local colorPicker = vgui.Create("DColorMixer", panel)
        colorPicker:SetPalette(true)
        colorPicker:SetAlphaBar(false)
        colorPicker:SetWangs(true)
        colorPicker:SetColor(Color(
            cv_color_r:GetInt(),
            cv_color_g:GetInt(),
            cv_color_b:GetInt()
        ))
        
        -- Update ConVars when color changes
        colorPicker.ValueChanged = function(self, col)
            RunConsoleCommand("rtx_flashlight_color_r", tostring(col.r))
            RunConsoleCommand("rtx_flashlight_color_g", tostring(col.g))
            RunConsoleCommand("rtx_flashlight_color_b", tostring(col.b))
        end
        
        panel:AddItem(colorPicker)

        panel:Help("Position Offset")
        panel:NumSlider("Forward Offset", "rtx_flashlight_offset_forward", -100, 100, 0)
        panel:NumSlider("Right Offset", "rtx_flashlight_offset_right", -100, 100, 0)
        panel:NumSlider("Up Offset", "rtx_flashlight_offset_up", -100, 100, 0)
    end)
end)

-- Real-time color update when ConVars change
local lastColorUpdate = 0
local COLOR_UPDATE_DELAY = 0.05 -- Throttle to 20 updates per second max

local function SendColorUpdate()
    local currentTime = CurTime()
    if currentTime - lastColorUpdate < COLOR_UPDATE_DELAY then return end
    
    -- Only send if our flashlight is active
    if not flashlightActive then return end
    
    lastColorUpdate = currentTime
    
    -- Send color update to server
    net.Start("rtx_flashlight_update_color")
    net.WriteUInt(cv_color_r:GetInt(), 8)
    net.WriteUInt(cv_color_g:GetInt(), 8)
    net.WriteUInt(cv_color_b:GetInt(), 8)
    net.SendToServer()
    
    -- Update local flashlight color immediately
    local flashData = playerFlashlights[LocalPlayer()]
    if flashData then
        flashData.color = Color(cv_color_r:GetInt(), cv_color_g:GetInt(), cv_color_b:GetInt())
    end
end

-- Add ConVar change callbacks for real-time updates
cvars.AddChangeCallback("rtx_flashlight_color_r", SendColorUpdate, "rtx_flashlight_color_update")
cvars.AddChangeCallback("rtx_flashlight_color_g", SendColorUpdate, "rtx_flashlight_color_update")
cvars.AddChangeCallback("rtx_flashlight_color_b", SendColorUpdate, "rtx_flashlight_color_update")

print("[RTX Flashlight] Loaded! Press 'F' (your flashlight key) to toggle the RTX flashlight")
print("[RTX Flashlight] Or bind manually: bind f rtx_flashlight_toggle")
