if not CLIENT then return end

-- ConVars
local cv_enabled = CreateClientConVar("rtx_pseudoplayer", 0, true, false)
local cv_pseudoweapon = CreateClientConVar("rtx_pseudoweapon", 0, true, false)
local cv_disablevertexlighting = CreateClientConVar("rtx_disablevertexlighting", 1, true, false)
local cv_disablevertexlighting_old = CreateClientConVar("rtx_disablevertexlighting_old", 0, true, false)
local cv_fixmaterials = CreateClientConVar("rtx_fixmaterials", 1, true, false)
local cv_lightupdater = CreateClientConVar("rtx_lightupdater", 1, true, false)
local cv_experimental_manuallight = CreateClientConVar("rtx_experimental_manuallight", 0, true, false)
local cv_experimental_mightcrash_combinedlightingmode = CreateClientConVar("rtx_experimental_mightcrash_combinedlightingmode", 0, false, false)
local cv_disable_when_unsupported = CreateClientConVar("rtx_disable_when_unsupported", 1, false, false)

-- Light system cache
local lastLightUpdate = 0
local LIGHT_UPDATE_INTERVAL = 1.0

-- Initialize NikNaks
require("niknaks")

-- Utility function to concatenate tables
local function TableConcat(t1, t2)
    for i = 1, #t2 do
        t1[#t1 + 1] = t2[i]
    end
    return t1
end

-- Light management
local function DoCustomLights()
    render.ResetModelLighting(0, 0, 0)

    -- Update light cache periodically
    local currentTime = RealTime()
    if currentTime - lastLightUpdate > LIGHT_UPDATE_INTERVAL then
        -- Get all lights
        local lights = NikNaks.CurrentMap:FindByClass("light")
        TableConcat(lights, NikNaks.CurrentMap:FindByClass("light_spot"))
        TableConcat(lights, NikNaks.CurrentMap:FindByClass("light_environment"))
        
        -- Update the C++ light cache
        EntityManager.UpdateLightCache(lights)
        lastLightUpdate = currentTime
    end

    -- Get 4 random lights from our cached collection
    local randomLights = EntityManager.GetRandomLights(4)
    render.SetLocalModelLights(randomLights)
end

-- Material management
local bannedmaterials = {
    "materials/particle/warp3_warp_noz.vmt",
    "materials/particle/warp4_warp.vmt",
    "materials/particle/warp4_warp_noz.vmt",
    "materials/particle/warp5_warp.vmt",
    "materials/particle/warp5_explosion.vmt",
    "materials/particle/warp_ripple.vmt"
}

local function FixupMaterial(filepath)
    -- Skip banned materials
    for _, v in pairs(bannedmaterials) do
        if v == filepath then return end
    end

    local mattrim = (filepath:sub(0, #"materials/") == "materials/") and filepath:sub(#"materials/" + 1) or filepath
    local matname = mattrim:gsub(".vmt" .. "$", "")
    local mat = Material(matname)

    if mat:IsError() then
        print("[RTX Fixes] - This texture loaded as an error? Trying to fix anyways but this shouldn't happen.")
    end

    if mat:GetString("$addself") ~= nil then
        mat:SetInt("$additive", 1)
    end

    if mat:GetString("$basetexture") == nil then
        local blankmat = Material("debug/particleerror")
        mat:SetTexture("$basetexture", blankmat:GetTexture("$basetexture"))
    end
end

local function MaterialFixupInDir(dir)
    print("[RTX Fixes] - Starting root material fixup in " .. dir)
    local _, allfolders = file.Find(dir .. "*", "GAME")
    
    -- Fix materials in root directory
    local allfiles, _ = file.Find(dir .. "*.vmt", "GAME")
    for _, v in pairs(allfiles) do
        FixupMaterial(dir .. v)
    end

    -- Fix materials in subdirectories
    for _, folder in pairs(allfolders) do
        local subfiles, _ = file.Find(dir .. folder .. "/*.vmt", "GAME")
        for _, v in pairs(subfiles) do
            FixupMaterial(dir .. folder .. "/" .. v)
        end
    end
end

local function MaterialFixups()
    MaterialFixupInDir("materials/particle/")
    MaterialFixupInDir("materials/effects/")

    -- Fix GUI materials
    local function FixupGUIMaterial(mat)
        local blankmat = Material("rtx/guiwhite")
        mat:SetTexture("$basetexture", blankmat:GetTexture("$basetexture"))
    end

    local guiMaterials = {
        Material("vgui/white"),
        Material("vgui/white_additive"),
        Material("vgui/black"),
        Material("white"),
        Material("VGUI_White"),
        Material("!VGUI_White"),
        Material("!white")
    }

    for _, mat in ipairs(guiMaterials) do
        FixupGUIMaterial(mat)
    end
end

-- Entity management
local function DrawFix(self, flags)
    if cv_experimental_manuallight:GetBool() then return end
    render.SuppressEngineLighting(cv_disablevertexlighting:GetBool())

    -- Handle material overrides
    if self:GetMaterial() ~= "" then
        render.MaterialOverride(Material(self:GetMaterial()))
    end

    -- Handle submaterials
    for k, _ in pairs(self:GetMaterials()) do
        if self:GetSubMaterial(k - 1) ~= "" then
            render.MaterialOverrideByIndex(k - 1, Material(self:GetSubMaterial(k - 1)))
        end
    end

    -- Draw the model with static lighting
    self:DrawModel(flags + STUDIO_STATIC_LIGHTING)
    render.MaterialOverride(nil)
    render.SuppressEngineLighting(false)
end

local function FixupEntity(ent)
    if IsValid(ent) and ent:GetClass() ~= "procedural_shard" then
        ent.RenderOverride = DrawFix
    end
end

local function FixupEntities()
    for _, ent in pairs(ents.GetAll()) do
        FixupEntity(ent)
    end
end

-- RTX initialization
local function RTXLoad()
    print("[RTX Fixes] - Initializing Client")

    if render.SupportsVertexShaders_2_0() and cv_disable_when_unsupported:GetBool() then
        print("[RTX Fixes] - No RTX Remix Detected! Disabling! (You can force enable by changing rtx_disable_when_unsupported to 0 and reloading)")
        return
    end

    -- Set up console commands
    RunConsoleCommand("r_radiosity", "0")
    RunConsoleCommand("r_PhysPropStaticLighting", "1")
    RunConsoleCommand("r_colorstaticprops", "0")
    RunConsoleCommand("r_lightinterp", "0")
    RunConsoleCommand("mat_fullbright", cv_experimental_manuallight:GetBool() and "1" or "0")

    -- Create entities
    local pseudoply = ents.CreateClientside("rtx_pseudoplayer")
    pseudoply:Spawn()

    local flashlightent = ents.CreateClientside("rtx_flashlight_ent")
    flashlightent:SetOwner(LocalPlayer())
    flashlightent:Spawn()

    if cv_lightupdater:GetBool() then
        local lightManager = ents.CreateClientside("rtx_lightupdatermanager")
        lightManager:Spawn()
    end

    -- Initialize systems
    FixupEntities()
    halo.Add = function() end

    if cv_fixmaterials:GetBool() then
        MaterialFixups()
    end
end

-- Render hooks
local function PreRender()
    if render.SupportsVertexShaders_2_0() then return end

    render.SuppressEngineLighting(
        cv_disablevertexlighting_old:GetBool() or 
        cv_experimental_manuallight:GetBool()
    )

    if cv_experimental_mightcrash_combinedlightingmode:GetBool() then
        render.SuppressEngineLighting(false)
    end

    if cv_experimental_manuallight:GetBool() then
        DoCustomLights()
    end
end

-- Register hooks
hook.Add("InitPostEntity", "RTXReady", RTXLoad)
hook.Add("PreRender", "RTXPreRender", PreRender)
hook.Add("PreDrawOpaqueRenderables", "RTXPreRenderOpaque", PreRender)
hook.Add("PreDrawTranslucentRenderables", "RTXPreRenderTranslucent", PreRender)
hook.Add("OnEntityCreated", "RTXEntityFixups", FixupEntity)

-- Console commands
concommand.Add("rtx_fixnow", RTXLoad)
concommand.Add("rtx_fixmaterials_fixnow", MaterialFixups)
concommand.Add("rtx_force_no_fullbright", function()
    render.SetLightingMode(0)
end)

-- Settings UI
hook.Add("PopulateToolMenu", "RTXOptionsClient", function()
    spawnmenu.AddToolMenuOption("Utilities", "User", "RTX_Client", "#RTX", "", "", function(panel)
        panel:ClearControls()

        panel:ControlHelp("Thanks for using RTX Remix Fixes! In order to allow me to continue to fix and support this addon while keeping it free, it would be nice if you could PLEASE consider donating to my patreon!")
        panel:ControlHelp("https://www.patreon.com/xenthio")

        panel:CheckBox("Fix Materials on Load.", "rtx_fixmaterials")
        panel:ControlHelp("Fixup broken and unsupported materials, this fixes things from UI to particles.")

        panel:CheckBox("Pseudoplayer Enabled", "rtx_pseudoplayer")
        panel:ControlHelp("Pseudoplayer allows you to see your own playermodel, this when marked as a 'Playermodel Texture' in remix allows you to see your own shadow and reflection.")

        panel:CheckBox("Pseudoweapon Enabled", "rtx_pseudoweapon")
        panel:ControlHelp("Similar to above, but for the weapon you're holding.")

        panel:CheckBox("Disable Vertex Lighting.", "rtx_disablevertexlighting")
        panel:ControlHelp("Disables vertex lighting on models and props, these look incorrect with rtx and aren't needed when lightupdaters are enabled.")

        panel:CheckBox("Light Updaters", "rtx_lightupdater")
        panel:ControlHelp("Prevent lights from disappearing in remix, works well when high and 'Supress Light Keeping' in remix settings is on.")

        local qualityslider = panel:NumSlider("Light Updater Count", "rtx_lightupdater_count", 0, 2048, 0)
        panel:ControlHelp("The amount of light updaters to use, this can be as low as 8 when 'Supress Light Keeping' is off.")
        panel:ControlHelp("Requires map reload to take affect!")
    end)
end)