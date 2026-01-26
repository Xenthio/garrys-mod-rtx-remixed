#pragma once

#ifdef _WIN64

#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <mutex>
#include <cstdint>
#include <functional>
#include <thread>
#include <atomic>
#include <condition_variable>
#include <queue>

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

// LegacyTextureProcessor namespace - handles conversion of Source Engine textures to RTX Remix PBR format
// This system can be extended with custom processors for different texture/material types
namespace LegacyTextureProcessor {

// Forward declaration for modular format handlers
struct ProcessingContext;

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
    bool isSSBump;              // $ssbump=1 - SSBump format requires conversion to normal map
    std::string envMapMaskPath;
    std::string phongExponentTexturePath;  // $phongexponenttexture - per-pixel phong exponent
    float phongExponent;
    float phongBoost;
    float roughness;        // Calculated from phong
    float metallic;         // Estimated from envmap
    bool hasPhong;
    bool hasBumpMap;
    bool hasEnvMapMask;
    bool hasPhongExponentTexture;  // $phongexponenttexture available
    bool isSelfIllum;
    bool isTranslucent;
    uint64_t baseTextureHash;  // Hash from rendered texture
    
    // Extended Source Engine properties
    bool normalMapAlphaEnvMapMask;  // $normalmapalphaenvmapmask - use normal map alpha as envmap mask
    float phongFresnelRanges[3];    // $phongfresnelranges - fresnel values
    bool hasPhongFresnelRanges;
    float envMapTint[3];            // $envmaptint - tints the environment map
    bool hasEnvMapTint;
    bool hasEnvMap;                 // $envmap - has environment mapping
    float baseMapAlphaPhongMask;    // $basemapalphaphongmask - use base texture alpha as phong mask
    bool hasBaseMapAlphaPhongMask;
    bool hasBaseAlphaEnvMapMask;    // $basealphaenvmapmask - use base texture alpha as envmap mask (LightmappedGeneric)
    
    // Glass material detection
    bool isGlass;                   // Material detected as glass (translucent + glass surfaceprop or Refract shader)
    bool isRefractShader;           // Material uses Refract shader (important: baseTexture may be set to normalmap by fixer)
    std::string shaderName;         // The shader name (VertexLitGeneric, LightmappedGeneric, Refract, etc.)
    std::string surfaceProp;        // $surfaceprop value
    std::string refractTintTexturePath;  // $refracttinttexture - color texture for Refract shader
    
    // Metallic detection from base texture darkness
    float baseTextureBrightness;    // Average brightness of base texture (0.0 = black, 1.0 = white)
    bool hasBaseTextureBrightness;  // Whether we successfully analyzed the base texture
    
    // Self-illumination / Emissive properties
    std::string selfIllumMaskPath;  // $selfillummask - separate mask texture for emissive areas
    bool hasSelfIllumMask;
    float selfIllumTint[3];         // $selfillumtint - tint color for self-illumination
    bool hasSelfIllumTint;
    
    // Rim lighting properties (affects specular appearance)
    bool hasRimLight;               // $rimlight - enables rim lighting
    float rimLightExponent;         // $rimlightexponent - sharpness of rim light
    float rimLightBoost;            // $rimlightboost - intensity of rim light
    bool hasRimLightExponent;
    bool hasRimLightBoost;
    
    // Additional phong properties
    bool phongAlbedoTint;           // $phongalbedotint - uses base color to tint phong highlight
    float phongAlbedoBoost;         // $phongalbedoboost - boost for albedo tint
    bool hasPhongAlbedoBoost;
    float phongTint[3];             // $phongtint - direct tint for phong highlight
    bool hasPhongTint;
    
    // Parallax/heightmap properties (for displacement)
    std::string parallaxMapPath;    // $parallaxmap - heightmap for parallax effect
    bool hasParallaxMap;
    float parallaxMapScale;         // $parallaxmapscale - depth scale for parallax
    bool hasParallaxMapScale;
    
    // Additional envmap properties
    float envMapContrast;           // $envmapcontrast - contrast for environment reflections
    bool hasEnvMapContrast;
    float envMapSaturation;         // $envmapsaturation - saturation for environment reflections
    bool hasEnvMapSaturation;
    
    // Additional texture masks
    std::string envMapMask2Path;    // $envmapmask2 - secondary envmap mask (rare)
    bool hasEnvMapMask2;
    
