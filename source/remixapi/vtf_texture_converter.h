#pragma once

#ifdef _WIN64

#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <mutex>
#include <cstdint>

#include <remix/remix.h>
#include <remix/remix_c.h>

// Forward declarations
class IMaterial;
class IFileSystem;

namespace GarrysMod {
namespace Lua {
class ILuaBase;
}
}

namespace VTFConverter {

// VTF file format structures
#pragma pack(push, 1)
struct VTFFileHeader {
    char signature[4];          // "VTF\0"
    uint32_t version[2];        // Major and minor version
    uint32_t headerSize;        // Size of the header
    uint16_t width;             // Width of the largest mipmap
    uint16_t height;            // Height of the largest mipmap
    uint32_t flags;             // VTF flags
    uint16_t frames;            // Number of frames (animated textures)
    uint16_t firstFrame;        // First frame in animation
    uint8_t padding0[4];        // reflectivity padding (before vector)
    float reflectivity[3];      // Reflectivity vector
    uint8_t padding1[4];        // reflectivity padding (after vector)
    float bumpScale;            // Bump map scale
    uint32_t imageFormat;       // Image format (DXT1, DXT5, RGBA8888, etc.)
    uint8_t mipmapCount;        // Number of mipmaps
    uint32_t lowResImageFormat; // Low res image format
    uint8_t lowResImageWidth;   // Low res image width
    uint8_t lowResImageHeight;  // Low res image height
    // VTF 7.2+
    uint16_t depth;             // Depth (for volume textures)
    // VTF 7.3+
    uint8_t padding2[3];        // Alignment padding before numResources
    uint32_t numResources;      // Number of resources (7.3+)
};
#pragma pack(pop)

// VTF image formats (subset of common formats)
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

// Converted texture data
struct ConvertedTexture {
    std::vector<uint8_t> pixelData;
    uint32_t width;
    uint32_t height;
    uint32_t mipLevels;
    remixapi_Format format;
    uint64_t hash;          // Generated hash for this texture
    std::string sourcePath; // Original VTF path
    bool isNormalMap;       // Whether this is a normal map
};

// Material PBR properties extracted from Source Engine
struct MaterialPBRProperties {
    std::string materialName;
    std::string baseTexturePath;
    std::string bumpMapPath;
    std::string envMapMaskPath;
    float phongExponent;
    float phongBoost;
    float roughness;        // Calculated from phong
    float metallic;         // Estimated from envmap
    bool hasPhong;
    bool hasBumpMap;
    bool hasEnvMapMask;
    bool isSelfIllum;
    bool isTranslucent;
    uint64_t baseTextureHash;  // Hash from rendered texture
};

// Main converter class
class VTFTextureConverter {
public:
    static VTFTextureConverter& Instance();
    
    // Initialize with Remix interface
    bool Initialize(remix::Interface* remixInterface);
    void Shutdown();
    
    // Convert and upload a VTF texture to Remix
    // Returns the texture hash that can be used in materials
    uint64_t ConvertAndUploadTexture(const std::string& vtfPath, bool isNormalMap = false);
    
    // Extract PBR properties from a Source Engine material
    bool ExtractMaterialPBR(const std::string& materialName, MaterialPBRProperties& outProps);
    
    // Create a Remix PBR material from extracted properties
    // This binds to an existing rendered texture hash
    bool CreatePBRMaterial(const MaterialPBRProperties& props, uint64_t textureHash);
    
    // Process all materials in the texture tracker cache
    int ProcessAllTrackedMaterials();
    
    // Check if a material has already been processed
    bool IsMaterialProcessed(const std::string& materialName) const;
    
    // Get conversion statistics
    struct Stats {
        int materialsProcessed;
        int texturesUploaded;
        int materialsWithNormals;
        int materialsWithRoughness;
        int failedConversions;
    };
    Stats GetStats() const;
    
    // Clear processed materials cache (for map changes)
    void ClearCache();
    
    // Enable/disable auto-processing of new materials
    void SetAutoProcessing(bool enabled) { m_autoProcessing = enabled; }
    bool IsAutoProcessingEnabled() const { return m_autoProcessing; }
    
    // Debug output control
    void SetDebugOutput(bool enabled) { m_debugOutput = enabled; }
    
    // Set output directory for generated textures
    void SetOutputDirectory(const std::string& path);
    std::string GetOutputDirectory() const { return m_outputDirectory; }
    
