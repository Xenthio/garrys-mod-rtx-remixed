-- Tool Menu for RTX Light Wrapper
if not CLIENT then return end

hook.Add("PopulateToolMenu", "RTXLightWrapper_Menu", function()
    spawnmenu.AddToolMenuOption("Utilities", "RTX Remix - API Lights", "RTX_Light_Wrapper", "Wrapper - Basic", "", "", function(panel)
        panel:ClearControls()

        panel:Help("Automatically converts Garry's Mod point and spot lights into Remix API Lights")
        
        -- Enable/Disable
        local enableCheckbox = panel:CheckBox("Enable Light Wrapper", "rtx_light_wrapper_enabled")
        
        local brightnessSlider = panel:NumSlider("Brightness Scale", "rtx_light_wrapper_brightness_scale", 0, 500, 0)
        if brightnessSlider then brightnessSlider:SetDecimals(0) end
        
        -- Update controls based on lightupdater state
        local function UpdateControls()
            local lightupdaterEnabled = GetConVar("rtx_lightupdater"):GetBool()
            local enabled = not lightupdaterEnabled
            if enableCheckbox then enableCheckbox:SetEnabled(enabled) end
            if brightnessSlider then brightnessSlider:SetEnabled(enabled) end
        end
        
        UpdateControls()
    end)
end)
