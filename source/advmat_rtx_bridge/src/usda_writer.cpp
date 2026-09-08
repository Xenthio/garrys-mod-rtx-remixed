#include "internal.h"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <iterator>
#include <locale>
#include <map>
#include <sstream>

namespace advmat::rtx_bridge::detail {
namespace {

bool InRange(const std::optional<float>& value, float minimum, float maximum,
             const char* name, std::string& error) {
    if (!value) {
        return true;
    }
    if (!std::isfinite(*value) || *value < minimum || *value > maximum) {
        error = std::string(name) + " must be finite and in [" +
            std::to_string(minimum) + ", " + std::to_string(maximum) + "]";
        return false;
    }
    return true;
}

bool VecInRange(const std::optional<Vec3>& value, float minimum, float maximum,
                const char* name, std::string& error) {
    if (!value) {
        return true;
    }
    if (!std::isfinite(value->x) || !std::isfinite(value->y) || !std::isfinite(value->z) ||
        value->x < minimum || value->x > maximum ||
        value->y < minimum || value->y > maximum ||
        value->z < minimum || value->z > maximum) {
        error = std::string(name) + " components must be finite and in [" +
            std::to_string(minimum) + ", " + std::to_string(maximum) + "]";
        return false;
    }
    return true;
}

bool IntInRange(const std::optional<int>& value, int minimum, int maximum,
                const char* name, std::string& error) {
    if (value && (*value < minimum || *value > maximum)) {
        error = std::string(name) + " must be in [" + std::to_string(minimum) +
            ", " + std::to_string(maximum) + "]";
        return false;
    }
    return true;
}

template <std::size_t Size>
bool IntInSet(const std::optional<int>& value, const int (&allowed)[Size],
              const char* name, std::string& error) {
    if (value && std::find(std::begin(allowed), std::end(allowed), *value) ==
                     std::end(allowed)) {
        error = std::string(name) + " is not a supported enum value";
        return false;
    }
    return true;
}

void Float(std::ostream& output, float value) {
    // max_digits10 avoids locale-dependent output and preserves the authored
    // float without littering the layer with unnecessary trailing zeroes.
    output << std::setprecision(9) << value;
}

void WriteFloat(std::ostream& output, const char* name,
                const std::optional<float>& value) {
    if (!value) {
        return;
    }
    output << "                float inputs:" << name << " = ";
    Float(output, *value);
    output << "\n";
}

void WriteInt(std::ostream& output, const char* name,
              const std::optional<int>& value) {
    if (value) {
        output << "                int inputs:" << name << " = " << *value << "\n";
    }
}

void WriteBool(std::ostream& output, const char* name,
               const std::optional<bool>& value) {
    if (value) {
        output << "                bool inputs:" << name << " = " << (*value ? 1 : 0) << "\n";
    }
}

void WriteColor(std::ostream& output, const char* name,
                const std::optional<Vec3>& value) {
    if (!value) {
        return;
    }
    output << "                color3f inputs:" << name << " = (";
    Float(output, value->x);
    output << ", ";
    Float(output, value->y);
    output << ", ";
    Float(output, value->z);
    output << ")\n";
}

const char* TextureInputName(TextureRole role) {
    switch (role) {
    case TextureRole::Albedo: return "diffuse_texture";
    case TextureRole::Normal: return "normalmap_texture";
    case TextureRole::Anisotropy: return "anisotropy_texture";
    case TextureRole::Roughness: return "reflectionroughness_texture";
    case TextureRole::Metallic: return "metallic_texture";
    case TextureRole::Height: return "height_texture";
    case TextureRole::Emissive: return "emissive_mask_texture";
    case TextureRole::SubsurfaceTransmittance: return "subsurface_transmittance_texture";
    case TextureRole::SubsurfaceThickness: return "subsurface_thickness_texture";
    case TextureRole::SubsurfaceScattering: return "subsurface_single_scattering_texture";
    case TextureRole::SubsurfaceRadius: return "subsurface_radius_texture";
    }
    return "diffuse_texture";
}

const char* TextureColorSpace(TextureRole role) {
    return (role == TextureRole::Albedo || role == TextureRole::Emissive ||
            role == TextureRole::SubsurfaceTransmittance ||
            role == TextureRole::SubsurfaceScattering) ? "sRGB" : "raw";
}

void WriteShader(std::ostream& output, const MaterialParameters& material,
                 const std::vector<ImportedTexture>& textures) {
    output << "                uniform asset info:mdl:sourceAsset = @AperturePBR_Opacity.mdl@\n";
    output << "                uniform token info:mdl:sourceAsset:subIdentifier = \"AperturePBR_Opacity\"\n";

    if (material.preloadTextures) {
        WriteBool(output, "preload_textures", material.preloadTextures);
    } else if (!textures.empty()) {
        // Protocol 1 requests predate an explicit preload flag. Preserve their
        // safe eager-loading behaviour only when the caller did not own it.
        // File replacements are not backed by Source's sampler-feedback stamp.
        output << "                bool inputs:preload_textures = 1\n";
    }
    if (material.normalEncoding) {
        WriteInt(output, "encoding", material.normalEncoding);
    } else if (std::any_of(textures.begin(), textures.end(), [](const auto& texture) {
        return texture.role == TextureRole::Normal;
    })) {
        // Older requests had no encoding field. Source and AdvMat-generated
        // RGB normals use DirectX tangent space in AperturePBR_Normal.
        output << "                int inputs:encoding = 2\n";
    }
    for (const auto& texture : textures) {
        output << "                asset inputs:" << TextureInputName(texture.role)
               << " = @" << texture.layerAssetPath << "@ (\n";
        output << "                    colorSpace = \"" << TextureColorSpace(texture.role) << "\"\n";
        output << "                )\n";
    }

    WriteColor(output, "diffuse_color_constant", material.albedo);
    WriteFloat(output, "opacity_constant", material.opacity);
    WriteFloat(output, "reflection_roughness_constant", material.roughness);
    WriteFloat(output, "metallic_constant", material.metallic);
    WriteColor(output, "emissive_color_constant", material.emissiveColor);
    WriteFloat(output, "emissive_intensity", material.emissiveIntensity);
    if (material.enableEmission) {
        WriteBool(output, "enable_emission", material.enableEmission);
    } else if (material.emissiveIntensity && *material.emissiveIntensity > 0.0f) {
        // Compatibility with requests created before enable_emission existed.
        output << "                bool inputs:enable_emission = 1\n";
    }
    WriteFloat(output, "anisotropy_constant", material.anisotropy);
    const bool hasThinFilmThickness = material.thinFilmThickness &&
        *material.thinFilmThickness > 0.0f;
    if (material.enableThinFilm) {
        WriteBool(output, "enable_thin_film", material.enableThinFilm);
    } else if (hasThinFilmThickness ||
        (material.alphaIsThinFilmThickness && *material.alphaIsThinFilmThickness)) {
        // Compatibility with requests created before enable_thin_film existed.
        output << "                bool inputs:enable_thin_film = 1\n";
    }
    if (hasThinFilmThickness) {
        WriteFloat(output, "thin_film_thickness_constant", material.thinFilmThickness);
    }
    WriteFloat(output, "displace_in", material.displaceIn);
    WriteFloat(output, "displace_out", material.displaceOut);
    WriteColor(output, "subsurface_transmittance_color",
               material.subsurfaceTransmittanceColor);
    WriteFloat(output, "subsurface_measurement_distance",
               material.subsurfaceMeasurementDistance);
    WriteColor(output, "subsurface_single_scattering_albedo",
               material.subsurfaceSingleScatteringAlbedo);
    WriteFloat(output, "subsurface_volumetric_anisotropy",
               material.subsurfaceVolumetricAnisotropy);
    WriteBool(output, "subsurface_diffusion_profile",
              material.subsurfaceDiffusionProfile);
    WriteColor(output, "subsurface_radius", material.subsurfaceRadius);
    WriteFloat(output, "subsurface_radius_scale", material.subsurfaceRadiusScale);
    WriteFloat(output, "subsurface_max_sample_radius",
               material.subsurfaceMaxSampleRadius);

    WriteInt(output, "sprite_sheet_rows", material.spriteSheetRows);
    WriteInt(output, "sprite_sheet_cols", material.spriteSheetColumns);
    WriteInt(output, "sprite_sheet_fps", material.spriteSheetFps);
    WriteInt(output, "filter_mode", material.filterMode);
    WriteInt(output, "wrap_mode_u", material.wrapModeU);
    WriteInt(output, "wrap_mode_v", material.wrapModeV);
    WriteBool(output, "ignore_material", material.ignoreMaterial);
    WriteBool(output, "thin_film_thickness_from_albedo_alpha",
              material.alphaIsThinFilmThickness);
    WriteBool(output, "use_legacy_alpha_state", material.useDrawCallAlphaState);
    if (material.blendEnabled) {
        WriteBool(output, "blend_enabled", material.blendEnabled);
    } else if (material.blendType) {
        // Compatibility with requests created before blend_enabled existed.
        output << "                bool inputs:blend_enabled = 1\n";
    }
    WriteInt(output, "blend_type", material.blendType);
    WriteBool(output, "inverted_blend", material.invertedBlend);
    WriteInt(output, "alpha_test_type", material.alphaTestType);
    WriteFloat(output, "alpha_test_reference_value", material.alphaReferenceValue);
}

} // namespace

bool ValidateMaterial(const MaterialParameters& material, std::string& error) {
    static constexpr int filterModes[]{0, 1};
    static constexpr int wrapModes[]{0, 1, 2, 3};
    static_assert(kTextureRoleCount ==
                  static_cast<std::size_t>(TextureRole::SubsurfaceRadius) + 1);
    if (material.textures.size() > kTextureRoleCount) {
        error = "at most one texture per supported map role is allowed";
        return false;
    }
    bool seenRoles[kTextureRoleCount]{};
    for (const auto& texture : material.textures) {
        const auto role = static_cast<std::size_t>(texture.role);
        if (role >= kTextureRoleCount || seenRoles[role]) {
            error = "texture map roles must be valid and unique";
            return false;
        }
        seenRoles[role] = true;
        if (texture.source.empty()) {
            error = std::string(TextureRoleName(texture.role)) + " texture source is empty";
            return false;
        }
        if (texture.operations.size() > kMaxTextureOperations) {
            error = "a texture map cannot contain more than " +
                std::to_string(kMaxTextureOperations) + " bake operations";
            return false;
        }
        for (const auto& operation : texture.operations) {
            if (!std::isfinite(operation.strength) || operation.strength < 0.0f ||
                operation.strength > 64.0f ||
                !std::isfinite(operation.color.x) || !std::isfinite(operation.color.y) ||
                !std::isfinite(operation.color.z) || operation.color.x < 0.0f ||
                operation.color.y < 0.0f || operation.color.z < 0.0f ||
                operation.color.x > 16.0f || operation.color.y > 16.0f ||
                operation.color.z > 16.0f) {
                error = "texture bake operation values are outside the supported range";
                return false;
            }
        }
    }

    return VecInRange(material.albedo, 0.0f, 1.0f, "albedo", error) &&
        InRange(material.opacity, 0.0f, 1.0f, "opacity", error) &&
        InRange(material.roughness, 0.0f, 1.0f, "roughness", error) &&
        InRange(material.metallic, 0.0f, 1.0f, "metallic", error) &&
        VecInRange(material.emissiveColor, 0.0f, 1.0f, "emissive color", error) &&
        InRange(material.emissiveIntensity, 0.0f, 65504.0f, "emissive intensity", error) &&
        InRange(material.anisotropy, -1.0f, 1.0f, "anisotropy", error) &&
        InRange(material.thinFilmThickness, 0.0f, 1500.0f,
                "thin film thickness", error) &&
        InRange(material.displaceIn, 0.0f, 0.2f, "displace in", error) &&
        InRange(material.displaceOut, 0.0f, 0.2f, "displace out", error) &&
        VecInRange(material.subsurfaceTransmittanceColor, 0.0f, 1.0f,
                   "subsurface transmittance color", error) &&
        InRange(material.subsurfaceMeasurementDistance, 0.0f, 16.0f,
                "subsurface measurement distance", error) &&
        VecInRange(material.subsurfaceSingleScatteringAlbedo, 0.0f, 1.0f,
                   "subsurface single scattering albedo", error) &&
        InRange(material.subsurfaceVolumetricAnisotropy, -0.99f, 0.99f,
                "subsurface volumetric anisotropy", error) &&
        VecInRange(material.subsurfaceRadius, 0.0f, 1.0f,
                   "subsurface radius", error) &&
        InRange(material.subsurfaceRadiusScale, 0.0f, 1000.0f,
                "subsurface radius scale", error) &&
        InRange(material.subsurfaceMaxSampleRadius, 0.0f, 65504.0f,
                "subsurface max sample radius", error) &&
        IntInRange(material.spriteSheetRows, 1, 255, "sprite sheet rows", error) &&
        IntInRange(material.spriteSheetColumns, 1, 255, "sprite sheet columns", error) &&
        IntInRange(material.spriteSheetFps, 0, 255, "sprite sheet fps", error) &&
        IntInSet(material.filterMode, filterModes, "filter mode", error) &&
        IntInSet(material.wrapModeU, wrapModes, "wrap mode U", error) &&
        IntInSet(material.wrapModeV, wrapModes, "wrap mode V", error) &&
        IntInRange(material.normalEncoding, 0, 2, "normal encoding", error) &&
        IntInRange(material.blendType, 0, 8, "blend type", error) &&
        IntInRange(material.alphaTestType, 0, 7, "alpha test type", error) &&
        InRange(material.alphaReferenceValue, 0.0f, 1.0f, "alpha reference value", error);
}

std::string BuildProfileLayer(const std::vector<std::string>& hashes,
                              const MaterialParameters& material,
                              const std::vector<ImportedTexture>& textures) {
    std::ostringstream output;
    output.imbue(std::locale::classic());
    output << "#usda 1.0\n(\n    upAxis = \"Z\"\n)\n\n";
    output << "over \"RootNode\"\n{\n    over \"Looks\"\n    {\n";
    for (const auto& hash : hashes) {
        output << "        over \"mat_" << hash << "\"\n";
        output << "        {\n            over \"Shader\"\n            {\n";
        WriteShader(output, material, textures);
        output << "            }\n        }\n\n";
    }
    output << "    }\n}\n";
    return output.str();
}

std::string BuildEmptyProfileLayer() {
    return "#usda 1.0\n(\n    upAxis = \"Z\"\n)\n\n"
           "over \"RootNode\"\n{\n    over \"Looks\"\n    {\n    }\n}\n";
}

std::string BuildModLayer(const std::vector<std::string>& profileIds) {
    std::ostringstream output;
    output << "#usda 1.0\n(\n";
    output << "    customLayerData = {\n";
    output << "        string lightspeed_game_name = \"Garry's Mod (x64)\"\n";
    output << "        string lightspeed_layer_type = \"replacement\"\n";
    output << "    }\n";
    output << "    metersPerUnit = 0.01\n";
    output << "    subLayers = [\n";
    for (std::size_t index = 0; index < profileIds.size(); ++index) {
        output << "        @./profiles/" << profileIds[index] << ".usda@";
        if (index + 1 < profileIds.size()) {
            output << ',';
        }
        output << "\n";
    }
    output << "    ]\n";
    output << "    timeCodesPerSecond = 24\n";
    output << "    upAxis = \"Z\"\n)\n";
    return output.str();
}

} // namespace advmat::rtx_bridge::detail

