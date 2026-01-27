// =========================================================================
// material_pipeline.cpp - Unified Material Processing Pipeline Implementation
// =========================================================================
// This implementation wraps the existing components (D3D9TextureTracker,
// ToPBR) into a unified pipeline interface.
//
// The design philosophy:
// - Pipeline is the ONLY public interface for material processing
// - All configuration flows through Pipeline
// - All processing flows through Pipeline
// - Internal components can be refactored without breaking the API
// =========================================================================

#ifdef _WIN64

#include "material_pipeline.h"
#include "../d3d9_texture_tracker.h"
#include "to_pbr/to_pbr.h"
#include "hash_collision_fixer/hash_collision_fixer.h"
#include "auto_categorisation/auto_categorisation.h"
#include "shader_fixes/shader_fixes.h"

#include <tier0/dbg.h>
#include <filesystem.h>
#include <d3d9.h>
#include <Windows.h>
#include <remix/remix.h>
#include <materialsystem/imaterialsystem.h>
#include <materialsystem/imaterial.h>
#include <materialsystem/imaterialvar.h>

namespace MaterialPipeline {

// =========================================================================
// Static Filesystem Interface
// =========================================================================
static IFileSystem* s_pFileSystem = nullptr;

static IFileSystem* GetFileSystemInterface() {
    if (s_pFileSystem) return s_pFileSystem;
    
    HMODULE fsModule = GetModuleHandle("filesystem_stdio.dll");
    if (!fsModule) {
        fsModule = GetModuleHandle("filesystem.dll");
    }
    
    if (fsModule) {
        typedef void* (*CreateInterfaceFn)(const char* name, int* returnCode);
        CreateInterfaceFn createInterface = (CreateInterfaceFn)GetProcAddress(fsModule, "CreateInterface");
        if (createInterface) {
            s_pFileSystem = (IFileSystem*)createInterface("VFileSystem022", nullptr);
            if (!s_pFileSystem) {
                s_pFileSystem = (IFileSystem*)createInterface("VFileSystem021", nullptr);
            }
        }
    }
    
    return s_pFileSystem;
}

// =========================================================================
// Pipeline Implementation
// =========================================================================

Pipeline& Pipeline::Instance() {
    static Pipeline instance;
    return instance;
}

Pipeline::Pipeline() = default;
Pipeline::~Pipeline() {
    ShutdownInternal();
}

// =========================================================================
// Static convenience functions
// =========================================================================

// Static Initialize - main entry point from RemixAPI
bool Pipeline::Initialize(remix::Interface* remix, GarrysMod::Lua::ILuaBase* LUA) {
    // Get D3D device from global (set up by module.cpp)
    extern IDirect3DDevice9Ex* g_d3dDevice;
    
    if (!Instance().InitializeInternal(g_d3dDevice, remix)) {
        return false;
    }
    
    // Register all Lua bindings for pipeline components
    if (LUA) {
        // Register HashCollisionFixer Lua bindings
        HashCollisionFixer::RegisterLuaBindings(LUA);
        
        // Register ToPBR (LegacyTextureProcessor) Lua bindings
        ToPBR::InitializeLegacyTextureProcessorLuaBindings(LUA);
        
        // Register AutoCategorisation Lua bindings
        AutoCategorisation::RegisterLuaBindings(LUA);
        
        // Register ShaderFixes Lua bindings
        ShaderFixes::RegisterLuaBindings(LUA);
    }
    
    return true;
}

// Static Shutdown - convenience wrapper
void Pipeline::Shutdown() {
    Instance().ShutdownInternal();
}

// =========================================================================
// Instance lifecycle methods
// =========================================================================

bool Pipeline::InitializeInternal(IDirect3DDevice9Ex* device, remix::Interface* remix) {
    std::lock_guard<std::mutex> lock(m_mutex);
    
    if (m_initialized) {
        Msg("[MaterialPipeline] Already initialized\n");
        return true;
    }
    
    if (!device) {
        Warning("[MaterialPipeline] Invalid D3D9 device\n");
        return false;
    }
    
    if (!remix) {
        Warning("[MaterialPipeline] Invalid Remix interface\n");
        return false;
    }
    
    m_device = device;
    m_remix = remix;
    m_fileSystem = GetFileSystemInterface();
    
    if (!m_fileSystem) {
        Warning("[MaterialPipeline] Could not get filesystem interface\n");
        return false;
    }
    
    // Initialize D3D9TextureTracker
    if (!D3D9TextureTracker::Instance().Initialize(device)) {
        Warning("[MaterialPipeline] Failed to initialize D3D9TextureTracker\n");
        return false;
    }
    
    // Initialize HashCollisionFixer
    HashCollisionFixer::Initialize();
    
    // Initialize LegacyTextureProcessor
    if (!ToPBR::TextureProcessor::Instance().Initialize(remix)) {
        Warning("[MaterialPipeline] Failed to initialize LegacyTextureProcessor\n");
        D3D9TextureTracker::Instance().Shutdown();
        return false;
    }
    
    // Apply default configuration
    ApplyConfig();
    
    m_initialized = true;
    Msg("[MaterialPipeline] Initialized successfully\n");
    
    return true;
}

void Pipeline::ShutdownInternal() {
    std::lock_guard<std::mutex> lock(m_mutex);
    
    if (!m_initialized) {
        return;
    }
    
    Msg("[MaterialPipeline] Shutting down...\n");
    
    // Shutdown components in reverse order
    ToPBR::TextureProcessor::Instance().Shutdown();
    D3D9TextureTracker::Instance().Shutdown();
    
    m_device = nullptr;
    m_remix = nullptr;
    m_fileSystem = nullptr;
    m_initialized = false;
    
    Msg("[MaterialPipeline] Shutdown complete\n");
}

// =========================================================================
// Configuration
// =========================================================================

void Pipeline::SetConfig(const PipelineConfig& config) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_config = config;
    ApplyConfig();
}

