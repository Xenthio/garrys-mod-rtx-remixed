// =========================================================================
// shader_fixes.cpp - Shader-Related Fixes for RTX Remix Implementation
// =========================================================================
// Part of the Material Pipeline - Pre-processing stage
// =========================================================================

#ifdef _WIN64

#include "shader_fixes.h"
#include <tier0/dbg.h>
#include <materialsystem/imaterial.h>
#include <materialsystem/imaterialvar.h>
#include <algorithm>

namespace MaterialPipeline {
namespace ShaderFixes {

// =========================================================================
// Internal State
// =========================================================================
static Config s_config;
static std::mutex s_mutex;
static std::unordered_set<std::string> s_fixedMaterials;
static std::unordered_map<FixType, int> s_fixCounts;
static bool s_initialized = false;

// =========================================================================
// Main Interface Implementation
// =========================================================================

void Initialize() {
    std::lock_guard<std::mutex> lock(s_mutex);
    if (s_initialized) return;
    
    s_config = Config();
    s_fixedMaterials.clear();
    s_fixCounts.clear();
    s_initialized = true;
    
    Msg("[MaterialPipeline::ShaderFixes] Initialized\n");
}

void Shutdown() {
    std::lock_guard<std::mutex> lock(s_mutex);
    s_fixedMaterials.clear();
    s_fixCounts.clear();
    s_initialized = false;
    
    Msg("[MaterialPipeline::ShaderFixes] Shutdown\n");
}

void SetConfig(const Config& config) {
    std::lock_guard<std::mutex> lock(s_mutex);
    s_config = config;
}

const Config& GetConfig() {
    return s_config;
}

// =========================================================================
// Material Fixing
// =========================================================================

bool NeedsFix(const std::string& materialName, IMaterial* material) {
    if (!s_config.enabled) return false;
    if (!material) return false;
    if (materialName.empty()) return false;
    
    // Already fixed?
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        if (s_fixedMaterials.count(materialName) > 0) return false;
    }
    
    // Get shader name
    const char* shaderName = material->GetShaderName();
    if (!shaderName) return false;
    
    std::string shader = shaderName;
    std::transform(shader.begin(), shader.end(), shader.begin(), ::tolower);
    
    // Check if this is a Refract shader (needs special handling)
    if (s_config.fixRefractShaders && shader.find("refract") != std::string::npos) {
        return true;
    }
    
    // TODO: Add more fix detection as needed
    
    return false;
}

FixResult ApplyFix(const std::string& materialName, IMaterial* material) {
    FixResult result;
    
    if (!s_config.enabled || !material || materialName.empty()) {
        return result;
    }
    
    // Check if already fixed
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        if (s_fixedMaterials.count(materialName) > 0) {
            return result;
        }
    }
    
    // Get shader name
    const char* shaderName = material->GetShaderName();
    if (!shaderName) return result;
    
    std::string shader = shaderName;
    std::transform(shader.begin(), shader.end(), shader.begin(), ::tolower);
    
    // Try Refract shader fix
    if (s_config.fixRefractShaders && shader.find("refract") != std::string::npos) {
        result = FixRefractShader(materialName, material);
        if (result.applied) {
            std::lock_guard<std::mutex> lock(s_mutex);
            s_fixedMaterials.insert(materialName);
            s_fixCounts[FixType::RefractToTranslucent]++;
            return result;
        }
    }
    
    // Try proxy fixes
    if (s_config.fixProxies) {
        result = FixProxies(materialName, material);
        if (result.applied) {
            std::lock_guard<std::mutex> lock(s_mutex);
            s_fixedMaterials.insert(materialName);
            s_fixCounts[FixType::ProxyDisable]++;
            return result;
        }
    }
    
    // Try parameter normalization
    if (s_config.normalizeParams) {
        result = NormalizeParameters(materialName, material);
        if (result.applied) {
            std::lock_guard<std::mutex> lock(s_mutex);
            s_fixedMaterials.insert(materialName);
            s_fixCounts[FixType::ParameterNormalize]++;
            return result;
        }
    }
    
    return result;
}

bool IsFixed(const std::string& materialName) {
    std::lock_guard<std::mutex> lock(s_mutex);
    return s_fixedMaterials.count(materialName) > 0;
}

// =========================================================================
// Specific Fixes
// =========================================================================

