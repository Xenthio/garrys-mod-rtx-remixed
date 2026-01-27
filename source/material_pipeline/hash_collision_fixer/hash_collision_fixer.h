// =========================================================================
// hash_collision_fixer.h - Fix RTX Remix hash collisions for solid-color textures
// =========================================================================
// Detects solid-color VTF textures that would have hash collisions in RTX Remix
// and exposes API for Lua to fix them by swapping $basetexture.
//
// Pipeline Stage: Pre-processing
// This runs BEFORE material processing to identify problematic textures.
// =========================================================================

#pragma once

#ifdef _WIN64

#include <string>
#include <vector>
#include <unordered_set>
#include <unordered_map>
#include <mutex>
#include <cstdint>

// Forward declarations
class IFileSystem;

namespace GarrysMod {
namespace Lua {
class ILuaBase;
}
}

namespace MaterialPipeline {
namespace HashCollisionFixer {

// =========================================================================
// Solid Color Material Info
// =========================================================================
struct SolidColorMaterial {
    std::string materialName;
    std::string texturePath;
    uint8_t r, g, b, a;
    bool fixed = false;
};

// =========================================================================
// Main Interface
// =========================================================================

// Initialize the hash collision fixer
void Initialize();

// Shutdown and cleanup
void Shutdown();

// Check if a material's $basetexture is a solid color
// @param fileSystem - Source Engine filesystem
// @param materialName - Name of the material
// @param texturePath - Path to the $basetexture VTF
// @param debugOutput - Enable debug logging
// @return true if it's a solid color and should be fixed
bool CheckMaterial(IFileSystem* fileSystem,
                   const std::string& materialName,
                   const std::string& texturePath,
                   bool debugOutput = false);

// Get list of solid-color materials that need fixing
// @return Vector of material names needing fixes
std::vector<std::string> GetMaterialsNeedingFix();

// Mark a material as fixed (so it won't be returned again)
void MarkMaterialFixed(const std::string& materialName);

// Check if a material has been fixed
bool IsMaterialFixed(const std::string& materialName);

// Check if a material is known to be solid-color
bool IsSolidColorMaterial(const std::string& materialName);

// Get the solid color info for a material (if known)
// @return true if found, fills in r,g,b,a values
bool GetMaterialColor(const std::string& materialName, 
                      uint8_t& r, uint8_t& g, uint8_t& b, uint8_t& a);

// Clear all tracking data (call on map change)
void Reset();

// Get statistics
size_t GetTotalDetected();
size_t GetTotalFixed();
size_t GetPendingCount();

// =========================================================================
// Lua Bindings
// =========================================================================

// Register Lua functions for hash collision fixer
void RegisterLuaBindings(GarrysMod::Lua::ILuaBase* LUA);

} // namespace HashCollisionFixer
} // namespace MaterialPipeline

// =========================================================================
// Legacy Namespace Alias (for backwards compatibility)
// =========================================================================
namespace HashCollisionFixer = MaterialPipeline::HashCollisionFixer;

#endif // _WIN64
