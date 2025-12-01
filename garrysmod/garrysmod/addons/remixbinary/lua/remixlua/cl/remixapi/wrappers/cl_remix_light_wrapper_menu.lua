-- Tool Menu for RTX Light Wrapper
if not CLIENT then return end

hook.Add("PopulateToolMenu", "RTXLightWrapper_Menu", function()
    spawnmenu.AddToolMenuOption("Utilities", "RTX Remix - API Lights", "RTX_Light_Wrapper", "Wrapper - Basic", "", "", function(panel)
        panel:ClearControls()

        panel:Help("Automatically converts Garry's Mod point and spot lights into Remix API Lights")
        
        -- Enable/Disable
        panel:CheckBox("Enable Light Wrapper", "rtx_light_wrapper_enabled")
        
        local brightnessSlider = panel:NumSlider("Brightness Scale", "rtx_light_wrapper_brightness_scale", 0, 1, 3)
        if brightnessSlider then brightnessSlider:SetDecimals(3) end
        
        local radiusPowerSlider = panel:NumSlider("Radius Power", "rtx_light_wrapper_radius_power", 0.10, 2.0, 1.0)
        if radiusPowerSlider then radiusPowerSlider:SetDecimals(2) end

        local radiusPointSlider = panel:NumSlider("Point Light Radius Scale", "rtx_light_wrapper_radius_scale_point", 1.5, 5.0, 2)
        if radiusPointSlider then radiusPointSlider:SetDecimals(3) end
        
        local radiusSpotSlider = panel:NumSlider("Spotlight Radius Scale", "rtx_light_wrapper_radius_scale_spot", 0.1, 5.0, 2)
        if radiusSpotSlider then radiusSpotSlider:SetDecimals(3) end
    end)
end)
