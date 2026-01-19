#ifdef _WIN64

#include "remixapi.h"
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

// Constants for VTF processing
constexpr int VTF_MAJOR_VERSION_SUPPORTED = 7;
constexpr int VTF_MAX_MINOR_VERSION = 5;
constexpr size_t MAX_VTF_FILE_SIZE = 256 * 1024 * 1024;  // 256 MB
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
    , m_needsUSDAUpdate(false) {
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
    DDSHeader header = {};
    
    header.magic = DDS_MAGIC;
    header.size = 124;  // Size of header minus magic number
    header.flags = DDSD_CAPS | DDSD_HEIGHT | DDSD_WIDTH | DDSD_PIXELFORMAT | DDSD_LINEARSIZE | DDSD_MIPMAPCOUNT;
    header.height = height;
    header.width = width;
    header.pitchOrLinearSize = width * height * (hasAlpha ? 4 : 3);
    header.depth = 1;
    header.mipMapCount = mipCount;
    
    // Pixel format for RGBA8888 or RGB888
    header.pixelFormat.size = 32;
    header.pixelFormat.flags = DDPF_RGB | (hasAlpha ? DDPF_ALPHAPIXELS : 0);
    header.pixelFormat.rgbBitCount = hasAlpha ? 32 : 24;
    header.pixelFormat.rBitMask = 0x00FF0000;  // Red
    header.pixelFormat.gBitMask = 0x0000FF00;  // Green
    header.pixelFormat.bBitMask = 0x000000FF;  // Blue
    header.pixelFormat.aBitMask = hasAlpha ? 0xFF000000 : 0;  // Alpha
    
    header.caps = DDSCAPS_TEXTURE | DDSCAPS_MIPMAP | DDSCAPS_COMPLEX;
    
    file.write(reinterpret_cast<const char*>(&header), sizeof(header));
    return file.good();
}

bool TextureProcessor::WriteTextureToDDS(const ConvertedTexture& texture, const std::string& outputPath) {
    if (texture.pixelData.empty()) {
        Warning("[LegacyTextureProcessor] Cannot write empty texture\n");
        return false;
    }
    
    std::ofstream file(outputPath, std::ios::binary);
    if (!file.is_open()) {
        Warning("[LegacyTextureProcessor] Failed to open file for writing: %s\n", outputPath.c_str());
        return false;
    }
    
    // Calculate number of mip levels
    uint32_t mipCount = CalculateMipLevels(texture.width, texture.height);
    
    // Write DDS header with mipmap info
    if (!WriteDDSHeader(file, texture.width, texture.height, true, mipCount)) {
        Warning("[LegacyTextureProcessor] Failed to write DDS header\n");
        return false;
    }
    
    // Convert from RGBA to BGRA (DDS expects BGRA)
    std::vector<uint8_t> bgraData(texture.pixelData.size());
    for (size_t i = 0; i < texture.pixelData.size(); i += 4) {
        bgraData[i + 0] = texture.pixelData[i + 2];  // B <- R
        bgraData[i + 1] = texture.pixelData[i + 1];  // G <- G
        bgraData[i + 2] = texture.pixelData[i + 0];  // R <- B
        bgraData[i + 3] = texture.pixelData[i + 3];  // A <- A
    }
    
    // Write base mip level (level 0)
    file.write(reinterpret_cast<const char*>(bgraData.data()), bgraData.size());
    
    // Generate and write subsequent mip levels
    uint32_t mipWidth = texture.width;
    uint32_t mipHeight = texture.height;
    std::vector<uint8_t> currentMip = bgraData;
    
    for (uint32_t mip = 1; mip < mipCount; mip++) {
        uint32_t newWidth = max(1u, mipWidth / 2);
        uint32_t newHeight = max(1u, mipHeight / 2);
        
        std::vector<uint8_t> newMip(newWidth * newHeight * 4);
        
        // Box filter downscale (2x2 average)
        for (uint32_t y = 0; y < newHeight; y++) {
            for (uint32_t x = 0; x < newWidth; x++) {
                uint32_t srcX = x * 2;
                uint32_t srcY = y * 2;
                
                // Sample 2x2 block from source
                uint32_t r = 0, g = 0, b = 0, a = 0;
                int sampleCount = 0;
                
                for (int dy = 0; dy < 2 && (srcY + dy) < mipHeight; dy++) {
                    for (int dx = 0; dx < 2 && (srcX + dx) < mipWidth; dx++) {
                        size_t srcIdx = ((srcY + dy) * mipWidth + (srcX + dx)) * 4;
                        b += currentMip[srcIdx + 0];
                        g += currentMip[srcIdx + 1];
                        r += currentMip[srcIdx + 2];
                        a += currentMip[srcIdx + 3];
                        sampleCount++;
                    }
                }
                
                size_t dstIdx = (y * newWidth + x) * 4;
                newMip[dstIdx + 0] = static_cast<uint8_t>(b / sampleCount);
                newMip[dstIdx + 1] = static_cast<uint8_t>(g / sampleCount);
                newMip[dstIdx + 2] = static_cast<uint8_t>(r / sampleCount);
                newMip[dstIdx + 3] = static_cast<uint8_t>(a / sampleCount);
            }
        }
        
        // Write this mip level
        file.write(reinterpret_cast<const char*>(newMip.data()), newMip.size());
        
        // Prepare for next iteration
        currentMip = std::move(newMip);
        mipWidth = newWidth;
        mipHeight = newHeight;
    }
    
    if (!file.good()) {
        Warning("[LegacyTextureProcessor] Failed to write texture data\n");
        return false;
    }
    
    file.close();
    
    if (m_debugOutput) {
        Msg("[LegacyTextureProcessor] Wrote DDS file: %s (%dx%d, %d mips)\n", outputPath.c_str(), texture.width, texture.height, mipCount);
    }
    
    return true;
}

