// =========================================================================
// vtf_parser.cpp - Standalone VTF (Valve Texture Format) parser
// =========================================================================
// Generic VTF file reading and processing that can be used by any system.
// =========================================================================

#ifdef _WIN64

#include "vtf_parser.h"
#include <tier0/dbg.h>
#include <filesystem.h>
#include <algorithm>
#include <cstring>
#include <cmath>

namespace VTFParser {

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

const char* GetFormatName(VTFImageFormat format) {
    switch (format) {
        case IMAGE_FORMAT_RGBA8888: return "RGBA8888";
        case IMAGE_FORMAT_ABGR8888: return "ABGR8888";
        case IMAGE_FORMAT_RGB888: return "RGB888";
        case IMAGE_FORMAT_BGR888: return "BGR888";
        case IMAGE_FORMAT_DXT1: return "DXT1";
        case IMAGE_FORMAT_DXT3: return "DXT3";
        case IMAGE_FORMAT_DXT5: return "DXT5";
        case IMAGE_FORMAT_BGRA8888: return "BGRA8888";
        case IMAGE_FORMAT_BGRX8888: return "BGRX8888";
        default: return "Unknown";
    }
}

// =========================================================================
// VTF File Operations
// =========================================================================

bool ReadVTFFile(IFileSystem* fileSystem, 
                 const std::string& path, 
                 std::vector<uint8_t>& outData,
                 bool debugOutput) {
    if (!fileSystem) {
        if (debugOutput) Warning("[VTFParser] Filesystem not available\n");
        return false;
    }
    
    // Build full path
    std::string fullPath = path;
    
    // Add materials/ prefix if not present
    if (fullPath.find("materials/") != 0 && fullPath.find("materials\\") != 0) {
        fullPath = "materials/" + fullPath;
    }
    
    // Add .vtf extension if not present
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
                Msg("[VTFParser] Could not open: %s\n", fullPath.c_str());
            }
            return false;
        }
    }
    
    // Get file size
    int fileSize = fileSystem->Size(file);
    if (fileSize <= 0 || static_cast<size_t>(fileSize) > MAX_VTF_FILE_SIZE) {
        fileSystem->Close(file);
        if (debugOutput) Warning("[VTFParser] Invalid file size for %s: %d\n", fullPath.c_str(), fileSize);
        return false;
    }
    
    // Read file data
    outData.resize(fileSize);
    int bytesRead = fileSystem->Read(outData.data(), fileSize, file);
    fileSystem->Close(file);
    
    if (bytesRead != fileSize) {
        if (debugOutput) Warning("[VTFParser] Read error for %s: expected %d, got %d\n", 
                fullPath.c_str(), fileSize, bytesRead);
        return false;
    }
    
    return true;
}

