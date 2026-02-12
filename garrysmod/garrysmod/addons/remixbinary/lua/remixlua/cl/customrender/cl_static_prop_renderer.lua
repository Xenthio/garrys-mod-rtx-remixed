if not CLIENT then return end
local RenderCore = include("remixlua/cl/customrender/render_core.lua") or RemixRenderCore
-- Custom Static Prop Renderer -- disabled due to engine culling patches.
-- Re-Renders all static props to bypass engine culling 
-- Author: CR

local convar_Enable = CreateClientConVar("rtx_spr_enable", "0", true, false, "Enable custom rendering of static props")
local convar_Debug = CreateClientConVar("rtx_spr_debug", "0", true, false, "Enable debug prints for static prop renderer")
local convar_Whitelist = CreateClientConVar("rtx_spr_mat_whitelist", "", true, false, "Comma-separated material name substrings to include")
local convar_Blacklist = CreateClientConVar("rtx_spr_mat_blacklist", "", true, false, "Comma-separated material name substrings to exclude")
local convar_UsePVS = CreateClientConVar("rtx_spr_use_pvs", "1", true, false, "Enable PVS culling for static props")
local convar_PVSSafetyDistance = CreateClientConVar("rtx_spr_pvs_safety_distance", "0", true, false, "Distance within which PVS culling is disabled (prevents close-range culling bugs)")
local convar_FrameSkip = CreateClientConVar("rtx_spr_frame_skip", "2", true, false, "Update prop visibility every N frames (2 = every other frame, 1 = every frame)")
local convar_UseMeshCombining = CreateClientConVar("rtx_spr_mesh_combining", "0", true, false, "Combine props into single meshes per material to reduce draw calls")

-- Per-map PVS safety distance persistence
local PVS_SAFETY_FILE = "rtx_pvs_safety_distances.txt"

local function GetCurrentMapName()
    return game.GetMap() or "unknown"
end

local function LoadPerMapPVSSettings()
    if not file.Exists(PVS_SAFETY_FILE, "DATA") then
        return {}
    end
    
    local json = file.Read(PVS_SAFETY_FILE, "DATA")
    if not json then return {} end
    
    local ok, data = pcall(util.JSONToTable, json)
    if not ok or not data then return {} end
    
    return data
end

local function SavePerMapPVSSettings(mapSettings)
    local json = util.TableToJSON(mapSettings, true)
    if json then
        file.Write(PVS_SAFETY_FILE, json)
    end
end

local function ApplyMapPVSSettings()
    local mapName = GetCurrentMapName()
    local settings = LoadPerMapPVSSettings()
    
    if settings[mapName] and settings[mapName].pvs_safety_distance then
        local savedDistance = settings[mapName].pvs_safety_distance
        RunConsoleCommand("rtx_spr_pvs_safety_distance", tostring(savedDistance))
    else
        -- No saved value for this map, reset to default (0)
        RunConsoleCommand("rtx_spr_pvs_safety_distance", "0")
    end
end

local function SaveCurrentMapPVSSettings()
    local mapName = GetCurrentMapName()
    local currentDistance = convar_PVSSafetyDistance:GetFloat()
    
    local settings = LoadPerMapPVSSettings()
    settings[mapName] = settings[mapName] or {}
    settings[mapName].pvs_safety_distance = currentDistance
    
    SavePerMapPVSSettings(settings)
end

-- Apply saved settings when map loads
hook.Add("InitPostEntity", "RTX_SPR_LoadMapSettings", function()
    timer.Simple(0.1, ApplyMapPVSSettings)
end)

-- Save settings when convar changes
cvars.AddChangeCallback("rtx_spr_pvs_safety_distance", function(convar, oldValue, newValue)
    -- Only save if the value actually changed and we're in a map
    if oldValue ~= newValue and GetCurrentMapName() ~= "unknown" then
        timer.Simple(0.5, SaveCurrentMapPVSSettings)
    end
end, "RTX_SPR_SavePVSDistance")

-- Global state
local isDataReady = false
local isCachingInProgress = false
local cachedStaticProps = {}
local meshCache = {}  -- Maps model path to IMesh objects
local lastDebugFrame = 0
local bDrawingSkybox = false
local skyboxProps = {}
local worldProps = {}
local sprStats = { rendered = 0, total = 0 }
local sprBuildStats = { startTime = 0, endTime = 0, built = 0, active = false }
-- Expose build state for progress tracking
if RemixRenderCore then RemixRenderCore._sprBuildState = sprBuildStats end

