#ifdef _WIN64
#include "remixapi.h"
#include "bsp_geometry_manager.h"
#include "rtx_option_defaults.h"
#include "material_pipeline/material_pipeline.h"
#include <Windows.h>
#include <remix/remix_c.h>
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
// Resolve optional Remix C API extension at runtime to avoid compile-time dependency on wrapper additions
static PFN_remixapi_AutoInstancePersistentLights s_pfnAutoInstancePersistentLights = nullptr;
static PFN_remixapi_CreatePersistentInstance s_pfnCreatePersistentInstance = nullptr;
static PFN_remixapi_UpdatePersistentInstance s_pfnUpdatePersistentInstance = nullptr;
static PFN_remixapi_DestroyPersistentInstance s_pfnDestroyPersistentInstance = nullptr;

static void EnsureRemixCApiResolved() {
    static bool resolved = false;
    if (resolved)
        return;
    
    HMODULE hRemix = GetModuleHandleA("d3d9.dll");
    if (hRemix) {
        s_pfnAutoInstancePersistentLights = reinterpret_cast<PFN_remixapi_AutoInstancePersistentLights>(
            GetProcAddress(hRemix, "remixapi_AutoInstancePersistentLights"));
        s_pfnCreatePersistentInstance = reinterpret_cast<PFN_remixapi_CreatePersistentInstance>(
            GetProcAddress(hRemix, "remixapi_CreatePersistentInstance"));
        s_pfnUpdatePersistentInstance = reinterpret_cast<PFN_remixapi_UpdatePersistentInstance>(
            GetProcAddress(hRemix, "remixapi_UpdatePersistentInstance"));
        s_pfnDestroyPersistentInstance = reinterpret_cast<PFN_remixapi_DestroyPersistentInstance>(
            GetProcAddress(hRemix, "remixapi_DestroyPersistentInstance"));
        resolved = true;
    }
}

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
#ifdef _DEBUG
        Msg("[RemixAPI] Already initialized\n");
#endif
        return false;
    }

    if (!remixInterface || !LUA) {
        Warning("[RemixAPI] Invalid parameters for initialization\n");
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
        m_bspGeometryManager = std::make_unique<BSPGeometryManager>(remixInterface, LUA, m_materialManager.get());
        m_atmosphereManager = std::make_unique<AtmosphereManager>(remixInterface, LUA);

            // Initialize Lua bindings for all managers
        m_materialManager->InitializeLuaBindings();
        m_meshManager->InitializeLuaBindings();
        m_cameraManager->InitializeLuaBindings();
        m_instanceManager->InitializeLuaBindings();
        m_configManager->InitializeLuaBindings();
        m_resourceManager->InitializeLuaBindings();
        m_lightManager->InitializeLuaBindings();
        m_bspGeometryManager->InitializeLuaBindings();
        m_atmosphereManager->InitializeLuaBindings();
        
        // Initialize the unified material pipeline and its Lua bindings
        MaterialPipeline::Pipeline::Initialize(remixInterface, LUA);

    m_initialized = true;
#ifdef _DEBUG
    Msg("[RemixAPI] Initialization complete\n");
#endif
    return true;
}

void RemixAPI::Shutdown() {
    if (!m_initialized) return;

    // Shutdown unified material pipeline first
    MaterialPipeline::Pipeline::Shutdown();
    
    m_atmosphereManager.reset();
    m_bspGeometryManager.reset();
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
    
#ifdef _DEBUG
    Msg("[RemixAPI] Shutdown complete\n");
#endif
}

