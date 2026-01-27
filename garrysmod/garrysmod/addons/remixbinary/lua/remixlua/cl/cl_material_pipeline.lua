--[[
================================================================================
    RTX Material Processing Pipeline - Lua/C++ Hybrid
    
    This pipeline processes materials through two phases:
    
    PHASE 1 - LUA FIXERS (run on InitPostEntity and new entities):
    ==============================================================
    These modify Source Engine IMaterial properties BEFORE D3D9 sees them:
    - RTXFixPBR.ProcessMaterial()    → Fix ExoPBR/GPBR materials
    - RTXFixRefract.ProcessMaterial() → Fix Refract shader materials
    - RTXRemoveDetail.ProcessMaterial() → Remove detail textures
    
    PHASE 2 - C++ PIPELINE (runs every frame via Think hook):
    =========================================================
    Processes textures AFTER D3D9 captures them:
    1. ShaderFixes        → Handle Refract shaders, proxies
    2. HashCollisionFixer → Detect solid-color textures  
    3. AutoCategorisation → Classify particles, decals, emissive
    4. ToPBR              → VTF→DDS, PBR extraction, USDA output
    
    FLOW:
    =====
    InitPostEntity / Entity spawned:
      └── Lua discovers materials from entities/world
          └── Lua fixers run (RTXFixPBR, RTXFixRefract, RTXRemoveDetail)
    
    D3D9 Hook detects texture:
      └── Pipeline::OnNewMaterialDetected() adds to pending queue (C++)
    
    Every frame (Think hook):
      └── MaterialPipeline.ProcessPendingMaterials() (C++)
          └── Runs C++ stages asynchronously
    
    AUTHORS: RTX Remix GMod Team
    VERSION: 4.0 - Lua/C++ Hybrid Pipeline
================================================================================
]]--

if not CLIENT then return end
if not (BRANCH == "x86-64" or BRANCH == "chromium") then return end

-- =============================================================================
-- GLOBAL PIPELINE MODULE
-- =============================================================================

RTXMaterialPipeline = RTXMaterialPipeline or {}

-- =============================================================================
-- CONVARS
-- =============================================================================

CreateClientConVar("rtx_mat_enabled", "1", true, false, 
    "Enable/disable the material processing pipeline")

CreateClientConVar("rtx_mat_debug", "0", true, false, 
    "Enable debug output for material pipeline")

-- =============================================================================
-- INTERNAL STATE
-- =============================================================================

local State = {
    initialized = false,
    thinkHookActive = false,
    lastProcessTime = 0,
    luaProcessedMaterials = {},  -- Track which materials Lua fixers have processed
    luaFixersRun = false
}

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

-- =============================================================================
-- LUA FIXERS - Run on materials BEFORE D3D9 sees them
-- =============================================================================

-- Process a single material through all Lua fixers
local function RunLuaFixers(matName)
    if not matName or matName == "" then return end
    if State.luaProcessedMaterials[matName] then return end
    
    State.luaProcessedMaterials[matName] = true
    
    local debug = GetConVar("rtx_mat_debug"):GetBool()
    
    -- Stage 0a: PBR Material Fixer (ExoPBR/GPBR)
    if RTXFixPBR and RTXFixPBR.IsEnabled and RTXFixPBR.IsEnabled() then
        local fixed = RTXFixPBR.ProcessMaterial(matName)
        if fixed and debug then
            DebugPrint("Lua Stage 0a FIXED (PBR): ", matName)
        end
    end
    
    -- Stage 0b: Refract Material Fixer
    if RTXFixRefract and RTXFixRefract.IsEnabled and RTXFixRefract.IsEnabled() then
        local fixed = RTXFixRefract.ProcessMaterial(matName)
        if fixed and debug then
            DebugPrint("Lua Stage 0b FIXED (Refract): ", matName)
        end
    end
    
    -- Stage 0c: Detail Texture Remover
    if RTXRemoveDetail and RTXRemoveDetail.IsEnabled and RTXRemoveDetail.IsEnabled() then
        local fixed = RTXRemoveDetail.ProcessMaterial(matName)
        if fixed and debug then
            DebugPrint("Lua Stage 0c FIXED (Detail): ", matName)
        end
    end
end

-- Get all materials from an entity
local function GetEntityMaterials(ent)
    if not IsValid(ent) then return {} end
    
    local materials = {}
    
    -- Get materials from the entity's model
    local modelMats = ent:GetMaterials()
    if modelMats then
        for _, matName in ipairs(modelMats) do
            if matName and matName ~= "" then
                materials[matName] = true
            end
        end
    end
    
    -- Get sub-materials
    for i = 0, 31 do
        local subMat = ent:GetSubMaterial(i)
        if subMat and subMat ~= "" then
            materials[subMat] = true
        end
    end
    
    return materials
end

-- Process all materials from an entity through Lua fixers
local function ProcessEntityMaterials(ent)
    local materials = GetEntityMaterials(ent)
    for matName, _ in pairs(materials) do
        RunLuaFixers(matName)
    end
