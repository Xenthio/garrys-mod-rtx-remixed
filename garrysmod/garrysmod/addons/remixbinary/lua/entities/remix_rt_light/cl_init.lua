include("shared.lua")

local function vec_to_table(v) return { x = v.x, y = v.y, z = v.z } end

function ENT:Draw()
    self:DrawModel()
end

local function ensure_light(ent)
    if not ent.LightId and RemixLight then
        local pos = ent:GetPos() + Vector(0,0,10)
        local base = {
            hash = tonumber(util.CRC("ent_light_" .. ent:EntIndex()), 16) or 1,
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
        ent.LightId = RemixLight.CreateSphere(base, sphere, ent:EntIndex())
    end
end

function ENT:Think()
    ensure_light(self)
    local pos = self:GetNWVector("rtx_light_pos", self:GetPos())
    local col = self:GetNWVector("rtx_light_col", Vector(15,15,15))
    local radius = self:GetNWFloat("rtx_light_radius", 20)

    if RemixLight and self.LightId and RemixLight.UpdateSphere then
        local base = {
            hash = tonumber(util.CRC("ent_light_" .. self:EntIndex()), 16) or 1,
            radiance = { x = col.x, y = col.y, z = col.z },
        }
        local sphere = {
            position = vec_to_table(pos),
            radius = radius,
            shaping = {
                direction = { x = 0, y = 0, z = -1 },
                coneAngleDegrees = 90,
                coneSoftness = 0.1,
                focusExponent = 1.0,
            },
            volumetricRadianceScale = 1.0,
        }
        RemixLight.UpdateSphere(base, sphere, self.LightId)
    end
end

function ENT:OnRemove()
    if RemixLight and self.LightId then
        RemixLight.DestroyLight(self.LightId)
        self.LightId = nil
    end
end