void RemixAPI::Present() {
    if (!m_initialized || !m_remixInterface) return;
    
    // Ensure external API lights are auto-instanced and pending updates are flushed once per frame
    if (m_lightManager) {
        m_lightManager->SubmitLightsForCurrentFrame();
    }

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
    
    // FIX: Pre-create the ManagedLight with cached data to ensure stable memory
    uint64_t id = m_nextLightId++;
    ManagedLight ml;
    ml.entityId = entityId;
    ml.isSphere = true;
    ml.cachedBase = base;
    ml.cachedBase.pNext = nullptr;
    ml.cachedSphere = ext;
    
    // Point to the CACHED data (stable memory address)
    remix::LightInfo info = ml.cachedBase;
    info.pNext = &ml.cachedSphere;
    
    // Use batched API for safer light creation
    auto created = m_remixInterface->CreateLightBatched(info);
        
    if (!created) {
        Warning("[LightManager] Failed to create sphere light: %d\n", created.status());
        return 0;
    }
    
    remixapi_LightHandle handle = created.value();
    ml.handle = handle;
    
    m_lights.emplace(id, std::move(ml));
    if (entityId) m_entityToLight.emplace(entityId, id);
    m_activeLightHandles.insert(handle);
#ifdef _DEBUG
    Msg("[LightManager] Created light ID %llu, handle %p. Active handles: %zu\n", id, handle, m_activeLightHandles.size());
#endif
    return id;
}

uint64_t LightManager::CreateRectLight(const remix::LightInfo& base, const remix::LightInfoRectEXT& ext, uint64_t entityId) {
    if (!m_remixInterface) return 0;
    
    std::lock_guard<std::mutex> guard(m_mutex);
    
    // FIX: Pre-create the ManagedLight with cached data to ensure stable memory
    uint64_t id = m_nextLightId++;
    ManagedLight ml;
    ml.entityId = entityId;
    ml.isSphere = false;
    ml.cachedBase = base;
    ml.cachedBase.pNext = nullptr;
    ml.cachedRect = ext;
    
    // Point to the CACHED data (stable memory address)
    remix::LightInfo info = ml.cachedBase;
    info.pNext = &ml.cachedRect;
    
    // Use batched API for safer light creation
    auto created = m_remixInterface->CreateLightBatched(info);
        
    if (!created) {
        Warning("[LightManager] Failed to create rect light: %d\n", created.status());
        return 0;
    }
    
    remixapi_LightHandle handle = created.value();
    ml.handle = handle;
    
    m_lights.emplace(id, std::move(ml));
    if (entityId) m_entityToLight.emplace(entityId, id);
    m_activeLightHandles.insert(handle);
#ifdef _DEBUG
    Msg("[LightManager] Created light ID %llu, handle %p. Active handles: %zu\n", id, handle, m_activeLightHandles.size());
#endif
    return id;
}

uint64_t LightManager::CreateDiskLight(const remix::LightInfo& base, const remix::LightInfoDiskEXT& ext, uint64_t entityId) {
    if (!m_remixInterface) return 0;
    
    std::lock_guard<std::mutex> guard(m_mutex);
    
    // FIX: Pre-create the ManagedLight with cached data to ensure stable memory
    uint64_t id = m_nextLightId++;
    ManagedLight ml;
    ml.entityId = entityId;
    ml.isSphere = false;
    ml.cachedBase = base;
    ml.cachedBase.pNext = nullptr;
    ml.cachedDisk = ext;
    
    // Point to the CACHED data (stable memory address)
    remix::LightInfo info = ml.cachedBase;
    info.pNext = &ml.cachedDisk;
    
    // Use batched API for safer light creation
    auto created = m_remixInterface->CreateLightBatched(info);
        
    if (!created) {
        Warning("[LightManager] Failed to create disk light: %d\n", created.status());
        return 0;
    }
    
    remixapi_LightHandle handle = created.value();
    ml.handle = handle;
    
    m_lights.emplace(id, std::move(ml));
    if (entityId) m_entityToLight.emplace(entityId, id);
    m_activeLightHandles.insert(handle);
#ifdef _DEBUG
    Msg("[LightManager] Created light ID %llu, handle %p. Active handles: %zu\n", id, handle, m_activeLightHandles.size());
#endif
    return id;
}

