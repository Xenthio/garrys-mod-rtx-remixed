if not CLIENT then return end
require("niknaks")
local RenderCore = include("remixlua/cl/customrender/render_core.lua") or RemixRenderCore

-- ConVars
local CONVARS = {
    ENABLED = CreateClientConVar("rtx_dpr_enable", "1", true, false, "Enable custom displacement rendering"),
    DEBUG = CreateClientConVar("rtx_dpr_debug", "0", true, false, "Debug prints for displacement renderer"),
    CHUNK_SIZE = CreateClientConVar("rtx_dpr_chunk_size", "65536", true, false, "Size of chunks for displacement grouping"),
    MAT_WHITELIST = CreateClientConVar("rtx_dpr_mat_whitelist", "", true, false, "Comma-separated material name substrings to include"),
    MAT_BLACKLIST = CreateClientConVar("rtx_dpr_mat_blacklist", "toolsskybox,skybox/", true, false, "Comma-separated material name substrings to exclude"),
    ALPHA_SCALE = CreateClientConVar("rtx_dpr_alpha_scale", "0", true, false, "Alpha scaling: 0=auto-normalize, >0=manual scale"),
    SEAM_EPSILON = CreateClientConVar("rtx_dpr_seam_epsilon", "0.1", true, false, "Position tolerance for vertex matching (0.1 = good default, higher = more matching)")
}

local function DebugPrint(...)
    if CONVARS.DEBUG:GetBool() then
        print("[DispRenderer]", ...)
    end
end

-- Local state
local dispMeshes = {}
-- dispMeshes[chunkKey] = {
--   _mins=Vector,_maxs=Vector,_clusters={ [cluster]=true },
--   [matKey] = { material=IMaterial, meshes=IMesh[] }
-- }

local buildState = { active = false, processed = 0, total = 0 }
-- Expose build state for progress tracking
if RemixRenderCore then RemixRenderCore._dispBuildState = buildState end
local stats = { draws = 0, chunksVisited = 0 }
local lastDebugPrint = 0
local skyboxWorldMatrix = nil -- Matrix to transform skybox-space meshes to world-space at render time


local function IsMaterialAllowed(matName)
    if not matName then return false end
    if RenderCore and RenderCore.IsMaterialAllowed then
        return RenderCore.IsMaterialAllowed(matName, CONVARS.MAT_WHITELIST:GetString(), CONVARS.MAT_BLACKLIST:GetString())
    end
    return true
end

local function GetChunkKey(x, y, z)
    -- Use integer hash from RenderCore instead of string concat
    if RenderCore and RenderCore.HashChunkKey then
        return RenderCore.HashChunkKey(x, y, z)
    end
    return x .. "," .. y .. "," .. z  -- Fallback
end

-- Try to get or build a material that supports 2-texture blending.
-- Prefer the face's material if it already has $basetexture2; otherwise try a dynamic WorldVertexTransition.
local dispMatCache = {}
local function GetDispBlendMaterial(faceMat)
    if not faceMat then return nil end
    -- Normalize to a shared material instance by name so adjacent faces use the same IMaterial object
    local matName = faceMat.GetName and faceMat:GetName() or nil
    local shared = (RenderCore and RenderCore.GetMaterial and matName) and RenderCore.GetMaterial(matName) or faceMat

    local baseTex = shared.GetTexture and shared:GetTexture("$basetexture")
    if not baseTex then return shared end
    local baseName = baseTex.GetName and baseTex:GetName() or nil
    if not baseName or baseName == "" then return shared end

    local second = shared.GetTexture and shared:GetTexture("$basetexture2")
    if second then
        -- Ensure the material uses vertex alpha/color so our per-vertex alpha blends
        pcall(function()
            shared:SetInt("$vertexalpha", 1)
            shared:SetInt("$vertexcolor", 1)
        end)
        return shared
    end

    -- Try to find $basetexture2 via material proxies or alternatives; fallback: none
    local secondName = nil
    if second and second.GetName then secondName = second:GetName() end
    if not secondName or secondName == "" then
        return shared
    end

    -- Dynamic WorldVertexTransition (best-effort); cache per combo
    local key = string.format("rtx_dispblend[%s|%s]", baseName, secondName)
    local cached = dispMatCache[key]
    if cached ~= nil then return cached end
    local dyn
    -- CreateMaterial can take a unique name and shader; this may fail on some branches so guard with pcall
    local ok, err = pcall(function()
        dyn = CreateMaterial(key, "WorldVertexTransition", {
            ["$basetexture"] = baseName,
            ["$basetexture2"] = secondName,
            ["$vertexalpha"] = 1,
            ["$vertexcolor"] = 1,
            ["$translucent"] = 0
        })
    end)
    if not ok or not dyn then
        DebugPrint("Failed to create WorldVertexTransition material:", err)
        dispMatCache[key] = shared
        return shared
    end
    dispMatCache[key] = dyn
    return dyn
