if not (BRANCH == "x86-64" or BRANCH == "chromium") then return end
include("shared.lua")

-- Only the lowest-EntIndex sky editor drives the atmosphere
local function IsAuthoritative(self)
    for _, ent in ipairs(ents.FindByClass("remix_sky_editor")) do
        if IsValid(ent) then
            return ent == self
        end
    end
    return false
end

function ENT:Initialize()
    if RemixAtmosphere and IsAuthoritative(self) then
        RemixAtmosphere.SetSkyMode(RemixAtmosphere.MODE_PHYSICAL)
    end
    self.LastUpdateCache = {}
end

function ENT:Draw()
    self:DrawModel()
end

-- Preset values for restoring NW vars after ApplyPreset
local PRESETS = {
    earth = {
        sky_illum_r = 20, sky_illum_g = 20, sky_illum_b = 20,
        sky_planet_radius = 6371, sky_atmo_thickness = 100,
        sky_rayleigh_r = 0.0058, sky_rayleigh_g = 0.0135, sky_rayleigh_b = 0.0331,
        sky_mie_r = 0.003996, sky_mie_g = 0.003996, sky_mie_b = 0.003996,
        sky_mie_anisotropy = 0.97,
        sky_ozone_r = 0.00204, sky_ozone_g = 0.00497, sky_ozone_b = 0.000214,
        sky_ozone_alt = 25, sky_ozone_width = 15,
    },
    mars = {
        sky_illum_r = 15, sky_illum_g = 12, sky_illum_b = 10,
        sky_planet_radius = 3389.5, sky_atmo_thickness = 50,
        sky_rayleigh_r = 0.008, sky_rayleigh_g = 0.01, sky_rayleigh_b = 0.012,
        sky_mie_r = 0.008, sky_mie_g = 0.008, sky_mie_b = 0.008,
        sky_mie_anisotropy = 0.7,
        sky_ozone_r = 0, sky_ozone_g = 0, sky_ozone_b = 0,
        sky_ozone_alt = 0, sky_ozone_width = 1,
    },
    clearsky = {
        sky_illum_r = 25, sky_illum_g = 25, sky_illum_b = 25,
        sky_planet_radius = 6371, sky_atmo_thickness = 80,
        sky_rayleigh_r = 0.004, sky_rayleigh_g = 0.009, sky_rayleigh_b = 0.022,
        sky_mie_r = 0.001, sky_mie_g = 0.001, sky_mie_b = 0.001,
        sky_mie_anisotropy = 0.9,
        sky_ozone_r = 0.00204, sky_ozone_g = 0.00497, sky_ozone_b = 0.000214,
        sky_ozone_alt = 25, sky_ozone_width = 15,
    },
    hazy = {
        sky_illum_r = 18, sky_illum_g = 18, sky_illum_b = 18,
        sky_planet_radius = 6371, sky_atmo_thickness = 100,
        sky_rayleigh_r = 0.0058, sky_rayleigh_g = 0.0135, sky_rayleigh_b = 0.0331,
        sky_mie_r = 0.012, sky_mie_g = 0.012, sky_mie_b = 0.012,
        sky_mie_anisotropy = 0.65,
        sky_ozone_r = 0.00204, sky_ozone_g = 0.00497, sky_ozone_b = 0.000214,
        sky_ozone_alt = 25, sky_ozone_width = 15,
    },
    alien = {
        sky_illum_r = 15, sky_illum_g = 22, sky_illum_b = 18,
        sky_planet_radius = 5000, sky_atmo_thickness = 120,
        sky_rayleigh_r = 0.004, sky_rayleigh_g = 0.018, sky_rayleigh_b = 0.01,
        sky_mie_r = 0.005, sky_mie_g = 0.005, sky_mie_b = 0.005,
        sky_mie_anisotropy = 0.75,
        sky_ozone_r = 0.001, sky_ozone_g = 0.0005, sky_ozone_b = 0.003,
        sky_ozone_alt = 30, sky_ozone_width = 20,
    },
    desert = {
        sky_illum_r = 28, sky_illum_g = 24, sky_illum_b = 18,
        sky_planet_radius = 6000, sky_atmo_thickness = 90,
        sky_rayleigh_r = 0.007, sky_rayleigh_g = 0.011, sky_rayleigh_b = 0.018,
        sky_mie_r = 0.015, sky_mie_g = 0.012, sky_mie_b = 0.008,
        sky_mie_anisotropy = 0.6,
        sky_ozone_r = 0.0005, sky_ozone_g = 0.001, sky_ozone_b = 0.0001,
        sky_ozone_alt = 20, sky_ozone_width = 10,
    },
}

