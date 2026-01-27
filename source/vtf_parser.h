// =========================================================================
// vtf_parser.h - Standalone VTF (Valve Texture Format) parser
// =========================================================================
// Generic VTF file reading and processing that can be used by any system.
// No dependencies on LegacyTextureProcessor or other subsystems.
// =========================================================================

#pragma once

#ifdef _WIN64

#include <vector>
#include <string>
#include <cstdint>

// Forward declarations
class IFileSystem;

namespace VTFParser {

// =========================================================================
// Constants
// =========================================================================
constexpr int VTF_MAJOR_VERSION_SUPPORTED = 7;
constexpr int VTF_MAX_MINOR_VERSION = 5;

// Maximum VTF file size to load into memory.
// 256 MB should cover all Source Engine textures (typical max is 4096x4096 RGBA = 64MB).
// Larger files are rejected to prevent out-of-memory issues.
constexpr size_t MAX_VTF_FILE_SIZE = 256 * 1024 * 1024;  // 256 MB

// Maximum texture dimension for solid-color checking.
// Larger textures are skipped for performance (solid-color textures are typically small).
constexpr size_t MAX_SOLID_COLOR_CHECK_SIZE = 512;  // Only check textures up to 512x512

// =========================================================================
// VTF Image Format enumeration (matches Source Engine's ImageFormat)
// =========================================================================
enum VTFImageFormat {
    IMAGE_FORMAT_NONE = -1,
    IMAGE_FORMAT_RGBA8888 = 0,
    IMAGE_FORMAT_ABGR8888 = 1,
    IMAGE_FORMAT_RGB888 = 2,
    IMAGE_FORMAT_BGR888 = 3,
    IMAGE_FORMAT_RGB565 = 4,
    IMAGE_FORMAT_I8 = 5,
    IMAGE_FORMAT_IA88 = 6,
    IMAGE_FORMAT_P8 = 7,
    IMAGE_FORMAT_A8 = 8,
    IMAGE_FORMAT_RGB888_BLUESCREEN = 9,
    IMAGE_FORMAT_BGR888_BLUESCREEN = 10,
    IMAGE_FORMAT_ARGB8888 = 11,
    IMAGE_FORMAT_BGRA8888 = 12,
    IMAGE_FORMAT_DXT1 = 13,
    IMAGE_FORMAT_DXT3 = 14,
    IMAGE_FORMAT_DXT5 = 15,
    IMAGE_FORMAT_BGRX8888 = 16,
    IMAGE_FORMAT_BGR565 = 17,
    IMAGE_FORMAT_BGRX5551 = 18,
    IMAGE_FORMAT_BGRA4444 = 19,
    IMAGE_FORMAT_DXT1_ONEBITALPHA = 20,
    IMAGE_FORMAT_BGRA5551 = 21,
    IMAGE_FORMAT_UV88 = 22,
    IMAGE_FORMAT_UVWQ8888 = 23,
    IMAGE_FORMAT_RGBA16161616F = 24,
    IMAGE_FORMAT_RGBA16161616 = 25,
    IMAGE_FORMAT_UVLX8888 = 26,
};

// =========================================================================
// VTF File Header Structure
// =========================================================================
#pragma pack(push, 1)
struct VTFFileHeader {
    char signature[4];          // "VTF\0"
    uint32_t version[2];        // [major, minor]
    uint32_t headerSize;
    uint16_t width;
    uint16_t height;
    uint32_t flags;
    uint16_t frames;
    uint16_t firstFrame;
    uint8_t padding0[4];
    float reflectivity[3];
    uint8_t padding1[4];
    float bumpmapScale;
    uint32_t imageFormat;       // VTFImageFormat
    uint8_t mipmapCount;
    uint32_t lowResImageFormat;
    uint8_t lowResImageWidth;
    uint8_t lowResImageHeight;
    // Additional fields for version 7.2+
    uint16_t depth;             // For volume textures
};
#pragma pack(pop)

// =========================================================================
// Parsed Texture Info
// =========================================================================
struct TextureInfo {
    uint32_t width = 0;
    uint32_t height = 0;
    VTFImageFormat format = IMAGE_FORMAT_NONE;
    uint8_t mipmapCount = 0;
    std::vector<uint8_t> pixelData;  // RGBA8888 format
    bool valid = false;
};

// =========================================================================
// Solid Color Result
// =========================================================================
struct SolidColorResult {
    bool isSolidColor = false;
    uint8_t r = 0, g = 0, b = 0, a = 255;
    bool error = false;
    std::string errorMessage;
};

// =========================================================================
// VTF File Operations
// =========================================================================

// Read a VTF file from the Source Engine filesystem
// @param fileSystem - Source Engine IFileSystem interface
// @param path - Path to VTF file (with or without "materials/" prefix and ".vtf" extension)
// @param outData - Output buffer for file data
// @param debugOutput - Whether to output debug messages
// @return true on success
bool ReadVTFFile(IFileSystem* fileSystem, 
                 const std::string& path, 
                 std::vector<uint8_t>& outData,
                 bool debugOutput = false);

// Parse and validate a VTF header from file data
// @param fileData - Raw VTF file data
// @param outHeader - Output header structure
// @param debugOutput - Whether to output debug messages
// @return true if header is valid
bool ParseVTFHeader(const std::vector<uint8_t>& fileData, 
                    VTFFileHeader& outHeader,
                    bool debugOutput = false);

// Extract pixel data from a VTF file, converting to RGBA8888
// @param fileData - Raw VTF file data
// @param header - Parsed VTF header
// @param outTexture - Output texture info with pixel data
// @param debugOutput - Whether to output debug messages
// @return true on success
bool ExtractPixelData(const std::vector<uint8_t>& fileData,
                      const VTFFileHeader& header,
                      TextureInfo& outTexture,
                      bool debugOutput = false);

// =========================================================================
// Solid Color Detection
// =========================================================================

// Check if a VTF texture is a solid color (all pixels identical)
// @param fileSystem - Source Engine IFileSystem interface
// @param texturePath - Path to VTF file
// @param debugOutput - Whether to output debug messages
// @return SolidColorResult with detection results
SolidColorResult CheckSolidColor(IFileSystem* fileSystem,
                                  const std::string& texturePath,
                                  bool debugOutput = false);

// Check if raw RGBA pixel data is a solid color
// @param pixelData - RGBA8888 pixel data
// @param width - Texture width
// @param height - Texture height
// @param outR, outG, outB, outA - Output color values if solid
// @return true if all pixels are identical
bool IsSolidColorRGBA(const std::vector<uint8_t>& pixelData,
                      uint32_t width, uint32_t height,
                      uint8_t& outR, uint8_t& outG, uint8_t& outB, uint8_t& outA);

// =========================================================================
// DXT Decompression
// =========================================================================

// Decompress DXT1 compressed texture data to RGBA8888
bool DecompressDXT1(const uint8_t* compressedData, 
                    uint32_t width, 
                    uint32_t height,
                    std::vector<uint8_t>& outRGBA);

// Decompress DXT5 compressed texture data to RGBA8888
bool DecompressDXT5(const uint8_t* compressedData, 
                    uint32_t width, 
                    uint32_t height,
                    std::vector<uint8_t>& outRGBA);

// =========================================================================
// Utility Functions
// =========================================================================

// Calculate image data size for a given VTF format
size_t GetImageDataSize(uint32_t width, uint32_t height, VTFImageFormat format);

// Calculate total size of all mipmaps for one frame
size_t GetTotalMipmapSize(uint32_t width, uint32_t height, uint8_t mipmapCount, VTFImageFormat format);

// Get format name as string (for debugging)
const char* GetFormatName(VTFImageFormat format);

} // namespace VTFParser

#endif // _WIN64
