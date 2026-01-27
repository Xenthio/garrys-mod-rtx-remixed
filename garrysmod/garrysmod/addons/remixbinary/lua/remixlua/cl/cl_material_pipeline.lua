--[[
    RTX Material Processing Pipeline
    
    Centralized material processing that ensures operations happen in the correct order:
    1. Hash Collision Fix (detect and fix solid-color textures)
    2. Autocategorisation (apply Remix categories)
    3. Queue for ToPBR (convert textures to PBR format)
    
    This pipeline is triggered when new materials are discovered.
]]--

RTXMaterialPipeline = RTXMaterialPipeline or {}

-- Pipeline state
local pipelineState = {
    initialized = false,
    processing = false,
    currentStage = nil,
    
    -- Material queues for each stage
    pendingMaterials = {},      -- Materials waiting to enter pipeline
    stage1Queue = {},           -- Hash collision fix queue
    stage2Queue = {},           -- Autocategorization queue  
    stage3Queue = {},           -- ToPBR queue
    
    -- Processing tracking
    processedMaterials = {},    -- Already fully processed
    
    -- Batch processing
    batchSize = 5,
    currentBatchIndex = 0,
    
    -- Stats
    stats = {
        totalProcessed = 0,
        hashCollisionsFixed = 0,
        categorized = 0,
        convertedToPBR = 0
    }
}

-- ConVars
CreateClientConVar("rtx_material_pipeline", "1", true, false, "Enable/disable the material processing pipeline")
CreateClientConVar("rtx_material_pipeline_debug", "0", true, false, "Debug output for material pipeline")
CreateClientConVar("rtx_material_pipeline_batch", "5", true, false, "Materials processed per tick")

local function DebugPrint(...)
    if GetConVar("rtx_material_pipeline_debug"):GetBool() then
        print("[RTX Pipeline]", ...)
    end
end

--[[
    Stage 1: Hash Collision Fix
    
    Detects solid-color textures and fixes them to prevent hash collisions.
    Uses HashCollisionFixer C++ module to read VTF files directly.
]]--
local function ProcessStage1_HashCollisionFix(materialName)
    if not HashCollisionFixer then
        DebugPrint("HashCollisionFixer not available, skipping stage 1 for:", materialName)
        return true -- Continue to next stage
    end
    
    local mat = Material(materialName)
    if not mat or mat:IsError() then
        DebugPrint("Invalid material in stage 1:", materialName)
        return true -- Skip invalid materials
    end
    
    -- Get the basetexture path
    local baseTex = mat:GetTexture("$basetexture")
    if not baseTex then
        DebugPrint("No basetexture for:", materialName)
        return true -- No texture to check
    end
    
    local texturePath = baseTex:GetName()
    if not texturePath or texturePath == "" then
        return true
    end
    
    -- Check if it's a solid color texture
    local isSolid, r, g, b, a = false, 0, 0, 0, 255
    if HashCollisionFixer.CheckSolidColor then
        isSolid, r, g, b, a = HashCollisionFixer.CheckSolidColor(texturePath)
    end
    
    if isSolid then
        DebugPrint("Solid color detected:", materialName, "Color:", r, g, b, a)
        
        -- Create unique replacement texture
        local uniqueName = "rtx_solid_" .. util.CRC(materialName)
        local uniqueMat = CreateMaterial(uniqueName, "UnlitGeneric", {
            ["$basetexture"] = "color/white",
            ["$color"] = string.format("{%d %d %d}", r, g, b),
            ["$alpha"] = tostring(a / 255)
        })
        
        if uniqueMat and not uniqueMat:IsError() then
            mat:SetTexture("$basetexture", uniqueMat:GetTexture("$basetexture"))
            pipelineState.stats.hashCollisionsFixed = pipelineState.stats.hashCollisionsFixed + 1
            
            -- Mark as fixed in C++
            if HashCollisionFixer.MarkMaterialFixed then
                HashCollisionFixer.MarkMaterialFixed(materialName)
            end
            
            DebugPrint("Fixed hash collision for:", materialName)
        end
    end
    
    return true -- Always continue to next stage
end

