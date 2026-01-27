// =========================================================================
// material_pipeline.h - Unified Material Processing Pipeline
// =========================================================================
// This is the single entry point for all material/texture processing in
// the RTX Remix integration. It orchestrates the flow from material
// detection through to PBR output.
//
// =========================================================================
// UNIFIED PIPELINE STAGES (executed in order for each material):
// =========================================================================
//
//   1. ShaderFixes (shader_fixes/)
//      - Handle Refract shader materials (glass, water distortion)
//      - Fix material proxies that cause issues with RTX
//      - Normalize shader parameters to expected ranges
//
//   2. HashCollisionFixer (hash_collision_fixer/)
//      - Detect solid-color VTF textures that would have hash collisions
//      - Report materials needing $basetexture replacement to Lua
//
//   3. AutoCategorisation (auto_categorisation/)
//      - Classify materials: particles, decals, emissive, sky, water, etc.
//      - Apply Remix category flags to texture hashes
//      - Handle pending categorizations for textures without hashes yet
//
//   4. ToPBR (to_pbr/)
//      - Extract PBR properties from VMT files
//      - Convert VTF textures to DDS format
//      - Generate normal, roughness, metallic maps
//      - Output USDA material definitions
//
// =========================================================================
// Usage:
//   Pipeline::Instance().Initialize(device, remix);
//   Pipeline::Instance().ProcessMaterial("materials/metal/metal001");
//   // or for all materials:
//   Pipeline::Instance().ProcessAllMaterialsThroughPipeline();
// =========================================================================
//
// This header provides the unified interface. Internal components are
// implementation details that can be refactored without breaking the API.
// =========================================================================

#pragma once

#ifdef _WIN64

#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <mutex>
#include <atomic>
#include <functional>
#include <cstdint>
#include <GarrysMod/Lua/Interface.h>

// Forward declarations
class IDirect3DDevice9Ex;
class IDirect3DTexture9;
class IMaterial;
class IFileSystem;

namespace remix {
class Interface;
}

namespace MaterialPipeline {

// =========================================================================
// Pipeline Configuration
// =========================================================================
struct PipelineConfig {
    // Processing options
    bool autoProcessing = true;             // Auto-process materials as detected
    bool debugOutput = false;               // Verbose logging
    bool metallicGeneration = false;        // Experimental metallic from brightness
    bool autoDiscoverTextures = true;       // Find _normal, _height, _mask textures
    bool parseCommentedProperties = false;  // Parse commented-out VMT properties
    
    // Categorization options
    bool particleCategorization = true;     // Auto-categorize particles
    bool decalCategorization = true;        // Auto-categorize decals
    bool emissiveCategorization = true;     // Auto-categorize emissive materials
    
    // Output paths
    std::string outputDirectory;            // Where to write DDS files
};

// =========================================================================
// Pipeline Statistics
// =========================================================================
struct PipelineStats {
    // Detection stats
    int materialsTracked = 0;               // Materials seen by D3D9 hooks
    int texturesCached = 0;                 // Unique textures cached
    
    // Processing stats
    int materialsProcessed = 0;             // Materials fully processed
    int materialsQueued = 0;                // Materials waiting in queue
    int texturesConverted = 0;              // VTF->DDS conversions
    int texturesSkipped = 0;                // Already existed
    
    // PBR stats
    int materialsWithNormals = 0;           // Have normal maps
    int materialsWithRoughness = 0;         // Have roughness maps
    int materialsWithEmission = 0;          // Have emissive textures
    int glassMaterials = 0;                 // Translucent/glass materials
    