uint64_t LightManager::CreateDistantLight(const remix::LightInfo& base, const remix::LightInfoDistantEXT& ext, uint64_t entityId) {
    if (!m_remixInterface) return 0;
    
    std::lock_guard<std::mutex> guard(m_mutex);
    
    // FIX: Pre-create the ManagedLight with cached data to ensure stable memory
    uint64_t id = m_nextLightId++;
    ManagedLight ml;
    ml.entityId = entityId;
    ml.isSphere = false;
    ml.cachedBase = base;
    ml.cachedBase.pNext = nullptr;
    ml.cachedDistant = ext;
    
    // Point to the CACHED data (stable memory address)
    remix::LightInfo info = ml.cachedBase;
    info.pNext = &ml.cachedDistant;
    
    // Use batched API for safer light creation
    auto created = m_remixInterface->CreateLightBatched(info);
        
    if (!created) {
        Warning("[LightManager] Failed to create distant light: %d\n", created.status());
        return 0;
    }
    
    remixapi_LightHandle handle = created.value();
    ml.handle = handle;
    
    m_lights.emplace(id, std::move(ml));
    if (entityId) m_entityToLight.emplace(entityId, id);
    m_activeLightHandles.insert(handle);
#ifdef _DEBUG
    Msg("[LightManager] Created light ID %llu, handle %p. Active handles: %zu\n", id, handle, m_activeLightHandles.size());
#endif
    return id;
}

uint64_t LightManager::CreateCylinderLight(const remix::LightInfo& base, const remix::LightInfoCylinderEXT& ext, uint64_t entityId) {
    if (!m_remixInterface) return 0;
    
    std::lock_guard<std::mutex> guard(m_mutex);
    
    // FIX: Pre-create the ManagedLight with cached data to ensure stable memory
    uint64_t id = m_nextLightId++;
    ManagedLight ml;
    ml.entityId = entityId;
    ml.isSphere = false;
    ml.cachedBase = base;
    ml.cachedBase.pNext = nullptr;
    ml.cachedCylinder = ext;
    
    // Point to the CACHED data (stable memory address)
    remix::LightInfo info = ml.cachedBase;
    info.pNext = &ml.cachedCylinder;
    
    // Use batched API for safer light creation
    auto created = m_remixInterface->CreateLightBatched(info);
        
    if (!created) {
        Warning("[LightManager] Failed to create cylinder light: %d\n", created.status());
        return 0;
    }
    
    remixapi_LightHandle handle = created.value();
    ml.handle = handle;
    
    m_lights.emplace(id, std::move(ml));
    if (entityId) m_entityToLight.emplace(entityId, id);
    m_activeLightHandles.insert(handle);
#ifdef _DEBUG
    Msg("[LightManager] Created light ID %llu, handle %p. Active handles: %zu\n", id, handle, m_activeLightHandles.size());
#endif
    return id;
}

uint64_t LightManager::CreateDomeLight(const remix::LightInfo& base, const remix::LightInfoDomeEXT& ext, uint64_t entityId) {
    if (!m_remixInterface) return 0;
    
    std::lock_guard<std::mutex> guard(m_mutex);
    
    // FIX: Pre-create the ManagedLight with cached data to ensure stable memory
    uint64_t id = m_nextLightId++;
    ManagedLight ml;
    ml.entityId = entityId;
    ml.isSphere = false;
    ml.cachedBase = base;
    ml.cachedBase.pNext = nullptr;
    ml.cachedDome = ext;
    
    // Point to the CACHED data (stable memory address)
    remix::LightInfo info = ml.cachedBase;
    info.pNext = &ml.cachedDome;
    
    // Use batched API for safer light creation
    auto created = m_remixInterface->CreateLightBatched(info);
        
    if (!created) {
        Warning("[LightManager] Failed to create dome light: %d\n", created.status());
        return 0;
    }
    
    remixapi_LightHandle handle = created.value();
    ml.handle = handle;
    
    m_lights.emplace(id, std::move(ml));
    if (entityId) m_entityToLight.emplace(entityId, id);
    m_activeLightHandles.insert(handle);
#ifdef _DEBUG
    Msg("[LightManager] Created light ID %llu, handle %p. Active handles: %zu\n", id, handle, m_activeLightHandles.size());
#endif
    return id;
}

