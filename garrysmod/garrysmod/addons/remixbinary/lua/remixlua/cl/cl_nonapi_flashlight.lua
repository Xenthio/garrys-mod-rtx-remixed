if (BRANCH == "x86-64" or BRANCH == "chromium") then return end
if not CLIENT then return end

-- Create a minimal config system if FlashlightOverride doesn't exist
if not FlashlightOverride then
    FlashlightOverride = {
        Config = {
            enabled = true,
            mesh = {
                deterministic_seed = 99999,
                color = Color(255, 255, 255, 255)
            },
            debounce = {
                default_delay = 0.3,
                max_delay = 2.0
            }
        },
        Utils = {
            DebugPrint = function(msg)
                if GetConVar("developer"):GetInt() > 0 then
                    print("[gmRTX - Flashlight]", msg)
                end
            end,
            ChatMessage = function(msg)
                chat.AddText(Color(100, 200, 255), "[RTX] ", Color(255, 255, 255), msg)
            end
        }
    }
    end

    local flashlight_enabled = false
    local flashlight_mesh_id = nil
    local flashlight_attachment_offset = Vector(20, 5, -5) -- Offset from player's eye position
    local flashlight_attachment_angles = Angle(0, 0, 0)
    local last_toggle_time = 0 -- Debounce timer
    local toggle_debounce_delay = FlashlightOverride.Config.debounce.default_delay -- Debounce delay from config
    
    -- Custom flashlight mesh system using MapMarker rendering
    local FlashlightMesh = {}
    FlashlightMesh.ActiveMeshes = {}
    
    -- Generate a flashlight-specific mesh
    function FlashlightMesh:GenerateFlashlightMesh()
        -- Use a deterministic seed for consistent mesh across all maps and players
        local flashlight_id = FlashlightOverride.Config.mesh.deterministic_seed
    
    -- Check if MapMarkerMarkers is available
    if _G.MapMarkerMarkers and _G.MapMarkerMarkers.GenerateMesh then
        local mesh_data = _G.MapMarkerMarkers:GenerateMesh(flashlight_id)
        
        return {
            id = flashlight_id,
            mesh = mesh_data,
            hash = mesh_data.hash
        }
    else
        -- Fallback: return a simple mesh structure
        return {
            id = flashlight_id,
            mesh = nil,
            hash = flashlight_id
        }
    end
    end
    
    -- Render the flashlight mesh attached to player
    function FlashlightMesh:RenderFlashlightMesh()
        if not flashlight_enabled or not flashlight_mesh_id then return end
        
        local ply = LocalPlayer()
        if not IsValid(ply) then return end
        
        -- Calculate attachment position and angles
        local eyePos = ply:EyePos()
        local eyeAng = ply:EyeAngles()
        
        -- Transform the offset to world coordinates
        local forward = eyeAng:Forward()
        local right = eyeAng:Right()
        local up = eyeAng:Up()
        
        local attachPos = eyePos + 
            forward * flashlight_attachment_offset.x + 
            right * flashlight_attachment_offset.y + 
            up * flashlight_attachment_offset.z
        
        local attachAng = eyeAng + flashlight_attachment_angles
        
    -- Use MapMarker's render system if available
    if _G.MapMarkerMarkers and _G.MapMarkerMarkers.RenderMesh then
            _G.MapMarkerMarkers:RenderMesh(
                flashlight_mesh_id, 
                attachPos, 
                attachAng, 
                FlashlightOverride.Config.mesh.color
            )
    else
        -- Fallback: Draw a simple debug visualization
        render.DrawWireframeSphere(attachPos, 5, 8, 8, Color(255, 255, 0, 150))
        end
    end
    
    local function CustomFlashlightOn()
        -- Check if the override system is enabled
        if not FlashlightOverride.Config.enabled then return end
        
        flashlight_enabled = true
        
        -- Generate and store the flashlight mesh
        local mesh_data = FlashlightMesh:GenerateFlashlightMesh()
        if mesh_data then
            flashlight_mesh_id = mesh_data.id
            FlashlightMesh.ActiveMeshes[flashlight_mesh_id] = mesh_data
        end
        
        -- Custom behavior when flashlight is turned on
        FlashlightOverride.Utils.DebugPrint("Flashlight ON - Custom mesh spawned with ID: " .. (flashlight_mesh_id or "none"))
        
        -- Add the render hook
        hook.Add("PostDrawOpaqueRenderables", "FlashlightMeshRender", function()
            FlashlightMesh:RenderFlashlightMesh()
        end)
    end
    
    local function CustomFlashlightOff()
        if not FlashlightOverride.Config.enabled then return end
        
        flashlight_enabled = false
        
        -- Clean up the mesh
        if flashlight_mesh_id then
            FlashlightMesh.ActiveMeshes[flashlight_mesh_id] = nil
            flashlight_mesh_id = nil
        end
        
        -- Remove the render hook
        hook.Remove("PostDrawOpaqueRenderables", "FlashlightMeshRender")
    end
    
