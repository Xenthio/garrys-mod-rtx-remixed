// =========================================================================
// legacy_texture_processor_vtf.cpp - VTF file reading and processing utilities
// =========================================================================
// Implementation of low-level VTF operations
// =========================================================================

#ifdef _WIN64

#include "legacy_texture_processor_vtf.h"
#include "legacy_texture_processor.h"
#include <tier0/dbg.h>
#include <filesystem.h>
#include <algorithm>
#include <cstring>
#include <cmath>

namespace LegacyTextureProcessor {
namespace VTF {

// =========================================================================
// DXT Block Decompression Helpers (file-local)
// =========================================================================

// Decompress a single DXT1 4x4 block
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

// Decompress DXT5 alpha block
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

// =========================================================================
// Utility Functions
// =========================================================================

size_t GetImageDataSize(uint32_t width, uint32_t height, VTFImageFormat format) {
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

size_t GetTotalMipmapSize(uint32_t width, uint32_t height, uint8_t mipmapCount, VTFImageFormat format) {
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

// =========================================================================
// VTF File Operations
// =========================================================================

bool ReadVTFFile(IFileSystem* fileSystem, 
                 const std::string& path, 
                 std::vector<uint8_t>& outData,
                 bool debugOutput) {
    if (!fileSystem) {
        Warning("[VTF] Filesystem not available\n");
        return false;
    }
    
    // Build full path
    std::string fullPath = "materials/" + path;
    if (fullPath.find(".vtf") == std::string::npos) {
        fullPath += ".vtf";
    }
    
    // Open file
    FileHandle_t file = fileSystem->Open(fullPath.c_str(), "rb", "GAME");
    if (!file) {
        // Try without materials/ prefix
        file = fileSystem->Open(path.c_str(), "rb", "GAME");
        if (!file) {
            if (debugOutput) {
                Msg("[VTF] Could not open: %s\n", fullPath.c_str());
            }
            return false;
        }
    }
    
    // Get file size
    int fileSize = fileSystem->Size(file);
    if (fileSize <= 0 || static_cast<size_t>(fileSize) > MAX_VTF_FILE_SIZE) {
        fileSystem->Close(file);
        Warning("[VTF] Invalid file size for %s: %d\n", fullPath.c_str(), fileSize);
        return false;
    }
    
    // Read file data
    outData.resize(fileSize);
    int bytesRead = fileSystem->Read(outData.data(), fileSize, file);
    fileSystem->Close(file);
    
    if (bytesRead != fileSize) {
        Warning("[VTF] Read error for %s: expected %d, got %d\n", 
                fullPath.c_str(), fileSize, bytesRead);
        return false;
    }
    
    return true;
}

bool ParseVTFHeader(const std::vector<uint8_t>& fileData, 
                    VTFFileHeader& outHeader,
                    bool debugOutput) {
    if (fileData.size() < sizeof(VTFFileHeader)) {
        Warning("[VTF] File too small for header\n");
        return false;
    }
    
    memcpy(&outHeader, fileData.data(), sizeof(VTFFileHeader));
    
    // Verify signature
    if (memcmp(outHeader.signature, "VTF\0", 4) != 0) {
        Warning("[VTF] Invalid signature\n");
        return false;
    }
    
    // Verify version (support 7.0 - 7.5)
    if (outHeader.version[0] != VTF_MAJOR_VERSION_SUPPORTED || outHeader.version[1] > VTF_MAX_MINOR_VERSION) {
        Warning("[VTF] Unsupported version %d.%d\n", 
                outHeader.version[0], outHeader.version[1]);
        return false;
    }
    
    if (debugOutput) {
        Msg("[VTF] %dx%d, format %d, %d mips\n",
            outHeader.width, outHeader.height, outHeader.imageFormat, outHeader.mipmapCount);
    }
    
    return true;
}

// =========================================================================
// DXT Decompression
// =========================================================================

bool DecompressDXT1(const uint8_t* compressedData, 
                    uint32_t width, 
                    uint32_t height,
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

bool DecompressDXT5(const uint8_t* compressedData, 
                    uint32_t width, 
                    uint32_t height,
                    std::vector<uint8_t>& outRGBA) {
    uint32_t blocksX = (width + 3) / 4;
    uint32_t blocksY = (height + 3) / 4;
    
    outRGBA.resize(width * height * 4);
    
    const uint8_t* blockPtr = compressedData;
    
    for (uint32_t by = 0; by < blocksY; by++) {
        for (uint32_t bx = 0; bx < blocksX; bx++) {
            uint8_t blockPixels[4 * 4 * 4]; // 4x4 RGBA
            
            // First 8 bytes: alpha block
            DecompressDXT5AlphaBlock(blockPtr, blockPixels, 4 * 4);
            blockPtr += 8;
            
            // Save alpha values before color decompression
            uint8_t savedAlpha[16];
            for (int i = 0; i < 16; i++) {
                savedAlpha[i] = blockPixels[i * 4 + 3];
            }
            
            // Next 8 bytes: color block (DXT1)
            DecompressDXT1Block(blockPtr, blockPixels, 4 * 4);
            blockPtr += 8;
            
            // Restore alpha values
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

// =========================================================================
// Pixel Data Extraction
// =========================================================================

bool ExtractVTFPixelData(const std::vector<uint8_t>& fileData, 
                         const VTFFileHeader& header,
                         ConvertedTexture& outTexture, 
                         bool isNormalMap,
                         bool debugOutput) {
    outTexture.width = header.width;
    outTexture.height = header.height;
    outTexture.mipLevels = 1;
    outTexture.isNormalMap = isNormalMap;
    outTexture.format = isNormalMap ? REMIXAPI_FORMAT_R8G8B8A8_UNORM : REMIXAPI_FORMAT_R8G8B8A8_SRGB;
    
    VTFImageFormat srcFormat = (VTFImageFormat)header.imageFormat;
    
    // Calculate offset to high-res image data
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
    // Skip all smaller mipmaps and all frames except the first
    size_t smallerMipsSize = 0;
    for (int mip = mipmapCount - 1; mip >= 1; mip--) {
        uint32_t mipW = max(1u, header.width >> mip);
        uint32_t mipH = max(1u, header.height >> mip);
        smallerMipsSize += GetImageDataSize(mipW, mipH, srcFormat) * frameCount;
    }
    
    dataOffset += smallerMipsSize;
    
    // Calculate size of largest mip
    size_t largestMipSize = GetImageDataSize(header.width, header.height, srcFormat);
    
    if (dataOffset + largestMipSize > fileData.size()) {
        // Try alternative: maybe single frame with no low-res image
        dataOffset = header.headerSize;
        for (int mip = mipmapCount - 1; mip >= 1; mip--) {
            uint32_t mipW = max(1u, header.width >> mip);
            uint32_t mipH = max(1u, header.height >> mip);
            dataOffset += GetImageDataSize(mipW, mipH, srcFormat);
        }
        
        if (dataOffset + largestMipSize > fileData.size()) {
            if (debugOutput) {
                Msg("[VTF] Data too small: offset %zu + size %zu > total %zu\n",
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
            if (debugOutput) {
                Msg("[VTF] Unsupported format: %d\n", srcFormat);
            }
            return false;
    }
    
    outTexture.pixelData = std::move(rgba);
    
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
    
    const float normalStrength = 1.5f;
    
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
        
        // Boost normal strength
        nx *= normalStrength;
        ny *= normalStrength;
        
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
        Msg("[VTF] Converted SSBump to normal (%dx%d) strength %.1fx\n", width, height, normalStrength);
    }
}

} // namespace VTF
} // namespace LegacyTextureProcessor

#endif // _WIN64