-- Frame skipping cache (separate for skybox and world)
local cachedRenderList = { world = {}, skybox = {} }
local lastUpdateFrame = { world = -1, skybox = -1 }

-- Combined mesh cache (per material) - built once during initialization
local combinedMeshes = {} -- [materialName] = { material = IMaterial, mesh = IMesh, propCount = N, props = {prop indices} }
local combinedMeshesBuilt = false
local MAX_VERTS_PER_COMBINED_MESH = 21000 -- Leave room under 21845 vertex limit

-- Build a combined mesh from multiple props with the same material (forward declare)
local BuildCombinedMesh

-- Build all combined meshes once during initialization
local function BuildAllCombinedMeshes()
    combinedMeshes = {}
    combinedMeshesBuilt = false
    
    -- Group world props by material
    local materialGroups = {} -- [materialName] = { props = {indices}, material = IMaterial }
    
    for idx, prop in ipairs(worldProps) do
        if prop.cachedMesh and prop.cachedMesh.meshes then
            for _, meshInfo in ipairs(prop.cachedMesh.meshes) do
                local mat = meshInfo.material
                if mat and mat.GetName then
                    local matName = mat:GetName()
                    
                    if not materialGroups[matName] then
                        materialGroups[matName] = {
                            material = mat,
                            propIndices = {}
                        }
                    end
                    
                    table.insert(materialGroups[matName].propIndices, idx)
                end
            end
        end
    end
    
    -- Build combined mesh for each material group (may create multiple batches per material)
    local batchCounter = 0
    for matName, group in pairs(materialGroups) do
        -- Smart batching: accumulate props until we approach vertex limit
        local currentBatch = {}
        local currentBatchIndices = {}
        local estimatedVerts = 0
        local VERT_ESTIMATE_PER_PROP = 300 -- Conservative average
        
        for _, propIdx in ipairs(group.propIndices) do
            local prop = worldProps[propIdx]
            local propVerts = (prop.cachedMesh and prop.cachedMesh.vertexCount) or VERT_ESTIMATE_PER_PROP
            
            -- Start new batch if this prop would exceed limit
            if estimatedVerts + propVerts > MAX_VERTS_PER_COMBINED_MESH and #currentBatch > 0 then
                -- Build current batch
                local combinedMesh = BuildCombinedMesh(currentBatch, group.material)
                if combinedMesh then
                    batchCounter = batchCounter + 1
                    local batchKey = matName .. "_batch" .. batchCounter
                    
                    -- Pre-calculate unique clusters for this batch for faster PVS checks
                    local batchClusters = {}
                    for _, idx in ipairs(currentBatchIndices) do
                        local prop = worldProps[idx]
                        if prop.clusters then
                            for cl in pairs(prop.clusters) do
                                batchClusters[cl] = true
                            end
                        end
                    end
                    
                    combinedMeshes[batchKey] = {
                        material = group.material,
                        mesh = combinedMesh,
                        propCount = #currentBatch,
                        propIndices = currentBatchIndices,
                        clusters = batchClusters -- Cache for fast PVS checks
                    }
                end
                
                -- Reset for next batch
                currentBatch = {}
                currentBatchIndices = {}
                estimatedVerts = 0
            end
            
            -- Add prop to current batch
            table.insert(currentBatch, {
                prop = prop,
                mesh = nil
            })
            table.insert(currentBatchIndices, propIdx)
            estimatedVerts = estimatedVerts + propVerts
        end
        
        -- Build final batch for this material
        if #currentBatch > 0 then
            local combinedMesh = BuildCombinedMesh(currentBatch, group.material)
            if combinedMesh then
                batchCounter = batchCounter + 1
                local batchKey = matName .. "_batch" .. batchCounter
                
                -- Pre-calculate unique clusters for this batch for faster PVS checks
                local batchClusters = {}
                for _, idx in ipairs(currentBatchIndices) do
                    local prop = worldProps[idx]
                    if prop.clusters then
                        for cl in pairs(prop.clusters) do
                            batchClusters[cl] = true
                        end
                    end
                end
                
                combinedMeshes[batchKey] = {
                    material = group.material,
                    mesh = combinedMesh,
                    propCount = #currentBatch,
                    propIndices = currentBatchIndices,
                    clusters = batchClusters -- Cache for fast PVS checks
                }
            end
        end
    end
    
    combinedMeshesBuilt = true
    
    -- Free triangle data to save memory (no longer needed after combining)
    if convar_UseMeshCombining:GetBool() then
        for _, prop in ipairs(worldProps) do
            if prop.cachedMesh and prop.cachedMesh.meshes then
                for _, meshInfo in ipairs(prop.cachedMesh.meshes) do
                    meshInfo.triangles = nil
                end
            end
        end
    end