local function ApplyAtmosphere(self)
    if not RemixAtmosphere then return end

    local ang = self:GetAngles()
    RemixAtmosphere.SetSunElevation(-ang.p)
    RemixAtmosphere.SetSunRotation((90 - ang.y) % 360)

    RemixAtmosphere.SetSunIntensity(self:GetNWFloat("sky_sun_intensity", 1.0))
    RemixAtmosphere.SetSunSize(self:GetNWFloat("sky_sun_size", 0.545))
    RemixAtmosphere.SetSunDisc(self:GetNWBool("sky_sun_disc", true))
    RemixAtmosphere.SetSunIlluminance(
        self:GetNWFloat("sky_illum_r", 20),
        self:GetNWFloat("sky_illum_g", 20),
        self:GetNWFloat("sky_illum_b", 20))

    RemixAtmosphere.SetAirDensity(self:GetNWFloat("sky_air_density", 1.0))
    RemixAtmosphere.SetDustDensity(self:GetNWFloat("sky_dust_density", 1.0))
    RemixAtmosphere.SetOzoneDensity(self:GetNWFloat("sky_ozone_density", 1.0))

    RemixAtmosphere.SetAltitude(self:GetNWFloat("sky_altitude", 100))
    RemixAtmosphere.SetPlanetRadius(self:GetNWFloat("sky_planet_radius", 6371))
    RemixAtmosphere.SetAtmosphereThickness(self:GetNWFloat("sky_atmo_thickness", 100))

    RemixAtmosphere.SetMieAnisotropy(self:GetNWFloat("sky_mie_anisotropy", 0.97))
    RemixAtmosphere.SetRayleighScattering(
        self:GetNWFloat("sky_rayleigh_r", 0.0058),
        self:GetNWFloat("sky_rayleigh_g", 0.0135),
        self:GetNWFloat("sky_rayleigh_b", 0.0331))
    RemixAtmosphere.SetMieScattering(
        self:GetNWFloat("sky_mie_r", 0.003996),
        self:GetNWFloat("sky_mie_g", 0.003996),
        self:GetNWFloat("sky_mie_b", 0.003996))
    RemixAtmosphere.SetOzoneAbsorption(
        self:GetNWFloat("sky_ozone_r", 0.00204),
        self:GetNWFloat("sky_ozone_g", 0.00497),
        self:GetNWFloat("sky_ozone_b", 0.000214))
    RemixAtmosphere.SetOzoneLayer(
        self:GetNWFloat("sky_ozone_alt", 25),
        self:GetNWFloat("sky_ozone_width", 15))
end

function ENT:Think()
    if not IsAuthoritative(self) then
        self:NextThink(CurTime() + 0.5)
        return true
    end

    -- Ensure physical atmosphere mode stays on
    if RemixAtmosphere then
        RemixAtmosphere.SetSkyMode(RemixAtmosphere.MODE_PHYSICAL)
    end

    -- Change detection: only push to API when something changed
    local ang = self:GetAngles()
    local cache = self.LastUpdateCache
    local needsUpdate = false

    if not cache.pitch or math.abs(cache.pitch - ang.p) > 0.05 or math.abs(cache.yaw - ang.y) > 0.05 then
        needsUpdate = true
    end

    local floatKeys = {
        "sky_sun_intensity", "sky_sun_size",
        "sky_illum_r", "sky_illum_g", "sky_illum_b",
        "sky_air_density", "sky_dust_density", "sky_ozone_density",
        "sky_altitude", "sky_planet_radius", "sky_atmo_thickness",
        "sky_mie_anisotropy",
        "sky_rayleigh_r", "sky_rayleigh_g", "sky_rayleigh_b",
        "sky_mie_r", "sky_mie_g", "sky_mie_b",
        "sky_ozone_r", "sky_ozone_g", "sky_ozone_b",
        "sky_ozone_alt", "sky_ozone_width",
    }

    if not needsUpdate then
        for _, k in ipairs(floatKeys) do
            if cache[k] ~= self:GetNWFloat(k) then
                needsUpdate = true
                break
            end
        end
    end
    if not needsUpdate and cache.sky_sun_disc ~= self:GetNWBool("sky_sun_disc", true) then
        needsUpdate = true
    end

    if needsUpdate then
        cache.pitch = ang.p
        cache.yaw = ang.y
        for _, k in ipairs(floatKeys) do cache[k] = self:GetNWFloat(k) end
        cache.sky_sun_disc = self:GetNWBool("sky_sun_disc", true)

        ApplyAtmosphere(self)
    end

    local phys = self:GetPhysicsObject()
    if IsValid(phys) and not phys:IsAsleep() then
        self:NextThink(CurTime())
    else
        self:NextThink(CurTime() + 0.05)
    end
    return true
