// =========================================================================
// Legacy Texture Processor - BlueFlyTrap PseudoPBR Format Handler
// =========================================================================
// BlueFlyTrap PseudoPBR is a technique that encodes PBR properties into
// Source Engine's standard phong workflow. Developed by BlueFlytrap/Galaxyi.
//
// Uses stacked model layers to approximate PBR rendering:
//   Stack 1 - Base (dielectric): Phong + envmap with dielectric fresnel
//   Stack 2 - Metallic: $translucent + $phongalbedotint for metal
//   Stack 3-5 - EnvR/G/B: Colored environment reflections
//
// Detection markers:
//   - VertexlitGeneric shader
//   - $phongexponenttexture present (contains inverted roughness)
//   - $color2 "[0 0 0]" (black for stacking)
//   - High $phongboost (3-20 range)
//   - Characteristic $phongfresnelranges patterns
//
// Roughness encoding in $phongexponenttexture:
//   Original: gloss -> levels(middle=0.24) -> red channel
//   Reverse: roughness = 1.0 - pow(value/255, 0.24)
//
// Metallic detection:
//   Metallic layer has $translucent "1" + $phongalbedotint "1"
// =========================================================================

#ifdef _WIN64

#include "legacy_texture_processor_formats.h"
#include <tier0/dbg.h>
#include <algorithm>
#include <cstdlib>
#include <cmath>

