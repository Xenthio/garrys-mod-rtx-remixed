#ifdef _WIN64

#pragma once
#include "GarrysMod/Lua/Interface.h"

#include <Windows.h>
#include <d3d9.h>

#include <remix/remix.h>
#include <remix/remix_c.h>

#include <unordered_map>
#include <memory>
#include <string>
#include <vector>
#include <mutex>

namespace RemixAPI {
    // Forward declarations
    class MaterialManager;
    class MeshManager;
    class CameraManager;
    class InstanceManager;
    class ConfigManager;
    class ResourceManager;
    class LightManager;

    // Main RemixAPI class
    class RemixAPI {
    public:
        static RemixAPI& Instance();
        
        // Core initialization
        bool Initialize(remix::Interface* remixInterface, GarrysMod::Lua::ILuaBase* LUA);
        void Shutdown();
        
        // Manager access
        MaterialManager& GetMaterialManager() { return *m_materialManager; }
        MeshManager& GetMeshManager() { return *m_meshManager; }
        CameraManager& GetCameraManager() { return *m_cameraManager; }
        InstanceManager& GetInstanceManager() { return *m_instanceManager; }
        ConfigManager& GetConfigManager() { return *m_configManager; }
        ResourceManager& GetResourceManager() { return *m_resourceManager; }
        LightManager& GetLightManager() { return *m_lightManager; }
        
        // Direct interface access
        remix::Interface* GetRemixInterface() { return m_remixInterface; }
        
        // Frame management
        void BeginFrame();
        void EndFrame();
        void Present();
        
    private:
        RemixAPI();
        ~RemixAPI();
        
        remix::Interface* m_remixInterface;
        GarrysMod::Lua::ILuaBase* m_lua;
        
        std::unique_ptr<MaterialManager> m_materialManager;
        std::unique_ptr<MeshManager> m_meshManager;
        std::unique_ptr<CameraManager> m_cameraManager;
        std::unique_ptr<InstanceManager> m_instanceManager;
        std::unique_ptr<ConfigManager> m_configManager;
        std::unique_ptr<ResourceManager> m_resourceManager;
        std::unique_ptr<LightManager> m_lightManager;
        
        bool m_initialized;
    };

    // Light Management
    class LightManager {
    public:
        LightManager(remix::Interface* remixInterface, GarrysMod::Lua::ILuaBase* LUA);
        ~LightManager();

        // Light creation
        uint64_t CreateSphereLight(const remix::LightInfo& base, const remix::LightInfoSphereEXT& ext, uint64_t entityId);
        uint64_t CreateRectLight(const remix::LightInfo& base, const remix::LightInfoRectEXT& ext, uint64_t entityId);
        uint64_t CreateDiskLight(const remix::LightInfo& base, const remix::LightInfoDiskEXT& ext, uint64_t entityId);
        uint64_t CreateDistantLight(const remix::LightInfo& base, const remix::LightInfoDistantEXT& ext, uint64_t entityId);

        // Update existing light definition (hash preserved)
        bool UpdateSphereLight(uint64_t lightId, const remix::LightInfo& base, const remix::LightInfoSphereEXT& ext);
        bool UpdateRectLight(uint64_t lightId, const remix::LightInfo& base, const remix::LightInfoRectEXT& ext);
        bool UpdateDiskLight(uint64_t lightId, const remix::LightInfo& base, const remix::LightInfoDiskEXT& ext);
        bool UpdateDistantLight(uint64_t lightId, const remix::LightInfo& base, const remix::LightInfoDistantEXT& ext);

        // Lifecycle
        bool DestroyLight(uint64_t lightId);
        bool HasLight(uint64_t lightId) const;
        bool HasLightForEntity(uint64_t entityId) const;
        void DestroyLightsForEntity(uint64_t entityId);
        void ClearAllLights();
        size_t GetLightCount() const;

        // Per-frame submission
        void SubmitLightsForCurrentFrame();

        // Lua bindings
        void InitializeLuaBindings();

    private:
        struct ManagedLight {
            remixapi_LightHandle handle { nullptr };
            uint64_t entityId { 0 };
        };

        remix::Interface* m_remixInterface;
        GarrysMod::Lua::ILuaBase* m_lua;
        std::mutex m_mutex;
        std::unordered_map<uint64_t, ManagedLight> m_lights; // lightId -> data
        std::unordered_multimap<uint64_t, uint64_t> m_entityToLight; // entityId -> lightId
        uint64_t m_nextLightId { 1 };
        // No per-frame queue needed with internal auto-instancing
    };

    // Material Management
    class MaterialManager {
    public:
        MaterialManager(remix::Interface* remixInterface, GarrysMod::Lua::ILuaBase* LUA);
        ~MaterialManager();
        
