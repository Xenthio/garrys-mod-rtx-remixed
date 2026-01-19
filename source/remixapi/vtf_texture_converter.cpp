#ifdef _WIN64

#include "vtf_texture_converter.h"
#include "../d3d9_texture_tracker.h"

#include <tier0/dbg.h>
#include <Windows.h>
#include <materialsystem/imaterialsystem.h>
#include <materialsystem/imaterial.h>
#include <materialsystem/imaterialvar.h>
#include <filesystem.h>

#include <algorithm>
#include <cstring>
#include <cmath>

// External globals
extern IMaterialSystem* materials;
extern remix::Interface* g_remix;

namespace VTFConverter {

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
// VTFTextureConverter Implementation
//=============================================================================

VTFTextureConverter& VTFTextureConverter::Instance() {
    static VTFTextureConverter instance;
    return instance;
}

VTFTextureConverter::VTFTextureConverter()
    : m_remixInterface(nullptr)
    , m_fileSystem(nullptr)
    , m_initialized(false)
    , m_autoProcessing(true)
    , m_debugOutput(false) {
    m_stats = {};
}

VTFTextureConverter::~VTFTextureConverter() {
    Shutdown();
}

bool VTFTextureConverter::Initialize(remix::Interface* remixInterface) {
    if (m_initialized) {
        return false;
    }
    
    if (!remixInterface) {
        Warning("[VTFConverter] Invalid Remix interface\n");
        return false;
    }
    
    m_remixInterface = remixInterface;
    m_fileSystem = GetFileSystemInterface();
    
    if (!m_fileSystem) {
        Warning("[VTFConverter] Could not get filesystem interface\n");
        return false;
    }
    
    m_initialized = true;
    Msg("[VTFConverter] Initialized successfully\n");
    return true;
}

void VTFTextureConverter::Shutdown() {
    if (!m_initialized) return;
    
    // Destroy all uploaded textures
    for (auto& pair : m_textureHandles) {
        if (m_remixInterface && pair.second) {
            m_remixInterface->DestroyTexture(pair.second);
        }
    }
    m_textureHandles.clear();
    
    // Destroy all created materials
    for (auto& pair : m_materialHandles) {
        if (m_remixInterface && pair.second) {
            m_remixInterface->DestroyMaterial(pair.second);
        }
    }
    m_materialHandles.clear();
    
    m_uploadedTextures.clear();
    m_processedMaterials.clear();
    
    m_remixInterface = nullptr;
    m_initialized = false;
    
    Msg("[VTFConverter] Shutdown complete\n");
}

IFileSystem* VTFTextureConverter::GetFileSystem() {
    return m_fileSystem;
}

bool VTFTextureConverter::ReadVTFFile(const std::string& path, std::vector<uint8_t>& outData) {
    if (!m_fileSystem) {
        Warning("[VTFConverter] Filesystem not available\n");
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
                Msg("[VTFConverter] Could not open VTF: %s\n", fullPath.c_str());
            }
            return false;
        }
    }
    
    // Get file size
    int fileSize = m_fileSystem->Size(file);
    if (fileSize <= 0 || fileSize > 256 * 1024 * 1024) { // 256MB max
        m_fileSystem->Close(file);
        Warning("[VTFConverter] Invalid file size for %s: %d\n", fullPath.c_str(), fileSize);
        return false;
    }
    
    // Read file data
    outData.resize(fileSize);
    int bytesRead = m_fileSystem->Read(outData.data(), fileSize, file);
    m_fileSystem->Close(file);
    
    if (bytesRead != fileSize) {
        Warning("[VTFConverter] Read error for %s: expected %d, got %d\n", 
                fullPath.c_str(), fileSize, bytesRead);
        return false;
    }
    
    return true;
}