    // Auto-discovered companion textures (not explicitly referenced in VMT)
    std::string discoveredNormalPath;   // basetexture_normal (auto-discovered)
    bool hasDiscoveredNormal;
    std::string discoveredHeightPath;   // basetexture_height (auto-discovered)
    bool hasDiscoveredHeight;
    std::string discoveredMaskPath;     // basetexture_mask or basetexture_spec (auto-discovered)
    bool hasDiscoveredMask;
    std::string discoveredAOPath;       // basetexture_ao (auto-discovered)
    bool hasDiscoveredAO;
    
    // =========================================================================
    // ExoPBR community PBR format support (screenspace_general_8tex shader)
    // =========================================================================
    bool isExoPBR;                      // Detected ExoPBR format
    std::string armTexturePath;         // $texture1 - ARM map (AO/Roughness/Metallic)
    bool hasARMTexture;
    std::string exoNormalPath;          // $texture2 - Normal map (DirectX Y- format)
    bool hasExoNormal;
    std::string emissionTexturePath;    // $texture3 - Emission texture
    bool hasEmissionTexture;
    float emissionScale;                // $emissionscale - emission intensity
    bool hasEmissionScale;
    float emissionTint[3];              // $emissiontint - emission color tint
    bool hasEmissionTint;
    
    // =========================================================================
    // GPBR (Strata Source) community PBR format support ("PBR" shader)
    // =========================================================================
    bool isGPBR;                        // Detected GPBR format (shader name = "PBR")
    std::string mraoTexturePath;        // $mraotexture - MRAO map (Metallic/Roughness/AO)
    bool hasMRAOTexture;
    float mraoScale;                    // $mraoscale - MRAO intensity multiplier
    bool hasMRAOScale;
    std::string gpbrEmissionPath;       // $emissiontexture - Emission/glow map
    bool hasGPBREmission;
    float gpbrEmissionScale;            // $emissionscale - Emission intensity
    bool hasGPBREmissionScale;
    bool gpbrParallax;                  // $parallax - Enable parallax mapping
    float gpbrParallaxDepth;            // $parallaxdepth - Displacement depth
    float gpbrAlpha;                    // $alpha - Transparency value
    bool hasGPBRAlpha;
    
    // =========================================================================
    // BlueFlyTrap PseudoPBR format support
    // A technique encoding PBR properties into Source Engine's phong workflow
    // Detection: VertexlitGeneric + $phongexponenttexture + specific patterns
    // =========================================================================
    bool isBFTPseudoPBR;                // Detected BlueFlyTrap PseudoPBR format
    bool isBFTMetallicLayer;            // This is the metallic layer ($translucent + $phongalbedotint)
    std::string bftExponentTexturePath; // $phongexponenttexture - encodes roughness (inverted)
    bool hasBFTExponentTexture;
    
    // BFT $color2 - used to darken albedo for layer stacking (need to invert for PBR)
    float bftColor2[3];                 // RGB values from $color2
    bool hasBFTColor2;
    
    // BFT $blendTintByBaseAlpha - when combined with dark $color2, tints albedo by alpha
    // This pattern means the alpha channel IS the metallic mask!
    // We need to: 1) extract metallic from alpha, 2) disable tinting at runtime via Lua fix
    bool hasBFTBlendTintByBaseAlpha;    // $blendtintbybasealpha "1" detected
    bool isBFTDiffuseLayer;             // This is a BFT diffuse layer that uses blend tinting
    
    // =========================================================================
    // MWB PBR Gen format support
    // Separate from BFT - uses pow(gloss,4.0) encoding and stores metalness in green channel
    // Detection: _rgb suffix, pbr\output\ path, MwEnvMapTint/Arc9EnvMapTint proxies
    // =========================================================================
    bool isMWBPBR;                       // Detected MWB PBR Gen format
};

// Main converter class - core VTF to DDS/PBR conversion
class TextureProcessor {
public:
    static TextureProcessor& Instance();
    
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
    
    // Process materials in batches to avoid stuttering (returns number processed)
    // maxBatch: maximum materials to process per call (0 = process all)
    // Returns: number of materials processed in this batch
    int ProcessTrackedMaterialsBatch(int maxBatch = 5);
    
    // Process a single material (for auto-processing when new textures appear)
    // Returns true if the material was processed successfully
    bool ProcessSingleMaterial(const std::string& materialName);
    
    // Called when a new material is detected by the texture tracker
    // This is for automatic processing when auto-processing is enabled
    void OnNewMaterialDetected(const std::string& materialName, uint64_t textureHash);
    
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
    