bool LightManager::DestroyLight(uint64_t lightId) {
    remixapi_LightHandle handleToDestroy = nullptr;
    uint64_t entityId = 0;
    // Copy of cached state for optional zero-radiance update before destroy
    bool wasSphere = false;
    remix::LightInfo cachedBaseCopy{};
    remix::LightInfoSphereEXT cachedSphereCopy{};
    
    {
        std::lock_guard<std::mutex> guard(m_mutex);
        auto it = m_lights.find(lightId);
        if (it == m_lights.end()) {
            // Light not found - might have already been destroyed
#ifdef _DEBUG
            Msg("[LightManager] Warning: Attempted to destroy non-existent light ID %llu\n", lightId);
#endif
            return false;
        }
        
        handleToDestroy = it->second.handle;
        entityId = it->second.entityId;
        // Stash cached state for zero-radiance update outside the lock
        wasSphere = it->second.isSphere;
        cachedBaseCopy = it->second.cachedBase; cachedBaseCopy.pNext = nullptr;
        if (wasSphere) cachedSphereCopy = it->second.cachedSphere;
        
        // remove from entity map while holding the lock
        if (it->second.entityId) {
            auto range = m_entityToLight.equal_range(it->second.entityId);
            for (auto r = range.first; r != range.second; ) {
                if (r->second == lightId) r = m_entityToLight.erase(r); 
                else ++r;
            }
        }
        m_lights.erase(it);
    }
    
    // Optional: make the light invisible immediately to avoid one-frame persistence
    if (handleToDestroy && wasSphere) {
        remix::LightInfo zeroInfo = cachedBaseCopy;
        zeroInfo.radiance = { 0.0f, 0.0f, 0.0f };
        zeroInfo.pNext = const_cast<remix::LightInfoSphereEXT*>(&cachedSphereCopy);
        auto ok = m_remixInterface->UpdateLightDefinition(handleToDestroy, zeroInfo);
#ifdef _DEBUG
        if (!ok) {
            Msg("[LightManager] Zero-radiance pre-destroy update failed for handle %p\n", handleToDestroy);
        } else {
            Msg("[LightManager] Zeroed radiance before destroy for handle %p\n", handleToDestroy);
        }
#endif
    }
    
    // Destroy the light handle outside of the mutex lock
    // The batched API will handle persistent light unregistration internally
    if (handleToDestroy) {
        size_t handlesBefore, handlesAfter;
        {
            std::lock_guard<std::mutex> guard(m_mutex);
            handlesBefore = m_activeLightHandles.size();
        }
        
#ifdef _DEBUG
        Msg("[LightManager] Destroying light ID %llu, handle %p. Active handles before: %zu\n", lightId, handleToDestroy, handlesBefore);
#endif
        m_remixInterface->DestroyLight(handleToDestroy);
        
        {
            std::lock_guard<std::mutex> guard(m_mutex);
            m_activeLightHandles.erase(handleToDestroy); // Ensure it's removed from active list
            handlesAfter = m_activeLightHandles.size();
        }
        
#ifdef _DEBUG
        Msg("[LightManager] Destroyed light ID %llu, handle %p. Active handles after: %zu\n", lightId, handleToDestroy, handlesAfter);
#endif
    }
    
    return true;
}

bool LightManager::UpdateSphereLight(uint64_t lightId, const remix::LightInfo& base, const remix::LightInfoSphereEXT& ext) {
    std::lock_guard<std::mutex> guard(m_mutex);
    auto it = m_lights.find(lightId);
    if (it == m_lights.end() || !it->second.handle) return false;
    
    // FIX: Store data in cached storage FIRST to ensure pNext points to stable memory
    it->second.cachedBase = base; 
    it->second.cachedBase.pNext = nullptr;
    it->second.cachedSphere = ext;
    it->second.isSphere = true;
    
    // Point to the CACHED data (stable memory address)
    remix::LightInfo info = it->second.cachedBase;
    info.pNext = &it->second.cachedSphere;
    
    auto ok = m_remixInterface->UpdateLightDefinition(it->second.handle, info);
    return ok;
}

