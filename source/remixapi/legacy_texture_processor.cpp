#ifdef _WIN64

#include "remixapi.h"
#include "legacy_texture_processor_formats.h"
#include "legacy_texture_processor_vtf.h"
#include "legacy_texture_processor_vmt.h"
#include "legacy_texture_processor_texgen.h"
#include "legacy_texture_processor_usda.h"
#include <tier0/dbg.h>
#include <materialsystem/imaterialsystem.h>
#include <materialsystem/imaterial.h>
#include <materialsystem/imaterialvar.h>
#include <materialsystem/itexture.h>
#include <filesystem.h>
#include <d3d9.h>
#include <Windows.h>
#include "../d3d9_texture_tracker.h"
#include "legacy_texture_processor.h"

#include <algorithm>
#include <cstring>
#include <cmath>
#include <fstream>
#include <sstream>
#include <direct.h>  // For _mkdir on Windows

// External globals
extern IMaterialSystem* materials;
extern remix::Interface* g_remix;

namespace LegacyTextureProcessor {

// PBR conversion constants
constexpr float MAX_PHONG_EXPONENT = 150.0f;  // Typical max in Source Engine

// DDS format constants
constexpr uint32_t DDS_MAGIC = 0x20534444;  // "DDS "
constexpr uint32_t DDSD_CAPS = 0x1;
constexpr uint32_t DDSD_HEIGHT = 0x2;
constexpr uint32_t DDSD_WIDTH = 0x4;
constexpr uint32_t DDSD_PIXELFORMAT = 0x1000;
constexpr uint32_t DDSD_MIPMAPCOUNT = 0x20000;
constexpr uint32_t DDSD_LINEARSIZE = 0x80000;
constexpr uint32_t DDPF_ALPHAPIXELS = 0x1;
constexpr uint32_t DDPF_RGB = 0x40;
constexpr uint32_t DDSCAPS_TEXTURE = 0x1000;
constexpr uint32_t DDSCAPS_MIPMAP = 0x400000;
constexpr uint32_t DDSCAPS_COMPLEX = 0x8;

// DDS header structures
#pragma pack(push, 1)
struct DDSPixelFormat {
    uint32_t size;
    uint32_t flags;
    uint32_t fourCC;
    uint32_t rgbBitCount;
    uint32_t rBitMask;
    uint32_t gBitMask;
    uint32_t bBitMask;
    uint32_t aBitMask;
};

struct DDSHeader {
    uint32_t magic;
    uint32_t size;
    uint32_t flags;
    uint32_t height;
    uint32_t width;
    uint32_t pitchOrLinearSize;
    uint32_t depth;
    uint32_t mipMapCount;
    uint32_t reserved1[11];
    DDSPixelFormat pixelFormat;
    uint32_t caps;
    uint32_t caps2;
    uint32_t caps3;
    uint32_t caps4;
    uint32_t reserved2;
};
#pragma pack(pop)

// Static filesystem pointer
static IFileSystem* s_pFileSystem = nullptr;

// Get filesystem interface
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

//=============================================================================
// TextureProcessor Implementation
//=============================================================================

TextureProcessor& TextureProcessor::Instance() {
    static TextureProcessor instance;
    return instance;
}

TextureProcessor::TextureProcessor()
    : m_remixInterface(nullptr)
    , m_fileSystem(nullptr)
    , m_initialized(false)
    , m_autoProcessing(true)
    , m_debugOutput(false)
    , m_metallicGenerationEnabled(false)  // Disabled by default - experimental feature
    , m_autoDiscoverEnabled(true)         // Enabled by default - helps find unreferenced textures
    , m_parseCommentedPropertiesEnabled(false)  // Disabled by default - respects VMT comments
    , m_needsUSDAUpdate(false)
    , m_lastKnownMaterialCount(0)
    , m_allMaterialsProcessed(false)
    , m_workerRunning(false)
    , m_shutdownRequested(false)
    , m_backgroundProcessing(false)
    , m_lastProcessedCount(0) {
    m_stats = {};
}

TextureProcessor::~TextureProcessor() {
    Shutdown();
}

bool TextureProcessor::Initialize(remix::Interface* remixInterface) {
    if (m_initialized) {
        // Already initialized - this is success, not failure
        return true;
    }
    
    if (!remixInterface) {
        Warning("[LegacyTextureProcessor] Invalid Remix interface\n");
        return false;
    }
    
    m_remixInterface = remixInterface;
    m_fileSystem = GetFileSystemInterface();
    
    if (!m_fileSystem) {
        Warning("[LegacyTextureProcessor] Could not get filesystem interface\n");
        return false;
    }
    
    // Set default output directory for generated textures
    // The executable is at bin/win64/gmod.exe, we need to go up to the game root
    // Output should be: GarrysModWithRTXAgain/rtx-remix/mods/gmod_topbr/textures/
    char gamePath[MAX_PATH];
    if (GetModuleFileNameA(NULL, gamePath, MAX_PATH)) {
        std::string gameDir(gamePath);
        // Remove executable name (gmod.exe)
        size_t lastSlash = gameDir.find_last_of("\\/");
        if (lastSlash != std::string::npos) {
            gameDir = gameDir.substr(0, lastSlash);
            // Remove "win64" directory
            lastSlash = gameDir.find_last_of("\\/");
            if (lastSlash != std::string::npos) {
                gameDir = gameDir.substr(0, lastSlash);
                // Remove "bin" directory to get to game root
                lastSlash = gameDir.find_last_of("\\/");
                if (lastSlash != std::string::npos) {
                    gameDir = gameDir.substr(0, lastSlash);
                }
            }
            m_outputDirectory = gameDir + "\\rtx-remix\\mods\\gmod_topbr\\textures";
        }
    }
    
    m_initialized = true;
    Msg("[LegacyTextureProcessor] Initialized successfully\n");
    Msg("[LegacyTextureProcessor] Output directory: %s\n", m_outputDirectory.c_str());
    return true;
}

void TextureProcessor::Shutdown() {
    if (!m_initialized) return;
    
    // Stop background worker thread first
    StopWorkerThread();
    
    // Destroy all uploaded textures
    for (auto& pair : m_textureHandles) {
        if (m_remixInterface && pair.second) {
            m_remixInterface->DestroyTexture(pair.second);
        }
    }
    m_textureHandles.clear();
    
    m_uploadedTextures.clear();
    m_processedMaterials.clear();
    m_processedMaterialInfo.clear();
    m_writtenTexturePaths.clear();
    m_materialsWrittenToUSDA.clear();
    
    // Clear queue
    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        while (!m_materialQueue.empty()) m_materialQueue.pop();
        m_queuedMaterials.clear();
    }
    
    // Clear pending USDA
    {
        std::lock_guard<std::mutex> lock(m_pendingUSDAMutex);
        m_pendingUSDAMaterials.clear();
    }
    
    m_remixInterface = nullptr;
    m_initialized = false;
    
    Msg("[LegacyTextureProcessor] Shutdown complete\n");
}

void TextureProcessor::SetOutputDirectory(const std::string& path) {
    m_outputDirectory = path;
    if (m_debugOutput) {
        Msg("[LegacyTextureProcessor] Output directory set to: %s\n", path.c_str());
    }
}

IFileSystem* TextureProcessor::GetFileSystem() {
    return m_fileSystem;
}

bool TextureProcessor::EnsureOutputDirectory() {
    if (m_outputDirectory.empty()) {
        Warning("[LegacyTextureProcessor] Output directory not set\n");
        return false;
    }
    
    // Create directory hierarchy
    std::string path = m_outputDirectory;
    
    // Replace forward slashes with backslashes for Windows
    for (char& c : path) {
        if (c == '/') c = '\\';
    }
    
    // Create each directory level
    size_t pos = 0;
    while ((pos = path.find('\\', pos + 1)) != std::string::npos) {
        std::string subPath = path.substr(0, pos);
        _mkdir(subPath.c_str());
    }
    _mkdir(path.c_str());
    
    // Check if directory exists now
    DWORD attrib = GetFileAttributesA(m_outputDirectory.c_str());
    if (attrib == INVALID_FILE_ATTRIBUTES || !(attrib & FILE_ATTRIBUTE_DIRECTORY)) {
        Warning("[LegacyTextureProcessor] Failed to create output directory: %s\n", m_outputDirectory.c_str());
        return false;
    }
    
    return true;
}

std::string TextureProcessor::GenerateOutputPath(uint64_t hash, const std::string& suffix) {
    std::ostringstream oss;
    oss << m_outputDirectory << "\\" << std::hex << std::uppercase << hash << suffix << ".dds";
    return oss.str();
}

// Calculate number of mipmap levels for a given dimension
static uint32_t CalculateMipLevels(uint32_t width, uint32_t height) {
    uint32_t levels = 1;
    uint32_t size = max(width, height);
    while (size > 1) {
        size /= 2;
        levels++;
    }
    return levels;
}

bool TextureProcessor::WriteDDSHeader(std::ofstream& file, uint32_t width, uint32_t height, bool hasAlpha, uint32_t mipCount) {
    return TextureGen::WriteDDSHeader(file, width, height, hasAlpha, mipCount, m_debugOutput);
}

bool TextureProcessor::WriteTextureToDDS(const ConvertedTexture& texture, const std::string& outputPath) {
    return TextureGen::WriteTextureToDDS(texture, outputPath, m_debugOutput);
}

bool TextureProcessor::GenerateRoughnessTexture(const MaterialPBRProperties& props, ConvertedTexture& outTexture) {
    return TextureGen::GenerateRoughnessTexture(props, outTexture, m_fileSystem, m_debugOutput);
}

bool TextureProcessor::GenerateMetallicTexture(const MaterialPBRProperties& props, ConvertedTexture& outTexture) {
    return TextureGen::GenerateMetallicTexture(props, outTexture, m_fileSystem, m_debugOutput);
}

bool TextureProcessor::ReadVTFFile(const std::string& path, std::vector<uint8_t>& outData) {
    if (!m_fileSystem) {
        Warning("[LegacyTextureProcessor] Filesystem not available\n");
        return false;
    }
    // Delegate to VTF module
    return VTF::ReadVTFFile(m_fileSystem, path, outData, m_debugOutput);
}

bool TextureProcessor::ParseVTFHeader(const std::vector<uint8_t>& fileData, VTFFileHeader& outHeader) {
    // Delegate to VTF module
    return VTF::ParseVTFHeader(fileData, outHeader, m_debugOutput);
}

// DXT decompression - delegate to VTF module
bool TextureProcessor::DecompressDXT1(const uint8_t* compressedData, uint32_t width, uint32_t height,
                                          std::vector<uint8_t>& outRGBA) {
    return VTF::DecompressDXT1(compressedData, width, height, outRGBA);
}

bool TextureProcessor::DecompressDXT5(const uint8_t* compressedData, uint32_t width, uint32_t height,
                                          std::vector<uint8_t>& outRGBA) {
    return VTF::DecompressDXT5(compressedData, width, height, outRGBA);
}

bool TextureProcessor::ExtractVTFPixelData(const std::vector<uint8_t>& fileData, 
                                               const VTFFileHeader& header,
                                               ConvertedTexture& outTexture, 
                                               bool isNormalMap) {
    // Delegate to VTF module
    return VTF::ExtractVTFPixelData(fileData, header, outTexture, isNormalMap, m_debugOutput);
}

void TextureProcessor::ConvertNormalMapToOctahedral(ConvertedTexture& texture) {
    // Delegate to VTF module
    VTF::ConvertNormalMapToOctahedral(texture, m_debugOutput);
}

void TextureProcessor::ConvertSSBumpToNormal(ConvertedTexture& texture) {
    // Delegate to VTF module
    VTF::ConvertSSBumpToNormal(texture, m_debugOutput);
}

uint64_t TextureProcessor::GenerateTextureHash(const std::string& path, uint32_t width, uint32_t height) {
    // Simple FNV-1a hash
    uint64_t hash = 14695981039346656037ULL;
    
    for (char c : path) {
        hash ^= static_cast<uint64_t>(c);
        hash *= 1099511628211ULL;
    }
    
    // Mix in dimensions
    hash ^= width;
    hash *= 1099511628211ULL;
    hash ^= height;
    hash *= 1099511628211ULL;
    
    // Ensure it's not 0
    if (hash == 0) hash = 1;
    
    return hash;
}

bool TextureProcessor::UploadTextureToRemix(const ConvertedTexture& texture, 
                                                remixapi_TextureHandle* outHandle) {
    if (!m_remixInterface) {
        Warning("[LegacyTextureProcessor] Remix interface not available\n");
        return false;
    }
    
    remixapi_TextureInfo texInfo = {};
    texInfo.sType = REMIXAPI_STRUCT_TYPE_TEXTURE_INFO;
    texInfo.pNext = nullptr;
    texInfo.hash = texture.hash;
    texInfo.width = texture.width;
    texInfo.height = texture.height;
    texInfo.depth = 1;
    texInfo.mipLevels = texture.mipLevels;
    texInfo.format = texture.format;
    texInfo.data = texture.pixelData.data();
    texInfo.dataSize = texture.pixelData.size();
    
    auto result = m_remixInterface->CreateTexture(texInfo);
    if (!result) {
        Warning("[LegacyTextureProcessor] Failed to create texture: error %d\n", result.status());
        return false;
    }
    
    if (outHandle) {
        *outHandle = result.value();
    }
    
    if (m_debugOutput) {
        Msg("[LegacyTextureProcessor] Uploaded texture: %dx%d, hash 0x%llX\n", 
            texture.width, texture.height, texture.hash);
    }
    
    return true;
}

