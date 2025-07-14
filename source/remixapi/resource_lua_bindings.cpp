#ifdef _WIN64
#include "remixapi.h"
#include <tier0/dbg.h>

using namespace GarrysMod::Lua;

namespace RemixAPI {

// Lua function: RemixResource.ClearResources()
LUA_FUNCTION(RemixResource_ClearResources) {
    try {
        auto& resourceManager = RemixAPI::Instance().GetResourceManager();
        resourceManager.ClearResources();
        
        LUA->PushBool(true);
        return 1;
    } catch (...) {
        Error("[RemixResource] Exception in ClearResources\n");
        LUA->PushBool(false);
        return 1;
    }
}

// Lua function: RemixResource.ForceCleanup()
LUA_FUNCTION(RemixResource_ForceCleanup) {
    try {
        auto& resourceManager = RemixAPI::Instance().GetResourceManager();
        resourceManager.ForceCleanup();
        
        LUA->PushBool(true);
        return 1;
    } catch (...) {
        Error("[RemixResource] Exception in ForceCleanup\n");
        LUA->PushBool(false);
        return 1;
    }
}

// Lua function: RemixResource.SetMemoryLimits(maxCacheSize, maxVRAM)
LUA_FUNCTION(RemixResource_SetMemoryLimits) {
    if (!LUA->IsType(1, Type::Number)) {
        LUA->ThrowError("Expected number for max cache size (MB)");
        return 0;
    }
    
    if (!LUA->IsType(2, Type::Number)) {
        LUA->ThrowError("Expected number for max VRAM (MB)");
        return 0;
    }
    
    size_t maxCacheSize = static_cast<size_t>(LUA->GetNumber(1));
    size_t maxVRAM = static_cast<size_t>(LUA->GetNumber(2));
    
    auto& resourceManager = RemixAPI::Instance().GetResourceManager();
    resourceManager.SetMemoryLimits(maxCacheSize, maxVRAM);
    
    LUA->PushBool(true);
    return 1;
}

// Lua function: RemixResource.Present()
LUA_FUNCTION(RemixResource_Present) {
    try {
        RemixAPI::Instance().Present();
        
        LUA->PushBool(true);
        return 1;
    } catch (...) {
        Error("[RemixResource] Exception in Present\n");
        LUA->PushBool(false);
        return 1;
    }
}

// Initialize Resource Manager Lua bindings
void ResourceManager::InitializeLuaBindings() {
    if (!m_lua) return;
    
    // Get the global table
    m_lua->PushSpecial(GarrysMod::Lua::SPECIAL_GLOB);
    
    // Create RemixResource table
    m_lua->CreateTable();
    
    // Add resource management functions
    m_lua->PushCFunction(RemixResource_ClearResources);
    m_lua->SetField(-2, "ClearResources");
    
    m_lua->PushCFunction(RemixResource_ForceCleanup);
    m_lua->SetField(-2, "ForceCleanup");
    
    m_lua->PushCFunction(RemixResource_SetMemoryLimits);
    m_lua->SetField(-2, "SetMemoryLimits");
    
    m_lua->PushCFunction(RemixResource_Present);
    m_lua->SetField(-2, "Present");
    
    // Set the table as a global field
    m_lua->SetField(-2, "RemixResource");
    
    // Pop the global table
    m_lua->Pop();
    
    Msg("[ResourceManager] Lua bindings initialized\n");
}

} // namespace RemixAPI

#endif // _WIN64 