bool LightManager::UpdateRectLight(uint64_t lightId, const remix::LightInfo& base, const remix::LightInfoRectEXT& ext) {
    std::lock_guard<std::mutex> guard(m_mutex);
    auto it = m_lights.find(lightId);
    if (it == m_lights.end() || !it->second.handle) return false;
    
    // FIX: Store data in cached storage FIRST to ensure pNext points to stable memory
    it->second.cachedBase = base;
    it->second.cachedBase.pNext = nullptr;
    it->second.cachedRect = ext;
    
    // Point to the CACHED data (stable memory address)
    remix::LightInfo info = it->second.cachedBase;
    info.pNext = &it->second.cachedRect;
    
    return m_remixInterface->UpdateLightDefinition(it->second.handle, info);
}

bool LightManager::UpdateDiskLight(uint64_t lightId, const remix::LightInfo& base, const remix::LightInfoDiskEXT& ext) {
    std::lock_guard<std::mutex> guard(m_mutex);
    auto it = m_lights.find(lightId);
    if (it == m_lights.end() || !it->second.handle) return false;
    
    // FIX: Store data in cached storage FIRST to ensure pNext points to stable memory
    it->second.cachedBase = base;
    it->second.cachedBase.pNext = nullptr;
    it->second.cachedDisk = ext;
    
    // Point to the CACHED data (stable memory address)
    remix::LightInfo info = it->second.cachedBase;
    info.pNext = &it->second.cachedDisk;
    
    return m_remixInterface->UpdateLightDefinition(it->second.handle, info);
}

bool LightManager::UpdateDistantLight(uint64_t lightId, const remix::LightInfo& base, const remix::LightInfoDistantEXT& ext) {
    std::lock_guard<std::mutex> guard(m_mutex);
    auto it = m_lights.find(lightId);
    if (it == m_lights.end() || !it->second.handle) return false;
    
    // FIX: Store data in cached storage FIRST to ensure pNext points to stable memory
    it->second.cachedBase = base;
    it->second.cachedBase.pNext = nullptr;
    it->second.cachedDistant = ext;
    
    // Point to the CACHED data (stable memory address)
    remix::LightInfo info = it->second.cachedBase;
    info.pNext = &it->second.cachedDistant;
    
    return m_remixInterface->UpdateLightDefinition(it->second.handle, info);
}

bool LightManager::UpdateCylinderLight(uint64_t lightId, const remix::LightInfo& base, const remix::LightInfoCylinderEXT& ext) {
    std::lock_guard<std::mutex> guard(m_mutex);
    auto it = m_lights.find(lightId);
    if (it == m_lights.end() || !it->second.handle) return false;
    
    // FIX: Store data in cached storage FIRST to ensure pNext points to stable memory
    it->second.cachedBase = base;
    it->second.cachedBase.pNext = nullptr;
    it->second.cachedCylinder = ext;
    
    // Point to the CACHED data (stable memory address)
    remix::LightInfo info = it->second.cachedBase;
    info.pNext = &it->second.cachedCylinder;
    
    return m_remixInterface->UpdateLightDefinition(it->second.handle, info);
}