void Pipeline::ApplyConfig() {
    auto& tracker = D3D9TextureTracker::Instance();
    auto& processor = ToPBR::TextureProcessor::Instance();
    
    // Apply to tracker
    tracker.SetAutoCategorization(m_config.particleCategorization || 
                                   m_config.decalCategorization || 
                                   m_config.emissiveCategorization);
    tracker.SetParticleCategorization(m_config.particleCategorization);
    tracker.SetDecalCategorization(m_config.decalCategorization);
    tracker.SetEmissiveCategorization(m_config.emissiveCategorization);
    tracker.SetDebugOutput(m_config.debugOutput);
    
    // Apply to processor
    processor.SetAutoProcessing(m_config.autoProcessing);
    processor.SetDebugOutput(m_config.debugOutput);
    processor.SetMetallicGeneration(m_config.metallicGeneration);
    processor.SetAutoDiscover(m_config.autoDiscoverTextures);
    processor.SetParseCommentedProperties(m_config.parseCommentedProperties);
    
    if (!m_config.outputDirectory.empty()) {
        processor.SetOutputDirectory(m_config.outputDirectory);
    }
}

void Pipeline::SetAutoProcessing(bool enabled) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_config.autoProcessing = enabled;
    ToPBR::TextureProcessor::Instance().SetAutoProcessing(enabled);
}

void Pipeline::SetDebugOutput(bool enabled) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_config.debugOutput = enabled;
    D3D9TextureTracker::Instance().SetDebugOutput(enabled);
    ToPBR::TextureProcessor::Instance().SetDebugOutput(enabled);
}

void Pipeline::SetOutputDirectory(const std::string& path) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_config.outputDirectory = path;
    ToPBR::TextureProcessor::Instance().SetOutputDirectory(path);
}

// =========================================================================
// Processing Control
// =========================================================================

int Pipeline::QueueAllMaterials() {
    if (!m_initialized) {
        Warning("[MaterialPipeline] Not initialized\n");
        return 0;
    }
    return ToPBR::TextureProcessor::Instance().QueueMaterialsForProcessing();
}