-- Flashlight toggle command - can still be used for manual toggle
concommand.Add("rtx_nonapi_flashlight_toggle", function(ply, cmd, args)
        -- Debounce check to prevent accidental double-activation
        local current_time = CurTime()
        if current_time - last_toggle_time < toggle_debounce_delay then
            FlashlightOverride.Utils.DebugPrint("Flashlight toggle ignored due to debounce (too soon)")
            return
        end
        
        last_toggle_time = current_time
        
        if flashlight_enabled then
            CustomFlashlightOff()
        else
            CustomFlashlightOn()
        end
    end)
    
    -- Mesh adjustment
concommand.Add("rtx_nonapi_flashlight_mesh_offset", function(ply, cmd, args)
        if #args >= 3 then
            flashlight_attachment_offset.x = tonumber(args[1]) or flashlight_attachment_offset.x
            flashlight_attachment_offset.y = tonumber(args[2]) or flashlight_attachment_offset.y
            flashlight_attachment_offset.z = tonumber(args[3]) or flashlight_attachment_offset.z
            FlashlightOverride.Utils.ChatMessage("Flashlight offset set to: " .. tostring(flashlight_attachment_offset))
        else
            FlashlightOverride.Utils.ChatMessage("Current flashlight offset: " .. tostring(flashlight_attachment_offset))
        FlashlightOverride.Utils.ChatMessage("Usage: rtx_nonapi_flashlight_mesh_offset <x> <y> <z>")
        end
    end)
    
concommand.Add("rtx_nonapi_flashlight_mesh_angles", function(ply, cmd, args)
        if #args >= 3 then
            flashlight_attachment_angles.p = tonumber(args[1]) or flashlight_attachment_angles.p
            flashlight_attachment_angles.y = tonumber(args[2]) or flashlight_attachment_angles.y
            flashlight_attachment_angles.r = tonumber(args[3]) or flashlight_attachment_angles.r
            FlashlightOverride.Utils.ChatMessage("Flashlight angles set to: " .. tostring(flashlight_attachment_angles))
        else
            FlashlightOverride.Utils.ChatMessage("Current flashlight angles: " .. tostring(flashlight_attachment_angles))
        FlashlightOverride.Utils.ChatMessage("Usage: rtx_nonapi_flashlight_mesh_angles <pitch> <yaw> <roll>")
        end
    end)
    
concommand.Add("rtx_nonapi_flashlight_debounce", function(ply, cmd, args)
        if #args >= 1 then
            local new_delay = tonumber(args[1])
            if new_delay and new_delay >= 0 and new_delay <= FlashlightOverride.Config.debounce.max_delay then
                toggle_debounce_delay = new_delay
                FlashlightOverride.Utils.ChatMessage("Flashlight debounce delay set to: " .. toggle_debounce_delay .. "s")
            else
                FlashlightOverride.Utils.ChatMessage("Invalid debounce delay. Must be between 0 and " .. FlashlightOverride.Config.debounce.max_delay .. " seconds.")
            end
        else
            FlashlightOverride.Utils.ChatMessage("Current debounce delay: " .. toggle_debounce_delay .. "s")
        FlashlightOverride.Utils.ChatMessage("Usage: rtx_nonapi_flashlight_debounce <seconds> (0-" .. FlashlightOverride.Config.debounce.max_delay .. ")")
        end
    end)
    
    local function CleanupFlashlightMesh()
        if flashlight_enabled then
            CustomFlashlightOff()
        end
        FlashlightMesh.ActiveMeshes = {}
        hook.Remove("PostDrawOpaqueRenderables", "FlashlightMeshRender")
        FlashlightOverride.Utils.DebugPrint("Flashlight mesh system cleaned up")
    end

-- Network: Receive flashlight state updates from server (engine flashlight interception)
net.Receive("rtx_nonapi_flashlight_state", function()
    local state = net.ReadBool()
    
    if state then
        -- Turn on flashlight
        if not flashlight_enabled then
            CustomFlashlightOn()
        end
    else
        -- Turn off flashlight
        if flashlight_enabled then
            CustomFlashlightOff()
        end
    end
end)
    
    -- Add cleanup hooks
    hook.Add("ShutDown", "FlashlightMeshCleanup", CleanupFlashlightMesh)
    hook.Add("OnReloaded", "FlashlightMeshCleanup", CleanupFlashlightMesh)
    
    print("[gmRTX - Flashlight] Client-side flashlight mesh override loaded!")
print("[gmRTX - Flashlight] Engine flashlight (F key) will now be intercepted automatically") 