bool VTFTextureConverter::ParseVTFHeader(const std::vector<uint8_t>& fileData, VTFFileHeader& outHeader) {
    if (fileData.size() < sizeof(VTFFileHeader)) {
        Warning("[VTFConverter] File too small for VTF header\n");
        return false;
    }
    
    memcpy(&outHeader, fileData.data(), sizeof(VTFFileHeader));
    
    // Verify signature
    if (memcmp(outHeader.signature, "VTF\0", 4) != 0) {
        Warning("[VTFConverter] Invalid VTF signature\n");
        return false;
    }
    
    // Verify version (support 7.0 - 7.5)
    if (outHeader.version[0] != 7 || outHeader.version[1] > 5) {
        Warning("[VTFConverter] Unsupported VTF version %d.%d\n", 
                outHeader.version[0], outHeader.version[1]);
        return false;
    }
    
    if (m_debugOutput) {
        Msg("[VTFConverter] VTF: %dx%d, format %d, %d mips\n",
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

bool VTFTextureConverter::DecompressDXT1(const uint8_t* compressedData, uint32_t width, uint32_t height,
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

bool VTFTextureConverter::DecompressDXT5(const uint8_t* compressedData, uint32_t width, uint32_t height,
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
            
            // Next 8 bytes: color block (DXT1)
            DecompressDXT1Block(blockPtr, blockPixels, 4 * 4);
            blockPtr += 8;
            
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
        w = std::max(1u, w / 2);
        h = std::max(1u, h / 2);
    }
    
    return totalSize;
}

bool VTFTextureConverter::ExtractVTFPixelData(const std::vector<uint8_t>& fileData, 
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
    // VTF stores mipmaps from smallest to largest, then the high-res image
    size_t dataOffset = header.headerSize;
    
    // Skip low-res thumbnail if present
    if (header.lowResImageFormat != IMAGE_FORMAT_NONE) {
        dataOffset += GetImageDataSize(header.lowResImageWidth, header.lowResImageHeight, 
                                       (VTFImageFormat)header.lowResImageFormat);
    }
    
    // Skip all mipmaps (stored smallest to largest) to get to largest mip
    uint8_t mipmapCount = header.mipmapCount > 0 ? header.mipmapCount : 1;
    
    // Calculate size of all smaller mipmaps
    size_t smallerMipsSize = 0;
    uint32_t w = header.width;
    uint32_t h = header.height;
    for (int i = 0; i < mipmapCount - 1; i++) {
        w = std::max(1u, w / 2);
        h = std::max(1u, h / 2);
    }
    // Now accumulate from smallest to second-largest
    for (int i = mipmapCount - 1; i > 0; i--) {
        smallerMipsSize += GetImageDataSize(w, h, srcFormat);
        w = std::min(header.width, w * 2);
        h = std::min(header.height, h * 2);
    }
    
    dataOffset += smallerMipsSize;
    
    // Calculate size of largest mip
    size_t largestMipSize = GetImageDataSize(header.width, header.height, srcFormat);
    
    if (dataOffset + largestMipSize > fileData.size()) {
        Warning("[VTFConverter] VTF data truncated: need %zu bytes, have %zu\n",
                dataOffset + largestMipSize, fileData.size());
        return false;
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
                Msg("[VTFConverter] Unsupported VTF format: %d\n", srcFormat);
            }
            return false;
    }
    
    outTexture.pixelData = std::move(rgba);
    return true;
}

uint64_t VTFTextureConverter::GenerateTextureHash(const std::string& path, uint32_t width, uint32_t height) {
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

bool VTFTextureConverter::UploadTextureToRemix(const ConvertedTexture& texture, 
                                                remixapi_TextureHandle* outHandle) {
    if (!m_remixInterface) {
        Warning("[VTFConverter] Remix interface not available\n");
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
    
    auto result = m_remixInterface->CreateTexture(&texInfo, outHandle);
    if (result != REMIXAPI_ERROR_CODE_SUCCESS) {
        Warning("[VTFConverter] Failed to create texture: error %d\n", result);
        return false;
    }
    
    if (m_debugOutput) {
        Msg("[VTFConverter] Uploaded texture: %dx%d, hash 0x%llX\n", 
            texture.width, texture.height, texture.hash);
    }
    
    return true;
}

uint64_t VTFTextureConverter::ConvertAndUploadTexture(const std::string& vtfPath, bool isNormalMap) {
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

float VTFTextureConverter::PhongToRoughness(float phongExponent) {
    if (phongExponent <= 0) return 0.5f;
    
    // Clamp to reasonable range
    phongExponent = std::clamp(phongExponent, 1.0f, 256.0f);
    
    // Logarithmic conversion
    float maxExp = 150.0f;
    float roughness = 1.0f - (std::log(phongExponent) / std::log(maxExp));
    
    return std::clamp(roughness, 0.05f, 0.95f);
}

float VTFTextureConverter::EstimateMetallic(const MaterialPBRProperties& props) {
    float metallic = 0.0f;
    
    // High phong boost suggests metal-like reflections
    if (props.phongBoost > 2.0f) {
        metallic = std::clamp((props.phongBoost - 2.0f) / 8.0f, 0.0f, 0.5f);
    }
    
    // Having an envmap mask suggests reflective surface
    if (props.hasEnvMapMask) {
        metallic = std::max(metallic, 0.2f);
    }
    
    return metallic;
}

bool VTFTextureConverter::ExtractMaterialPBR(const std::string& materialName, 
                                              MaterialPBRProperties& outProps) {
    if (!materials) {
        Warning("[VTFConverter] Material system not available\n");
        return false;
    }
    
    IMaterial* pMaterial = materials->FindMaterial(materialName.c_str(), TEXTURE_GROUP_OTHER, false);
    if (!pMaterial || pMaterial->IsErrorMaterial()) {
        if (m_debugOutput) {
            Msg("[VTFConverter] Material not found: %s\n", materialName.c_str());
        }
        return false;
    }
    
    outProps.materialName = materialName;
    outProps.hasPhong = false;
    outProps.hasBumpMap = false;
    outProps.hasEnvMapMask = false;
    outProps.isSelfIllum = false;
    outProps.isTranslucent = false;
    outProps.phongExponent = 0;
    outProps.phongBoost = 1.0f;
    outProps.roughness = 0.5f;
    outProps.metallic = 0.0f;
    
    // Get $basetexture
    bool found = false;
    IMaterialVar* pVar = pMaterial->FindVar("$basetexture", &found, false);
    if (found && pVar) {
        ITexture* pTex = pVar->GetTextureValue();
        if (pTex) {
            outProps.baseTexturePath = pTex->GetName();
        }
    }
    
    // Get $bumpmap
    pVar = pMaterial->FindVar("$bumpmap", &found, false);
    if (found && pVar) {
        ITexture* pTex = pVar->GetTextureValue();
        if (pTex) {
            outProps.bumpMapPath = pTex->GetName();
            outProps.hasBumpMap = true;
        }
    }
    
    // Get $envmapmask
    pVar = pMaterial->FindVar("$envmapmask", &found, false);
    if (found && pVar) {
        ITexture* pTex = pVar->GetTextureValue();
        if (pTex) {
            outProps.envMapMaskPath = pTex->GetName();
            outProps.hasEnvMapMask = true;
        }
    }
    
    // Get $phongexponent
    pVar = pMaterial->FindVar("$phongexponent", &found, false);
    if (found && pVar) {
        outProps.phongExponent = pVar->GetFloatValue();
        outProps.hasPhong = true;
    }
    
    // Get $phongboost
    pVar = pMaterial->FindVar("$phongboost", &found, false);
    if (found && pVar) {
        outProps.phongBoost = pVar->GetFloatValue();
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
    
    // Calculate PBR values
    outProps.roughness = PhongToRoughness(outProps.phongExponent);
    outProps.metallic = EstimateMetallic(outProps);
    
    return true;
}

bool VTFTextureConverter::CreatePBRMaterial(const MaterialPBRProperties& props, uint64_t textureHash) {
    if (!m_remixInterface || textureHash == 0) {
        return false;
    }
    
    // Check if we've already created a material for this hash
    if (m_materialHandles.find(textureHash) != m_materialHandles.end()) {
        return true; // Already done
    }
    
    // Build material info
    remixapi_MaterialInfo matInfo = {};
    matInfo.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO;
    matInfo.hash = textureHash;
    
    // Build opaque extension for PBR properties
    remixapi_MaterialInfoOpaqueEXT opaqueExt = {};
    opaqueExt.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_OPAQUE_EXT;
    opaqueExt.roughnessConstant = props.roughness;
    opaqueExt.metallicConstant = props.metallic;
    opaqueExt.albedoConstant = {1.0f, 1.0f, 1.0f};
    opaqueExt.opacityConstant = 1.0f;
    
    // Upload and set normal map if available
    if (props.hasBumpMap && !props.bumpMapPath.empty()) {
        uint64_t normalHash = ConvertAndUploadTexture(props.bumpMapPath, true);
        if (normalHash != 0) {
            // Note: Remix expects a path, not a hash for normalTexture
            // For runtime textures, we need to use a special path format
            // This is a limitation - we may need to write the texture to disk
            // For now, we set the roughness/metallic constants which work
            m_stats.materialsWithNormals++;
        }
    }
    
    if (props.hasPhong) {
        m_stats.materialsWithRoughness++;
    }
    
    // Chain the extension
    matInfo.pNext = &opaqueExt;
    
    // Create the material
    remixapi_MaterialHandle handle = nullptr;
    auto result = m_remixInterface->CreateMaterial(&matInfo, &handle);
    
    if (result != REMIXAPI_ERROR_CODE_SUCCESS) {
        if (m_debugOutput) {
            Warning("[VTFConverter] Failed to create material for hash 0x%llX: error %d\n", 
                    textureHash, result);
        }
        return false;
    }
    
    m_materialHandles[textureHash] = handle;
    m_stats.materialsProcessed++;
    
    if (m_debugOutput) {
        Msg("[VTFConverter] Created PBR material for '%s': roughness=%.2f, metallic=%.2f\n",
            props.materialName.c_str(), props.roughness, props.metallic);
    }
    
    return true;
}

int VTFTextureConverter::ProcessAllTrackedMaterials() {
    std::lock_guard<std::mutex> lock(m_mutex);
    
    if (!m_initialized) {
        Warning("[VTFConverter] Not initialized\n");
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
        if (!props.hasBumpMap && !props.hasPhong && !props.hasEnvMapMask) {
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
        
        // Create PBR material
        if (CreatePBRMaterial(props, textureHash)) {
            processedCount++;
        }
        
        m_processedMaterials.insert(matName);
    }
    
    if (processedCount > 0) {
        Msg("[VTFConverter] Processed %d materials with PBR properties\n", processedCount);
    }
    
    return processedCount;
}

bool VTFTextureConverter::IsMaterialProcessed(const std::string& materialName) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_processedMaterials.find(materialName) != m_processedMaterials.end();
}

VTFTextureConverter::Stats VTFTextureConverter::GetStats() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_stats;
}

void VTFTextureConverter::ClearCache() {
    std::lock_guard<std::mutex> lock(m_mutex);
    
    // Don't destroy handles here - Remix manages their lifecycle
    m_processedMaterials.clear();
    m_uploadedTextures.clear();
    m_stats = {};
    
    Msg("[VTFConverter] Cache cleared\n");
}

//=============================================================================
// Lua Bindings
//=============================================================================

using namespace GarrysMod::Lua;

LUA_FUNCTION(VTFConverter_Initialize) {
    if (!g_remix) {
        LUA->PushBool(false);
        return 1;
    }
    
    bool result = VTFTextureConverter::Instance().Initialize(g_remix);
    LUA->PushBool(result);
    return 1;
}

LUA_FUNCTION(VTFConverter_ProcessAllMaterials) {
    int count = VTFTextureConverter::Instance().ProcessAllTrackedMaterials();
    LUA->PushNumber(count);
    return 1;
}

LUA_FUNCTION(VTFConverter_SetAutoProcessing) {
    if (!LUA->IsType(1, Type::Bool)) {
        LUA->ThrowError("Expected boolean for auto processing");
        return 0;
    }
    
    VTFTextureConverter::Instance().SetAutoProcessing(LUA->GetBool(1));
    return 0;
}

LUA_FUNCTION(VTFConverter_SetDebugOutput) {
    if (!LUA->IsType(1, Type::Bool)) {
        LUA->ThrowError("Expected boolean for debug output");
        return 0;
    }
    
    VTFTextureConverter::Instance().SetDebugOutput(LUA->GetBool(1));
    return 0;
}

LUA_FUNCTION(VTFConverter_GetStats) {
    auto stats = VTFTextureConverter::Instance().GetStats();
    
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

LUA_FUNCTION(VTFConverter_ClearCache) {
    VTFTextureConverter::Instance().ClearCache();
    return 0;
}

LUA_FUNCTION(VTFConverter_ConvertTexture) {
    if (!LUA->IsType(1, Type::String)) {
        LUA->ThrowError("Expected string for texture path");
        return 0;
    }
    
    const char* path = LUA->GetString(1);
    bool isNormalMap = LUA->IsType(2, Type::Bool) ? LUA->GetBool(2) : false;
    
    uint64_t hash = VTFTextureConverter::Instance().ConvertAndUploadTexture(path, isNormalMap);
    
    // Return hash as string to preserve precision
    char hashStr[32];
    sprintf_s(hashStr, "0x%llX", hash);
    LUA->PushString(hashStr);
    
    return 1;
}

LUA_FUNCTION(VTFConverter_InspectMaterial) {
    if (!LUA->IsType(1, Type::String)) {
        LUA->ThrowError("Expected string for material name");
        return 0;
    }
    
    const char* matName = LUA->GetString(1);
    
    MaterialPBRProperties props;
    if (!VTFTextureConverter::Instance().ExtractMaterialPBR(matName, props)) {
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
    
    LUA->PushBool(props.isSelfIllum);
    LUA->SetField(-2, "isSelfIllum");
    
    LUA->PushBool(props.isTranslucent);
    LUA->SetField(-2, "isTranslucent");
    
    return 1;
}

void InitializeVTFConverterLuaBindings(GarrysMod::Lua::ILuaBase* LUA) {
    // Create VTFConverter table
    LUA->PushSpecial(SPECIAL_GLOB);
    LUA->CreateTable();
    
    LUA->PushCFunction(VTFConverter_Initialize);
    LUA->SetField(-2, "Initialize");
    
    LUA->PushCFunction(VTFConverter_ProcessAllMaterials);
    LUA->SetField(-2, "ProcessAllMaterials");
    
    LUA->PushCFunction(VTFConverter_SetAutoProcessing);
    LUA->SetField(-2, "SetAutoProcessing");
    
    LUA->PushCFunction(VTFConverter_SetDebugOutput);
    LUA->SetField(-2, "SetDebugOutput");
    
    LUA->PushCFunction(VTFConverter_GetStats);
    LUA->SetField(-2, "GetStats");
    
    LUA->PushCFunction(VTFConverter_ClearCache);
    LUA->SetField(-2, "ClearCache");
    
    LUA->PushCFunction(VTFConverter_ConvertTexture);
    LUA->SetField(-2, "ConvertTexture");
    
    LUA->PushCFunction(VTFConverter_InspectMaterial);
    LUA->SetField(-2, "InspectMaterial");
    
    LUA->SetField(-2, "VTFConverter");
    LUA->Pop();
    
    Msg("[VTFConverter] Lua bindings initialized\n");
}

} // namespace VTFConverter

#endif // _WIN64
