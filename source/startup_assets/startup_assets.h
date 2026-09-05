#pragma once

#include <filesystem>
#include <optional>
#include <vector>
#include "vendor/json.hpp"

namespace astra::startup_assets {

// Performs no rendering or image conversion. The caller must finish this work
// before Remix creates its mod search paths. Explicit sources replace default
// addon discovery, but the game's own data_static directory is always included.
nlohmann::json Prepare(const std::filesystem::path& gameRoot,
    const std::optional<std::vector<std::filesystem::path>>& addonSources = std::nullopt);

// A runtime fallback can disable only the already prepared matching generation.
// Returns false on stale generation, absent receipt, unsafe path or write error.
bool DeactivateMap(const std::filesystem::path& gameRoot, const std::string& map,
                   const std::string& generation);

} // namespace astra::startup_assets
