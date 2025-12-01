-- Map Marker Marker System
-- Creates unique meshes for anchoring remix assets

AddCSLuaFile()

local MapMarker = {}
MapMarker.Markers = {}
MapMarker.Meshes = {}
MapMarker.Config = {
    -- Set to true to show debug visuals of the markers in-game
    Debug = CreateConVar("rtx_marker_debug", "0", FCVAR_ARCHIVE, "Show Map Marker markers for debugging"),
    -- Material used for debug rendering
    DebugMaterial = Material("rtx/marker"),
    -- Should we auto-create spawn markers?
    AutoCreateSpawnMarkers = true
}

-- Setup networking
if SERVER then
    util.AddNetworkString("MapMarker_SyncMarker")
end

-- Generate a unique hash from a string
function MapMarker:GetHashFromString(str)
    local hash = 0
    for i = 1, #str do
        hash = bit.bxor(bit.lshift(hash, 5) - hash, string.byte(str, i))
        hash = bit.band(hash, 0xFFFFFFFF) -- Keep within 32 bits
    end
    return hash
end

-- Create a mesh with a unique shape based on the hash
function MapMarker:GenerateMesh(id)
    if self.Meshes[id] then return self.Meshes[id] end
    
    local hash = type(id) == "string" and self:GetHashFromString(id) or id
    local meshData = {}
    
    -- Use the hash to determine mesh geometry
    local baseSize = 10 + (hash % 40) / 10  -- Base size varies between 10-14 units
    local width = baseSize * (0.9 + (hash % 10) / 100)  -- Slight variation in width
    local height = baseSize * (0.9 + ((hash * 3) % 10) / 100)  -- Slight variation in height
    local depth = baseSize * (0.9 + ((hash * 7) % 10) / 100)  -- Slight variation in depth
    
    -- Slight irregularities
    local offset1 = (hash % 5) / 10  -- Small offset for one corner
    local offset2 = ((hash * 3) % 5) / 10  -- Small offset for another corner
    
    -- Cube vertices (8 corners)
    local vertices = {
        Vector(-width/2, -height/2, 0),           -- Bottom 1
        Vector(width/2, -height/2, 0),            -- Bottom 2
        Vector(width/2, height/2, 0),             -- Bottom 3
        Vector(-width/2, height/2, 0),            -- Bottom 4
        Vector(-width/2 + offset1, -height/2 + offset2, depth),  -- Top 1 (with slight offset)
        Vector(width/2 - offset2, -height/2 + offset1, depth),   -- Top 2 (with slight offset)
        Vector(width/2 - offset1, height/2 - offset2, depth),    -- Top 3 (with slight offset)
        Vector(-width/2 + offset2, height/2 - offset1, depth)    -- Top 4 (with slight offset)
    }
    
    -- Define the faces of the cube (2 triangles per face, 6 faces)
    local faces = {
        -- Bottom face
        {1, 2, 3, normal = Vector(0, 0, -1)},
        {1, 3, 4, normal = Vector(0, 0, -1)},
        -- Top face
        {5, 7, 6, normal = Vector(0, 0, 1)},
        {5, 8, 7, normal = Vector(0, 0, 1)},
        -- Side 1
        {1, 5, 2, normal = Vector(0, -1, 0)},
        {2, 5, 6, normal = Vector(0, -1, 0)},
        -- Side 2
        {2, 6, 3, normal = Vector(1, 0, 0)},
        {3, 6, 7, normal = Vector(1, 0, 0)},
        -- Side 3
        {3, 7, 4, normal = Vector(0, 1, 0)},
        {4, 7, 8, normal = Vector(0, 1, 0)},
        -- Side 4
        {4, 8, 1, normal = Vector(-1, 0, 0)},
        {1, 8, 5, normal = Vector(-1, 0, 0)}
    }
    
    -- Generate mesh data from the faces
    for _, face in ipairs(faces) do
        for i = 1, 3 do
            local vertIndex = face[i]
            table.insert(meshData, {
                pos = vertices[vertIndex],
                normal = face.normal,
                u = (i == 1 or i == 3) and 0 or 1,  -- Simple UV mapping
                v = (i == 1 or i == 2) and 0 or 1
            })
        end
    end
    
    local imesh = Mesh()
    mesh.Begin(imesh, MATERIAL_TRIANGLES, #meshData / 3)
    for _, vertex in ipairs(meshData) do
        mesh.Position(vertex.pos)
        mesh.Normal(vertex.normal)
        mesh.TexCoord(0, vertex.u, vertex.v)
        mesh.AdvanceVertex()
    end
    mesh.End()
    
    -- Store mesh data for this ID
    self.Meshes[id] = {
        data = meshData,
        imesh = imesh,  -- Store the IMesh object
        hash = hash,
        width = width,
        height = height,
        depth = depth,
        triangles = #meshData / 3
    }
    
    return self.Meshes[id]
end

-- Add a marker at a specific position
function MapMarker:AddMarker(name, pos, ang, color)
    -- Generate a unique ID by combining map hash and marker name
    local mapHash = self:GetHashFromString(game.GetMap())
    local id = mapHash + self:GetHashFromString(name)
    
    -- Create marker data
    self.Markers[name] = {
        id = id,
        pos = pos,
        ang = ang or Angle(0, 0, 0),
        color = color or Color(255, 255, 255, 255), -- Fully visible by default
        mesh = self:GenerateMesh(id)
    }
    
    print("[gmRTX - Map Markers] Created marker '" .. name .. "' with hash: " .. id)
    return id
end

-- Render all markers
function MapMarker:RenderMarkers()
    -- Always render markers, regardless of debug setting
    cam.Start3D()
    for name, data in pairs(self.Markers) do
        self:RenderMesh(data.id, data.pos, data.ang, data.color)
    end
    cam.End3D()
end

-- Render a specific mesh
function MapMarker:RenderMesh(id, pos, ang, color)
    local meshData = self:GenerateMesh(id)
    if not meshData or not meshData.imesh then return end
    
    pos = pos or Vector(0, 0, 0)
    ang = ang or Angle(0, 0, 0)
    color = color or Color(255, 255, 255, 255)
    
    render.SetColorModulation(color.r/255, color.g/255, color.b/255)
    render.SetBlend(color.a/255)
    
    -- Use wireframe material for debugging
    render.SetMaterial(self.Config.DebugMaterial)
    
    -- Set up transform matrix
    local matrix = Matrix()
    matrix:Translate(pos)
    matrix:Rotate(ang)
    
    cam.PushModelMatrix(matrix)
    
    -- Draw the pre-built
    meshData.imesh:Draw()
    
    cam.PopModelMatrix()
end

-- Create ideterministc spawn marker
function MapMarker:CreateSpawnMarker()
    if not NikNaks or not NikNaks.CurrentMap then return end
    
    local spawnEntities = NikNaks.CurrentMap:FindByClass("info_player_start")
    if #spawnEntities > 0 then
        -- Sort the spawn points deterministically by position
        table.sort(spawnEntities, function(a, b)
            if a.origin.x ~= b.origin.x then return a.origin.x < b.origin.x end
            if a.origin.y ~= b.origin.y then return a.origin.y < b.origin.y end
            return a.origin.z < b.origin.z
        end)
        
        -- Always select the first spawn point after sorting
        local selectedSpawn = spawnEntities[1]
        local markerName = "map_spawn_point"
        local markerPos = selectedSpawn.origin + Vector(0, 0, 10)
        local markerAng = selectedSpawn.angles
        
        -- Create the marker
        local id = self:AddMarker(markerName, markerPos, markerAng, Color(255, 255, 255, 255))
        
        -- Network to clients if on server
        if SERVER then
            net.Start("MapMarker_SyncMarker")
            net.WriteString(markerName)
            net.WriteVector(markerPos)
            net.WriteAngle(markerAng)
            net.Broadcast()
        end
        
        MsgC(Color(0, 255, 200), "[gmRTX - Map Markers] Added spawn marker at " .. tostring(markerPos) .. "\n")
        return id
    end
    
    return nil
end

-- Network handler for clients
if CLIENT then
    net.Receive("MapMarker_SyncMarker", function()
        local name = net.ReadString()
        local pos = net.ReadVector()
        local ang = net.ReadAngle()
        MapMarker:AddMarker(name, pos, ang, Color(255, 255, 255, 255))
    end)
end

-- Initialize the marker system
function MapMarker:Initialize()
    MsgC(Color(0, 255, 200), "[gmRTX - Map Markers] Initializing Map Marker Marker System\n")
    
    -- Initialize hooks for rendering
    hook.Add("PostDrawOpaqueRenderables", "RenderMapMarkerMarkers", function()
        self:RenderMarkers()
    end)
    
    -- Create deterministic spawn marker with a delay to ensure NikNaks is loaded
    if self.Config.AutoCreateSpawnMarkers then
        timer.Simple(2, function()
            self:CreateSpawnMarker()
        end)
    end
end

-- Clean up
function MapMarker:Cleanup()
    hook.Remove("PostDrawOpaqueRenderables", "RenderMapMarkerMarkers")
    
    -- Clear meshes
    for id, meshData in pairs(self.Meshes) do
        if meshData.imesh and meshData.imesh.Destroy then
            meshData.imesh:Destroy()
        end
    end
    
    self.Meshes = {}
    self.Markers = {}
    
    MsgC(Color(0, 255, 200), "[gmRTX - Map Markers] Map Marker Marker System cleaned up\n")
end

-- Add console commands
if CLIENT then
    concommand.Add("rtx_marker_add", function(ply, cmd, args)
        local name = args[1] or "custom_marker_" .. math.random(1000)
        local pos = ply:GetEyeTrace().HitPos
        local ang = Angle(0, ply:EyeAngles().y, 0)
        local id = MapMarker:AddMarker(name, pos, ang, Color(255, 255, 0, 30))
        
        ply:ChatPrint("Added marker '" .. name .. "' with hash: " .. id)
    end)
    
    concommand.Add("rtx_marker_list", function()
        print("=== Map Marker Markers ===")
        for name, data in pairs(MapMarker.Markers) do
            print(string.format("%s: ID=%d, Pos=%s", name, data.id, tostring(data.pos)))
        end
        print("=========================")
    end)
    
    concommand.Add("rtx_marker_clear", function()
        MapMarker.Markers = {}
        print("All markers cleared")
    end)
    
    concommand.Add("rtx_marker_respawn", function()
        MapMarker:CreateSpawnMarker()
        print("Respawned markers")
    end)
end

-- Initialize the system
hook.Add("Initialize", "MapMarkerInit", function()
    MapMarker:Initialize()
end)

-- Clean up on map change
hook.Add("ShutDown", "MapMarkerCleanup", function()
    MapMarker:Cleanup()
end)

-- Export the API
_G.MapMarkerMarkers = MapMarker