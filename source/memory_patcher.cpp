#include "memory_patcher.h"
#include <Psapi.h>
#include <iostream>
#include <iomanip>
#include <sstream>
#include <cctype>
#include <GarrysMod/Lua/LuaInterface.h>

// Get singleton instance
MemoryPatcher& MemoryPatcher::GetInstance() {
    static MemoryPatcher instance;
    return instance;
}

// Destructor - ensure all patches are disabled
MemoryPatcher::~MemoryPatcher() {
    try {
        Shutdown();
    }
    catch (...) {
        Msg("[MemoryPatcher] Exception during shutdown\n");
    }
}

// Convert a hex string (like "48 89 5C 24 08") to a byte vector
std::vector<uint8_t> MemoryPatcher::HexStringToBytes(const std::string& hexString) {
    try {
        std::vector<uint8_t> bytes;
        std::string hex = hexString;

        // Remove spaces
        hex.erase(std::remove_if(hex.begin(), hex.end(), ::isspace), hex.end());

        // Convert each pair of hex digits to a byte
        for (size_t i = 0; i < hex.length(); i += 2) {
            if (i + 1 >= hex.length()) break;

            std::string byteString = hex.substr(i, 2);
            if (byteString == "??") {
                // Wildcard byte
                bytes.push_back(0);
            }
            else {
                // Normal byte
                uint8_t byte = (uint8_t)strtol(byteString.c_str(), nullptr, 16);
                bytes.push_back(byte);
            }
        }

        return bytes;
    }
    catch (const std::exception& e) {
        Msg("[MemoryPatcher] Exception in HexStringToBytes: %s\n", e.what());
        return std::vector<uint8_t>();
    }
}

// Find a pattern in a module's memory
void* MemoryPatcher::FindPattern(HMODULE module, const char* pattern, size_t patternLength) {
    try {
        if (!module) {
            Msg("[MemoryPatcher] FindPattern: Module handle is NULL\n");
            return nullptr;
        }

        MODULEINFO moduleInfo;
        if (!GetModuleInformation(GetCurrentProcess(), module, &moduleInfo, sizeof(moduleInfo))) {
            Msg("[MemoryPatcher] Failed to get module information (Error: %d)\n", GetLastError());
            return nullptr;
        }

        uintptr_t baseAddress = (uintptr_t)module;
        uintptr_t endAddress = baseAddress + moduleInfo.SizeOfImage;

        Msg("[MemoryPatcher] Searching for pattern in module 0x%p (size: %u bytes)\n",
            module, moduleInfo.SizeOfImage);

        for (uintptr_t currentAddress = baseAddress; currentAddress < endAddress - patternLength; currentAddress++) {
            bool found = true;

            for (size_t i = 0; i < patternLength; i++) {
                if (*(unsigned char*)(currentAddress + i) != (unsigned char)pattern[i]) {
                    found = false;
                    break;
                }
            }

            if (found) {
                Msg("[MemoryPatcher] Pattern found at address 0x%p\n", (void*)currentAddress);
                return (void*)currentAddress;
            }
        }

        Msg("[MemoryPatcher] Pattern not found in module\n");
        return nullptr;
    }
    catch (const std::exception& e) {
        Msg("[MemoryPatcher] Exception in FindPattern: %s\n", e.what());
        return nullptr;
    }
}

// Find a pattern from a string
void* MemoryPatcher::FindPattern(HMODULE module, const std::string& pattern) {
    return FindPattern(module, pattern.c_str(), pattern.length());
}

// Find a pattern with wildcards
void* MemoryPatcher::FindPatternWildcard(HMODULE module, const char* pattern) {
    try {
        if (!module) {
            Msg("[MemoryPatcher] FindPatternWildcard: Module handle is NULL\n");
            return nullptr;
        }

        MODULEINFO moduleInfo;
        if (!GetModuleInformation(GetCurrentProcess(), module, &moduleInfo, sizeof(moduleInfo))) {
            Msg("[MemoryPatcher] Failed to get module information (Error: %d)\n", GetLastError());
            return nullptr;
        }

        // Convert pattern to bytes and wildcards
        std::vector<std::pair<uint8_t, bool>> patternBytes;
        std::string patternStr(pattern);
        std::string token;
        std::istringstream tokenStream(patternStr);

        while (std::getline(tokenStream, token, ' ')) {
            if (token == "?" || token == "??") {
                patternBytes.push_back(std::make_pair(0, true)); // Wildcard
            }
            else {
                uint8_t byte;
                try {
                    byte = (uint8_t)strtol(token.c_str(), nullptr, 16);
                }
                catch (...) {
                    Msg("[MemoryPatcher] Invalid hex byte in pattern: %s\n", token.c_str());
                    continue;
                }
                patternBytes.push_back(std::make_pair(byte, false)); // Normal byte
            }
        }

        if (patternBytes.empty()) {
            Msg("[MemoryPatcher] Empty pattern or pattern parsing failed\n");
            return nullptr;
        }

        uintptr_t baseAddress = (uintptr_t)module;
        uintptr_t endAddress = baseAddress + moduleInfo.SizeOfImage - patternBytes.size();

        Msg("[MemoryPatcher] Searching for wildcarded pattern (%zu bytes) in module 0x%p\n",
            patternBytes.size(), module);

        for (uintptr_t currentAddress = baseAddress; currentAddress < endAddress; currentAddress++) {
            bool found = true;

            for (size_t i = 0; i < patternBytes.size(); i++) {
                if (!patternBytes[i].second) { // Not a wildcard
                    if (*(uint8_t*)(currentAddress + i) != patternBytes[i].first) {
                        found = false;
                        break;
                    }
                }
            }

            if (found) {
                Msg("[MemoryPatcher] Pattern found at address 0x%p\n", (void*)currentAddress);
                return (void*)currentAddress;
            }
        }

        Msg("[MemoryPatcher] Wildcarded pattern not found in module\n");
        return nullptr;
    }
    catch (const std::exception& e) {
        Msg("[MemoryPatcher] Exception in FindPatternWildcard: %s\n", e.what());
        return nullptr;
    }
}

