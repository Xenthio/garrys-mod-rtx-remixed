#define NOMINMAX
#include "startup_assets.h"
#include <windows.h>
#include <bcrypt.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <fstream>
#include <limits>
#include <map>
#include <regex>
#include <set>
#include <stdexcept>
#include <string>

namespace astra::startup_assets {
namespace {
namespace fs = std::filesystem;
using Json = nlohmann::json;
constexpr std::uint64_t kManifestBytes = 8 * 1024 * 1024;
constexpr std::uint64_t kAssetBytes = 128 * 1024 * 1024;
constexpr std::size_t kMaxAssets = 32768;
constexpr std::size_t kMaxArchiveEntries = 262144;
constexpr std::size_t kMaxSources = 4096;
constexpr const char* kOwnedPrefix = "!astra_startup_";
constexpr const char* kEmptyLayer = "#usda 1.0\n(\n    subLayers = []\n)\n";

void Need(bool condition, const std::string& error) {
    if (!condition) throw std::runtime_error(error);
}

double Seconds(std::chrono::steady_clock::time_point began) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - began).count();
}

bool IsMap(const std::string& value) {
    return std::regex_match(value, std::regex("[a-z0-9_-]{1,96}"));
}
bool IsHash(const std::string& value) {
    return std::regex_match(value, std::regex("[a-f0-9]{64}"));
}

bool SafeRelative(const std::string& path) {
    if (path.empty() || path.size() > 240 || path.front() == '/' || path.back() == '/') return false;
    for (const unsigned char c : path) {
        if (c < 32 || c >= 127 || c == '\\' || c == ':' || c == '<' || c == '>' ||
            c == '"' || c == '|' || c == '?' || c == '*' || (c >= 'A' && c <= 'Z')) return false;
    }
    std::size_t at = 0;
    while (at < path.size()) {
        const auto end = path.find('/', at);
        const auto part = path.substr(at, end == std::string::npos ? end : end - at);
        if (part.empty() || part == "." || part == ".." || part.back() == '.' || part.back() == ' ') return false;
        const auto base = part.substr(0, part.find('.'));
        if (base == "con" || base == "prn" || base == "aux" || base == "nul" ||
            std::regex_match(base, std::regex("(com|lpt)[1-9]"))) return false;
        if (end == std::string::npos) break;
        at = end + 1;
    }
    return true;
}

std::wstring NativePath(const fs::path& path) {
    auto value = fs::absolute(path).lexically_normal().native();
    std::replace(value.begin(), value.end(), L'/', L'\\');
    if (value.rfind(L"\\\\?\\", 0) == 0) return value;
    if (value.rfind(L"\\\\", 0) == 0) return L"\\\\?\\UNC\\" + value.substr(2);
    return L"\\\\?\\" + value;
}

bool Exists(const fs::path& path) { return fs::exists(fs::path(NativePath(path))); }