bool TextureProcessor::GenerateRoughnessTexture(const MaterialPBRProperties& props, ConvertedTexture& outTexture) {
    // Check for roughness sources in order of preference:
    // 1. $phongexponenttexture - best source, per-pixel phong exponent
    // 2. $normalmapalphaenvmapmask - use normal map's alpha channel
    // 3. $phong with $bumpmap - Source Engine uses normal map alpha as phong mask by default!
    // 4. $basemapalphaphongmask - use base texture alpha as mask
    // 5. $envmapmask - use separate envmap mask texture
    // Otherwise, return false and let the USDA use a constant value instead
    
    if (m_debugOutput) {
        Msg("[LegacyTextureProcessor] GenerateRoughnessTexture for %s:\n", props.materialName.c_str());
        Msg("  hasPhongExpTex=%d (%s)\n", props.hasPhongExponentTexture, props.phongExponentTexturePath.c_str());
        Msg("  normMapAlphaEnvMapMask=%d, hasPhong=%d, hasBump=%d (%s)\n", props.normalMapAlphaEnvMapMask, props.hasPhong, props.hasBumpMap, props.bumpMapPath.c_str());
        Msg("  hasBaseMapAlphaPhongMask=%d, hasBaseAlphaEnvMapMask=%d\n", props.hasBaseMapAlphaPhongMask, props.hasBaseAlphaEnvMapMask);
        Msg("  hasEnvMapMask=%d (%s), hasEnvMap=%d\n", props.hasEnvMapMask, props.envMapMaskPath.c_str(), props.hasEnvMap);
        Msg("  baseTexturePath=%s\n", props.baseTexturePath.c_str());
    }
    
    std::string vtfPath;
    bool useAlphaChannel = false;
    bool isPhongExponentTexture = false;
    
    if (props.hasPhongExponentTexture && !props.phongExponentTexturePath.empty()) {
        // Use the phong exponent texture - best source!
        vtfPath = props.phongExponentTexturePath;
        useAlphaChannel = false;
        isPhongExponentTexture = true;
        if (m_debugOutput) {
            Msg("[LegacyTextureProcessor] %s: Using $phongexponenttexture for roughness (best quality)\n", props.materialName.c_str());
        }
    } else if (props.normalMapAlphaEnvMapMask && props.hasBumpMap && !props.bumpMapPath.empty()) {
        // Use the normal map's alpha channel as the roughness source (explicit flag)
        vtfPath = props.bumpMapPath;
        useAlphaChannel = true;
        if (m_debugOutput) {
            Msg("[LegacyTextureProcessor] %s: Using normal map alpha channel for roughness ($normalmapalphaenvmapmask)\n", props.materialName.c_str());
        }
    } else if (props.hasPhong && props.hasBumpMap && !props.bumpMapPath.empty()) {
        // Source Engine default behavior: when $phong is enabled, the normal map's alpha
        // channel contains the phong mask (determines which areas get specular highlights)
        // This is the DEFAULT behavior in Source, not just when $normalmapalphaenvmapmask is set
        vtfPath = props.bumpMapPath;
        useAlphaChannel = true;
        if (m_debugOutput) {
            Msg("[LegacyTextureProcessor] %s: Using normal map alpha as phong mask (default Source behavior)\n", props.materialName.c_str());
        }
    } else if (props.hasBaseMapAlphaPhongMask && !props.baseTexturePath.empty()) {
        // Use the base texture's alpha channel as phong mask
        vtfPath = props.baseTexturePath;
        useAlphaChannel = true;
        if (m_debugOutput) {
            Msg("[LegacyTextureProcessor] %s: Using base texture alpha for roughness ($basemapalphaphongmask)\n", props.materialName.c_str());
        }
    } else if (props.hasBaseAlphaEnvMapMask && !props.baseTexturePath.empty()) {
        // Use the base texture's alpha channel as envmap mask (common in LightmappedGeneric brushes)
        vtfPath = props.baseTexturePath;
        useAlphaChannel = true;
        if (m_debugOutput) {
            Msg("[LegacyTextureProcessor] %s: Using base texture alpha for roughness ($basealphaenvmapmask - LightmappedGeneric)\n", props.materialName.c_str());
        }
    } else if (props.hasEnvMapMask && !props.envMapMaskPath.empty()) {
        // Use the envmap mask texture
        vtfPath = props.envMapMaskPath;
        useAlphaChannel = false;
        if (m_debugOutput) {
            Msg("[LegacyTextureProcessor] %s: Using envmap mask for roughness\n", props.materialName.c_str());
        }
    } else if ((props.hasEnvMap || props.hasEnvMapTint) && !props.baseTexturePath.empty()) {
        // FALLBACK: If $envmap or $envmaptint is set but no explicit roughness source,
        // try using base texture alpha as envmap mask (implicit $basealphaenvmapmask behavior)
        // This handles cases where FindVar doesn't find $basealphaenvmapmask but it's set in VMT
        vtfPath = props.baseTexturePath;
        useAlphaChannel = true;
        if (m_debugOutput) {
            Msg("[LegacyTextureProcessor] %s: Trying base texture alpha as fallback roughness (implicit envmap alpha)\n", props.materialName.c_str());
        }
    } else {
        // NO REFLECTIVE PROPERTIES - don't try to generate roughness texture
        // Materials without $envmap, $phong, $envmaptint, etc. should just use constant roughness
        // This prevents matte materials from incorrectly getting roughness textures
        // No texture source for roughness - use constant in USDA
        if (m_debugOutput) {
            Msg("[LegacyTextureProcessor] No roughness texture source for %s, will use roughness constant %.2f\n",
                props.materialName.c_str(), props.roughness);
        }
        return false;
    }
    
    // Try to read the texture
    std::vector<uint8_t> fileData;
    
    if (m_debugOutput) {
        Msg("[LegacyTextureProcessor] Attempting to read texture for roughness: %s\n", vtfPath.c_str());
    }
    
    if (!ReadVTFFile(vtfPath, fileData)) {
        if (m_debugOutput) {
            Msg("[LegacyTextureProcessor] Failed to read VTF: %s (will use constant)\n", vtfPath.c_str());
        }
        return false;
    }
    
    VTFFileHeader header;
    if (!ParseVTFHeader(fileData, header)) {
        if (m_debugOutput) {
            Msg("[LegacyTextureProcessor] Failed to parse VTF header: %s\n", vtfPath.c_str());
        }
        return false;
    }
    
    ConvertedTexture sourceTex;
    if (!ExtractVTFPixelData(fileData, header, sourceTex, false)) {
        if (m_debugOutput) {
            Msg("[LegacyTextureProcessor] Failed to extract pixel data: %s\n", vtfPath.c_str());
        }
        return false;
    }
    
    // If using alpha channel, check if the alpha actually has variation
    // DXT1 textures only have 1-bit alpha (0 or 255), so they're not useful for masks
    if (useAlphaChannel) {
        bool hasAlphaVariation = false;
        uint8_t firstAlpha = sourceTex.pixelData.size() >= 4 ? sourceTex.pixelData[3] : 255;
        for (size_t i = 3; i < sourceTex.pixelData.size(); i += 4) {
            if (sourceTex.pixelData[i] != firstAlpha) {
                hasAlphaVariation = true;
                break;
            }
        }
        if (!hasAlphaVariation) {
            if (m_debugOutput) {
                Msg("[LegacyTextureProcessor] %s: Alpha channel has no variation (all %d), will use constant roughness\n", 
                    vtfPath.c_str(), firstAlpha);
            }
            return false;
        }
    }
    
    // Convert source texture to roughness
    outTexture.width = sourceTex.width;
    outTexture.height = sourceTex.height;
    outTexture.pixelData.resize(sourceTex.width * sourceTex.height * 4);
    
    for (size_t i = 0; i < sourceTex.pixelData.size(); i += 4) {
        uint8_t roughness;
        
        if (isPhongExponentTexture) {
            // $phongexponenttexture: pixel value is the phong exponent (0-255 maps to exponent)
            // Higher exponent = shinier = LOWER roughness
            // The texture typically uses the red channel (or all channels for grayscale)
            uint8_t exponentValue = sourceTex.pixelData[i];  // Red channel
            
            // Convert exponent to roughness using perceptual curve
            // Exponent 0 (value 0) = very rough (roughness ~0.85)
            // Exponent 255 (max) = very shiny (roughness ~0.20)
            float normalizedExp = exponentValue / 255.0f;
            // Use sqrt curve to maintain perceptual half-shininess at half-value
            float shininessPerceptual = sqrtf(normalizedExp);
            float roughnessF = 0.85f - (shininessPerceptual * 0.65f);  // 0->0.85, 255->0.20
            roughness = static_cast<uint8_t>(std::clamp(roughnessF * 255.0f, 0.0f, 255.0f));
        } else if (useAlphaChannel) {
            // Use alpha channel from normal map or base texture
            // This is the phong mask: bright = shiny areas = LOW roughness
            uint8_t sourceValue = sourceTex.pixelData[i + 3];
            
            // The phong mask represents "shininess" intensity (0-255)
            // Half mask value should give HALF SHININESS perception, not half roughness
            // Since roughness is roughly inverse-square to shininess perception,
            // we apply the perceptual curve to the shininess value first
            //
            // Shininess perception: half mask (127) = half shininess
            // Map: mask 0 -> low shininess -> high roughness (0.85)
            //      mask 255 -> high shininess -> low roughness (0.20)
            // 
            // Use sqrt on the mask value to preserve perceptual half-shininess at half-mask
            float normalizedMask = sourceValue / 255.0f;
            // Apply sqrt curve so half-mask gives perceptually half-shiny appearance
            float shininessPerceptual = sqrtf(normalizedMask);
            // Map to roughness range: full shininess (1.0) -> 0.20, no shininess (0.0) -> 0.85
            float roughnessF = 0.85f - (shininessPerceptual * 0.65f);
            roughness = static_cast<uint8_t>(std::clamp(roughnessF * 255.0f, 0.0f, 255.0f));
        } else {
            // Use the red channel (envmap mask)
            // Envmap mask controls environment reflections - similar to phong mask
            uint8_t sourceValue = sourceTex.pixelData[i];
            
            // Same perceptual curve as phong mask
            float normalizedMask = sourceValue / 255.0f;
            float shininessPerceptual = sqrtf(normalizedMask);
            float roughnessF = 0.85f - (shininessPerceptual * 0.65f);  // 0->0.85, 255->0.20
            roughness = static_cast<uint8_t>(std::clamp(roughnessF * 255.0f, 0.0f, 255.0f));
        }
        
        outTexture.pixelData[i + 0] = roughness;
        outTexture.pixelData[i + 1] = roughness;
        outTexture.pixelData[i + 2] = roughness;
        outTexture.pixelData[i + 3] = 255;
    }
    
    outTexture.format = REMIXAPI_FORMAT_R8G8B8A8_UNORM;
    outTexture.mipLevels = 1;
    
    if (m_debugOutput) {
        const char* sourceType = isPhongExponentTexture ? "phong exponent texture" :
                                 useAlphaChannel ? "alpha channel (phong mask)" : "envmap mask";
        Msg("[LegacyTextureProcessor] Generated roughness from %s: %s (%dx%d)\n",
            sourceType, vtfPath.c_str(), outTexture.width, outTexture.height);
    }
    return true;
}

bool TextureProcessor::GenerateMetallicTexture(const MaterialPBRProperties& props, ConvertedTexture& outTexture) {
    // Source Engine materials don't have actual metallic texture data
    // We only have estimated constants from phongboost, so always use constants in USDA
    // Return false to indicate no texture should be written
    if (m_debugOutput) {
        Msg("[LegacyTextureProcessor] No metallic texture for %s, will use metallic constant %.2f\n",
            props.materialName.c_str(), props.metallic);
    }
    return false;
}

bool TextureProcessor::ReadVTFFile(const std::string& path, std::vector<uint8_t>& outData) {
    if (!m_fileSystem) {
        Warning("[LegacyTextureProcessor] Filesystem not available\n");
        return false;
    }
    
    // Build full path
    std::string fullPath = "materials/" + path;
    if (fullPath.find(".vtf") == std::string::npos) {
        fullPath += ".vtf";
    }
    
    // Open file
    FileHandle_t file = m_fileSystem->Open(fullPath.c_str(), "rb", "GAME");
    if (!file) {
        // Try without materials/ prefix
        file = m_fileSystem->Open(path.c_str(), "rb", "GAME");
        if (!file) {
            if (m_debugOutput) {
                Msg("[LegacyTextureProcessor] Could not open VTF: %s\n", fullPath.c_str());
            }
            return false;
        }
    }
    
    // Get file size
    int fileSize = m_fileSystem->Size(file);
    if (fileSize <= 0 || static_cast<size_t>(fileSize) > MAX_VTF_FILE_SIZE) {
        m_fileSystem->Close(file);
        Warning("[LegacyTextureProcessor] Invalid file size for %s: %d\n", fullPath.c_str(), fileSize);
        return false;
    }
    
    // Read file data
    outData.resize(fileSize);
    int bytesRead = m_fileSystem->Read(outData.data(), fileSize, file);
    m_fileSystem->Close(file);
    
    if (bytesRead != fileSize) {
        Warning("[LegacyTextureProcessor] Read error for %s: expected %d, got %d\n", 
                fullPath.c_str(), fileSize, bytesRead);
        return false;
    }
    
    return true;
}