uint64_t TextureProcessor::ConvertAndUploadTexture(const std::string& vtfPath, bool isNormalMap) {
    std::lock_guard<std::mutex> lock(m_mutex);
    
    // Check cache
    auto it = m_uploadedTextures.find(vtfPath);
    if (it != m_uploadedTextures.end()) {
        return it->second;
    }
    
    // Read VTF file
    std::vector<uint8_t> fileData;
    if (!ReadVTFFile(vtfPath, fileData)) {
        return 0;
    }
    
    // Parse header
    VTFFileHeader header;
    if (!ParseVTFHeader(fileData, header)) {
        return 0;
    }
    
    // Extract pixel data
    ConvertedTexture texture;
    texture.sourcePath = vtfPath;
    if (!ExtractVTFPixelData(fileData, header, texture, isNormalMap)) {
        return 0;
    }
    
    // Generate hash
    texture.hash = GenerateTextureHash(vtfPath, texture.width, texture.height);
    
    // Upload to Remix
    remixapi_TextureHandle handle = nullptr;
    if (!UploadTextureToRemix(texture, &handle)) {
        m_stats.failedConversions++;
        return 0;
    }
    
    // Cache the results
    m_uploadedTextures[vtfPath] = texture.hash;
    m_textureHandles[texture.hash] = handle;
    m_stats.texturesUploaded++;
    
    return texture.hash;
}

float TextureProcessor::PhongToRoughness(float phongExponent) {
    // Default to max roughness (most Source materials without phong are fully matte photoscans)
    if (phongExponent <= 0) return 1.0f;
    
    // Clamp to reasonable range
    phongExponent = std::clamp(phongExponent, 1.0f, 512.0f);
    
    // Standard formula to convert Phong exponent to PBR roughness:
    // roughness = sqrt(2 / (phongExponent + 2))
    // 
    // This is derived from the relationship between Phong specular and PBR GGX:
    // phongExponent 1   -> roughness ~0.82 (very broad highlight)
    // phongExponent 10  -> roughness ~0.41 (moderate)
    // phongExponent 25  -> roughness ~0.27 (fairly smooth)
    // phongExponent 50  -> roughness ~0.19 (smooth, like glossy plastic)
    // phongExponent 150 -> roughness ~0.11 (very smooth, like polished metal)
    // phongExponent 256 -> roughness ~0.09 (highly glossy)
    
    float roughness = sqrtf(2.0f / (phongExponent + 2.0f));
    
    // Clamp minimum to avoid perfectly mirror-like reflections (which can look broken)
    // and maximum to ensure some specular response for phong materials
    return std::clamp(roughness, 0.05f, 0.90f);
}

float TextureProcessor::CalculateRoughness(const MaterialPBRProperties& props) {
    // Start with roughness from phong exponent (defaults to fairly rough)
    float roughness = PhongToRoughness(props.phongExponent);
    
    // For materials without phong enabled, check if they have $envmap
    // This is common for LightmappedGeneric brushes (floors, walls, etc.)
    if (!props.hasPhong) {
        if (props.hasEnvMap) {
            // Material has $envmap, so it should be reflective
            // Base roughness depends on envmap tint
            if (props.hasEnvMapTint) {
                float tintIntensity = (props.envMapTint[0] + props.envMapTint[1] + props.envMapTint[2]) / 3.0f;
                // tintIntensity 1.0 (full) -> roughness 0.30 (shiny)
                // tintIntensity 0.5 (half) -> roughness 0.50 (semi-shiny)
                // tintIntensity 0.25 (quarter) -> roughness 0.60 (moderate)
                // tintIntensity 0.0 (none) -> roughness 0.75 (matte)
                roughness = 0.75f - (tintIntensity * 0.45f);
            } else {
                // Has envmap but no tint specified - assume moderate reflectivity
                roughness = 0.50f;
            }
            
            // If material has envmap mask, it will use per-pixel roughness later
            // Here we just set a reasonable constant for the USDA fallback
            if (props.hasEnvMapMask) {
                // Will use texture, but constant should be moderate
                roughness = min(roughness, 0.50f);
            }
            
            // NEW: If we detected metallic from dark base texture, use much lower roughness
            // Metallic materials like chrome need very low roughness to look right
            if (props.hasBaseTextureBrightness && props.baseTextureBrightness < 0.20f) {
                // Very dark base texture = polished metal = low roughness
                // brightness 0.0 -> roughness 0.05 (perfect chrome)
                // brightness 0.1 -> roughness 0.10 (polished metal)
                // brightness 0.2 -> roughness 0.15 (brushed metal)
                roughness = 0.05f + (props.baseTextureBrightness * 0.50f);
                if (m_debugOutput) {
                    Msg("[LegacyTextureProcessor] %s: Metallic roughness from dark texture: brightness=%.3f -> roughness=%.2f\n",
                        props.materialName.c_str(), props.baseTextureBrightness, roughness);
                }
            }
            
            return std::clamp(roughness, 0.05f, 0.75f);
        }
        // No phong and no envmap - just a matte photoscanned surface
        return 1.0f;
    }
    
    // If there's an envmap with tint, the tint controls reflection intensity
    // LOWER tint = LESS reflective = HIGHER roughness
    if (props.hasEnvMapTint && props.hasEnvMap) {
        float tintIntensity = (props.envMapTint[0] + props.envMapTint[1] + props.envMapTint[2]) / 3.0f;
        
        // Low tint means the material reflects less, so increase roughness a bit
        // But don't completely override the phong-based roughness
        if (tintIntensity < 0.5f) {
            roughness = roughness + (0.5f - tintIntensity) * 0.3f;
        }
        roughness = std::clamp(roughness, 0.30f, 0.85f);
    }
    
    // Phong boost affects highlight brightness
    // Higher boost suggests intentionally shiny material - DECREASE roughness slightly
    if (props.phongBoost > 1.0f) {
        // phongBoost 2 -> small decrease
        // phongBoost 5 -> moderate decrease
        // phongBoost 10 -> capped decrease (don't go below 0.30)
        float boostFactor = min((props.phongBoost - 1.0f) * 0.03f, 0.15f);
        roughness = max(0.30f, roughness - boostFactor);
    }
    
    // NEW: Rim lighting affects perceived glossiness
    // Materials with rim lighting typically have shiny edges, suggesting lower roughness overall
    if (props.hasRimLight) {
        // Rim lighting active - material is shinier
        // rimlightboost 0.5 -> small decrease
        // rimlightboost 1.0 -> moderate decrease
        // rimlightboost 2.0+ -> significant decrease
        float rimFactor = 0.05f;  // Base rim factor
        if (props.hasRimLightBoost && props.rimLightBoost > 0.5f) {
            rimFactor = min((props.rimLightBoost - 0.5f) * 0.04f + 0.05f, 0.15f);
        }
        roughness = max(0.30f, roughness - rimFactor);
        if (m_debugOutput) {
            Msg("[LegacyTextureProcessor] %s: Rim light adjustment: rimFactor=%.2f, roughness=%.2f\n",
                props.materialName.c_str(), rimFactor, roughness);
        }
    }
    
    // NEW: $envmapcontrast affects perceived glossiness
    // Higher contrast means sharper reflections = lower roughness
    if (props.hasEnvMapContrast && props.envMapContrast > 0.0f) {
        float contrastFactor = min(props.envMapContrast * 0.05f, 0.10f);
        roughness = max(0.25f, roughness - contrastFactor);
    }
    
    // NEW: $phongalbedotint with high boost suggests color-tinted metal-like reflections
    if (props.phongAlbedoTint && props.hasPhongAlbedoBoost && props.phongAlbedoBoost > 1.0f) {
        float albedoBoostFactor = min((props.phongAlbedoBoost - 1.0f) * 0.02f, 0.08f);
        roughness = max(0.30f, roughness - albedoBoostFactor);
    }
    
    return std::clamp(roughness, 0.30f, 0.85f);
}

float TextureProcessor::EstimateMetallic(const MaterialPBRProperties& props) {
    float metallic = 0.0f;
    
    // NEW: Experimental metallic detection from base texture brightness
    // This feature is DISABLED by default because:
    // - In Source Engine: black texture + envmap = chrome look (envmap provides reflections)
    // - In PBR: metallic = 1 means "use base color as reflection color", so black = no reflections
    // The correct approach for most Source Engine materials is to use low roughness, not metallic.
    //
    // Enable with rtx_topbr_metallic 1 for experimentation
    if (m_metallicGenerationEnabled && props.hasEnvMap && props.hasBaseTextureBrightness) {
        // Brightness threshold for metallic detection:
        // Very dark textures (brightness < 0.1) with strong envmap = highly metallic
        // The metallic value decreases as brightness increases
        // At brightness 0.3+, we consider it non-metallic (just reflective)
        if (props.baseTextureBrightness < 0.30f) {
            // Inverse relationship: darker = more metallic
            // brightness 0.0 -> metallic 1.0
            // brightness 0.1 -> metallic 0.8
            // brightness 0.3 -> metallic 0.0
            metallic = std::clamp(1.0f - (props.baseTextureBrightness / 0.30f), 0.0f, 1.0f);
            
            // Scale by envmap tint intensity if available (brighter envmap = more reflective)
            if (props.hasEnvMapTint) {
                float envmapIntensity = (props.envMapTint[0] + props.envMapTint[1] + props.envMapTint[2]) / 3.0f;
                metallic *= envmapIntensity;
            }
            
            if (m_debugOutput) {
                Msg("[LegacyTextureProcessor] %s: [EXPERIMENTAL] Metallic from base texture darkness: brightness=%.3f -> metallic=%.2f\n",
                    props.materialName.c_str(), props.baseTextureBrightness, metallic);
            }
        }
    }
    
    // High phong boost suggests metal-like reflections (fallback, only if metallic generation enabled)
    if (m_metallicGenerationEnabled && props.phongBoost > 2.0f) {
        float phongMetallic = std::clamp((props.phongBoost - 2.0f) / 8.0f, 0.0f, 0.5f);
        metallic = max(metallic, phongMetallic);
    }
    
    // Having an envmap mask suggests reflective surface (only if metallic generation enabled)
    if (m_metallicGenerationEnabled && props.hasEnvMapMask) {
        metallic = max(metallic, 0.2f);
    }
    
    return metallic;
}

// Analyze base texture brightness to detect metallic materials
// Black textures + envmap = metallic, grey/colored textures = non-metallic
bool TextureProcessor::AnalyzeBaseTextureBrightness(const std::string& texturePath, float& outBrightness) {
    if (texturePath.empty()) {
        return false;
    }
    
    // Read the VTF file
    std::vector<uint8_t> fileData;
    if (!ReadVTFFile(texturePath, fileData)) {
        if (m_debugOutput) {
            Msg("[LegacyTextureProcessor] Failed to read base texture for brightness analysis: %s\n", texturePath.c_str());
        }
        return false;
    }
    
    VTFFileHeader header;
    if (!ParseVTFHeader(fileData, header)) {
        if (m_debugOutput) {
            Msg("[LegacyTextureProcessor] Failed to parse VTF header for brightness analysis: %s\n", texturePath.c_str());
        }
        return false;
    }
    
    ConvertedTexture sourceTex;
    if (!ExtractVTFPixelData(fileData, header, sourceTex, false)) {
        if (m_debugOutput) {
            Msg("[LegacyTextureProcessor] Failed to extract pixel data for brightness analysis: %s\n", texturePath.c_str());
        }
        return false;
    }
    
    // Calculate average brightness (luminance) from the texture
    // Using standard luminance weights: 0.299*R + 0.587*G + 0.114*B
    double totalLuminance = 0.0;
    size_t pixelCount = 0;
    
    for (size_t i = 0; i < sourceTex.pixelData.size(); i += 4) {
        float r = sourceTex.pixelData[i] / 255.0f;
        float g = sourceTex.pixelData[i + 1] / 255.0f;
        float b = sourceTex.pixelData[i + 2] / 255.0f;
        
        // Standard luminance calculation
        float luminance = 0.299f * r + 0.587f * g + 0.114f * b;
        totalLuminance += luminance;
        pixelCount++;
    }
    
    if (pixelCount == 0) {
        return false;
    }
    
    outBrightness = static_cast<float>(totalLuminance / pixelCount);
    
    if (m_debugOutput) {
        Msg("[LegacyTextureProcessor] %s: Base texture brightness analyzed: %.3f (pixels=%zu)\n",
            texturePath.c_str(), outBrightness, pixelCount);
    }
    
    return true;
}

// Discover companion textures that might not be explicitly referenced in the VMT
void TextureProcessor::DiscoverCompanionTextures(const std::string& baseTexturePath, MaterialPBRProperties& props) {
    TextureGen::DiscoverCompanionTextures(baseTexturePath, props, m_fileSystem, m_autoDiscoverEnabled, m_debugOutput);
}

// Helper function to check if a file exists
static bool FileExists(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    return file.good();
}

// VMT parsing is now in legacy_texture_processor_vmt.cpp
// Using VMTParser::VMTProperties and VMTParser::ParseVMTFile

    
    return true;
}