bool LightManager::UpdateDomeLight(uint64_t lightId, const remix::LightInfo& base, const remix::LightInfoDomeEXT& ext) {
    std::lock_guard<std::mutex> guard(m_mutex);
    auto it = m_lights.find(lightId);
    if (it == m_lights.end() || !it->second.handle) return false;
    
    // FIX: Store data in cached storage FIRST to ensure pNext points to stable memory
    it->second.cachedBase = base;
    it->second.cachedBase.pNext = nullptr;
    it->second.cachedDome = ext;
    
    // Point to the CACHED data (stable memory address)
    remix::LightInfo info = it->second.cachedBase;
    info.pNext = &it->second.cachedDome;
    
    return m_remixInterface->UpdateLightDefinition(it->second.handle, info);
}
bool LightManager::HasLight(uint64_t lightId) const {
    std::lock_guard<std::mutex> guard(m_mutex);
    return m_lights.find(lightId) != m_lights.end();
}

bool LightManager::HasLightForEntity(uint64_t entityId) const {
    std::lock_guard<std::mutex> guard(m_mutex);
    return m_entityToLight.find(entityId) != m_entityToLight.end();
}

std::vector<uint64_t> LightManager::GetLightsForEntity(uint64_t entityId) const {
    std::lock_guard<std::mutex> guard(m_mutex);
    std::vector<uint64_t> out;
    auto range = m_entityToLight.equal_range(entityId);
    for (auto it = range.first; it != range.second; ++it) out.push_back(it->second);
    return out;
}

std::vector<uint64_t> LightManager::GetAllLightIds() const {
    std::lock_guard<std::mutex> guard(m_mutex);
    std::vector<uint64_t> out; out.reserve(m_lights.size());
    for (const auto& kv : m_lights) out.push_back(kv.first);
    return out;
}

bool LightManager::GetSphereState(uint64_t lightId, remix::LightInfo& outBase, remix::LightInfoSphereEXT& outSphere) const {
    std::lock_guard<std::mutex> guard(m_mutex);
    auto it = m_lights.find(lightId);
    if (it == m_lights.end() || !it->second.isSphere) return false;
    outBase = it->second.cachedBase; outBase.pNext = nullptr;
    outSphere = it->second.cachedSphere;
    return true;
}

bool LightManager::ApplySphereState(uint64_t lightId, const remix::LightInfo& base, const remix::LightInfoSphereEXT& sphere) {
    return UpdateSphereLight(lightId, base, sphere);
}

void LightManager::DestroyLightsForEntity(uint64_t entityId) {
    std::vector<uint64_t> lightsToDestroy;
    
    // Collect light IDs while holding the lock
    {
        std::lock_guard<std::mutex> guard(m_mutex);
        auto range = m_entityToLight.equal_range(entityId);
        for (auto it = range.first; it != range.second; ++it) {
            lightsToDestroy.push_back(it->second);
        }
    }
    
    // Destroy lights without holding the lock to avoid deadlock
    for (uint64_t lightId : lightsToDestroy) {
        DestroyLight(lightId);
    }
}

void LightManager::ClearAllLights() {
    std::vector<remixapi_LightHandle> handlesToDestroy;
    
    {
        std::lock_guard<std::mutex> guard(m_mutex);
        
        // Collect all handles to destroy
        for (auto& [id, light] : m_lights) {
            if (light.handle) {
                handlesToDestroy.push_back(light.handle);
            }
        }
        
        // Clear the maps while holding the lock
        m_lights.clear();
        m_entityToLight.clear();
    }
    
    // Destroy all light handles outside of the mutex lock
    // The batched API will handle persistent light unregistration internally
    for (auto handle : handlesToDestroy) {
        m_remixInterface->DestroyLight(handle);
    }
}

size_t LightManager::GetLightCount() const {
    std::lock_guard<std::mutex> guard(m_mutex);
    return m_lights.size();
}