FixResult FixRefractShader(const std::string& materialName, IMaterial* material) {
    FixResult result;
    result.type = FixType::RefractToTranslucent;
    
    if (!material) return result;
    
    // Refract shaders in Source Engine use $basetexture for the normal map
    // and $refracttinttexture for color. For RTX Remix, we need to:
    // 1. Treat the material as translucent glass
    // 2. Use $refracttinttexture as the transmittance texture
    // 3. NOT use $basetexture as albedo (it's actually a normal map)
    
    // This fix is informational - actual processing happens in ToPBR
    // where the Refract shader detection is used to set isRefractShader flag
    
    result.applied = true;
    result.description = "Refract shader detected - will be processed as translucent glass";
    
    if (s_config.debugOutput) {
        Msg("[ShaderFixes] %s: %s\n", materialName.c_str(), result.description.c_str());
    }
    
    return result;
}

FixResult FixProxies(const std::string& materialName, IMaterial* material) {
    FixResult result;
    result.type = FixType::ProxyDisable;
    
    // TODO: Implement proxy detection and fixing
    // Some proxies that cause issues:
    // - AnimatedTexture (texture changes break hash consistency)
    // - TextureScroll (UV changes don't translate well to RTX)
    // - MaterialModify (runtime modifications)
    
    return result;
}

FixResult NormalizeParameters(const std::string& materialName, IMaterial* material) {
    FixResult result;
    result.type = FixType::ParameterNormalize;
    
    // TODO: Implement parameter normalization
    // Examples:
    // - Clamp $phongexponent to 0-150 range
    // - Normalize $envmaptint to 0-1 range
    // - Fix inverted $bumpmap alpha channels
    
    return result;
}

// =========================================================================
// Query
// =========================================================================

std::vector<std::string> GetFixedMaterials() {
    std::lock_guard<std::mutex> lock(s_mutex);
    return std::vector<std::string>(s_fixedMaterials.begin(), s_fixedMaterials.end());
}

int GetFixCount(FixType type) {
    std::lock_guard<std::mutex> lock(s_mutex);
    auto it = s_fixCounts.find(type);
    return (it != s_fixCounts.end()) ? it->second : 0;
}

int GetTotalFixCount() {
    std::lock_guard<std::mutex> lock(s_mutex);
    return static_cast<int>(s_fixedMaterials.size());
}

void Reset() {
    std::lock_guard<std::mutex> lock(s_mutex);
    s_fixedMaterials.clear();
    // Keep fix counts for statistics
    
    Msg("[MaterialPipeline::ShaderFixes] Reset\n");
}

// =========================================================================
// Lua Bindings
// =========================================================================

LUA_FUNCTION(ShaderFixes_SetEnabled) {
    s_config.enabled = LUA->GetBool(1);
    return 0;
}

LUA_FUNCTION(ShaderFixes_IsFixed) {
    const char* materialName = LUA->CheckString(1);
    LUA->PushBool(IsFixed(materialName));
    return 1;
}

LUA_FUNCTION(ShaderFixes_GetFixedMaterials) {
    auto materials = GetFixedMaterials();
    
    LUA->CreateTable();
    int i = 1;
    for (const auto& mat : materials) {
        LUA->PushNumber(i++);
        LUA->PushString(mat.c_str());
        LUA->SetTable(-3);
    }
    
    return 1;
}

LUA_FUNCTION(ShaderFixes_GetTotalFixCount) {
    LUA->PushNumber(GetTotalFixCount());
    return 1;
}

LUA_FUNCTION(ShaderFixes_Reset) {
    Reset();
    return 0;
}

void RegisterLuaBindings(GarrysMod::Lua::ILuaBase* LUA) {
    LUA->PushSpecial(GarrysMod::Lua::SPECIAL_GLOB);
    LUA->CreateTable();
    
    LUA->PushCFunction(ShaderFixes_SetEnabled);
    LUA->SetField(-2, "SetEnabled");
    
    LUA->PushCFunction(ShaderFixes_IsFixed);
    LUA->SetField(-2, "IsFixed");
    
    LUA->PushCFunction(ShaderFixes_GetFixedMaterials);
    LUA->SetField(-2, "GetFixedMaterials");
    
    LUA->PushCFunction(ShaderFixes_GetTotalFixCount);
    LUA->SetField(-2, "GetTotalFixCount");
    
    LUA->PushCFunction(ShaderFixes_Reset);
    LUA->SetField(-2, "Reset");
    
    LUA->SetField(-2, "ShaderFixes");
    LUA->Pop();
    
    Msg("[MaterialPipeline::ShaderFixes] Lua bindings registered\n");
}

} // namespace ShaderFixes
} // namespace MaterialPipeline

#endif // _WIN64
