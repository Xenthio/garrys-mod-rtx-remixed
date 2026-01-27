if not CLIENT then return end

-- ConVars
local CONVARS = {
    SHOW_3DSKY_WARNING = CreateClientConVar("rtx_show_3dsky_warning", "1", true, false, "Show warning when enabling r_3dsky")
}

-- Force enable lightupdaters on 32-bit (API lights not available)
local is32Bit = not (BRANCH == "x86-64" or BRANCH == "chromium")
if is32Bit then
    RunConsoleCommand("rtx_lightupdater", "1")
end

hook.Add( "PopulateToolMenu", "RTXOptionsClient_BaseOptions", function()
    spawnmenu.AddToolMenuOption( "Utilities", "RTX Remix", "RTX_Client_BaseOptions", "#Base Options", "", "", function( panel )
        panel:ClearControls()

        panel:CheckBox( "Pseudoplayer Enabled", "rtx_pseudoplayer" )
        panel:ControlHelp( "Pseudoplayer allows you to see your own playermodel, this when marked as a 'Playermodel Texture' in remix allows you to see your own shadow and reflection." )
        panel:CheckBox( "Pseudoweapon Enabled", "rtx_pseudoweapon" )
        panel:ControlHelp( "Similar to above, but for the weapon you're holding." )

        panel:CheckBox( "Fix Skybox Leaking", "rtx_sbr_enable" )
        panel:ControlHelp( "Hides geometry behind the skybox to prevent it from leaking through, doesn't allow light_enviornment entities to pass through though." )
        panel:ControlHelp( "Also breaks HDRIs.")

        -- Only show lightupdater option on 64-bit (32-bit must use lightupdaters)
        local is64Bit = (BRANCH == "x86-64" or BRANCH == "chromium")
        if is64Bit then
            panel:CheckBox( "Disable Remix API Lights", "rtx_lightupdater" )
            panel:ControlHelp( "Enables lightupdaters, which will make Remix use the detected d3d9 lights from the game instead of API lights." )
        else
        end

        panel:Help("Map Fixes")
        panel:ControlHelp("Fixes broken geometry rendering for the current map, can lower FPS.")
        panel:Button("Enable Map Fixes", "rtx_mf_enable_current_map")
        panel:Button("Disable Map Fixes", "rtx_mf_disable_current_map")
    end )
end )

hook.Add( "PopulateToolMenu", "RTXOptionsClient_Performance", function()
    spawnmenu.AddToolMenuOption( "Utilities", "RTX Remix", "RTX_Client_Performance", "#Performance", "", "", function( panel )
        panel:ClearControls()   
        
        local sprCheckbox = panel:CheckBox("Use SPR for Static Props", "rtx_spr_enable")
        panel:ControlHelp("Disables engine rendering of static props and replaces it with a lua-based renderer. Potentially large performance boost on dense maps.")
        panel:ControlHelp("")
        panel:ControlHelp("This breaks remix mesh replacements for engine rendered static props.")

        local pvsDebugCheckbox = panel:CheckBox("Show SPR PVS Debug HUD", "rtx_render_debug")

        local pvsCheckbox = panel:CheckBox("Use PVS Culling", "rtx_spr_use_pvs")
        panel:ControlHelp("Enables Potentially Visible Set culling for static prop renderer. Improves performance but may cause some props to cull incorrectly.")
        
        local pvsSlider = panel:NumSlider("PVS Safety Distance", "rtx_spr_pvs_safety_distance", 0, 8192, 0)
        panel:ControlHelp("Distance within which PVS culling is disabled (prevents close-range culling bugs).")
        panel:ControlHelp("Saves value per map")
        
        -- Update PVS controls based on SPR enabled state
        local function UpdatePVSControls()
            local sprEnabled = GetConVar("rtx_spr_enable"):GetBool()
            if pvsCheckbox then pvsCheckbox:SetEnabled(sprEnabled) end
            if pvsSlider then pvsSlider:SetEnabled(sprEnabled) end
            if pvsDebugCheckbox then pvsDebugCheckbox:SetEnabled(sprEnabled) end
        end
        
        -- Set initial state
        UpdatePVSControls()
        
        -- Update when SPR checkbox changes
        if sprCheckbox then
            sprCheckbox.OnChange = function(self, value)
                UpdatePVSControls()
            end
        end
    end )
end )

local function Show3DSkyWarning()
    -- Don't show if user has disabled warnings
    if not CONVARS.SHOW_3DSKY_WARNING:GetBool() then return end
    
    -- Create the warning panel
    local frame = vgui.Create("DFrame")
    frame:SetTitle("RTX Remix Fixes 2")
    frame:SetSize(400, 200) 
    frame:Center()
    frame:MakePopup()
    
    local warningText = vgui.Create("DLabel", frame)
    warningText:SetPos(20, 40)
    warningText:SetSize(360, 80)
    warningText:SetText("You have enabled r_3dsky which may cause rendering issues with RTX Remix due how the engine culls the skybox. It's recommended to keep r_3dsky disabled for best results.")
    warningText:SetWrap(true)
    
    local dontShowAgain = vgui.Create("DCheckBoxLabel", frame)
    dontShowAgain:SetPos(20, 130)
    dontShowAgain:SetText("Don't show this warning again")
    dontShowAgain:SetValue(false)
    dontShowAgain.OnChange = function(self, val)
        if val then
            RunConsoleCommand("rtx_show_3dsky_warning", "0")
        else
            RunConsoleCommand("rtx_show_3dsky_warning", "1")
        end
    end
    
    local okButton = vgui.Create("DButton", frame)
    okButton:SetText("OK")
    okButton:SetPos(150, 160)
    okButton:SetSize(100, 25)
    okButton.DoClick = function()
        frame:Close()
    end
