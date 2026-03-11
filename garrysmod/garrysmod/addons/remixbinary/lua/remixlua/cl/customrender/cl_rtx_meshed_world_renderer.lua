-- Disables source engine world rendering and replaces it with chunked mesh rendering instead, fixes engine culling issues. 
-- MAJOR THANK YOU to the creator of NikNaks, a lot of this would not be possible without it.
if not CLIENT then return end
require("niknaks")
local RenderCore = include("remixlua/cl/customrender/render_core.lua") or RemixRenderCore

-- ConVars
local CONVARS = {
    ENABLED = CreateClientConVar("rtx_mwr_enable", "1", true, false, "Forces custom mesh rendering of map"),
    DEBUG = CreateClientConVar("rtx_mwr_debug", "0", true, false, "Shows debug info for mesh rendering"),
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
local skyboxWorldMatrix = nil -- Matrix to transform skybox-space meshes to world-space at render time
local flatOpaqueList = {}
local flatTranslucentList = {}

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

local function CreateMeshBatch(vertices, material, maxVertsPerMesh)
    local meshes = {}
    local n = #vertices
    if n == 0 then return meshes end
    local i = 1
    while i <= n do
        local batchSize = n - i + 1
        if batchSize > maxVertsPerMesh then batchSize = maxVertsPerMesh end
        batchSize = batchSize - (batchSize % 3) -- keep on a triangle boundary
        if batchSize <= 0 then break end
        local newMesh = Mesh(material)
        mesh.Begin(newMesh, MATERIAL_TRIANGLES, batchSize / 3)
        local endIdx = i + batchSize - 1
        for vi = i, endIdx do
            local v = vertices[vi]
            mesh.Position(v.pos)
            mesh.Normal(v.normal)
            mesh.TexCoord(0, v.u or 0, v.v or 0)
            mesh.Color(255, 255, 255, 255)
            mesh.AdvanceVertex()
        end
        mesh.End()
        meshes[#meshes + 1] = newMesh
        if RenderCore and RenderCore.TrackMesh then
            RenderCore.TrackMesh(newMesh)
        end
        i = endIdx + 1
    end
    return meshes
end

-- Morton-code (Z-order curve) spatial sort for face records.
-- Triangles sorted by Morton code are spatially coherent, which gives the
-- GPU BVH builder better BLAS quality and improves ray traversal performance.
--
-- Each face record is {face=..., cx=..., cy=..., cz=...} where cx/cy/cz is the
-- face centroid.  Source maps fit inside ±32768 world units; we map that range
-- to [0, 1023] for 10-bit per-axis Morton encoding (30-bit total code).
local _bit = bit
local _MORTON_HALF  = 32768.0
local _MORTON_SCALE = 1023.0 / 65536.0  -- maps [-32768, +32768] → [0, 1023]

local function _mortonSplit3(x)
    -- Spread 10 bits into every third bit position (bit 0, 3, 6, … 27)
    x = _bit.band(x, 0x3ff)
    x = _bit.band(_bit.bor(x, _bit.lshift(x, 16)), 0x30000ff)
    x = _bit.band(_bit.bor(x, _bit.lshift(x,  8)), 0x300f00f)
    x = _bit.band(_bit.bor(x, _bit.lshift(x,  4)), 0x30c30c3)
    x = _bit.band(_bit.bor(x, _bit.lshift(x,  2)), 0x9249249)
    return x
end

local function _mortonCode3(ix, iy, iz)
    return _bit.bor(_mortonSplit3(ix),
                    _bit.lshift(_mortonSplit3(iy), 1),
                    _bit.lshift(_mortonSplit3(iz), 2))
end

local function SortFaceRecordsByMorton(recs)
    local scale  = _MORTON_SCALE
    local half   = _MORTON_HALF
    local clamp0 = math.max
    local min1   = math.min
    local ifloor = math_floor
    for i = 1, #recs do
        local r  = recs[i]
        local ix = clamp0(0, min1(1023, ifloor((r.cx + half) * scale)))
        local iy = clamp0(0, min1(1023, ifloor((r.cy + half) * scale)))
        local iz = clamp0(0, min1(1023, ifloor((r.cz + half) * scale)))
        r._morton = _mortonCode3(ix, iy, iz)
    end
    table.sort(recs, function(a, b) return a._morton < b._morton end)
end

-- Cleanup helper with proper error tracking
local function CleanupMeshes()
    local destroyed = 0
    local failed = 0
    
    for renderType, matGroups in pairs(mapMeshes) do
        for _, group in pairs(matGroups) do
            if group.meshes then
                for _, m in ipairs(group.meshes) do
                    if m then
                        if RenderCore and RenderCore.DestroyMesh then
                            local result = RenderCore.DestroyMesh(m)
                            if result == true then
                                destroyed = destroyed + 1
                            elseif result == nil then
                                -- Mesh was not tracked: already cleaned up by another system
                                -- (e.g. DestroyTrackedMeshes from a concurrent rebuild sink).
                                -- This is not a failure; count it as gone.
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
    
    if failed > 0 then
        ErrorNoHalt("[RTX Fixes] Failed to destroy " .. failed .. " meshes during cleanup\n")
    end
    
    return destroyed, failed
end

-- Flatten mapMeshes into two sequential arrays of pre-initialized Submit item tables.
-- Called once after the build coroutine finishes. The flat arrays are then reused on every render frame
local function BuildFlatRenderList()
    local oList = {}
    local tList = {}
    local function fill(renderGroups, list, isTranslucent)
        for _, group in pairs(renderGroups) do
            if not group or not group.meshes then continue end
            local skyMatrix = (group.isSkybox and not group.bakedToWorld) and skyboxWorldMatrix or nil
            local isSkybox  = group.isSkybox or false
            local mat       = group.material
            local meshList  = group.meshes
            for i = 1, #meshList do
                local m = meshList[i]
                if m then
                    list[#list + 1] = {
                        material    = mat,
                        mesh        = m,
                        matrix      = skyMatrix,
                        translucent = isTranslucent,
                        isSkybox    = isSkybox,
                    }
                end
            end
        end
    end
    fill(mapMeshes.opaque,      oList, false)
    fill(mapMeshes.translucent, tList, true)
    flatOpaqueList      = oList
    flatTranslucentList = tList
    print(string.format("[WorldRenderer] Flat render list: %d opaque + %d translucent items",
        #oList, #tList))
end

-- Main Mesh Building Function
local function BuildMapMeshes(cancelToken)
    -- Clean up existing meshes first
    CleanupMeshes()

    mapMeshes = {
        opaque = {},
        translucent = {},
    }

    flatOpaqueList      = {}
    flatTranslucentList = {}
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
        for _, rec in ipairs(faces) do
            local verts = rec.face:GenerateVertexTriangleData()
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

        -- Global material groups: all faces sharing the same material go into one group
        -- regardless of position.  This minimises TLAS entries and lets the BVH builder
        -- see the full set of triangles for each material at once.
        local chunks = { opaque = {}, translucent = {} }

        -- Determine 3D skybox bounds and transform parameters
        local hasSkyAABB = false
        local skyMins, skyMaxs
        local skyPos, skyScale
        skyboxWorldMatrix = nil
        if NikNaks.CurrentMap and NikNaks.CurrentMap.HasSkyBox and NikNaks.CurrentMap:HasSkyBox() and NikNaks.CurrentMap.GetSkyboxSize then
            local okSky, mins, maxs = pcall(function() return NikNaks.CurrentMap:GetSkyboxSize() end)
            if okSky and mins and maxs then
                hasSkyAABB = true
                skyMins, skyMaxs = mins, maxs
                skyPos = NikNaks.CurrentMap:GetSkyBoxPos()
                skyScale = NikNaks.CurrentMap:GetSkyBoxScale() or 16
                -- Build a matrix: worldPos = (pos - skyPos) * skyScale
                -- Matrix order: Scale first, then Translate by -skyPos*skyScale
                local m = Matrix()
                m:Scale(Vector(skyScale, skyScale, skyScale))
                m:Translate(-skyPos)
                skyboxWorldMatrix = m
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

        -- Accumulate world (non-skybox) face vertex bounds during the face scan
        -- so skybox faces can later be clipped to remove overlap with the main map.
        local worldFaceMins = Vector(math_huge, math_huge, math_huge)
        local worldFaceMaxs = Vector(-math_huge, -math_huge, -math_huge)

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
                                -- Check if this face is inside the 3D skybox area
                                local isInSkybox = hasSkyAABB and _tempCenter.WithinAABox and _tempCenter:WithinAABox(skyMins, skyMaxs)

                                -- Expand world AABB from non-skybox face vertices
                                if not isInSkybox then
                                    for _, vert in ipairs(vertices) do
                                        if vert.x < worldFaceMins.x then worldFaceMins.x = vert.x end
                                        if vert.y < worldFaceMins.y then worldFaceMins.y = vert.y end
                                        if vert.z < worldFaceMins.z then worldFaceMins.z = vert.z end
                                        if vert.x > worldFaceMaxs.x then worldFaceMaxs.x = vert.x end
                                        if vert.y > worldFaceMaxs.y then worldFaceMaxs.y = vert.y end
                                        if vert.z > worldFaceMaxs.z then worldFaceMaxs.z = vert.z end
                                    end
                                end

                                local material = face:GetMaterial()
                                if material then
                                    local matName = material:GetName()
                                    if matName and IsMaterialAllowed(matName) then
                                        if RenderCore and RenderCore.GetMaterial then
                                            material = RenderCore.GetMaterial(matName)
                                        end
                                        local chunkGroup = face:IsTranslucent() and chunks.translucent or chunks.opaque
                                        -- Include skybox flag in key so world and skybox faces with
                                        -- the same material never get merged into one group (wrong matrix)
                                        local groupKey = isInSkybox and (matName .. "\0sky") or matName
                                        chunkGroup[groupKey] = chunkGroup[groupKey] or {
                                            material = material,
                                            faces    = {},
                                            isSkybox = isInSkybox
                                        }
                                        -- Store face + centroid so Morton sort can run after the scan
                                        local rec = chunkGroup[groupKey].faces
                                        rec[#rec + 1] = {
                                            face = face,
                                            cx   = _tempCenter.x,
                                            cy   = _tempCenter.y,
                                            cz   = _tempCenter.z,
                                        }
                                    end
                                end
                            end
                        end
                        if SysTime() - frameStartTime > frameBudget then
                            -- Publish world face bounds incrementally so the displacement
                            -- renderer (running concurrently) can use them at clip time.
                            if hasSkyAABB and worldFaceMins.x < math_huge and RenderCore and RenderCore.ExpandWorldGeometryAABB then
                                RenderCore.ExpandWorldGeometryAABB(worldFaceMins, worldFaceMaxs)
                            end
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

        -- Publish world face bounds so other renderers (and prop culling) can use them.
        local hasWorldFaceAABB = worldFaceMins.x < math_huge
        if hasSkyAABB then
            if hasWorldFaceAABB then
                if RenderCore and RenderCore.ExpandWorldGeometryAABB then
                    RenderCore.ExpandWorldGeometryAABB(worldFaceMins, worldFaceMaxs)
                end
                print(string.format("[WorldRenderer] World face AABB: (%.0f,%.0f,%.0f)-(%.0f,%.0f,%.0f)",
                    worldFaceMins.x, worldFaceMins.y, worldFaceMins.z, worldFaceMaxs.x, worldFaceMaxs.y, worldFaceMaxs.z))
            else
                print("[WorldRenderer] Sky present but no world faces found — no clip possible")
            end
        else
            print("[WorldRenderer] No 3D skybox detected, skipping clip pass")
        end

        -- Clipping stats accumulated across CreateSkyboxMeshGroupBaked calls
        local mwrSkyClipStats = { groupsIn = 0, trisIn = 0, trisOut = 0, groupsEmpty = 0 }

        -- Auto clip margin: scan projected sky face vertices (cheap GetVertexs(), no triangle data)
        -- to measure how far sky geometry bleeds above the map floor and into/past the world AABB.
        --
        -- The algorithm only activates when at least one projected sky vertex lands INSIDE the
        -- world AABB while at or above the map's floor level (Z >= worldFaceMins.z).  On maps
        -- where the 3D sky is correctly placed outside the map, no such vertex exists and the
        -- auto margin is 0 (no extra clipping beyond the base AABB).
        --
        -- When bleed is detected the margin is: max capped overshoot + 1 skybox-unit buffer.
        -- "Capped overshoot" is the farthest outside-AABB distance of any above-floor sky vertex
        -- that is still within skyScale*64 world units of the AABB edge.  Vertices beyond that
        -- threshold are legitimate horizon terrain and do not inflate the margin.
        local autoClipMargin = 0
        if hasSkyAABB and hasWorldFaceAABB and skyPos and skyScale then
            local spx, spy, spz = skyPos.x, skyPos.y, skyPos.z
            local sc              = skyScale
            local wmx, wmy, wmz   = worldFaceMins.x, worldFaceMins.y, worldFaceMins.z
            local wMx, wMy        = worldFaceMaxs.x, worldFaceMaxs.y
            local cap             = sc * 64
            local hasInside       = false
            local maxOvershoot    = 0
            local function scanSkyFaces(chunkSet)
                for _, groupData in pairs(chunkSet) do
                    if groupData.isSkybox then
                        for _, rec in ipairs(groupData.faces) do
                            local verts = rec.face:GetVertexs()
                            if verts then
                                for _, vert in ipairs(verts) do
                                    local px = (vert.x - spx) * sc
                                    local py = (vert.y - spy) * sc
                                    local pz = (vert.z - spz) * sc
                                    if pz >= wmz then
                                        if px >= wmx and px <= wMx and py >= wmy and py <= wMy then
                                            hasInside = true
                                        else
                                            local ox = math.max(0, px - wMx, wmx - px)
                                            local oy = math.max(0, py - wMy, wmy - py)
                                            local ov = math.max(ox, oy)
                                            if ov <= cap and ov > maxOvershoot then
                                                maxOvershoot = ov
                                            end
                                        end
                                    end
                                end
                            end
                        end
                    end
                end
            end
            scanSkyFaces(chunks.opaque)
            scanSkyFaces(chunks.translucent)
            if hasInside then
                autoClipMargin = maxOvershoot + sc
                print(string.format("[WorldRenderer] Auto clip margin: %.0f (%.0f overshoot + %.0f skyunit buffer, cap=%.0f)",
                    autoClipMargin, maxOvershoot, sc, cap))
            else
                print("[WorldRenderer] Auto clip margin: 0 — no sky geometry bleeds above map floor")
            end
        end

        -- Compute the clip AABB once here, before any group is built.
        -- Computing it per-call inside the function creates a race: the displacement renderer
        -- publishes its AABB incrementally, so later groups would get a larger clip volume
        -- than earlier ones, making the result non-deterministic across reloads.
        local skyClipMins, skyClipMaxs
        if hasSkyAABB and skyPos and skyScale then
            local manualMargin = (RenderCore and RenderCore.GetSkyClipMargin and RenderCore.GetSkyClipMargin()) or 0
            local margin = (manualMargin > 0) and manualMargin or autoClipMargin
            if hasWorldFaceAABB then
                skyClipMins = Vector(worldFaceMins.x, worldFaceMins.y, worldFaceMins.z)
                skyClipMaxs = Vector(worldFaceMaxs.x, worldFaceMaxs.y, worldFaceMaxs.z)
            end
            if RenderCore and RenderCore.GetWorldGeometryAABB then
                local sm, sM = RenderCore.GetWorldGeometryAABB()
                if sm then
                    if skyClipMins then
                        if sm.x < skyClipMins.x then skyClipMins.x = sm.x end
                        if sm.y < skyClipMins.y then skyClipMins.y = sm.y end
                        if sm.z < skyClipMins.z then skyClipMins.z = sm.z end
                        if sM.x > skyClipMaxs.x then skyClipMaxs.x = sM.x end
                        if sM.y > skyClipMaxs.y then skyClipMaxs.y = sM.y end
                        if sM.z > skyClipMaxs.z then skyClipMaxs.z = sM.z end
                    else
                        skyClipMins = Vector(sm.x, sm.y, sm.z)
                        skyClipMaxs = Vector(sM.x, sM.y, sM.z)
                    end
                end
            end
            if skyClipMins and margin > 0 then
                skyClipMins.x = skyClipMins.x - margin
                skyClipMins.y = skyClipMins.y - margin
                skyClipMaxs.x = skyClipMaxs.x + margin
                skyClipMaxs.y = skyClipMaxs.y + margin
            end
            -- Also extend the clip volume downward by one skyScale unit so that
            -- skybox geometry below the map floor (e.g. Group #2 flat at Z=-1888)
            -- is removed when it falls within the XY clip footprint.
            -- We do NOT extend upward — above the clip AABB's Z max the sky is open.
            if skyClipMins and skyScale then
                skyClipMins.z = skyClipMins.z - (skyScale * 256)
            end
            local marginSrc = (manualMargin > 0) and "manual" or "auto"
            print(string.format("[WorldRenderer] Sky bake params: skyPos=(%.1f,%.1f,%.1f) skyScale=%.1f  clipMargin=%.0f (%s)",
                skyPos.x, skyPos.y, skyPos.z, skyScale, margin, marginSrc))
            if skyClipMins then
                print(string.format("[WorldRenderer] Sky clip AABB: (%.0f,%.0f,%.0f)-(%.0f,%.0f,%.0f)",
                    skyClipMins.x, skyClipMins.y, skyClipMins.z, skyClipMaxs.x, skyClipMaxs.y, skyClipMaxs.z))
            end
        end

        -- Build skybox face groups with world-space baking and AABB clipping.
        -- Vertices are transformed from skybox space to world space, then the triangles
        -- that overlap the main map are removed via Sutherland-Hodgman plane clipping.
        local function CreateSkyboxMeshGroupBaked(faces, material)
            if not faces or #faces == 0 or not material then return nil end
            if not skyPos or not skyScale then return CreateRegularMeshGroup(faces, material) end

            -- Use the pre-computed clip AABB (skyClipMins/skyClipMaxs captured from outer scope).
            local wMins = skyClipMins
            local wMaxs = skyClipMaxs
            local spx, spy, spz = skyPos.x, skyPos.y, skyPos.z
            local sc = skyScale

            -- Collect all face vertices, bake to world space
            local allVerts = {}
            for _, rec in ipairs(faces) do
                local verts = rec.face:GenerateVertexTriangleData()
                if verts then
                    for _, vert in ipairs(verts) do
                        if ValidateVertex(vert.pos) then
                            local p = vert.pos
                            allVerts[#allVerts + 1] = {
                                pos    = Vector((p.x - spx) * sc, (p.y - spy) * sc, (p.z - spz) * sc),
                                normal = vert.normal,
                                u      = vert.u,
                                v      = vert.v,
                            }
                        end
                    end
                end
            end
            if #allVerts == 0 then return nil end

            -- Clip baked triangles against the main map AABB
            mwrSkyClipStats.groupsIn = mwrSkyClipStats.groupsIn + 1
            mwrSkyClipStats.trisIn   = mwrSkyClipStats.trisIn + #allVerts
            if wMins and wMaxs and RenderCore and RenderCore.ClipTrianglesOutsideAABB then
                allVerts = RenderCore.ClipTrianglesOutsideAABB(allVerts, wMins, wMaxs)
            end
            if not allVerts or #allVerts == 0 then
                mwrSkyClipStats.groupsEmpty = mwrSkyClipStats.groupsEmpty + 1
                return nil
            end
            mwrSkyClipStats.trisOut = mwrSkyClipStats.trisOut + #allVerts

            -- Compute bounds of the result
            local minBounds = Vector(math_huge, math_huge, math_huge)
            local maxBounds = Vector(-math_huge, -math_huge, -math_huge)
            for _, vert in ipairs(allVerts) do
                local p = vert.pos
                minBounds.x = math_min(minBounds.x, p.x)
                maxBounds.x = math_max(maxBounds.x, p.x)
                minBounds.y = math_min(minBounds.y, p.y)
                maxBounds.y = math_max(maxBounds.y, p.y)
                minBounds.z = math_min(minBounds.z, p.z)
                maxBounds.z = math_max(maxBounds.z, p.z)
            end
            print(string.format("[WorldRenderer] Baked sky group #%d world bounds: (%.0f,%.0f,%.0f)-(%.0f,%.0f,%.0f)  verts=%d",
                mwrSkyClipStats.groupsIn,
                minBounds.x, minBounds.y, minBounds.z, maxBounds.x, maxBounds.y, maxBounds.z,
                #allVerts))

            -- Batch into IMesh objects
            local meshes = {}
            local batchVerts = {}
            local batchCount = 0
            local function flushBatch()
                if batchCount == 0 then return end
                local chunkMeshes = CreateMeshBatch(batchVerts, material, MAX_VERTICES)
                if chunkMeshes then
                    for _, m in ipairs(chunkMeshes) do table_insert(meshes, m) end
                end
                totalVertexCount = totalVertexCount + batchCount
                batchVerts = {}
                batchCount = 0
            end
            for _, vert in ipairs(allVerts) do
                batchCount = batchCount + 1
                batchVerts[batchCount] = vert
                if batchCount >= MAX_VERTICES - 3 then
                    flushBatch()
                    if totalVertexCount >= MAX_TOTAL_VERTICES then
                        if cancelToken then cancelToken.cancelled = true end
                        return nil, nil, nil
                    end
                end
            end
            flushBatch()
            return meshes, minBounds, maxBounds
        end

        for renderType, matGroups in pairs(chunks) do
            for matName, group in pairs(matGroups) do
                if cancelToken and cancelToken.cancelled then return end
                if group.faces and #group.faces > 0 then
                    -- Sort triangles spatially before baking so the BVH builder sees
                    -- coherent primitive runs, improving BLAS quality and traversal speed.
                    SortFaceRecordsByMorton(group.faces)

                    local meshes, mins, maxs
                    if group.isSkybox and skyPos then
                        meshes, mins, maxs = CreateSkyboxMeshGroupBaked(group.faces, group.material)
                    else
                        meshes, mins, maxs = CreateRegularMeshGroup(group.faces, group.material)
                    end
                    if meshes then
                        mapMeshes[renderType][matName] = {
                            meshes       = meshes,
                            material     = group.material,
                            isSkybox     = group.isSkybox or false,
                            bakedToWorld = (group.isSkybox and skyPos ~= nil) or false,
                        }
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
        buildState.active = false
        BuildFlatRenderList()
        print(string.format("[RTX Fixes] Built chunked meshes in %.2f seconds (total vertices: %d, memory: ~%.1fMB)", 
            SysTime() - frameStartTime, totalVertexCount, (totalVertexCount * 40) / (1024 * 1024)))
        if mwrSkyClipStats.groupsIn > 0 then
            local pctRemoved = (1 - mwrSkyClipStats.trisOut / math.max(1, mwrSkyClipStats.trisIn)) * 100
            print(string.format("[WorldRenderer] Sky clip: %d material groups in → %d empty (fully removed)  |  %d/%d tris kept (%.1f%% removed)",
                mwrSkyClipStats.groupsIn, mwrSkyClipStats.groupsEmpty,
                mwrSkyClipStats.trisOut / 3, mwrSkyClipStats.trisIn / 3, pctRemoved))
        else
            print("[WorldRenderer] Sky clip: no skybox face groups processed")
        end
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
    if RenderCore and RenderCore.IsOffscreen and RenderCore.IsOffscreen() then return end

    local list = translucent and flatTranslucentList or flatOpaqueList
    local n = #list
    if n == 0 then return end

    -- Hoist the skybox-enabled check outside the loop and pick a fast path.
    local sky3DEnabled = not (RenderCore and RenderCore.Is3DSkyEnabled) or RenderCore.Is3DSkyEnabled()
    local draws = 0
    if sky3DEnabled then
        for i = 1, n do
            RenderCore.Submit(list[i])
        end
        draws = n
    else
        for i = 1, n do
            local item = list[i]
            if not item.isSkybox then
                RenderCore.Submit(item)
                draws = draws + 1
            end
        end
    end

    renderStats.draws = draws
    renderStats.chunksVisited = n
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
        if RenderCore.IsInSkyboxPass and RenderCore.IsInSkyboxPass() then return end
        RenderCustomWorld(false)
    end)
    
    RenderCore.Register("PreDrawTranslucentRenderables", "RTXCustomWorldTranslucent", function()
        if RenderCore.IsInSkyboxPass and RenderCore.IsInSkyboxPass() then return end
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
RenderCore.Register("InitPostEntity", "RTXMeshInit", function(token)
    if not CONVARS.ENABLED:GetBool() then return end
    Initialize(token)
end)


RenderCore.Register("ShutDown", "RTXCustomWorldShutdown", function()
    DisableCustomRendering()
    -- Rely on RenderCore global cleanup for tracked meshes; just clear tables locally
    mapMeshes = { opaque = {}, translucent = {} }
end)

-- ConVar Changes
cvars.AddChangeCallback("rtx_mwr_enable", function(_, _, new)
    if tobool(new) then
        if not buildState.active and not next(mapMeshes.opaque) and not next(mapMeshes.translucent) then
            Initialize()
        else
            EnableCustomRendering()
        end
    else
        DisableCustomRendering()
    end
end)

cvars.AddChangeCallback("rtx_mwr_capture_mode", function(_, _, new)
    -- In capture mode, temporarily restore engine world drawing by disabling the patch.
    -- r_drawworld stays at 1 so the engine always collects surfaces for overlays/decals.
    RunConsoleCommand("rtx_patch_skip_world_draw", new == "1" and "0" or "1")
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

 DebounceRebuildOnCvar("rtx_mwr_mat_whitelist")
 DebounceRebuildOnCvar("rtx_mwr_mat_blacklist")

-- Console Commands
concommand.Add("rtx_rebuild_meshes", BuildMapMeshes)