end

-- Process all BSP/world materials
local function ProcessWorldMaterials()
    local world = game.GetWorld()
    if not IsValid(world) then return end
    
    local worldMats = world:GetMaterials()
    if worldMats then
        for _, matName in ipairs(worldMats) do
            RunLuaFixers(matName)
        end
    end
    
    -- Also process NikNaks BSP textures if available
    if NikNaks and NikNaks.CurrentMap then
        local bsp = NikNaks.CurrentMap
        if bsp and bsp.GetAllTextureNames then
            local textures = bsp:GetAllTextureNames()
            if textures then
                for _, texName in ipairs(textures) do
                    RunLuaFixers(texName)
                end
            end
        end
    end
end

-- Process all existing entities
local function ProcessAllEntities()
    for _, ent in ipairs(ents.GetAll()) do
        ProcessEntityMaterials(ent)
    end
end

-- Run all Lua fixers on existing materials
function RTXMaterialPipeline.RunLuaFixers()
    InfoPrint("Running Lua fixers on all materials...")
    
    local startTime = SysTime()
    
    -- Process world/BSP materials first
    ProcessWorldMaterials()
    
    -- Process all entities
    ProcessAllEntities()
    
    local elapsed = SysTime() - startTime
    local count = table.Count(State.luaProcessedMaterials)
    
    InfoPrint(string.format("Lua fixers processed %d materials in %.2fms", count, elapsed * 1000))
    State.luaFixersRun = true
end

-- =============================================================================
-- MAIN THINK HOOK - Process pending materials from C++ queue
-- =============================================================================

local function OnThink()
    -- Check if pipeline is enabled
    if not GetConVar("rtx_mat_enabled"):GetBool() then
        return
    end
    
    -- Check if C++ MaterialPipeline is available
    if not MaterialPipeline or not MaterialPipeline.ProcessPendingMaterials then
        return
    end
    
    -- Process any materials that were detected by D3D9 hooks
    -- This calls Pipeline::ProcessPendingMaterials() in C++ which:
    -- 1. Takes materials from the pending queue
    -- 2. Runs them through all 4 C++ pipeline stages
    -- 3. Returns count of processed materials
    local processed = MaterialPipeline.ProcessPendingMaterials()
    
    if processed > 0 then
        DebugPrint("Processed ", processed, " materials through C++ pipeline")
    end
end

-- =============================================================================
-- PUBLIC API
-- =============================================================================

-- Get stats from C++ pipeline
function RTXMaterialPipeline.GetStats()
    if MaterialPipeline and MaterialPipeline.GetStats then
        return MaterialPipeline.GetStats()
    end
    return {}
end

-- Process a single material through both Lua fixers and C++ pipeline
function RTXMaterialPipeline.ProcessMaterial(materialName)
    -- First run Lua fixers
    RunLuaFixers(materialName)
    
    -- Then process through C++ pipeline
    if MaterialPipeline and MaterialPipeline.ProcessMaterial then
        return MaterialPipeline.ProcessMaterial(materialName)
    end
    return false
end

-- Process all tracked materials through the C++ pipeline
function RTXMaterialPipeline.ProcessAllMaterials()
    if MaterialPipeline and MaterialPipeline.ProcessAllMaterials then
        return MaterialPipeline.ProcessAllMaterials()
    end
    return 0
end

-- Clear the cache
function RTXMaterialPipeline.ClearCache()
    State.luaProcessedMaterials = {}
    
    -- Clear Lua fixer caches
    if RTXFixPBR and RTXFixPBR.ClearCache then RTXFixPBR.ClearCache() end
    if RTXFixRefract and RTXFixRefract.ClearCache then RTXFixRefract.ClearCache() end
    if RTXRemoveDetail and RTXRemoveDetail.ClearCache then RTXRemoveDetail.ClearCache() end
    
    -- Clear C++ cache
    if MaterialPipeline and MaterialPipeline.ClearCache then
        MaterialPipeline.ClearCache()
    end
end

-- Set debug output
function RTXMaterialPipeline.SetDebug(enabled)
    if MaterialPipeline and MaterialPipeline.SetDebugOutput then
        MaterialPipeline.SetDebugOutput(enabled)
    end
end

-- Check if C++ pipeline is available
function RTXMaterialPipeline.IsAvailable()
    return MaterialPipeline ~= nil and MaterialPipeline.ProcessPendingMaterials ~= nil
end

-- Get Lua fixer stats
function RTXMaterialPipeline.GetLuaStats()
    local stats = {
        luaProcessed = table.Count(State.luaProcessedMaterials),
        pbrFixed = 0,
        refractFixed = 0,
        detailRemoved = 0
    }
    
    if RTXFixPBR and RTXFixPBR.GetStats then
        local s = RTXFixPBR.GetStats()
        stats.pbrFixed = s.fixed or 0
    end
    
    if RTXFixRefract and RTXFixRefract.GetStats then
        local s = RTXFixRefract.GetStats()
        stats.refractFixed = s.fixed or 0
    end
    
    if RTXRemoveDetail and RTXRemoveDetail.GetStats then
        local s = RTXRemoveDetail.GetStats()
        stats.detailRemoved = s.removed or 0
    end
    
    return stats
