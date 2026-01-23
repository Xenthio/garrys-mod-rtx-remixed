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
// Helper: Check if alpha channel has meaningful variation
// =========================================================================
static bool HasAlphaVariation(const std::vector<uint8_t>& pixelData) {
    if (pixelData.size() < 4) return false;
    
    uint8_t firstAlpha = pixelData[3];
    // Check all alpha values starting at index 7 (second pixel's alpha)
    for (size_t i = 7; i < pixelData.size(); i += 4) {
        if (pixelData[i] != firstAlpha) {
            return true;
        }
    }
    return false;
}

// =========================================================================
// Helper: Generate roughness texture from source data
// =========================================================================
static bool GenerateRoughnessFromSource(
    const ConvertedTexture& sourceTex,
    bool useAlphaChannel,
    bool isPhongExponentTexture,
    bool isInvertedMask,
    const std::string& sourcePath,
    const ProcessingContext& ctx,
    ProcessedMaterial& result) {
    
    // If using alpha channel, check for meaningful variation
    if (useAlphaChannel && !HasAlphaVariation(sourceTex.pixelData)) {
        if (ctx.debugOutput) {
            Msg("[Source] Alpha channel has no variation, skipping roughness texture\n");
        }
        return false;
    }
    
    ConvertedTexture roughTex;
    roughTex.width = sourceTex.width;
    roughTex.height = sourceTex.height;
    roughTex.mipLevels = 1;
    roughTex.format = REMIXAPI_FORMAT_R8G8B8A8_UNORM;
    roughTex.isNormalMap = false;
    roughTex.pixelData.resize(sourceTex.width * sourceTex.height * 4);
    
    for (uint32_t i = 0; i < sourceTex.width * sourceTex.height; i++) {
        uint8_t sourceValue;
        
        if (useAlphaChannel) {
            sourceValue = sourceTex.pixelData[i * 4 + 3];  // Alpha channel
        } else {
            sourceValue = sourceTex.pixelData[i * 4 + 0];  // Red channel
        }
        
        uint8_t roughness;
        if (isPhongExponentTexture) {
            // $phongexponenttexture: value is phong exponent (0-255)
            // Higher = shinier = LOWER roughness
            float exp = static_cast<float>(sourceValue);
            float rough = PhongToRoughness(exp * MAX_PHONG_EXPONENT / 255.0f);
            roughness = static_cast<uint8_t>(rough * 255.0f);
        } else if (isInvertedMask) {
            // $basealphaenvmapmask: white = masked (matte), black = reflective (shiny)
            // So we DON'T invert - high value = high roughness
            roughness = sourceValue;
        } else {
            // Standard envmap mask: bright = reflective = low roughness
            roughness = 255 - sourceValue;
        }
        
        roughTex.pixelData[i * 4 + 0] = roughness;
        roughTex.pixelData[i * 4 + 1] = roughness;
        roughTex.pixelData[i * 4 + 2] = roughness;
        roughTex.pixelData[i * 4 + 3] = 255;
    }
    
    uint64_t hash = ctx.generateHash(sourcePath + "_rough", roughTex.width, roughTex.height);
    std::string path = ctx.generateOutputPath(hash, "_roughness");
    
    if (ctx.fileExists(path)) {
        result.roughnessPath = path;
        result.skippedCount++;
        return true;
    } else if (ctx.writeDDS(roughTex, path)) {
        result.roughnessPath = path;
        if (ctx.materialsWithRoughness) (*ctx.materialsWithRoughness)++;
        return true;
    }
    
    return false;
}

// =========================================================================
// Texture Processing - Full implementation matching original behavior
// =========================================================================

