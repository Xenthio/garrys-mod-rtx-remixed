--[[
================================================================================
    RTX Material Processing Pipeline
    
    Unified, ordered material processing for RTX Remix integration.
    
    This module replaces fragmented texture/material processing with a single,
    well-architected pipeline that guarantees correct execution order.
    
    PIPELINE STAGES:
    ================
    1. HASH COLLISION FIX - Detect solid-color textures and create unique 
       replacement textures to prevent Remix hash collisions
       
    2. AUTOCATEGORIZATION - Apply Remix category flags (decals, particles, 
       emissive) based on material properties
       
    3. TOPBR CONVERSION - Queue for PBR texture generation and upload
    
    DESIGN PRINCIPLES:
    ==================
    - Single source of truth for material processing order
    - Non-blocking batch processing to prevent frame drops
    - Integration with existing RemixCategoryManager and RTXToPBR systems
    - Clean separation of concerns with extensible stage system
    - Comprehensive statistics and debugging
    
    USAGE:
    ======
    -- Queue a material for processing
    RTXMaterialPipeline.QueueMaterial("models/mymodel/texture")
    
    -- Process all materials immediately (blocking)
    RTXMaterialPipeline.ProcessAllNow()
    
    -- Check if material was processed
    local wasProcessed = RTXMaterialPipeline.IsMaterialProcessed("models/mymodel/texture")
    
    HOOKS:
    ======
    hook.Add("RTX_MaterialPipelineStageComplete", "MyHandler", function(materialName, stageName)
        -- Called after each stage completes for a material
    end)
    
    hook.Add("RTX_MaterialPipelineComplete", "MyHandler", function(materialName)
        -- Called when a material finishes all pipeline stages
    end)
    
    AUTHORS: RTX Remix GMod Team
    VERSION: 2.0 - Unified Pipeline
================================================================================
]]--

if not CLIENT then return end
if not (BRANCH == "x86-64" or BRANCH == "chromium") then return end

-- =============================================================================
-- GLOBAL PIPELINE MODULE
-- =============================================================================

RTXMaterialPipeline = RTXMaterialPipeline or {}

-- =============================================================================
-- CONVARS - Configuration
-- =============================================================================

-- Master enable/disable
CreateClientConVar("rtx_mat_enabled", "1", true, false, 
    "Enable/disable the entire material processing pipeline")

-- Per-stage enable controls
CreateClientConVar("rtx_mat_hashfix_enabled", "1", true, false, 
    "Enable/disable hash collision fixer stage")
CreateClientConVar("rtx_mat_category_enabled", "1", true, false, 
    "Enable/disable auto-categorization stage")
CreateClientConVar("rtx_mat_pbr_enabled", "1", true, false, 
    "Enable/disable ToPBR conversion stage")

-- Pipeline settings
CreateClientConVar("rtx_mat_debug", "0", true, false, 
    "Enable debug output for material pipeline")
CreateClientConVar("rtx_mat_batch", "5", true, false, 
    "Number of materials processed per tick (1-20)")
CreateClientConVar("rtx_mat_delay", "2", true, false, 
    "Delay in seconds before auto-processing starts")
CreateClientConVar("rtx_mat_continuous", "1", true, false, 
    "Continuously discover new materials (1=enabled)")
CreateClientConVar("rtx_mat_continuous_interval", "5", true, false, 
    "Interval in seconds between material discovery scans")

-- =============================================================================
-- INTERNAL STATE
-- =============================================================================

local State = {
    initialized = false,
    processing = false,
    discoveryTimer = nil,
    
    -- Processing queue
    queue = {},
    queueSet = {},  -- Fast lookup for duplicate prevention
    
    -- Tracking
    processedMaterials = {},  -- Materials that completed all stages
    
    -- Statistics
    stats = {
        totalQueued = 0,
        totalProcessed = 0,
        hashCollisionsDetected = 0,
        hashCollisionsFixed = 0,
        categorized = 0,
        queuedForToPBR = 0,
        errors = 0
    }
}

-- Unique textures cache for hash collision fixes
local UniqueTextures = {}

