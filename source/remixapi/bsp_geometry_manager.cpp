#ifdef _WIN64
#include "bsp_geometry_manager.h"
#include "remixapi.h"
#include <tier0/dbg.h>

namespace RemixAPI {

BSPGeometryManager::BSPGeometryManager(remix::Interface* remixInterface, GarrysMod::Lua::ILuaBase* LUA, MaterialManager* materialManager)
    : m_remixInterface(remixInterface)
    , m_lua(LUA)
    , m_materialManager(materialManager)
    , m_nextMeshId(1)
    , m_stats({}) {
}

BSPGeometryManager::~BSPGeometryManager() {
    ClearAllMeshes();
}

// Helper hash function (FNV-1a 64-bit)
static uint64_t HashString64(const std::string& str) {
    uint64_t hash = 0xcbf29ce484222325ULL;
    for (char c : str) {
        hash ^= static_cast<uint64_t>(c);
        hash *= 0x100000001b3ULL;
    }
    return hash;
}

uint64_t BSPGeometryManager::UploadStaticPropMesh(
    const std::string& modelName,
    const std::vector<remixapi_HardcodedVertex>& vertices,
    const std::vector<uint32_t>& indices,
    uint64_t materialId
) {
    if (!m_remixInterface) return 0;

    remixapi_MaterialHandle matHandle = m_materialManager->GetMaterialHandle(materialId);
    if (!matHandle) {
        Warning("[BSPGeometryManager] Invalid material ID %llu for mesh '%s'\n", materialId, modelName.c_str());
        return 0;
    }

    // Generate hash from name
    uint64_t hash = HashString64(modelName);

    remixapi_MeshHandle meshHandle = CreateRemixMesh(vertices, indices, matHandle, hash);
    if (!meshHandle) return 0;

    std::lock_guard<std::mutex> guard(m_mutex);
    uint64_t meshId = m_nextMeshId++;
    
    CachedMesh mesh;
    mesh.remixHandle = meshHandle;
    mesh.materialId = materialId;
    mesh.name = modelName;
    mesh.vertexCount = vertices.size();
    mesh.indexCount = indices.size();
    mesh.isDisplacement = false;

    m_meshes[meshId] = mesh;
    UpdateStatistics(mesh, true);

#ifdef _DEBUG
    Msg("[BSPGeometryManager] Uploaded static prop mesh '%s' (ID: %llu, Hash: 0x%llX, %zu verts, %zu indices)\n", 
        modelName.c_str(), meshId, hash, vertices.size(), indices.size());
#endif

    return meshId;
}

uint64_t BSPGeometryManager::UploadDisplacementChunk(
    const std::string& chunkName,
    const std::vector<remixapi_HardcodedVertex>& vertices,
    const std::vector<uint32_t>& indices,
    uint64_t materialId
) {
    if (!m_remixInterface) return 0;

    remixapi_MaterialHandle matHandle = m_materialManager->GetMaterialHandle(materialId);
    if (!matHandle) {
        Warning("[BSPGeometryManager] Invalid material ID %llu for displacement '%s'\n", materialId, chunkName.c_str());
        return 0;
    }

    // Generate hash from name
    uint64_t hash = HashString64(chunkName);

    remixapi_MeshHandle meshHandle = CreateRemixMesh(vertices, indices, matHandle, hash);
    if (!meshHandle) return 0;

    std::lock_guard<std::mutex> guard(m_mutex);
    uint64_t meshId = m_nextMeshId++;
    
    CachedMesh mesh;
    mesh.remixHandle = meshHandle;
    mesh.materialId = materialId;
    mesh.name = chunkName;
    mesh.vertexCount = vertices.size();
    mesh.indexCount = indices.size();
    mesh.isDisplacement = true;

    m_meshes[meshId] = mesh;
    UpdateStatistics(mesh, true);

#ifdef _DEBUG
    Msg("[BSPGeometryManager] Uploaded displacement chunk '%s' (ID: %llu, Hash: 0x%llX, %zu verts, %zu indices)\n", 
        chunkName.c_str(), meshId, hash, vertices.size(), indices.size());
#endif

    return meshId;
}

std::vector<uint64_t> BSPGeometryManager::UploadMeshBatch(const std::vector<MeshUploadData>& meshes) {
    std::vector<uint64_t> resultIds;
    resultIds.reserve(meshes.size());

    for (const auto& data : meshes) {
        // Determine type based on name or separate flag? Assuming static prop for now unless name says otherwise
        // Actually, just use UploadStaticPropMesh logic internally
        bool isDisplacement = (data.name.find("displacement") != std::string::npos) || 
                              (data.name.find("chunk") != std::string::npos);
        
        if (isDisplacement) {
            resultIds.push_back(UploadDisplacementChunk(data.name, data.vertices, data.indices, data.materialId));
        } else {
            resultIds.push_back(UploadStaticPropMesh(data.name, data.vertices, data.indices, data.materialId));
        }
    }
    return resultIds;
}

bool BSPGeometryManager::DrawMeshInstance(
    uint64_t meshId,
    const InstanceTransform& transform,
    uint32_t categoryFlags
) {
    if (!m_remixInterface) return false;

    remixapi_MeshHandle meshHandle = nullptr;
    {
        std::lock_guard<std::mutex> guard(m_mutex);
        auto it = m_meshes.find(meshId);
        if (it == m_meshes.end()) return false;
        meshHandle = it->second.remixHandle;
    }

    remixapi_InstanceInfo info = {};
    info.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO;
    info.mesh = meshHandle;
    info.categoryFlags = categoryFlags;
    
    // Copy transform (Remix expects row-major 3x4)
    memcpy(info.transform.matrix, transform.matrix, sizeof(float) * 12);

    auto result = m_remixInterface->DrawInstance(info);
    
    m_stats.instancesDrawnThisFrame++;
    
    return (result.status() == REMIXAPI_ERROR_CODE_SUCCESS);
}

void BSPGeometryManager::DrawInstanceBatch(const std::vector<DrawInstanceData>& instances) {
    for (const auto& inst : instances) {
        DrawMeshInstance(inst.meshId, inst.transform, inst.categoryFlags);
    }
}

void BSPGeometryManager::AddMapInstance(uint64_t meshId, const InstanceTransform& transform) {
    std::lock_guard<std::mutex> guard(m_mutex);
    m_mapInstances.push_back({ meshId, transform });
}

void BSPGeometryManager::DrawMapInstances() {
    // Create a local copy of instances to avoid holding the lock during draw calls?
    // DrawMeshInstance takes the lock briefly for lookup.
    // Iterating m_mapInstances requires lock protection if other threads modify it (which shouldn't happen during draw)
    // But AddMapInstance might be called from main thread while rendering happens?
    // Assuming single-threaded rendering from Lua for now.
    
    std::lock_guard<std::mutex> guard(m_mutex);
    for (const auto& inst : m_mapInstances) {
        // We bypass the public DrawMeshInstance to avoid re-locking for every instance
        auto it = m_meshes.find(inst.meshId);
        if (it != m_meshes.end()) {
            remixapi_InstanceInfo info = {};
            info.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO;
            info.mesh = it->second.remixHandle;
            info.categoryFlags = 0; // Default for map geometry
            memcpy(info.transform.matrix, inst.transform.matrix, sizeof(float) * 12);
            
            m_remixInterface->DrawInstance(info);
            m_stats.instancesDrawnThisFrame++;
        }
    }
}

void BSPGeometryManager::ClearMapInstances() {
    std::lock_guard<std::mutex> guard(m_mutex);
    m_mapInstances.clear();
}

bool BSPGeometryManager::DestroyMesh(uint64_t meshId) {
    std::lock_guard<std::mutex> guard(m_mutex);
    auto it = m_meshes.find(meshId);
    if (it == m_meshes.end()) return false;

    m_remixInterface->DestroyMesh(it->second.remixHandle);
    UpdateStatistics(it->second, false);
    m_meshes.erase(it);
    return true;
}

void BSPGeometryManager::ClearAllMeshes() {
    std::lock_guard<std::mutex> guard(m_mutex);
    for (auto& pair : m_meshes) {
        m_remixInterface->DestroyMesh(pair.second.remixHandle);
    }
    m_meshes.clear();
    m_stats.totalMeshes = 0;
    m_stats.totalVertices = 0;
    m_stats.totalIndices = 0;
    m_stats.staticPropMeshes = 0;
    m_stats.displacementMeshes = 0;
}

bool BSPGeometryManager::HasMesh(uint64_t meshId) const {
    std::lock_guard<std::mutex> guard(m_mutex);
    return m_meshes.find(meshId) != m_meshes.end();
}

size_t BSPGeometryManager::GetMeshCount() const {
    std::lock_guard<std::mutex> guard(m_mutex);
    return m_meshes.size();
}

BSPGeometryManager::Statistics BSPGeometryManager::GetStatistics() const {
    std::lock_guard<std::mutex> guard(m_mutex);
    return m_stats;
}

void BSPGeometryManager::ResetFrameStats() {
    // m_stats.instancesDrawnThisFrame = 0; // Reset this per frame if needed
    // Actually, m_stats is mutable, but this function should be called at start/end of frame
    m_stats.instancesDrawnThisFrame = 0;
}

remixapi_MeshHandle BSPGeometryManager::CreateRemixMesh(
    const std::vector<remixapi_HardcodedVertex>& vertices,
    const std::vector<uint32_t>& indices,
    remixapi_MaterialHandle materialHandle,
    uint64_t hash
) {
    if (!ValidateVertexData(vertices) || !ValidateIndexData(indices, vertices.size())) {
        return nullptr;
    }

    remixapi_MeshInfo info = {};
    info.sType = REMIXAPI_STRUCT_TYPE_MESH_INFO;
    info.hash = hash;
    
    remixapi_MeshInfoSurfaceTriangles surface = {};
    surface.material = materialHandle;
    surface.vertices_count = vertices.size();
    surface.vertices_values = vertices.data();
    surface.indices_count = indices.size();
    surface.indices_values = indices.data();
    surface.skinning_hasvalue = 0;
    
    info.surfaces_count = 1;
    info.surfaces_values = &surface;

    auto result = m_remixInterface->CreateMesh(info);
    if (result.status() != REMIXAPI_ERROR_CODE_SUCCESS) {
        Warning("[BSPGeometryManager] Failed to create Remix mesh: %d\n", result.status());
        return nullptr;
    }

    return result.value();
}

bool BSPGeometryManager::ValidateVertexData(const std::vector<remixapi_HardcodedVertex>& vertices) const {
    if (vertices.empty()) {
        Warning("[BSPGeometryManager] Empty vertex data\n");
        return false;
    }
    return true;
}

bool BSPGeometryManager::ValidateIndexData(const std::vector<uint32_t>& indices, size_t vertexCount) const {
    if (indices.empty()) {
        Warning("[BSPGeometryManager] Empty index data\n");
        return false;
    }
    for (uint32_t idx : indices) {
        if (idx >= vertexCount) {
            Warning("[BSPGeometryManager] Index out of bounds: %u >= %zu\n", idx, vertexCount);
            return false;
        }
    }
    return true;
}

void BSPGeometryManager::UpdateStatistics(const CachedMesh& mesh, bool added) {
    if (added) {
        m_stats.totalMeshes++;
        m_stats.totalVertices += mesh.vertexCount;
        m_stats.totalIndices += mesh.indexCount;
        if (mesh.isDisplacement) m_stats.displacementMeshes++;
        else m_stats.staticPropMeshes++;
    } else {
        m_stats.totalMeshes--;
        m_stats.totalVertices -= mesh.vertexCount;
        m_stats.totalIndices -= mesh.indexCount;
        if (mesh.isDisplacement) m_stats.displacementMeshes--;
        else m_stats.staticPropMeshes--;
    }
}

} // namespace RemixAPI

#endif // _WIN64