bool TextureProcessor::ExtractMaterialPBR(const std::string& materialName, 
                                              MaterialPBRProperties& outProps) {
    if (!materials) {
        Warning("[LegacyTextureProcessor] Material system not available\n");
        return false;
    }
    
    IMaterial* pMaterial = materials->FindMaterial(materialName.c_str(), TEXTURE_GROUP_OTHER, false);
    if (!pMaterial || pMaterial->IsErrorMaterial()) {
        if (m_debugOutput) {
            Msg("[LegacyTextureProcessor] Material not found: %s\n", materialName.c_str());
        }
        return false;
    }
    
    outProps.materialName = materialName;
    outProps.hasPhong = false;
    outProps.hasBumpMap = false;
    outProps.isSSBump = false;
    outProps.hasEnvMapMask = false;
    outProps.hasPhongExponentTexture = false;
    outProps.isSelfIllum = false;
    outProps.isTranslucent = false;
    outProps.phongExponent = 0;
    outProps.phongBoost = 1.0f;
    outProps.roughness = 1.0f;  // Default to max roughness (matte)
    outProps.metallic = 0.0f;   // Default to non-metallic
    
    // Extended properties
    outProps.normalMapAlphaEnvMapMask = false;
    outProps.hasPhongFresnelRanges = false;
    outProps.phongFresnelRanges[0] = 0.0f;
    outProps.phongFresnelRanges[1] = 0.0f;
    outProps.phongFresnelRanges[2] = 0.0f;
    outProps.hasEnvMapTint = false;
    outProps.envMapTint[0] = 1.0f;
    outProps.envMapTint[1] = 1.0f;
    outProps.envMapTint[2] = 1.0f;
    outProps.hasEnvMap = false;
    outProps.hasBaseMapAlphaPhongMask = false;
    outProps.baseMapAlphaPhongMask = 0.0f;
    outProps.hasBaseAlphaEnvMapMask = false;  // $basealphaenvmapmask for LightmappedGeneric
    
    // Glass properties
    outProps.isGlass = false;
    outProps.isRefractShader = false;
    outProps.shaderName = "";
    outProps.surfaceProp = "";
    outProps.refractTintTexturePath = "";
    
    // Metallic detection from base texture brightness
    outProps.baseTextureBrightness = 0.5f;  // Default to mid-grey (non-metallic)
    outProps.hasBaseTextureBrightness = false;
    
    // NEW: Initialize additional properties
    // Self-illumination / Emissive
    outProps.selfIllumMaskPath = "";
    outProps.hasSelfIllumMask = false;
    outProps.selfIllumTint[0] = outProps.selfIllumTint[1] = outProps.selfIllumTint[2] = 1.0f;
    outProps.hasSelfIllumTint = false;
    
    // Rim lighting
    outProps.hasRimLight = false;
    outProps.rimLightExponent = 4.0f;
    outProps.rimLightBoost = 1.0f;
    outProps.hasRimLightExponent = false;
    outProps.hasRimLightBoost = false;
    
    // Additional phong properties
    outProps.phongAlbedoTint = false;
    outProps.phongAlbedoBoost = 1.0f;
    outProps.hasPhongAlbedoBoost = false;
    outProps.phongTint[0] = outProps.phongTint[1] = outProps.phongTint[2] = 1.0f;
    outProps.hasPhongTint = false;
    
    // Parallax/heightmap
    outProps.parallaxMapPath = "";
    outProps.hasParallaxMap = false;
    outProps.parallaxMapScale = 0.05f;
    outProps.hasParallaxMapScale = false;
    
    // Additional envmap properties
    outProps.envMapContrast = 0.0f;
    outProps.hasEnvMapContrast = false;
    outProps.envMapSaturation = 1.0f;
    outProps.hasEnvMapSaturation = false;
    
    // Secondary envmap mask
    outProps.envMapMask2Path = "";
    outProps.hasEnvMapMask2 = false;
    
    // Auto-discovered companion textures
    outProps.discoveredNormalPath = "";
    outProps.hasDiscoveredNormal = false;
    outProps.discoveredHeightPath = "";
    outProps.hasDiscoveredHeight = false;
    outProps.discoveredMaskPath = "";
    outProps.hasDiscoveredMask = false;
    outProps.discoveredAOPath = "";
    outProps.hasDiscoveredAO = false;
    
    // ExoPBR community PBR format
    outProps.isExoPBR = false;
    outProps.armTexturePath = "";
    outProps.hasARMTexture = false;
    outProps.exoNormalPath = "";
    outProps.hasExoNormal = false;
    outProps.emissionTexturePath = "";
    outProps.hasEmissionTexture = false;
    outProps.emissionScale = 1.0f;
    outProps.hasEmissionScale = false;
    outProps.emissionTint[0] = outProps.emissionTint[1] = outProps.emissionTint[2] = 1.0f;
    outProps.hasEmissionTint = false;
    
    // GPBR (Strata Source) community PBR format
    outProps.isGPBR = false;
    outProps.mraoTexturePath = "";
    outProps.hasMRAOTexture = false;
    outProps.mraoScale = 1.0f;
    outProps.hasMRAOScale = false;
    outProps.gpbrEmissionPath = "";
    outProps.hasGPBREmission = false;
    outProps.gpbrEmissionScale = 1.0f;
    outProps.hasGPBREmissionScale = false;
    outProps.gpbrParallax = false;
    outProps.gpbrParallaxDepth = 0.1f;
    outProps.gpbrAlpha = 1.0f;
    outProps.hasGPBRAlpha = false;
    
    // BlueFlyTrap PseudoPBR format
    outProps.isBFTPseudoPBR = false;
    outProps.isBFTMetallicLayer = false;
    outProps.isBFTDiffuseLayer = false;
    outProps.bftExponentTexturePath = "";
    outProps.hasBFTExponentTexture = false;
    outProps.bftColor2[0] = 1.0f;
    outProps.bftColor2[1] = 1.0f;
    outProps.bftColor2[2] = 1.0f;
    outProps.hasBFTColor2 = false;
    
    // MWB PBR Gen format
    outProps.isMWBPBR = false;
    
    // Get the shader name
    const char* shaderName = pMaterial->GetShaderName();
    if (shaderName && shaderName[0] != '\0') {
        outProps.shaderName = shaderName;
    }
    
    // =========================================================================
    // IMPORTANT: Parse VMT file directly to get material properties
    // This is crucial because when running with DX6 fallback shaders (which Remix
    // forces), FindVar() returns incorrect or missing values for many properties.
    // We use VMT-parsed values as the primary source, falling back to FindVar
    // only when VMT parsing fails.
    // =========================================================================
    VMTParser::VMTProperties vmtParsed;
    bool hasVMTParsed = VMTParser::ParseVMTFile(m_fileSystem, materialName, vmtParsed, m_debugOutput, m_parseCommentedPropertiesEnabled);
    
    // If VMT parsing succeeded, prefer VMT shader name over the DX6 fallback name
    if (hasVMTParsed && !vmtParsed.shaderName.empty()) {
        if (m_debugOutput) {
            Msg("[LegacyTextureProcessor] %s: Using VMT shader '%s' instead of runtime '%s'\n", 
                materialName.c_str(), vmtParsed.shaderName.c_str(), outProps.shaderName.c_str());
        }
        outProps.shaderName = vmtParsed.shaderName;
    }
    
    // Helper lambda to check if a texture path is valid (not a placeholder/internal texture)
    auto IsValidTexturePath = [](const std::string& path) -> bool {
        if (path.empty()) return false;
        // Filter out RTX internal textures
        if (path.find("rtx/") == 0 || path.find("rtx\\") == 0) return false;
        // Filter out render targets and internal textures
        if (path.find("_rt_") != std::string::npos) return false;
        if (path.find("__") == 0) return false;  // Internal textures like __error
        // Filter out procedural textures
        if (path.find("env_cubemap") != std::string::npos) return false;
        // Filter out undefined/invalid texture markers (with or without angle brackets)
        if (path == "UNDEFINED" || path == "<UNDEFINED>") return false;
        if (path.find("UNDEFINED") != std::string::npos) return false;
        // Filter out error textures that Source Engine returns for missing/invalid textures
        if (path == "error" || path == "Error" || path == "ERROR") return false;
        // Filter out paths that are just numbers or very short
        if (path.length() < 3) return false;
        return true;
    };
    
    // Get $basetexture - prefer VMT-parsed value, fallback to FindVar
    if (hasVMTParsed && vmtParsed.hasBaseTexture && IsValidTexturePath(vmtParsed.baseTexture)) {
        outProps.baseTexturePath = vmtParsed.baseTexture;
        if (m_debugOutput) {
            Msg("[LegacyTextureProcessor] %s: $basetexture (from VMT) = %s\n", materialName.c_str(), outProps.baseTexturePath.c_str());
        }
    } else {
        // Fallback to FindVar
        bool found = false;
        IMaterialVar* pVar = pMaterial->FindVar("$basetexture", &found, false);
        if (found && pVar) {
            std::string texPath;
            const char* strVal = pVar->GetStringValue();
            if (strVal && strVal[0] != '\0') {
                texPath = strVal;
            }
            if (!IsValidTexturePath(texPath)) {
                ITexture* pTex = pVar->GetTextureValue();
                if (pTex) {
                    texPath = pTex->GetName();
                }
            }
            if (IsValidTexturePath(texPath)) {
                outProps.baseTexturePath = texPath;
            }
            if (m_debugOutput && !outProps.baseTexturePath.empty()) {
                Msg("[LegacyTextureProcessor] %s: $basetexture (from FindVar) = %s\n", materialName.c_str(), outProps.baseTexturePath.c_str());
            } else if (m_debugOutput && strVal) {
                Msg("[LegacyTextureProcessor] %s: $basetexture filtered (invalid path: '%s')\n", materialName.c_str(), strVal);
            }
        }
    }
    
    // Get $bumpmap - prefer VMT-parsed value, fallback to FindVar
    bool found = false;
    if (hasVMTParsed && vmtParsed.hasBumpMap && IsValidTexturePath(vmtParsed.bumpMap)) {
        outProps.bumpMapPath = vmtParsed.bumpMap;
        outProps.hasBumpMap = true;
        if (m_debugOutput) {
            Msg("[LegacyTextureProcessor] %s: $bumpmap (from VMT) = %s\n", materialName.c_str(), outProps.bumpMapPath.c_str());
        }
    } else {
        IMaterialVar* pVar = pMaterial->FindVar("$bumpmap", &found, false);
        if (found && pVar) {
            std::string texPath;
            const char* strVal = pVar->GetStringValue();
            if (strVal && strVal[0] != '\0') {
                texPath = strVal;
            }
            if (!IsValidTexturePath(texPath)) {
                ITexture* pTex = pVar->GetTextureValue();
                if (pTex) {
                    texPath = pTex->GetName();
                }
            }
            if (IsValidTexturePath(texPath)) {
                outProps.bumpMapPath = texPath;
                outProps.hasBumpMap = true;
            }
            if (m_debugOutput && outProps.hasBumpMap) {
                Msg("[LegacyTextureProcessor] %s: $bumpmap (from FindVar) = %s\n", materialName.c_str(), outProps.bumpMapPath.c_str());
            } else if (m_debugOutput && strVal) {
                Msg("[LegacyTextureProcessor] %s: $bumpmap filtered (invalid path: '%s')\n", materialName.c_str(), strVal);
            }
        }
    }
    
    // Check for $ssbump flag - prefer VMT-parsed value
    if (hasVMTParsed && vmtParsed.hasSSBump && vmtParsed.ssbump != 0) {
        outProps.isSSBump = true;
        if (m_debugOutput) {
            Msg("[LegacyTextureProcessor] %s: $ssbump = 1 (from VMT, will convert to normal map)\n", materialName.c_str());
        }
    } else {
        IMaterialVar* pVar = pMaterial->FindVar("$ssbump", &found, false);
        if (found && pVar) {
            int ssbumpValue = pVar->GetIntValue();
            if (ssbumpValue != 0) {
                outProps.isSSBump = true;
                if (m_debugOutput) {
                    Msg("[LegacyTextureProcessor] %s: $ssbump = 1 (will convert to normal map)\n", materialName.c_str());
                }
            }
        }
    }
    
    // Get $normalmap - prefer VMT-parsed value, only if we don't have $bumpmap
    if (!outProps.hasBumpMap) {
        if (hasVMTParsed && vmtParsed.hasNormalMap && IsValidTexturePath(vmtParsed.normalMap)) {
            outProps.bumpMapPath = vmtParsed.normalMap;
            outProps.hasBumpMap = true;
            if (m_debugOutput) {
                Msg("[LegacyTextureProcessor] %s: $normalmap (from VMT) = %s\n", materialName.c_str(), outProps.bumpMapPath.c_str());
            }
        } else {
            IMaterialVar* pVar = pMaterial->FindVar("$normalmap", &found, false);
            if (found && pVar) {
                std::string texPath;
                const char* strVal = pVar->GetStringValue();
                if (strVal && strVal[0] != '\0') {
                    texPath = strVal;
                }
                if (!IsValidTexturePath(texPath)) {
                    ITexture* pTex = pVar->GetTextureValue();
                    if (pTex) {
                        texPath = pTex->GetName();
                    }
                }
                if (IsValidTexturePath(texPath)) {
                    outProps.bumpMapPath = texPath;
                    outProps.hasBumpMap = true;
                }
                if (m_debugOutput && outProps.hasBumpMap) {
                    Msg("[LegacyTextureProcessor] %s: $normalmap = %s\n", materialName.c_str(), outProps.bumpMapPath.c_str());
                } else if (m_debugOutput && strVal) {
                    Msg("[LegacyTextureProcessor] %s: $normalmap filtered (invalid path: '%s')\n", materialName.c_str(), strVal);
                }
            }
        }
    }
    
    // Get $envmapmask - prefer VMT-parsed value
    if (hasVMTParsed && vmtParsed.hasEnvMapMask && IsValidTexturePath(vmtParsed.envMapMask)) {
        outProps.envMapMaskPath = vmtParsed.envMapMask;
        outProps.hasEnvMapMask = true;
        if (m_debugOutput) {
            Msg("[LegacyTextureProcessor] %s: $envmapmask (from VMT) = %s\n", materialName.c_str(), outProps.envMapMaskPath.c_str());
        }
    } else {
        IMaterialVar* pVar = pMaterial->FindVar("$envmapmask", &found, false);
        if (found && pVar) {
            std::string texPath;
            const char* strVal = pVar->GetStringValue();
            if (strVal && strVal[0] != '\0') {
                texPath = strVal;
            }
            if (!IsValidTexturePath(texPath)) {
                ITexture* pTex = pVar->GetTextureValue();
                if (pTex) {
                    texPath = pTex->GetName();
                }
            }
            if (IsValidTexturePath(texPath)) {
                outProps.envMapMaskPath = texPath;
                outProps.hasEnvMapMask = true;
            }
            if (m_debugOutput && outProps.hasEnvMapMask) {
                Msg("[LegacyTextureProcessor] %s: $envmapmask = %s\n", materialName.c_str(), outProps.envMapMaskPath.c_str());
            } else if (m_debugOutput && strVal) {
                Msg("[LegacyTextureProcessor] %s: $envmapmask filtered (invalid path: '%s')\n", materialName.c_str(), strVal);
            }
        } else if (m_debugOutput) {
            Msg("[LegacyTextureProcessor] %s: $envmapmask not found\n", materialName.c_str());
        }
    }
    
    // Get $envmap - prefer VMT-parsed value
    if (hasVMTParsed && vmtParsed.hasEnvMap) {
        outProps.hasEnvMap = true;
        if (m_debugOutput) {
            Msg("[LegacyTextureProcessor] %s: $envmap (from VMT) = %s\n", materialName.c_str(), vmtParsed.envMap.c_str());
        }
    } else {
        IMaterialVar* pVar = pMaterial->FindVar("$envmap", &found, false);
        if (found && pVar) {
            const char* strVal = pVar->GetStringValue();
            bool isUndefined = false;
            if (strVal) {
                if (strcmp(strVal, "UNDEFINED") == 0 || strcmp(strVal, "<UNDEFINED>") == 0) {
                    isUndefined = true;
                } else if (strstr(strVal, "UNDEFINED") != nullptr) {
                    isUndefined = true;
                }
            }
            if (strVal && strVal[0] != '\0' && !isUndefined) {
                outProps.hasEnvMap = true;
                if (m_debugOutput) {
                    Msg("[LegacyTextureProcessor] %s: $envmap = %s\n", materialName.c_str(), strVal);
                }
            } else if (m_debugOutput && strVal && isUndefined) {
                Msg("[LegacyTextureProcessor] %s: $envmap = %s (ignored)\n", materialName.c_str(), strVal);
            }
        }
    }
    
    // Get $normalmapalphaenvmapmask - prefer VMT-parsed value
    if (hasVMTParsed && vmtParsed.hasNormalMapAlphaEnvMapMask) {
        outProps.normalMapAlphaEnvMapMask = (vmtParsed.normalMapAlphaEnvMapMask != 0);
        if (m_debugOutput) {
            Msg("[LegacyTextureProcessor] %s: $normalmapalphaenvmapmask (from VMT) = %d\n", materialName.c_str(), vmtParsed.normalMapAlphaEnvMapMask);
        }
    } else {
        IMaterialVar* pVar = pMaterial->FindVar("$normalmapalphaenvmapmask", &found, false);
        if (found && pVar) {
            int intVal = pVar->GetIntValue();
            float floatVal = pVar->GetFloatValue();
            const char* strVal = pVar->GetStringValue();
            
            bool isTruthy = (intVal != 0) || (floatVal != 0.0f);
            if (!isTruthy && strVal && strVal[0] != '\0') {
                isTruthy = (atoi(strVal) != 0) || (atof(strVal) != 0.0);
            }
            
            outProps.normalMapAlphaEnvMapMask = isTruthy;
            if (m_debugOutput) {
                if (outProps.normalMapAlphaEnvMapMask) {
                    Msg("[LegacyTextureProcessor] %s: $normalmapalphaenvmapmask = 1 (will use normal alpha for roughness)\n", materialName.c_str());
                } else {
                    Msg("[LegacyTextureProcessor] %s: $normalmapalphaenvmapmask found but value = 0\n", materialName.c_str());
                }
            }
        } else if (m_debugOutput) {
            Msg("[LegacyTextureProcessor] %s: $normalmapalphaenvmapmask NOT FOUND\n", materialName.c_str());
        }
    }
    
    // Get $phong - prefer VMT-parsed value
    if (hasVMTParsed && vmtParsed.hasPhong) {
        outProps.hasPhong = (vmtParsed.phong != 0);
    } else {
        IMaterialVar* pVar = pMaterial->FindVar("$phong", &found, false);
        if (found && pVar) {
            outProps.hasPhong = (pVar->GetIntValue() == 1);
        }
    }
    
    // Get $phongexponent - prefer VMT-parsed value
    if (hasVMTParsed && vmtParsed.hasPhongExponent) {
        outProps.phongExponent = vmtParsed.phongExponent;
        if (!outProps.hasPhong) outProps.hasPhong = true;
    } else {
        IMaterialVar* pVar = pMaterial->FindVar("$phongexponent", &found, false);
        if (found && pVar) {
            outProps.phongExponent = pVar->GetFloatValue();
            if (!outProps.hasPhong) outProps.hasPhong = true;
        }
    }
    
    // Get $phongboost - prefer VMT-parsed value
    if (hasVMTParsed && vmtParsed.hasPhongBoost) {
        outProps.phongBoost = vmtParsed.phongBoost;
    } else {
        IMaterialVar* pVar = pMaterial->FindVar("$phongboost", &found, false);
        if (found && pVar) {
            outProps.phongBoost = pVar->GetFloatValue();
        }
    }
    
    // Get $phongexponenttexture - prefer VMT-parsed value
    if (hasVMTParsed && vmtParsed.hasPhongExponentTexture && IsValidTexturePath(vmtParsed.phongExponentTexture)) {
        outProps.phongExponentTexturePath = vmtParsed.phongExponentTexture;
        outProps.hasPhongExponentTexture = true;
        if (m_debugOutput) {
            Msg("[LegacyTextureProcessor] %s: $phongexponenttexture (from VMT) = %s\n", materialName.c_str(), outProps.phongExponentTexturePath.c_str());
        }
    } else {
        IMaterialVar* pVar = pMaterial->FindVar("$phongexponenttexture", &found, false);
        if (found && pVar) {
            std::string texPath;
            const char* strVal = pVar->GetStringValue();
            if (strVal && strVal[0] != '\0') {
                texPath = strVal;
                if (m_debugOutput) {
                    Msg("[LegacyTextureProcessor] %s: $phongexponenttexture raw string = '%s'\n", materialName.c_str(), strVal);
                }
            }
            if (!IsValidTexturePath(texPath)) {
                ITexture* pTex = pVar->GetTextureValue();
                if (pTex) {
                    texPath = pTex->GetName();
                    if (m_debugOutput) {
                        Msg("[LegacyTextureProcessor] %s: $phongexponenttexture from texture = '%s'\n", materialName.c_str(), texPath.c_str());
                    }
                }
            }
            if (IsValidTexturePath(texPath)) {
                outProps.phongExponentTexturePath = texPath;
                outProps.hasPhongExponentTexture = true;
                if (m_debugOutput) {
                    Msg("[LegacyTextureProcessor] %s: $phongexponenttexture = %s\n", materialName.c_str(), texPath.c_str());
                }
            } else if (m_debugOutput) {
                Msg("[LegacyTextureProcessor] %s: $phongexponenttexture FILTERED as invalid path: '%s'\n", materialName.c_str(), texPath.c_str());
            }
        } else if (m_debugOutput) {
            Msg("[LegacyTextureProcessor] %s: $phongexponenttexture not found\n", materialName.c_str());
        }
    }
    
    // Get $basemapalphaphongmask - prefer VMT-parsed value
    if (hasVMTParsed && vmtParsed.hasBaseMapAlphaPhongMask) {
        outProps.hasBaseMapAlphaPhongMask = (vmtParsed.baseMapAlphaPhongMask != 0);
        if (m_debugOutput && outProps.hasBaseMapAlphaPhongMask) {
            Msg("[LegacyTextureProcessor] %s: $basemapalphaphongmask (from VMT) = 1\n", materialName.c_str());
        }
    } else {
        IMaterialVar* pVar = pMaterial->FindVar("$basemapalphaphongmask", &found, false);
        if (found && pVar) {
            outProps.hasBaseMapAlphaPhongMask = (pVar->GetIntValue() == 1);
            if (m_debugOutput && outProps.hasBaseMapAlphaPhongMask) {
                Msg("[LegacyTextureProcessor] %s: $basemapalphaphongmask = 1\n", materialName.c_str());
            }
        }
    }
    
    // Get $basealphaenvmapmask - prefer VMT-parsed value
    if (hasVMTParsed && vmtParsed.hasBaseAlphaEnvMapMask) {
        outProps.hasBaseAlphaEnvMapMask = (vmtParsed.baseAlphaEnvMapMask != 0);
        if (m_debugOutput) {
            Msg("[LegacyTextureProcessor] %s: $basealphaenvmapmask (from VMT) = %d\n", materialName.c_str(), vmtParsed.baseAlphaEnvMapMask);
        }
    } else {
        IMaterialVar* pVar = pMaterial->FindVar("$basealphaenvmapmask", &found, false);
        if (found && pVar) {
            int intVal = pVar->GetIntValue();
            float floatVal = pVar->GetFloatValue();
            const char* strVal = pVar->GetStringValue();
            
            bool isTruthy = (intVal != 0) || (floatVal != 0.0f);
            if (!isTruthy && strVal && strVal[0] != '\0') {
                isTruthy = (atoi(strVal) != 0) || (atof(strVal) != 0.0);
            }
            
            outProps.hasBaseAlphaEnvMapMask = isTruthy;
            if (m_debugOutput) {
                Msg("[LegacyTextureProcessor] %s: $basealphaenvmapmask found (int=%d, float=%.2f, str='%s') -> %d\n", 
                    materialName.c_str(), intVal, floatVal, strVal ? strVal : "null", 
                    outProps.hasBaseAlphaEnvMapMask ? 1 : 0);
            }
        } else if (m_debugOutput) {
            Msg("[LegacyTextureProcessor] %s: $basealphaenvmapmask NOT FOUND\n", materialName.c_str());
        }
    }
    
    // Get $phongfresnelranges - prefer VMT-parsed value
    if (hasVMTParsed && vmtParsed.hasPhongFresnelRanges) {
        outProps.phongFresnelRanges[0] = vmtParsed.phongFresnelRanges[0];
        outProps.phongFresnelRanges[1] = vmtParsed.phongFresnelRanges[1];
        outProps.phongFresnelRanges[2] = vmtParsed.phongFresnelRanges[2];
        outProps.hasPhongFresnelRanges = true;
        if (m_debugOutput) {
            Msg("[LegacyTextureProcessor] %s: $phongfresnelranges (from VMT) = [%.2f %.2f %.2f]\n", 
                materialName.c_str(), outProps.phongFresnelRanges[0], outProps.phongFresnelRanges[1], outProps.phongFresnelRanges[2]);
        }
    } else {
        IMaterialVar* pVar = pMaterial->FindVar("$phongfresnelranges", &found, false);
        if (found && pVar) {
            const char* strVal = pVar->GetStringValue();
            if (strVal && strVal[0] != '\0') {
                float x = 0, y = 0, z = 0;
                if (sscanf(strVal, "[%f %f %f]", &x, &y, &z) == 3 ||
                    sscanf(strVal, "%f %f %f", &x, &y, &z) == 3) {
                    outProps.phongFresnelRanges[0] = x;
                    outProps.phongFresnelRanges[1] = y;
                    outProps.phongFresnelRanges[2] = z;
                    outProps.hasPhongFresnelRanges = true;
                    if (m_debugOutput) {
                        Msg("[LegacyTextureProcessor] %s: $phongfresnelranges = [%.2f %.2f %.2f]\n", 
                            materialName.c_str(), x, y, z);
                    }
                }
            }
        }
    }
    
    // Get $envmaptint - prefer VMT-parsed value
    if (hasVMTParsed && vmtParsed.hasEnvMapTint) {
        outProps.envMapTint[0] = vmtParsed.envMapTint[0];
        outProps.envMapTint[1] = vmtParsed.envMapTint[1];
        outProps.envMapTint[2] = vmtParsed.envMapTint[2];
        
        // Check if non-default
        bool isDefaultTint = (fabs(vmtParsed.envMapTint[0] - 1.0f) < 0.01f && 
                             fabs(vmtParsed.envMapTint[1] - 1.0f) < 0.01f && 
                             fabs(vmtParsed.envMapTint[2] - 1.0f) < 0.01f);
        if (!isDefaultTint) {
            outProps.hasEnvMapTint = true;
            if (!outProps.hasEnvMap) {
                outProps.hasEnvMap = true;
            }
        }
        if (m_debugOutput) {
            Msg("[LegacyTextureProcessor] %s: $envmaptint (from VMT) = [%.2f %.2f %.2f]%s\n", 
                materialName.c_str(), vmtParsed.envMapTint[0], vmtParsed.envMapTint[1], vmtParsed.envMapTint[2],
                isDefaultTint ? " (default)" : "");
        }
    } else {
        IMaterialVar* pVar = pMaterial->FindVar("$envmaptint", &found, false);
        if (found && pVar) {
            const char* strVal = pVar->GetStringValue();
            if (strVal && strVal[0] != '\0') {
                float r = 1, g = 1, b = 1;
                bool parsed = false;
                
                if (sscanf(strVal, "[%f %f %f]", &r, &g, &b) == 3 ||
                    sscanf(strVal, "%f %f %f", &r, &g, &b) == 3) {
                    parsed = true;
                } else if (sscanf(strVal, "%f", &r) == 1) {
                    g = r;
                    b = r;
                    parsed = true;
                }
                
                if (parsed) {
                    outProps.envMapTint[0] = r;
                    outProps.envMapTint[1] = g;
                    outProps.envMapTint[2] = b;
                    
                    bool isDefaultTint = (fabs(r - 1.0f) < 0.01f && fabs(g - 1.0f) < 0.01f && fabs(b - 1.0f) < 0.01f);
                    
                    if (!isDefaultTint) {
                        outProps.hasEnvMapTint = true;
                        if (!outProps.hasEnvMap) {
                            outProps.hasEnvMap = true;
                            if (m_debugOutput) {
                                Msg("[LegacyTextureProcessor] %s: Setting hasEnvMap=true based on non-default $envmaptint\n", materialName.c_str());
                            }
                        }
                    }
                    
                    if (m_debugOutput) {
                        Msg("[LegacyTextureProcessor] %s: $envmaptint = [%.2f %.2f %.2f]%s\n", 
                            materialName.c_str(), r, g, b, isDefaultTint ? " (default, ignoring)" : "");
                    }
                } else if (m_debugOutput) {
                    Msg("[LegacyTextureProcessor] %s: $envmaptint found but parse failed: '%s'\n", materialName.c_str(), strVal);
                }
            } else if (m_debugOutput) {
                Msg("[LegacyTextureProcessor] %s: $envmaptint found but empty value\n", materialName.c_str());
            }
        } else if (m_debugOutput) {
            Msg("[LegacyTextureProcessor] %s: $envmaptint not found by FindVar\n", materialName.c_str());
        }
    }
    
    // Get $selfillum - prefer VMT-parsed value
    if (hasVMTParsed && vmtParsed.hasSelfIllum) {
        outProps.isSelfIllum = (vmtParsed.selfIllum != 0);
    } else {
        IMaterialVar* pVar = pMaterial->FindVar("$selfillum", &found, false);
        if (found && pVar) {
            outProps.isSelfIllum = (pVar->GetIntValue() == 1);
        }
    }
    
    // =========================================================================
    // NEW: Copy additional VMT-parsed properties for comprehensive PBR extraction
    // =========================================================================
    
    // $selfillummask - separate emissive mask texture
    if (hasVMTParsed && vmtParsed.hasSelfIllumMask && IsValidTexturePath(vmtParsed.selfIllumMask)) {
        outProps.selfIllumMaskPath = vmtParsed.selfIllumMask;
        outProps.hasSelfIllumMask = true;
        if (m_debugOutput) {
            Msg("[LegacyTextureProcessor] %s: $selfillummask (from VMT) = %s\n", materialName.c_str(), outProps.selfIllumMaskPath.c_str());
        }
    }
    
    // $selfillumtint - tint color for self-illumination
    if (hasVMTParsed && vmtParsed.hasSelfIllumTint) {
        outProps.selfIllumTint[0] = vmtParsed.selfIllumTint[0];
        outProps.selfIllumTint[1] = vmtParsed.selfIllumTint[1];
        outProps.selfIllumTint[2] = vmtParsed.selfIllumTint[2];
        outProps.hasSelfIllumTint = true;
        if (m_debugOutput) {
            Msg("[LegacyTextureProcessor] %s: $selfillumtint (from VMT) = [%.2f %.2f %.2f]\n", 
                materialName.c_str(), outProps.selfIllumTint[0], outProps.selfIllumTint[1], outProps.selfIllumTint[2]);
        }
    }
    
    // Rim lighting properties
    if (hasVMTParsed && vmtParsed.hasRimLight) {
        outProps.hasRimLight = (vmtParsed.rimLight != 0);
        if (m_debugOutput && outProps.hasRimLight) {
            Msg("[LegacyTextureProcessor] %s: $rimlight=1 (from VMT)\n", materialName.c_str());
        }
    }
    if (hasVMTParsed && vmtParsed.hasRimLightExponent) {
        outProps.rimLightExponent = vmtParsed.rimLightExponent;
        outProps.hasRimLightExponent = true;
    }
    if (hasVMTParsed && vmtParsed.hasRimLightBoost) {
        outProps.rimLightBoost = vmtParsed.rimLightBoost;
        outProps.hasRimLightBoost = true;
    }
    
    // Additional phong properties
    if (hasVMTParsed && vmtParsed.hasPhongAlbedoTint) {
        outProps.phongAlbedoTint = (vmtParsed.phongAlbedoTint != 0);
    }
    if (hasVMTParsed && vmtParsed.hasPhongAlbedoBoost) {
        outProps.phongAlbedoBoost = vmtParsed.phongAlbedoBoost;
        outProps.hasPhongAlbedoBoost = true;
    }
    if (hasVMTParsed && vmtParsed.hasPhongTint) {
        outProps.phongTint[0] = vmtParsed.phongTint[0];
        outProps.phongTint[1] = vmtParsed.phongTint[1];
        outProps.phongTint[2] = vmtParsed.phongTint[2];
        outProps.hasPhongTint = true;
        if (m_debugOutput) {
            Msg("[LegacyTextureProcessor] %s: $phongtint (from VMT) = [%.2f %.2f %.2f]\n", 
                materialName.c_str(), outProps.phongTint[0], outProps.phongTint[1], outProps.phongTint[2]);
        }
    }
    
    // Parallax/heightmap
    if (hasVMTParsed && vmtParsed.hasParallaxMap && IsValidTexturePath(vmtParsed.parallaxMap)) {
        outProps.parallaxMapPath = vmtParsed.parallaxMap;
        outProps.hasParallaxMap = true;
        if (m_debugOutput) {
            Msg("[LegacyTextureProcessor] %s: $parallaxmap (from VMT) = %s\n", materialName.c_str(), outProps.parallaxMapPath.c_str());
        }
    }
    if (hasVMTParsed && vmtParsed.hasParallaxMapScale) {
        outProps.parallaxMapScale = vmtParsed.parallaxMapScale;
        outProps.hasParallaxMapScale = true;
    }
    
    // Additional envmap properties
    if (hasVMTParsed && vmtParsed.hasEnvMapContrast) {
        outProps.envMapContrast = vmtParsed.envMapContrast;
        outProps.hasEnvMapContrast = true;
        if (m_debugOutput) {
            Msg("[LegacyTextureProcessor] %s: $envmapcontrast (from VMT) = %.2f\n", materialName.c_str(), outProps.envMapContrast);
        }
    }
    if (hasVMTParsed && vmtParsed.hasEnvMapSaturation) {
        outProps.envMapSaturation = vmtParsed.envMapSaturation;
        outProps.hasEnvMapSaturation = true;
    }
    
    // =========================================================================
    // ExoPBR community PBR format detection
    // ExoPBR provides direct PBR textures - ARM map, normal, emission
    // =========================================================================
    if (hasVMTParsed && vmtParsed.isExoPBR) {
        outProps.isExoPBR = true;
        
        if (vmtParsed.hasTexture1 && IsValidTexturePath(vmtParsed.texture1)) {
            outProps.armTexturePath = vmtParsed.texture1;
            outProps.hasARMTexture = true;
            if (m_debugOutput) {
                Msg("[LegacyTextureProcessor] %s: [ExoPBR] ARM texture = %s\n", materialName.c_str(), vmtParsed.texture1.c_str());
            }
        }
        
        if (vmtParsed.hasTexture2 && IsValidTexturePath(vmtParsed.texture2)) {
            outProps.exoNormalPath = vmtParsed.texture2;
            outProps.hasExoNormal = true;
            if (m_debugOutput) {
                Msg("[LegacyTextureProcessor] %s: [ExoPBR] Normal texture = %s\n", materialName.c_str(), vmtParsed.texture2.c_str());
            }
        }
        
        if (vmtParsed.hasTexture3 && IsValidTexturePath(vmtParsed.texture3)) {
            outProps.emissionTexturePath = vmtParsed.texture3;
            outProps.hasEmissionTexture = true;
            if (m_debugOutput) {
                Msg("[LegacyTextureProcessor] %s: [ExoPBR] Emission texture = %s\n", materialName.c_str(), vmtParsed.texture3.c_str());
            }
        }
        
        if (vmtParsed.hasEmissionScale) {
            outProps.emissionScale = vmtParsed.emissionScale;
            outProps.hasEmissionScale = true;
        }
        
        if (vmtParsed.hasEmissionTint) {
            outProps.emissionTint[0] = vmtParsed.emissionTint[0];
            outProps.emissionTint[1] = vmtParsed.emissionTint[1];
            outProps.emissionTint[2] = vmtParsed.emissionTint[2];
            outProps.hasEmissionTint = true;
        }
        
        // ExoPBR materials have direct PBR data - set roughness and metallic to use textures
        // The ARM map will be split in CreatePBRMaterial
        outProps.roughness = 0.5f;  // Default, will be overridden by ARM map
        outProps.metallic = 0.0f;   // Default, will be overridden by ARM map
        
        if (m_debugOutput) {
            Msg("[LegacyTextureProcessor] %s: [ExoPBR] Material detected - using direct PBR path\n", materialName.c_str());
        }
    }
    
    // =========================================================================
    // GPBR (Strata Source) community PBR format detection
    // GPBR provides direct PBR textures - MRAO map, normal, emission
    // =========================================================================
    if (hasVMTParsed && vmtParsed.isGPBR) {
        outProps.isGPBR = true;
        
        // Copy MRAO texture path
        if (vmtParsed.hasMRAOTexture && IsValidTexturePath(vmtParsed.mraoTexture)) {
            outProps.mraoTexturePath = vmtParsed.mraoTexture;
            outProps.hasMRAOTexture = true;
            if (m_debugOutput) {
                Msg("[LegacyTextureProcessor] %s: [GPBR] MRAO texture = %s\n", materialName.c_str(), vmtParsed.mraoTexture.c_str());
            }
        }
        
        // MRAO scale
        if (vmtParsed.hasMRAOScale) {
            outProps.mraoScale = vmtParsed.mraoScale;
            outProps.hasMRAOScale = true;
        }
        
        // Emission texture
        if (vmtParsed.hasGPBREmissionTexture && IsValidTexturePath(vmtParsed.gpbrEmissionTexture)) {
            outProps.gpbrEmissionPath = vmtParsed.gpbrEmissionTexture;
            outProps.hasGPBREmission = true;
            if (m_debugOutput) {
                Msg("[LegacyTextureProcessor] %s: [GPBR] Emission texture = %s\n", materialName.c_str(), vmtParsed.gpbrEmissionTexture.c_str());
            }
        }
        
        // Emission scale
        if (vmtParsed.hasGPBREmissionScale) {
            outProps.gpbrEmissionScale = vmtParsed.gpbrEmissionScale;
            outProps.hasGPBREmissionScale = true;
        }
        
        // Parallax settings
        if (vmtParsed.hasGPBRParallax) {
            outProps.gpbrParallax = vmtParsed.gpbrParallax;
        }
        if (vmtParsed.hasGPBRParallaxDepth) {
            outProps.gpbrParallaxDepth = vmtParsed.gpbrParallaxDepth;
        }
        
        // Alpha transparency
        if (vmtParsed.hasGPBRAlpha) {
            outProps.gpbrAlpha = vmtParsed.gpbrAlpha;
            outProps.hasGPBRAlpha = true;
        }
        
        // GPBR materials have direct PBR data
        outProps.roughness = 0.5f;  // Default, will be overridden by MRAO map
        outProps.metallic = 0.0f;   // Default, will be overridden by MRAO map
        
        if (m_debugOutput) {
            Msg("[LegacyTextureProcessor] %s: [GPBR] Material detected - using direct PBR path\n", materialName.c_str());
        }
    }
    
    // =========================================================================
    // MWB PBR Gen format detection (must check before BFT - more specific)
    // Uses $phongexponenttexture with special encoding for PBR data
    // =========================================================================
    if (hasVMTParsed && vmtParsed.isMWBPBR) {
        outProps.isMWBPBR = true;
        
        // MWB uses $phongexponenttexture for PBR data
        // Red channel: pow(gloss, 4.0) -> roughness via pow^0.25
        // Green channel: direct metallic value
        if (vmtParsed.hasPhongExponentTexture && IsValidTexturePath(vmtParsed.phongExponentTexture)) {
            outProps.bftExponentTexturePath = vmtParsed.phongExponentTexture;  // Reuse BFT field
            outProps.hasBFTExponentTexture = true;
            if (m_debugOutput) {
                Msg("[LegacyTextureProcessor] %s: [MWB-PBR] Exponent texture = %s\n", 
                    materialName.c_str(), vmtParsed.phongExponentTexture.c_str());
            }
        }
        
        // MWB materials have default mid-range values (textures provide actual data)
        outProps.roughness = 0.5f;  // Will be decoded from exponent texture red channel
        outProps.metallic = 0.0f;   // Will be extracted from exponent green or base alpha
        
        if (m_debugOutput) {
            Msg("[LegacyTextureProcessor] %s: [MWB-PBR] Material detected - using MWB handler\n", 
                materialName.c_str());
        }
    }
    // =========================================================================
    // BlueFlyTrap PseudoPBR format detection
    // Uses $phongexponenttexture for roughness encoding
    // =========================================================================
    else if (hasVMTParsed && vmtParsed.isBFTPseudoPBR) {
        outProps.isBFTPseudoPBR = true;
        outProps.isBFTMetallicLayer = vmtParsed.isBFTMetallicLayer;
        
        // The exponent texture contains the roughness info (inverted)
        if (vmtParsed.hasPhongExponentTexture && IsValidTexturePath(vmtParsed.phongExponentTexture)) {
            outProps.bftExponentTexturePath = vmtParsed.phongExponentTexture;
            outProps.hasBFTExponentTexture = true;
            if (m_debugOutput) {
                Msg("[LegacyTextureProcessor] %s: [BFT] Exponent texture = %s\n", 
                    materialName.c_str(), vmtParsed.phongExponentTexture.c_str());
            }
        }
        
        // Set metallic based on layer type
        if (outProps.isBFTMetallicLayer) {
            outProps.metallic = 0.9f;  // High metallic for metallic layers
        } else {
            outProps.metallic = 0.0f;  // Non-metallic for base layers
        }
        
        outProps.roughness = 0.5f;  // Will be overridden by exponent texture
        
        if (m_debugOutput) {
            Msg("[LegacyTextureProcessor] %s: [BFT] Material detected - %s layer\n", 
                materialName.c_str(), outProps.isBFTMetallicLayer ? "metallic" : "base");
        }
    }
    
    // =========================================================================
    // END: Additional VMT-parsed properties
    // =========================================================================
    
    // Get $translucent - prefer VMT-parsed value
    if (hasVMTParsed && vmtParsed.hasTranslucent) {
        outProps.isTranslucent = vmtParsed.translucent;
    } else {
        IMaterialVar* pVar = pMaterial->FindVar("$translucent", &found, false);
        if (found && pVar) {
            outProps.isTranslucent = (pVar->GetIntValue() == 1);
        }
    }
    
    // Get $surfaceprop - prefer VMT-parsed value
    if (hasVMTParsed && !vmtParsed.surfaceProp.empty()) {
        outProps.surfaceProp = vmtParsed.surfaceProp;
    } else {
        IMaterialVar* pVar = pMaterial->FindVar("$surfaceprop", &found, false);
        if (found && pVar) {
            const char* surfaceVal = pVar->GetStringValue();
            if (surfaceVal && surfaceVal[0] != '\0') {
                outProps.surfaceProp = surfaceVal;
            }
        }
    }
    
    // Glass detection: Material is considered glass if:
    // 1. Shader is "Refract" (always glass), OR
    // 2. VMT has $refractamount (indicates Refract shader even if FindVar says otherwise), OR
    // 3. $surfaceprop = "glass" AND $translucent = 1 (surfaceprop alone is for physics, needs translucent)
    // NOTE: $surfaceprop "glass" alone is NOT enough - materials like nukwindowa have it for physics sounds
    //       but are not meant to be transparent. They need $translucent=1 to actually be glass.
    {
        // Check if shader name indicates glass/refraction
        bool isRefractShader = false;
        if (!outProps.shaderName.empty()) {
            std::string shaderLower = outProps.shaderName;
            std::transform(shaderLower.begin(), shaderLower.end(), shaderLower.begin(), ::tolower);
            isRefractShader = (shaderLower.find("refract") != std::string::npos);
        }
        
        // Check if surfaceprop is glass
        bool isSurfaceGlass = false;
        if (!outProps.surfaceProp.empty()) {
            std::string surfaceLower = outProps.surfaceProp;
            std::transform(surfaceLower.begin(), surfaceLower.end(), surfaceLower.begin(), ::tolower);
            isSurfaceGlass = (surfaceLower == "glass" || surfaceLower.find("glass") != std::string::npos);
        }
        
        // We already parsed the VMT file at the start - reuse that data
        // Check VMT shader name and $refractamount
        bool vmtIsRefract = false;
        bool vmtHasRefractAmount = false;
        if (hasVMTParsed) {
            if (!vmtParsed.shaderName.empty()) {
                std::string vmtShaderLower = vmtParsed.shaderName;
                std::transform(vmtShaderLower.begin(), vmtShaderLower.end(), vmtShaderLower.begin(), ::tolower);
                vmtIsRefract = (vmtShaderLower.find("refract") != std::string::npos);
            }
            vmtHasRefractAmount = vmtParsed.hasRefractAmount;
            
            // Get $refracttinttexture if present
            if (vmtParsed.hasRefractTintTexture && !vmtParsed.refractTintTexture.empty()) {
                outProps.refractTintTexturePath = vmtParsed.refractTintTexture;
            }
        }
        
        // Track if this is a Refract shader (important for transmittance texture handling)
        if (isRefractShader || vmtIsRefract) {
            outProps.isRefractShader = true;
        }
        
        // Determine if this is a glass material
        // Be CONSERVATIVE - only mark as glass if we're SURE it's meant to be refractive glass
        // NOT just any translucent material with reflections (like doors with glass cutouts)
        if (isRefractShader || vmtIsRefract) {
            outProps.isGlass = true;  // Refract shader is always glass
        } else if (vmtHasRefractAmount) {
            outProps.isGlass = true;  // Has $refractamount means it's a refractive material
        } else if (isSurfaceGlass && outProps.isTranslucent) {
            // surfaceprop=glass ONLY triggers glass shader if ALSO translucent
            // Materials like nukwindowa have $surfaceprop "glass" but no $translucent
            // They're just textures with glass surface properties for physics/sounds
            outProps.isGlass = true;
        }
        // NOTE: We REMOVED the "translucent + envmap = glass" heuristic because it catches
        // too many materials that aren't glass (like doors with glass cutouts, windows with frames, etc.)
        // Those materials have $translucent for alpha blending, not for glass refraction.
        // NOTE 2: $surfaceprop alone is NOT enough - it's just for physics. Need $translucent too.
        
        if (m_debugOutput) {
            if (outProps.isGlass) {
                Msg("[LegacyTextureProcessor] %s: DETECTED AS GLASS (shader=%s, vmtShader=%s, vmtRefract=%d, translucent=%d, surfaceprop=%s, hasEnvMap=%d)\n",
                    materialName.c_str(), outProps.shaderName.c_str(), 
                    hasVMTParsed ? vmtParsed.shaderName.c_str() : "N/A",
                    vmtHasRefractAmount, outProps.isTranslucent, outProps.surfaceProp.c_str(), outProps.hasEnvMap);
            } else if (isSurfaceGlass || isRefractShader || vmtIsRefract || vmtHasRefractAmount) {
                // This shouldn't happen, but log it for debugging
                Msg("[LegacyTextureProcessor] %s: GLASS DETECTION FAILED - shader=%s, isRefract=%d, vmtIsRefract=%d, vmtRefract=%d, isSurfaceGlass=%d, translucent=%d, hasEnvMap=%d\n",
                    materialName.c_str(), outProps.shaderName.c_str(), isRefractShader, vmtIsRefract, vmtHasRefractAmount, isSurfaceGlass, outProps.isTranslucent, outProps.hasEnvMap);
            }
        }
    }
    
    // Analyze base texture brightness for metallic detection (only if material has envmap)
    // Black textures + envmap = metallic materials (like chrome)
    // Grey/colored textures + envmap = non-metallic with reflections
    if (outProps.hasEnvMap && !outProps.baseTexturePath.empty() && !outProps.isGlass) {
        float brightness = 0.5f;
        if (AnalyzeBaseTextureBrightness(outProps.baseTexturePath, brightness)) {
            outProps.baseTextureBrightness = brightness;
            outProps.hasBaseTextureBrightness = true;
            
            if (m_debugOutput) {
                Msg("[LegacyTextureProcessor] %s: Base texture brightness=%.3f (for metallic detection)\n",
                    materialName.c_str(), brightness);
            }
        }
    }
    
    // Auto-discover companion textures that aren't explicitly referenced in the VMT
    // This helps find textures that follow naming conventions (e.g., _normal, _mask, _spec)
    if (m_autoDiscoverEnabled && !outProps.baseTexturePath.empty()) {
        DiscoverCompanionTextures(outProps.baseTexturePath, outProps);
        
        // If we discovered a normal map and don't have one yet, use it
        if (outProps.hasDiscoveredNormal && !outProps.hasBumpMap) {
            outProps.bumpMapPath = outProps.discoveredNormalPath;
            outProps.hasBumpMap = true;
            if (m_debugOutput) {
                Msg("[LegacyTextureProcessor] %s: Using auto-discovered normal map: %s\n", 
                    materialName.c_str(), outProps.discoveredNormalPath.c_str());
            }
        }
        
        // If we discovered a height map and don't have one yet, use it
        if (outProps.hasDiscoveredHeight && !outProps.hasParallaxMap) {
            outProps.parallaxMapPath = outProps.discoveredHeightPath;
            outProps.hasParallaxMap = true;
            if (m_debugOutput) {
                Msg("[LegacyTextureProcessor] %s: Using auto-discovered height map: %s\n", 
                    materialName.c_str(), outProps.discoveredHeightPath.c_str());
            }
        }
    }
    
    // Calculate PBR values with enhanced logic
    outProps.roughness = CalculateRoughness(outProps);
    outProps.metallic = EstimateMetallic(outProps);
    
    if (m_debugOutput) {
        Msg("[LegacyTextureProcessor] %s extracted: hasPhong=%d, phongExp=%.0f, hasBump=%d, hasEnvMask=%d, hasPhongExpTex=%d, hasEnvMapTint=%d, hasEnvMap=%d, normMapAlpha=%d, isGlass=%d\n",
            materialName.c_str(), outProps.hasPhong, outProps.phongExponent, outProps.hasBumpMap, 
            outProps.hasEnvMapMask, outProps.hasPhongExponentTexture, outProps.hasEnvMapTint, outProps.hasEnvMap, outProps.normalMapAlphaEnvMapMask, outProps.isGlass);
    }
    
    return true;
}