-- =============================================================================
-- UTILITY FUNCTIONS
-- =============================================================================

local function DebugPrint(...)
    if GetConVar("rtx_mat_debug"):GetBool() then
        MsgC(Color(100, 200, 255), "[RTX Pipeline] ", Color(255, 255, 255), ...)
        MsgC(Color(255, 255, 255), "\n")
    end
end

local function InfoPrint(...)
    MsgC(Color(100, 255, 150), "[RTX Pipeline] ", Color(255, 255, 255), ...)
    MsgC(Color(255, 255, 255), "\n")
end

local function WarnPrint(...)
    MsgC(Color(255, 200, 100), "[RTX Pipeline] ", Color(255, 255, 255), ...)
    MsgC(Color(255, 255, 255), "\n")
end

local function ErrorPrint(...)
    MsgC(Color(255, 100, 100), "[RTX Pipeline] ", Color(255, 255, 255), ...)
    MsgC(Color(255, 255, 255), "\n")
    State.stats.errors = State.stats.errors + 1
end

-- Generate deterministic hash from string
local function StringHash(str)
    local hash = 5381
    for i = 1, #str do
        hash = ((hash * 33) + string.byte(str, i)) % 4294967296
    end
    return hash
end

-- Get a unique color based on material name (for hash collision fix)
local function GetUniqueColor(materialName)
    local hash = StringHash(materialName)
    -- Keep values low to minimize visual impact but unique enough for different hashes
    local r = (hash % 64) / 255
    local g = ((hash / 64) % 64) / 255
    local b = ((hash / 4096) % 64) / 255
    return r, g, b
end

-- =============================================================================
-- STAGE 1: HASH COLLISION FIX
-- =============================================================================

--[[
    Detects solid-color textures using VTF file analysis (via C++ HashCollisionFixer)
    and creates unique replacement textures to prevent RTX Remix hash collisions.
    
    This is CRITICAL for materials like envballs that share identical solid colors
    but represent different materials.
]]--
local function Stage1_HashCollisionFix(materialName)
    -- Check if stage is enabled
    if not GetConVar("rtx_mat_hashfix_enabled"):GetBool() then
        DebugPrint("Stage 1 SKIP (disabled): ", materialName)
        return true, false
    end
    
    -- Check if HashCollisionFixer C++ module is available
    if not HashCollisionFixer then
        DebugPrint("Stage 1 SKIP (no HashCollisionFixer): ", materialName)
        return true, false  -- Continue, but wasn't fixed
    end
    
    -- Get the material object
    local mat = Material(materialName)
    if not mat or mat:IsError() then
        DebugPrint("Stage 1 SKIP (invalid material): ", materialName)
        return true, false
    end
    
    -- Get the $basetexture
    local baseTex = mat:GetTexture("$basetexture")
    if not baseTex then
        DebugPrint("Stage 1 SKIP (no $basetexture): ", materialName)
        return true, false
    end
    
    local texturePath = baseTex:GetName()
    if not texturePath or texturePath == "" then
        return true, false
    end
    
    -- Check if it's a solid color texture using VTF analysis
    local isSolid, r, g, b, a = false, 0, 0, 0, 255
    
    if HashCollisionFixer.CheckSolidColor then
        local success
        success, r, g, b, a = HashCollisionFixer.CheckSolidColor(texturePath)
        
        -- Handle error case where second return is error string
        if type(r) == "string" then
            DebugPrint("Stage 1 VTF read error for ", texturePath, ": ", r)
            return true, false
        end
        
        isSolid = success
    end
    
    if not isSolid then
        DebugPrint("Stage 1 PASS (not solid color): ", materialName)
        return true, false
    end
    
    State.stats.hashCollisionsDetected = State.stats.hashCollisionsDetected + 1
    DebugPrint("Stage 1 DETECTED solid color: ", materialName, " RGBA(", r, ",", g, ",", b, ",", a, ")")
    
    -- Create a unique replacement texture
    local uniqueKey = "rtx_unique_" .. util.CRC(materialName)
    
    if not UniqueTextures[uniqueKey] then
        -- Generate unique color offset based on material name
        local ur, ug, ub = GetUniqueColor(materialName)
        
        -- Blend original color with unique offset
        local finalR = math.Clamp((r or 0) + ur * 255, 0, 255)
        local finalG = math.Clamp((g or 0) + ug * 255, 0, 255)
        local finalB = math.Clamp((b or 0) + ub * 255, 0, 255)
        
        local uniqueMat = CreateMaterial(uniqueKey, "UnlitGeneric", {
            ["$basetexture"] = "color/white",
            ["$color"] = string.format("{%d %d %d}", finalR, finalG, finalB),
            ["$alpha"] = tostring((a or 255) / 255),
            ["$vertexcolor"] = 0,
            ["$vertexalpha"] = 0,
        })
        
        if uniqueMat and not uniqueMat:IsError() then
            UniqueTextures[uniqueKey] = uniqueMat
            DebugPrint("Created unique texture: ", uniqueKey)
        else
            ErrorPrint("Failed to create unique texture for: ", materialName)
            return true, false
        end
    end
    
    -- Swap the material's $basetexture
    local uniqueMat = UniqueTextures[uniqueKey]
    if uniqueMat then
        local uniqueTex = uniqueMat:GetTexture("$basetexture")
        if uniqueTex then
            mat:SetTexture("$basetexture", uniqueTex)
            State.stats.hashCollisionsFixed = State.stats.hashCollisionsFixed + 1
            
            -- Notify C++ that we fixed this
            if HashCollisionFixer.MarkMaterialFixed then
                HashCollisionFixer.MarkMaterialFixed(materialName)
            end
            
            DebugPrint("Stage 1 FIXED: ", materialName)
            
            -- Fire hook
            hook.Run("RTX_MaterialPipelineStageComplete", materialName, "HashCollisionFix")
            return true, true  -- Continue, was fixed
        end
    end
    
    return true, false