    // Check if the converter is initialized
    bool IsInitialized() const { return m_initialized; }
    
private:
    VTFTextureConverter();
    ~VTFTextureConverter();
    
    // Prevent copying
    VTFTextureConverter(const VTFTextureConverter&) = delete;
    VTFTextureConverter& operator=(const VTFTextureConverter&) = delete;
    
    // Read a VTF file from the Source Engine filesystem
    bool ReadVTFFile(const std::string& path, std::vector<uint8_t>& outData);
    
    // Parse VTF header
    bool ParseVTFHeader(const std::vector<uint8_t>& fileData, VTFFileHeader& outHeader);
    
    // Get pixel data from VTF (handles format conversion)
    bool ExtractVTFPixelData(const std::vector<uint8_t>& fileData, const VTFFileHeader& header, 
                             ConvertedTexture& outTexture, bool isNormalMap);
    
    // Convert DXT compressed data to RGBA
    bool DecompressDXT1(const uint8_t* compressedData, uint32_t width, uint32_t height,
                        std::vector<uint8_t>& outRGBA);
    bool DecompressDXT5(const uint8_t* compressedData, uint32_t width, uint32_t height,
                        std::vector<uint8_t>& outRGBA);
    
    // Generate a unique hash for a texture
    uint64_t GenerateTextureHash(const std::string& path, uint32_t width, uint32_t height);
    
    // Upload texture to Remix
    bool UploadTextureToRemix(const ConvertedTexture& texture, remixapi_TextureHandle* outHandle);
    
    // Write texture to DDS file
    bool WriteTextureToDDS(const ConvertedTexture& texture, const std::string& outputPath);
    bool WriteDDSHeader(std::ofstream& file, uint32_t width, uint32_t height, bool hasAlpha, uint32_t mipCount);
    
    // Generate roughness texture from envmap mask or phong constant
    bool GenerateRoughnessTexture(const MaterialPBRProperties& props, ConvertedTexture& outTexture);
    
    // Generate metallic texture from material properties
    bool GenerateMetallicTexture(const MaterialPBRProperties& props, ConvertedTexture& outTexture);
    
    // Ensure output directory exists
    bool EnsureOutputDirectory();
    
    // Generate output filename for a texture type
    std::string GenerateOutputPath(uint64_t hash, const std::string& suffix);
    
    // Convert normal map from DirectX format to octahedral format for RTX Remix
    void ConvertNormalMapToOctahedral(ConvertedTexture& texture);
    
    // Write the mod.usda file with all processed materials
    bool WriteModUSDA();
    
    // Calculate roughness from phong exponent
    float PhongToRoughness(float phongExponent);
    
    // Calculate metallic hint from material properties
    float EstimateMetallic(const MaterialPBRProperties& props);
    
    // Get filesystem interface
    IFileSystem* GetFileSystem();
    
    remix::Interface* m_remixInterface;
    IFileSystem* m_fileSystem;
    bool m_initialized;
    bool m_autoProcessing;
    bool m_debugOutput;
    std::string m_outputDirectory;       // Absolute output directory for writing DDS files
    
    // Cache of processed materials
    std::unordered_set<std::string> m_processedMaterials;
    
    // Cache of uploaded textures (path -> hash)
    std::unordered_map<std::string, uint64_t> m_uploadedTextures;
    
    // Cache of texture handles for cleanup
    std::unordered_map<uint64_t, remixapi_TextureHandle> m_textureHandles;
    
    // Cache of material handles for cleanup
    std::unordered_map<uint64_t, remixapi_MaterialHandle> m_materialHandles;
    
    // Cache of written DDS file paths (hash -> path)
    std::unordered_map<uint64_t, std::string> m_writtenTexturePaths;
    
    // Track material data for USDA generation
    struct ProcessedMaterialInfo {
        uint64_t textureHash;
        std::string normalPath;
        std::string roughnessPath;
        std::string metallicPath;
        float roughnessConstant;
        float metallicConstant;
    };
    std::unordered_map<uint64_t, ProcessedMaterialInfo> m_processedMaterialInfo;
    
    // Statistics
    mutable std::mutex m_mutex;
    Stats m_stats;
};

// Lua bindings initialization
void InitializeVTFConverterLuaBindings(GarrysMod::Lua::ILuaBase* LUA);

} // namespace VTFConverter

#endif // _WIN64
