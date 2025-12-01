AddCSLuaFile("shared.lua")
AddCSLuaFile("cl_init.lua")
include("shared.lua")

function ENT:Initialize()
    print("[remix_rt_light] Initialize (server)")
    self:SetModel("models/hunter/blocks/cube025x025x025.mdl")
    self:PhysicsInit(SOLID_VPHYSICS)
    self:SetMoveType(MOVETYPE_VPHYSICS)
    self:SetSolid(SOLID_VPHYSICS)
    
    -- Hide the physics prop
    self:SetNoDraw(true)
    self:DrawShadow(false)
    
    -- Configure physics: no gravity by default, but not frozen
    local phys = self:GetPhysicsObject()
    if IsValid(phys) then
        phys:EnableGravity(false) -- Disable gravity
        phys:Wake()
    end

    self.LightId = nil
    -- Default parameters; can be overridden via net message
    self:SetNWVector("rtx_light_col", Vector(15, 15, 15))
    self:SetNWFloat("rtx_light_radius", 20)
    self:SetNWFloat("rtx_light_brightness", 1)
    self:SetNWFloat("rtx_light_volumetric", 1)
    -- Light type is authoritative on server
    self:SetNWString("rtx_light_type", "sphere")
    -- Shaping defaults
    self:SetNWBool("rtx_light_shape_enabled", false)
    self:SetNWFloat("rtx_light_shape_cone", 90)
    self:SetNWFloat("rtx_light_shape_softness", 0.1)
    self:SetNWFloat("rtx_light_shape_focus", 1.0)
    -- Direction expressed as yaw/pitch
    self:SetNWFloat("rtx_light_dir_yaw", 0)
    self:SetNWFloat("rtx_light_dir_pitch", -90)
    -- Rect/Disk sizes
    self:SetNWFloat("rtx_light_xsize", 40)
    self:SetNWFloat("rtx_light_ysize", 40)
    self:SetNWFloat("rtx_light_xradius", 20)
    self:SetNWFloat("rtx_light_yradius", 20)
    -- Cylinder axis length
    self:SetNWFloat("rtx_light_axis_len", 40)
    -- Distant angular diameter
    self:SetNWFloat("rtx_light_distant_angle", 0.5)
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
    -- Server no longer creates Remix lights; client is responsible.
    -- This function is kept as a no-op for backward compatibility.
end

function ENT:Think()
    -- Client reads position directly from entity for smooth movement
    -- Only light properties need to be networked, not position/angles
    self:NextThink(CurTime())
    return true
end

-- Duplicator support: persist NWVar configuration
local function getNWTable(ent)
    return {
        rtx_light_type = ent:GetNWString("rtx_light_type", "sphere"),
        rtx_light_radius = ent:GetNWFloat("rtx_light_radius", 20),
        rtx_light_brightness = ent:GetNWFloat("rtx_light_brightness", 1),
        rtx_light_volumetric = ent:GetNWFloat("rtx_light_volumetric", 1),
        rtx_light_shape_enabled = ent:GetNWBool("rtx_light_shape_enabled", false),
        rtx_light_shape_cone = ent:GetNWFloat("rtx_light_shape_cone", 90),
        rtx_light_shape_softness = ent:GetNWFloat("rtx_light_shape_softness", 0.1),
        rtx_light_shape_focus = ent:GetNWFloat("rtx_light_shape_focus", 1.0),
        rtx_light_dir_yaw = ent:GetNWFloat("rtx_light_dir_yaw", 0),
        rtx_light_dir_pitch = ent:GetNWFloat("rtx_light_dir_pitch", -90),
        rtx_light_xsize = ent:GetNWFloat("rtx_light_xsize", 40),
        rtx_light_ysize = ent:GetNWFloat("rtx_light_ysize", 40),
        rtx_light_xradius = ent:GetNWFloat("rtx_light_xradius", 20),
        rtx_light_yradius = ent:GetNWFloat("rtx_light_yradius", 20),
        rtx_light_axis_len = ent:GetNWFloat("rtx_light_axis_len", 40),
        rtx_light_distant_angle = ent:GetNWFloat("rtx_light_distant_angle", 0.5),
        rtx_light_col = ent:GetNWVector("rtx_light_col", Vector(15, 15, 15)),
    }
end

local function applyNWTable(ent, t)
    if not istable(t) then return end
    if t.rtx_light_type then ent:SetNWString("rtx_light_type", t.rtx_light_type) end
    if t.rtx_light_radius then ent:SetNWFloat("rtx_light_radius", t.rtx_light_radius) end
    if t.rtx_light_brightness then ent:SetNWFloat("rtx_light_brightness", t.rtx_light_brightness) end
    if t.rtx_light_volumetric then ent:SetNWFloat("rtx_light_volumetric", t.rtx_light_volumetric) end
    if t.rtx_light_shape_enabled ~= nil then ent:SetNWBool("rtx_light_shape_enabled", t.rtx_light_shape_enabled and true or false) end
    if t.rtx_light_shape_cone then ent:SetNWFloat("rtx_light_shape_cone", t.rtx_light_shape_cone) end
    if t.rtx_light_shape_softness then ent:SetNWFloat("rtx_light_shape_softness", t.rtx_light_shape_softness) end
    if t.rtx_light_shape_focus then ent:SetNWFloat("rtx_light_shape_focus", t.rtx_light_shape_focus) end
    if t.rtx_light_dir_yaw then ent:SetNWFloat("rtx_light_dir_yaw", t.rtx_light_dir_yaw) end
    if t.rtx_light_dir_pitch then ent:SetNWFloat("rtx_light_dir_pitch", t.rtx_light_dir_pitch) end
    if t.rtx_light_xsize then ent:SetNWFloat("rtx_light_xsize", t.rtx_light_xsize) end
    if t.rtx_light_ysize then ent:SetNWFloat("rtx_light_ysize", t.rtx_light_ysize) end
    if t.rtx_light_xradius then ent:SetNWFloat("rtx_light_xradius", t.rtx_light_xradius) end
    if t.rtx_light_yradius then ent:SetNWFloat("rtx_light_yradius", t.rtx_light_yradius) end
    if t.rtx_light_axis_len then ent:SetNWFloat("rtx_light_axis_len", t.rtx_light_axis_len) end
    if t.rtx_light_distant_angle then ent:SetNWFloat("rtx_light_distant_angle", t.rtx_light_distant_angle) end
    if t.rtx_light_col then ent:SetNWVector("rtx_light_col", t.rtx_light_col) end
end

function ENT:PreEntityCopy()
    local t = getNWTable(self)
    duplicator.StoreEntityModifier(self, "RemixRTLightData", t)
end

function ENT:PostEntityPaste(ply, ent, createdEntities)
    local mod = ent.EntityMods and ent.EntityMods["RemixRTLightData"]
    if mod then
        applyNWTable(self, mod)
    end
    
    -- Ensure pasted entities also have physics configured correctly
    local phys = self:GetPhysicsObject()
    if IsValid(phys) then
        phys:EnableGravity(false)
    end
    self:SetNoDraw(true)
    self:DrawShadow(false)
end


