// =========================================================================
// legacy_texture_processor_vtf.h - VTF file reading and processing utilities
// =========================================================================
// Low-level VTF (Valve Texture Format) file operations including:
// - VTF file reading from Source Engine filesystem
// - Header parsing and validation
// - DXT1/DXT5 decompression
// - Pixel data extraction and format conversion
// - Normal map conversion (octahedral encoding, SSBump)
// =========================================================================

#pragma once

#ifdef _WIN64

#include <vector>
#include <string>
#include <cstdint>

// Forward declarations
class IFileSystem;

namespace MaterialPipeline {
namespace ToPBR {

// Forward declarations within namespace
struct VTFFileHeader;
struct ConvertedTexture;
namespace VTF {

// =========================================================================
// Constants
// =========================================================================
constexpr int VTF_MAJOR_VERSION_SUPPORTED = 7;
constexpr int VTF_MAX_MINOR_VERSION = 5;
constexpr size_t MAX_VTF_FILE_SIZE = 256 * 1024 * 1024;  // 256 MB

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

// =========================================================================
// DXT Decompression
// =========================================================================

// Decompress DXT1 compressed texture data to RGBA8888
// @param compressedData - Pointer to compressed data
// @param width - Texture width in pixels
// @param height - Texture height in pixels
// @param outRGBA - Output buffer for RGBA pixel data
// @return true on success
bool DecompressDXT1(const uint8_t* compressedData, 
                    uint32_t width, 
                    uint32_t height,
                    std::vector<uint8_t>& outRGBA);

// Decompress DXT5 compressed texture data to RGBA8888
// @param compressedData - Pointer to compressed data
// @param width - Texture width in pixels
// @param height - Texture height in pixels
// @param outRGBA - Output buffer for RGBA pixel data
// @return true on success
bool DecompressDXT5(const uint8_t* compressedData, 
                    uint32_t width, 
                    uint32_t height,
                    std::vector<uint8_t>& outRGBA);

// =========================================================================
// Pixel Data Extraction
// =========================================================================

// Extract pixel data from a VTF file, converting to RGBA8888
// @param fileData - Raw VTF file data
// @param header - Parsed VTF header
// @param outTexture - Output converted texture data
// @param isNormalMap - Whether this texture is a normal map (affects conversion)
// @param debugOutput - Whether to output debug messages
// @return true on success
bool ExtractVTFPixelData(const std::vector<uint8_t>& fileData, 
                         const VTFFileHeader& header,
                         ConvertedTexture& outTexture, 
                         bool isNormalMap,
                         bool debugOutput = false);

// =========================================================================
// Normal Map Conversion
// =========================================================================

// Convert DirectX-style normal map to hemispherical octahedral format for RTX Remix
// Based on NVIDIA's LightspeedOctahedralConverter
// @param texture - Texture to convert in-place
// @param debugOutput - Whether to output debug messages
void ConvertNormalMapToOctahedral(ConvertedTexture& texture, bool debugOutput = false);

// Convert SSBump texture to standard tangent-space normal map
// SSBump stores directional occlusion in 3 basis directions, not XYZ components
// Based on Valve's Source SDK common_fxc.h
// @param texture - Texture to convert in-place
// @param debugOutput - Whether to output debug messages
void ConvertSSBumpToNormal(ConvertedTexture& texture, bool debugOutput = false);

// =========================================================================
// Utility Functions
// =========================================================================

// Calculate image data size for a given VTF format
// @param width - Image width
// @param height - Image height  
// @param format - VTF image format
// @return Size in bytes
size_t GetImageDataSize(uint32_t width, uint32_t height, VTFImageFormat format);

// Calculate total size of all mipmaps for one frame
// @param width - Base mip width
// @param height - Base mip height
// @param mipmapCount - Number of mip levels
// @param format - VTF image format
// @return Total size in bytes
size_t GetTotalMipmapSize(uint32_t width, uint32_t height, uint8_t mipmapCount, VTFImageFormat format);

} // namespace VTF
} // namespace ToPBR
} // namespace MaterialPipeline

#endif // _WIN64