bool Pipeline::ProcessMaterial(const std::string& materialName) {
    if (!m_initialized) {
        Warning("[MaterialPipeline] Not initialized\n");
        return false;
    }
    
    // =========================================================================
    // UNIFIED PIPELINE: Process material through all stages in order
    // =========================================================================
    // Stage 1: ShaderFixes   - Handle special shaders (Refract, proxies, etc.)
    // Stage 2: HashCollision - Detect solid-color texture hash collisions
    // Stage 3: AutoCategory  - Classify material (particle, decal, emissive, etc.)
    // Stage 4: ToPBR         - Convert VTF→DDS and generate PBR materials
    // =========================================================================
    
    if (m_config.debugOutput) {
        Msg("[MaterialPipeline] Processing material through pipeline: %s\n", materialName.c_str());
    }
    
    // Get the IMaterial for stages that need it
    IMaterial* material = nullptr;
    extern IMaterialSystem* materials;
    if (materials) {
        material = materials->FindMaterial(materialName.c_str(), TEXTURE_GROUP_OTHER, false);
        if (material && material->IsErrorMaterial()) {
            material = nullptr;
        }
    }
    
    // Get texture pointer for stages that need it
    IDirect3DTexture9* texture = D3D9TextureTracker::Instance().GetTextureForMaterial(materialName.c_str());
    
    // -------------------------------------------------------------------------
    // STAGE 1: ShaderFixes
    // -------------------------------------------------------------------------
    // Handle Refract shaders, material proxies, parameter normalization
    if (ShaderFixes::NeedsFix(materialName, material)) {
        auto fixResult = ShaderFixes::ApplyFix(materialName, material);
        if (fixResult.applied && m_config.debugOutput) {
            Msg("[MaterialPipeline] Stage 1 (ShaderFixes): %s - %s\n", 
                materialName.c_str(), fixResult.description.c_str());
        }
    }
    
    // -------------------------------------------------------------------------
    // STAGE 2: HashCollisionFixer
    // -------------------------------------------------------------------------
    // Detect solid-color textures that would cause hash collisions in Remix
    // Extract $basetexture path from material for solid color checking
    std::string baseTexturePath;
    if (material) {
        bool found = false;
        IMaterialVar* pVar = material->FindVar("$basetexture", &found, false);
        if (found && pVar) {
            const char* texPath = pVar->GetStringValue();
            if (texPath && texPath[0]) {
                baseTexturePath = texPath;
            }
        }
    }
    
    if (!baseTexturePath.empty()) {
        bool needsFix = HashCollisionFixer::CheckMaterial(
            m_fileSystem, materialName, baseTexturePath, m_config.debugOutput);
        if (needsFix && m_config.debugOutput) {
            Msg("[MaterialPipeline] Stage 2 (HashCollisionFixer): %s - solid color detected\n", 
                materialName.c_str());
        }
    }
    
    // -------------------------------------------------------------------------
    // STAGE 3: AutoCategorisation
    // -------------------------------------------------------------------------
    // Classify material as particle, decal, emissive, sky, water, etc.
    if (texture) {
        uint32_t categoryFlags = AutoCategorisation::DetectAndApply(materialName, material, texture);
        if (categoryFlags != 0 && m_config.debugOutput) {
            Msg("[MaterialPipeline] Stage 3 (AutoCategorisation): %s - flags 0x%X\n", 
                materialName.c_str(), categoryFlags);
        }
    }
    
    // -------------------------------------------------------------------------
    // STAGE 4: ToPBR
    // -------------------------------------------------------------------------
    // Convert VTF textures to DDS, extract PBR properties, generate USDA
    bool result = ToPBR::TextureProcessor::Instance().ProcessSingleMaterial(materialName);
    if (m_config.debugOutput) {
        Msg("[MaterialPipeline] Stage 4 (ToPBR): %s - %s\n", 
            materialName.c_str(), result ? "success" : "failed/skipped");
    }
    
    // =========================================================================
    // Pipeline Complete
    // =========================================================================
    
    if (m_onProcessed) {
        m_onProcessed(materialName, result);
    }
    
    return result;
}

int Pipeline::ProcessBatch(int maxCount) {
    if (!m_initialized) {
        return 0;
    }
    return ToPBR::TextureProcessor::Instance().ProcessTrackedMaterialsBatch(maxCount);
}

