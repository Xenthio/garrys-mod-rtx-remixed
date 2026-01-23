// =========================================================================
// legacy_texture_processor_usda.h - USD/USDA file generation
// =========================================================================
// Functions for generating RTX Remix mod.usda and materials.usda files
// =========================================================================

#pragma once

#ifdef _WIN64

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <cstdint>
#include <ostream>

// Include full header for TextureProcessor::ProcessedMaterialInfo type
#include "legacy_texture_processor.h"

namespace LegacyTextureProcessor {
namespace USDA {

// =========================================================================
// USDA Generation
// =========================================================================

// Check if there are new materials to write to the USDA file
// @param materialsUsdaPath - Path to materials.usda
// @param materialInfo - Map of processed materials
// @param existingHashes - Output: set of hashes already in the file
// @param newMaterialCount - Output: count of new materials to add
// @param debugOutput - Whether to output debug messages
// @return true if file was read successfully (or didn't exist)
bool CheckExistingMaterials(const std::string& materialsUsdaPath,
                            const std::unordered_map<uint64_t, TextureProcessor::ProcessedMaterialInfo>& materialInfo,
                            std::unordered_set<uint64_t>& existingHashes,
                            int& newMaterialCount,
                            bool debugOutput = false);

// Write the main mod.usda file (references materials.usda)
// @param modDir - Directory to write to
// @return true on success
bool WriteModUSDAFile(const std::string& modDir);

// Write material definitions to materials.usda
// @param modDir - Directory to write to
// @param outputDirectory - Base output directory for relative path calculation
// @param materialInfo - Map of processed materials
// @return true on success
bool WriteMaterialsUSDAFile(const std::string& modDir,
                            const std::string& outputDirectory,
                            const std::unordered_map<uint64_t, TextureProcessor::ProcessedMaterialInfo>& materialInfo,
                            bool debugOutput = false);

// Write a single glass material entry to the stream
// @param stream - Output stream
// @param hash - Material hash
// @param info - Material info
// @param outputDirectory - For relative path calculation
void WriteGlassMaterial(std::ostream& stream,
                        uint64_t hash,
                        const TextureProcessor::ProcessedMaterialInfo& info,
                        const std::string& outputDirectory);

// Write a single opaque material entry to the stream
// @param stream - Output stream  
// @param hash - Material hash
// @param info - Material info
// @param outputDirectory - For relative path calculation
void WriteOpaqueMaterial(std::ostream& stream,
                         uint64_t hash,
                         const TextureProcessor::ProcessedMaterialInfo& info,
                         const std::string& outputDirectory);

// =========================================================================
// Utility Functions
// =========================================================================

// Convert absolute path to relative path from mod directory
// @param absolutePath - Full path to texture file
// @param outputDir - Output directory for context
// @return Relative path like "./textures/HASH_type.dds"
std::string GetRelativeTexturePath(const std::string& absolutePath, const std::string& outputDir);

// Get mod directory from output directory (parent of textures folder)
// @param outputDirectory - Output directory containing textures subfolder
// @return Mod directory path
std::string GetModDirectory(const std::string& outputDirectory);

} // namespace USDA
} // namespace LegacyTextureProcessor

#endif // _WIN64
