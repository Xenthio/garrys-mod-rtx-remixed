// =========================================================================
// formats.h - Format Handler Interface
// =========================================================================
// This header defines the interface and shared types for format-specific
// texture processors. Each format (ExoPBR, GPBR, BFT, Source) implements
// these interfaces in its own file.
//
// Processing Pipeline:
//   1. VMT Parsing    → Raw VMT content extracted
//   2. Format Detection → Identify which format handler to use  
//   3. Property Extract → Format handler extracts its properties
//   4. Texture Process  → Format handler processes textures to DDS
//   5. Material Output  → Paths collected, USDA written
//
// Branching happens at step 2 - once a format is detected, that handler
// owns the rest of the pipeline for that material.
// =========================================================================

#pragma once

#ifdef _WIN64

#include "to_pbr.h"
#include <string>
#include <vector>
#include <functional>

// Forward declarations
class IFileSystem;

namespace MaterialPipeline {
namespace ToPBR {

// =========================================================================
// Processed Material Output
// =========================================================================
// This is what each format handler produces after processing.
// Contains paths to generated DDS files and constant values.
struct ProcessedMaterial {
    std::string normalPath;
    std::string roughnessPath;
    std::string metallicPath;
    std::string heightPath;
    std::string emissivePath;
    std::string transmittancePath;
    std::string albedoPath;      // For modified albedo (e.g., BFT metallic reconstruction)
    
    float roughnessConstant = 0.5f;
    float metallicConstant = 0.0f;
    float heightScale = 0.025f;
    float emissionIntensity = 1.0f;
    
    bool isGlass = false;
    bool enableTransmissionMask = false;
    float ior = 1.5f;
    
    // Albedo color multiplier (for brightening dark metallic textures)
    float albedoBoostR = 1.0f;
    float albedoBoostG = 1.0f;
    float albedoBoostB = 1.0f;
    bool hasAlbedoBoost = false;
    
    int skippedCount = 0;  // Files that already existed
    bool success = false;
};

// =========================================================================
// Texture Processing Context
// =========================================================================
// Passed to format handlers - provides access to shared utilities
// without exposing the entire TextureProcessor class.
struct ProcessingContext {
    // Function pointers to TextureProcessor utilities
    std::function<bool(const std::string&, std::vector<uint8_t>&)> readVTFFile;
    std::function<bool(const std::vector<uint8_t>&, VTFFileHeader&)> parseVTFHeader;
    std::function<bool(const std::vector<uint8_t>&, const VTFFileHeader&, ConvertedTexture&, bool)> extractPixelData;
    std::function<bool(const ConvertedTexture&, const std::string&)> writeDDS;
    std::function<void(ConvertedTexture&)> convertToOctahedral;
    std::function<void(ConvertedTexture&)> convertSSBumpToNormal;
    std::function<std::string(const std::string&, const std::string&)> generateOutputPath;
    std::function<bool(const std::string&)> fileExists;
    
    bool debugOutput = false;
    
    // Feature flags (passed from TextureProcessor settings)
    bool metallicGenerationEnabled = false;  // Enable experimental metallic extraction from envmap mask + brightness
    
    // Stats tracking (incremented by handlers)
    int* materialsWithNormals = nullptr;
    int* materialsWithRoughness = nullptr;
};

// =========================================================================
// VMT Parse Result
// =========================================================================
// Raw properties extracted from VMT file before format-specific processing.
struct VMTParseResult {
    std::string shaderName;
    std::string content;        // Full VMT content
    std::string contentLower;   // Lowercase for case-insensitive matching
    
    // Common textures (most formats use these)
    std::string baseTexture;
    std::string bumpMap;
    std::string envMapMask;
    std::string phongExponentTexture;
    
    // Common properties
    bool hasPhong = false;
    float phongExponent = 0.0f;
    float phongBoost = 1.0f;
    float phongFresnelRanges[3] = {0, 0, 0};
    bool hasPhongFresnelRanges = false;
    
    bool hasEnvMap = false;
    float envMapTint[3] = {1, 1, 1};
    bool hasEnvMapTint = false;
    
    bool isSSBump = false;
    bool isTranslucent = false;
    bool isSelfIllum = false;
    
    std::string surfaceProp;
    
    // Helper to find value in VMT content
    std::string findValue(const std::string& key) const;
};

// =========================================================================
// Format Handler Base
// =========================================================================
// Each format implements these functions. They're called in order:
//   1. Detect() - Does this VMT match this format?
//   2. ExtractProperties() - Pull format-specific data into MaterialPBRProperties
//   3. ProcessTextures() - Convert textures to DDS, fill ProcessedMaterial

namespace FormatHandler {
    
    // Priority order for detection (first match wins)
    enum class Format {
        Unknown = 0,
        ExoPBR,         // Community PBR - screenspace_general_8tex + ExoPBR proxy
        GPBR,           // Strata Source - "PBR" shader
        MWBPBR,         // MWB PBR Gen - _rgb suffix, pow(gloss,4) encoding
        BFTPseudoPBR,   // BlueFlyTrap - VertexlitGeneric + specific patterns
        SourceEngine    // Standard Source Engine materials (fallback)
    };
    
    // Detect which format a material uses
    Format DetectFormat(const VMTParseResult& vmt);
    
    // Get format name for logging
    const char* GetFormatName(Format format);
}

// =========================================================================
// Format-Specific Handlers (implemented in separate files)
// =========================================================================

namespace ExoPBR {
    bool Detect(const VMTParseResult& vmt);
    void ExtractProperties(const VMTParseResult& vmt, MaterialPBRProperties& props);
    ProcessedMaterial ProcessTextures(const MaterialPBRProperties& props, 
                                       uint64_t textureHash,
                                       const ProcessingContext& ctx);
}

namespace GPBR {
    bool Detect(const VMTParseResult& vmt);
    void ExtractProperties(const VMTParseResult& vmt, MaterialPBRProperties& props);
    ProcessedMaterial ProcessTextures(const MaterialPBRProperties& props,
                                       uint64_t textureHash, 
                                       const ProcessingContext& ctx);
}

namespace MWBPBR {
    // MWB PBR Gen - uses pow(gloss,4) encoding in exponent texture
    bool Detect(const VMTParseResult& vmt);
    void ExtractProperties(const VMTParseResult& vmt, MaterialPBRProperties& props);
    ProcessedMaterial ProcessTextures(const MaterialPBRProperties& props,
                                       uint64_t textureHash,
                                       const ProcessingContext& ctx);
}

namespace BFTPseudoPBR {
    // BlueFlyTrap PseudoPBR - simpler linear encoding
    bool Detect(const VMTParseResult& vmt);
    void ExtractProperties(const VMTParseResult& vmt, MaterialPBRProperties& props);
    ProcessedMaterial ProcessTextures(const MaterialPBRProperties& props,
                                       uint64_t textureHash,
                                       const ProcessingContext& ctx);
    
    // Check if this is a BFT channel overlay material (_ch, _ch_r, _ch_g, _ch_b)
    // These additive glow layers should be hidden in RTX as they cause white overlaps
    bool IsChannelOverlayMaterial(const VMTParseResult& vmt, const std::string& materialPath);
}

namespace SourceEngine {
    // Always matches as fallback
    void ExtractProperties(const VMTParseResult& vmt, MaterialPBRProperties& props);
    ProcessedMaterial ProcessTextures(const MaterialPBRProperties& props,
                                       uint64_t textureHash,
                                       const ProcessingContext& ctx);
}

} // namespace ToPBR
} // namespace MaterialPipeline

#endif // _WIN64
