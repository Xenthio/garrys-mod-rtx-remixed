if not (BRANCH == "x86-64" or BRANCH == "chromium") then return end
include("shared.lua")

-- Optional queue include to throttle RemixLight operations
if file.Exists("remixlua/cl/remixapi/cl_remix_light_queue.lua", "LUA") then
    include("remixlua/cl/remixapi/cl_remix_light_queue.lua")
end

local cv_visualize = CreateClientConVar("remix_rt_light_visualize", "1", true, false, "Show HUD visualization for RTX lights")
local cv_vis_range = CreateClientConVar("remix_rt_light_visualize_range", "2048", true, false, "Max distance to show light visualization")
local cv_vis_always = CreateClientConVar("remix_rt_light_visualize_always", "0", true, false, "Always show visualization, even when not looking at lights")
local cv_vis_scale = CreateClientConVar("remix_rt_light_visualize_scale", "1.0", true, false, "Scale factor for visualization size (0.1 to 10.0)")
local cv_vis_fill_opacity = CreateClientConVar("remix_rt_light_visualize_fill_opacity", "135", true, false, "Fill opacity for shape visualization (0-255)")
local cv_debug_updates = CreateClientConVar("remix_rt_light_debug_updates", "0", true, false, "Print debug info when lights update")

-- Reusable tables to reduce GC pressure during frequent updates
local _vec_pos = { x = 0, y = 0, z = 0 }
local _vec_rad = { x = 0, y = 0, z = 0 }
local _vec_dir = { x = 0, y = 0, z = 0 }
local _vec_axis = { x = 0, y = 0, z = 0 }
local _vec_xaxis = { x = 0, y = 0, z = 0 }
local _vec_yaxis = { x = 0, y = 0, z = 0 }

local _base_info = { hash = 0, radiance = _vec_rad, isDynamic = false }

local _shaping = { direction = _vec_dir, coneAngleDegrees = 0, coneSoftness = 0, focusExponent = 0 }
local _sphere_info = { position = _vec_pos, radius = 0, volumetricRadianceScale = 0 } 

local _cylinder_info = { position = _vec_pos, radius = 0, axis = _vec_axis, axisLength = 0, volumetricRadianceScale = 0 }
local _disk_info = { position = _vec_pos, xAxis = _vec_xaxis, yAxis = _vec_yaxis, xRadius = 0, yRadius = 0, direction = _vec_dir, volumetricRadianceScale = 0 }
local _rect_info = { position = _vec_pos, xAxis = _vec_xaxis, yAxis = _vec_yaxis, xSize = 0, ySize = 0, direction = _vec_dir, volumetricRadianceScale = 0 }
local _distant_info = { direction = _vec_dir, angularDiameterDegrees = 0, volumetricRadianceScale = 0 }

local function vec_to_table(v) return { x = v.x, y = v.y, z = v.z } end

-- Helper function to determine outline color based on luminance
local function GetOutlineColor(r, g, b)
    -- Calculate perceived luminance (ITU-R BT.709)
    local luminance = 0.2126 * r + 0.7152 * g + 0.0722 * b
    -- Use white outline for dark colors, black for bright colors
    if luminance < 128 then
        return 255, 255, 255 -- White
    else
        return 0, 0, 0 -- Black
    end
end

-- Helper function to draw thick lines with outline
local function DrawThickLine(x1, y1, x2, y2, thickness, r, g, b, a)
    -- Draw outline first (thicker, contrasting color)
    local outlineR, outlineG, outlineB = GetOutlineColor(r, g, b)
    surface.SetDrawColor(outlineR, outlineG, outlineB, a * 0.8)
    for i = 0, thickness + 2 do
        local offset = i - math.floor((thickness + 2) / 2)
        surface.DrawLine(x1 + offset, y1, x2 + offset, y2)
        surface.DrawLine(x1, y1 + offset, x2, y2 + offset)
    end
    
    -- Draw main line on top
    surface.SetDrawColor(r, g, b, a)
    for i = 0, thickness - 1 do
        local offset = i - math.floor(thickness / 2)
        surface.DrawLine(x1 + offset, y1, x2 + offset, y2)
        surface.DrawLine(x1, y1 + offset, x2, y2 + offset)
    end
end

