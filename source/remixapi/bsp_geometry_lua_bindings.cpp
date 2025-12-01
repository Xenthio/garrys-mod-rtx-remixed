#ifdef _WIN64
#include "bsp_geometry_manager.h"
#include "remixapi.h"
#include <tier0/dbg.h>

using namespace GarrysMod::Lua;

namespace RemixAPI {

// Helper function to extract vertices from Lua table
static std::vector<remixapi_HardcodedVertex> ExtractVertices(ILuaBase* LUA, int stackPos) {
    std::vector<remixapi_HardcodedVertex> vertices;
    
    if (!LUA->IsType(stackPos, Type::Table)) {
        LUA->ThrowError("Expected table for vertices");
        return vertices;
    }

    size_t vertexCount = LUA->ObjLen(stackPos);
    vertices.reserve(vertexCount);

    for (size_t i = 1; i <= vertexCount; i++) {
        LUA->PushNumber(i);
        LUA->GetTable(stackPos);
        
        if (!LUA->IsType(-1, Type::Table)) {
            LUA->Pop();
            continue;
        }

        remixapi_HardcodedVertex vertex = {};

        // Position
        LUA->GetField(-1, "pos");
        if (LUA->IsType(-1, Type::Vector)) {
            Vector pos = LUA->GetVector(-1);
            vertex.position[0] = pos.x;
            vertex.position[1] = pos.y;
            vertex.position[2] = pos.z;
        }
        LUA->Pop();

        // Normal
        LUA->GetField(-1, "normal");
        if (LUA->IsType(-1, Type::Vector)) {
            Vector normal = LUA->GetVector(-1);
            vertex.normal[0] = normal.x;
            vertex.normal[1] = normal.y;
            vertex.normal[2] = normal.z;
        }
        LUA->Pop();

        // Texcoord
        LUA->GetField(-1, "u");
        vertex.texcoord[0] = LUA->IsType(-1, Type::Number) ? LUA->GetNumber(-1) : 0.0f;
        LUA->Pop();

        LUA->GetField(-1, "v");
        vertex.texcoord[1] = LUA->IsType(-1, Type::Number) ? LUA->GetNumber(-1) : 0.0f;
        LUA->Pop();

        // Color (optional, default to white)
        LUA->GetField(-1, "color");
        if (LUA->IsType(-1, Type::Number)) {
            vertex.color = static_cast<uint32_t>(LUA->GetNumber(-1));
        } else {
            vertex.color = 0xFFFFFFFF; // White
        }
        LUA->Pop();

        vertices.push_back(vertex);
        LUA->Pop(); // Pop vertex table
    }

    return vertices;
}

// Helper function to extract indices from Lua table
static std::vector<uint32_t> ExtractIndices(ILuaBase* LUA, int stackPos) {
    std::vector<uint32_t> indices;
    
    if (!LUA->IsType(stackPos, Type::Table)) {
        LUA->ThrowError("Expected table for indices");
        return indices;
    }

    size_t indexCount = LUA->ObjLen(stackPos);
    indices.reserve(indexCount);

    for (size_t i = 1; i <= indexCount; i++) {
        LUA->PushNumber(i);
        LUA->GetTable(stackPos);
        
        if (LUA->IsType(-1, Type::Number)) {
            indices.push_back(static_cast<uint32_t>(LUA->GetNumber(-1)));
        }
        
        LUA->Pop();
    }

    return indices;
}

// Helper to extract transform matrix from Lua
static BSPGeometryManager::InstanceTransform ExtractTransform(ILuaBase* LUA, int stackPos) {
    BSPGeometryManager::InstanceTransform transform = {};
    
    if (!LUA->IsType(stackPos, Type::Matrix)) {
        // Return identity transform if not a matrix
        transform.matrix[0][0] = 1.0f;
        transform.matrix[1][1] = 1.0f;
        transform.matrix[2][2] = 1.0f;
        return transform;
    }

    // Extract matrix (GMod VMatrix is 4x4, Remix uses 3x4 row-major)
    // GMod Matrix is userdata with GetField(row, col) method or direct indexing
    // We need to call matrix:GetField(row, col) for each element
    
    // Method 1: Try direct table access (matrix is a special table-like userdata)
    // GMod matrices can be accessed as: matrix[1][1], matrix[1][2], etc. (1-indexed)
    
    LUA->Push(stackPos);  // Push the matrix
    
    // Try to call matrix:ToTable() if it exists, otherwise extract directly
    LUA->GetField(-1, "ToTable");
    if (LUA->IsType(-1, Type::Function)) {
        // Call matrix:ToTable()
        LUA->Push(stackPos);  // Push matrix as 'self'
        LUA->Call(1, 1);  // Call with 1 arg (self), expect 1 return
        
        // Now we have a table on the stack
        if (LUA->IsType(-1, Type::Table)) {
            for (int row = 0; row < 3; row++) {
                LUA->PushNumber(row + 1);  // 1-indexed
                LUA->GetTable(-2);
                
                if (LUA->IsType(-1, Type::Table)) {
                    for (int col = 0; col < 4; col++) {
                        LUA->PushNumber(col + 1);  // 1-indexed
                        LUA->GetTable(-2);
                        
                        if (LUA->IsType(-1, Type::Number)) {
                            transform.matrix[row][col] = static_cast<float>(LUA->GetNumber(-1));
                        }
                        LUA->Pop();  // Pop value
                    }
                }
                LUA->Pop();  // Pop row
            }
        }
        LUA->Pop();  // Pop table result
    } else {
        LUA->Pop();  // Pop nil/non-function
    }
    
    LUA->Pop();  // Pop the matrix
    
#ifdef _DEBUG_VERBOSE
    // Debug log the extracted matrix (first 10 calls only)
    static int callCount = 0;
    if (callCount < 10) {
        Msg("[ExtractTransform] Matrix extracted:\n");
        Msg("  [%.2f %.2f %.2f %.2f]\n", transform.matrix[0][0], transform.matrix[0][1], transform.matrix[0][2], transform.matrix[0][3]);
        Msg("  [%.2f %.2f %.2f %.2f]\n", transform.matrix[1][0], transform.matrix[1][1], transform.matrix[1][2], transform.matrix[1][3]);
        Msg("  [%.2f %.2f %.2f %.2f]\n", transform.matrix[2][0], transform.matrix[2][1], transform.matrix[2][2], transform.matrix[2][3]);
        callCount++;
    }
#endif
    
    return transform;
}

//=============================================================================
// Lua Function: remixapi.UploadStaticPropMesh
// Usage: meshId = remixapi.UploadStaticPropMesh(name, vertices, indices, materialId)
//=============================================================================
LUA_FUNCTION(RemixAPI_UploadStaticPropMesh) {
    auto& manager = RemixAPI::Instance().GetBSPGeometryManager();
    
    const char* name = LUA->CheckString(1);
    std::vector<remixapi_HardcodedVertex> vertices = ExtractVertices(LUA, 2);
    std::vector<uint32_t> indices = ExtractIndices(LUA, 3);
    uint64_t materialId = static_cast<uint64_t>(LUA->CheckNumber(4));
    
    uint64_t meshId = manager.UploadStaticPropMesh(name, vertices, indices, materialId);
    
    LUA->PushNumber(static_cast<double>(meshId));
    return 1;
}

//=============================================================================
// Lua Function: remixapi.UploadDisplacementChunk
// Usage: meshId = remixapi.UploadDisplacementChunk(name, vertices, indices, materialId)
//=============================================================================
LUA_FUNCTION(RemixAPI_UploadDisplacementChunk) {
    auto& manager = RemixAPI::Instance().GetBSPGeometryManager();
    
    const char* name = LUA->CheckString(1);
    std::vector<remixapi_HardcodedVertex> vertices = ExtractVertices(LUA, 2);
    std::vector<uint32_t> indices = ExtractIndices(LUA, 3);
    uint64_t materialId = static_cast<uint64_t>(LUA->CheckNumber(4));
    
    uint64_t meshId = manager.UploadDisplacementChunk(name, vertices, indices, materialId);
    
    LUA->PushNumber(static_cast<double>(meshId));
    return 1;
}

//=============================================================================
// Lua Function: remixapi.DrawMeshInstance
// Usage: success = remixapi.DrawMeshInstance(meshId, transform, categoryFlags)
//=============================================================================
LUA_FUNCTION(RemixAPI_DrawMeshInstance) {
    auto& manager = RemixAPI::Instance().GetBSPGeometryManager();
    
    uint64_t meshId = static_cast<uint64_t>(LUA->CheckNumber(1));
    BSPGeometryManager::InstanceTransform transform = ExtractTransform(LUA, 2);
    uint32_t categoryFlags = LUA->IsType(3, Type::Number) ? static_cast<uint32_t>(LUA->GetNumber(3)) : 0;
    
    bool success = manager.DrawMeshInstance(meshId, transform, categoryFlags);
    
    LUA->PushBool(success);
    return 1;
}

//=============================================================================
// Lua Function: remixapi.DestroyBSPMesh
// Usage: success = remixapi.DestroyBSPMesh(meshId)
//=============================================================================
LUA_FUNCTION(RemixAPI_DestroyBSPMesh) {
    auto& manager = RemixAPI::Instance().GetBSPGeometryManager();
    
    uint64_t meshId = static_cast<uint64_t>(LUA->CheckNumber(1));
    bool success = manager.DestroyMesh(meshId);
    
    LUA->PushBool(success);
    return 1;
}

//=============================================================================
// Lua Function: remixapi.ClearAllBSPMeshes
// Usage: remixapi.ClearAllBSPMeshes()
//=============================================================================
LUA_FUNCTION(RemixAPI_ClearAllBSPMeshes) {
    auto& manager = RemixAPI::Instance().GetBSPGeometryManager();
    manager.ClearAllMeshes();
    return 0;
}

//=============================================================================
// Lua Function: remixapi.GetBSPGeometryStats
// Usage: stats = remixapi.GetBSPGeometryStats()
// Returns: { totalMeshes, totalVertices, totalIndices, staticPropMeshes, displacementMeshes, instancesDrawnThisFrame }
//=============================================================================
LUA_FUNCTION(RemixAPI_GetBSPGeometryStats) {
    auto& manager = RemixAPI::Instance().GetBSPGeometryManager();
    auto stats = manager.GetStatistics();
    
    LUA->CreateTable();
    
    LUA->PushNumber(stats.totalMeshes);
    LUA->SetField(-2, "totalMeshes");
    
    LUA->PushNumber(stats.totalVertices);
    LUA->SetField(-2, "totalVertices");
    
    LUA->PushNumber(stats.totalIndices);
    LUA->SetField(-2, "totalIndices");
    
    LUA->PushNumber(stats.staticPropMeshes);
    LUA->SetField(-2, "staticPropMeshes");
    
    LUA->PushNumber(stats.displacementMeshes);
    LUA->SetField(-2, "displacementMeshes");
    
    LUA->PushNumber(stats.instancesDrawnThisFrame);
    LUA->SetField(-2, "instancesDrawnThisFrame");
    
    return 1;
}

//=============================================================================
// Initialize Lua Bindings
//=============================================================================
void BSPGeometryManager::InitializeLuaBindings() {
    if (!m_lua) return;

    m_lua->PushSpecial(SPECIAL_GLOB);
    m_lua->GetField(-1, "remixapi");
    
    if (!m_lua->IsType(-1, Type::Table)) {
        m_lua->Pop();
        m_lua->CreateTable();
        m_lua->SetField(-2, "remixapi");
        m_lua->GetField(-1, "remixapi");
    }

    // Register functions
    m_lua->PushCFunction(RemixAPI_UploadStaticPropMesh);
    m_lua->SetField(-2, "UploadStaticPropMesh");

    m_lua->PushCFunction(RemixAPI_UploadDisplacementChunk);
    m_lua->SetField(-2, "UploadDisplacementChunk");

    m_lua->PushCFunction(RemixAPI_DrawMeshInstance);
    m_lua->SetField(-2, "DrawMeshInstance");

    m_lua->PushCFunction(RemixAPI_DestroyBSPMesh);
    m_lua->SetField(-2, "DestroyBSPMesh");

    m_lua->PushCFunction(RemixAPI_ClearAllBSPMeshes);
    m_lua->SetField(-2, "ClearAllBSPMeshes");

    m_lua->PushCFunction(RemixAPI_GetBSPGeometryStats);
    m_lua->SetField(-2, "GetBSPGeometryStats");

    m_lua->Pop(2); // Pop remixapi table and global

#ifdef _DEBUG
    Msg("[BSPGeometryManager] Lua bindings initialized\n");
#endif
}

} // namespace RemixAPI

#endif // _WIN64