bool TextureProcessor::ParseVTFHeader(const std::vector<uint8_t>& fileData, VTFFileHeader& outHeader) {
    if (fileData.size() < sizeof(VTFFileHeader)) {
        Warning("[LegacyTextureProcessor] File too small for VTF header\n");
        return false;
    }
    
    memcpy(&outHeader, fileData.data(), sizeof(VTFFileHeader));
    
    // Verify signature
    if (memcmp(outHeader.signature, "VTF\0", 4) != 0) {
        Warning("[LegacyTextureProcessor] Invalid VTF signature\n");
        return false;
    }
    
    // Verify version (support 7.0 - 7.5)
    if (outHeader.version[0] != VTF_MAJOR_VERSION_SUPPORTED || outHeader.version[1] > VTF_MAX_MINOR_VERSION) {
        Warning("[LegacyTextureProcessor] Unsupported VTF version %d.%d\n", 
                outHeader.version[0], outHeader.version[1]);
        return false;
    }
    
    if (m_debugOutput) {
        Msg("[LegacyTextureProcessor] VTF: %dx%d, format %d, %d mips\n",
            outHeader.width, outHeader.height, outHeader.imageFormat, outHeader.mipmapCount);
    }
    
    return true;
}

// DXT1 block decompression helper
static void DecompressDXT1Block(const uint8_t* block, uint8_t* output, int stride) {
    uint16_t c0 = block[0] | (block[1] << 8);
    uint16_t c1 = block[2] | (block[3] << 8);
    
    uint8_t colors[4][4]; // RGBA
    
    // Extract RGB565 colors
    colors[0][0] = ((c0 >> 11) & 0x1F) * 255 / 31;  // R
    colors[0][1] = ((c0 >> 5) & 0x3F) * 255 / 63;   // G
    colors[0][2] = (c0 & 0x1F) * 255 / 31;          // B
    colors[0][3] = 255;
    
    colors[1][0] = ((c1 >> 11) & 0x1F) * 255 / 31;
    colors[1][1] = ((c1 >> 5) & 0x3F) * 255 / 63;
    colors[1][2] = (c1 & 0x1F) * 255 / 31;
    colors[1][3] = 255;
    
    if (c0 > c1) {
        // 4-color mode
        colors[2][0] = (2 * colors[0][0] + colors[1][0]) / 3;
        colors[2][1] = (2 * colors[0][1] + colors[1][1]) / 3;
        colors[2][2] = (2 * colors[0][2] + colors[1][2]) / 3;
        colors[2][3] = 255;
        
        colors[3][0] = (colors[0][0] + 2 * colors[1][0]) / 3;
        colors[3][1] = (colors[0][1] + 2 * colors[1][1]) / 3;
        colors[3][2] = (colors[0][2] + 2 * colors[1][2]) / 3;
        colors[3][3] = 255;
    } else {
        // 3-color + transparent mode
        colors[2][0] = (colors[0][0] + colors[1][0]) / 2;
        colors[2][1] = (colors[0][1] + colors[1][1]) / 2;
        colors[2][2] = (colors[0][2] + colors[1][2]) / 2;
        colors[2][3] = 255;
        
        colors[3][0] = 0;
        colors[3][1] = 0;
        colors[3][2] = 0;
        colors[3][3] = 0;
    }
    
    uint32_t indices = block[4] | (block[5] << 8) | (block[6] << 16) | (block[7] << 24);
    
    for (int y = 0; y < 4; y++) {
        for (int x = 0; x < 4; x++) {
            int idx = indices & 0x3;
            indices >>= 2;
            
            int offset = y * stride + x * 4;
            output[offset + 0] = colors[idx][0];
            output[offset + 1] = colors[idx][1];
            output[offset + 2] = colors[idx][2];
            output[offset + 3] = colors[idx][3];
        }
    }
}

// DXT5 alpha block decompression helper
static void DecompressDXT5AlphaBlock(const uint8_t* block, uint8_t* output, int stride) {
    uint8_t a0 = block[0];
    uint8_t a1 = block[1];
    
    uint8_t alphas[8];
    alphas[0] = a0;
    alphas[1] = a1;
    
    if (a0 > a1) {
        alphas[2] = (6 * a0 + 1 * a1) / 7;
        alphas[3] = (5 * a0 + 2 * a1) / 7;
        alphas[4] = (4 * a0 + 3 * a1) / 7;
        alphas[5] = (3 * a0 + 4 * a1) / 7;
        alphas[6] = (2 * a0 + 5 * a1) / 7;
        alphas[7] = (1 * a0 + 6 * a1) / 7;
    } else {
        alphas[2] = (4 * a0 + 1 * a1) / 5;
        alphas[3] = (3 * a0 + 2 * a1) / 5;
        alphas[4] = (2 * a0 + 3 * a1) / 5;
        alphas[5] = (1 * a0 + 4 * a1) / 5;
        alphas[6] = 0;
        alphas[7] = 255;
    }
    
    // Unpack 48-bit index data
    uint64_t indices = 0;
    for (int i = 0; i < 6; i++) {
        indices |= ((uint64_t)block[2 + i]) << (i * 8);
    }
    
    for (int y = 0; y < 4; y++) {
        for (int x = 0; x < 4; x++) {
            int idx = indices & 0x7;
            indices >>= 3;
            
            int offset = y * stride + x * 4 + 3; // Alpha channel
            output[offset] = alphas[idx];
        }
    }
}

bool TextureProcessor::DecompressDXT1(const uint8_t* compressedData, uint32_t width, uint32_t height,
                                          std::vector<uint8_t>& outRGBA) {
    uint32_t blocksX = (width + 3) / 4;
    uint32_t blocksY = (height + 3) / 4;
    
    outRGBA.resize(width * height * 4);
    
    const uint8_t* blockPtr = compressedData;
    
    for (uint32_t by = 0; by < blocksY; by++) {
        for (uint32_t bx = 0; bx < blocksX; bx++) {
            uint8_t blockPixels[4 * 4 * 4]; // 4x4 RGBA
            DecompressDXT1Block(blockPtr, blockPixels, 4 * 4);
            blockPtr += 8; // DXT1 block is 8 bytes
            
            // Copy to output
            for (int py = 0; py < 4; py++) {
                int y = by * 4 + py;
                if (y >= (int)height) continue;
                
                for (int px = 0; px < 4; px++) {
                    int x = bx * 4 + px;
                    if (x >= (int)width) continue;
                    
                    int srcOffset = py * 16 + px * 4;
                    int dstOffset = (y * width + x) * 4;
                    
                    outRGBA[dstOffset + 0] = blockPixels[srcOffset + 0];
                    outRGBA[dstOffset + 1] = blockPixels[srcOffset + 1];
                    outRGBA[dstOffset + 2] = blockPixels[srcOffset + 2];
                    outRGBA[dstOffset + 3] = blockPixels[srcOffset + 3];
                }
            }
        }
    }
    
    return true;
}

bool TextureProcessor::DecompressDXT5(const uint8_t* compressedData, uint32_t width, uint32_t height,
                                          std::vector<uint8_t>& outRGBA) {
    uint32_t blocksX = (width + 3) / 4;
    uint32_t blocksY = (height + 3) / 4;
    
    outRGBA.resize(width * height * 4);
    
    const uint8_t* blockPtr = compressedData;
    
    for (uint32_t by = 0; by < blocksY; by++) {
        for (uint32_t bx = 0; bx < blocksX; bx++) {
            uint8_t blockPixels[4 * 4 * 4]; // 4x4 RGBA
            
            // First 8 bytes: alpha block - this sets the alpha channel
            DecompressDXT5AlphaBlock(blockPtr, blockPixels, 4 * 4);
            blockPtr += 8;
            
            // Save alpha values before color decompression (DXT1Block overwrites alpha)
            uint8_t savedAlpha[16];
            for (int i = 0; i < 16; i++) {
                savedAlpha[i] = blockPixels[i * 4 + 3];
            }
            
            // Next 8 bytes: color block (DXT1) - this overwrites alpha with 255
            DecompressDXT1Block(blockPtr, blockPixels, 4 * 4);
            blockPtr += 8;
            
            // Restore alpha values from DXT5 alpha block
            for (int i = 0; i < 16; i++) {
                blockPixels[i * 4 + 3] = savedAlpha[i];
            }
            
            // Copy to output
            for (int py = 0; py < 4; py++) {
                int y = by * 4 + py;
                if (y >= (int)height) continue;
                
                for (int px = 0; px < 4; px++) {
                    int x = bx * 4 + px;
                    if (x >= (int)width) continue;
                    
                    int srcOffset = py * 16 + px * 4;
                    int dstOffset = (y * width + x) * 4;
                    
                    outRGBA[dstOffset + 0] = blockPixels[srcOffset + 0];
                    outRGBA[dstOffset + 1] = blockPixels[srcOffset + 1];
                    outRGBA[dstOffset + 2] = blockPixels[srcOffset + 2];
                    outRGBA[dstOffset + 3] = blockPixels[srcOffset + 3];
                }
            }
        }
    }
    
    return true;
}

// Calculate image data size for a given format
static size_t GetImageDataSize(uint32_t width, uint32_t height, VTFImageFormat format) {
    uint32_t blocksX = (width + 3) / 4;
    uint32_t blocksY = (height + 3) / 4;
    
    switch (format) {
        case IMAGE_FORMAT_DXT1:
        case IMAGE_FORMAT_DXT1_ONEBITALPHA:
            return blocksX * blocksY * 8;
        case IMAGE_FORMAT_DXT3:
        case IMAGE_FORMAT_DXT5:
            return blocksX * blocksY * 16;
        case IMAGE_FORMAT_RGBA8888:
        case IMAGE_FORMAT_ABGR8888:
        case IMAGE_FORMAT_ARGB8888:
        case IMAGE_FORMAT_BGRA8888:
        case IMAGE_FORMAT_BGRX8888:
        case IMAGE_FORMAT_UVWQ8888:
        case IMAGE_FORMAT_UVLX8888:
            return width * height * 4;
        case IMAGE_FORMAT_RGB888:
        case IMAGE_FORMAT_BGR888:
        case IMAGE_FORMAT_RGB888_BLUESCREEN:
        case IMAGE_FORMAT_BGR888_BLUESCREEN:
            return width * height * 3;
        case IMAGE_FORMAT_RGB565:
        case IMAGE_FORMAT_BGR565:
        case IMAGE_FORMAT_BGRA5551:
        case IMAGE_FORMAT_BGRX5551:
        case IMAGE_FORMAT_BGRA4444:
        case IMAGE_FORMAT_IA88:
        case IMAGE_FORMAT_UV88:
            return width * height * 2;
        case IMAGE_FORMAT_I8:
        case IMAGE_FORMAT_P8:
        case IMAGE_FORMAT_A8:
            return width * height;
        case IMAGE_FORMAT_RGBA16161616F:
        case IMAGE_FORMAT_RGBA16161616:
            return width * height * 8;
        default:
            return 0;
    }
}

