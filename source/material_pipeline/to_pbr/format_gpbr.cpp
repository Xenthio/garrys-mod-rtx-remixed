// =========================================================================
// format_gpbr.cpp - GPBR (Strata Source) Format Handler
// =========================================================================
// GPBR is a community PBR format used by Strata Source engine mods.
// Uses the "PBR" shader with:
//
//   $mraotexture - MRAO map (R=Metallic, G=Roughness, B=AO)
//   $bumpmap - Normal map (alpha may contain height for parallax)
//   $emissiontexture - Emission/glow map
//   $emissionscale - Emission intensity
//   $parallax - Enable parallax mapping
//   $parallaxdepth - Height displacement depth
//   $alpha - Transparency value
// =========================================================================

#ifdef _WIN64

#include "formats.h"
#include <tier0/dbg.h>
#include <algorithm>
#include <cstdlib>

namespace MaterialPipeline {
namespace ToPBR {
namespace GPBR {

// =========================================================================
// Detection
// =========================================================================

bool Detect(const VMTParseResult& vmt) {
    std::string shaderLower = vmt.shaderName;
    std::transform(shaderLower.begin(), shaderLower.end(), shaderLower.begin(), ::tolower);
    return shaderLower == "pbr";
}

// =========================================================================
// Property Extraction
// =========================================================================

void ExtractProperties(const VMTParseResult& vmt, MaterialPBRProperties& props) {
    props.isGPBR = true;
    
    // $mraotexture - MRAO map
    std::string mrao = vmt.findValue("$mraotexture");
    if (!mrao.empty()) {
        props.mraoTexturePath = mrao;
        props.hasMRAOTexture = true;
    }
    
    // $mraoscale
    std::string mraoScale = vmt.findValue("$mraoscale");
    if (!mraoScale.empty()) {
        props.mraoScale = static_cast<float>(atof(mraoScale.c_str()));
        props.hasMRAOScale = true;
    }
    
    // $emissiontexture
    std::string emission = vmt.findValue("$emissiontexture");
    if (!emission.empty()) {
        props.gpbrEmissionPath = emission;
        props.hasGPBREmission = true;
    }
    
    // $emissionscale (can be float or vector)
    std::string emScale = vmt.findValue("$emissionscale");
    if (!emScale.empty()) {
        if (emScale[0] == '[') {
            float r, g, b;
            if (sscanf(emScale.c_str(), "[%f %f %f]", &r, &g, &b) >= 3) {
                props.gpbrEmissionScale = (r + g + b) / 3.0f;
            }
        } else {
            props.gpbrEmissionScale = static_cast<float>(atof(emScale.c_str()));
        }
        props.hasGPBREmissionScale = true;
    }
    
    // $parallax
    std::string parallax = vmt.findValue("$parallax");
    if (!parallax.empty()) {
        props.gpbrParallax = (atoi(parallax.c_str()) != 0);
    }
    
    // $parallaxdepth
    std::string depth = vmt.findValue("$parallaxdepth");
    if (!depth.empty()) {
        props.gpbrParallaxDepth = static_cast<float>(atof(depth.c_str()));
    }
    
    // $alpha
    std::string alpha = vmt.findValue("$alpha");
    if (!alpha.empty()) {
        props.gpbrAlpha = static_cast<float>(atof(alpha.c_str()));
        props.hasGPBRAlpha = true;
    }
    
    // GPBR provides direct PBR - defaults will be overridden by MRAO
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
        Msg("[GPBR] Processing material: %s\n", props.materialName.c_str());
    }
    