void LightManager::SubmitLightsForCurrentFrame() {
    if (!m_remixInterface) return;
    
    // Copy the active handles under lock to avoid holding mutex during submission
    std::unordered_set<remixapi_LightHandle> handlesToSubmit;
    {
        std::lock_guard<std::mutex> guard(m_mutex);
        handlesToSubmit = m_activeLightHandles;
    }
    
    // Always clear and resubmit the active lights each frame
    // This ensures that destroyed lights are removed and new/updated lights are included
#ifdef _DEBUG
    Msg("[LightManager] Submitting %zu active light handles this frame.\n", handlesToSubmit.size());
#endif
    for (const auto& handle : handlesToSubmit) {
        if (handle) {
            // Msg("  - Submitting handle %p\n", handle); // uncomment for very verbose logging
            m_remixInterface->DrawLightInstance(handle);
        }
    }
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

#ifdef _DEBUG
    Msg("[MaterialManager] Creating material '%s'\n", name.c_str());
    Msg("  hash: %llu\n", info.hash);
    Msg("  pNext: %p\n", info.pNext);
    Msg("  albedoTexture: %ls\n", info.albedoTexture ? info.albedoTexture : L"<null>");
#endif

    auto result = m_remixInterface->CreateMaterial(info);
    if (!result) {
        Warning("[MaterialManager] Failed to create material '%s': %d\n", name.c_str(), result.status());
        Warning("  This usually means invalid material parameters\n");
        Warning("  Check that texture paths are valid or use empty paths\n");
        return 0;
    }

    uint64_t materialId = m_nextMaterialId++;
    ManagedMaterial material = {
        result.value(),
        name,
        info
    };
    
    m_materials[materialId] = material;
#ifdef _DEBUG
    Msg("[MaterialManager] Created material '%s' with ID %llu\n", name.c_str(), materialId);
#endif
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

uint64_t MaterialManager::CreatePortalMaterial(const std::string& name, const remix::MaterialInfo& info,
                                               const remix::MaterialInfoPortalEXT& portalInfo) {
    if (!m_remixInterface) return 0;

    remix::MaterialInfo materialInfo = info;
    materialInfo.pNext = const_cast<remix::MaterialInfoPortalEXT*>(&portalInfo);

    return CreateMaterial(name, materialInfo);
}

bool MaterialManager::UpdateMaterial(uint64_t materialId, const remix::MaterialInfo& info) {
    auto it = m_materials.find(materialId);
    if (it == m_materials.end()) {
        Warning("[MaterialManager] Material ID %llu not found\n", materialId);
        return false;
    }

    // For now, we need to recreate the material
    // TODO: Check if Remix API supports material updates
    auto oldHandle = it->second.handle;
    auto result = m_remixInterface->CreateMaterial(info);
    if (!result) {
        Warning("[MaterialManager] Failed to update material ID %llu: %d\n", materialId, result.status());
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
        Warning("[MaterialManager] Material ID %llu not found\n", materialId);
        return false;
    }

    if (it->second.handle) {
        m_remixInterface->DestroyMaterial(it->second.handle);
    }
    
    m_materials.erase(it);
#ifdef _DEBUG
    Msg("[MaterialManager] Destroyed material ID %llu\n", materialId);
#endif
    return true;
}

bool MaterialManager::HasMaterial(uint64_t materialId) const {
    return m_materials.find(materialId) != m_materials.end();
}

remixapi_MaterialHandle MaterialManager::GetMaterialHandle(uint64_t materialId) const {
    auto it = m_materials.find(materialId);
    if (it != m_materials.end()) {
        return it->second.handle;
    }
    return nullptr;
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
        Warning("[MeshManager] Failed to create mesh '%s': %d\n", name.c_str(), result.status());
        return 0;
    }

    uint64_t meshId = m_nextMeshId++;
    ManagedMesh mesh = {
        result.value(),
        name,
        info
    };
    
    m_meshes[meshId] = mesh;
#ifdef _DEBUG
    Msg("[MeshManager] Created mesh '%s' with ID %llu\n", name.c_str(), meshId);
#endif
    return meshId;
}