// Calculate total size of all mipmaps
static size_t GetTotalMipmapSize(uint32_t width, uint32_t height, uint8_t mipmapCount, VTFImageFormat format) {
    size_t totalSize = 0;
    uint32_t w = width;
    uint32_t h = height;
    
    for (int i = 0; i < mipmapCount; i++) {
        totalSize += GetImageDataSize(w, h, format);
        w = max(1u, w / 2);
        h = max(1u, h / 2);
    }
    
    return totalSize;
}

bool TextureProcessor::ExtractVTFPixelData(const std::vector<uint8_t>& fileData, 
                                               const VTFFileHeader& header,
                                               ConvertedTexture& outTexture, 
                                               bool isNormalMap) {
    outTexture.width = header.width;
    outTexture.height = header.height;
    outTexture.mipLevels = 1; // We only extract the largest mip for now
    outTexture.isNormalMap = isNormalMap;
    outTexture.format = isNormalMap ? REMIXAPI_FORMAT_R8G8B8A8_UNORM : REMIXAPI_FORMAT_R8G8B8A8_SRGB;
    
    VTFImageFormat srcFormat = (VTFImageFormat)header.imageFormat;
    
    // Calculate offset to high-res image data
    // VTF stores: header, low-res image (optional), mipmaps from smallest to largest
    size_t dataOffset = header.headerSize;
    
    // Skip low-res thumbnail if present
    if (header.lowResImageFormat != (uint32_t)IMAGE_FORMAT_NONE && 
        header.lowResImageFormat != 0xFFFFFFFF) {
        size_t lowResSize = GetImageDataSize(header.lowResImageWidth, header.lowResImageHeight, 
                                             (VTFImageFormat)header.lowResImageFormat);
        dataOffset += lowResSize;
    }
    
    // Get mipmap count (at least 1)
    uint8_t mipmapCount = header.mipmapCount > 0 ? header.mipmapCount : 1;
    uint16_t frameCount = header.frames > 0 ? header.frames : 1;
    
    // VTF stores mipmaps from smallest to largest
    // We need to skip all smaller mipmaps and all frames except the first
    // The data order is: for each mip level (smallest to largest): for each frame: image data
    
    // Calculate total size of all mipmaps for ONE frame
    size_t totalMipSizeOneFrame = GetTotalMipmapSize(header.width, header.height, mipmapCount, srcFormat);
    
    // For the largest mip (mip 0), skip all smaller mipmaps (mipmaps mipmapCount-1 down to 1)
    size_t smallerMipsSize = 0;
    for (int mip = mipmapCount - 1; mip >= 1; mip--) {
        uint32_t mipW = max(1u, header.width >> mip);
        uint32_t mipH = max(1u, header.height >> mip);
        // Each mip level has frameCount frames
        smallerMipsSize += GetImageDataSize(mipW, mipH, srcFormat) * frameCount;
    }
    
    dataOffset += smallerMipsSize;
    
    // Now we're at the start of the largest mip (mip 0) for frame 0
    // Calculate size of largest mip
    size_t largestMipSize = GetImageDataSize(header.width, header.height, srcFormat);
    
    if (dataOffset + largestMipSize > fileData.size()) {
        // Try alternative: maybe the file only has data for one frame with no low-res image
        dataOffset = header.headerSize;
        for (int mip = mipmapCount - 1; mip >= 1; mip--) {
            uint32_t mipW = max(1u, header.width >> mip);
            uint32_t mipH = max(1u, header.height >> mip);
            dataOffset += GetImageDataSize(mipW, mipH, srcFormat);
        }
        
        if (dataOffset + largestMipSize > fileData.size()) {
            if (m_debugOutput) {
                Msg("[LegacyTextureProcessor] VTF data too small: offset %zu + size %zu > total %zu\n",
                    dataOffset, largestMipSize, fileData.size());
            }
            return false;
        }
    }
    
    const uint8_t* imageData = fileData.data() + dataOffset;
    
    // Convert to RGBA8888
    std::vector<uint8_t> rgba;
    
    switch (srcFormat) {
        case IMAGE_FORMAT_DXT1:
        case IMAGE_FORMAT_DXT1_ONEBITALPHA:
            if (!DecompressDXT1(imageData, header.width, header.height, rgba)) {
                return false;
            }
            break;
            
        case IMAGE_FORMAT_DXT5:
            if (!DecompressDXT5(imageData, header.width, header.height, rgba)) {
                return false;
            }
            break;
            
        case IMAGE_FORMAT_RGBA8888:
            rgba.assign(imageData, imageData + header.width * header.height * 4);
            break;
            
        case IMAGE_FORMAT_BGRA8888:
        case IMAGE_FORMAT_BGRX8888:
            rgba.resize(header.width * header.height * 4);
            for (size_t i = 0; i < header.width * header.height; i++) {
                rgba[i * 4 + 0] = imageData[i * 4 + 2]; // R from B
                rgba[i * 4 + 1] = imageData[i * 4 + 1]; // G
                rgba[i * 4 + 2] = imageData[i * 4 + 0]; // B from R
                rgba[i * 4 + 3] = (srcFormat == IMAGE_FORMAT_BGRX8888) ? 255 : imageData[i * 4 + 3];
            }
            break;
            
        case IMAGE_FORMAT_RGB888:
            rgba.resize(header.width * header.height * 4);
            for (size_t i = 0; i < header.width * header.height; i++) {
                rgba[i * 4 + 0] = imageData[i * 3 + 0];
                rgba[i * 4 + 1] = imageData[i * 3 + 1];
                rgba[i * 4 + 2] = imageData[i * 3 + 2];
                rgba[i * 4 + 3] = 255;
            }
            break;
            
        case IMAGE_FORMAT_BGR888:
            rgba.resize(header.width * header.height * 4);
            for (size_t i = 0; i < header.width * header.height; i++) {
                rgba[i * 4 + 0] = imageData[i * 3 + 2];
                rgba[i * 4 + 1] = imageData[i * 3 + 1];
                rgba[i * 4 + 2] = imageData[i * 3 + 0];
                rgba[i * 4 + 3] = 255;
            }
            break;
            
        default:
            if (m_debugOutput) {
                Msg("[LegacyTextureProcessor] Unsupported VTF format: %d\n", srcFormat);
            }
            return false;
    }
    
    outTexture.pixelData = std::move(rgba);
    
    // If this is a normal map, convert to octahedral format for RTX Remix
    if (isNormalMap) {
        ConvertNormalMapToOctahedral(outTexture);
    }
    
    return true;
}

// Convert DirectX-style normal map to hemispherical octahedral format for RTX Remix
// Based on NVIDIA's LightspeedOctahedralConverter
// See: https://github.com/NVIDIAGameWorks/dxvk-remix
void TextureProcessor::ConvertNormalMapToOctahedral(ConvertedTexture& texture) {
    if (texture.pixelData.empty()) return;
    
    uint32_t width = texture.width;
    uint32_t height = texture.height;
    size_t pixelCount = width * height;
    
    std::vector<uint8_t> octahedralData(pixelCount * 4);
    
    for (size_t i = 0; i < pixelCount; i++) {
        size_t srcIdx = i * 4;
        size_t dstIdx = i * 4;
        
        // Read RGB as normal components
        uint8_t r = texture.pixelData[srcIdx + 0];
        uint8_t g = texture.pixelData[srcIdx + 1];
        uint8_t b = texture.pixelData[srcIdx + 2];
        
        // Check for inward-pointing normals (z < 0, b < 128) and flip them
        // RTX Remix only supports hemispherical normals pointing away from surface
        if (b < 128) {
            b = 255 - b;
        }
        
        // Convert from [0, 255] to [-1, 1] range
        float nx = (r / 255.0f) * 2.0f - 1.0f;
        float ny = (g / 255.0f) * 2.0f - 1.0f;
        float nz = (b / 255.0f) * 2.0f - 1.0f;
        
        // Normalize the vector
        float len = std::sqrt(nx * nx + ny * ny + nz * nz);
        if (len > 0.0001f) {
            nx /= len;
            ny /= len;
            nz /= len;
        } else {
            // Default to straight up if invalid
            nx = 0.0f;
            ny = 0.0f;
            nz = 1.0f;
        }
        
        // Convert to octahedral encoding
        // snorm_octahedral = xy / (|x| + |y| + |z|)
        float absSum = std::abs(nx) + std::abs(ny) + std::abs(nz);
        float octX = nx / absSum;
        float octY = ny / absSum;
        
        // Hemispherical encoding (for normals pointing outward, z >= 0)
        // result.x = octX + octY
        // result.y = octX - octY
        float resultX = octX + octY;
        float resultY = octX - octY;
        
        // Convert from [-1, 1] to [0, 1]
        resultX = resultX * 0.5f + 0.5f;
        resultY = resultY * 0.5f + 0.5f;
        
        // Convert to [0, 255] with rounding
        uint8_t outR = static_cast<uint8_t>(std::clamp(resultX * 255.0f + 0.5f, 0.0f, 255.0f));
        uint8_t outG = static_cast<uint8_t>(std::clamp(resultY * 255.0f + 0.5f, 0.0f, 255.0f));
        
        // Store as RGB with B=0 (octahedral only uses 2 channels)
        octahedralData[dstIdx + 0] = outR;
        octahedralData[dstIdx + 1] = outG;
        octahedralData[dstIdx + 2] = 0;
        octahedralData[dstIdx + 3] = 255;
    }
    
    texture.pixelData = std::move(octahedralData);
    
    if (m_debugOutput) {
        Msg("[LegacyTextureProcessor] Converted normal map to octahedral format (%dx%d)\n", width, height);
    }
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
    // Default to fairly rough (most Source materials without phong are matte)
    if (phongExponent <= 0) return 0.75f;
    
    // Clamp to reasonable range
    phongExponent = std::clamp(phongExponent, 1.0f, 256.0f);
    
    // Source Engine phong materials have specular highlights, but they're not mirrors.
    // PBR roughness 0.35-0.40 is "smooth plastic" territory - appropriate for game assets.
    // 
    // phongExponent 1 -> roughness ~0.75 (fairly broad highlight)
    // phongExponent 25 -> roughness ~0.55 (moderate)
    // phongExponent 50 -> roughness ~0.45 (fairly smooth)
    // phongExponent 150 -> roughness ~0.35 (smooth plastic)
    // phongExponent 256 -> roughness ~0.30 (very smooth, but not mirror)
    
    float roughness = 0.8f - (std::log(phongExponent) / std::log(300.0f)) * 0.5f;
    
    return std::clamp(roughness, 0.30f, 0.85f);
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
            
            return std::clamp(roughness, 0.30f, 0.75f);
        }
        // No phong and no envmap - just a matte surface
        return 0.85f;
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
    
    return std::clamp(roughness, 0.30f, 0.85f);
}

