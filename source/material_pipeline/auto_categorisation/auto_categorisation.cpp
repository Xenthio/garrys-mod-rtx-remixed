// =========================================================================
// auto_categorisation.cpp - Automatic Material Categorization for RTX Remix
// =========================================================================
// Part of the Material Pipeline - Detection stage
// =========================================================================

#ifdef _WIN64

#include "auto_categorisation.h"
#include "../../d3d9_texture_tracker.h"
#include "../../remixapi/material_lua_bindings.h"
#include <tier0/dbg.h>
#include <materialsystem/imaterialsystem.h>
#include <materialsystem/imaterial.h>
#include <materialsystem/imaterialvar.h>
#include <filesystem.h>
#include <d3d9.h>
#include <Windows.h>
#include <remix/remix.h>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <sstream>

// External globals
extern IMaterialSystem* materials;

namespace MaterialPipeline {
namespace AutoCategorisation {

// Safe tolower helper for use with std::transform
static char SafeToLower(unsigned char c) {
    return static_cast<char>(std::tolower(c));
}

// =========================================================================
// Internal State
// =========================================================================
static remix::Interface* s_remix = nullptr;
static Config s_config;
static std::recursive_mutex s_mutex;
static std::unordered_map<uint64_t, uint32_t> s_hashToCategoryFlags;
static std::unordered_map<std::string, uint32_t> s_materialToCategoryFlags;
static std::vector<PendingCategory> s_pendingCategories;
static std::unordered_set<std::string> s_worldTextureNames;
static Stats s_stats;
static bool s_initialized = false;

// Unlit shaders emit their full albedo regardless of $selfillum or alpha-mask
// parameters, so their hashes require rtx.legacyEmissiveForceAlbedoString.
static std::unordered_set<std::string> s_forceAlbedoEmissiveMaterials;

// Filesystem access
static IFileSystem* s_pFileSystem = nullptr;

// Helper function to convert hash to string format for Remix API
static std::string HashToString(uint64_t hash) {
    char buffer[32];
    snprintf(buffer, sizeof(buffer), "0x%llX", (unsigned long long)hash);
    return std::string(buffer);
}

static IFileSystem* GetFileSystem() {
    if (s_pFileSystem) return s_pFileSystem;
    
    HMODULE hModule = GetModuleHandleA("filesystem_stdio.dll");
    if (hModule) {
        typedef void* (*CreateInterfaceFn)(const char* pName, int* pReturnCode);
        CreateInterfaceFn createInterface = (CreateInterfaceFn)GetProcAddress(hModule, "CreateInterface");
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
// VMT Parsing Helper
// =========================================================================

bool CheckVMTForSelfillum(const std::string& materialName, bool debug) {
    IFileSystem* fs = GetFileSystem();
    if (!fs) {
        if (debug) Msg("[AutoCategorisation] CheckVMT: Could not get filesystem interface\n");
        return false;
    }
    
    // Build VMT path
    std::string vmtPath = "materials/" + materialName + ".vmt";
    
    // Try to open the file
    FileHandle_t file = fs->Open(vmtPath.c_str(), "rb", "GAME");
    if (!file) {
        vmtPath = "materials/" + materialName;
        file = fs->Open(vmtPath.c_str(), "rb", "GAME");
        if (!file) {
            if (debug) Msg("[AutoCategorisation] CheckVMT: Could not open '%s'\n", vmtPath.c_str());
            return false;
        }
    }
    
    // Read file content
    int fileSize = fs->Size(file);
    if (fileSize <= 0 || fileSize > 65536) {
        if (debug) Msg("[AutoCategorisation] CheckVMT: Invalid file size %d\n", fileSize);
        fs->Close(file);
        return false;
    }
    
    std::string content;
    content.resize(fileSize);
    int bytesRead = fs->Read(&content[0], fileSize, file);
    fs->Close(file);
    
    if (bytesRead <= 0) {
        if (debug) Msg("[AutoCategorisation] CheckVMT: Read failed\n");
        return false;
    }
    
    // Parse line by line to handle comments properly
    std::vector<std::string> lines;
    size_t lineStart = 0;
    for (size_t i = 0; i <= content.size(); ++i) {
        if (i == content.size() || content[i] == '\n' || content[i] == '\r') {
            if (i > lineStart) {
                lines.push_back(content.substr(lineStart, i - lineStart));
            }
            lineStart = i + 1;
        }
    }
    
    // Look for $selfillum followed by 1, excluding commented lines
    for (const auto& line : lines) {
        std::string lowerLine = line;
        std::transform(lowerLine.begin(), lowerLine.end(), lowerLine.begin(), SafeToLower);
        
        if (lowerLine.empty()) continue;
        
        size_t firstChar = lowerLine.find_first_not_of(" \t");
        if (firstChar == std::string::npos) continue;
        
        // Skip commented lines
        if (firstChar + 1 < lowerLine.size() && 
            lowerLine[firstChar] == '/' && lowerLine[firstChar + 1] == '/') {
            continue;
        }
        
        // Look for $selfillum with word boundary
        size_t pos = 0;
        while ((pos = lowerLine.find("$selfillum", pos)) != std::string::npos) {
            size_t endPos = pos + 10;
            if (endPos >= lowerLine.size() || 
                lowerLine[endPos] == ' ' || lowerLine[endPos] == '\t' || 
                lowerLine[endPos] == '"' || lowerLine[endPos] == '\'' ||
                lowerLine[endPos] == '\r' || lowerLine[endPos] == '\n') {
                
                pos = endPos;
                
                while (pos < lowerLine.size() && 
                       (lowerLine[pos] == ' ' || lowerLine[pos] == '\t' || 
                        lowerLine[pos] == '"' || lowerLine[pos] == '\'')) {
                    pos++;
                }
                
                if (pos < lowerLine.size() && lowerLine[pos] == '1') {
                    if (debug) Msg("[AutoCategorisation] CheckVMT: Found active '$selfillum 1'\n");
                    return true;
                }
                
                break;
            }
            pos++;
        }
    }
    
    return false;
}

// Check for an active unlit-emission opt-out. This is the raw-VMT fallback
// for materials whose compiled flag state is unavailable or incomplete.
static bool CheckVMTForUnlitEmissionOptOut(
    const std::string& materialName,
    bool debug,
    bool* hasVertexColor = nullptr,
    bool* hasTranslucent = nullptr,
    bool* hasAdditive = nullptr) {
    if (hasVertexColor) {
        *hasVertexColor = false;
    }
    if (hasTranslucent) {
        *hasTranslucent = false;
    }
    if (hasAdditive) {
        *hasAdditive = false;
    }

    IFileSystem* fs = GetFileSystem();
    if (!fs) return false;

    std::string vmtPath = "materials/" + materialName + ".vmt";
    FileHandle_t file = fs->Open(vmtPath.c_str(), "rb", "GAME");
    if (!file) {
        vmtPath = "materials/" + materialName;
        file = fs->Open(vmtPath.c_str(), "rb", "GAME");
        if (!file) return false;
    }

    const auto fileSize = fs->Size(file);
    if (fileSize == 0 || fileSize > 65536) {
        fs->Close(file);
        return false;
    }

    std::string content(static_cast<size_t>(fileSize), '\0');
    const int bytesRead =
        fs->Read(&content[0], static_cast<int>(fileSize), file);
    fs->Close(file);
    if (bytesRead <= 0) return false;
    content.resize(bytesRead);

    std::istringstream lines(content);
    std::string line;
    bool foundOptOut = false;
    while (std::getline(lines, line)) {
        const size_t commentPos = line.find("//");
        if (commentPos != std::string::npos) {
            line.resize(commentPos);
        }

        std::transform(line.begin(), line.end(), line.begin(), SafeToLower);
        std::replace(line.begin(), line.end(), '"', ' ');
        std::replace(line.begin(), line.end(), '\'', ' ');

        std::istringstream tokens(line);
        std::string key;
        std::string value;
        if (tokens >> key >> value && value == "1") {
            if (key == "$translucent" && hasTranslucent) {
                *hasTranslucent = true;
            } else if (key == "$additive" && hasAdditive) {
                *hasAdditive = true;
            }

            if (key == "$no_fullbright" ||
                key == "$vertexalpha" ||
                key == "$vertexcolor") {
                foundOptOut = true;
                if (hasVertexColor && key == "$vertexcolor") {
                    *hasVertexColor = true;
                }
                if (debug) {
                    Msg("[AutoCategorisation] CheckVMT: Found active unlit emission opt-out '%s 1'\n",
                        key.c_str());
                }
            }
        }
    }

    return foundOptOut;
}

std::string GetShaderName(IMaterial* material) {
    if (!material) return "";
    const char* shaderName = material->GetShaderName();
    if (!shaderName) return "";
    return std::string(shaderName);
}

// Read the shader name from the first non-empty, non-comment line of a VMT.
// Used as a fallback when no IMaterial pointer is available.
std::string ReadVMTShaderName(const std::string& materialName) {
    IFileSystem* fs = GetFileSystem();
    if (!fs) return "";

    std::string vmtPath = "materials/" + materialName + ".vmt";
    FileHandle_t file = fs->Open(vmtPath.c_str(), "rb", "GAME");
    if (!file) {
        vmtPath = "materials/" + materialName;
        file = fs->Open(vmtPath.c_str(), "rb", "GAME");
        if (!file) return "";
    }

    int fileSize = fs->Size(file);
    if (fileSize <= 0 || fileSize > 65536) { fs->Close(file); return ""; }

    std::string content;
    content.resize(fileSize);
    int bytesRead = fs->Read(&content[0], fileSize, file);
    fs->Close(file);
    if (bytesRead <= 0) return "";

    // The shader name is the first non-empty, non-comment token (before the opening brace)
    std::istringstream ss(content);
    std::string line;
    while (std::getline(ss, line)) {
        // Trim leading whitespace
        size_t start = line.find_first_not_of(" \t\r\n");
        if (start == std::string::npos) continue;
        line = line.substr(start);
        if (line.substr(0, 2) == "//") continue;
        // Strip surrounding quotes
        if (!line.empty() && line.front() == '"') {
            size_t end = line.find('"', 1);
            if (end != std::string::npos) {
                std::string name = line.substr(1, end - 1);
                std::transform(name.begin(), name.end(), name.begin(), SafeToLower);
                return name;
            }
        }
        // Unquoted shader name — take up to first whitespace or brace
        size_t end = line.find_first_of(" \t\r\n{");
        std::string name = (end == std::string::npos) ? line : line.substr(0, end);
        std::transform(name.begin(), name.end(), name.begin(), SafeToLower);
        return name;
    }
    return "";
}

// =========================================================================
// Main Interface Implementation
// =========================================================================

void Initialize(remix::Interface* remix) {
    std::lock_guard<std::recursive_mutex> lock(s_mutex);
    if (s_initialized) return;
    
    s_remix = remix;
    s_config = Config();
    s_hashToCategoryFlags.clear();
    s_materialToCategoryFlags.clear();
    s_pendingCategories.clear();
    s_worldTextureNames.clear();
    s_stats = Stats();
    s_initialized = true;
    
    Msg("[MaterialPipeline::AutoCategorisation] Initialized\n");
}

void Shutdown() {
    std::lock_guard<std::recursive_mutex> lock(s_mutex);
    
    // Release texture references
    for (auto& pending : s_pendingCategories) {
        if (pending.texture) {
            pending.texture->Release();
        }
    }
    s_pendingCategories.clear();
    s_hashToCategoryFlags.clear();
    s_materialToCategoryFlags.clear();
    s_worldTextureNames.clear();
    s_forceAlbedoEmissiveMaterials.clear();
    ClearForceAlbedoHashesCpp();
    s_remix = nullptr;
    s_initialized = false;
    
    Msg("[MaterialPipeline::AutoCategorisation] Shutdown\n");
}

void ClearMapState() {
    std::lock_guard<std::recursive_mutex> lock(s_mutex);

    for (auto& pending : s_pendingCategories) {
        if (pending.texture) {
            pending.texture->Release();
        }
    }
    s_pendingCategories.clear();
    s_materialToCategoryFlags.clear();
    s_worldTextureNames.clear();
    s_forceAlbedoEmissiveMaterials.clear();
    ClearForceAlbedoHashesCpp();
    s_stats.pendingCategories = 0;
}

void SetConfig(const Config& config) {
    std::lock_guard<std::recursive_mutex> lock(s_mutex);
    s_config = config;
}

const Config& GetConfig() {
    return s_config;
}

void SetEnabled(bool enabled) {
    std::lock_guard<std::recursive_mutex> lock(s_mutex);
    s_config.enabled = enabled;
}

void SetParticleCategorisation(bool enabled) {
    std::lock_guard<std::recursive_mutex> lock(s_mutex);
    s_config.particleEnabled = enabled;
}

void SetDecalCategorisation(bool enabled) {
    std::lock_guard<std::recursive_mutex> lock(s_mutex);
    s_config.decalEnabled = enabled;
}

void SetEmissiveCategorisation(bool enabled) {
    std::lock_guard<std::recursive_mutex> lock(s_mutex);
    s_config.emissiveEnabled = enabled;
}

void SetDebugOutput(bool enabled) {
    std::lock_guard<std::recursive_mutex> lock(s_mutex);
    s_config.debugOutput = enabled;
}

// =========================================================================
// Material Detection
// =========================================================================

uint32_t DetectCategory(const std::string& materialName, IMaterial* material) {
    std::lock_guard<std::recursive_mutex> lock(s_mutex);
    if (!s_config.enabled) return 0;
    if (materialName.empty()) return 0;

    std::string lowerName = materialName;
    std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(), SafeToLower);

    // Engine placeholders are not stable material identities. In particular,
    // engine/preloadtexture can be bound while Source uploads many unrelated
    // textures on the first launch after an addon install. Categorizing that
    // UnlitGeneric placeholder would incorrectly mark every uploaded hash as
    // emissive.
    if (lowerName.rfind("__", 0) == 0 ||
        lowerName.rfind("engine/", 0) == 0) {
        return 0;
    }

    std::string shaderName = GetShaderName(material);
    std::transform(shaderName.begin(), shaderName.end(), shaderName.begin(), SafeToLower);
    if (shaderName.empty()) {
        shaderName = ReadVMTShaderName(materialName);
    }
    const bool isUnlitGeneric =
        shaderName.find("unlitgeneric") == 0;
    const bool isUnlitTwoTexture =
        shaderName.find("unlittwotexture") == 0;
    const bool isUnlitShader =
        isUnlitGeneric || isUnlitTwoTexture;
    bool hasUnlitEmissionOptOut = false;
    bool hasRawUnlitVertexColor = false;
    if (isUnlitShader) {
        // MATERIAL_VAR_NO_DEBUG_OVERRIDE is Source's compiled representation
        // of $no_fullbright. Vertex color/alpha are independent opt-outs:
        // either one is enough to suppress forced emission. Non-additive
        // translucency also identifies alpha-blended foliage and similar
        // materials that must not receive forced albedo emission.
        bool hasRawUnlitTranslucency = false;
        bool hasRawUnlitAdditive = false;
        const bool hasRawUnlitEmissionOptOut =
            CheckVMTForUnlitEmissionOptOut(
                materialName,
                s_config.debugOutput,
                &hasRawUnlitVertexColor,
                &hasRawUnlitTranslucency,
                &hasRawUnlitAdditive);
        const bool hasMaterialTranslucency =
            material &&
            material->GetMaterialVarFlag(MATERIAL_VAR_TRANSLUCENT);
        const bool hasMaterialAdditive =
            material &&
            material->GetMaterialVarFlag(MATERIAL_VAR_ADDITIVE);
        const bool hasNonAdditiveTranslucency =
            (hasRawUnlitTranslucency || hasMaterialTranslucency) &&
            !(hasRawUnlitAdditive || hasMaterialAdditive);
        hasUnlitEmissionOptOut =
            (material &&
             (material->GetMaterialVarFlag(MATERIAL_VAR_NO_DEBUG_OVERRIDE) ||
              material->GetMaterialVarFlag(MATERIAL_VAR_VERTEXALPHA) ||
              material->GetMaterialVarFlag(MATERIAL_VAR_VERTEXCOLOR))) ||
            hasRawUnlitEmissionOptOut ||
            hasNonAdditiveTranslucency;
    }
    const bool hasUnlitVertexColor =
        isUnlitGeneric &&
        (hasRawUnlitVertexColor ||
         (material &&
          material->GetMaterialVarFlag(MATERIAL_VAR_VERTEXCOLOR)));
    const bool isUnlitEmissive =
        isUnlitShader && !hasUnlitEmissionOptOut;

    if (hasUnlitEmissionOptOut) {
        s_forceAlbedoEmissiveMaterials.erase(materialName);
        if (s_config.debugOutput) {
            Msg("[AutoCategorisation] Suppressing unlit emission for '%s': VMT opt-out flag\n",
                materialName.c_str());
        }
    }
    
    uint32_t flags = 0;
    
    // === PRIORITY 1: PARTICLES ===
    if (s_config.particleEnabled) {
        bool isParticle = false;
        if (lowerName.find("particles/") == 0 || 
            lowerName.find("particle/") == 0 ||
            lowerName.find("effects/") == 0 || 
            lowerName.find("sprites/") == 0 ||
            lowerName.find("/particles/") != std::string::npos ||
            lowerName.find("/particle/") != std::string::npos ||
            lowerName.find("/effects/") != std::string::npos ||
            lowerName.find("/sprites/") != std::string::npos) {
            isParticle = true;
        }
        
        // A raw VMT can still identify this case when no IMaterial is available.
        if (!isParticle && isUnlitGeneric && hasUnlitVertexColor) {
            isParticle = true;
        }

        // Shader-based particle detection
        if (!isParticle && material) {
            // Check for particle shaders
            // NOTE: Use prefix matching (== 0) to handle DX version suffixes (e.g., Sprite_dx6, Cable_dx6, Modulate_dx6)
            // This also naturally excludes DecalModulate since "decalmodulate" doesn't start with "modulate"
            bool isParticleShader = (shaderName.find("sprite") == 0 ||
                                     shaderName.find("cable") == 0 ||
                                     shaderName.find("modulate") == 0);
            
            if (isParticleShader) {
                isParticle = true;
            }
            
            // $vertexcolor alone identifies a vertex-colored UnlitGeneric
            // particle; $vertexalpha is optional.
            if (!isParticle && isUnlitGeneric) {
                bool foundVC = false;
                IMaterialVar* pVC = material->FindVar("$vertexcolor", &foundVC, false);
                if (hasUnlitVertexColor ||
                    (foundVC && pVC && pVC->GetIntValue() == 1)) {
                    isParticle = true;
                }
            }
        }

        if (isParticle) {
            flags |= CategoryFlags::PARTICLE;
            s_stats.particlesCategorized++;

            // Preserve the established particle priority for ordinary shaders,
            // but an unlit particle is still inherently emissive and must carry
            // both category bits.
            if (!isUnlitEmissive || !s_config.emissiveEnabled) {
                s_stats.materialsScanned++;
                return flags;
            }
        }
    }
    
    // === PRIORITY 2: DECALS ===
    if (s_config.decalEnabled && !(flags & CategoryFlags::PARTICLE)) {
        bool isDecal = false;
        
        // Method 1: Check $decal VMT parameter
        if (material && !material->IsErrorMaterial()) {
            bool found = false;
            IMaterialVar* pDecalVar = material->FindVar("$decal", &found, false);
            if (found && pDecalVar && pDecalVar->GetIntValue() == 1) {
                isDecal = true;
            }
        }
        
        // Method 2: Shader-based detection (Decal, DecalModulate shaders)
        // Note: Runtime shaders have DX version suffixes (e.g., DecalModulate_dx6)
        if (!isDecal && material) {
            // Check for decal shaders using prefix match to handle DX suffixes
            // - DecalModulate, DecalModulate_dx6, etc.
            // - Decal, Decal_dx6, etc. (but not DecalModulate which starts with "decal" too)
            if (shaderName.find("decalmodulate") == 0 ||
                (shaderName.find("decal") == 0 && shaderName.find("modulate") == std::string::npos)) {
                isDecal = true;
            }
        }
        
        // Method 3: Path-based detection (exclude light materials)
        if (!isDecal &&
            ((lowerName.find("decals/") == 0 ||
              lowerName.find("/decals/") != std::string::npos ||
              lowerName.find("overlay") != std::string::npos ||
              lowerName.find("bulleth") != std::string::npos ||
              lowerName.find("_blood") != std::string::npos ||
              lowerName.find("blood_") != std::string::npos ||
              lowerName.find("/blood") != std::string::npos ||
              lowerName.find("scorch") != std::string::npos) &&
             lowerName.find("light") == std::string::npos &&
             lowerName.find("/lights/") == std::string::npos &&
             lowerName.find("lights/") != 0)) {
            isDecal = true;
        }
        
        // Method 4: Check if in world texture list from BSP
        if (!isDecal && IsWorldTexture(materialName)) {
            isDecal = true;
        }
        
        if (isDecal) {
            flags = CategoryFlags::DECAL_STATIC;
            s_stats.decalsCategorized++;
        }
    }
    
    // === EMISSIVE CHECK (can combine with other categories) ===
    if (s_config.emissiveEnabled && !hasUnlitEmissionOptOut) {
        bool isDecal = ((flags & CategoryFlags::DECAL_STATIC) != 0);
        // Source's unlit shaders do not receive scene lighting. Their complete
        // albedo is emission unless an explicit VMT flag opts out.
        bool isEmissive = isUnlitEmissive;
        if (isUnlitEmissive) {
            s_forceAlbedoEmissiveMaterials.insert(materialName);
        }
        
        // Method 1: Check IMaterial::FindVar for $selfillum
        if (!isDecal && !isEmissive && material) {
            bool found = false;
            IMaterialVar* pVar = material->FindVar("$selfillum", &found, false);
            if (found && pVar && pVar->GetIntValue() == 1) {
                isEmissive = true;
            }
            
            // Method 2: Check $emissive var (vector)
            if (!isEmissive) {
                bool foundEmissive = false;
                IMaterialVar* pEmissiveVar = material->FindVar("$emissive", &foundEmissive, false);
                if (foundEmissive && pEmissiveVar) {
                    const float* val = pEmissiveVar->GetVecValue();
                    if (val && (val[0] > 0.0f || val[1] > 0.0f || val[2] > 0.0f)) {
                        isEmissive = true;
                    }
                }
            }
        }
        
        // Method 3: Read VMT file directly (fallback when material pointer is unavailable)
        if (!isDecal && !isEmissive) {
            if (CheckVMTForSelfillum(materialName, s_config.debugOutput)) {
                isEmissive = true;
            }
        }
        
        // Method 4: Keyword-based detection
        if (!isDecal && !isEmissive) {
            bool hasOnSuffix = (lowerName.find("_on") != std::string::npos);
            bool isKnownEmissivePack = (lowerName.find("pkvoidplaces") != std::string::npos ||
                                        lowerName.find("/pb_") != std::string::npos ||
                                        lowerName.find("pb_") == 0);
            bool hasLightOn = (lowerName.find("light") != std::string::npos && 
                              lowerName.find("_on") != std::string::npos);
            
            if ((hasOnSuffix && isKnownEmissivePack) || hasLightOn) {
                isEmissive = true;
            }
        }
        
        if (isEmissive) {
            flags |= CategoryFlags::EMISSIVE;
            s_stats.emissivesCategorized++;
        }
    }
    
    s_stats.materialsScanned++;
    return flags;
}

uint32_t DetectAndApply(const std::string& materialName, 
                        IMaterial* material,
                        IDirect3DTexture9* texture) {
    if (!s_remix || !texture) return 0;
    
    uint32_t flags = DetectCategory(materialName, material);
    if (flags == 0) return 0;
    
    {
        std::lock_guard<std::recursive_mutex> lock(s_mutex);
        s_materialToCategoryFlags[materialName] = flags;
    }
    
    // Get hash
    auto result = s_remix->dxvk_GetTextureHash(texture);
    if (!result) return 0;
    
    uint64_t hash = result.value();
    
    if (hash == 0) {
        // Add to pending queue
        AddPendingCategory(texture, materialName, flags);
        return flags;
    }
    
    // Apply category
    ApplyToHash(hash, flags, materialName);
    return flags;
}

uint32_t DetectAndApplyAllVariants(const std::string& materialName, 
                                    IMaterial* material,
                                    const std::vector<IDirect3DTexture9*>* textureVariants) {
    if (!s_remix || !textureVariants || textureVariants->empty()) return 0;
    
    uint32_t flags = DetectCategory(materialName, material);
    if (flags == 0) return 0;
    
    {
        std::lock_guard<std::recursive_mutex> lock(s_mutex);
        s_materialToCategoryFlags[materialName] = flags;
    }
    
    bool anyApplied = false;
    int pendingCount = 0;
    
    for (IDirect3DTexture9* texture : *textureVariants) {
        if (!texture) continue;
        
        auto result = s_remix->dxvk_GetTextureHash(texture);
        if (!result) continue;
        
        uint64_t hash = result.value();
        
        if (hash != 0) {
            ApplyToHash(hash, flags, materialName);
            anyApplied = true;
        } else {
            AddPendingCategory(texture, materialName, flags);
            pendingCount++;
        }
    }
    
    if (s_config.debugOutput && (anyApplied || pendingCount > 0)) {
        Msg("[AutoCategorisation] Applied flags 0x%X to '%s' (%zu variants, %d applied, %d pending)\n", 
            flags, materialName.c_str(), textureVariants->size(), 
            anyApplied ? (int)textureVariants->size() - pendingCount : 0, pendingCount);
    }
    
    return flags;
}

bool ApplyKnownCategoryToTexture(const std::string& materialName, IDirect3DTexture9* texture) {
    if (!s_remix || !texture) return false;
    
    uint32_t flags = 0;
    {
        std::lock_guard<std::recursive_mutex> lock(s_mutex);
        auto it = s_materialToCategoryFlags.find(materialName);
        if (it == s_materialToCategoryFlags.end() || it->second == 0) return false;
        flags = it->second;
    }
    
    auto result = s_remix->dxvk_GetTextureHash(texture);
    if (!result) return false;
    
    uint64_t hash = result.value();
    
    if (hash != 0) {
        ApplyToHash(hash, flags, materialName);
        if (s_config.debugOutput) {
            Msg("[AutoCategorisation] Applied stored flags 0x%X to new variant of '%s' (hash 0x%llX)\n",
                flags, materialName.c_str(), hash);
        }
        return true;
    } else {
        AddPendingCategory(texture, materialName, flags);
        return false;
    }
}

void ApplyToHash(uint64_t hash, uint32_t flags, const std::string& materialName) {
    if (!s_remix || hash == 0 || flags == 0) return;

    // Snapshot tracker state before taking the AutoCategorisation lock. The
    // render hook uses the opposite direction (tracker -> AutoCategorisation),
    // so holding both locks here would allow a worker/render-thread deadlock.
    auto& tracker = D3D9TextureTracker::Instance();
    uint32_t existingTracker = 0;
    tracker.GetHashCategoryFlags(hash, &existingTracker);
    const bool hasBSPWorldMaterial =
        (flags & CategoryFlags::PARTICLE) &&
        tracker.IsAnyBSPWorldMaterialForHash(hash);
    const bool blockDecal =
        (flags & CategoryFlags::DECAL_STATIC) &&
        (tracker.IsHashContested(hash) ||
         tracker.HasMaterialNotInWorldListForHash(hash));

    {
        std::lock_guard<std::recursive_mutex> lock(s_mutex);

        // Retrieve existing flags from both maps for priority decisions.
        uint32_t existingInternal = 0;
        auto existingIt = s_hashToCategoryFlags.find(hash);
        if (existingIt != s_hashToCategoryFlags.end()) {
            existingInternal = existingIt->second;
        }

        uint32_t existingAny = existingInternal | existingTracker;
    
        // DECAL_STATIC (world geometry) takes priority over PARTICLE.
        // Check both the category flag maps and the BSP world material reverse map so
        // that translucent world brushes (intentionally left uncategorized) are also
        // protected from hash collisions with particle/sprite effects.
        if (flags & CategoryFlags::PARTICLE) {
            const bool isWorldGeometry =
                (existingAny & CategoryFlags::DECAL_STATIC) != 0 ||
                hasBSPWorldMaterial;
            if (isWorldGeometry) {
                flags &= ~CategoryFlags::PARTICLE;
                if (s_config.debugOutput) {
                    Msg("[AutoCategorisation] Skipping PARTICLE for hash 0x%llX (%s): world geometry\n",
                        hash, materialName.c_str());
                }
                if (flags == 0) return;
            }
        }

        // DECAL_STATIC must not be applied when any material sharing this hash is not
        // in the world-texture decal list.  This covers:
        //   • non-BSP model textures (HasNonBSPWorldMaterialForHash)
        //   • BSP brushes excluded from DECAL_STATIC, e.g. translucent/nodecal surfaces
        //     (HasMaterialNotInWorldListForHash, which checks the world texture list directly)
        //   • hashes already resolved as contested retroactively (IsHashContested)
        if ((flags & CategoryFlags::DECAL_STATIC) && blockDecal) {
            flags &= ~CategoryFlags::DECAL_STATIC;
            if (s_config.debugOutput) {
                Msg("[AutoCategorisation] Skipping DECAL_STATIC for hash 0x%llX (%s): hash shared with non-decal material\n",
                    hash, materialName.c_str());
            }
            if (flags == 0) return;
        }
    
        // Convert hash to string format for Remix API
        std::string hashStr = HashToString(hash);
    
        // When DECAL_STATIC (world geometry) is being applied, remove any prior
        // PARTICLE tag that may have been set before the BSP scan completed.
        if ((flags & CategoryFlags::DECAL_STATIC) && (existingAny & CategoryFlags::PARTICLE)) {
            s_remix->RemoveTextureHash("rtx.particleTextures", hashStr.c_str());
            if (s_config.debugOutput) {
                Msg("[AutoCategorisation] Removing PARTICLE for hash 0x%llX (%s): overridden by world geometry\n",
                    hash, materialName.c_str());
            }
        }
    
        // Store mapping without discarding an existing particle bit when a
        // second pass only adds EMISSIVE. DECAL_STATIC is the one category
        // that deliberately supersedes PARTICLE.
        uint32_t mergedFlags = existingInternal | flags;
        if (flags & CategoryFlags::DECAL_STATIC) {
            mergedFlags &= ~CategoryFlags::PARTICLE;
        }
        s_hashToCategoryFlags[hash] = mergedFlags;
    
        // Apply to Remix API
        if (flags & CategoryFlags::PARTICLE) {
            s_remix->AddTextureHash("rtx.particleTextures", hashStr.c_str());
        }
        if (flags & CategoryFlags::DECAL_STATIC) {
            s_remix->AddTextureHash("rtx.decalTextures", hashStr.c_str());
        }
        if (flags & CategoryFlags::EMISSIVE) {
            s_remix->AddTextureHash("rtx.legacyEmissiveTextures", hashStr.c_str());
            // Source unlit shaders emit their full albedo. Register the hash in
            // the shared force-albedo registry so Remix does the same.
            if (s_forceAlbedoEmissiveMaterials.count(materialName)) {
                AddForceAlbedoHashCpp(hash);
                if (s_config.debugOutput) {
                    Msg("[AutoCategorisation] Registered force-albedo for unlit emissive hash 0x%llX (%s)\n",
                        hash, materialName.c_str());
                }
            }
        }

        if (s_config.debugOutput) {
            Msg("[AutoCategorisation] Applied flags 0x%X to hash 0x%llX (%s)\n",
                flags, hash, materialName.c_str());
        }
    }

    // Reconciliation may query the tracker, so run it after releasing s_mutex.
    ReconcileHashCategories(hash);
}

void ReconcileHashCategories(uint64_t hash) {
    if (!s_remix || hash == 0) return;

    auto& tracker = D3D9TextureTracker::Instance();
    uint32_t trackerFlags = 0;
    tracker.GetHashCategoryFlags(hash, &trackerFlags);
    const auto materialNames = tracker.GetMaterialsForHash(hash);
    if (materialNames.empty()) {
        return;
    }

    bool clearTrackerEmissive = false;
    {
        std::lock_guard<std::recursive_mutex> lock(s_mutex);

        uint32_t internalFlags = 0;
        auto internalIt = s_hashToCategoryFlags.find(hash);
        if (internalIt != s_hashToCategoryFlags.end()) {
            internalFlags = internalIt->second;
        }
        if (!((internalFlags | trackerFlags) & CategoryFlags::EMISSIVE)) {
            return;
        }

        std::string conflictingMaterial;
        for (const auto& materialName : materialNames) {
            IMaterial* material = nullptr;
            if (materials) {
                material = materials->FindMaterial(
                    materialName.c_str(), TEXTURE_GROUP_OTHER, false);
                if (material && material->IsErrorMaterial()) {
                    material = nullptr;
                }
            }

            // Validation must describe the material, independent of whether the
            // user currently enabled automatic emissive assignment.
            const Config savedConfig = s_config;
            s_config.enabled = true;
            s_config.emissiveEnabled = true;
            const uint32_t detectedFlags =
                DetectCategory(materialName, material);
            s_config = savedConfig;
            if (!(detectedFlags & CategoryFlags::EMISSIVE)) {
                conflictingMaterial = materialName;
                break;
            }
        }

        if (conflictingMaterial.empty()) {
            return;
        }

        const std::string hashStr = HashToString(hash);
        s_remix->RemoveTextureHash(
            "rtx.legacyEmissiveTextures", hashStr.c_str());

        if (internalIt != s_hashToCategoryFlags.end()) {
            internalIt->second &= ~CategoryFlags::EMISSIVE;
            if (internalIt->second == 0) {
                s_hashToCategoryFlags.erase(internalIt);
            }
        }
        clearTrackerEmissive =
            (trackerFlags & CategoryFlags::EMISSIVE) != 0;

        RemoveForceAlbedoHashCpp(hash);

        if (s_config.debugOutput) {
            Msg("[AutoCategorisation] Removed EMISSIVE from contested hash "
                "0x%llX: material '%s' sharing the hash is not emissive\n",
                hash, conflictingMaterial.c_str());
        }
    }

    // Keep tracker and AutoCategorisation lock acquisition disjoint.
    if (clearTrackerEmissive) {
        tracker.SetHashCategoryFlags(
            hash, trackerFlags & ~CategoryFlags::EMISSIVE);
    }
}

// =========================================================================
// World Textures
// =========================================================================

void SetWorldTextureNames(const std::vector<std::string>& textureNames) {
    std::lock_guard<std::recursive_mutex> lock(s_mutex);
    
    s_worldTextureNames.clear();
    for (const auto& name : textureNames) {
        std::string lower = name;
        std::transform(lower.begin(), lower.end(), lower.begin(), SafeToLower);
        s_worldTextureNames.insert(lower);
    }
    
    if (s_config.debugOutput) {
        Msg("[AutoCategorisation] Set %zu world texture names\n", s_worldTextureNames.size());
    }
}

void ClearWorldTextureNames() {
    std::lock_guard<std::recursive_mutex> lock(s_mutex);
    s_worldTextureNames.clear();
}

bool IsWorldTexture(const std::string& materialName) {
    std::string lower = materialName;
    std::transform(lower.begin(), lower.end(), lower.begin(), SafeToLower);
    return s_worldTextureNames.count(lower) > 0;
}

int RecheckWorldTextures() {
    // TODO: Implement
    return 0;
}

// =========================================================================
// Hash-to-Category Mapping
// =========================================================================

void SetHashCategoryFlags(uint64_t hash, uint32_t flags) {
    {
        std::lock_guard<std::recursive_mutex> lock(s_mutex);
        s_hashToCategoryFlags[hash] = flags;
    }
    
    // Apply after releasing s_mutex because ApplyToHash snapshots tracker state.
    if (s_remix && hash != 0 && flags != 0) {
        ApplyToHash(hash, flags, "manual");
    }
}

void RemoveHashCategoryFlags(uint64_t hash) {
    std::lock_guard<std::recursive_mutex> lock(s_mutex);
    s_hashToCategoryFlags.erase(hash);
}

void ClearHashCategoryMappings() {
    std::lock_guard<std::recursive_mutex> lock(s_mutex);
    s_hashToCategoryFlags.clear();
    s_materialToCategoryFlags.clear();
}

bool GetHashCategoryFlags(uint64_t hash, uint32_t* outFlags) {
    std::lock_guard<std::recursive_mutex> lock(s_mutex);
    auto it = s_hashToCategoryFlags.find(hash);
    if (it != s_hashToCategoryFlags.end()) {
        if (outFlags) *outFlags = it->second;
        return true;
    }
    return false;
}

void UnapplyFromRemix(uint64_t hash, const std::string& materialName) {
    std::lock_guard<std::recursive_mutex> lock(s_mutex);
    if (!s_remix || hash == 0) return;

    auto it = s_hashToCategoryFlags.find(hash);
    if (it == s_hashToCategoryFlags.end()) return;

    uint32_t flags = it->second;
    std::string hashStr = HashToString(hash);

    if (flags & CategoryFlags::PARTICLE) {
        s_remix->RemoveTextureHash("rtx.particleTextures", hashStr.c_str());
    }
    if (flags & CategoryFlags::DECAL_STATIC) {
        s_remix->RemoveTextureHash("rtx.decalTextures", hashStr.c_str());
    }
    if (flags & CategoryFlags::EMISSIVE) {
        s_remix->RemoveTextureHash("rtx.legacyEmissiveTextures", hashStr.c_str());
    }

    s_hashToCategoryFlags.erase(it);
    s_materialToCategoryFlags.erase(materialName);

    RemoveForceAlbedoHashCpp(hash);

    if (s_config.debugOutput) {
        Msg("[AutoCategorisation] UnapplyFromRemix: removed hash %s (flags 0x%X) for material '%s'\n",
            hashStr.c_str(), flags, materialName.c_str());
    }
}

// =========================================================================
// Pending Categories
// =========================================================================

void AddPendingCategory(IDirect3DTexture9* texture, 
                        const std::string& materialName,
                        uint32_t flags) {
    std::lock_guard<std::recursive_mutex> lock(s_mutex);
    
    // Check if already pending
    for (const auto& pending : s_pendingCategories) {
        if (pending.texture == texture) return;
    }
    
    texture->AddRef();
    PendingCategory pending;
    pending.texture = texture;
    pending.materialName = materialName;
    pending.categoryFlags = flags;
    s_pendingCategories.push_back(pending);
    s_stats.pendingCategories++;
}

int RetryPendingCategories() {
    if (!s_remix) return 0;

    struct ReadyCategory {
        PendingCategory pending;
        uint64_t hash;
    };
    std::vector<ReadyCategory> ready;
    bool debugOutput = false;

    {
        std::lock_guard<std::recursive_mutex> lock(s_mutex);
        auto it = s_pendingCategories.begin();

        while (it != s_pendingCategories.end()) {
            auto result = s_remix->dxvk_GetTextureHash(it->texture);
            if (result && result.value() != 0) {
                // Move the queue's owned COM reference into the local work list.
                ready.push_back({*it, result.value()});
                it = s_pendingCategories.erase(it);
            } else {
                ++it;
            }
        }

        s_stats.pendingCategories = static_cast<int>(s_pendingCategories.size());
        debugOutput = s_config.debugOutput;
    }

    // Apply outside s_mutex. ApplyToHash reads tracker state, while the render
    // hook can call into AutoCategorisation with the tracker lock already held.
    for (const auto& item : ready) {
        ApplyToHash(
            item.hash,
            item.pending.categoryFlags,
            item.pending.materialName);

        if (debugOutput) {
            Msg("[AutoCategorisation] Retry succeeded for '%s' -> hash 0x%llX, flags 0x%X\n",
                item.pending.materialName.c_str(),
                item.hash,
                item.pending.categoryFlags);
        }

        item.pending.texture->Release();
    }

    return static_cast<int>(ready.size());
}

size_t GetPendingCount() {
    std::lock_guard<std::recursive_mutex> lock(s_mutex);
    return s_pendingCategories.size();
}

// =========================================================================
// Re-scanning
// =========================================================================

int RescanAllMaterials() {
    // TODO: Implement - would need access to texture cache
    return 0;
}

// =========================================================================
// Hash Export/Import
// =========================================================================

std::unordered_map<std::string, std::vector<uint64_t>> GetCategorizedHashes() {
    std::lock_guard<std::recursive_mutex> lock(s_mutex);
    
    std::unordered_map<std::string, std::vector<uint64_t>> result;
    
    // Group hashes by category flags
    for (const auto& pair : s_hashToCategoryFlags) {
        uint64_t hash = pair.first;
        uint32_t flags = pair.second;
        
        if (flags & CategoryFlags::PARTICLE) {
            result["particleTextures"].push_back(hash);
        }
        if (flags & CategoryFlags::DECAL_STATIC) {
            result["decalTextures"].push_back(hash);
        }
        if (flags & CategoryFlags::EMISSIVE) {
            result["legacyEmissiveTextures"].push_back(hash);
        }
        if (flags & CategoryFlags::BEAM) {
            result["beamTextures"].push_back(hash);
        }
    }
    
    return result;
}

int ExportHashesToFile(const std::string& filepath) {
    std::lock_guard<std::recursive_mutex> lock(s_mutex);
    
    if (s_hashToCategoryFlags.empty()) {
        Msg("[AutoCategorisation] No hashes to export\n");
        return 0;
    }
    
    // Get categorized hashes
    auto categorized = GetCategorizedHashes();
    
    // Open file for writing
    FILE* file = fopen(filepath.c_str(), "w");
    if (!file) {
        Warning("[AutoCategorisation] Failed to open file for writing: %s\n", filepath.c_str());
        return -1;
    }
    
    int totalExported = 0;
    
    // Write header
    fprintf(file, "# Auto-categorized texture hashes\n");
    fprintf(file, "# Generated by Garry's Mod RTX Remix Auto-Categorisation\n\n");
    
    // Write each category
    for (const auto& pair : categorized) {
        const std::string& categoryName = pair.first;
        const std::vector<uint64_t>& hashes = pair.second;
        
        if (hashes.empty()) continue;
        
        fprintf(file, "rtx.%s = ", categoryName.c_str());
        
        for (size_t i = 0; i < hashes.size(); i++) {
            fprintf(file, "0x%llX", (unsigned long long)hashes[i]);
            if (i < hashes.size() - 1) {
                fprintf(file, ", ");
            }
        }
        
        fprintf(file, "\n");
        totalExported += static_cast<int>(hashes.size());
    }
    
    fclose(file);
    
    Msg("[AutoCategorisation] Exported %d hashes to %s\n", totalExported, filepath.c_str());
    return totalExported;
}

// =========================================================================
// Statistics
// =========================================================================

Stats GetStats() {
    std::lock_guard<std::recursive_mutex> lock(s_mutex);
    return s_stats;
}

} // namespace AutoCategorisation
} // namespace MaterialPipeline

// =========================================================================
// Lua Bindings - Must be at global scope
// =========================================================================

LUA_FUNCTION(AutoCat_SetEnabled) {
    bool enabled = LUA->GetBool(1);
    MaterialPipeline::AutoCategorisation::SetEnabled(enabled);
    return 0;
}

LUA_FUNCTION(AutoCat_SetParticleCategorisation) {
    bool enabled = LUA->GetBool(1);
    MaterialPipeline::AutoCategorisation::SetParticleCategorisation(enabled);
    return 0;
}

LUA_FUNCTION(AutoCat_SetDecalCategorisation) {
    bool enabled = LUA->GetBool(1);
    MaterialPipeline::AutoCategorisation::SetDecalCategorisation(enabled);
    return 0;
}

LUA_FUNCTION(AutoCat_SetEmissiveCategorisation) {
    bool enabled = LUA->GetBool(1);
    MaterialPipeline::AutoCategorisation::SetEmissiveCategorisation(enabled);
    return 0;
}

LUA_FUNCTION(AutoCat_SetCategoryFlags) {
    uint64_t hash = 0;
    if (LUA->IsType(1, GarrysMod::Lua::Type::String)) {
        const char* hashStr = LUA->GetString(1);
        hash = std::strtoull(hashStr, nullptr, 0);
    } else {
        hash = static_cast<uint64_t>(LUA->GetNumber(1));
    }
    
    uint32_t flags = static_cast<uint32_t>(LUA->GetNumber(2));
    MaterialPipeline::AutoCategorisation::SetHashCategoryFlags(hash, flags);
    return 0;
}

LUA_FUNCTION(AutoCat_GetCategoryFlags) {
    uint64_t hash = 0;
    if (LUA->IsType(1, GarrysMod::Lua::Type::String)) {
        const char* hashStr = LUA->GetString(1);
        hash = std::strtoull(hashStr, nullptr, 0);
    } else {
        hash = static_cast<uint64_t>(LUA->GetNumber(1));
    }
    
    uint32_t flags = 0;
    MaterialPipeline::AutoCategorisation::GetHashCategoryFlags(hash, &flags);
    LUA->PushNumber(flags);
    return 1;
}

LUA_FUNCTION(AutoCat_RetryPending) {
    int count = MaterialPipeline::AutoCategorisation::RetryPendingCategories();
    LUA->PushNumber(count);
    return 1;
}

LUA_FUNCTION(AutoCat_GetStats) {
    MaterialPipeline::AutoCategorisation::Stats stats = MaterialPipeline::AutoCategorisation::GetStats();
    
    LUA->CreateTable();
    
    LUA->PushNumber(stats.materialsScanned);
    LUA->SetField(-2, "materialsScanned");
    
    LUA->PushNumber(stats.particlesCategorized);
    LUA->SetField(-2, "particlesCategorized");
    
    LUA->PushNumber(stats.decalsCategorized);
    LUA->SetField(-2, "decalsCategorized");
    
    LUA->PushNumber(stats.emissivesCategorized);
    LUA->SetField(-2, "emissivesCategorized");
    
    LUA->PushNumber(stats.pendingCategories);
    LUA->SetField(-2, "pendingCategories");
    
    return 1;
}

LUA_FUNCTION(AutoCat_ExportHashes) {
    const char* filepath = LUA->GetString(1);
    int count = MaterialPipeline::AutoCategorisation::ExportHashesToFile(filepath);
    LUA->PushNumber(count);
    return 1;
}

namespace MaterialPipeline {
namespace AutoCategorisation {

void RegisterLuaBindings(GarrysMod::Lua::ILuaBase* LUA) {
    // Get or create MaterialPipeline table
    LUA->PushSpecial(GarrysMod::Lua::SPECIAL_GLOB);
    LUA->GetField(-1, "MaterialPipeline");
    
    if (LUA->IsType(-1, GarrysMod::Lua::Type::Nil)) {
        LUA->Pop(); // pop nil
        LUA->CreateTable();
        LUA->SetField(-2, "MaterialPipeline");
        LUA->GetField(-1, "MaterialPipeline");
    }
    
    // Create AutoCategorisation sub-table
    LUA->CreateTable();
    
    LUA->PushCFunction(AutoCat_SetEnabled);
    LUA->SetField(-2, "SetEnabled");
    
    LUA->PushCFunction(AutoCat_SetParticleCategorisation);
    LUA->SetField(-2, "SetParticleCategorisation");
    
    LUA->PushCFunction(AutoCat_SetDecalCategorisation);
    LUA->SetField(-2, "SetDecalCategorisation");
    
    LUA->PushCFunction(AutoCat_SetEmissiveCategorisation);
    LUA->SetField(-2, "SetEmissiveCategorisation");
    
    LUA->PushCFunction(AutoCat_SetCategoryFlags);
    LUA->SetField(-2, "SetCategoryFlags");
    
    LUA->PushCFunction(AutoCat_GetCategoryFlags);
    LUA->SetField(-2, "GetCategoryFlags");
    
    LUA->PushCFunction(AutoCat_RetryPending);
    LUA->SetField(-2, "RetryPending");
    
    LUA->PushCFunction(AutoCat_GetStats);
    LUA->SetField(-2, "GetStats");
    
    LUA->PushCFunction(AutoCat_ExportHashes);
    LUA->SetField(-2, "ExportHashes");
    
    // Set AutoCategorisation table into MaterialPipeline
    LUA->SetField(-2, "AutoCategorisation");
    
    // Pop MaterialPipeline and _G
    LUA->Pop(2);
    
    Msg("[MaterialPipeline::AutoCategorisation] Lua bindings registered\n");
}

} // namespace AutoCategorisation
} // namespace MaterialPipeline

#endif // _WIN64
