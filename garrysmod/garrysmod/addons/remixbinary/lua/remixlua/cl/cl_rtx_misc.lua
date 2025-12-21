
-- ConVars
local cv_enabled = CreateClientConVar("rtx_pseudoplayer", 1, true, false)
local cv_pseudoweapon = CreateClientConVar("rtx_pseudoweapon", 1, true, false)

-- Entity references
local pseudoplayer_entity = nil
local pseudoweapon_entity = nil
local cv_disablevertexlighting = CreateClientConVar("rtx_disablevertexlighting", 0, true, false)
local cv_disablevertexlighting_old = CreateClientConVar("rtx_disablevertexlighting_old", 0, true, false)
local cv_experimental_manuallight = CreateClientConVar("rtx_experimental_manuallight", 0, true, false)
local cv_experimental_mightcrash_combinedlightingmode = CreateClientConVar("rtx_experimental_mightcrash_combinedlightingmode", 0, false, false)
local cv_disable_when_unsupported = CreateClientConVar("rtx_disable_when_unsupported", 1, false, false)

-- Light system cache
local lastLightUpdate = 0
local LIGHT_UPDATE_INTERVAL = 1.0

-- Remove halos
halo.Add = function() end


-- Pseudoplayer management
local function CreatePseudoplayer()
    if IsValid(pseudoplayer_entity) then
        return -- Already exists
    end
    
    pseudoplayer_entity = ents.CreateClientside("rtx_pseudoplayer")
    if IsValid(pseudoplayer_entity) then
        pseudoplayer_entity:Spawn()
        DebugPrint("[gmRTX] - Pseudoplayer created")
        
        -- Trigger auto-categorization after a short delay
        timer.Simple(3, function()
            RunConsoleCommand("rtx_categorize_pseudoplayer")
        end)
    else
        DebugPrint("[gmRTX] - Failed to create pseudoplayer")
    end
end

local function RemovePseudoplayer()
    if IsValid(pseudoplayer_entity) then
        pseudoplayer_entity:Remove()
        pseudoplayer_entity = nil
        DebugPrint("[gmRTX] - Pseudoplayer removed")
    end
end

-- Pseudoweapon management
local function CreatePseudoweapon()
    if IsValid(pseudoweapon_entity) then
        return -- Already exists
    end
    
    pseudoweapon_entity = ents.CreateClientside("rtx_pseudoweapon")
    if IsValid(pseudoweapon_entity) then
        pseudoweapon_entity:Spawn()
        DebugPrint("[gmRTX] - Pseudoweapon created")
        
        -- Trigger auto-categorization after a short delay
        timer.Simple(3, function()
            RunConsoleCommand("rtx_categorize_pseudoweapon")
        end)
    else
        DebugPrint("[gmRTX] - Failed to create pseudoweapon")
    end
end

local function RemovePseudoweapon()
    if IsValid(pseudoweapon_entity) then
        pseudoweapon_entity:Remove()
        pseudoweapon_entity = nil
        DebugPrint("[gmRTX] - Pseudoweapon removed")
    end
end

-- ConVar callback for pseudoplayer toggle
cvars.AddChangeCallback("rtx_pseudoplayer", function(convar, oldValue, newValue)
    if tonumber(newValue) == 1 then
        CreatePseudoplayer()
    else
        RemovePseudoplayer()
    end
end, "RTX_PseudoplayerToggle")

-- ConVar callback for pseudoweapon toggle
cvars.AddChangeCallback("rtx_pseudoweapon", function(convar, oldValue, newValue)
    if tonumber(newValue) == 1 then
        CreatePseudoweapon()
    else
        RemovePseudoweapon()
    end
end, "RTX_PseudoweaponToggle")

-- Entity management
local function DrawFix(self, flags)
    if cv_experimental_manuallight:GetBool() then return end
    render.SuppressEngineLighting(cv_disablevertexlighting:GetBool())

    -- Handle material overrides
    if self:GetMaterial() ~= "" then
        render.MaterialOverride(Material(self:GetMaterial()))
    end

    -- Handle submaterials
    for k, _ in pairs(self:GetMaterials()) do
        if self:GetSubMaterial(k - 1) ~= "" then
            render.MaterialOverrideByIndex(k - 1, Material(self:GetSubMaterial(k - 1)))
        end
    end

    -- Draw the model with static lighting (handle nil flags from infmap addon)
    local drawFlags = (flags or 0) + STUDIO_STATIC_LIGHTING
    self:DrawModel(drawFlags)
    render.MaterialOverride(nil)
    render.SuppressEngineLighting(false)
