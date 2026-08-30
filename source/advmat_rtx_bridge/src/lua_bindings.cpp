#include "advmat_rtx_bridge/lua_bindings.h"
#include "advmat_rtx_bridge/bridge.h"

#include <GarrysMod/Lua/Interface.h>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

#include <algorithm>
#include <cctype>
#include <cmath>
#include <exception>
#include <filesystem>
#include <iomanip>
#include <memory>
#include <mutex>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

using GarrysMod::Lua::ILuaBase;
namespace Type = GarrysMod::Lua::Type;

namespace advmat::rtx_bridge {
namespace {

std::mutex g_bridgeMutex;
std::unique_ptr<Bridge> g_bridge;

bool PushTableField(ILuaBase* lua, int table, const char* name, int& absoluteIndex) {
    lua->GetField(table, name);
    if (!lua->IsType(-1, Type::Table)) {
        lua->Pop();
        return false;
    }
    absoluteIndex = lua->Top();
    return true;
}

bool ParseUtf8Path(const std::string& value, std::filesystem::path& output,
                   std::string& error) {
    if (value.empty() || value.size() > 2048) {
        error = "texture source path must contain 1 to 2048 UTF-8 bytes";
        return false;
    }
#ifdef _WIN32
    const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), nullptr, 0);
    if (length <= 0) {
        error = "texture source path is not valid UTF-8";
        return false;
    }
    std::wstring wide(static_cast<std::size_t>(length), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
            static_cast<int>(value.size()), wide.data(), length) != length) {
        error = "texture source path could not be converted to a Windows path";
        return false;
    }
    output = std::filesystem::path(wide);
#else
    output = std::filesystem::path(value);
#endif
    return true;
}

std::string PathToUtf8(const std::filesystem::path& value) {
#ifdef _WIN32
    const auto& wide = value.native();
    if (wide.empty()) return {};
    if (wide.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return {};
    }
    const auto count = static_cast<int>(wide.size());
    const int length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
        wide.data(), count, nullptr, 0, nullptr, nullptr);
    if (length <= 0) return {};
    std::string utf8(static_cast<std::size_t>(length), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
            wide.data(), count, utf8.data(), length, nullptr, nullptr) != length) {
        return {};
    }
    return utf8;
#else
    return value.u8string();
#endif
}

bool ReadString(ILuaBase* lua, int table, const char* name,
                std::string& output, bool& present, std::string& error) {
    lua->GetField(table, name);
    present = !lua->IsType(-1, Type::Nil);
    if (!present) {
        lua->Pop();
        return true;
    }
    if (!lua->IsType(-1, Type::String)) {
        error = std::string(name) + " must be a string";
        lua->Pop();
        return false;
    }
    output = lua->GetString(-1);
    lua->Pop();
    return true;
}

bool ReadNumber(ILuaBase* lua, int table, const char* name,
                std::optional<float>& output, std::string& error) {
    lua->GetField(table, name);
    if (lua->IsType(-1, Type::Nil)) {
        lua->Pop();
        return true;
    }
    if (!lua->IsType(-1, Type::Number)) {
        error = std::string(name) + " must be a number";
        lua->Pop();
        return false;
    }
    output = static_cast<float>(lua->GetNumber(-1));
    lua->Pop();
    return true;
}

bool ReadInteger(ILuaBase* lua, int table, const char* name,
                 std::optional<int>& output, std::string& error) {
    lua->GetField(table, name);
    if (lua->IsType(-1, Type::Nil)) {
        lua->Pop();
        return true;
    }
    if (!lua->IsType(-1, Type::Number)) {
        error = std::string(name) + " must be a number";
        lua->Pop();
        return false;
    }
    const double value = lua->GetNumber(-1);
    lua->Pop();
    if (!std::isfinite(value) || std::floor(value) != value ||
        value < static_cast<double>(std::numeric_limits<int>::min()) ||
        value > static_cast<double>(std::numeric_limits<int>::max())) {
        error = std::string(name) + " must be an integer";
        return false;
    }
    output = static_cast<int>(value);
    return true;
}

struct EnumEntry { const char* name; int value; };

bool ReadEnum(ILuaBase* lua, int table, const char* name,
              const EnumEntry* entries, std::size_t entryCount,
              std::optional<int>& output, std::string& error,
              bool emptyFirstEntry = false) {
    lua->GetField(table, name);
    if (lua->IsType(-1, Type::Nil)) {
        lua->Pop();
        return true;
    }
    if (lua->IsType(-1, Type::Number)) {
        const auto value = lua->GetNumber(-1);
        lua->Pop();
        if (!std::isfinite(value) || std::floor(value) != value ||
            value < std::numeric_limits<int>::min() ||
            value > std::numeric_limits<int>::max()) {
            error = std::string(name) + " numeric value must be an integer";
            return false;
        }
        output = static_cast<int>(value);
        return true;
    }
    if (!lua->IsType(-1, Type::String)) {
        error = std::string(name) + " must be a supported name or integer";
        lua->Pop();
        return false;
    }
    std::string value = lua->GetString(-1);
    lua->Pop();
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    for (std::size_t index = 0; index < entryCount; ++index) {
        if (value == entries[index].name) {
            if (emptyFirstEntry && index == 0) output.reset();
            else output = entries[index].value;
            return true;
        }
    }
    error = std::string(name) + " contains unsupported value " + value;
    return false;
}

