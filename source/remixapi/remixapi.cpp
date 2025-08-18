#ifdef _WIN64
#include "remixapi.h"
#include "rtx_option_defaults.h"
#include <tier0/dbg.h>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>

// Lua bindings are implemented in separate .cpp files that are compiled independently
// No need to include them here since they define their own functions

// External global variables
extern remix::Interface* g_remix;
extern IDirect3DDevice9Ex* g_d3dDevice;

using namespace GarrysMod::Lua;

namespace RemixAPI {

//=============================================================================
// RemixAPI Main Class
//=============================================================================
RemixAPI& RemixAPI::Instance() {
    static RemixAPI instance;
    return instance;
}

RemixAPI::RemixAPI() 
    : m_remixInterface(nullptr)
    , m_lua(nullptr)
    , m_initialized(false) {
}

RemixAPI::~RemixAPI() {
    Shutdown();
}

bool RemixAPI::Initialize(remix::Interface* remixInterface, GarrysMod::Lua::ILuaBase* LUA) {
    if (m_initialized) {
        Msg("[RemixAPI] Already initialized\n");
        return false;
    }

    if (!remixInterface || !LUA) {
        Error("[RemixAPI] Invalid parameters for initialization\n");
        return false;
    }

    m_remixInterface = remixInterface;
    m_lua = LUA;

            // Initialize all managers
        m_materialManager = std::make_unique<MaterialManager>(remixInterface, LUA);
        m_meshManager = std::make_unique<MeshManager>(remixInterface, LUA);
        m_cameraManager = std::make_unique<CameraManager>(remixInterface, LUA);
        m_instanceManager = std::make_unique<InstanceManager>(remixInterface, LUA);
        m_configManager = std::make_unique<ConfigManager>(remixInterface, LUA);
        m_resourceManager = std::make_unique<ResourceManager>(remixInterface, LUA);
        m_lightManager = std::make_unique<LightManager>(remixInterface, LUA);

            // Initialize Lua bindings for all managers
        m_materialManager->InitializeLuaBindings();
        m_meshManager->InitializeLuaBindings();
        m_cameraManager->InitializeLuaBindings();
        m_instanceManager->InitializeLuaBindings();
        m_configManager->InitializeLuaBindings();
        m_resourceManager->InitializeLuaBindings();
        m_lightManager->InitializeLuaBindings();

    m_initialized = true;
    Msg("[RemixAPI] Initialization complete\n");
    return true;
}

void RemixAPI::Shutdown() {
    if (!m_initialized) return;

    m_resourceManager.reset();
    m_lightManager.reset();
    m_configManager.reset();
    m_instanceManager.reset();
    m_cameraManager.reset();
    m_meshManager.reset();
    m_materialManager.reset();

    m_remixInterface = nullptr;
    m_lua = nullptr;
    m_initialized = false;
    
    Msg("[RemixAPI] Shutdown complete\n");
}

void RemixAPI::Present() {
    if (!m_initialized || !m_remixInterface) return;
    
    remixapi_PresentInfo presentInfo = {};
    presentInfo.sType = REMIXAPI_STRUCT_TYPE_PRESENT_INFO;
    presentInfo.pNext = nullptr;
    presentInfo.hwndOverride = nullptr;
    
    m_remixInterface->Present(&presentInfo);
}

//=============================================================================
// LightManager
//=============================================================================
LightManager::LightManager(remix::Interface* remixInterface, GarrysMod::Lua::ILuaBase* LUA)
    : m_remixInterface(remixInterface)
    , m_lua(LUA) {
}

LightManager::~LightManager() {
    ClearAllLights();
}

uint64_t LightManager::CreateSphereLight(const remix::LightInfo& base, const remix::LightInfoSphereEXT& ext, uint64_t entityId) {
    if (!m_remixInterface) return 0;
    std::lock_guard<std::mutex> guard(m_mutex);
    remix::LightInfo info = base;
    info.pNext = const_cast<remix::LightInfoSphereEXT*>(&ext);
    auto created = m_remixInterface->CreateLight(info);
    if (!created) {
        Error("[LightManager] Failed to create sphere light: %d\n", created.status());
        return 0;
    }
    uint64_t id = m_nextLightId++;
    m_lights.emplace(id, ManagedLight{ created.value(), entityId });
    if (entityId) m_entityToLight.emplace(entityId, id);
    return id;
}

uint64_t LightManager::CreateRectLight(const remix::LightInfo& base, const remix::LightInfoRectEXT& ext, uint64_t entityId) {
    if (!m_remixInterface) return 0;
    std::lock_guard<std::mutex> guard(m_mutex);
    remix::LightInfo info = base;
    info.pNext = const_cast<remix::LightInfoRectEXT*>(&ext);
    auto created = m_remixInterface->CreateLight(info);
    if (!created) {
        Error("[LightManager] Failed to create rect light: %d\n", created.status());
        return 0;
    }
    uint64_t id = m_nextLightId++;
    m_lights.emplace(id, ManagedLight{ created.value(), entityId });
    if (entityId) m_entityToLight.emplace(entityId, id);
    return id;
}

uint64_t LightManager::CreateDiskLight(const remix::LightInfo& base, const remix::LightInfoDiskEXT& ext, uint64_t entityId) {
    if (!m_remixInterface) return 0;
    std::lock_guard<std::mutex> guard(m_mutex);
    remix::LightInfo info = base;
    info.pNext = const_cast<remix::LightInfoDiskEXT*>(&ext);
    auto created = m_remixInterface->CreateLight(info);
    if (!created) {
        Error("[LightManager] Failed to create disk light: %d\n", created.status());
        return 0;
    }
    uint64_t id = m_nextLightId++;
    m_lights.emplace(id, ManagedLight{ created.value(), entityId });
    if (entityId) m_entityToLight.emplace(entityId, id);
    return id;
}

uint64_t LightManager::CreateDistantLight(const remix::LightInfo& base, const remix::LightInfoDistantEXT& ext, uint64_t entityId) {
    if (!m_remixInterface) return 0;
    std::lock_guard<std::mutex> guard(m_mutex);
    remix::LightInfo info = base;
    info.pNext = const_cast<remix::LightInfoDistantEXT*>(&ext);
    auto created = m_remixInterface->CreateLight(info);
    if (!created) {
        Error("[LightManager] Failed to create distant light: %d\n", created.status());
        return 0;
    }
    uint64_t id = m_nextLightId++;
    m_lights.emplace(id, ManagedLight{ created.value(), entityId });
    if (entityId) m_entityToLight.emplace(entityId, id);
    return id;
}

bool LightManager::DestroyLight(uint64_t lightId) {
    std::lock_guard<std::mutex> guard(m_mutex);
    auto it = m_lights.find(lightId);
    if (it == m_lights.end()) return false;
    if (it->second.handle) {
        m_remixInterface->DestroyLight(it->second.handle);
    }
    // remove from entity map
    if (it->second.entityId) {
        auto range = m_entityToLight.equal_range(it->second.entityId);
        for (auto r = range.first; r != range.second; ) {
            if (r->second == lightId) r = m_entityToLight.erase(r); else ++r;
        }
    }
    m_lights.erase(it);
    return true;
}

bool LightManager::UpdateSphereLight(uint64_t lightId, const remix::LightInfo& base, const remix::LightInfoSphereEXT& ext) {
    std::lock_guard<std::mutex> guard(m_mutex);
    auto it = m_lights.find(lightId);
    if (it == m_lights.end() || !it->second.handle) return false;
    remix::LightInfo info = base; info.pNext = const_cast<remix::LightInfoSphereEXT*>(&ext);
    return m_remixInterface->UpdateLightDefinition(it->second.handle, info);
}

bool LightManager::UpdateRectLight(uint64_t lightId, const remix::LightInfo& base, const remix::LightInfoRectEXT& ext) {
    std::lock_guard<std::mutex> guard(m_mutex);
    auto it = m_lights.find(lightId);
    if (it == m_lights.end() || !it->second.handle) return false;
    remix::LightInfo info = base; info.pNext = const_cast<remix::LightInfoRectEXT*>(&ext);
    return m_remixInterface->UpdateLightDefinition(it->second.handle, info);
}

bool LightManager::UpdateDiskLight(uint64_t lightId, const remix::LightInfo& base, const remix::LightInfoDiskEXT& ext) {
    std::lock_guard<std::mutex> guard(m_mutex);
    auto it = m_lights.find(lightId);
    if (it == m_lights.end() || !it->second.handle) return false;
    remix::LightInfo info = base; info.pNext = const_cast<remix::LightInfoDiskEXT*>(&ext);
    return m_remixInterface->UpdateLightDefinition(it->second.handle, info);
}

bool LightManager::UpdateDistantLight(uint64_t lightId, const remix::LightInfo& base, const remix::LightInfoDistantEXT& ext) {
    std::lock_guard<std::mutex> guard(m_mutex);
    auto it = m_lights.find(lightId);
    if (it == m_lights.end() || !it->second.handle) return false;
    remix::LightInfo info = base; info.pNext = const_cast<remix::LightInfoDistantEXT*>(&ext);
    return m_remixInterface->UpdateLightDefinition(it->second.handle, info);
}
bool LightManager::HasLight(uint64_t lightId) const {
    return m_lights.find(lightId) != m_lights.end();
}

bool LightManager::HasLightForEntity(uint64_t entityId) const {
    return m_entityToLight.find(entityId) != m_entityToLight.end();
}

void LightManager::DestroyLightsForEntity(uint64_t entityId) {
    std::lock_guard<std::mutex> guard(m_mutex);
    auto range = m_entityToLight.equal_range(entityId);
    for (auto it = range.first; it != range.second; ++it) {
        DestroyLight(it->second);
    }
}

void LightManager::ClearAllLights() {
    std::lock_guard<std::mutex> guard(m_mutex);
    for (auto& [id, light] : m_lights) {
        if (light.handle) m_remixInterface->DestroyLight(light.handle);
    }
    m_lights.clear();
    m_entityToLight.clear();
}

size_t LightManager::GetLightCount() const {
    return m_lights.size();
}

void LightManager::SubmitLightsForCurrentFrame() {
    // No-op: native Remix auto-instancing handles per-frame submission
}

//=============================================================================
// MaterialManager
//=============================================================================
MaterialManager::MaterialManager(remix::Interface* remixInterface, GarrysMod::Lua::ILuaBase* LUA)
    : m_remixInterface(remixInterface)
    , m_lua(LUA)
    , m_nextMaterialId(1) {
}

MaterialManager::~MaterialManager() {
    // Clean up all materials
    for (auto& pair : m_materials) {
        if (pair.second.handle) {
            m_remixInterface->DestroyMaterial(pair.second.handle);
        }
    }
    m_materials.clear();
}

uint64_t MaterialManager::CreateMaterial(const std::string& name, const remix::MaterialInfo& info) {
    if (!m_remixInterface) return 0;

    auto result = m_remixInterface->CreateMaterial(info);
    if (!result) {
        Error("[MaterialManager] Failed to create material '%s': %d\n", name.c_str(), result.status());
        return 0;
    }

    uint64_t materialId = m_nextMaterialId++;
    ManagedMaterial material = {
        result.value(),
        name,
        info
    };
    
    m_materials[materialId] = material;
    Msg("[MaterialManager] Created material '%s' with ID %llu\n", name.c_str(), materialId);
    return materialId;
}

uint64_t MaterialManager::CreateOpaqueMaterial(const std::string& name, const remix::MaterialInfo& info, const remix::MaterialInfoOpaqueEXT& opaqueInfo) {
    if (!m_remixInterface) return 0;

    remix::MaterialInfo materialInfo = info;
    materialInfo.pNext = const_cast<remix::MaterialInfoOpaqueEXT*>(&opaqueInfo);

    return CreateMaterial(name, materialInfo);
}

uint64_t MaterialManager::CreateTranslucentMaterial(const std::string& name, const remix::MaterialInfo& info, const remix::MaterialInfoTranslucentEXT& translucentInfo) {
    if (!m_remixInterface) return 0;

    remix::MaterialInfo materialInfo = info;
    materialInfo.pNext = const_cast<remix::MaterialInfoTranslucentEXT*>(&translucentInfo);

    return CreateMaterial(name, materialInfo);
}

bool MaterialManager::UpdateMaterial(uint64_t materialId, const remix::MaterialInfo& info) {
    auto it = m_materials.find(materialId);
    if (it == m_materials.end()) {
        Error("[MaterialManager] Material ID %llu not found\n", materialId);
        return false;
    }

    // For now, we need to recreate the material
    // TODO: Check if Remix API supports material updates
    auto oldHandle = it->second.handle;
    auto result = m_remixInterface->CreateMaterial(info);
    if (!result) {
        Error("[MaterialManager] Failed to update material ID %llu: %d\n", materialId, result.status());
        return false;
    }

    m_remixInterface->DestroyMaterial(oldHandle);
    it->second.handle = result.value();
    it->second.info = info;
    
    return true;
}

bool MaterialManager::DestroyMaterial(uint64_t materialId) {
    auto it = m_materials.find(materialId);
    if (it == m_materials.end()) {
        Error("[MaterialManager] Material ID %llu not found\n", materialId);
        return false;
    }

    if (it->second.handle) {
        m_remixInterface->DestroyMaterial(it->second.handle);
    }
    
    m_materials.erase(it);
    Msg("[MaterialManager] Destroyed material ID %llu\n", materialId);
    return true;
}

bool MaterialManager::HasMaterial(uint64_t materialId) const {
    return m_materials.find(materialId) != m_materials.end();
}

//=============================================================================
// MeshManager
//=============================================================================
MeshManager::MeshManager(remix::Interface* remixInterface, GarrysMod::Lua::ILuaBase* LUA)
    : m_remixInterface(remixInterface)
    , m_lua(LUA)
    , m_nextMeshId(1) {
}

MeshManager::~MeshManager() {
    // Clean up all meshes
    for (auto& pair : m_meshes) {
        if (pair.second.handle) {
            m_remixInterface->DestroyMesh(pair.second.handle);
        }
    }
    m_meshes.clear();
}

uint64_t MeshManager::CreateMesh(const std::string& name, const remix::MeshInfo& info) {
    if (!m_remixInterface) return 0;

    auto result = m_remixInterface->CreateMesh(info);
    if (!result) {
        Error("[MeshManager] Failed to create mesh '%s': %d\n", name.c_str(), result.status());
        return 0;
    }

    uint64_t meshId = m_nextMeshId++;
    ManagedMesh mesh = {
        result.value(),
        name,
        info
    };
    
    m_meshes[meshId] = mesh;
    Msg("[MeshManager] Created mesh '%s' with ID %llu\n", name.c_str(), meshId);
    return meshId;
}

bool MeshManager::UpdateMesh(uint64_t meshId, const remix::MeshInfo& info) {
    auto it = m_meshes.find(meshId);
    if (it == m_meshes.end()) {
        Error("[MeshManager] Mesh ID %llu not found\n", meshId);
        return false;
    }

    // For now, we need to recreate the mesh
    // TODO: Check if Remix API supports mesh updates
    auto oldHandle = it->second.handle;
    auto result = m_remixInterface->CreateMesh(info);
        if (!result) {
        Error("[MeshManager] Failed to update mesh ID %llu: %d\n", meshId, result.status());
        return false;
    }

    m_remixInterface->DestroyMesh(oldHandle);
    it->second.handle = result.value();
    it->second.info = info;
    
    return true;
}

bool MeshManager::DestroyMesh(uint64_t meshId) {
    auto it = m_meshes.find(meshId);
    if (it == m_meshes.end()) {
        Error("[MeshManager] Mesh ID %llu not found\n", meshId);
        return false;
    }

    if (it->second.handle) {
        m_remixInterface->DestroyMesh(it->second.handle);
    }
    
    m_meshes.erase(it);
    Msg("[MeshManager] Destroyed mesh ID %llu\n", meshId);
    return true;
}

bool MeshManager::HasMesh(uint64_t meshId) const {
    return m_meshes.find(meshId) != m_meshes.end();
}

//=============================================================================
// CameraManager
//=============================================================================
CameraManager::CameraManager(remix::Interface* remixInterface, GarrysMod::Lua::ILuaBase* LUA)
    : m_remixInterface(remixInterface)
    , m_lua(LUA) {
}

CameraManager::~CameraManager() {
}

bool CameraManager::SetupCamera(const remix::CameraInfo& info) {
    if (!m_remixInterface) return false;

    auto result = m_remixInterface->SetupCamera(info);
    if (!result) {
        Error("[CameraManager] Failed to setup camera: %d\n", result.status());
        return false;
    }
    
    return true;
}

bool CameraManager::SetupParameterizedCamera(const remix::CameraInfoParameterizedEXT& info) {
    if (!m_remixInterface) return false;

    remix::CameraInfo cameraInfo;
    cameraInfo.sType = REMIXAPI_STRUCT_TYPE_CAMERA_INFO;
    cameraInfo.pNext = const_cast<remix::CameraInfoParameterizedEXT*>(&info);
    cameraInfo.type = REMIXAPI_CAMERA_TYPE_WORLD;

    return SetupCamera(cameraInfo);
}

//=============================================================================
// InstanceManager
//=============================================================================
InstanceManager::InstanceManager(remix::Interface* remixInterface, GarrysMod::Lua::ILuaBase* LUA)
    : m_remixInterface(remixInterface)
    , m_lua(LUA) {
}

InstanceManager::~InstanceManager() {
}

bool InstanceManager::DrawInstance(const remix::InstanceInfo& info) {
    if (!m_remixInterface) return false;

    auto result = m_remixInterface->DrawInstance(info);
    if (!result) {
        Error("[InstanceManager] Failed to draw instance: %d\n", result.status());
        return false;
    }
    
    return true;
}

bool InstanceManager::DrawInstanceWithBlend(const remix::InstanceInfo& info, const remix::InstanceInfoBlendEXT& blendInfo) {
    if (!m_remixInterface) return false;

    remix::InstanceInfo instanceInfo = info;
    instanceInfo.pNext = const_cast<remix::InstanceInfoBlendEXT*>(&blendInfo);

    return DrawInstance(instanceInfo);
}

bool InstanceManager::DrawInstanceWithBones(const remix::InstanceInfo& info, const remix::InstanceInfoBoneTransformsEXT& boneInfo) {
    if (!m_remixInterface) return false;

    remix::InstanceInfo instanceInfo = info;
    instanceInfo.pNext = const_cast<remix::InstanceInfoBoneTransformsEXT*>(&boneInfo);

    return DrawInstance(instanceInfo);
}

//=============================================================================
// ConfigManager
//=============================================================================
ConfigManager::ConfigManager(remix::Interface* remixInterface, GarrysMod::Lua::ILuaBase* LUA)
    : m_remixInterface(remixInterface)
    , m_lua(LUA) {
}

ConfigManager::~ConfigManager() {
}

bool ConfigManager::SetConfigVariable(const std::string& key, const std::string& value) {
    if (!m_remixInterface) return false;

    // Handle deprecated/invalid config variables with suggestions
    if (key == "rtx.enableAdvancedMode") {
        Warning("[ConfigManager] 'rtx.enableAdvancedMode' is not a valid RTX option. Use 'rtx.showUI' (0=Don't Show, 1=Show Simple, 2=Show Advanced) or 'rtx.defaultToAdvancedUI' (True/False) instead.\n");
        
        // Auto-convert to the correct option
        if (value == "1" || value == "true" || value == "True") {
            // Try to set advanced UI as default
            auto result = m_remixInterface->SetConfigVariable("rtx.defaultToAdvancedUI", "True");
            if (result) {
                return true;
            }
        }
        return false;
    }

    auto result = m_remixInterface->SetConfigVariable(key.c_str(), value.c_str());
    if (!result) {
        Error("[ConfigManager] Failed to set config variable '%s': %d\n", key.c_str(), result.status());
        return false;
    }
    
    return true;
}

std::string ConfigManager::GetConfigVariable(const std::string& key) {
    // Try to read from rtx.conf file first
    std::string confPath = FindRtxConfPath();
    if (!confPath.empty() && std::filesystem::exists(confPath)) {
        std::unordered_map<std::string, std::string> fileConfig = ParseConfigFile(confPath);
        auto fileIt = fileConfig.find(key);
        if (fileIt != fileConfig.end()) {
            return fileIt->second;
        }
    }
    
    // If not found in file, return empty string (no default fallback)
    // Note: RTX Remix API doesn't support reading config variables back
    return "";
}



remix::UIState ConfigManager::GetUIState() {
    if (!m_remixInterface) return remix::UIState::None;

    auto result = m_remixInterface->GetUIState();
    if (!result) {
        Error("[ConfigManager] Failed to get UI state: %d\n", result.status());
        return remix::UIState::None;
    }
    
    return result.value();
}

bool ConfigManager::SetUIState(remix::UIState state) {
    if (!m_remixInterface) return false;

    auto result = m_remixInterface->SetUIState(state);
    if (!result) {
        Error("[ConfigManager] Failed to set UI state: %d\n", result.status());
        return false;
    }
    
    return true;
}

std::string ConfigManager::FindGameDirectory() const {
    // Get the path to the current DLL (which should be in bin/win64/)
    HMODULE hModule = GetModuleHandle(nullptr);
    if (!hModule) {
        hModule = GetModuleHandle("stdshader_dx6.dll"); // this can be any dll in the executable directory
    }
    
    if (!hModule) {
        return "";
    }
    
    char dllPath[MAX_PATH];
    if (GetModuleFileName(hModule, dllPath, sizeof(dllPath)) == 0) {
        return "";
    }
    
    // Convert to filesystem path
    std::filesystem::path path(dllPath);
    
    // Navigate from bin/win64/ to the game root
    // Expected structure: GameRoot/bin/win64/stdshader_dx6.dll
    auto gameRoot = path.parent_path().parent_path().parent_path();
    
    return gameRoot.string();
}

std::string ConfigManager::FindRtxConfPath() const {
    std::string gameDir = FindGameDirectory();
    if (gameDir.empty()) {
        return "";
    }
    
    // RTX config is typically in the game root directory
    std::filesystem::path confPath = std::filesystem::path(gameDir) / "rtx.conf";
    return confPath.string();
}

std::unordered_map<std::string, std::string> ConfigManager::ParseConfigFile(const std::string& filePath) const {
    std::unordered_map<std::string, std::string> config;
    
    std::ifstream file(filePath);
    if (!file.is_open()) {
        Msg("[ConfigManager] Could not open config file: %s\n", filePath.c_str());
        return config;
    }
    
    std::string line;
    while (std::getline(file, line)) {
        // Skip empty lines and comments
        if (line.empty() || line[0] == '#' || line[0] == ';') {
            continue;
        }
        
        // Find the equals sign
        size_t equalPos = line.find('=');
        if (equalPos == std::string::npos) {
            continue;
        }
        
        std::string key = line.substr(0, equalPos);
        std::string value = line.substr(equalPos + 1);
        
        // Trim whitespace
        key.erase(0, key.find_first_not_of(" \t"));
        key.erase(key.find_last_not_of(" \t") + 1);
        value.erase(0, value.find_first_not_of(" \t"));
        value.erase(value.find_last_not_of(" \t") + 1);
        
        // Remove quotes if present
        if (value.length() >= 2 && value[0] == '"' && value[value.length()-1] == '"') {
            value = value.substr(1, value.length()-2);
        }
        
        config[key] = value;
    }
    
    Msg("[ConfigManager] Parsed %zu config entries from %s\n", config.size(), filePath.c_str());
    return config;
}

std::string ConfigManager::GetDefaultValueFromRtxOptions(const std::string& key) const {
    // Look up the exact default value from the extracted RTX options defaults
    auto it = RTX_OPTION_DEFAULTS.find(key);
    if (it != RTX_OPTION_DEFAULTS.end()) {
        return it->second;
    }
    
    // Fallback for unknown keys - use pattern-based guessing as last resort
    if (key.find("enable") != std::string::npos || key.find("Enable") != std::string::npos) {
        return "True";
    }
    if (key.find("Color") != std::string::npos || key.find("Albedo") != std::string::npos) {
        return "1.0, 1.0, 1.0";
    }
    if (key.find("Scale") != std::string::npos || key.find("Factor") != std::string::npos) {
        return "1.0";
    }
    
    return "1.0"; // Ultimate fallback
}

std::string ConfigManager::ExtractDefaultFromPattern(const std::string& line, const std::string& key) const {
    // This method could be enhanced to parse actual RTX_OPTION declarations from rtx_options.h
    // For now, just return the pattern-based default
    return GetDefaultValueFromRtxOptions(key);
}

//=============================================================================
// ResourceManager
//=============================================================================
ResourceManager::ResourceManager(remix::Interface* remixInterface, GarrysMod::Lua::ILuaBase* LUA)
    : m_remixInterface(remixInterface)
    , m_lua(LUA) {
}

ResourceManager::~ResourceManager() {
}

void ResourceManager::ClearResources() {
    if (!m_remixInterface) return;

    // Force a present cycle to clear resources
    remixapi_PresentInfo presentInfo = {};
    presentInfo.sType = REMIXAPI_STRUCT_TYPE_PRESENT_INFO;
    presentInfo.pNext = nullptr;
    presentInfo.hwndOverride = nullptr;
    
    m_remixInterface->Present(&presentInfo);
    
    // Clear D3D9 managed resources if available
    if (g_d3dDevice) {
        g_d3dDevice->EvictManagedResources();
    }
}

void ResourceManager::ForceCleanup() {
    if (!m_remixInterface) return;

    // Force cleanup through config
    m_remixInterface->SetConfigVariable("rtx.resourceLimits.forceCleanup", "1");
    
    ClearResources();
    
    // Reset to normal cleanup behavior
    m_remixInterface->SetConfigVariable("rtx.resourceLimits.forceCleanup", "0");
}

void ResourceManager::SetMemoryLimits(size_t maxCacheSize, size_t maxVRAM) {
    if (!m_remixInterface) return;

    m_remixInterface->SetConfigVariable("rtx.resourceLimits.maxCacheSize", std::to_string(maxCacheSize).c_str());
    m_remixInterface->SetConfigVariable("rtx.resourceLimits.maxVRAM", std::to_string(maxVRAM).c_str());
}

//=============================================================================
// Lua Bindings - Implementation will be in separate files
//=============================================================================

// All Lua bindings are implemented in their respective separate .cpp files:
// - material_lua_bindings.cpp
// - config_lua_bindings.cpp  
// - resource_lua_bindings.cpp
// - light_lua_bindings.cpp

void MeshManager::InitializeLuaBindings() {
    // TODO: Implement mesh Lua bindings
}

void CameraManager::InitializeLuaBindings() {
    // TODO: Implement camera Lua bindings
}

void InstanceManager::InitializeLuaBindings() {
    // TODO: Implement instance Lua bindings
}

//=============================================================================
// Legacy Functions for Backwards Compatibility
//=============================================================================
void Initialize(GarrysMod::Lua::ILuaBase* LUA) {
    // Legacy initialization - use the new API
    if (g_remix && LUA) {
        RemixAPI::Instance().Initialize(g_remix, LUA);
    }
}

void ClearRemixResources() {
    RemixAPI::Instance().GetResourceManager().ClearResources();
}

} // namespace RemixAPI

#endif // _WIN64