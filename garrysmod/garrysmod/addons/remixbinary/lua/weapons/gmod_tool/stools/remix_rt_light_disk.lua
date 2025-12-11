TOOL.Category		= "RTX Remix"
TOOL.Name			= "API Light - Disk"
TOOL.Command		= nil
TOOL.ConfigName		= ""

-- Client ConVars
TOOL.ClientConVar["radius"]			= "20"
TOOL.ClientConVar["brightness"]		= "1"
TOOL.ClientConVar["volumetric"]		= "1"
TOOL.ClientConVar["color_r"]		= "255"
TOOL.ClientConVar["color_g"]		= "220"
TOOL.ClientConVar["color_b"]		= "180"
TOOL.ClientConVar["freeze"]			= "1"
TOOL.ClientConVar["xradius"]		= "20"
TOOL.ClientConVar["yradius"]		= "20"
TOOL.ClientConVar["yaw"]			= "0"
TOOL.ClientConVar["pitch"]			= "-90"

if CLIENT then
    language.Add("tool.remix_rt_light_disk.name", "API Light - Disk")
    language.Add("tool.remix_rt_light_disk.desc", "Spawn and edit Disk/Ellipse Area Lights")
    language.Add("tool.remix_rt_light_disk.0", "Left-click: Spawn | Right-click: Update")
end

local function computeRadianceVector(r, g, b, brightness)
    local scale = math.max(0, brightness or 1)
    return Vector((r/12)*scale, (g/12)*scale, (b/12)*scale)
end

function TOOL:LeftClick(trace)
    if CLIENT then return true end
    local ply = self:GetOwner()
    if not IsValid(ply) then return false end
    if not trace.HitPos then return false end

    local pos = trace.HitPos + (trace.HitNormal or Vector(0,0,1)) * 16
    local ang = Angle(0, ply:EyeAngles().y, 0)

    local ent = ents.Create("remix_rt_light")
    if not IsValid(ent) then return false end
    ent:SetPos(pos)
    ent:SetAngles(ang)
    ent:Spawn()
    ent:Activate()

    ent:SetNWString("rtx_light_type", "disk")
    ent:SetNWFloat("rtx_light_radius", self:GetClientNumber("radius") or 20)
    ent:SetNWFloat("rtx_light_brightness", self:GetClientNumber("brightness") or 1)
    ent:SetNWFloat("rtx_light_volumetric", self:GetClientNumber("volumetric") or 1)
    ent:SetNWFloat("rtx_light_xradius", self:GetClientNumber("xradius") or 20)
    ent:SetNWFloat("rtx_light_yradius", self:GetClientNumber("yradius") or 20)

    local r = self:GetClientNumber("color_r") or 255
    local g = self:GetClientNumber("color_g") or 220
    local b = self:GetClientNumber("color_b") or 180
    ent:SetNWFloat("rtx_light_color_r", r)
    ent:SetNWFloat("rtx_light_color_g", g)
    ent:SetNWFloat("rtx_light_color_b", b)

    if self:GetClientNumber("freeze") ~= 0 then
        local phys = ent:GetPhysicsObject()
        if IsValid(phys) then phys:EnableMotion(false) end
    end

    undo.Create("Remix Disk Light")
        undo.AddEntity(ent)
        undo.SetPlayer(ply)
    undo.Finish()

    return true
end

function TOOL:RightClick(trace)
    if CLIENT then return true end
    if not IsValid(trace.Entity) then return false end
    local ent = trace.Entity
    if ent:GetClass() ~= "remix_rt_light" then return false end

    ent:SetNWString("rtx_light_type", "disk")
    ent:SetNWFloat("rtx_light_radius", self:GetClientNumber("radius") or 20)
    ent:SetNWFloat("rtx_light_brightness", self:GetClientNumber("brightness") or 1)
    ent:SetNWFloat("rtx_light_volumetric", self:GetClientNumber("volumetric") or 1)
    ent:SetNWFloat("rtx_light_xradius", self:GetClientNumber("xradius") or 20)
    ent:SetNWFloat("rtx_light_yradius", self:GetClientNumber("yradius") or 20)
    ent:SetNWFloat("rtx_light_dir_yaw", self:GetClientNumber("yaw") or 0)
    ent:SetNWFloat("rtx_light_dir_pitch", self:GetClientNumber("pitch") or -90)

    local r = self:GetClientNumber("color_r") or 255
    local g = self:GetClientNumber("color_g") or 220
    local b = self:GetClientNumber("color_b") or 180
    ent:SetNWFloat("rtx_light_color_r", r)
    ent:SetNWFloat("rtx_light_color_g", g)
    ent:SetNWFloat("rtx_light_color_b", b)

    return true
end

function TOOL:Reload(trace)
    return false
end

function TOOL:Think()
end

function TOOL.BuildCPanel(panel)
    panel:Help("Spawn and edit Disk/Ellipse Area Lights")


    panel:NumSlider("Brightness", "remix_rt_light_disk_brightness", 0, 10000, 2)
    panel:NumSlider("Volumetrics", "remix_rt_light_disk_volumetric", 0, 5, 2)
    
    panel:NumSlider("X Radius", "remix_rt_light_disk_xradius", 1, 2000, 0)
    panel:NumSlider("Y Radius", "remix_rt_light_disk_yradius", 1, 2000, 0)
    
    panel:AddControl("Color", {
        Label = "Color",
        Red = "remix_rt_light_disk_color_r",
        Green = "remix_rt_light_disk_color_g",
        Blue = "remix_rt_light_disk_color_b",
        ShowAlpha = 0,
        ShowHSV = 1,
        ShowRGB = 1,
        Multiplier = 1
    })
    
    panel:CheckBox("Freeze on Spawn", "remix_rt_light_disk_freeze")
end

