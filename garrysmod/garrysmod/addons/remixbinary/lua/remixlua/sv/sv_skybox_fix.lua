if not SERVER then return end

-- Overrides "painted" shader-based skybox to prevent Remix from rasterizing

local BLACKLISTED_MAPS = {
	["gm_construct"] = true,
	["gm_construct_rtx"] = true,
	["gm_flatgrass"] = true,
}

hook.Add("PlayerInitialSpawn", "RTX_SkyboxFix", function(ply)
	if BLACKLISTED_MAPS[game.GetMap()] then return end
	if GetConVar("sv_skyname"):GetString() == "painted" then
		RunConsoleCommand("sv_skyname", "sky_day01_01")
	end
end)