    // Processed material info for USDA generation (public for static helper access)
    struct ProcessedMaterialInfo {
        uint64_t textureHash;
        std::string normalPath;
        std::string roughnessPath;
        std::string metallicPath;
        std::string heightPath;       // Displacement/height map path
        std::string emissivePath;     // Emission/self-illumination texture path
        std::string baseTexturePath;  // For non-Refract glass: used as transmittance_texture to color the glass
        std::string transmittancePath;  // For Refract glass: $refracttinttexture converted to DDS
        std::string albedoPath;       // For modified albedo (e.g., BFT metallic reconstruction)
        float roughnessConstant;
        float metallicConstant;
        float heightScale;            // Displacement scale (from $parallaxmapscale, default 0.025)
        bool isGlass;               // Whether this material should use the translucent glass shader
        bool isRefractShader;       // Whether this is a Refract shader (don't use baseTexture for transmittance)
        float ior;                  // Index of Refraction (for glass, default 1.52 for window/crown glass)
        float emissionIntensity;    // Emission intensity (from $emissionscale)
    };
    
    // Clear processed materials cache (for map changes)
    void ClearCache();
    
    // Enable/disable auto-processing of new materials
    void SetAutoProcessing(bool enabled) { m_autoProcessing = enabled; }
    bool IsAutoProcessingEnabled() const { return m_autoProcessing; }
    
    // Debug output control
    void SetDebugOutput(bool enabled) { m_debugOutput = enabled; }
    
    // Enable/disable experimental metallic generation from base texture brightness
    // WARNING: This is experimental and may not look correct for all materials
    // Default: disabled - dark envmap materials will use low roughness instead
    void SetMetallicGeneration(bool enabled) { m_metallicGenerationEnabled = enabled; }
    bool IsMetallicGenerationEnabled() const { return m_metallicGenerationEnabled; }
    
    // Enable/disable auto-discovery of companion textures (e.g., _normal, _height, _mask)
    // When enabled, searches for textures that follow naming conventions but aren't
    // explicitly referenced in the VMT file. Default: enabled
    void SetAutoDiscover(bool enabled) { m_autoDiscoverEnabled = enabled; }
    bool IsAutoDiscoverEnabled() const { return m_autoDiscoverEnabled; }
    
    // Enable/disable parsing of commented-out VMT properties
    // When enabled, properties that are commented out with // will still be parsed.
    // This is useful for maps where envmap/masks were commented out for vanilla Source
    // performance, but would benefit RTX Remix with roughness variation. Default: disabled
    void SetParseCommentedProperties(bool enabled) { m_parseCommentedPropertiesEnabled = enabled; }
    bool IsParseCommentedPropertiesEnabled() const { return m_parseCommentedPropertiesEnabled; }
    
    // Set output directory for generated textures
    void SetOutputDirectory(const std::string& path);
    std::string GetOutputDirectory() const { return m_outputDirectory; }
    
    // Check if the converter is initialized
    bool IsInitialized() const { return m_initialized; }
    
    // Flag to indicate USDA needs to be rewritten (new materials added since last write)
    bool NeedsUSDAUpdate() const { return m_needsUSDAUpdate; }
    void WriteUSDAIfNeeded();
    
    // =========================================================================
    // Background Processing API
    // =========================================================================
    
    // Queue materials for background processing (non-blocking, returns immediately)
    // Returns number of NEW materials queued (excludes already processed/queued)
    int QueueMaterialsForProcessing();
    
    // Check if background processing is currently active
    bool IsProcessingInBackground() const { return m_backgroundProcessing.load(std::memory_order_relaxed); }
    
    // Get count of materials waiting in queue
    size_t GetQueuedMaterialCount() const;
    
    // Get count of materials processed in current/last background run
    int GetLastProcessedCount() const { return m_lastProcessedCount.load(std::memory_order_relaxed); }
    
    // Append new materials to USDA (non-blocking, runs on background thread)
    void AppendToUSDAAsync();
    
private:
    TextureProcessor();
    ~TextureProcessor();
    
    // Prevent copying
    TextureProcessor(const TextureProcessor&) = delete;
    TextureProcessor& operator=(const TextureProcessor&) = delete;
    
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
    
    // Generate hash for a texture with pixel data (handles solid color detection)
    uint64_t GenerateTextureHashWithPixelData(const std::string& path, uint32_t width, uint32_t height, 
                                              const std::vector<uint8_t>& pixelData);
    
    // Check if texture is a solid color
    bool IsSolidColorTexture(const std::vector<uint8_t>& pixelData, uint32_t width, uint32_t height);
    
    // Mix material name into texture hash to differentiate materials using the same base texture
    uint64_t MixMaterialNameIntoHash(uint64_t baseHash, const std::string& materialName);
    
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
    
    // Convert SSBump texture to standard tangent-space normal map
    // SSBump stores directional occlusion, not normal vectors
    void ConvertSSBumpToNormal(ConvertedTexture& texture);
    
