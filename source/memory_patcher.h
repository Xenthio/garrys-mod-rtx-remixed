#pragma once

#include <Windows.h>
#include <vector>
#include <string>
#include <mutex>
#include <memory>
#include <unordered_map>

// Structure to store information about a patch
struct PatchInfo {
    void* address;
    std::vector<uint8_t> originalBytes;
    std::vector<uint8_t> patchedBytes;
    std::string description;
    bool isEnabled;
};

class MemoryPatcher {
public:
    // Singleton instance
    static MemoryPatcher& GetInstance();

    // Delete copy and move constructors/operators
    MemoryPatcher(const MemoryPatcher&) = delete;
    MemoryPatcher& operator=(const MemoryPatcher&) = delete;
    MemoryPatcher(MemoryPatcher&&) = delete;
    MemoryPatcher& operator=(MemoryPatcher&&) = delete;

    // Find a pattern in a module's memory
    void* FindPattern(HMODULE module, const char* pattern, size_t patternLength);
    void* FindPattern(HMODULE module, const std::string& pattern);

    // Find pattern with wildcards (e.g., "48 89 ? ? 48 89")
    void* FindPatternWildcard(HMODULE module, const char* pattern);

    // Convert a hex string to bytes (e.g., "48 89 5C 24 08" -> byte array)
    std::vector<uint8_t> HexStringToBytes(const std::string& hexString);

    // Create and apply a patch
    bool CreatePatch(const std::string& patchName, void* address, const std::vector<uint8_t>& newBytes, const std::string& description = "");
    bool CreatePatch(const std::string& patchName, void* address, const std::string& hexBytes, const std::string& description = "");

    // Find and patch in one operation
    bool FindAndPatch(const std::string& patchName, HMODULE module, const std::string& pattern, const std::string& hexBytes, const std::string& description = "");

    // Enable/disable a specific patch
    bool EnablePatch(const std::string& patchName);
    bool DisablePatch(const std::string& patchName);

    // Enable/disable all patches
    void EnableAllPatches();
    void DisableAllPatches();

    // Check if a patch exists and is enabled
    bool IsPatchEnabled(const std::string& patchName);
    bool DoesPatchExist(const std::string& patchName);

    // Get information about a patch
    PatchInfo GetPatchInfo(const std::string& patchName);

    // Log all patches
    void LogAllPatches();

    // Clean up
    void Shutdown();

    // Verbose logging toggle. Off by default; callers opt in (e.g. when
    // r_hwskin_debug is set) to avoid spam on hot-path Enable/Disable calls.
    void SetVerbose(bool verbose) { m_verbose = verbose; }
    bool IsVerbose() const { return m_verbose; }

private:
    MemoryPatcher() = default;
    ~MemoryPatcher();

    // Internal memory access functions
    bool ProtectMemory(void* address, size_t size, DWORD newProtection, DWORD* oldProtection);
    bool WriteToMemory(void* address, const void* data, size_t size);

    // Map of patch name to patch info
    std::unordered_map<std::string, PatchInfo> m_patches;

    // Mutex for thread safety
    std::mutex m_mutex;

    bool m_verbose = false;
};

// Convenience functions for global access
#define g_MemoryPatcher MemoryPatcher::GetInstance()

// Utility function to get a module handle by name
HMODULE GetModuleHandleEx(const char* moduleName);