bool ReadAlphaReference(ILuaBase* lua, int table, const char* name,
                        std::optional<float>& output, std::string& error) {
    std::optional<float> value;
    if (!ReadNumber(lua, table, name, value, error) || !value) return error.empty();
    if (!std::isfinite(*value) || *value < 0.0f || *value > 255.0f) {
        error = std::string(name) + " must be in [0, 1] or an integer byte in [0, 255]";
        return false;
    }
    if (*value <= 1.0f) {
        output = *value;
    } else if (std::floor(*value) == *value) {
        // RemixMaterial.CreateOpaqueMaterial exposes this compatibility field
        // as a byte. Normalize that secondary request shape to the float the
        // installed AperturePBR MDL actually declares.
        output = *value / 255.0f;
    } else {
        error = std::string(name) + " values above 1 must be integer bytes";
        return false;
    }
    return true;
}

bool ReadBoolean(ILuaBase* lua, int table, const char* name,
                 std::optional<bool>& output, std::string& error) {
    lua->GetField(table, name);
    if (lua->IsType(-1, Type::Nil)) {
        lua->Pop();
        return true;
    }
    if (!lua->IsType(-1, Type::Bool)) {
        error = std::string(name) + " must be a boolean";
        lua->Pop();
        return false;
    }
    output = lua->GetBool(-1);
    lua->Pop();
    return true;
}

bool ReadVector(ILuaBase* lua, int table, const char* name,
                std::optional<Vec3>& output, std::string& error) {
    int vectorTable = 0;
    lua->GetField(table, name);
    if (lua->IsType(-1, Type::Nil)) {
        lua->Pop();
        return true;
    }
    if (!lua->IsType(-1, Type::Table)) {
        error = std::string(name) + " must be a vector/color table";
        lua->Pop();
        return false;
    }
    vectorTable = lua->Top();
    Vec3 value;
    const char* xyz[3]{"x", "y", "z"};
    const char* rgb[3]{"r", "g", "b"};
    float* components[3]{&value.x, &value.y, &value.z};
    for (int component = 0; component < 3; ++component) {
        lua->GetField(vectorTable, xyz[component]);
        if (lua->IsType(-1, Type::Nil)) {
            lua->Pop();
            lua->GetField(vectorTable, rgb[component]);
        }
        if (!lua->IsType(-1, Type::Number)) {
            error = std::string(name) + " requires numeric x/y/z or r/g/b components";
            lua->Pop(2);
            return false;
        }
        *components[component] = static_cast<float>(lua->GetNumber(-1));
        lua->Pop();
    }
    lua->Pop();
    output = value;
    return true;
}

std::uint64_t StableHash(std::string value) {
    std::replace(value.begin(), value.end(), '\\', '/');
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    std::uint64_t hash = 14695981039346656037ull;
    for (const unsigned char character : value) {
        hash ^= character;
        hash *= 1099511628211ull;
    }
    return hash;
}

std::string DerivedProfileId(ILuaBase* lua, int request, std::string& error) {
    std::string identity;
    bool present = false;
    int target = 0;
    if (PushTableField(lua, request, "target", target)) {
        if (!ReadString(lua, target, "material", identity, present, error)) {
            lua->Pop();
            return {};
        }
        lua->Pop();
    }
    if (!error.empty()) return {};

    if (identity.empty()) {
        int compiled = 0;
        if (PushTableField(lua, request, "compiled", compiled)) {
            if (!ReadString(lua, compiled, "profile_id", identity, present, error)) {
                lua->Pop();
                return {};
            }
            lua->Pop();
        }
    }
    if (identity.empty()) {
        int profile = 0;
        if (PushTableField(lua, request, "profile", profile)) {
            if (!ReadString(lua, profile, "profile_id", identity, present, error)) {
                lua->Pop();
                return {};
            }
            lua->Pop();
        }
    }
    if (identity.empty()) {
        error = "request.target.material is required to identify an owned replacement";
        return {};
    }
    std::ostringstream id;
    id << "material_" << std::uppercase << std::hex << std::setfill('0')
       << std::setw(16) << StableHash(identity);
    return id.str();
}

