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
    , m_metallicGenerationEnabled(false)  // Disabled by default - experimental feature
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
    bool isInvertedMask = false;  // For $basealphaenvmapmask where white=masked(matte), black=reflective(shiny)
    
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
        // NOTE: $basealphaenvmapmask is INVERTED: white (255) = masked/no reflection, black (0) = reflective
        vtfPath = props.baseTexturePath;
        useAlphaChannel = true;
        isInvertedMask = true;  // This mask type is inverted!
        if (m_debugOutput) {
            Msg("[LegacyTextureProcessor] %s: Using base texture alpha for roughness ($basealphaenvmapmask - INVERTED mask)\n", props.materialName.c_str());
        }
    } else if (props.hasEnvMapMask && !props.envMapMaskPath.empty()) {
        // Use the envmap mask texture
        vtfPath = props.envMapMaskPath;
        useAlphaChannel = false;
        if (m_debugOutput) {
            Msg("[LegacyTextureProcessor] %s: Using envmap mask for roughness\n", props.materialName.c_str());
        }
    } else if ((props.hasEnvMap || props.hasEnvMapTint) && props.hasBumpMap && !props.bumpMapPath.empty()) {
        // FALLBACK for $normalmapalphaenvmapmask: If $envmap or $envmaptint is set and we have a bumpmap,
        // try using normal map alpha first (implicit $normalmapalphaenvmapmask behavior)
        // This handles cases where FindVar doesn't find $normalmapalphaenvmapmask but it's set in VMT
        vtfPath = props.bumpMapPath;
        useAlphaChannel = true;
        if (m_debugOutput) {
            Msg("[LegacyTextureProcessor] %s: Trying normal map alpha as fallback roughness (implicit $normalmapalphaenvmapmask)\n", props.materialName.c_str());
        }
    } else if ((props.hasEnvMap || props.hasEnvMapTint) && !props.baseTexturePath.empty()) {
        // FALLBACK: If $envmap or $envmaptint is set but no explicit roughness source,
        // try using base texture alpha as envmap mask (implicit $basealphaenvmapmask behavior)
        // This handles cases where FindVar doesn't find $basealphaenvmapmask but it's set in VMT
        // NOTE: $basealphaenvmapmask is INVERTED: white (255) = masked/no reflection, black (0) = reflective
        vtfPath = props.baseTexturePath;
        useAlphaChannel = true;
        isInvertedMask = true;  // This mask type is inverted!
        if (m_debugOutput) {
            Msg("[LegacyTextureProcessor] %s: Trying base texture alpha as fallback roughness (implicit envmap alpha - INVERTED)\n", props.materialName.c_str());
        }
    } else if (props.hasBumpMap && !props.bumpMapPath.empty()) {
        // LAST RESORT: If we have a bumpmap but couldn't detect envmap reliably,
        // try normal map alpha anyway. The alpha variation check will filter out
        // normal maps without useful alpha data. This handles cases where:
        // - $envmap is set but FindVar returns UNDEFINED
        // - $normalmapalphaenvmapmask is set but FindVar doesn't find it
        vtfPath = props.bumpMapPath;
        useAlphaChannel = true;
        if (m_debugOutput) {
            Msg("[LegacyTextureProcessor] %s: Last resort - trying normal map alpha (FindVar limitations)\n", props.materialName.c_str());
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
            uint8_t sourceValue = sourceTex.pixelData[i + 3];
            
            // Handle inverted mask semantics for $basealphaenvmapmask
            // Normal masks (phong, $normalmapalphaenvmapmask): bright = shiny areas = LOW roughness
            // Inverted masks ($basealphaenvmapmask): bright = MASKED (matte), dark = reflective (shiny)
            if (isInvertedMask) {
                // $basealphaenvmapmask: white (255) = masked/no reflection/matte, black (0) = reflective/shiny
                // Invert the source value first, then apply the same curve
                sourceValue = 255 - sourceValue;
            }
            
            // The mask represents "shininess" intensity (0-255) after potential inversion
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
    // Generate per-pixel metallic maps from base texture brightness
    // In Source Engine, dark areas + envmap = metallic (like chrome/metal parts)
    // Brighter areas = non-metallic (diffuse surfaces)
    
    // Only generate metallic map if material has envmap and average brightness suggests some metallic areas
    if (!props.hasEnvMap || props.baseTextureBrightness >= 0.4f || props.metallic <= 0.05f) {
        if (m_debugOutput) {
            Msg("[LegacyTextureProcessor] %s: No metallic texture needed (hasEnvMap=%d, avgBrightness=%.2f, metallic=%.2f)\n",
                props.materialName.c_str(), props.hasEnvMap ? 1 : 0, props.baseTextureBrightness, props.metallic);
        }
        return false;
    }
    
    // Read the base texture VTF
    std::vector<uint8_t> fileData;
    if (!ReadVTFFile(props.baseTexturePath, fileData)) {
        if (m_debugOutput) {
            Msg("[LegacyTextureProcessor] %s: Could not read base texture for metallic map generation\n",
                props.materialName.c_str());
        }
        return false;
    }
    
    // Parse VTF header
    VTFFileHeader header;
    if (!ParseVTFHeader(fileData, header)) {
        if (m_debugOutput) {
            Msg("[LegacyTextureProcessor] %s: Could not parse base texture VTF header for metallic map\n",
                props.materialName.c_str());
        }
        return false;
    }
    
    // Extract pixel data
    ConvertedTexture sourceTex;
    if (!ExtractVTFPixelData(fileData, header, sourceTex, false)) {
        if (m_debugOutput) {
            Msg("[LegacyTextureProcessor] %s: Could not extract base texture pixel data for metallic map\n",
                props.materialName.c_str());
        }
        return false;
    }
    
    // Generate metallic map from per-pixel brightness
    // Dark pixels (brightness < 0.3) = metallic (scaled), bright pixels = non-metallic
    outTexture.width = sourceTex.width;
    outTexture.height = sourceTex.height;
    outTexture.pixelData.resize(sourceTex.width * sourceTex.height * 4);
    
    // Metallic map: store metallic value in R channel (grayscale)
    // Pixels darker than threshold become metallic, brighter = non-metallic
    constexpr float METALLIC_THRESHOLD = 0.30f;  // Brightness below this is considered metallic
    
    for (uint32_t y = 0; y < sourceTex.height; y++) {
        for (uint32_t x = 0; x < sourceTex.width; x++) {
            size_t srcIdx = (y * sourceTex.width + x) * 4;
            size_t dstIdx = (y * sourceTex.width + x) * 4;
            
            uint8_t r = sourceTex.pixelData[srcIdx];
            uint8_t g = sourceTex.pixelData[srcIdx + 1];
            uint8_t b = sourceTex.pixelData[srcIdx + 2];
            
            // Calculate per-pixel brightness (luminance)
            float brightness = (0.299f * r + 0.587f * g + 0.114f * b) / 255.0f;
            
            // Calculate metallic value: darker = more metallic
            // brightness 0.0 -> metallic 1.0
            // brightness 0.3 -> metallic 0.0
            // brightness 0.3+ -> metallic 0.0
            float metallic = 0.0f;
            if (brightness < METALLIC_THRESHOLD) {
                metallic = std::clamp(1.0f - (brightness / METALLIC_THRESHOLD), 0.0f, 1.0f);
            }
            
            uint8_t metallicByte = static_cast<uint8_t>(metallic * 255.0f);
            
            // Store as grayscale (R=G=B=metallic, A=255)
            outTexture.pixelData[dstIdx] = metallicByte;
            outTexture.pixelData[dstIdx + 1] = metallicByte;
            outTexture.pixelData[dstIdx + 2] = metallicByte;
            outTexture.pixelData[dstIdx + 3] = 255;
        }
    }
    
    if (m_debugOutput) {
        Msg("[LegacyTextureProcessor] %s: Generated %dx%d per-pixel metallic map from base texture brightness\n",
            props.materialName.c_str(), sourceTex.width, sourceTex.height);
    }
    
    return true;
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

// Convert SSBump texture to standard tangent-space normal map
// SSBump stores directional occlusion in 3 basis directions, not XYZ normal components
// Based on Valve's Source SDK common_fxc.h and rob5300's ssbumpToNormal converter
// Reference: https://github.com/ValveSoftware/source-sdk-2013/blob/master/sp/src/materialsystem/stdshaders/common_fxc.h
void TextureProcessor::ConvertSSBumpToNormal(ConvertedTexture& texture) {
    if (texture.pixelData.empty()) return;
    
    uint32_t width = texture.width;
    uint32_t height = texture.height;
    size_t pixelCount = width * height;
    
    std::vector<uint8_t> normalData(pixelCount * 4);
    
    // Bump basis transpose from Source SDK common_fxc.h
    // This transforms from SSBump space (3 basis light responses) to tangent-space normal (XYZ)
    // Each row is used to compute one component of the output normal
    const float OO_SQRT_3 = 0.57735025882720947f;
    
    // Row 0: computes normal.x
    const float basisT0_x = 0.81649661064147949f;
    const float basisT0_y = -0.40824833512306213f;
    const float basisT0_z = -0.40824833512306213f;
    
    // Row 1: computes normal.y
    const float basisT1_x = 0.0f;
    const float basisT1_y = 0.70710676908493042f;
    const float basisT1_z = -0.7071068286895752f;
    
    // Row 2: computes normal.z
    const float basisT2_x = OO_SQRT_3;
    const float basisT2_y = OO_SQRT_3;
    const float basisT2_z = OO_SQRT_3;
    
    for (size_t i = 0; i < pixelCount; i++) {
        size_t srcIdx = i * 4;
        size_t dstIdx = i * 4;
        
        // Read SSBump values (light response in 3 basis directions)
        // Normalized to 0.0-1.0 range
        float r = texture.pixelData[srcIdx + 0] / 255.0f;  // Basis 0 response
        float g = texture.pixelData[srcIdx + 1] / 255.0f;  // Basis 1 response
        float b = texture.pixelData[srcIdx + 2] / 255.0f;  // Basis 2 response
        
        // Transform SSBump to tangent-space normal using bumpBasisTranspose
        // normal.x = dot(ssbump, basisTranspose[0])
        // normal.y = dot(ssbump, basisTranspose[1])
        // normal.z = dot(ssbump, basisTranspose[2])
        float nx = r * basisT0_x + g * basisT0_y + b * basisT0_z;
        float ny = r * basisT1_x + g * basisT1_y + b * basisT1_z;
        float nz = r * basisT2_x + g * basisT2_y + b * basisT2_z;
        
        // Convert from [-1, 1] to [0, 255] with 0.5 bias (standard normal map encoding)
        // The formula is: output = (normal * 0.5 + 0.5) * 255
        uint8_t outR = static_cast<uint8_t>(std::clamp((nx * 0.5f + 0.5f) * 255.0f + 0.5f, 0.0f, 255.0f));
        uint8_t outG = static_cast<uint8_t>(std::clamp((ny * 0.5f + 0.5f) * 255.0f + 0.5f, 0.0f, 255.0f));
        uint8_t outB = static_cast<uint8_t>(std::clamp((nz * 0.5f + 0.5f) * 255.0f + 0.5f, 0.0f, 255.0f));
        
        normalData[dstIdx + 0] = outR;
        normalData[dstIdx + 1] = outG;
        normalData[dstIdx + 2] = outB;
        normalData[dstIdx + 3] = 255;
    }
    
    texture.pixelData = std::move(normalData);
    
    if (m_debugOutput) {
        Msg("[LegacyTextureProcessor] Converted SSBump to normal map (%dx%d)\n", width, height);
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

// Helper function to check if a file exists
static bool FileExists(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    return file.good();
}

// Helper struct for VMT properties parsed from file
struct VMTProperties {
    std::string shaderName;
    bool hasRefractAmount;
    float refractAmount;
    bool hasTranslucent;
    bool translucent;
    std::string surfaceProp;
    bool hasEnvMap;
    std::string envMap;
    std::string refractTintTexture;  // $refracttinttexture - color texture for Refract shader
    bool hasRefractTintTexture;
};

// Parse a VMT file and extract properties that FindVar doesn't reliably expose
static bool ParseVMTFile(IFileSystem* fileSystem, const std::string& materialName, VMTProperties& outProps, bool debugOutput) {
    if (!fileSystem) return false;
    
    outProps = VMTProperties{};
    outProps.hasRefractAmount = false;
    outProps.refractAmount = 0.0f;
    outProps.hasTranslucent = false;
    outProps.translucent = false;
    outProps.hasEnvMap = false;
    outProps.hasRefractTintTexture = false;
    
    // Build VMT path
    std::string vmtPath = "materials/" + materialName;
    if (vmtPath.find(".vmt") == std::string::npos) {
        vmtPath += ".vmt";
    }
    
    FileHandle_t file = fileSystem->Open(vmtPath.c_str(), "rb", "GAME");
    if (!file) {
        // Try without materials/ prefix
        vmtPath = materialName;
        if (vmtPath.find(".vmt") == std::string::npos) {
            vmtPath += ".vmt";
        }
        file = fileSystem->Open(vmtPath.c_str(), "rb", "GAME");
        if (!file) {
            return false;
        }
    }
    
    // Get file size
    int fileSize = fileSystem->Size(file);
    if (fileSize <= 0 || fileSize > 64 * 1024) {  // Max 64KB VMT
        fileSystem->Close(file);
        return false;
    }
    
    // Read file content
    std::vector<char> buffer(fileSize + 1);
    int bytesRead = fileSystem->Read(buffer.data(), fileSize, file);
    fileSystem->Close(file);
    
    if (bytesRead != fileSize) {
        return false;
    }
    buffer[fileSize] = '\0';
    
    // Parse the VMT content (simple key-value parsing)
    std::string content(buffer.data());
    
    // Convert to lowercase for case-insensitive matching
    std::string contentLower = content;
    std::transform(contentLower.begin(), contentLower.end(), contentLower.begin(), ::tolower);
    
    // Extract shader name (first non-whitespace word, possibly in quotes)
    size_t start = contentLower.find_first_not_of(" \t\r\n");
    if (start != std::string::npos) {
        // Skip quotes if present
        if (content[start] == '"') {
            start++;
            size_t end = content.find('"', start);
            if (end != std::string::npos) {
                outProps.shaderName = content.substr(start, end - start);
            }
        } else {
            // Find end of shader name (whitespace or brace)
            size_t end = content.find_first_of(" \t\r\n{", start);
            if (end != std::string::npos) {
                outProps.shaderName = content.substr(start, end - start);
            }
        }
    }
    
    // Helper to find a key-value pair (case-insensitive key)
    auto findValue = [&contentLower, &content](const std::string& keyLower) -> std::string {
        size_t pos = contentLower.find(keyLower);
        if (pos == std::string::npos) return "";
        
        // Find the value after the key
        pos += keyLower.length();
        // Skip whitespace
        while (pos < content.length() && (content[pos] == ' ' || content[pos] == '\t' || content[pos] == '"')) {
            pos++;
        }
        
        // Read value until whitespace, quote, or newline
        size_t valueStart = pos;
        while (pos < content.length() && content[pos] != '"' && content[pos] != '\r' && content[pos] != '\n' && content[pos] != ' ' && content[pos] != '\t') {
            pos++;
        }
        
        return content.substr(valueStart, pos - valueStart);
    };
    
    // Check for $refractamount
    size_t refractPos = contentLower.find("$refractamount");
    if (refractPos != std::string::npos) {
        outProps.hasRefractAmount = true;
        std::string valStr = findValue("$refractamount");
        if (!valStr.empty()) {
            try {
                outProps.refractAmount = std::stof(valStr);
            } catch (...) {
                outProps.refractAmount = 0.25f;  // Default
            }
        }
    }
    
    // Check for $translucent
    size_t translucentPos = contentLower.find("$translucent");
    if (translucentPos != std::string::npos) {
        outProps.hasTranslucent = true;
        std::string valStr = findValue("$translucent");
        outProps.translucent = (valStr == "1" || valStr == "true");
    }
    
    // Check for $surfaceprop
    size_t surfacePos = contentLower.find("$surfaceprop");
    if (surfacePos != std::string::npos) {
        outProps.surfaceProp = findValue("$surfaceprop");
    }
    
    // Check for $envmap
    size_t envmapPos = contentLower.find("$envmap");
    if (envmapPos != std::string::npos) {
        outProps.hasEnvMap = true;
        outProps.envMap = findValue("$envmap");
    }
    
    // Check for $refracttinttexture - the actual color texture for Refract shader
    size_t refractTintPos = contentLower.find("$refracttinttexture");
    if (refractTintPos != std::string::npos) {
        outProps.hasRefractTintTexture = true;
        outProps.refractTintTexture = findValue("$refracttinttexture");
    }
    
    if (debugOutput && (outProps.hasRefractAmount || !outProps.shaderName.empty())) {
        Msg("[LegacyTextureProcessor] VMT parse: shader='%s', $refractamount=%d (%.2f), $translucent=%d (%d), $surfaceprop='%s', $envmap=%d, $refracttinttexture='%s'\n",
            outProps.shaderName.c_str(), outProps.hasRefractAmount, outProps.refractAmount,
            outProps.hasTranslucent, outProps.translucent, outProps.surfaceProp.c_str(), outProps.hasEnvMap,
            outProps.refractTintTexture.c_str());
    }
    
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
    
    // Get the shader name
    const char* shaderName = pMaterial->GetShaderName();
    if (shaderName && shaderName[0] != '\0') {
        outProps.shaderName = shaderName;
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
    
    // Check for $ssbump flag - indicates SSBump format that needs conversion to standard normal map
    pVar = pMaterial->FindVar("$ssbump", &found, false);
    if (found && pVar) {
        int ssbumpValue = pVar->GetIntValue();
        if (ssbumpValue != 0) {
            outProps.isSSBump = true;
            if (m_debugOutput) {
                Msg("[LegacyTextureProcessor] %s: $ssbump = 1 (will convert to normal map)\n", materialName.c_str());
            }
        }
    }
    
    // Get $normalmap - used by Refract shader instead of $bumpmap
    // Only check this if we don't already have a bump map
    if (!outProps.hasBumpMap) {
        pVar = pMaterial->FindVar("$normalmap", &found, false);
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
        // Check if it has any value (env_cubemap, or a texture path) - but filter UNDEFINED and <UNDEFINED>
        bool isUndefined = false;
        if (strVal) {
            if (strcmp(strVal, "UNDEFINED") == 0 || strcmp(strVal, "<UNDEFINED>") == 0) {
                isUndefined = true;
            } else if (strstr(strVal, "UNDEFINED") != nullptr) {
                isUndefined = true;  // Also catch partial matches
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
    
    // Get $normalmapalphaenvmapmask - use normal map's alpha as envmap mask for roughness
    pVar = pMaterial->FindVar("$normalmapalphaenvmapmask", &found, false);
    if (found && pVar) {
        // Try multiple methods to get the value - Source Engine can be inconsistent
        int intVal = pVar->GetIntValue();
        float floatVal = pVar->GetFloatValue();
        const char* strVal = pVar->GetStringValue();
        
        // Accept any truthy value
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
            // Parse "[r g b]" or "r g b" or single float format
            float r = 1, g = 1, b = 1;
            bool parsed = false;
            
            if (sscanf(strVal, "[%f %f %f]", &r, &g, &b) == 3 ||
                sscanf(strVal, "%f %f %f", &r, &g, &b) == 3) {
                parsed = true;
            } else if (sscanf(strVal, "%f", &r) == 1) {
                // Single float value - Source Engine sometimes returns this for "$envmaptint .2 .2 .2" 
                // Treat as uniform tint
                g = r;
                b = r;
                parsed = true;
            }
            
            if (parsed) {
                outProps.envMapTint[0] = r;
                outProps.envMapTint[1] = g;
                outProps.envMapTint[2] = b;
                
                // IMPORTANT: Only treat as having explicit envmaptint if value is NOT the default [1 1 1]
                // Source Engine shaders return [1 1 1] as default even when not set in VMT
                bool isDefaultTint = (fabs(r - 1.0f) < 0.01f && fabs(g - 1.0f) < 0.01f && fabs(b - 1.0f) < 0.01f);
                
                if (!isDefaultTint) {
                    outProps.hasEnvMapTint = true;
                    // If envmaptint is explicitly set (non-default), envmap must be enabled
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
    
    // Get $surfaceprop
    pVar = pMaterial->FindVar("$surfaceprop", &found, false);
    if (found && pVar) {
        const char* surfaceVal = pVar->GetStringValue();
        if (surfaceVal && surfaceVal[0] != '\0') {
            outProps.surfaceProp = surfaceVal;
        }
    }
    
    // Glass detection: Material is considered glass if:
    // 1. Shader is "Refract" (always glass), OR
    // 2. VMT has $refractamount (indicates Refract shader even if FindVar says otherwise), OR
    // 3. $surfaceprop = "glass" (FindVar doesn't reliably expose $translucent), OR
    // 4. $translucent = 1 AND material has envmap + reflective properties
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
        
        // Try to parse VMT file directly for properties that FindVar doesn't expose reliably
        // This is especially important for Refract shaders where FindVar returns wrong shader name
        VMTProperties vmtProps;
        bool hasVMTParsed = ParseVMTFile(m_fileSystem, materialName, vmtProps, m_debugOutput);
        
        // Check VMT shader name and $refractamount
        bool vmtIsRefract = false;
        bool vmtHasRefractAmount = false;
        if (hasVMTParsed) {
            if (!vmtProps.shaderName.empty()) {
                std::string vmtShaderLower = vmtProps.shaderName;
                std::transform(vmtShaderLower.begin(), vmtShaderLower.end(), vmtShaderLower.begin(), ::tolower);
                vmtIsRefract = (vmtShaderLower.find("refract") != std::string::npos);
            }
            vmtHasRefractAmount = vmtProps.hasRefractAmount;
            
            // Get $refracttinttexture if present
            if (vmtProps.hasRefractTintTexture && !vmtProps.refractTintTexture.empty()) {
                outProps.refractTintTexturePath = vmtProps.refractTintTexture;
            }
            
            // Also pick up surfaceprop from VMT if not found via FindVar
            if (outProps.surfaceProp.empty() && !vmtProps.surfaceProp.empty()) {
                outProps.surfaceProp = vmtProps.surfaceProp;
                std::string surfaceLower = vmtProps.surfaceProp;
                std::transform(surfaceLower.begin(), surfaceLower.end(), surfaceLower.begin(), ::tolower);
                isSurfaceGlass = (surfaceLower == "glass" || surfaceLower.find("glass") != std::string::npos);
            }
            
            // Also pick up translucent from VMT if not found via FindVar
            if (!outProps.isTranslucent && vmtProps.hasTranslucent && vmtProps.translucent) {
                outProps.isTranslucent = true;
            }
            
            // Also pick up envmap from VMT if not found via FindVar
            if (!outProps.hasEnvMap && vmtProps.hasEnvMap && !vmtProps.envMap.empty()) {
                outProps.hasEnvMap = true;
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
        } else if (isSurfaceGlass) {
            outProps.isGlass = true;  // surfaceprop=glass means it's glass
        }
        // NOTE: We REMOVED the "translucent + envmap = glass" heuristic because it catches
        // too many materials that aren't glass (like doors with glass cutouts, windows with frames, etc.)
        // Those materials have $translucent for alpha blending, not for glass refraction.
        
        if (m_debugOutput) {
            if (outProps.isGlass) {
                Msg("[LegacyTextureProcessor] %s: DETECTED AS GLASS (shader=%s, vmtShader=%s, vmtRefract=%d, translucent=%d, surfaceprop=%s, hasEnvMap=%d)\n",
                    materialName.c_str(), outProps.shaderName.c_str(), 
                    hasVMTParsed ? vmtProps.shaderName.c_str() : "N/A",
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
    matInfo.isGlass = props.isGlass;
    matInfo.isRefractShader = props.isRefractShader;
    matInfo.ior = props.isGlass ? 1.5f : 1.0f;  // Default glass IOR is 1.5
    
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
                    
                    // For SSBump textures, extract raw data first then convert
                    if (props.isSSBump) {
                        // Extract without octahedral conversion
                        if (ExtractVTFPixelData(fileData, header, normalTex, false)) {
                            // Convert SSBump to standard normal map
                            ConvertSSBumpToNormal(normalTex);
                            // Then convert to octahedral for RTX Remix
                            ConvertNormalMapToOctahedral(normalTex);
                            
                            normalTex.hash = GenerateTextureHash(props.bumpMapPath + "_normal", normalTex.width, normalTex.height);
                            std::string outputPath = GenerateOutputPath(normalTex.hash, "_normal");
                            
                            // Double-check with actual dimensions hash
                            if (FileExists(outputPath)) {
                                matInfo.normalPath = outputPath;
                                m_writtenTexturePaths[normalTex.hash] = outputPath;
                                skippedCount++;
                                
                                if (m_debugOutput) {
                                    Msg("[LegacyTextureProcessor] Skipping existing SSBump-converted normal: %s\n", outputPath.c_str());
                                }
                            } else if (WriteTextureToDDS(normalTex, outputPath)) {
                                matInfo.normalPath = outputPath;
                                m_writtenTexturePaths[normalTex.hash] = outputPath;
                                m_stats.materialsWithNormals++;
                                
                                if (m_debugOutput) {
                                    Msg("[LegacyTextureProcessor] Wrote SSBump-converted normal: %s\n", outputPath.c_str());
                                }
                            } else if (m_debugOutput) {
                                Msg("[LegacyTextureProcessor] Failed to write SSBump DDS for %s\n", props.bumpMapPath.c_str());
                            }
                        } else if (m_debugOutput) {
                            Msg("[LegacyTextureProcessor] Failed to extract SSBump pixel data for %s\n", props.bumpMapPath.c_str());
                        }
                    } else {
                        // Standard normal map - extract with octahedral conversion
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
    
    // For glass materials, handle transmittance texture
    // For Refract shaders: ONLY use $refracttinttexture (don't use baseTexture - it might be set to normalmap by fixer)
    // For non-Refract glass (surfaceprop=glass): use baseTexture as transmittance
    if (props.isGlass) {
        std::string transmittanceTexPath;
        std::string transmittanceSuffix;
        
        if (props.isRefractShader && !props.refractTintTexturePath.empty()) {
            // Refract shader: use $refracttinttexture
            transmittanceTexPath = props.refractTintTexturePath;
            transmittanceSuffix = "_refracttint";
            if (m_debugOutput) {
                Msg("[LegacyTextureProcessor] Using $refracttinttexture for Refract glass transmittance: %s\n", transmittanceTexPath.c_str());
            }
        } else if (!props.isRefractShader && !props.baseTexturePath.empty()) {
            // Non-Refract glass (like bottles): use baseTexture
            transmittanceTexPath = props.baseTexturePath;
            transmittanceSuffix = "_base";
            if (m_debugOutput) {
                Msg("[LegacyTextureProcessor] Using $basetexture for glass transmittance: %s\n", transmittanceTexPath.c_str());
            }
        }
        // Note: If Refract shader has no $refracttinttexture, glass will be clear (no transmittance texture)
        
        if (!transmittanceTexPath.empty()) {
            uint64_t texHash = GenerateTextureHash(transmittanceTexPath + transmittanceSuffix, 0, 0);
            std::string expectedOutputPath = GenerateOutputPath(texHash, transmittanceSuffix.c_str());
            
            if (FileExists(expectedOutputPath)) {
                matInfo.transmittancePath = expectedOutputPath;
                m_writtenTexturePaths[texHash] = expectedOutputPath;
                skippedCount++;
                
                if (m_debugOutput) {
                    Msg("[LegacyTextureProcessor] Skipping existing transmittance texture for glass: %s\n", expectedOutputPath.c_str());
                }
            } else {
                std::vector<uint8_t> fileData;
                
                if (ReadVTFFile(transmittanceTexPath, fileData)) {
                    VTFFileHeader header;
                    if (ParseVTFHeader(fileData, header)) {
                        ConvertedTexture tex;
                        tex.isNormalMap = false;
                        if (ExtractVTFPixelData(fileData, header, tex, false)) {
                            tex.hash = GenerateTextureHash(transmittanceTexPath + transmittanceSuffix, tex.width, tex.height);
                            std::string outputPath = GenerateOutputPath(tex.hash, transmittanceSuffix.c_str());
                            
                            if (FileExists(outputPath)) {
                                matInfo.transmittancePath = outputPath;
                                m_writtenTexturePaths[tex.hash] = outputPath;
                                skippedCount++;
                                
                                if (m_debugOutput) {
                                    Msg("[LegacyTextureProcessor] Skipping existing transmittance texture: %s\n", outputPath.c_str());
                                }
                            } else if (WriteTextureToDDS(tex, outputPath)) {
                                matInfo.transmittancePath = outputPath;
                                m_writtenTexturePaths[tex.hash] = outputPath;
                                
                                if (m_debugOutput) {
                                    Msg("[LegacyTextureProcessor] Wrote transmittance texture for glass: %s\n", outputPath.c_str());
                                }
                            } else if (m_debugOutput) {
                                Msg("[LegacyTextureProcessor] Failed to write transmittance DDS for glass: %s\n", transmittanceTexPath.c_str());
                            }
                        } else if (m_debugOutput) {
                            Msg("[LegacyTextureProcessor] Failed to extract transmittance texture data for glass: %s\n", transmittanceTexPath.c_str());
                        }
                    }
                } else if (m_debugOutput) {
                    Msg("[LegacyTextureProcessor] Failed to read VTF file for glass transmittance texture: %s\n", transmittanceTexPath.c_str());
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
        
        if (info.isGlass) {
            // Glass materials use AperturePBR_Translucent shader
            materialsUsda << "                uniform asset info:mdl:sourceAsset = @AperturePBR_Translucent.mdl@\n";
            materialsUsda << "                uniform token info:mdl:sourceAsset:subIdentifier = \"AperturePBR_Translucent\"\n";
            
            // Glass-specific properties
            materialsUsda << "                float inputs:ior_constant = " << info.ior << "\n";
            materialsUsda << "                bool inputs:thin_walled = 1\n";  // Most game glass is thin-walled
            
            // Use roughness texture if available, otherwise use constant
            // This allows frosted/textured glass to have varying roughness
            if (!info.roughnessPath.empty()) {
                std::string relPath = GetRelativeTexturePath(info.roughnessPath, m_outputDirectory);
                materialsUsda << "                asset inputs:reflectionroughness_texture = @" << relPath << "@ (\n";
                materialsUsda << "                    colorSpace = \"raw\"\n";
                materialsUsda << "                )\n";
            } else {
                // Use calculated roughness (default for glass is lower)
                // If no roughness info, use 0.05 for clear glass
                float glassRoughness = (info.roughnessConstant >= 0.99f) ? 0.05f : info.roughnessConstant;
                materialsUsda << "                float inputs:reflection_roughness_constant = " << glassRoughness << "\n";
            }
            
            // Transmittance texture for colored/tinted glass
            // For Refract shaders: use $refracttinttexture if present
            // For non-Refract glass (like bottles): use base texture
            // Note: We DON'T use baseTexture for Refract because the Lua fixer may have set it to normalmap
            if (!info.transmittancePath.empty()) {
                std::string relPath = GetRelativeTexturePath(info.transmittancePath, m_outputDirectory);
                materialsUsda << "                asset inputs:transmittance_texture = @" << relPath << "@ (\n";
                materialsUsda << "                    colorSpace = \"srgb\"\n";
                materialsUsda << "                )\n";
                // use_diffuse_layer controls whether the texture appears as a diffuse surface layer
                // Enable for both Refract and non-Refract glass, but reduce opacity for Refract
                materialsUsda << "                bool inputs:use_diffuse_layer = 1\n";
                
                // For Refract shaders: reduce diffuse opacity to prevent overly bright/contrasty look
                if (info.isRefractShader) {
                    materialsUsda << "                float inputs:diffuse_color_constant_opacity = 0.3\n";
                }
            }
            
            // Add normal map support for glass (for frosted/textured glass)
            if (!info.normalPath.empty()) {
                std::string relPath = GetRelativeTexturePath(info.normalPath, m_outputDirectory);
                materialsUsda << "                int inputs:encoding = 0\n";  // 0 = octahedral
                materialsUsda << "                asset inputs:normalmap_texture = @" << relPath << "@ (\n";
                materialsUsda << "                    colorSpace = \"raw\"\n";
                materialsUsda << "                )\n";
            }
        } else {
            // Standard opaque materials use AperturePBR_Opaque shader
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

LUA_FUNCTION(LegacyTextureProcessor_SetMetallicGeneration) {
    if (!LUA->IsType(1, Type::Bool)) {
        LUA->ThrowError("Expected boolean for metallic generation");
        return 0;
    }
    
    bool enabled = LUA->GetBool(1);
    TextureProcessor::Instance().SetMetallicGeneration(enabled);
    
    if (enabled) {
        Msg("[LegacyTextureProcessor] Experimental metallic generation ENABLED\n");
        Msg("[LegacyTextureProcessor] WARNING: This may cause dark envmap materials to appear black.\n");
        Msg("[LegacyTextureProcessor] In PBR, metallic surfaces reflect their base color - black base = no reflections.\n");
    } else {
        Msg("[LegacyTextureProcessor] Metallic generation DISABLED (default)\n");
        Msg("[LegacyTextureProcessor] Dark envmap materials will use low roughness for reflections instead.\n");
    }
    return 0;
}

LUA_FUNCTION(LegacyTextureProcessor_IsMetallicGenerationEnabled) {
    LUA->PushBool(TextureProcessor::Instance().IsMetallicGenerationEnabled());
    return 1;
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
    
    LUA->PushBool(props.isSSBump);
    LUA->SetField(-2, "isSSBump");
    
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
    
    LUA->PushBool(props.isGlass);
    LUA->SetField(-2, "isGlass");
    
    LUA->PushString(props.shaderName.c_str());
    LUA->SetField(-2, "shaderName");
    
    LUA->PushString(props.surfaceProp.c_str());
    LUA->SetField(-2, "surfaceProp");
    
    // Metallic detection info
    LUA->PushNumber(props.baseTextureBrightness);
    LUA->SetField(-2, "baseTextureBrightness");
    
    LUA->PushBool(props.hasBaseTextureBrightness);
    LUA->SetField(-2, "hasBaseTextureBrightness");
    
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
    
    LUA->PushCFunction(LegacyTextureProcessor_SetMetallicGeneration);
    LUA->SetField(-2, "SetMetallicGeneration");
    
    LUA->PushCFunction(LegacyTextureProcessor_IsMetallicGenerationEnabled);
    LUA->SetField(-2, "IsMetallicGenerationEnabled");
    
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
    
    LUA->PushCFunction(LegacyTextureProcessor_SetMetallicGeneration);
    LUA->SetField(-2, "SetMetallicGeneration");
    
    LUA->PushCFunction(LegacyTextureProcessor_IsMetallicGenerationEnabled);
    LUA->SetField(-2, "IsMetallicGenerationEnabled");
    
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
