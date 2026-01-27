// =========================================================================
// format_mwbpbr.cpp - MWB PBR Gen Format Handler
// =========================================================================
// MWB PBR Gen (Modern Warfare Blender PBR Generator) is a tool that converts
// PBR textures to Source Engine's phong workflow.
//
// Original code reference:
// https://github.com/mushroom-guy/mwb-materials/blob/main/mwb-materials/MwbMats/MaterialManipulation.cs
//
// MWB Texture Encoding:
//   Albedo (_rgb):
//     - RGB: Original albedo color
//     - Alpha: Metalness mask
//
//   Exponent (_e):
//     - Red: pow(inverted_roughness, 4.0) * metalness_lerp
//     - Green: Direct metalness value
//     - Alpha: Roughness for rimlight
//
//   Normal (_n):
//     - RGB: Normal map
//     - Alpha: pow(roughness, 2.5)
//
// Detection markers:
//   - $basetexture with *_rgb suffix
//   - $phongexponenttexture with *_e suffix
//   - "pbr\output\" or "pbr/output/" in texture path
//   - "mwb pbr" or "pbr gen" in VMT comments
//   - Proxies with "MwEnvMapTint" or "Arc9EnvMapTint"
//
// Decoding (Source → PBR):
//   Roughness: 1.0 - pow(exponent_red / 255, 0.25)  (reverses pow(gloss, 4.0))
//   Metallic: exponent_green OR albedo_alpha
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

namespace MWBPBR {

// =========================================================================
// Detection
// =========================================================================
// MWB PBR Gen has distinct patterns that differentiate it from BFT:
//   - Uses _rgb suffix for base textures (BFT doesn't consistently use this)
//   - Often has "pbr\output\" in paths (tool output folder)
//   - Has MWB-specific proxy names in VMT

bool Detect(const VMTParseResult& vmt) {
    std::string shaderLower = vmt.shaderName;
    std::transform(shaderLower.begin(), shaderLower.end(), shaderLower.begin(), ::tolower);
    
    // Must be VertexlitGeneric
    if (shaderLower != "vertexlitgeneric") return false;
    
    // Check for $phongexponenttexture (required for MWB PBR)
    std::string expTex = vmt.findValue("$phongexponenttexture");
    if (expTex.empty()) return false;
    
    // =========================================================================
    // MWB-specific detection patterns
    // =========================================================================
    
    // 1. Check for _rgb base texture suffix (strong MWB indicator)
    std::string baseTex = vmt.findValue("$basetexture");
    if (!baseTex.empty()) {
        std::string baseTexLower = baseTex;
        std::transform(baseTexLower.begin(), baseTexLower.end(), baseTexLower.begin(), ::tolower);
        
        // _rgb suffix is MWB's naming convention
        if (baseTexLower.length() >= 4 && baseTexLower.substr(baseTexLower.length() - 4) == "_rgb") {
            return true;
        }
        
        // pbr\output\ path is MWB's output folder
        if (baseTexLower.find("pbr\\output\\") != std::string::npos ||
            baseTexLower.find("pbr/output/") != std::string::npos) {
            return true;
        }
    }
    
    // 2. Check for MWB-specific comments or proxy names
    if (vmt.contentLower.find("mwb pbr") != std::string::npos ||
        vmt.contentLower.find("pbr gen") != std::string::npos ||
        vmt.contentLower.find("mwenvmaptint") != std::string::npos ||
        vmt.contentLower.find("arc9envmaptint") != std::string::npos) {
        return true;
    }
    
    return false;
}

// =========================================================================
// Property Extraction
// =========================================================================

void ExtractProperties(const VMTParseResult& vmt, MaterialPBRProperties& props) {
    props.isMWBPBR = true;
    
    // Get the exponent texture path
    std::string expTex = vmt.findValue("$phongexponenttexture");
    if (!expTex.empty()) {
        props.bftExponentTexturePath = expTex;  // Reuse the same field
        props.hasBFTExponentTexture = true;
    }
    
    // MWB doesn't use the BFT layer stacking concept
    // It's a single-layer material with PBR data encoded in textures
    props.isBFTMetallicLayer = false;
    props.isBFTDiffuseLayer = false;
    
    // Default to mid-range values - textures will provide actual data
    props.roughness = 0.5f;
    props.metallic = 0.0f;
}

// =========================================================================
// MWB Roughness Decoding
// =========================================================================
// MWB encoding (from MaterialManipulation.cs):
//   1. Original roughness is INVERTED to get gloss
//   2. Then pow(gloss, 4.0) is applied and stored in red channel
//   3. Metalness lerp may affect the final value
//
// To decode back to roughness:
//   1. Normalize: value / 255.0
//   2. Reverse the pow(4.0): pow(normalized, 1/4.0) = pow(normalized, 0.25)
//   3. Invert to convert gloss back to roughness: roughness = 1.0 - gloss

static float ExponentRedToRoughness(uint8_t expValue) {
    // Normalize to 0-1 range
    float normalized = static_cast<float>(expValue) / 255.0f;
    
    // Reverse the pow(4.0) transformation used during MWB encoding
    // pow(gloss, 4.0) was applied, so we use pow(value, 0.25) = 4th root
    float gloss = std::pow(normalized, 0.25f);
    
    // Convert gloss to roughness (MWB inverted roughness before encoding)
    float roughness = 1.0f - gloss;
    
    // Clamp to valid PBR range
    if (roughness < 0.04f) roughness = 0.04f;
    if (roughness > 1.0f) roughness = 1.0f;
    
    return roughness;
}

// =========================================================================
// Constants
// =========================================================================
static const uint8_t VARIATION_THRESHOLD = 10;
static const uint32_t SAMPLE_STEP = 8;

// =========================================================================
// Texture Processing
// =========================================================================

ProcessedMaterial ProcessTextures(const MaterialPBRProperties& props,
                                   uint64_t textureHash,
                                   const ProcessingContext& ctx) {
    ProcessedMaterial result;
    
    if (ctx.debugOutput) {
        Msg("[MWB-PBR] Processing material: %s\n", props.materialName.c_str());
    }
    
    // =========================================================================
    // NORMAL MAP PROCESSING
    // =========================================================================
    // MWB materials have standard normal maps in $bumpmap
    if (props.hasBumpMap && !props.bumpMapPath.empty()) {
        std::vector<uint8_t> fileData;
        if (ctx.readVTFFile(props.bumpMapPath, fileData)) {
            VTFFileHeader header;
            if (ctx.parseVTFHeader(fileData, header)) {
                ConvertedTexture normalTex;
                normalTex.isNormalMap = true;
                
                if (ctx.extractPixelData(fileData, header, normalTex, false)) {
                    // Handle SSBump conversion if needed
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
                        if (ctx.debugOutput) Msg("[MWB-PBR] Wrote normal: %s\n", path.c_str());
                    }
                }
            }
        }
    }
    
