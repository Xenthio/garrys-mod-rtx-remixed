#pragma once

#include <filesystem>

namespace GarrysMod::Lua { class ILuaBase; }

namespace advmat::rtx_bridge {

// Registers the global AdvMatRTXBridge table. The game root must be the
// directory containing hl2.exe and rtx-remix. Call this on the client realm.
bool RegisterLua(GarrysMod::Lua::ILuaBase* lua, const std::filesystem::path& gameRoot);
void UnregisterLua(GarrysMod::Lua::ILuaBase* lua);

} // namespace advmat::rtx_bridge