    // Error stats
    int failedConversions = 0;              // Conversion errors
    int pendingCategories = 0;              // Awaiting hash
};

// =========================================================================
// Material Info (for querying processed materials)
// =========================================================================
struct MaterialInfo {
    std::string name;
    uint64_t textureHash = 0;
    bool processed = false;
    bool isGlass = false;
    bool hasNormal = false;
    bool hasRoughness = false;
    bool hasEmission = false;
    float roughnessConstant = 0.5f;
    float metallicConstant = 0.0f;
};

// =========================================================================
// Pipeline Event Callbacks
// =========================================================================
using MaterialDetectedCallback = std::function<void(const std::string& materialName, uint64_t hash)>;
using MaterialProcessedCallback = std::function<void(const std::string& materialName, bool success)>;

// =========================================================================
// Main Pipeline Class
// =========================================================================
class Pipeline {
public:
    // Singleton access
    static Pipeline& Instance();
    
    // =====================================================================
    // Static convenience functions for initialization
    // =====================================================================
    
    // Initialize the unified pipeline and register all Lua bindings
    // This is the primary entry point - call this from RemixAPI initialization
    static bool Initialize(remix::Interface* remix, GarrysMod::Lua::ILuaBase* LUA);
    
    // Shutdown the unified pipeline (static wrapper)
    static void Shutdown();
    
    // Called by D3D9TextureTracker when a new material is detected (static wrapper)
    // This routes to the unified pipeline for processing through all stages
    static void OnNewMaterialDetected(const std::string& materialName, uint64_t textureHash, IDirect3DTexture9* pTexture = nullptr);
    
    // =====================================================================
    // Lifecycle (instance methods)
    // =====================================================================
    
    // Initialize the pipeline with D3D9 device and Remix interface
    // This sets up all internal components
    bool InitializeInternal(IDirect3DDevice9Ex* device, remix::Interface* remix);
    
    // Shutdown and cleanup all resources
    void ShutdownInternal();
    
    // Check if pipeline is ready
    bool IsInitialized() const { return m_initialized; }
    
    // =====================================================================
    // Configuration
    // =====================================================================
    
    // Get/set configuration
    const PipelineConfig& GetConfig() const { return m_config; }
    void SetConfig(const PipelineConfig& config);
    
    // Individual config setters for convenience
    void SetAutoProcessing(bool enabled);
    void SetDebugOutput(bool enabled);
    void SetOutputDirectory(const std::string& path);
    
    // =====================================================================
    // Processing Control
    // =====================================================================
    
    // Queue all tracked materials for background processing
    // Returns number of materials queued
    int QueueAllMaterials();
    
    // Process a single material through the unified pipeline (blocking)
    // Pipeline stages: ShaderFixes → HashCollisionFixer → AutoCategorisation → ToPBR
    bool ProcessMaterial(const std::string& materialName);
    
    // Process ALL tracked materials through the unified pipeline
    // Returns number of materials processed
    int ProcessAllMaterialsThroughPipeline();
    
    // Process a batch of materials (for frame-distributed processing)
    // Returns number processed
    int ProcessBatch(int maxCount = 5);
    
    // Check if background processing is active
    bool IsProcessing() const;
    
    // Get current queue size
    size_t GetQueueSize() const;
    
    // =====================================================================
    // Texture Tracking
    // =====================================================================
    
    // Get texture hash for a material (0 if not tracked)
    uint64_t GetTextureHash(const std::string& materialName) const;
    
    // Get all tracked material names
    std::vector<std::string> GetTrackedMaterials() const;
    
    // Clear tracking cache (for map changes)
    void ClearCache();
    
    // =====================================================================
    // World Textures (BSP-based materials)
    // =====================================================================
    
    // Set list of world texture names from BSP
    void SetWorldTextures(const std::vector<std::string>& textureNames);
    
    // Clear world texture list
    void ClearWorldTextures();
    
    // Check if material is a world texture
    bool IsWorldTexture(const std::string& materialName) const;
    
    // =====================================================================
    // Categorization
    // =====================================================================
    
    // Set category flags for a texture hash
    void SetCategoryFlags(uint64_t hash, uint32_t flags);
    
    // Get category flags for a texture hash
    uint32_t GetCategoryFlags(uint64_t hash) const;
    
    // Re-scan all materials for categorization
    int RescanCategories();
    
    // Retry pending categorizations (for textures that had hash=0)
    int RetryPendingCategories();
    