bool Pipeline::IsProcessing() const {
    if (!m_initialized) return false;
    return ToPBR::TextureProcessor::Instance().IsProcessingInBackground();
}

size_t Pipeline::GetQueueSize() const {
    if (!m_initialized) return 0;
    return ToPBR::TextureProcessor::Instance().GetQueuedMaterialCount();
}

// =========================================================================
// Texture Tracking
// =========================================================================

uint64_t Pipeline::GetTextureHash(const std::string& materialName) const {
    if (!m_initialized) return 0;
    
    auto* texture = D3D9TextureTracker::Instance().GetTextureForMaterial(materialName.c_str());
    if (!texture) return 0;
    
    if (m_remix) {
        auto result = m_remix->dxvk_GetTextureHash(texture);
        if (result) {
            return result.value();
        }
    }
    
    return 0;
}

std::vector<std::string> Pipeline::GetTrackedMaterials() const {
    if (!m_initialized) return {};
    return D3D9TextureTracker::Instance().GetCachedMaterials();
}

void Pipeline::ClearCache() {
    if (!m_initialized) return;
    
    D3D9TextureTracker::Instance().ClearCache();
    ToPBR::TextureProcessor::Instance().ClearCache();
    
    Msg("[MaterialPipeline] Cache cleared\n");
}

// =========================================================================
// World Textures
// =========================================================================

void Pipeline::SetWorldTextures(const std::vector<std::string>& textureNames) {
    if (!m_initialized) return;
    D3D9TextureTracker::Instance().SetWorldTextureNames(textureNames);
}

void Pipeline::ClearWorldTextures() {
    if (!m_initialized) return;
    D3D9TextureTracker::Instance().ClearWorldTextureNames();
}

bool Pipeline::IsWorldTexture(const std::string& materialName) const {
    if (!m_initialized) return false;
    return D3D9TextureTracker::Instance().IsWorldTexture(materialName);
}

// =========================================================================
// Categorization
// =========================================================================

void Pipeline::SetCategoryFlags(uint64_t hash, uint32_t flags) {
    if (!m_initialized) return;
    D3D9TextureTracker::Instance().SetHashCategoryFlags(hash, flags);
}

uint32_t Pipeline::GetCategoryFlags(uint64_t hash) const {
    if (!m_initialized) return 0;
    
    uint32_t flags = 0;
    D3D9TextureTracker::Instance().GetHashCategoryFlags(hash, &flags);
    return flags;
}

int Pipeline::RescanCategories() {
    if (!m_initialized) return 0;
    return D3D9TextureTracker::Instance().RescanAllMaterials();
}

int Pipeline::RetryPendingCategories() {
    if (!m_initialized) return 0;
    return D3D9TextureTracker::Instance().RetryPendingCategories();
}

// =========================================================================
// USDA Output
// =========================================================================

void Pipeline::WriteUSDAIfNeeded() {
    if (!m_initialized) return;
    ToPBR::TextureProcessor::Instance().WriteUSDAIfNeeded();
}

bool Pipeline::WriteUSDA() {
    if (!m_initialized) return false;
    // Force write by marking as needing update, then writing
    ToPBR::TextureProcessor::Instance().WriteUSDAIfNeeded();
    return true;
}

bool Pipeline::AppendToUSDA() {
    if (!m_initialized) return false;
    ToPBR::TextureProcessor::Instance().AppendToUSDAAsync();
    return true;
}

// =========================================================================
// Query
// =========================================================================

PipelineStats Pipeline::GetStats() const {
    PipelineStats stats;
    
    if (!m_initialized) return stats;
    
    // Get tracker stats
    stats.materialsTracked = static_cast<int>(D3D9TextureTracker::Instance().GetCacheSize());
    stats.pendingCategories = static_cast<int>(D3D9TextureTracker::Instance().GetPendingCount());
    
    // Get processor stats
    auto procStats = ToPBR::TextureProcessor::Instance().GetStats();
    stats.materialsProcessed = procStats.materialsProcessed;
    stats.texturesConverted = procStats.texturesUploaded;
    stats.materialsWithNormals = procStats.materialsWithNormals;
    stats.materialsWithRoughness = procStats.materialsWithRoughness;
    stats.failedConversions = procStats.failedConversions;
    
    // Queue stats
    stats.materialsQueued = static_cast<int>(GetQueueSize());
    
    return stats;
}

