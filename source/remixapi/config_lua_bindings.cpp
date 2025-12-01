#ifdef _WIN64
#include "remixapi.h"
#include <tier0/dbg.h>

using namespace GarrysMod::Lua;

namespace RemixAPI {

// Lua function: RemixConfig.SetConfigVariable(key, value)
LUA_FUNCTION(RemixConfig_SetConfigVariable) {
    if (!LUA->IsType(1, Type::String)) {
        Warning("[RemixConfig] SetConfigVariable: Expected string for config key, got %s\n", LUA->GetTypeName(LUA->GetType(1)));
        LUA->PushBool(false);
        return 1;
    }
    
    if (!LUA->IsType(2, Type::String)) {
        Warning("[RemixConfig] SetConfigVariable: Expected string for config value, got %s\n", LUA->GetTypeName(LUA->GetType(2)));
        LUA->PushBool(false);
        return 1;
    }
    
    std::string key = LUA->GetString(1);
    std::string value = LUA->GetString(2);
    
    auto& configManager = RemixAPI::Instance().GetConfigManager();
    bool result = configManager.SetConfigVariable(key, value);
    
    if (!result) {
        Warning("[RemixConfig] SetConfigVariable: Failed to set '%s' = '%s' (check if key exists and value is valid)\n", 
                key.c_str(), value.c_str());
    }
    
    LUA->PushBool(result);
    return 1;
}

// Lua function: RemixConfig.GetConfigVariable(key)
LUA_FUNCTION(RemixConfig_GetConfigVariable) {
    if (!LUA->IsType(1, Type::String)) {
        Warning("[RemixConfig] GetConfigVariable: Expected string for config key, got %s\n", LUA->GetTypeName(LUA->GetType(1)));
        LUA->PushString("");
        return 1;
    }
    
    std::string key = LUA->GetString(1);
    
    auto& configManager = RemixAPI::Instance().GetConfigManager();
    std::string value = configManager.GetConfigVariable(key);
    
    LUA->PushString(value.c_str());
    return 1;
}

// Lua function: RemixConfig.GetUIState()
LUA_FUNCTION(RemixConfig_GetUIState) {
    auto& configManager = RemixAPI::Instance().GetConfigManager();
    remix::UIState state = configManager.GetUIState();
    
    LUA->PushNumber(static_cast<double>(state));
    return 1;
}

// Lua function: RemixConfig.SetUIState(state)
LUA_FUNCTION(RemixConfig_SetUIState) {
    if (!LUA->IsType(1, Type::Number)) {
        Warning("[RemixConfig] SetUIState: Expected number for UI state, got %s\n", LUA->GetTypeName(LUA->GetType(1)));
        LUA->PushBool(false);
        return 1;
    }
    
    int stateNum = static_cast<int>(LUA->GetNumber(1));
    remix::UIState state = static_cast<remix::UIState>(stateNum);
    
    auto& configManager = RemixAPI::Instance().GetConfigManager();
    bool result = configManager.SetUIState(state);
    
    LUA->PushBool(result);
    return 1;
}

// Lua function: RemixConfig.SetAdvancedUI(enabled)
LUA_FUNCTION(RemixConfig_SetAdvancedUI) {
    if (!LUA->IsType(1, Type::Bool)) {
        Warning("[RemixConfig] SetAdvancedUI: Expected boolean for advanced UI enabled, got %s\n", LUA->GetTypeName(LUA->GetType(1)));
        LUA->PushBool(false);
        return 1;
    }
    
    bool enabled = LUA->GetBool(1);
    
    auto& configManager = RemixAPI::Instance().GetConfigManager();
    bool result = configManager.SetConfigVariable("rtx.defaultToAdvancedUI", enabled ? "True" : "False");
    
    LUA->PushBool(result);
    return 1;
}

// Lua function: RemixConfig.SetRaytracing(enabled)
LUA_FUNCTION(RemixConfig_SetRaytracing) {
    if (!LUA->IsType(1, Type::Bool)) {
        Warning("[RemixConfig] SetRaytracing: Expected boolean for raytracing state, got %s\n", LUA->GetTypeName(LUA->GetType(1)));
        LUA->PushBool(false);
        return 1;
    }
    
    bool enabled = LUA->GetBool(1);
    std::string value = enabled ? "1" : "0";
    
    auto& configManager = RemixAPI::Instance().GetConfigManager();
    bool result = configManager.SetConfigVariable("rtx.enableRaytracing", value);
    
    LUA->PushBool(result);
    return 1;
}

// Lua function: RemixConfig.SetIgnoreDirectionalLights(enabled)
LUA_FUNCTION(RemixConfig_SetIgnoreDirectionalLights) {
    if (!LUA->IsType(1, Type::Bool)) {
        Warning("[RemixConfig] SetIgnoreDirectionalLights: Expected boolean for ignore directional lights state, got %s\n", LUA->GetTypeName(LUA->GetType(1)));
        LUA->PushBool(false);
        return 1;
    }
    
    bool enabled = LUA->GetBool(1);
    std::string value = enabled ? "1" : "0";
    
    auto& configManager = RemixAPI::Instance().GetConfigManager();
    bool result = configManager.SetConfigVariable("rtx.ignoreGameDirectionalLights", value);
    
    LUA->PushBool(result);
    return 1;
}

// Lua function: RemixConfig.SetUpscaler(upscaler)
LUA_FUNCTION(RemixConfig_SetUpscaler) {
    if (!LUA->IsType(1, Type::String)) {
        Warning("[RemixConfig] SetUpscaler: Expected string for upscaler type, got %s\n", LUA->GetTypeName(LUA->GetType(1)));
        LUA->PushBool(false);
        return 1;
    }
    
    std::string upscaler = LUA->GetString(1);
    
    auto& configManager = RemixAPI::Instance().GetConfigManager();
    bool result = configManager.SetConfigVariable("rtx.upscaler", upscaler);
    
    LUA->PushBool(result);
    return 1;
}

// Lua function: RemixConfig.SetDenoiser(denoiser)
LUA_FUNCTION(RemixConfig_SetDenoiser) {
    if (!LUA->IsType(1, Type::String)) {
        Warning("[RemixConfig] SetDenoiser: Expected string for denoiser type, got %s\n", LUA->GetTypeName(LUA->GetType(1)));
        LUA->PushBool(false);
        return 1;
    }
    
    std::string denoiser = LUA->GetString(1);
    
    auto& configManager = RemixAPI::Instance().GetConfigManager();
    bool result = configManager.SetConfigVariable("rtx.denoiser", denoiser);
    
    LUA->PushBool(result);
    return 1;
}

// Lua function: RemixConfig.SetReflectionUpscaler(upscaler)
LUA_FUNCTION(RemixConfig_SetReflectionUpscaler) {
    if (!LUA->IsType(1, Type::String)) {
        Warning("[RemixConfig] SetReflectionUpscaler: Expected string for reflection upscaler type, got %s\n", LUA->GetTypeName(LUA->GetType(1)));
        LUA->PushBool(false);
        return 1;
    }
    
    std::string upscaler = LUA->GetString(1);
    
    auto& configManager = RemixAPI::Instance().GetConfigManager();
    bool result = configManager.SetConfigVariable("rtx.reflectionUpscaler", upscaler);
    
    LUA->PushBool(result);
    return 1;
}

// Lua function: RemixConfig.SetRenderResolutionScale(scale)
LUA_FUNCTION(RemixConfig_SetRenderResolutionScale) {
    if (!LUA->IsType(1, Type::Number)) {
        Warning("[RemixConfig] SetRenderResolutionScale: Expected number for render resolution scale, got %s\n", LUA->GetTypeName(LUA->GetType(1)));
        LUA->PushBool(false);
        return 1;
    }
    
    float scale = static_cast<float>(LUA->GetNumber(1));
    std::string value = std::to_string(scale);
    
    auto& configManager = RemixAPI::Instance().GetConfigManager();
    bool result = configManager.SetConfigVariable("rtx.renderResolutionScale", value);
    
    LUA->PushBool(result);
    return 1;
}

// Lua function: RemixConfig.SetMaxBounces(bounces)
LUA_FUNCTION(RemixConfig_SetMaxBounces) {
    if (!LUA->IsType(1, Type::Number)) {
        Warning("[RemixConfig] SetMaxBounces: Expected number for max bounces, got %s\n", LUA->GetTypeName(LUA->GetType(1)));
        LUA->PushBool(false);
        return 1;
    }
    
    int bounces = static_cast<int>(LUA->GetNumber(1));
    std::string value = std::to_string(bounces);
    
    auto& configManager = RemixAPI::Instance().GetConfigManager();
    bool result = configManager.SetConfigVariable("rtx.maxBounces", value);
    
    LUA->PushBool(result);
    return 1;
}

// Lua function: RemixConfig.SetVolumetricEnabled(enabled)
LUA_FUNCTION(RemixConfig_SetVolumetricEnabled) {
    if (!LUA->IsType(1, Type::Bool)) {
        Warning("[RemixConfig] SetVolumetricEnabled: Expected boolean for volumetric enabled state, got %s\n", LUA->GetTypeName(LUA->GetType(1)));
        LUA->PushBool(false);
        return 1;
    }
    
    bool enabled = LUA->GetBool(1);
    std::string value = enabled ? "1" : "0";
    
    auto& configManager = RemixAPI::Instance().GetConfigManager();
    bool result = configManager.SetConfigVariable("rtx.volumetricEnabled", value);
    
    LUA->PushBool(result);
    return 1;
}


// Initialize Configuration Manager Lua bindings
void ConfigManager::InitializeLuaBindings() {
    if (!m_lua) return;
    
    // Get the global table
    m_lua->PushSpecial(GarrysMod::Lua::SPECIAL_GLOB);
    
    // Create RemixConfig table
    m_lua->CreateTable();
    
    // Core configuration functions
    m_lua->PushCFunction(RemixConfig_SetConfigVariable);
    m_lua->SetField(-2, "SetConfigVariable");
    
    m_lua->PushCFunction(RemixConfig_GetConfigVariable);
    m_lua->SetField(-2, "GetConfigVariable");
    
    // UI state functions
    m_lua->PushCFunction(RemixConfig_GetUIState);
    m_lua->SetField(-2, "GetUIState");
    
    m_lua->PushCFunction(RemixConfig_SetUIState);
    m_lua->SetField(-2, "SetUIState");
    
    m_lua->PushCFunction(RemixConfig_SetAdvancedUI);
    m_lua->SetField(-2, "SetAdvancedUI");
    
    // Common configuration shortcuts
    m_lua->PushCFunction(RemixConfig_SetRaytracing);
    m_lua->SetField(-2, "SetRaytracing");
    
    m_lua->PushCFunction(RemixConfig_SetIgnoreDirectionalLights);
    m_lua->SetField(-2, "SetIgnoreDirectionalLights");
    
    m_lua->PushCFunction(RemixConfig_SetUpscaler);
    m_lua->SetField(-2, "SetUpscaler");
    
    m_lua->PushCFunction(RemixConfig_SetDenoiser);
    m_lua->SetField(-2, "SetDenoiser");
    
    m_lua->PushCFunction(RemixConfig_SetReflectionUpscaler);
    m_lua->SetField(-2, "SetReflectionUpscaler");
    
    m_lua->PushCFunction(RemixConfig_SetRenderResolutionScale);
    m_lua->SetField(-2, "SetRenderResolutionScale");
    
    m_lua->PushCFunction(RemixConfig_SetMaxBounces);
    m_lua->SetField(-2, "SetMaxBounces");
    
    m_lua->PushCFunction(RemixConfig_SetVolumetricEnabled);
    m_lua->SetField(-2, "SetVolumetricEnabled");
    
    // Add UI state constants
    m_lua->CreateTable();
    m_lua->PushNumber(static_cast<double>(remix::UIState::None));
    m_lua->SetField(-2, "None");
    m_lua->PushNumber(static_cast<double>(remix::UIState::Basic));
    m_lua->SetField(-2, "Basic");
    m_lua->PushNumber(static_cast<double>(remix::UIState::Advanced));
    m_lua->SetField(-2, "Advanced");
    m_lua->SetField(-2, "UIState");
    
    // Set the table as a global field
    m_lua->SetField(-2, "RemixConfig");
    
    // Pop the global table
    m_lua->Pop();
    
    Msg("[ConfigManager] Lua bindings initialized\n");
}

} // namespace RemixAPI

#endif // _WIN64 