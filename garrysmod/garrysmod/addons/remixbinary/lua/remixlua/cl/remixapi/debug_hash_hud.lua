if not CLIENT then return end

local cvar_debug_hud = CreateClientConVar("rtx_debug_hud", "0", true, false, "Enable RTX Remix texture hash debug HUD")

surface.CreateFont("RTXDebugFont", {
    font = "Consolas",
    size = 16,
    weight = 500,
    antialias = true,
    outline = true
})

local hashCache = {}

local function Draw3DText(pos, text, color, offsetY)
    local screenPos = pos:ToScreen()
    
    if not screenPos.visible then return end
    
    surface.SetFont("RTXDebugFont")
    local w, h = surface.GetTextSize(text)
    
    local x = screenPos.x - w / 2
    local y = screenPos.y - h / 2 + (offsetY or 0)
    
    -- Background
    surface.SetDrawColor(0, 0, 0, 150)
    surface.DrawRect(x - 4, y - 4, w + 8, h + 8)
    
    -- Text
    surface.SetTextColor(color.r, color.g, color.b, 255)
    surface.SetTextPos(x, y)
    surface.DrawText(text)
    
    return h + 4 -- Return height for stacking
end

hook.Add("HUDPaint", "RTXDebugHashHUD", function()
    if not cvar_debug_hud:GetBool() then return end
    if not RemixMaterial or not RemixMaterial.GetTextureHash then return end
    
    local ply = LocalPlayer()
    if not IsValid(ply) then return end
    
    -- Trace from eyes
    local tr = ply:GetEyeTrace()
    
    if tr.Hit and not tr.HitSky then
        local ent = tr.Entity
        local materialName = ""
        
        -- Try to get material from trace
        if tr.MatType then
            -- This is usually just surface property, not material name
            -- We need the actual material name
        end
        
        -- Method 1: Get material from entity
        if IsValid(ent) then
            materialName = ent:GetMaterial() -- Overridden material
            
            -- If no override, we need the model material
            -- This is tricky for models with multiple materials
            if materialName == "" then
                -- Just show the model name for now, getting sub-materials is complex
                -- without specific submaterial index from trace
                local mats = ent:GetMaterials()
                if mats and #mats > 0 then
                    -- For now, just pick the first one or list them
                    -- Ideally we'd use the trace's hit group/surface to pick the right one
                    -- but GMod doesn't easily expose "which material index did I hit"
                    materialName = mats[1] 
                    -- We'll display the count separately, don't append it to the name we query!
                end
            end
        else
            -- World geometry (BSP)
            -- trace.HitTexture returns the material name!
            materialName = tr.HitTexture
        end
        
        if materialName and materialName ~= "" then
            -- Clean up material name (remove "maps/mapname/" prefix if present)
            -- materialName = string.StripExtension(materialName)
            
            -- Check local cache first to avoid spamming C++ calls
            local cached = hashCache[materialName]
            local hash, hashStr
            
            if cached then
                hash = cached.hash
                hashStr = cached.hashStr
            else
                -- Query C++ API
                hash, hashStr = RemixMaterial.GetTextureHash(materialName)
                
                -- Cache the result if valid (or even if invalid to stop spamming)
                -- Only cache if we got a result or if we want to throttle failures
                if hash and hash > 0 then
                    hashCache[materialName] = { hash = hash, hashStr = hashStr }
                end
            end
            
            local displayHash = hashStr or (hash and string.format("0x%X", hash) or "NOT CACHED")
            local color = (hash and hash > 0) and Color(0, 255, 0) or Color(255, 100, 100)
            
            local displayText = "Mat: " .. materialName
            if IsValid(ent) then
                local mats = ent:GetMaterials()
                if mats and #mats > 1 then
                    displayText = displayText .. " (+" .. (#mats - 1) .. " others)"
                end
            end

            -- Draw stacked text using 2D offsets
            local h1 = Draw3DText(tr.HitPos, displayText, Color(255, 255, 255), 0)
            Draw3DText(tr.HitPos, "Hash: " .. displayHash, color, h1)
            
            -- If not cached, show hint and auto-track (throttled)
            if not hash or hash == 0 then
                local now = SysTime()
                if not hashCache[materialName] or (now - (hashCache[materialName].lastTrack or 0) > 1.0) then
                    -- Auto-track if looking at it
                    RemixMaterial.TrackMaterial(materialName)
                    -- Update cache to prevent spamming track
                    hashCache[materialName] = hashCache[materialName] or {}
                    hashCache[materialName].lastTrack = now
                end
            end
        end
    end
end)

print("[RTX Debug HUD] Loaded. Enable with 'rtx_debug_hud 1'")

