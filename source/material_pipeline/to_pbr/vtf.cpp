// =========================================================================
// vtf.cpp - VTF file reading and processing utilities
// =========================================================================
// Implementation that delegates to the standalone vtf_parser module for
// core VTF functionality, while providing ToPBR-specific features like
// ConvertedTexture integration and normal map conversion.
// =========================================================================

#ifdef _WIN64

#include "vtf.h"
#include "to_pbr.h"
#include "../../vtf_parser.h"  // Use standalone VTF parser for core functionality
#include <tier0/dbg.h>
#include <filesystem.h>
#include <algorithm>
#include <cstring>
#include <cmath>

namespace MaterialPipeline {
namespace ToPBR {
namespace VTF {

// =========================================================================
// Utility Functions - Delegate to standalone VTFParser
// =========================================================================

size_t GetImageDataSize(uint32_t width, uint32_t height, VTFImageFormat format) {
    // Convert to standalone parser's format enum
    return VTFParser::GetImageDataSize(width, height, static_cast<VTFParser::VTFImageFormat>(format));
}

size_t GetTotalMipmapSize(uint32_t width, uint32_t height, uint8_t mipmapCount, VTFImageFormat format) {
    return VTFParser::GetTotalMipmapSize(width, height, mipmapCount, static_cast<VTFParser::VTFImageFormat>(format));
}

// =========================================================================
// VTF File Operations - Delegate to standalone VTFParser
// =========================================================================

bool ReadVTFFile(IFileSystem* fileSystem, 
                 const std::string& path, 
                 std::vector<uint8_t>& outData,
                 bool debugOutput) {
    return VTFParser::ReadVTFFile(fileSystem, path, outData, debugOutput);
}

bool ParseVTFHeader(const std::vector<uint8_t>& fileData, 
                    VTFFileHeader& outHeader,
                    bool debugOutput) {
    // Parse using standalone parser
    VTFParser::VTFFileHeader parserHeader;
    if (!VTFParser::ParseVTFHeader(fileData, parserHeader, debugOutput)) {
        return false;
    }
    
    // Copy to our header structure (they should be layout-compatible)
    static_assert(sizeof(VTFFileHeader) >= sizeof(VTFParser::VTFFileHeader),
                  "VTFFileHeader must be at least as large as VTFParser::VTFFileHeader");
    
    memcpy(outHeader.signature, parserHeader.signature, 4);
    outHeader.version[0] = parserHeader.version[0];
    outHeader.version[1] = parserHeader.version[1];
    outHeader.headerSize = parserHeader.headerSize;
    outHeader.width = parserHeader.width;
    outHeader.height = parserHeader.height;
    outHeader.flags = parserHeader.flags;
    outHeader.frames = parserHeader.frames;
    outHeader.firstFrame = parserHeader.firstFrame;
    outHeader.reflectivity[0] = parserHeader.reflectivity[0];
    outHeader.reflectivity[1] = parserHeader.reflectivity[1];
    outHeader.reflectivity[2] = parserHeader.reflectivity[2];
    outHeader.bumpScale = parserHeader.bumpmapScale;
    outHeader.imageFormat = parserHeader.imageFormat;
    outHeader.mipmapCount = parserHeader.mipmapCount;
    outHeader.lowResImageFormat = parserHeader.lowResImageFormat;
    outHeader.lowResImageWidth = parserHeader.lowResImageWidth;
    outHeader.lowResImageHeight = parserHeader.lowResImageHeight;
    outHeader.depth = parserHeader.depth;
    
    return true;
}

// =========================================================================
// DXT Decompression - Delegate to standalone VTFParser
// =========================================================================

bool DecompressDXT1(const uint8_t* compressedData, 
                    uint32_t width, 
                    uint32_t height,
                    std::vector<uint8_t>& outRGBA) {
    return VTFParser::DecompressDXT1(compressedData, width, height, outRGBA);
}

bool DecompressDXT5(const uint8_t* compressedData, 
                    uint32_t width, 
                    uint32_t height,
                    std::vector<uint8_t>& outRGBA) {
    return VTFParser::DecompressDXT5(compressedData, width, height, outRGBA);
}

// =========================================================================
// Pixel Data Extraction
// =========================================================================

bool ExtractVTFPixelData(const std::vector<uint8_t>& fileData, 
                         const VTFFileHeader& header,
                         ConvertedTexture& outTexture, 
                         bool isNormalMap,
                         bool debugOutput) {
    // Use standalone parser to extract pixel data
    VTFParser::VTFFileHeader parserHeader;
    memcpy(parserHeader.signature, header.signature, 4);
    parserHeader.version[0] = header.version[0];
    parserHeader.version[1] = header.version[1];
    parserHeader.headerSize = header.headerSize;
    parserHeader.width = header.width;
    parserHeader.height = header.height;
    parserHeader.flags = header.flags;
    parserHeader.frames = header.frames;
    parserHeader.firstFrame = header.firstFrame;
    parserHeader.reflectivity[0] = header.reflectivity[0];
    parserHeader.reflectivity[1] = header.reflectivity[1];
    parserHeader.reflectivity[2] = header.reflectivity[2];
    parserHeader.bumpmapScale = header.bumpScale;
    parserHeader.imageFormat = header.imageFormat;
    parserHeader.mipmapCount = header.mipmapCount;
    parserHeader.lowResImageFormat = header.lowResImageFormat;
    parserHeader.lowResImageWidth = header.lowResImageWidth;
    parserHeader.lowResImageHeight = header.lowResImageHeight;
    parserHeader.depth = header.depth;
    
    VTFParser::TextureInfo parserTexture;
    if (!VTFParser::ExtractPixelData(fileData, parserHeader, parserTexture, debugOutput)) {
        return false;
    }
    
    // Convert to ConvertedTexture
    outTexture.width = parserTexture.width;
    outTexture.height = parserTexture.height;
    outTexture.mipLevels = 1;
    outTexture.isNormalMap = isNormalMap;
    outTexture.format = isNormalMap ? REMIXAPI_FORMAT_R8G8B8A8_UNORM : REMIXAPI_FORMAT_R8G8B8A8_SRGB;
    outTexture.pixelData = std::move(parserTexture.pixelData);
    
    // Convert normal maps to octahedral format
    if (isNormalMap) {
        ConvertNormalMapToOctahedral(outTexture, debugOutput);
    }
    
    return true;
}

// =========================================================================
// Normal Map Conversion
// =========================================================================

void ConvertNormalMapToOctahedral(ConvertedTexture& texture, bool debugOutput) {
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
        
        // Flip inward-pointing normals (z < 0)
        if (b < 128) {
            b = 255 - b;
        }
        
        // Convert from [0, 255] to [-1, 1]
        float nx = (r / 255.0f) * 2.0f - 1.0f;
        float ny = (g / 255.0f) * 2.0f - 1.0f;
        float nz = (b / 255.0f) * 2.0f - 1.0f;
        
        // Normalize
        float len = std::sqrt(nx * nx + ny * ny + nz * nz);
        if (len > 0.0001f) {
            nx /= len;
            ny /= len;
            nz /= len;
        } else {
            nx = 0.0f;
            ny = 0.0f;
            nz = 1.0f;
        }
        
        // Convert to octahedral encoding
        float absSum = fabs(nx) + fabs(ny) + fabs(nz);
        float octX = nx / absSum;
        float octY = ny / absSum;
        
        // Hemispherical encoding
        float resultX = octX + octY;
        float resultY = octX - octY;
        
        // Convert from [-1, 1] to [0, 1]
        resultX = resultX * 0.5f + 0.5f;
        resultY = resultY * 0.5f + 0.5f;
        
        // Convert to [0, 255]
        uint8_t outR = static_cast<uint8_t>(std::clamp(resultX * 255.0f + 0.5f, 0.0f, 255.0f));
        uint8_t outG = static_cast<uint8_t>(std::clamp(resultY * 255.0f + 0.5f, 0.0f, 255.0f));
        
        octahedralData[dstIdx + 0] = outR;
        octahedralData[dstIdx + 1] = outG;
        octahedralData[dstIdx + 2] = 0;
        octahedralData[dstIdx + 3] = 255;
    }
    
