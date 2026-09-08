#define NOMINMAX
#include "workshop_discovery.h"
#include <windows.h>
#include <algorithm>
#include <cstdint>
#include <cwctype>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <utility>

namespace astra::startup_assets {
namespace {
namespace fs = std::filesystem;
using Json = nlohmann::json;
using MetadataReader = std::function<std::optional<std::string>(const fs::path&)>;
constexpr std::size_t kMaxMetadataBytes = 8 * 1024 * 1024;
constexpr std::size_t kMaxNodes = 262144;
constexpr std::size_t kMaxSubscriptions = 32768;
constexpr std::size_t kMaxLibraries = 64;
constexpr std::size_t kMaxSources = 4096;

std::string Lower(std::string value) {
    for (auto& c : value) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    return value;
}

struct Node {
    bool object = true;
    std::string value;
    std::vector<std::pair<std::string, Node>> children;
    std::map<std::string, std::size_t> fields;
};

// Steam's text VDF is KeyValues, not JSON. Preserve its tree boundaries and
// reject directives, duplicate queried fields and malformed/truncated input.
class KeyValues {
    const std::string& input_;
    std::size_t at_ = 0, nodes_ = 0;
    void Fail(const char* why) const { throw std::runtime_error(why); }
    static bool Space(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }
    void Skip() {
        for (;;) {
            while (at_ < input_.size() && Space(input_[at_])) ++at_;
            if (input_.compare(at_, 2, "//") == 0) {
                at_ += 2;
                while (at_ < input_.size() && input_[at_] != '\n') ++at_;
            } else if (input_.compare(at_, 2, "/*") == 0) {
                const auto end = input_.find("*/", at_ + 2);
                if (end == std::string::npos) Fail("unterminated KeyValues comment");
                at_ = end + 2;
            } else return;
        }
    }
    std::string Token() {
        Skip();
        if (at_ >= input_.size() || input_[at_] == '{' || input_[at_] == '}') Fail("missing KeyValues token");
        std::string result;
        if (input_[at_] == '"') {
            ++at_;
            bool closed = false;
            while (at_ < input_.size()) {
                char c = input_[at_++];
                if (c == '"') { closed = true; break; }
                if (c == '\\') {
                    if (at_ >= input_.size()) Fail("truncated KeyValues escape");
                    c = input_[at_++];
                    if (c == 'n') c = '\n';
                    else if (c == 'r') c = '\r';
                    else if (c == 't') c = '\t';
                    else if (c != '\\' && c != '"') Fail("unknown KeyValues escape");
                }
                if (c == '\0') Fail("NUL in KeyValues token");
                result += c;
                if (result.size() > 32768) Fail("KeyValues token exceeds limit");
            }
            if (!closed) Fail("unterminated KeyValues string");
        } else {
            while (at_ < input_.size() && !Space(input_[at_]) && input_[at_] != '{' && input_[at_] != '}') {
                if (input_[at_] == '"' || input_[at_] == '\0') Fail("invalid unquoted KeyValues token");
                result += input_[at_++];
                if (result.size() > 32768) Fail("KeyValues token exceeds limit");
            }
        }
        if (!result.empty() && result.front() == '#') Fail("KeyValues directives are not allowed");
        return result;
    }
    Node Object(unsigned int depth, bool closing) {
        if (depth > 32) Fail("KeyValues nesting exceeds limit");
        Node output;
        for (;;) {
            Skip();
            if (at_ == input_.size()) {
                if (closing) Fail("unclosed KeyValues object");
                return output;
            }
            if (input_[at_] == '}') {
                if (!closing) Fail("unexpected KeyValues closing brace");
                ++at_; return output;
            }
            const auto key = Token();
            if (key.empty() || key.size() > 2048) Fail("invalid KeyValues key");
            if (++nodes_ > kMaxNodes) Fail("KeyValues node count exceeds limit");
            Skip();
            Node value;
            if (at_ < input_.size() && input_[at_] == '{') {
                ++at_; value = Object(depth + 1, true);
            } else {
                value.object = false; value.value = Token();
            }
            if (!output.fields.emplace(Lower(key), output.children.size()).second)
                Fail("duplicate KeyValues key");
            output.children.emplace_back(key, std::move(value));
        }
    }
public:
    explicit KeyValues(const std::string& input) : input_(input) {}
    Node Parse() {
        if (input_.size() > kMaxMetadataBytes) Fail("KeyValues metadata exceeds 8 MiB");
        if (input_.compare(0, 3, "\xef\xbb\xbf") == 0) at_ = 3;
        return Object(0, false);
    }
};

const Node* Field(const Node& node, const std::string& name) {
    if (!node.object) throw std::runtime_error("expected KeyValues object");
    const auto field = node.fields.find(Lower(name));
    return field == node.fields.end() ? nullptr : &node.children.at(field->second).second;
}

std::optional<std::string> Text(const Node& node, const std::string& name) {
    const auto* value = Field(node, name);
    if (!value) return std::nullopt;
    if (value->object) throw std::runtime_error("expected scalar KeyValues field: " + name);
    return value->value;
}

std::uint64_t Decimal(const std::string& value, bool allowZero = false) {
    if (value.empty() || value.size() > 20 || (value.size() > 1 && value.front() == '0'))
        throw std::runtime_error("invalid decimal identifier");
    std::uint64_t result = 0;
    for (const auto c : value) {
        if (c < '0' || c > '9' || result > (std::numeric_limits<std::uint64_t>::max() - (c - '0')) / 10)
            throw std::runtime_error("invalid decimal identifier");
        result = result * 10 + (c - '0');
    }
    if (!result && !allowZero) throw std::runtime_error("zero identifier is not allowed");
    return result;
}

std::wstring PathKey(const fs::path& path) {
    auto key = fs::absolute(path).lexically_normal().native();
    std::transform(key.begin(), key.end(), key.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return key;
}

std::wstring NativePath(const fs::path& path) {
    auto value = fs::absolute(path).lexically_normal().native();
    std::replace(value.begin(), value.end(), L'/', L'\\');
    if (value.rfind(L"\\\\?\\", 0) == 0) return value;
    if (value.rfind(L"\\\\", 0) == 0) return L"\\\\?\\UNC\\" + value.substr(2);
    return L"\\\\?\\" + value;
}

// Validate directory ancestors without following symlinks or junctions. File
// identity additionally deduplicates case and short-name aliases of libraries.
std::optional<std::pair<DWORD, std::uint64_t>> DirectoryIdentity(const fs::path& supplied) {
    const auto path = fs::absolute(supplied).lexically_normal();
    fs::path part = path.root_path();
    for (const auto& component : path.relative_path()) {
        part /= component;
        const auto attributes = GetFileAttributesW(NativePath(part).c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
            !(attributes & FILE_ATTRIBUTE_DIRECTORY)) return std::nullopt;
    }
    const auto handle = CreateFileW(NativePath(path).c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return std::nullopt;
    BY_HANDLE_FILE_INFORMATION information{};
    const bool success = GetFileInformationByHandle(handle, &information) != 0;
    CloseHandle(handle);
    if (!success || (information.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) return std::nullopt;
    return std::make_pair(information.dwVolumeSerialNumber,
        (static_cast<std::uint64_t>(information.nFileIndexHigh) << 32) | information.nFileIndexLow);
}

void Skip(Json& report, const std::string& reason) {
    report["skipped"][reason] = report["skipped"].value(reason, std::uint64_t{0}) + 1;
}

void Warning(Json& report, const std::string& reason, const fs::path& path, const std::string& detail = {}) {
    Skip(report, reason);
    if (report["warnings"].size() < 64)
        report["warnings"].push_back({{"reason", reason}, {"path", path.u8string()}, {"detail", detail}});
}

std::optional<Node> Read(const MetadataReader& reader, const fs::path& path, Json& report) {
    try {
        const auto bytes = reader(path);
        if (!bytes) return std::nullopt;
        return KeyValues(*bytes).Parse();
    } catch (const std::exception& error) {
        Warning(report, "metadata_invalid", path, error.what());
        return std::nullopt;
    }
}

std::optional<fs::path> RegistrySteamRoot() {
    DWORD type = 0, bytes = 0;
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath", RRF_RT_REG_SZ,
        &type, nullptr, &bytes) != ERROR_SUCCESS || bytes < sizeof(wchar_t) || bytes > 65536) return std::nullopt;
    std::vector<wchar_t> value(bytes / sizeof(wchar_t) + 1, L'\0');
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath", RRF_RT_REG_SZ,
        &type, value.data(), &bytes) != ERROR_SUCCESS) return std::nullopt;
    const fs::path path(value.data());
    return path.is_absolute() ? std::optional<fs::path>{path} : std::nullopt;
}

std::optional<std::string> RegistryAccount() {
    DWORD account = 0, bytes = sizeof(account);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam\\ActiveProcess", L"ActiveUser", RRF_RT_REG_DWORD,
        nullptr, &account, &bytes) == ERROR_SUCCESS && account) return std::to_string(account);
    return std::nullopt;
}