// Helper to create ProcessingContext for modular format handlers
ProcessingContext TextureProcessor::CreateProcessingContext() {
    ProcessingContext ctx;
    
    // Bind member functions to the context
    ctx.readVTFFile = [this](const std::string& path, std::vector<uint8_t>& data) {
        return ReadVTFFile(path, data);
    };
    ctx.parseVTFHeader = [this](const std::vector<uint8_t>& data, VTFFileHeader& header) {
        return ParseVTFHeader(data, header);
    };
    ctx.extractPixelData = [this](const std::vector<uint8_t>& data, const VTFFileHeader& header, 
                                   ConvertedTexture& tex, bool convertNormal) {
        return ExtractVTFPixelData(data, header, tex, convertNormal);
    };
    ctx.writeDDS = [this](const ConvertedTexture& tex, const std::string& path) {
        return WriteTextureToDDS(tex, path);
    };
    ctx.convertToOctahedral = [this](ConvertedTexture& tex) {
        ConvertNormalMapToOctahedral(tex);
    };
    ctx.convertSSBumpToNormal = [this](ConvertedTexture& tex) {
        ConvertSSBumpToNormal(tex);
    };
    ctx.generateHash = [this](const std::string& name, uint32_t w, uint32_t h) {
        return GenerateTextureHash(name, w, h);
    };
    ctx.generateOutputPath = [this](uint64_t hash, const std::string& suffix) {
        return GenerateOutputPath(hash, suffix.c_str());
    };
    ctx.fileExists = [this](const std::string& path) {
        return FileExists(path);
    };
    
    ctx.debugOutput = m_debugOutput;
    ctx.materialsWithNormals = &m_stats.materialsWithNormals;
    ctx.materialsWithRoughness = &m_stats.materialsWithRoughness;
    
    return ctx;
}