namespace LegacyTextureProcessor {

// Shared utilities (defined in formats.cpp)
bool ParseVector3(const std::string& str, float& r, float& g, float& b);
bool ApproxEqual(float a, float b, float epsilon);

namespace BFTPseudoPBR {

// =========================================================================
// Detection
// =========================================================================

bool Detect(const VMTParseResult& vmt) {
    std::string shaderLower = vmt.shaderName;
    std::transform(shaderLower.begin(), shaderLower.end(), shaderLower.begin(), ::tolower);
    
    // Must be VertexlitGeneric with $phongexponenttexture
    if (shaderLower != "vertexlitgeneric") return false;
    
    std::string expTex = vmt.findValue("$phongexponenttexture");
    if (expTex.empty()) return false;
    
    bool hasMarker = false;
    
    // Check for $color2 black/grey (stacking marker)
    std::string color2 = vmt.findValue("$color2");
    if (!color2.empty()) {
        float r, g, b;
        if (ParseVector3(color2, r, g, b)) {
            // Black (base) or mid-grey (metallic layer)
            if ((r < 0.1f && g < 0.1f && b < 0.1f) ||
                (r >= 0.4f && r <= 0.6f && g >= 0.4f && g <= 0.6f && b >= 0.4f && b <= 0.6f)) {
                hasMarker = true;
            }
        }
    }
    
    // Check for high phongboost (BFT uses 3-25)
    std::string boost = vmt.findValue("$phongboost");
    if (!boost.empty()) {
        float val = static_cast<float>(atof(boost.c_str()));
        if (val >= 3.0f && val <= 25.0f) hasMarker = true;
    }
    
    // Check characteristic fresnel ranges
    std::string fresnel = vmt.findValue("$phongfresnelranges");
    if (!fresnel.empty()) {
        float f1, f2, f3;
        if (ParseVector3(fresnel, f1, f2, f3)) {
            // Metallic: [0.87 0.9 1.0]
            bool metallic = ApproxEqual(f1, 0.87f, 0.1f) && ApproxEqual(f2, 0.9f, 0.1f) && ApproxEqual(f3, 1.0f, 0.1f);
            // Dielectric: [0.05 0.115 0.945] or similar
            bool dielectric = (f1 < 0.2f) && (f2 < 0.3f) && (f3 > 0.8f);
            if (metallic || dielectric) hasMarker = true;
        }
    }
    
    // Check for method comments in VMT
    if (vmt.contentLower.find("blueflytrap") != std::string::npos ||
        vmt.contentLower.find("pseudo pbr") != std::string::npos ||
        vmt.contentLower.find("pbr method") != std::string::npos) {
        hasMarker = true;
    }
    
    return hasMarker;
}

// =========================================================================
// Property Extraction
// =========================================================================

void ExtractProperties(const VMTParseResult& vmt, MaterialPBRProperties& props) {
    props.isBFTPseudoPBR = true;
    
    // Get the exponent texture path
    std::string expTex = vmt.findValue("$phongexponenttexture");
    if (!expTex.empty()) {
        props.bftExponentTexturePath = expTex;
        props.hasBFTExponentTexture = true;
    }
    
    // Detect if this is the metallic layer
    // Metallic layer has: $translucent "1" + $phongalbedotint "1"
    std::string translucent = vmt.findValue("$translucent");
    std::string albedoTint = vmt.findValue("$phongalbedotint");
    
    bool isTranslucent = !translucent.empty() && atoi(translucent.c_str()) == 1;
    bool hasAlbedoTint = !albedoTint.empty() && atoi(albedoTint.c_str()) == 1;
    
    props.isBFTMetallicLayer = isTranslucent && hasAlbedoTint;
    
    // Parse $color2 for metallic albedo reconstruction
    // In BFT, $color2 is used to darken the texture: [0 0 0] = full black, [0.5 0.5 0.5] = half
    // We need to boost the albedo to undo this darkening for proper PBR
    std::string color2 = vmt.findValue("$color2");
    if (!color2.empty()) {
        float r, g, b;
        if (ParseVector3(color2, r, g, b)) {
            props.bftColor2[0] = r;
            props.bftColor2[1] = g;
            props.bftColor2[2] = b;
            props.hasBFTColor2 = true;
        }
    }
    
    // Set metallic based on layer type
    if (props.isBFTMetallicLayer) {
        props.metallic = 0.9f;  // High metallic
    } else {
        props.metallic = 0.0f;  // Dielectric
    }
    
    // Roughness will come from exponent texture
    props.roughness = 0.5f;
}

// =========================================================================
// Texture Processing
// =========================================================================

// Convert BFT exponent value to roughness
static float ExponentToRoughness(uint8_t expValue) {
    // BFT encoding: gloss -> levels(middle=0.24) -> texture
    // Reverse: normalize -> pow(0.24) -> invert
    float normalized = static_cast<float>(expValue) / 255.0f;
    float gloss = std::pow(normalized, 0.24f);
    float roughness = 1.0f - gloss;
    
    // Clamp to valid PBR range
    if (roughness < 0.04f) roughness = 0.04f;
    if (roughness > 1.0f) roughness = 1.0f;
    
    return roughness;
}

ProcessedMaterial ProcessTextures(const MaterialPBRProperties& props,
                                   uint64_t textureHash,
                                   const ProcessingContext& ctx) {
    ProcessedMaterial result;
    
    if (ctx.debugOutput) {
        Msg("[BFT] Processing material: %s (%s layer)\n", 
            props.materialName.c_str(), 
            props.isBFTMetallicLayer ? "metallic" : "base");
        if (props.hasBFTColor2) {
            Msg("[BFT] $color2: [%.2f %.2f %.2f]\n", 
                props.bftColor2[0], props.bftColor2[1], props.bftColor2[2]);
        }
    }
    
    // =========================================================================
    // METALLIC ALBEDO RECONSTRUCTION
    // =========================================================================
    // In BFT PseudoPBR, metallic surfaces use $color2 "[0 0 0]" to darken the 
    // albedo. The phong reflections provide the metallic sheen. For proper PBR,
    // we need to brighten the albedo to recover the original metal color.
    //
    // The boost factor depends on how dark $color2 is:
    //   $color2 "[0 0 0]" → Albedo is black, need maximum boost (typically 2-4x)
    //   $color2 "[0.5 0.5 0.5]" → Albedo is half brightness, need 2x boost
    //
    // We also use the base texture's alpha channel if it contains a metallic mask.
    // =========================================================================
    if (props.isBFTMetallicLayer && !props.baseTexturePath.empty()) {
        std::vector<uint8_t> fileData;
        if (ctx.readVTFFile(props.baseTexturePath, fileData)) {
            VTFFileHeader header;
            if (ctx.parseVTFHeader(fileData, header)) {
                ConvertedTexture baseTex;
                baseTex.isNormalMap = false;
                
                if (ctx.extractPixelData(fileData, header, baseTex, false)) {
                    // Calculate boost factor based on $color2
                    // If $color2 is dark, the original texture was multiplied by that dark color
                    // To recover: boost = 1.0 / color2 (clamped to reasonable range)
                    float boostR = 2.5f;  // Default boost for metallic recovery
                    float boostG = 2.5f;
                    float boostB = 2.5f;
                    
                    if (props.hasBFTColor2) {
                        // Calculate inverse of $color2 to undo the darkening
                        // Clamp minimum to avoid division by zero, max boost ~4x
                        float c2r = props.bftColor2[0];
                        float c2g = props.bftColor2[1];
                        float c2b = props.bftColor2[2];
                        
                        if (c2r < 0.1f && c2g < 0.1f && c2b < 0.1f) {
                            // Full black $color2 - use standard metallic boost
                            boostR = boostG = boostB = 2.5f;
                        } else {
                            // Partial darkening - calculate inverse
                            if (c2r > 0.1f) boostR = 1.0f / c2r;
                            if (c2g > 0.1f) boostG = 1.0f / c2g;
                            if (c2b > 0.1f) boostB = 1.0f / c2b;
                            
                            // Clamp to reasonable range (1x to 4x boost)
                            boostR = min(4.0f, max(1.0f, boostR));
                            boostG = min(4.0f, max(1.0f, boostG));
                            boostB = min(4.0f, max(1.0f, boostB));
                        }
                    }
                    
                    if (ctx.debugOutput) {
                        Msg("[BFT] Applying metallic albedo boost: [%.2f %.2f %.2f]\n", 
                            boostR, boostG, boostB);
                    }
                    
                    // Create boosted albedo texture
                    ConvertedTexture albedoTex;
                    albedoTex.width = baseTex.width;
                    albedoTex.height = baseTex.height;
                    albedoTex.mipLevels = 1;
                    albedoTex.format = REMIXAPI_FORMAT_R8G8B8A8_UNORM;
                    albedoTex.isNormalMap = false;
                    albedoTex.pixelData.resize(baseTex.width * baseTex.height * 4);
                    
                    for (uint32_t i = 0; i < baseTex.width * baseTex.height; i++) {
                        // Read original pixel
                        float r = static_cast<float>(baseTex.pixelData[i * 4 + 0]) / 255.0f;
                        float g = static_cast<float>(baseTex.pixelData[i * 4 + 1]) / 255.0f;
                        float b = static_cast<float>(baseTex.pixelData[i * 4 + 2]) / 255.0f;
                        uint8_t a = baseTex.pixelData[i * 4 + 3];
                        
                        // Apply boost (metals reflect their color, so boosting dark metals reveals their true color)
                        r = min(1.0f, r * boostR);
                        g = min(1.0f, g * boostG);
                        b = min(1.0f, b * boostB);
                        
                        // Apply a slight saturation boost for metals (they should have some color)
                        // This helps recover gold, copper, bronze tones from nearly-black textures
                        float luminance = 0.2126f * r + 0.7152f * g + 0.0722f * b;
                        float satBoost = 1.2f;
                        r = min(1.0f, luminance + (r - luminance) * satBoost);
                        g = min(1.0f, luminance + (g - luminance) * satBoost);
                        b = min(1.0f, luminance + (b - luminance) * satBoost);
                        
                        // Ensure minimum brightness for metals (no true black metals exist)
                        float minMetal = 0.3f;
                        if (r < minMetal && g < minMetal && b < minMetal) {
                            // Scale up to minimum brightness while preserving hue
                            float maxC = max(r, max(g, b));
                            if (maxC > 0.01f) {
                                float scale = minMetal / maxC;
                                r *= scale;
                                g *= scale;
                                b *= scale;
                            } else {
                                // Pure black - default to neutral metal (like chrome/silver)
                                r = g = b = 0.7f;
                            }
                        }
                        
                        albedoTex.pixelData[i * 4 + 0] = static_cast<uint8_t>(r * 255.0f);
                        albedoTex.pixelData[i * 4 + 1] = static_cast<uint8_t>(g * 255.0f);
                        albedoTex.pixelData[i * 4 + 2] = static_cast<uint8_t>(b * 255.0f);
                        albedoTex.pixelData[i * 4 + 3] = a;  // Preserve alpha
                    }
                    
                    // Write the boosted albedo
                    uint64_t hash = ctx.generateHash(props.baseTexturePath + "_bft_albedo", albedoTex.width, albedoTex.height);
                    std::string path = ctx.generateOutputPath(hash, "_albedo");
                    
                    if (ctx.fileExists(path)) {
                        result.albedoPath = path;
                        result.skippedCount++;
                    } else if (ctx.writeDDS(albedoTex, path)) {
                        result.albedoPath = path;
                        if (ctx.debugOutput) Msg("[BFT] Wrote reconstructed metallic albedo: %s\n", path.c_str());
                    }
                    
                    // Store the boost values in result for potential use in material override
                    result.albedoBoostR = boostR;
                    result.albedoBoostG = boostG;
                    result.albedoBoostB = boostB;
                    result.hasAlbedoBoost = true;
                }
            }
        }
    }
    
    // Process normal map (standard Source Engine bumpmap)
    if (props.hasBumpMap && !props.bumpMapPath.empty()) {
        std::vector<uint8_t> fileData;
        if (ctx.readVTFFile(props.bumpMapPath, fileData)) {
            VTFFileHeader header;
            if (ctx.parseVTFHeader(fileData, header)) {
                ConvertedTexture normalTex;
                normalTex.isNormalMap = true;
                
                if (ctx.extractPixelData(fileData, header, normalTex, false)) {
                    // Handle SSBump if needed
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
                        if (ctx.debugOutput) Msg("[BFT] Wrote normal: %s\n", path.c_str());
                    }
                }
            }
        }
    }
    