end

local function FixupEntity(ent)
    if IsValid(ent) and ent:GetClass() ~= "procedural_shard" then
        ent.RenderOverride = DrawFix
    end
end

local function FixupEntities()
    for _, ent in pairs(ents.GetAll()) do
        FixupEntity(ent)
    end
end

-- RTX initialization
local function RTXLoad()
    DebugPrint("[gmRTX] - Initializing Client")

    -- Set up console commands
    RunConsoleCommand("r_radiosity", "0")
    RunConsoleCommand("r_PhysPropStaticLighting", "1")
    RunConsoleCommand("r_colorstaticprops", "0")
    RunConsoleCommand("r_lightinterp", "0")
    RunConsoleCommand("mat_fullbright", cv_experimental_manuallight:GetBool() and "1" or "0")

    -- Create pseudoplayer if enabled
    if cv_enabled:GetBool() then
        CreatePseudoplayer()
    end
    
    -- Create pseudoweapon if enabled
    if cv_pseudoweapon:GetBool() then
        CreatePseudoweapon()
    end

    -- Initialize systems
    FixupEntities()
    halo.Add = function() end

end

-- Render hooks
local function PreRender()
    if render.SupportsVertexShaders_2_0() then return end

    render.SuppressEngineLighting(
        cv_disablevertexlighting_old:GetBool() or 
        cv_experimental_manuallight:GetBool()
    )

    if cv_experimental_mightcrash_combinedlightingmode:GetBool() then
        render.SuppressEngineLighting(false)
    end

    if cv_experimental_manuallight:GetBool() then
        -- DoCustomLights() - Function not implemented yet
        -- TODO: Implement custom lighting system
    end
end

-- Register hooks
hook.Add("InitPostEntity", "RTXReady", RTXLoad)
hook.Add("PreRender", "RTXPreRender", PreRender)
hook.Add("PreDrawOpaqueRenderables", "RTXPreRenderOpaque", PreRender)
hook.Add("PreDrawTranslucentRenderables", "RTXPreRenderTranslucent", PreRender)
hook.Add("OnEntityCreated", "RTXEntityFixups", FixupEntity)

-- Console commands
concommand.Add("rtx_fixnow", RTXLoad)
concommand.Add("rtx_pseudoplayer_reload", function()
    RemovePseudoplayer()
    timer.Simple(0.1, function()
        if cv_enabled:GetBool() then
            CreatePseudoplayer()
        end
    end)
end)
concommand.Add("rtx_pseudoweapon_reload", function()
    RemovePseudoweapon()
    timer.Simple(0.1, function()
        if cv_pseudoweapon:GetBool() then
            CreatePseudoweapon()
        end
    end)
end)
concommand.Add("rtx_force_no_fullbright", function()
    render.SetLightingMode(0)
end)