bool Pipeline::IsMaterialProcessed(const std::string& materialName) const {
    if (!m_initialized) return false;
    return ToPBR::TextureProcessor::Instance().IsMaterialProcessed(materialName);
}

MaterialInfo Pipeline::GetMaterialInfo(const std::string& materialName) const {
    MaterialInfo info;
    info.name = materialName;
    
    if (!m_initialized) return info;
    
    info.textureHash = GetTextureHash(materialName);
    info.processed = IsMaterialProcessed(materialName);
    
    // Additional details would require accessing internal processor state
    // which is encapsulated - this is intentional for clean API
    
    return info;
}

std::vector<std::pair<std::string, uint64_t>> Pipeline::FindTextures(const std::string& searchTerm) const {
    if (!m_initialized) return {};
    return D3D9TextureTracker::Instance().FindTexturesByName(searchTerm);
}

// =========================================================================
// Event Callbacks
// =========================================================================

void Pipeline::SetOnMaterialDetected(MaterialDetectedCallback callback) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_onDetected = callback;
}

void Pipeline::SetOnMaterialProcessed(MaterialProcessedCallback callback) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_onProcessed = callback;
}

void Pipeline::OnMaterialDetected(const std::string& materialName, uint64_t textureHash) {
    // Fire callback
    if (m_onDetected) {
        m_onDetected(materialName, textureHash);
    }
    
    // If auto-processing is enabled, process through the unified pipeline
    // Otherwise, just forward to ToPBR for potential queuing
    if (m_config.autoProcessing) {
        // Process through the unified pipeline (all 4 stages)
        ProcessMaterial(materialName);
    } else {
        // Forward to processor for potential queuing/later processing
        ToPBR::TextureProcessor::Instance().OnNewMaterialDetected(materialName, textureHash);
    }
}

int Pipeline::ProcessAllMaterialsThroughPipeline() {
    if (!m_initialized) {
        Warning("[MaterialPipeline] Not initialized\n");
        return 0;
    }
    
    std::vector<std::string> materials = GetTrackedMaterials();
    int processed = 0;
    
    Msg("[MaterialPipeline] Processing %zu materials through unified pipeline...\n", materials.size());
    
    for (const auto& materialName : materials) {
        if (ProcessMaterial(materialName)) {
            processed++;
        }
    }
    
    Msg("[MaterialPipeline] Processed %d materials through unified pipeline\n", processed);
    return processed;
}

// =========================================================================
// Internal
// =========================================================================

IFileSystem* Pipeline::GetFileSystem() {
    return m_fileSystem;
}

// =========================================================================
// Lua Bindings
// =========================================================================

// Forward declaration for Lua function implementations
static int MaterialPipeline_GetStats(lua_State* L);
static int MaterialPipeline_ProcessMaterial(lua_State* L);
static int MaterialPipeline_ProcessAllMaterials(lua_State* L);
static int MaterialPipeline_ProcessBatch(lua_State* L);
static int MaterialPipeline_QueueAllMaterials(lua_State* L);
static int MaterialPipeline_ClearCache(lua_State* L);
static int MaterialPipeline_SetAutoProcessing(lua_State* L);
static int MaterialPipeline_SetDebugOutput(lua_State* L);
static int MaterialPipeline_GetTrackedMaterials(lua_State* L);
static int MaterialPipeline_IsMaterialProcessed(lua_State* L);
static int MaterialPipeline_GetTextureHash(lua_State* L);
static int MaterialPipeline_RescanCategories(lua_State* L);
static int MaterialPipeline_WriteUSDA(lua_State* L);
static int MaterialPipeline_SetCategoryFlags(lua_State* L);
static int MaterialPipeline_GetCategoryFlags(lua_State* L);
static int MaterialPipeline_FindTextures(lua_State* L);