// Helper to copy ProcessedMaterial results to ProcessedMaterialInfo
static void CopyProcessedMaterial(const ProcessedMaterial& src, TextureProcessor::ProcessedMaterialInfo& dst) {
    if (!src.normalPath.empty()) dst.normalPath = src.normalPath;
    if (!src.roughnessPath.empty()) dst.roughnessPath = src.roughnessPath;
    if (!src.metallicPath.empty()) dst.metallicPath = src.metallicPath;
    if (!src.heightPath.empty()) {
        dst.heightPath = src.heightPath;
        dst.heightScale = src.heightScale;
    }
    if (!src.emissivePath.empty()) {
        dst.emissivePath = src.emissivePath;
        dst.emissionIntensity = src.emissionIntensity;
    }
    if (!src.transmittancePath.empty()) dst.transmittancePath = src.transmittancePath;
    if (!src.albedoPath.empty()) dst.albedoPath = src.albedoPath;
    if (src.roughnessConstant != 0.5f) dst.roughnessConstant = src.roughnessConstant;
    if (src.metallicConstant != 0.0f) dst.metallicConstant = src.metallicConstant;
    if (src.isGlass) {
        dst.isGlass = src.isGlass;
        dst.ior = src.ior;
    }
}

bool TextureProcessor::CreatePBRMaterial(const MaterialPBRProperties& props, uint64_t textureHash) {
    if (textureHash == 0) {
        return false;
    }
    
    // Check if we've already created a material for this hash (thread-safe)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_processedMaterialInfo.find(textureHash) != m_processedMaterialInfo.end()) {
            return true; // Already done
        }
    }
    
    // Ensure output directory exists for texture files
    if (!EnsureOutputDirectory()) {
        Warning("[LegacyTextureProcessor] Cannot create output directory, skipping material\n");
        return false;
    }
    
    // Track material info for USDA generation
    ProcessedMaterialInfo matInfo;
    matInfo.textureHash = textureHash;
    matInfo.roughnessConstant = props.roughness;
    matInfo.metallicConstant = props.metallic;
    matInfo.heightScale = 0.025f;  // Default height scale
    matInfo.isGlass = props.isGlass;
    matInfo.isRefractShader = props.isRefractShader;
    matInfo.ior = props.isGlass ? 1.5f : 1.0f;  // Default glass IOR is 1.5
    matInfo.emissionIntensity = props.hasEmissionScale ? props.emissionScale : 1.0f;
    
    // Create processing context for modular handlers
    ProcessingContext ctx = CreateProcessingContext();
    
    // =========================================================================
    // Delegate to format-specific handlers (defined in separate files)
    // Priority: ExoPBR -> GPBR -> BFT -> SourceEngine (fallback)
    // =========================================================================
    
    // ExoPBR format
    if (props.isExoPBR) {
        ProcessedMaterial result = ExoPBR::ProcessTextures(props, textureHash, ctx);
        if (result.success) {
            CopyProcessedMaterial(result, matInfo);
            std::lock_guard<std::mutex> lock(m_mutex);
            m_processedMaterialInfo[textureHash] = matInfo;
            m_stats.materialsProcessed++;
            return true;
        }
    }
    
    // GPBR format
    if (props.isGPBR) {
        ProcessedMaterial result = GPBR::ProcessTextures(props, textureHash, ctx);
        if (result.success) {
            CopyProcessedMaterial(result, matInfo);
            std::lock_guard<std::mutex> lock(m_mutex);
            m_processedMaterialInfo[textureHash] = matInfo;
            m_stats.materialsProcessed++;
            return true;
        }
    }
    
    // MWB PBR Gen format (must check before BFT - it has more specific patterns)
    if (props.isMWBPBR) {
        ProcessedMaterial result = MWBPBR::ProcessTextures(props, textureHash, ctx);
        if (result.success) {
            CopyProcessedMaterial(result, matInfo);
            std::lock_guard<std::mutex> lock(m_mutex);
            m_processedMaterialInfo[textureHash] = matInfo;
            m_stats.materialsProcessed++;
            return true;
        }
    }
    
    // BlueFlyTrap PseudoPBR format
    if (props.isBFTPseudoPBR) {
        ProcessedMaterial result = BFTPseudoPBR::ProcessTextures(props, textureHash, ctx);
        if (result.success) {
            CopyProcessedMaterial(result, matInfo);
            std::lock_guard<std::mutex> lock(m_mutex);
            m_processedMaterialInfo[textureHash] = matInfo;
            m_stats.materialsProcessed++;
            return true;
        }
    }
    
    // =========================================================================
    // Standard Source Engine material processing (fallback)
    // For non-PBR format materials, use the SourceEngine handler
    // =========================================================================
    ProcessedMaterial result = SourceEngine::ProcessTextures(props, textureHash, ctx);
    if (result.success) {
        CopyProcessedMaterial(result, matInfo);
        
        // Store for USDA generation (thread-safe)
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_processedMaterialInfo[textureHash] = matInfo;
            m_stats.materialsProcessed++;
        }
        
        if (m_debugOutput) {
            Msg("[LegacyTextureProcessor] Processed material '%s' (hash 0x%llX): roughness=%.2f, metallic=%.2f%s%s%s%s\n",
                props.materialName.c_str(), textureHash, matInfo.roughnessConstant, matInfo.metallicConstant,
                !matInfo.normalPath.empty() ? " [normal]" : "",
                !matInfo.roughnessPath.empty() ? " [roughness]" : "",
                !matInfo.metallicPath.empty() ? " [metallic]" : "",
                !matInfo.heightPath.empty() ? " [height]" : "");
        }
        
        return true;
    }
    
    return false;
}

