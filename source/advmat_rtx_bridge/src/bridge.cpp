#include "advmat_rtx_bridge/bridge.h"
#include "internal.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <string_view>
#include <system_error>
#include <utility>

namespace advmat::rtx_bridge {
namespace {

constexpr std::uintmax_t kMaximumLegacyLayerBytes = 4ull * 1024ull * 1024ull;

bool TextFileEquals(const std::filesystem::path& path, std::string_view expected) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec) || ec ||
        std::filesystem::file_size(path, ec) != expected.size() || ec) {
        return false;
    }

    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return false;
    }
    std::array<char, 4096> buffer{};
    std::size_t offset = 0;
    while (offset < expected.size()) {
        const auto count = std::min(buffer.size(), expected.size() - offset);
        input.read(buffer.data(), static_cast<std::streamsize>(count));
        if (input.gcount() != static_cast<std::streamsize>(count) ||
            !std::equal(buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(count),
                        expected.begin() + static_cast<std::ptrdiff_t>(offset))) {
            return false;
        }
        offset += count;
    }
    return true;
}

bool ReadTextFileBounded(const std::filesystem::path& path, std::uintmax_t maximum,
                         std::string& contents, std::string& error) {
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec || size > maximum) {
        error = ec ? "cannot inspect text file: " + ec.message() :
                     "text file exceeds the safety limit";
        return false;
    }
    contents.resize(static_cast<std::size_t>(size));
    std::ifstream input(path, std::ios::binary);
    if (!input || (!contents.empty() &&
        !input.read(contents.data(), static_cast<std::streamsize>(contents.size())))) {
        error = "cannot read text file";
        return false;
    }
    return true;
}

std::string WrapHashBlock(std::string_view block) {
    std::string output = "#usda 1.0\n(\n    upAxis = \"Z\"\n)\n\n"
                         "over \"RootNode\"\n{\n    over \"Looks\"\n    {\n";
    output.append(block.data(), block.size());
    output += "\n    }\n}\n";
    return output;
}

bool SplitLegacyLayer(const std::string& contents,
                      std::map<std::string, std::string>& layers,
                      std::string& error) {
    if (contents.find("#usda 1.0") == std::string::npos ||
        contents.find("over \"RootNode\"") == std::string::npos ||
        contents.find("over \"Looks\"") == std::string::npos) {
        error = "legacy profile layer is not an AdvMat USDA layer";
        return false;
    }

    constexpr std::string_view marker = "over \"mat_";
    std::size_t cursor = 0;
    while ((cursor = contents.find(marker, cursor)) != std::string::npos) {
        const auto hashStart = cursor + marker.size();
        if (hashStart + 17 > contents.size() || contents[hashStart + 16] != '"') {
            error = "legacy profile contains a malformed captured texture hash";
            return false;
        }
        std::string canonical;
        if (!CanonicalizeHash(contents.substr(hashStart, 16), canonical, error)) {
            error = "legacy profile contains an invalid captured texture hash: " + error;
            return false;
        }

        const auto open = contents.find('{', hashStart + 17);
        if (open == std::string::npos) {
            error = "legacy profile material block has no opening brace";
            return false;
        }
        std::size_t depth = 0;
        std::size_t close = std::string::npos;
        bool inString = false;
        bool inAsset = false;
        bool escaped = false;
        for (std::size_t index = open; index < contents.size(); ++index) {
            const char character = contents[index];
            if (inString) {
                if (escaped) escaped = false;
                else if (character == '\\') escaped = true;
                else if (character == '"') inString = false;
                continue;
            }
            if (inAsset) {
                if (character == '@') inAsset = false;
                continue;
            }
            if (character == '"') {
                inString = true;
            } else if (character == '@') {
                inAsset = true;
            } else if (character == '{') {
                ++depth;
            } else if (character == '}') {
                if (depth == 0) {
                    error = "legacy profile material block has unbalanced braces";
                    return false;
                }
                --depth;
                if (depth == 0) {
                    close = index;
                    break;
                }
            }
        }
        if (close == std::string::npos) {
            error = "legacy profile material block has no closing brace";
            return false;
        }
        const auto previousNewline = contents.rfind('\n', cursor);
        const auto blockStart = previousNewline == std::string::npos ? 0 : previousNewline + 1;
        if (!layers.emplace(canonical,
                WrapHashBlock(std::string_view(contents).substr(blockStart, close - blockStart + 1))).second) {
            error = "legacy profile contains a duplicate captured texture hash";
            return false;
        }
        cursor = close + 1;
    }
    return true;
}

} // namespace

Result Result::Success(std::string profileId) {
    Result result;
    result.ok = true;
    result.code = "ok";
    result.profileId = std::move(profileId);
    return result;
}

Result Result::Failure(std::string code, std::string message) {
    Result result;
    result.ok = false;
    result.code = std::move(code);
    result.message = std::move(message);
    return result;
}

bool ValidateProfileId(const std::string& value, std::string& error) {
    if (value.empty() || value.size() > 64) {
        error = "profile ID must contain 1 to 64 characters";
        return false;
    }
    for (const unsigned char character : value) {
        if (!(std::isalnum(character) || character == '_' || character == '-' || character == '.')) {
            error = "profile ID may only contain ASCII letters, digits, underscore, hyphen and dot";
            return false;
        }
    }
    if (value == "." || value == ".." || value.front() == '.') {
        error = "profile ID cannot be a dot path or begin with dot";
        return false;
    }
    return true;
}

