// =========================================================================
// format_bftpseudopbr.cpp - BlueFlyTrap PseudoPBR Format Handler
// =========================================================================
// BlueFlyTrap PseudoPBR is a technique that encodes PBR properties into
// Source Engine's standard phong workflow. Developed by BlueFlytrap/Galaxyi.
//
// Uses stacked model layers to approximate PBR rendering:
//   Stack 1 - Base (dielectric): Phong + envmap with dielectric fresnel
//   Stack 2 - Metallic: $translucent + $phongalbedotint for metal
//   Stack 3-5 - EnvR/G/B: Colored environment reflections (_ch_r, _ch_g, _ch_b)
//
// Detection markers:
//   - VertexlitGeneric shader
//   - $phongexponenttexture present (contains inverted roughness, *_e suffix)
//   - $color2 "[0 0 0]" + $blendTintByBaseAlpha "1" (metallic mask in alpha)
//   - Characteristic $phongfresnelranges patterns
//   - "BlueFlytrap" or "PseudoPBR" comments in VMT
//
// BFT Texture Encoding (simpler than MWB):
//   Exponent (_e):
//     - Red: Inverted roughness (255 = smooth, 0 = rough) - LINEAR encoding
//   Albedo:
//     - Alpha: Metalness mask (where $blendTintByBaseAlpha darkens to black)
//
// Decoding (Source → PBR):
//   Roughness: 1.0 - (exponent_red / 255.0)  (simple linear inversion)
//   Metallic: From base texture alpha channel
//
// NOTE: MWB PBR Gen uses a DIFFERENT encoding (pow(gloss,4.0)) - see mwbpbr.cpp
// =========================================================================

#ifdef _WIN64

#include "formats.h"
#include <tier0/dbg.h>
#include <algorithm>
#include <cstdlib>
#include <cmath>