bool MeshManager::UpdateMesh(uint64_t meshId, const remix::MeshInfo& info) {
    auto it = m_meshes.find(meshId);
    if (it == m_meshes.end()) {
        Warning("[MeshManager] Mesh ID %llu not found\n", meshId);
        return false;
    }

    auto oldHandle = it->second.handle;
    auto result = m_remixInterface->CreateMesh(info);
    if (!result) {
        Warning("[MeshManager] Failed to update mesh ID %llu: %d\n", meshId, result.status());
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
        Warning("[MeshManager] Mesh ID %llu not found\n", meshId);
        return false;
    }

    if (it->second.handle) {
        m_remixInterface->DestroyMesh(it->second.handle);
    }
    
    m_meshes.erase(it);
#ifdef _DEBUG
    Msg("[MeshManager] Destroyed mesh ID %llu\n", meshId);
#endif
    return true;
}

bool MeshManager::HasMesh(uint64_t meshId) const {
    return m_meshes.find(meshId) != m_meshes.end();
}

remixapi_MeshHandle MeshManager::GetMeshHandle(uint64_t meshId) const {
    auto it = m_meshes.find(meshId);
    if (it != m_meshes.end()) {
        return it->second.handle;
    }
    return nullptr;
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
        Warning("[CameraManager] Failed to setup camera: %d\n", result.status());
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
        Warning("[InstanceManager] Failed to draw instance: %d\n", result.status());
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

bool InstanceManager::SupportsPersistentInstances() {
    EnsureRemixCApiResolved();
    return s_pfnCreatePersistentInstance &&
           s_pfnUpdatePersistentInstance &&
           s_pfnDestroyPersistentInstance;
}

bool InstanceManager::CreatePersistentInstance(
    const remix::InstanceInfo& info,
    remixapi_PersistentInstanceHandle& outHandle) {
    outHandle = 0;
    if (!m_remixInterface || !SupportsPersistentInstances()) return false;

    const remixapi_ErrorCode status = s_pfnCreatePersistentInstance(&info, &outHandle);
    if (status != REMIXAPI_ERROR_CODE_SUCCESS) {
        Warning("[InstanceManager] Failed to create persistent instance: %d\n", status);
        outHandle = 0;
        return false;
    }
    return true;
}

bool InstanceManager::UpdatePersistentInstance(
    remixapi_PersistentInstanceHandle handle,
    const remix::InstanceInfo& info) {
    if (!m_remixInterface || !SupportsPersistentInstances() || handle == 0) return false;

    const remixapi_ErrorCode status = s_pfnUpdatePersistentInstance(handle, &info);
    if (status != REMIXAPI_ERROR_CODE_SUCCESS) {
        Warning("[InstanceManager] Failed to update persistent instance: %d\n", status);
        return false;
    }
    return true;
}

bool InstanceManager::DestroyPersistentInstance(remixapi_PersistentInstanceHandle handle) {
    if (!m_remixInterface || !SupportsPersistentInstances() || handle == 0) return false;

    const remixapi_ErrorCode status = s_pfnDestroyPersistentInstance(handle);
    if (status != REMIXAPI_ERROR_CODE_SUCCESS) {
        Warning("[InstanceManager] Failed to destroy persistent instance: %d\n", status);
        return false;
    }
    return true;
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
        Warning("[ConfigManager] Failed to set config variable '%s': %d\n", key.c_str(), result.status());
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
        Warning("[ConfigManager] Failed to get UI state: %d\n", result.status());
        return remix::UIState::None;
    }
    
    return result.value();
}

bool ConfigManager::SetUIState(remix::UIState state) {
    if (!m_remixInterface) return false;

    auto result = m_remixInterface->SetUIState(state);
    if (!result) {
        Warning("[ConfigManager] Failed to set UI state: %d\n", result.status());
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
#ifdef _DEBUG
        Msg("[ConfigManager] Could not open config file: %s\n", filePath.c_str());
#endif
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
    
#ifdef _DEBUG
    Msg("[ConfigManager] Parsed %zu config entries from %s\n", config.size(), filePath.c_str());
#endif
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
// - mesh_lua_bindings.cpp
// - instance_lua_bindings.cpp
// - bsp_geometry_lua_bindings.cpp
// - atmosphere_lua_bindings.cpp

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