int TextureProcessor::ProcessAllTrackedMaterials() {
    // Process all materials without limit (legacy behavior)
    return ProcessTrackedMaterialsBatch(0);
}

int TextureProcessor::ProcessTrackedMaterialsBatch(int maxBatch) {
    // LOCK-FREE FAST PATH: Check if we know everything is processed without taking any locks
    // This atomic load has zero contention and returns instantly
    if (m_allMaterialsProcessed.load(std::memory_order_relaxed)) {
        return 0;
    }
    
    auto startTime = std::chrono::high_resolution_clock::now();
    
    std::lock_guard<std::mutex> lock(m_mutex);
    
    if (!m_initialized) {
        Warning("[LegacyTextureProcessor] Not initialized\n");
        return 0;
    }
    
    auto afterLock = std::chrono::high_resolution_clock::now();
    
    // Check if material count changed before expensive GetCachedMaterials() call
    size_t currentCount = D3D9TextureTracker::Instance().GetCacheSize();
    
    // Early exit: if count hasn't changed and we processed everything, nothing new to do
    if (currentCount == m_lastKnownMaterialCount && m_processedMaterials.size() >= currentCount) {
        // Set the lock-free flag so next time we don't even take the lock
        m_allMaterialsProcessed.store(true, std::memory_order_relaxed);
        
        auto endTime = std::chrono::high_resolution_clock::now();
        if (m_debugOutput) {
            auto lockTime = std::chrono::duration_cast<std::chrono::microseconds>(afterLock - startTime).count();
            auto totalTime = std::chrono::duration_cast<std::chrono::microseconds>(endTime - startTime).count();
            Msg("[LegacyTextureProcessor] Batch: early exit, set fast-path flag (lock: %lld us, total: %lld us)\n", lockTime, totalTime);
        }
        return 0;
    }
    
    // Material count changed - clear the flag so we check again next time
    m_allMaterialsProcessed.store(false, std::memory_order_relaxed);
    
    auto afterCheck = std::chrono::high_resolution_clock::now();
    
    // Count changed or there might be unprocessed materials - do the expensive work
    std::vector<std::string> cachedMaterials = D3D9TextureTracker::Instance().GetCachedMaterials();
    m_lastKnownMaterialCount = cachedMaterials.size();
    
    auto afterGetMaterials = std::chrono::high_resolution_clock::now();
    
    if (m_debugOutput) {
        auto lockTime = std::chrono::duration_cast<std::chrono::microseconds>(afterLock - startTime).count();
        auto checkTime = std::chrono::duration_cast<std::chrono::microseconds>(afterCheck - afterLock).count();
        auto getMaterialsTime = std::chrono::duration_cast<std::chrono::microseconds>(afterGetMaterials - afterCheck).count();
        Msg("[LegacyTextureProcessor] Batch timings: lock=%lld us, check=%lld us, getMaterials=%lld us\n",
            lockTime, checkTime, getMaterialsTime);
    }
    
    int processedCount = 0;
    int skippedCount = 0;
    
    for (const std::string& matName : cachedMaterials) {
        // Check batch limit (0 = no limit)
        if (maxBatch > 0 && processedCount >= maxBatch) {
            break;
        }
        
        // Skip already processed
        if (m_processedMaterials.find(matName) != m_processedMaterials.end()) {
            skippedCount++;
            // Early exit optimization: if we've skipped many materials in a row, likely nothing new
            if (skippedCount > 100 && processedCount == 0) {
                // We've checked 100 materials and found nothing new - probably nothing left
                break;
            }
            continue;
        }
        
        // Reset skip counter when we find something unprocessed
        skippedCount = 0;
        
        // Skip internal materials
        if (matName.find("__") == 0 || matName.find("vgui") == 0) {
            m_processedMaterials.insert(matName);
            continue;
        }
        
        // Extract PBR properties
        MaterialPBRProperties props;
        if (!ExtractMaterialPBR(matName, props)) {
            // Mark as processed even if it failed - don't check it again
            m_processedMaterials.insert(matName);
            continue;
        }
        
        // Only process materials with PBR-relevant data
        // Include materials with roughness texture sources OR envmap/envmaptint (which implies reflectivity)
        if (!props.hasBumpMap && !props.hasPhong && !props.hasEnvMapMask && 
            !props.normalMapAlphaEnvMapMask && !props.hasPhongExponentTexture && 
            !props.hasBaseMapAlphaPhongMask && !props.hasBaseAlphaEnvMapMask &&
            !props.hasEnvMap && !props.hasEnvMapTint && !props.isGlass) {
            m_processedMaterials.insert(matName);
            continue;
        }
        
        // Get the texture hash from the tracker
        const std::vector<IDirect3DTexture9*>* variants = 
            D3D9TextureTracker::Instance().GetTextureVariantsForMaterial(matName.c_str());
        
        if (!variants || variants->empty()) {
            continue;
        }
        
        // Get hash for first variant
        uint64_t textureHash = 0;
        for (IDirect3DTexture9* tex : *variants) {
            if (!tex) continue;
            auto result = g_remix->dxvk_GetTextureHash(tex);
            if (result && result.value() != 0) {
                textureHash = result.value();
                break;
            }
        }
        
        if (textureHash == 0) {
            continue;
        }
        
        props.baseTextureHash = textureHash;
        
        // Create PBR material (generates textures and tracks info for USDA)
        if (CreatePBRMaterial(props, textureHash)) {
            processedCount++;
        }
        
        m_processedMaterials.insert(matName);
    }
    
    if (processedCount > 0) {
        if (m_debugOutput) {
            Msg("[LegacyTextureProcessor] Processed %d materials with PBR properties\n", processedCount);
            Msg("[LegacyTextureProcessor] Stats: %d with normals, %d with roughness textures\n", 
                m_stats.materialsWithNormals, m_stats.materialsWithRoughness);
        }
        
        // Flag that USDA needs updating - caller must call WriteUSDAIfNeeded() to actually write
        m_needsUSDAUpdate = true;
    }
    
    return processedCount;
}

