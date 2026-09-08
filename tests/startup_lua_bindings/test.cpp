#include <cstring>
#include <cwchar>
#include <cstdlib>
#include <iostream>
#include <string>
#include "../../source/startup_lua_bindings.cpp"

using GarrysMod::Lua::ILuaBase;
using GarrysMod::Lua::Value;
namespace Type = GarrysMod::Lua::Type;

namespace {
bool loaded = false, statusExport = true, disableExport = true, accepted = true;
bool nullStatus = false;
unsigned int lookups = 0, disables = 0;
unsigned int requires = 0;
bool helperInstalled = false;
std::string status = "{\"version\":1,\"maps\":{}}", receivedMap, receivedGeneration;

const char* __cdecl FakeStatus() { return nullStatus ? nullptr : status.c_str(); }
bool __cdecl FakeDisable(const char* map, const char* generation) {
    ++disables; receivedMap = map; receivedGeneration = generation; return accepted;
}
void Check(bool condition, const char* message) {
    if (!condition) { std::cerr << "FAIL: " << message << '\n'; std::exit(1); }
}
Value String(const std::string& text) { Value value; value.type = Type::String; value.string = text; return value; }
int FakeRequire(ILuaBase* lua) {
    ++requires;
    Check(std::strcmp(lua->GetString(1), "astra_rtx_bridge") == 0, "require only the existing optional full provider");
    if (!helperInstalled) throw std::runtime_error("binary helper absent or failed to initialize");
    lua->CreateTable();
    const auto provider = lua->stack.back();
    (*provider.table)["writer_marker"] = String("installed full provider");
    (*lua->globals.table)["AstraRTXBridge"] = provider;
    return 0;
}
void InstallRequire(ILuaBase& lua) {
    Value function; function.type = Type::Function; function.function = FakeRequire;
    (*lua.globals.table)["require"] = function;
}
Value Call(ILuaBase& lua, const char* method, std::vector<Value> arguments = {}) {
    auto table = lua.globals.table->at("AstraRTXBridge").table;
    const auto function = table->at(method).function;
    lua.stack = std::move(arguments);
    Check(function(&lua) == 1, "callback returns exactly one value");
    const auto result = lua.stack.back(); lua.stack.clear(); return result;
}
template <typename T> FARPROC Address(T function) {
    FARPROC output = nullptr;
    static_assert(sizeof(output) == sizeof(function));
    std::memcpy(&output, &function, sizeof(output)); return output;
}
}

HMODULE GetModuleHandleW(const wchar_t* name) {
    ++lookups;
    Check(std::wcscmp(name, L"d3d9.dll") == 0, "inspect only the loaded renderer proxy");
    return loaded ? reinterpret_cast<HMODULE>(1) : nullptr;
}
FARPROC GetProcAddress(HMODULE module, const char* name) {
    Check(module == reinterpret_cast<HMODULE>(1), "resolve from the loaded proxy");
    if (std::strcmp(name, "AstraStartupStatusJson") == 0) return statusExport ? Address(FakeStatus) : nullptr;
    if (std::strcmp(name, "AstraDisableStartupMap") == 0) return disableExport ? Address(FakeDisable) : nullptr;
    Check(false, "no unrelated native entry points are resolved"); return nullptr;
}