bool ParseHashes(ILuaBase* lua, int request, std::vector<std::string>& hashes,
                 std::string& error) {
    int table = 0;
    if (!PushTableField(lua, request, "hashes", table)) {
        error = "request.hashes must be an array of hexadecimal strings";
        return false;
    }
    const auto count = lua->ObjLen(table);
    if (count == 0 || count > 128) {
        error = "request.hashes must contain one to 128 entries";
        lua->Pop();
        return false;
    }
    hashes.reserve(count);
    for (int index = 1; index <= count; ++index) {
        lua->PushNumber(static_cast<double>(index));
        lua->GetTable(table);
        if (!lua->IsType(-1, Type::String)) {
            error = "every request.hashes entry must be a hexadecimal string";
            lua->Pop(2);
            return false;
        }
        hashes.emplace_back(lua->GetString(-1));
        lua->Pop();
    }
    lua->Pop();
    return true;
}

std::optional<TextureRole> ParseRole(const std::string& value) {
    if (value == "albedo" || value == "base_color" || value == "basecolor") return TextureRole::Albedo;
    if (value == "normal") return TextureRole::Normal;
    if (value == "anisotropy") return TextureRole::Anisotropy;
    if (value == "roughness") return TextureRole::Roughness;
    if (value == "metallic" || value == "metalness") return TextureRole::Metallic;
    if (value == "height" || value == "displacement") return TextureRole::Height;
    if (value == "emissive" || value == "emission") return TextureRole::Emissive;
    if (value == "subsurface_transmittance") return TextureRole::SubsurfaceTransmittance;
    if (value == "subsurface_thickness") return TextureRole::SubsurfaceThickness;
    if (value == "subsurface_scattering") return TextureRole::SubsurfaceScattering;
    if (value == "subsurface_radius") return TextureRole::SubsurfaceRadius;
    return std::nullopt;
}