-- Helper function to draw filled polygon from world space points (both sides) with outline
local function DrawFilledWorldPoly(worldPoints, r, g, b, a)
    local screenVerts = {}
    for _, wp in ipairs(worldPoints) do
        local sp = wp:ToScreen()
        if not sp.visible then return end -- Skip if any vertex is not visible
        table.insert(screenVerts, { x = sp.x, y = sp.y })
    end
    
    -- Draw outline first
    local outlineR, outlineG, outlineB = GetOutlineColor(r, g, b)
    for i = 1, #screenVerts do
        local next_i = (i % #screenVerts) + 1
        local v1 = screenVerts[i]
        local v2 = screenVerts[next_i]
        
        -- Draw thick outline edge
        surface.SetDrawColor(outlineR, outlineG, outlineB, a * 0.8)
        for t = 0, 2 do
            local offset = t - 1
            surface.DrawLine(v1.x + offset, v1.y, v2.x + offset, v2.y)
            surface.DrawLine(v1.x, v1.y + offset, v2.x, v2.y + offset)
        end
    end
    
    surface.SetDrawColor(r, g, b, a)
    draw.NoTexture()
    
    -- Draw front face
    surface.DrawPoly(screenVerts)
    
    -- Draw back face (reversed winding order)
    local reversedVerts = {}
    for i = #screenVerts, 1, -1 do
        table.insert(reversedVerts, screenVerts[i])
    end
    surface.DrawPoly(reversedVerts)
end

-- Color scheme for different light types
local lightColors = {
    sphere = Color(255, 200, 100),
    rect = Color(100, 200, 255),
    disk = Color(255, 100, 200),
    cylinder = Color(200, 100, 255),
    distant = Color(255, 255, 100),
}

local lightIcons = {
    sphere = "●",
    rect = "▭",
    disk = "◯",
    cylinder = "▯",
    distant = "☀",
}

function ENT:Draw()
    -- Don't draw the physics prop - only visualizations in HUDPaint
end

-- Cache for light entities
local cachedLightEnts = {}
local lastLightCacheTime = 0
local LIGHT_CACHE_INTERVAL = 0.5  -- Update cache every 0.5 seconds

-- HUD Paint visualization
hook.Add("HUDPaint", "RemixRTLight_Visualize", function()
    if not cv_visualize:GetBool() then return end
    
    local ply = LocalPlayer()
    if not IsValid(ply) then return end
    
    local curTime = CurTime()
    if curTime - lastLightCacheTime > LIGHT_CACHE_INTERVAL then
        cachedLightEnts = ents.FindByClass("remix_rt_light")
        lastLightCacheTime = curTime
    end
    
    if #cachedLightEnts == 0 then return end
    
    -- Hide visualizations when using the camera tool
    local wep = ply:GetActiveWeapon()
    if IsValid(wep) and wep:GetClass() == "gmod_camera" then return end
    
    local eyePos = ply:EyePos()
    local eyeAng = ply:EyeAngles()
    local maxRange = cv_vis_range:GetFloat()
    local alwaysShow = cv_vis_always:GetBool()
    local vizScale = math.Clamp(cv_vis_scale:GetFloat(), 0.1, 10.0)
    local fillOpacity = math.Clamp(cv_vis_fill_opacity:GetInt(), 0, 255)
    local lineThickness = 2
    
    for _, ent in ipairs(cachedLightEnts) do
        if not IsValid(ent) then continue end
        
        local pos = ent:GetPos()
        local dist = eyePos:Distance(pos)
        
        -- Range check
        if dist > maxRange then continue end
        
        -- Check if looking towards the light (unless always show is enabled)
        if not alwaysShow then
            local toLight = (pos - eyePos):GetNormalized()
            local dot = eyeAng:Forward():Dot(toLight)
            if dot < 0.3 then continue end -- ~70 degree FOV
        end
        
        local screenPos = pos:ToScreen()
        if not screenPos.visible then continue end
        
        local x, y = screenPos.x, screenPos.y
        local lt = ent:GetNWString("rtx_light_type", "sphere")
        local radius = ent:GetNWFloat("rtx_light_radius", 20)
        local brightness = ent:GetNWFloat("rtx_light_brightness", 1)
        
        -- Get the light's RGB color directly
        local r = ent:GetNWFloat("rtx_light_color_r", 255)
        local g = ent:GetNWFloat("rtx_light_color_g", 220)
        local b = ent:GetNWFloat("rtx_light_color_b", 180)
        local col = Color(r, g, b)
        
        -- Alpha fade based on distance
        local alpha = math.Clamp(255 * (1 - dist / maxRange), 50, 255)
        col.a = alpha
        
        -- Get outline color (more subtle for text)
        local outlineR, outlineG, outlineB = GetOutlineColor(col.r, col.g, col.b)
        local outlineCol = Color(outlineR, outlineG, outlineB, alpha * 0.5)
        
        -- Draw icon/symbol with subtle outline
        local icon = lightIcons[lt] or "●"
        draw.SimpleTextOutlined(icon, "DermaLarge", x, y, col, TEXT_ALIGN_CENTER, TEXT_ALIGN_CENTER, 1, outlineCol)
        
        -- Draw light type label with subtle outline
        local label = string.upper(lt)
        draw.SimpleTextOutlined(label, "DermaDefault", x, y + 20, col, TEXT_ALIGN_CENTER, TEXT_ALIGN_TOP, 1, outlineCol)
        
        -- Draw properties with subtle outline
        local info = string.format("R:%.0f B:%.1f", radius, brightness)
        local infoCol = Color(255, 255, 255, alpha * 0.8)
        draw.SimpleTextOutlined(info, "DermaDefaultBold", x, y + 35, infoCol, TEXT_ALIGN_CENTER, TEXT_ALIGN_TOP, 1, Color(0, 0, 0, alpha * 0.4))
        
        -- Draw bonemerge status if applicable
        if ent:GetNWBool("rtx_light_is_bonemerged", false) then
            local parentID = ent:GetNWInt("rtx_light_parent_id", -1)
            local boneID = ent:GetNWInt("rtx_light_bone_id", -1)
            local parent = Entity(parentID)
            
            local bonemergeCol = Color(100, 255, 100, alpha * 0.9)
            local bonemergeOutline = Color(0, 0, 0, alpha * 0.5)
            
            if IsValid(parent) then
                local boneName = parent:GetBoneName(boneID) or "Entity"
                local parentName = parent:GetClass()
                local bonemergeText = string.format("🔗 %s [%s]", parentName, boneName)
                draw.SimpleTextOutlined(bonemergeText, "DermaDefault", x, y + 50, bonemergeCol, TEXT_ALIGN_CENTER, TEXT_ALIGN_TOP, 1, bonemergeOutline)
                
                -- Draw line from light to parent bone
                local parentPos = parent:GetPos()
                if boneID >= 0 and parent:GetBoneCount() > 0 and boneID < parent:GetBoneCount() then
                    local boneMatrix = parent:GetBoneMatrix(boneID)
                    if boneMatrix then
                        parentPos = boneMatrix:GetTranslation()
                    end
                end
                
                local parentScreen = parentPos:ToScreen()
                if parentScreen.visible then
                    DrawThickLine(x, y, parentScreen.x, parentScreen.y, lineThickness * 0.8, 100, 255, 100, alpha * 0.4)
                end
            else
                draw.SimpleTextOutlined("🔗 DETACHED", "DermaDefault", x, y + 50, Color(255, 100, 100, alpha * 0.9), TEXT_ALIGN_CENTER, TEXT_ALIGN_TOP, 1, bonemergeOutline)
            end
        end
        
        -- Draw direction indicator for directional lights
        if lt == "distant" or lt == "rect" or lt == "disk" then
            local ang = ent:GetAngles()
            local dir = ang:Forward()
            local endPos = pos + dir * (radius * 2 * vizScale)
            local endScreen = endPos:ToScreen()
            
            if endScreen.visible then
                DrawThickLine(x, y, endScreen.x, endScreen.y, lineThickness, col.r, col.g, col.b, alpha * 0.8)
                
                -- Draw arrow head
                local arrowLen = 10 * vizScale
                local lineVec = Vector(endScreen.x - x, endScreen.y - y, 0):GetNormalized()
                local perpVec = Vector(-lineVec.y, lineVec.x, 0)
                
                local tip = { x = endScreen.x, y = endScreen.y }
                local left = { x = endScreen.x - lineVec.x * arrowLen + perpVec.x * arrowLen * 0.5, y = endScreen.y - lineVec.y * arrowLen + perpVec.y * arrowLen * 0.5 }
                local right = { x = endScreen.x - lineVec.x * arrowLen - perpVec.x * arrowLen * 0.5, y = endScreen.y - lineVec.y * arrowLen - perpVec.y * arrowLen * 0.5 }
                
                DrawThickLine(tip.x, tip.y, left.x, left.y, lineThickness, col.r, col.g, col.b, alpha * 0.8)
                DrawThickLine(tip.x, tip.y, right.x, right.y, lineThickness, col.r, col.g, col.b, alpha * 0.8)
            end
        end
        
        -- Draw shaping cone indicator for sphere lights (spotlight style)
        if lt == "sphere" and ent:GetNWBool("rtx_light_shape_enabled", false) then
            local coneAngle = ent:GetNWFloat("rtx_light_shape_cone", 90)
            local coneSoftness = ent:GetNWFloat("rtx_light_shape_softness", 0.1)
            local ang = ent:GetAngles()
            local dir = ang:Forward()
            
            -- Calculate cone geometry
            local coneLen = radius * 2.5 * vizScale
            local coneEnd = pos + dir * coneLen
            local coneRadius = math.tan(math.rad(coneAngle / 2)) * coneLen
            
            local up = ang:Up()
            local right = ang:Right()
            
            -- Draw filled cone with gradient (more lines for smoother appearance)
            local numRays = 16
            for i = 0, numRays - 1 do
                local angle = (i / numRays) * math.pi * 2
                local offset = (up * math.cos(angle) + right * math.sin(angle)) * coneRadius
                local edgePos = coneEnd + offset
                local edgeScreen = edgePos:ToScreen()
                
                if edgeScreen.visible then
                    -- Brighter lines for better visibility
                    DrawThickLine(x, y, edgeScreen.x, edgeScreen.y, lineThickness * 1.2, col.r, col.g, col.b, alpha * 0.6)
                end
            end
            
            -- Draw cone base circle (spotlight coverage area)
            local numCirclePoints = 24
            local circlePoints = {}
            for i = 0, numCirclePoints do
                local angle = (i / numCirclePoints) * math.pi * 2
                local offset = (up * math.cos(angle) + right * math.sin(angle)) * coneRadius
                local circlePos = coneEnd + offset
                local circleScreen = circlePos:ToScreen()
                if circleScreen.visible then
                    table.insert(circlePoints, {x = circleScreen.x, y = circleScreen.y})
                end
            end
            
            -- Draw circle segments
            for i = 1, #circlePoints - 1 do
                DrawThickLine(circlePoints[i].x, circlePoints[i].y, circlePoints[i+1].x, circlePoints[i+1].y, 
                             lineThickness, col.r, col.g, col.b, alpha * 0.7)
            end
            
            -- Draw center indicator at spotlight target
            local endScreen = coneEnd:ToScreen()
            if endScreen.visible then
                local dotSize = 4
                surface.SetDrawColor(col.r, col.g, col.b, alpha)
                surface.DrawRect(endScreen.x - dotSize/2, endScreen.y - dotSize/2, dotSize, dotSize)
            end
            
            -- Draw cone angle text with subtle outline
            local coneTextCol = Color(col.r, col.g, col.b, alpha * 0.8)
            local subtleOutline = Color(outlineR, outlineG, outlineB, alpha * 0.3)
            draw.SimpleTextOutlined(string.format("∠%.0f° (softness: %.2f)", coneAngle, coneSoftness), 
                                   "DermaDefault", x, y + 50, coneTextCol, TEXT_ALIGN_CENTER, TEXT_ALIGN_TOP, 1, subtleOutline)
        end
        
        -- Draw 3D shape visualizations for physical dimensions
        local ang = ent:GetAngles()
        
        if lt == "rect" then
            local xsize = ent:GetNWFloat("rtx_light_xsize", 40) * vizScale
            local ysize = ent:GetNWFloat("rtx_light_ysize", 40) * vizScale
            
            -- Calculate rectangle corners in world space
            local right = ang:Right()
            local up = ang:Up()
            local halfX = xsize / 2
            local halfY = ysize / 2
            
            -- Reuse table to avoid allocation
            local corners = corners or {}
            corners[1] = pos + right * halfX + up * halfY      -- Top-right
            corners[2] = pos + right * halfX - up * halfY      -- Bottom-right
            corners[3] = pos - right * halfX - up * halfY      -- Bottom-left
            corners[4] = pos - right * halfX + up * halfY      -- Top-left
            
            -- Draw filled rectangle
            if fillOpacity > 0 then
                DrawFilledWorldPoly(corners, col.r, col.g, col.b, fillOpacity * (alpha / 255))
            end
            
            -- Draw rectangle outline with thick lines
            for i = 1, 4 do
                local next_i = (i % 4) + 1
                local c1 = corners[i]:ToScreen()
                local c2 = corners[next_i]:ToScreen()
                if c1.visible and c2.visible then
                    DrawThickLine(c1.x, c1.y, c2.x, c2.y, lineThickness, col.r, col.g, col.b, alpha * 0.9)
                end
            end
            
            -- Draw dimensions text with subtle outline
            local dimTextCol = Color(col.r, col.g, col.b, alpha * 0.8)
            local subtleOutline = Color(outlineR, outlineG, outlineB, alpha * 0.3)
            draw.SimpleTextOutlined(string.format("%.0f×%.0f", xsize / vizScale, ysize / vizScale), "DermaDefault", x, y + 50, dimTextCol, TEXT_ALIGN_CENTER, TEXT_ALIGN_TOP, 1, subtleOutline)
            
        elseif lt == "disk" then
            local xrad = ent:GetNWFloat("rtx_light_xradius", 20) * vizScale
            local yrad = ent:GetNWFloat("rtx_light_yradius", 20) * vizScale
            
            -- Draw ellipse outline (24 segments)
            local right = ang:Right()
            local up = ang:Up()
            local segments = 24
            
            -- Build ellipse points for fill (skip if fillOpacity is 0 to save work)
            if fillOpacity > 0 then
                local ellipsePoints = ellipsePoints or {}  -- Reuse table
                for i = 0, segments - 1 do
                    local angle = (i / segments) * math.pi * 2
                    ellipsePoints[i + 1] = pos + right * (math.cos(angle) * xrad) + up * (math.sin(angle) * yrad)
                end
                -- Trim table if needed
                for i = segments + 1, #ellipsePoints do
                    ellipsePoints[i] = nil
                end
                DrawFilledWorldPoly(ellipsePoints, col.r, col.g, col.b, fillOpacity * (alpha / 255))
            end
            
            -- Draw outline with thick lines
            for i = 0, segments - 1 do
                local angle1 = (i / segments) * math.pi * 2
                local angle2 = ((i + 1) / segments) * math.pi * 2
                
                local p1 = pos + right * (math.cos(angle1) * xrad) + up * (math.sin(angle1) * yrad)
                local p2 = pos + right * (math.cos(angle2) * xrad) + up * (math.sin(angle2) * yrad)
                
                local s1 = p1:ToScreen()
                local s2 = p2:ToScreen()
                
                if s1.visible and s2.visible then
                    DrawThickLine(s1.x, s1.y, s2.x, s2.y, lineThickness, col.r, col.g, col.b, alpha * 0.9)
                end
            end
            
            -- Draw radii text with subtle outline
            local radiiTextCol = Color(col.r, col.g, col.b, alpha * 0.8)
            local subtleOutline = Color(outlineR, outlineG, outlineB, alpha * 0.3)
            draw.SimpleTextOutlined(string.format("R:%.0f,%.0f", xrad / vizScale, yrad / vizScale), "DermaDefault", x, y + 50, radiiTextCol, TEXT_ALIGN_CENTER, TEXT_ALIGN_TOP, 1, subtleOutline)
            
        elseif lt == "cylinder" then
            local axisLen = ent:GetNWFloat("rtx_light_axis_len", 40) * vizScale
            local cylRadius = radius * vizScale
            
            -- Draw cylinder outline
            local up = ang:Up()
            local right = ang:Right()
            local forward = ang:Forward()
            
            local topCenter = pos + up * (axisLen / 2)
            local bottomCenter = pos - up * (axisLen / 2)
            
            -- Draw top and bottom circles (12 segments each)
            local segments = 12
            
            -- Build circle points for filling (skip if fillOpacity is 0)
            if fillOpacity > 0 then
                local topPoints = topPoints or {}
                local bottomPoints = bottomPoints or {}
                for i = 0, segments - 1 do
                    local angle = (i / segments) * math.pi * 2
                    local offset = right * (math.cos(angle) * cylRadius) + forward * (math.sin(angle) * cylRadius)
                    topPoints[i + 1] = topCenter + offset
                    bottomPoints[i + 1] = bottomCenter + offset
                end
                -- Trim tables if needed
                for i = segments + 1, #topPoints do
                    topPoints[i] = nil
                    bottomPoints[i] = nil
                end
                DrawFilledWorldPoly(topPoints, col.r, col.g, col.b, fillOpacity * (alpha / 255))
                DrawFilledWorldPoly(bottomPoints, col.r, col.g, col.b, fillOpacity * (alpha / 255))
                
                -- Fill cylinder sides with quads between top and bottom
                for i = 0, segments - 1 do
                    local angle1 = (i / segments) * math.pi * 2
                    local angle2 = ((i + 1) / segments) * math.pi * 2
                    
                    local offset1 = right * (math.cos(angle1) * cylRadius) + forward * (math.sin(angle1) * cylRadius)
                    local offset2 = right * (math.cos(angle2) * cylRadius) + forward * (math.sin(angle2) * cylRadius)
                    
                    -- Create quad: top1 -> top2 -> bottom2 -> bottom1
                    local sideQuad = {
                        topCenter + offset1,
                        topCenter + offset2,
                        bottomCenter + offset2,
                        bottomCenter + offset1,
                    }
                    DrawFilledWorldPoly(sideQuad, col.r, col.g, col.b, fillOpacity * (alpha / 255) * 0.7)
                end
            end
            
            -- Draw circles outline with thick lines
            for i = 0, segments - 1 do
                local angle1 = (i / segments) * math.pi * 2
                local angle2 = ((i + 1) / segments) * math.pi * 2
                
                local offset1 = right * (math.cos(angle1) * cylRadius) + forward * (math.sin(angle1) * cylRadius)
                local offset2 = right * (math.cos(angle2) * cylRadius) + forward * (math.sin(angle2) * cylRadius)
                
                -- Top circle
                local top1 = (topCenter + offset1):ToScreen()
                local top2 = (topCenter + offset2):ToScreen()
                if top1.visible and top2.visible then
                    DrawThickLine(top1.x, top1.y, top2.x, top2.y, lineThickness, col.r, col.g, col.b, alpha * 0.9)
                end
                
                -- Bottom circle
                local bot1 = (bottomCenter + offset1):ToScreen()
                local bot2 = (bottomCenter + offset2):ToScreen()
                if bot1.visible and bot2.visible then
                    DrawThickLine(bot1.x, bot1.y, bot2.x, bot2.y, lineThickness, col.r, col.g, col.b, alpha * 0.9)
                end
                
                -- Connecting lines (draw 4 vertical lines)
                if i % 3 == 0 then
                    if top1.visible and bot1.visible then
                        DrawThickLine(top1.x, top1.y, bot1.x, bot1.y, lineThickness, col.r, col.g, col.b, alpha * 0.7)
                    end
                end
            end
            
            -- Draw cylinder dimensions text with subtle outline
            local cylTextCol = Color(col.r, col.g, col.b, alpha * 0.8)
            local subtleOutline = Color(outlineR, outlineG, outlineB, alpha * 0.3)
            draw.SimpleTextOutlined(string.format("L:%.0f R:%.0f", axisLen / vizScale, cylRadius / vizScale), "DermaDefault", x, y + 50, cylTextCol, TEXT_ALIGN_CENTER, TEXT_ALIGN_TOP, 1, subtleOutline)
            
        elseif lt == "sphere" then
            -- Draw a circle representing the sphere radius
            -- Billboard effect: always face the camera
            local sphereRadius = radius * vizScale
            
            -- Calculate billboard vectors (perpendicular to camera view direction)
            local toCamera = (eyePos - pos):GetNormalized()
            local worldUp = Vector(0, 0, 1)
            
            -- If looking straight up/down, use a different reference vector
            if math.abs(toCamera.z) > 0.99 then
                worldUp = Vector(1, 0, 0)
            end
            
            -- Create perpendicular vectors for the circle plane
            local right = toCamera:Cross(worldUp):GetNormalized()
            local up = right:Cross(toCamera):GetNormalized()
            
            local segments = 20
            
            -- Build circle points for fill (skip if fillOpacity is 0)
            if fillOpacity > 0 then
                local circlePoints = circlePoints or {}
                for i = 0, segments - 1 do
                    local angle = (i / segments) * math.pi * 2
                    circlePoints[i + 1] = pos + right * (math.cos(angle) * sphereRadius) + up * (math.sin(angle) * sphereRadius)
                end
                -- Trim table if needed
                for i = segments + 1, #circlePoints do
                    circlePoints[i] = nil
                end
                DrawFilledWorldPoly(circlePoints, col.r, col.g, col.b, fillOpacity * (alpha / 255) * 0.5)
            end
            
            -- Draw circle outline with thick lines
            for i = 0, segments - 1 do
                local angle1 = (i / segments) * math.pi * 2
                local angle2 = ((i + 1) / segments) * math.pi * 2
                
                local p1 = pos + right * (math.cos(angle1) * sphereRadius) + up * (math.sin(angle1) * sphereRadius)
                local p2 = pos + right * (math.cos(angle2) * sphereRadius) + up * (math.sin(angle2) * sphereRadius)
                
                local s1 = p1:ToScreen()
                local s2 = p2:ToScreen()
                
                if s1.visible and s2.visible then
                    DrawThickLine(s1.x, s1.y, s2.x, s2.y, lineThickness, col.r, col.g, col.b, alpha * 0.7)
                end
            end
            
        elseif lt == "distant" then
            local angDiam = ent:GetNWFloat("rtx_light_distant_angle", 0.5)
            -- Draw angular diameter text with subtle outline
            local angTextCol = Color(col.r, col.g, col.b, alpha * 0.8)
            local subtleOutline = Color(outlineR, outlineG, outlineB, alpha * 0.3)
            draw.SimpleTextOutlined(string.format("∅%.2f°", angDiam), "DermaDefault", x, y + 50, angTextCol, TEXT_ALIGN_CENTER, TEXT_ALIGN_TOP, 1, subtleOutline)
        end
    end
end)

local function onDistantLightCreated()
    local cv = GetConVar("rtx_api_map_lights_env_brightness_mult")
    if cv and cv:GetFloat() ~= 0 then
        RunConsoleCommand("rtx_api_map_lights_env_brightness_mult", "0")
    end
end

local function ensure_light(ent)
    -- Add defensive checks to prevent multiple creation attempts
    if not IsValid(ent) then return end
    if ent.LightId then return end  -- Already has a light
    if ent.LightCreateQueued then return end  -- Already trying to create
    if not RemixLight then return end  -- API not available
    
    local pos = ent:GetPos() + Vector(0,0,10)
    local ang = ent:GetAngles()
    local dir = ang:Forward()
    local lt = ent:GetNWString("rtx_light_type", "sphere")
    local col = ent:GetNWVector("rtx_light_col", Vector(15,15,15))
    local radius = ent:GetNWFloat("rtx_light_radius", 20)
    local volScale = ent:GetNWFloat("rtx_light_volumetric", 1.0)

    local base = {
        -- Use decimal CRC to avoid nil from base-16 conversion; ensures unique, stable hash per entity
        hash = tonumber(util.CRC("ent_light_" .. ent:EntIndex())) or 1,
        radiance = { x = col.x, y = col.y, z = col.z },
    }

    -- Mark as queued before attempting creation
    ent.LightCreateQueued = true

    local createdId = nil
    if lt == "sphere" then
        local sphere = {
            position = vec_to_table(pos),
            radius = radius,
            volumetricRadianceScale = volScale,
        }
        local shapingEnabled = ent:GetNWBool("rtx_light_shape_enabled", false)
        if shapingEnabled then
            sphere.shaping = { direction = { x = dir.x, y = dir.y, z = dir.z }, coneAngleDegrees = ent:GetNWFloat("rtx_light_shape_cone", 90), coneSoftness = ent:GetNWFloat("rtx_light_shape_softness", 0.1), focusExponent = ent:GetNWFloat("rtx_light_shape_focus", 1.0) }
        end
        if RemixLightQueue and RemixLightQueue.CreateSphere then
            createdId = RemixLightQueue.CreateSphere(base, sphere, ent:EntIndex())
        elseif RemixLight.CreateSphere then
            createdId = RemixLight.CreateSphere(base, sphere, ent:EntIndex())
        end
    elseif lt == "cylinder" then
        local cyl = {
            position = vec_to_table(pos),
            radius = radius,
            axis = { x = ang:Up().x, y = ang:Up().y, z = ang:Up().z },
            axisLength = ent:GetNWFloat("rtx_light_axis_len", radius*2),
            volumetricRadianceScale = volScale,
        }
        if RemixLightQueue and RemixLightQueue.CreateCylinder then
            createdId = RemixLightQueue.CreateCylinder(base, cyl, ent:EntIndex())
        elseif RemixLight.CreateCylinder then
            createdId = RemixLight.CreateCylinder(base, cyl, ent:EntIndex())
        end
    elseif lt == "disk" then
        local disk = {
            position = vec_to_table(pos),
            xAxis = { x = ang:Right().x, y = ang:Right().y, z = ang:Right().z }, xRadius = ent:GetNWFloat("rtx_light_xradius", radius),
            yAxis = { x = ang:Up().x, y = ang:Up().y, z = ang:Up().z }, yRadius = ent:GetNWFloat("rtx_light_yradius", radius),
            direction = { x = dir.x, y = dir.y, z = dir.z },
            volumetricRadianceScale = volScale,
        }
        if RemixLightQueue and RemixLightQueue.CreateDisk then
            createdId = RemixLightQueue.CreateDisk(base, disk, ent:EntIndex())
        elseif RemixLight.CreateDisk then
            createdId = RemixLight.CreateDisk(base, disk, ent:EntIndex())
        end
    elseif lt == "rect" then
        local rect = {
            position = vec_to_table(pos),
            xAxis = { x = ang:Right().x, y = ang:Right().y, z = ang:Right().z }, xSize = ent:GetNWFloat("rtx_light_xsize", radius*2),
            yAxis = { x = ang:Up().x, y = ang:Up().y, z = ang:Up().z }, ySize = ent:GetNWFloat("rtx_light_ysize", radius*2),
            direction = { x = dir.x, y = dir.y, z = dir.z },
            volumetricRadianceScale = volScale,
        }
        if RemixLightQueue and RemixLightQueue.CreateRect then
            createdId = RemixLightQueue.CreateRect(base, rect, ent:EntIndex())
        elseif RemixLight.CreateRect then
            createdId = RemixLight.CreateRect(base, rect, ent:EntIndex())
        end
    elseif lt == "distant" then
        local distant = { direction = { x = dir.x, y = dir.y, z = dir.z }, angularDiameterDegrees = ent:GetNWFloat("rtx_light_distant_angle", 0.5), volumetricRadianceScale = volScale }
        if RemixLightQueue and RemixLightQueue.CreateDistant then
            createdId = RemixLightQueue.CreateDistant(base, distant, ent:EntIndex())
        elseif RemixLight.CreateDistant then
            createdId = RemixLight.CreateDistant(base, distant, ent:EntIndex())
        end
    end

    ent.LightId = createdId
    ent.LightCreateQueued = nil
end

-- Helper function to perform light update (called from Think and per-frame update)
local function updateLight(self)
    if not self.LightId or not RemixLight then return end
    
    -- Read position directly from entity (not networked) for smooth movement
    local pos = self:GetPos()
    local ang = self:GetAngles()
    
    -- Read all properties
    local lt = self:GetNWString("rtx_light_type", "sphere")
    local colorR = self:GetNWFloat("rtx_light_color_r", 255)
    local colorG = self:GetNWFloat("rtx_light_color_g", 220)
    local colorB = self:GetNWFloat("rtx_light_color_b", 180)
    local brightness = self:GetNWFloat("rtx_light_brightness", 1)
    
    -- Calculate radiance from RGB color and brightness
    local scale = math.max(0, brightness)
    local col = Vector((colorR/12)*scale, (colorG/12)*scale, (colorB/12)*scale)
    
    local radius = self:GetNWFloat("rtx_light_radius", 20)
    local shapingEnabled = self:GetNWBool("rtx_light_shape_enabled", false)
    local cone = self:GetNWFloat("rtx_light_shape_cone", 90)
    local softness = self:GetNWFloat("rtx_light_shape_softness", 0.1)
    local focus = self:GetNWFloat("rtx_light_shape_focus", 1.0)
    local volScale = self:GetNWFloat("rtx_light_volumetric", 1.0)
    local xsize = self:GetNWFloat("rtx_light_xsize", 40)
    local ysize = self:GetNWFloat("rtx_light_ysize", 40)
    local xradius = self:GetNWFloat("rtx_light_xradius", 20)
    local yradius = self:GetNWFloat("rtx_light_yradius", 20)
    local axislen = self:GetNWFloat("rtx_light_axis_len", 40)
    local distantang = self:GetNWFloat("rtx_light_distant_angle", 0.5)
    
    -- Initialize cache on first run
    if not self.LastUpdateCache then
        self.LastUpdateCache = {}
        self.NeedsUpdate = true
    end
    
    -- Check if physics object is awake (being moved/interacted with)
    local phys = self:GetPhysicsObject()
    local isMoving = IsValid(phys) and not phys:IsAsleep()
    
    -- Check if anything has changed (with small threshold for position/angles to avoid floating point noise)
    local cache = self.LastUpdateCache
    local needsUpdate = self.NeedsUpdate or false
    
    -- If moving, use tighter thresholds for more responsive updates
    local posThreshold = isMoving and 0.01 or 1  -- 0.01 units when moving, 1 unit when static
    local angThreshold = isMoving and 0.01 or 0.1  -- 0.01 degrees when moving, 0.1 when static
    
    if not needsUpdate then
        -- Position change check
        if not cache.pos or cache.pos:DistToSqr(pos) > (posThreshold * posThreshold) then
            needsUpdate = true
        end
        
        -- Angle change check
        if not needsUpdate and cache.ang then
            local pitchDiff = math.abs(math.AngleDifference(ang.p, cache.ang.p))
            local yawDiff = math.abs(math.AngleDifference(ang.y, cache.ang.y))
            local rollDiff = math.abs(math.AngleDifference(ang.r, cache.ang.r))
            if pitchDiff > angThreshold or yawDiff > angThreshold or rollDiff > angThreshold then
                needsUpdate = true
            end
        end
        
        -- Property change checks
        if not needsUpdate then
            if cache.lt ~= lt or cache.colorR ~= colorR or cache.colorG ~= colorG or cache.colorB ~= colorB or
               cache.brightness ~= brightness or cache.radius ~= radius or
               cache.shapingEnabled ~= shapingEnabled or cache.cone ~= cone or
               cache.softness ~= softness or cache.focus ~= focus or
               cache.volScale ~= volScale or cache.xsize ~= xsize or
               cache.ysize ~= ysize or cache.xradius ~= xradius or
               cache.yradius ~= yradius or cache.axislen ~= axislen or
               cache.distantang ~= distantang then
                needsUpdate = true
            end
        end
    end
    
    -- Skip update if nothing changed
    if not needsUpdate then return end
    
    -- Debug output
    if cv_debug_updates:GetBool() then
        local movingStr = isMoving and " [MOVING]" or ""
        local brightness = self:GetNWFloat("rtx_light_brightness", 1)
        print(string.format("[RTX Light #%d] Updating (type=%s)%s Brightness=%.2f Radiance=(%.2f,%.2f,%.2f)",
            self:EntIndex(), lt, movingStr, brightness, col.x, col.y, col.z))
    end
    
    -- Update cache
    cache.pos = Vector(pos.x, pos.y, pos.z)
    cache.ang = Angle(ang.p, ang.y, ang.r)
    cache.lt = lt
    cache.colorR = colorR
    cache.colorG = colorG
    cache.colorB = colorB
    cache.brightness = brightness
    cache.radius = radius
    cache.shapingEnabled = shapingEnabled
    cache.cone = cone
    cache.softness = softness
    cache.focus = focus
    cache.volScale = volScale
    cache.xsize = xsize
    cache.ysize = ysize
    cache.xradius = xradius
    cache.yradius = yradius
    cache.axislen = axislen
    cache.distantang = distantang
    self.NeedsUpdate = false
    
    -- Perform the actual light update
    local dir = ang:Forward()

    -- Update reusable vectors
    _vec_pos.x, _vec_pos.y, _vec_pos.z = pos.x, pos.y, pos.z
    _vec_dir.x, _vec_dir.y, _vec_dir.z = dir.x, dir.y, dir.z
    _vec_rad.x, _vec_rad.y, _vec_rad.z = col.x, col.y, col.z

    -- Cache hash if not present
    if not self.LightHash then
        self.LightHash = tonumber(util.CRC("ent_light_" .. self:EntIndex())) or 1
    end

    _base_info.hash = self.LightHash
    -- _base_info.radiance is already _vec_rad
    
    -- Always mark as dynamic to allow real-time updates (matches flashlight behavior)
    -- Static lights may be cached aggressively by RTX Remix and refuse updates
    _base_info.isDynamic = true

    if lt == "sphere" and RemixLight.UpdateSphere then
        _sphere_info.radius = radius
        _sphere_info.volumetricRadianceScale = volScale
        -- _sphere_info.position is already _vec_pos
        
        if shapingEnabled then
            _shaping.coneAngleDegrees = cone
            _shaping.coneSoftness = softness
            _shaping.focusExponent = focus
            -- _shaping.direction is already _vec_dir
            _sphere_info.shaping = _shaping
        else
            _sphere_info.shaping = nil
        end
        RemixLight.UpdateSphere(_base_info, _sphere_info, self.LightId)
        
    elseif lt == "cylinder" and RemixLight.UpdateCylinder then
        local up = ang:Up()
        _vec_axis.x, _vec_axis.y, _vec_axis.z = up.x, up.y, up.z
        
        _cylinder_info.radius = radius
        _cylinder_info.axisLength = axislen
        _cylinder_info.volumetricRadianceScale = volScale
        -- _cylinder_info.position is already _vec_pos
        -- _cylinder_info.axis is already _vec_axis
        
        RemixLight.UpdateCylinder(_base_info, _cylinder_info, self.LightId)
        
    elseif lt == "disk" and RemixLight.UpdateDisk then
        local right = ang:Right()
        local up = ang:Up()
        _vec_xaxis.x, _vec_xaxis.y, _vec_xaxis.z = right.x, right.y, right.z
        _vec_yaxis.x, _vec_yaxis.y, _vec_yaxis.z = up.x, up.y, up.z
        
        _disk_info.xRadius = xradius
        _disk_info.yRadius = yradius
        _disk_info.volumetricRadianceScale = volScale
        -- _disk_info.position is already _vec_pos
        -- _disk_info.direction is already _vec_dir
        
        RemixLight.UpdateDisk(_base_info, _disk_info, self.LightId)
        
    elseif lt == "rect" and RemixLight.UpdateRect then
        local right = ang:Right()
        local up = ang:Up()
        _vec_xaxis.x, _vec_xaxis.y, _vec_xaxis.z = right.x, right.y, right.z
        _vec_yaxis.x, _vec_yaxis.y, _vec_yaxis.z = up.x, up.y, up.z
        
        _rect_info.xSize = xsize
        _rect_info.ySize = ysize
        _rect_info.volumetricRadianceScale = volScale
        -- _rect_info.position is already _vec_pos
        -- _rect_info.direction is already _vec_dir
        
        RemixLight.UpdateRect(_base_info, _rect_info, self.LightId)
        
    elseif lt == "distant" and RemixLight.UpdateDistant then
        _distant_info.angularDiameterDegrees = distantang
        _distant_info.volumetricRadianceScale = volScale
        -- _distant_info.direction is already _vec_dir
        
        RemixLight.UpdateDistant(_base_info, _distant_info, self.LightId)
    end
end

-- Helper function to update bonemerged light position
local function updateBonemergedPosition(self)
    if not self:GetNWBool("rtx_light_is_bonemerged", false) then return end
    
    local parentID = self:GetNWInt("rtx_light_parent_id", -1)
    if parentID == -1 then return end
    
    local parent = Entity(parentID)
    if not IsValid(parent) then
        -- Parent removed, convert to static light
        self:SetNWBool("rtx_light_is_bonemerged", false)
        return
    end
    
    local boneID = self:GetNWInt("rtx_light_bone_id", -1)
    local offsetPos = self:GetNWVector("rtx_light_offset_pos", Vector(0, 0, 0))
    local offsetAng = self:GetNWAngle("rtx_light_offset_ang", Angle(0, 0, 0))
    
    -- Get bone matrix if valid bone, otherwise use entity position
    local boneMatrix = nil
    if boneID >= 0 and parent:GetBoneCount() > 0 and boneID < parent:GetBoneCount() then
        boneMatrix = parent:GetBoneMatrix(boneID)
    end
    
    if boneMatrix then
        -- Calculate world position/angle from bone matrix + offsets
        local bonePos = boneMatrix:GetTranslation()
        local boneAng = boneMatrix:GetAngles()
        
        -- Apply position offset in bone's local space
        local worldPos = bonePos + boneAng:Forward() * offsetPos.x + 
                                     boneAng:Right() * offsetPos.y + 
                                     boneAng:Up() * offsetPos.z
        
        -- Apply angle offset
        local worldAng = boneAng + offsetAng
        
        -- Update light position and angle
        self:SetPos(worldPos)
        self:SetAngles(worldAng)
    else
        -- Fallback to entity position if no valid bone
        local worldPos = parent:GetPos() + parent:GetAngles():Forward() * offsetPos.x + 
                                            parent:GetAngles():Right() * offsetPos.y + 
                                            parent:GetAngles():Up() * offsetPos.z
        local worldAng = parent:GetAngles() + offsetAng
        
        self:SetPos(worldPos)
        self:SetAngles(worldAng)
    end
end

function ENT:Think()
    -- Ensure we have a light, but be defensive about it
    if not self.LightId and not self.LightCreateQueued then
        ensure_light(self)
    end
    
    -- Fire onDistantLightCreated when type first becomes distant (NWVars are reliable here)
    local lt = self:GetNWString("rtx_light_type", "sphere")
    if lt == "distant" and self.LastSeenLightType ~= "distant" then
        onDistantLightCreated()
    end
    self.LastSeenLightType = lt
    
    -- Update bonemerged position if applicable
    updateBonemergedPosition(self)
    
    -- Call the update function
    updateLight(self)
    
    -- Think more frequently if physics object is awake (being moved) or if bonemerged
    local isBonemerged = self:GetNWBool("rtx_light_is_bonemerged", false)
    local phys = self:GetPhysicsObject()
    if isBonemerged or (IsValid(phys) and not phys:IsAsleep()) then
        self:NextThink(CurTime()) -- Think every frame when moving or bonemerged
    else
        self:NextThink(CurTime() + 0.05) -- Think every 50ms when static
    end
    return true
end

-- Context menu for tweaking light parameters
function ENT:PopulateToolMenu(panel)
    -- Not used; using context menu hook below
end

-- Store editor function globally so it can be accessed by other properties
local function OpenLightEditor(ent)
        if not IsValid(ent) then return end
        local frame = vgui.Create("DFrame")
        frame:SetTitle("Remix Light")
        frame:SetSize(math.min(ScrW()*0.35, 420), math.min(ScrH()*0.7, 520))
        frame:SetSizable(true)
        frame:Center()
        frame:MakePopup()

        local body = vgui.Create("DScrollPanel", frame)
        body:Dock(FILL)
        body:DockMargin(0, 0, 0, 40)

        local typeCombo = vgui.Create("DComboBox", body)
        typeCombo:Dock(TOP)
        typeCombo:DockMargin(10, 10, 10, 5)
        local lt_init = ent:GetNWString("rtx_light_type", "sphere")
        typeCombo:AddChoice("Sphere", "sphere")
        typeCombo:AddChoice("Rect", "rect")
        typeCombo:AddChoice("Disk", "disk")
        typeCombo:AddChoice("Cylinder", "cylinder")
        typeCombo:AddChoice("Distant", "distant")
        -- Ensure internal selected ID/data is set so refreshVisibility reads the correct type
        if typeCombo.ChooseOption then
            typeCombo:ChooseOption(string.upper(lt_init))
        else
            typeCombo:SetValue(string.upper(lt_init))
        end

        local radius = vgui.Create("DNumSlider", body)
        radius:Dock(TOP)
        radius:DockMargin(10, 5, 10, 5)
        radius:SetText("Radius")
        radius:SetMin(1)
        radius:SetMax(2000)
        radius:SetDecimals(0)
        radius:SetValue(ent:GetNWFloat("rtx_light_radius", 20))

        local vol = vgui.Create("DNumSlider", body)
        vol:Dock(TOP)
        vol:DockMargin(10, 5, 10, 5)
        vol:SetText("Volumetrics Scale")
        vol:SetMin(0)
        vol:SetMax(5)
        vol:SetDecimals(2)
        vol:SetValue(ent:GetNWFloat("rtx_light_volumetric", 1))

        local mixer = vgui.Create("DColorMixer", body)
        mixer:Dock(TOP)
        mixer:DockMargin(10, 5, 10, 10)
        mixer:SetTall(140)
        mixer:SetAlphaBar(false)
        mixer:SetPalette(false)
        mixer:SetWangs(true)
        local r = ent:GetNWFloat("rtx_light_color_r", 255)
        local g = ent:GetNWFloat("rtx_light_color_g", 220)
        local b = ent:GetNWFloat("rtx_light_color_b", 180)
        mixer:SetColor(Color(r, g, b))

        local brightness = vgui.Create("DNumSlider", body)
        brightness:Dock(TOP)
        brightness:DockMargin(10, 5, 10, 5)
        brightness:SetText("Brightness")
        brightness:SetMin(0)
        brightness:SetMax(10000)
        brightness:SetDecimals(2)
        brightness:SetValue(ent:GetNWFloat("rtx_light_brightness", 1))

        -- Sphere shaping
        local shapeToggle = vgui.Create("DCheckBoxLabel", body)
        shapeToggle:Dock(TOP)
        shapeToggle:DockMargin(10, 5, 10, 5)
        shapeToggle:SetText("Enable Light Shaping")
        shapeToggle:SetValue(ent:GetNWBool("rtx_light_shape_enabled", false) and 1 or 0)

        local cone = vgui.Create("DNumSlider", body)
        cone:Dock(TOP)
        cone:DockMargin(10, 5, 10, 5)
        cone:SetText("Cone Angle (deg)")
        cone:SetMin(0)
        cone:SetMax(180)
        cone:SetDecimals(0)
        cone:SetValue(ent:GetNWFloat("rtx_light_shape_cone", 90))

        local soft = vgui.Create("DNumSlider", body)
        soft:Dock(TOP)
        soft:DockMargin(10, 5, 10, 5)
        soft:SetText("Cone Softness")
        soft:SetMin(0)
        soft:SetMax(1)
        soft:SetDecimals(2)
        soft:SetValue(ent:GetNWFloat("rtx_light_shape_softness", 0.1))

        local focus = vgui.Create("DNumSlider", body)
        focus:Dock(TOP)
        focus:DockMargin(10, 5, 10, 5)
        focus:SetText("Focus Exponent")
        focus:SetMin(0)
        focus:SetMax(10)
        focus:SetDecimals(2)
        focus:SetValue(ent:GetNWFloat("rtx_light_shape_focus", 1.0))

        -- direction is taken from the entity's rotation; no manual yaw/pitch here

        -- Per-type extra controls
        local xsize = vgui.Create("DNumSlider", body)
        xsize:Dock(TOP)
        xsize:DockMargin(10, 5, 10, 5)
        xsize:SetText("Rect X Size")
        xsize:SetMin(1)
        xsize:SetMax(2000)
        xsize:SetDecimals(0)
        xsize:SetValue(ent:GetNWFloat("rtx_light_xsize", 40))

        local ysize = vgui.Create("DNumSlider", body)
        ysize:Dock(TOP)
        ysize:DockMargin(10, 5, 10, 5)
        ysize:SetText("Rect Y Size")
        ysize:SetMin(1)
        ysize:SetMax(2000)
        ysize:SetDecimals(0)
        ysize:SetValue(ent:GetNWFloat("rtx_light_ysize", 40))

        local xradius = vgui.Create("DNumSlider", body)
        xradius:Dock(TOP)
        xradius:DockMargin(10, 5, 10, 5)
        xradius:SetText("Disk X Radius")
        xradius:SetMin(1)
        xradius:SetMax(2000)
        xradius:SetDecimals(0)
        xradius:SetValue(ent:GetNWFloat("rtx_light_xradius", 20))

        local yradius = vgui.Create("DNumSlider", body)
        yradius:Dock(TOP)
        yradius:DockMargin(10, 5, 10, 5)
        yradius:SetText("Disk Y Radius")
        yradius:SetMin(1)
        yradius:SetMax(2000)
        yradius:SetDecimals(0)
        yradius:SetValue(ent:GetNWFloat("rtx_light_yradius", 20))

        local axislen = vgui.Create("DNumSlider", body)
        axislen:Dock(TOP)
        axislen:DockMargin(10, 5, 10, 5)
        axislen:SetText("Cylinder Axis Length")
        axislen:SetMin(1)
        axislen:SetMax(2000)
        axislen:SetDecimals(0)
        axislen:SetValue(ent:GetNWFloat("rtx_light_axis_len", 40))

        local distantang = vgui.Create("DNumSlider", body)
        distantang:Dock(TOP)
        distantang:DockMargin(10, 5, 10, 5)
        distantang:SetText("Distant Angular Diameter")
        distantang:SetMin(0)
        distantang:SetMax(10)
        distantang:SetDecimals(2)
        distantang:SetValue(ent:GetNWFloat("rtx_light_distant_angle", 0.5))

        -- Bonemerge offset controls (only visible for bonemerged lights)
        local offsetLabel = vgui.Create("DLabel", body)
        offsetLabel:Dock(TOP)
        offsetLabel:DockMargin(10, 10, 10, 0)
        offsetLabel:SetText("-- Bonemerge Offsets --")
        offsetLabel:SetTextColor(Color(100, 255, 100))
        offsetLabel:SetFont("DermaDefaultBold")
        
        local offsetX = vgui.Create("DNumSlider", body)
        offsetX:Dock(TOP)
        offsetX:DockMargin(10, 5, 10, 5)
        offsetX:SetText("Position X (Forward)")
        offsetX:SetMin(-500)
        offsetX:SetMax(500)
        offsetX:SetDecimals(1)
        offsetX:SetValue(ent:GetNWVector("rtx_light_offset_pos", Vector(0,0,0)).x)
        
        local offsetY = vgui.Create("DNumSlider", body)
        offsetY:Dock(TOP)
        offsetY:DockMargin(10, 5, 10, 5)
        offsetY:SetText("Position Y (Right)")
        offsetY:SetMin(-500)
        offsetY:SetMax(500)
        offsetY:SetDecimals(1)
        offsetY:SetValue(ent:GetNWVector("rtx_light_offset_pos", Vector(0,0,0)).y)
        
        local offsetZ = vgui.Create("DNumSlider", body)
        offsetZ:Dock(TOP)
        offsetZ:DockMargin(10, 5, 10, 5)
        offsetZ:SetText("Position Z (Up)")
        offsetZ:SetMin(-500)
        offsetZ:SetMax(500)
        offsetZ:SetDecimals(1)
        offsetZ:SetValue(ent:GetNWVector("rtx_light_offset_pos", Vector(0,0,0)).z)
        
        local angleP = vgui.Create("DNumSlider", body)
        angleP:Dock(TOP)
        angleP:DockMargin(10, 5, 10, 5)
        angleP:SetText("Angle Pitch")
        angleP:SetMin(-180)
        angleP:SetMax(180)
        angleP:SetDecimals(1)
        angleP:SetValue(ent:GetNWAngle("rtx_light_offset_ang", Angle(0,0,0)).p)
        
        local angleY = vgui.Create("DNumSlider", body)
        angleY:Dock(TOP)
        angleY:DockMargin(10, 5, 10, 5)
        angleY:SetText("Angle Yaw")
        angleY:SetMin(-180)
        angleY:SetMax(180)
        angleY:SetDecimals(1)
        angleY:SetValue(ent:GetNWAngle("rtx_light_offset_ang", Angle(0,0,0)).y)
        
        local angleR = vgui.Create("DNumSlider", body)
        angleR:Dock(TOP)
        angleR:DockMargin(10, 5, 10, 5)
        angleR:SetText("Angle Roll")
        angleR:SetMin(-180)
        angleR:SetMax(180)
        angleR:SetDecimals(1)
        angleR:SetValue(ent:GetNWAngle("rtx_light_offset_ang", Angle(0,0,0)).r)

        -- Realtime apply as user adjusts controls
        -- Throttled server apply helper
        local function sendApplyThrottled()
            if not IsValid(ent) then return end
            local id = ent:EntIndex()
            local timerName = "remix_rt_light_apply_" .. tostring(id)
            timer.Create(timerName, 0.05, 1, function()
                if not IsValid(ent) then return end
                if not net then return end
                net.Start("remix_rt_light_apply")
                net.WriteEntity(ent)
                -- Build a compact table of values
                local t = {
                    rtx_light_type = (function()
                        local sid = typeCombo:GetSelectedID()
                        return (sid and typeCombo:GetOptionData(sid)) or ent:GetNWString("rtx_light_type", "sphere")
                    end)(),
                    rtx_light_radius = math.floor(radius:GetValue()),
                    rtx_light_brightness = brightness:GetValue(),
                    rtx_light_volumetric = vol:GetValue(),
                    rtx_light_shape_enabled = shapeToggle:GetChecked() and true or false,
                    rtx_light_shape_cone = cone:GetValue(),
                    rtx_light_shape_softness = soft:GetValue(),
                    rtx_light_shape_focus = focus:GetValue(),
                    rtx_light_xsize = xsize:GetValue(),
                    rtx_light_ysize = ysize:GetValue(),
                    rtx_light_xradius = xradius:GetValue(),
                    rtx_light_yradius = yradius:GetValue(),
                    rtx_light_axis_len = axislen:GetValue(),
                    rtx_light_distant_angle = distantang:GetValue(),
                }
                local col = mixer:GetColor()
                t.rtx_light_color_r = col.r
                t.rtx_light_color_g = col.g
                t.rtx_light_color_b = col.b
                
                -- Bonemerge offsets
                t.rtx_light_offset_pos = {x = offsetX:GetValue(), y = offsetY:GetValue(), z = offsetZ:GetValue()}
                t.rtx_light_offset_ang = {p = angleP:GetValue(), y = angleY:GetValue(), r = angleR:GetValue()}
                
                -- Debug logging
                if cv_debug_updates:GetBool() then
                    local brightnessVal = brightness:GetValue()
                    local scale = math.max(0.0, brightnessVal)
                    local radiance_x = (col.r/12)*scale
                    local radiance_y = (col.g/12)*scale
                    local radiance_z = (col.b/12)*scale
                    print(string.format("[Editor->Server] Brightness=%.2f, Color=(%d,%d,%d), Radiance=(%.2f,%.2f,%.2f)",
                        brightnessVal, col.r, col.g, col.b, radiance_x, radiance_y, radiance_z))
                end
                
                net.WriteTable(t)
                net.SendToServer()
            end)
        end

        local function applyRealtime()
            if not IsValid(ent) then return end
            -- Set NWVars locally for instant client-side visual feedback
            local col = mixer:GetColor()
            local brightnessVal = brightness:GetValue()
            local scale = math.max(0.0, brightnessVal)
            
            ent:SetNWFloat("rtx_light_radius", math.floor(radius:GetValue()))
            ent:SetNWFloat("rtx_light_brightness", brightnessVal)
            ent:SetNWFloat("rtx_light_volumetric", vol:GetValue())
            ent:SetNWBool("rtx_light_shape_enabled", shapeToggle:GetChecked())
            ent:SetNWFloat("rtx_light_shape_cone", cone:GetValue())
            ent:SetNWFloat("rtx_light_shape_softness", soft:GetValue())
            ent:SetNWFloat("rtx_light_shape_focus", focus:GetValue())
            ent:SetNWFloat("rtx_light_xsize", xsize:GetValue())
            ent:SetNWFloat("rtx_light_ysize", ysize:GetValue())
            ent:SetNWFloat("rtx_light_xradius", xradius:GetValue())
            ent:SetNWFloat("rtx_light_yradius", yradius:GetValue())
            ent:SetNWFloat("rtx_light_axis_len", axislen:GetValue())
            ent:SetNWFloat("rtx_light_distant_angle", distantang:GetValue())
            ent:SetNWFloat("rtx_light_color_r", col.r)
            ent:SetNWFloat("rtx_light_color_g", col.g)
            ent:SetNWFloat("rtx_light_color_b", col.b)
            
            -- Bonemerge offsets
            ent:SetNWVector("rtx_light_offset_pos", Vector(offsetX:GetValue(), offsetY:GetValue(), offsetZ:GetValue()))
            ent:SetNWAngle("rtx_light_offset_ang", Angle(angleP:GetValue(), angleY:GetValue(), angleR:GetValue()))
            
            -- Debug logging
            if cv_debug_updates:GetBool() then
                local radiance_x = (col.r/12)*scale
                local radiance_y = (col.g/12)*scale
                local radiance_z = (col.b/12)*scale
                print(string.format("[Editor Local] Brightness=%.2f, Color=(%d,%d,%d), Radiance=(%.2f,%.2f,%.2f)",
                    brightnessVal, col.r, col.g, col.b, radiance_x, radiance_y, radiance_z))
            end
            
            local sid = typeCombo:GetSelectedID()
            local sel = (sid and typeCombo:GetOptionData(sid)) or ent:GetNWString("rtx_light_type", "sphere")
            ent:SetNWString("rtx_light_type", sel)
            
            if sel == "distant" then
                onDistantLightCreated()
            end
            
            -- Flag for instant visual update
            ent.NeedsUpdate = true
            
            -- Send throttled update to server for persistence
            sendApplyThrottled()
        end

        radius.OnValueChanged = function(_, _val)
            applyRealtime()
        end

        if mixer.ValueChanged then
            function mixer:ValueChanged(_col)
                applyRealtime()
            end
                end
        -- Note: If mixer.ValueChanged doesn't exist, we rely on the other control callbacks
        -- The entity already has the correct initial values, so we don't force an update

        brightness.OnValueChanged = function(_, _val)
            applyRealtime()
        end

        shapeToggle.OnChange = function(_, _val) applyRealtime() end
        vol.OnValueChanged = function(_, _val) applyRealtime() end
        cone.OnValueChanged = function(_, _val) applyRealtime() end
        soft.OnValueChanged = function(_, _val) applyRealtime() end
        focus.OnValueChanged = function(_, _val) applyRealtime() end
        xsize.OnValueChanged = function(_, _val) applyRealtime() end
        ysize.OnValueChanged = function(_, _val) applyRealtime() end
        xradius.OnValueChanged = function(_, _val) applyRealtime() end
        yradius.OnValueChanged = function(_, _val) applyRealtime() end
        axislen.OnValueChanged = function(_, _val) applyRealtime() end
        distantang.OnValueChanged = function(_, _val) applyRealtime() end
        
        -- Bonemerge offset callbacks
        offsetX.OnValueChanged = function(_, _val) applyRealtime() end
        offsetY.OnValueChanged = function(_, _val) applyRealtime() end
        offsetZ.OnValueChanged = function(_, _val) applyRealtime() end
        angleP.OnValueChanged = function(_, _val) applyRealtime() end
        angleY.OnValueChanged = function(_, _val) applyRealtime() end
        angleR.OnValueChanged = function(_, _val) applyRealtime() end

        -- Show only relevant controls per light type
        local function refreshVisibility()
            local selectedId = typeCombo:GetSelectedID()
            local lt = (selectedId and typeCombo:GetOptionData(selectedId)) or ent:GetNWString("rtx_light_type", "sphere")
            -- hide all optional controls first
            shapeToggle:SetVisible(false)
            cone:SetVisible(false)
            soft:SetVisible(false)
            focus:SetVisible(false)
            xsize:SetVisible(false)
            ysize:SetVisible(false)
            xradius:SetVisible(false)
            yradius:SetVisible(false)
            axislen:SetVisible(false)
            distantang:SetVisible(false)
            -- Always show common
            radius:SetVisible(true)
            brightness:SetVisible(true)
            mixer:SetVisible(true)
            vol:SetVisible(true)
            if lt == "sphere" then
                shapeToggle:SetVisible(true)
                cone:SetVisible(true)
                soft:SetVisible(true)
                focus:SetVisible(true)
            elseif lt == "rect" then
                xsize:SetVisible(true)
                ysize:SetVisible(true)
            elseif lt == "disk" then
                xradius:SetVisible(true)
                yradius:SetVisible(true)
            elseif lt == "cylinder" then
                axislen:SetVisible(true)
            elseif lt == "distant" then
                distantang:SetVisible(true)
            end
            
            -- Show bonemerge offsets only for bonemerged lights
            local isBonemerged = ent:GetNWBool("rtx_light_is_bonemerged", false)
            offsetLabel:SetVisible(isBonemerged)
            offsetX:SetVisible(isBonemerged)
            offsetY:SetVisible(isBonemerged)
            offsetZ:SetVisible(isBonemerged)
            angleP:SetVisible(isBonemerged)
            angleY:SetVisible(isBonemerged)
            angleR:SetVisible(isBonemerged)
        end
        refreshVisibility()
        -- Don't call applyRealtime() here - entity already has correct values
        -- Callbacks will handle updates when user changes controls
        typeCombo.OnSelect = function(panel, index, value, data)
            -- data contains the actual light type string (sphere, rect, disk, etc.)
            applyRealtime()
            refreshVisibility()
        end

        local close = vgui.Create("DButton", frame)
        close:Dock(BOTTOM)
        close:DockMargin(10, 5, 10, 10)
        close:SetText("Close")
        close.DoClick = function()
            frame:Close()
        end
end

-- Add property menu for editing remix lights
properties.Add("remix_rt_light_edit", {
    MenuLabel = "Edit Remix Light", Order = 0, MenuIcon = "icon16/lightbulb.png",
    Filter = function(self, ent, ply)
        return IsValid(ent) and ent:GetClass() == "remix_rt_light" and ply:IsAdmin() ~= false
    end,
    Action = function(self, ent)
        OpenLightEditor(ent)
    end
})

-- Property menu for entities with attached bonemerged lights
properties.Add("remix_rt_light_edit_attached", {
    MenuLabel = "Edit Attached Lights", Order = 1, MenuIcon = "icon16/attach.png",
    Filter = function(self, ent, ply)
        if not IsValid(ent) then return false end
        if ent:GetClass() == "remix_rt_light" then return false end -- Don't show for lights themselves
        if not ply:IsAdmin() then return false end
        
        -- Check if entity has attached lights
        for _, light in ipairs(ents.FindByClass("remix_rt_light")) do
            if light:GetNWBool("rtx_light_is_bonemerged", false) and 
               light:GetNWInt("rtx_light_parent_id", -1) == ent:EntIndex() then
                return true
            end
        end
        return false
    end,
    Action = function(self, ent)
        -- Find all attached lights
        local attachedLights = {}
        for _, light in ipairs(ents.FindByClass("remix_rt_light")) do
            if light:GetNWBool("rtx_light_is_bonemerged", false) and 
               light:GetNWInt("rtx_light_parent_id", -1) == ent:EntIndex() then
                table.insert(attachedLights, light)
            end
        end
        
        -- If only one light, open it directly
        if #attachedLights == 1 then
            OpenLightEditor(attachedLights[1])
            return
        end
        
        -- Multiple lights: Show selection menu
        local menu = DermaMenu()
        menu:SetMinimumWidth(200)
        
        for i, light in ipairs(attachedLights) do
            local lightType = light:GetNWString("rtx_light_type", "sphere")
            local boneID = light:GetNWInt("rtx_light_bone_id", -1)
            local boneName = "origin"
            
            if boneID >= 0 and ent:GetBoneCount() > 0 and boneID < ent:GetBoneCount() then
                boneName = ent:GetBoneName(boneID) or ("Bone " .. boneID)
            end
            
            local label = string.format("Light #%d: %s [%s]", i, lightType, boneName)
            menu:AddOption(label, function()
                OpenLightEditor(light)
            end):SetIcon("icon16/lightbulb.png")
        end
        
        menu:Open()
    end
})

function ENT:OnRemove()
    if RemixLight and RemixLight.DestroyLightsForEntity then
        RemixLight.DestroyLightsForEntity(self:EntIndex())
    end
    
    if self:GetNWString("rtx_light_type", "sphere") == "distant" then
        local hasOtherDistant = false
        for _, ent in ipairs(ents.FindByClass("remix_rt_light")) do
            if ent ~= self and IsValid(ent) and ent:GetNWString("rtx_light_type", "sphere") == "distant" then
                hasOtherDistant = true
                break
            end
        end
        if not hasOtherDistant then
            local cv = GetConVar("rtx_api_map_lights_env_brightness_mult")
            if cv and cv:GetFloat() == 0 then
                RunConsoleCommand("rtx_api_map_lights_env_brightness_mult", "1")
            end
        end
    end
end

-- Console commands for visualization control
concommand.Add("remix_rt_light_vis_toggle", function()
    local newVal = not cv_visualize:GetBool()
    cv_visualize:SetBool(newVal)
    print("[Remix RT Light] Visualization " .. (newVal and "enabled" or "disabled"))
end, nil, "Toggle RTX light visualization overlay")

concommand.Add("remix_rt_light_vis_range", function(ply, cmd, args)
    if #args < 1 then
        print("[Remix RT Light] Current range: " .. cv_vis_range:GetFloat())
        print("Usage: remix_rt_light_vis_range <distance>")
        return
    end
    local range = tonumber(args[1])
    if range then
        cv_vis_range:SetFloat(range)
        print("[Remix RT Light] Visualization range set to " .. range)
    end
end, nil, "Set visualization range for RTX lights")

concommand.Add("remix_rt_light_vis_scale", function(ply, cmd, args)
    if #args < 1 then
        print("[Remix RT Light] Current scale: " .. cv_vis_scale:GetFloat())
        print("Usage: remix_rt_light_vis_scale <scale> (0.1 to 10.0)")
        return
    end
    local scale = tonumber(args[1])
    if scale then
        scale = math.Clamp(scale, 0.1, 10.0)
        cv_vis_scale:SetFloat(scale)
        print("[Remix RT Light] Visualization scale set to " .. scale)
    end
end, nil, "Set visualization scale for RTX lights (0.1 to 10.0)")

concommand.Add("remix_rt_light_vis_fill", function(ply, cmd, args)
    if #args < 1 then
        print("[Remix RT Light] Current fill opacity: " .. cv_vis_fill_opacity:GetInt())
        print("Usage: remix_rt_light_vis_fill <opacity> (0 to 255)")
        print("Recommended: 30-50 for subtle fill, 0 to disable")
        return
    end
    local opacity = tonumber(args[1])
    if opacity then
        opacity = math.Clamp(math.floor(opacity), 0, 255)
        cv_vis_fill_opacity:SetInt(opacity)
        print("[Remix RT Light] Fill opacity set to " .. opacity)
    end
end, nil, "Set fill opacity for RTX light visualization (0-255)")

-- Add to tool menu if available
hook.Add("PopulateToolMenu", "RemixRTLight_ToolMenu", function()
    spawnmenu.AddToolMenuOption("Utilities", "RTX Remix - API Lights", "RTX_Remix_Light_Viz", "Entity Light HUD", "", "", function(panel)
        panel:ClearControls()

        panel:CheckBox("Enable HUD", "remix_rt_light_visualize")
        panel:NumSlider("HUD Range", "remix_rt_light_visualize_range", 512, 8192, 0)
        panel:NumSlider("Fill Opacity", "remix_rt_light_visualize_fill_opacity", 0, 255, 0)
        
        local btnReset = panel:Button("Reset to Defaults")
        btnReset.DoClick = function()
            RunConsoleCommand("remix_rt_light_visualize", "1")
            RunConsoleCommand("remix_rt_light_visualize_range", "2048")
            RunConsoleCommand("remix_rt_light_visualize_always", "0")
            RunConsoleCommand("remix_rt_light_visualize_scale", "1.0")
            RunConsoleCommand("remix_rt_light_visualize_fill_opacity", "135")
        end
    end)
end)


