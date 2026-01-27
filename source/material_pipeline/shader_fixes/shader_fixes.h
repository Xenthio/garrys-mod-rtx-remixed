// =========================================================================
// shader_fixes.h - Shader-Related Fixes for RTX Remix
// =========================================================================
// Handles shader-related fixes and workarounds for RTX Remix compatibility:
// - Shader parameter normalization
// - Shader fallback handling
// - Material proxy fixes
// - Runtime shader property modifications
//
// Pipeline Stage: Pre-processing
// This runs BEFORE material detection to prepare shaders for processing.
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

namespace GarrysMod {
namespace Lua {
class ILuaBase;
}
}

namespace MaterialPipeline {
namespace ShaderFixes {

// =========================================================================
// Configuration
// =========================================================================
struct Config {
    bool enabled = true;              // Master enable switch
    bool fixRefractShaders = true;    // Fix Refract shader materials
    bool fixProxies = true;           // Fix material proxies
    bool normalizeParams = true;      // Normalize shader parameters
    bool debugOutput = false;         // Enable debug logging
};

// =========================================================================
// Shader Fix Types
// =========================================================================
enum class FixType {
    None = 0,
    RefractToTranslucent,     // Convert Refract shader to translucent
    ProxyDisable,             // Disable problematic proxies
    ParameterNormalize,       // Normalize parameter values
    AlphaFix,                 // Fix alpha blending issues
    EmissiveFix               // Fix emissive material issues
};

// =========================================================================
// Fix Result
// =========================================================================
struct FixResult {
    bool applied = false;
    FixType type = FixType::None;
    std::string description;
};

// =========================================================================
// Main Interface
// =========================================================================

// Initialize the shader fixes system
void Initialize();

// Shutdown and cleanup
void Shutdown();

// Configure the system
void SetConfig(const Config& config);
const Config& GetConfig();

// =========================================================================
// Material Fixing
// =========================================================================

// Check if a material needs shader fixes
// @param materialName - Name of the material
// @param material - Source Engine IMaterial (can be null)
// @return true if fixes are needed
bool NeedsFix(const std::string& materialName, IMaterial* material);

// Apply shader fixes to a material
// @param materialName - Name of the material
// @param material - Source Engine IMaterial
// @return Fix result with details
FixResult ApplyFix(const std::string& materialName, IMaterial* material);

// Check if a material has been fixed
bool IsFixed(const std::string& materialName);

// =========================================================================
// Specific Fixes
// =========================================================================

// Fix Refract shader materials (glass, water distortion, etc.)
// These need special handling as they use $basetexture for the normal map
FixResult FixRefractShader(const std::string& materialName, IMaterial* material);

// Fix material proxies that cause issues with RTX
// Some proxies animate or modify textures in ways that break hashing
FixResult FixProxies(const std::string& materialName, IMaterial* material);

// Normalize shader parameters to expected ranges
FixResult NormalizeParameters(const std::string& materialName, IMaterial* material);

// =========================================================================
// Query
// =========================================================================

// Get list of fixed materials
std::vector<std::string> GetFixedMaterials();

// Get fix count by type
int GetFixCount(FixType type);

// Get total fix count
int GetTotalFixCount();

// Reset tracking (for map changes)
void Reset();

// =========================================================================
// Lua Bindings
// =========================================================================

void RegisterLuaBindings(GarrysMod::Lua::ILuaBase* LUA);

} // namespace ShaderFixes
} // namespace MaterialPipeline

#endif // _WIN64
