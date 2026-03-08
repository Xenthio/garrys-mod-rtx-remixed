if not SERVER then return end

-- Overrides "painted" shader-based skybox to prevent Remix from rasterizing

hook.Add("PlayerInitialSpawn", "RTX_SkyboxFix", function(ply)
	if GetConVar("sv_skyname"):GetString() == "painted" then
		RunConsoleCommand("sv_skyname", "sky_day01_01")
	end
end)
