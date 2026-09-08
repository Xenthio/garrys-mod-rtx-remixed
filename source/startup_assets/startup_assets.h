#pragma once

#include <filesystem>
#include <optional>
#include <vector>
#include "vendor/json.hpp"
#include "workshop_discovery.h"

namespace astra::startup_assets {

// Performs no rendering or image conversion. The caller must finish this work
// before Remix creates its mod search paths. Explicit sources replace default
// addon discovery, but the game's own data_static directory is always included.
// Each addon-directory source includes its loose data_static and immediate
// regular .gma children. Default discovery also includes installed, enabled
// Workshop subscriptions for the current Steam user. Linked paths are rejected.
nlohmann::json Prepare(const std::filesystem::path& gameRoot,
    const std::optional<std::vector<std::filesystem::path>>& addonSources = std::nullopt,
    const WorkshopOptions& workshopOptions = {});

// A runtime fallback can disable only the already prepared matching generation.
// Returns false on stale generation, absent receipt, unsafe path or write error.
bool DeactivateMap(const std::filesystem::path& gameRoot, const std::string& map,
                   const std::string& generation);

} // namespace astra::startup_assets
