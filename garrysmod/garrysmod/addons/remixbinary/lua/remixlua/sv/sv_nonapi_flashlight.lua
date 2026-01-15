if (BRANCH == "x86-64" or BRANCH == "chromium") then return end
if not SERVER then return end

util.AddNetworkString("rtx_nonapi_flashlight_state")

-- Track flashlight states per player
local playerFlashlightStates = {} -- [player] = bool

-- Toggle flashlight for a player
local function TogglePlayerFlashlight(ply)
    if not IsValid(ply) then return end
    
    -- Initialize if needed
    if playerFlashlightStates[ply] == nil then
        playerFlashlightStates[ply] = false
    end
    
    -- Toggle state
    playerFlashlightStates[ply] = not playerFlashlightStates[ply]
    
    -- Send state to the specific player (client-side handles their own flashlight)
    net.Start("rtx_nonapi_flashlight_state")
    net.WriteBool(playerFlashlightStates[ply])
    net.Send(ply)
end

-- Clean up when player disconnects
hook.Add("PlayerDisconnected", "RTXNonAPIFlashlight_Cleanup", function(ply)
    playerFlashlightStates[ply] = nil
end)

-- Clean up on death
hook.Add("PlayerDeath", "RTXNonAPIFlashlight_Death", function(victim, inflictor, attacker)
    if playerFlashlightStates[victim] then
        playerFlashlightStates[victim] = false
        
        net.Start("rtx_nonapi_flashlight_state")
        net.WriteBool(false)
        net.Send(victim)
    end
end)

-- Intercept Source engine flashlight toggle and use non-API flashlight instead
local cv_override = CreateConVar("rtx_nonapi_flashlight_override_default", "1", FCVAR_ARCHIVE, "Override default flashlight with non-API flashlight (32-bit)")

hook.Add("PlayerSwitchFlashlight", "RTXNonAPIFlashlight_Override", function(ply, enabled)
    -- If override is disabled, allow default behavior
    if not cv_override:GetBool() then return end
    
    -- Initialize state if needed
    if playerFlashlightStates[ply] == nil then
        playerFlashlightStates[ply] = false
    end
    
    -- Set flashlight to match the desired state
    playerFlashlightStates[ply] = enabled
    
    -- Send state to the player
    net.Start("rtx_nonapi_flashlight_state")
    net.WriteBool(enabled)
    net.Send(ply)
    
    -- Allow the engine to track flashlight state normally
    return true
end)
