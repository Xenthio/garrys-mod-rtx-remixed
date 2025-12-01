#ifdef _WIN64
#include "remixapi.h"
#include <tier0/dbg.h>
#include <vector>

using namespace GarrysMod::Lua;

namespace RemixAPI {

// Helper to parse a Float3 from Lua table
static void LuaToFloat3(ILuaBase* LUA, int index, float out[3]) {
    if (LUA->IsType(index, Type::Table) || LUA->IsType(index, Type::Vector)) {
        // Handle GMod Vector type or table {x, y, z} or {1, 2, 3}
        LUA->Push(index); // Push table/vector to top
        
        // Try numbered indices first (1, 2, 3)
        LUA->PushNumber(1); LUA->GetTable(-2);
        bool hasNumbered = !LUA->IsType(-1, Type::Nil);
        LUA->Pop();
        
        if (hasNumbered) {
            LUA->PushNumber(1); LUA->GetTable(-2); out[0] = (float)LUA->GetNumber(-1); LUA->Pop();
            LUA->PushNumber(2); LUA->GetTable(-2); out[1] = (float)LUA->GetNumber(-1); LUA->Pop();
            LUA->PushNumber(3); LUA->GetTable(-2); out[2] = (float)LUA->GetNumber(-1); LUA->Pop();
        } else {
            // Try named fields (x, y, z)
            LUA->GetField(-1, "x"); out[0] = (float)LUA->GetNumber(-1); LUA->Pop();
            LUA->GetField(-1, "y"); out[1] = (float)LUA->GetNumber(-1); LUA->Pop();
            LUA->GetField(-1, "z"); out[2] = (float)LUA->GetNumber(-1); LUA->Pop();
        }
        LUA->Pop(); // Pop table/vector
    }
}

// Helper to parse a Float2 from Lua table
static void LuaToFloat2(ILuaBase* LUA, int index, float out[2]) {
    if (LUA->IsType(index, Type::Table)) {
        LUA->Push(index);
        
        LUA->PushNumber(1); LUA->GetTable(-2);
        if (!LUA->IsType(-1, Type::Nil)) {
            out[0] = (float)LUA->GetNumber(-1); LUA->Pop();
            LUA->PushNumber(2); LUA->GetTable(-2); out[1] = (float)LUA->GetNumber(-1); LUA->Pop();
        } else {
            LUA->Pop();
            LUA->GetField(-1, "u"); 
            if (LUA->IsType(-1, Type::Nil)) {
                LUA->Pop(); 
                LUA->GetField(-1, "x"); 
            }
            out[0] = (float)LUA->GetNumber(-1); LUA->Pop();
            
            LUA->GetField(-1, "v");
            if (LUA->IsType(-1, Type::Nil)) {
                LUA->Pop(); 
                LUA->GetField(-1, "y"); 
            }
            out[1] = (float)LUA->GetNumber(-1); LUA->Pop();
        }
        LUA->Pop();
    }
}

struct MeshData {
    std::vector<remixapi_MeshInfoSurfaceTriangles> surfaceInfos;
    // Each surface needs its own vertex and index buffers
    std::vector<std::vector<remixapi_HardcodedVertex>> vertexBuffers;
    std::vector<std::vector<uint32_t>> indexBuffers;
};

LUA_FUNCTION(RemixMesh_CreateMesh) {
    if (!LUA->IsType(1, Type::String)) {
        LUA->ThrowError("Expected string for mesh name");
        return 0;
    }
    if (!LUA->IsType(2, Type::Table)) {
        LUA->ThrowError("Expected table for mesh info");
        return 0;
    }

    std::string name = LUA->GetString(1);
    
    remix::MeshInfo meshInfo;
    MeshData meshData;

    // Parse Hash
    LUA->GetField(2, "hash");
    if (LUA->IsType(-1, Type::String)) {
        const char* hashStr = LUA->GetString(-1);
        meshInfo.hash = std::strtoull(hashStr, nullptr, 0);
    } else if (LUA->IsType(-1, Type::Number)) {
        meshInfo.hash = (uint64_t)LUA->GetNumber(-1);
    }
    LUA->Pop();

    // Parse Surfaces
    LUA->GetField(2, "surfaces");
    if (LUA->IsType(-1, Type::Table)) {
        int surfaceCount = LUA->ObjLen(-1);
        
        meshData.vertexBuffers.resize(surfaceCount);
        meshData.indexBuffers.resize(surfaceCount);
        meshData.surfaceInfos.resize(surfaceCount);

        for (int i = 0; i < surfaceCount; i++) {
            LUA->PushNumber(i + 1);
            LUA->GetTable(-2); // Get surface table

            if (LUA->IsType(-1, Type::Table)) {
                auto& vertices = meshData.vertexBuffers[i];
                auto& indices = meshData.indexBuffers[i];
                auto& surfInfo = meshData.surfaceInfos[i];

                // Material
                LUA->GetField(-1, "material");
                if (LUA->IsType(-1, Type::Number)) {
                    uint64_t matId = (uint64_t)LUA->GetNumber(-1);
                    surfInfo.material = RemixAPI::Instance().GetMaterialManager().GetMaterialHandle(matId);
                }
                LUA->Pop();

                // Vertices
                LUA->GetField(-1, "vertices");
                if (LUA->IsType(-1, Type::Table)) {
                    int vertCount = LUA->ObjLen(-1);
                    vertices.resize(vertCount);
                    
                    for (int v = 0; v < vertCount; v++) {
                        LUA->PushNumber(v + 1);
                        LUA->GetTable(-2); // Get vertex table
                        
                        remixapi_HardcodedVertex& vert = vertices[v];
                        
                        // Position
                        LUA->GetField(-1, "pos");
                        LuaToFloat3(LUA, -1, vert.position);
                        LUA->Pop();
                        
                        // Normal
                        LUA->GetField(-1, "normal");
                        LuaToFloat3(LUA, -1, vert.normal);
                        LUA->Pop();
                        
                        // Texcoord
                        LUA->GetField(-1, "texcoord");
                        LuaToFloat2(LUA, -1, vert.texcoord);
                        LUA->Pop();
                        
                        // Color
                        LUA->GetField(-1, "color");
                        if (LUA->IsType(-1, Type::Number)) {
                            vert.color = (uint32_t)LUA->GetNumber(-1);
                        } else {
                            vert.color = 0xFFFFFFFF;
                        }
                        LUA->Pop();

                        LUA->Pop(); // Pop vertex table
                    }
                }
                LUA->Pop(); // Pop vertices list

                // Indices
                LUA->GetField(-1, "indices");
                if (LUA->IsType(-1, Type::Table)) {
                    int indCount = LUA->ObjLen(-1);
                    indices.resize(indCount);
                    
                    for (int idx = 0; idx < indCount; idx++) {
                        LUA->PushNumber(idx + 1);
                        LUA->GetTable(-2);
                        indices[idx] = (uint32_t)LUA->GetNumber(-1);
                        LUA->Pop();
                    }
                }
                LUA->Pop(); // Pop indices list

                // Setup surface info pointers
                surfInfo.vertices_values = vertices.data();
                surfInfo.vertices_count = vertices.size();
                surfInfo.indices_values = indices.data();
                surfInfo.indices_count = indices.size();
                surfInfo.skinning_hasvalue = false; // Skinning not supported in Lua binding yet
            }
            LUA->Pop(); // Pop surface table
        }
    }
    LUA->Pop(); // Pop surfaces list

    meshInfo.surfaces_values = meshData.surfaceInfos.data();
    meshInfo.surfaces_count = meshData.surfaceInfos.size();

    auto& meshManager = RemixAPI::Instance().GetMeshManager();
    uint64_t meshId = meshManager.CreateMesh(name, meshInfo);

    LUA->PushNumber((double)meshId);
    return 1;
}

LUA_FUNCTION(RemixMesh_DestroyMesh) {
    uint64_t meshId = (uint64_t)LUA->CheckNumber(1);
    bool result = RemixAPI::Instance().GetMeshManager().DestroyMesh(meshId);
    LUA->PushBool(result);
    return 1;
}

LUA_FUNCTION(RemixMesh_HasMesh) {
    uint64_t meshId = (uint64_t)LUA->CheckNumber(1);
    bool result = RemixAPI::Instance().GetMeshManager().HasMesh(meshId);
    LUA->PushBool(result);
    return 1;
}

void MeshManager::InitializeLuaBindings() {
    if (!m_lua) return;

    m_lua->PushSpecial(GarrysMod::Lua::SPECIAL_GLOB);
    m_lua->CreateTable();

    m_lua->PushCFunction(RemixMesh_CreateMesh);
    m_lua->SetField(-2, "CreateMesh");

    m_lua->PushCFunction(RemixMesh_DestroyMesh);
    m_lua->SetField(-2, "DestroyMesh");

    m_lua->PushCFunction(RemixMesh_HasMesh);
    m_lua->SetField(-2, "HasMesh");

    m_lua->SetField(-2, "RemixMesh");
    m_lua->Pop();
}

} // namespace RemixAPI
#endif

