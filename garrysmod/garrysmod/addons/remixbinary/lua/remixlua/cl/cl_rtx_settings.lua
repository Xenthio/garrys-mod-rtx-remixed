if not CLIENT then return end

-- ConVars
local CONVARS = {
    SHOW_3DSKY_WARNING = CreateClientConVar("rtx_show_3dsky_warning", "1", true, false, "Show warning when enabling r_3dsky")
}

hook.Add( "PopulateToolMenu", "RTXOptionsClient_BaseOptions", function()
    spawnmenu.AddToolMenuOption( "Utilities", "RTX Remix", "RTX_Client_BaseOptions", "#Base Options", "", "", function( panel )
        panel:ClearControls()

        panel:CheckBox( "Pseudoplayer Enabled", "rtx_pseudoplayer" )
        panel:ControlHelp( "Pseudoplayer allows you to see your own playermodel, this when marked as a 'Playermodel Texture' in remix allows you to see your own shadow and reflection." )
        panel:CheckBox( "Pseudoweapon Enabled", "rtx_pseudoweapon" )
        panel:ControlHelp( "Similar to above, but for the weapon you're holding." )

        panel:Help("Map Fixes")
        panel:ControlHelp("Fixes broken geometry rendering for current map, can lower FPS.")
        panel:Button("Enable Map Fixes", "rtx_mf_enable_current_map")
        panel:Button("Disable Map Fixes", "rtx_mf_disable_current_map")
    end )
end )

hook.Add( "PopulateToolMenu", "RTXOptionsClient_Culling", function()
    spawnmenu.AddToolMenuOption( "Utilities", "RTX Remix", "RTX_Client_Culling", "#Culling", "", "", function( panel )
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