void Pipeline::RegisterLuaBindings(GarrysMod::Lua::ILuaBase* LUA) {
    if (!LUA) return;
    
    Msg("[MaterialPipeline] Registering Lua bindings...\n");
    
    // Create MaterialPipeline table
    LUA->PushSpecial(GarrysMod::Lua::SPECIAL_GLOB);
    LUA->CreateTable();
    
    // Register functions
    LUA->PushCFunction(MaterialPipeline_GetStats);
    LUA->SetField(-2, "GetStats");
    
    LUA->PushCFunction(MaterialPipeline_ProcessMaterial);
    LUA->SetField(-2, "ProcessMaterial");
    
    LUA->PushCFunction(MaterialPipeline_ProcessAllMaterials);
    LUA->SetField(-2, "ProcessAllMaterials");
    
    LUA->PushCFunction(MaterialPipeline_ProcessBatch);
    LUA->SetField(-2, "ProcessBatch");
    
    LUA->PushCFunction(MaterialPipeline_QueueAllMaterials);
    LUA->SetField(-2, "QueueAllMaterials");
    
    LUA->PushCFunction(MaterialPipeline_ClearCache);
    LUA->SetField(-2, "ClearCache");
    
    LUA->PushCFunction(MaterialPipeline_SetAutoProcessing);
    LUA->SetField(-2, "SetAutoProcessing");
    
    LUA->PushCFunction(MaterialPipeline_SetDebugOutput);
    LUA->SetField(-2, "SetDebugOutput");
    
    LUA->PushCFunction(MaterialPipeline_GetTrackedMaterials);
    LUA->SetField(-2, "GetTrackedMaterials");
    
    LUA->PushCFunction(MaterialPipeline_IsMaterialProcessed);
    LUA->SetField(-2, "IsMaterialProcessed");
    
    LUA->PushCFunction(MaterialPipeline_GetTextureHash);
    LUA->SetField(-2, "GetTextureHash");
    
    LUA->PushCFunction(MaterialPipeline_RescanCategories);
    LUA->SetField(-2, "RescanCategories");
    
    LUA->PushCFunction(MaterialPipeline_WriteUSDA);
    LUA->SetField(-2, "WriteUSDA");
    
    LUA->PushCFunction(MaterialPipeline_SetCategoryFlags);
    LUA->SetField(-2, "SetCategoryFlags");
    
    LUA->PushCFunction(MaterialPipeline_GetCategoryFlags);
    LUA->SetField(-2, "GetCategoryFlags");
    
    LUA->PushCFunction(MaterialPipeline_FindTextures);
    LUA->SetField(-2, "FindTextures");
    
    LUA->SetField(-2, "MaterialPipeline");
    LUA->Pop();
    
    Msg("[MaterialPipeline] Lua bindings registered\n");
}

// =========================================================================
// Lua Function Implementations
// =========================================================================

static int MaterialPipeline_GetStats(lua_State* L) {
    auto* LUA = reinterpret_cast<GarrysMod::Lua::ILuaBase*>(L);
    
    auto stats = Pipeline::Instance().GetStats();
    
    LUA->CreateTable();
    
    LUA->PushNumber(stats.materialsTracked);
    LUA->SetField(-2, "materialsTracked");
    
    LUA->PushNumber(stats.materialsProcessed);
    LUA->SetField(-2, "materialsProcessed");
    
    LUA->PushNumber(stats.materialsQueued);
    LUA->SetField(-2, "materialsQueued");
    
    LUA->PushNumber(stats.texturesConverted);
    LUA->SetField(-2, "texturesConverted");
    
    LUA->PushNumber(stats.materialsWithNormals);
    LUA->SetField(-2, "materialsWithNormals");
    
    LUA->PushNumber(stats.materialsWithRoughness);
    LUA->SetField(-2, "materialsWithRoughness");
    
    LUA->PushNumber(stats.failedConversions);
    LUA->SetField(-2, "failedConversions");
    
    LUA->PushNumber(stats.pendingCategories);
    LUA->SetField(-2, "pendingCategories");
    
    return 1;
}