        // Material creation and management
        uint64_t CreateMaterial(const std::string& name, const remix::MaterialInfo& info);
        uint64_t CreateOpaqueMaterial(const std::string& name, const remix::MaterialInfo& info, const remix::MaterialInfoOpaqueEXT& opaqueInfo);
        uint64_t CreateTranslucentMaterial(const std::string& name, const remix::MaterialInfo& info, const remix::MaterialInfoTranslucentEXT& translucentInfo);
        
        bool UpdateMaterial(uint64_t materialId, const remix::MaterialInfo& info);
        bool DestroyMaterial(uint64_t materialId);
        bool HasMaterial(uint64_t materialId) const;
        
        // Lua bindings
        void InitializeLuaBindings();
        
    private:
        struct ManagedMaterial {
            remixapi_MaterialHandle handle;
            std::string name;
            remix::MaterialInfo info;
        };
        
        remix::Interface* m_remixInterface;
        GarrysMod::Lua::ILuaBase* m_lua;
        std::unordered_map<uint64_t, ManagedMaterial> m_materials;
        uint64_t m_nextMaterialId;
    };

    // Mesh Management
    class MeshManager {
    public:
        MeshManager(remix::Interface* remixInterface, GarrysMod::Lua::ILuaBase* LUA);
        ~MeshManager();
        
        // Mesh creation and management
        uint64_t CreateMesh(const std::string& name, const remix::MeshInfo& info);
        bool UpdateMesh(uint64_t meshId, const remix::MeshInfo& info);
        bool DestroyMesh(uint64_t meshId);
        bool HasMesh(uint64_t meshId) const;
        
        // Lua bindings
        void InitializeLuaBindings();
        
    private:
        struct ManagedMesh {
            remixapi_MeshHandle handle;
            std::string name;
            remix::MeshInfo info;
        };
        
        remix::Interface* m_remixInterface;
        GarrysMod::Lua::ILuaBase* m_lua;
        std::unordered_map<uint64_t, ManagedMesh> m_meshes;
        uint64_t m_nextMeshId;
    };

    // Camera Management
    class CameraManager {
    public:
        CameraManager(remix::Interface* remixInterface, GarrysMod::Lua::ILuaBase* LUA);
        ~CameraManager();
        
        // Camera control
        bool SetupCamera(const remix::CameraInfo& info);
        bool SetupParameterizedCamera(const remix::CameraInfoParameterizedEXT& info);
        
        // Lua bindings
        void InitializeLuaBindings();
        
    private:
        remix::Interface* m_remixInterface;
        GarrysMod::Lua::ILuaBase* m_lua;
    };

    // Instance Management
    class InstanceManager {
    public:
        InstanceManager(remix::Interface* remixInterface, GarrysMod::Lua::ILuaBase* LUA);
        ~InstanceManager();
        
        // Instance drawing
        bool DrawInstance(const remix::InstanceInfo& info);
        bool DrawInstanceWithBlend(const remix::InstanceInfo& info, const remix::InstanceInfoBlendEXT& blendInfo);
        bool DrawInstanceWithBones(const remix::InstanceInfo& info, const remix::InstanceInfoBoneTransformsEXT& boneInfo);
        
        // Lua bindings
        void InitializeLuaBindings();
        
    private:
        remix::Interface* m_remixInterface;
        GarrysMod::Lua::ILuaBase* m_lua;
    };

    // Configuration Management
    class ConfigManager {
    public:
        ConfigManager(remix::Interface* remixInterface, GarrysMod::Lua::ILuaBase* LUA);
        ~ConfigManager();
        
        // Config variables
        bool SetConfigVariable(const std::string& key, const std::string& value);
        std::string GetConfigVariable(const std::string& key);
        
        // UI State
        remix::UIState GetUIState();
        bool SetUIState(remix::UIState state);
        
        // Lua bindings
        void InitializeLuaBindings();
        
    private:
        remix::Interface* m_remixInterface;
        GarrysMod::Lua::ILuaBase* m_lua;
        
        // Config file parsing
        std::string FindGameDirectory() const;
        std::string FindRtxConfPath() const;
        std::unordered_map<std::string, std::string> ParseConfigFile(const std::string& filePath) const;
        std::string GetDefaultValueFromRtxOptions(const std::string& key) const;
        std::string ExtractDefaultFromPattern(const std::string& line, const std::string& key) const;
    };

    // Resource Management
    class ResourceManager {
    public:
        ResourceManager(remix::Interface* remixInterface, GarrysMod::Lua::ILuaBase* LUA);
        ~ResourceManager();
        
        // Resource cleanup
        void ClearResources();
        void ForceCleanup();
        
        // Memory management
        void SetMemoryLimits(size_t maxCacheSize, size_t maxVRAM);
        
        // Lua bindings
        void InitializeLuaBindings();
        
    private:
        remix::Interface* m_remixInterface;
        GarrysMod::Lua::ILuaBase* m_lua;
    };

    // Legacy functions for backwards compatibility
    void Initialize(GarrysMod::Lua::ILuaBase* LUA);
    void ClearRemixResources();
}

#endif