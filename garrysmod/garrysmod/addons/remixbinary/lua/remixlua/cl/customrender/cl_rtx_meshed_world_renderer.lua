-- Disables source engine world rendering and replaces it with chunked mesh rendering instead, fixes engine culling issues. 
-- MAJOR THANK YOU to the creator of NikNaks, a lot of this would not be possible without it.
if not CLIENT then return end
require("niknaks")
local RenderCore = include("remixlua/cl/customrender/render_core.lua") or RemixRenderCore

-- ConVars
local CONVARS = {
    ENABLED = CreateClientConVar("rtx_mwr_enable", "1", true, false, "Forces custom mesh rendering of map"),
    DEBUG = CreateClientConVar("rtx_mwr_debug", "0", true, false, "Shows debug info for mesh rendering"),
    CHUNK_SIZE = CreateClientConVar("rtx_mwr_chunk_size", "65536", true, false, "Size of chunks for mesh combining"),
    CAPTURE_MODE = CreateClientConVar("rtx_mwr_capture_mode", "0", true, false, "Toggles r_drawworld for capture mode"),
    MAT_WHITELIST = CreateClientConVar("rtx_mwr_mat_whitelist", "", true, false, "Comma-separated material name substrings to include"),
    MAT_BLACKLIST = CreateClientConVar("rtx_mwr_mat_blacklist", "toolsskybox,skybox/", true, false, "Comma-separated material name substrings to exclude")
}

-- Local Variables and Caches
local mapMeshes = {
    opaque = {},
    translucent = {},
}
local isEnabled = false
local renderStats = {draws = 0}
local buildState = { active = false, processed = 0, total = 0 }
-- Expose build state for progress tracking
if RemixRenderCore then RemixRenderCore._worldBuildState = buildState end
local Vector = Vector
local math_min = math.min
local math_max = math.max
local math_huge = math.huge
local math_floor = math.floor
local table_insert = table.insert
local MAX_VERTICES = 30000
local MAX_TOTAL_VERTICES = 10000000 -- 10 million vertex budget (roughly 400MB)
local totalVertexCount = 0

local function IsMaterialAllowed(matName)
    if not matName then return false end
    if RenderCore and RenderCore.IsMaterialAllowed then
        return RenderCore.IsMaterialAllowed(matName, CONVARS.MAT_WHITELIST:GetString(), CONVARS.MAT_BLACKLIST:GetString())
    end
    return true -- fallback allow
end

-- Pre-allocate reusable vectors to avoid GC pressure
local _tempCenter = Vector(0, 0, 0)

local function ValidateVertex(pos)
    if RenderCore and RenderCore.ValidateVertex then
        return RenderCore.ValidateVertex(pos)
    end
    if not pos or not pos.x or not pos.y or not pos.z then return false end
    if pos.x ~= pos.x or pos.y ~= pos.y or pos.z ~= pos.z then return false end
    if math.abs(pos.x) > 16384 or math.abs(pos.y) > 16384 or math.abs(pos.z) > 16384 then return false end
    return true
end

local function IsBrushEntity(face)
    if not face then return false end
    
    -- First check if it's a brush model
    if face.__bmodel and face.__bmodel > 0 then
        return true -- Any non-zero bmodel index indicates it's a brush entity
    end
    
    -- Secondary check for brush entities using parent entity
    local parent = face.__parent
    if parent and isentity(parent) and parent:GetClass() then
        -- If the face has a valid parent entity, it's likely a brush entity
        return true
    end
    
    return false
end

local function IsSkyboxFace(face)
    if not face then return false end
    
    local material = face:GetMaterial()
    if not material then return false end
    
    local matName = material:GetName():lower()
    
    return matName:find("tools/toolsskybox", 1, true) ~= nil or
           matName:find("skybox/", 1, true) ~= nil
end

-- Forward declare to allow usage in SplitChunk
local GetChunkKey