end

-- Build a combined mesh from multiple props with the same material
BuildCombinedMesh = function(propList, material)
    if not propList or #propList == 0 or not material then return nil end
    
    local allVertices = {}
    local totalVerts = 0
    
    -- Cache material name once
    local targetMatName = material:GetName()
    
    -- Extract and transform vertices from each prop using cached vertex data
    for _, propData in ipairs(propList) do
        local prop = propData.prop
        local matrix = prop.matrix
        
        -- Pre-calculate rotation vectors once per prop (not per vertex)
        local ang = matrix:GetAngles()
        local fwd = ang:Forward()
        local right = ang:Right()
        local up = ang:Up()
        
        -- Use cached mesh data which includes raw triangles
        if prop.cachedMesh and prop.cachedMesh.meshes then
            for _, meshInfo in ipairs(prop.cachedMesh.meshes) do
                -- Skip if no triangles cached or material doesn't match
                if not meshInfo.triangles or not meshInfo.material then continue end
                
                local meshMatName = meshInfo.material:GetName()
                
                if meshMatName == targetMatName then
                    local triangles = meshInfo.triangles
                    
                    -- Transform each vertex
                    for i = 1, #triangles do
                        local vert = triangles[i]
                        
                        -- Transform position
                        local transformedPos = matrix * vert.pos
                        
                        -- Transform normal by rotation part of matrix
                        local transformedNormal = vert.normal
                        if vert.normal then
                            local nx, ny, nz = vert.normal.x, vert.normal.y, vert.normal.z
                            transformedNormal = fwd * nx + right * ny + up * nz
                            transformedNormal:Normalize()
                        end
                        
                        allVertices[totalVerts + 1] = {
                            pos = transformedPos,
                            normal = transformedNormal,
                            u = vert.u,
                            v = vert.v,
                            userdata = vert.userdata
                        }
                        
                        totalVerts = totalVerts + 1
                        
                        -- Safety check: don't exceed vertex limit
                        if totalVerts >= MAX_VERTS_PER_COMBINED_MESH then
                            break
                        end
                    end
                end
                
                if totalVerts >= MAX_VERTS_PER_COMBINED_MESH then
                    break
                end
            end
        end
        
        if totalVerts >= MAX_VERTS_PER_COMBINED_MESH then
            break
        end
    end
    
    if #allVertices == 0 then return nil end
    
    -- Build the combined mesh
    local combinedMesh = Mesh(material)
    mesh.Begin(combinedMesh, MATERIAL_TRIANGLES, #allVertices / 3)
    
    for _, vert in ipairs(allVertices) do
        mesh.Position(vert.pos)
        mesh.Normal(vert.normal or Vector(0, 0, 1))
        mesh.TexCoord(0, vert.u or 0, vert.v or 0)
        if vert.userdata then
            mesh.UserData(vert.userdata[1] or 0, vert.userdata[2] or 0, vert.userdata[3] or 0, vert.userdata[4] or 0)
        end
        mesh.AdvanceVertex()
    end
    
    mesh.End()
    
    if RenderCore and RenderCore.TrackMesh then
        RenderCore.TrackMesh(combinedMesh)
    end
    
    return combinedMesh
end

local function IsPVSValid(pvs)
    if not pvs then return false end
    for _, v in pairs(pvs) do
        if v then return true end
    end
    return false
end

-- Debug helper function
local DebugPrint = (RenderCore and RenderCore.CreateDebugPrint)
    and RenderCore.CreateDebugPrint("Static Render Debug", convar_Debug)
    or function(...)
        if convar_Debug:GetBool() then
            print("[Static Render Debug]", ...)
        end
    end

-- Use RenderCore material cache directly
local function GetCachedMaterial(matName)
    return (RenderCore and RenderCore.GetMaterial) and RenderCore.GetMaterial(matName) or Material(matName or "debug/debugwhite")
end

-- Get mesh data directly using GetModelMeshes
local function GetModelMeshes(modelPath, skin)
    -- Load the model if not already loaded
    if not util.IsModelLoaded(modelPath) then
        util.PrecacheModel(modelPath)
    end
    
    -- Try to get mesh data directly with skin support
    return util.GetModelMeshes(modelPath, 0, 0, skin or 0)
end

local function IsMaterialAllowedName(matName)
    if not matName then return false end
    if RenderCore and RenderCore.IsMaterialAllowed then
        return RenderCore.IsMaterialAllowed(matName, convar_Whitelist:GetString(), convar_Blacklist:GetString())
    end
    -- Fallback: allow by default if core helper missing
    return true
end

-- Process a static prop and prepare rendering data
local function ProcessStaticProp(propData)
    local modelPath = propData.PropType
    if not modelPath or modelPath == "" then
        DebugPrint("Static prop has no model path")
        return nil
    end
    
    -- Create the prop data structure
    local prop = {
        model = modelPath,
        origin = propData.Origin,
        angles = propData.Angles,
        skin = propData.Skin or 0,
        color = propData.DiffuseModulation or Color(255, 255, 255),
        vertexCount = 0 -- Will be set during mesh processing
    }

    -- Precompute transform matrix (static props don't move)
    local matrix = Matrix()
    matrix:Translate(prop.origin)
    matrix:Rotate(prop.angles)
    prop.matrix = matrix

    -- Check if this is a skybox prop
    local isSkyboxProp = false
    if NikNaks and NikNaks.CurrentMap and NikNaks.CurrentMap:HasSkyBox() then
        local skyPos = NikNaks.CurrentMap:GetSkyBoxPos()
        local skyMinBounds, skyMaxBounds = NikNaks.CurrentMap:GetSkyboxSize()
        
        -- Check if the prop is within skybox bounds
        if skyMinBounds and skyMaxBounds and propData.Origin then
            isSkyboxProp = propData.Origin:WithinAABox(skyMinBounds, skyMaxBounds)
        end
    end
    
    -- Store this information in the prop data
    prop.isSkybox = isSkyboxProp
    -- Cache BSP clusters for PVS checks (multi-cluster for better precision)
    -- Use AABB to find all clusters this prop touches
    if NikNaks and NikNaks.CurrentMap and meshCache[cacheKey] and not meshCache[cacheKey].error then
        local meshData = meshCache[cacheKey]
        if meshData.mins and meshData.maxs then
            -- Transform bounds to world space
            local corners = {
                prop.matrix * meshData.mins,
                prop.matrix * meshData.maxs,
                prop.matrix * Vector(meshData.mins.x, meshData.mins.y, meshData.maxs.z),
                prop.matrix * Vector(meshData.mins.x, meshData.maxs.y, meshData.mins.z),
                prop.matrix * Vector(meshData.maxs.x, meshData.mins.y, meshData.mins.z),
                prop.matrix * Vector(meshData.maxs.x, meshData.maxs.y, meshData.mins.z),
                prop.matrix * Vector(meshData.maxs.x, meshData.mins.y, meshData.maxs.z),
                prop.matrix * Vector(meshData.mins.x, meshData.maxs.y, meshData.maxs.z),
            }
            -- Find AABB of transformed corners
            local worldMins = Vector(math.huge, math.huge, math.huge)
            local worldMaxs = Vector(-math.huge, -math.huge, -math.huge)
            for _, corner in ipairs(corners) do
                if corner.x < worldMins.x then worldMins.x = corner.x end
                if corner.y < worldMins.y then worldMins.y = corner.y end
                if corner.z < worldMins.z then worldMins.z = corner.z end
                if corner.x > worldMaxs.x then worldMaxs.x = corner.x end
                if corner.y > worldMaxs.y then worldMaxs.y = corner.y end
                if corner.z > worldMaxs.z then worldMaxs.z = corner.z end
            end
            
            -- Get all leaves that intersect this AABB
            prop.clusters = {}
            if NikNaks.CurrentMap.AABBInLeafs then
                local ok, leaves = pcall(function() return NikNaks.CurrentMap:AABBInLeafs(0, worldMins, worldMaxs) end)
                if ok and leaves then
                    for _, leaf in ipairs(leaves) do
                        local cl = leaf.GetCluster and leaf:GetCluster() or -1
                        if cl and cl >= 0 then
                            prop.clusters[cl] = true
                        end
                    end
                end
            end
            -- Fallback to origin-based cluster if multi-cluster failed
            if not next(prop.clusters) and NikNaks.CurrentMap.ClusterFromPoint then
                local cl = NikNaks.CurrentMap:ClusterFromPoint(prop.origin) or -1
                if cl >= 0 then
                    prop.clusters[cl] = true
                end
            end
        end
    else
        -- Fallback: single cluster from origin
        prop.clusters = {}
        if NikNaks and NikNaks.CurrentMap and NikNaks.CurrentMap.ClusterFromPoint then
            local cl = NikNaks.CurrentMap:ClusterFromPoint(prop.origin) or -1
            if cl >= 0 then
                prop.clusters[cl] = true
            end
        end
    end
    
    -- Check if we already cached this model's mesh
    local cacheKey = modelPath .. "_skin" .. prop.skin
    if not meshCache[cacheKey] then
        -- Get the mesh data with skin support
        local meshData = GetModelMeshes(modelPath, prop.skin)
        
        if not meshData or #meshData == 0 then
            DebugPrint("Failed to get mesh data for:", modelPath)
            -- Store an empty entry to avoid repeatedly trying to process it
            meshCache[cacheKey] = {
                meshes = nil,
                error = true
            }
            return nil
        end
        
        -- Process mesh groups and maintain their material relationships
        local processedMeshes = {}
        local mins = Vector(math.huge, math.huge, math.huge)
        local maxs = Vector(-math.huge, -math.huge, -math.huge)
        local totalVertexCount = 0
        
        -- Process each mesh group
        for _, group in ipairs(meshData) do
            if group.triangles and #group.triangles > 0 then
                local material = group.material or "models/debug/debugwhite"
                if material and not IsMaterialAllowedName(material) then
                    continue
                end
                
                -- Create or get cached material
                local mat = GetCachedMaterial(material)
                
                -- Validate vertices for this group
                local valid = true
                if RenderCore and RenderCore.ValidateVertex then
                    for _, vert in ipairs(group.triangles) do
                        if not RenderCore.ValidateVertex(vert.pos) then
                            valid = false
                            break
                        end
                    end
                end
                if not valid then
                    continue
                end

                -- Create mesh for this group
                local mesh = Mesh()
                mesh:BuildFromTriangles(group.triangles)
                
                -- Add to processed meshes (store raw triangles for combining)
                table.insert(processedMeshes, {
                    mesh = mesh,
                    material = mat,
                    triangles = group.triangles -- Store raw vertex data for mesh combining
                })

                if RenderCore and RenderCore.TrackMesh then
                    RenderCore.TrackMesh(mesh)
                end
                
                -- Update bounds and vertex count
                totalVertexCount = totalVertexCount + #group.triangles
                for _, vert in ipairs(group.triangles) do
                    if vert.pos.x < mins.x then mins.x = vert.pos.x end
                    if vert.pos.y < mins.y then mins.y = vert.pos.y end
                    if vert.pos.z < mins.z then mins.z = vert.pos.z end
                    if vert.pos.x > maxs.x then maxs.x = vert.pos.x end
                    if vert.pos.y > maxs.y then maxs.y = vert.pos.y end
                    if vert.pos.z > maxs.z then maxs.z = vert.pos.z end
                end
            end
        end
        
        if #processedMeshes > 0 then
            -- Store in the cache
            meshCache[cacheKey] = {
                meshes = processedMeshes,
                mins = mins,
                maxs = maxs,
                vertexCount = totalVertexCount
            }
            
            DebugPrint("Cached mesh for model:", modelPath, "skin:", prop.skin, "#mesh groups:", #processedMeshes, "vertices:", totalVertexCount)
        else
            DebugPrint("No valid mesh groups found for model:", modelPath)
            meshCache[cacheKey] = {
                meshes = nil,
                error = true
            }
            return nil
        end
    elseif meshCache[cacheKey].error then
        return nil  -- Skip previously failed models
    end
    
    -- Link to the cached mesh data
    prop.cachedMesh = meshCache[cacheKey]
    prop.vertexCount = meshCache[cacheKey].vertexCount or 0
    return prop
end

-- Separate skybox props from world props
local function SeparateSkyboxProps()
    skyboxProps = {}
    worldProps = {}
    
    for _, prop in ipairs(cachedStaticProps) do
        if prop.isSkybox then
            table.insert(skyboxProps, prop)
        else
            table.insert(worldProps, prop)
        end
    end
    
    print(string.format("[Static Render] Separated props: %d world props, %d skybox props", 
                        #worldProps, #skyboxProps))
end

-- Cache static props from NikNaks data
local function CacheMapStaticProps()
    if isCachingInProgress then return end
    
    DebugPrint("Checking NikNaks availability...")
    
    if not NikNaks then
        DebugPrint("NikNaks module not found!")
        timer.Simple(1, CacheMapStaticProps)
        return
    end
    
    if not NikNaks.CurrentMap then
        DebugPrint("NikNaks.CurrentMap not available yet.")
        timer.Simple(1, CacheMapStaticProps)
        return
    end
    
    if not NikNaks.CurrentMap.GetStaticProps then
        DebugPrint("NikNaks.CurrentMap.GetStaticProps function doesn't exist!")
        DebugPrint("Available functions:", table.concat(table.GetKeys(NikNaks.CurrentMap), ", "))
        timer.Simple(1, CacheMapStaticProps)
        return
    end
    
    isCachingInProgress = true
    print("[Static Render] Starting static prop data caching...")
    sprBuildStats.startTime = SysTime()
    sprBuildStats.endTime = 0
    sprBuildStats.built = 0
    sprBuildStats.active = true
    
    -- Clear previous caches
    table.Empty(cachedStaticProps)
    
    -- Get static props data from NikNaks
    local okProps, staticPropsRaw = pcall(function() return NikNaks.CurrentMap:GetStaticProps() end)
    if not okProps then
        print("[Static Render] GetStaticProps() errored")
        isCachingInProgress = false
        sprBuildStats.active = false
        isDataReady = true
        return
    end
    if not staticPropsRaw or type(staticPropsRaw) ~= "table" then
        print("[Static Render] GetStaticProps() returned invalid data:", staticPropsRaw)
        isCachingInProgress = false
        sprBuildStats.active = false
        isDataReady = true -- Mark as ready to prevent retries
        return
    end
    
    -- Debug output
    print("[Static Render] Retrieved", #staticPropsRaw, "static props")
    
    -- Coroutine-driven processing within a per-frame budget
    local processedSoFar = 0
    local skippedSoFar = 0
    local co
    co = coroutine.create(function()
        local startTime = SysTime()
        local frameBudget = 0.003
        for i = 1, #staticPropsRaw do
            local propData = staticPropsRaw[i]
            local prop = ProcessStaticProp(propData)
            if prop then
                table.insert(cachedStaticProps, prop)
                processedSoFar = processedSoFar + 1
                sprBuildStats.built = sprBuildStats.built + 1
            else
                skippedSoFar = skippedSoFar + 1
            end
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
        SeparateSkyboxProps()
        
        -- Build combined meshes once after caching
        if convar_UseMeshCombining:GetBool() then
            print("[Static Render] Building combined meshes...")
            BuildAllCombinedMeshes()
            print(string.format("[Static Render] Built %d combined meshes", table.Count(combinedMeshes)))
        end
        
        isDataReady = true
        isCachingInProgress = false
        print(string.format("[Static Render] Caching complete. %d static props processed, %d skipped.", 
                           processedSoFar, skippedSoFar))
        sprStats.total = processedSoFar
        sprBuildStats.endTime = SysTime()
        sprBuildStats.active = false
    end)
    -- Schedule coroutine advancement via RenderCore job system to reduce timer overhead
    local jobId = "StaticPropsCacheJob"
    RenderCore.ScheduleJob(jobId, function()
        if not co or coroutine.status(co) == "dead" then
            isCachingInProgress = false
            sprBuildStats.active = false
            return false
        end
        
        local ok, err = coroutine.resume(co)
        if not ok then
            ErrorNoHalt("[Static Render] Cache coroutine error: " .. tostring(err) .. "\n")
            isCachingInProgress = false
            sprBuildStats.active = false
            sprBuildStats.built = 0
            isDataReady = true -- Mark as ready to prevent infinite retries
            return false
        end
        
        local isDead = coroutine.status(co) == "dead"
        if isDead then
            isCachingInProgress = false
            sprBuildStats.active = false
        end
        return not isDead
    end)
end

-- Skybox detection hooks
RenderCore.Register("PreDrawSkyBox", "RTXStaticPropsSkyboxDetection", function()
    bDrawingSkybox = true
end)

RenderCore.Register("PostDrawSkyBox", "RTXStaticPropsSkyboxDetection", function()
    bDrawingSkybox = false
end)

-- Hook to initiate caching when the map is ready
RenderCore.Register("InitPostEntity", "CustomStaticRender_InitCache", function()
    -- Only start caching if the addon is enabled
    if convar_Enable:GetBool() then
        -- Delay slightly to ensure NikNaks has loaded its data
        timer.Simple(3, CacheMapStaticProps)
    else
        print("[Custom Static Renderer] Skipping initial cache (addon disabled)")
    end
end)

-- Clean up caches on disconnect/map change
RenderCore.Register("ShutDown", "CustomStaticRender_Cleanup", function()
    print("[Static Render] Cleaning up caches.")
    -- Rely on core to destroy tracked meshes
    if RenderCore and RenderCore.DestroyTrackedMeshes then
        RenderCore.DestroyTrackedMeshes()
    end
    table.Empty(cachedStaticProps)
    table.Empty(skyboxProps)
    table.Empty(worldProps)
    table.Empty(meshCache)
    table.Empty(combinedMeshes)
    combinedMeshesBuilt = false
    
    isDataReady = false
    isCachingInProgress = false
end)

-- Render the static props
RenderCore.Register("PreDrawOpaqueRenderables", "CustomStaticRender_DrawProps", function(bDrawingDepth, bDrawingSkybox_param)
    if not convar_Enable:GetBool() or not isDataReady or isCachingInProgress then
        return
    end
    
    -- Skip rendering static props in offscreen RTs (e.g., rear-view camera with dynamic_only filter)
    if RenderCore and RenderCore.IsOffscreen and RenderCore.IsOffscreen() then
        return
    end
    
    -- Choose which prop list to render based on skybox state
    local propsToRender = bDrawingSkybox and skyboxProps or worldProps
    local cacheKey = bDrawingSkybox and "skybox" or "world"
    
    -- Frame skip optimization: only rebuild visibility list every N frames
    local currentFrame = FrameNumber()
    local frameSkip = math.max(1, convar_FrameSkip:GetInt())
    local shouldUpdate = (currentFrame - lastUpdateFrame[cacheKey]) >= frameSkip
    
    -- Update visibility list if needed
    if shouldUpdate then
        lastUpdateFrame[cacheKey] = currentFrame
        table.Empty(cachedRenderList[cacheKey])
    end
    
    if #propsToRender == 0 then
        if convar_Debug:GetBool() then
            local frameCount = FrameNumber()
            if lastDebugFrame ~= frameCount then
                lastDebugFrame = frameCount
            end
        end
        return
    end
    
    local renderedProps = 0
    local skippedProps = 0
    
    -- Use cached EyePos from RenderCore to avoid redundant engine calls
    local playerPos = (RenderCore and RenderCore.GetCachedEyePos and RenderCore.GetCachedEyePos()) or EyePos()
    -- Use centralized PVS from RenderCore
    local usePVS = convar_UsePVS:GetBool()
    local pvs = (usePVS and RenderCore and RenderCore.GetPVS) and RenderCore.GetPVS(playerPos) or nil
    
    -- Cache ConVar checks to avoid duplicate calls
    local useMeshCombining = convar_UseMeshCombining:GetBool()
    local shouldDebug = convar_Debug:GetBool()
    local frameCount = FrameNumber()
    local isNewFrame = lastDebugFrame ~= frameCount

    if shouldDebug and isNewFrame then
        DebugPrint("Attempting to render", #propsToRender, "props in " .. (bDrawingSkybox and "skybox" or "world"))
    end
    
    -- Build or use cached render list
    if shouldUpdate then
        -- Pre-compute PVS visibility for all clusters once (faster than per-prop checks)
        local visibleClusters = {}
        if pvs then
            for cl, visible in pairs(pvs) do
                if visible then
                    visibleClusters[cl] = true
                end
            end
        end
        
        -- Compute safety distance once
        local safetyDist = convar_PVSSafetyDistance:GetFloat()
        local safetyDistSqr = safetyDist * safetyDist
        
        for _, prop in ipairs(propsToRender) do
            local meshData = prop.cachedMesh
            if not meshData or not meshData.meshes then
                skippedProps = skippedProps + 1
                continue
            end
            -- PVS culling for world props only (skip skybox props)
            if not bDrawingSkybox and pvs and prop.clusters and next(prop.clusters) then
                -- Safety distance check: always render props very close to player
                local withinSafetyDistance = false
                if safetyDist > 0 and playerPos then
                    local distSqr = prop.origin:DistToSqr(playerPos)
                    withinSafetyDistance = distSqr < safetyDistSqr
                end
                
                if not withinSafetyDistance then
                    -- Fast cluster visibility check using pre-computed visible set
                    local anyVisible = false
                    for cl in pairs(prop.clusters) do
                        if visibleClusters[cl] then
                            anyVisible = true
                            break
                        end
                    end
                    if not anyVisible then
                        skippedProps = skippedProps + 1
                        continue
                    end
                end
            end
            
            -- Add to render list
            local list = cachedRenderList[cacheKey]
            list[#list + 1] = prop
        end
    end
    
    -- Render combined meshes or individual props
    if useMeshCombining and combinedMeshesBuilt then
        -- Track unique props to avoid double-counting (props with multiple materials)
        local renderedPropIndices = {}
        
        -- Render pre-built combined meshes (just check PVS per material group)
        for matName, combined in pairs(combinedMeshes) do
            -- Fast PVS check using cached cluster set
            local anyVisible = false
            if not pvs or not bDrawingSkybox then
                if not pvs then
                    anyVisible = true -- No PVS, render everything
                else
                    -- Check if any cached cluster is visible (much faster than iterating props)
                    if combined.clusters then
                        for cl in pairs(combined.clusters) do
                            if pvs[cl] then
                                anyVisible = true
                                break
                            end
                        end
                    else
                        anyVisible = true -- No cluster data, render to be safe
                    end
                end
            else
                anyVisible = true -- Skybox props always visible
            end
            
            if anyVisible and combined.mesh and combined.material then
                RenderCore.Submit({
                    material = combined.material,
                    mesh = combined.mesh,
                    translucent = false
                })
                
                -- Count unique props only (avoid double-counting multi-material props)
                for _, propIdx in ipairs(combined.propIndices or {}) do
                    if not renderedPropIndices[propIdx] then
                        renderedPropIndices[propIdx] = true
                        renderedProps = renderedProps + 1
                    end
                end
            end
        end
    else
        -- Fallback: render individual props
        for _, prop in ipairs(cachedRenderList[cacheKey]) do
            for _, meshInfo in ipairs(prop.cachedMesh.meshes) do
                if meshInfo.mesh and meshInfo.material then
                    RenderCore.Submit({
                        material = meshInfo.material,
                        mesh = meshInfo.mesh,
                        matrix = prop.matrix,
                        translucent = false,
                        color = prop.color
                    })
                end
            end
            renderedProps = renderedProps + 1
        end
    end
    
    sprStats.rendered = renderedProps
    sprStats.skipped = skippedProps
    
    -- Debug output
    if shouldDebug and isNewFrame then
        DebugPrint("Rendered", renderedProps, "props in " .. (bDrawingSkybox and "skybox" or "world"),
                  skippedProps, "skipped")
        lastDebugFrame = frameCount
    end
end)

-- Add reload command
concommand.Add("rtx_spr_reload", function()
    print("[Static Render] Manually reloading cache...")
    isDataReady = false
    isCachingInProgress = false
    
    if RenderCore and RenderCore.DestroyTrackedMeshes then
        RenderCore.DestroyTrackedMeshes()
    end
    table.Empty(cachedStaticProps)
    table.Empty(skyboxProps)
    table.Empty(worldProps)
    table.Empty(meshCache)
    
    timer.Simple(0.1, CacheMapStaticProps)
end)

-- Disable engine props only if custom renderer is enabled
if convar_Enable:GetBool() then
    RunConsoleCommand("r_drawstaticprops", "0")
end

-- ConVar Changes
cvars.AddChangeCallback("rtx_spr_enable", function(_, _, new)
    if tobool(new) then
        RunConsoleCommand("r_drawstaticprops", "0")
    else
        RunConsoleCommand("r_drawstaticprops", "1")
    end
end, "RTXStaticPropEnable")

print("[Custom Static Renderer] Loaded.")

-- Stats provider
RenderCore.RegisterStats("StaticProps", function()
    local built = sprBuildStats.built or 0
    local t = (sprBuildStats.endTime > 0 and sprBuildStats.endTime or SysTime()) - (sprBuildStats.startTime or 0)
    local rate = (t > 0) and (built / t) or 0
    return string.format("Static props: %d/%d (skipped:%d) | build: %.2fs, %.1f/s", sprStats.rendered or 0, sprStats.total or 0, sprStats.skipped or 0, t, rate)
end)

-- Rebuild sink and debounced cvar watchers
RenderCore.RegisterRebuildSink("StaticPropsRebuild", function(token, reason)
    -- Best-effort: clear current caches and rebuild
    isDataReady = false
    isCachingInProgress = false
    if RenderCore and RenderCore.DestroyTrackedMeshes then
        RenderCore.DestroyTrackedMeshes()
    end
    table.Empty(cachedStaticProps)
    table.Empty(skyboxProps)
    table.Empty(worldProps)
    table.Empty(meshCache)
    timer.Simple(0.1, CacheMapStaticProps)
end)

local function DebounceRebuildOnCvar(name)
    cvars.AddChangeCallback(name, function()
        if RenderCore and RenderCore.RequestRebuild then
            RenderCore.RequestRebuild(name)
        end
    end, "StaticPropsRebuild-" .. name)
end

DebounceRebuildOnCvar("rtx_spr_mat_whitelist")
DebounceRebuildOnCvar("rtx_spr_mat_blacklist")
DebounceRebuildOnCvar("rtx_spr_distance")