#ifdef _WIN64
#include "remixapi.h"
#include <tier0/dbg.h>

using namespace GarrysMod::Lua;

namespace RemixAPI {

// Helper to convert Lua table to matrix
static void LuaToTransform(ILuaBase* LUA, int index, float outMatrix[4][4]) {
    // Initialize identity
    memset(outMatrix, 0, sizeof(float) * 16);
    outMatrix[0][0] = 1.0f; outMatrix[1][1] = 1.0f; outMatrix[2][2] = 1.0f; outMatrix[3][3] = 1.0f;

    if (LUA->IsType(index, Type::Table)) {
        LUA->Push(index); // Push table copy to top
        // Table is at -1
        
        // Lua 1-based to C++ 0-based
        for (int r = 0; r < 4; r++) {
            LUA->PushNumber(r + 1);
            LUA->GetTable(-2); // Get row table from table at -2
            if (LUA->IsType(-1, Type::Table)) {
                for (int c = 0; c < 4; c++) {
                    LUA->PushNumber(c + 1);
                    LUA->GetTable(-2); // Get value
                    if (LUA->IsType(-1, Type::Number)) {
                        outMatrix[r][c] = (float)LUA->GetNumber(-1);
                    }
                    LUA->Pop(); // Pop value
                }
            }
            LUA->Pop(); // Pop row table
        }
        LUA->Pop(); // Pop table copy
    }
}

LUA_FUNCTION(RemixCamera_SetupCamera) {
    if (!LUA->IsType(1, Type::Table)) {
        LUA->ThrowError("Expected table for camera info");
        return 0;
    }

    remix::CameraInfo info;
    
    // Type
    LUA->GetField(1, "type");
    if (LUA->IsType(-1, Type::Number)) {
        info.type = (remix::CameraType)(int)LUA->GetNumber(-1);
    }
    LUA->Pop();

    // View Matrix
    LUA->GetField(1, "view");
    LuaToTransform(LUA, -1, info.view);
    LUA->Pop();

    // Projection Matrix
    LUA->GetField(1, "projection");
    LuaToTransform(LUA, -1, info.projection);
    LUA->Pop();

    bool result = RemixAPI::Instance().GetCameraManager().SetupCamera(info);
    LUA->PushBool(result);
    return 1;
}

LUA_FUNCTION(RemixCamera_SetupParameterizedCamera) {
    if (!LUA->IsType(1, Type::Table)) {
        LUA->ThrowError("Expected table for camera parameters");
        return 0;
    }

    remix::CameraInfoParameterizedEXT params;

    remix::CameraType cameraType = REMIXAPI_CAMERA_TYPE_WORLD;
    LUA->GetField(1, "type");
    if (LUA->IsType(-1, Type::Number)) {
        cameraType = (remix::CameraType)(int)LUA->GetNumber(-1);
    }
    LUA->Pop();

    // Position
    LUA->GetField(1, "position");
    if (LUA->IsType(-1, Type::Vector)) {
        Vector pos = LUA->GetVector(-1);
        params.position = { pos.x, pos.y, pos.z };
    }
    LUA->Pop();

    // Forward
    LUA->GetField(1, "forward");
    if (LUA->IsType(-1, Type::Vector)) {
        Vector fwd = LUA->GetVector(-1);
        params.forward = { fwd.x, fwd.y, fwd.z };
    }
    LUA->Pop();

    // Up
    LUA->GetField(1, "up");
    if (LUA->IsType(-1, Type::Vector)) {
        Vector up = LUA->GetVector(-1);
        params.up = { up.x, up.y, up.z };
    }
    LUA->Pop();

    // Right
    LUA->GetField(1, "right");
    if (LUA->IsType(-1, Type::Vector)) {
        Vector right = LUA->GetVector(-1);
        params.right = { right.x, right.y, right.z };
    }
    LUA->Pop();

    // FOV
    LUA->GetField(1, "fovYInDegrees");
    if (LUA->IsType(-1, Type::Number)) {
        params.fovYInDegrees = (float)LUA->GetNumber(-1);
    }
    LUA->Pop();

    // Aspect
    LUA->GetField(1, "aspect");
    if (LUA->IsType(-1, Type::Number)) {
        params.aspect = (float)LUA->GetNumber(-1);
    }
    LUA->Pop();

    // Near Plane
    LUA->GetField(1, "nearPlane");
    if (LUA->IsType(-1, Type::Number)) {
        params.nearPlane = (float)LUA->GetNumber(-1);
    }
    LUA->Pop();

    // Far Plane
    LUA->GetField(1, "farPlane");
    if (LUA->IsType(-1, Type::Number)) {
        params.farPlane = (float)LUA->GetNumber(-1);
    }
    LUA->Pop();

    bool result = RemixAPI::Instance().GetCameraManager().SetupParameterizedCamera(params, cameraType);
    LUA->PushBool(result);
    return 1;
}

void CameraManager::InitializeLuaBindings() {
    if (!m_lua) return;

    m_lua->PushSpecial(GarrysMod::Lua::SPECIAL_GLOB);
    m_lua->CreateTable();

    m_lua->PushCFunction(RemixCamera_SetupCamera);
    m_lua->SetField(-2, "SetupCamera");

    m_lua->PushCFunction(RemixCamera_SetupParameterizedCamera);
    m_lua->SetField(-2, "SetupParameterizedCamera");

    // Expose Camera Types
    m_lua->PushNumber((double)remix::CameraType::REMIXAPI_CAMERA_TYPE_WORLD);
    m_lua->SetField(-2, "TYPE_WORLD");
    
    m_lua->PushNumber((double)remix::CameraType::REMIXAPI_CAMERA_TYPE_SKY);
    m_lua->SetField(-2, "TYPE_SKY");
    
    m_lua->PushNumber((double)remix::CameraType::REMIXAPI_CAMERA_TYPE_VIEW_MODEL);
    m_lua->SetField(-2, "TYPE_VIEW_MODEL");

    m_lua->PushNumber((double)remix::CameraType::REMIXAPI_CAMERA_TYPE_RENDER_TO_TEXTURE);
    m_lua->SetField(-2, "TYPE_RENDER_TO_TEXTURE");

    m_lua->PushNumber((double)remix::CameraType::REMIXAPI_CAMERA_TYPE_RENDER_TO_TEXTURE);
    m_lua->SetField(-2, "TYPE_RENDER_VIEW");

    m_lua->SetField(-2, "RemixCamera");
    m_lua->Pop();
}

} // namespace RemixAPI
#endif
