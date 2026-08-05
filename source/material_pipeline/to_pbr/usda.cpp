// =========================================================================
// usda.cpp - USD/USDA file generation implementation
// =========================================================================
// Implementation of USD file generation for RTX Remix mods
// =========================================================================

#ifdef _WIN64

#include "usda.h"
#include "to_pbr.h"
#include <tier0/dbg.h>
#include <fstream>
#include <sstream>

namespace MaterialPipeline {
namespace ToPBR {
namespace USDA {

// =========================================================================
// ProcessedMaterialInfo Definition (needed for complete type)
// =========================================================================

// Note: ProcessedMaterialInfo is defined in the TextureProcessor class
// We need to reference it from the header

// =========================================================================
// Utility Functions
// =========================================================================

std::string GetRelativeTexturePath(const std::string& absolutePath, const std::string& outputDir) {
    // We want a path relative to the mod directory like "./textures/models_props_foo_type.dds"
    size_t texturesPos = absolutePath.find("textures");
    if (texturesPos != std::string::npos) {
        return "./" + absolutePath.substr(texturesPos);
    }
    // Fallback: just use filename
    size_t lastSlash = absolutePath.find_last_of("/\\");
    if (lastSlash != std::string::npos) {
        return "./textures/" + absolutePath.substr(lastSlash + 1);
    }
    return absolutePath;
}

std::string GetModDirectory(const std::string& outputDirectory) {
    std::string modDir = outputDirectory;
    size_t texturesPos = modDir.find("textures");
    if (texturesPos != std::string::npos && texturesPos > 0) {
        modDir = modDir.substr(0, texturesPos);
        // Remove trailing slash
        while (!modDir.empty() && (modDir.back() == '/' || modDir.back() == '\\')) {
            modDir.pop_back();
        }
    }
    return modDir;
}

// =========================================================================
// USDA Generation
// =========================================================================

bool CheckExistingMaterials(const std::string& materialsUsdaPath,
                            const std::unordered_map<uint64_t, TextureProcessor::ProcessedMaterialInfo>& materialInfo,
                            std::unordered_set<uint64_t>& existingHashes,
                            int& newMaterialCount,
                            bool debugOutput) {
    existingHashes.clear();
    newMaterialCount = 0;
    
    // Read existing materials.usda to find which hashes are already defined
    std::ifstream existingFile(materialsUsdaPath);
    if (existingFile.is_open()) {
        std::string line;
        while (std::getline(existingFile, line)) {
            // Look for lines like: over "mat_HASH"
            size_t matPos = line.find("over \"mat_");
            if (matPos != std::string::npos) {
                size_t hashStart = matPos + 10; // Length of 'over "mat_'
                size_t hashEnd = line.find("\"", hashStart);
                if (hashEnd != std::string::npos) {
                    std::string hashStr = line.substr(hashStart, hashEnd - hashStart);
                    uint64_t hash = 0;
                    if (sscanf(hashStr.c_str(), "%llX", (unsigned long long*)&hash) == 1) {
                        existingHashes.insert(hash);
                    }
                }
            }
        }
        existingFile.close();
        
        if (debugOutput && !existingHashes.empty()) {
            Msg("[USDA] Found %d existing material entries\n", (int)existingHashes.size());
        }
    }
    
    // Count new materials
    for (const auto& pair : materialInfo) {
        if (existingHashes.find(pair.first) == existingHashes.end()) {
            newMaterialCount++;
        }
    }
    
    return true;
}

bool LoadExistingHashes(const std::string& materialsUsdaPath,
                        std::unordered_set<uint64_t>& existingHashes,
                        bool debugOutput) {
    existingHashes.clear();
    
    // Read existing materials.usda to find which hashes are already defined
    std::ifstream existingFile(materialsUsdaPath);
    if (!existingFile.is_open()) {
        // File doesn't exist - that's OK, just means no existing hashes
        if (debugOutput) {
            Msg("[USDA] No existing materials.usda found at %s\n", materialsUsdaPath.c_str());
        }
        return true;
    }
    
    std::string line;
    while (std::getline(existingFile, line)) {
        // Look for lines like: over "mat_HASH"
        size_t matPos = line.find("over \"mat_");
        if (matPos != std::string::npos) {
            size_t hashStart = matPos + 10; // Length of 'over "mat_'
            size_t hashEnd = line.find("\"", hashStart);
            if (hashEnd != std::string::npos) {
                std::string hashStr = line.substr(hashStart, hashEnd - hashStart);
                uint64_t hash = 0;
                if (sscanf(hashStr.c_str(), "%llX", (unsigned long long*)&hash) == 1) {
                    existingHashes.insert(hash);
                }
            }
        }
    }
    existingFile.close();
    
    if (debugOutput) {
        Msg("[USDA] Loaded %zu existing material hashes from %s\n", 
            existingHashes.size(), materialsUsdaPath.c_str());
    }
    
    return true;
}

bool WriteModUSDAFile(const std::string& modDir) {
    std::string modUsdaPath = modDir + "/mod.usda";
    std::ofstream modUsda(modUsdaPath);
    if (!modUsda.is_open()) {
        Warning("[USDA] Failed to create mod.usda at %s\n", modUsdaPath.c_str());
        return false;
    }
    
    // Write USDA header
    modUsda << "#usda 1.0\n";
    modUsda << "(\n";
    modUsda << "    customLayerData = {\n";
    modUsda << "        string lightspeed_game_name = \"Garry's Mod (x64)\"\n";
    modUsda << "        string lightspeed_layer_type = \"replacement\"\n";
    modUsda << "    }\n";
    modUsda << "    metersPerUnit = 0.01\n";
    modUsda << "    subLayers = [\n";
    modUsda << "        @./materials.usda@\n";
    modUsda << "    ]\n";
    modUsda << "    timeCodesPerSecond = 24\n";
    modUsda << "    upAxis = \"Z\"\n";
    modUsda << ")\n\n";
    modUsda.close();
    
    return true;
}

void WriteGlassMaterial(std::ostream& stream,
                        uint64_t hash,
                        const TextureProcessor::ProcessedMaterialInfo& info,
                        const std::string& outputDirectory) {
    char hashStr[32];
    snprintf(hashStr, sizeof(hashStr), "%llX", (unsigned long long)hash);
    
    stream << "        over \"mat_" << hashStr << "\"\n";
    stream << "        {\n";
    stream << "            over \"Shader\"\n";
    stream << "            {\n";
    
    // Glass materials use AperturePBR_Translucent shader
    stream << "                uniform asset info:mdl:sourceAsset = @AperturePBR_Translucent.mdl@\n";
    stream << "                uniform token info:mdl:sourceAsset:subIdentifier = \"AperturePBR_Translucent\"\n";
    
    // Glass-specific properties
    stream << "                float inputs:ior_constant = " << info.ior << "\n";
    stream << "                bool inputs:thin_walled = 0\n";  // Use solid glass (false) for proper refraction, not thin_walled (true)
    
    // Roughness
    if (!info.roughnessPath.empty()) {
        std::string relPath = GetRelativeTexturePath(info.roughnessPath, outputDirectory);
        stream << "                asset inputs:reflectionroughness_texture = @" << relPath << "@ (\n";
        stream << "                    colorSpace = \"raw\"\n";
        stream << "                )\n";
    } else {
        float glassRoughness = (info.roughnessConstant >= 0.99f) ? 0.05f : info.roughnessConstant;
        stream << "                float inputs:reflection_roughness_constant = " << glassRoughness << "\n";
    }
    
    // Transmittance texture for colored/tinted glass
    if (!info.transmittancePath.empty()) {
        std::string relPath = GetRelativeTexturePath(info.transmittancePath, outputDirectory);
        stream << "                asset inputs:transmittance_texture = @" << relPath << "@ (\n";
        stream << "                    colorSpace = \"srgb\"\n";
        stream << "                )\n";
        stream << "                bool inputs:use_diffuse_layer = 1\n";
        
        // For Refract shaders: reduce diffuse opacity
        if (info.isRefractShader) {
            stream << "                float inputs:diffuse_color_constant_opacity = 0.3\n";
        }
    }
    
    // Normal map support for frosted/textured glass
    if (!info.normalPath.empty()) {
        std::string relPath = GetRelativeTexturePath(info.normalPath, outputDirectory);
        stream << "                int inputs:encoding = 0\n";  // octahedral
        stream << "                asset inputs:normalmap_texture = @" << relPath << "@ (\n";
        stream << "                    colorSpace = \"raw\"\n";
        stream << "                )\n";
    }
    
    stream << "            }\n";  // Close Shader
    stream << "        }\n\n";  // Close material
}

void WriteOpaqueMaterial(std::ostream& stream,
                         uint64_t hash,
                         const TextureProcessor::ProcessedMaterialInfo& info,
                         const std::string& outputDirectory) {
    char hashStr[32];
    snprintf(hashStr, sizeof(hashStr), "%llX", (unsigned long long)hash);
    
    stream << "        over \"mat_" << hashStr << "\"\n";
    stream << "        {\n";
    stream << "            over \"Shader\"\n";
    stream << "            {\n";
    
    // Standard opaque materials use AperturePBR_Opaque shader
    stream << "                uniform asset info:mdl:sourceAsset = @AperturePBR_Opaque.mdl@\n";
    
    // Albedo/Diffuse texture override (e.g., BFT metallic albedo reconstruction)
    if (!info.albedoPath.empty()) {
        std::string relPath = GetRelativeTexturePath(info.albedoPath, outputDirectory);
        stream << "                asset inputs:diffuse_texture = @" << relPath << "@ (\n";
        stream << "                    colorSpace = \"srgb\"\n";
        stream << "                )\n";
    }
    
    // Normal map
    if (!info.normalPath.empty()) {
        std::string relPath = GetRelativeTexturePath(info.normalPath, outputDirectory);
        stream << "                int inputs:encoding = 0\n";  // octahedral
        stream << "                asset inputs:normalmap_texture = @" << relPath << "@ (\n";
        stream << "                    colorSpace = \"raw\"\n";
        stream << "                )\n";
    }
    
    // Roughness
    if (!info.roughnessPath.empty()) {
        std::string relPath = GetRelativeTexturePath(info.roughnessPath, outputDirectory);
        stream << "                asset inputs:reflectionroughness_texture = @" << relPath << "@ (\n";
        stream << "                    colorSpace = \"raw\"\n";
        stream << "                )\n";
    } else {
        stream << "                float inputs:reflection_roughness_constant = " << info.roughnessConstant << "\n";
    }
    
    // Metallic
    if (!info.metallicPath.empty()) {
        std::string relPath = GetRelativeTexturePath(info.metallicPath, outputDirectory);
        stream << "                asset inputs:metallic_texture = @" << relPath << "@ (\n";
        stream << "                    colorSpace = \"raw\"\n";
        stream << "                )\n";
    } else {
        stream << "                float inputs:metallic_constant = " << info.metallicConstant << "\n";
    }
    
    // Height/Displacement map
    if (!info.heightPath.empty()) {
        std::string relPath = GetRelativeTexturePath(info.heightPath, outputDirectory);
        stream << "                asset inputs:height_texture = @" << relPath << "@ (\n";
        stream << "                    colorSpace = \"raw\"\n";
        stream << "                )\n";
        float displaceIn = info.heightScale * 0.5f;
        stream << "                float inputs:displace_in = " << displaceIn << "\n";
    }
    
    // Emissive/Self-illumination
    if (!info.emissivePath.empty()) {
        std::string relPath = GetRelativeTexturePath(info.emissivePath, outputDirectory);
        stream << "                asset inputs:emissive_mask_texture = @" << relPath << "@ (\n";
        stream << "                    colorSpace = \"srgb\"\n";
        stream << "                )\n";
        float emitIntensity = (info.emissionIntensity > 0.0f) ? info.emissionIntensity : 1.0f;
        stream << "                float inputs:emissive_intensity = " << emitIntensity << "\n";
    }
    
    stream << "            }\n";  // Close Shader
    stream << "        }\n\n";  // Close material
}

// Helper function to write fresh USDA content
static void WriteFreshUSDAContent(std::ostream& stream,
                                  const std::unordered_map<uint64_t, TextureProcessor::ProcessedMaterialInfo>& materialInfo,
                                  const std::string& outputDirectory) {
    stream << "#usda 1.0\n";
    stream << "(\n";
    stream << "    upAxis = \"Z\"\n";
    stream << ")\n\n";
    
    stream << "over \"RootNode\"\n";
    stream << "{\n";
    stream << "    over \"Looks\"\n";
    stream << "    {\n";
    
    for (const auto& pair : materialInfo) {
        const TextureProcessor::ProcessedMaterialInfo& info = pair.second;
        uint64_t hash = info.textureHash;
        
        if (info.isGlass) {
            WriteGlassMaterial(stream, hash, info, outputDirectory);
        } else {
            WriteOpaqueMaterial(stream, hash, info, outputDirectory);
        }
    }
    
    stream << "    }\n";  // Close Looks
    stream << "}\n";  // Close RootNode
}

bool WriteMaterialsUSDAFile(const std::string& modDir,
                            const std::string& outputDirectory,
                            const std::unordered_map<uint64_t, TextureProcessor::ProcessedMaterialInfo>& materialInfo,
                            bool debugOutput,
                            const std::string& overridePath) {
    // If overridePath is provided, write to that path (staging file).
    // Otherwise write directly to modDir/materials.usda.
    std::string materialsUsdaPath = overridePath.empty() ? (modDir + "/materials.usda") : overridePath;
    
    // When using a staging path, we still need to read the EXISTING materials.usda
    // (not the staging file) to know which hashes are already present.
    std::string existingFilePath = modDir + "/materials.usda";
    
    // First, read existing file content to preserve it
    std::string existingContent;
    std::unordered_set<uint64_t> existingHashes;
    bool fileExists = false;
    
    // Always read from the real materials.usda (not the staging path)
    LoadExistingHashes(existingFilePath, existingHashes, false);
    
    {
        std::ifstream existingFile(existingFilePath);
        if (existingFile.is_open()) {
            fileExists = true;
            std::stringstream buffer;
            buffer << existingFile.rdbuf();
            existingContent = buffer.str();
            existingFile.close();
            
            if (debugOutput && !existingHashes.empty()) {
                Msg("[USDA] Preserving %d existing material entries from file\n", (int)existingHashes.size());
            }
        }
    }
    
    // Count how many new materials we'll add
    int newMaterialCount = 0;
    for (const auto& pair : materialInfo) {
        if (existingHashes.find(pair.first) == existingHashes.end()) {
            newMaterialCount++;
        }
    }
    
    // If file exists and no new materials, skip writing
    if (fileExists && newMaterialCount == 0) {
        if (debugOutput) {
            Msg("[USDA] No new materials to add, preserving existing file\n");
        }
        return true;
    }
    
    std::ofstream materialsUsda(materialsUsdaPath);
    if (!materialsUsda.is_open()) {
        Warning("[USDA] Failed to create materials.usda at %s\n", materialsUsdaPath.c_str());
        return false;
    }
    
    // Decide whether to append or write fresh
    bool shouldAppend = fileExists && !existingContent.empty() && newMaterialCount > 0;
    
    if (shouldAppend) {
        // Find the closing brackets for the USDA structure.
        // The structure ends with:
        //     }  <-- closes "over Looks"
        // }  <-- closes "over RootNode"
        //
        // We search backwards from the end to find the RootNode closing brace first,
        // then find the Looks closing brace before it.
        
        bool validPosition = false;
        size_t closeLooksPos = std::string::npos;
        
        // Find the last '}' (should be RootNode close)
        size_t closeRootPos = existingContent.rfind('}');
        if (closeRootPos != std::string::npos && closeRootPos > 0) {
            // Find the second-to-last '}' (should be Looks close)
            size_t searchFrom = closeRootPos - 1;
            size_t closeLooksCandidate = existingContent.rfind('}', searchFrom);
            
            if (closeLooksCandidate != std::string::npos) {
                // Verify the structure: between Looks close and RootNode close should only be whitespace
                bool onlyWhitespace = true;
                for (size_t i = closeLooksCandidate + 1; i < closeRootPos; i++) {
                    char c = existingContent[i];
                    if (c != ' ' && c != '\t' && c != '\n' && c != '\r') {
                        onlyWhitespace = false;
                        break;
                    }
                }
                
                if (onlyWhitespace) {
                    // Find the start of the line containing the Looks close brace
                    // This is where we'll insert new materials
                    size_t lineStart = closeLooksCandidate;
                    while (lineStart > 0 && existingContent[lineStart - 1] != '\n') {
                        lineStart--;
                    }
                    closeLooksPos = lineStart;
                    validPosition = true;
                }
            }
        }
        
        if (validPosition) {
            // Write content up to the closing Looks line
            materialsUsda << existingContent.substr(0, closeLooksPos);
            
            // Write new materials
            for (const auto& pair : materialInfo) {
                const TextureProcessor::ProcessedMaterialInfo& info = pair.second;
                uint64_t hash = info.textureHash;
                
                // Skip if already in file
                if (existingHashes.find(hash) != existingHashes.end()) {
                    continue;
                }
                
                if (info.isGlass) {
                    WriteGlassMaterial(materialsUsda, hash, info, outputDirectory);
                } else {
                    WriteOpaqueMaterial(materialsUsda, hash, info, outputDirectory);
                }
            }
            
            // Write the closing brackets (from the Looks close line onwards)
            materialsUsda << existingContent.substr(closeLooksPos);
        } else {
            // Fallback: couldn't find proper structure, write fresh
            Warning("[USDA] Could not parse existing file structure, writing fresh\n");
            shouldAppend = false;
        }
    }
    
    if (!shouldAppend) {
        // Write fresh USDA file
        WriteFreshUSDAContent(materialsUsda, materialInfo, outputDirectory);
    }
    
    materialsUsda.close();
    
    if (debugOutput) {
        if (fileExists && shouldAppend) {
            Msg("[USDA] Appended %d new materials to materials.usda (total: %d existing + %d new)\n", 
                newMaterialCount, (int)existingHashes.size(), newMaterialCount);
        } else {
            Msg("[USDA] Wrote materials.usda with %d materials to %s\n", 
                (int)materialInfo.size(), modDir.c_str());
        }
    }
    
    return true;
}

} // namespace USDA
} // namespace ToPBR
} // namespace MaterialPipeline

#endif // _WIN64