-- Toggle RT -- 
if jit.arch == "x64" then
    local isRaytracingDisabledBySpawnmenu = false
    local spawnIconTimerName = "RTXFixes_ReenableRaytracingTimer"
    
    -- Hook into SpawniconGenerated: Disable RT while icons are generating
    hook.Add("SpawniconGenerated", "RTXFixes_HandleSpawniconGenerated", function(lastmodel, imagename, modelsleft)
        DebugPrint(string.format("[gmRTX] SpawniconGenerated hook called. Models left: %s, isRaytracingDisabledBySpawnmenu = %s", tostring(modelsleft), tostring(isRaytracingDisabledBySpawnmenu)))
        
        -- First, always cancel any pending timers to re-enable RT
        if timer.Exists(spawnIconTimerName) then
            timer.Remove(spawnIconTimerName)
            DebugPrint("[gmRTX] Cancelled timer to re-enable RT (new icons are being generated)")
        end
    
        -- Disable RT if it hasn't already been disabled by us
        if not isRaytracingDisabledBySpawnmenu then
            DebugPrint("[gmRTX] SpawniconGenerated hook: Disabling RT for icon generation...")
            local success, err = pcall(function()
                if RemixConfig and RemixConfig.SetRaytracing then
                    return RemixConfig.SetRaytracing(false)
                else
                    error("RemixConfig API not available")
                end
            end)
            if success then
                DebugPrint("[gmRTX] SpawniconGenerated hook: RemixConfig.SetRaytracing(false) call succeeded.")
                isRaytracingDisabledBySpawnmenu = true
            else
                DebugPrint("[gmRTX] SpawniconGenerated hook: Warning: Failed to disable raytracing. Error: " .. tostring(err))
            end
        end
    
        -- Start a timer to re-enable RT after a delay (similar to how the progress UI works)
        -- This will get constantly reset as long as icons are being generated
        timer.Create(spawnIconTimerName, 3, 1, function()
            if isRaytracingDisabledBySpawnmenu then
                DebugPrint("[gmRTX] Timer: Re-enabling RT after icon generation completed...")
                local success, err = pcall(function()
                    if RemixConfig and RemixConfig.SetRaytracing then
                        return RemixConfig.SetRaytracing(true)
                    else
                        error("RemixConfig API not available")
                    end
                end)
                if success then
                    DebugPrint("[gmRTX] Timer: RemixConfig.SetRaytracing(true) call succeeded.")
                    isRaytracingDisabledBySpawnmenu = false
                else
                    DebugPrint("[gmRTX] Timer: Warning: Failed to re-enable raytracing. Error: " .. tostring(err))
                    -- Even if the call fails, reset the flag
                    isRaytracingDisabledBySpawnmenu = false
                end
            else
                DebugPrint("[gmRTX] Timer: Skipped enabling RT (was not disabled by us).")
            end
        end)
    end)
    
    hook.Remove("FinishToolMode", "RTXFixes_EnableRTOnFinishToolMode")
    
    DebugPrint("[gmRTX] Spawnicon RT toggle hooks initialized (Using SpawniconGenerated + timer).")
    
    -- Test Toggle Command --
    local currentRaytracingState = true -- Assume it starts enabled (default)
    
    local function ToggleRaytracingCommand()
        currentRaytracingState = not currentRaytracingState -- Flip the state
        DebugPrint("[gmRTX] Toggling raytracing via command. Setting enabled to: " .. tostring(currentRaytracingState))
        local success, err = pcall(function()
            if RemixConfig and RemixConfig.SetRaytracing then
                return RemixConfig.SetRaytracing(currentRaytracingState)
            else
                error("RemixConfig API not available")
            end
        end)
        if success then
            DebugPrint("[gmRTX] Successfully called RemixConfig.SetRaytracing.")
        else
            DebugPrint("[gmRTX] Warning: Failed to toggle raytracing via command. Error: " .. tostring(err))
            -- Flip state back if the call failed, so the next toggle attempt is correct
            currentRaytracingState = not currentRaytracingState
        end
    end
    concommand.Add("rtx_toggle_raytracing", ToggleRaytracingCommand)
    DebugPrint("[gmRTX] Added console command: rtx_toggle_raytracing")
    
    -- HUD notification for RT disabled state
    hook.Add("HUDPaint", "RTXFixes_RTDisabledNotification", function()
        if isRaytracingDisabledBySpawnmenu then
            -- Get screen dimensions
            local scrW, scrH = ScrW(), ScrH()
            
            -- Set up font and text
            local message = "RT Disabled (Building Spawnicons)"
            local font = "DermaLarge" -- Large built-in font
            surface.SetFont(font)
            local textW, textH = surface.GetTextSize(message)
            
            -- Position: upper right corner with some padding
            local padding = 20
            local x = scrW - textW - padding
            local y = padding
            
            -- Draw background
            local bgPadding = 10
            surface.SetDrawColor(0, 0, 0, 200) -- Semi-transparent black
            surface.DrawRect(x - bgPadding, y - bgPadding, textW + (bgPadding * 2), textH + (bgPadding * 2))
            
            -- Draw text
            surface.SetTextColor(255, 100, 100, 255) -- Red text
            surface.SetTextPos(x, y)
            surface.DrawText(message)
        end
    end)
else
    DebugPrint("[gmRTX] RT toggle features disabled: x64 required (current: " .. jit.arch .. ")")
end