void PlainPath(const fs::path& supplied) {
    const auto path = fs::absolute(supplied).lexically_normal();
    fs::path part = path.root_path();
    for (const auto& component : path.relative_path()) {
        part /= component;
        const DWORD attributes = GetFileAttributesW(NativePath(part).c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES) {
            const auto error = GetLastError();
            Need(error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND,
                 "Cannot inspect filesystem path: " + part.u8string());
        } else Need((attributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0,
                    "Reparse points are not allowed: " + part.u8string());
    }
}

Json FileStamp(const fs::path& path) {
    PlainPath(path);
    const fs::path native(NativePath(path));
    Need(fs::is_regular_file(native), "Missing regular file: " + path.u8string());
    return {{"bytes", fs::file_size(native)},
            {"mtime", fs::last_write_time(native).time_since_epoch().count()}};
}

void EnsureDirectory(const fs::path& path) {
    PlainPath(path);
    fs::create_directories(fs::path(NativePath(path)));
    PlainPath(path);
    Need(fs::is_directory(fs::path(NativePath(path))), "Not a directory: " + path.u8string());
}

class Hash {
    BCRYPT_ALG_HANDLE algorithm_ = nullptr;
    BCRYPT_HASH_HANDLE hash_ = nullptr;
public:
    Hash() {
        Need(BCryptOpenAlgorithmProvider(&algorithm_, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0,
             "BCrypt SHA256 provider unavailable");
        if (BCryptCreateHash(algorithm_, &hash_, nullptr, 0, nullptr, 0, 0) < 0) {
            BCryptCloseAlgorithmProvider(algorithm_, 0);
            algorithm_ = nullptr;
            throw std::runtime_error("BCrypt SHA256 hash unavailable");
        }
    }
    ~Hash() {
        if (hash_) BCryptDestroyHash(hash_);
        if (algorithm_) BCryptCloseAlgorithmProvider(algorithm_, 0);
    }
    void Update(const char* bytes, std::size_t size) {
        Need(size <= std::numeric_limits<ULONG>::max(), "Hash chunk too large");
        Need(BCryptHashData(hash_, reinterpret_cast<PUCHAR>(const_cast<char*>(bytes)),
                            static_cast<ULONG>(size), 0) >= 0, "SHA256 update failed");
    }
    std::string Finish() {
        std::array<unsigned char, 32> result{};
        Need(BCryptFinishHash(hash_, result.data(), static_cast<ULONG>(result.size()), 0) >= 0,
             "SHA256 finalization failed");
        constexpr char hex[] = "0123456789abcdef";
        std::string text;
        for (auto byte : result) {
            text += hex[byte >> 4];
            text += hex[byte & 15];
        }
        return text;
    }
};

class AtomicOutput {
    fs::path destination_, temporary_;
    HANDLE file_ = INVALID_HANDLE_VALUE;
public:
    explicit AtomicOutput(const fs::path& destination) : destination_(destination) {
        EnsureDirectory(destination.parent_path());
        PlainPath(destination);
        for (unsigned int attempt = 0; attempt < 100; ++attempt) {
            temporary_ = destination;
            temporary_ += L".astra_tmp_" + std::to_wstring(GetCurrentProcessId()) + L"_" +
                          std::to_wstring(GetTickCount64()) + L"_" + std::to_wstring(attempt);
            file_ = CreateFileW(NativePath(temporary_).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
            if (file_ != INVALID_HANDLE_VALUE) return;
            Need(GetLastError() == ERROR_FILE_EXISTS, "Could not create atomic output: " + destination.u8string());
        }
        throw std::runtime_error("Could not reserve atomic output filename");
    }
    ~AtomicOutput() {
        if (file_ != INVALID_HANDLE_VALUE) CloseHandle(file_);
        if (!temporary_.empty()) DeleteFileW(NativePath(temporary_).c_str());
    }
    void Write(const char* bytes, std::size_t size) {
        Need(size <= std::numeric_limits<DWORD>::max(), "Write chunk too large");
        DWORD written = 0;
        Need(WriteFile(file_, bytes, static_cast<DWORD>(size), &written, nullptr) && written == size,
             "Atomic output write failed");
    }
    void Commit() {
        Need(FlushFileBuffers(file_) != 0, "Atomic output flush failed");
        CloseHandle(file_);
        file_ = INVALID_HANDLE_VALUE;
        PlainPath(destination_);
        PlainPath(temporary_);
        Need(MoveFileExW(NativePath(temporary_).c_str(), NativePath(destination_).c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0,
             "Atomic output publication failed: " + destination_.u8string());
        temporary_.clear();
    }
};

void WriteText(const fs::path& path, const std::string& text) {
    AtomicOutput output(path);
    output.Write(text.data(), text.size());
    output.Commit();
}

struct Member {
    fs::path source;
    std::uint64_t offset = 0, bytes = 0;
    std::optional<std::uint32_t> crc;
    Json stamp;
};
using Inventory = std::map<std::string, std::vector<Member>>;

class Reader {
    std::ifstream stream_;
    std::uint64_t remaining_;
    Json& report_;
public:
    Reader(const Member& member, Json& report) : remaining_(member.bytes), report_(report) {
        Need(FileStamp(member.source) == member.stamp, "Input changed since discovery: " + member.source.u8string());
        stream_.open(fs::path(NativePath(member.source)), std::ios::binary);
        Need(stream_.good(), "Could not read input: " + member.source.u8string());
        stream_.seekg(static_cast<std::streamoff>(member.offset));
        Need(stream_.good(), "Could not seek input member");
    }
    void Read(char* bytes, std::size_t size) {
        Need(size <= remaining_, "Truncated input member");
        stream_.read(bytes, static_cast<std::streamsize>(size));
        const auto count = static_cast<std::uint64_t>(stream_.gcount());
        report_["bytes_read"] = report_.value("bytes_read", std::uint64_t{0}) + count;
        Need(count == size, "Truncated input stream");
        remaining_ -= count;
    }
    std::uint64_t Number(unsigned int size) {
        std::array<char, 8> bytes{};
        Read(bytes.data(), size);
        std::uint64_t value = 0;
        for (unsigned int i = 0; i < size; ++i) value |= static_cast<std::uint64_t>(static_cast<unsigned char>(bytes[i])) << (i * 8);
        return value;
    }
    std::string String(std::size_t maximum) {
        std::string text;
        for (std::size_t i = 0; i <= maximum; ++i) {
            char byte = 0;
            Read(&byte, 1);
            if (!byte) return text;
            text += byte;
        }
        throw std::runtime_error("Input string exceeds bound");
    }
    std::uint64_t Remaining() const { return remaining_; }
};

bool Relevant(const std::string& path) { return path.rfind("data_static/astra/", 0) == 0; }

void ScanArchive(const fs::path& path, Inventory& inventory, Json& report) {
    const auto stamp = FileStamp(path);
    Member archive{path, 0, stamp.at("bytes").get<std::uint64_t>(), std::nullopt, stamp};
    Reader reader(archive, report);
    char magic[4];
    reader.Read(magic, 4);
    Need(std::string(magic, 4) == "GMAD" && reader.Number(1) == 3, "Unsupported GMA header: " + path.u8string());
    reader.Number(8); reader.Number(8);
    Need(reader.String(1024).empty(), "Required-content GMA is unsupported");
    reader.String(1024 * 1024); reader.String(1024 * 1024); reader.String(1024 * 1024);
    reader.Number(4);
    std::vector<std::pair<std::string, Member>> entries;
    std::uint64_t payloadBytes = 0;
    std::set<std::string> names;
    std::size_t count = 0;
    while (reader.Number(4)) {
        Need(++count <= kMaxArchiveEntries, "GMA table exceeds entry bound");
        const auto name = reader.String(4096);
        const auto size = reader.Number(8);
        const auto crc = static_cast<std::uint32_t>(reader.Number(4));
        Need(size <= archive.bytes && payloadBytes <= archive.bytes - size, "Invalid GMA payload lengths");
        if (Relevant(name)) {
            Need(SafeRelative(name) && names.insert(name).second, "Unsafe or duplicate Astra virtual path in GMA");
            entries.emplace_back(name, Member{path, payloadBytes, size, crc, stamp});
        }
        payloadBytes += size;
    }
    const auto dataOffset = archive.bytes - reader.Remaining();
    Need(reader.Remaining() >= 4 && payloadBytes <= reader.Remaining() - 4, "GMA payload is truncated");
    Need(FileStamp(path) == stamp, "GMA changed during table discovery");
    for (auto& entry : entries) {
        entry.second.offset += dataOffset;
        inventory[entry.first].push_back(std::move(entry.second));
    }
}

void ScanLoose(const fs::path& addon, Inventory& inventory) {
    const auto base = addon / "data_static/astra";
    PlainPath(base);
    if (!Exists(base)) return;
    Need(fs::is_directory(base), "Astra data_static root is not a directory");
    std::size_t count = 0;
    for (const auto& entry : fs::recursive_directory_iterator(base)) {
        Need(++count <= kMaxArchiveEntries, "Loose Astra tree exceeds entry bound");
        PlainPath(entry.path());
        if (!entry.is_regular_file()) continue;
        const auto name = entry.path().lexically_relative(addon).generic_string();
        Need(SafeRelative(name), "Unsafe loose Astra virtual path");
        const auto stamp = FileStamp(entry.path());
        inventory[name].push_back(Member{entry.path(), 0, stamp.at("bytes").get<std::uint64_t>(), std::nullopt, stamp});
    }
}

const Member& Unique(const Inventory& inventory, const std::string& path) {
    const auto found = inventory.find(path);
    Need(found != inventory.end() && !found->second.empty(), "Missing package member: " + path);
    const auto& first = found->second.front();
    for (const auto& entry : found->second) {
        Need(entry.bytes == first.bytes && (entry.source == first.source ||
             (entry.crc && first.crc && entry.crc == first.crc)), "Conflicting package member: " + path);
    }
    return first;
}

std::string ReadSmall(const Member& member, std::uint64_t maximum, Json& report) {
    Need(member.bytes <= maximum, "Metadata exceeds byte bound");
    Reader reader(member, report);
    std::string text(static_cast<std::size_t>(member.bytes), '\0');
    if (!text.empty()) reader.Read(text.data(), text.size());
    Need(FileStamp(member.source) == member.stamp, "Input changed during read");
    return text;
}

std::string Digest(const std::string& bytes) {
    Hash hash;
    hash.Update(bytes.data(), bytes.size());
    return hash.Finish();
}

void ValidateDescriptor(const Json& entry, const std::string& prefix, bool layer) {
    Need(entry.is_object(), "Invalid file descriptor");
    const auto path = entry.at("path").get<std::string>();
    const auto target = entry.at("target").get<std::string>();
    const auto sha = entry.at("sha256").get<std::string>();
    Need(SafeRelative(path) && path.rfind(prefix, 0) == 0 && SafeRelative(target), "Unsafe startup asset path");
    Need(IsHash(sha), "Invalid asset SHA256");
    const auto bytes = entry.at("bytes").get<std::int64_t>();
    Need(bytes > 0 && static_cast<std::uint64_t>(bytes) <= (layer ? kManifestBytes : kAssetBytes), "Asset size exceeds bounds");
    if (layer) Need(target == "mod.usda" && path == prefix + "mod.usda.dat", "Invalid layer target");
    else Need(target == "textures/" + sha + ".dds" && path.size() >= 8 && path.substr(path.size() - 8) == ".dds.dat",
              "DDS assets must have content-addressed output targets");
}

void ValidateLayer(const std::string& layer, const std::set<std::string>& targets) {
    Need(layer.rfind("#usda 1.0", 0) == 0, "Layer lacks USDA header");
    // Asset references must remain in this package. Generated material layers
    // contain no sublayers, external prim references, or payload composition.
    for (const auto* keyword : {"subLayers", "references", "payload"})
        Need(layer.find(keyword) == std::string::npos, "Layer composition is not allowed in startup packages");
    std::size_t at = 0;
    while ((at = layer.find('@', at)) != std::string::npos) {
        const auto end = layer.find('@', at + 1);
        Need(end != std::string::npos, "Unterminated layer asset reference");
        auto target = layer.substr(at + 1, end - at - 1);
        const bool builtinShader = target == "AperturePBR_Opacity.mdl";
        if (target.rfind("./", 0) == 0) target.erase(0, 2);
        // This is Remix's built-in shader module, resolved by the renderer.
        // Packages may reference it but cannot supply/replace an MDL file.
        Need(builtinShader || targets.count(target) == 1,
             "Layer references an undeclared asset");
        at = end + 1;
    }
}

Json ReadReceipt(const fs::path& path, Json& report) {
    if (!Exists(path)) return Json::object();
    try {
        const auto stamp = FileStamp(path);
        const Member member{path, 0, stamp.at("bytes").get<std::uint64_t>(), std::nullopt, stamp};
        const auto result = Json::parse(ReadSmall(member, kManifestBytes, report));
        return result.is_object() ? result : Json::object();
    } catch (const std::exception&) { return Json::object(); }
}

bool Cached(const fs::path& output, const Json& descriptor, const Json& old) {
    if (!Exists(output) || !old.is_object()) return false;
    const auto stamp = FileStamp(output);
    return old.value("sha256", std::string{}) == descriptor.at("sha256").get<std::string>() &&
           old.value("stamp", Json::object()) == stamp && stamp.at("bytes") == descriptor.at("bytes");
}

Json InstallAsset(const Member& source, const fs::path& output, const Json& descriptor, const Json& old, Json& report) {
    Need(source.bytes == descriptor.at("bytes").get<std::uint64_t>(), "Packaged asset size differs from manifest");
    if (Cached(output, descriptor, old)) {
        report["cache_hits"] = report.value("cache_hits", 0) + 1;
        return old;
    }
    // If a timestamp changed, hash the output once before deciding to replace it.
    if (Exists(output) && FileStamp(output).at("bytes") == descriptor.at("bytes")) {
        const auto stamp = FileStamp(output);
        Member current{output, 0, stamp.at("bytes").get<std::uint64_t>(), std::nullopt, stamp};
        Reader existing(current, report);
        Hash hash;
        std::vector<char> buffer(1024 * 1024);
        while (existing.Remaining()) {
            const auto size = static_cast<std::size_t>(std::min<std::uint64_t>(buffer.size(), existing.Remaining()));
            existing.Read(buffer.data(), size);
            hash.Update(buffer.data(), size);
        }
        if (hash.Finish() == descriptor.at("sha256").get<std::string>() && FileStamp(output) == stamp)
            return {{"sha256", descriptor.at("sha256")}, {"stamp", stamp}};
    }
    Reader reader(source, report);
    Hash hash;
    AtomicOutput destination(output);
    std::vector<char> buffer(1024 * 1024);
    bool first = true;
    while (reader.Remaining()) {
        const auto size = static_cast<std::size_t>(std::min<std::uint64_t>(buffer.size(), reader.Remaining()));
        reader.Read(buffer.data(), size);
        if (first) Need(size >= 128 && std::string(buffer.data(), 4) == "DDS ", "Packaged texture lacks DDS header");
        first = false;
        hash.Update(buffer.data(), size);
        destination.Write(buffer.data(), size);
        report["bytes_written"] = report.value("bytes_written", std::uint64_t{0}) + size;
    }
    Need(hash.Finish() == descriptor.at("sha256").get<std::string>(), "Packaged DDS SHA256 mismatch");
    Need(FileStamp(source.source) == source.stamp, "Asset source changed during preparation");
    destination.Commit();
    return {{"sha256", descriptor.at("sha256")}, {"stamp", FileStamp(output)}};
}

void Deactivate(const fs::path& modDirectory) {
    EnsureDirectory(modDirectory);
    // Never erase textures or recurse through someone else's mod. Replacing
    // this fixed owner's root also deactivates a previously valid stale layer.
    WriteText(modDirectory / "mod.usda", kEmptyLayer);
}

bool RecognizedLegacyRoot(const std::string& original, bool& empty) {
    std::string contents = original;
    contents.erase(std::remove(contents.begin(), contents.end(), '\r'), contents.end());
    if (contents == kEmptyLayer) { empty = true; return true; }
    // Exact one-time bootstrap emitted by astra/native_rtx.py EMPTY_MOD before
    // the legacy native writer has ever published a material sublayer.
    const std::string bootstrap = "#usda 1.0\n(\n    customLayerData = {\n"
        "        string lightspeed_game_name = \"Garry's Mod (x64)\"\n"
        "        string lightspeed_layer_type = \"replacement\"\n"
        "    }\n    metersPerUnit = 0.01\n    upAxis = \"Z\"\n)\n\n"
        "over \"RootNode\"\n{\n    over \"Looks\"\n    {\n    }\n}\n";
    if (contents == bootstrap) { empty = true; return true; }
    const std::string prefix = "#usda 1.0\n(\n    customLayerData = {\n"
        "        string lightspeed_game_name = \"Garry's Mod (x64)\"\n"
        "        string lightspeed_layer_type = \"replacement\"\n"
        "    }\n    metersPerUnit = 0.01\n    subLayers = [\n";
    const std::string suffix = "    ]\n    timeCodesPerSecond = 24\n    upAxis = \"Z\"\n)\n";
    if (contents.size() < prefix.size() + suffix.size() || contents.rfind(prefix, 0) != 0 ||
        contents.substr(contents.size() - suffix.size()) != suffix) return false;
    const auto entries = contents.substr(prefix.size(), contents.size() - prefix.size() - suffix.size());
    empty = entries.empty();
    if (empty) return true;
    const std::regex linePattern("        @\\./profiles/(hash_[A-F0-9]{16}(_[A-F0-9]{16})?|material_[a-zA-Z0-9_-]{1,96})\\.usda@(,?)");
    std::set<std::string> seen;
    std::size_t begin = 0;
    while (begin < entries.size()) {
        const auto end = entries.find('\n', begin);
        if (end == std::string::npos || seen.size() >= 4096) return false;
        const auto line = entries.substr(begin, end - begin);
        std::smatch match;
        if (!std::regex_match(line, match, linePattern) || !seen.insert(match[1].str()).second) return false;
        const bool final = end + 1 == entries.size();
        if (match[3].str() != (final ? "" : ",")) return false;
        begin = end + 1;
    }
    return true;
}

void MigrateLegacyRoot(const fs::path& gameRoot, Json& report) {
    report = {{"required", true}, {"ready", false}, {"changed", false},
              {"bytes_read", 0}, {"bytes_written", 0}};
    const auto root = gameRoot / "rtx-remix/mods/!astra_map_importer/mod.usda";
    PlainPath(root);
    if (!Exists(root)) { report["ready"] = true; report["reason"] = "Legacy root absent"; return; }
    const auto stamp = FileStamp(root);
    const Member member{root, 0, stamp.at("bytes").get<std::uint64_t>(), std::nullopt, stamp};
    const auto bytes = ReadSmall(member, kManifestBytes, report);
    bool empty = false;
    Need(RecognizedLegacyRoot(bytes, empty), "Legacy Astra root has an unexpected structure; refusing ownership guess");
    const auto before = Digest(bytes);
    report["before_sha256"] = before;
    if (empty) {
        report["ready"] = true;
        report["reason"] = "Legacy root already empty; mtime preserved";
        report["after_sha256"] = before;
        return;
    }
    const auto backup = gameRoot / "garrysmod/data/astra/startup/legacy_roots" / (before + ".usda.dat");
    PlainPath(backup);
    if (Exists(backup)) {
        const auto savedStamp = FileStamp(backup);
        const Member saved{backup, 0, savedStamp.at("bytes").get<std::uint64_t>(), std::nullopt, savedStamp};
        Need(Digest(ReadSmall(saved, kManifestBytes, report)) == before, "Immutable legacy root backup differs from its content address");
    } else {
        WriteText(backup, bytes);
        report["bytes_written"] = bytes.size();
        const auto savedStamp = FileStamp(backup);
        const Member saved{backup, 0, savedStamp.at("bytes").get<std::uint64_t>(), std::nullopt, savedStamp};
        Need(Digest(ReadSmall(saved, kManifestBytes, report)) == before, "Legacy root backup verification failed");
    }
    report["backup"] = backup.u8string();
    report["backup_sha256"] = before;
    Need(FileStamp(root) == stamp, "Legacy root changed before deactivation");
    Deactivate(root.parent_path());
    report["bytes_written"] = report.value("bytes_written", std::uint64_t{0}) + std::char_traits<char>::length(kEmptyLayer);
    const auto afterStamp = FileStamp(root);
    const Member after{root, 0, afterStamp.at("bytes").get<std::uint64_t>(), std::nullopt, afterStamp};
    const auto afterBytes = ReadSmall(after, kManifestBytes, report);
    Need(afterBytes == kEmptyLayer, "Legacy root did not remain empty after publication");
    report["after_sha256"] = Digest(afterBytes);
    report["changed"] = true;
    report["ready"] = true;
}

struct Package {
    std::string map;
    Json manifest;
    std::set<std::string> hashes;
    std::vector<std::string> errors;
};
} // namespace

Json Prepare(const fs::path& suppliedRoot, const std::optional<std::vector<fs::path>>& addonSources) {
    const auto began = std::chrono::steady_clock::now();
    const auto gameRoot = fs::absolute(suppliedRoot).lexically_normal();
    PlainPath(gameRoot);
    Need(fs::is_directory(gameRoot / "garrysmod"), "Game root lacks garrysmod directory");
    Json report{{"version", 1}, {"ready", true}, {"game_root", gameRoot.u8string()},
                {"bytes_read", 0}, {"maps", Json::object()}, {"errors", Json::array()}, {"warnings", Json::array()},
                {"accounting", "Reads include GMA tables, selected metadata and asset verification. Writes count texture/layer payloads, excluding receipts/status. OS read-ahead and renderer streaming are excluded. No archive payload scan or full-archive CRC is performed."}};
    Inventory inventory;
    auto sources = addonSources.value_or(std::vector<fs::path>{});
    if (!addonSources) {
        const auto addons = gameRoot / "garrysmod/addons";
        PlainPath(addons);
        if (fs::is_directory(addons)) {
            for (const auto& entry : fs::directory_iterator(addons)) {
                try {
                    PlainPath(entry.path());
                    if (entry.is_directory() || entry.path().extension() == ".gma") sources.push_back(entry.path());
                } catch (const std::exception& error) {
                    // Installed games commonly contain linked unrelated addons.
                    // Do not follow them or fail all independent Astra packages.
                    report["warnings"].push_back({{"source", entry.path().u8string()}, {"error", error.what()}});
                }
            }
        }
    }
    Need(sources.size() <= kMaxSources, "Too many addon sources");
    std::sort(sources.begin(), sources.end());
    sources.erase(std::unique(sources.begin(), sources.end()), sources.end());
    sources.insert(sources.begin(), gameRoot / "garrysmod");
    for (const auto& source : sources) {
        try {
            PlainPath(source);
            if (fs::is_directory(source)) ScanLoose(source, inventory);
            else ScanArchive(source, inventory, report);
        } catch (const std::exception& error) {
            report["errors"].push_back({{"source", source.u8string()}, {"error", error.what()}});
            report["ready"] = false;
        }
    }
    report["scan_seconds"] = Seconds(began);
    report["table_bytes_read"] = report["bytes_read"];
    std::map<std::string, Package> packages;
    const std::regex manifestPattern("data_static/astra/([a-z0-9_-]{1,96})/rtx/startup\\.json");
    for (const auto& item : inventory) {
        std::smatch match;
        if (!std::regex_match(item.first, match, manifestPattern)) continue;
        const auto name = match[1].str();
        auto& package = packages[name];
        package.map = name;
        auto& out = report["maps"][name];
        out = {{"ready", false}, {"bytes_read", 0}, {"bytes_written", 0}, {"cache_hits", 0}, {"errors", Json::array()}};
        try {
            package.manifest = Json::parse(ReadSmall(Unique(inventory, item.first), kManifestBytes, out));
            const auto& manifest = package.manifest;
            Need(manifest.at("version") == 1 && manifest.at("map") == name && IsMap(name), "Invalid startup manifest identity");
            const auto generation = manifest.at("generation").get<std::string>();
            Need(IsHash(generation), "Invalid startup generation");
            out["generation"] = generation;
            const auto& files = manifest.at("files");
            Need(files.is_array() && files.size() <= kMaxAssets, "Startup asset count exceeds bound");
            const auto prefix = "data_static/astra/" + name + "/rtx/";
            std::set<std::string> targets;
            std::uint64_t total = 0;
            for (const auto& descriptor : files) {
                ValidateDescriptor(descriptor, prefix, false);
                Need(targets.insert(descriptor.at("target").get<std::string>()).second, "Duplicate startup output target");
                const auto& member = Unique(inventory, descriptor.at("path").get<std::string>());
                Need(member.bytes == descriptor.at("bytes").get<std::uint64_t>(), "Startup asset member size mismatch");
                total += member.bytes;
                Need(total <= 128ULL * 1024 * 1024 * 1024, "Startup package exceeds 128 GiB bound");
            }
            ValidateDescriptor(manifest.at("layer"), prefix, true);
            const auto& layer = Unique(inventory, manifest.at("layer").at("path").get<std::string>());
            Need(layer.bytes == manifest.at("layer").at("bytes").get<std::uint64_t>(), "Layer member size mismatch");
            if (manifest.contains("hashes")) {
                Need(manifest.at("hashes").is_array() && manifest.at("hashes").size() <= 262144, "Invalid material hash inventory");
                for (const auto& value : manifest.at("hashes")) {
                    const auto hash = value.get<std::string>();
                    Need(std::regex_match(hash, std::regex("[A-F0-9]{16}")) && package.hashes.insert(hash).second,
                         "Invalid or duplicate material hash");
                }
            }
        } catch (const std::exception& error) { package.errors.push_back(error.what()); }
    }
    std::map<std::string, std::string> hashOwners;
    for (const auto& entry : packages) {
        for (const auto& hash : entry.second.hashes) {
            const auto found = hashOwners.emplace(hash, entry.first);
            if (!found.second && found.first->second != entry.first) {
                packages[entry.first].errors.push_back("Material hash overlaps map " + found.first->second + ": " + hash);
                packages[found.first->second].errors.push_back("Material hash overlaps map " + entry.first + ": " + hash);
            }
        }
    }
    const auto modsRoot = gameRoot / "rtx-remix/mods";
    EnsureDirectory(modsRoot);
    if (!packages.empty()) {
        try { MigrateLegacyRoot(gameRoot, report["legacy_migration"]); }
        catch (const std::exception& error) {
            report["legacy_migration"]["error"] = error.what();
            report["ready"] = false;
            for (auto& entry : packages) entry.second.errors.push_back(std::string("Legacy root migration failed: ") + error.what());
        }
    } else report["legacy_migration"] = {{"required", false}, {"ready", true}};
    for (auto& item : packages) {
        const auto mapBegan = std::chrono::steady_clock::now();
        auto& package = item.second;
        auto& out = report["maps"][item.first];
        const auto destination = modsRoot / (std::string(kOwnedPrefix) + item.first);
        try {
            Need(package.errors.empty(), package.errors.empty() ? "" : package.errors.front());
            const auto& manifest = package.manifest;
            const auto& descriptor = manifest.at("layer");
            const auto layer = ReadSmall(Unique(inventory, descriptor.at("path").get<std::string>()), kManifestBytes, out);
            Need(Digest(layer) == descriptor.at("sha256").get<std::string>(), "Packaged layer SHA256 mismatch");
            std::set<std::string> targets;
            for (const auto& file : manifest.at("files")) targets.insert(file.at("target").get<std::string>());
            ValidateLayer(layer, targets);
            EnsureDirectory(destination);
            const auto receipt = ReadReceipt(destination / "receipt.json", out);
            Json next{{"version", 1}, {"generation", manifest.at("generation")}, {"files", Json::object()}};
            const auto oldFiles = receipt.value("files", Json::object());
            for (const auto& file : manifest.at("files")) {
                const auto target = file.at("target").get<std::string>();
                next["files"][target] = InstallAsset(Unique(inventory, file.at("path").get<std::string>()),
                    destination / target, file, oldFiles.value(target, Json::object()), out);
            }
            // The visible material root is committed only after every dependency
            // validates. Unchanged roots keep their mtime to avoid reload storms.
            if (!Cached(destination / "mod.usda", descriptor, receipt.value("layer", Json::object()))) {
                WriteText(destination / "mod.usda", layer);
                out["bytes_written"] = out.value("bytes_written", std::uint64_t{0}) + layer.size();
            } else out["cache_hits"] = out.value("cache_hits", 0) + 1;
            next["layer"] = {{"sha256", descriptor.at("sha256")}, {"stamp", FileStamp(destination / "mod.usda")}};
            WriteText(destination / "receipt.json", next.dump());
            out["ready"] = true;
            out["assets"] = manifest.at("files").size();
            out["mod_directory"] = destination.u8string();
        } catch (const std::exception& error) {
            out["errors"] = package.errors;
            if (package.errors.empty() || package.errors.front() != error.what()) out["errors"].push_back(error.what());
            try { Deactivate(destination); out["deactivated"] = true; }
            catch (const std::exception& cleanupError) { out["errors"].push_back(std::string("Failed to deactivate own root: ") + cleanupError.what()); }
            report["ready"] = false;
        }
        out["seconds"] = Seconds(mapBegan);
    }
    for (const auto& entry : fs::directory_iterator(modsRoot)) {
        const auto name = entry.path().filename().u8string();
        if (name.rfind(kOwnedPrefix, 0) != 0) continue;
        const auto map = name.substr(std::char_traits<char>::length(kOwnedPrefix));
        if (!IsMap(map) || packages.count(map)) continue;
        auto& out = report["maps"][map];
        out = {{"ready", false}, {"removed", true}, {"errors", Json::array()}};
        try { Deactivate(entry.path()); out["deactivated"] = true; }
        catch (const std::exception& error) { out["errors"].push_back(error.what()); report["ready"] = false; }
    }
    report["seconds"] = Seconds(began);
    report["bytes_written"] = 0;
    for (const auto& map : report["maps"].items()) {
        report["bytes_read"] = report.value("bytes_read", std::uint64_t{0}) + map.value().value("bytes_read", std::uint64_t{0});
        report["bytes_written"] = report.value("bytes_written", std::uint64_t{0}) + map.value().value("bytes_written", std::uint64_t{0});
    }
    report["bytes_read"] = report.value("bytes_read", std::uint64_t{0}) + report["legacy_migration"].value("bytes_read", std::uint64_t{0});
    report["bytes_written"] = report.value("bytes_written", std::uint64_t{0}) + report["legacy_migration"].value("bytes_written", std::uint64_t{0});
    WriteText(gameRoot / "garrysmod/data/astra/startup/status.json", report.dump(2));
    return report;
}

bool DeactivateMap(const fs::path& suppliedRoot, const std::string& map, const std::string& generation) {
    try {
        Need(IsMap(map) && IsHash(generation), "Invalid runtime deactivation identity");
        const auto gameRoot = fs::absolute(suppliedRoot).lexically_normal();
        PlainPath(gameRoot);
        const auto destination = gameRoot / "rtx-remix/mods" / (std::string(kOwnedPrefix) + map);
        Json accounting{{"bytes_read", 0}};
        const auto receipt = ReadReceipt(destination / "receipt.json", accounting);
        Need(receipt.value("generation", std::string{}) == generation, "Runtime deactivation generation is stale");
        Deactivate(destination);
        return true;
    } catch (const std::exception&) { return false; }
}
} // namespace astra::startup_assets
