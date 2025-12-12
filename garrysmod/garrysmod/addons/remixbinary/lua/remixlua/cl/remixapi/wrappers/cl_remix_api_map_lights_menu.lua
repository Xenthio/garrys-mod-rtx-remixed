-- Dedicated Tool Menu for Remix API Lights runtime controls
-- Loaded automatically by autorun/cl_rtx.lua which includes all files under remixlua/cl/remixapi/

if not CLIENT then return end

hook.Add("PopulateToolMenu", "RemixAPILights", function()
    spawnmenu.AddToolMenuOption("Utilities", "RTX Remix - API Lights", "RTX_Remix_API_Lights", "Map", "", "", function(panel)
        panel:ClearControls()

        -- Section: Actions
        panel:Help("Actions")
        panel:Button("Process Map Lights", "rtx_api_map_lights_process")
        panel:Button("Clear All Map Lights", "rtx_api_map_lights_clear")

        -- Section: Runtime Modifiers
        panel:Help("")
        panel:Help("Point Lights (light)")
        panel:NumSlider("Point Brightness", "rtx_api_map_lights_point_brightness_mult", 0, 10, 2)
        panel:NumSlider("Point Radius", "rtx_api_map_lights_point_radius_mult", 0, 10, 2)
        panel:NumSlider("Point Size", "rtx_api_map_lights_point_size_mult", 0, 25, 2)
        panel:NumSlider("Point Volumetrics", "rtx_api_map_lights_point_volumetric_mult", 0, 10, 2)
        local btnPointReset = panel:Button("Reset", "")
        btnPointReset.DoClick = function()
            RunConsoleCommand("rtx_api_map_lights_point_brightness_mult", "0.5")
            RunConsoleCommand("rtx_api_map_lights_point_radius_mult", "1.0")
            RunConsoleCommand("rtx_api_map_lights_point_size_mult", "1.0")
            RunConsoleCommand("rtx_api_map_lights_point_volumetric_mult", "1.0")
        end

        panel:Help("")
        panel:Help("Spot Lights (light_spot)")
        panel:NumSlider("Spot Brightness", "rtx_api_map_lights_spot_brightness_mult", 0, 10, 2)
        panel:NumSlider("Spot Radius", "rtx_api_map_lights_spot_radius_mult", 0, 10, 2)
        panel:NumSlider("Spot Size", "rtx_api_map_lights_spot_size_mult", 0, 25, 2)
        panel:NumSlider("Spot Volumetrics", "rtx_api_map_lights_spot_volumetric_mult", 0, 10, 2)
        local btnSpotReset = panel:Button("Reset", "")
        btnSpotReset.DoClick = function()
            RunConsoleCommand("rtx_api_map_lights_spot_brightness_mult", "0.5")
            RunConsoleCommand("rtx_api_map_lights_spot_radius_mult", "1.5")
            RunConsoleCommand("rtx_api_map_lights_spot_size_mult", "1.0")
            RunConsoleCommand("rtx_api_map_lights_spot_volumetric_mult", "1.0")
        end

        panel:Help("")
        panel:Help("Directional (light_environment)")
        panel:NumSlider("Brightness", "rtx_api_map_lights_env_brightness_mult", 0, 10, 2)
        panel:NumSlider("Angular Multiplier", "rtx_api_map_lights_env_angular_mult", 0, 5, 2)
        panel:NumSlider("Env Size", "rtx_api_map_lights_env_size_mult", 0, 25, 2)
        panel:NumSlider("Env Volumetrics", "rtx_api_map_lights_env_volumetric_mult", 0, 10, 2)
        local btnEnvReset = panel:Button("Reset", "")
        btnEnvReset.DoClick = function()
            RunConsoleCommand("rtx_api_map_lights_env_brightness_mult", "0.15")
            RunConsoleCommand("rtx_api_map_lights_env_angular_mult", "1.0")
            RunConsoleCommand("rtx_api_map_lights_env_size_mult", "1.0")
            RunConsoleCommand("rtx_api_map_lights_env_volumetric_mult", "1.0")
            RunConsoleCommand("rtx_api_map_lights_env_dir_flip", "0")
            RunConsoleCommand("rtx_api_map_lights_env_max_brightness", "10")
        end

        -- Section: Debug
        panel:Help("Debug")
        panel:CheckBox("Draw Debug Directions", "rtx_api_map_lights_debug_vis")
    end)
end)