end

-- Build a batch of IMesh objects from a streamed triangle vertex list, preserving per-vertex alpha via mesh.Color
local MAX_VERTICES = 30000
local function CreateMeshBatchWithAlpha(vertices, material, maxVertsPerMesh)
    local meshes = {}
    local currentVerts = {}
    local currentAlphas = {}
    local vertCount = 0

    maxVertsPerMesh = maxVertsPerMesh or MAX_VERTICES

    local function flush()
        if #currentVerts == 0 then return end
        local newMesh = Mesh(material)
        mesh.Begin(newMesh, MATERIAL_TRIANGLES, #currentVerts / 3)
        for i = 1, #currentVerts do
            local v = currentVerts[i]
            local a = currentAlphas[i] or 1
            mesh.Position(v.pos)
            mesh.Normal(v.normal or Vector(0, 0, 1))
            mesh.TexCoord(0, v.u or 0, v.v or 0)
            if v.u1 and v.v1 then
                mesh.TexCoord(1, v.u1, v.v1)
            end
            -- Use base UVs on channel 2 so WorldVertexTransition $blendmodulatetexture is stable across seams
            mesh.TexCoord(2, v.u or 0, v.v or 0)
            local ia = math.Clamp(math.floor((a or 1) * 255 + 0.5), 0, 255)
            mesh.Color(255, 255, 255, ia)
            mesh.AdvanceVertex()
        end
        mesh.End()
        if RenderCore and RenderCore.TrackMesh then RenderCore.TrackMesh(newMesh) end
        table.insert(meshes, newMesh)
        currentVerts = {}
        currentAlphas = {}
        vertCount = 0
    end

    for i = 1, #vertices do
        local v = vertices[i]
        currentVerts[#currentVerts + 1] = v
        currentAlphas[#currentAlphas + 1] = v._alpha or 1
        vertCount = vertCount + 1
        if vertCount >= (maxVertsPerMesh - 3) then
            flush()
        end
    end
    flush()
    return meshes
end

-- Accumulate face normals from a displacement grid into global normal accumulators keyed by position.
-- Called once per grid in pass 1; normals are averaged and written back in pass 1.5.
local function AccumulateGridNormals(grid, posKeyFn, gnx, gny, gnz)
    if not grid or #grid == 0 then return end
    local count = #grid
    local width = math.sqrt(count)
    if width ~= math.floor(width) or width <= 1 then return end
    local height = count / width

    for i = 1, height - 1 do
        for j = 1, width - 1 do
            local idx1 = (i - 1) * width + j
            local idx2 = i * width + j
            local idx3 = i * width + j + 1
            local idx4 = (i - 1) * width + j + 1

            local p1 = grid[idx1].pos
            local p2 = grid[idx2].pos
            local p3 = grid[idx3].pos
            local p4 = grid[idx4].pos

            -- Triangle 1: idx1, idx2, idx3
            local e1x, e1y, e1z = p2.x - p1.x, p2.y - p1.y, p2.z - p1.z
            local e2x, e2y, e2z = p3.x - p1.x, p3.y - p1.y, p3.z - p1.z
            local fn1x = e1y * e2z - e1z * e2y
            local fn1y = e1z * e2x - e1x * e2z
            local fn1z = e1x * e2y - e1y * e2x

            local k1 = posKeyFn(p1)
            local k2 = posKeyFn(p2)
            local k3 = posKeyFn(p3)
            gnx[k1] = (gnx[k1] or 0) + fn1x  gny[k1] = (gny[k1] or 0) + fn1y  gnz[k1] = (gnz[k1] or 0) + fn1z
            gnx[k2] = (gnx[k2] or 0) + fn1x  gny[k2] = (gny[k2] or 0) + fn1y  gnz[k2] = (gnz[k2] or 0) + fn1z
            gnx[k3] = (gnx[k3] or 0) + fn1x  gny[k3] = (gny[k3] or 0) + fn1y  gnz[k3] = (gnz[k3] or 0) + fn1z

            -- Triangle 2: idx1, idx3, idx4
            local e3x, e3y, e3z = p3.x - p1.x, p3.y - p1.y, p3.z - p1.z
            local e4x, e4y, e4z = p4.x - p1.x, p4.y - p1.y, p4.z - p1.z
            local fn2x = e3y * e4z - e3z * e4y
            local fn2y = e3z * e4x - e3x * e4z
            local fn2z = e3x * e4y - e3y * e4x

            local k4 = posKeyFn(p4)
            gnx[k1] = gnx[k1] + fn2x  gny[k1] = gny[k1] + fn2y  gnz[k1] = gnz[k1] + fn2z
            gnx[k3] = gnx[k3] + fn2x  gny[k3] = gny[k3] + fn2y  gnz[k3] = gnz[k3] + fn2z
            gnx[k4] = (gnx[k4] or 0) + fn2x  gny[k4] = (gny[k4] or 0) + fn2y  gnz[k4] = (gnz[k4] or 0) + fn2z
        end
    end
end

-- Apply globally averaged normals back to a grid's vertices.
local function ApplyAveragedNormals(grid, posKeyFn, avgNormals)
    if not grid or not avgNormals then return end
    for i = 1, #grid do
        local k = posKeyFn(grid[i].pos)
        local n = avgNormals[k]
        if n then
            grid[i].normal = n
        end
    end
end

-- Triangulate a displacement grid (width x height) into a flat triangle vertex list, copying per-vertex alpha
local function GridToTriangles(grid, alphas)
    local tri = {}
    if not grid or #grid == 0 then return tri end
    local width = math.sqrt(#grid)
    
    -- Validate that grid is a perfect square
    if width ~= math.floor(width) then
        ErrorNoHalt("[DispRenderer] Invalid grid size: " .. #grid .. " is not a perfect square\\n")
        return tri
    end
    
    if width <= 1 then return tri end
    local height = #grid / width
    local n = 0
    for i = 1, height - 1 do
        for j = 1, width - 1 do
            local idx1 = (i - 1) * width + j
            local idx2 = i * width + j
            local idx3 = i * width + j + 1
            local idx4 = (i - 1) * width + j + 1

            local v1, v2, v3 = grid[idx1], grid[idx2], grid[idx3]
            v1 = { pos = v1.pos, normal = v1.normal, u = v1.u, v = v1.v, u1 = v1.u1, v1 = v1.v1, _alpha = (alphas and alphas[idx1]) or 1 }
            v2 = { pos = v2.pos, normal = v2.normal, u = v2.u, v = v2.v, u1 = v2.u1, v1 = v2.v1, _alpha = (alphas and alphas[idx2]) or 1 }
            v3 = { pos = v3.pos, normal = v3.normal, u = v3.u, v = v3.v, u1 = v3.u1, v1 = v3.v1, _alpha = (alphas and alphas[idx3]) or 1 }
            tri[#tri + 1] = v1
            tri[#tri + 1] = v2
            tri[#tri + 1] = v3

            local v4 = grid[idx4]
            v1 = { pos = grid[idx1].pos, normal = grid[idx1].normal, u = grid[idx1].u, v = grid[idx1].v, u1 = grid[idx1].u1, v1 = grid[idx1].v1, _alpha = (alphas and alphas[idx1]) or 1 }
            v3 = { pos = v3.pos, normal = v3.normal, u = v3.u, v = v3.v, u1 = v3.u1, v1 = v3.v1, _alpha = (alphas and alphas[idx3]) or 1 }
            v4 = { pos = v4.pos, normal = v4.normal, u = v4.u, v = v4.v, u1 = v4.u1, v1 = v4.v1, _alpha = (alphas and alphas[idx4]) or 1 }
            tri[#tri + 1] = v1
            tri[#tri + 1] = v3
            tri[#tri + 1] = v4
        end
    end
    return tri
end

-- Build all displacement meshes in a coroutine with a frame budget
local function BuildDisplacementMeshes(cancelToken)
    -- Cleanup existing
    for chunkKey, materials in pairs(dispMeshes) do
        for matKey, group in pairs(materials) do
            if type(group) == "table" and group.meshes then
                for _, m in ipairs(group.meshes) do
                    if m and m.Destroy then pcall(function() m:Destroy() end) end
                end
            end
        end
    end
    dispMeshes = {}

    if not NikNaks or not NikNaks.CurrentMap then return end

    DebugPrint("Building displacement meshes...")

    local co
    co = coroutine.create(function()
        local startTime = SysTime()
        local frameBudget = 0.003

        -- Prepare chunk table
        local chunks = {}
        local chunkSize = CONVARS.CHUNK_SIZE:GetInt()
        if not chunkSize or chunkSize <= 0 then chunkSize = 65536 end

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
                local m = Matrix()
                m:Scale(Vector(skyScale, skyScale, skyScale))
                m:Translate(-skyPos)
                skyboxWorldMatrix = m
            end
        end
        -- For seam-free blending, collect vertex alphas across all displacements and average by position
        local faceRecords = {}
        local alphaMin = math.huge
        local alphaMax = -math.huge
        -- Use epsilon-based position snapping for better seam matching
        local POSITION_EPSILON = CONVARS.SEAM_EPSILON:GetFloat()
        if POSITION_EPSILON <= 0 then POSITION_EPSILON = 0.1 end
        
        -- Use integer-based hashing for exact matching without floating point precision issues
        local function posKey(p)
            local scale = 1.0 / POSITION_EPSILON
            local ix = math.floor(p.x * scale + 0.5)
            local iy = math.floor(p.y * scale + 0.5)
            local iz = math.floor(p.z * scale + 0.5)
            -- Simple hash combining three integers
            return ix + iy * 100000 + iz * 10000000000
        end

        -- Iterate leafs and include displacements (may insert duplicates across leaves)
        local okLeafs, allLeafs = pcall(function() return NikNaks.CurrentMap:GetLeafs() end)
        if not okLeafs or not allLeafs then
            ErrorNoHalt("[DispRenderer] GetLeafs failed\n")
            return
        end
        local seenFaces = {}
        local skipStats = { notDisp = 0, duplicate = 0, noMaterial = 0, filtered = 0, noGrid = 0, total = 0 }
        buildState.active = true
        buildState.processed = 0
        buildState.total = 0
        for _ in pairs(allLeafs) do buildState.total = buildState.total + 1 end
        local faceCheckCounter = 0
        for _, leaf in pairs(allLeafs) do
            if cancelToken and cancelToken.cancelled then return end
            if leaf then
                local okFaces, leafFaces = pcall(function() return leaf:GetFaces(true) end) -- include displacements
                if leafFaces then
                    for _, face in pairs(leafFaces) do
                        -- Check cancellation every 100 faces to avoid long delays
                        faceCheckCounter = faceCheckCounter + 1
                        if faceCheckCounter >= 100 then
                            faceCheckCounter = 0
                            if cancelToken and cancelToken.cancelled then return end
                        end
                        repeat
                            skipStats.total = skipStats.total + 1
                            if not face or not face.IsDisplacement or not face:IsDisplacement() then 
                                skipStats.notDisp = skipStats.notDisp + 1
                                break 
                            end
                            local faceId = face.GetIndex and face:GetIndex() or tostring(face)
                            if seenFaces[faceId] then 
                                skipStats.duplicate = skipStats.duplicate + 1
                                break 
                            end
                            seenFaces[faceId] = true
                            -- REMOVED: ShouldRender check - it culls based on build-time viewpoint
                            -- if not face.ShouldRender or not face:ShouldRender() then break end

                            local mat = face.GetMaterial and face:GetMaterial() or nil
                            if not mat then 
                                skipStats.noMaterial = skipStats.noMaterial + 1
                                DebugPrint("Skipped displacement face " .. tostring(faceId) .. ": no material")
                                break 
                            end
                            local matName = mat.GetName and mat:GetName() or ""
                            if not IsMaterialAllowed(matName) then 
                                skipStats.filtered = skipStats.filtered + 1
                                DebugPrint("Skipped displacement face " .. tostring(faceId) .. ": material filtered - " .. matName)
                                break 
                            end

                            -- Determine center from base quad (use vertex grid average)
                            local grid = face.GenerateVertexData and face:GenerateVertexData() or nil
                            if not grid or #grid == 0 then 
                                skipStats.noGrid = skipStats.noGrid + 1
                                DebugPrint("Skipped displacement face " .. tostring(faceId) .. ": no vertex grid")
                                break 
                            end
                            local cx, cy, cz = 0, 0, 0
                            for i = 1, #grid do local p = grid[i].pos cx = cx + p.x cy = cy + p.y cz = cz + p.z end
                            cx = cx / #grid cy = cy / #grid cz = cz / #grid
                            local center = Vector(cx, cy, cz)

                            -- Check if displacement is inside the 3D skybox area
                            local isInSkybox = hasSkyAABB and center:WithinAABox(skyMins, skyMaxs)

                            local chunkX = math.floor(center.x / chunkSize)
                            local chunkY = math.floor(center.y / chunkSize)
                            local chunkZ = math.floor(center.z / chunkSize)
                            local chunkKey = GetChunkKey(chunkX, chunkY, chunkZ)

                            chunks[chunkKey] = chunks[chunkKey] or {}
                            local chunkData = chunks[chunkKey]

                            -- Build per-vertex alpha from disp verts (pass 1: accumulate by position)
                            local dispInfo = face.GetDisplacementInfo and face:GetDisplacementInfo() or nil
                            local power = dispInfo and dispInfo.power or 2
                            local w = (2 ^ power) + 1
                            local vertStart = dispInfo and dispInfo.DispVertStart or 0
                            local vertEnd = vertStart + (w * w)
                            local dispVerts = NikNaks.CurrentMap:GetDispVerts()
                            local alphaKeys = {}
                            local rawAlphas = {}  -- Store raw alpha values
                            for v = vertStart, vertEnd - 1 do
                                local dv = dispVerts[v]
                                local idx = (v - vertStart) + 1
                                local rawAlpha = (dv and dv.alpha) or 0
                                rawAlphas[idx] = rawAlpha
                                
                                -- Track min/max for auto-normalization
                                if rawAlpha < alphaMin then alphaMin = rawAlpha end
                                if rawAlpha > alphaMax then alphaMax = rawAlpha end
                                
                                -- Store position key for averaging
                                local pk = posKey(grid[idx].pos)
                                alphaKeys[idx] = pk
                            end

                            -- Choose material now and record for pass 2
                            local useMat = GetDispBlendMaterial(mat)
                            local useName = (useMat and useMat.GetName and useMat:GetName()) or matName or "__unnamed__"

                            faceRecords[#faceRecords + 1] = {
                                grid = grid,
                                alphaKeys = alphaKeys,
                                rawAlphas = rawAlphas,  -- Store raw values
                                mat = useMat,
                                matName = useName,
                                chunkKey = chunkKey,
                                isSkybox = isInSkybox or false
                            }
                        until true

                        if SysTime() - startTime > frameBudget then
                            coroutine.yield()
                            local spent = SysTime() - startTime
                            if RenderCore and RenderCore.UpdateFrameBudget then
                                frameBudget = RenderCore.UpdateFrameBudget(spent, frameBudget)
                            else
                                if spent > frameBudget * 1.2 then
                                    frameBudget = math.max(0.001, frameBudget * 0.95)
                                elseif spent < frameBudget * 0.8 then
                                    frameBudget = math.min(0.006, frameBudget * 1.05)
                                end
                            end
                            startTime = SysTime()
                        end
                    end
                end
            end
            buildState.processed = buildState.processed + 1
            if SysTime() - startTime > frameBudget then
                coroutine.yield()
                local spent = SysTime() - startTime
                if RenderCore and RenderCore.UpdateFrameBudget then
                    frameBudget = RenderCore.UpdateFrameBudget(spent, frameBudget)
                else
                    if spent > frameBudget * 1.2 then
                        frameBudget = math.max(0.001, frameBudget * 0.95)
                    elseif spent < frameBudget * 0.8 then
                        frameBudget = math.min(0.006, frameBudget * 1.05)
                    end
                end
                startTime = SysTime()
            end
        end

        -- Determine normalization scale
        local alphaScale = CONVARS.ALPHA_SCALE:GetFloat()
        if alphaScale <= 0 then
            -- Clamp to reasonable range (Source displacement alphas are typically 0-255)
            -- Negative or extreme values indicate data corruption or format issues
            local clampedMin = math.max(alphaMin, 0)
            local clampedMax = math.min(alphaMax, 255)
            
            -- If we have invalid range, use sensible defaults
            if clampedMax <= clampedMin then
                clampedMin = 0
                clampedMax = 255
            end
            
            alphaMin = clampedMin
            alphaMax = clampedMax
            alphaScale = math.max(alphaMax - alphaMin, 0.001)  -- Avoid division by zero
            DebugPrint(string.format("Auto-detected alpha range: %.3f to %.3f (scale: %.3f)", alphaMin, alphaMax, alphaScale))
        else
            DebugPrint(string.format("Using manual alpha scale: %.3f", alphaScale))
        end
        
        -- Normalize raw alphas FIRST, then average in normalized space
        -- This ensures cross-material blending works correctly
        local normalizedSumByKey = {}
        local normalizedCountByKey = {}
        
        for i = 1, #faceRecords do
            local rec = faceRecords[i]
            for gi = 1, #rec.rawAlphas do
                local rawAlpha = rec.rawAlphas[gi]
                local normalized = math.Clamp((rawAlpha - alphaMin) / alphaScale, 0, 1)
                local k = rec.alphaKeys[gi]
                if k then
                    normalizedSumByKey[k] = (normalizedSumByKey[k] or 0) + normalized
                    normalizedCountByKey[k] = (normalizedCountByKey[k] or 0) + 1
                end
            end
        end
        
        -- Average normalized alphas per shared vertex position
        local alphaAvgByKey = {}
        local sharedVertices = 0
        for k, sum in pairs(normalizedSumByKey) do
            local c = normalizedCountByKey[k] or 1
            alphaAvgByKey[k] = sum / c
            if c > 1 then sharedVertices = sharedVertices + 1 end
        end
        DebugPrint(string.format("Averaged %d shared vertex positions (%.1f%% of total)", sharedVertices, (sharedVertices / math.max(1, table.Count(normalizedSumByKey))) * 100))

        -- Pass 1.5: compute smooth normals across all displacement faces globally
        -- Accumulate face normals by position key so shared edge vertices get averaged normals
        local gnx, gny, gnz = {}, {}, {}
        for i = 1, #faceRecords do
            AccumulateGridNormals(faceRecords[i].grid, posKey, gnx, gny, gnz)
        end
        -- Normalize accumulated normals
        local avgNormals = {}
        local math_sqrt = math.sqrt
        for k, x in pairs(gnx) do
            local y, z = gny[k], gnz[k]
            local len = math_sqrt(x * x + y * y + z * z)
            if len > 0.0001 then
                avgNormals[k] = Vector(x / len, y / len, z / len)
            end
        end
        gnx, gny, gnz = nil, nil, nil -- free accumulator memory
        DebugPrint(string.format("Computed %d globally averaged vertex normals", table.Count(avgNormals)))

        -- Pass 2: stream triangles using averaged normalized alphas into chunk/material groups
        for i = 1, #faceRecords do
            local rec = faceRecords[i]
            local grid = rec.grid
            local alphas = {}
            for gi = 1, #grid do
                local k = rec.alphaKeys[gi]
                if k and alphaAvgByKey[k] then
                    -- Use averaged normalized alpha
                    alphas[gi] = alphaAvgByKey[k]
                else
                    -- Fallback: normalize raw alpha
                    local rawAlpha = rec.rawAlphas[gi] or alphaMin
                    alphas[gi] = math.Clamp((rawAlpha - alphaMin) / alphaScale, 0, 1)
                end
            end
            ApplyAveragedNormals(grid, posKey, avgNormals)
            local triangles = GridToTriangles(grid, alphas)

            local chunkKey = rec.chunkKey
            local materials = chunks[chunkKey] or {}
            chunks[chunkKey] = materials

            local useName = rec.matName
            local useMat = rec.mat
            -- Include skybox flag in key so world and skybox displacements with the
            -- same material never get merged into one group (wrong matrix applied)
            local groupKey = rec.isSkybox and (useName .. "\0sky") or useName
            materials[groupKey] = materials[groupKey] or { material = useMat, _stream = {}, _mins = Vector(math.huge, math.huge, math.huge), _maxs = Vector(-math.huge, -math.huge, -math.huge), isSkybox = rec.isSkybox or false }
            local group = materials[groupKey]

            for t = 1, #triangles do
                local v = triangles[t]
                group._stream[#group._stream + 1] = v
                local p = v.pos
                if p.x < group._mins.x then group._mins.x = p.x end
                if p.y < group._mins.y then group._mins.y = p.y end
                if p.z < group._mins.z then group._mins.z = p.z end
                if p.x > group._maxs.x then group._maxs.x = p.x end
                if p.y > group._maxs.y then group._maxs.y = p.y end
                if p.z > group._maxs.z then group._maxs.z = p.z end
            end

            if cancelToken and cancelToken.cancelled then return end
            if SysTime() - startTime > frameBudget then
                coroutine.yield()
                local spent = SysTime() - startTime
                if RenderCore and RenderCore.UpdateFrameBudget then
                    frameBudget = RenderCore.UpdateFrameBudget(spent, frameBudget)
                else
                    if spent > frameBudget * 1.2 then
                        frameBudget = math.max(0.001, frameBudget * 0.9)
                    elseif spent < frameBudget * 0.8 then
                        frameBudget = math.min(0.006, frameBudget * 1.1)
                    end
                end
                startTime = SysTime()
            end
        end

        -- Build IMeshes per chunk/material
        for chunkKey, materials in pairs(chunks) do
            dispMeshes[chunkKey] = {}
            for matKey, group in pairs(materials) do
                    local material = group.material
                    local triStream = group._stream
                    if triStream and #triStream > 0 and material then
                        local meshes = CreateMeshBatchWithAlpha(triStream, material, MAX_VERTICES)
                        dispMeshes[chunkKey][matKey] = {
                            meshes = meshes,
                            material = material,
                            isSkybox = group.isSkybox or false
                        }
                        -- Merge bounds into chunk level
                        local c = dispMeshes[chunkKey]
                        if not c._mins then
                            c._mins = group._mins
                            c._maxs = group._maxs
                        else
                            local cmins = c._mins
                            local cmaxs = c._maxs
                            local gmins = group._mins
                            local gmaxs = group._maxs
                            if gmins.x < cmins.x then cmins.x = gmins.x end
                            if gmins.y < cmins.y then cmins.y = gmins.y end
                            if gmins.z < cmins.z then cmins.z = gmins.z end
                            if gmaxs.x > cmaxs.x then cmaxs.x = gmaxs.x end
                            if gmaxs.y > cmaxs.y then cmaxs.y = gmaxs.y end
                            if gmaxs.z > cmaxs.z then cmaxs.z = gmaxs.z end
                        end
                    end
                end
                if cancelToken and cancelToken.cancelled then return end
                if SysTime() - startTime > frameBudget then
                    coroutine.yield()
                    local spent = SysTime() - startTime
                    if RenderCore and RenderCore.UpdateFrameBudget then
                        frameBudget = RenderCore.UpdateFrameBudget(spent, frameBudget)
                    else
                        if spent > frameBudget * 1.2 then
                            frameBudget = math.max(0.001, frameBudget * 0.95)
                        elseif spent < frameBudget * 0.8 then
                            frameBudget = math.min(0.006, frameBudget * 1.05)
                        end
                    end
                    startTime = SysTime()
                end
            end

        buildState.active = false
        local totalChunks = 0
        local totalMeshes = 0
        local totalFaces = 0
        for _, mats in pairs(chunks) do
            totalChunks = totalChunks + 1
            for k, _ in pairs(mats) do
                if k ~= "_clusters" then totalMeshes = totalMeshes + 1 end
            end
        end
        totalFaces = table.Count(seenFaces)
        DebugPrint(string.format("Built %d displacement chunks with %d material groups from %d faces", totalChunks, totalMeshes, totalFaces))
        DebugPrint(string.format("Skipped faces: %d total, %d not disp, %d duplicate, %d no material, %d filtered, %d no grid",
            skipStats.total, skipStats.notDisp, skipStats.duplicate, skipStats.noMaterial, skipStats.filtered, skipStats.noGrid))
    end)

    -- Drive coroutine
    local jobId = "RTXDispMeshBuildJob"
    RenderCore.ScheduleJob(jobId, function()
        if not co or coroutine.status(co) == "dead" then
            buildState.active = false
            return false
        end
        
        local ok, err = coroutine.resume(co)
        if not ok then
            ErrorNoHalt("[DispRenderer] Build coroutine error: " .. tostring(err) .. "\n")
            buildState.active = false
            buildState.processed = 0
            buildState.total = 0
            -- Clean up partial meshes
            for chunkKey, materials in pairs(dispMeshes) do
                for matKey, group in pairs(materials) do
                    if type(group) == "table" and group.meshes then
                        for _, m in ipairs(group.meshes) do
                            if m then
                                if RenderCore and RenderCore.DestroyMesh then
                                    RenderCore.DestroyMesh(m)
                                else
                                    pcall(function() if m.Destroy then m:Destroy() end end)
                                end
                            end
                        end
                    end
                end
            end
            dispMeshes = {}
            return false
        end
        
        -- Check if cancelled
        if cancelToken and cancelToken.cancelled then
            buildState.active = false
            DebugPrint("Build cancelled")
            return false
        end
        
        local isDead = coroutine.status(co) == "dead"
        if isDead then
            buildState.active = false
        end
        return not isDead
    end)
end

-- Render
local function RenderDisplacements()
    if not CONVARS.ENABLED:GetBool() then return end
    
    -- Skip rendering displacements in offscreen RTs (e.g., rear-view camera with dynamic_only filter)
    if RenderCore and RenderCore.IsOffscreen and RenderCore.IsOffscreen() then
        return
    end
    
    local draws = 0
    local chunksVisited = 0

    for _, chunkMaterials in pairs(dispMeshes) do
        chunksVisited = chunksVisited + 1
        for key, group in pairs(chunkMaterials) do
                if key ~= "_mins" and key ~= "_maxs" then
                    if group and group.meshes then
                        local skipSkybox = group.isSkybox and RenderCore and RenderCore.Is3DSkyEnabled and not RenderCore.Is3DSkyEnabled()
                        if not skipSkybox then
                            local meshes = group.meshes
                            local skyMatrix = group.isSkybox and skyboxWorldMatrix or nil
                            for i = 1, #meshes do
                                local m = meshes[i]
                                if m then
                                    RenderCore.Submit({
                                        material = group.material,
                                        mesh = m,
                                        matrix = skyMatrix,
                                        translucent = false
                                    })
                                    draws = draws + 1
                                end
                            end
                        end
                    end
                end
            end
    end

    stats.draws = draws
    stats.chunksVisited = chunksVisited
    
    -- Only log once per second to avoid console spam
    if CONVARS.DEBUG:GetBool() and (SysTime() - lastDebugPrint) > 1.0 then
        lastDebugPrint = SysTime()
        DebugPrint(string.format("Rendered: %d draws from %d chunks", draws, chunksVisited))
    end
end

-- Enable/Disable
local isEnabled = false
local function EnableRendering()
    if isEnabled then return end
    isEnabled = true
    RenderCore.Register("PreDrawOpaqueRenderables", "RTXDisp_Draw", function()
        if RenderCore.IsInSkyboxPass and RenderCore.IsInSkyboxPass() then return end
        RenderDisplacements()
    end)
end

local function DisableRendering()
    if not isEnabled then return end
    isEnabled = false
    RenderCore.Unregister("PreDrawOpaqueRenderables", "RTXDisp_Draw")
end

-- Init / Rebuild
local function Initialize(token)
    local ok, err = pcall(BuildDisplacementMeshes, token)
    if not ok then
        ErrorNoHalt("[DispRenderer] Failed to build: " .. tostring(err) .. "\n")
        DisableRendering()
        return
    end
    timer.Simple(1, function()
        if CONVARS.ENABLED:GetBool() then
            local success, e = pcall(EnableRendering)
            if not success then
                ErrorNoHalt("[DispRenderer] Failed to enable: " .. tostring(e) .. "\n")
                DisableRendering()
            end
        end
    end)
end

RenderCore.Register("InitPostEntity", "RTXDisp_Init", function(token)
    if not CONVARS.ENABLED:GetBool() then return end
    Initialize(token)
end)

RenderCore.Register("PostCleanupMap", "RTXDisp_Rebuild", function()
    RenderCore.RequestRebuild("PostCleanupMap")
end)

RenderCore.Register("ShutDown", "RTXDisp_Shutdown", function()
    DisableRendering()
    for _, mats in pairs(dispMeshes) do
        for _, group in pairs(mats) do
            if type(group) == "table" and group.meshes then
                for _, m in ipairs(group.meshes) do
                    if m and m.Destroy then pcall(function() m:Destroy() end) end
                end
            end
        end
    end
    dispMeshes = {}
end)

-- Stats
RenderCore.RegisterStats("Displacements", function()
    local extra = ""
    if buildState.active and (buildState.total or 0) > 0 then
        extra = string.format(" | build: %d/%d", buildState.processed or 0, buildState.total or 0)
    end
    return string.format("Disp draws: %d | chunks: %d%s", stats.draws or 0, stats.chunksVisited or 0, extra)
end)

-- ConVar Changes
cvars.AddChangeCallback("rtx_dpr_enable", function(_, _, new)
    if tobool(new) then
        if not buildState.active and not next(dispMeshes) then
            Initialize()
        else
            EnableRendering()
        end
    else
        DisableRendering()
    end
end)

-- Rebuild sink and debounced cvars
RenderCore.RegisterRebuildSink("RTXDispRebuildSink", function(token, reason)
    Initialize(token)
end)

local function DebounceRebuildOnCvar(name)
    cvars.AddChangeCallback(name, function()
        RenderCore.RequestRebuild(name)
    end, "RTXDispRebuild-" .. name)
end

DebounceRebuildOnCvar("rtx_dpr_chunk_size")
DebounceRebuildOnCvar("rtx_dpr_mat_whitelist")
DebounceRebuildOnCvar("rtx_dpr_mat_blacklist")
DebounceRebuildOnCvar("rtx_dpr_alpha_scale")
DebounceRebuildOnCvar("rtx_dpr_seam_epsilon")

-- Console helper
concommand.Add("rtx_rebuild_displacements", function()
    Initialize(RenderCore and RenderCore.NewToken and RenderCore.NewToken("RTXDispRebuildManual") or {})
end)

print("[Custom Displacement Renderer] Loaded.")


