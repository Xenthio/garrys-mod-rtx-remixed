-- Make the combined first-person models available through the server model
-- precache table. RTXLauncher mounts these paths directly from Half-Life 2 RTX.
local mountMarker = ".rtxlauncher-hl2rtx-overlay.json"

if not file.Exists(mountMarker, "GAME") then
    return
end

local combinedViewModels = {
    "models/weapons/v_357.mdl",
    "models/weapons/v_bugbait.mdl",
    "models/weapons/v_crossbow.mdl",
    "models/weapons/v_crowbar.mdl",
    "models/weapons/v_grenade.mdl",
    "models/weapons/v_irifle.mdl",
    "models/weapons/v_physcannon.mdl",
    "models/weapons/v_pistol.mdl",
    "models/weapons/v_rpg.mdl",
    "models/weapons/v_shotgun.mdl",
    "models/weapons/v_smg1.mdl",
    "models/weapons/v_stunbaton.mdl",
    "models/weapons/v_superphyscannon.mdl"
}

for _, viewModel in ipairs(combinedViewModels) do
    util.PrecacheModel(viewModel)
end