end

function ENT:OnRemove()
    if RemixAtmosphere and IsAuthoritative(self) then
        -- Check if there's another sky editor that will take over
        for _, ent in ipairs(ents.FindByClass("remix_sky_editor")) do
            if IsValid(ent) and ent ~= self then
                return -- another will pick up
            end
        end
        RemixAtmosphere.SetSkyMode(RemixAtmosphere.MODE_SKYBOX)
    end
end

-- ==========================================================================
-- Editor panel
-- ==========================================================================

local function OpenSkyEditor(ent)
    if not IsValid(ent) then return end

    local ROW_H = 22
    local allControls = {}
    local sunDiscCheck
    local presetButtons = {}

    local frame = vgui.Create("DFrame")
    frame:SetTitle("Sky Editor")
    frame:SetSize(440, math.min(ScrH() * 0.75, 460))
    frame:SetSizable(true)
    frame:Center()
    frame:MakePopup()

    local scroll = vgui.Create("DScrollPanel", frame)
    scroll:Dock(FILL)

    -- Helpers ----------------------------------------------------------------

    local function Section(text)
        local row = vgui.Create("DPanel", scroll)
        row:Dock(TOP)
        row:SetTall(20)
        row:DockMargin(0, 6, 0, 1)
        row.Paint = function(_, w, h)
            surface.SetDrawColor(70, 70, 70)
            surface.DrawLine(8, h - 1, w - 8, h - 1)
        end
        local lbl = vgui.Create("DLabel", row)
        lbl:Dock(FILL)
        lbl:DockMargin(8, 0, 0, 0)
        lbl:SetText(text)
        lbl:SetTextColor(Color(100, 200, 255))
        lbl:SetFont("DermaDefaultBold")
    end

    local function MakeWang(parent, nwKey, min, max, dec)
        local w = vgui.Create("DNumberWang", parent)
        w:SetMin(min)
        w:SetMax(max)
        w:SetDecimals(dec)
        w:SetValue(ent:GetNWFloat(nwKey))
        w._nwKey = nwKey
        table.insert(allControls, w)
        return w
    end

    local function Slider(label, nwKey, min, max, dec)
        local s = vgui.Create("DNumSlider", scroll)
        s:Dock(TOP)
        s:DockMargin(8, 2, 8, 2)
        s:SetText(label)
        s:SetMin(min)
        s:SetMax(max)
        s:SetDecimals(dec)
        s:SetValue(ent:GetNWFloat(nwKey))
        s._nwKey = nwKey
        table.insert(allControls, s)
        return s
    end

    local function ColorRow(label, kR, kG, kB, min, max, dec)
        local range = max - min
        local function valToByte(v)
            return math.Clamp(math.floor((v - min) / range * 255 + 0.5), 0, 255)
        end

        local proxies = {}
        for i, key in ipairs({kR, kG, kB}) do
            local p = {_nwKey = key, _val = ent:GetNWFloat(key)}
            function p:GetValue() return self._val end
            function p:SetValue(v)
                self._val = v
            end
            p.OnValueChanged = function() end
            table.insert(allControls, p)
            proxies[i] = p
        end

        local row = vgui.Create("DPanel", scroll)
        row:Dock(TOP)
        row:SetTall(ROW_H)
        row:DockMargin(8, 1, 8, 1)
        row.Paint = function() end

        local lbl = vgui.Create("DLabel", row)
        lbl:Dock(LEFT)
        lbl:SetWide(130)
        lbl:SetText(label)

        local swatch = vgui.Create("DPanel", row)
        swatch:Dock(FILL)
        swatch:DockMargin(0, 1, 0, 1)
        swatch:SetCursor("hand")

        swatch.Paint = function(_, w, h)
            surface.SetDrawColor(
                valToByte(proxies[1]._val),
                valToByte(proxies[2]._val),
                valToByte(proxies[3]._val))
            surface.DrawRect(0, 0, w, h)
            surface.SetDrawColor(100, 100, 100)
            surface.DrawOutlinedRect(0, 0, w, h)
        end

        swatch.OnMousePressed = function(_, mc)
            if mc ~= MOUSE_LEFT then return end

            local popup = vgui.Create("DFrame")
            popup:SetTitle(label)
            popup:SetSize(240, 220)
            local mx, my = gui.MousePos()
            popup:SetPos(
                math.Clamp(mx, 0, ScrW() - 250),
                math.Clamp(my, 0, ScrH() - 230))
            popup:MakePopup()
            popup:SetDeleteOnClose(true)

            local mixer = vgui.Create("DColorMixer", popup)
            mixer:Dock(FILL)
            mixer:SetPalette(false)
            mixer:SetAlphaBar(false)
            mixer:SetWangs(true)
            mixer:SetColor(Color(
                valToByte(proxies[1]._val),
                valToByte(proxies[2]._val),
                valToByte(proxies[3]._val)))

            mixer.ValueChanged = function(_, col)
                proxies[1]._val = min + (col.r / 255) * range
                proxies[2]._val = min + (col.g / 255) * range
                proxies[3]._val = min + (col.b / 255) * range
                proxies[1].OnValueChanged()
            end
        end
    end

    -- Layout -----------------------------------------------------------------

    -- Presets
    Section("Presets")
    local presetNames = {"Earth", "Mars", "Clear Sky", "Hazy", "Alien", "Desert"}
    local presetKeys  = {"earth", "mars", "clearsky", "hazy", "alien", "desert"}

    local presetRow = vgui.Create("DPanel", scroll)
    presetRow:Dock(TOP)
    presetRow:DockMargin(8, 2, 8, 2)
    presetRow:SetTall(24)
    presetRow.Paint = function() end
    for i, name in ipairs(presetNames) do
        local btn = vgui.Create("DButton", presetRow)
        btn:Dock(LEFT)
        btn:DockMargin(0, 0, 3, 0)
        btn:SetWide(60)
        btn:SetText(name)
        presetButtons[i] = btn
    end

    -- Sun
    Section("Sun")
    Slider("Intensity", "sky_sun_intensity", 0, 100, 2)

    -- Density
    Section("Density")
    Slider("Air", "sky_air_density", 0, 100, 2)
    Slider("Dust", "sky_dust_density", 0, 100, 2)
    Slider("Ozone", "sky_ozone_density", 0, 100, 2)

    -- Scattering
    Section("Scattering")
    Slider("Mie Anisotropy", "sky_mie_anisotropy", 0, 1, 3)
    ColorRow("Illuminance", "sky_illum_r", "sky_illum_g", "sky_illum_b", 0, 100, 1)
    ColorRow("Rayleigh", "sky_rayleigh_r", "sky_rayleigh_g", "sky_rayleigh_b", 0, 0.1, 6)
    ColorRow("Mie", "sky_mie_r", "sky_mie_g", "sky_mie_b", 0, 0.1, 6)

    -- Ozone
    Section("Ozone")
    ColorRow("Absorption", "sky_ozone_r", "sky_ozone_g", "sky_ozone_b", 0, 0.01, 6)

    -- Realtime apply ---------------------------------------------------------

    local function sendApplyThrottled()
        if not IsValid(ent) then return end
        local id = ent:EntIndex()
        timer.Create("remix_sky_editor_apply_" .. id, 0.05, 1, function()
            if not IsValid(ent) or not net then return end
            net.Start("remix_sky_editor_apply")
            net.WriteEntity(ent)
            local t = {}
            for _, c in ipairs(allControls) do
                t[c._nwKey] = c:GetValue()
            end
            net.WriteTable(t)
            net.SendToServer()
        end)
    end

    local function applyRealtime()
        if not IsValid(ent) then return end
        for _, c in ipairs(allControls) do
            ent:SetNWFloat(c._nwKey, c:GetValue())
        end
        sendApplyThrottled()
    end

    for _, c in ipairs(allControls) do
        c.OnValueChanged = function() applyRealtime() end
    end

    for i, btn in ipairs(presetButtons) do
        btn.DoClick = function()
            local key = presetKeys[i]
            if RemixAtmosphere then
                RemixAtmosphere.ApplyPreset(key)
            end
            local p = PRESETS[key]
            if p then
                for nwKey, val in pairs(p) do
                    ent:SetNWFloat(nwKey, val)
                end
                for _, c in ipairs(allControls) do
                    if p[c._nwKey] ~= nil then
                        c:SetValue(p[c._nwKey])
                    end
                end
                sendApplyThrottled()
            end
        end
    end
end

-- Right-click context menu
properties.Add("remix_sky_editor_edit", {
    MenuLabel = "Edit Sky",
    Order = 0,
    MenuIcon = "icon16/weather_sun.png",
    Filter = function(self, ent, ply)
        return IsValid(ent) and ent:GetClass() == "remix_sky_editor"
    end,
    Action = function(self, ent)
        OpenSkyEditor(ent)
    end
})