// Create and apply a patch with raw bytes
bool MemoryPatcher::CreatePatch(const std::string& patchName, void* address,
    const std::vector<uint8_t>& newBytes,
    const std::string& description) {
    try {
        if (address == nullptr) {
            Msg("[MemoryPatcher] CreatePatch failed: NULL address for patch '%s'\n", patchName.c_str());
            return false;
        }

        if (newBytes.empty()) {
            Msg("[MemoryPatcher] CreatePatch failed: Empty patch bytes for patch '%s'\n", patchName.c_str());
            return false;
        }

        {
            std::lock_guard<std::mutex> lock(m_mutex);

            // Check if patch already exists
            if (m_patches.find(patchName) != m_patches.end()) {
                Msg("[MemoryPatcher] Patch '%s' already exists\n", patchName.c_str());
                return false;
            }

            PatchInfo patchInfo;
            patchInfo.address = address;
            patchInfo.patchedBytes = newBytes;
            patchInfo.description = description;
            patchInfo.isEnabled = false;

            // Read the original bytes
            patchInfo.originalBytes.resize(newBytes.size());
            SIZE_T bytesRead;
            if (!ReadProcessMemory(GetCurrentProcess(), address, patchInfo.originalBytes.data(),
                patchInfo.originalBytes.size(), &bytesRead) ||
                bytesRead != patchInfo.originalBytes.size()) {
                Msg("[MemoryPatcher] Failed to read original bytes for patch '%s' (Error: %d)\n",
                    patchName.c_str(), GetLastError());
                return false;
            }

            // Debug info - show original bytes
            std::stringstream ss;
            ss << "Original bytes: ";
            for (auto b : patchInfo.originalBytes) {
                ss << std::setw(2) << std::setfill('0') << std::hex << (int)b << " ";
            }
            Msg("[MemoryPatcher] %s\n", ss.str().c_str());

            // Store the patch
            m_patches[patchName] = patchInfo;
        }

        // Apply the patch - OUTSIDE the mutex lock to prevent deadlock
        bool result = EnablePatch(patchName);
        if (result) {
            Msg("[MemoryPatcher] Successfully created patch '%s'\n", patchName.c_str());
        }

        return result;
    }
    catch (const std::exception& e) {
        Msg("[MemoryPatcher] Exception in CreatePatch('%s'): %s\n", patchName.c_str(), e.what());
        return false;
    }
}

// Create and apply a patch with hex string
bool MemoryPatcher::CreatePatch(const std::string& patchName, void* address,
    const std::string& hexBytes,
    const std::string& description) {
    try {
        auto bytes = HexStringToBytes(hexBytes);
        if (bytes.empty()) {
            Msg("[MemoryPatcher] CreatePatch failed: Invalid hex string for patch '%s'\n", patchName.c_str());
            return false;
        }

        return CreatePatch(patchName, address, bytes, description);
    }
    catch (const std::exception& e) {
        Msg("[MemoryPatcher] Exception in CreatePatch (hex string version): %s\n", e.what());
        return false;
    }
}

// Find a pattern and patch it in one operation
bool MemoryPatcher::FindAndPatch(const std::string& patchName, HMODULE module,
    const std::string& pattern,
    const std::string& hexBytes,
    const std::string& description) {
    try {
        void* address = FindPatternWildcard(module, pattern.c_str());
        if (!address) {
            Msg("[MemoryPatcher] FindAndPatch failed: Pattern not found for patch '%s'\n", patchName.c_str());
            return false;
        }

        return CreatePatch(patchName, address, hexBytes, description);
    }
    catch (const std::exception& e) {
        Msg("[MemoryPatcher] Exception in FindAndPatch('%s'): %s\n", patchName.c_str(), e.what());
        return false;
    }
}