end

-- =============================================================================
-- STAGE 2: AUTOCATEGORIZATION
-- =============================================================================

--[[
    Applies Remix category flags based on material properties.
    Integrates with existing RemixCategoryManager system.
    
    Categories:
    - DECAL_STATIC (0x1000) - For materials with $decal
    - PARTICLE (0x400) - For particle effect materials  
    - LEGACY_EMISSIVE (0x1000000) - For self-illuminated materials
]]--
local function Stage2_Autocategorize(materialName)
    -- Check if stage is enabled
    if not GetConVar("rtx_mat_category_enabled"):GetBool() then
        DebugPrint("Stage 2 SKIP (disabled): ", materialName)
        return true, false
    end
    
    -- Check if RemixCategoryManager is available
    if not RemixCategoryManager then
        DebugPrint("Stage 2 SKIP (no RemixCategoryManager): ", materialName)
        return true, false
    end
    
    -- Defer to RemixCategoryManager's own autocategorization if available
    if RemixCategoryManager.AutoCategorizeMaterial then
        local categorized = RemixCategoryManager.AutoCategorizeMaterial(materialName)
        if categorized then
            State.stats.categorized = State.stats.categorized + 1
            DebugPrint("Stage 2 CATEGORIZED (via manager): ", materialName)
            hook.Run("RTX_MaterialPipelineStageComplete", materialName, "Autocategorization")
            return true, true
        end
        return true, false
    end
    
    -- Fallback: Manual category checks
    local mat = Material(materialName)
    if not mat or mat:IsError() then
        return true, false
    end
    
    local categorized = false
    local CATEGORY = RemixCategoryManager.CATEGORY or {}
    
    -- Check for decals
    if RemixCategoryManager.IsMaterialDecal then
        if RemixCategoryManager.IsMaterialDecal(materialName) then
            -- Apply DECAL_STATIC category
            if RemixCategoryManager.SetMaterialCategory then
                RemixCategoryManager.SetMaterialCategory(materialName, CATEGORY.DECAL_STATIC or 0x1000)
                categorized = true
                DebugPrint("Stage 2 CATEGORIZED as DECAL: ", materialName)
            end
        end
    end
    
    -- Check for particles  
    if not categorized and RemixCategoryManager.IsMaterialParticle then
        if RemixCategoryManager.IsMaterialParticle(materialName) then
            if RemixCategoryManager.SetMaterialCategory then
                RemixCategoryManager.SetMaterialCategory(materialName, CATEGORY.PARTICLE or 0x400)
                categorized = true
                DebugPrint("Stage 2 CATEGORIZED as PARTICLE: ", materialName)
            end
        end
    end
    
    -- Check for emissive
    if not categorized and RemixCategoryManager.IsMaterialEmissive then
        if RemixCategoryManager.IsMaterialEmissive(materialName) then
            if RemixCategoryManager.SetMaterialCategory then
                RemixCategoryManager.SetMaterialCategory(materialName, CATEGORY.LEGACY_EMISSIVE or 0x1000000)
                categorized = true
                DebugPrint("Stage 2 CATEGORIZED as EMISSIVE: ", materialName)
            end
        end
    end
    
    if categorized then
        State.stats.categorized = State.stats.categorized + 1
        hook.Run("RTX_MaterialPipelineStageComplete", materialName, "Autocategorization")
    end
    
    return true, categorized