bool ParseOperation(ILuaBase* lua, int table, TextureOperation& operation,
                    std::string& error) {
    std::string kind;
    bool present = false;
    if (!ReadString(lua, table, "op", kind, present, error) || !present) {
        if (error.empty()) error = "texture operation requires an op string";
        return false;
    }
    std::transform(kind.begin(), kind.end(), kind.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    if (kind == "invert") {
        operation.kind = TextureOperationKind::Invert;
    } else if (kind == "multiply") {
        operation.kind = TextureOperationKind::Multiply;
    } else if (kind == "normal_from_height") {
        operation.kind = TextureOperationKind::NormalFromHeight;
    } else {
        error = "unsupported texture operation: " + kind;
        return false;
    }

    std::optional<float> strength;
    if (!ReadNumber(lua, table, "strength", strength, error)) return false;
    if (strength) operation.strength = *strength;

    std::string channel;
    if (!ReadString(lua, table, "channel", channel, present, error)) return false;
    if (present) {
        std::transform(channel.begin(), channel.end(), channel.begin(), [](unsigned char character) {
            return static_cast<char>(std::tolower(character));
        });
        if (channel == "luminance") operation.channel = HeightChannel::Luminance;
        else if (channel == "r") operation.channel = HeightChannel::Red;
        else if (channel == "g") operation.channel = HeightChannel::Green;
        else if (channel == "b") operation.channel = HeightChannel::Blue;
        else if (channel == "a") operation.channel = HeightChannel::Alpha;
        else {
            error = "normal_from_height channel must be luminance, r, g, b or a";
            return false;
        }
    }

    std::optional<Vec3> color;
    if (!ReadVector(lua, table, "color", color, error)) return false;
    if (color) {
        operation.color = *color;
    } else {
        std::optional<float> factor;
        if (!ReadNumber(lua, table, "factor", factor, error)) return false;
        if (!factor && !ReadNumber(lua, table, "value", factor, error)) return false;
        if (factor) operation.color = {*factor, *factor, *factor};
    }
    return true;
}

bool ParseMapValue(ILuaBase* lua, int valueIndex, TextureRole role,
                   TextureInput& texture, std::string& error) {
    texture.role = role;
    if (lua->IsType(valueIndex, Type::String)) {
        return ParseUtf8Path(lua->GetString(valueIndex), texture.source, error);
    }
    if (!lua->IsType(valueIndex, Type::Table)) {
        error = std::string(TextureRoleName(role)) + " map must be a path or descriptor table";
        return false;
    }
    std::string source;
    bool present = false;
    if (!ReadString(lua, valueIndex, "generatedFile", source, present, error)) return false;
    if (!present) {
        if (!ReadString(lua, valueIndex, "source", source, present, error) || !present) {
            if (error.empty()) error = std::string(TextureRoleName(role)) + " map descriptor requires source";
            return false;
        }
    }
    if (!ParseUtf8Path(source, texture.source, error)) return false;

    int operations = 0;
    if (PushTableField(lua, valueIndex, "ops", operations)) {
        const auto count = lua->ObjLen(operations);
        if (count > static_cast<int>(kMaxTextureOperations)) {
            error = "texture map cannot contain more than " +
                std::to_string(kMaxTextureOperations) + " operations";
            lua->Pop();
            return false;
        }
        for (int index = 1; index <= count; ++index) {
            lua->PushNumber(static_cast<double>(index));
            lua->GetTable(operations);
            if (!lua->IsType(-1, Type::Table)) {
                error = "texture operations must be tables";
                lua->Pop(2);
                return false;
            }
            const int operationTable = lua->Top();
            TextureOperation operation;
            if (!ParseOperation(lua, operationTable, operation, error)) {
                lua->Pop(2);
                return false;
            }
            texture.operations.push_back(operation);
            lua->Pop();
        }
        lua->Pop();
    }
    return true;
}

bool ParseMaps(ILuaBase* lua, int table, MaterialParameters& material,
               std::string& error) {
    static const char* roles[]{"albedo", "normal", "anisotropy", "roughness",
                               "metallic", "height", "emissive",
                               "subsurface_transmittance", "subsurface_thickness",
                               "subsurface_scattering", "subsurface_radius"};
    static_assert(sizeof(roles) / sizeof(roles[0]) == kTextureRoleCount);
    for (const char* roleName : roles) {
        lua->GetField(table, roleName);
        if (lua->IsType(-1, Type::Nil)) {
            lua->Pop();
            continue;
        }
        TextureInput texture;
        if (!ParseMapValue(lua, lua->Top(), *ParseRole(roleName), texture, error)) {
            lua->Pop();
            return false;
        }
        material.textures.push_back(std::move(texture));
        lua->Pop();
    }
    return true;
}

bool ParsePbr(ILuaBase* lua, int pbr, MaterialParameters& material,
              std::string& error) {
    int constants = 0;
    if (PushTableField(lua, pbr, "constants", constants)) {
        const bool ok =
            ReadVector(lua, constants, "albedo", material.albedo, error) &&
            ReadNumber(lua, constants, "opacity", material.opacity, error) &&
            ReadNumber(lua, constants, "roughness", material.roughness, error) &&
            ReadNumber(lua, constants, "metallic", material.metallic, error) &&
            ReadVector(lua, constants, "emissive_color", material.emissiveColor, error) &&
            ReadNumber(lua, constants, "emissive_intensity", material.emissiveIntensity, error) &&
            ReadNumber(lua, constants, "anisotropy", material.anisotropy, error) &&
            ReadNumber(lua, constants, "thin_film_thickness", material.thinFilmThickness, error) &&
            ReadNumber(lua, constants, "displace_in", material.displaceIn, error) &&
            ReadNumber(lua, constants, "displace_out", material.displaceOut, error) &&
            ReadVector(lua, constants, "subsurface_transmittance_color",
                       material.subsurfaceTransmittanceColor, error) &&
            ReadNumber(lua, constants, "subsurface_measurement_distance",
                       material.subsurfaceMeasurementDistance, error) &&
            ReadVector(lua, constants, "subsurface_single_scattering_albedo",
                       material.subsurfaceSingleScatteringAlbedo, error) &&
            ReadNumber(lua, constants, "subsurface_volumetric_anisotropy",
                       material.subsurfaceVolumetricAnisotropy, error) &&
            ReadVector(lua, constants, "subsurface_radius",
                       material.subsurfaceRadius, error) &&
            ReadNumber(lua, constants, "subsurface_radius_scale",
                       material.subsurfaceRadiusScale, error) &&
            ReadNumber(lua, constants, "subsurface_max_sample_radius",
                       material.subsurfaceMaxSampleRadius, error);
        lua->Pop();
        if (!ok) return false;
        // Zero is the schema's explicit "thin film disabled" sentinel. The
        // Remix API only has an optional positive thickness value.
        if (material.thinFilmThickness && *material.thinFilmThickness == 0.0f) {
            material.thinFilmThickness.reset();
        }
    }
    int maps = 0;
    if (PushTableField(lua, pbr, "maps", maps)) {
        const bool ok = ParseMaps(lua, maps, material, error);
        lua->Pop();
        if (!ok) return false;
    }
    int options = 0;
    if (PushTableField(lua, pbr, "options", options)) {
        static const EnumEntry filterModes[]{
            {"nearest", 0}, {"linear", 1},
        };
        static const EnumEntry wrapModes[]{
            {"clamp", 0}, {"repeat", 1}, {"mirror", 2}, {"clip", 3},
        };
        static const EnumEntry normalEncodings[]{
            {"octahedral", 0}, {"tangent_space_ogl", 1}, {"tangent_space_dx", 2},
        };
        // Opaque is represented by no authored blend type; any explicit type
        // enables blending in the generated layer.
        static const EnumEntry blendTypes[]{
            {"opaque", 0},
            {"alpha", 0},
            {"alpha_emissive", 1},
            {"reverse_alpha_emissive", 2},
            {"color", 3},
            {"color_emissive", 4},
            {"reverse_color_emissive", 5},
            {"emissive", 6},
            {"multiplicative", 7},
            {"double_multiplicative", 8},
        };
        static const EnumEntry alphaTests[]{
            {"none", 0}, {"always", 0}, {"never", 1}, {"less", 2},
            {"equal", 3}, {"less_equal", 4}, {"greater", 5},
            {"not_equal", 6}, {"greater_equal", 7},
        };
        const bool ok =
            ReadInteger(lua, options, "sprite_sheet_rows", material.spriteSheetRows, error) &&
            ReadInteger(lua, options, "sprite_sheet_columns", material.spriteSheetColumns, error) &&
            ReadInteger(lua, options, "sprite_sheet_fps", material.spriteSheetFps, error) &&
            ReadEnum(lua, options, "filter_mode", filterModes, 2, material.filterMode, error) &&
            ReadEnum(lua, options, "wrap_u", wrapModes, 4, material.wrapModeU, error) &&
            ReadEnum(lua, options, "wrap_v", wrapModes, 4, material.wrapModeV, error) &&
            ReadEnum(lua, options, "normal_encoding", normalEncodings, 3,
                     material.normalEncoding, error) &&
            ReadBoolean(lua, options, "enable_thin_film", material.enableThinFilm, error) &&
            ReadBoolean(lua, options, "enable_emission", material.enableEmission, error) &&
            ReadBoolean(lua, options, "preload_textures", material.preloadTextures, error) &&
            ReadBoolean(lua, options, "ignore_material", material.ignoreMaterial, error) &&
            ReadBoolean(lua, options, "alpha_is_thin_film", material.alphaIsThinFilmThickness, error) &&
            ReadBoolean(lua, options, "use_draw_call_alpha", material.useDrawCallAlphaState, error) &&
            ReadBoolean(lua, options, "blend_enabled", material.blendEnabled, error) &&
            ReadBoolean(lua, options, "inverted_blend", material.invertedBlend, error) &&
            ReadBoolean(lua, options, "subsurface_diffusion_profile",
                        material.subsurfaceDiffusionProfile, error) &&
            ReadEnum(lua, options, "blend_type", blendTypes, 10,
                     material.blendType, error, true) &&
            ReadEnum(lua, options, "alpha_test_type", alphaTests, 9,
                     material.alphaTestType, error) &&
            ReadAlphaReference(lua, options, "alpha_reference", material.alphaReferenceValue, error);
        lua->Pop();
        if (!ok) return false;
    }
    return true;
}

bool AddDirectMap(ILuaBase* lua, int table, const char* field, TextureRole role,
                  MaterialParameters& material, std::string& error) {
    lua->GetField(table, field);
    if (lua->IsType(-1, Type::Nil)) {
        lua->Pop();
        return true;
    }
    TextureInput texture;
    const bool ok = ParseMapValue(lua, lua->Top(), role, texture, error);
    lua->Pop();
    if (ok) material.textures.push_back(std::move(texture));
    return ok;
}

bool ParseDirectTables(ILuaBase* lua, int request, MaterialParameters& material,
                       std::string& error) {
    int info = 0;
    if (PushTableField(lua, request, "materialInfo", info)) {
        const bool ok =
            AddDirectMap(lua, info, "albedoTexture", TextureRole::Albedo, material, error) &&
            AddDirectMap(lua, info, "normalTexture", TextureRole::Normal, material, error) &&
            AddDirectMap(lua, info, "emissiveTexture", TextureRole::Emissive, material, error) &&
            ReadNumber(lua, info, "emissiveIntensity", material.emissiveIntensity, error) &&
            ReadVector(lua, info, "emissiveColorConstant", material.emissiveColor, error) &&
            ReadInteger(lua, info, "spriteSheetRow", material.spriteSheetRows, error) &&
            ReadInteger(lua, info, "spriteSheetCol", material.spriteSheetColumns, error) &&
            ReadInteger(lua, info, "spriteSheetFps", material.spriteSheetFps, error) &&
            ReadInteger(lua, info, "filterMode", material.filterMode, error) &&
            ReadInteger(lua, info, "wrapModeU", material.wrapModeU, error) &&
            ReadInteger(lua, info, "wrapModeV", material.wrapModeV, error);
        lua->Pop();
        if (!ok) return false;
    }
    int opaque = 0;
    if (PushTableField(lua, request, "opaqueInfo", opaque)) {
        const bool ok =
            AddDirectMap(lua, opaque, "roughnessTexture", TextureRole::Roughness, material, error) &&
            AddDirectMap(lua, opaque, "metallicTexture", TextureRole::Metallic, material, error) &&
            AddDirectMap(lua, opaque, "heightTexture", TextureRole::Height, material, error) &&
            AddDirectMap(lua, opaque, "anisotropyTexture", TextureRole::Anisotropy, material, error) &&
            ReadNumber(lua, opaque, "anisotropy", material.anisotropy, error) &&
            ReadVector(lua, opaque, "albedoConstant", material.albedo, error) &&
            ReadNumber(lua, opaque, "opacityConstant", material.opacity, error) &&
            ReadNumber(lua, opaque, "roughnessConstant", material.roughness, error) &&
            ReadNumber(lua, opaque, "metallicConstant", material.metallic, error) &&
            ReadNumber(lua, opaque, "thinFilmThickness", material.thinFilmThickness, error) &&
            ReadBoolean(lua, opaque, "alphaIsThinFilmThickness", material.alphaIsThinFilmThickness, error) &&
            ReadBoolean(lua, opaque, "useDrawCallAlphaState", material.useDrawCallAlphaState, error) &&
            ReadBoolean(lua, opaque, "invertedBlend", material.invertedBlend, error) &&
            ReadInteger(lua, opaque, "blendType", material.blendType, error) &&
            ReadInteger(lua, opaque, "alphaTestType", material.alphaTestType, error) &&
            ReadAlphaReference(lua, opaque, "alphaReferenceValue", material.alphaReferenceValue, error) &&
            ReadNumber(lua, opaque, "displaceIn", material.displaceIn, error) &&
            ReadNumber(lua, opaque, "displaceOut", material.displaceOut, error);
        lua->Pop();
        if (!ok) return false;
    }
    return true;
}

bool ParseMaterial(ILuaBase* lua, int request, MaterialParameters& material,
                   std::string& error) {
    lua->GetField(request, "compiled");
    if (!lua->IsType(-1, Type::Nil)) {
        if (!lua->IsType(-1, Type::Table)) {
            error = "request.compiled must be a table";
            lua->Pop();
            return false;
        }
        const int compiled = lua->Top();
        lua->GetField(compiled, "pbr");
        if (!lua->IsType(-1, Type::Table)) {
            error = "request.compiled.pbr must be a table";
            lua->Pop(2);
            return false;
        }
        const int pbr = lua->Top();
        const bool ok = ParsePbr(lua, pbr, material, error);
        lua->Pop(2);
        return ok;
    }
    lua->Pop();

    lua->GetField(request, "profile");
    if (!lua->IsType(-1, Type::Nil)) {
        if (!lua->IsType(-1, Type::Table)) {
            error = "request.profile must be a table";
            lua->Pop();
            return false;
        }
        const int profile = lua->Top();
        lua->GetField(profile, "pbr");
        if (!lua->IsType(-1, Type::Table)) {
            error = "request.profile.pbr must be a table when compiled is absent";
            lua->Pop(2);
            return false;
        }
        const int pbr = lua->Top();
        const bool ok = ParsePbr(lua, pbr, material, error);
        lua->Pop();
        lua->Pop();
        return ok;
    }
    lua->Pop();
    return ParseDirectTables(lua, request, material, error);
}

bool ValidateProtocol(ILuaBase* lua, int request, std::string& error) {
    lua->GetField(request, "protocol");
    if (lua->IsType(-1, Type::Nil)) {
        lua->Pop();
        return true;
    }
    if (!lua->IsType(-1, Type::Number) || lua->GetNumber(-1) != kApiVersion) {
        lua->Pop();
        error = "unsupported bridge protocol; expected 1";
        return false;
    }
    lua->Pop();
    return true;
}

void PushFailure(ILuaBase* lua, const std::string& reason) {
    lua->PushBool(false);
    lua->PushString(reason.c_str());
}

void PushResultTable(ILuaBase* lua, const Result& result) {
    lua->CreateTable();
    lua->PushString(result.code.c_str());
    lua->SetField(-2, "code");
    lua->PushString(result.profileId.c_str());
    lua->SetField(-2, "profileId");
    const auto layer = PathToUtf8(result.layerPath);
    lua->PushString(layer.c_str());
    lua->SetField(-2, "layerPath");
    lua->CreateTable();
    int layerIndex = 1;
    for (const auto& layerPath : result.layerPaths) {
        const auto path = PathToUtf8(layerPath);
        lua->PushNumber(layerIndex++);
        lua->PushString(path.c_str());
        lua->SetTable(-3);
    }
    lua->SetField(-2, "layerPaths");
    lua->CreateTable();
    int index = 1;
    for (const auto& texture : result.importedTextures) {
        const auto path = PathToUtf8(texture);
        lua->PushNumber(index++);
        lua->PushString(path.c_str());
        lua->SetTable(-3);
    }
    lua->SetField(-2, "importedTextures");
}

int Apply(ILuaBase* lua) {
    if (!lua->IsType(1, Type::Table)) {
        PushFailure(lua, "request must be a table");
        return 2;
    }
    std::string error;
    if (!ValidateProtocol(lua, 1, error)) {
        PushFailure(lua, error);
        return 2;
    }
    std::string operationName;
    bool operationPresent = false;
    if (!ReadString(lua, 1, "operation", operationName, operationPresent, error)) {
        PushFailure(lua, error);
        return 2;
    }
    BridgeOperation operation = BridgeOperation::Commit;
    if (!ParseBridgeOperation(operationPresent ? operationName : std::string{}, operation, error)) {
        PushFailure(lua, error);
        return 2;
    }
    if (operation == BridgeOperation::Preview) {
        PushFailure(lua,
            "preview_unsupported: protocol 1 cannot provide reversible live preview without publishing persistent USDA state");
        return 2;
    }
    const auto profileId = DerivedProfileId(lua, 1, error);
    if (!error.empty()) {
        PushFailure(lua, error);
        return 2;
    }
    std::vector<std::string> clearHashes;
    if ((operation == BridgeOperation::Restore || operation == BridgeOperation::Clear) &&
        !ParseHashes(lua, 1, clearHashes, error)) {
        PushFailure(lua, error);
        return 2;
    }

    if (operation == BridgeOperation::Restore || operation == BridgeOperation::Clear) {
        Result result;
        bool initialized = false;
        {
            std::lock_guard<std::mutex> lock(g_bridgeMutex);
            initialized = static_cast<bool>(g_bridge);
            if (initialized) {
                result = g_bridge->ClearLegacyMaterial(
                    {profileId, std::move(clearHashes)});
            }
        }
        if (!initialized) {
            PushFailure(lua, "native bridge is not initialized");
            return 2;
        }
        if (!result.ok) {
            PushFailure(lua, result.code + ": " + result.message);
            return 2;
        }
        lua->PushBool(true);
        PushResultTable(lua, result);
        return 2;
    }

    ApplyRequest request;
    request.operation = operation;
    request.profileId = profileId;
    if (!ParseHashes(lua, 1, request.hashes, error) ||
        !ParseMaterial(lua, 1, request.material, error)) {
        PushFailure(lua, error);
        return 2;
    }
    if (request.material.thinFilmThickness &&
        *request.material.thinFilmThickness == 0.0f) {
        request.material.thinFilmThickness.reset();
    }
    Result result;
    bool initialized = false;
    {
        std::lock_guard<std::mutex> lock(g_bridgeMutex);
        initialized = static_cast<bool>(g_bridge);
        if (initialized) {
            result = g_bridge->ApplyLegacyMaterial(request);
        }
    }
    if (!initialized) {
        PushFailure(lua, "native bridge is not initialized");
        return 2;
    }
    if (!result.ok) {
        PushFailure(lua, result.code + ": " + result.message);
        return 2;
    }
    lua->PushBool(true);
    PushResultTable(lua, result);
    return 2;
}

int Clear(ILuaBase* lua) {
    if (!lua->IsType(1, Type::Table)) {
        PushFailure(lua, "request must be a table");
        return 2;
    }
    std::string error;
    if (!ValidateProtocol(lua, 1, error)) {
        PushFailure(lua, error);
        return 2;
    }
    std::vector<std::string> hashes;
    if (!ParseHashes(lua, 1, hashes, error)) {
        PushFailure(lua, error);
        return 2;
    }
    const auto profileId = DerivedProfileId(lua, 1, error);
    if (!error.empty()) {
        PushFailure(lua, error);
        return 2;
    }
    Result result;
    bool initialized = false;
    {
        std::lock_guard<std::mutex> lock(g_bridgeMutex);
        initialized = static_cast<bool>(g_bridge);
        if (initialized) {
            result = g_bridge->ClearLegacyMaterial(
                {profileId, std::move(hashes)});
        }
    }
    if (!initialized) {
        PushFailure(lua, "native bridge is not initialized");
        return 2;
    }
    if (!result.ok) {
        PushFailure(lua, result.code + ": " + result.message);
        return 2;
    }
    lua->PushBool(true);
    PushResultTable(lua, result);
    return 2;
}

int ClearAll(ILuaBase* lua) {
    if (!lua->IsType(1, Type::Table)) {
        PushFailure(lua, "request must be a table");
        return 2;
    }
    std::string error;
    if (!ValidateProtocol(lua, 1, error)) {
        PushFailure(lua, error);
        return 2;
    }
    Result result;
    bool initialized = false;
    {
        std::lock_guard<std::mutex> lock(g_bridgeMutex);
        initialized = static_cast<bool>(g_bridge);
        if (initialized) {
            result = g_bridge->ClearAllOwned();
        }
    }
    if (!initialized) {
        PushFailure(lua, "native bridge is not initialized");
        return 2;
    }
    if (!result.ok) {
        PushFailure(lua, result.code + ": " + result.message);
        return 2;
    }
    lua->PushBool(true);
    PushResultTable(lua, result);
    return 2;
}

int GetCapabilities(ILuaBase* lua) {
    std::optional<Capabilities> capabilities;
    {
        std::lock_guard<std::mutex> lock(g_bridgeMutex);
        if (g_bridge) {
            capabilities = g_bridge->GetCapabilities();
        }
    }
    if (!capabilities) {
        lua->CreateTable();
        lua->PushNumber(kApiVersion); lua->SetField(-2, "apiVersion");
        lua->PushBool(false); lua->SetField(-2, "legacyMaterialOverrides");
        lua->PushBool(false); lua->SetField(-2, "livePreview");
        lua->PushBool(false); lua->SetField(-2, "authoritativeReset");
        lua->PushBool(true); lua->SetField(-2, "synchronous");
        return 1;
    }
    lua->CreateTable();
    lua->PushNumber(capabilities->apiVersion); lua->SetField(-2, "apiVersion");
    lua->PushBool(capabilities->available); lua->SetField(-2, "legacyMaterialOverrides");
    lua->PushBool(capabilities->livePreview); lua->SetField(-2, "livePreview");
    lua->PushBool(capabilities->available && capabilities->authoritativeReset);
    lua->SetField(-2, "authoritativeReset");
    lua->PushBool(capabilities->available); lua->SetField(-2, "textureWriter");
    lua->PushBool(capabilities->atomicProfileLayers); lua->SetField(-2, "atomicProfileLayers");
    lua->PushBool(capabilities->ddsImport); lua->SetField(-2, "ddsImport");
    lua->PushBool(capabilities->wicImageConversion); lua->SetField(-2, "wicImageConversion");
    lua->PushBool(capabilities->textureOperations); lua->SetField(-2, "textureOperations");
    lua->PushBool(capabilities->synchronous); lua->SetField(-2, "synchronous");
    lua->PushNumber(static_cast<double>(capabilities->maxTextureBytes));
    lua->SetField(-2, "maxTextureBytes");
    lua->PushNumber(static_cast<double>(capabilities->maxTextureDimension));
    lua->SetField(-2, "maxTextureDimension");
    lua->PushNumber(static_cast<double>(capabilities->maxTextureOperations));
    lua->SetField(-2, "maxTextureOperations");
    lua->PushNumber(static_cast<double>(capabilities->maxActiveProfiles));
    lua->SetField(-2, "maxActiveProfiles");
    lua->PushNumber(static_cast<double>(capabilities->maxActiveTextureFiles));
    lua->SetField(-2, "maxActiveTextureFiles");
    lua->PushNumber(static_cast<double>(capabilities->maxActiveTextureBytes));
    lua->SetField(-2, "maxActiveTextureBytes");
    const auto modDirectory = PathToUtf8(capabilities->modDirectory);
    lua->PushString(modDirectory.c_str()); lua->SetField(-2, "modDirectory");
    return 1;
}

using LuaCall = int (*)(ILuaBase*);

int GuardLuaCall(ILuaBase* lua, LuaCall call) noexcept {
    try {
        return call(lua);
    } catch (const std::exception& exception) {
        try {
            lua->PushBool(false);
            lua->PushString(exception.what());
            return 2;
        } catch (...) {
            return 0;
        }
    } catch (...) {
        try {
            lua->PushBool(false);
            lua->PushString("native bridge operation failed");
            return 2;
        } catch (...) {
            return 0;
        }
    }
}

LUA_FUNCTION(AdvMatRTXBridge_Capabilities) {
    return GuardLuaCall(LUA, GetCapabilities);
}
LUA_FUNCTION(AdvMatRTXBridge_ApplyLegacyMaterial) {
    return GuardLuaCall(LUA, Apply);
}
LUA_FUNCTION(AdvMatRTXBridge_ClearLegacyMaterial) {
    return GuardLuaCall(LUA, Clear);
}
LUA_FUNCTION(AdvMatRTXBridge_ClearAllOwned) {
    return GuardLuaCall(LUA, ClearAll);
}

} // namespace

