#pragma once

#ifdef _WIN64

#include <string>
#include <vector>
#include <cstdint>
#include <fstream>

// Forward declarations
class IFileSystem;

namespace LegacyTextureProcessor {

// Forward declarations
struct ConvertedTexture;
struct MaterialPBRProperties;

// =========================================================================
// Texture Generation Utilities
// =========================================================================
// This module handles:
// - DDS file writing with mipmaps
// - Roughness texture generation from Source Engine properties
// - Metallic texture generation from base texture analysis
// - Companion texture discovery (auto-finding _normal, _spec, etc.)
// =========================================================================

namespace TextureGen {

// =========================================================================
// DDS File Writing
// =========================================================================

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

// Calculate number of mip levels for a given texture size
// @param width - Texture width
// @param height - Texture height
// @return Number of mip levels
inline uint32_t CalculateMipLevels(uint32_t width, uint32_t height) {
    uint32_t mipCount = 1;
    uint32_t size = (width > height) ? width : height;
    while (size > 1) {
        size /= 2;
        mipCount++;
    }
    return mipCount;
}

// Write DDS header to file stream
// @param file - Output file stream
// @param width - Texture width
// @param height - Texture height  
// @param hasAlpha - Whether texture has alpha channel
// @param mipCount - Number of mip levels
// @param debugOutput - Whether to output debug messages
// @return true on success
bool WriteDDSHeader(std::ofstream& file, 
                    uint32_t width, 
                    uint32_t height, 
                    bool hasAlpha, 
                    uint32_t mipCount,
                    bool debugOutput = false);

// Write texture to DDS file with mipmaps
// @param texture - Source texture data (RGBA8888)
// @param outputPath - Path to output DDS file
// @param debugOutput - Whether to output debug messages
// @return true on success
bool WriteTextureToDDS(const ConvertedTexture& texture, 
                       const std::string& outputPath,
                       bool debugOutput = false);

// =========================================================================
// Roughness Texture Generation
// =========================================================================

// Generate roughness texture from Source Engine material properties
// Sources (in priority order):
// - Phong materials: $phongexponenttexture, $basemapalphaphongmask, normal alpha
// - Non-phong: $envmapmask, $basealphaenvmapmask, discovered masks, normal alpha
// 
// @param props - Material PBR properties
// @param outTexture - Output roughness texture (grayscale in RGB, alpha=255)
// @param fileSystem - Source Engine filesystem for reading VTF files
// @param debugOutput - Whether to output debug messages
// @return true if roughness texture was generated, false if constant value should be used
bool GenerateRoughnessTexture(const MaterialPBRProperties& props,
                               ConvertedTexture& outTexture,
                               IFileSystem* fileSystem,
                               bool debugOutput = false);

// =========================================================================
// Metallic Texture Generation
// =========================================================================

// Generate metallic texture from base texture brightness analysis
// Dark pixels + envmap = metallic (chrome/metal)
// Bright pixels = non-metallic (diffuse)
//
// @param props - Material PBR properties
// @param outTexture - Output metallic texture (grayscale in RGB, alpha=255)
// @param fileSystem - Source Engine filesystem for reading VTF files
// @param debugOutput - Whether to output debug messages
// @return true if metallic texture was generated, false if constant value should be used
bool GenerateMetallicTexture(const MaterialPBRProperties& props,
                              ConvertedTexture& outTexture,
                              IFileSystem* fileSystem,
                              bool debugOutput = false);

// =========================================================================
// Companion Texture Discovery
// =========================================================================

// Discover companion textures that follow naming conventions
// Searches for textures like:
// - _normal, _n, _norm (normal maps)
// - _height, _h, _bump (height maps)
// - _mask, _spec, _gloss (roughness masks)
// - _ao, _occlusion (ambient occlusion)
//
// @param baseTexturePath - Base texture path (e.g., "metal/metal001")
// @param props - Material properties to update with discovered textures
// @param fileSystem - Source Engine filesystem for checking texture existence
// @param autoDiscoverEnabled - Whether auto-discovery is enabled
// @param debugOutput - Whether to output debug messages
void DiscoverCompanionTextures(const std::string& baseTexturePath,
                                MaterialPBRProperties& props,
                                IFileSystem* fileSystem,
                                bool autoDiscoverEnabled,
                                bool debugOutput = false);

} // namespace TextureGen
} // namespace LegacyTextureProcessor

#endif // _WIN64
