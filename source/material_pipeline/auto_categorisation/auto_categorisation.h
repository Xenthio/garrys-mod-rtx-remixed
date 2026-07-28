// =========================================================================
// auto_categorisation.h - Automatic Material Categorization for RTX Remix
// =========================================================================
// Automatically detects and categorizes materials based on their properties:
// - Particles, effects, sprites
// - Decals (static/dynamic)
// - Emissive/self-illuminated materials
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
#include <GarrysMod/Lua/Interface.h>

// Forward declarations
class IMaterial;
class IFileSystem;
struct IDirect3DTexture9;

namespace remix {
class Interface;
}

namespace MaterialPipeline {
namespace AutoCategorisation {

// =========================================================================
// Category Flags (matches Remix API categories and d3d9_texture_tracker)
// =========================================================================
namespace CategoryFlags {
    constexpr uint32_t NONE                      = 0x0;
    constexpr uint32_t WORLD_UI                  = 0x1;       // WARNING: Do not apply - can cause issues
    constexpr uint32_t WORLD_MATTE               = 0x2;
    constexpr uint32_t SKY                       = 0x4;       // Must match tracker
    constexpr uint32_t IGNORED                   = 0x8;       // Renamed from IGNORE to avoid Windows macro conflict
    constexpr uint32_t TERRAIN                   = 0x10;
    constexpr uint32_t ALPHA_BLEND_TO_CUTOUT     = 0x20;
    constexpr uint32_t THIRD_PERSON_PLAYER_MODEL = 0x40;
    constexpr uint32_t THIRD_PERSON_PLAYER_BODY  = 0x80;
    constexpr uint32_t BEAM                      = 0x100;
    constexpr uint32_t ANIMATED                  = 0x200;
    constexpr uint32_t PARTICLE                  = 0x400;     // Must match tracker
    constexpr uint32_t DECAL                     = 0x800;
    constexpr uint32_t DECAL_STATIC              = 0x1000;    // Must match tracker
    constexpr uint32_t DECAL_DYNAMIC             = 0x2000;
    constexpr uint32_t DECAL_SINGLE_OFFSET       = 0x4000;
    constexpr uint32_t DECAL_NO_OFFSET           = 0x8000;
    constexpr uint32_t IGNORE_BAKED_LIGHTING     = 0x10000;
    constexpr uint32_t HIDDEN                    = 0x20000;
    constexpr uint32_t ANIMATED_WATER            = 0x40000;   // Must match tracker
    constexpr uint32_t EMISSIVE                  = 0x1000000; // Must match tracker
}

// =========================================================================
// Configuration
// =========================================================================
struct Config {
    bool enabled = true;              // Master enable switch
    bool particleEnabled = true;      // Auto-categorize particles
    bool decalEnabled = true;         // Auto-categorize decals
    bool emissiveEnabled = true;      // Auto-categorize emissive materials
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

// Clear material-name observations and pending texture references at a map
// boundary while retaining applied hash flags for collision reconciliation.
void ClearMapState();

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

// Detect and apply categories for a material, trying ALL texture variants
// This is more robust for animated textures where some variants may have hash=0
// @param materialName - Name of the material (lowercase)
// @param material - Source Engine IMaterial (can be null)
// @param textureVariants - Vector of all texture variants for this material
// @return Category flags that were applied (or queued for pending)
uint32_t DetectAndApplyAllVariants(const std::string& materialName, 
                                    IMaterial* material,
                                    const std::vector<IDirect3DTexture9*>* textureVariants);

// Apply previously-detected category flags to a newly-discovered texture variant.
// Uses stored flags from a prior DetectAndApply/DetectAndApplyAllVariants call.
// @param materialName - Name of the material (lowercase)
// @param texture - The new D3D9 texture variant
// @return true if flags were applied immediately, false if queued for pending or no stored flags
bool ApplyKnownCategoryToTexture(const std::string& materialName, IDirect3DTexture9* texture);

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

// Re-evaluate global category bits after another verified Source material is
// associated with a hash. LEGACY_EMISSIVE is removed when any material sharing
// the hash is not emissive, since Remix categories operate on the hash globally.
void ReconcileHashCategories(uint64_t hash);

// Remove all Remix API registrations for a hash.
// Calls RemoveTextureHash for every category bit stored under this hash,
// erases the hash from s_hashToCategoryFlags, and removes it from the shared
// force-albedo registry.
// Use this when a material's hash changes so stale Remix-side state is cleaned up.
// @param hash        - The old/wrong hash to remove
// @param materialName - Clears the per-material flag cache as well
void UnapplyFromRemix(uint64_t hash, const std::string& materialName);

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
// Hash Export/Import
// =========================================================================

// Export all categorized hashes to a conf file for RTX Remix to load
// @param filepath - Path to write the conf file (e.g., "rtx-remix/rtx_auto_hashes.conf")
// @return Number of hashes exported, or -1 on error
int ExportHashesToFile(const std::string& filepath);

// Get all categorized hashes grouped by category
// @return Map of category name to list of hashes
std::unordered_map<std::string, std::vector<uint64_t>> GetCategorizedHashes();

// =========================================================================
// Statistics
// =========================================================================
struct Stats {
    int materialsScanned = 0;
    int particlesCategorized = 0;
    int decalsCategorized = 0;
    int emissivesCategorized = 0;
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
