-- Tool Menu for RTX ProjectedTexture Wrapper
if not CLIENT then return end

hook.Add("PopulateToolMenu", "RTXProjectedTextureWrapper_Menu", function()
    spawnmenu.AddToolMenuOption("Utilities", "RTX Remix - API Lights", "RTX_ProjectedTexture_Wrapper", "Wrapper - Projected Texture", "", "", function(panel)
        panel:ClearControls()
        
        panel:Help("Automatically convert ProjectedTexture() calls into Remix API spotlights")
        panel:Help("Works with Glide base headlights, spotlight entities, and other addons that use ProjectedTexture()")

        -- Enable/Disable
        panel:CheckBox("Enable Projected Texture Wrapper", "rtx_projectedtexture_wrapper_enabled")
        
        
        local brightnessSlider = panel:NumSlider("Brightness Scale", "rtx_projectedtexture_wrapper_brightness_scale", 1, 500, 0)
        if brightnessSlider then brightnessSlider:SetDecimals(0) end
        panel:Help("Higher values = brighter lights. Default: 10")
        
        local radiusSlider = panel:NumSlider("Radius Scale", "rtx_projectedtexture_wrapper_radius_scale", 0.001, 5.000, 2.000)
        if radiusSlider then radiusSlider:SetDecimals(4) end
        
        panel:Help("Position Offsets")
        local offsetXSlider = panel:NumSlider("Offset X (Forward/Back)", "rtx_projectedtexture_wrapper_offset_x", -200, 200, 0)
        if offsetXSlider then offsetXSlider:SetDecimals(0) end
        
        local offsetYSlider = panel:NumSlider("Offset Y (Left/Right)", "rtx_projectedtexture_wrapper_offset_y", -200, 200, 0)
        if offsetYSlider then offsetYSlider:SetDecimals(0) end
        
        local offsetZSlider = panel:NumSlider("Offset Z (Up/Down)", "rtx_projectedtexture_wrapper_offset_z", -200, 200, 0)
        if offsetZSlider then offsetZSlider:SetDecimals(0) end
        
        panel:Button("Reset Offsets", "rtx_projectedtexture_reset_offsets")
        
        -- Clear button
        panel:Button("Clear All Wrapped Lights", "rtx_projectedtexture_clear")
    end)
end)
