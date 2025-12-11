TOOL.Category = "RTX Remix"
TOOL.Name = "#tool.remix_rt_light_bonemerge.name"
TOOL.Command = nil
TOOL.ConfigName = ""

-- Add language strings
if CLIENT then
    language.Add("tool.remix_rt_light_bonemerge.name", "API Light - Bonemerged")
    language.Add("tool.remix_rt_light_bonemerge.desc", "Attach RTX lights to entity bones")
    language.Add("tool.remix_rt_light_bonemerge.0", "Left-click to attach light, Right-click entity to grab bones, Reload to detach")
end

-- Client ConVars for bonemerge settings
TOOL.ClientConVar["light_type"] = "sphere"
TOOL.ClientConVar["bone_id"] = "0"
TOOL.ClientConVar["offset_x"] = "0"
TOOL.ClientConVar["offset_y"] = "0"
TOOL.ClientConVar["offset_z"] = "0"
TOOL.ClientConVar["angle_p"] = "0"
TOOL.ClientConVar["angle_y"] = "0"
TOOL.ClientConVar["angle_r"] = "0"

-- Standard light properties
TOOL.ClientConVar["radius"] = "20"
TOOL.ClientConVar["brightness"] = "1"
TOOL.ClientConVar["volumetric"] = "1"
TOOL.ClientConVar["color_r"] = "255"
TOOL.ClientConVar["color_g"] = "220"
TOOL.ClientConVar["color_b"] = "180"

-- Shaping parameters
TOOL.ClientConVar["shape_enabled"] = "0"
TOOL.ClientConVar["cone"] = "90"
TOOL.ClientConVar["softness"] = "0.1"
TOOL.ClientConVar["focus"] = "1.0"

-- Shape-specific parameters
TOOL.ClientConVar["xsize"] = "40"
TOOL.ClientConVar["ysize"] = "40"
TOOL.ClientConVar["xradius"] = "20"
TOOL.ClientConVar["yradius"] = "20"
TOOL.ClientConVar["axislen"] = "40"
TOOL.ClientConVar["distantang"] = "0.5"