    // =====================================================================
    // USDA Output
    // =====================================================================
    
    // Write USDA files if there are new materials
    void WriteUSDAIfNeeded();
    
    // Force write USDA files
    bool WriteUSDA();
    
    // Append new materials to existing USDA
    bool AppendToUSDA();
    
    // =====================================================================
    // Query
    // =====================================================================
    
    // Get statistics
    PipelineStats GetStats() const;
    
    // Check if a material has been processed
    bool IsMaterialProcessed(const std::string& materialName) const;
    
    // Get info about a processed material
    MaterialInfo GetMaterialInfo(const std::string& materialName) const;
    
    // Find textures by partial name
    std::vector<std::pair<std::string, uint64_t>> FindTextures(const std::string& searchTerm) const;
    
    // =====================================================================
    // Lua Bindings
    // =====================================================================
    
    // Register all Lua bindings for the pipeline
    void RegisterLuaBindings(GarrysMod::Lua::ILuaBase* LUA);
    
    // =====================================================================
    // Event Callbacks
    // =====================================================================
    
    // Set callback for when materials are detected
    void SetOnMaterialDetected(MaterialDetectedCallback callback);
    
    // Set callback for when materials are processed
    void SetOnMaterialProcessed(MaterialProcessedCallback callback);
    
    // =====================================================================
    // Internal (used by pipeline components, not for external use)
    // =====================================================================
    
    // Called by D3D9TextureTracker when a new material is detected
    void OnMaterialDetected(const std::string& materialName, uint64_t textureHash);
    
    // Get filesystem interface
    IFileSystem* GetFileSystem();
    
    // Get Remix interface
    remix::Interface* GetRemixInterface() { return m_remix; }
    
private:
    Pipeline();
    ~Pipeline();
    
    // Prevent copying
    Pipeline(const Pipeline&) = delete;
    Pipeline& operator=(const Pipeline&) = delete;
    
    // Apply current config to internal components
    void ApplyConfig();
    
    // Internal state
    bool m_initialized = false;
    PipelineConfig m_config;
    
    // External interfaces
    IDirect3DDevice9Ex* m_device = nullptr;
    remix::Interface* m_remix = nullptr;
    IFileSystem* m_fileSystem = nullptr;
    
    // Callbacks
    MaterialDetectedCallback m_onDetected;
    MaterialProcessedCallback m_onProcessed;
    
    // Thread safety
    mutable std::mutex m_mutex;
};

// =========================================================================
// Category Flags (shared with Remix API)
// =========================================================================
namespace CategoryFlags {
    constexpr uint32_t NONE = 0;
    constexpr uint32_t WORLD_UI = (1 << 0);
    constexpr uint32_t WORLD_MATTE = (1 << 1);
    constexpr uint32_t PARTICLE = (1 << 2);
    constexpr uint32_t BEAM = (1 << 3);
    constexpr uint32_t DECAL_STATIC = (1 << 4);
    constexpr uint32_t DECAL_DYNAMIC = (1 << 5);
    constexpr uint32_t DECAL_SINGLE_OFFSET = (1 << 6);
    constexpr uint32_t DECAL_NO_OFFSET = (1 << 7);
    constexpr uint32_t ALPHA_BLEND_TO_CUTOUT = (1 << 8);
    constexpr uint32_t TERRAIN = (1 << 9);
    constexpr uint32_t ANIMATED_WATER = (1 << 10);
    constexpr uint32_t THIRD_PERSON_PLAYER_MODEL = (1 << 11);
    constexpr uint32_t THIRD_PERSON_PLAYER_BODY = (1 << 12);
    constexpr uint32_t IGNORED = (1 << 13);  // Renamed from IGNORE to avoid Windows macro conflict
    constexpr uint32_t IGNORE_BAKED_LIGHTING = (1 << 14);
    constexpr uint32_t HIDDEN = (1 << 15);
    constexpr uint32_t SKY = (1 << 16);
}

} // namespace MaterialPipeline

#endif // _WIN64
