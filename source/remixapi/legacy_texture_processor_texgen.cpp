#ifdef _WIN64

#include "legacy_texture_processor_texgen.h"
#include "legacy_texture_processor.h"
#include "legacy_texture_processor_vtf.h"
#include <tier0/dbg.h>
#include <filesystem.h>

#include <algorithm>
#include <cmath>
#include <fstream>

namespace LegacyTextureProcessor {
namespace TextureGen {

// =========================================================================
// DDS File Writing
// =========================================================================

bool WriteDDSHeader(std::ofstream& file, uint32_t width, uint32_t height, bool hasAlpha, uint32_t mipCount, bool debugOutput) {
    DDSHeader header = {};
    
    header.magic = DDS_MAGIC;
    header.size = 124;  // Size of header minus magic number
    header.flags = DDSD_CAPS | DDSD_HEIGHT | DDSD_WIDTH | DDSD_PIXELFORMAT | DDSD_LINEARSIZE | DDSD_MIPMAPCOUNT;
    header.height = height;
    header.width = width;
    header.pitchOrLinearSize = width * height * (hasAlpha ? 4 : 3);
    header.depth = 1;
    header.mipMapCount = mipCount;
    
    // Pixel format for RGBA8888 or RGB888
    header.pixelFormat.size = 32;
    header.pixelFormat.flags = DDPF_RGB | (hasAlpha ? DDPF_ALPHAPIXELS : 0);
    header.pixelFormat.rgbBitCount = hasAlpha ? 32 : 24;
    header.pixelFormat.rBitMask = 0x00FF0000;  // Red
    header.pixelFormat.gBitMask = 0x0000FF00;  // Green
    header.pixelFormat.bBitMask = 0x000000FF;  // Blue
    header.pixelFormat.aBitMask = hasAlpha ? 0xFF000000 : 0;  // Alpha
    
    header.caps = DDSCAPS_TEXTURE | DDSCAPS_MIPMAP | DDSCAPS_COMPLEX;
    
    file.write(reinterpret_cast<const char*>(&header), sizeof(header));
    return file.good();
}

bool WriteTextureToDDS(const ConvertedTexture& texture, const std::string& outputPath, bool debugOutput) {
    if (texture.pixelData.empty()) {
        Warning("[LegacyTextureProcessor] Cannot write empty texture\n");
        return false;
    }
    
    std::ofstream file(outputPath, std::ios::binary);
    if (!file.is_open()) {
        Warning("[LegacyTextureProcessor] Failed to open file for writing: %s\n", outputPath.c_str());
        return false;
    }
    
    // Calculate number of mip levels
    uint32_t mipCount = CalculateMipLevels(texture.width, texture.height);
    
    // Write DDS header with mipmap info
    if (!WriteDDSHeader(file, texture.width, texture.height, true, mipCount, debugOutput)) {
        Warning("[LegacyTextureProcessor] Failed to write DDS header\n");
        return false;
    }
    
    // Convert from RGBA to BGRA (DDS expects BGRA)
    std::vector<uint8_t> bgraData(texture.pixelData.size());
    for (size_t i = 0; i < texture.pixelData.size(); i += 4) {
        bgraData[i + 0] = texture.pixelData[i + 2];  // B <- R
        bgraData[i + 1] = texture.pixelData[i + 1];  // G <- G
        bgraData[i + 2] = texture.pixelData[i + 0];  // R <- B
        bgraData[i + 3] = texture.pixelData[i + 3];  // A <- A
    }
    
    // Write base mip level (level 0)
    file.write(reinterpret_cast<const char*>(bgraData.data()), bgraData.size());
    
    // Generate and write subsequent mip levels
    uint32_t mipWidth = texture.width;
    uint32_t mipHeight = texture.height;
    std::vector<uint8_t> currentMip = bgraData;
    
    for (uint32_t mip = 1; mip < mipCount; mip++) {
        uint32_t newWidth = max(1u, mipWidth / 2);
        uint32_t newHeight = max(1u, mipHeight / 2);
        
        std::vector<uint8_t> newMip(newWidth * newHeight * 4);
        
        // Box filter downscale (2x2 average)
        for (uint32_t y = 0; y < newHeight; y++) {
            for (uint32_t x = 0; x < newWidth; x++) {
                uint32_t srcX = x * 2;
                uint32_t srcY = y * 2;
                
                // Sample 2x2 block from source
                uint32_t r = 0, g = 0, b = 0, a = 0;
                int sampleCount = 0;
                
                for (int dy = 0; dy < 2 && (srcY + dy) < mipHeight; dy++) {
                    for (int dx = 0; dx < 2 && (srcX + dx) < mipWidth; dx++) {
                        size_t srcIdx = ((srcY + dy) * mipWidth + (srcX + dx)) * 4;
                        b += currentMip[srcIdx + 0];
                        g += currentMip[srcIdx + 1];
                        r += currentMip[srcIdx + 2];
                        a += currentMip[srcIdx + 3];
                        sampleCount++;
                    }
                }
                
                size_t dstIdx = (y * newWidth + x) * 4;
                newMip[dstIdx + 0] = static_cast<uint8_t>(b / sampleCount);
                newMip[dstIdx + 1] = static_cast<uint8_t>(g / sampleCount);
                newMip[dstIdx + 2] = static_cast<uint8_t>(r / sampleCount);
                newMip[dstIdx + 3] = static_cast<uint8_t>(a / sampleCount);
            }
        }
        
        // Write this mip level
        file.write(reinterpret_cast<const char*>(newMip.data()), newMip.size());
        
        // Prepare for next iteration
        currentMip = std::move(newMip);
        mipWidth = newWidth;
        mipHeight = newHeight;
    }
    
    if (!file.good()) {
        Warning("[LegacyTextureProcessor] Failed to write texture data\n");
        return false;
    }
    
    file.close();
    
    if (debugOutput) {
        Msg("[LegacyTextureProcessor] Wrote DDS file: %s (%dx%d, %d mips)\n", outputPath.c_str(), texture.width, texture.height, mipCount);
    }
    
    return true;
}

// =========================================================================
// Roughness Texture Generation
// =========================================================================

bool GenerateRoughnessTexture(const MaterialPBRProperties& props, ConvertedTexture& outTexture, IFileSystem* fileSystem, bool debugOutput) {
    // =========================================================================
    // ROUGHNESS SOURCE PRIORITY (reorganized with phong-first approach)
    // =========================================================================
    // PHONG MATERIALS ($phong=1): Use phong-specific properties first
    //   1. $phongexponenttexture - dedicated per-pixel phong exponent (BEST)
    //   2. $basemapalphaphongmask - base texture alpha as phong mask  
    //   3. $normalmapalphaenvmapmask - normal map alpha as phong mask
    //   4. $phong + $bumpmap - default Source Engine behavior (normal alpha)
    //
    // NON-PHONG MATERIALS: Use envmap-based properties
    //   5. $envmapmask - separate envmap mask texture
    //   6. $basealphaenvmapmask - base texture alpha as envmap mask
    //   7. Auto-discovered _mask/_spec textures
    //   8. $envmap + normal map alpha as last resort
    //   9. $envmap + base texture alpha as last resort
    //
    // If no valid source, return false -> use constant roughness in USDA
    // =========================================================================
    
    if (debugOutput) {
        Msg("[LegacyTextureProcessor] GenerateRoughnessTexture for %s:\n", props.materialName.c_str());
        Msg("  hasPhong=%d, hasPhongExpTex=%d (%s)\n", props.hasPhong, props.hasPhongExponentTexture, props.phongExponentTexturePath.c_str());
        Msg("  normMapAlphaEnvMapMask=%d, hasBaseMapAlphaPhongMask=%d\n", props.normalMapAlphaEnvMapMask, props.hasBaseMapAlphaPhongMask);
        Msg("  hasBump=%d (%s)\n", props.hasBumpMap, props.bumpMapPath.c_str());
        Msg("  hasEnvMapMask=%d (%s), hasBaseAlphaEnvMapMask=%d\n", props.hasEnvMapMask, props.envMapMaskPath.c_str(), props.hasBaseAlphaEnvMapMask);
        Msg("  hasEnvMap=%d, hasEnvMapTint=%d\n", props.hasEnvMap, props.hasEnvMapTint);
        Msg("  hasDiscoveredMask=%d (%s)\n", props.hasDiscoveredMask, props.discoveredMaskPath.c_str());
        Msg("  baseTexturePath=%s\n", props.baseTexturePath.c_str());
    }
    
    std::string vtfPath;
    bool useAlphaChannel = false;
    bool isPhongExponentTexture = false;
    bool isInvertedMask = false;  // For $basealphaenvmapmask where white=masked(matte), black=reflective(shiny)
    
    // =========================================================================
    // PHONG MATERIAL PATH - prioritize phong-specific properties
    // =========================================================================
    if (props.hasPhong) {
        // Priority 1: $phongexponenttexture (best quality - dedicated roughness data)
        if (props.hasPhongExponentTexture && !props.phongExponentTexturePath.empty()) {
            vtfPath = props.phongExponentTexturePath;
            useAlphaChannel = false;
            isPhongExponentTexture = true;
            if (debugOutput) {
                Msg("[LegacyTextureProcessor] %s: [PHONG] Using $phongexponenttexture (best quality)\n", props.materialName.c_str());
            }
        }
        // Priority 2: $basemapalphaphongmask - base texture alpha as phong mask
        else if (props.hasBaseMapAlphaPhongMask && !props.baseTexturePath.empty()) {
            vtfPath = props.baseTexturePath;
            useAlphaChannel = true;
            if (debugOutput) {
                Msg("[LegacyTextureProcessor] %s: [PHONG] Using base texture alpha ($basemapalphaphongmask)\n", props.materialName.c_str());
            }
        }
        // Priority 3: $normalmapalphaenvmapmask - normal map alpha as mask
        else if (props.normalMapAlphaEnvMapMask && props.hasBumpMap && !props.bumpMapPath.empty()) {
            vtfPath = props.bumpMapPath;
            useAlphaChannel = true;
            if (debugOutput) {
                Msg("[LegacyTextureProcessor] %s: [PHONG] Using normal map alpha ($normalmapalphaenvmapmask)\n", props.materialName.c_str());
            }
        }
        // Priority 4: Default Source Engine behavior - phong + bumpmap = normal alpha has phong mask
        else if (props.hasBumpMap && !props.bumpMapPath.empty()) {
            vtfPath = props.bumpMapPath;
            useAlphaChannel = true;
            if (debugOutput) {
                Msg("[LegacyTextureProcessor] %s: [PHONG] Using normal map alpha (default Source behavior)\n", props.materialName.c_str());
            }
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
            if (debugOutput) {
                Msg("[LegacyTextureProcessor] %s: Using $envmapmask for roughness\n", props.materialName.c_str());
            }
        }
        // Priority 6: $basealphaenvmapmask - base texture alpha as envmap mask (INVERTED!)
        else if (props.hasBaseAlphaEnvMapMask && !props.baseTexturePath.empty()) {
            vtfPath = props.baseTexturePath;
            useAlphaChannel = true;
            isInvertedMask = true;
            if (debugOutput) {
                Msg("[LegacyTextureProcessor] %s: Using base texture alpha ($basealphaenvmapmask - INVERTED)\n", props.materialName.c_str());
            }
        }
        // Priority 7: Auto-discovered _mask/_spec textures
        else if (props.hasDiscoveredMask && !props.discoveredMaskPath.empty()) {
            vtfPath = props.discoveredMaskPath;
            useAlphaChannel = false;  // Use the RGB channels of the discovered mask
            if (debugOutput) {
                Msg("[LegacyTextureProcessor] %s: Using auto-discovered mask/spec texture: %s\n", props.materialName.c_str(), props.discoveredMaskPath.c_str());
            }
        }
        // Priority 8: $envmap + normal map alpha (implicit $normalmapalphaenvmapmask)
        else if ((props.hasEnvMap || props.hasEnvMapTint) && props.hasBumpMap && !props.bumpMapPath.empty()) {
            vtfPath = props.bumpMapPath;
            useAlphaChannel = true;
            if (debugOutput) {
                Msg("[LegacyTextureProcessor] %s: Trying normal map alpha (implicit envmap roughness)\n", props.materialName.c_str());
            }
        }
        // Priority 9: $envmap + base texture alpha (implicit $basealphaenvmapmask)
        else if ((props.hasEnvMap || props.hasEnvMapTint) && !props.baseTexturePath.empty()) {
            vtfPath = props.baseTexturePath;
            useAlphaChannel = true;
            isInvertedMask = true;
            if (debugOutput) {
                Msg("[LegacyTextureProcessor] %s: Trying base texture alpha (implicit envmap - INVERTED)\n", props.materialName.c_str());
            }
        }
        // Priority 10 (LAST RESORT): Try normal map alpha anyway for materials with bumpmap
        else if (props.hasBumpMap && !props.bumpMapPath.empty()) {
            vtfPath = props.bumpMapPath;
            useAlphaChannel = true;
            if (debugOutput) {
                Msg("[LegacyTextureProcessor] %s: Last resort - trying normal map alpha\n", props.materialName.c_str());
            }
        }
    }
    
    // No valid roughness source found - use constant value in USDA
    if (vtfPath.empty()) {
        if (debugOutput) {
            Msg("[LegacyTextureProcessor] No roughness texture source for %s, will use constant %.2f\n",
                props.materialName.c_str(), props.roughness);
        }
        return false;
    }
    
    // Try to read the texture
    std::vector<uint8_t> fileData;
    
    if (debugOutput) {
        Msg("[LegacyTextureProcessor] Attempting to read texture for roughness: %s\n", vtfPath.c_str());
    }
    
    if (!VTF::ReadVTFFile(fileSystem, vtfPath, fileData, debugOutput)) {
        if (debugOutput) {
            Msg("[LegacyTextureProcessor] Failed to read VTF: %s (will use constant)\n", vtfPath.c_str());
        }
        return false;
    }
    
    VTFFileHeader header;
    if (!VTF::ParseVTFHeader(fileData, header, debugOutput)) {
        if (debugOutput) {
            Msg("[LegacyTextureProcessor] Failed to parse VTF header: %s\n", vtfPath.c_str());
        }
        return false;
    }
    
    ConvertedTexture sourceTex;
    if (!VTF::ExtractVTFPixelData(fileData, header, sourceTex, false, debugOutput)) {
        if (debugOutput) {
            Msg("[LegacyTextureProcessor] Failed to extract pixel data: %s\n", vtfPath.c_str());
        }
        return false;
    }
    
    // If using alpha channel, check if the alpha actually has variation
    // DXT1 textures only have 1-bit alpha (0 or 255), so they're not useful for masks
    if (useAlphaChannel) {
        bool hasAlphaVariation = false;
        uint8_t firstAlpha = sourceTex.pixelData.size() >= 4 ? sourceTex.pixelData[3] : 255;
        for (size_t i = 3; i < sourceTex.pixelData.size(); i += 4) {
            if (sourceTex.pixelData[i] != firstAlpha) {
                hasAlphaVariation = true;
                break;
            }
        }
        if (!hasAlphaVariation) {
            if (debugOutput) {
                Msg("[LegacyTextureProcessor] %s: Alpha channel has no variation (all %d), will use constant roughness\n", 
                    vtfPath.c_str(), firstAlpha);
            }
            return false;
        }
    }
    
    // Convert source texture to roughness
    outTexture.width = sourceTex.width;
    outTexture.height = sourceTex.height;
    outTexture.pixelData.resize(sourceTex.width * sourceTex.height * 4);
    
    for (size_t i = 0; i < sourceTex.pixelData.size(); i += 4) {
        uint8_t roughness;
        
        if (isPhongExponentTexture) {
            // $phongexponenttexture: pixel value is the phong exponent (0-255 maps to exponent)
            // Higher exponent = shinier = LOWER roughness
            // The texture typically uses the red channel (or all channels for grayscale)
            uint8_t exponentValue = sourceTex.pixelData[i];  // Red channel
            
            // Convert exponent to roughness using perceptual curve
            // Exponent 0 (value 0) = very rough (roughness ~0.85)
            // Exponent 255 (max) = very shiny (roughness ~0.20)
            float normalizedExp = exponentValue / 255.0f;
            // Use sqrt curve to maintain perceptual half-shininess at half-value
            float shininessPerceptual = sqrtf(normalizedExp);
            float roughnessF = 0.85f - (shininessPerceptual * 0.65f);  // 0->0.85, 255->0.20
            roughness = static_cast<uint8_t>(std::clamp(roughnessF * 255.0f, 0.0f, 255.0f));
        } else if (useAlphaChannel) {
            // Use alpha channel from normal map or base texture
            uint8_t sourceValue = sourceTex.pixelData[i + 3];
            
            // Handle inverted mask semantics for $basealphaenvmapmask
            // Normal masks (phong, $normalmapalphaenvmapmask): bright = shiny areas = LOW roughness
            // Inverted masks ($basealphaenvmapmask): bright = MASKED (matte), dark = reflective (shiny)
            if (isInvertedMask) {
                // $basealphaenvmapmask: white (255) = masked/no reflection/matte, black (0) = reflective/shiny
                // Invert the source value first, then apply the same curve
                sourceValue = 255 - sourceValue;
            }
            
            // The mask represents "shininess" intensity (0-255) after potential inversion
            // Half mask value should give HALF SHININESS perception, not half roughness
            // Since roughness is roughly inverse-square to shininess perception,
            // we apply the perceptual curve to the shininess value first
            //
            // Shininess perception: half mask (127) = half shininess
            // Map: mask 0 -> low shininess -> high roughness (0.85)
            //      mask 255 -> high shininess -> low roughness (0.20)
            // 
            // Use sqrt on the mask value to preserve perceptual half-shininess at half-mask
            float normalizedMask = sourceValue / 255.0f;
            // Apply sqrt curve so half-mask gives perceptually half-shiny appearance
            float shininessPerceptual = sqrtf(normalizedMask);
            // Map to roughness range: full shininess (1.0) -> 0.20, no shininess (0.0) -> 0.85
            float roughnessF = 0.85f - (shininessPerceptual * 0.65f);
            roughness = static_cast<uint8_t>(std::clamp(roughnessF * 255.0f, 0.0f, 255.0f));
        } else {
            // Use the red channel (envmap mask)
            // Envmap mask controls environment reflections - similar to phong mask
            uint8_t sourceValue = sourceTex.pixelData[i];
            
            // Same perceptual curve as phong mask
            float normalizedMask = sourceValue / 255.0f;
            float shininessPerceptual = sqrtf(normalizedMask);
            float roughnessF = 0.85f - (shininessPerceptual * 0.65f);  // 0->0.85, 255->0.20
            roughness = static_cast<uint8_t>(std::clamp(roughnessF * 255.0f, 0.0f, 255.0f));
        }
        
        outTexture.pixelData[i + 0] = roughness;
        outTexture.pixelData[i + 1] = roughness;
        outTexture.pixelData[i + 2] = roughness;
        outTexture.pixelData[i + 3] = 255;
    }
    
    outTexture.format = REMIXAPI_FORMAT_R8G8B8A8_UNORM;
    outTexture.mipLevels = 1;
    
    if (debugOutput) {
        const char* sourceType = isPhongExponentTexture ? "phong exponent texture" :
                                 useAlphaChannel ? "alpha channel (phong mask)" : "envmap mask";
        Msg("[LegacyTextureProcessor] Generated roughness from %s: %s (%dx%d)\n",
            sourceType, vtfPath.c_str(), outTexture.width, outTexture.height);
    }
    return true;
}

// =========================================================================
// Metallic Texture Generation
// =========================================================================

bool GenerateMetallicTexture(const MaterialPBRProperties& props, ConvertedTexture& outTexture, IFileSystem* fileSystem, bool debugOutput) {
    // Generate per-pixel metallic maps from base texture brightness
    // In Source Engine, dark areas + envmap = metallic (like chrome/metal parts)
    // Brighter areas = non-metallic (diffuse surfaces)
    
    // Only generate metallic map if material has envmap and average brightness suggests some metallic areas
    if (!props.hasEnvMap || props.baseTextureBrightness >= 0.4f || props.metallic <= 0.05f) {
        if (debugOutput) {
            Msg("[LegacyTextureProcessor] %s: No metallic texture needed (hasEnvMap=%d, avgBrightness=%.2f, metallic=%.2f)\n",
                props.materialName.c_str(), props.hasEnvMap ? 1 : 0, props.baseTextureBrightness, props.metallic);
        }
        return false;
    }
    
    // Read the base texture VTF
    std::vector<uint8_t> fileData;
    if (!VTF::ReadVTFFile(fileSystem, props.baseTexturePath, fileData, debugOutput)) {
        if (debugOutput) {
            Msg("[LegacyTextureProcessor] %s: Could not read base texture for metallic map generation\n",
                props.materialName.c_str());
        }
        return false;
    }
    
    // Parse VTF header
    VTFFileHeader header;
    if (!VTF::ParseVTFHeader(fileData, header, debugOutput)) {
        if (debugOutput) {
            Msg("[LegacyTextureProcessor] %s: Could not parse base texture VTF header for metallic map\n",
                props.materialName.c_str());
        }
        return false;
    }
    
    // Extract pixel data
    ConvertedTexture sourceTex;
    if (!VTF::ExtractVTFPixelData(fileData, header, sourceTex, false, debugOutput)) {
        if (debugOutput) {
            Msg("[LegacyTextureProcessor] %s: Could not extract base texture pixel data for metallic map\n",
                props.materialName.c_str());
        }
        return false;
    }
    
    // Generate metallic map from per-pixel brightness
    // Dark pixels (brightness < 0.3) = metallic (scaled), bright pixels = non-metallic
    outTexture.width = sourceTex.width;
    outTexture.height = sourceTex.height;
    outTexture.pixelData.resize(sourceTex.width * sourceTex.height * 4);
    
    // Metallic map: store metallic value in R channel (grayscale)
    // Pixels darker than threshold become metallic, brighter = non-metallic
    constexpr float METALLIC_THRESHOLD = 0.30f;  // Brightness below this is considered metallic
    
    for (uint32_t y = 0; y < sourceTex.height; y++) {
        for (uint32_t x = 0; x < sourceTex.width; x++) {
            size_t srcIdx = (y * sourceTex.width + x) * 4;
            size_t dstIdx = (y * sourceTex.width + x) * 4;
            
            uint8_t r = sourceTex.pixelData[srcIdx];
            uint8_t g = sourceTex.pixelData[srcIdx + 1];
            uint8_t b = sourceTex.pixelData[srcIdx + 2];
            
            // Calculate per-pixel brightness (luminance)
            float brightness = (0.299f * r + 0.587f * g + 0.114f * b) / 255.0f;
            
            // Calculate metallic value: darker = more metallic
            // brightness 0.0 -> metallic 1.0
            // brightness 0.3 -> metallic 0.0
            // brightness 0.3+ -> metallic 0.0
            float metallic = 0.0f;
            if (brightness < METALLIC_THRESHOLD) {
                metallic = std::clamp(1.0f - (brightness / METALLIC_THRESHOLD), 0.0f, 1.0f);
            }
            
            uint8_t metallicByte = static_cast<uint8_t>(metallic * 255.0f);
            
            // Store as grayscale (R=G=B=metallic, A=255)
            outTexture.pixelData[dstIdx] = metallicByte;
            outTexture.pixelData[dstIdx + 1] = metallicByte;
            outTexture.pixelData[dstIdx + 2] = metallicByte;
            outTexture.pixelData[dstIdx + 3] = 255;
        }
    }
    
    if (debugOutput) {
        Msg("[LegacyTextureProcessor] %s: Generated %dx%d per-pixel metallic map from base texture brightness\n",
            props.materialName.c_str(), sourceTex.width, sourceTex.height);
    }
    
    return true;
}

// =========================================================================
// Companion Texture Discovery
// =========================================================================

void DiscoverCompanionTextures(const std::string& baseTexturePath, MaterialPBRProperties& props, IFileSystem* fileSystem, bool autoDiscoverEnabled, bool debugOutput) {
    // Initialize discovered texture flags
    props.hasDiscoveredNormal = false;
    props.hasDiscoveredHeight = false;
    props.hasDiscoveredMask = false;
    props.hasDiscoveredAO = false;
    props.discoveredNormalPath = "";
    props.discoveredHeightPath = "";
    props.discoveredMaskPath = "";
    props.discoveredAOPath = "";
    
    if (baseTexturePath.empty() || !autoDiscoverEnabled) {
        return;
    }
    
    // Get the base path without extension
    std::string basePath = baseTexturePath;
    
    // Remove any .vtf extension if present
    size_t extPos = basePath.rfind(".vtf");
    if (extPos != std::string::npos) {
        basePath = basePath.substr(0, extPos);
    }
    extPos = basePath.rfind(".VTF");
    if (extPos != std::string::npos) {
        basePath = basePath.substr(0, extPos);
    }
    
    if (debugOutput) {
        Msg("[LegacyTextureProcessor] Searching for companion textures for: %s\n", basePath.c_str());
    }
    
    // List of suffix variants to check for each type
    // We'll try to read the VTF file to verify it exists
    
    // Helper lambda to check if a VTF texture exists
    auto TextureExists = [fileSystem, debugOutput](const std::string& texPath) -> bool {
        std::vector<uint8_t> fileData;
        return VTF::ReadVTFFile(fileSystem, texPath, fileData, debugOutput);
    };
    
    // Normal map variants - only discover if we don't already have a bumpmap
    if (!props.hasBumpMap) {
        const char* normalSuffixes[] = { "_normal", "_n", "_norm", "_nrm", "_Normal", "_N" };
        for (const char* suffix : normalSuffixes) {
            std::string candidatePath = basePath + suffix;
            if (TextureExists(candidatePath)) {
                props.discoveredNormalPath = candidatePath;
                props.hasDiscoveredNormal = true;
                if (debugOutput) {
                    Msg("[LegacyTextureProcessor] Discovered normal map: %s\n", candidatePath.c_str());
                }
                break;
            }
        }
    }
    
    // Height/parallax map variants - only discover if we don't already have one
    if (!props.hasParallaxMap) {
        const char* heightSuffixes[] = { "_height", "_h", "_bump", "_disp", "_Height", "_H" };
        for (const char* suffix : heightSuffixes) {
            std::string candidatePath = basePath + suffix;
            if (TextureExists(candidatePath)) {
                props.discoveredHeightPath = candidatePath;
                props.hasDiscoveredHeight = true;
                if (debugOutput) {
                    Msg("[LegacyTextureProcessor] Discovered height map: %s\n", candidatePath.c_str());
                }
                break;
            }
        }
    }
    
    // Mask/spec map variants (for roughness) - only discover if we don't have any roughness source
    if (!props.hasEnvMapMask && !props.hasPhongExponentTexture) {
        const char* maskSuffixes[] = { "_mask", "_spec", "_gloss", "_roughness", "_s", "_Mask", "_Spec" };
        for (const char* suffix : maskSuffixes) {
            std::string candidatePath = basePath + suffix;
            if (TextureExists(candidatePath)) {
                props.discoveredMaskPath = candidatePath;
                props.hasDiscoveredMask = true;
                if (debugOutput) {
                    Msg("[LegacyTextureProcessor] Discovered mask/spec map: %s\n", candidatePath.c_str());
                }
                break;
            }
        }
    }
    
    // AO (ambient occlusion) map variants
    const char* aoSuffixes[] = { "_ao", "_occlusion", "_AO", "_Occlusion" };
    for (const char* suffix : aoSuffixes) {
        std::string candidatePath = basePath + suffix;
        if (TextureExists(candidatePath)) {
            props.discoveredAOPath = candidatePath;
            props.hasDiscoveredAO = true;
            if (debugOutput) {
                Msg("[LegacyTextureProcessor] Discovered AO map: %s\n", candidatePath.c_str());
            }
            break;
        }
    }
}

} // namespace TextureGen
} // namespace LegacyTextureProcessor

#endif // _WIN64