namespace MaterialPipeline {
namespace ToPBR {

// Shared utilities (defined in formats.cpp)
bool ParseVector3(const std::string& str, float& r, float& g, float& b);
bool ApproxEqual(float a, float b, float epsilon);

namespace BFTPseudoPBR {

// =========================================================================
// Detection Constants
// =========================================================================
// Brightness threshold for detecting dark $color2 values
// Used to identify BFT layer stacking patterns
// NOTE: This threshold must match cl_fix_bft_materials.lua for consistency!
static const float DARK_COLOR2_THRESHOLD = 0.3f;

// =========================================================================
// Channel Overlay Detection
// =========================================================================
// BFT materials may have channel overlay layers (_ch, _ch_r, _ch_g, _ch_b)
// These are additive glow layers that use $selfillum and colored $envmaptint.
// In RTX they appear as white overlaps and should be hidden.

bool IsChannelOverlayMaterial(const VMTParseResult& vmt, const std::string& materialPath) {
    // Check if material name ends with channel suffixes
    std::string pathLower = materialPath;
    std::transform(pathLower.begin(), pathLower.end(), pathLower.begin(), ::tolower);
    
    bool hasChannelSuffix = 
        pathLower.length() >= 3 && pathLower.substr(pathLower.length() - 3) == "_ch" ||
        pathLower.length() >= 5 && pathLower.substr(pathLower.length() - 5) == "_ch_r" ||
        pathLower.length() >= 5 && pathLower.substr(pathLower.length() - 5) == "_ch_g" ||
        pathLower.length() >= 5 && pathLower.substr(pathLower.length() - 5) == "_ch_b";
    
    if (!hasChannelSuffix) return false;
    
    // Verify it has BFT channel overlay characteristics
    std::string additive = vmt.findValue("$additive");
    std::string selfillum = vmt.findValue("$selfillum");
    
    bool isAdditive = !additive.empty() && atoi(additive.c_str()) == 1;
    bool hasSelfillum = !selfillum.empty() && atoi(selfillum.c_str()) == 1;
    
    return hasChannelSuffix && (isAdditive || hasSelfillum);
}

// =========================================================================
// Detection
// =========================================================================
// BFT materials often have predictable naming:
//   base_material      - Base/diffuse layer (has $blendTintByBaseAlpha)
//   base_material_metal - Metallic layer (has $phongalbedotint, $additive)
//   base_material_e     - Exponent/roughness texture (key detection marker)
//   base_material_n     - Normal map
//   base_material_ch*   - Channel overlays (should be hidden)

bool Detect(const VMTParseResult& vmt) {
    std::string shaderLower = vmt.shaderName;
    std::transform(shaderLower.begin(), shaderLower.end(), shaderLower.begin(), ::tolower);
    
    // Must be VertexlitGeneric
    if (shaderLower != "vertexlitgeneric") return false;
    
    // Check for $phongexponenttexture (core BFT requirement)
    std::string expTex = vmt.findValue("$phongexponenttexture");
    if (expTex.empty()) return false;
    
    bool hasMarker = false;
    
    // =========================================================================
    // Suffix-based detection (no comments needed)
    // =========================================================================
    // Check if exponent texture follows BFT naming convention (*_e)
    std::string expLower = expTex;
    std::transform(expLower.begin(), expLower.end(), expLower.begin(), ::tolower);
    if (expLower.length() >= 2 && expLower.substr(expLower.length() - 2) == "_e") {
        hasMarker = true;
    }
    
    // =========================================================================
    // Pattern-based detection (original logic)
    // =========================================================================
    
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
    
    // =========================================================================
    // Strong BFT marker: $blendTintByBaseAlpha with dark $color2
    // =========================================================================
    // This is the PRIMARY detection pattern for BFT base/diffuse layers.
    // When $blendTintByBaseAlpha "1" + $color2 "[0 0 0]" is used, the engine
    // darkens the texture based on the alpha channel. This is BFT's method
    // for encoding the metallic mask in the base texture alpha.
    //
    // This pattern is so specific to BFT that we can treat it as a definitive marker.
    std::string blendTint = vmt.findValue("$blendtintbybasealpha");
    if (!blendTint.empty() && atoi(blendTint.c_str()) == 1) {
        // If blendTintByBaseAlpha is used with a dark color2, this is BFT
        if (!color2.empty()) {
            float r, g, b;
            if (ParseVector3(color2, r, g, b)) {
                if ((r + g + b) < DARK_COLOR2_THRESHOLD) {
                    // This is a VERY strong indicator - BFT base layer
                    return true;  // Skip other checks, this is definitely BFT
                }
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
    // NOTE: MWB-specific markers are handled by MWBPBR::Detect() which runs first
    if (vmt.contentLower.find("blueflytrap") != std::string::npos ||
        vmt.contentLower.find("pseudo pbr") != std::string::npos ||
        vmt.contentLower.find("pbr method") != std::string::npos ||
        vmt.contentLower.find("diffuse texture") != std::string::npos ||  // "1/3 Diffuse Texture"
        vmt.contentLower.find("metals texture") != std::string::npos) {   // "2/3 Metals Texture"
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
    
    // Detect if this is the metallic layer (stack 2/3)
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
    
    // =========================================================================
    // Detect $blendTintByBaseAlpha pattern (BFT Diffuse/Base layer)
    // =========================================================================
    // When $blendTintByBaseAlpha "1" + $color2 "[0 0 0]" is used, the engine
    // tints the albedo texture's white areas (where alpha > 0) to black.
    // 
    // This pattern indicates:
    // 1. The base texture alpha channel IS the metallic mask!
    // 2. The albedo would appear black without a runtime fix
    // 3. We need to extract metallic from alpha and disable the tinting via Lua
    std::string blendTint = vmt.findValue("$blendtintbybasealpha");
    bool hasBlendTint = !blendTint.empty() && atoi(blendTint.c_str()) == 1;
    
    props.hasBFTBlendTintByBaseAlpha = hasBlendTint;
    
    // If we have blendTintByBaseAlpha + dark color2, this is a BFT diffuse layer
    // where the alpha channel stores the metallic mask
    if (hasBlendTint && props.hasBFTColor2) {
        float brightness = props.bftColor2[0] + props.bftColor2[1] + props.bftColor2[2];
        if (brightness < DARK_COLOR2_THRESHOLD) {  // Dark color2 = tinting to black
            props.isBFTDiffuseLayer = true;
        }
    }
    
    // Set metallic based on layer type
    if (props.isBFTMetallicLayer) {
        props.metallic = 0.9f;  // High metallic (explicit metallic layer)
    } else if (props.isBFTDiffuseLayer) {
        props.metallic = 0.5f;  // Variable - will come from alpha channel
    } else {
        props.metallic = 0.0f;  // Dielectric
    }
    
    // Roughness will come from exponent texture
    props.roughness = 0.5f;
}

// =========================================================================
// Texture Processing
// =========================================================================

// =========================================================================
// BFT Roughness Decoding from Exponent Texture
// =========================================================================
// BlueFlyTrap encoding for roughness:
//   - BFT encodes gloss using a levels adjustment with middle point around 0.24
//   - Exponent red channel stores encoded gloss value (255 = smooth, 0 = rough)
//
// To decode back to roughness:
//   1. Normalize: value / 255.0
//   2. Apply inverse gamma: pow(normalized, 0.24)
//   3. Invert to get roughness: 1.0 - gloss
//
// NOTE: This is DIFFERENT from MWB PBR Gen which uses pow(gloss, 4.0) encoding.
// MWB materials are handled by MWBPBR::ProcessTextures() instead.
// =========================================================================

static float ExponentToRoughness(uint8_t expValue) {
    // Normalize to 0-1 range
    float normalized = static_cast<float>(expValue) / 255.0f;
    
    // BFT encoding: gloss -> levels(middle=0.24) -> texture
    // Reverse: normalize -> pow(0.24) -> invert
    float gloss = std::pow(normalized, 0.24f);
    float roughness = 1.0f - gloss;
    
    // Clamp to valid PBR range
    if (roughness < 0.04f) roughness = 0.04f;
    if (roughness > 1.0f) roughness = 1.0f;
    
    return roughness;
}

// =========================================================================
// Metallic Albedo Reconstruction Constants
// =========================================================================
// NOTE: Using min()/max() instead of std::min()/std::max() for Windows
// compatibility. Windows headers define min/max as macros which conflict
// with std:: versions. The unqualified versions work correctly.

// Default boost factor for recovering metallic color from darkened textures
static const float DEFAULT_METALLIC_BOOST = 2.5f;
// Maximum allowed boost to avoid over-brightening
static const float MAX_METALLIC_BOOST = 4.0f;
// Saturation enhancement for metals (helps recover gold/copper tones)
static const float METALLIC_SATURATION_BOOST = 1.2f;
// Minimum brightness for metals (no true black metals in PBR)
static const float MIN_METALLIC_BRIGHTNESS = 0.3f;
// Default fallback color for pure black metals (neutral chrome/silver)
static const float DEFAULT_NEUTRAL_METAL = 0.7f;

// Minimum difference between min/max alpha values to consider the alpha
// channel as having meaningful metallic information. Values below this
// threshold are treated as uniform (no metallic variation).
// 10/255 = ~4% variation threshold - filters out compression artifacts
static const uint8_t ALPHA_VARIATION_THRESHOLD = 10;

// Sampling step for alpha variation check. Lower = more accurate but slower.
// 8 provides good balance between speed and catching small metallic details.
static const uint32_t ALPHA_SAMPLE_STEP = 8;

ProcessedMaterial ProcessTextures(const MaterialPBRProperties& props,
                                   uint64_t textureHash,
                                   const ProcessingContext& ctx) {
    ProcessedMaterial result;
    
    if (ctx.debugOutput) {
        Msg("[BFT] Processing material: %s (%s layer)\n", 
            props.materialName.c_str(), 
            props.isBFTMetallicLayer ? "metallic" : 
            props.isBFTDiffuseLayer ? "diffuse (alpha=metallic)" : "base");
        if (props.hasBFTColor2) {
            Msg("[BFT] $color2: [%.2f %.2f %.2f]\n", 
                props.bftColor2[0], props.bftColor2[1], props.bftColor2[2]);
        }
        if (props.hasBFTBlendTintByBaseAlpha) {
            Msg("[BFT] $blendTintByBaseAlpha detected - alpha channel is metallic mask\n");
        }
    }
    
    // =========================================================================
    // BFT METALLIC EXTRACTION FROM BASE TEXTURE ALPHA
    // =========================================================================
    // BFT materials use stacked VMT layers that share the same base texture.
    // The base texture alpha channel stores the metallic mask. However, which 
    // layer (diffuse vs metallic) gets processed first is unpredictable.
    //
    // Solution: ALWAYS extract metallic from base texture alpha for ANY BFT
    // material. The alpha channel consistently stores the metallic mask across
    // all layers. This ensures we get the metallic map regardless of which
    // layer was processed.
    //
    // The runtime Lua fix (cl_fix_bft_materials.lua) handles disabling the
    // $blendTintByBaseAlpha tinting so the albedo displays correctly.
    if (!props.baseTexturePath.empty()) {
        std::vector<uint8_t> fileData;
        if (ctx.readVTFFile(props.baseTexturePath, fileData)) {
            VTFFileHeader header;
            if (ctx.parseVTFHeader(fileData, header)) {
                ConvertedTexture baseTex;
                baseTex.isNormalMap = false;
                
                if (ctx.extractPixelData(fileData, header, baseTex, false)) {
                    // Verify we have valid pixel data before accessing
                    if (baseTex.pixelData.size() < 4) {
                        if (ctx.debugOutput) {
                            Msg("[BFT] Invalid pixel data size - skipping metallic extraction\n");
                        }
                    } else {
                        // Check if alpha channel has meaningful variation (not uniform like DXT1)
                        // If alpha is all 255 or all 0, there's no metallic information
                        uint8_t minAlpha = 255;
                        uint8_t maxAlpha = 0;
                        uint8_t firstAlpha = baseTex.pixelData[3];
                        bool hasVariation = false;
                        
                        // Sample pixels to check for variation
                        uint32_t totalPixels = baseTex.width * baseTex.height;
                        for (uint32_t i = 0; i < totalPixels && !hasVariation; i += ALPHA_SAMPLE_STEP) {
                            uint8_t a = baseTex.pixelData[i * 4 + 3];
                            if (a != firstAlpha) {
                                hasVariation = true;
                            }
                            if (a < minAlpha) minAlpha = a;
                            if (a > maxAlpha) maxAlpha = a;
                        }
                        
                        // Only extract metallic if alpha has meaningful variation
                        // (not just uniform 255 or 0)
                        if (hasVariation || (maxAlpha - minAlpha) > ALPHA_VARIATION_THRESHOLD) {
                            if (ctx.debugOutput) {
                                Msg("[BFT] Alpha channel has variation (min=%d, max=%d) - extracting metallic\n", 
                                    minAlpha, maxAlpha);
                            }
                            
                            // Create metallic texture from alpha channel
                            ConvertedTexture metallicTex;
                            metallicTex.width = baseTex.width;
                            metallicTex.height = baseTex.height;
                            metallicTex.mipLevels = 1;
                            metallicTex.format = REMIXAPI_FORMAT_R8G8B8A8_UNORM;
                            metallicTex.isNormalMap = false;
                            metallicTex.pixelData.resize(totalPixels * 4);
                            
                            for (uint32_t i = 0; i < totalPixels; i++) {
                                // Alpha channel = metallic value
                                uint8_t metallic = baseTex.pixelData[i * 4 + 3];
                                
                                metallicTex.pixelData[i * 4 + 0] = metallic;
                                metallicTex.pixelData[i * 4 + 1] = metallic;
                                metallicTex.pixelData[i * 4 + 2] = metallic;
                                metallicTex.pixelData[i * 4 + 3] = 255;
                            }
                            
                            std::string path = ctx.generateOutputPath(props.baseTexturePath, "_metallic");
                            
                            if (ctx.fileExists(path)) {
                                result.metallicPath = path;
                                result.skippedCount++;
                            } else if (ctx.writeDDS(metallicTex, path)) {
                                result.metallicPath = path;
                                if (ctx.debugOutput) Msg("[BFT] Extracted metallic from alpha: %s\n", path.c_str());
                            }
                        } else {
                            if (ctx.debugOutput) {
                                Msg("[BFT] Alpha channel is uniform (%d) - skipping metallic extraction\n", firstAlpha);
                            }
                        }
                    }
                }
            }
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
    //if (props.isBFTMetallicLayer && !props.baseTexturePath.empty()) {
    //    std::vector<uint8_t> fileData;
    //    if (ctx.readVTFFile(props.baseTexturePath, fileData)) {
    //        VTFFileHeader header;
    //        if (ctx.parseVTFHeader(fileData, header)) {
    //            ConvertedTexture baseTex;
    //            baseTex.isNormalMap = false;
    //            
    //            if (ctx.extractPixelData(fileData, header, baseTex, false)) {
    //                // Calculate boost factor based on $color2
    //                // If $color2 is dark, the original texture was multiplied by that dark color
    //                // To recover: boost = 1.0 / color2 (clamped to reasonable range)
    //                float boostR = DEFAULT_METALLIC_BOOST;
    //                float boostG = DEFAULT_METALLIC_BOOST;
    //                float boostB = DEFAULT_METALLIC_BOOST;
    //                
    //                if (props.hasBFTColor2) {
    //                    // Calculate inverse of $color2 to undo the darkening
    //                    float c2r = props.bftColor2[0];
    //                    float c2g = props.bftColor2[1];
    //                    float c2b = props.bftColor2[2];
    //                    
    //                    if (c2r < 0.1f && c2g < 0.1f && c2b < 0.1f) {
    //                        // Full black $color2 - use standard metallic boost
    //                        boostR = boostG = boostB = DEFAULT_METALLIC_BOOST;
    //                    } else {
    //                        // Partial darkening - calculate inverse
    //                        if (c2r > 0.1f) boostR = 1.0f / c2r;
    //                        if (c2g > 0.1f) boostG = 1.0f / c2g;
    //                        if (c2b > 0.1f) boostB = 1.0f / c2b;
    //                        
    //                        // Clamp to reasonable range (1x to max boost)
    //                        boostR = min(MAX_METALLIC_BOOST, max(1.0f, boostR));
    //                        boostG = min(MAX_METALLIC_BOOST, max(1.0f, boostG));
    //                        boostB = min(MAX_METALLIC_BOOST, max(1.0f, boostB));
    //                    }
    //                }
    //                
    //                if (ctx.debugOutput) {
    //                    Msg("[BFT] Applying metallic albedo boost: [%.2f %.2f %.2f] (masked by alpha channel)\n", 
    //                        boostR, boostG, boostB);
    //                }
    //                
    //                // Create boosted albedo texture
    //                ConvertedTexture albedoTex;
    //                albedoTex.width = baseTex.width;
    //                albedoTex.height = baseTex.height;
    //                albedoTex.mipLevels = 1;
    //                albedoTex.format = REMIXAPI_FORMAT_R8G8B8A8_UNORM;
    //                albedoTex.isNormalMap = false;
    //                albedoTex.pixelData.resize(baseTex.width * baseTex.height * 4);
    //                
    //                for (uint32_t i = 0; i < baseTex.width * baseTex.height; i++) {
    //                    // Read original pixel
    //                    float r = static_cast<float>(baseTex.pixelData[i * 4 + 0]) / 255.0f;
    //                    float g = static_cast<float>(baseTex.pixelData[i * 4 + 1]) / 255.0f;
    //                    float b = static_cast<float>(baseTex.pixelData[i * 4 + 2]) / 255.0f;
    //                    uint8_t a = baseTex.pixelData[i * 4 + 3];
    //                    
    //                    // Use alpha channel as metallic mask - only brighten metallic parts
    //                    // Alpha = 255 means fully metallic, alpha = 0 means non-metallic
    //                    float metallicMask = static_cast<float>(a) / 255.0f;
    //                    
    //                    // Save original colors for non-metallic parts
    //                    float origR = r;
    //                    float origG = g;
    //                    float origB = b;
    //                    
    //                    // Calculate boosted metallic colors
    //                    float metalR = min(1.0f, r * boostR);
    //                    float metalG = min(1.0f, g * boostG);
    //                    float metalB = min(1.0f, b * boostB);
    //                    
    //                    // Apply saturation boost for metals to recover gold, copper, bronze tones
    //                    float luminance = 0.2126f * metalR + 0.7152f * metalG + 0.0722f * metalB;
    //                    metalR = min(1.0f, luminance + (metalR - luminance) * METALLIC_SATURATION_BOOST);
    //                    metalG = min(1.0f, luminance + (metalG - luminance) * METALLIC_SATURATION_BOOST);
    //                    metalB = min(1.0f, luminance + (metalB - luminance) * METALLIC_SATURATION_BOOST);
    //                    
    //                    // Ensure minimum brightness for metals (no true black metals exist)
    //                    if (metalR < MIN_METALLIC_BRIGHTNESS && metalG < MIN_METALLIC_BRIGHTNESS && metalB < MIN_METALLIC_BRIGHTNESS) {
    //                        // Scale up to minimum brightness while preserving hue
    //                        float maxC = max(metalR, max(metalG, metalB));
    //                        if (maxC > 0.01f) {
    //                            float scale = MIN_METALLIC_BRIGHTNESS / maxC;
    //                            metalR *= scale;
    //                            metalG *= scale;
    //                            metalB *= scale;
    //                        } else {
    //                            // Pure black - default to neutral metal (like chrome/silver)
    //                            metalR = metalG = metalB = DEFAULT_NEUTRAL_METAL;
    //                        }
    //                    }
    //                    
    //                    // Blend between original (non-metallic) and boosted (metallic) based on mask
    //                    // metallicMask = 1.0 → fully boosted metal color
    //                    // metallicMask = 0.0 → original color unchanged
    //                    r = origR + (metalR - origR) * metallicMask;
    //                    g = origG + (metalG - origG) * metallicMask;
    //                    b = origB + (metalB - origB) * metallicMask;
    //                    
    //                    albedoTex.pixelData[i * 4 + 0] = static_cast<uint8_t>(r * 255.0f);
    //                    albedoTex.pixelData[i * 4 + 1] = static_cast<uint8_t>(g * 255.0f);
    //                    albedoTex.pixelData[i * 4 + 2] = static_cast<uint8_t>(b * 255.0f);
    //                    // Set alpha to fully opaque - original alpha was used as a mask,
    //                    // not as transparency. RTX Remix diffuse_texture shouldn't have alpha.
    //                    albedoTex.pixelData[i * 4 + 3] = 255;
    //                }
    //                
    //                // Write the boosted albedo
    //                std::string path = ctx.generateOutputPath(props.baseTexturePath, "_albedo");
    //                
    //                if (ctx.fileExists(path)) {
    //                    result.albedoPath = path;
    //                    result.skippedCount++;
    //                } else if (ctx.writeDDS(albedoTex, path)) {
    //                    result.albedoPath = path;
    //                    if (ctx.debugOutput) Msg("[BFT] Wrote reconstructed metallic albedo: %s\n", path.c_str());
    //                }
    //                
    //                // Store the boost values in result for potential use in material override
    //                result.albedoBoostR = boostR;
    //                result.albedoBoostG = boostG;
    //                result.albedoBoostB = boostB;
    //                result.hasAlbedoBoost = true;
    //            }
    //        }
    //    }
    //}
    
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
                    
                    std::string path = ctx.generateOutputPath(props.bumpMapPath, "_normal");
                    
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
    
    // =========================================================================
    // EXPONENT TEXTURE PROCESSING (BFT)
    // =========================================================================
    // BlueFlyTrap PseudoPBR encodes roughness in the exponent texture:
    //   - Red channel: Inverted roughness (LINEAR encoding)
    //     255 = smooth/glossy = roughness 0
    //     0 = rough = roughness 1
    //
    // NOTE: BFT does NOT use the green channel for metallic (that's MWB).
    // BFT stores metallic in the BASE TEXTURE ALPHA, not the exponent texture.
    // =========================================================================
    if (props.hasBFTExponentTexture && !props.bftExponentTexturePath.empty()) {
        std::vector<uint8_t> fileData;
        if (ctx.readVTFFile(props.bftExponentTexturePath, fileData)) {
            VTFFileHeader header;
            if (ctx.parseVTFHeader(fileData, header)) {
                ConvertedTexture expTex;
                expTex.isNormalMap = false;
                
                if (ctx.extractPixelData(fileData, header, expTex, false)) {
                    uint32_t totalPixels = expTex.width * expTex.height;
                    
                    // =============================================================
                    // Extract ROUGHNESS from red channel (simple linear inversion)
                    // =============================================================
                    ConvertedTexture roughTex;
                    roughTex.width = expTex.width;
                    roughTex.height = expTex.height;
                    roughTex.mipLevels = 1;
                    roughTex.format = REMIXAPI_FORMAT_R8G8B8A8_UNORM;
                    roughTex.isNormalMap = false;
                    roughTex.pixelData.resize(totalPixels * 4);
                    
                    for (uint32_t i = 0; i < totalPixels; i++) {
                        // Red channel = inverted roughness (BFT uses linear encoding)
                        uint8_t expValue = expTex.pixelData[i * 4 + 0];
                        float roughness = ExponentToRoughness(expValue);
                        uint8_t roughByte = static_cast<uint8_t>(roughness * 255.0f);
                        
                        roughTex.pixelData[i * 4 + 0] = roughByte;
                        roughTex.pixelData[i * 4 + 1] = roughByte;
                        roughTex.pixelData[i * 4 + 2] = roughByte;
                        roughTex.pixelData[i * 4 + 3] = 255;
                    }
                    
                    std::string roughPath = ctx.generateOutputPath(props.bftExponentTexturePath, "_roughness");
                    
                    if (ctx.fileExists(roughPath)) {
                        result.roughnessPath = roughPath;
                        result.skippedCount++;
                    } else if (ctx.writeDDS(roughTex, roughPath)) {
                        result.roughnessPath = roughPath;
                        if (ctx.materialsWithRoughness) (*ctx.materialsWithRoughness)++;
                        if (ctx.debugOutput) Msg("[BFT] Wrote roughness (linear decode): %s\n", roughPath.c_str());
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
} // namespace ToPBR
} // namespace MaterialPipeline

#endif // _WIN64