-- Left Click: Spawn bonemerged light on target entity
function TOOL:LeftClick(trace)
    if CLIENT then return true end
    if not IsValid(trace.Entity) then return false end
    
    local parent = trace.Entity
    local ply = self:GetOwner()
    
    -- Don't attach to other lights
    if parent:GetClass() == "remix_rt_light" then return false end
    
    -- Get bone from hit position (closest bone)
    local boneID = self:GetClientNumber("bone_id") or 0
    if boneID < 0 then boneID = 0 end
    
    -- If entity has bones, ensure bone ID is valid
    if parent:GetBoneCount() > 0 then
        if boneID >= parent:GetBoneCount() then
            boneID = 0
        end
    else
        boneID = -1 -- No bones, attach to entity origin
    end
    
    -- Calculate spawn position based on bone or hit position
    local spawnPos = trace.HitPos
    if boneID >= 0 and parent:GetBoneCount() > 0 then
        local boneMatrix = parent:GetBoneMatrix(boneID)
        if boneMatrix then
            spawnPos = boneMatrix:GetTranslation()
        end
    end
    
    -- Create the light entity
    local ent = ents.Create("remix_rt_light")
    if not IsValid(ent) then return false end
    
    ent:SetPos(spawnPos)
    ent:SetAngles(parent:GetAngles())
    ent:Spawn()
    ent:Activate()
    
    -- Set light type and properties
    local lightType = self:GetClientInfo("light_type") or "sphere"
    ent:SetNWString("rtx_light_type", lightType)
    ent:SetNWFloat("rtx_light_radius", self:GetClientNumber("radius") or 20)
    ent:SetNWFloat("rtx_light_brightness", self:GetClientNumber("brightness") or 1)
    ent:SetNWFloat("rtx_light_volumetric", self:GetClientNumber("volumetric") or 1)
    
    -- Set color
    local r = self:GetClientNumber("color_r") or 255
    local g = self:GetClientNumber("color_g") or 220
    local b = self:GetClientNumber("color_b") or 180
    ent:SetNWFloat("rtx_light_color_r", r)
    ent:SetNWFloat("rtx_light_color_g", g)
    ent:SetNWFloat("rtx_light_color_b", b)
    
    -- Set shaping parameters
    ent:SetNWBool("rtx_light_shape_enabled", (self:GetClientNumber("shape_enabled") or 0) ~= 0)
    ent:SetNWFloat("rtx_light_shape_cone", self:GetClientNumber("cone") or 90)
    ent:SetNWFloat("rtx_light_shape_softness", self:GetClientNumber("softness") or 0.1)
    ent:SetNWFloat("rtx_light_shape_focus", self:GetClientNumber("focus") or 1.0)
    
    -- Set shape-specific parameters
    ent:SetNWFloat("rtx_light_xsize", self:GetClientNumber("xsize") or 40)
    ent:SetNWFloat("rtx_light_ysize", self:GetClientNumber("ysize") or 40)
    ent:SetNWFloat("rtx_light_xradius", self:GetClientNumber("xradius") or 20)
    ent:SetNWFloat("rtx_light_yradius", self:GetClientNumber("yradius") or 20)
    ent:SetNWFloat("rtx_light_axis_len", self:GetClientNumber("axislen") or 40)
    ent:SetNWFloat("rtx_light_distant_angle", self:GetClientNumber("distantang") or 0.5)
    
    -- Set bonemerge parameters
    ent:SetNWBool("rtx_light_is_bonemerged", true)
    ent:SetNWInt("rtx_light_parent_id", parent:EntIndex())
    ent:SetNWInt("rtx_light_bone_id", boneID)
    
    local offsetX = self:GetClientNumber("offset_x") or 0
    local offsetY = self:GetClientNumber("offset_y") or 0
    local offsetZ = self:GetClientNumber("offset_z") or 0
    ent:SetNWVector("rtx_light_offset_pos", Vector(offsetX, offsetY, offsetZ))
    
    local angleP = self:GetClientNumber("angle_p") or 0
    local angleY = self:GetClientNumber("angle_y") or 0
    local angleR = self:GetClientNumber("angle_r") or 0
    ent:SetNWAngle("rtx_light_offset_ang", Angle(angleP, angleY, angleR))
    
    -- Disable physics (bonemerged lights don't need physics)
    local phys = ent:GetPhysicsObject()
    if IsValid(phys) then
        phys:EnableMotion(false)
    end
    
    undo.Create("Remix Bonemerged Light")
        undo.AddEntity(ent)
        undo.SetPlayer(ply)
    undo.Finish()
    
    return true
end

-- Network string for sending bone list
if SERVER then
    util.AddNetworkString("remix_rt_light_bonemerge_bones")
end

-- Right Click: Grab bone names from target entity
function TOOL:RightClick(trace)
    if not IsValid(trace.Entity) then return false end
    local ent = trace.Entity
    
    -- Don't grab bones from lights
    if ent:GetClass() == "remix_rt_light" then return false end
    
    if SERVER then
        local ply = self:GetOwner()
        if not IsValid(ply) then return false end
        
        local boneCount = ent:GetBoneCount()
        
        -- Send bone list to client
        net.Start("remix_rt_light_bonemerge_bones")
        net.WriteUInt(boneCount, 16)
        
        if boneCount > 0 then
            for i = 0, boneCount - 1 do
                local boneName = ent:GetBoneName(i) or "Bone_" .. i
                net.WriteString(boneName)
            end
        end
        
        net.Send(ply)
        return true
    end
    
    return true
end

-- Reload: Detach bonemerged light (convert to static)
function TOOL:Reload(trace)
    if CLIENT then return true end
    if not IsValid(trace.Entity) then return false end
    local ent = trace.Entity
    if ent:GetClass() ~= "remix_rt_light" then return false end
    if not ent:GetNWBool("rtx_light_is_bonemerged", false) then return false end
    
    -- Convert to static light
    ent:SetNWBool("rtx_light_is_bonemerged", false)
    ent:SetNWInt("rtx_light_parent_id", -1)
    ent:SetNWInt("rtx_light_bone_id", -1)
    
    -- Enable physics for the now-static light
    local phys = ent:GetPhysicsObject()
    if IsValid(phys) then
        phys:EnableMotion(true)
        phys:Wake()
    end
    
    return true
end

function TOOL:Think()
end

-- Client-side bone list storage
if CLIENT then
    TOOL.BoneList = TOOL.BoneList or {}
    
    -- Receive bone list from server
    net.Receive("remix_rt_light_bonemerge_bones", function()
        local boneCount = net.ReadUInt(16)
        local tool = LocalPlayer():GetTool("remix_rt_light_bonemerge")
        if not tool then return end
        
        tool.BoneList = {}
        
        if boneCount > 0 then
            for i = 0, boneCount - 1 do
                local boneName = net.ReadString()
                table.insert(tool.BoneList, {id = i, name = boneName})
            end
            
            -- Notify user
            chat.AddText(Color(100, 255, 100), "[Bonemerge] ", Color(255, 255, 255), string.format("Grabbed %d bones. Check tool menu to select.", boneCount))
        else
            chat.AddText(Color(255, 200, 100), "[Bonemerge] ", Color(255, 255, 255), "Entity has no bones (will attach to origin)")
        end
        
        -- Refresh the tool panel
        if tool.BoneListPanel and IsValid(tool.BoneListPanel) then
            tool.BoneListPanel:Clear()
            tool:PopulateBoneList(tool.BoneListPanel)
        end
    end)
end

-- Draw HUD to show prompt and info when aiming at entity
if CLIENT then
    function TOOL:DrawHUD()
        local trace = LocalPlayer():GetEyeTrace()
        if not IsValid(trace.Entity) then return end
        local ent = trace.Entity
        
        -- Don't show for lights
        if ent:GetClass() == "remix_rt_light" then return end
        
        local screenPos = ent:GetPos():ToScreen()
        if not screenPos.visible then return end
        
        local boneCount = ent:GetBoneCount()
        
        -- Show right-click prompt
        draw.SimpleText("Right-Click to grab bones", "DermaDefaultBold", screenPos.x, screenPos.y - 30, Color(255, 255, 100, 255), TEXT_ALIGN_CENTER, TEXT_ALIGN_BOTTOM)
        
        if boneCount == 0 then
            -- No bones, show entity info
            draw.SimpleText("Entity: " .. ent:GetClass(), "DermaDefault", screenPos.x, screenPos.y - 15, Color(255, 255, 255, 255), TEXT_ALIGN_CENTER, TEXT_ALIGN_BOTTOM)
            draw.SimpleText("(No bones - will attach to origin)", "DermaDefault", screenPos.x, screenPos.y, Color(200, 200, 200, 200), TEXT_ALIGN_CENTER, TEXT_ALIGN_CENTER)
        else
            -- Has bones
            draw.SimpleText(string.format("Entity: %s (%d bones)", ent:GetClass(), boneCount), "DermaDefault", screenPos.x, screenPos.y - 15, Color(255, 255, 255, 255), TEXT_ALIGN_CENTER, TEXT_ALIGN_BOTTOM)
            
            -- Show selected bone preview if valid
            local selectedBoneID = self:GetClientNumber("bone_id") or 0
            if selectedBoneID < boneCount then
                local boneName = ent:GetBoneName(selectedBoneID) or "unknown"
                local boneMatrix = ent:GetBoneMatrix(selectedBoneID)
                
                if boneMatrix then
                    local bonePos = boneMatrix:GetTranslation()
                    local boneScreen = bonePos:ToScreen()
                    
                    if boneScreen.visible then
                        -- Draw small crosshair at selected bone
                        local size = 6
                        surface.SetDrawColor(100, 255, 100, 150)
                        surface.DrawLine(boneScreen.x - size, boneScreen.y, boneScreen.x + size, boneScreen.y)
                        surface.DrawLine(boneScreen.x, boneScreen.y - size, boneScreen.x, boneScreen.y + size)
                        
                        -- Show selected bone name
                        draw.SimpleText(string.format("Selected: %s (#%d)", boneName, selectedBoneID), "DermaDefault", screenPos.x, screenPos.y, Color(100, 255, 100, 200), TEXT_ALIGN_CENTER, TEXT_ALIGN_CENTER)
                    end
                end
            end
        end
        
        -- Show attached lights count
        local attachedLights = 0
        for _, light in ipairs(ents.FindByClass("remix_rt_light")) do
            if light:GetNWBool("rtx_light_is_bonemerged", false) and light:GetNWInt("rtx_light_parent_id", -1) == ent:EntIndex() then
                attachedLights = attachedLights + 1
            end
        end
        
        if attachedLights > 0 then
            draw.SimpleText(string.format("🔗 %d light(s) attached", attachedLights), "DermaDefault", screenPos.x, screenPos.y + 20, Color(100, 255, 100, 255), TEXT_ALIGN_CENTER, TEXT_ALIGN_TOP)
        end
    end
end

-- Build the tool panel
function TOOL.BuildCPanel(panel)
    -- Light type selection
    local typeCombo = panel:ComboBox("Light Type", "remix_rt_light_bonemerge_light_type")
    typeCombo:AddChoice("Sphere", "sphere")
    typeCombo:AddChoice("Rectangle", "rect")
    typeCombo:AddChoice("Disk", "disk")
    typeCombo:AddChoice("Cylinder", "cylinder")
    typeCombo:AddChoice("Distant", "distant")
    
    -- Bone selection section
    panel:Help("Bone Selection")
    panel:Help("Right-click an entity to grab its bones:")
    
    -- Create bone list panel
    local boneListContainer = vgui.Create("DPanel", panel)
    boneListContainer:SetTall(150)
    boneListContainer:Dock(TOP)
    boneListContainer:DockMargin(10, 5, 10, 5)
    boneListContainer.Paint = function(self, w, h)
        draw.RoundedBox(4, 0, 0, w, h, Color(50, 50, 50, 255))
    end
    
    local boneList = vgui.Create("DListView", boneListContainer)
    boneList:Dock(FILL)
    boneList:SetMultiSelect(false)
    boneList:AddColumn("ID"):SetFixedWidth(40)
    boneList:AddColumn("Bone Name")
    
    -- Store reference for updating
    local tool = LocalPlayer():GetTool("remix_rt_light_bonemerge")
    if tool then
        tool.BoneListPanel = boneList
        
        -- Function to populate bone list
        function tool:PopulateBoneList(listPanel)
            if not self.BoneList or #self.BoneList == 0 then
                local line = listPanel:AddLine("--", "No bones grabbed yet")
                line:SetSelectable(false)
                return
            end
            
            for _, bone in ipairs(self.BoneList) do
                local line = listPanel:AddLine(bone.id, bone.name)
                line.OnSelect = function()
                    RunConsoleCommand("remix_rt_light_bonemerge_bone_id", tostring(bone.id))
                end
            end
        end
        
        -- Initial population
        tool:PopulateBoneList(boneList)
    end
    
    -- Position offset
    panel:Help("Position Offset (Local Space)")
    panel:NumSlider("X (Forward/Back)", "remix_rt_light_bonemerge_offset_x", -100, 100, 1)
    panel:NumSlider("Y (Left/Right)", "remix_rt_light_bonemerge_offset_y", -100, 100, 1)
    panel:NumSlider("Z (Up/Down)", "remix_rt_light_bonemerge_offset_z", -100, 100, 1)
    
    -- Angle offset
    panel:Help("Angle Offset")
    panel:NumSlider("Pitch", "remix_rt_light_bonemerge_angle_p", -180, 180, 0)
    panel:NumSlider("Yaw", "remix_rt_light_bonemerge_angle_y", -180, 180, 0)
    panel:NumSlider("Roll", "remix_rt_light_bonemerge_angle_r", -180, 180, 0)
    
    -- Standard light properties
    panel:Help("Light Properties")
    panel:NumSlider("Radius", "remix_rt_light_bonemerge_radius", 1, 2000, 0)
    panel:NumSlider("Brightness", "remix_rt_light_bonemerge_brightness", 0, 10000, 2)
    panel:NumSlider("Volumetrics", "remix_rt_light_bonemerge_volumetric", 0, 5, 2)
    
    -- Color picker
    panel:AddControl("Color", {
        Label = "Color",
        Red = "remix_rt_light_bonemerge_color_r",
        Green = "remix_rt_light_bonemerge_color_g",
        Blue = "remix_rt_light_bonemerge_color_b",
        ShowAlpha = 0,
        ShowHSV = 1,
        ShowRGB = 1,
        Multiplier = 255
    })
    
    -- Shaping (for sphere lights)
    panel:CheckBox("Enable Light Shaping", "remix_rt_light_bonemerge_shape_enabled")
    panel:NumSlider("Cone Angle", "remix_rt_light_bonemerge_cone", 0, 180, 0)
    panel:NumSlider("Cone Softness", "remix_rt_light_bonemerge_softness", 0, 1, 2)
    panel:NumSlider("Focus Exponent", "remix_rt_light_bonemerge_focus", 0, 10, 2)
    
    -- Shape-specific parameters
    panel:Help("Shape-Specific Parameters")
    panel:NumSlider("X Size/Radius", "remix_rt_light_bonemerge_xsize", 1, 2000, 0)
    panel:NumSlider("Y Size/Radius", "remix_rt_light_bonemerge_ysize", 1, 2000, 0)
    panel:NumSlider("Axis Length", "remix_rt_light_bonemerge_axislen", 1, 2000, 0)
    panel:NumSlider("Angular Diameter", "remix_rt_light_bonemerge_distantang", 0, 10, 2)
end
