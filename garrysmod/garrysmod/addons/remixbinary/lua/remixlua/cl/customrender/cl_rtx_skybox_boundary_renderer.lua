-- Renders the map's skybox boundaries (tools/toolsskybox) with a debug material for visualization
-- Useful for identifying map leaks or the "non-playable" boundary.

if not CLIENT then return end
require("niknaks")
local RenderCore = include("remixlua/cl/customrender/render_core.lua") or RemixRenderCore

-- ConVars
local CONVARS = {
    ENABLED = CreateClientConVar("rtx_sbr_enable", "0", true, false, "Enables skybox boundary debug rendering"),
    DEBUG = CreateClientConVar("rtx_sbr_debug", "0", true, false, "Shows debug info for skybox rendering"),
    CHUNK_SIZE = CreateClientConVar("rtx_sbr_chunk_size", "65536", true, false, "Size of chunks for mesh combining"),
    COLOR_R = CreateClientConVar("rtx_sbr_color_r", "255", true, false, "Red component of debug color"),
    COLOR_G = CreateClientConVar("rtx_sbr_color_g", "0", true, false, "Green component of debug color"),
    COLOR_B = CreateClientConVar("rtx_sbr_color_b", "255", true, false, "Blue component of debug color"),
    ALPHA = CreateClientConVar("rtx_sbr_alpha", "255", true, false, "Alpha of debug material (255 = opaque)"),
    WIRE = CreateClientConVar("rtx_sbr_wireframe", "0", true, false, "Render as wireframe"),
    INVERT = CreateClientConVar("rtx_sbr_invert", "0", true, false, "Invert normals (show inside-out)"),
}

-- Local Variables
local mapMeshes = {
    chunks = {}, -- Single group since we force one material
}
local isEnabled = false
local renderStats = {draws = 0}
local buildState = { active = false, processed = 0, total = 0 }
local Vector = Vector
local math_min = math.min
local math_max = math.max
local math_huge = math.huge
local math_floor = math.floor
local table_insert = table.insert
local MAX_VERTICES = 30000
local MAX_TOTAL_VERTICES = 5000000 -- Lower budget for skybox only

-- Reuse debug white material
local DEBUG_MAT = Material("models/debug/debugwhite")
local WIRE_MAT = Material("models/wireframe")

local function GetDebugMaterial()
    return CONVARS.WIRE:GetBool() and WIRE_MAT or DEBUG_MAT
end

-- Validation helpers (borrowed from render_core)
local function ValidateVertex(pos)
    if RenderCore and RenderCore.ValidateVertex then
        return RenderCore.ValidateVertex(pos)
    end
    if not pos or not pos.x or not pos.y or not pos.z then return false end
    if pos.x ~= pos.x or pos.y ~= pos.y or pos.z ~= pos.z then return false end
    return true
end

local function IsSkyboxFace(face)
    if not face then return false end
    local material = face:GetMaterial()
    if not material then return false end
    local matName = material:GetName():lower()
    return matName:find("tools/toolsskybox", 1, true) ~= nil or
           matName:find("skybox/", 1, true) ~= nil
end

local function GetChunkKey(x, y, z)
    if RenderCore and RenderCore.HashChunkKey then
        return RenderCore.HashChunkKey(x, y, z)
    end
    return x .. "," .. y .. "," .. z
end