    texture.pixelData = std::move(octahedralData);
    
    if (debugOutput) {
        Msg("[VTF] Converted to octahedral format (%dx%d)\n", width, height);
    }
}

void ConvertSSBumpToNormal(ConvertedTexture& texture, bool debugOutput) {
    if (texture.pixelData.empty()) return;
    
    uint32_t width = texture.width;
    uint32_t height = texture.height;
    size_t pixelCount = width * height;
    
    std::vector<uint8_t> normalData(pixelCount * 4);
    
    // Bump basis transpose from Source SDK
    const float OO_SQRT_3 = 0.57735025882720947f;
    const float basisT0_x = 0.81649661064147949f;
    const float basisT0_y = -0.40824833512306213f;
    const float basisT0_z = -0.40824833512306213f;
    const float basisT1_x = 0.0f;
    const float basisT1_y = 0.70710676908493042f;
    const float basisT1_z = -0.7071068286895752f;
    const float basisT2_x = OO_SQRT_3;
    const float basisT2_y = OO_SQRT_3;
    const float basisT2_z = OO_SQRT_3;
    
    // Source's SSBump lighting response has more pronounced relief than the
    // reconstructed unit normal otherwise produces in Remix. Strengthen only
    // the tangent-plane components before renormalizing; ordinary normal maps
    // do not pass through this conversion.
    const float normalStrength = 2.0f;
    // Valve's SSBump generator preserves the source height field in alpha.
    // Reintroduce its gradient here; RGB reconstruction alone recovers only
    // the directional-lighting ratios and discards much of that relief.
    const float heightDetailStrength = 0.1f;
    
    for (size_t i = 0; i < pixelCount; i++) {
        size_t srcIdx = i * 4;
        size_t dstIdx = i * 4;
        
        float r = texture.pixelData[srcIdx + 0] / 255.0f;
        float g = texture.pixelData[srcIdx + 1] / 255.0f;
        float b = texture.pixelData[srcIdx + 2] / 255.0f;
        
        // Transform SSBump to tangent-space normal
        float nx = r * basisT0_x + g * basisT0_y + b * basisT0_z;
        float ny = r * basisT1_x + g * basisT1_y + b * basisT1_z;
        float nz = r * basisT2_x + g * basisT2_y + b * basisT2_z;

        // Match Source's wrapping height-field construction. The central
        // difference yields the local height slope without baking the height
        // value itself into the normal.
        const uint32_t x = static_cast<uint32_t>(i % width);
        const uint32_t y = static_cast<uint32_t>(i / width);
        const uint32_t xLeft = (x + width - 1) % width;
        const uint32_t xRight = (x + 1) % width;
        const uint32_t yUp = (y + height - 1) % height;
        const uint32_t yDown = (y + 1) % height;
        const float heightLeft = texture.pixelData[(y * width + xLeft) * 4 + 3] / 255.0f;
        const float heightRight = texture.pixelData[(y * width + xRight) * 4 + 3] / 255.0f;
        const float heightUp = texture.pixelData[(yUp * width + x) * 4 + 3] / 255.0f;
        const float heightDown = texture.pixelData[(yDown * width + x) * 4 + 3] / 255.0f;
        const float heightDx = (heightRight - heightLeft) * 0.5f;
        const float heightDy = (heightDown - heightUp) * 0.5f;

        // Work in tangent-plane slopes so the existing directional boost and
        // the recovered height detail combine predictably before normalization.
        if (nz > 0.0001f) {
            nx = (nx / nz) * normalStrength - heightDx * heightDetailStrength;
            ny = (ny / nz) * normalStrength - heightDy * heightDetailStrength;
            nz = 1.0f;
        } else {
            nx *= normalStrength;
            ny *= normalStrength;
        }
        
        // Renormalize
        float len = sqrtf(nx * nx + ny * ny + nz * nz);
        if (len > 0.0001f) {
            nx /= len;
            ny /= len;
            nz /= len;
        }
        
        // Convert to [0, 255]
        uint8_t outR = static_cast<uint8_t>(std::clamp((nx * 0.5f + 0.5f) * 255.0f + 0.5f, 0.0f, 255.0f));
        uint8_t outG = static_cast<uint8_t>(std::clamp((ny * 0.5f + 0.5f) * 255.0f + 0.5f, 0.0f, 255.0f));
        uint8_t outB = static_cast<uint8_t>(std::clamp((nz * 0.5f + 0.5f) * 255.0f + 0.5f, 0.0f, 255.0f));
        
        normalData[dstIdx + 0] = outR;
        normalData[dstIdx + 1] = outG;
        normalData[dstIdx + 2] = outB;
        normalData[dstIdx + 3] = 255;
    }
    
    texture.pixelData = std::move(normalData);
    
    if (debugOutput) {
        Msg("[VTF] Converted SSBump to normal (%dx%d) strength %.1fx, height detail %.1fx\n",
            width, height, normalStrength, heightDetailStrength);
    }
}

} // namespace VTF
} // namespace ToPBR
} // namespace MaterialPipeline

#endif // _WIN64