float TextureProcessor::EstimateMetallic(const MaterialPBRProperties& props) {
    float metallic = 0.0f;
    
    // High phong boost suggests metal-like reflections
    if (props.phongBoost > 2.0f) {
        metallic = std::clamp((props.phongBoost - 2.0f) / 8.0f, 0.0f, 0.5f);
    }
    
    // Having an envmap mask suggests reflective surface
    if (props.hasEnvMapMask) {
        metallic = max(metallic, 0.2f);
    }
    
    return metallic;
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
    
    // Get $basetexture - try string value first, then texture name
    bool found = false;
    IMaterialVar* pVar = pMaterial->FindVar("$basetexture", &found, false);
    if (found && pVar) {
        std::string texPath;
        // Try to get the string value (path from VMT) first
        const char* strVal = pVar->GetStringValue();
        if (strVal && strVal[0] != '\0') {
            texPath = strVal;
        }
        // If string is empty or invalid, try texture name
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
            Msg("[LegacyTextureProcessor] %s: $basetexture = %s\n", materialName.c_str(), outProps.baseTexturePath.c_str());
        } else if (m_debugOutput && strVal) {
            Msg("[LegacyTextureProcessor] %s: $basetexture filtered (invalid path: '%s')\n", materialName.c_str(), strVal);
        }
    }
    
    // Get $bumpmap - try string value first, then texture name
    pVar = pMaterial->FindVar("$bumpmap", &found, false);
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
            Msg("[LegacyTextureProcessor] %s: $bumpmap = %s\n", materialName.c_str(), outProps.bumpMapPath.c_str());
        } else if (m_debugOutput && strVal) {
            Msg("[LegacyTextureProcessor] %s: $bumpmap filtered (invalid path: '%s')\n", materialName.c_str(), strVal);
        }
    }
    
    // Get $envmapmask - try string value first, then texture name
    pVar = pMaterial->FindVar("$envmapmask", &found, false);
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
    
    // Get $envmap (to check if envmapping is enabled)
    pVar = pMaterial->FindVar("$envmap", &found, false);
    if (found && pVar) {
        const char* strVal = pVar->GetStringValue();
        // Check if it has any value (env_cubemap, or a texture path) - but filter UNDEFINED
        if (strVal && strVal[0] != '\0' && strcmp(strVal, "UNDEFINED") != 0) {
            outProps.hasEnvMap = true;
            if (m_debugOutput) {
                Msg("[LegacyTextureProcessor] %s: $envmap = %s\n", materialName.c_str(), strVal);
            }
        } else if (m_debugOutput && strVal && strcmp(strVal, "UNDEFINED") == 0) {
            Msg("[LegacyTextureProcessor] %s: $envmap = UNDEFINED (ignored)\n", materialName.c_str());
        }
    }
    
    // Get $normalmapalphaenvmapmask - use normal map's alpha as envmap mask for roughness
    pVar = pMaterial->FindVar("$normalmapalphaenvmapmask", &found, false);
    if (found && pVar) {
        outProps.normalMapAlphaEnvMapMask = (pVar->GetIntValue() == 1);
        if (m_debugOutput && outProps.normalMapAlphaEnvMapMask) {
            Msg("[LegacyTextureProcessor] %s: $normalmapalphaenvmapmask = 1 (will use normal alpha for roughness)\n", materialName.c_str());
        }
    }
    
    // Get $phong (check if phong is enabled)
    pVar = pMaterial->FindVar("$phong", &found, false);
    if (found && pVar) {
        outProps.hasPhong = (pVar->GetIntValue() == 1);
    }
    
    // Get $phongexponent
    pVar = pMaterial->FindVar("$phongexponent", &found, false);
    if (found && pVar) {
        outProps.phongExponent = pVar->GetFloatValue();
        if (!outProps.hasPhong) outProps.hasPhong = true;
    }
    
    // Get $phongboost
    pVar = pMaterial->FindVar("$phongboost", &found, false);
    if (found && pVar) {
        outProps.phongBoost = pVar->GetFloatValue();
    }
    
    // Get $phongexponenttexture - per-pixel phong exponent texture (very useful for roughness!)
    pVar = pMaterial->FindVar("$phongexponenttexture", &found, false);
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
            // Also try getting the texture name directly (some materials use texture references)
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
    
    // Get $basemapalphaphongmask - use base texture alpha as phong mask
    pVar = pMaterial->FindVar("$basemapalphaphongmask", &found, false);
    if (found && pVar) {
        outProps.hasBaseMapAlphaPhongMask = (pVar->GetIntValue() == 1);
        if (m_debugOutput && outProps.hasBaseMapAlphaPhongMask) {
            Msg("[LegacyTextureProcessor] %s: $basemapalphaphongmask = 1\n", materialName.c_str());
        }
    }
    
    // Get $basealphaenvmapmask - use base texture alpha as envmap mask (common in LightmappedGeneric)
    pVar = pMaterial->FindVar("$basealphaenvmapmask", &found, false);
    if (found && pVar) {
        // Try multiple ways to get the value - Source Engine can be inconsistent
        int intVal = pVar->GetIntValue();
        float floatVal = pVar->GetFloatValue();
        const char* strVal = pVar->GetStringValue();
        
        // Accept any truthy value
        bool isTruthy = (intVal != 0) || (floatVal != 0.0f);
        if (!isTruthy && strVal && strVal[0] != '\0') {
            // Try parsing string - could be "1", " 1", "1.0", etc.
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
    
    // Get $phongfresnelranges "[x y z]"
    pVar = pMaterial->FindVar("$phongfresnelranges", &found, false);
    if (found && pVar) {
        // Get vector value from the material var
        const char* strVal = pVar->GetStringValue();
        if (strVal && strVal[0] != '\0') {
            // Parse "[x y z]" format
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
    
    // Get $envmaptint "[r g b]"
    pVar = pMaterial->FindVar("$envmaptint", &found, false);
    if (found && pVar) {
        const char* strVal = pVar->GetStringValue();
        if (strVal && strVal[0] != '\0') {
            // Parse "[r g b]" format
            float r = 1, g = 1, b = 1;
            if (sscanf(strVal, "[%f %f %f]", &r, &g, &b) == 3 ||
                sscanf(strVal, "%f %f %f", &r, &g, &b) == 3) {
                outProps.envMapTint[0] = r;
                outProps.envMapTint[1] = g;
                outProps.envMapTint[2] = b;
                outProps.hasEnvMapTint = true;
                // If envmaptint is set, envmap must be enabled even if FindVar didn't find $envmap
                if (!outProps.hasEnvMap) {
                    outProps.hasEnvMap = true;
                    if (m_debugOutput) {
                        Msg("[LegacyTextureProcessor] %s: Setting hasEnvMap=true based on $envmaptint presence\n", materialName.c_str());
                    }
                }
                if (m_debugOutput) {
                    Msg("[LegacyTextureProcessor] %s: $envmaptint = [%.2f %.2f %.2f]\n", 
                        materialName.c_str(), r, g, b);
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
    
    // Get $selfillum
    pVar = pMaterial->FindVar("$selfillum", &found, false);
    if (found && pVar) {
        outProps.isSelfIllum = (pVar->GetIntValue() == 1);
    }
    
    // Get $translucent
    pVar = pMaterial->FindVar("$translucent", &found, false);
    if (found && pVar) {
        outProps.isTranslucent = (pVar->GetIntValue() == 1);
    }
    
    // Calculate PBR values with enhanced logic
    outProps.roughness = CalculateRoughness(outProps);
    outProps.metallic = EstimateMetallic(outProps);
    
    if (m_debugOutput) {
        Msg("[LegacyTextureProcessor] %s extracted: hasPhong=%d, phongExp=%.0f, hasBump=%d, hasEnvMask=%d, hasPhongExpTex=%d, hasEnvMapTint=%d, hasEnvMap=%d\n",
            materialName.c_str(), outProps.hasPhong, outProps.phongExponent, outProps.hasBumpMap, 
            outProps.hasEnvMapMask, outProps.hasPhongExponentTexture, outProps.hasEnvMapTint, outProps.hasEnvMap);
    }
    
    return true;
}


// Helper function to check if a file exists
static bool FileExists(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    return file.good();
}

bool TextureProcessor::CreatePBRMaterial(const MaterialPBRProperties& props, uint64_t textureHash) {
    if (textureHash == 0) {
        return false;
    }
    
    // Check if we've already created a material for this hash
    if (m_processedMaterialInfo.find(textureHash) != m_processedMaterialInfo.end()) {
        return true; // Already done
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
    
    int skippedCount = 0;
    
    // Write normal map to disk if available
    if (props.hasBumpMap && !props.bumpMapPath.empty()) {
        // Generate the expected output path first to check if it exists
        uint64_t normalHash = GenerateTextureHash(props.bumpMapPath + "_normal", 0, 0);
        std::string expectedOutputPath = GenerateOutputPath(normalHash, "_normal");
        
        // Check if the file already exists
        if (FileExists(expectedOutputPath)) {
            // File already exists, just use it
            matInfo.normalPath = expectedOutputPath;
            m_writtenTexturePaths[normalHash] = expectedOutputPath;
            skippedCount++;
            
            if (m_debugOutput) {
                Msg("[LegacyTextureProcessor] Skipping existing normal texture: %s\n", expectedOutputPath.c_str());
            }
        } else {
            std::string vtfPath = props.bumpMapPath;
            std::vector<uint8_t> fileData;
            
            if (ReadVTFFile(vtfPath, fileData)) {
                VTFFileHeader header;
                if (ParseVTFHeader(fileData, header)) {
                    ConvertedTexture normalTex;
                    normalTex.isNormalMap = true;
                    if (ExtractVTFPixelData(fileData, header, normalTex, true)) {
                        normalTex.hash = GenerateTextureHash(props.bumpMapPath + "_normal", normalTex.width, normalTex.height);
                        std::string outputPath = GenerateOutputPath(normalTex.hash, "_normal");
                        
                        // Double-check with actual dimensions hash
                        if (FileExists(outputPath)) {
                            matInfo.normalPath = outputPath;
                            m_writtenTexturePaths[normalTex.hash] = outputPath;
                            skippedCount++;
                            
                            if (m_debugOutput) {
                                Msg("[LegacyTextureProcessor] Skipping existing normal texture: %s\n", outputPath.c_str());
                            }
                        } else if (WriteTextureToDDS(normalTex, outputPath)) {
                            matInfo.normalPath = outputPath;
                            m_writtenTexturePaths[normalTex.hash] = outputPath;
                            m_stats.materialsWithNormals++;
                            
                            if (m_debugOutput) {
                                Msg("[LegacyTextureProcessor] Wrote normal texture: %s\n", outputPath.c_str());
                            }
                        } else if (m_debugOutput) {
                            Msg("[LegacyTextureProcessor] Failed to write normal DDS for %s\n", props.bumpMapPath.c_str());
                        }
                    } else if (m_debugOutput) {
                        Msg("[LegacyTextureProcessor] Failed to extract pixel data for %s\n", props.bumpMapPath.c_str());
                    }
                } else if (m_debugOutput) {
                    Msg("[LegacyTextureProcessor] Failed to parse VTF header for %s\n", props.bumpMapPath.c_str());
                }
            } else if (m_debugOutput) {
                Msg("[LegacyTextureProcessor] Failed to read VTF file for bump map: %s\n", props.bumpMapPath.c_str());
            }
        }
    }
    
    // Generate and write roughness texture
    {
        uint64_t roughnessHash = GenerateTextureHash(props.materialName + "_roughness", 256, 256);
        std::string outputPath = GenerateOutputPath(roughnessHash, "_rough");
        
        // Check if the file already exists
        if (FileExists(outputPath)) {
            matInfo.roughnessPath = outputPath;
            m_writtenTexturePaths[roughnessHash] = outputPath;
            skippedCount++;
            
            if (m_debugOutput) {
                Msg("[LegacyTextureProcessor] Skipping existing roughness texture: %s\n", outputPath.c_str());
            }
        } else {
            ConvertedTexture roughnessTex;
            if (GenerateRoughnessTexture(props, roughnessTex)) {
                roughnessTex.hash = GenerateTextureHash(props.materialName + "_roughness", roughnessTex.width, roughnessTex.height);
                outputPath = GenerateOutputPath(roughnessTex.hash, "_rough");
                
                if (FileExists(outputPath)) {
                    matInfo.roughnessPath = outputPath;
                    m_writtenTexturePaths[roughnessTex.hash] = outputPath;
                    skippedCount++;
                    
                    if (m_debugOutput) {
                        Msg("[LegacyTextureProcessor] Skipping existing roughness texture: %s\n", outputPath.c_str());
                    }
                } else if (WriteTextureToDDS(roughnessTex, outputPath)) {
                    matInfo.roughnessPath = outputPath;
                    m_writtenTexturePaths[roughnessTex.hash] = outputPath;
                    m_stats.materialsWithRoughness++;
                    
                    if (m_debugOutput) {
                        Msg("[LegacyTextureProcessor] Wrote roughness texture: %s\n", outputPath.c_str());
                    }
                }
            }
        }
    }
    
    // Generate and write metallic texture if significant
    if (props.metallic > 0.05f) {
        uint64_t metallicHash = GenerateTextureHash(props.materialName + "_metallic", 256, 256);
        std::string outputPath = GenerateOutputPath(metallicHash, "_metal");
        
        // Check if the file already exists
        if (FileExists(outputPath)) {
            matInfo.metallicPath = outputPath;
            m_writtenTexturePaths[metallicHash] = outputPath;
            skippedCount++;
            
            if (m_debugOutput) {
                Msg("[LegacyTextureProcessor] Skipping existing metallic texture: %s\n", outputPath.c_str());
            }
        } else {
            ConvertedTexture metallicTex;
            if (GenerateMetallicTexture(props, metallicTex)) {
                metallicTex.hash = GenerateTextureHash(props.materialName + "_metallic", metallicTex.width, metallicTex.height);
                outputPath = GenerateOutputPath(metallicTex.hash, "_metal");
                
                if (FileExists(outputPath)) {
                    matInfo.metallicPath = outputPath;
                    m_writtenTexturePaths[metallicTex.hash] = outputPath;
                    skippedCount++;
                    
                    if (m_debugOutput) {
                        Msg("[LegacyTextureProcessor] Skipping existing metallic texture: %s\n", outputPath.c_str());
                    }
                } else if (WriteTextureToDDS(metallicTex, outputPath)) {
                    matInfo.metallicPath = outputPath;
                    m_writtenTexturePaths[metallicTex.hash] = outputPath;
                    
                    if (m_debugOutput) {
                        Msg("[LegacyTextureProcessor] Wrote metallic texture: %s\n", outputPath.c_str());
                    }
                }
            }
        }
    }
    
    // Store for USDA generation
    m_processedMaterialInfo[textureHash] = matInfo;
    m_stats.materialsProcessed++;
    
    if (m_debugOutput) {
        Msg("[LegacyTextureProcessor] Processed material '%s' (hash 0x%llX): roughness=%.2f, metallic=%.2f%s%s%s%s\n",
            props.materialName.c_str(), textureHash, props.roughness, props.metallic,
            !matInfo.normalPath.empty() ? " [normal]" : "",
            !matInfo.roughnessPath.empty() ? " [roughness]" : "",
            !matInfo.metallicPath.empty() ? " [metallic]" : "",
            skippedCount > 0 ? " (some textures already existed)" : "");
    }
    
    return true;
}

int TextureProcessor::ProcessAllTrackedMaterials() {
    std::lock_guard<std::mutex> lock(m_mutex);
    
    if (!m_initialized) {
        Warning("[LegacyTextureProcessor] Not initialized\n");
        return 0;
    }
    
    // Get all cached materials from the texture tracker
    std::vector<std::string> cachedMaterials = D3D9TextureTracker::Instance().GetCachedMaterials();
    
    int processedCount = 0;
    
    for (const std::string& matName : cachedMaterials) {
        // Skip already processed
        if (m_processedMaterials.find(matName) != m_processedMaterials.end()) {
            continue;
        }
        
        // Skip internal materials
        if (matName.find("__") == 0 || matName.find("vgui") == 0) {
            continue;
        }
        
        // Extract PBR properties
        MaterialPBRProperties props;
        if (!ExtractMaterialPBR(matName, props)) {
            continue;
        }
        
        // Only process materials with PBR-relevant data
        // Include materials with roughness texture sources OR envmap/envmaptint (which implies reflectivity)
        if (!props.hasBumpMap && !props.hasPhong && !props.hasEnvMapMask && 
            !props.normalMapAlphaEnvMapMask && !props.hasPhongExponentTexture && 
            !props.hasBaseMapAlphaPhongMask && !props.hasBaseAlphaEnvMapMask &&
            !props.hasEnvMap && !props.hasEnvMapTint) {
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
        Msg("[LegacyTextureProcessor] Processed %d materials with PBR properties\n", processedCount);
        Msg("[LegacyTextureProcessor] Stats: %d with normals, %d with roughness textures\n", 
            m_stats.materialsWithNormals, m_stats.materialsWithRoughness);
        
        m_needsUSDAUpdate = true;
        
        // Write the USDA mod files
        if (WriteModUSDA()) {
            m_needsUSDAUpdate = false;
            Msg("[LegacyTextureProcessor] IMPORTANT: Restart the game for material replacements to take effect.\n");
            Msg("[LegacyTextureProcessor] The mod is written to: rtx-remix/mods/gmod_topbr/\n");
        }
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
    if (!props.hasBumpMap && !props.hasPhong && !props.hasEnvMapMask && 
        !props.normalMapAlphaEnvMapMask && !props.hasPhongExponentTexture &&
        !props.hasBaseMapAlphaPhongMask && !props.hasBaseAlphaEnvMapMask &&
        !props.hasEnvMap && !props.hasEnvMapTint) {
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
static std::string GetRelativeTexturePath(const std::string& absolutePath, const std::string& outputDir) {
    // We want a path relative to the mod directory like "./textures/HASH_type.dds"
    size_t texturesPos = absolutePath.find("textures");
    if (texturesPos != std::string::npos) {
        return "./" + absolutePath.substr(texturesPos);
    }
    // Fallback: just use filename
    size_t lastSlash = absolutePath.find_last_of("/\\");
    if (lastSlash != std::string::npos) {
        return "./textures/" + absolutePath.substr(lastSlash + 1);
    }
    return absolutePath;
}

bool TextureProcessor::WriteModUSDA() {
    if (m_outputDirectory.empty() || m_processedMaterialInfo.empty()) {
        return false;
    }
    
    // Get the mod directory (parent of textures directory)
    std::string modDir = m_outputDirectory;
    size_t texturesPos = modDir.find("textures");
    if (texturesPos != std::string::npos && texturesPos > 0) {
        modDir = modDir.substr(0, texturesPos);
        // Remove trailing slash
        while (!modDir.empty() && (modDir.back() == '/' || modDir.back() == '\\')) {
            modDir.pop_back();
        }
    }
    
    // Track which hashes are already in the existing USDA
    std::unordered_set<uint64_t> existingHashes;
    std::string materialsUsdaPath = modDir + "/materials.usda";
    
    // Read existing materials.usda to find which hashes are already defined
    std::ifstream existingFile(materialsUsdaPath);
    if (existingFile.is_open()) {
        std::string line;
        while (std::getline(existingFile, line)) {
            // Look for lines like: over "mat_HASH"
            size_t matPos = line.find("over \"mat_");
            if (matPos != std::string::npos) {
                size_t hashStart = matPos + 10; // Length of 'over "mat_'
                size_t hashEnd = line.find("\"", hashStart);
                if (hashEnd != std::string::npos) {
                    std::string hashStr = line.substr(hashStart, hashEnd - hashStart);
                    uint64_t hash = 0;
                    if (sscanf(hashStr.c_str(), "%llX", (unsigned long long*)&hash) == 1) {
                        existingHashes.insert(hash);
                    }
                }
            }
        }
        existingFile.close();
        
        if (m_debugOutput && !existingHashes.empty()) {
            Msg("[LegacyTextureProcessor] Found %d existing material entries in USDA\n", (int)existingHashes.size());
        }
    }
    
    // Count how many new materials we'll be adding
    int newMaterialCount = 0;
    for (const auto& pair : m_processedMaterialInfo) {
        if (existingHashes.find(pair.first) == existingHashes.end()) {
            newMaterialCount++;
        }
    }
    
    // If no new materials to add and file exists, skip writing
    if (newMaterialCount == 0 && !existingHashes.empty()) {
        if (m_debugOutput) {
            Msg("[LegacyTextureProcessor] No new materials to add to USDA\n");
        }
        return true;
    }
    
    // Write mod.usda (always write this as it's small)
    std::string modUsdaPath = modDir + "/mod.usda";
    std::ofstream modUsda(modUsdaPath);
    if (!modUsda.is_open()) {
        Warning("[LegacyTextureProcessor] Failed to create mod.usda at %s\n", modUsdaPath.c_str());
        return false;
    }
    
    // Write USDA header
    modUsda << "#usda 1.0\n";
    modUsda << "(\n";
    modUsda << "    customLayerData = {\n";
    modUsda << "        string lightspeed_game_name = \"Garry's Mod (x64)\"\n";
    modUsda << "        string lightspeed_layer_type = \"replacement\"\n";
    modUsda << "    }\n";
    modUsda << "    metersPerUnit = 0.01\n";
    modUsda << "    subLayers = [\n";
    modUsda << "        @./materials.usda@\n";
    modUsda << "    ]\n";
    modUsda << "    timeCodesPerSecond = 24\n";
    modUsda << "    upAxis = \"Z\"\n";
    modUsda << ")\n\n";
    modUsda.close();
    
    // Write materials.usda with all material definitions (including existing ones we're re-writing)
    std::ofstream materialsUsda(materialsUsdaPath);
    if (!materialsUsda.is_open()) {
        Warning("[LegacyTextureProcessor] Failed to create materials.usda at %s\n", materialsUsdaPath.c_str());
        return false;
    }
    
    // Write USDA header for materials
    materialsUsda << "#usda 1.0\n";
    materialsUsda << "(\n";
    materialsUsda << "    upAxis = \"Z\"\n";
    materialsUsda << ")\n\n";
    
    // Write material overrides
    materialsUsda << "over \"RootNode\"\n";
    materialsUsda << "{\n";
    materialsUsda << "    over \"Looks\"\n";
    materialsUsda << "    {\n";
    
    for (const auto& pair : m_processedMaterialInfo) {
        const ProcessedMaterialInfo& info = pair.second;
        uint64_t hash = info.textureHash;
        
        // Write material override
        // Format the hash as uppercase hex without leading zeros
        char hashStr[32];
        snprintf(hashStr, sizeof(hashStr), "%llX", (unsigned long long)hash);
        
        materialsUsda << "        over \"mat_" << hashStr << "\"\n";
        materialsUsda << "        {\n";
        materialsUsda << "            over \"Shader\"\n";
        materialsUsda << "            {\n";
        
        // Source asset - use AperturePBR_Opaque for standard materials
        materialsUsda << "                uniform asset info:mdl:sourceAsset = @AperturePBR_Opaque.mdl@\n";
        
        // Normal map encoding - octahedral for 2-channel textures
        if (!info.normalPath.empty()) {
            std::string relPath = GetRelativeTexturePath(info.normalPath, m_outputDirectory);
            materialsUsda << "                int inputs:encoding = 0\n";  // 0 = octahedral
            materialsUsda << "                asset inputs:normalmap_texture = @" << relPath << "@ (\n";
            materialsUsda << "                    colorSpace = \"raw\"\n";
            materialsUsda << "                )\n";
        }
        
        // Roughness
        if (!info.roughnessPath.empty()) {
            std::string relPath = GetRelativeTexturePath(info.roughnessPath, m_outputDirectory);
            materialsUsda << "                asset inputs:reflectionroughness_texture = @" << relPath << "@ (\n";
            materialsUsda << "                    colorSpace = \"raw\"\n";
            materialsUsda << "                )\n";
        } else {
            materialsUsda << "                float inputs:reflection_roughness_constant = " << info.roughnessConstant << "\n";
        }
        
        // Metallic
        if (!info.metallicPath.empty()) {
            std::string relPath = GetRelativeTexturePath(info.metallicPath, m_outputDirectory);
            materialsUsda << "                asset inputs:metallic_texture = @" << relPath << "@ (\n";
            materialsUsda << "                    colorSpace = \"raw\"\n";
            materialsUsda << "                )\n";
        } else {
            materialsUsda << "                float inputs:metallic_constant = " << info.metallicConstant << "\n";
        }
        
        materialsUsda << "            }\n";  // Close Shader
        materialsUsda << "        }\n\n";  // Close material
    }
    
    materialsUsda << "    }\n";  // Close Looks
    materialsUsda << "}\n";  // Close RootNode
    
    materialsUsda.close();
    
    Msg("[LegacyTextureProcessor] Wrote mod.usda and materials.usda with %d materials (%d new) to %s\n", 
        (int)m_processedMaterialInfo.size(), newMaterialCount, modDir.c_str());
    
    return true;
}

//=============================================================================
// Lua Bindings
//=============================================================================

using namespace GarrysMod::Lua;

LUA_FUNCTION(LegacyTextureProcessor_Initialize) {
    // If already initialized (by C++ during RemixAPI init), return success
    if (TextureProcessor::Instance().IsInitialized()) {
        LUA->PushBool(true);
        return 1;
    }
    
    // Not initialized yet - need g_remix to initialize
    if (!g_remix) {
        LUA->PushBool(false);
        return 1;
    }
    
    bool result = TextureProcessor::Instance().Initialize(g_remix);
    LUA->PushBool(result);
    return 1;
}

LUA_FUNCTION(LegacyTextureProcessor_IsInitialized) {
    LUA->PushBool(TextureProcessor::Instance().IsInitialized());
    return 1;
}

LUA_FUNCTION(LegacyTextureProcessor_ProcessAllMaterials) {
    int count = TextureProcessor::Instance().ProcessAllTrackedMaterials();
    LUA->PushNumber(count);
    return 1;
}

LUA_FUNCTION(LegacyTextureProcessor_SetAutoProcessing) {
    if (!LUA->IsType(1, Type::Bool)) {
        LUA->ThrowError("Expected boolean for auto processing");
        return 0;
    }
    
    TextureProcessor::Instance().SetAutoProcessing(LUA->GetBool(1));
    return 0;
}

LUA_FUNCTION(LegacyTextureProcessor_SetDebugOutput) {
    if (!LUA->IsType(1, Type::Bool)) {
        LUA->ThrowError("Expected boolean for debug output");
        return 0;
    }
    
    TextureProcessor::Instance().SetDebugOutput(LUA->GetBool(1));
    return 0;
}

LUA_FUNCTION(LegacyTextureProcessor_GetStats) {
    auto stats = TextureProcessor::Instance().GetStats();
    
    LUA->CreateTable();
    
    LUA->PushNumber(stats.materialsProcessed);
    LUA->SetField(-2, "materialsProcessed");
    
    LUA->PushNumber(stats.texturesUploaded);
    LUA->SetField(-2, "texturesUploaded");
    
    LUA->PushNumber(stats.materialsWithNormals);
    LUA->SetField(-2, "materialsWithNormals");
    
    LUA->PushNumber(stats.materialsWithRoughness);
    LUA->SetField(-2, "materialsWithRoughness");
    
    LUA->PushNumber(stats.failedConversions);
    LUA->SetField(-2, "failedConversions");
    
    return 1;
}

LUA_FUNCTION(LegacyTextureProcessor_ClearCache) {
    TextureProcessor::Instance().ClearCache();
    return 0;
}

LUA_FUNCTION(LegacyTextureProcessor_ConvertTexture) {
    if (!LUA->IsType(1, Type::String)) {
        LUA->ThrowError("Expected string for texture path");
        return 0;
    }
    
    const char* path = LUA->GetString(1);
    bool isNormalMap = LUA->IsType(2, Type::Bool) ? LUA->GetBool(2) : false;
    
    uint64_t hash = TextureProcessor::Instance().ConvertAndUploadTexture(path, isNormalMap);
    
    // Return hash as string to preserve precision
    char hashStr[32];
    sprintf_s(hashStr, "0x%llX", hash);
    LUA->PushString(hashStr);
    
    return 1;
}

LUA_FUNCTION(LegacyTextureProcessor_InspectMaterial) {
    if (!LUA->IsType(1, Type::String)) {
        LUA->ThrowError("Expected string for material name");
        return 0;
    }
    
    const char* matName = LUA->GetString(1);
    
    MaterialPBRProperties props;
    if (!TextureProcessor::Instance().ExtractMaterialPBR(matName, props)) {
        LUA->PushNil();
        return 1;
    }
    
    LUA->CreateTable();
    
    LUA->PushString(props.materialName.c_str());
    LUA->SetField(-2, "name");
    
    LUA->PushString(props.baseTexturePath.c_str());
    LUA->SetField(-2, "baseTexture");
    
    LUA->PushString(props.bumpMapPath.c_str());
    LUA->SetField(-2, "bumpMap");
    
    LUA->PushString(props.envMapMaskPath.c_str());
    LUA->SetField(-2, "envMapMask");
    
    LUA->PushNumber(props.phongExponent);
    LUA->SetField(-2, "phongExponent");
    
    LUA->PushNumber(props.phongBoost);
    LUA->SetField(-2, "phongBoost");
    
    LUA->PushNumber(props.roughness);
    LUA->SetField(-2, "roughness");
    
    LUA->PushNumber(props.metallic);
    LUA->SetField(-2, "metallic");
    
    LUA->PushBool(props.hasBumpMap);
    LUA->SetField(-2, "hasBumpMap");
    
    LUA->PushBool(props.hasPhong);
    LUA->SetField(-2, "hasPhong");
    
    LUA->PushBool(props.hasEnvMapMask);
    LUA->SetField(-2, "hasEnvMapMask");
    
    LUA->PushBool(props.hasPhongExponentTexture);
    LUA->SetField(-2, "hasPhongExponentTexture");
    
    LUA->PushString(props.phongExponentTexturePath.c_str());
    LUA->SetField(-2, "phongExponentTexture");
    
    LUA->PushBool(props.hasBaseMapAlphaPhongMask);
    LUA->SetField(-2, "hasBaseMapAlphaPhongMask");
    
    LUA->PushBool(props.normalMapAlphaEnvMapMask);
    LUA->SetField(-2, "normalMapAlphaEnvMapMask");
    
    LUA->PushBool(props.isSelfIllum);
    LUA->SetField(-2, "isSelfIllum");
    
    LUA->PushBool(props.isTranslucent);
    LUA->SetField(-2, "isTranslucent");
    
    return 1;
}

LUA_FUNCTION(LegacyTextureProcessor_SetOutputDirectory) {
    if (!LUA->IsType(1, Type::String)) {
        LUA->ThrowError("Expected string for output directory path");
        return 0;
    }
    
    const char* path = LUA->GetString(1);
    TextureProcessor::Instance().SetOutputDirectory(path);
    return 0;
}

LUA_FUNCTION(LegacyTextureProcessor_GetOutputDirectory) {
    LUA->PushString(TextureProcessor::Instance().GetOutputDirectory().c_str());
    return 1;
}

LUA_FUNCTION(LegacyTextureProcessor_ProcessSingleMaterial) {
    if (!LUA->IsType(1, Type::String)) {
        LUA->ThrowError("Expected string for material name");
        return 0;
    }
    
    const char* matName = LUA->GetString(1);
    bool result = TextureProcessor::Instance().ProcessSingleMaterial(matName);
    LUA->PushBool(result);
    return 1;
}

LUA_FUNCTION(LegacyTextureProcessor_WriteUSDAIfNeeded) {
    TextureProcessor::Instance().WriteUSDAIfNeeded();
    return 0;
}

LUA_FUNCTION(LegacyTextureProcessor_NeedsUSDAUpdate) {
    LUA->PushBool(TextureProcessor::Instance().NeedsUSDAUpdate());
    return 1;
}

void InitializeLegacyTextureProcessorLuaBindings(GarrysMod::Lua::ILuaBase* LUA) {
    // Create LegacyTextureProcessor table
    LUA->PushSpecial(SPECIAL_GLOB);
    LUA->CreateTable();
    
    LUA->PushCFunction(LegacyTextureProcessor_Initialize);
    LUA->SetField(-2, "Initialize");
    
    LUA->PushCFunction(LegacyTextureProcessor_IsInitialized);
    LUA->SetField(-2, "IsInitialized");
    
    LUA->PushCFunction(LegacyTextureProcessor_ProcessAllMaterials);
    LUA->SetField(-2, "ProcessAllMaterials");
    
    LUA->PushCFunction(LegacyTextureProcessor_ProcessSingleMaterial);
    LUA->SetField(-2, "ProcessSingleMaterial");
    
    LUA->PushCFunction(LegacyTextureProcessor_SetAutoProcessing);
    LUA->SetField(-2, "SetAutoProcessing");
    
    LUA->PushCFunction(LegacyTextureProcessor_SetDebugOutput);
    LUA->SetField(-2, "SetDebugOutput");
    
    LUA->PushCFunction(LegacyTextureProcessor_GetStats);
    LUA->SetField(-2, "GetStats");
    
    LUA->PushCFunction(LegacyTextureProcessor_ClearCache);
    LUA->SetField(-2, "ClearCache");
    
    LUA->PushCFunction(LegacyTextureProcessor_ConvertTexture);
    LUA->SetField(-2, "ConvertTexture");
    
    LUA->PushCFunction(LegacyTextureProcessor_InspectMaterial);
    LUA->SetField(-2, "InspectMaterial");
    
    LUA->PushCFunction(LegacyTextureProcessor_SetOutputDirectory);
    LUA->SetField(-2, "SetOutputDirectory");
    
    LUA->PushCFunction(LegacyTextureProcessor_GetOutputDirectory);
    LUA->SetField(-2, "GetOutputDirectory");
    
    LUA->PushCFunction(LegacyTextureProcessor_WriteUSDAIfNeeded);
    LUA->SetField(-2, "WriteUSDAIfNeeded");
    
    LUA->PushCFunction(LegacyTextureProcessor_NeedsUSDAUpdate);
    LUA->SetField(-2, "NeedsUSDAUpdate");
    
    LUA->SetField(-2, "LegacyTextureProcessor");
    
    // Also create an alias as VTFConverter for backwards compatibility
    LUA->CreateTable();
    
    LUA->PushCFunction(LegacyTextureProcessor_Initialize);
    LUA->SetField(-2, "Initialize");
    
    LUA->PushCFunction(LegacyTextureProcessor_IsInitialized);
    LUA->SetField(-2, "IsInitialized");
    
    LUA->PushCFunction(LegacyTextureProcessor_ProcessAllMaterials);
    LUA->SetField(-2, "ProcessAllMaterials");
    
    LUA->PushCFunction(LegacyTextureProcessor_ProcessSingleMaterial);
    LUA->SetField(-2, "ProcessSingleMaterial");
    
    LUA->PushCFunction(LegacyTextureProcessor_SetAutoProcessing);
    LUA->SetField(-2, "SetAutoProcessing");
    
    LUA->PushCFunction(LegacyTextureProcessor_SetDebugOutput);
    LUA->SetField(-2, "SetDebugOutput");
    
    LUA->PushCFunction(LegacyTextureProcessor_GetStats);
    LUA->SetField(-2, "GetStats");
    
    LUA->PushCFunction(LegacyTextureProcessor_ClearCache);
    LUA->SetField(-2, "ClearCache");
    
    LUA->PushCFunction(LegacyTextureProcessor_ConvertTexture);
    LUA->SetField(-2, "ConvertTexture");
    
    LUA->PushCFunction(LegacyTextureProcessor_InspectMaterial);
    LUA->SetField(-2, "InspectMaterial");
    
    LUA->PushCFunction(LegacyTextureProcessor_SetOutputDirectory);
    LUA->SetField(-2, "SetOutputDirectory");
    
    LUA->PushCFunction(LegacyTextureProcessor_GetOutputDirectory);
    LUA->SetField(-2, "GetOutputDirectory");
    
    LUA->PushCFunction(LegacyTextureProcessor_WriteUSDAIfNeeded);
    LUA->SetField(-2, "WriteUSDAIfNeeded");
    
    LUA->PushCFunction(LegacyTextureProcessor_NeedsUSDAUpdate);
    LUA->SetField(-2, "NeedsUSDAUpdate");
    
    LUA->SetField(-2, "VTFConverter");
    LUA->Pop();
    
    Msg("[LegacyTextureProcessor] Lua bindings initialized\n");
}

} // namespace LegacyTextureProcessor

#endif // _WIN64