bool ParseVTFHeader(const std::vector<uint8_t>& fileData, 
                    VTFFileHeader& outHeader,
                    bool debugOutput) {
    if (fileData.size() < sizeof(VTFFileHeader)) {
        if (debugOutput) Warning("[VTFParser] File too small for header\n");
        return false;
    }
    
    memcpy(&outHeader, fileData.data(), sizeof(VTFFileHeader));
    
    // Verify signature
    if (memcmp(outHeader.signature, "VTF\0", 4) != 0) {
        if (debugOutput) Warning("[VTFParser] Invalid signature\n");
        return false;
    }
    
    // Verify version (support 7.0 - 7.5)
    if (outHeader.version[0] != VTF_MAJOR_VERSION_SUPPORTED || outHeader.version[1] > VTF_MAX_MINOR_VERSION) {
        if (debugOutput) Warning("[VTFParser] Unsupported version %d.%d\n", 
                outHeader.version[0], outHeader.version[1]);
        return false;
    }
    
    if (debugOutput) {
        Msg("[VTFParser] %dx%d, format %s (%d), %d mips\n",
            outHeader.width, outHeader.height, 
            GetFormatName((VTFImageFormat)outHeader.imageFormat),
            outHeader.imageFormat, outHeader.mipmapCount);
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

bool ExtractPixelData(const std::vector<uint8_t>& fileData,
                      const VTFFileHeader& header,
                      TextureInfo& outTexture,
                      bool debugOutput) {
    outTexture.width = header.width;
    outTexture.height = header.height;
    outTexture.format = (VTFImageFormat)header.imageFormat;
    outTexture.mipmapCount = header.mipmapCount;
    outTexture.valid = false;
    
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
                Msg("[VTFParser] Data too small: offset %zu + size %zu > total %zu\n",
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
                Msg("[VTFParser] Unsupported format: %s (%d)\n", GetFormatName(srcFormat), srcFormat);
            }
            return false;
    }
    
    outTexture.pixelData = std::move(rgba);
    outTexture.valid = true;
    
    return true;
}

// =========================================================================
// Solid Color Detection
// =========================================================================

bool IsSolidColorRGBA(const std::vector<uint8_t>& pixelData,
                      uint32_t width, uint32_t height,
                      uint8_t& outR, uint8_t& outG, uint8_t& outB, uint8_t& outA) {
    if (pixelData.empty() || width == 0 || height == 0) {
        return false;
    }
    
    size_t pixelCount = width * height;
    if (pixelData.size() < pixelCount * 4) {
        return false;
    }
    
    // Get first pixel color
    outR = pixelData[0];
    outG = pixelData[1];
    outB = pixelData[2];
    outA = pixelData[3];
    
    // For large textures, use sampling instead of checking every pixel
    size_t step = 1;
    if (pixelCount > 4096) {
        step = pixelCount / 1024; // Check ~1024 pixels
    }
    
    // Check sampled pixels
    for (size_t i = 0; i < pixelCount; i += step) {
        size_t idx = i * 4;
        if (pixelData[idx + 0] != outR ||
            pixelData[idx + 1] != outG ||
            pixelData[idx + 2] != outB ||
            pixelData[idx + 3] != outA) {
            return false;
        }
    }
    
    // Also check corners and edges for accuracy
    size_t checkPositions[] = {
        0,                          // Top-left
        (width - 1) * 4,            // Top-right
        (height - 1) * width * 4,   // Bottom-left
        ((height - 1) * width + width - 1) * 4,  // Bottom-right
        (height / 2) * width * 4,   // Middle-left
        (height / 2 * width + width - 1) * 4,    // Middle-right
        (width / 2) * 4,            // Top-middle
        ((height - 1) * width + width / 2) * 4,  // Bottom-middle
        (height / 2 * width + width / 2) * 4     // Center
    };
    
    for (size_t pos : checkPositions) {
        if (pos + 3 < pixelData.size()) {
            if (pixelData[pos + 0] != outR ||
                pixelData[pos + 1] != outG ||
                pixelData[pos + 2] != outB ||
                pixelData[pos + 3] != outA) {
                return false;
            }
        }
    }
    
    return true;
}

SolidColorResult CheckSolidColor(IFileSystem* fileSystem,
                                  const std::string& texturePath,
                                  bool debugOutput) {
    SolidColorResult result;
    
    if (!fileSystem || texturePath.empty()) {
        result.error = true;
        result.errorMessage = "Invalid parameters";
        return result;
    }
    
    // Read VTF file
    std::vector<uint8_t> fileData;
    if (!ReadVTFFile(fileSystem, texturePath, fileData, debugOutput)) {
        result.error = true;
        result.errorMessage = "Failed to read VTF file";
        return result;
    }
    
    // Parse header
    VTFFileHeader header;
    if (!ParseVTFHeader(fileData, header, debugOutput)) {
        result.error = true;
        result.errorMessage = "Failed to parse VTF header";
        return result;
    }
    
    // Skip textures that are too large (for performance)
    if (header.width > MAX_SOLID_COLOR_CHECK_SIZE || header.height > MAX_SOLID_COLOR_CHECK_SIZE) {
        result.error = false;
        result.isSolidColor = false;
        return result;
    }
    
    // Extract pixel data
    TextureInfo texture;
    if (!ExtractPixelData(fileData, header, texture, debugOutput)) {
        result.error = true;
        result.errorMessage = "Failed to extract pixel data";
        return result;
    }
    
    // Check if solid color
    result.isSolidColor = IsSolidColorRGBA(texture.pixelData, texture.width, texture.height,
                                           result.r, result.g, result.b, result.a);
    result.error = false;
    
    if (debugOutput && result.isSolidColor) {
        Msg("[VTFParser] Solid color detected: %s = RGBA(%d, %d, %d, %d)\n",
            texturePath.c_str(), result.r, result.g, result.b, result.a);
    }
    
    return result;
}

} // namespace VTFParser

#endif // _WIN64
