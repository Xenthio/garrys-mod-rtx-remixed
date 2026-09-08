#pragma once
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>
#include "vendor/json.hpp"

namespace astra::startup_assets {

struct WorkshopOptions {
    bool enabled = true;
    std::optional<std::filesystem::path> steamRoot;
    std::optional<std::string> accountId;
};

// Reads only the active account's enabled subscriptions and corresponding
// completed install records. It never initializes Steam or scans cached items
// that are absent from that subscription inventory.
std::vector<std::filesystem::path> DiscoverWorkshopSources(
    const std::filesystem::path& gameRoot, const WorkshopOptions& options,
    const std::function<std::optional<std::string>(const std::filesystem::path&)>& readMetadata,
    nlohmann::json& report);

}