--[[
    Stage 2: Autocategorisation
    
    Applies Remix category flags based on material properties.
]]--
local function ProcessStage2_Autocategorize(materialName)
    if not RemixCategoryManager then
        DebugPrint("RemixCategoryManager not available, skipping stage 2 for:", materialName)
        return true
    end
    
    local mat = Material(materialName)
    if not mat or mat:IsError() then
        return true
    end
    
    -- Check various material types and apply categories
    local categorized = false
    
    -- Check for decals
    if RemixCategoryManager.IsMaterialDecal and RemixCategoryManager.IsMaterialDecal(materialName) then
        if RemixCategoryManager.SetMaterialCategory then
            RemixCategoryManager.SetMaterialCategory(materialName, 0x1000) -- DECAL_STATIC
            categorized = true
            DebugPrint("Categorized as decal:", materialName)
        end
    end
    
    -- Check for particles
    if RemixCategoryManager.IsMaterialParticle and RemixCategoryManager.IsMaterialParticle(materialName) then
        if RemixCategoryManager.SetMaterialCategory then
            RemixCategoryManager.SetMaterialCategory(materialName, 0x400) -- PARTICLE
            categorized = true
            DebugPrint("Categorized as particle:", materialName)
        end
    end
    
    -- Check for emissive
    if RemixCategoryManager.IsMaterialEmissive and RemixCategoryManager.IsMaterialEmissive(materialName) then
        if RemixCategoryManager.SetMaterialCategory then
            RemixCategoryManager.SetMaterialCategory(materialName, 0x1000000) -- LEGACY_EMISSIVE
            categorized = true
            DebugPrint("Categorized as emissive:", materialName)
        end
    end
    
    if categorized then
        pipelineState.stats.categorized = pipelineState.stats.categorized + 1
    end
    
    return true
end

--[[
    Stage 3: Queue for ToPBR
    
    Queues the material for PBR conversion.
]]--
local function ProcessStage3_QueueToPBR(materialName)
    if not RTXToPBR then
        DebugPrint("RTXToPBR not available, skipping stage 3 for:", materialName)
        return true
    end
    
    -- Queue for ToPBR processing
    if RTXToPBR.QueueMaterial then
        RTXToPBR.QueueMaterial(materialName)
        pipelineState.stats.convertedToPBR = pipelineState.stats.convertedToPBR + 1
        DebugPrint("Queued for ToPBR:", materialName)
    end
    
    return true
end

--[[
    Process a single material through all pipeline stages
]]--
local function ProcessMaterialThroughPipeline(materialName)
    if pipelineState.processedMaterials[materialName] then
        return -- Already processed
    end
    
    DebugPrint("Processing material:", materialName)
    
    -- Stage 1: Hash Collision Fix
    if not ProcessStage1_HashCollisionFix(materialName) then
        DebugPrint("Stage 1 failed for:", materialName)
        return
    end
    
    -- Stage 2: Autocategorisation
    if not ProcessStage2_Autocategorize(materialName) then
        DebugPrint("Stage 2 failed for:", materialName)
        return
    end
    
    -- Stage 3: Queue for ToPBR
    if not ProcessStage3_QueueToPBR(materialName) then
        DebugPrint("Stage 3 failed for:", materialName)
        return
    end
    
    -- Mark as fully processed
    pipelineState.processedMaterials[materialName] = true
    pipelineState.stats.totalProcessed = pipelineState.stats.totalProcessed + 1
end

--[[
    Add a material to the pipeline for processing
]]--
function RTXMaterialPipeline.QueueMaterial(materialName)
    if not GetConVar("rtx_material_pipeline"):GetBool() then
        return
    end
    
    if pipelineState.processedMaterials[materialName] then
        return -- Already processed
    end
    
    if pipelineState.pendingMaterials[materialName] then
        return -- Already queued
    end
    
    pipelineState.pendingMaterials[materialName] = true
    table.insert(pipelineState.stage1Queue, materialName)
    
    DebugPrint("Queued material:", materialName)
end

--[[
    Add multiple materials to the pipeline
]]--
function RTXMaterialPipeline.QueueMaterials(materials)
    for _, matName in ipairs(materials) do
        RTXMaterialPipeline.QueueMaterial(matName)
    end
end

--[[
    Check if a material has been processed
]]--
function RTXMaterialPipeline.IsMaterialProcessed(materialName)
    return pipelineState.processedMaterials[materialName] == true
end

--[[
    Get pipeline statistics
]]--
function RTXMaterialPipeline.GetStats()
    return {
        pending = table.Count(pipelineState.pendingMaterials),
        processed = pipelineState.stats.totalProcessed,
        hashCollisionsFixed = pipelineState.stats.hashCollisionsFixed,
        categorized = pipelineState.stats.categorized,
        convertedToPBR = pipelineState.stats.convertedToPBR,
        isProcessing = pipelineState.processing
    }
end

--[[
    Process pending materials (called each tick)
]]--
local function ProcessPendingMaterials()
    if not GetConVar("rtx_material_pipeline"):GetBool() then
        return
    end
    
    if #pipelineState.stage1Queue == 0 then
        return
    end
    
    pipelineState.processing = true
    local batchSize = GetConVar("rtx_material_pipeline_batch"):GetInt()
    local processed = 0
    
    while processed < batchSize and #pipelineState.stage1Queue > 0 do
        local materialName = table.remove(pipelineState.stage1Queue, 1)
        pipelineState.pendingMaterials[materialName] = nil
        
        ProcessMaterialThroughPipeline(materialName)
        processed = processed + 1
    end
    
    if #pipelineState.stage1Queue == 0 then
        pipelineState.processing = false
        DebugPrint("Pipeline finished processing batch")
    end