local function SplitChunk(faces, chunkSize)
    local subChunks = {}
    for _, face in ipairs(faces) do
        local vertices = face:GetVertexs()
        if not vertices or #vertices == 0 then continue end
        
        -- Calculate face center
        local center = Vector(0, 0, 0)
        for _, vert in ipairs(vertices) do
            center:Add(vert)
        end
        center:Div(#vertices)
        
        -- Use smaller chunk size for subdivision
        local subX = math_floor(center.x / (chunkSize/2))
        local subY = math_floor(center.y / (chunkSize/2))
        local subZ = math_floor(center.z / (chunkSize/2))
        local subKey = GetChunkKey(subX, subY, subZ)
        
        subChunks[subKey] = subChunks[subKey] or {}
        table_insert(subChunks[subKey], face)
    end
    return subChunks
end

local function DetermineOptimalChunkSize(totalFaces)
    -- Base chunk size on face density, but keep within reasonable bounds
    if not totalFaces or totalFaces <= 0 then return 65536 end
    local density = totalFaces / (16384 * 16384 * 16384) -- Approximate map volume
    local size = math_max(8192, math_min(131072, math_floor(1 / density * 32768)))
    print("[RTX Fixes] Auto-determined chunk size: " .. size .. " for " .. totalFaces .. " faces")
    return size
end

local function CreateMeshBatch(vertices, material, maxVertsPerMesh)
    local meshes = {}
    local currentVerts = {}
    local vertCount = 0
    
    for i = 1, #vertices, 3 do -- Process in triangles
        -- Add all three vertices of the triangle
        for j = 0, 2 do
            if vertices[i + j] then
                table_insert(currentVerts, vertices[i + j])
                vertCount = vertCount + 1
            end
        end
        
        -- Create new mesh when we hit the vertex limit
        if vertCount >= maxVertsPerMesh - 3 then -- Leave room for one more triangle
            local newMesh = Mesh(material)
            mesh.Begin(newMesh, MATERIAL_TRIANGLES, #currentVerts / 3)
            for _, vert in ipairs(currentVerts) do
                mesh.Position(vert.pos)
                mesh.Normal(vert.normal)
                mesh.TexCoord(0, vert.u or 0, vert.v or 0)
                mesh.Color(255, 255, 255, 255)
                mesh.AdvanceVertex()
            end
            mesh.End()
            
            table_insert(meshes, newMesh)
            if RenderCore and RenderCore.TrackMesh then
                RenderCore.TrackMesh(newMesh)
            end
            currentVerts = {}
            vertCount = 0
        end
    end
    
    -- Handle remaining vertices
    if #currentVerts > 0 then
        local newMesh = Mesh(material)
        mesh.Begin(newMesh, MATERIAL_TRIANGLES, #currentVerts / 3)
        for _, vert in ipairs(currentVerts) do
            mesh.Position(vert.pos)
            mesh.Normal(vert.normal)
            mesh.TexCoord(0, vert.u or 0, vert.v or 0)
            mesh.Color(255, 255, 255, 255)
            mesh.AdvanceVertex()
        end
        mesh.End()
        
        table_insert(meshes, newMesh)
        if RenderCore and RenderCore.TrackMesh then
            RenderCore.TrackMesh(newMesh)
        end
    end
    
    return meshes
end

GetChunkKey = function(x, y, z)
    -- Use integer hash from RenderCore instead of string concat
    if RenderCore and RenderCore.HashChunkKey then
        return RenderCore.HashChunkKey(x, y, z)
    end
    return x .. "," .. y .. "," .. z  -- Fallback
end

-- Cleanup helper with proper error tracking
local function CleanupMeshes()
    local destroyed = 0
    local failed = 0
    
    for renderType, chunks in pairs(mapMeshes) do
        for chunkKey, materials in pairs(chunks) do
            for matName, group in pairs(materials) do
                if group.meshes then
                    for _, m in ipairs(group.meshes) do
                        if m then
                            if RenderCore and RenderCore.DestroyMesh then
                                if RenderCore.DestroyMesh(m) then
                                    destroyed = destroyed + 1
                                else
                                    failed = failed + 1
                                end
                            else
                                local ok = pcall(function() if m.Destroy then m:Destroy() end end)
                                if ok then destroyed = destroyed + 1 else failed = failed + 1 end
                            end
                        end
                    end
                end
            end
        end
    end
    
    if failed > 0 then
        ErrorNoHalt("[RTX Fixes] Failed to destroy " .. failed .. " meshes during cleanup\n")
    end
    
    return destroyed, failed
end

-- Main Mesh Building Function
local function BuildMapMeshes(cancelToken)
    -- Clean up existing meshes first
    CleanupMeshes()

    mapMeshes = {
        opaque = {},
        translucent = {},
    }
    
    totalVertexCount = 0
    
    if not NikNaks or not NikNaks.CurrentMap then return end

    print("[RTX Fixes] Building chunked meshes...")
    local startTime = SysTime()
    
    -- Create separate mesh creation functions for regular faces and displacements
    local function CreateRegularMeshGroup(faces, material)
        if not faces or #faces == 0 or not material then return nil end
        
        -- Track chunk bounds
        local minBounds = Vector(math_huge, math_huge, math_huge)
        local maxBounds = Vector(-math_huge, -math_huge, -math_huge)
        
        -- Stream vertices to avoid massive intermediate arrays
        local meshes = {}
        local batchVerts = {}
        local batchCount = 0
        local processed = 0
        for _, face in ipairs(faces) do
            local verts = face:GenerateVertexTriangleData()
            if verts then
                local faceValid = true
                for _, vert in ipairs(verts) do
                    if not ValidateVertex(vert.pos) then
                        faceValid = false
                        break
                    end
                    
                    -- Update bounds
                    minBounds.x = math_min(minBounds.x, vert.pos.x)
                    minBounds.y = math_min(minBounds.y, vert.pos.y)
                    minBounds.z = math_min(minBounds.z, vert.pos.z)
                    maxBounds.x = math_max(maxBounds.x, vert.pos.x)
                    maxBounds.y = math_max(maxBounds.y, vert.pos.y)
                    maxBounds.z = math_max(maxBounds.z, vert.pos.z)
                end
                
                if faceValid then
                    for _, vert in ipairs(verts) do
                        batchCount = batchCount + 1
                        batchVerts[batchCount] = vert
                        if batchCount >= (MAX_VERTICES - 3) then
                            local chunkMeshes = CreateMeshBatch(batchVerts, material, MAX_VERTICES)
                            if chunkMeshes then
                                for i = 1, #chunkMeshes do
                                    table_insert(meshes, chunkMeshes[i])
                                end
                            end
                            totalVertexCount = totalVertexCount + batchCount
                            -- Check budget limit - mark token as cancelled for clean rollback
                            if totalVertexCount >= MAX_TOTAL_VERTICES then
                                ErrorNoHalt("[RTX Fixes] Vertex budget exceeded! Stopping mesh build at " .. totalVertexCount .. " vertices\n")
                                if cancelToken then cancelToken.cancelled = true end
                                return nil, nil, nil -- Signal failure
                            end
                            batchVerts = {}
                            batchCount = 0
                        end
                    end
                end
            end
            processed = processed + 1
            if processed % 128 == 0 then
                -- Cooperative yield during very large groups
                if coroutine.isyieldable and coroutine.isyieldable() then
                    coroutine.yield()
                end
            end
        end
        
        -- Flush any remaining vertices
        if batchCount > 0 then
            local chunkMeshes = CreateMeshBatch(batchVerts, material, MAX_VERTICES)
            if chunkMeshes then
                for i = 1, #chunkMeshes do
                    table_insert(meshes, chunkMeshes[i])
                end
            end
        end
        
        return meshes, minBounds, maxBounds
    end

    -- Create combined meshes with frame-budgeted coroutine
    local co
    co = coroutine.create(function()
        local frameStartTime = SysTime()
        local frameBudget = 0.003 -- start ~3ms per frame

        -- Prepare chunk table and inputs inside coroutine
        local chunks = { opaque = {}, translucent = {} }
        local chunkSize = CONVARS.CHUNK_SIZE:GetInt()
        if not chunkSize or chunkSize <= 0 then
            -- Auto-determine chunk size if not set or invalid
            local faceCount = 0
            if NikNaks and NikNaks.CurrentMap and NikNaks.CurrentMap.GetLeafs then
                local ok, leafs = pcall(function() return NikNaks.CurrentMap:GetLeafs() end)
                if ok and leafs then
                    for _, leaf in pairs(leafs) do
                        if leaf and leaf.GetFaces then
                            local ok2, faces = pcall(function() return leaf:GetFaces(false) end)
                            if ok2 and faces then
                                faceCount = faceCount + #faces
                            end
                        end
                    end
                end
            end
            chunkSize = DetermineOptimalChunkSize(faceCount)
        end

        -- Determine 3D skybox bounds (to exclude miniature skybox geometry from world pass)
        local hasSkyAABB = false
        local skyMins, skyMaxs
        if NikNaks.CurrentMap and NikNaks.CurrentMap.HasSkyBox and NikNaks.CurrentMap:HasSkyBox() and NikNaks.CurrentMap.GetSkyboxSize then
            local okSky, mins, maxs = pcall(function() return NikNaks.CurrentMap:GetSkyboxSize() end)
            if okSky and mins and maxs then
                hasSkyAABB = true
                skyMins, skyMaxs = mins, maxs
            end
        end

        -- Sort faces into chunks with time-budgeted yields
        local okLeafs, allLeafs = pcall(function() return NikNaks.CurrentMap:GetLeafs() end)
        if not okLeafs or not allLeafs then
            ErrorNoHalt("[RTX Fixes] GetLeafs failed\n")
            return
        end
        buildState.active = true
        buildState.processed = 0
        buildState.total = 0
        for _ in pairs(allLeafs) do buildState.total = buildState.total + 1 end
        local faceCheckCounter = 0
        for _, leaf in pairs(allLeafs) do  
            if cancelToken and cancelToken.cancelled then return end
            if leaf and not leaf:IsOutsideMap() then
                local okFaces, leafFaces = pcall(function() return leaf:GetFaces(true) end)
                if leafFaces then
                    for _, face in pairs(leafFaces) do
                        -- Check cancellation every 100 faces to avoid long delays
                        faceCheckCounter = faceCheckCounter + 1
                        if faceCheckCounter >= 100 then
                            faceCheckCounter = 0
                            if cancelToken and cancelToken.cancelled then return end
                        end
                        local process = true
                        if not face or face:IsDisplacement() or IsBrushEntity(face) or not face:ShouldRender() or IsSkyboxFace(face) then
                            process = false
                        end
                        if process then
                            local vertices = face:GetVertexs()
                            if not vertices or #vertices == 0 then
                                process = false
                            else
                                -- Optimized center calculation using reusable vector
                                _tempCenter:Zero()
                                local vertCount = #vertices
                                for i = 1, vertCount do
                                    local vert = vertices[i]
                                    if vert then _tempCenter:Add(vert) end
                                end
                                _tempCenter:Div(vertCount)
                                if hasSkyAABB and _tempCenter.WithinAABox and _tempCenter:WithinAABox(skyMins, skyMaxs) then
                                    process = false
                                else
                                    local chunkX = math_floor(_tempCenter.x / chunkSize)
                                    local chunkY = math_floor(_tempCenter.y / chunkSize)
                                    local chunkZ = math_floor(_tempCenter.z / chunkSize)
                                    local chunkKey = GetChunkKey(chunkX, chunkY, chunkZ)
                                    local material = face:GetMaterial()
                                    if material then
                                        local matName = material:GetName()
                                        if matName and IsMaterialAllowed(matName) then
                                            if RenderCore and RenderCore.GetMaterial then
                                                material = RenderCore.GetMaterial(matName)
                                            end
                                            local chunkGroup = face:IsTranslucent() and chunks.translucent or chunks.opaque
                                            chunkGroup[chunkKey] = chunkGroup[chunkKey] or {}
                                            local chunkData = chunkGroup[chunkKey]
                                            chunkData[matName] = chunkData[matName] or {
                                                material = material,
                                                faces = {}
                                            }
                                            table_insert(chunkData[matName].faces, face)
                                        end
                                    end
                                end
                            end
                        end
                        if SysTime() - frameStartTime > frameBudget then
                            coroutine.yield()
                            local spent = SysTime() - frameStartTime
                            if RenderCore and RenderCore.UpdateFrameBudget then
                                frameBudget = RenderCore.UpdateFrameBudget(spent, frameBudget)
                            else
                                -- Fallback: simple adaptation
                                if spent > frameBudget * 1.2 then
                                    frameBudget = math.max(0.001, frameBudget * 0.95)
                                elseif spent < frameBudget * 0.8 then
                                    frameBudget = math.min(0.006, frameBudget * 1.05)
                                end
                            end
                            frameStartTime = SysTime()
                            -- More frequent cancellation checks after yield
                            if cancelToken and cancelToken.cancelled then return end
                        end
                    end
                end
            end
            buildState.processed = buildState.processed + 1
            if SysTime() - frameStartTime > frameBudget then
                coroutine.yield()
                local spent = SysTime() - frameStartTime
                if RenderCore and RenderCore.UpdateFrameBudget then
                    frameBudget = RenderCore.UpdateFrameBudget(spent, frameBudget)
                else
                    if spent > frameBudget * 1.2 then
                        frameBudget = math.max(0.001, frameBudget * 0.95)
                    elseif spent < frameBudget * 0.8 then
                        frameBudget = math.min(0.006, frameBudget * 1.05)
                    end
                end
                frameStartTime = SysTime()
            end
        end

        for renderType, chunkGroup in pairs(chunks) do
            for chunkKey, materials in pairs(chunkGroup) do
                mapMeshes[renderType][chunkKey] = {}
                for matName, group in pairs(materials) do
                    if cancelToken and cancelToken.cancelled then return end
                    if group.faces and #group.faces > 0 then
                        local meshes, mins, maxs = CreateRegularMeshGroup(group.faces, group.material)
                        if meshes then
                            mapMeshes[renderType][chunkKey][matName] = {
                                meshes = meshes,
                                material = group.material
                            }
                            -- update chunk bounds
                            local chunkTable = mapMeshes[renderType][chunkKey]
                            if mins and maxs then
                                local cmins = chunkTable._mins
                                local cmaxs = chunkTable._maxs
                                if not cmins or not cmaxs then
                                    chunkTable._mins = mins
                                    chunkTable._maxs = maxs
                                else
                                    cmins.x = math_min(cmins.x, mins.x)
                                    cmins.y = math_min(cmins.y, mins.y)
                                    cmins.z = math_min(cmins.z, mins.z)
                                    cmaxs.x = math_max(cmaxs.x, maxs.x)
                                    cmaxs.y = math_max(cmaxs.y, maxs.y)
                                    cmaxs.z = math_max(cmaxs.z, maxs.z)
                                end
                            end
                        end
                    end
                    if cancelToken and cancelToken.cancelled then return end
                    if SysTime() - frameStartTime > frameBudget then
                        coroutine.yield()
                        local spent = SysTime() - frameStartTime
                        if RenderCore and RenderCore.UpdateFrameBudget then
                            frameBudget = RenderCore.UpdateFrameBudget(spent, frameBudget)
                        else
                            if spent > frameBudget * 1.2 then
                                frameBudget = math.max(0.001, frameBudget * 0.95)
                            elseif spent < frameBudget * 0.8 then
                                frameBudget = math.min(0.006, frameBudget * 1.05)
                            end
                        end
                        frameStartTime = SysTime()
                    end
                end
            end
        end
        buildState.active = false
        print(string.format("[RTX Fixes] Built chunked meshes in %.2f seconds (total vertices: %d, memory: ~%.1fMB)", 
            SysTime() - frameStartTime, totalVertexCount, (totalVertexCount * 40) / (1024 * 1024)))
    end)

    -- Drive the coroutine over frames via RenderCore job scheduler (less timer overhead)
    local jobId = "RTXWorldMeshBuildJob"
    RenderCore.ScheduleJob(jobId, function()
        if not co or coroutine.status(co) == "dead" then
            buildState.active = false
            return false
        end
        
        local ok, err = coroutine.resume(co)
        if not ok then
            ErrorNoHalt("[RTX Fixes] Build coroutine error: " .. tostring(err) .. "\n")
            buildState.active = false
            buildState.processed = 0
            buildState.total = 0
            -- Clean up partial meshes
            CleanupMeshes()
            mapMeshes = { opaque = {}, translucent = {} }
            return false
        end
        
        -- Check if cancelled
        if cancelToken and cancelToken.cancelled then
            buildState.active = false
            print("[RTX Fixes] Build cancelled, cleaning up...")
            CleanupMeshes()
            mapMeshes = { opaque = {}, translucent = {} }
            return false
        end
        
        local isDead = coroutine.status(co) == "dead"
        if isDead then
            buildState.active = false
        end
        return not isDead
    end)

end

-- Rendering Functions
local function RenderCustomWorld(translucent)
    if not isEnabled then return end
    
    -- Skip rendering world in offscreen RTs (e.g., rear-view camera with dynamic_only filter)
    if RenderCore and RenderCore.IsOffscreen and RenderCore.IsOffscreen() then
        return
    end

    local draws = 0
    local chunksVisited = 0
    
    -- Regular faces
    local groups = translucent and mapMeshes.translucent or mapMeshes.opaque

    for _, chunkMaterials in pairs(groups) do
        chunksVisited = chunksVisited + 1
        for key, group in pairs(chunkMaterials) do
            if key == "_mins" or key == "_maxs" then continue end
            if not group or not group.meshes then continue end
            -- Submit meshes to central render queue
            local meshes = group.meshes
            for i = 1, #meshes do
                local m = meshes[i]
                if m then
                    RenderCore.Submit({
                        material = group.material,
                        mesh = m,
                        translucent = translucent
                    })
                    draws = draws + 1
                end
            end
        end
    end
    
    renderStats.draws = draws
    renderStats.chunksVisited = chunksVisited
end

-- Stats provider for unified overlay
RenderCore.RegisterStats("RTXWorld", function()
    local extra = ""
    if buildState.active and (buildState.total or 0) > 0 then
        extra = string.format(" | build: %d/%d", buildState.processed or 0, buildState.total or 0)
    end
    return string.format("World draws: %d | chunks: %d%s",
        renderStats.draws or 0,
        renderStats.chunksVisited or 0,
        extra)
end)

-- Enable/Disable Functions
local function EnableCustomRendering()
    if isEnabled then return end
    isEnabled = true
    
    RenderCore.Register("PreDrawOpaqueRenderables", "RTXCustomWorldOpaque", function()
        RenderCustomWorld(false)
    end)
    
    RenderCore.Register("PreDrawTranslucentRenderables", "RTXCustomWorldTranslucent", function()
        RenderCustomWorld(true)
    end)
end

local function DisableCustomRendering()
    if not isEnabled then return end
    isEnabled = false

    RenderCore.Unregister("PreDrawOpaqueRenderables", "RTXCustomWorldOpaque")
    RenderCore.Unregister("PreDrawTranslucentRenderables", "RTXCustomWorldTranslucent")
end

-- Initialization and Cleanup
local function Initialize(token)
    local success, err = pcall(BuildMapMeshes, token)
    if not success then
        ErrorNoHalt("[RTX Fixes] Failed to build meshes: " .. tostring(err) .. "\n")
        DisableCustomRendering()
        return
    end
    
    timer.Simple(1, function()
        if CONVARS.ENABLED:GetBool() then
            local success, err = pcall(EnableCustomRendering)
            if not success then
                ErrorNoHalt("[RTX Fixes] Failed to enable custom rendering: " .. tostring(err) .. "\n")
                DisableCustomRendering()
            end
        end
    end)
end

-- Hooks
RenderCore.Register("InitPostEntity", "RTXMeshInit", Initialize)

RenderCore.Register("PostCleanupMap", "RTXMeshRebuild", function()
    RenderCore.RequestRebuild("PostCleanupMap")
end)

RenderCore.Register("ShutDown", "RTXCustomWorldShutdown", function()
    DisableCustomRendering()
    -- Rely on RenderCore global cleanup for tracked meshes; just clear tables locally
    mapMeshes = { opaque = {}, translucent = {} }
end)

-- ConVar Changes
cvars.AddChangeCallback("rtx_mwr_enable", function(_, _, new)
    if tobool(new) then
        EnableCustomRendering()
    else
        DisableCustomRendering()
    end
end)

cvars.AddChangeCallback("rtx_mwr_capture_mode", function(_, _, new)
    -- Invert the value: if capture_mode is 1, r_drawworld should be 0 and vice versa
    RunConsoleCommand("r_drawworld", new == "1" and "0" or "1")
end)

-- Rebuild sinks and debounce on relevant ConVars
RenderCore.RegisterRebuildSink("RTXMeshRebuildSink", function(token, reason)
    Initialize(token)
end)

local function DebounceRebuildOnCvar(name)
    cvars.AddChangeCallback(name, function()
        RenderCore.RequestRebuild(name)
    end, "RTXMeshRebuild-" .. name)
end

 DebounceRebuildOnCvar("rtx_mwr_chunk_size")
 DebounceRebuildOnCvar("rtx_mwr_mat_whitelist")
 DebounceRebuildOnCvar("rtx_mwr_mat_blacklist")

-- Console Commands
concommand.Add("rtx_rebuild_meshes", BuildMapMeshes)