static int MaterialPipeline_ProcessMaterial(lua_State* L) {
    auto* LUA = reinterpret_cast<GarrysMod::Lua::ILuaBase*>(L);
    
    if (!LUA->IsType(1, GarrysMod::Lua::Type::String)) {
        LUA->ThrowError("Expected string argument for material name");
        return 0;
    }
    
    const char* materialName = LUA->GetString(1);
    bool success = Pipeline::Instance().ProcessMaterial(materialName);
    
    LUA->PushBool(success);
    return 1;
}

static int MaterialPipeline_ProcessAllMaterials(lua_State* L) {
    auto* LUA = reinterpret_cast<GarrysMod::Lua::ILuaBase*>(L);
    
    int processed = Pipeline::Instance().ProcessAllMaterialsThroughPipeline();
    
    LUA->PushNumber(processed);
    return 1;
}

static int MaterialPipeline_ProcessBatch(lua_State* L) {
    auto* LUA = reinterpret_cast<GarrysMod::Lua::ILuaBase*>(L);
    
    int maxCount = 5;
    if (LUA->IsType(1, GarrysMod::Lua::Type::Number)) {
        maxCount = static_cast<int>(LUA->GetNumber(1));
    }
    
    int processed = Pipeline::Instance().ProcessBatch(maxCount);
    
    LUA->PushNumber(processed);
    return 1;
}

static int MaterialPipeline_QueueAllMaterials(lua_State* L) {
    auto* LUA = reinterpret_cast<GarrysMod::Lua::ILuaBase*>(L);
    
    int queued = Pipeline::Instance().QueueAllMaterials();
    
    LUA->PushNumber(queued);
    return 1;
}

static int MaterialPipeline_ClearCache(lua_State* L) {
    Pipeline::Instance().ClearCache();
    return 0;
}

static int MaterialPipeline_SetAutoProcessing(lua_State* L) {
    auto* LUA = reinterpret_cast<GarrysMod::Lua::ILuaBase*>(L);
    
    if (!LUA->IsType(1, GarrysMod::Lua::Type::Bool)) {
        LUA->ThrowError("Expected boolean argument");
        return 0;
    }
    
    bool enabled = LUA->GetBool(1);
    Pipeline::Instance().SetAutoProcessing(enabled);
    
    return 0;
}

static int MaterialPipeline_SetDebugOutput(lua_State* L) {
    auto* LUA = reinterpret_cast<GarrysMod::Lua::ILuaBase*>(L);
    
    if (!LUA->IsType(1, GarrysMod::Lua::Type::Bool)) {
        LUA->ThrowError("Expected boolean argument");
        return 0;
    }
    
    bool enabled = LUA->GetBool(1);
    Pipeline::Instance().SetDebugOutput(enabled);
    
    return 0;
}

static int MaterialPipeline_GetTrackedMaterials(lua_State* L) {
    auto* LUA = reinterpret_cast<GarrysMod::Lua::ILuaBase*>(L);
    
    auto materials = Pipeline::Instance().GetTrackedMaterials();
    
    LUA->CreateTable();
    
    int index = 1;
    for (const auto& name : materials) {
        LUA->PushNumber(index++);
        LUA->PushString(name.c_str());
        LUA->SetTable(-3);
    }
    
    return 1;
}

static int MaterialPipeline_IsMaterialProcessed(lua_State* L) {
    auto* LUA = reinterpret_cast<GarrysMod::Lua::ILuaBase*>(L);
    
    if (!LUA->IsType(1, GarrysMod::Lua::Type::String)) {
        LUA->ThrowError("Expected string argument for material name");
        return 0;
    }
    
    const char* materialName = LUA->GetString(1);
    bool processed = Pipeline::Instance().IsMaterialProcessed(materialName);
    
    LUA->PushBool(processed);
    return 1;
}