bool TextureProcessor::IsMaterialProcessed(const std::string& materialName) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_processedMaterials.find(materialName) != m_processedMaterials.end();
}

TextureProcessor::Stats TextureProcessor::GetStats() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_stats;
}

void TextureProcessor::ClearCache() {
    std::lock_guard<std::mutex> lock(m_mutex);
    
    // Clear all tracking data for reprocessing.
    // Note: USDA files remain on disk and need game restart to reload.
    m_processedMaterials.clear();
    m_uploadedTextures.clear();
    m_processedMaterialInfo.clear();
    m_needsUSDAUpdate = false;
    m_stats = {};
    
    Msg("[LegacyTextureProcessor] Cache cleared\n");
}

bool TextureProcessor::ProcessSingleMaterial(const std::string& materialName) {
    std::lock_guard<std::mutex> lock(m_mutex);
    
    if (!m_initialized) {
        return false;
    }
    
    // Skip already processed
    if (m_processedMaterials.find(materialName) != m_processedMaterials.end()) {
        return true; // Already done
    }
    
    // Skip internal materials
    if (materialName.find("__") == 0 || materialName.find("vgui") == 0) {
        return false;
    }
    
    // Extract PBR properties
    MaterialPBRProperties props;
    if (!ExtractMaterialPBR(materialName, props)) {
        return false;
    }
    
    // Only process materials with PBR-relevant data
    // Include materials with roughness texture sources OR envmap/envmaptint (which implies reflectivity)
    // OR glass materials (which need special shader)
    if (!props.hasBumpMap && !props.hasPhong && !props.hasEnvMapMask && 
        !props.normalMapAlphaEnvMapMask && !props.hasPhongExponentTexture &&
        !props.hasBaseMapAlphaPhongMask && !props.hasBaseAlphaEnvMapMask &&
        !props.hasEnvMap && !props.hasEnvMapTint && !props.isGlass) {
        m_processedMaterials.insert(materialName);
        return false;
    }
    
    // Get the texture hash from the tracker
    const std::vector<IDirect3DTexture9*>* variants = 
        D3D9TextureTracker::Instance().GetTextureVariantsForMaterial(materialName.c_str());
    
    if (!variants || variants->empty()) {
        return false;
    }
    
    // Get hash for first variant
    uint64_t textureHash = 0;
    for (IDirect3DTexture9* tex : *variants) {
        if (!tex) continue;
        auto result = g_remix->dxvk_GetTextureHash(tex);
        if (result && result.value() != 0) {
            textureHash = result.value();
            break;
        }
    }
    
    if (textureHash == 0) {
        return false;
    }
    
    props.baseTextureHash = textureHash;
    
    // Create PBR material (generates textures and tracks info for USDA)
    bool success = CreatePBRMaterial(props, textureHash);
    
    m_processedMaterials.insert(materialName);
    
    if (success) {
        m_needsUSDAUpdate = true;
    }
    
    return success;
}

void TextureProcessor::OnNewMaterialDetected(const std::string& materialName, uint64_t textureHash) {
    if (!m_initialized || !m_autoProcessing) {
        return;
    }
    
    // Skip already processed
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_processedMaterials.find(materialName) != m_processedMaterials.end()) {
            return;
        }
    }
    
    // Skip internal materials
    if (materialName.find("__") == 0 || materialName.find("vgui") == 0) {
        return;
    }
    
    // Clear the lock-free flag so batch processing will check for new materials
    m_allMaterialsProcessed.store(false, std::memory_order_relaxed);
    
    // Process in background (don't hold up rendering)
    // For now, just mark as needing processing - we'll batch process later
    // The actual processing happens in ProcessAllTrackedMaterials or ProcessSingleMaterial
    
    if (m_debugOutput) {
        Msg("[LegacyTextureProcessor] New material detected for auto-processing: %s (hash 0x%llX)\n", 
            materialName.c_str(), textureHash);
    }
}