std::optional<std::string> RecentAccount(const fs::path& steamRoot, const MetadataReader& reader, Json& report) {
    const auto path = steamRoot / "config/loginusers.vdf";
    const auto data = Read(reader, path, report);
    if (!data) return std::nullopt;
    try {
        const auto* users = Field(*data, "users");
        if (!users || !users->object || users->children.size() > 256) throw std::runtime_error("invalid loginusers inventory");
        std::optional<std::string> result;
        for (const auto& user : users->children) {
            if (Text(user.second, "MostRecent") != std::optional<std::string>{"1"}) continue;
            constexpr std::uint64_t base = 76561197960265728ull;
            const auto steamId = Decimal(user.first);
            if (steamId <= base || steamId - base > std::numeric_limits<std::uint32_t>::max() || result)
                throw std::runtime_error("ambiguous most-recent Steam account");
            result = std::to_string(steamId - base);
        }
        return result;
    } catch (const std::exception& error) {
        Warning(report, "account_metadata_invalid", path, error.what());
        return std::nullopt;
    }
}

bool Pending(const Node& item) {
    for (const auto* key : {"NeedsUpdate", "NeedsDownload", "Downloading", "DownloadPending"}) {
        const auto value = Text(item, key);
        if (value && *value != "0") return true;
    }
    return false;
}