static int MaterialPipeline_GetTextureHash(lua_State* L) {
    auto* LUA = reinterpret_cast<GarrysMod::Lua::ILuaBase*>(L);
    
    if (!LUA->IsType(1, GarrysMod::Lua::Type::String)) {
        LUA->ThrowError("Expected string argument for material name");
        return 0;
    }
    
    const char* materialName = LUA->GetString(1);
    uint64_t hash = Pipeline::Instance().GetTextureHash(materialName);
    
    // Return as string for Lua (can't represent full uint64 as number)
    char hashStr[32];
    snprintf(hashStr, sizeof(hashStr), "0x%llX", (unsigned long long)hash);
    LUA->PushString(hashStr);
    
    return 1;
}

static int MaterialPipeline_RescanCategories(lua_State* L) {
    auto* LUA = reinterpret_cast<GarrysMod::Lua::ILuaBase*>(L);
    
    int count = Pipeline::Instance().RescanCategories();
    
    LUA->PushNumber(count);
    return 1;
}

static int MaterialPipeline_WriteUSDA(lua_State* L) {
    auto* LUA = reinterpret_cast<GarrysMod::Lua::ILuaBase*>(L);
    
    bool success = Pipeline::Instance().WriteUSDA();
    
    LUA->PushBool(success);
    return 1;
}

static int MaterialPipeline_SetCategoryFlags(lua_State* L) {
    auto* LUA = reinterpret_cast<GarrysMod::Lua::ILuaBase*>(L);
    
    if (!LUA->IsType(1, GarrysMod::Lua::Type::String) && !LUA->IsType(1, GarrysMod::Lua::Type::Number)) {
        LUA->ThrowError("Expected hash (string or number) as first argument");
        return 0;
    }
    
    if (!LUA->IsType(2, GarrysMod::Lua::Type::Number)) {
        LUA->ThrowError("Expected flags (number) as second argument");
        return 0;
    }
    
    uint64_t hash = 0;
    if (LUA->IsType(1, GarrysMod::Lua::Type::String)) {
        const char* hashStr = LUA->GetString(1);
        hash = std::strtoull(hashStr, nullptr, 0);
    } else {
        hash = static_cast<uint64_t>(LUA->GetNumber(1));
    }
    
    uint32_t flags = static_cast<uint32_t>(LUA->GetNumber(2));
    
    Pipeline::Instance().SetCategoryFlags(hash, flags);
    
    return 0;
}

static int MaterialPipeline_GetCategoryFlags(lua_State* L) {
    auto* LUA = reinterpret_cast<GarrysMod::Lua::ILuaBase*>(L);
    
    if (!LUA->IsType(1, GarrysMod::Lua::Type::String) && !LUA->IsType(1, GarrysMod::Lua::Type::Number)) {
        LUA->ThrowError("Expected hash (string or number) as first argument");
        return 0;
    }
    
    uint64_t hash = 0;
    if (LUA->IsType(1, GarrysMod::Lua::Type::String)) {
        const char* hashStr = LUA->GetString(1);
        hash = std::strtoull(hashStr, nullptr, 0);
    } else {
        hash = static_cast<uint64_t>(LUA->GetNumber(1));
    }
    
    uint32_t flags = Pipeline::Instance().GetCategoryFlags(hash);
    
    LUA->PushNumber(flags);
    return 1;
}

static int MaterialPipeline_FindTextures(lua_State* L) {
    auto* LUA = reinterpret_cast<GarrysMod::Lua::ILuaBase*>(L);
    
    if (!LUA->IsType(1, GarrysMod::Lua::Type::String)) {
        LUA->ThrowError("Expected string argument for search term");
        return 0;
    }
    
    const char* searchTerm = LUA->GetString(1);
    auto results = Pipeline::Instance().FindTextures(searchTerm);
    
    LUA->CreateTable();
    
    int index = 1;
    for (const auto& pair : results) {
        LUA->PushNumber(index++);
        LUA->CreateTable();
        
        LUA->PushString(pair.first.c_str());
        LUA->SetField(-2, "name");
        
        char hashStr[32];
        snprintf(hashStr, sizeof(hashStr), "0x%llX", (unsigned long long)pair.second);
        LUA->PushString(hashStr);
        LUA->SetField(-2, "hash");
        
        LUA->SetTable(-3);
    }
    
    return 1;
}

} // namespace MaterialPipeline

#endif // _WIN64
