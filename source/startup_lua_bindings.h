#pragma once

namespace GarrysMod { namespace Lua { class ILuaBase; } }

namespace AstraStartup {

// Startup status and scoped deactivation only. This does not register a material
// writer, read assets from Lua, or load another renderer DLL.
void RegisterLua(GarrysMod::Lua::ILuaBase* lua);

}