end

-- =============================================================================
-- STAGE 3: QUEUE FOR TOPBR
-- =============================================================================

--[[
    Queues the material for PBR texture conversion.
    Integrates with existing RTXToPBR / LegacyTextureProcessor system.
]]--
local function Stage3_QueueForToPBR(materialName)
    -- Check if stage is enabled
    if not GetConVar("rtx_mat_pbr_enabled"):GetBool() then
        DebugPrint("Stage 3 SKIP (disabled): ", materialName)
        return true, false
    end
    
    -- Check what ToPBR system is available
    local processor = LegacyTextureProcessor or VTFConverter
    
    if not processor then
        -- Try RTXToPBR Lua wrapper
        if RTXToPBR and RTXToPBR.QueueMaterial then
            RTXToPBR.QueueMaterial(materialName)
            State.stats.queuedForToPBR = State.stats.queuedForToPBR + 1
            DebugPrint("Stage 3 QUEUED (via RTXToPBR): ", materialName)
            hook.Run("RTX_MaterialPipelineStageComplete", materialName, "ToPBRQueue")
            return true, true
        end
        
        DebugPrint("Stage 3 SKIP (no ToPBR system): ", materialName)
        return true, false
    end
    
    -- Use C++ LegacyTextureProcessor directly
    if processor.QueueMaterial then
        processor.QueueMaterial(materialName)
        State.stats.queuedForToPBR = State.stats.queuedForToPBR + 1
        DebugPrint("Stage 3 QUEUED (via processor): ", materialName)
        hook.Run("RTX_MaterialPipelineStageComplete", materialName, "ToPBRQueue")
        return true, true
    end
    
    -- Fallback: queue for batch processing
    if processor.QueueMaterialsForProcessing then
        -- Material will be picked up in next batch
        State.stats.queuedForToPBR = State.stats.queuedForToPBR + 1
        DebugPrint("Stage 3 QUEUED (batch): ", materialName)
        return true, true
    end
    
    return true, false
end

-- =============================================================================
-- PIPELINE CORE
-- =============================================================================

--[[
    Process a single material through all pipeline stages in order.
]]--
local function ProcessMaterial(materialName)
    if State.processedMaterials[materialName] then
        return  -- Already processed
    end
    
    DebugPrint("PROCESSING: ", materialName)
    
    -- Stage 1: Hash Collision Fix
    local s1Continue, s1Result = Stage1_HashCollisionFix(materialName)
    if not s1Continue then
        ErrorPrint("Stage 1 ABORT for: ", materialName)
        return
    end
    
    -- Stage 2: Autocategorization
    local s2Continue, s2Result = Stage2_Autocategorize(materialName)
    if not s2Continue then
        ErrorPrint("Stage 2 ABORT for: ", materialName)
        return
    end
    
    -- Stage 3: Queue for ToPBR
    local s3Continue, s3Result = Stage3_QueueForToPBR(materialName)
    if not s3Continue then
        ErrorPrint("Stage 3 ABORT for: ", materialName)
        return
    end
    
    -- Mark as fully processed
    State.processedMaterials[materialName] = true
    State.stats.totalProcessed = State.stats.totalProcessed + 1
    
    -- Fire completion hook
    hook.Run("RTX_MaterialPipelineComplete", materialName)
    
    DebugPrint("COMPLETE: ", materialName, 
        " (HashFix:", s1Result and "Y" or "N",
        " Cat:", s2Result and "Y" or "N",
        " ToPBR:", s3Result and "Y" or "N", ")")
