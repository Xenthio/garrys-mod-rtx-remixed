#ifdef _WIN64
#include "remixapi.h"
#include <tier0/dbg.h>

using namespace GarrysMod::Lua;

namespace RemixAPI {

static void LuaToTransform(ILuaBase* LUA, int index, float outMatrix[3][4]) {
    // Initialize identity
    outMatrix[0][0] = 1; outMatrix[0][1] = 0; outMatrix[0][2] = 0; outMatrix[0][3] = 0;
    outMatrix[1][0] = 0; outMatrix[1][1] = 1; outMatrix[1][2] = 0; outMatrix[1][3] = 0;
    outMatrix[2][0] = 0; outMatrix[2][1] = 0; outMatrix[2][2] = 1; outMatrix[2][3] = 0;

    if (LUA->IsType(index, Type::Table)) {
        // Push table to top so relative index -1 refers to it (if index was negative)
        // But if index is absolute, it won't work.
        // Safest: Convert index to absolute before loop or copy to top.
        // Let's copy to top.
        LUA->Push(index); 
        // Now table is at -1.
        
        for (int r = 0; r < 3; r++) {
            LUA->PushNumber(r + 1);
            LUA->GetTable(-2); // Get from table at -2 (pushed copy)
            if (LUA->IsType(-1, Type::Table)) {
                for (int c = 0; c < 4; c++) {
                    LUA->PushNumber(c + 1);
                    LUA->GetTable(-2); // Get from row table at -2
                    outMatrix[r][c] = (float)LUA->GetNumber(-1);
                    LUA->Pop(); // Pop value
                }
            }
            LUA->Pop(); // Pop row table
        }
        LUA->Pop(); // Pop table copy
    }
}

LUA_FUNCTION(RemixInstance_DrawInstance) {
    if (!LUA->IsType(1, Type::Table)) {
        LUA->ThrowError("Expected table for instance info");
        return 0;
    }

    remix::InstanceInfo info;
    
    // Mesh ID -> Handle
    LUA->GetField(1, "meshId");
    if (LUA->IsType(-1, Type::Number)) {
        uint64_t meshId = (uint64_t)LUA->GetNumber(-1);
        info.mesh = RemixAPI::Instance().GetMeshManager().GetMeshHandle(meshId);
    }
    LUA->Pop();

    if (!info.mesh) {
        // If no valid mesh, don't draw
        LUA->PushBool(false);
        return 1;
    }

    // Transform
    LUA->GetField(1, "transform");
    LuaToTransform(LUA, -1, info.transform.matrix);
    LUA->Pop();

    // Category Flags
    LUA->GetField(1, "categoryFlags");
    if (LUA->IsType(-1, Type::Number)) {
        info.categoryFlags = (uint32_t)LUA->GetNumber(-1);
    }
    LUA->Pop();

    // Double Sided
    LUA->GetField(1, "doubleSided");
    if (LUA->IsType(-1, Type::Bool)) {
        info.doubleSided = LUA->GetBool(-1);
    }
    LUA->Pop();

    bool result = RemixAPI::Instance().GetInstanceManager().DrawInstance(info);
    LUA->PushBool(result);
    return 1;
}

void InstanceManager::InitializeLuaBindings() {
    if (!m_lua) return;

    m_lua->PushSpecial(GarrysMod::Lua::SPECIAL_GLOB);
    m_lua->CreateTable();

    m_lua->PushCFunction(RemixInstance_DrawInstance);
    m_lua->SetField(-2, "DrawInstance");

    m_lua->SetField(-2, "RemixInstance");
    m_lua->Pop();
}

} // namespace RemixAPI
#endif