bool RegisterLua(ILuaBase* lua, const std::filesystem::path& gameRoot) {
    if (!lua) return false;
    {
        std::lock_guard<std::mutex> lock(g_bridgeMutex);
        Config config;
        config.gameRoot = gameRoot;
        g_bridge = std::make_unique<Bridge>(std::move(config));
    }
    lua->PushSpecial(GarrysMod::Lua::SPECIAL_GLOB);
    lua->CreateTable();
    lua->PushNumber(kApiVersion);
    lua->SetField(-2, "API_VERSION");
    lua->PushCFunction(AdvMatRTXBridge_Capabilities);
    lua->SetField(-2, "Capabilities");
    lua->PushCFunction(AdvMatRTXBridge_ApplyLegacyMaterial);
    lua->SetField(-2, "ApplyLegacyMaterial");
    lua->PushCFunction(AdvMatRTXBridge_ClearLegacyMaterial);
    lua->SetField(-2, "ClearLegacyMaterial");
    lua->PushCFunction(AdvMatRTXBridge_ClearAllOwned);
    lua->SetField(-2, "ClearAllOwned");
    lua->SetField(-2, "AdvMatRTXBridge");
    lua->Pop();
    return true;
}

void UnregisterLua(ILuaBase* lua) {
    {
        std::lock_guard<std::mutex> lock(g_bridgeMutex);
        g_bridge.reset();
    }
    if (!lua) return;
    try {
        lua->PushSpecial(GarrysMod::Lua::SPECIAL_GLOB);
        lua->PushNil();
        lua->SetField(-2, "AdvMatRTXBridge");
        lua->Pop();
    } catch (...) {
        // Module teardown must continue even if the Lua state is already
        // partially unavailable. The native writer has already been released.
    }
}

} // namespace advmat::rtx_bridge