    // Write the mod.usda file with all processed materials
    bool WriteModUSDA();
    
    // Calculate roughness from phong exponent
    float PhongToRoughness(float phongExponent);
    
    // Calculate roughness using all available material properties
    float CalculateRoughness(const MaterialPBRProperties& props);
    
    // Calculate metallic hint from material properties
    float EstimateMetallic(const MaterialPBRProperties& props);
    
    // Analyze base texture brightness to help detect metallic materials
    // Black textures + envmap = metallic, grey/colored textures + envmap = non-metallic
    bool AnalyzeBaseTextureBrightness(const std::string& texturePath, float& outBrightness);
    
    // Discover companion textures that might not be explicitly referenced in the VMT
    // E.g., if basetexture is "metal/metal001", look for "metal/metal001_normal", "_height", "_mask", "_spec"
    void DiscoverCompanionTextures(const std::string& baseTexturePath, MaterialPBRProperties& props);
    
    // Create processing context for modular format handlers
    ProcessingContext CreateProcessingContext();
    
    // Get filesystem interface
    IFileSystem* GetFileSystem();
    
    remix::Interface* m_remixInterface;
    IFileSystem* m_fileSystem;
    bool m_initialized;
    bool m_autoProcessing;
    bool m_debugOutput;
    bool m_metallicGenerationEnabled;  // Experimental metallic generation from base texture brightness (default: false)
    bool m_autoDiscoverEnabled;        // Auto-discover companion textures (default: true)
    bool m_parseCommentedPropertiesEnabled; // Parse commented-out VMT properties (default: false)
    std::string m_outputDirectory;       // Absolute output directory for writing DDS files
    size_t m_lastKnownMaterialCount;     // Cache to avoid expensive GetCachedMaterials() calls
    std::atomic<bool> m_allMaterialsProcessed; // Lock-free flag for instant early exit
    
    // Cache of processed materials (includes failed/skipped - never reprocess)
    std::unordered_set<std::string> m_processedMaterials;
    
    // Cache of uploaded textures (path -> hash)
    std::unordered_map<std::string, uint64_t> m_uploadedTextures;
    
    // Cache of texture handles for cleanup
    std::unordered_map<uint64_t, remixapi_TextureHandle> m_textureHandles;
    
    // Cache of written DDS file paths (hash -> path)
    std::unordered_map<uint64_t, std::string> m_writtenTexturePaths;
    
    // Track material data for USDA generation
    std::unordered_map<uint64_t, ProcessedMaterialInfo> m_processedMaterialInfo;
    
    // Track which materials have been written to USDA (to support append-only)
    std::unordered_set<uint64_t> m_materialsWrittenToUSDA;
    
    // Flag to indicate USDA needs to be rewritten
    bool m_needsUSDAUpdate;
    
    // Statistics
    mutable std::mutex m_mutex;
    Stats m_stats;
    
    // =========================================================================
    // Background Processing Infrastructure
    // =========================================================================
    
    // Background worker thread
    std::thread m_workerThread;
    std::atomic<bool> m_workerRunning{false};
    std::atomic<bool> m_shutdownRequested{false};
    
    // Work queue for materials to process
    std::queue<std::string> m_materialQueue;
    std::unordered_set<std::string> m_queuedMaterials;  // Fast lookup to avoid duplicates
    mutable std::mutex m_queueMutex;
    std::condition_variable m_queueCondition;
    
    // Background processing state
    std::atomic<bool> m_backgroundProcessing{false};
    std::atomic<int> m_lastProcessedCount{0};
    
    // Pending USDA materials (processed but not yet written)
    std::vector<std::pair<uint64_t, ProcessedMaterialInfo>> m_pendingUSDAMaterials;
    mutable std::mutex m_pendingUSDAMutex;
    
    // Worker thread function
    void WorkerThreadFunc();
    
    // Process a single material on the worker thread (no main thread locks)
    bool ProcessMaterialOnWorker(const std::string& materialName);
    
    // Append materials to USDA file (called from worker thread)
    bool AppendMaterialsToUSDA();
    
    // Start/stop worker thread
    void StartWorkerThread();
    void StopWorkerThread();
};

// Lua bindings initialization
void InitializeLegacyTextureProcessorLuaBindings(GarrysMod::Lua::ILuaBase* LUA);

// Backwards compatibility alias
using VTFTextureConverter = TextureProcessor;

} // namespace LegacyTextureProcessor

// Backwards compatibility - allow old VTFConverter namespace to still work
namespace VTFConverter = LegacyTextureProcessor;

#endif // _WIN64
