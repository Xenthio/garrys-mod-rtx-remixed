#pragma once

#include <string_view>

namespace MaterialPipeline {
namespace MaterialFilter {

constexpr char NormalizeMaterialNameChar(char c) {
    if (c == '\\') {
        return '/';
    }
    if (c >= 'A' && c <= 'Z') {
        return static_cast<char>(c - 'A' + 'a');
    }
    return c;
}

constexpr bool StartsWith(std::string_view value, std::string_view prefix) {
    if (value.size() < prefix.size()) {
        return false;
    }

    for (std::size_t i = 0; i < prefix.size(); ++i) {
        if (NormalizeMaterialNameChar(value[i]) !=
            NormalizeMaterialNameChar(prefix[i])) {
            return false;
        }
    }
    return true;
}

// These namespaces are rendered as 2D interface content rather than scene
// geometry. Feeding them into hash resolution and the material pipeline is
// both unnecessary and especially expensive when the spawn menu creates
// hundreds of icons at once.
constexpr bool IsNonSceneMaterialName(std::string_view materialName) {
    if (StartsWith(materialName, "materials/")) {
        materialName.remove_prefix(sizeof("materials/") - 1);
    }

    return StartsWith(materialName, "__") ||
           StartsWith(materialName, "engine/") ||
           StartsWith(materialName, "spawnicons/") ||
           StartsWith(materialName, "vgui/") ||
           StartsWith(materialName, "vgui_") ||
           StartsWith(materialName, "gui/") ||
           StartsWith(materialName, "console/");
}

static_assert(IsNonSceneMaterialName("spawnicons/models/props_c17/oildrum001"));
static_assert(IsNonSceneMaterialName("Materials\\VGUI\\HUD\\800corner1"));
static_assert(IsNonSceneMaterialName("__vgui_texture_0"));
static_assert(!IsNonSceneMaterialName("models/props_c17/oildrum001"));

} // namespace MaterialFilter
} // namespace MaterialPipeline
