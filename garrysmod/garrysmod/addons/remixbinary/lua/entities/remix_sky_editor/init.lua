AddCSLuaFile("shared.lua")
AddCSLuaFile("cl_init.lua")
include("shared.lua")

function ENT:Initialize()
    self:SetModel("models/maxofs2d/cube_tool.mdl")
    self:SetBodygroup(1, 1)
    self:PhysicsInit(SOLID_VPHYSICS)
    self:SetMoveType(MOVETYPE_VPHYSICS)
    self:SetSolid(SOLID_VPHYSICS)
    self:SetMaterial("gmod/edit_sun")
    self:DrawShadow(false)

    local phys = self:GetPhysicsObject()
    if IsValid(phys) then
        phys:EnableGravity(false)
        phys:Wake()
    end

    -- Sun
    self:SetNWFloat("sky_sun_intensity", 1.0)
    self:SetNWFloat("sky_sun_size", 0.545)
    self:SetNWBool("sky_sun_disc", true)
    self:SetNWFloat("sky_illum_r", 20.0)
    self:SetNWFloat("sky_illum_g", 20.0)
    self:SetNWFloat("sky_illum_b", 20.0)

    -- Density multipliers
    self:SetNWFloat("sky_air_density", 1.0)
    self:SetNWFloat("sky_dust_density", 1.0)
    self:SetNWFloat("sky_ozone_density", 1.0)

    -- Planet / observer
    self:SetNWFloat("sky_altitude", 100.0)
    self:SetNWFloat("sky_planet_radius", 6371.0)
    self:SetNWFloat("sky_atmo_thickness", 100.0)

    -- Advanced scattering
    self:SetNWFloat("sky_mie_anisotropy", 0.97)
    self:SetNWFloat("sky_rayleigh_r", 0.0058)
    self:SetNWFloat("sky_rayleigh_g", 0.0135)
    self:SetNWFloat("sky_rayleigh_b", 0.0331)
    self:SetNWFloat("sky_mie_r", 0.003996)
    self:SetNWFloat("sky_mie_g", 0.003996)
    self:SetNWFloat("sky_mie_b", 0.003996)
    self:SetNWFloat("sky_ozone_r", 0.00204)
    self:SetNWFloat("sky_ozone_g", 0.00497)
    self:SetNWFloat("sky_ozone_b", 0.000214)
    self:SetNWFloat("sky_ozone_alt", 25.0)
    self:SetNWFloat("sky_ozone_width", 15.0)
end

function ENT:SpawnFunction(ply, tr, ClassName)
    if not tr.Hit then return end
    local ent = ents.Create(ClassName or "remix_sky_editor")
    if not IsValid(ent) then return end
    ent:SetPos(tr.HitPos + tr.HitNormal * 16)
    -- Default angles: pitch -15 => 15 degree sun elevation, yaw from player
    ent:SetAngles(Angle(-15, ply:EyeAngles().y, 0))
    ent:Spawn()
    ent:Activate()
    return ent
end

function ENT:Think()
    self:NextThink(CurTime())
    return true
end

-- Duplicator support
local NW_KEYS_FLOAT = {
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
local NW_KEYS_BOOL = { "sky_sun_disc" }

local function getNWTable(ent)
    local t = {}
    for _, k in ipairs(NW_KEYS_FLOAT) do t[k] = ent:GetNWFloat(k) end
    for _, k in ipairs(NW_KEYS_BOOL) do t[k] = ent:GetNWBool(k) end
    return t
end

local function applyNWTable(ent, t)
    if not istable(t) then return end
    for _, k in ipairs(NW_KEYS_FLOAT) do
        if t[k] ~= nil then ent:SetNWFloat(k, t[k]) end
    end
    for _, k in ipairs(NW_KEYS_BOOL) do
        if t[k] ~= nil then ent:SetNWBool(k, t[k] and true or false) end
    end
end

function ENT:PreEntityCopy()
    duplicator.StoreEntityModifier(self, "RemixSkyEditorData", getNWTable(self))
end

function ENT:PostEntityPaste(ply, ent, createdEntities)
    local mod = ent.EntityMods and ent.EntityMods["RemixSkyEditorData"]
    if mod then applyNWTable(self, mod) end

    local phys = self:GetPhysicsObject()
    if IsValid(phys) then
        phys:EnableGravity(false)
    end
    self:SetBodygroup(1, 1)
    self:SetMaterial("gmod/edit_sun")
    self:DrawShadow(false)
end