    // =========================================================================
    // EXPONENT TEXTURE PROCESSING
    // =========================================================================
    // MWB encodes multiple channels in the exponent texture:
    //   - Red channel: Encoded roughness (pow(gloss, 4.0))
    //   - Green channel: Metalness (direct value)
    //   - Alpha channel: Roughness for rimlight
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
                    // Extract ROUGHNESS from red channel using MWB formula
                    // =============================================================
                    ConvertedTexture roughTex;
                    roughTex.width = expTex.width;
                    roughTex.height = expTex.height;
                    roughTex.mipLevels = 1;
                    roughTex.format = REMIXAPI_FORMAT_R8G8B8A8_UNORM;
                    roughTex.isNormalMap = false;
                    roughTex.pixelData.resize(totalPixels * 4);
                    
                    for (uint32_t i = 0; i < totalPixels; i++) {
                        // Red channel contains the encoded gloss (pow(gloss, 4.0))
                        uint8_t expValue = expTex.pixelData[i * 4 + 0];
                        float roughness = ExponentRedToRoughness(expValue);
                        uint8_t roughByte = static_cast<uint8_t>(roughness * 255.0f);
                        
                        roughTex.pixelData[i * 4 + 0] = roughByte;
                        roughTex.pixelData[i * 4 + 1] = roughByte;
                        roughTex.pixelData[i * 4 + 2] = roughByte;
                        roughTex.pixelData[i * 4 + 3] = 255;
                    }
                    
                    uint64_t roughHash = ctx.generateHash(props.bftExponentTexturePath + "_mwb_rough", roughTex.width, roughTex.height);
                    std::string roughPath = ctx.generateOutputPath(roughHash, "_roughness");
                    
                    if (ctx.fileExists(roughPath)) {
                        result.roughnessPath = roughPath;
                        result.skippedCount++;
                    } else if (ctx.writeDDS(roughTex, roughPath)) {
                        result.roughnessPath = roughPath;
                        if (ctx.materialsWithRoughness) (*ctx.materialsWithRoughness)++;
                        if (ctx.debugOutput) Msg("[MWB-PBR] Wrote roughness (pow^0.25 decode): %s\n", roughPath.c_str());
                    }
                    
                    // =============================================================
                    // Extract METALLIC from green channel (MWB stores metalness directly)
                    // =============================================================
                    // Check if green channel has meaningful metallic information
                    uint8_t minGreen = 255;
                    uint8_t maxGreen = 0;
                    for (uint32_t i = 0; i < totalPixels; i += SAMPLE_STEP) {
                        uint8_t g = expTex.pixelData[i * 4 + 1]; // Green channel
                        if (g < minGreen) minGreen = g;
                        if (g > maxGreen) maxGreen = g;
                    }
                    