end

-- =============================================================================
-- INITIALIZATION
-- =============================================================================

local function Initialize()
    if State.initialized then return end
    
    -- Add Think hook to process pending materials every frame
    hook.Add("Think", "RTXMaterialPipeline_ProcessQueue", OnThink)
    State.thinkHookActive = true
    State.initialized = true
    
    -- Check if C++ pipeline is available
    if RTXMaterialPipeline.IsAvailable() then
        InfoPrint("C++ MaterialPipeline connected - processing enabled")
        
        -- Sync debug setting
        if GetConVar("rtx_mat_debug"):GetBool() then
            RTXMaterialPipeline.SetDebug(true)
        end
    else
        InfoPrint("Waiting for C++ MaterialPipeline binary module...")
    end
    
    -- Run Lua fixers on existing materials (deferred to not block)
    timer.Simple(0.1, function()
        RTXMaterialPipeline.RunLuaFixers()
    end)
end

-- =============================================================================
-- CONSOLE COMMANDS
-- =============================================================================

concommand.Add("rtx_mat_status", function()
    local available = RTXMaterialPipeline.IsAvailable()
    print("\n=== RTX Material Pipeline Status ===")
    print("C++ Pipeline Available: " .. (available and "YES" or "NO"))
    
    if available then
        local stats = RTXMaterialPipeline.GetStats()
        print("Materials Tracked: " .. (stats.materialsTracked or 0))
        print("Materials Processed: " .. (stats.materialsProcessed or 0))
        print("Materials Queued: " .. (stats.materialsQueued or 0))
        print("Textures Converted: " .. (stats.texturesConverted or 0))
        print("Failed Conversions: " .. (stats.failedConversions or 0))
    end
    
    local luaStats = RTXMaterialPipeline.GetLuaStats()
    print("\n--- Lua Fixers ---")
    print("Lua Processed: " .. luaStats.luaProcessed)
    print("PBR Fixed: " .. luaStats.pbrFixed)
    print("Refract Fixed: " .. luaStats.refractFixed)
    print("Detail Removed: " .. luaStats.detailRemoved)
    print("=====================================\n")
end, nil, "Show material pipeline status")

concommand.Add("rtx_mat_process_all", function()
    -- First run Lua fixers
    RTXMaterialPipeline.RunLuaFixers()
    
    -- Then process through C++ pipeline
    if not RTXMaterialPipeline.IsAvailable() then
        print("[RTX Pipeline] C++ MaterialPipeline not available")
        return
    end
    
    print("[RTX Pipeline] Processing all tracked materials through C++...")
    local count = RTXMaterialPipeline.ProcessAllMaterials()
    print("[RTX Pipeline] Processed " .. count .. " materials")
end, nil, "Process all tracked materials through the pipeline")

concommand.Add("rtx_mat_run_lua_fixers", function()
    RTXMaterialPipeline.RunLuaFixers()
end, nil, "Run Lua material fixers on all materials")

concommand.Add("rtx_mat_clear", function()
    RTXMaterialPipeline.ClearCache()
    print("[RTX Pipeline] All caches cleared")
end, nil, "Clear the material pipeline cache")

-- =============================================================================
-- HOOKS
-- =============================================================================

-- Initialize when client connects
hook.Add("InitPostEntity", "RTXMaterialPipeline_Init", function()
    -- Small delay to ensure C++ module is loaded
    timer.Simple(0.5, Initialize)
end)

-- Re-initialize on map cleanup
hook.Add("PostCleanupMap", "RTXMaterialPipeline_Reinit", function()
    State.initialized = false
    State.luaProcessedMaterials = {}
    State.luaFixersRun = false
    timer.Simple(0.5, Initialize)
end)

-- Process new entities that spawn
hook.Add("OnEntityCreated", "RTXMaterialPipeline_NewEntity", function(ent)
    if not State.initialized then return end
    if not IsValid(ent) then return end
    
    -- Delay slightly to ensure entity is fully set up
    timer.Simple(0.1, function()
        if IsValid(ent) then
            ProcessEntityMaterials(ent)
        end
    end)
end)

-- =============================================================================
-- STARTUP MESSAGE
-- =============================================================================

print("\n========================================")
print(" RTX Material Pipeline v4.0 Loaded")
print(" Lua/C++ Hybrid Pipeline")
print(" Lua Fixers: PBR, Refract, Detail")
print(" C++ Stages: ShaderFix, Hash, Cat, ToPBR")
print("========================================\n")

-- Try to initialize immediately if already in game
if LocalPlayer and IsValid(LocalPlayer()) then
    timer.Simple(0.1, Initialize)
end
