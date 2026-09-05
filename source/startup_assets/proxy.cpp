// Run the package preparation before Remix's Direct3D interface constructs its
// device/mod manager. Never perform filesystem work or LoadLibrary in DllMain.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d9.h>
#include <shellapi.h>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>
#include "startup_assets.h"

namespace {
std::once_flag g_startOnce;
std::mutex g_statusMutex;
nlohmann::json g_status = {{"version", 1}, {"phase", "pending"}, {"maps", nlohmann::json::object()}};
std::filesystem::path g_gameRoot;
HMODULE g_renderer = nullptr;

std::optional<std::vector<std::filesystem::path>> SelectSources(std::string& selection) {
    constexpr const wchar_t* variable = L"ASTRA_RTX_STARTUP_SOURCES";
    const DWORD needed = GetEnvironmentVariableW(variable, nullptr, 0);
    if (needed) {
        if (needed > 65536) throw std::runtime_error("Startup sources environment exceeds 64K characters");
        std::wstring wide(needed, L'\0');
        const DWORD length = GetEnvironmentVariableW(variable, wide.data(), needed);
        if (!length || length >= needed) throw std::runtime_error("Startup sources environment changed during read");
        wide.resize(length);
        const int utf8Size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(), static_cast<int>(wide.size()),
                                               nullptr, 0, nullptr, nullptr);
        if (!utf8Size) throw std::runtime_error("Invalid startup sources Unicode");
        std::string utf8(static_cast<size_t>(utf8Size), '\0');
        if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(), static_cast<int>(wide.size()),
                                utf8.data(), utf8Size, nullptr, nullptr)) throw std::runtime_error("Invalid startup sources Unicode");
        const auto json = nlohmann::json::parse(utf8);
        if (!json.is_array() || json.size() > 4096) throw std::runtime_error("Startup sources must be a bounded JSON array");
        std::vector<std::filesystem::path> paths;
        for (const auto& value : json) {
            if (!value.is_string()) throw std::runtime_error("Startup source entries must be strings");
            auto path = std::filesystem::u8path(value.get<std::string>());
            if (!path.is_absolute()) throw std::runtime_error("Startup sources must be absolute paths");
            paths.push_back(std::move(path));
        }
        selection = "explicit_environment";
        return paths;
    }
    int count = 0;
    wchar_t** arguments = CommandLineToArgvW(GetCommandLineW(), &count);
    if (!arguments) throw std::runtime_error("Cannot parse startup command line");
    bool noAddons = false;
    for (int index = 1; index < count; ++index) {
        if (_wcsicmp(arguments[index], L"-noaddons") == 0) noAddons = true;
    }
    LocalFree(arguments);
    if (noAddons) {
        selection = "noaddons";
        return std::vector<std::filesystem::path>{};
    }
    selection = "default_discovery";
    return std::nullopt;
}

std::filesystem::path ModulePath() {
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&ModulePath), &module)) {
        throw std::runtime_error("Cannot locate the startup proxy module");
    }
    std::vector<wchar_t> path(32768);
    const DWORD length = GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
    if (!length || length >= path.size()) throw std::runtime_error("Cannot resolve the startup proxy path");
    return std::filesystem::path(std::wstring(path.data(), length));
}

void Initialize() noexcept {
    try {
        std::call_once(g_startOnce, [] {
            try {
                const auto modulePath = ModulePath();
                g_gameRoot = modulePath.parent_path().parent_path().parent_path();
                if (modulePath.parent_path().filename() != L"win64" ||
                    modulePath.parent_path().parent_path().filename() != L"bin") {
                    throw std::runtime_error("The startup proxy must be installed in bin/win64");
                }
                nlohmann::json result;
                try {
                    std::string selection;
                    const auto sources = SelectSources(selection);
                    result = astra::startup_assets::Prepare(g_gameRoot, sources);
                    result["phase"] = "prepared";
                    result["source_selection"] = selection;
                } catch (const std::exception& error) {
                    // Remove stale owned packages when an explicit launch
                    // contract is invalid; never reuse another session's set.
                    try { astra::startup_assets::Prepare(g_gameRoot, std::vector<std::filesystem::path>{}); }
                    catch (...) {}
                    result = {{"version", 1}, {"phase", "failed"}, {"error", error.what()},
                              {"maps", nlohmann::json::object()}};
                }
                {
                    std::lock_guard<std::mutex> lock(g_statusMutex);
                    g_status = std::move(result);
                }
                const auto rendererPath = modulePath.parent_path() / L"d3d9_astra_renderer.dll";
                g_renderer = LoadLibraryExW(rendererPath.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
                if (!g_renderer) throw std::runtime_error("Cannot load d3d9_astra_renderer.dll");
            } catch (const std::exception& error) {
                std::lock_guard<std::mutex> lock(g_statusMutex);
                g_status["phase"] = "failed";
                g_status["error"] = error.what();
            }
        });
    } catch (...) {
        // Preserve the ABI even on allocation/OS failures. The normal D3D9
        // unavailable return below lets the engine handle initialization failure.
    }
}

bool Identifier(const std::string& value, bool hexadecimal) {
    if (value.empty() || value.size() > 96) return false;
    for (const unsigned char c : value) {
        if (hexadecimal ? !((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))
                        : !((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || c == '_' || c == '-')) return false;
    }
    return true;
}

} // namespace

extern "C" IDirect3D9* WINAPI Direct3DCreate9(UINT sdkVersion) {
    Initialize();
    using Create = IDirect3D9* (WINAPI*)(UINT);
    const auto create = g_renderer ? reinterpret_cast<Create>(GetProcAddress(g_renderer, "Direct3DCreate9")) : nullptr;
    return create ? create(sdkVersion) : nullptr;
}

extern "C" HRESULT WINAPI Direct3DCreate9Ex(UINT sdkVersion, IDirect3D9Ex** result) {
    Initialize();
    using Create = HRESULT (WINAPI*)(UINT, IDirect3D9Ex**);
    const auto create = g_renderer ? reinterpret_cast<Create>(GetProcAddress(g_renderer, "Direct3DCreate9Ex")) : nullptr;
    if (create) return create(sdkVersion, result);
    if (result) *result = nullptr;
    return D3DERR_NOTAVAILABLE;
}

extern "C" const char* __cdecl AstraStartupStatusJson() noexcept {
    thread_local std::string snapshot;
    try {
        std::lock_guard<std::mutex> lock(g_statusMutex);
        snapshot = g_status.dump();
        return snapshot.c_str();
    } catch (...) {
        return "{\"version\":1,\"phase\":\"failed\",\"maps\":{},\"error\":\"Status allocation failed\"}";
    }
}

extern "C" bool __cdecl AstraDisableStartupMap(const char* mapName, const char* generation) noexcept {
    try {
        if (!mapName || !generation) return false;
        const std::string map(mapName), expectedGeneration(generation);
        if (!Identifier(map, false) || !Identifier(expectedGeneration, true)) return false;
        std::lock_guard<std::mutex> lock(g_statusMutex);
        if (!g_status.contains("maps") || !g_status["maps"].contains(map)) return false;
        auto& entry = g_status["maps"][map];
        if (entry.value("generation", std::string()) != expectedGeneration) return false;
        if (entry.value("disabled", false)) return true;
        if (!astra::startup_assets::DeactivateMap(g_gameRoot, map, expectedGeneration)) return false;
        entry["ready"] = false;
        entry["disabled"] = true;
        entry["error"] = "Map activation failed; the owned replacement root was disabled";
        return true;
    } catch (...) {
        return false;
    }
}