local function CreateMeshBatch(vertices, material, maxVertsPerMesh)
    local meshes = {}
    local currentVerts = {}
    local vertCount = 0
    local invert = CONVARS.INVERT:GetBool()
    
    for i = 1, #vertices, 3 do
        -- Add all three vertices of the triangle
        -- If inverting, swap order of 2nd and 3rd vertex to flip winding
        local v1, v2, v3 = vertices[i], vertices[i+1], vertices[i+2]
        
        if v1 and v2 and v3 then
            if invert then
                -- Flip winding
                table_insert(currentVerts, v1)
                table_insert(currentVerts, v3)
                table_insert(currentVerts, v2)
            else
                table_insert(currentVerts, v1)
                table_insert(currentVerts, v2)
                table_insert(currentVerts, v3)
            end
            vertCount = vertCount + 1
        end
        
        if vertCount >= maxVertsPerMesh - 3 then
            local newMesh = Mesh(material)
            mesh.Begin(newMesh, MATERIAL_TRIANGLES, #currentVerts / 3)
            for _, vert in ipairs(currentVerts) do
                mesh.Position(vert.pos)
                mesh.Normal(invert and (vert.normal * -1) or vert.normal)
                mesh.TexCoord(0, vert.u or 0, vert.v or 0)
                mesh.Color(255, 255, 255, 255)
                mesh.AdvanceVertex()
            end
            mesh.End()
            table_insert(meshes, newMesh)
            if RenderCore and RenderCore.TrackMesh then RenderCore.TrackMesh(newMesh) end
            currentVerts = {}
            vertCount = 0
        end
    end
    
    if #currentVerts > 0 then
        local newMesh = Mesh(material)
        mesh.Begin(newMesh, MATERIAL_TRIANGLES, #currentVerts / 3)
        for _, vert in ipairs(currentVerts) do
            mesh.Position(vert.pos)
            mesh.Normal(invert and (vert.normal * -1) or vert.normal)
            mesh.TexCoord(0, vert.u or 0, vert.v or 0)
            mesh.Color(255, 255, 255, 255)
            mesh.AdvanceVertex()
        end
        mesh.End()
        table_insert(meshes, newMesh)
        if RenderCore and RenderCore.TrackMesh then RenderCore.TrackMesh(newMesh) end
    end
    
    return meshes
end

local function CleanupMeshes()
    for chunkKey, group in pairs(mapMeshes.chunks) do
        if group.meshes then
            for _, m in ipairs(group.meshes) do
                if RenderCore and RenderCore.DestroyMesh then
                    RenderCore.DestroyMesh(m)
                elseif m and m.Destroy then
                    m:Destroy()
                end
            end
        end
    end
    mapMeshes.chunks = {}
end

local function BuildSkyboxMeshes(cancelToken)
    CleanupMeshes()
    local totalVertexCount = 0
    
    if not NikNaks or not NikNaks.CurrentMap then return end
    print("[RTX SBR] Building skybox boundary meshes...")
    local startTime = SysTime()

    -- We do this in a coroutine to avoid freezing, similar to the main renderer
    local co = coroutine.create(function()
        local frameStartTime = SysTime()
        local frameBudget = 0.003
        local chunks = {}
        local chunkSize = CONVARS.CHUNK_SIZE:GetInt()
        local debugMat = GetDebugMaterial()

        local ok, allLeafs = pcall(function() return NikNaks.CurrentMap:GetLeafs() end)
        if not ok or not allLeafs then return end

        buildState.active = true
        buildState.total = 0
        for _ in pairs(allLeafs) do buildState.total = buildState.total + 1 end
        buildState.processed = 0

        for _, leaf in pairs(allLeafs) do
            if cancelToken and cancelToken.cancelled then return end
            
            -- Only process leaves that might have skybox faces (usually outer leaves)
            if leaf then
                local okFaces, leafFaces = pcall(function() return leaf:GetFaces(true) end)
                if leafFaces then
                    for _, face in pairs(leafFaces) do
                        -- THE CORE FILTER LOGIC: Only allow Skybox faces
                        if IsSkyboxFace(face) then
                            local vertices = face:GetVertexs()
                            if vertices and #vertices > 0 then
                                -- Calculate center for chunking
                                local cx, cy, cz = 0, 0, 0
                                for _, v in ipairs(vertices) do
                                    cx = cx + v.x
                                    cy = cy + v.y
                                    cz = cz + v.z
                                end
                                cx = cx / #vertices
                                cy = cy / #vertices
                                cz = cz / #vertices

                                local chunkX = math_floor(cx / chunkSize)
                                local chunkY = math_floor(cy / chunkSize)
                                local chunkZ = math_floor(cz / chunkSize)
                                local chunkKey = GetChunkKey(chunkX, chunkY, chunkZ)

                                chunks[chunkKey] = chunks[chunkKey] or { faces = {} }
                                table_insert(chunks[chunkKey].faces, face)
                            end
                        end
                    end
                end
            end

            buildState.processed = buildState.processed + 1
            if SysTime() - frameStartTime > frameBudget then
                coroutine.yield()
                frameStartTime = SysTime()
                if cancelToken and cancelToken.cancelled then return end
            end
        end

        -- Generate meshes from collected faces
        for chunkKey, group in pairs(chunks) do
            if cancelToken and cancelToken.cancelled then return end
            
            local batchVerts = {}
            for _, face in ipairs(group.faces) do
                local verts = face:GenerateVertexTriangleData()
                if verts then
                    for _, v in ipairs(verts) do
                        if ValidateVertex(v.pos) then
                            table_insert(batchVerts, v)
                        end
                    end
                end
            end

            if #batchVerts > 0 then
                local meshes = CreateMeshBatch(batchVerts, debugMat, MAX_VERTICES)
                if meshes then
                    mapMeshes.chunks[chunkKey] = { meshes = meshes }
                end
                totalVertexCount = totalVertexCount + #batchVerts
            end
            
            if SysTime() - frameStartTime > frameBudget then
                coroutine.yield()
                frameStartTime = SysTime()
            end
        end

        buildState.active = false
        print(string.format("[RTX SBR] Built skybox meshes in %.2f seconds (%d vertices)", 
            SysTime() - startTime, totalVertexCount))
    end)

    -- Schedule the job
    RenderCore.ScheduleJob("RTXSkyboxBuildJob", function()
        if not co or coroutine.status(co) == "dead" then
            buildState.active = false
            return false
        end
        local ok, err = coroutine.resume(co)
        if not ok then
            ErrorNoHalt("[RTX SBR] Build error: " .. tostring(err) .. "\n")
            buildState.active = false
            return false
        end
        return not (coroutine.status(co) == "dead")
    end)
end

local function RenderSkyboxBoundaries()
    if not isEnabled then return end
    if RenderCore.IsOffscreen and RenderCore.IsOffscreen() then return end

    local r = CONVARS.COLOR_R:GetInt() / 255
    local g = CONVARS.COLOR_G:GetInt() / 255
    local b = CONVARS.COLOR_B:GetInt() / 255
    local a = CONVARS.ALPHA:GetInt() / 255
    
    local debugMat = GetDebugMaterial()
    local col = {r = r * 255, g = g * 255, b = b * 255}
    local translucent = (a < 1.0)

    for _, group in pairs(mapMeshes.chunks) do
        if group.meshes then
            for _, m in ipairs(group.meshes) do
                if m then
                    RenderCore.Submit({
                        material = debugMat,
                        mesh = m,
                        color = col,
                        translucent = translucent -- Force translucent queue if alpha < 1
                    })
                end
            end
        end
    end
end

-- Enable/Disable
local function EnableRenderer()
    if isEnabled then return end
    isEnabled = true
    -- Register to translucent pass to ensure we draw over world if needed, 
    -- or opaque if we want to be occluded properly.
    -- Using translucent queue usually safer for debug visualization.
    RenderCore.Register("PostDrawTranslucentRenderables", "RTXSkyboxBoundaryRender", function()
        -- Only if using translucent queue
        if CONVARS.ALPHA:GetInt() < 255 then
            RenderSkyboxBoundaries()
        end
    end)
    
    RenderCore.Register("PostDrawOpaqueRenderables", "RTXSkyboxBoundaryRenderOpaque", function()
        -- If opaque, draw here
        if CONVARS.ALPHA:GetInt() >= 255 then
            RenderSkyboxBoundaries()
        end
    end)
end

local function DisableRenderer()
    if not isEnabled then return end
    isEnabled = false
    RenderCore.Unregister("PostDrawTranslucentRenderables", "RTXSkyboxBoundaryRender")
    RenderCore.Unregister("PostDrawOpaqueRenderables", "RTXSkyboxBoundaryRenderOpaque")
end

local function Initialize(token)
    local success, err = pcall(BuildSkyboxMeshes, token)
    if not success then
        ErrorNoHalt("[RTX SBR] Failed to build: " .. tostring(err) .. "\n")
        DisableRenderer()
        return
    end
    
    timer.Simple(1, function()
        if CONVARS.ENABLED:GetBool() then
            local success, err = pcall(EnableRenderer)
            if not success then
                ErrorNoHalt("[RTX SBR] Failed to enable renderer: " .. tostring(err) .. "\n")
                DisableRenderer()
            end
        end
    end)
end

-- Hooks & Commands
RenderCore.Register("InitPostEntity", "RTXSBRInit", Initialize)
RenderCore.Register("PostCleanupMap", "RTXSBRRebuild", function() RenderCore.RequestRebuild("PostCleanupMap") end)
RenderCore.RegisterRebuildSink("RTXSBRRebuildSink", Initialize)

cvars.AddChangeCallback("rtx_sbr_enable", function(_, _, new)
    if tobool(new) then EnableRenderer() else DisableRenderer() end
end)

concommand.Add("rtx_sbr_rebuild", BuildSkyboxMeshes)

-- Stats
RenderCore.RegisterStats("RTXSBR", function()
    if not isEnabled then return "" end
    return string.format("Skybox Debug: %d chunks | %s", table.Count(mapMeshes.chunks), buildState.active and "BUILDING" or "READY")
end)
