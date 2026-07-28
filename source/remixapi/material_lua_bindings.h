#pragma once

#ifdef _WIN64

#include <cstdint>

// Thread-safe shared registry for rtx.legacyEmissiveForceAlbedoString.
// Both Lua-driven scans and C++ AutoCategorisation must use this registry so
// neither path overwrites hashes registered by the other.
bool AddForceAlbedoHashCpp(uint64_t hash);
bool RemoveForceAlbedoHashCpp(uint64_t hash);
void ClearForceAlbedoHashesCpp();

#endif // _WIN64