end

cvars.AddChangeCallback("r_3dsky", function(_, _, newValue)
    if newValue == "1" then
        Show3DSkyWarning()
    end
end)

-- Function to apply lightupdater state to Remix variables
-- @param processImmediately: if true, manually process/clear lights (for user toggle). If false, just set autospawn (for map load)
local function ApplyLightupdaterState(processImmediately)
    if not RemixConfig or not RemixConfig.SetConfigVariable then
        -- Remix not ready yet
        return
    end
    
    local enabled = GetConVar("rtx_lightupdater"):GetBool()
    -- When lightupdater is enabled, we want Remix to NOT ignore game lights
    -- When disabled, we want Remix to ignore them (since we'll use API lights instead)
    local ignoreValue = enabled and "False" or "True"
    
    RemixConfig.SetConfigVariable("rtx.ignoreGamePointLights", ignoreValue)
    RemixConfig.SetConfigVariable("rtx.ignoreGameSpotLights", ignoreValue)
    RemixConfig.SetConfigVariable("rtx.ignoreGameDirectionalLights", ignoreValue)
    
    if enabled then
        -- Lightupdater ON: disable API lights autospawn
        RunConsoleCommand("rtx_api_map_lights_autospawn", "0")
        if processImmediately then
            -- Only clear existing lights when user manually toggles
            RunConsoleCommand("rtx_api_map_lights_clear")
            print("[RTX Settings] Lightupdater enabled - Using D3D9 game lights, cleared API lights")
        end
    else
        -- Lightupdater OFF: enable API lights autospawn
        RunConsoleCommand("rtx_api_map_lights_autospawn", "1")
        if processImmediately then
            -- Only manually process when user toggles (autospawn will handle it on map load)
            RunConsoleCommand("rtx_api_map_lights_process")
            print("[RTX Settings] Lightupdater disabled - Using API lights, processing map lights")
        end
    end
end

cvars.AddChangeCallback("rtx_lightupdater", function(_, _, newValue)
    -- User manually changed the setting, process immediately
    ApplyLightupdaterState(true)
end)

-- Apply lightupdater state on map load to ensure Remix variables are set correctly
-- Don't process immediately - let the autospawn system handle it
hook.Add("InitPostEntity", "RTX_ApplyLightupdaterState", function()
    timer.Simple(0.5, function() -- Small delay to ensure RemixConfig is ready
        ApplyLightupdaterState(false)
    end)
end)

-- ToPBR Conversion Settings (Only show for 64-bit client)
if BRANCH == "x86-64" or BRANCH == "chromium" then
    hook.Add( "PopulateToolMenu", "RTXOptionsClient_ToPBR", function()
        spawnmenu.AddToolMenuOption( "Utilities", "RTX Remix", "RTX_Client_ToPBR", "#ToPBR Conversion", "", "", function( panel )
            panel:ClearControls()
            
            panel:Help("Automatic PBR Conversion")
            panel:ControlHelp("Converts Source Engine materials to PBR format for RTX Remix.")
            
            -- Master toggle
            local enabledCheckbox = panel:CheckBox("Enable ToPBR Conversion", "rtx_topbr_enabled")
            panel:ControlHelp("Enable automatic conversion of materials to PBR format.")
            
            -- Auto-processing toggle
            local autoCheckbox = panel:CheckBox("Auto-Process on Map Load", "rtx_topbr_auto")
            panel:ControlHelp("Automatically process materials when a map loads.")
            
            -- Delay slider
            local delaySlider = panel:NumSlider("Processing Delay (seconds)", "rtx_topbr_delay", 1, 30, 0)
            panel:ControlHelp("Delay before auto-processing starts after map load.")
            
            -- Experimental metallic generation
            local metallicCheckbox = panel:CheckBox("Experimental Metallic Detection", "rtx_topbr_metallic")
            panel:ControlHelp("Generate metallic maps from base texture brightness.")
            panel:ControlHelp("WARNING: May cause dark materials to appear completely black!")
            
            -- Auto-discover companion textures
            local autodiscoverCheckbox = panel:CheckBox("Auto-Discover Textures", "rtx_topbr_autodiscover")
            panel:ControlHelp("Search for companion textures (_normal, _mask, _spec) not in VMT.")
            
            -- Debug output toggle
            local debugCheckbox = panel:CheckBox("Debug Output", "rtx_topbr_debug")
            panel:ControlHelp("Shows debug messages in console for troubleshooting.")
            
            panel:Help("Manual Controls")
            
            -- Process button
            local processButton = panel:Button("Process Materials Now")
            processButton.DoClick = function()
                RunConsoleCommand("rtx_topbr_process")
            end
            panel:ControlHelp("Manually trigger PBR conversion for all tracked materials.")
            
            -- Stats button
            local statsButton = panel:Button("Show Statistics")
            statsButton.DoClick = function()
                RunConsoleCommand("rtx_topbr_stats")
            end
            
            -- Clear cache button
            local clearButton = panel:Button("Clear Cache")
            clearButton.DoClick = function()
                RunConsoleCommand("rtx_topbr_clear")
            end
            panel:ControlHelp("Clear the conversion cache (useful after changing settings).")
            
            -- Update controls based on enabled state
            local function UpdateControls()
                local enabled = GetConVar("rtx_topbr_enabled"):GetBool()
                if autoCheckbox then autoCheckbox:SetEnabled(enabled) end
                if delaySlider then delaySlider:SetEnabled(enabled) end
                if metallicCheckbox then metallicCheckbox:SetEnabled(enabled) end
                if autodiscoverCheckbox then autodiscoverCheckbox:SetEnabled(enabled) end
                if processButton then processButton:SetEnabled(enabled) end
                if statsButton then statsButton:SetEnabled(enabled) end
                if clearButton then clearButton:SetEnabled(enabled) end
            end
            
            -- Set initial state
            UpdateControls()
            
            -- Update when master checkbox changes
            if enabledCheckbox then
                enabledCheckbox.OnChange = function(self, value)
                    UpdateControls()
                end
            end
        end )
    end )
end

-- Auto-Categorization Settings (Only show for 64-bit client)
if BRANCH == "x86-64" or BRANCH == "chromium" then
    hook.Add( "PopulateToolMenu", "RTXOptionsClient_AutoCategorization", function()
        spawnmenu.AddToolMenuOption( "Utilities", "RTX Remix", "RTX_Client_AutoCategorization", "#Auto-Categorization", "", "", function( panel )
            panel:ClearControls()

            -- Master toggle
            local masterCheckbox = panel:CheckBox("Enable Auto-Categorization", "rtx_auto_categorize")
            panel:ControlHelp("Automatically categorizes textures for RTX Remix.")
            
            -- Delay slider
            local delaySlider = panel:NumSlider("Delay (seconds)", "rtx_auto_categorize_delay", 0, 10, 1)
            panel:ControlHelp("How long to wait after map load before scanning (allows BSP data to load).")

            -- World geometry toggle
            local worldCheckbox = panel:CheckBox("World Geometry", "rtx_auto_categorize_world")
            panel:ControlHelp("Categorizes walls, floors, and ceilings from BSP as Decals for proper blending.")
            
            -- Particles toggle
            local particlesCheckbox = panel:CheckBox("Particles", "rtx_auto_categorize_particles")
            panel:ControlHelp("Categorizes particle effects (smoke, fire, sparks, etc) as Particles.")
            
            -- Decals toggle
            local decalsCheckbox = panel:CheckBox("Overlay Decals", "rtx_auto_categorize_decals")
            panel:ControlHelp("Categorizes materials with $decal parameter (posters, graffiti, bullet holes, etc) as Decals.")
            
            -- Emissive toggle
            local emissiveCheckbox = panel:CheckBox("Legacy Emissive", "rtx_auto_categorize_emissive")
            panel:ControlHelp("Categorizes materials with $selfillum parameter as Legacy Emissive.")
            panel:ControlHelp("This can incorrectly categorized some materials due to them using the $selfillum parameter incorrectly.")
            
            panel:Help("Debug Options")
            -- Debug output toggle
            panel:CheckBox("Debug Output", "rtx_debug_categorization")
            panel:ControlHelp("Shows debug messages in console for categorization activity (useful for troubleshooting).")
            
            panel:Help("Manual Controls")
            -- Manual trigger button
            local scanButton = panel:Button("Scan Now")
            scanButton.DoClick = function()
                RunConsoleCommand("rtx_smart_mark_world", "force")
            end
            panel:ControlHelp("Manually parse BSP and categorize world textures, decals, and emissive materials.")
            
            -- Update sub-controls based on master toggle
            local function UpdateControls()
                local enabled = GetConVar("rtx_auto_categorize"):GetBool()
                if delaySlider then delaySlider:SetEnabled(enabled) end
                if worldCheckbox then worldCheckbox:SetEnabled(enabled) end
                if particlesCheckbox then particlesCheckbox:SetEnabled(enabled) end
                if decalsCheckbox then decalsCheckbox:SetEnabled(enabled) end
                if emissiveCheckbox then emissiveCheckbox:SetEnabled(enabled) end
            end
            
            -- Set initial state
            UpdateControls()
            
            -- Update when master checkbox changes
            if masterCheckbox then
                masterCheckbox.OnChange = function(self, value)
                    UpdateControls()
                end
            end
        end )
    end )
end