int main() {
    AstraStartup::RegisterLua(nullptr);
    ILuaBase lua;
    lua.PushBool(true);
    AstraStartup::RegisterLua(&lua);
    Check(lua.stack.size() == 1 && lua.stack.back().boolean, "registration preserves caller stack");
    Check(lookups == 0, "registration never invokes the renderer");
    const auto bridge = lua.globals.table->at("AstraRTXBridge").table;
    const auto canonical = lua.globals.table->at("RemixStartupAssets").table;
    Check(bridge->size() == 4 && canonical->size() == 4, "only the startup API is exposed");
    Check(bridge->at("API_VERSION").number == 1, "existing GMA API version is retained");
    AstraStartup::RegisterLua(&lua);
    Check(lua.globals.table->at("AstraRTXBridge").table == bridge &&
          lua.globals.table->at("RemixStartupAssets").table == canonical, "registration is idempotent");

    auto capabilities = Call(lua, "Capabilities").table;
    Check(!capabilities->at("startupAssetDiscovery").boolean, "no proxy means unavailable");
    Check(capabilities->at("startupOnly").boolean && capabilities->at("synchronous").boolean,
          "startup-only synchronous boundary is explicit");
    Check(capabilities->at("ownedNamespace").string == "astra_map_importer", "GMA provider contract matches");
    for (const auto* key : {"legacyMaterialOverrides", "livePreview", "authoritativeReset", "atomicMapBatches",
                           "atomicProfileLayers", "textureWriter", "ddsImport", "wicImageConversion", "textureOperations"})
        Check(!capabilities->at(key).boolean, "never advertises a material writer");
    Check(Call(lua, "GetStartupStatus").type == Type::Nil, "missing proxy returns nil status");
    const std::string generation(64, 'a');
    Check(!Call(lua, "DisableStartupMap", {String("gm_fixture"), String(generation)}).boolean && disables == 0,
          "missing proxy cannot disable a map");

    loaded = true;
    Check(Call(lua, "Capabilities").table->at("startupAssetDiscovery").boolean, "both loaded exports expose startup capability");
    disableExport = false;
    Check(!Call(lua, "Capabilities").table->at("startupAssetDiscovery").boolean, "partial proxy cannot claim full capability");
    disableExport = true;
    const auto snapshot = Call(lua, "GetStartupStatus");
    status.assign(2097152, 'x');
    Check(snapshot.string == "{\"version\":1,\"maps\":{}}", "status is copied into Lua-owned storage");
    Check(Call(lua, "GetStartupStatus").string.size() == 2097152, "2 MiB status boundary is accepted");
    status.push_back('x');
    Check(Call(lua, "GetStartupStatus").type == Type::Nil, "oversized status is rejected");
    status.clear();
    Check(Call(lua, "GetStartupStatus").type == Type::Nil, "empty status is rejected");
    nullStatus = true;
    Check(Call(lua, "GetStartupStatus").type == Type::Nil, "null status is rejected");
    nullStatus = false;
    statusExport = false;
    Check(Call(lua, "GetStartupStatus").type == Type::Nil, "missing status export is unavailable");
    statusExport = true;

    for (const auto& map : {std::string(), std::string(97, 'a'), std::string("../foreign"),
                           std::string("GM_fixture"), std::string("gm_fixture\0other", 16)})
        Check(!Call(lua, "DisableStartupMap", {String(map), String(generation)}).boolean, "invalid map is rejected");
    for (const auto& hash : {std::string(63, 'a'), std::string(65, 'a'), std::string(64, 'A'),
                            std::string(64, 'g'), std::string(63, 'a') + '\0'})
        Check(!Call(lua, "DisableStartupMap", {String("gm_fixture"), String(hash)}).boolean, "invalid generation is rejected");
    Check(!Call(lua, "DisableStartupMap", {Value{}, String(generation)}).boolean, "wrong map type is rejected");
    Check(!Call(lua, "DisableStartupMap", {String("gm_fixture"), Value{}}).boolean, "wrong generation type is rejected");
    Check(disables == 0, "invalid inputs never reach proxy deactivation");
    Check(Call(lua, "DisableStartupMap", {String("gm_fixture-1"), String(generation)}).boolean,
          "valid deactivation delegates to the proxy");
    Check(disables == 1 && receivedMap == "gm_fixture-1" && receivedGeneration == generation,
          "map and generation are forwarded exactly");
    accepted = false;
    Check(!Call(lua, "DisableStartupMap", {String(std::string(96, 'a')), String(generation)}).boolean && disables == 2,
          "maximum identifier length and native refusal propagate");

    ILuaBase existing;
    existing.CreateTable();
    auto fullProvider = existing.stack.back();
    fullProvider.table->emplace("writer_marker", String("preserve me"));
    (*existing.globals.table)["AstraRTXBridge"] = fullProvider;
    existing.stack.clear();
    AstraStartup::RegisterLua(&existing);
    Check(existing.globals.table->at("AstraRTXBridge").table == fullProvider.table && fullProvider.table->size() == 1,
          "full provider table, identity and methods remain untouched");
    Check(existing.globals.table->count("RemixStartupAssets") == 1 && existing.stack.empty(),
          "canonical API remains available alongside a full provider");

    InstallRequire(existing);
    AstraStartup::RegisterLua(&existing);
    Check(requires == 0 && existing.globals.table->at("AstraRTXBridge").table == fullProvider.table,
          "already registered provider never triggers redundant require");
    ILuaBase missing;
    InstallRequire(missing);
    missing.PushString("caller stack");
    AstraStartup::RegisterLua(&missing);
    Check(requires == 1 && missing.globals.table->at("AstraRTXBridge").table->size() == 4,
          "absent or failed optional helper falls back to startup-only compatibility");
    Check(missing.stack.size() == 1 && missing.stack.back().string == "caller stack",
          "protected require error leaves no error object or stack damage");
    AstraStartup::RegisterLua(&missing);
    Check(requires == 1, "repeated registration does not retry a missing helper");
    helperInstalled = true;
    ILuaBase lazy;
    InstallRequire(lazy);
    lazy.PushBool(true);
    AstraStartup::RegisterLua(&lazy);
    Check(requires == 2 && lazy.globals.table->at("AstraRTXBridge").table->at("writer_marker").string == "installed full provider",
          "installed but lazy full helper receives its normal require opportunity");
    Check(lazy.globals.table->at("AstraRTXBridge").table->size() == 1 &&
          lazy.globals.table->at("RemixStartupAssets").table->size() == 4,
          "full provider and independent startup API retain separate identities and capabilities");
    Check(lazy.stack.size() == 1 && lazy.stack.back().boolean, "successful require preserves caller stack");
    std::cout << "PASS: actual startup Lua registration, proxy boundary, bounded status, provider coexistence and scoped deactivation\n";
}
