// =========================================================================
// format_exopbr.cpp - ExoPBR Format Handler
// =========================================================================
// ExoPBR is a community PBR format using screenspace_general_8tex shader
// with an ExoPBR proxy marker. Provides direct PBR textures:
//
//   $texture1 - ARM map (R=AO, G=Roughness, B=Metallic, A=Height)
//   $texture2 - Normal map (DirectX Y- format, green channel inverted)
//   $texture3 - Emission texture
//   $emissionscale - Emission intensity multiplier
//   $emissiontint - Emission color tint [r g b]
// =========================================================================

#ifdef _WIN64

#include "formats.h"
#include <tier0/dbg.h>
#include <algorithm>

namespace MaterialPipeline {
namespace ToPBR {
namespace ExoPBR {

// =========================================================================
// Detection
// =========================================================================

bool Detect(const VMTParseResult& vmt) {
    std::string shaderLower = vmt.shaderName;
    std::transform(shaderLower.begin(), shaderLower.end(), shaderLower.begin(), ::tolower);
    
    if (shaderLower != "screenspace_general_8tex") {
        return false;
    }
    
    // Must have ExoPBR proxy marker
    return vmt.contentLower.find("exopbr") != std::string::npos;
}

// =========================================================================
// Property Extraction
// =========================================================================

void ExtractProperties(const VMTParseResult& vmt, MaterialPBRProperties& props) {
    props.isExoPBR = true;
    
    // $texture1 - ARM map
    std::string tex1 = vmt.findValue("$texture1");
    if (!tex1.empty()) {
        props.armTexturePath = tex1;
        props.hasARMTexture = true;
    }
    
    // $texture2 - Normal map (DirectX Y-)
    std::string tex2 = vmt.findValue("$texture2");
    if (!tex2.empty()) {
        props.exoNormalPath = tex2;
        props.hasExoNormal = true;
    }
    
    // $texture3 - Emission
    std::string tex3 = vmt.findValue("$texture3");
    if (!tex3.empty()) {
        props.emissionTexturePath = tex3;
        props.hasEmissionTexture = true;
    }
    
    // $emissionscale
    std::string scale = vmt.findValue("$emissionscale");
    if (!scale.empty()) {
        props.emissionScale = static_cast<float>(atof(scale.c_str()));
        props.hasEmissionScale = true;
    }
    
    // $emissiontint [r g b]
    std::string tint = vmt.findValue("$emissiontint");
    if (!tint.empty()) {
        float r, g, b;
        if (sscanf(tint.c_str(), "[%f %f %f]", &r, &g, &b) == 3) {
            props.emissionTint[0] = r;
            props.emissionTint[1] = g;
            props.emissionTint[2] = b;
            props.hasEmissionTint = true;
        }
    }
    
    // ExoPBR provides direct PBR - set defaults that textures will override
    props.roughness = 0.5f;
    props.metallic = 0.0f;
}

// =========================================================================
// Texture Processing
// =========================================================================

ProcessedMaterial ProcessTextures(const MaterialPBRProperties& props,
                                   uint64_t textureHash,
                                   const ProcessingContext& ctx) {
    ProcessedMaterial result;
    
    if (ctx.debugOutput) {
        Msg("[ExoPBR] Processing material: %s\n", props.materialName.c_str());
    }
    
    // Process ARM texture - split into roughness, metallic, height
    if (props.hasARMTexture && !props.armTexturePath.empty()) {
        std::vector<uint8_t> fileData;
        if (ctx.readVTFFile(props.armTexturePath, fileData)) {
            VTFFileHeader header;
            if (ctx.parseVTFHeader(fileData, header)) {
                ConvertedTexture armTex;
                armTex.isNormalMap = false;
                
                if (ctx.extractPixelData(fileData, header, armTex, false)) {
                    uint32_t pixelCount = armTex.width * armTex.height;
                    
                    // Extract roughness from green channel
                    {
                        ConvertedTexture roughTex;
                        roughTex.width = armTex.width;
                        roughTex.height = armTex.height;
                        roughTex.mipLevels = 1;
                        roughTex.format = REMIXAPI_FORMAT_R8G8B8A8_UNORM;
                        roughTex.isNormalMap = false;
                        roughTex.pixelData.resize(pixelCount * 4);
                        
                        for (uint32_t i = 0; i < pixelCount; i++) {
                            uint8_t val = armTex.pixelData[i * 4 + 1];  // Green
                            roughTex.pixelData[i * 4 + 0] = val;
                            roughTex.pixelData[i * 4 + 1] = val;
                            roughTex.pixelData[i * 4 + 2] = val;
                            roughTex.pixelData[i * 4 + 3] = 255;
                        }
                        
                        uint64_t hash = ctx.generateHash(props.armTexturePath + "_rough", roughTex.width, roughTex.height);
                        std::string path = ctx.generateOutputPath(hash, "_roughness");
                        
                        if (ctx.fileExists(path)) {
                            result.roughnessPath = path;
                            result.skippedCount++;
                        } else if (ctx.writeDDS(roughTex, path)) {
                            result.roughnessPath = path;
                            if (ctx.materialsWithRoughness) (*ctx.materialsWithRoughness)++;
                            if (ctx.debugOutput) Msg("[ExoPBR] Wrote roughness: %s\n", path.c_str());
                        }
                    }
                    
                    // Extract metallic from blue channel
                    {
                        ConvertedTexture metalTex;
                        metalTex.width = armTex.width;
                        metalTex.height = armTex.height;
                        metalTex.mipLevels = 1;
                        metalTex.format = REMIXAPI_FORMAT_R8G8B8A8_UNORM;
                        metalTex.isNormalMap = false;
                        metalTex.pixelData.resize(pixelCount * 4);
                        
                        for (uint32_t i = 0; i < pixelCount; i++) {
                            uint8_t val = armTex.pixelData[i * 4 + 2];  // Blue
                            metalTex.pixelData[i * 4 + 0] = val;
                            metalTex.pixelData[i * 4 + 1] = val;
                            metalTex.pixelData[i * 4 + 2] = val;
                            metalTex.pixelData[i * 4 + 3] = 255;
                        }
                        
                        uint64_t hash = ctx.generateHash(props.armTexturePath + "_metal", metalTex.width, metalTex.height);
                        std::string path = ctx.generateOutputPath(hash, "_metallic");
                        
                        if (ctx.fileExists(path)) {
                            result.metallicPath = path;
                            result.skippedCount++;
                        } else if (ctx.writeDDS(metalTex, path)) {
                            result.metallicPath = path;
                            if (ctx.debugOutput) Msg("[ExoPBR] Wrote metallic: %s\n", path.c_str());
                        }
                    }
                    
                    // Extract height from alpha if present
                    {
                        bool hasHeight = false;
                        for (uint32_t i = 0; i < pixelCount && !hasHeight; i++) {
                            if (armTex.pixelData[i * 4 + 3] != 255) hasHeight = true;
                        }
                        
                        if (hasHeight) {
                            ConvertedTexture heightTex;
                            heightTex.width = armTex.width;
                            heightTex.height = armTex.height;
                            heightTex.mipLevels = 1;
                            heightTex.format = REMIXAPI_FORMAT_R8G8B8A8_UNORM;
                            heightTex.isNormalMap = false;
                            heightTex.pixelData.resize(pixelCount * 4);
                            
                            for (uint32_t i = 0; i < pixelCount; i++) {
                                uint8_t val = armTex.pixelData[i * 4 + 3];  // Alpha
                                heightTex.pixelData[i * 4 + 0] = val;
                                heightTex.pixelData[i * 4 + 1] = val;
                                heightTex.pixelData[i * 4 + 2] = val;
                                heightTex.pixelData[i * 4 + 3] = 255;
                            }
                            
                            uint64_t hash = ctx.generateHash(props.armTexturePath + "_height", heightTex.width, heightTex.height);
                            std::string path = ctx.generateOutputPath(hash, "_height");
                            
                            if (ctx.fileExists(path)) {
                                result.heightPath = path;
                                result.skippedCount++;
                            } else if (ctx.writeDDS(heightTex, path)) {
                                result.heightPath = path;
                                if (ctx.debugOutput) Msg("[ExoPBR] Wrote height: %s\n", path.c_str());
                            }
                        }
                    }
                }
            }
        }
    }
    
    // Process normal map - flip green channel for DirectX Y-
    if (props.hasExoNormal && !props.exoNormalPath.empty()) {
        std::vector<uint8_t> fileData;
        if (ctx.readVTFFile(props.exoNormalPath, fileData)) {
            VTFFileHeader header;
            if (ctx.parseVTFHeader(fileData, header)) {
                ConvertedTexture normalTex;
                normalTex.isNormalMap = true;
                
                if (ctx.extractPixelData(fileData, header, normalTex, false)) {
                    // Flip green channel (DirectX Y- to OpenGL Y+)
                    for (uint32_t i = 0; i < normalTex.width * normalTex.height; i++) {
                        normalTex.pixelData[i * 4 + 1] = 255 - normalTex.pixelData[i * 4 + 1];
                    }
                    
                    ctx.convertToOctahedral(normalTex);
                    
                    uint64_t hash = ctx.generateHash(props.exoNormalPath + "_normal", normalTex.width, normalTex.height);
                    std::string path = ctx.generateOutputPath(hash, "_normal");
                    
                    if (ctx.fileExists(path)) {
                        result.normalPath = path;
                        result.skippedCount++;
                    } else if (ctx.writeDDS(normalTex, path)) {
                        result.normalPath = path;
                        if (ctx.materialsWithNormals) (*ctx.materialsWithNormals)++;
                        if (ctx.debugOutput) Msg("[ExoPBR] Wrote normal (Y-flipped): %s\n", path.c_str());
                    }
                }
            }
        }
    }
    
    // Process emission texture
    if (props.hasEmissionTexture && !props.emissionTexturePath.empty()) {
        std::vector<uint8_t> fileData;
        if (ctx.readVTFFile(props.emissionTexturePath, fileData)) {
            VTFFileHeader header;
            if (ctx.parseVTFHeader(fileData, header)) {
                ConvertedTexture emitTex;
                emitTex.isNormalMap = false;
                
                if (ctx.extractPixelData(fileData, header, emitTex, false)) {
                    uint64_t hash = ctx.generateHash(props.emissionTexturePath + "_emit", emitTex.width, emitTex.height);
                    std::string path = ctx.generateOutputPath(hash, "_emission");
                    
                    if (ctx.fileExists(path)) {
                        result.emissivePath = path;
                        result.skippedCount++;
                    } else if (ctx.writeDDS(emitTex, path)) {
                        result.emissivePath = path;
                        if (ctx.debugOutput) Msg("[ExoPBR] Wrote emission: %s\n", path.c_str());
                    }
                    if (!result.emissivePath.empty()) {
                        result.emissionIntensity = props.hasEmissionScale ? props.emissionScale : 1.0f;
                    }
                }
            }
        }
    }
    
    result.success = true;
    if (ctx.debugOutput) {
        Msg("[ExoPBR] Complete: %s (skipped %d existing)\n", props.materialName.c_str(), result.skippedCount);
    }
    
    return result;
}

} // namespace ExoPBR
} // namespace ToPBR
} // namespace MaterialPipeline

#endif // _WIN64