end

--[[
    Discover and queue all materials from world and entities
]]--
function RTXMaterialPipeline.DiscoverAllMaterials()
    local discovered = 0
    
    -- Get materials from all entities
    for _, ent in ipairs(ents.GetAll()) do
        if IsValid(ent) then
            local materials = ent:GetMaterials()
            for _, matName in ipairs(materials) do
                if matName and matName ~= "" then
                    RTXMaterialPipeline.QueueMaterial(matName)
                    discovered = discovered + 1
                end
            end
        end
    end
    
    -- Get world materials
    local worldEnt = game.GetWorld()
    if IsValid(worldEnt) then
        local materials = worldEnt:GetMaterials()
        for _, matName in ipairs(materials) do
            if matName and matName ~= "" then
                RTXMaterialPipeline.QueueMaterial(matName)
                discovered = discovered + 1
            end
        end
    end
    
    DebugPrint("Discovered", discovered, "materials")
    return discovered
end

--[[
    Process all materials immediately (blocking)
]]--
function RTXMaterialPipeline.ProcessAllNow()
    RTXMaterialPipeline.DiscoverAllMaterials()
    
    while #pipelineState.stage1Queue > 0 do
        local materialName = table.remove(pipelineState.stage1Queue, 1)
        pipelineState.pendingMaterials[materialName] = nil
        ProcessMaterialThroughPipeline(materialName)
    end
    
    print("[RTX Pipeline] Processed all materials. Stats:", 
        "Total:", pipelineState.stats.totalProcessed,
        "Hash fixes:", pipelineState.stats.hashCollisionsFixed,
        "Categorized:", pipelineState.stats.categorized)
end

--[[
    Reset the pipeline state
]]--
function RTXMaterialPipeline.Reset()
    pipelineState.pendingMaterials = {}
    pipelineState.stage1Queue = {}
    pipelineState.stage2Queue = {}
    pipelineState.stage3Queue = {}
    pipelineState.processedMaterials = {}
    pipelineState.processing = false
    pipelineState.stats = {
        totalProcessed = 0,
        hashCollisionsFixed = 0,
        categorized = 0,
        convertedToPBR = 0
    }
    DebugPrint("Pipeline reset")
end

-- Hook into Think for batch processing
hook.Add("Think", "RTXMaterialPipeline_Process", ProcessPendingMaterials)

-- Hook into InitPostEntity to start discovery
hook.Add("InitPostEntity", "RTXMaterialPipeline_Init", function()
    if not GetConVar("rtx_material_pipeline"):GetBool() then
        return
    end
    
    -- Delay to allow other systems to initialize
    timer.Simple(3, function()
        print("[RTX Pipeline] Starting material discovery...")
        RTXMaterialPipeline.DiscoverAllMaterials()
    end)
end)

-- Reset on map cleanup
hook.Add("ShutDown", "RTXMaterialPipeline_Cleanup", function()
    RTXMaterialPipeline.Reset()
end)

-- Console commands
concommand.Add("rtx_pipeline_process", function()
    print("[RTX Pipeline] Processing all materials now...")
    RTXMaterialPipeline.ProcessAllNow()
end, nil, "Process all materials through the RTX pipeline immediately")

concommand.Add("rtx_pipeline_discover", function()
    local count = RTXMaterialPipeline.DiscoverAllMaterials()
    print("[RTX Pipeline] Discovered and queued", count, "materials")
end, nil, "Discover all materials and add to pipeline queue")

concommand.Add("rtx_pipeline_stats", function()
    local stats = RTXMaterialPipeline.GetStats()
    print("[RTX Pipeline] Statistics:")
    print("  Pending:", stats.pending)
    print("  Total Processed:", stats.processed)
    print("  Hash Collisions Fixed:", stats.hashCollisionsFixed)
    print("  Categorized:", stats.categorized)
    print("  Queued for ToPBR:", stats.convertedToPBR)
    print("  Currently Processing:", stats.isProcessing and "Yes" or "No")
end, nil, "Show material pipeline statistics")

concommand.Add("rtx_pipeline_reset", function()
    RTXMaterialPipeline.Reset()
    print("[RTX Pipeline] Pipeline reset")
end, nil, "Reset the material pipeline state")

-- Expose hook for other systems to know when a material is processed
-- hook.Run("RTX_MaterialProcessed", materialName, stats)

print("[RTX Pipeline] Material processing pipeline loaded")