bool SubscribedBy(const Node& item, const std::string& account) {
    const auto owners = Text(item, "subscribedby");
    if (!owners) return true; // The per-user subscription inventory is mandatory.
    bool found = false;
    std::size_t begin = 0;
    do {
        const auto end = owners->find(',', begin);
        const auto owner = owners->substr(begin, end == std::string::npos ? end : end - begin);
        Decimal(owner);
        if (owner == account) found = true;
        if (end == std::string::npos) break;
        begin = end + 1;
    } while (begin <= owners->size());
    return found;
}

bool PathPresent(const fs::path& path) {
    const auto attributes = GetFileAttributesW(NativePath(path).c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES) return true;
    const auto error = GetLastError();
    // Uninspectable staging paths are uncertainty, not proof of completion.
    return error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND;
}

} // namespace

std::vector<fs::path> DiscoverWorkshopSources(const fs::path& gameRoot, const WorkshopOptions& options,
    const MetadataReader& readMetadata, Json& report) {
    report = {{"enabled", options.enabled}, {"phase", "disabled"}, {"selected", 0},
              {"skipped", Json::object()}, {"warnings", Json::array()}};
    std::vector<fs::path> result;
    if (!options.enabled) return result;
    report["phase"] = "unavailable";
    try {
        const auto steamRoot = options.steamRoot ? options.steamRoot : RegistrySteamRoot();
        if (!steamRoot || !steamRoot->is_absolute() || !DirectoryIdentity(*steamRoot)) {
            report["reason"] = "Steam root is unavailable or unsafe"; return result;
        }
        // Explicit Steam roots are isolated from host identity, including tests
        // and offline tools; only that root's loginusers cache may fill the user.
        auto account = options.accountId;
        if (!account && !options.steamRoot) account = RegistryAccount();
        if (!account) account = RecentAccount(*steamRoot, readMetadata, report);
        if (!account || Decimal(*account) > std::numeric_limits<std::uint32_t>::max()) {
            report["reason"] = "Current Steam account is unavailable"; return result;
        }
        report["steam_root"] = steamRoot->u8string();
        const auto subscriptionsPath = *steamRoot / "userdata" / *account / "ugc/4000_subscriptions.vdf";
        const auto subscriptionData = Read(readMetadata, subscriptionsPath, report);
        if (!subscriptionData) { report["reason"] = "Current-user subscription cache is missing or invalid"; return result; }
        const auto* subscriptions = Field(*subscriptionData, "subscribedfiles");
        if (!subscriptions || Text(*subscriptions, "appid") != std::optional<std::string>{"4000"})
            throw std::runtime_error("subscription cache has wrong root or app ID");
        std::set<std::string> selected, seen;
        for (const auto& entry : subscriptions->children) {
            if (!entry.second.object) continue;
            if (seen.size() >= kMaxSubscriptions) throw std::runtime_error("subscription inventory exceeds limit");
            const auto id = Text(entry.second, "publishedfileid");
            if (!id) throw std::runtime_error("subscription entry has no published file ID");
            Decimal(*id);
            if (!seen.insert(*id).second) throw std::runtime_error("duplicate subscription ID");
            if (Text(entry.second, "disabled_locally") != std::optional<std::string>{"0"}) {
                Skip(report, "disabled_locally"); continue;
            }
            selected.insert(*id);
        }
        report["subscriptions"] = seen.size();
        const auto disabledPath = gameRoot / "garrysmod/cfg/addonnomount.txt";
        // Missing disable config means the engine has no saved exclusions. A
        // malformed existing config cannot be treated as enabling every addon.
        std::optional<std::string> disabledBytes;
        try { disabledBytes = readMetadata(disabledPath); }
        catch (const std::exception& error) {
            Warning(report, "disabled_config_invalid", disabledPath, error.what());
            report["reason"] = "Cannot verify Garry's Mod disabled addons"; return result;
        }
        if (disabledBytes) {
            const auto disabledData = KeyValues(*disabledBytes).Parse();
            const auto* disabled = Field(disabledData, "addonnomount");
            if (!disabled || !disabled->object) throw std::runtime_error("invalid Garry's Mod disabled-addon config");
            for (const auto& entry : disabled->children) {
                if (entry.second.object) throw std::runtime_error("invalid disabled-addon entry");
                Decimal(entry.second.value);
                if (selected.erase(entry.second.value)) Skip(report, "disabled_in_gmod");
            }
        }

        struct Library { fs::path path; unsigned int priority = 0; };
        std::vector<Library> libraries;
        std::map<std::wstring, std::size_t> libraryPaths;
        std::map<std::pair<DWORD, std::uint64_t>, std::size_t> libraryIdentities;
        bool libraryLimitExceeded = false;
        bool declaredAppOwner = false;
        const auto addLibrary = [&](const fs::path& library, unsigned int priority = 0) {
            if (!library.is_absolute()) { Warning(report, "library_unsafe", library); return; }
            const auto key = PathKey(library);
            const auto knownPath = libraryPaths.find(key);
            if (knownPath != libraryPaths.end()) {
                auto& current = libraries.at(knownPath->second).priority;
                current = std::max(current, priority); return;
            }
            const auto identity = DirectoryIdentity(library);
            if (!identity) { Warning(report, "library_unsafe", library); return; }
            const auto knownIdentity = libraryIdentities.find(*identity);
            if (knownIdentity != libraryIdentities.end()) {
                libraryPaths.emplace(key, knownIdentity->second);
                auto& current = libraries.at(knownIdentity->second).priority;
                current = std::max(current, priority); return;
            }
            if (libraries.size() >= kMaxLibraries) {
                libraryLimitExceeded = true;
                throw std::runtime_error("Steam library count exceeds limit");
            }
            libraryPaths.emplace(key, libraries.size());
            libraryIdentities.emplace(*identity, libraries.size());
            libraries.push_back({library.lexically_normal(), priority});
        };
        addLibrary(*steamRoot);
        auto librariesPath = *steamRoot / "steamapps/libraryfolders.vdf";
        auto libraryData = Read(readMetadata, librariesPath, report);
        if (!libraryData) {
            librariesPath = *steamRoot / "config/libraryfolders.vdf";
            libraryData = Read(readMetadata, librariesPath, report);
        }
        if (libraryData) {
            try {
                const auto* folders = Field(*libraryData, "libraryfolders");
                if (!folders || !folders->object)
                    throw std::runtime_error("invalid Steam library inventory");
                if (folders->children.size() > kMaxLibraries + 8) {
                    libraryLimitExceeded = true;
                    throw std::runtime_error("Steam library inventory exceeds limit");
                }
                for (const auto& entry : folders->children) {
                    // New Steam clients use objects; older text inventories use
                    // numeric scalar path entries alongside non-path metadata.
                    bool numeric = !entry.first.empty();
                    for (const auto c : entry.first) numeric = numeric && c >= '0' && c <= '9';
                    if (!numeric) continue;
                    try {
                        unsigned int priority = 0;
                        if (entry.second.object) {
                            const auto* apps = Field(entry.second, "apps");
                            if (apps && Field(*apps, "4000")) {
                                declaredAppOwner = true;
                                if (Text(*apps, "4000")) priority = 2;
                            }
                        }
                        const auto path = entry.second.object ? Text(entry.second, "path") : std::optional<std::string>{entry.second.value};
                        if (!path) { Skip(report, "library_missing_path"); continue; }
                        addLibrary(fs::u8path(*path), priority);
                    } catch (const std::exception& error) {
                        Warning(report, "library_entry_invalid", librariesPath, entry.first + ": " + error.what());
                    }
                }
            } catch (const std::exception& error) { Warning(report, "library_metadata_invalid", librariesPath, error.what()); }
        }
        // A copied RTX game can live beside the actual Steam install, or in an
        // additional library not present in the current libraryfolders snapshot.
        auto ancestor = fs::absolute(gameRoot).lexically_normal();
        for (unsigned int depth = 0; depth < 16 && !ancestor.empty(); ++depth) {
            if (Lower(ancestor.filename().u8string()) == "steamapps") {
                const auto appPath = ancestor / "appmanifest_4000.acf";
                const auto appData = Read(readMetadata, appPath, report);
                if (appData) {
                    try {
                        const auto* app = Field(*appData, "AppState");
                        if (app && Text(*app, "appid") == std::optional<std::string>{"4000"}) addLibrary(ancestor.parent_path(), 1);
                    } catch (const std::exception& error) { Warning(report, "app_metadata_invalid", appPath, error.what()); }
                }
                break;
            }
            const auto parent = ancestor.parent_path();
            if (parent == ancestor) break;
            ancestor = parent;
        }
        if (libraryLimitExceeded) throw std::runtime_error("Steam library count exceeds limit");
        report["libraries"] = libraries.size();
        // An offline or unsafe declared owner is still authoritative: do not
        // silently use a retained copy in some other library while it is absent.
        const bool listedOwner = declaredAppOwner;
        if (!listedOwner) {
            for (auto& library : libraries) {
                if (library.priority) continue;
                const auto appPath = library.path / "steamapps/appmanifest_4000.acf";
                const auto data = Read(readMetadata, appPath, report);
                if (!data) continue;
                try {
                    const auto* app = Field(*data, "AppState");
                    if (app && Text(*app, "appid") == std::optional<std::string>{"4000"}) library.priority = 1;
                } catch (const std::exception& error) { Warning(report, "app_metadata_invalid", appPath, error.what()); }
            }
        }
        const unsigned int priority = listedOwner ? 2 : 1;
        report["library_policy"] = listedOwner ? "libraryfolders_app_owner" : "appmanifest_owner";
        report["eligible_libraries"] = 0;
        struct Candidate { fs::path path; std::string manifest; std::pair<DWORD, std::uint64_t> identity; bool ambiguous = false; };
        std::map<std::string, Candidate> candidates;
        for (const auto& library : libraries) {
            // Installed caches can survive a Steam library move. Once Steam
            // declares an app owner, never fall back to a stale other library.
            if (library.priority != priority) { Skip(report, "library_not_app_owner"); continue; }
            report["eligible_libraries"] = report["eligible_libraries"].get<std::size_t>() + 1;
            const auto workshop = library.path / "steamapps/workshop";
            const auto metadataPath = workshop / "appworkshop_4000.acf";
            const auto metadata = Read(readMetadata, metadataPath, report);
            if (!metadata) { Skip(report, "library_without_install_metadata"); continue; }
            try {
                const auto* app = Field(*metadata, "AppWorkshop");
                if (!app || Text(*app, "appid") != std::optional<std::string>{"4000"})
                    throw std::runtime_error("workshop metadata has wrong root or app ID");
                const auto* installed = Field(*app, "WorkshopItemsInstalled");
                const auto* details = Field(*app, "WorkshopItemDetails");
                if (!installed || !details || !installed->object || !details->object)
                    throw std::runtime_error("workshop install/detail inventories missing");
                for (const auto& id : selected) {
                    try {
                        const auto* item = Field(*installed, id);
                        const auto* detail = Field(*details, id);
                        if (!item || !detail) { Skip(report, "not_installed"); continue; }
                        const auto manifest = Text(*item, "manifest");
                        const auto size = Text(*item, "size");
                        if (!manifest || !size || !Decimal(*manifest) || !Decimal(*size) ||
                            Text(*detail, "manifest") != manifest || Text(*detail, "latest_manifest") != manifest) {
                            Skip(report, "incomplete_or_outdated"); continue;
                        }
                        if (!SubscribedBy(*detail, *account)) { Skip(report, "different_account"); continue; }
                        if (Pending(*item) || Pending(*detail) ||
                            PathPresent(workshop / "downloads/4000" / id) || PathPresent(workshop / "temp/4000" / id)) {
                            Skip(report, "download_pending"); continue;
                        }
                        const auto directory = workshop / "content/4000" / id;
                        const auto identity = DirectoryIdentity(directory);
                        if (!identity) { Warning(report, "item_directory_unsafe_or_missing", directory); continue; }
                        const auto found = candidates.find(id);
                        if (found == candidates.end()) {
                            candidates.emplace(id, Candidate{directory, *manifest, *identity});
                        } else if (found->second.manifest != *manifest) {
                            found->second.ambiguous = true;
                            Warning(report, "ambiguous_installed_version", directory, id);
                        }
                    } catch (const std::exception& error) {
                        Warning(report, "item_metadata_invalid", metadataPath, id + ": " + error.what());
                    }
                }
            } catch (const std::exception& error) { Warning(report, "install_metadata_invalid", metadataPath, error.what()); }
        }
        std::set<std::pair<DWORD, std::uint64_t>> sourceIdentities;
        for (const auto& candidate : candidates) {
            if (candidate.second.ambiguous || !sourceIdentities.insert(candidate.second.identity).second) continue;
            if (result.size() >= kMaxSources) throw std::runtime_error("Workshop source count exceeds limit");
            result.push_back(candidate.second.path);
        }
        report["selected"] = result.size();
        report["phase"] = report["eligible_libraries"] == 0 ? "unavailable" : "discovered";
        if (report["eligible_libraries"] == 0) report["reason"] = "No Steam library is a verified owner of app 4000";
        else if (result.empty()) report["reason"] = "No enabled subscriptions have a verified completed install";
        return result;
    } catch (const std::exception& error) {
        // Unknown user/config state must never activate a sweep of cached GMAs.
        report["phase"] = "unavailable";
        report["reason"] = error.what();
        report["selected"] = 0;
        return {};
    }
}

} // namespace astra::startup_assets
