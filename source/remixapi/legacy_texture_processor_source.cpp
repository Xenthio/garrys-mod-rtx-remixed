// =========================================================================
// Legacy Texture Processor - Standard Source Engine Format Handler
// =========================================================================
// Handles standard Source Engine materials using traditional phong/envmap:
//   - VertexLitGeneric
//   - LightmappedGeneric
//   - Other standard shaders
//
// This is the fallback handler when no community PBR format is detected.
//
// Roughness derivation (priority order):
//   PHONG MATERIALS:
//     1. $phongexponenttexture (per-pixel, best quality)
//     2. $basemapalphaphongmask (base alpha as mask)
//     3. $normalmapalphaenvmapmask (normal alpha as mask)
//     4. Normal map alpha (default phong behavior)
//     5. $phongexponent constant
//
//   NON-PHONG MATERIALS:
//     1. $envmapmask texture
//     2. $basealphaenvmapmask
//     3. Auto-discovered _mask/_spec textures
//     4. Default constant (0.5)
//
// Metallic estimation:
//   Source has no native metallic, estimate from:
//   - Dark basetexture + envmap = likely metallic
//   - Surface property hints (metal, etc.)
//   - Default: non-metallic (0.0)
// =========================================================================

#ifdef _WIN64

#include "legacy_texture_processor_formats.h"
#include <tier0/dbg.h>
#include <algorithm>
#include <cmath>