    // Process $phongexponenttexture to extract roughness
    if (props.hasBFTExponentTexture && !props.bftExponentTexturePath.empty()) {
        std::vector<uint8_t> fileData;
        if (ctx.readVTFFile(props.bftExponentTexturePath, fileData)) {
            VTFFileHeader header;
            if (ctx.parseVTFHeader(fileData, header)) {
                ConvertedTexture expTex;
                expTex.isNormalMap = false;
                
                if (ctx.extractPixelData(fileData, header, expTex, false)) {
                    // Convert exponent texture to roughness
                    ConvertedTexture roughTex;
                    roughTex.width = expTex.width;
                    roughTex.height = expTex.height;
                    roughTex.mipLevels = 1;
                    roughTex.format = REMIXAPI_FORMAT_R8G8B8A8_UNORM;
                    roughTex.isNormalMap = false;
                    roughTex.pixelData.resize(expTex.width * expTex.height * 4);
                    
                    for (uint32_t i = 0; i < expTex.width * expTex.height; i++) {
                        // Red channel contains the encoded gloss
                        uint8_t expValue = expTex.pixelData[i * 4 + 0];
                        float roughness = ExponentToRoughness(expValue);
                        uint8_t roughByte = static_cast<uint8_t>(roughness * 255.0f);
                        
                        roughTex.pixelData[i * 4 + 0] = roughByte;
                        roughTex.pixelData[i * 4 + 1] = roughByte;
                        roughTex.pixelData[i * 4 + 2] = roughByte;
                        roughTex.pixelData[i * 4 + 3] = 255;
                    }
                    
                    uint64_t hash = ctx.generateHash(props.bftExponentTexturePath + "_rough", roughTex.width, roughTex.height);
                    std::string path = ctx.generateOutputPath(hash, "_roughness");
                    
                    if (ctx.fileExists(path)) {
                        result.roughnessPath = path;
                        result.skippedCount++;
                    } else if (ctx.writeDDS(roughTex, path)) {
                        result.roughnessPath = path;
                        if (ctx.materialsWithRoughness) (*ctx.materialsWithRoughness)++;
                        if (ctx.debugOutput) Msg("[BFT] Wrote roughness from exponent: %s\n", path.c_str());
                    }
                }
            }
        }
    }
    
    // Set metallic constant based on layer type
    result.metallicConstant = props.isBFTMetallicLayer ? 0.9f : 0.0f;
    
    result.success = true;
    if (ctx.debugOutput) {
        Msg("[BFT] Complete: %s (skipped %d existing)\n", props.materialName.c_str(), result.skippedCount);
    }
    
    return result;
}

} // namespace BFTPseudoPBR
} // namespace LegacyTextureProcessor

#endif // _WIN64
