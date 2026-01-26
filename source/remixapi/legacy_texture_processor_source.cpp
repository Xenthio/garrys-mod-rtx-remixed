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
// Metallic extraction:
//   In Source Engine, envmap on a perfectly black texture creates a metallic
//   appearance (pure environment reflection). This is equivalent to white albedo
//   + metallic=1 in PBR. We extract metallic information by combining:
//   - $envmapmask: identifies reflective areas
//   - Base texture brightness: dark areas with reflections = metallic
//   
//   For proper PBR rendering, we also modify the albedo:
//   - Dark metallic areas need brightened albedo (toward white)
//   - This ensures reflections look correct (metals tint reflections by albedo)
//   - Without this, dark metallic areas would show dark/muddy reflections
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
    
    // Convert Source Engine phong exponent to PBR roughness
    // Use physically-based conversion from Blinn-Phong to GGX roughness
    // This produces more appropriate shininess for typical Source materials:
    //   phongExponent 5-10: fairly shiny (0.45-0.55 roughness)
    //   phongExponent 20-50: very shiny (0.25-0.35 roughness)
    //   phongExponent 100+: mirror-like (0.10-0.18 roughness)
    float roughness = std::sqrt(2.0f / (phongExp + 2.0f));
    
    if (roughness < 0.04f) roughness = 0.04f;
    if (roughness > 1.0f) roughness = 1.0f;
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
    bool isPhongMask,
    float constantPhongExponent,
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
    
    // Debug: Track min/max/avg values
    uint8_t minSource = 255, maxSource = 0;
    uint8_t minRough = 255, maxRough = 0;
    uint32_t totalSource = 0, totalRough = 0;
    
    for (uint32_t i = 0; i < sourceTex.width * sourceTex.height; i++) {
        uint8_t sourceValue;
        
        if (useAlphaChannel) {
            sourceValue = sourceTex.pixelData[i * 4 + 3];  // Alpha channel
        } else {
            sourceValue = sourceTex.pixelData[i * 4 + 0];  // Red channel
        }
        
        // Track stats
        if (sourceValue < minSource) minSource = sourceValue;
        if (sourceValue > maxSource) maxSource = sourceValue;
        totalSource += sourceValue;
        
        uint8_t roughness;
        if (isPhongExponentTexture) {
            // $phongexponenttexture: value is phong exponent (0-255)
            // Higher = shinier = LOWER roughness
            float exp = static_cast<float>(sourceValue);
            float rough = PhongToRoughness(exp * MAX_PHONG_EXPONENT / 255.0f);
            roughness = static_cast<uint8_t>(rough * 255.0f);
        } else if (isPhongMask) {
            // Phong mask (bumpmap alpha): modulates the constant $phongexponent
            // In Source Engine, this is typically painted as a fairly binary mask:
            //   Black (0) = no phong (matte areas like pores, wrinkles)
            //   White (255) = full phong (shiny areas like skin highlights)
            // Use a steep power curve so even modest mask values give mostly-shiny result
            float maskStrength = static_cast<float>(sourceValue) / 255.0f;
            float phongRoughness = PhongToRoughness(constantPhongExponent);
            
            // Power curve: low mask values stay rough, but it quickly drops to phong roughness
            // pow(1-mask, 5) gives: mask 0.0?rough 1.0, mask 0.2?rough 0.58, mask 0.5?rough 0.40, mask 1.0?rough 0.378
            float falloff = std::pow(1.0f - maskStrength, 5.0f);
            float rough = phongRoughness + (1.0f - phongRoughness) * falloff;
            
            roughness = static_cast<uint8_t>(rough * 255.0f);
            
            // Debug: Log first few pixels
            if (ctx.debugOutput && i < 10) {
                Msg("[Source] Pixel %d: alpha=%d, maskStr=%.3f, falloff=%.3f, phongRough=%.3f, finalRough=%.3f (byte=%d)\n",
                    i, sourceValue, maskStrength, falloff, phongRoughness, rough, roughness);
            }
        } else if (isInvertedMask) {
            // $basealphaenvmapmask: white = masked (matte), black = reflective (shiny)
            // So we DON'T invert - high value = high roughness
            roughness = sourceValue;
        } else {
            // Standard envmap mask: bright = reflective = low roughness
            roughness = 255 - sourceValue;
        }
        
        // Track roughness stats
        if (roughness < minRough) minRough = roughness;
        if (roughness > maxRough) maxRough = roughness;
        totalRough += roughness;
        
        roughTex.pixelData[i * 4 + 0] = roughness;
        roughTex.pixelData[i * 4 + 1] = roughness;
        roughTex.pixelData[i * 4 + 2] = roughness;
        roughTex.pixelData[i * 4 + 3] = 255;
    }
    
    // Log statistics
    if (ctx.debugOutput) {
        uint32_t pixelCount = sourceTex.width * sourceTex.height;
        float avgSource = static_cast<float>(totalSource) / pixelCount;
        float avgRough = static_cast<float>(totalRough) / pixelCount;
        Msg("[Source] Roughness texture stats:\n");
        Msg("[Source]   Source values: min=%d, max=%d, avg=%.1f\n", minSource, maxSource, avgSource);
        Msg("[Source]   Roughness values: min=%d, max=%d, avg=%.1f\n", minRough, maxRough, avgRough);
        Msg("[Source]   PhongExp=%.1f -> BaseRoughness=%.3f\n", constantPhongExponent, PhongToRoughness(constantPhongExponent));
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
// Metallic Extraction from Envmap Mask + Base Texture Brightness
// =========================================================================
// In Source Engine, envmap on a dark/black texture produces a metallic look:
// - Black diffuse + bright envmap = pure reflection = looks like polished metal
// - In PBR terms: this is white albedo + metallic=1.0
//
// This function generates a per-pixel metallic map by combining:
// 1. Envmap mask (where reflections are strong)
// 2. Base texture brightness (dark areas are more metallic when reflective)
//
// The key insight: in Source, a reflective dark surface IS metallic.
// A reflective bright surface is just a shiny non-metal (like plastic).
//
// Returns: metallicTex filled with per-pixel metallic values
//          albedoTex filled with modified albedo (brightened for metallic areas)
// =========================================================================
struct MetallicExtractionResult {
    bool hasMetallic;
    bool hasModifiedAlbedo;
};

// Envmap mask source types
enum class EnvmapMaskSource {
    RedChannel,     // Separate $envmapmask texture - use red channel
    AlphaChannel    // $normalmapalphaenvmapmask or $basealphaenvmapmask - use alpha channel
};

static MetallicExtractionResult GenerateMetallicFromEnvmapMaskAndBrightness(
    const ConvertedTexture& baseTex,
    const ConvertedTexture& envmapMaskTex,
    EnvmapMaskSource maskSource,
    bool invertMask,  // true for $basealphaenvmapmask (transparent=reflective, opaque=matte)
    const MaterialPBRProperties& props,
    const ProcessingContext& ctx,
    ProcessedMaterial& result) {
    
    MetallicExtractionResult extractResult = { false, false };
    
    // Textures must match in size for per-pixel analysis
    if (baseTex.width != envmapMaskTex.width || baseTex.height != envmapMaskTex.height) {
        if (ctx.debugOutput) {
            Msg("[Source] [Metallic] Texture size mismatch: base %dx%d vs envmapmask %dx%d\n",
                baseTex.width, baseTex.height, envmapMaskTex.width, envmapMaskTex.height);
        }
        return extractResult;
    }
    
    uint32_t width = baseTex.width;
    uint32_t height = baseTex.height;
    // Use uint64_t to prevent overflow for very large textures
    uint64_t pixelCount64 = static_cast<uint64_t>(width) * height;
    // Check for overflow before casting to uint32_t
    if (pixelCount64 > UINT32_MAX / 4) {
        Msg("[Source] [Metallic] Texture too large (%u x %u), skipping metallic extraction\n", width, height);
        return extractResult;
    }
    uint32_t pixelCount = static_cast<uint32_t>(pixelCount64);
    
    // Create output textures
    ConvertedTexture metallicTex;
    metallicTex.width = width;
    metallicTex.height = height;
    metallicTex.mipLevels = 1;
    metallicTex.format = REMIXAPI_FORMAT_R8G8B8A8_UNORM;
    metallicTex.isNormalMap = false;
    metallicTex.pixelData.resize(pixelCount * 4);
    
    ConvertedTexture albedoTex;
    albedoTex.width = width;
    albedoTex.height = height;
    albedoTex.mipLevels = 1;
    albedoTex.format = REMIXAPI_FORMAT_R8G8B8A8_UNORM;
    albedoTex.isNormalMap = false;
    albedoTex.pixelData.resize(pixelCount * 4);
    
    // Constants for metallic extraction
    // In Source, brightness below this threshold with strong envmap = metallic
    constexpr float BRIGHTNESS_THRESHOLD = 0.2f;  // Pixel must be very dark (near black); intentionally low to catch genuinely dark metallic surfaces while avoiding false positives on moderately dark diffuse areas
    // Minimum envmap mask strength to consider a pixel reflective enough for metallic
    constexpr float ENVMAP_MIN_STRENGTH = 0.3f;  // Lower threshold - 30%+ envmap
    // Minimum metallic value to count as "metallic" for statistics and processing
    constexpr float METALLIC_PIXEL_THRESHOLD = 0.05f;  // Count more pixels as metallic
    // Minimum max metallic value to justify generating textures
    constexpr float METALLIC_MAX_THRESHOLD = 0.15f;  // Lower threshold to generate textures
    // Minimum ratio of metallic pixels to justify generating textures  
    constexpr float METALLIC_RATIO_THRESHOLD = 0.001f;  // At least 0.1% of pixels
    // Small epsilon to prevent division by zero
    constexpr float EPSILON = 0.001f;
    
    // Track statistics (initialized to consistent values)
    uint32_t metallicPixels = 0;
    float totalMetallic = 0.0f;
    float minMetallic = 0.0f, maxMetallic = 0.0f;
    bool firstMetallicPixel = true;
    
    // =========================================================================
    // Generate metallic based on simple rule: dark pixel + strong envmap = metallic
    // In Source Engine, a black texture with envmap looks like polished metal
    // because envmap provides pure reflection with no diffuse to dim it
    // =========================================================================
    for (uint32_t i = 0; i < pixelCount; i++) {
        size_t srcIdx = i * 4;
        
        // Read base texture RGB
        uint8_t baseR = baseTex.pixelData[srcIdx + 0];
        uint8_t baseG = baseTex.pixelData[srcIdx + 1];
        uint8_t baseB = baseTex.pixelData[srcIdx + 2];
        uint8_t baseA = baseTex.pixelData[srcIdx + 3];
        
        // Read envmap mask from appropriate channel based on source
        // - Separate $envmapmask texture: grayscale, use red channel
        // - $normalmapalphaenvmapmask: use alpha channel of normal map  
        // - $basealphaenvmapmask: use alpha channel of base texture (INVERTED!)
        uint8_t envmapMaskRaw = (maskSource == EnvmapMaskSource::AlphaChannel) 
            ? envmapMaskTex.pixelData[srcIdx + 3]   // Alpha channel
            : envmapMaskTex.pixelData[srcIdx + 0];  // Red channel
        
        // Handle inverted masks ($basealphaenvmapmask: transparent=reflective, opaque=matte)
        uint8_t envmapMask = invertMask ? (255 - envmapMaskRaw) : envmapMaskRaw;
        
        // Calculate pixel brightness (luminance)
        float brightness = (0.299f * baseR + 0.587f * baseG + 0.114f * baseB) / 255.0f;
        float envmapStrength = envmapMask / 255.0f;
        
        // Calculate metallic value
        // Simple rule: dark pixel + strong envmap = metallic
        // In Source, a black/dark texture with strong envmap = polished metal look
        float metallic = 0.0f;
        
        if (envmapStrength > ENVMAP_MIN_STRENGTH) {
            // Calculate how "dark" this pixel is relative to the brightness threshold
            // brightness 0 -> darkness 1.0 (fully dark, max metallic potential)
            // brightness >= threshold -> darkness 0.0 (too bright, not metallic)
            float darkness = 1.0f - min(brightness / BRIGHTNESS_THRESHOLD, 1.0f);
            
            // Normalize envmap strength above the threshold to 0-1 range
            float envmapFactor = (envmapStrength - ENVMAP_MIN_STRENGTH) / (1.0f - ENVMAP_MIN_STRENGTH + EPSILON);
            envmapFactor = std::clamp(envmapFactor, 0.0f, 1.0f);
            
            // Metallic = darkness * envmap factor
            // Both must be high for strong metallic
            // No falloff - we want strong metallic for dark areas with good envmap
            metallic = darkness * envmapFactor;
        }
        
        // Track statistics - use first/subsequent logic for correct min/max
        if (metallic > METALLIC_PIXEL_THRESHOLD) {
            metallicPixels++;
            if (firstMetallicPixel) {
                minMetallic = metallic;
                maxMetallic = metallic;
                firstMetallicPixel = false;
            } else {
                minMetallic = min(minMetallic, metallic);
                maxMetallic = max(maxMetallic, metallic);
            }
        }
        totalMetallic += metallic;
        
        uint8_t metallicByte = static_cast<uint8_t>(std::clamp(metallic * 255.0f, 0.0f, 255.0f));
        
        // Write metallic texture (grayscale)
        metallicTex.pixelData[srcIdx + 0] = metallicByte;
        metallicTex.pixelData[srcIdx + 1] = metallicByte;
        metallicTex.pixelData[srcIdx + 2] = metallicByte;
        metallicTex.pixelData[srcIdx + 3] = 255;
        
        // Calculate modified albedo for metallic areas
        // In PBR, metallic surfaces use albedo to tint reflections
        // In Source, black + envmap = reflection tinted by $envmaptint (default white)
        // So we need to blend dark metallic areas toward the envmap tint color
        //
        // The formula:
        // - For non-metallic pixels: keep original albedo
        // - For metallic pixels: blend toward envmaptint using a sharp curve
        //   to avoid muddy grey colors from partial blending
        
        float albedoR = baseR / 255.0f;
        float albedoG = baseG / 255.0f;
        float albedoB = baseB / 255.0f;
        
        if (metallic > METALLIC_PIXEL_THRESHOLD) {
            // Determine target color for metallic areas
            // If $envmaptint is specified, use it to tint the reflections
            // Otherwise default to white (untinted reflections)
            float targetR = 1.0f, targetG = 1.0f, targetB = 1.0f;
            if (props.hasEnvMapTint) {
                targetR = props.envMapTint[0];
                targetG = props.envMapTint[1];
                targetB = props.envMapTint[2];
            }
            
            // Use metallic^2 as blend factor - conservative, reduces whitening for low metallic:
            // - Low metallic (0.3) → 9% blend → minimal change
            // - Medium metallic (0.5) → 25% blend → moderate change
            // - High metallic (0.8) → 64% blend → strong change
            // - Full metallic (1.0) → 100% blend → full tint
            // This prevents "too white" textures for low metallic values
            float blendFactor = metallic * metallic;  // metallic^2
            
            // For metallic areas, blend toward the tint color
            // Only significantly modify pixels with strong metallic values
            float newR = albedoR + (targetR - albedoR) * blendFactor;
            float newG = albedoG + (targetG - albedoG) * blendFactor;
            float newB = albedoB + (targetB - albedoB) * blendFactor;
            
            // Only boost brightness for strongly metallic pixels (metallic > 0.5)
            // This prevents slight metallic areas from becoming too bright
            if (metallic > 0.5f) {
                float currentBrightness = 0.299f * newR + 0.587f * newG + 0.114f * newB;
                float targetBrightness = 0.299f * targetR + 0.587f * targetG + 0.114f * targetB;
                // Scale the minimum brightness requirement by how metallic the pixel is
                // 0 at 0.5, full at 1.0
                float minBrightness = targetBrightness * (metallic - 0.5f) / 0.5f;
                
                if (currentBrightness < minBrightness && minBrightness > 0.01f) {
                    float boost = minBrightness / (currentBrightness + EPSILON);
                    newR = min(newR * boost, 1.0f);
                    newG = min(newG * boost, 1.0f);
                    newB = min(newB * boost, 1.0f);
                }
            }
            
            albedoR = newR;
            albedoG = newG;
            albedoB = newB;
        }
        
        // Write modified albedo
        albedoTex.pixelData[srcIdx + 0] = static_cast<uint8_t>(std::clamp(albedoR * 255.0f, 0.0f, 255.0f));
        albedoTex.pixelData[srcIdx + 1] = static_cast<uint8_t>(std::clamp(albedoG * 255.0f, 0.0f, 255.0f));
        albedoTex.pixelData[srcIdx + 2] = static_cast<uint8_t>(std::clamp(albedoB * 255.0f, 0.0f, 255.0f));
        albedoTex.pixelData[srcIdx + 3] = baseA; // Preserve alpha
    }
    
    // Only write textures if we found meaningful metallic content
    float metallicRatio = static_cast<float>(metallicPixels) / pixelCount;
    float avgMetallic = totalMetallic / pixelCount;
    
    if (ctx.debugOutput) {
        Msg("[Source] [Metallic] Analysis: %.1f%% pixels metallic, avg=%.3f, min=%.3f, max=%.3f\n",
            metallicRatio * 100.0f, avgMetallic, minMetallic, maxMetallic);
        if (props.hasEnvMapTint) {
            Msg("[Source] [Metallic] Using $envmaptint for reflection color: (%.2f, %.2f, %.2f)\n",
                props.envMapTint[0], props.envMapTint[1], props.envMapTint[2]);
        }
    }
    
    // Skip if no significant metallic content
    if (metallicRatio < METALLIC_RATIO_THRESHOLD || maxMetallic < METALLIC_MAX_THRESHOLD) {
        if (ctx.debugOutput) {
            Msg("[Source] [Metallic] No significant metallic content, skipping texture generation\n");
        }
        return extractResult;
    }
    
    // Write metallic texture
    uint64_t metallicHash = ctx.generateHash(props.baseTexturePath + "_envmetal", width, height);
    std::string metallicPath = ctx.generateOutputPath(metallicHash, "_metallic");
    
    if (ctx.fileExists(metallicPath)) {
        result.metallicPath = metallicPath;
        result.skippedCount++;
        extractResult.hasMetallic = true;
        if (ctx.debugOutput) {
            Msg("[Source] [Metallic] Metallic texture already exists: %s\n", metallicPath.c_str());
        }
    } else if (ctx.writeDDS(metallicTex, metallicPath)) {
        result.metallicPath = metallicPath;
        extractResult.hasMetallic = true;
        if (ctx.debugOutput) {
            Msg("[Source] [Metallic] Wrote metallic texture: %s\n", metallicPath.c_str());
        }
    }
    
    // Write modified albedo texture
    uint64_t albedoHash = ctx.generateHash(props.baseTexturePath + "_metalalbedo", width, height);
    std::string albedoPath = ctx.generateOutputPath(albedoHash, "_albedo");
    
    if (ctx.fileExists(albedoPath)) {
        result.albedoPath = albedoPath;
        result.skippedCount++;
        extractResult.hasModifiedAlbedo = true;
        if (ctx.debugOutput) {
            Msg("[Source] [Metallic] Modified albedo already exists: %s\n", albedoPath.c_str());
        }
    } else if (ctx.writeDDS(albedoTex, albedoPath)) {
        result.albedoPath = albedoPath;
        extractResult.hasModifiedAlbedo = true;
        if (ctx.debugOutput) {
            Msg("[Source] [Metallic] Wrote modified albedo: %s\n", albedoPath.c_str());
        }
    }
    
    return extractResult;
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
                // Check if output file already exists BEFORE expensive decompression
                uint64_t hash = ctx.generateHash(props.bumpMapPath + "_normal", header.width, header.height);
                std::string path = ctx.generateOutputPath(hash, "_normal");
                
                if (ctx.fileExists(path)) {
                    // File already exists - skip all processing
                    result.normalPath = path;
                    result.skippedCount++;
                    if (ctx.debugOutput) {
                        Msg("[Source] Normal map already exists (skipped): %s\n", path.c_str());
                    }
                } else {
                    // File doesn't exist - do the expensive work
                    ConvertedTexture normalTex;
                    normalTex.isNormalMap = true;
                    
                    if (ctx.extractPixelData(fileData, header, normalTex, false)) {
                        // Handle SSBump conversion
                        if (props.isSSBump) {
                            ctx.convertSSBumpToNormal(normalTex);
                        }
                        
                        ctx.convertToOctahedral(normalTex);
                        
                        if (ctx.writeDDS(normalTex, path)) {
                            result.normalPath = path;
                            if (ctx.materialsWithNormals) (*ctx.materialsWithNormals)++;
                            if (ctx.debugOutput) Msg("[Source] Wrote normal: %s\n", path.c_str());
                        }
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
    bool isPhongMask = false;
    
    // =========================================================================
    // PHONG MATERIAL PATH - prioritize phong-specific properties
    // Default Source Engine behavior: phong uses $bumpmap alpha as mask
    // Override with $basemapalphaphongmask to use $basetexture alpha instead
    // =========================================================================
    if (props.hasPhong) {
        // Priority 1: $phongexponenttexture (best quality - dedicated roughness data)
        if (props.hasPhongExponentTexture && !props.phongExponentTexturePath.empty()) {
            vtfPath = props.phongExponentTexturePath;
            useAlphaChannel = false;
            isPhongExponentTexture = true;
            if (ctx.debugOutput) Msg("[Source] [PHONG] Using $phongexponenttexture (best quality)\n");
        }
        // Priority 2: $basemapalphaphongmask - OVERRIDE to use base texture alpha as phong mask
        else if (props.hasBaseMapAlphaPhongMask && !props.baseTexturePath.empty()) {
            vtfPath = props.baseTexturePath;
            useAlphaChannel = true;
            isPhongMask = true;
            if (ctx.debugOutput) Msg("[Source] [PHONG] Using base texture alpha ($basemapalphaphongmask)\n");
        }
        // Priority 3: $normalmapalphaenvmapmask - explicit normal map alpha mask parameter
        else if (props.normalMapAlphaEnvMapMask && props.hasBumpMap && !props.bumpMapPath.empty()) {
            vtfPath = props.bumpMapPath;
            useAlphaChannel = true;
            isPhongMask = true;
            if (ctx.debugOutput) Msg("[Source] [PHONG] Using normal map alpha ($normalmapalphaenvmapmask)\n");
        }
        // Priority 4: DEFAULT Source Engine behavior - phong + bumpmap = bumpmap alpha is phong mask
        else if (props.hasBumpMap && !props.bumpMapPath.empty()) {
            vtfPath = props.bumpMapPath;
            useAlphaChannel = true;
            isPhongMask = true;
            if (ctx.debugOutput) Msg("[Source] [PHONG] Using bumpmap alpha (default phong behavior)\n");
        }
        // Priority 5: No bumpmap available - use constant $phongexponent
        else {
            if (ctx.debugOutput) Msg("[Source] [PHONG] No bumpmap found, using constant $phongexponent\n");
        }
    }
    
    // =========================================================================
    // NON-PHONG / FALLBACK PATH - use envmap-based properties
    // Only use these fallbacks if the material doesn't have phong enabled
    // If phong is enabled but no mask texture was found, use the constant phong exponent
    // =========================================================================
    if (vtfPath.empty() && !props.hasPhong) {
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
                // Check if output file already exists BEFORE expensive decompression
                uint64_t hash = ctx.generateHash(vtfPath + "_rough", header.width, header.height);
                std::string path = ctx.generateOutputPath(hash, "_roughness");
                
                if (ctx.fileExists(path)) {
                    // File already exists - skip all processing
                    result.roughnessPath = path;
                    result.skippedCount++;
                    hasRoughnessTexture = true;
                    if (ctx.debugOutput) {
                        Msg("[Source] Roughness texture already exists (skipped): %s\n", path.c_str());
                    }
                } else {
                    // File doesn't exist - do the expensive work
                    ConvertedTexture sourceTex;
                    if (ctx.extractPixelData(fileData, header, sourceTex, false)) {
                        hasRoughnessTexture = GenerateRoughnessFromSource(
                            sourceTex, useAlphaChannel, isPhongExponentTexture, isInvertedMask, isPhongMask,
                            props.phongExponent, vtfPath, ctx, result);
                        
                        if (hasRoughnessTexture && ctx.debugOutput) {
                            Msg("[Source] Generated roughness texture from: %s\n", vtfPath.c_str());
                        } else if (ctx.debugOutput) {
                            Msg("[Source] Failed to generate roughness texture (no variation or error)\n");
                        }
                    }
                }
            }
        }
    }
    
    // For phong materials that failed to generate a texture, ensure we use the constant
    if (!hasRoughnessTexture && props.hasPhong && ctx.debugOutput) {
        Msg("[Source] [PHONG] Using constant roughness from $phongexponent: %.2f\n", props.roughness);
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
    // Metallic extraction from envmap mask + base texture brightness
    // This is the key innovation: in Source, dark textures with envmap look
    // metallic. We extract per-pixel metallic by combining envmap mask with
    // base texture darkness, and modify the albedo to compensate for PBR.
    // 
    // This feature is gated behind the metallicGenerationEnabled flag as it's
    // experimental and may not produce correct results for all materials.
    //
    // Envmap mask sources (in priority order):
    //   1. $envmapmask - separate texture file (red channel)
    //   2. $normalmapalphaenvmapmask - normal map alpha channel
    //   3. $basealphaenvmapmask - base texture alpha channel
    //   4. No mask but $envmap is set - treat as full-strength envmap everywhere
    // =========================================================================
    bool hasMetallicTexture = false;
    
    // Only attempt metallic extraction if:
    // 1. Metallic generation is enabled (experimental feature flag)
    // 2. We have a base texture to analyze
    // 3. Material has envmap enabled
    // Note: We now also support materials with just $envmap (no mask) - full-strength envmap everywhere
    bool hasAnyEnvmapMask = (props.hasEnvMapMask && !props.envMapMaskPath.empty()) ||
                            props.normalMapAlphaEnvMapMask ||
                            props.hasBaseAlphaEnvMapMask;
    bool hasEnvmapNoMask = props.hasEnvMap && !hasAnyEnvmapMask;  // $envmap but no mask
    
    if (ctx.metallicGenerationEnabled &&
        !props.baseTexturePath.empty() && props.hasEnvMap) {
        
        if (ctx.debugOutput) {
            Msg("[Source] [Metallic] Attempting metallic extraction:\n");
            Msg("[Source] [Metallic]   Base texture: %s\n", props.baseTexturePath.c_str());
            Msg("[Source] [Metallic]   hasEnvMapTint=%d, envMapTint=[%.2f, %.2f, %.2f]\n",
                props.hasEnvMapTint, props.envMapTint[0], props.envMapTint[1], props.envMapTint[2]);
            if (props.hasEnvMapMask && !props.envMapMaskPath.empty()) {
                Msg("[Source] [Metallic]   Envmap mask source: $envmapmask (%s)\n", props.envMapMaskPath.c_str());
            } else if (props.normalMapAlphaEnvMapMask && props.hasBumpMap) {
                Msg("[Source] [Metallic]   Envmap mask source: $normalmapalphaenvmapmask (normal map alpha)\n");
            } else if (props.hasBaseAlphaEnvMapMask) {
                Msg("[Source] [Metallic]   Envmap mask source: $basealphaenvmapmask (base texture alpha - INVERTED)\n");
            } else if (hasEnvmapNoMask) {
                Msg("[Source] [Metallic]   Envmap mask source: NONE ($envmap only - full-strength everywhere)\n");
            }
        }
        
        // Read base texture (always needed)
        std::vector<uint8_t> baseFileData;
        if (ctx.readVTFFile(props.baseTexturePath, baseFileData)) {
            VTFFileHeader baseHeader;
            if (ctx.parseVTFHeader(baseFileData, baseHeader)) {
                ConvertedTexture baseTex;
                if (ctx.extractPixelData(baseFileData, baseHeader, baseTex, false)) {
                    
                    // Determine envmap mask source and load appropriate texture
                    std::string maskTexturePath;
                    EnvmapMaskSource maskSource = EnvmapMaskSource::RedChannel;
                    bool useFullStrengthEnvmap = false;
                    bool invertMask = false;  // $basealphaenvmapmask needs inversion
                    
                    if (props.hasEnvMapMask && !props.envMapMaskPath.empty()) {
                        // Priority 1: Separate $envmapmask texture (use red channel)
                        maskTexturePath = props.envMapMaskPath;
                        maskSource = EnvmapMaskSource::RedChannel;
                        invertMask = false;
                    } else if (props.normalMapAlphaEnvMapMask && props.hasBumpMap && !props.bumpMapPath.empty()) {
                        // Priority 2: Normal map alpha ($normalmapalphaenvmapmask)
                        maskTexturePath = props.bumpMapPath;
                        maskSource = EnvmapMaskSource::AlphaChannel;
                        invertMask = false;
                    } else if (props.hasBaseAlphaEnvMapMask) {
                        // Priority 3: Base texture alpha ($basealphaenvmapmask) - INVERTED!
                        // In Source: transparent (0) = reflective, opaque (255) = matte
                        maskTexturePath = props.baseTexturePath;
                        maskSource = EnvmapMaskSource::AlphaChannel;
                        invertMask = true;
                    } else if (hasEnvmapNoMask) {
                        // Priority 4: No mask but $envmap is set - full-strength envmap everywhere
                        useFullStrengthEnvmap = true;
                        invertMask = false;
                    }
                    
                    if (!maskTexturePath.empty() || useFullStrengthEnvmap) {
                        // Read mask texture (may be same as base texture for basealphaenvmapmask)
                        ConvertedTexture separateMaskTex;  // Only used when loading separate texture
                        ConvertedTexture fullStrengthMask; // Used when no mask - all white (255)
                        const ConvertedTexture* maskTexPtr = nullptr;
                        
                        if (useFullStrengthEnvmap) {
                            // Create a full-strength mask (all white = 255)
                            fullStrengthMask.width = baseTex.width;
                            fullStrengthMask.height = baseTex.height;
                            fullStrengthMask.pixelData.resize(baseTex.width * baseTex.height * 4, 255);
                            maskTexPtr = &fullStrengthMask;
                            maskSource = EnvmapMaskSource::RedChannel;
                        } else if (maskTexturePath == props.baseTexturePath) {
                            // Reuse already loaded base texture (no copy needed)
                            // But check if the alpha channel has variation when using $basealphaenvmapmask
                            if (invertMask && maskSource == EnvmapMaskSource::AlphaChannel && 
                                !HasAlphaVariation(baseTex.pixelData)) {
                                // Alpha has no variation - the material author set $basealphaenvmapmask
                                // but the texture has solid alpha (probably 255).
                                // After inversion this would be all 0 (no envmap), which is wrong.
                                // Fall back to full-strength envmap - they clearly wanted reflections.
                                if (ctx.debugOutput) {
                                    Msg("[Source] [Metallic] $basealphaenvmapmask: Alpha has no variation, using full-strength envmap\n");
                                }
                                fullStrengthMask.width = baseTex.width;
                                fullStrengthMask.height = baseTex.height;
                                fullStrengthMask.pixelData.resize(baseTex.width * baseTex.height * 4, 255);
                                maskTexPtr = &fullStrengthMask;
                                maskSource = EnvmapMaskSource::RedChannel;
                                invertMask = false;
                            } else {
                                maskTexPtr = &baseTex;
                            }
                        } else {
                            // Load separate mask texture
                            std::vector<uint8_t> maskFileData;
                            if (ctx.readVTFFile(maskTexturePath, maskFileData)) {
                                VTFFileHeader maskHeader;
                                if (ctx.parseVTFHeader(maskFileData, maskHeader)) {
                                    if (ctx.extractPixelData(maskFileData, maskHeader, separateMaskTex, false)) {
                                        maskTexPtr = &separateMaskTex;
                                    }
                                }
                            }
                        }
                        
                        if (maskTexPtr) {
                            // Generate metallic and modified albedo
                            MetallicExtractionResult metallicResult = 
                                GenerateMetallicFromEnvmapMaskAndBrightness(
                                    baseTex, *maskTexPtr, maskSource, invertMask, props, ctx, result);
                            
                            hasMetallicTexture = metallicResult.hasMetallic;
                            
                            if (metallicResult.hasMetallic && ctx.debugOutput) {
                                Msg("[Source] [Metallic] Successfully extracted metallic from envmap mask + brightness\n");
                            }
                            if (metallicResult.hasModifiedAlbedo && ctx.debugOutput) {
                                Msg("[Source] [Metallic] Generated modified albedo for proper PBR rendering\n");
                            }
                        } else if (ctx.debugOutput) {
                            Msg("[Source] [Metallic] Failed to load envmap mask texture: %s\n", maskTexturePath.c_str());
                        }
                    }
                }
            }
        }
    }
    
    // If we generated a metallic texture, clear the constant (texture takes precedence)
    if (hasMetallicTexture) {
        result.metallicConstant = 0.0f;
    }
    
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
                // Check if output file already exists BEFORE expensive decompression
                uint64_t hash = ctx.generateHash(heightMapPath + "_height", header.width, header.height);
                std::string path = ctx.generateOutputPath(hash, "_height");
                
                if (ctx.fileExists(path)) {
                    // File already exists - skip all processing
                    result.heightPath = path;
                    result.heightScale = props.hasParallaxMapScale ? props.parallaxMapScale : 0.025f;
                    result.skippedCount++;
                    if (ctx.debugOutput) {
                        Msg("[Source] Height map already exists (skipped): %s\n", path.c_str());
                    }
                } else {
                    // File doesn't exist - do the expensive work
                    ConvertedTexture heightTex;
                    if (ctx.extractPixelData(fileData, header, heightTex, false)) {
                        if (ctx.writeDDS(heightTex, path)) {
                            result.heightPath = path;
                            result.heightScale = props.hasParallaxMapScale ? props.parallaxMapScale : 0.025f;
                            if (ctx.debugOutput) Msg("[Source] Wrote height: %s\n", path.c_str());
                        }
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