ProcessedMaterial ProcessTextures(const MaterialPBRProperties& props,
                                   uint64_t textureHash,
                                   const ProcessingContext& ctx) {
    ProcessedMaterial result;
    
    if (ctx.debugOutput) {
        Msg("[Source] Processing material: %s\n", props.materialName.c_str());
        Msg("[Source]   hasPhong=%d, hasPhongExpTex=%d, normMapAlpha=%d, hasBaseMapAlphaPhong=%d\n",
            props.hasPhong, props.hasPhongExponentTexture, props.normalMapAlphaEnvMapMask, props.hasBaseMapAlphaPhongMask);
        Msg("[Source]   hasBump=%d, hasEnvMapMask=%d, hasBaseAlphaEnvMapMask=%d, hasEnvMap=%d\n",
            props.hasBumpMap, props.hasEnvMapMask, props.hasBaseAlphaEnvMapMask, props.hasEnvMap);
        Msg("[Source]   hasDiscoveredMask=%d (%s)\n", props.hasDiscoveredMask, props.discoveredMaskPath.c_str());
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
    
    // =========================================================================
    // Roughness texture generation - comprehensive priority system
    // =========================================================================
    bool hasRoughnessTexture = false;
    std::string vtfPath;
    bool useAlphaChannel = false;
    bool isPhongExponentTexture = false;
    bool isInvertedMask = false;
    
    // =========================================================================
    // PHONG MATERIAL PATH - prioritize phong-specific properties
    // =========================================================================
    if (props.hasPhong) {
        // Priority 1: $phongexponenttexture (best quality - dedicated roughness data)
        if (props.hasPhongExponentTexture && !props.phongExponentTexturePath.empty()) {
            vtfPath = props.phongExponentTexturePath;
            useAlphaChannel = false;
            isPhongExponentTexture = true;
            if (ctx.debugOutput) Msg("[Source] [PHONG] Using $phongexponenttexture (best quality)\n");
        }
        // Priority 2: $basemapalphaphongmask - base texture alpha as phong mask
        else if (props.hasBaseMapAlphaPhongMask && !props.baseTexturePath.empty()) {
            vtfPath = props.baseTexturePath;
            useAlphaChannel = true;
            if (ctx.debugOutput) Msg("[Source] [PHONG] Using base texture alpha ($basemapalphaphongmask)\n");
        }
        // Priority 3: $normalmapalphaenvmapmask - normal map alpha as mask
        else if (props.normalMapAlphaEnvMapMask && props.hasBumpMap && !props.bumpMapPath.empty()) {
            vtfPath = props.bumpMapPath;
            useAlphaChannel = true;
            if (ctx.debugOutput) Msg("[Source] [PHONG] Using normal map alpha ($normalmapalphaenvmapmask)\n");
        }
        // Priority 4: Default Source Engine behavior - phong + bumpmap = normal alpha has phong mask
        else if (props.hasBumpMap && !props.bumpMapPath.empty()) {
            vtfPath = props.bumpMapPath;
            useAlphaChannel = true;
            if (ctx.debugOutput) Msg("[Source] [PHONG] Using normal map alpha (default Source behavior)\n");
        }
    }
    
    // =========================================================================
    // NON-PHONG / FALLBACK PATH - use envmap-based properties
    // =========================================================================
    if (vtfPath.empty()) {
        // Priority 5: $envmapmask - separate envmap mask texture
        if (props.hasEnvMapMask && !props.envMapMaskPath.empty()) {
            vtfPath = props.envMapMaskPath;
            useAlphaChannel = false;
            if (ctx.debugOutput) Msg("[Source] Using $envmapmask for roughness\n");
        }
        // Priority 6: $basealphaenvmapmask - base texture alpha as envmap mask (INVERTED!)
        else if (props.hasBaseAlphaEnvMapMask && !props.baseTexturePath.empty()) {
            vtfPath = props.baseTexturePath;
            useAlphaChannel = true;
            isInvertedMask = true;
            if (ctx.debugOutput) Msg("[Source] Using base texture alpha ($basealphaenvmapmask - INVERTED)\n");
        }
        // Priority 7: Auto-discovered _mask/_spec textures
        else if (props.hasDiscoveredMask && !props.discoveredMaskPath.empty()) {
            vtfPath = props.discoveredMaskPath;
            useAlphaChannel = false;
            if (ctx.debugOutput) Msg("[Source] Using auto-discovered mask: %s\n", props.discoveredMaskPath.c_str());
        }
        // Priority 8: $normalmapalphaenvmapmask (non-phong path)
        else if (props.normalMapAlphaEnvMapMask && props.hasBumpMap && !props.bumpMapPath.empty()) {
            vtfPath = props.bumpMapPath;
            useAlphaChannel = true;
            if (ctx.debugOutput) Msg("[Source] Using normal map alpha ($normalmapalphaenvmapmask - non-phong)\n");
        }
        // Priority 9: $envmap + normal map alpha (implicit $normalmapalphaenvmapmask)
        else if ((props.hasEnvMap || props.hasEnvMapTint) && props.hasBumpMap && !props.bumpMapPath.empty()) {
            vtfPath = props.bumpMapPath;
            useAlphaChannel = true;
            if (ctx.debugOutput) Msg("[Source] Trying normal map alpha (implicit envmap roughness)\n");
        }
        // Priority 10: $envmap + base texture alpha (implicit $basealphaenvmapmask)
        else if ((props.hasEnvMap || props.hasEnvMapTint) && !props.baseTexturePath.empty()) {
            vtfPath = props.baseTexturePath;
            useAlphaChannel = true;
            isInvertedMask = true;
            if (ctx.debugOutput) Msg("[Source] Trying base texture alpha (implicit envmap - INVERTED)\n");
        }
        // Priority 11 (LAST RESORT): Try normal map alpha anyway for materials with bumpmap
        else if (props.hasBumpMap && !props.bumpMapPath.empty()) {
            vtfPath = props.bumpMapPath;
            useAlphaChannel = true;
            if (ctx.debugOutput) Msg("[Source] Last resort - trying normal map alpha\n");
        }
    }
    
    // Try to generate roughness texture from found source
    if (!vtfPath.empty()) {
        std::vector<uint8_t> fileData;
        if (ctx.readVTFFile(vtfPath, fileData)) {
            VTFFileHeader header;
            if (ctx.parseVTFHeader(fileData, header)) {
                ConvertedTexture sourceTex;
                if (ctx.extractPixelData(fileData, header, sourceTex, false)) {
                    hasRoughnessTexture = GenerateRoughnessFromSource(
                        sourceTex, useAlphaChannel, isPhongExponentTexture, isInvertedMask,
                        vtfPath, ctx, result);
                    
                    if (hasRoughnessTexture && ctx.debugOutput) {
                        Msg("[Source] Generated roughness texture from: %s\n", vtfPath.c_str());
                    }
                }
            }
        }
    }
    
    // Set constants for materials without textures
    if (!hasRoughnessTexture) {
        result.roughnessConstant = props.roughness;
        if (ctx.debugOutput) {
            Msg("[Source] No roughness texture generated, using constant: %.2f\n", props.roughness);
        }
    }
    result.metallicConstant = props.metallic;
    
    // =========================================================================
    // Height map processing
    // =========================================================================
    std::string heightMapPath;
    if (props.hasParallaxMap && !props.parallaxMapPath.empty()) {
        heightMapPath = props.parallaxMapPath;
    } else if (props.hasDiscoveredHeight && !props.discoveredHeightPath.empty()) {
        heightMapPath = props.discoveredHeightPath;
    }
    
    if (!heightMapPath.empty()) {
        std::vector<uint8_t> fileData;
        if (ctx.readVTFFile(heightMapPath, fileData)) {
            VTFFileHeader header;
            if (ctx.parseVTFHeader(fileData, header)) {
                ConvertedTexture heightTex;
                if (ctx.extractPixelData(fileData, header, heightTex, false)) {
                    uint64_t hash = ctx.generateHash(heightMapPath + "_height", heightTex.width, heightTex.height);
                    std::string path = ctx.generateOutputPath(hash, "_height");
                    
                    if (ctx.fileExists(path)) {
                        result.heightPath = path;
                        result.heightScale = props.hasParallaxMapScale ? props.parallaxMapScale : 0.025f;
                        result.skippedCount++;
                    } else if (ctx.writeDDS(heightTex, path)) {
                        result.heightPath = path;
                        result.heightScale = props.hasParallaxMapScale ? props.parallaxMapScale : 0.025f;
                        if (ctx.debugOutput) Msg("[Source] Wrote height: %s\n", path.c_str());
                    }
                }
            }
        }
    }
    
    result.success = true;
    if (ctx.debugOutput) {
        Msg("[Source] Complete: %s (skipped %d existing)\n", props.materialName.c_str(), result.skippedCount);
    }
    
    return result;
}

} // namespace SourceEngine
} // namespace LegacyTextureProcessor

#endif // _WIN64