// Enable a patch
// Enable a patch
bool MemoryPatcher::EnablePatch(const std::string& patchName) {
    try {
        PatchInfo patchInfoCopy;
        bool alreadyEnabled = false;

        // First, get a copy of the patch info under the lock
        {
            std::lock_guard<std::mutex> lock(m_mutex);

            auto it = m_patches.find(patchName);
            if (it == m_patches.end()) {
                Msg("[MemoryPatcher] EnablePatch failed: Patch '%s' not found\n", patchName.c_str());
                return false;
            }

            if (it->second.isEnabled) {
                // Already enabled
                Msg("[MemoryPatcher] Patch '%s' is already enabled\n", patchName.c_str());
                return true;
            }

            // Make a copy of the patch info to work with outside the lock
            patchInfoCopy = it->second;
        }

        // Now do the actual patching outside the lock
        DWORD oldProtect;
        if (!ProtectMemory(patchInfoCopy.address, patchInfoCopy.patchedBytes.size(),
            PAGE_EXECUTE_READWRITE, &oldProtect)) {
            Msg("[MemoryPatcher] Failed to change memory protection for patch '%s' (Error: %d)\n",
                patchName.c_str(), GetLastError());
            return false;
        }

        if (!WriteToMemory(patchInfoCopy.address, patchInfoCopy.patchedBytes.data(),
            patchInfoCopy.patchedBytes.size())) {
            // Restore protection
            DWORD temp;
            ProtectMemory(patchInfoCopy.address, patchInfoCopy.patchedBytes.size(), oldProtect, &temp);
            Msg("[MemoryPatcher] Failed to write patched bytes for patch '%s' (Error: %d)\n",
                patchName.c_str(), GetLastError());
            return false;
        }

        // Restore protection
        DWORD temp;
        ProtectMemory(patchInfoCopy.address, patchInfoCopy.patchedBytes.size(), oldProtect, &temp);

        // Flush instruction cache
        FlushInstructionCache(GetCurrentProcess(), patchInfoCopy.address, patchInfoCopy.patchedBytes.size());

        // Now mark as enabled under the lock
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            auto it = m_patches.find(patchName);
            if (it != m_patches.end()) {
                it->second.isEnabled = true;
            }
        }

        if (m_verbose) {
            std::stringstream ss;
            ss << "Enabled patch '" << patchName << "' at 0x" << std::hex << patchInfoCopy.address
                << ": changed ";
            for (auto b : patchInfoCopy.originalBytes)
                ss << std::setw(2) << std::setfill('0') << std::hex << (int)b << " ";
            ss << "to ";
            for (auto b : patchInfoCopy.patchedBytes)
                ss << std::setw(2) << std::setfill('0') << std::hex << (int)b << " ";
            Msg("[MemoryPatcher] %s\n", ss.str().c_str());
        }

        return true;
    }
    catch (const std::exception& e) {
        Msg("[MemoryPatcher] Exception in EnablePatch('%s'): %s\n", patchName.c_str(), e.what());
        return false;
    }
}

// Make similar changes to DisablePatch
bool MemoryPatcher::DisablePatch(const std::string& patchName) {
    try {
        PatchInfo patchInfoCopy;

        // First, get a copy of the patch info under the lock
        {
            std::lock_guard<std::mutex> lock(m_mutex);

            auto it = m_patches.find(patchName);
            if (it == m_patches.end()) {
                Msg("[MemoryPatcher] DisablePatch failed: Patch '%s' not found\n", patchName.c_str());
                return false;
            }

            if (!it->second.isEnabled) {
                // Already disabled
                Msg("[MemoryPatcher] Patch '%s' is already disabled\n", patchName.c_str());
                return true;
            }

            // Make a copy of the patch info to work with outside the lock
            patchInfoCopy = it->second;
        }

        // Now do the actual unpatching outside the lock
        DWORD oldProtect;
        if (!ProtectMemory(patchInfoCopy.address, patchInfoCopy.originalBytes.size(),
            PAGE_EXECUTE_READWRITE, &oldProtect)) {
            Msg("[MemoryPatcher] Failed to change memory protection for patch '%s' (Error: %d)\n",
                patchName.c_str(), GetLastError());
            return false;
        }

        if (!WriteToMemory(patchInfoCopy.address, patchInfoCopy.originalBytes.data(),
            patchInfoCopy.originalBytes.size())) {
            // Restore protection
            DWORD temp;
            ProtectMemory(patchInfoCopy.address, patchInfoCopy.originalBytes.size(), oldProtect, &temp);
            Msg("[MemoryPatcher] Failed to restore original bytes for patch '%s' (Error: %d)\n",
                patchName.c_str(), GetLastError());
            return false;
        }

        // Restore protection
        DWORD temp;
        ProtectMemory(patchInfoCopy.address, patchInfoCopy.originalBytes.size(), oldProtect, &temp);

        // Flush instruction cache
        FlushInstructionCache(GetCurrentProcess(), patchInfoCopy.address, patchInfoCopy.originalBytes.size());

        // Now mark as disabled under the lock
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            auto it = m_patches.find(patchName);
            if (it != m_patches.end()) {
                it->second.isEnabled = false;
            }
        }

        if (m_verbose)
            Msg("[MemoryPatcher] Disabled patch '%s'\n", patchName.c_str());

        return true;
    }
    catch (const std::exception& e) {
        Msg("[MemoryPatcher] Exception in DisablePatch('%s'): %s\n", patchName.c_str(), e.what());
        return false;
    }
}