namespace LegacyTextureProcessor {
namespace SourceEngine {

// Constants
constexpr float MAX_PHONG_EXPONENT = 150.0f;

// Forward declarations
float CalculateRoughness(const MaterialPBRProperties& props);
float EstimateMetallic(const MaterialPBRProperties& props, bool enabled);

// =========================================================================
// Property Extraction
// =========================================================================

void ExtractProperties(const VMTParseResult& vmt, MaterialPBRProperties& props) {
    // Copy basic properties from VMT parse
    props.hasPhong = vmt.hasPhong;
    props.phongExponent = vmt.phongExponent;
    props.phongBoost = vmt.phongBoost;
    props.hasEnvMap = vmt.hasEnvMap;
    props.isSSBump = vmt.isSSBump;
    props.isTranslucent = vmt.isTranslucent;
    props.isSelfIllum = vmt.isSelfIllum;
    
    // Copy texture paths
    if (!vmt.baseTexture.empty()) {
        props.baseTexturePath = vmt.baseTexture;
    }
    if (!vmt.bumpMap.empty()) {
        props.bumpMapPath = vmt.bumpMap;
        props.hasBumpMap = true;
    }
    if (!vmt.envMapMask.empty()) {
        props.envMapMaskPath = vmt.envMapMask;
        props.hasEnvMapMask = true;
    }
    if (!vmt.phongExponentTexture.empty()) {
        props.phongExponentTexturePath = vmt.phongExponentTexture;
        props.hasPhongExponentTexture = true;
    }
    
    // Fresnel ranges
    if (vmt.hasPhongFresnelRanges) {
        props.phongFresnelRanges[0] = vmt.phongFresnelRanges[0];
        props.phongFresnelRanges[1] = vmt.phongFresnelRanges[1];
        props.phongFresnelRanges[2] = vmt.phongFresnelRanges[2];
        props.hasPhongFresnelRanges = true;
    }
    
    // Envmap tint
    if (vmt.hasEnvMapTint) {
        props.envMapTint[0] = vmt.envMapTint[0];
        props.envMapTint[1] = vmt.envMapTint[1];
        props.envMapTint[2] = vmt.envMapTint[2];
        props.hasEnvMapTint = true;
    }
    
    // Surface prop
    props.surfaceProp = vmt.surfaceProp;
    
    // Calculate roughness and metallic estimates
    props.roughness = CalculateRoughness(props);
    props.metallic = EstimateMetallic(props, false);  // Conservative default
}

// =========================================================================
// Roughness Calculation
// =========================================================================

static float PhongToRoughness(float phongExp, float maxExp = MAX_PHONG_EXPONENT) {
    if (phongExp < 1.0f) phongExp = 1.0f;
    if (phongExp > maxExp) phongExp = maxExp;
    
    float normalized = phongExp / maxExp;
    float roughness = std::sqrt(1.0f - normalized);
    
    if (roughness < 0.04f) roughness = 0.04f;
    return roughness;
}

float CalculateRoughness(const MaterialPBRProperties& props) {
    float roughness = 0.5f;
    
    if (props.hasPhong) {
        if (props.phongExponent > 0) {
            roughness = PhongToRoughness(props.phongExponent);
        }
        
        // Adjust for phong boost
        if (props.phongBoost > 1.0f) {
            float factor = std::log2(props.phongBoost) / 4.0f;
            roughness -= factor * 0.1f;
        }
        
        // Fresnel hints
        if (props.hasPhongFresnelRanges) {
            float width = props.phongFresnelRanges[2] - props.phongFresnelRanges[0];
            if (width > 0.8f) roughness *= 0.9f;
        }
    } else if (props.hasEnvMap) {
        if (props.hasEnvMapTint) {
            float brightness = (props.envMapTint[0] + props.envMapTint[1] + props.envMapTint[2]) / 3.0f;
            roughness = 0.6f - (brightness * 0.4f);
        } else {
            roughness = 0.4f;
        }
    }
    
    if (roughness < 0.04f) roughness = 0.04f;
    if (roughness > 1.0f) roughness = 1.0f;
    
    return roughness;
}

// =========================================================================
// Metallic Estimation
// =========================================================================

float EstimateMetallic(const MaterialPBRProperties& props, bool enabled) {
    if (!enabled) return 0.0f;
    
    // Dark textures with envmap might be metallic
    if (props.hasBaseTextureBrightness && props.baseTextureBrightness < 0.1f && props.hasEnvMap) {
        float intensity = 1.0f;
        if (props.hasEnvMapTint) {
            intensity = (props.envMapTint[0] + props.envMapTint[1] + props.envMapTint[2]) / 3.0f;
        }
        return 0.8f * intensity;
    }
    
    // Surface property hints
    std::string surfLower = props.surfaceProp;
    std::transform(surfLower.begin(), surfLower.end(), surfLower.begin(), ::tolower);
    if (surfLower.find("metal") != std::string::npos) {
        return 0.3f;
    }
    
    return 0.0f;
}

// =========================================================================
// Texture Processing
// =========================================================================

ProcessedMaterial ProcessTextures(const MaterialPBRProperties& props,
                                   uint64_t textureHash,
                                   const ProcessingContext& ctx) {
    ProcessedMaterial result;
    
    if (ctx.debugOutput) {
        Msg("[Source] Processing material: %s\n", props.materialName.c_str());
    }
    
    // Process normal map
    if (props.hasBumpMap && !props.bumpMapPath.empty()) {
        std::vector<uint8_t> fileData;
        if (ctx.readVTFFile(props.bumpMapPath, fileData)) {
            VTFFileHeader header;
            if (ctx.parseVTFHeader(fileData, header)) {
                ConvertedTexture normalTex;
                normalTex.isNormalMap = true;
                
                if (ctx.extractPixelData(fileData, header, normalTex, false)) {
                    // Handle SSBump conversion
                    if (props.isSSBump) {
                        ctx.convertSSBumpToNormal(normalTex);
                    }
                    
                    ctx.convertToOctahedral(normalTex);
                    
                    uint64_t hash = ctx.generateHash(props.bumpMapPath + "_normal", normalTex.width, normalTex.height);
                    std::string path = ctx.generateOutputPath(hash, "_normal");
                    
                    if (ctx.fileExists(path)) {
                        result.normalPath = path;
                        result.skippedCount++;
                    } else if (ctx.writeDDS(normalTex, path)) {
                        result.normalPath = path;
                        if (ctx.materialsWithNormals) (*ctx.materialsWithNormals)++;
                        if (ctx.debugOutput) Msg("[Source] Wrote normal: %s\n", path.c_str());
                    }
                }
            }
        }
    }
    
    // Process roughness from various sources
    bool hasRoughnessTexture = false;
    
    // Try $phongexponenttexture first (best quality)
    if (props.hasPhongExponentTexture && !props.phongExponentTexturePath.empty()) {
        std::vector<uint8_t> fileData;
        if (ctx.readVTFFile(props.phongExponentTexturePath, fileData)) {
            VTFFileHeader header;
            if (ctx.parseVTFHeader(fileData, header)) {
                ConvertedTexture expTex;
                if (ctx.extractPixelData(fileData, header, expTex, false)) {
                    // Convert exponent to roughness
                    ConvertedTexture roughTex;
                    roughTex.width = expTex.width;
                    roughTex.height = expTex.height;
                    roughTex.mipLevels = 1;
                    roughTex.format = REMIXAPI_FORMAT_R8G8B8A8_UNORM;
                    roughTex.isNormalMap = false;
                    roughTex.pixelData.resize(expTex.width * expTex.height * 4);
                    
                    for (uint32_t i = 0; i < expTex.width * expTex.height; i++) {
                        // Exponent texture: higher = shinier = lower roughness
                        float exp = static_cast<float>(expTex.pixelData[i * 4 + 0]);
                        float roughness = PhongToRoughness(exp * MAX_PHONG_EXPONENT / 255.0f);
                        uint8_t roughByte = static_cast<uint8_t>(roughness * 255.0f);
                        
                        roughTex.pixelData[i * 4 + 0] = roughByte;
                        roughTex.pixelData[i * 4 + 1] = roughByte;
                        roughTex.pixelData[i * 4 + 2] = roughByte;
                        roughTex.pixelData[i * 4 + 3] = 255;
                    }
                    
                    uint64_t hash = ctx.generateHash(props.phongExponentTexturePath + "_rough", roughTex.width, roughTex.height);
                    std::string path = ctx.generateOutputPath(hash, "_roughness");
                    
                    if (ctx.fileExists(path)) {
                        result.roughnessPath = path;
                        result.skippedCount++;
                        hasRoughnessTexture = true;
                    } else if (ctx.writeDDS(roughTex, path)) {
                        result.roughnessPath = path;
                        if (ctx.materialsWithRoughness) (*ctx.materialsWithRoughness)++;
                        hasRoughnessTexture = true;
                        if (ctx.debugOutput) Msg("[Source] Wrote roughness from exponent: %s\n", path.c_str());
                    }
                }
            }
        }
    }
    
    // Try $envmapmask if no exponent texture
    if (!hasRoughnessTexture && props.hasEnvMapMask && !props.envMapMaskPath.empty()) {
        std::vector<uint8_t> fileData;
        if (ctx.readVTFFile(props.envMapMaskPath, fileData)) {
            VTFFileHeader header;
            if (ctx.parseVTFHeader(fileData, header)) {
                ConvertedTexture maskTex;
                if (ctx.extractPixelData(fileData, header, maskTex, false)) {
                    // Envmap mask: bright = reflective = low roughness
                    ConvertedTexture roughTex;
                    roughTex.width = maskTex.width;
                    roughTex.height = maskTex.height;
                    roughTex.mipLevels = 1;
                    roughTex.format = REMIXAPI_FORMAT_R8G8B8A8_UNORM;
                    roughTex.isNormalMap = false;
                    roughTex.pixelData.resize(maskTex.width * maskTex.height * 4);
                    
                    for (uint32_t i = 0; i < maskTex.width * maskTex.height; i++) {
                        uint8_t mask = maskTex.pixelData[i * 4 + 0];
                        uint8_t roughness = 255 - mask;  // Invert
                        
                        roughTex.pixelData[i * 4 + 0] = roughness;
                        roughTex.pixelData[i * 4 + 1] = roughness;
                        roughTex.pixelData[i * 4 + 2] = roughness;
                        roughTex.pixelData[i * 4 + 3] = 255;
                    }
                    
                    uint64_t hash = ctx.generateHash(props.envMapMaskPath + "_rough", roughTex.width, roughTex.height);
                    std::string path = ctx.generateOutputPath(hash, "_roughness");
                    
                    if (ctx.fileExists(path)) {
                        result.roughnessPath = path;
                        result.skippedCount++;
                        hasRoughnessTexture = true;
                    } else if (ctx.writeDDS(roughTex, path)) {
                        result.roughnessPath = path;
                        if (ctx.materialsWithRoughness) (*ctx.materialsWithRoughness)++;
                        hasRoughnessTexture = true;
                        if (ctx.debugOutput) Msg("[Source] Wrote roughness from envmapmask: %s\n", path.c_str());
                    }
                }
            }
        }
    }
    
    // Set constants for materials without textures
    if (!hasRoughnessTexture) {
        result.roughnessConstant = props.roughness;
    }
    result.metallicConstant = props.metallic;
    
    result.success = true;
    if (ctx.debugOutput) {
        Msg("[Source] Complete: %s (skipped %d existing)\n", props.materialName.c_str(), result.skippedCount);
    }
    
    return result;
}

} // namespace SourceEngine
} // namespace LegacyTextureProcessor

#endif // _WIN64