bool CanonicalizeHash(const std::string& value, std::string& canonical, std::string& error) {
    std::size_t offset = 0;
    if (value.size() >= 2 && value[0] == '0' && (value[1] == 'x' || value[1] == 'X')) {
        offset = 2;
    }
    const auto digits = value.size() - offset;
    if (digits == 0 || digits > 16) {
        error = "texture hash must contain 1 to 16 hexadecimal digits";
        return false;
    }
    canonical.assign(16 - digits, '0');
    bool nonzero = false;
    for (std::size_t index = offset; index < value.size(); ++index) {
        const unsigned char character = static_cast<unsigned char>(value[index]);
        if (!std::isxdigit(character)) {
            error = "texture hash contains a non-hexadecimal character";
            canonical.clear();
            return false;
        }
        const char upper = static_cast<char>(std::toupper(character));
        canonical.push_back(upper);
        nonzero = nonzero || upper != '0';
    }
    if (!nonzero) {
        error = "zero is not a valid replacement texture hash";
        canonical.clear();
        return false;
    }
    return true;
}

bool ParseBridgeOperation(const std::string& value, BridgeOperation& operation,
                          std::string& error) {
    std::string normalized = value;
    std::transform(normalized.begin(), normalized.end(), normalized.begin(),
        [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
    if (normalized.empty() || normalized == "commit") {
        operation = BridgeOperation::Commit;
        return true;
    }
    if (normalized == "preview") {
        operation = BridgeOperation::Preview;
        return true;
    }
    if (normalized == "restore") {
        operation = BridgeOperation::Restore;
        return true;
    }
    if (normalized == "clear") {
        operation = BridgeOperation::Clear;
        return true;
    }
    error = "request.operation is not supported";
    return false;
}

std::optional<std::filesystem::path> FindGameRootFromExecutable(
    const std::filesystem::path& executablePath) {
    if (executablePath.empty() || !executablePath.is_absolute()) {
        return std::nullopt;
    }
    std::error_code ec;
    if (!std::filesystem::is_regular_file(executablePath, ec) || ec) {
        return std::nullopt;
    }

    auto candidate = executablePath.parent_path();
    for (std::size_t depth = 0; depth <= kMaxGameRootAncestorDepth; ++depth) {
        ec.clear();
        const bool hasGarrysMod = std::filesystem::is_directory(candidate / "garrysmod", ec);
        if (!ec) {
            const bool hasRemix = std::filesystem::is_directory(candidate / "rtx-remix", ec);
            if (!ec && hasGarrysMod && hasRemix) {
                return candidate.lexically_normal();
            }
        }
        const auto parent = candidate.parent_path();
        if (parent.empty() || parent == candidate) {
            break;
        }
        candidate = parent;
    }
    return std::nullopt;
}

const char* TextureRoleName(TextureRole role) noexcept {
    switch (role) {
    case TextureRole::Albedo: return "albedo";
    case TextureRole::Normal: return "normal";
    case TextureRole::Anisotropy: return "anisotropy";
    case TextureRole::Roughness: return "roughness";
    case TextureRole::Metallic: return "metallic";
    case TextureRole::Height: return "height";
    case TextureRole::Emissive: return "emissive";
    case TextureRole::SubsurfaceTransmittance: return "subsurface_transmittance";
    case TextureRole::SubsurfaceThickness: return "subsurface_thickness";
    case TextureRole::SubsurfaceScattering: return "subsurface_scattering";
    case TextureRole::SubsurfaceRadius: return "subsurface_radius";
    }
    return "unknown";
}

namespace {

bool IsHexString(std::string_view value) {
    if (value.empty()) return false;
    return std::all_of(value.begin(), value.end(), [](unsigned char character) {
        return std::isxdigit(character) != 0;
    });
}

std::optional<std::string> HashFromProfileId(const std::string& id) {
    if (id.rfind("hash_", 0) != 0 || (id.size() != 21 && id.size() != 38)) {
        return std::nullopt;
    }
    if (id.size() == 38 && (id[21] != '_' ||
        !IsHexString(std::string_view(id).substr(22, 16)))) {
        return std::nullopt;
    }
    std::string canonical;
    std::string ignored;
    if (!CanonicalizeHash(id.substr(5, 16), canonical, ignored)) {
        return std::nullopt;
    }
    return canonical;
}

bool IsOwnedProfileId(const std::string& id) {
    std::string ignored;
    if (!ValidateProfileId(id, ignored)) return false;
    if (id.rfind("material_", 0) == 0) {
        return true;
    }
    return HashFromProfileId(id).has_value();
}

std::string ContentRevision(std::string_view contents) {
    std::uint64_t hash = 14695981039346656037ull;
    for (const unsigned char character : contents) {
        hash ^= character;
        hash *= 1099511628211ull;
    }
    std::ostringstream output;
    output << std::uppercase << std::hex << std::setfill('0') << std::setw(16) << hash;
    return output.str();
}

std::string VersionedProfileId(const std::string& hash, std::string_view contents) {
    return "hash_" + hash + "_" + ContentRevision(contents);
}

void NormalizeProfiles(std::vector<std::string>& profiles) {
    std::sort(profiles.begin(), profiles.end());
    profiles.erase(std::unique(profiles.begin(), profiles.end()), profiles.end());
}

void RemoveHash(std::vector<std::string>& profiles, const std::string& hash) {
    profiles.erase(std::remove_if(profiles.begin(), profiles.end(), [&hash](const std::string& id) {
        const auto layerHash = HashFromProfileId(id);
        return layerHash && *layerHash == hash;
    }), profiles.end());
}

bool ParseModLayer(const std::string& contents, std::vector<std::string>& profiles,
                   std::string& error) {
    const auto declaration = contents.find("subLayers");
    const auto open = declaration == std::string::npos ? std::string::npos :
        contents.find('[', declaration);
    const auto close = open == std::string::npos ? std::string::npos :
        contents.find(']', open + 1);
    if (open == std::string::npos || close == std::string::npos) {
        error = "owned mod root has no valid subLayers array";
        return false;
    }

    std::set<std::string> seen;
    std::size_t cursor = open + 1;
    while (true) {
        const auto assetStart = contents.find('@', cursor);
        if (assetStart == std::string::npos || assetStart >= close) break;
        const auto assetEnd = contents.find('@', assetStart + 1);
        if (assetEnd == std::string::npos || assetEnd > close) {
            error = "owned mod root contains an unterminated sublayer asset";
            return false;
        }
        const std::string asset = contents.substr(assetStart + 1,
            assetEnd - assetStart - 1);
        constexpr std::string_view prefix = "./profiles/";
        constexpr std::string_view suffix = ".usda";
        if (asset.size() <= prefix.size() + suffix.size() ||
            asset.compare(0, prefix.size(), prefix) != 0 ||
            asset.compare(asset.size() - suffix.size(), suffix.size(), suffix) != 0) {
            error = "owned mod root references a layer outside profiles/";
            return false;
        }
        const auto id = asset.substr(prefix.size(),
            asset.size() - prefix.size() - suffix.size());
        if (!IsOwnedProfileId(id) || !seen.insert(id).second) {
            error = "owned mod root contains an invalid or duplicate profile layer";
            return false;
        }
        profiles.push_back(id);
        cursor = assetEnd + 1;
    }
    NormalizeProfiles(profiles);
    return true;
}

bool IsOwnedTextureFileName(const std::string& filename) {
    constexpr std::string_view suffix = ".dds";
    // "tangent" is recognized only while tracing files referenced by an old
    // on-disk layer so migration cleanup cannot delete its active dependency.
    // The request parser and current writer do not expose that retired role.
    static constexpr std::string_view roles[]{
        "albedo", "normal", "anisotropy", "roughness",
        "metallic", "height", "emissive", "subsurface_transmittance",
        "subsurface_thickness", "subsurface_scattering", "subsurface_radius",
        "tangent",
    };
    static_assert(std::size(roles) == kTextureRoleCount + 1);
    if (filename.size() <= suffix.size() ||
        filename.compare(filename.size() - suffix.size(), suffix.size(), suffix) != 0) {
        return false;
    }
    for (const auto role : roles) {
        const std::string prefix = std::string(role) + "_";
        if (filename.size() == prefix.size() + 16 + suffix.size() &&
            filename.compare(0, prefix.size(), prefix) == 0 &&
            IsHexString(std::string_view(filename).substr(prefix.size(), 16))) {
            return true;
        }
    }
    return false;
}

bool CollectOwnedTextureReferences(
        const std::string& contents, const std::filesystem::path& modDirectory,
        std::vector<std::filesystem::path>& references, std::string& error) {
    constexpr std::string_view prefix = "../textures/";
    std::size_t cursor = 0;
    while (true) {
        const auto assetStart = contents.find('@', cursor);
        if (assetStart == std::string::npos) break;
        const auto assetEnd = contents.find('@', assetStart + 1);
        if (assetEnd == std::string::npos) {
            error = "active profile contains an unterminated asset path";
            return false;
        }
        std::string asset = contents.substr(assetStart + 1, assetEnd - assetStart - 1);
        std::replace(asset.begin(), asset.end(), '\\', '/');
        cursor = assetEnd + 1;
        if (asset.compare(0, prefix.size(), prefix) != 0) continue;

        const std::filesystem::path relative(asset.substr(prefix.size()));
        if (relative.empty() || relative.is_absolute()) {
            error = "active profile contains an invalid owned texture asset";
            return false;
        }
        std::vector<std::filesystem::path> components;
        for (const auto& component : relative) {
            if (component.empty() || component == "." || component == ".." ||
                component.has_root_path()) {
                error = "active profile contains an unsafe owned texture asset";
                return false;
            }
            components.push_back(component);
        }
        if (components.size() != 2) {
            error = "active profile contains an unexpected owned texture path shape";
            return false;
        }
        std::string ignored;
        if (!ValidateProfileId(components[0].string(), ignored) ||
            !IsOwnedTextureFileName(components[1].string())) {
            error = "active profile contains an invalid owned texture filename";
            return false;
        }
        const auto absolute = (modDirectory / "textures" / relative).lexically_normal();
        if (!detail::IsDescendantOrSame(absolute, modDirectory / "textures")) {
            error = "active profile texture asset escaped the owned cache";
            return false;
        }
        references.push_back(absolute);
    }
    return true;
}

} // namespace

struct Bridge::Impl {
    Config config;
    std::filesystem::path modDirectory;
    std::vector<std::filesystem::path> allowedRoots;
    std::string initializationError;
    mutable std::mutex mutex;

    explicit Impl(Config incoming) : config(std::move(incoming)) {
        std::error_code ec;
        if (config.maxTextureBytes == 0 || config.maxTextureDimension == 0 ||
            config.maxActiveProfiles < 128 || config.maxActiveTextureFiles == 0 ||
            config.maxActiveTextureBytes == 0) {
            initializationError = "native writer resource limits are invalid";
            return;
        }
        if (config.gameRoot.empty()) {
            initializationError = "game root is empty";
            return;
        }
        auto root = std::filesystem::canonical(config.gameRoot, ec);
        if (ec || !std::filesystem::is_directory(root, ec)) {
            initializationError = "game root is not an existing directory";
            return;
        }
        config.gameRoot = root;
        std::string pathError;
        bool pathExists = false;
        const auto runtimeDirectory = root / "rtx-remix";
        if (!detail::InspectPlainPath(runtimeDirectory, true, pathExists, pathError) ||
            !pathExists) {
            initializationError = "game root does not contain an rtx-remix runtime directory";
            if (!pathError.empty()) initializationError += ": " + pathError;
            return;
        }
        modDirectory = root / "rtx-remix" / "mods" / "!advanced_material_editor";
        if (modDirectory.filename() != "!advanced_material_editor" ||
            modDirectory.parent_path().filename() != "mods") {
            initializationError = "internal mod destination invariant failed";
            return;
        }

        // Do not create output during capability discovery, but reject every
        // existing component/leaf which could redirect later reads or writes.
        // Missing mods/addon directories are created one component at a time by
        // EnsureModDirectory on the first commit.
        const std::pair<std::filesystem::path, bool> ownedPaths[]{
            {root / "rtx-remix" / "mods", true},
            {modDirectory, true},
            {modDirectory / "mod.usda", false},
            {modDirectory / "profiles", true},
            {modDirectory / "profiles.retired", true},
            {modDirectory / "textures", true},
            {modDirectory / "textures.retired", true},
        };
        for (const auto& [path, requireDirectory] : ownedPaths) {
            pathError.clear();
            if (!detail::InspectPlainPath(path, requireDirectory, pathExists, pathError)) {
                initializationError = "unsafe existing native output path: " + pathError;
                return;
            }
        }

        if (config.allowedTextureRoots.empty()) {
            allowedRoots.push_back(root);
        } else {
            for (const auto& configuredRoot : config.allowedTextureRoots) {
                auto canonicalRoot = std::filesystem::canonical(configuredRoot, ec);
                if (ec || !std::filesystem::is_directory(canonicalRoot, ec)) {
                    initializationError = "an allowed texture root is not an existing directory";
                    return;
                }
                // Import roots must themselves be below the game install. This
                // prevents the Lua-facing API from becoming an arbitrary file reader.
                if (!detail::IsDescendantOrSame(canonicalRoot, root)) {
                    initializationError = "allowed texture roots must be inside the game root";
                    return;
                }
                allowedRoots.push_back(std::move(canonicalRoot));
            }
        }
    }

    bool Injected(std::string_view point) const noexcept {
        if (!config.failureInjector) return false;
        try {
            return config.failureInjector(point);
        } catch (...) {
            return true;
        }
    }

    std::filesystem::path ProfilePath(const std::string& id) const {
        return modDirectory / "profiles" / (id + ".usda");
    }

    bool InspectModDirectory(bool& exists, std::string& error) const {
        exists = false;
        bool componentExists = false;
        const auto runtimeDirectory = config.gameRoot / "rtx-remix";
        if (!detail::InspectPlainPath(runtimeDirectory, true, componentExists, error) ||
            !componentExists) {
            if (error.empty()) error = "rtx-remix runtime directory is missing";
            return false;
        }
        const auto modsDirectory = runtimeDirectory / "mods";
        if (!detail::InspectPlainPath(modsDirectory, true, componentExists, error)) return false;
        if (!componentExists) return true;
        if (!detail::InspectPlainPath(modDirectory, true, exists, error)) return false;
        if (!exists) return true;
        return detail::ValidatePlainDirectoryTree(config.gameRoot, modDirectory, error);
    }

    bool EnsureModDirectory(std::string& error) const {
        bool runtimeExists = false;
        if (!detail::InspectPlainPath(config.gameRoot / "rtx-remix", true,
                                      runtimeExists, error) || !runtimeExists) {
            if (error.empty()) error = "rtx-remix runtime directory is missing";
            return false;
        }
        return detail::EnsurePlainDirectoryTree(config.gameRoot, modDirectory, error);
    }

    bool RetireOwnedDirectory(const char* activeName, const char* retiredName,
                              bool& rotated, std::filesystem::path& retiredPath,
                              std::string& error) const {
        rotated = false;
        const auto active = modDirectory / activeName;
        const auto retired = modDirectory / retiredName;
        if (!detail::IsDescendantOrSame(active, modDirectory) ||
            !detail::IsDescendantOrSame(retired, modDirectory) ||
            active.parent_path() != modDirectory || retired.parent_path() != modDirectory) {
            error = "computed retirement path escaped the owned mod directory";
            return false;
        }
        if (!detail::ValidatePlainDirectoryTree(config.gameRoot, modDirectory, error)) {
            error = std::string("cannot validate retirement root: ") + error;
            return false;
        }

        bool activeExists = false;
        if (!detail::InspectPlainPath(active, true, activeExists, error)) {
            error = std::string("cannot retire ") + activeName + ": " + error;
            return false;
        }
        // An idempotent reset keeps the previous quarantine when no new
        // generation exists. This bounds storage without discarding the sole
        // inactive generation on every repeated reset call.
        if (!activeExists) return true;

        bool retiredExists = false;
        if (!detail::InspectPlainPath(retired, true, retiredExists, error)) {
            error = std::string("cannot replace ") + retiredName + ": " + error;
            return false;
        }
        if (retiredExists) {
            if (!detail::ValidatePlainDirectoryTree(config.gameRoot, modDirectory, error) ||
                !detail::InspectPlainPath(retired, true, retiredExists, error) ||
                !retiredExists) {
                if (error.empty()) error = "retirement quarantine changed before cleanup";
                return false;
            }
            // Validate every descendant too. A nested Win32 junction must not
            // turn bounded quarantine replacement into deletion outside this
            // addon's directory.
            if (!detail::RemovePlainDirectoryTree(retired, error)) {
                error = std::string("cannot remove prior ") + retiredName + ": " + error;
                return false;
            }
        }

        // Cleanup and rename are separate filesystem operations. Revalidate
        // every ancestor and both leaves after cleanup and immediately before
        // the contained same-directory move.
        if (!detail::ValidatePlainDirectoryTree(config.gameRoot, modDirectory, error) ||
            !detail::InspectPlainPath(active, true, activeExists, error) || !activeExists ||
            !detail::InspectPlainPath(retired, true, retiredExists, error) || retiredExists) {
            if (error.empty()) error = "retirement paths changed before rename";
            return false;
        }

        if (!detail::AtomicReplacePath(active, retired, error)) {
            error = std::string("cannot retire ") + activeName + ": " + error;
            return false;
        }
        rotated = true;
        retiredPath = retired;
        return true;
    }

    bool LoadActiveProfiles(std::vector<std::string>& profiles,
                            std::string& error) const {
        bool modExists = false;
        if (!InspectModDirectory(modExists, error)) return false;
        if (!modExists) return true;
        const auto modLayer = modDirectory / "mod.usda";
        bool modLayerExists = false;
        if (!detail::InspectPlainPath(modLayer, false, modLayerExists, error)) return false;
        if (!modLayerExists) return true;
        std::string contents;
        if (!ReadTextFileBounded(modLayer, kMaximumLegacyLayerBytes, contents, error) ||
            !ParseModLayer(contents, profiles, error)) {
            return false;
        }
        if (profiles.size() > config.maxActiveProfiles) {
            error = "owned mod root exceeds the active profile limit";
            return false;
        }
        const auto profilesDirectory = modDirectory / "profiles";
        bool profilesExist = false;
        if (!detail::InspectPlainPath(profilesDirectory, true, profilesExist, error)) return false;
        if (profilesExist &&
            !detail::ValidatePlainDirectoryTree(config.gameRoot, profilesDirectory, error)) {
            return false;
        }
        if (!profiles.empty() && !profilesExist) {
            error = "owned mod root references profiles but profiles/ is missing";
            return false;
        }
        for (const auto& id : profiles) {
            bool profileExists = false;
            if (!detail::InspectPlainPath(ProfilePath(id), false, profileExists, error) ||
                !profileExists) {
                error = "owned mod root references a missing profile layer: " + id;
                return false;
            }
        }
        return true;
    }

    bool CollectActiveOwnedFiles(
            const std::vector<std::string>& active,
            std::vector<std::filesystem::path>& profiles,
            std::vector<std::filesystem::path>& textures,
            std::string& error) const {
        bool modExists = false;
        if (!InspectModDirectory(modExists, error)) return false;
        if (!modExists) {
            if (!active.empty()) error = "active profiles exist without an owned mod directory";
            return active.empty();
        }

        profiles.clear();
        textures.clear();
        profiles.reserve(active.size());
        for (const auto& id : active) {
            const auto profile = ProfilePath(id);
            bool profileExists = false;
            if (!detail::InspectPlainPath(profile, false, profileExists, error) ||
                !profileExists) {
                if (error.empty()) error = "active profile disappeared during cache pruning";
                return false;
            }
            std::string contents;
            if (!ReadTextFileBounded(profile, kMaximumLegacyLayerBytes, contents, error) ||
                !CollectOwnedTextureReferences(
                    contents, modDirectory, textures, error)) {
                return false;
            }
            profiles.push_back(profile);
        }
        std::sort(textures.begin(), textures.end());
        textures.erase(std::unique(textures.begin(), textures.end()), textures.end());
        return true;
    }

    bool ValidateActiveTextureQuota(const std::vector<std::string>& active,
                                    std::string& error) const {
        std::vector<std::filesystem::path> activeProfiles;
        std::vector<std::filesystem::path> activeTextures;
        if (!CollectActiveOwnedFiles(
                active, activeProfiles, activeTextures, error)) {
            return false;
        }
        if (activeTextures.size() > config.maxActiveTextureFiles) {
            error = "prospective active texture set exceeds the file-count limit";
            return false;
        }
        std::uintmax_t totalBytes = 0;
        for (const auto& texture : activeTextures) {
            bool exists = false;
            if (!detail::InspectPlainPath(texture, false, exists, error) || !exists) {
                if (error.empty()) error = "an active owned texture is missing";
                return false;
            }
            std::error_code ec;
            const auto size = std::filesystem::file_size(texture, ec);
            if (ec) {
                error = "cannot inspect active owned texture size: " + ec.message();
                return false;
            }
            if (size > config.maxActiveTextureBytes - totalBytes) {
                error = "prospective active texture set exceeds the byte limit";
                return false;
            }
            totalBytes += size;
        }
        return true;
    }

    bool PruneInactiveOwnedFiles(const std::vector<std::string>& active,
                                 std::string& error) const {
        std::vector<std::filesystem::path> keepProfiles;
        std::vector<std::filesystem::path> keepTextures;
        if (!CollectActiveOwnedFiles(active, keepProfiles, keepTextures, error)) {
            return false;
        }

        if (!detail::PrunePlainDirectoryTree(
                modDirectory / "profiles", keepProfiles, error)) {
            error = "cannot prune inactive profile revisions: " + error;
            return false;
        }
        if (!detail::PrunePlainDirectoryTree(
                modDirectory / "textures", keepTextures, error)) {
            error = "cannot prune inactive texture revisions: " + error;
            return false;
        }
        return true;
    }

    Result WriteModLast(std::vector<std::string> profiles,
                        bool forcePublish = false) const {
        std::string error;
        const auto modLayer = modDirectory / "mod.usda";
        NormalizeProfiles(profiles);
        if (profiles.size() > config.maxActiveProfiles) {
            return Result::Failure("resource_limit",
                "prospective root exceeds the active profile limit");
        }
        const auto contents = detail::BuildModLayer(profiles);
        if (contents.size() > kMaximumLegacyLayerBytes) {
            return Result::Failure("resource_limit",
                "prospective root layer exceeds the byte limit");
        }
        if (!EnsureModDirectory(error)) {
            return Result::Failure("mod_index_write_failed", error);
        }
        bool modLayerExists = false;
        if (!detail::InspectPlainPath(modLayer, false, modLayerExists, error)) {
            return Result::Failure("mod_index_write_failed", error);
        }
        if (!forcePublish && TextFileEquals(modLayer, contents)) {
            return Result::Success();
        }
        if (!EnsureModDirectory(error) ||
            !detail::InspectPlainPath(modLayer, false, modLayerExists, error)) {
            return Result::Failure("mod_index_write_failed", error);
        }
        if (!detail::AtomicWriteText(modLayer, contents, error)) {
            return Result::Failure("mod_index_write_failed", error);
        }
        // The staging file already has a fresh timestamp. Avoid a second,
        // non-transactional metadata mutation after the root commit point.
        return Result::Success();
    }

    bool StageImmutableProfile(const std::string& id, const std::string& contents,
                               std::filesystem::path& path, std::string& error) const {
        if (!IsOwnedProfileId(id)) {
            error = "computed immutable profile ID is invalid";
            return false;
        }
        path = ProfilePath(id);
        if (!detail::IsDescendantOrSame(path, modDirectory)) {
            error = "computed immutable profile path escaped the mod directory";
            return false;
        }
        const auto profilesDirectory = modDirectory / "profiles";
        if (!EnsureModDirectory(error) ||
            !detail::EnsurePlainDirectoryTree(config.gameRoot, profilesDirectory, error)) {
            return false;
        }
        bool pathExists = false;
        if (!detail::InspectPlainPath(path, false, pathExists, error)) return false;
        if (pathExists) {
            if (!TextFileEquals(path, contents)) {
                error = "immutable profile revision collision or corruption";
                return false;
            }
            return true;
        }
        if (!detail::ValidatePlainDirectoryTree(config.gameRoot, profilesDirectory, error) ||
            !detail::InspectPlainPath(path, false, pathExists, error)) return false;
        if (pathExists) {
            if (!TextFileEquals(path, contents)) {
                error = "immutable profile revision collision or corruption";
                return false;
            }
            return true;
        }
        if (!detail::AtomicWriteText(path, contents, error)) {
            return false;
        }
        return true;
    }

    bool StageLegacyMigration(const std::string& profileId,
                              std::vector<std::string>& active,
                              std::string& error) const {
        if (profileId.rfind("material_", 0) != 0 ||
            std::find(active.begin(), active.end(), profileId) == active.end()) {
            return true;
        }
        const auto legacy = ProfilePath(profileId);
        bool legacyExists = false;
        if (!detail::ValidatePlainDirectoryTree(config.gameRoot,
                modDirectory / "profiles", error) ||
            !detail::InspectPlainPath(legacy, false, legacyExists, error) ||
            !legacyExists) {
            if (error.empty()) error = "active legacy profile layer is missing";
            return false;
        }
        std::string contents;
        if (!ReadTextFileBounded(legacy, kMaximumLegacyLayerBytes, contents, error)) {
            return false;
        }
        std::map<std::string, std::string> split;
        if (!SplitLegacyLayer(contents, split, error)) {
            return false;
        }
        std::vector<std::pair<std::string, std::string>> revisions;
        revisions.reserve(split.size());
        std::vector<std::string> migratedActive = active;
        migratedActive.erase(
            std::remove(migratedActive.begin(), migratedActive.end(), profileId),
            migratedActive.end());
        for (const auto& [hash, layerContents] : split) {
            const auto id = VersionedProfileId(hash, layerContents);
            RemoveHash(migratedActive, hash);
            migratedActive.push_back(id);
            revisions.emplace_back(id, layerContents);
        }
        NormalizeProfiles(migratedActive);
        if (migratedActive.size() > config.maxActiveProfiles) {
            error = "legacy migration exceeds the active profile limit";
            return false;
        }
        for (const auto& [id, layerContents] : revisions) {
            std::filesystem::path path;
            if (!StageImmutableProfile(id, layerContents, path, error)) {
                error = "cannot stage migrated profile " + id + ": " + error;
                return false;
            }
        }
        active = std::move(migratedActive);
        return true;
    }
};

Bridge::Bridge(Config config) : impl_(new Impl(std::move(config))) {}
Bridge::~Bridge() { delete impl_; }
Bridge::Bridge(Bridge&& other) noexcept : impl_(std::exchange(other.impl_, nullptr)) {}
Bridge& Bridge::operator=(Bridge&& other) noexcept {
    if (this != &other) {
        delete impl_;
        impl_ = std::exchange(other.impl_, nullptr);
    }
    return *this;
}

const std::filesystem::path& Bridge::ModDirectory() const noexcept {
    static const std::filesystem::path empty;
    return impl_ ? impl_->modDirectory : empty;
}

Capabilities Bridge::GetCapabilities() const {
    Capabilities capabilities;
    if (impl_) {
        capabilities.modDirectory = impl_->modDirectory;
        capabilities.available = impl_->initializationError.empty();
    } else {
        capabilities.available = false;
    }
    capabilities.wicImageConversion = detail::SupportsWicConversion();
    capabilities.textureOperations = detail::SupportsTextureOperations();
    if (impl_) {
        capabilities.maxTextureBytes = impl_->config.maxTextureBytes;
        capabilities.maxTextureDimension = impl_->config.maxTextureDimension;
        capabilities.maxActiveProfiles = impl_->config.maxActiveProfiles;
        capabilities.maxActiveTextureFiles = impl_->config.maxActiveTextureFiles;
        capabilities.maxActiveTextureBytes = impl_->config.maxActiveTextureBytes;
    }
    return capabilities;
}

Result Bridge::ApplyLegacyMaterial(const ApplyRequest& request) {
    if (request.operation == BridgeOperation::Preview) {
        return Result::Failure("preview_unsupported",
            "protocol 1 cannot provide reversible live preview without publishing persistent USDA state");
    }
    if (request.operation != BridgeOperation::Commit) {
        return Result::Failure("invalid_apply_operation",
            "restore and clear operations must use ClearLegacyMaterial");
    }
    if (!impl_ || !impl_->initializationError.empty()) {
        return Result::Failure("not_initialized", impl_ ? impl_->initializationError : "bridge was moved from");
    }
    std::lock_guard<std::mutex> lock(impl_->mutex);

    std::string error;
    if (!ValidateProfileId(request.profileId, error)) {
        return Result::Failure("invalid_profile_id", error);
    }
    if (request.hashes.empty() || request.hashes.size() > 128) {
        return Result::Failure("invalid_hash_count", "one to 128 texture hashes are required");
    }
    if (!detail::ValidateMaterial(request.material, error)) {
        return Result::Failure("invalid_material", error);
    }

    std::vector<std::string> hashes;
    hashes.reserve(request.hashes.size());
    for (const auto& input : request.hashes) {
        std::string canonical;
        if (!CanonicalizeHash(input, canonical, error)) {
            return Result::Failure("invalid_hash", error);
        }
        hashes.push_back(std::move(canonical));
    }
    std::sort(hashes.begin(), hashes.end());
    hashes.erase(std::unique(hashes.begin(), hashes.end()), hashes.end());

    std::vector<std::string> active;
    if (!impl_->LoadActiveProfiles(active, error)) {
        return Result::Failure("mod_index_read_failed", error);
    }
    if (!impl_->PruneInactiveOwnedFiles(active, error)) {
        return Result::Failure("owned_cache_cleanup_failed", error);
    }
    const auto failAndClean = [this, &active](Result failure) {
        std::string cleanupError;
        if (!impl_->PruneInactiveOwnedFiles(active, cleanupError)) {
            if (!failure.message.empty()) failure.message += "; ";
            failure.message += "transaction cleanup also failed: " + cleanupError;
        }
        return failure;
    };

    std::vector<std::string> nextActive = active;
    if (!impl_->StageLegacyMigration(request.profileId, nextActive, error)) {
        return failAndClean(Result::Failure("legacy_profile_migration_failed", error));
    }
    for (const auto& hash : hashes) {
        RemoveHash(nextActive, hash);
    }
    NormalizeProfiles(nextActive);
    if (nextActive.size() > impl_->config.maxActiveProfiles ||
        hashes.size() > impl_->config.maxActiveProfiles - nextActive.size()) {
        return failAndClean(Result::Failure("resource_limit",
            "prospective replacement set exceeds the active profile limit"));
    }

    std::vector<detail::ImportedTexture> imported;
    imported.reserve(request.material.textures.size());
    detail::TextureImportConfig textureConfig{
        impl_->config.gameRoot, impl_->modDirectory, impl_->allowedRoots, impl_->config.maxTextureBytes,
        impl_->config.maxTextureDimension};
    for (const auto& texture : request.material.textures) {
        detail::ImportedTexture result;
        auto status = detail::ImportTexture(textureConfig, request.profileId, texture, result);
        if (!status.ok) {
            return failAndClean(std::move(status));
        }
        imported.push_back(std::move(result));
    }
    std::sort(imported.begin(), imported.end(), [](const auto& left, const auto& right) {
        return static_cast<int>(left.role) < static_cast<int>(right.role);
    });

    std::vector<std::filesystem::path> layers;
    layers.reserve(hashes.size());
    for (const auto& hash : hashes) {
        const auto layerContents = detail::BuildProfileLayer({hash}, request.material, imported);
        const auto id = VersionedProfileId(hash, layerContents);
        std::filesystem::path layer;
        if (!impl_->StageImmutableProfile(id, layerContents, layer, error)) {
            return failAndClean(Result::Failure("profile_stage_failed", error));
        }
        if (impl_->Injected("apply_after_stage_layer")) {
            return failAndClean(Result::Failure("injected_failure",
                "test failure injected after staging an immutable profile layer"));
        }
        RemoveHash(nextActive, hash);
        nextActive.push_back(id);
        layers.push_back(layer);
    }
    NormalizeProfiles(nextActive);
    if (!impl_->ValidateActiveTextureQuota(nextActive, error)) {
        return failAndClean(Result::Failure("resource_limit", error));
    }

    const bool textureChanged = std::any_of(imported.begin(), imported.end(),
        [](const detail::ImportedTexture& texture) { return texture.changed; });
    if (impl_->Injected("apply_before_root_publish")) {
        return failAndClean(Result::Failure("injected_failure",
            "test failure injected before the transactional root publication"));
    }
    auto modStatus = impl_->WriteModLast(nextActive, textureChanged);
    if (!modStatus.ok) {
        return failAndClean(std::move(modStatus));
    }
    auto result = Result::Success(request.profileId);
    result.layerPaths = layers;
    if (!layers.empty()) result.layerPath = layers.front();
    for (const auto& texture : imported) {
        result.importedTextures.push_back(texture.absolutePath);
    }
    return result;
}

Result Bridge::ClearLegacyMaterial(const ClearRequest& request) {
    if (!impl_ || !impl_->initializationError.empty()) {
        return Result::Failure("not_initialized", impl_ ? impl_->initializationError : "bridge was moved from");
    }
    std::lock_guard<std::mutex> lock(impl_->mutex);

    std::string error;
    if (!ValidateProfileId(request.profileId, error)) {
        return Result::Failure("invalid_profile_id", error);
    }
    if (request.hashes.empty() || request.hashes.size() > 128) {
        return Result::Failure("invalid_hash_count",
            "one to 128 exact texture hashes are required for clear");
    }

    std::vector<std::string> hashes;
    hashes.reserve(request.hashes.size());
    for (const auto& input : request.hashes) {
        std::string canonical;
        if (!CanonicalizeHash(input, canonical, error)) {
            return Result::Failure("invalid_hash", error);
        }
        hashes.push_back(std::move(canonical));
    }
    std::sort(hashes.begin(), hashes.end());
    hashes.erase(std::unique(hashes.begin(), hashes.end()), hashes.end());

    std::vector<std::string> active;
    if (!impl_->LoadActiveProfiles(active, error)) {
        return Result::Failure("mod_index_read_failed", error);
    }
    if (!impl_->PruneInactiveOwnedFiles(active, error)) {
        return Result::Failure("owned_cache_cleanup_failed", error);
    }
    const auto previousActive = active;
    const auto failAndClean = [this, &previousActive](Result failure) {
        std::string cleanupError;
        if (!impl_->PruneInactiveOwnedFiles(previousActive, cleanupError)) {
            if (!failure.message.empty()) failure.message += "; ";
            failure.message += "transaction cleanup also failed: " + cleanupError;
        }
        return failure;
    };
    if (!impl_->StageLegacyMigration(request.profileId, active, error)) {
        return failAndClean(Result::Failure("legacy_profile_migration_failed", error));
    }

    auto result = Result::Success(request.profileId);
    for (const auto& hash : hashes) {
        for (const auto& id : active) {
            const auto layerHash = HashFromProfileId(id);
            if (layerHash && *layerHash == hash) {
                result.layerPaths.push_back(impl_->ProfilePath(id));
            }
        }
        RemoveHash(active, hash);
    }
    NormalizeProfiles(active);

    if (impl_->Injected("clear_before_root_publish")) {
        return failAndClean(Result::Failure("injected_failure",
            "test failure injected before the exact-hash root publication"));
    }
    auto modStatus = impl_->WriteModLast(active);
    if (!modStatus.ok) {
        return failAndClean(std::move(modStatus));
    }
    if (!result.layerPaths.empty()) result.layerPath = result.layerPaths.front();
    return result;
}

Result Bridge::ClearAllOwned() {
    if (!impl_ || !impl_->initializationError.empty()) {
        return Result::Failure("not_initialized",
            impl_ ? impl_->initializationError : "bridge was moved from");
    }
    std::lock_guard<std::mutex> lock(impl_->mutex);

    if (impl_->Injected("reset_before_root_publish")) {
        return Result::Failure("injected_failure",
            "test failure injected before the empty root publication");
    }
    // The empty root is the O(1) deactivation commit point. Never walk or
    // mutate individual layers before it has been atomically published.
    auto modStatus = impl_->WriteModLast({});
    if (!modStatus.ok) {
        return modStatus;
    }
    if (impl_->Injected("reset_after_root_publish")) {
        return Result::Failure("injected_failure_after_commit",
            "test failure injected after empty root publication and before retirement");
    }

    auto result = Result::Success();
    std::string error;
    bool profilesRotated = false;
    std::filesystem::path retiredProfiles;
    if (!impl_->RetireOwnedDirectory("profiles", "profiles.retired",
                                     profilesRotated, retiredProfiles, error)) {
        return Result::Failure("authoritative_reset_retire_failed_after_commit", error);
    }
    if (profilesRotated) {
        result.layerPath = retiredProfiles;
        result.layerPaths.push_back(retiredProfiles);
    }
    if (profilesRotated && impl_->Injected("reset_after_profile_retire")) {
        return Result::Failure("injected_failure_after_commit",
            "test failure injected after profile retirement and before texture retirement");
    }

    // Imported DDS names are immutable/content-addressed too. Rotate the whole
    // cache only after the empty activation root is durable, and keep one fixed
    // quarantine generation. Crucially this runs even when profiles/ was
    // already retired by a prior failed attempt, making the two-step reset
    // independently retryable after a crash.
    bool texturesRotated = false;
    std::filesystem::path retiredTextures;
    if (!impl_->RetireOwnedDirectory("textures", "textures.retired",
                                     texturesRotated, retiredTextures, error)) {
        return Result::Failure("authoritative_reset_retire_failed_after_commit", error);
    }
    return result;
}

} // namespace advmat::rtx_bridge
