#pragma once

#include "advmat_rtx_bridge/bridge.h"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace advmat::rtx_bridge::detail {

struct ImportedTexture {
    TextureRole role;
    std::filesystem::path absolutePath;
    std::string layerAssetPath;
    bool changed = false;
};

struct TextureImportConfig {
    std::filesystem::path gameRoot;
    std::filesystem::path modDirectory;
    std::vector<std::filesystem::path> allowedRoots;
    std::uintmax_t maxBytes;
    std::uint32_t maxDimension;
};

Result ImportTexture(const TextureImportConfig& config,
                     const std::string& profileId,
                     const TextureInput& input,
                     ImportedTexture& output);

bool SupportsWicConversion() noexcept;
bool SupportsTextureOperations() noexcept;

bool IsDescendantOrSame(const std::filesystem::path& candidate,
                        const std::filesystem::path& root);
bool InspectPlainPath(const std::filesystem::path& path, bool requireDirectory,
                      bool& exists, std::string& error);
bool ValidatePlainDirectoryTree(const std::filesystem::path& root,
                                const std::filesystem::path& directory,
                                std::string& error);
bool EnsurePlainDirectoryTree(const std::filesystem::path& root,
                              const std::filesystem::path& directory,
                              std::string& error);
bool RemovePlainDirectoryTree(const std::filesystem::path& directory,
                              std::string& error);
bool PrunePlainDirectoryTree(const std::filesystem::path& directory,
                             const std::vector<std::filesystem::path>& keepFiles,
                             std::string& error);
bool AtomicWriteText(const std::filesystem::path& destination,
                     const std::string& contents,
                     std::string& error);
bool AtomicWriteBytes(const std::filesystem::path& destination,
                      const std::vector<std::uint8_t>& contents,
                      std::string& error);
bool AtomicReplacePath(const std::filesystem::path& source,
                       const std::filesystem::path& destination,
                       std::string& error);
std::filesystem::path MakeTemporarySibling(const std::filesystem::path& destination);

std::string BuildProfileLayer(const std::vector<std::string>& hashes,
                              const MaterialParameters& material,
                              const std::vector<ImportedTexture>& textures);
std::string BuildEmptyProfileLayer();
std::string BuildModLayer(const std::vector<std::string>& profileIds);

bool ValidateMaterial(const MaterialParameters& material, std::string& error);

} // namespace advmat::rtx_bridge::detail