                    // Only use green channel if it has meaningful variation
                    if ((maxGreen - minGreen) > VARIATION_THRESHOLD) {
                        if (ctx.debugOutput) {
                            Msg("[MWB-PBR] Exponent green channel has metallic info (min=%d, max=%d)\n", 
                                minGreen, maxGreen);
                        }
                        
                        ConvertedTexture metallicTex;
                        metallicTex.width = expTex.width;
                        metallicTex.height = expTex.height;
                        metallicTex.mipLevels = 1;
                        metallicTex.format = REMIXAPI_FORMAT_R8G8B8A8_UNORM;
                        metallicTex.isNormalMap = false;
                        metallicTex.pixelData.resize(totalPixels * 4);
                        
                        for (uint32_t i = 0; i < totalPixels; i++) {
                            // Green channel = metallic value in MWB encoding
                            uint8_t metallic = expTex.pixelData[i * 4 + 1];
                            
                            metallicTex.pixelData[i * 4 + 0] = metallic;
                            metallicTex.pixelData[i * 4 + 1] = metallic;
                            metallicTex.pixelData[i * 4 + 2] = metallic;
                            metallicTex.pixelData[i * 4 + 3] = 255;
                        }
                        
                        uint64_t metalHash = ctx.generateHash(props.bftExponentTexturePath + "_mwb_metallic", metallicTex.width, metallicTex.height);
                        std::string metalPath = ctx.generateOutputPath(metalHash, "_metallic");
                        
                        if (ctx.fileExists(metalPath)) {
                            result.metallicPath = metalPath;
                            result.skippedCount++;
                        } else if (ctx.writeDDS(metallicTex, metalPath)) {
                            result.metallicPath = metalPath;
                            if (ctx.debugOutput) Msg("[MWB-PBR] Extracted metallic from exponent green: %s\n", metalPath.c_str());
                        }
                    }
                }
            }
        }
    }
    
    // =========================================================================
    // FALLBACK: Extract metallic from base texture alpha if not found in exponent
    // =========================================================================
    if (result.metallicPath.empty() && !props.baseTexturePath.empty()) {
        std::vector<uint8_t> fileData;
        if (ctx.readVTFFile(props.baseTexturePath, fileData)) {
            VTFFileHeader header;
            if (ctx.parseVTFHeader(fileData, header)) {
                ConvertedTexture baseTex;
                baseTex.isNormalMap = false;
                
                if (ctx.extractPixelData(fileData, header, baseTex, false)) {
                    if (baseTex.pixelData.size() >= 4) {
                        // Check if alpha channel has meaningful variation
                        uint8_t minAlpha = 255;
                        uint8_t maxAlpha = 0;
                        uint32_t totalPixels = baseTex.width * baseTex.height;
                        
                        for (uint32_t i = 0; i < totalPixels; i += SAMPLE_STEP) {
                            uint8_t a = baseTex.pixelData[i * 4 + 3];
                            if (a < minAlpha) minAlpha = a;
                            if (a > maxAlpha) maxAlpha = a;
                        }
                        
                        if ((maxAlpha - minAlpha) > VARIATION_THRESHOLD) {
                            if (ctx.debugOutput) {
                                Msg("[MWB-PBR] Base alpha has metallic info (min=%d, max=%d)\n", 
                                    minAlpha, maxAlpha);
                            }
                            
                            ConvertedTexture metallicTex;
                            metallicTex.width = baseTex.width;
                            metallicTex.height = baseTex.height;
                            metallicTex.mipLevels = 1;
                            metallicTex.format = REMIXAPI_FORMAT_R8G8B8A8_UNORM;
                            metallicTex.isNormalMap = false;
                            metallicTex.pixelData.resize(totalPixels * 4);
                            
                            for (uint32_t i = 0; i < totalPixels; i++) {
                                uint8_t metallic = baseTex.pixelData[i * 4 + 3];
                                
                                metallicTex.pixelData[i * 4 + 0] = metallic;
                                metallicTex.pixelData[i * 4 + 1] = metallic;
                                metallicTex.pixelData[i * 4 + 2] = metallic;
                                metallicTex.pixelData[i * 4 + 3] = 255;
                            }
                            
                            uint64_t metalHash = ctx.generateHash(props.baseTexturePath + "_mwb_metallic", metallicTex.width, metallicTex.height);
                            std::string metalPath = ctx.generateOutputPath(metalHash, "_metallic");
                            
                            if (ctx.fileExists(metalPath)) {
                                result.metallicPath = metalPath;
                                result.skippedCount++;
                            } else if (ctx.writeDDS(metallicTex, metalPath)) {
                                result.metallicPath = metalPath;
                                if (ctx.debugOutput) Msg("[MWB-PBR] Extracted metallic from base alpha: %s\n", metalPath.c_str());
                            }
                        }
                    }
                }
            }
        }
    }
    
    result.success = true;
    if (ctx.debugOutput) {
        Msg("[MWB-PBR] Complete: %s (skipped %d existing)\n", props.materialName.c_str(), result.skippedCount);
    }
    
    return result;
}

} // namespace MWBPBR
} // namespace ToPBR
} // namespace MaterialPipeline

#endif // _WIN64