void TextureProcessor::WriteUSDAIfNeeded() {
    std::lock_guard<std::mutex> lock(m_mutex);
    
    if (!m_needsUSDAUpdate || m_processedMaterialInfo.empty()) {
        return;
    }
    
    if (WriteModUSDA()) {
        m_needsUSDAUpdate = false;
        Msg("[LegacyTextureProcessor] USDA updated with %d materials. Restart game for changes to take effect.\n",
            (int)m_processedMaterialInfo.size());
    }
}

// Helper to convert absolute path to relative path from mod directory
// Delegate to USDA module for relative path calculation
static std::string GetRelativeTexturePath(const std::string& absolutePath, const std::string& outputDir) {
    return USDA::GetRelativeTexturePath(absolutePath, outputDir);
}

bool TextureProcessor::WriteModUSDA() {
    if (m_outputDirectory.empty() || m_processedMaterialInfo.empty()) {
        return false;
    }
    
    // Get the mod directory using USDA module utility
    std::string modDir = USDA::GetModDirectory(m_outputDirectory);
    
    // Check existing materials and count new ones
    std::unordered_set<uint64_t> existingHashes;
    int newMaterialCount = 0;
    std::string materialsUsdaPath = modDir + "/materials.usda";
    USDA::CheckExistingMaterials(materialsUsdaPath, m_processedMaterialInfo, 
                                  existingHashes, newMaterialCount, m_debugOutput);
    
    // If no new materials to add and file exists, skip writing
    if (newMaterialCount == 0 && !existingHashes.empty()) {
        if (m_debugOutput) {
            Msg("[LegacyTextureProcessor] No new materials to add to USDA\n");
        }
        return true;
    }
    
    // Write mod.usda using USDA module
    if (!USDA::WriteModUSDAFile(modDir)) {
        return false;
    }
    
    // Write materials.usda using USDA module
    if (!USDA::WriteMaterialsUSDAFile(modDir, m_outputDirectory, m_processedMaterialInfo, m_debugOutput)) {
        return false;
    }
    
    Msg("[LegacyTextureProcessor] Wrote mod.usda and materials.usda with %d materials (%d new) to %s\n", 
        (int)m_processedMaterialInfo.size(), newMaterialCount, modDir.c_str());
    
    return true;
}

//=============================================================================
// Background Processing Implementation
//=============================================================================

void TextureProcessor::StartWorkerThread() {
    if (m_workerRunning.load(std::memory_order_relaxed)) {
        return; // Already running
    }
    
    m_shutdownRequested.store(false, std::memory_order_relaxed);
    m_workerRunning.store(true, std::memory_order_relaxed);
    
    m_workerThread = std::thread(&TextureProcessor::WorkerThreadFunc, this);
    
    if (m_debugOutput) {
        Msg("[LegacyTextureProcessor] Background worker thread started\n");
    }
}

void TextureProcessor::StopWorkerThread() {
    if (!m_workerRunning.load(std::memory_order_relaxed)) {
        return; // Not running
    }
    
    // Signal shutdown
    m_shutdownRequested.store(true, std::memory_order_relaxed);
    
    // Wake up the worker thread
    m_queueCondition.notify_all();
    
    // Wait for thread to finish
    if (m_workerThread.joinable()) {
        m_workerThread.join();
    }
    
    m_workerRunning.store(false, std::memory_order_relaxed);
    
    if (m_debugOutput) {
        Msg("[LegacyTextureProcessor] Background worker thread stopped\n");
    }
}

void TextureProcessor::WorkerThreadFunc() {
    if (m_debugOutput) {
        Msg("[LegacyTextureProcessor] Worker thread running\n");
    }
    
    while (!m_shutdownRequested.load(std::memory_order_relaxed)) {
        std::string materialName;
        
        // Get next material from queue
        {
            std::unique_lock<std::mutex> lock(m_queueMutex);
            
            // Wait for work or shutdown
            m_queueCondition.wait(lock, [this] {
                return !m_materialQueue.empty() || m_shutdownRequested.load(std::memory_order_relaxed);
            });
            
            if (m_shutdownRequested.load(std::memory_order_relaxed)) {
                break;
            }
            
            if (m_materialQueue.empty()) {
                continue;
            }
            
            materialName = m_materialQueue.front();
            m_materialQueue.pop();
        }
        
        // Process the material (outside of queue lock)
        m_backgroundProcessing.store(true, std::memory_order_relaxed);
        
        bool success = ProcessMaterialOnWorker(materialName);
        
        // Remove from queued set after processing
        {
            std::lock_guard<std::mutex> lock(m_queueMutex);
            m_queuedMaterials.erase(materialName);
        }
        
        if (success) {
            m_lastProcessedCount.fetch_add(1, std::memory_order_relaxed);
        }
        
        // Check if queue is empty - if so, write pending USDA materials
        bool queueEmpty = false;
        {
            std::lock_guard<std::mutex> lock(m_queueMutex);
            queueEmpty = m_materialQueue.empty();
        }
        
        if (queueEmpty) {
            AppendMaterialsToUSDA();
            m_backgroundProcessing.store(false, std::memory_order_relaxed);
        }
    }
    
    // Final USDA write before exiting
    AppendMaterialsToUSDA();
    
    if (m_debugOutput) {
        Msg("[LegacyTextureProcessor] Worker thread exiting\n");
    }
}

bool TextureProcessor::ProcessMaterialOnWorker(const std::string& materialName) {
    // Check if already processed (lock-free check first, then verify under lock)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_processedMaterials.find(materialName) != m_processedMaterials.end()) {
            return false; // Already processed
        }
    }
    
    // Skip internal materials
    if (materialName.find("__") == 0 || materialName.find("vgui") == 0) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_processedMaterials.insert(materialName);
        return false;
    }
    
    // Extract PBR properties (this reads from Source Engine - thread safe for reading)
    MaterialPBRProperties props;
    if (!ExtractMaterialPBR(materialName, props)) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_processedMaterials.insert(materialName);
        return false;
    }
    
    // Check if material has PBR-relevant data
    if (!props.hasBumpMap && !props.hasPhong && !props.hasEnvMapMask && 
        !props.normalMapAlphaEnvMapMask && !props.hasPhongExponentTexture &&
        !props.hasBaseMapAlphaPhongMask && !props.hasBaseAlphaEnvMapMask &&
        !props.hasEnvMap && !props.hasEnvMapTint && !props.isGlass) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_processedMaterials.insert(materialName);
        return false;
    }
    
    // Get the texture hash from the tracker
    const std::vector<IDirect3DTexture9*>* variants = 
        D3D9TextureTracker::Instance().GetTextureVariantsForMaterial(materialName.c_str());
    
    if (!variants || variants->empty()) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_processedMaterials.insert(materialName);
        return false;
    }
    
    // Get hash for first variant
    uint64_t textureHash = 0;
    for (IDirect3DTexture9* tex : *variants) {
        if (!tex) continue;
        auto result = g_remix->dxvk_GetTextureHash(tex);
        if (result && result.value() != 0) {
            textureHash = result.value();
            break;
        }
    }
    
    if (textureHash == 0) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_processedMaterials.insert(materialName);
        return false;
    }
    
    props.baseTextureHash = textureHash;
    
    // Create PBR material (generates textures - file I/O is thread safe)
    bool success = CreatePBRMaterial(props, textureHash);
    
    // Mark as processed
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_processedMaterials.insert(materialName);
        
        if (success) {
            m_stats.materialsProcessed++;
        }
    }
    
    if (success && m_debugOutput) {
        Msg("[LegacyTextureProcessor] [BG] Processed: %s (hash 0x%llX)\n", 
            materialName.c_str(), textureHash);
    }
    
    return success;
}

int TextureProcessor::QueueMaterialsForProcessing() {
    if (!m_initialized) {
        Msg("[LegacyTextureProcessor] QueueMaterialsForProcessing: not initialized\n");
        return 0;
    }
    
    // Start worker thread if not running
    StartWorkerThread();
    
    // Reset processed count for this batch
    m_lastProcessedCount.store(0, std::memory_order_relaxed);
    
    // Get all cached materials from tracker (this is the only main-thread access)
    std::vector<std::string> cachedMaterials = D3D9TextureTracker::Instance().GetCachedMaterials();
    
    if (m_debugOutput) {
        Msg("[LegacyTextureProcessor] Found %d cached materials in tracker\n", (int)cachedMaterials.size());
    }
    
    int queuedCount = 0;
    
    // Quick check which materials need processing
    std::vector<std::string> materialsToQueue;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (const std::string& matName : cachedMaterials) {
            // Skip already processed
            if (m_processedMaterials.find(matName) != m_processedMaterials.end()) {
                continue;
            }
            materialsToQueue.push_back(matName);
        }
    }
    
    // Add to queue (separate lock)
    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        for (const std::string& matName : materialsToQueue) {
            // Skip if already in queue
            if (m_queuedMaterials.find(matName) != m_queuedMaterials.end()) {
                continue;
            }
            
            m_materialQueue.push(matName);
            m_queuedMaterials.insert(matName);
            queuedCount++;
        }
    }
    
    // Wake up worker thread
    if (queuedCount > 0) {
        m_queueCondition.notify_one();
        Msg("[LegacyTextureProcessor] Queued %d materials for background processing\n", queuedCount);
    } else if (m_debugOutput) {
        Msg("[LegacyTextureProcessor] No new materials to queue (all %d already processed)\n", 
            (int)cachedMaterials.size());
    }
    
    return queuedCount;
}

size_t TextureProcessor::GetQueuedMaterialCount() const {
    std::lock_guard<std::mutex> lock(m_queueMutex);
    return m_materialQueue.size();
}

bool TextureProcessor::AppendMaterialsToUSDA() {
    // Collect pending materials
    std::vector<std::pair<uint64_t, ProcessedMaterialInfo>> pendingMaterials;
    size_t totalProcessed = 0;
    size_t alreadyWritten = 0;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        totalProcessed = m_processedMaterialInfo.size();
        alreadyWritten = m_materialsWrittenToUSDA.size();
        
        // Find materials that have been processed but not written to USDA
        for (const auto& pair : m_processedMaterialInfo) {
            if (m_materialsWrittenToUSDA.find(pair.first) == m_materialsWrittenToUSDA.end()) {
                pendingMaterials.push_back(pair);
            }
        }
    }
    
    if (pendingMaterials.empty()) {
        if (m_debugOutput) {
            Msg("[LegacyTextureProcessor] AppendMaterialsToUSDA: no pending materials (total=%d, written=%d)\n",
                (int)totalProcessed, (int)alreadyWritten);
        }
        return true; // Nothing to write
    }
    
    Msg("[LegacyTextureProcessor] AppendMaterialsToUSDA: %d pending materials to write\n", (int)pendingMaterials.size());
    
    // Get mod directory
    std::string modDir = USDA::GetModDirectory(m_outputDirectory);
    std::string materialsUsdaPath = modDir + "/materials.usda";
    
    // Check if file exists - if not, write full file
    std::ifstream checkFile(materialsUsdaPath);
    bool fileExists = checkFile.good();
    checkFile.close();
    
    if (!fileExists) {
        // Write mod.usda first
        if (!USDA::WriteModUSDAFile(modDir)) {
            Warning("[LegacyTextureProcessor] Failed to write mod.usda\n");
            return false;
        }
        
        // Write full materials.usda with all materials
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!USDA::WriteMaterialsUSDAFile(modDir, m_outputDirectory, m_processedMaterialInfo, m_debugOutput)) {
            Warning("[LegacyTextureProcessor] Failed to write materials.usda\n");
            return false;
        }
        
        // Mark all as written
        for (const auto& pair : m_processedMaterialInfo) {
            m_materialsWrittenToUSDA.insert(pair.first);
        }
        
        Msg("[LegacyTextureProcessor] Created materials.usda with %d materials\n", 
            (int)m_processedMaterialInfo.size());
        return true;
    }
    
    // File exists - just rewrite the whole file with all materials
    // This is simpler and more reliable than trying to append
    // The file is small enough that this is not a performance concern
    
    // Write mod.usda (in case it's missing)
    USDA::WriteModUSDAFile(modDir);
    
    // Write full materials.usda with all materials (existing + pending)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!USDA::WriteMaterialsUSDAFile(modDir, m_outputDirectory, m_processedMaterialInfo, m_debugOutput)) {
            Warning("[LegacyTextureProcessor] Failed to write materials.usda\n");
            return false;
        }
        
        // Mark all as written
        for (const auto& pair : m_processedMaterialInfo) {
            m_materialsWrittenToUSDA.insert(pair.first);
        }
    }
    
    Msg("[LegacyTextureProcessor] Updated materials.usda with %d total materials (%d new)\n", 
        (int)m_processedMaterialInfo.size(), (int)pendingMaterials.size());
    
    return true;
}

void TextureProcessor::AppendToUSDAAsync() {
    // If worker thread is running, it will handle USDA writes
    // Otherwise, do it synchronously
    if (m_workerRunning.load(std::memory_order_relaxed)) {
        // Worker will write when queue is empty
        return;
    }
    
    // No worker running - write synchronously
    AppendMaterialsToUSDA();
}

} // namespace LegacyTextureProcessor

#endif // _WIN64
