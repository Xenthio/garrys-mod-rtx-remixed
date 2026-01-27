// =========================================================================
// auto_categorisation.h - Automatic Material Categorization for RTX Remix
// =========================================================================
// Automatically detects and categorizes materials based on their properties:
// - Particles, effects, sprites
// - Skybox textures
// - Water/refract materials
// - Decals (static/dynamic)
// - Emissive/self-illuminated materials
// - Tool textures (nodraw, clip, etc.)
//
// Pipeline Stage: Detection
// This runs during material detection to classify materials for Remix.
// =========================================================================

#pragma once

#ifdef _WIN64

#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <mutex>
#include <cstdint>

// Forward declarations
class IMaterial;
class IFileSystem;
class IDirect3DTexture9;

namespace remix {
class Interface;
}

namespace GarrysMod {
namespace Lua {
class ILuaBase;
}
}

namespace MaterialPipeline {
namespace AutoCategorisation {

// =========================================================================
// Category Flags (matches Remix API categories)
// =========================================================================
namespace CategoryFlags {
    constexpr uint32_t NONE = 0;
    constexpr uint32_t WORLD_UI = (1 << 0);
    constexpr uint32_t WORLD_MATTE = (1 << 1);
    constexpr uint32_t SKY = (1 << 2);
    constexpr uint32_t IGNORE = (1 << 3);
    constexpr uint32_t PARTICLE = (1 << 10);
    constexpr uint32_t BEAM = (1 << 11);
    constexpr uint32_t DECAL_STATIC = (1 << 12);
    constexpr uint32_t DECAL_DYNAMIC = (1 << 13);
    constexpr uint32_t DECAL_SINGLE_OFFSET = (1 << 14);
    constexpr uint32_t DECAL_NO_OFFSET = (1 << 15);
    constexpr uint32_t ALPHA_BLEND_TO_CUTOUT = (1 << 16);
    constexpr uint32_t TERRAIN = (1 << 17);
    constexpr uint32_t ANIMATED_WATER = (1 << 18);
    constexpr uint32_t THIRD_PERSON_PLAYER_MODEL = (1 << 19);
    constexpr uint32_t THIRD_PERSON_PLAYER_BODY = (1 << 20);
    constexpr uint32_t HIDDEN = (1 << 23);
    constexpr uint32_t EMISSIVE = (1 << 24);
}

// =========================================================================
// Configuration
// =========================================================================
struct Config {
    bool enabled = true;              // Master enable switch
    bool particleEnabled = true;      // Auto-categorize particles
    bool decalEnabled = true;         // Auto-categorize decals
    bool emissiveEnabled = true;      // Auto-categorize emissive materials
    bool skyEnabled = true;           // Auto-categorize skybox
    bool waterEnabled = true;         // Auto-categorize water
    bool toolEnabled = true;          // Auto-categorize tool textures
    bool debugOutput = false;         // Enable debug logging
};

// =========================================================================
// Pending Category Entry
// =========================================================================
struct PendingCategory {
    IDirect3DTexture9* texture = nullptr;
    std::string materialName;
    uint32_t categoryFlags = 0;
};

// =========================================================================
// Main Interface
// =========================================================================

// Initialize the auto-categorisation system
void Initialize(remix::Interface* remix);

// Shutdown and cleanup
void Shutdown();

// Configure the system
void SetConfig(const Config& config);
const Config& GetConfig();

// Individual config setters
void SetEnabled(bool enabled);
void SetParticleCategorisation(bool enabled);
void SetDecalCategorisation(bool enabled);
void SetEmissiveCategorisation(bool enabled);
void SetDebugOutput(bool enabled);

// =========================================================================
// Material Detection
// =========================================================================

// Detect and apply categories for a material
// @param materialName - Name of the material (lowercase)
// @param material - Source Engine IMaterial (can be null)
// @param texture - D3D9 texture pointer
// @return Category flags that were applied
uint32_t DetectAndApply(const std::string& materialName, 
                        IMaterial* material,
                        IDirect3DTexture9* texture);

// Detect category for a material without applying
// @param materialName - Name of the material (lowercase)
// @param material - Source Engine IMaterial (can be null)
// @return Detected category flags
uint32_t DetectCategory(const std::string& materialName, IMaterial* material);

// Apply category flags to a texture hash
// @param hash - Texture hash from Remix
// @param flags - Category flags to apply
// @param materialName - For logging
void ApplyToHash(uint64_t hash, uint32_t flags, const std::string& materialName);

// =========================================================================
// World Textures (BSP-based)
// =========================================================================

// Set list of world texture names from BSP parsing
void SetWorldTextureNames(const std::vector<std::string>& textureNames);

// Clear the world texture list (for map changes)
void ClearWorldTextureNames();

// Check if a material is a world texture
bool IsWorldTexture(const std::string& materialName);

// Re-check cached materials for world texture categorization
int RecheckWorldTextures();

// =========================================================================
// Hash-to-Category Mapping
// =========================================================================

// Set category flags for a texture hash
void SetHashCategoryFlags(uint64_t hash, uint32_t flags);

// Remove category flags for a texture hash
void RemoveHashCategoryFlags(uint64_t hash);

// Clear all hash-to-category mappings
void ClearHashCategoryMappings();

// Get category flags for a texture hash
// @return true if found, fills outFlags
bool GetHashCategoryFlags(uint64_t hash, uint32_t* outFlags);

// =========================================================================
// Pending Categories (textures that returned hash=0)
// =========================================================================

// Add a pending categorization
void AddPendingCategory(IDirect3DTexture9* texture, 
                        const std::string& materialName,
                        uint32_t flags);

// Retry pending categorizations
// @return Number of textures successfully categorized
int RetryPendingCategories();

// Get count of pending textures
size_t GetPendingCount();

// =========================================================================
// Re-scanning
// =========================================================================

// Re-scan all cached materials and apply categories
// @return Number of materials categorized
int RescanAllMaterials();

// =========================================================================
// Statistics
// =========================================================================
struct Stats {
    int materialsScanned = 0;
    int particlesCategorized = 0;
    int decalsCategorized = 0;
    int emissivesCategorized = 0;
    int skyCategorized = 0;
    int waterCategorized = 0;
    int ignoredCategorized = 0;
    int pendingCategories = 0;
};

Stats GetStats();

// =========================================================================
// Lua Bindings
// =========================================================================

void RegisterLuaBindings(GarrysMod::Lua::ILuaBase* LUA);

// =========================================================================
// VMT Parsing Helpers
// =========================================================================

// Check if VMT file contains "$selfillum" "1"
bool CheckVMTForSelfillum(const std::string& materialName, bool debug = false);

// Get shader name from material
std::string GetShaderName(IMaterial* material);

} // namespace AutoCategorisation
} // namespace MaterialPipeline

#endif // _WIN64
