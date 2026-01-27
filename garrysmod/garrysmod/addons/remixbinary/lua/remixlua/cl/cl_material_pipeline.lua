--[[
================================================================================
    RTX Material Processing Pipeline - Lua Wrapper
    
    THIN WRAPPER for the C++ MaterialPipeline implementation.
    
    The actual pipeline processing happens in C++ (material_pipeline.cpp).
    This Lua file just:
    1. Calls MaterialPipeline.ProcessPendingMaterials() every frame to process
       materials that were detected by D3D9 hooks
    2. Optionally runs Lua-only pre-processors before C++ pipeline
    
    C++ PIPELINE STAGES (in material_pipeline.cpp):
    ===============================================
    1. ShaderFixes        → Handle Refract shaders, proxies
    2. HashCollisionFixer → Detect solid-color textures  
    3. AutoCategorisation → Classify particles, decals, emissive
    4. ToPBR              → VTF→DDS, PBR extraction, USDA output
    
    FLOW:
    =====
    D3D9 Hook detects texture
      └── Pipeline::OnNewMaterialDetected() adds to pending queue (C++)
    
    Every frame (Think hook):
      └── MaterialPipeline.ProcessPendingMaterials() (C++)
          └── For each pending material:
              └── Pipeline::ProcessMaterial() runs all 4 C++ stages
    
    AUTHORS: RTX Remix GMod Team
    VERSION: 3.0 - C++ Pipeline Wrapper
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
    lastProcessTime = 0
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
-- MAIN THINK HOOK - Process pending materials from C++ queue
-- =============================================================================
-- This is the ONLY processing loop. It calls the C++ MaterialPipeline which
-- handles all 4 stages (ShaderFixes → HashCollision → AutoCat → ToPBR).

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

-- Process a single material through the C++ pipeline
function RTXMaterialPipeline.ProcessMaterial(materialName)
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
    print("=====================================\n")
end, nil, "Show material pipeline status")

concommand.Add("rtx_mat_process_all", function()
    if not RTXMaterialPipeline.IsAvailable() then
        print("[RTX Pipeline] C++ MaterialPipeline not available")
        return
    end
    
    print("[RTX Pipeline] Processing all tracked materials...")
    local count = RTXMaterialPipeline.ProcessAllMaterials()
    print("[RTX Pipeline] Processed " .. count .. " materials")
end, nil, "Process all tracked materials through the pipeline")

concommand.Add("rtx_mat_clear", function()
    RTXMaterialPipeline.ClearCache()
    print("[RTX Pipeline] Cache cleared")
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
    timer.Simple(0.5, Initialize)
end)

-- =============================================================================
-- STARTUP MESSAGE
-- =============================================================================

print("\n========================================")
print(" RTX Material Pipeline v3.0 Loaded")
print(" C++ Pipeline Wrapper")
print(" Processing: C++ handles all stages")
print("========================================\n")

-- Try to initialize immediately if already in game
if LocalPlayer and IsValid(LocalPlayer()) then
    timer.Simple(0.1, Initialize)
end