end

--[[
    Process a batch of materials from the queue.
    Called each tick when processing is active.
]]--
local function ProcessBatch()
    if not GetConVar("rtx_mat_enabled"):GetBool() then
        return
    end
    
    if #State.queue == 0 then
        State.processing = false
        return
    end
    
    State.processing = true
    local batchSize = math.Clamp(GetConVar("rtx_mat_batch"):GetInt(), 1, 20)
    local processed = 0
    
    while processed < batchSize and #State.queue > 0 do
        local materialName = table.remove(State.queue, 1)
        State.queueSet[materialName] = nil
        
        ProcessMaterial(materialName)
        processed = processed + 1
    end
    
    if #State.queue == 0 then
        State.processing = false
    end
end

-- =============================================================================
-- PUBLIC API
-- =============================================================================

--[[
    Queue a material for pipeline processing.
    @param materialName string - The material path/name
]]--
function RTXMaterialPipeline.QueueMaterial(materialName)
    if not materialName or materialName == "" then return end
    if not GetConVar("rtx_mat_enabled"):GetBool() then return end
    
    -- Skip if already processed or queued
    if State.processedMaterials[materialName] then return end
    if State.queueSet[materialName] then return end
    
    -- Add to queue
    table.insert(State.queue, materialName)
    State.queueSet[materialName] = true
    State.stats.totalQueued = State.stats.totalQueued + 1
    
    DebugPrint("QUEUED: ", materialName, " (", #State.queue, " pending)")
end

--[[
    Queue multiple materials for processing.
    @param materials table - Array of material names
]]--
function RTXMaterialPipeline.QueueMaterials(materials)
    for _, matName in ipairs(materials) do
        RTXMaterialPipeline.QueueMaterial(matName)
    end
end

--[[
    Check if a material has been fully processed.
    @param materialName string - The material path/name
    @return boolean
]]--
function RTXMaterialPipeline.IsMaterialProcessed(materialName)
    return State.processedMaterials[materialName] == true
end

--[[
    Get current pipeline statistics.
    @return table - Statistics table
]]--
function RTXMaterialPipeline.GetStats()
    return {
        queueLength = #State.queue,
        totalQueued = State.stats.totalQueued,
        totalProcessed = State.stats.totalProcessed,
        hashCollisionsDetected = State.stats.hashCollisionsDetected,
        hashCollisionsFixed = State.stats.hashCollisionsFixed,
        categorized = State.stats.categorized,
        queuedForToPBR = State.stats.queuedForToPBR,
        errors = State.stats.errors,
        isProcessing = State.processing,
        uniqueTexturesCreated = table.Count(UniqueTextures)
    }
end

--[[
    Discover and queue all materials from world and entities.
    @return number - Count of materials discovered
]]--
function RTXMaterialPipeline.DiscoverAllMaterials()
    local discovered = 0
    local seen = {}
    
    -- Get world materials
    local worldEnt = game.GetWorld()
    if IsValid(worldEnt) then
        for _, matName in ipairs(worldEnt:GetMaterials()) do
            if matName and matName ~= "" and not seen[matName] then
                seen[matName] = true
                RTXMaterialPipeline.QueueMaterial(matName)
                discovered = discovered + 1
            end
        end
    end
    
    -- Get materials from all entities
    for _, ent in ipairs(ents.GetAll()) do
        if IsValid(ent) then
            for _, matName in ipairs(ent:GetMaterials()) do
                if matName and matName ~= "" and not seen[matName] then
                    seen[matName] = true
                    RTXMaterialPipeline.QueueMaterial(matName)
                    discovered = discovered + 1
                end
            end
        end
    end
    
    InfoPrint("Discovered ", discovered, " materials")
    return discovered
end

--[[
    Process all queued materials immediately (blocking).
    Use with caution - may cause frame drops on large queues.
]]--
function RTXMaterialPipeline.ProcessAllNow()
    RTXMaterialPipeline.DiscoverAllMaterials()
    
    local count = #State.queue
    InfoPrint("Processing ", count, " materials synchronously...")
    
    while #State.queue > 0 do
        local materialName = table.remove(State.queue, 1)
        State.queueSet[materialName] = nil
        ProcessMaterial(materialName)
    end
    
    State.processing = false
    
    local stats = RTXMaterialPipeline.GetStats()
    InfoPrint("Complete! Processed: ", stats.totalProcessed,
        " | Hash Fixes: ", stats.hashCollisionsFixed,
        " | Categorized: ", stats.categorized,
        " | ToPBR: ", stats.queuedForToPBR)
end

--[[
    Reset all pipeline state.
]]--
function RTXMaterialPipeline.Reset()
    State.queue = {}
    State.queueSet = {}
    State.processedMaterials = {}
    State.processing = false
    State.stats = {
        totalQueued = 0,
        totalProcessed = 0,
        hashCollisionsDetected = 0,
        hashCollisionsFixed = 0,
        categorized = 0,
        queuedForToPBR = 0,
        errors = 0
    }
    UniqueTextures = {}
    
    DebugPrint("Pipeline RESET")
end

--[[
    Initialize the pipeline (called automatically).
]]--
function RTXMaterialPipeline.Initialize()
    if State.initialized then return end
    State.initialized = true
    
    InfoPrint("Initializing material processing pipeline...")
    
    -- Wait for other systems to initialize, then start discovery
    local delay = GetConVar("rtx_mat_delay"):GetFloat()
    timer.Simple(delay, function()
        if not GetConVar("rtx_mat_enabled"):GetBool() then return end
        
        InfoPrint("Starting automatic material discovery...")
        RTXMaterialPipeline.DiscoverAllMaterials()
        
        -- Set up continuous discovery if enabled
        if GetConVar("rtx_mat_continuous"):GetBool() then
            local interval = GetConVar("rtx_mat_continuous_interval"):GetFloat()
            timer.Create("RTXPipeline_Discovery", interval, 0, function()
                if not GetConVar("rtx_mat_enabled"):GetBool() then return end
                if not GetConVar("rtx_mat_continuous"):GetBool() then return end
                
                RTXMaterialPipeline.DiscoverAllMaterials()
            end)
        end
    end)
end

-- =============================================================================
-- HOOKS & TIMERS
-- =============================================================================

-- Batch processing on Think
hook.Add("Think", "RTXMaterialPipeline_Process", ProcessBatch)

-- Initialize on map load
hook.Add("InitPostEntity", "RTXMaterialPipeline_Init", function()
    -- Small delay to ensure everything is loaded
    timer.Simple(0.5, function()
        RTXMaterialPipeline.Initialize()
    end)
end)

-- Cleanup on shutdown
hook.Add("ShutDown", "RTXMaterialPipeline_Cleanup", function()
    timer.Remove("RTXPipeline_Discovery")
    RTXMaterialPipeline.Reset()
end)

-- =============================================================================
-- CONSOLE COMMANDS
-- =============================================================================

concommand.Add("rtx_mat_process", function()
    InfoPrint("Processing all materials now...")
    RTXMaterialPipeline.ProcessAllNow()
end, nil, "Process all materials through the RTX pipeline immediately")

concommand.Add("rtx_mat_discover", function()
    local count = RTXMaterialPipeline.DiscoverAllMaterials()
    InfoPrint("Discovered and queued ", count, " materials")
end, nil, "Discover all materials and add to pipeline queue")

concommand.Add("rtx_mat_stats", function()
    local stats = RTXMaterialPipeline.GetStats()
    
    MsgC(Color(100, 200, 255), "\n=== RTX Material Pipeline Statistics ===\n")
    MsgC(Color(255, 255, 255), "Queue Length:          ", Color(255, 255, 100), stats.queueLength, "\n")
    MsgC(Color(255, 255, 255), "Total Queued:          ", Color(200, 200, 200), stats.totalQueued, "\n")
    MsgC(Color(255, 255, 255), "Total Processed:       ", Color(100, 255, 100), stats.totalProcessed, "\n")
    MsgC(Color(255, 255, 255), "Hash Collisions Found: ", Color(255, 200, 100), stats.hashCollisionsDetected, "\n")
    MsgC(Color(255, 255, 255), "Hash Collisions Fixed: ", Color(100, 255, 100), stats.hashCollisionsFixed, "\n")
    MsgC(Color(255, 255, 255), "Categorized:           ", Color(200, 200, 200), stats.categorized, "\n")
    MsgC(Color(255, 255, 255), "Queued for ToPBR:      ", Color(200, 200, 200), stats.queuedForToPBR, "\n")
    MsgC(Color(255, 255, 255), "Unique Textures:       ", Color(200, 200, 200), stats.uniqueTexturesCreated, "\n")
    MsgC(Color(255, 255, 255), "Errors:                ", stats.errors > 0 and Color(255, 100, 100) or Color(100, 255, 100), stats.errors, "\n")
    MsgC(Color(255, 255, 255), "Currently Processing:  ", stats.isProcessing and Color(100, 255, 100) or Color(200, 200, 200), stats.isProcessing and "Yes" or "No", "\n")
    MsgC(Color(100, 200, 255), "=========================================\n\n")
end, nil, "Show material pipeline statistics")

concommand.Add("rtx_mat_reset", function()
    RTXMaterialPipeline.Reset()
    InfoPrint("Pipeline reset")
end, nil, "Reset the material pipeline state")

concommand.Add("rtx_mat_check", function(ply, cmd, args)
    if #args < 1 then
        WarnPrint("Usage: rtx_mat_check <material_name>")
        return
    end
    
    local matName = args[1]
    local isProcessed = RTXMaterialPipeline.IsMaterialProcessed(matName)
    local isQueued = State.queueSet[matName] == true
    
    MsgC(Color(100, 200, 255), "\n=== Material Status: ", matName, " ===\n")
    MsgC(Color(255, 255, 255), "Processed:  ", isProcessed and Color(100, 255, 100) or Color(200, 200, 200), isProcessed and "Yes" or "No", "\n")
    MsgC(Color(255, 255, 255), "In Queue:   ", isQueued and Color(255, 200, 100) or Color(200, 200, 200), isQueued and "Yes" or "No", "\n")
    
    -- Check with C++ if available
    if HashCollisionFixer then
        if HashCollisionFixer.IsMaterialFixed then
            local cppFixed = HashCollisionFixer.IsMaterialFixed(matName)
            MsgC(Color(255, 255, 255), "C++ Fixed:  ", cppFixed and Color(100, 255, 100) or Color(200, 200, 200), cppFixed and "Yes" or "No", "\n")
        end
        if HashCollisionFixer.IsSolidColorMaterial then
            local isSolid = HashCollisionFixer.IsSolidColorMaterial(matName)
            MsgC(Color(255, 255, 255), "Solid Color: ", isSolid and Color(255, 200, 100) or Color(200, 200, 200), isSolid and "Yes" or "No", "\n")
        end
    end
    
    MsgC(Color(100, 200, 255), "====================================\n\n")
end, nil, "Check the processing status of a specific material")

-- =============================================================================
-- STARTUP MESSAGE
-- =============================================================================

MsgC(Color(100, 255, 150), "\n========================================\n")
MsgC(Color(100, 255, 150), " RTX Material Pipeline v2.0 Loaded\n")
MsgC(Color(200, 200, 200), " Unified material processing system\n")
MsgC(Color(200, 200, 200), " Stages: HashFix → Categorize → ToPBR\n")
MsgC(Color(100, 255, 150), "========================================\n\n")
