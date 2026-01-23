if not CLIENT then return end
concommand.Add("get_vmt", function()
    local ply = LocalPlayer()
    
    -- TRACE FILTER: This ignores the invisible melon handles
    local trData = {
        start = ply:GetShootPos(),
        endpos = ply:GetShootPos() + (ply:GetAimVector() * 10000),
        filter = function(ent)
            -- Ignore the player and the annoying melon handles
            if ent == ply then return false end
            if ent:GetModel() == "models/props_junk/watermelon01.mdl" then return false end
            return true
        end
    }
    
    local tr = util.TraceLine(trData)
    local target = tr.Entity

    -- 1. Handle Map Brushes
    if tr.HitWorld or (not IsValid(target) and tr.Hit) then
        local tex = tr.HitTexture or "unknown"
        local path = "materials/" .. tex .. ".vmt"
        print("\n--- Map Brush: " .. tex .. " ---")
        print("Path: " .. path)
        print(file.Read(path, "GAME") or "!!! Missing or packed in BSP/VPK")
        return
    end

    -- 2. Final Safety Check
    if not IsValid(target) then 
        print("No valid entity found behind the handle.") 
        return 
    end

    -- 3. Print Materials for the REAL entity
    local materials = target:GetMaterials() or {}
    print("\n--- Final Target: " .. tostring(target:GetModel()) .. " ---")
    
    for i, matName in ipairs(materials) do
        local path = "materials/" .. matName .. ".vmt"
        local content = file.Read(path, "GAME")
        print("\n[" .. i .. "] Path: " .. path)
        print(content or "!!! Content unavailable (Missing or VPK)")
    end
end)

concommand.Add("print_vmt_path", function(ply, cmd, args)
    local rawPath = args[1]
    if not rawPath or rawPath == "" then
        print("Usage: print_vmt_path <material/path>")
        return
    end

    -- Automatically handle the 'materials/' prefix and '.vmt' extension
    local fullPath = rawPath
    if not string.StartWith(fullPath, "materials/") then
        fullPath = "materials/" .. fullPath
    end
    if not string.EndsWith(fullPath, ".vmt") then
        fullPath = fullPath .. ".vmt"
    end

    print("\n--- Reading VMT: " .. fullPath .. " ---")
    
    -- Use 'GAME' to search through all addons and mounted content
    local content = file.Read(fullPath, "GAME")
    
    if content then
        print(content)
    else
        print("!!! File not found or is packed in a VPK.")
    end
    print("--- End of File ---\n")
end)
