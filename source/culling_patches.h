#pragma once
#ifdef _WIN64

#include "GarrysMod/Lua/Interface.h"

// Register culling patch definitions, create ConVars, resolve signatures, and apply initial patches.
void InitCullingPatches();

// Sync patch states to current ConVar values (call periodically from Lua think hook).
void SyncCullingPatches();

// Register Lua-callable functions for patch management.
void RegisterCullingPatchLuaFunctions(GarrysMod::Lua::ILuaBase* LUA);

#endif // _WIN64
