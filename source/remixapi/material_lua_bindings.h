#pragma once

#ifdef _WIN64

#include <cstdint>

// Remove a single hash from the Lua-managed force-albedo set
// (rtx.legacyEmissiveForceAlbedoString) without going through Lua.
// Called by AutoCategorisation::UnapplyFromRemix to clean up stale state
// when a material's Remix hash changes.
void RemoveLuaForceAlbedoHashCpp(uint64_t hash);

#endif // _WIN64
