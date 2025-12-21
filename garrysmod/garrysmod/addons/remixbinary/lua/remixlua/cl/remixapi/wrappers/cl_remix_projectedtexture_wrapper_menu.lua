-- Tool Menu for RTX ProjectedTexture Wrapper
if not CLIENT then return end

hook.Add("PopulateToolMenu", "RTXProjectedTextureWrapper_Menu", function()
    spawnmenu.AddToolMenuOption("Utilities", "RTX Remix - API Lights", "RTX_ProjectedTexture_Wrapper", "Wrapper - Projected Texture", "", "", function(panel)
        panel:ClearControls()
        
        panel:Help("Automatically convert ProjectedTexture() calls into Remix API spotlights")
        panel:Help("Works with Glide base headlights, spotlight entities, and other addons that use ProjectedTexture()")

        -- Enable/Disable
        local enableCheckbox = panel:CheckBox("Enable Projected Texture Wrapper", "rtx_projectedtexture_wrapper_enabled")
        
        local brightnessSlider = panel:NumSlider("Brightness Scale", "rtx_projectedtexture_wrapper_brightness_scale", 1, 10000, 0)
        if brightnessSlider then brightnessSlider:SetDecimals(0) end
        
        panel:Help("Position Offsets")
        local offsetXSlider = panel:NumSlider("Offset X (Forward/Back)", "rtx_projectedtexture_wrapper_offset_x", -200, 200, 0)
        if offsetXSlider then offsetXSlider:SetDecimals(0) end
        
        local offsetYSlider = panel:NumSlider("Offset Y (Left/Right)", "rtx_projectedtexture_wrapper_offset_y", -200, 200, 0)
        if offsetYSlider then offsetYSlider:SetDecimals(0) end
        
        local offsetZSlider = panel:NumSlider("Offset Z (Up/Down)", "rtx_projectedtexture_wrapper_offset_z", -200, 200, 0)
        if offsetZSlider then offsetZSlider:SetDecimals(0) end
        
        -- Update controls based on lightupdater state
        local function UpdateControls()
            local lightupdaterEnabled = GetConVar("rtx_lightupdater"):GetBool()
            local enabled = not lightupdaterEnabled
            if enableCheckbox then enableCheckbox:SetEnabled(enabled) end
            if brightnessSlider then brightnessSlider:SetEnabled(enabled) end
            if offsetXSlider then offsetXSlider:SetEnabled(enabled) end
            if offsetYSlider then offsetYSlider:SetEnabled(enabled) end
            if offsetZSlider then offsetZSlider:SetEnabled(enabled) end
        end
        
        UpdateControls()
    end)
end)
