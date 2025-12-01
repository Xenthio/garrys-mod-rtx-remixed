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

    // Forward declaration
    class MaterialManager;

    // BSP Geometry Manager
    // Handles efficient upload and rendering of static props and displacement geometry
    class BSPGeometryManager {
    public:
        BSPGeometryManager(remix::Interface* remixInterface, GarrysMod::Lua::ILuaBase* LUA, MaterialManager* materialManager);
        ~BSPGeometryManager();

        // Mesh upload from Lua
        // Returns mesh ID on success, 0 on failure
        uint64_t UploadStaticPropMesh(
            const std::string& modelName,
            const std::vector<remixapi_HardcodedVertex>& vertices,
            const std::vector<uint32_t>& indices,
            uint64_t materialId
        );

        uint64_t UploadDisplacementChunk(
            const std::string& chunkName,
            const std::vector<remixapi_HardcodedVertex>& vertices,
            const std::vector<uint32_t>& indices,
            uint64_t materialId
        );

        // Batch mesh upload (more efficient for multiple meshes)
        struct MeshUploadData {
            std::string name;
            std::vector<remixapi_HardcodedVertex> vertices;
            std::vector<uint32_t> indices;
            uint64_t materialId;
        };

        std::vector<uint64_t> UploadMeshBatch(const std::vector<MeshUploadData>& meshes);

        // Instance rendering
        struct InstanceTransform {
            float matrix[3][4];  // Remix transform format
        };

        // Draw a single instance
        bool DrawMeshInstance(
            uint64_t meshId,
            const InstanceTransform& transform,
            uint32_t categoryFlags = 0  // remixapi_InstanceCategoryFlags
        );

        // Batch draw multiple instances (more efficient)
        struct DrawInstanceData {
            uint64_t meshId;
            InstanceTransform transform;
            uint32_t categoryFlags;
        };

        void DrawInstanceBatch(const std::vector<DrawInstanceData>& instances);

        // Map Instance Management (Persistent instances for world geometry)
        void AddMapInstance(uint64_t meshId, const InstanceTransform& transform);
        void DrawMapInstances();
        void ClearMapInstances();

        // Resource management
        bool DestroyMesh(uint64_t meshId);
        void ClearAllMeshes();
        bool HasMesh(uint64_t meshId) const;
        size_t GetMeshCount() const;

        // Statistics
        struct Statistics {
            size_t totalMeshes;
            size_t totalVertices;
            size_t totalIndices;
            size_t staticPropMeshes;
            size_t displacementMeshes;
            size_t instancesDrawnThisFrame;
        };

        Statistics GetStatistics() const;
        void ResetFrameStats();

        // Lua bindings
        void InitializeLuaBindings();

    private:
        struct CachedMesh {
            remixapi_MeshHandle remixHandle;
            uint64_t materialId;
            std::string name;
            size_t vertexCount;
            size_t indexCount;
            bool isDisplacement;
        };

        remix::Interface* m_remixInterface;
        GarrysMod::Lua::ILuaBase* m_lua;
        MaterialManager* m_materialManager;

        mutable std::mutex m_mutex;
        std::unordered_map<uint64_t, CachedMesh> m_meshes;
        uint64_t m_nextMeshId;

        // Statistics
        mutable Statistics m_stats;

        // Persistent map instances
        struct MapInstance {
            uint64_t meshId;
            InstanceTransform transform;
        };
        std::vector<MapInstance> m_mapInstances;

        // Helper functions
        remixapi_MeshHandle CreateRemixMesh(
            const std::vector<remixapi_HardcodedVertex>& vertices,
            const std::vector<uint32_t>& indices,
            remixapi_MaterialHandle materialHandle,
            uint64_t hash
        );

        bool ValidateVertexData(const std::vector<remixapi_HardcodedVertex>& vertices) const;
        bool ValidateIndexData(const std::vector<uint32_t>& indices, size_t vertexCount) const;

        void UpdateStatistics(const CachedMesh& mesh, bool added);
    };

} // namespace RemixAPI

#endif // _WIN64
