AddCSLuaFile("shared.lua")
AddCSLuaFile("cl_init.lua")
include("shared.lua")

function ENT:Initialize()
    print("[remix_rt_light] Initialize (server)")
    self:SetModel("models/hunter/blocks/cube025x025x025.mdl")
    self:PhysicsInit(SOLID_VPHYSICS)
    self:SetMoveType(MOVETYPE_VPHYSICS)
    self:SetSolid(SOLID_VPHYSICS)
    local phys = self:GetPhysicsObject()
    if IsValid(phys) then phys:Wake() end

    self.LightId = nil
    self.NextUpdate = 0
end

function ENT:SpawnFunction(ply, tr, ClassName)
    if not tr.Hit then return end
    local classname = ClassName or "remix_rt_light"
    local ent = ents.Create(classname)
    if not IsValid(ent) then return end
    ent:SetPos(tr.HitPos + tr.HitNormal * 16)
    ent:SetAngles(Angle(0, ply:EyeAngles().y, 0))
    ent:Spawn()
    ent:Activate()
    return ent
end

local function vec_to_table(v) return { x = v.x, y = v.y, z = v.z } end

function ENT:CreateRemixLight()
    if not RemixLight then return end
    local pos = self:GetPos() + Vector(0,0,10)
    local base = {
        hash = tonumber(util.CRC("ent_light_" .. self:EntIndex()), 16) or 1,
        radiance = { x = 15, y = 15, z = 15 },
    }
    local sphere = {
        position = vec_to_table(pos),
        radius = 20,
        shaping = {
            direction = { x = 0, y = 0, z = -1 },
            coneAngleDegrees = 90,
            coneSoftness = 0.1,
            focusExponent = 1.0,
        },
        volumetricRadianceScale = 1.0,
    }
    self.LightId = RemixLight.CreateSphere(base, sphere, self:EntIndex())
end

function ENT:Think()
    if not self.LightId then
        self:CreateRemixLight()
        self.NextUpdate = CurTime() + 0.1
        self:NextThink(CurTime())
        return true
    end

    if CurTime() >= self.NextUpdate then
        -- Server only updates pose. Client will issue the Update via net message
        self:SetNWVector("rtx_light_pos", self:GetPos())
        self:SetNWVector("rtx_light_col", Vector(15, 15, 15))
        self:SetNWFloat("rtx_light_radius", 20)
        self.NextUpdate = CurTime() + 0.1
    end

    self:NextThink(CurTime())
    return true
end