    // Process MRAO texture - split into roughness and metallic
    // MRAO layout: R=Metallic, G=Roughness, B=AO
    if (props.hasMRAOTexture && !props.mraoTexturePath.empty()) {
        std::vector<uint8_t> fileData;
        if (ctx.readVTFFile(props.mraoTexturePath, fileData)) {
            VTFFileHeader header;
            if (ctx.parseVTFHeader(fileData, header)) {
                ConvertedTexture mraoTex;
                mraoTex.isNormalMap = false;
                
                if (ctx.extractPixelData(fileData, header, mraoTex, false)) {
                    uint32_t pixelCount = mraoTex.width * mraoTex.height;
                    float scale = props.hasMRAOScale ? props.mraoScale : 1.0f;
                    
                    // Extract roughness from green channel
                    {
                        ConvertedTexture roughTex;
                        roughTex.width = mraoTex.width;
                        roughTex.height = mraoTex.height;
                        roughTex.mipLevels = 1;
                        roughTex.format = REMIXAPI_FORMAT_R8G8B8A8_UNORM;
                        roughTex.isNormalMap = false;
                        roughTex.pixelData.resize(pixelCount * 4);
                        
                        for (uint32_t i = 0; i < pixelCount; i++) {
                            float val = mraoTex.pixelData[i * 4 + 1] * scale;  // Green
                            if (val > 255.0f) val = 255.0f;
                            uint8_t byte = static_cast<uint8_t>(val);
                            roughTex.pixelData[i * 4 + 0] = byte;
                            roughTex.pixelData[i * 4 + 1] = byte;
                            roughTex.pixelData[i * 4 + 2] = byte;
                            roughTex.pixelData[i * 4 + 3] = 255;
                        }
                        
                        uint64_t hash = ctx.generateHash(props.mraoTexturePath + "_rough", roughTex.width, roughTex.height);
                        std::string path = ctx.generateOutputPath(hash, "_roughness");
                        
                        if (ctx.fileExists(path)) {
                            result.roughnessPath = path;
                            result.skippedCount++;
                        } else if (ctx.writeDDS(roughTex, path)) {
                            result.roughnessPath = path;
                            if (ctx.materialsWithRoughness) (*ctx.materialsWithRoughness)++;
                            if (ctx.debugOutput) Msg("[GPBR] Wrote roughness: %s\n", path.c_str());
                        }
                    }
                    
                    // Extract metallic from red channel
                    {
                        ConvertedTexture metalTex;
                        metalTex.width = mraoTex.width;
                        metalTex.height = mraoTex.height;
                        metalTex.mipLevels = 1;
                        metalTex.format = REMIXAPI_FORMAT_R8G8B8A8_UNORM;
                        metalTex.isNormalMap = false;
                        metalTex.pixelData.resize(pixelCount * 4);
                        
                        for (uint32_t i = 0; i < pixelCount; i++) {
                            float val = mraoTex.pixelData[i * 4 + 0] * scale;  // Red
                            if (val > 255.0f) val = 255.0f;
                            uint8_t byte = static_cast<uint8_t>(val);
                            metalTex.pixelData[i * 4 + 0] = byte;
                            metalTex.pixelData[i * 4 + 1] = byte;
                            metalTex.pixelData[i * 4 + 2] = byte;
                            metalTex.pixelData[i * 4 + 3] = 255;
                        }
                        
                        uint64_t hash = ctx.generateHash(props.mraoTexturePath + "_metal", metalTex.width, metalTex.height);
                        std::string path = ctx.generateOutputPath(hash, "_metallic");
                        
                        if (ctx.fileExists(path)) {
                            result.metallicPath = path;
                            result.skippedCount++;
                        } else if (ctx.writeDDS(metalTex, path)) {
                            result.metallicPath = path;
                            if (ctx.debugOutput) Msg("[GPBR] Wrote metallic: %s\n", path.c_str());
                        }
                    }
                }
            }
        }
    }
    
