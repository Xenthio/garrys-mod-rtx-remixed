-- Tool Menu for RTX DynamicLight Wrapper
if not CLIENT then return end

hook.Add("PopulateToolMenu", "RTXDynamicLightWrapper_Menu", function()
    spawnmenu.AddToolMenuOption("Utilities", "RTX Remix - API Lights", "RTX_DynamicLight_Wrapper", "Wrapper - Dynamic", "", "", function(panel)
        panel:ClearControls()
        
        panel:Help("Convert DynamicLight() calls to Remix API lights")
        panel:Help("Works with SWEPs like lanterns, and other addons that use dynamic lights")
        
        -- Enable/Disable
        local enableCheckbox = panel:CheckBox("Enable Dynamic Light Wrapper", "rtx_dynamiclight_wrapper_enabled")
        
        local brightnessSlider = panel:NumSlider("Brightness Scale", "rtx_dynamiclight_wrapper_brightness_scale", 1, 200, 0)
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
