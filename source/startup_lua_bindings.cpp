#ifdef _WIN64
#include "startup_lua_bindings.h"
#include <GarrysMod/Lua/Interface.h>
#include <Windows.h>
#include <cstring>
#include <initializer_list>

namespace AstraStartup {
namespace {

using GarrysMod::Lua::ILuaBase;
namespace Type = GarrysMod::Lua::Type;
using StatusFunction = const char* (__cdecl*)();
using DisableFunction = bool (__cdecl*)(const char*, const char*);

template <typename Function>
Function StartupExport(const char* name) {
    // The proxy has already run before device creation. Looking up its loaded
    // module neither starts extraction from Lua nor loads a second renderer.
    const auto module = GetModuleHandleW(L"d3d9.dll");
    if (!module) return nullptr;
    const auto address = GetProcAddress(module, name);
    Function function = nullptr;
    static_assert(sizeof(function) == sizeof(address));
    std::memcpy(&function, &address, sizeof(function));
    return function;
}

LUA_FUNCTION(StartupCapabilities) {
    const bool available = StartupExport<StatusFunction>("AstraStartupStatusJson") &&
        StartupExport<DisableFunction>("AstraDisableStartupMap");
    LUA->CreateTable();
    LUA->PushNumber(1); LUA->SetField(-2, "apiVersion");
    LUA->PushString("astra_map_importer"); LUA->SetField(-2, "ownedNamespace");
    LUA->PushBool(available); LUA->SetField(-2, "startupAssetDiscovery");
    LUA->PushBool(true); LUA->SetField(-2, "startupOnly");
    LUA->PushBool(true); LUA->SetField(-2, "synchronous");
    for (const auto* key : {"legacyMaterialOverrides", "livePreview", "authoritativeReset",
                           "atomicMapBatches", "atomicProfileLayers", "textureWriter",
                           "ddsImport", "wicImageConversion", "textureOperations"}) {
        LUA->PushBool(false); LUA->SetField(-2, key);
    }
    return 1;
}

LUA_FUNCTION(StartupGetStatus) {
    const auto function = StartupExport<StatusFunction>("AstraStartupStatusJson");
    if (function) {
        // Proxy storage is thread-local. PushString copies it before another
        // status call can invalidate it. Match the map runtime's 2 MiB limit.
        const auto* bytes = function();
        constexpr unsigned int limit = 2u * 1024u * 1024u;
        const auto length = bytes ? strnlen_s(bytes, limit + 1) : 0;
        if (length && length <= limit) {
            LUA->PushString(bytes, static_cast<unsigned int>(length));
            return 1;
        }
    }
    LUA->PushNil();
    return 1;
}

LUA_FUNCTION(StartupDisableMap) {
    if (!LUA->IsType(1, Type::String) || !LUA->IsType(2, Type::String)) {
        LUA->PushBool(false);
        return 1;
    }
    unsigned int mapLength = 0, generationLength = 0;
    const auto* map = LUA->GetString(1, &mapLength);
    const auto* generation = LUA->GetString(2, &generationLength);
    bool valid = map && generation && mapLength > 0 && mapLength <= 96 && generationLength == 64;
    for (unsigned int index = 0; valid && index < mapLength; ++index) {
        const char c = map[index];
        valid = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
    }
    for (unsigned int index = 0; valid && index < generationLength; ++index) {
        const char c = generation[index];
        valid = (c >= 'a' && c <= 'f') || (c >= '0' && c <= '9');
    }
    const auto function = StartupExport<DisableFunction>("AstraDisableStartupMap");
    LUA->PushBool(valid && function && function(map, generation));
    return 1;
}

void RegisterIfAbsent(ILuaBase* lua, const char* name) {
    lua->PushSpecial(GarrysMod::Lua::SPECIAL_GLOB);
    lua->GetField(-1, name);
    const bool absent = lua->IsType(-1, Type::Nil);
    lua->Pop();
    if (absent) {
        lua->CreateTable();
        lua->PushNumber(1); lua->SetField(-2, "API_VERSION");
        lua->PushCFunction(StartupCapabilities); lua->SetField(-2, "Capabilities");
        lua->PushCFunction(StartupGetStatus); lua->SetField(-2, "GetStartupStatus");
        lua->PushCFunction(StartupDisableMap); lua->SetField(-2, "DisableStartupMap");
        lua->SetField(-2, name);
    }
    lua->Pop();
}

void TryExistingFullProvider(ILuaBase* lua) {
    lua->PushSpecial(GarrysMod::Lua::SPECIAL_GLOB);
    lua->GetField(-1, "AstraRTXBridge");
    const bool absent = lua->IsType(-1, Type::Nil);
    lua->Pop();
    if (absent) {
        // Older map runtimes require the full helper lazily only while this
        // global is nil. Give an already installed helper its normal require
        // opportunity before publishing the startup-only compatibility table.
        lua->GetField(-1, "require");
        if (lua->IsType(-1, Type::Function)) {
            lua->PushString("astra_rtx_bridge");
            if (lua->PCall(1, 0, 0) != 0) lua->Pop(); // Missing helper is normal.
        } else {
            lua->Pop();
        }
    }
    lua->Pop();
}

} // namespace

void RegisterLua(ILuaBase* lua) {
    if (!lua) return;
    RegisterIfAbsent(lua, "RemixStartupAssets");
    // Existing GMA runtimes look for this table before attempting require().
    // Preserve a separately installed full provider whether it was registered
    // earlier or is still waiting for the usual first require().
    TryExistingFullProvider(lua);
    RegisterIfAbsent(lua, "AstraRTXBridge");
}

} // namespace AstraStartup
#endif