// Enable all patches
void MemoryPatcher::EnableAllPatches() {
    std::lock_guard<std::mutex> lock(m_mutex);

    int successCount = 0;
    int failCount = 0;

    for (auto& pair : m_patches) {
        if (!pair.second.isEnabled) {
            if (EnablePatch(pair.first)) {
                successCount++;
            }
            else {
                failCount++;
            }
        }
    }

    Msg("[MemoryPatcher] Enabled %d patches (%d failed)\n", successCount, failCount);
}

// Disable all patches
void MemoryPatcher::DisableAllPatches() {
    std::lock_guard<std::mutex> lock(m_mutex);

    int successCount = 0;
    int failCount = 0;

    for (auto& pair : m_patches) {
        if (pair.second.isEnabled) {
            if (DisablePatch(pair.first)) {
                successCount++;
            }
            else {
                failCount++;
            }
        }
    }

    Msg("[MemoryPatcher] Disabled %d patches (%d failed)\n", successCount, failCount);
}

// Check if a patch is enabled
bool MemoryPatcher::IsPatchEnabled(const std::string& patchName) {
    std::lock_guard<std::mutex> lock(m_mutex);

    auto it = m_patches.find(patchName);
    if (it == m_patches.end()) {
        return false;
    }

    return it->second.isEnabled;
}

// Check if a patch exists
bool MemoryPatcher::DoesPatchExist(const std::string& patchName) {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_patches.find(patchName) != m_patches.end();
}

// Get patch info
PatchInfo MemoryPatcher::GetPatchInfo(const std::string& patchName) {
    std::lock_guard<std::mutex> lock(m_mutex);

    auto it = m_patches.find(patchName);
    if (it == m_patches.end()) {
        // Return empty patch info
        Msg("[MemoryPatcher] GetPatchInfo: Patch '%s' not found\n", patchName.c_str());
        return PatchInfo{};
    }

    return it->second;
}

// Log all patches
void MemoryPatcher::LogAllPatches() {
    std::lock_guard<std::mutex> lock(m_mutex);

    Msg("[MemoryPatcher] --- Memory Patches (%zu total) ---\n", m_patches.size());
    for (const auto& pair : m_patches) {
        Msg("[MemoryPatcher] Patch '%s' at 0x%p: %s [%s]\n",
            pair.first.c_str(),
            pair.second.address,
            pair.second.description.c_str(),
            pair.second.isEnabled ? "ENABLED" : "DISABLED");
    }
    Msg("[MemoryPatcher] -----------------------------\n");
}

// Clean up
void MemoryPatcher::Shutdown() {
    Msg("[MemoryPatcher] Shutting down - disabling all patches\n");
    DisableAllPatches();
    m_patches.clear();
}

// Internal functions for memory access
bool MemoryPatcher::ProtectMemory(void* address, size_t size,
    DWORD newProtection, DWORD* oldProtection) {
    return VirtualProtect(address, size, newProtection, oldProtection) != 0;
}

bool MemoryPatcher::WriteToMemory(void* address, const void* data, size_t size) {
    SIZE_T bytesWritten;
    if (!WriteProcessMemory(GetCurrentProcess(), address, data, size, &bytesWritten)) {
        return false;
    }
    return bytesWritten == size;
}

// Utility function to get a module handle
HMODULE GetModuleHandleEx(const char* moduleName) {
    HMODULE module = GetModuleHandleA(moduleName);
    if (!module) {
        Msg("[MemoryPatcher] GetModuleHandleEx: Cannot find module '%s'\n", moduleName);
    }
    else {
        Msg("[MemoryPatcher] GetModuleHandleEx: Found module '%s' at 0x%p\n", moduleName, module);

        // Get module file path for validation
        char filePath[MAX_PATH];
        if (GetModuleFileNameA(module, filePath, MAX_PATH)) {
            Msg("[MemoryPatcher] Module path: %s\n", filePath);
        }
    }
    return module;
}