    // Process normal map (with optional parallax height in alpha)
    if (props.hasBumpMap && !props.bumpMapPath.empty()) {
        std::vector<uint8_t> fileData;
        if (ctx.readVTFFile(props.bumpMapPath, fileData)) {
            VTFFileHeader header;
            if (ctx.parseVTFHeader(fileData, header)) {
                ConvertedTexture normalTex;
                normalTex.isNormalMap = true;
                
                if (ctx.extractPixelData(fileData, header, normalTex, false)) {
                    // Extract height from alpha before octahedral conversion
                    if (props.gpbrParallax) {
                        ConvertedTexture heightTex;
                        heightTex.width = normalTex.width;
                        heightTex.height = normalTex.height;
                        heightTex.mipLevels = 1;
                        heightTex.format = REMIXAPI_FORMAT_R8G8B8A8_UNORM;
                        heightTex.isNormalMap = false;
                        heightTex.pixelData.resize(normalTex.width * normalTex.height * 4);
                        
                        for (uint32_t i = 0; i < normalTex.width * normalTex.height; i++) {
                            uint8_t val = normalTex.pixelData[i * 4 + 3];  // Alpha
                            heightTex.pixelData[i * 4 + 0] = val;
                            heightTex.pixelData[i * 4 + 1] = val;
                            heightTex.pixelData[i * 4 + 2] = val;
                            heightTex.pixelData[i * 4 + 3] = 255;
                        }
                        
                        uint64_t hash = ctx.generateHash(props.bumpMapPath + "_height", heightTex.width, heightTex.height);
                        std::string path = ctx.generateOutputPath(hash, "_height");
                        
                        if (ctx.fileExists(path)) {
                            result.heightPath = path;
                            result.heightScale = props.gpbrParallaxDepth;
                            result.skippedCount++;
                        } else if (ctx.writeDDS(heightTex, path)) {
                            result.heightPath = path;
                            result.heightScale = props.gpbrParallaxDepth;
                            if (ctx.debugOutput) Msg("[GPBR] Wrote height: %s (depth=%.3f)\n", path.c_str(), props.gpbrParallaxDepth);
                        }
                    }
                    
                    // Convert normal map
                    ctx.convertToOctahedral(normalTex);
                    
                    uint64_t hash = ctx.generateHash(props.bumpMapPath + "_normal", normalTex.width, normalTex.height);
                    std::string path = ctx.generateOutputPath(hash, "_normal");
                    
                    if (ctx.fileExists(path)) {
                        result.normalPath = path;
                        result.skippedCount++;
                    } else if (ctx.writeDDS(normalTex, path)) {
                        result.normalPath = path;
                        if (ctx.materialsWithNormals) (*ctx.materialsWithNormals)++;
                        if (ctx.debugOutput) Msg("[GPBR] Wrote normal: %s\n", path.c_str());
                    }
                }
            }
        }
    }
    
    // Process emission texture
    if (props.hasGPBREmission && !props.gpbrEmissionPath.empty()) {
        std::vector<uint8_t> fileData;
        if (ctx.readVTFFile(props.gpbrEmissionPath, fileData)) {
            VTFFileHeader header;
            if (ctx.parseVTFHeader(fileData, header)) {
                ConvertedTexture emitTex;
                emitTex.isNormalMap = false;
                
                if (ctx.extractPixelData(fileData, header, emitTex, false)) {
                    uint64_t hash = ctx.generateHash(props.gpbrEmissionPath + "_emit", emitTex.width, emitTex.height);
                    std::string path = ctx.generateOutputPath(hash, "_emission");
                    
                    if (ctx.fileExists(path)) {
                        result.emissivePath = path;
                        result.skippedCount++;
                    } else if (ctx.writeDDS(emitTex, path)) {
                        result.emissivePath = path;
                        result.emissionIntensity = props.hasGPBREmissionScale ? props.gpbrEmissionScale : 1.0f;
                        if (ctx.debugOutput) Msg("[GPBR] Wrote emission: %s\n", path.c_str());
                    }
                }
            }
        }
    }
    
    result.success = true;
    if (ctx.debugOutput) {
        Msg("[GPBR] Complete: %s (skipped %d existing)\n", props.materialName.c_str(), result.skippedCount);
    }
    
    return result;
}

} // namespace GPBR
} // namespace ToPBR
} // namespace MaterialPipeline

#endif // _WIN64
