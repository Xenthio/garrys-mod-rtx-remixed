#ifdef _WIN64
#include "remixapi.h"
#include <tier0/dbg.h>

using namespace GarrysMod::Lua;

namespace RemixAPI {
// No per-frame submission required with internal auto-instancing

// Helper function to extract LightInfo from Lua table
static remix::LightInfo LuaToLightInfo(ILuaBase* LUA, int index) {
    remix::LightInfo info;
    
    if (!LUA->IsType(index, Type::Table)) {
        LUA->ThrowError("Expected table for LightInfo");
        return info;
    }
    
    // Get hash
    LUA->GetField(index, "hash");
    if (LUA->IsType(-1, Type::Number)) {
        info.hash = static_cast<uint64_t>(LUA->GetNumber(-1));
    }
    LUA->Pop();
    
    // Get radiance (RGB color/intensity)
    LUA->GetField(index, "radiance");
    if (LUA->IsType(-1, Type::Table)) {
        LUA->GetField(-1, "x");
        if (LUA->IsType(-1, Type::Number)) {
            info.radiance.x = static_cast<float>(LUA->GetNumber(-1));
        }
        LUA->Pop();
        
        LUA->GetField(-1, "y");
        if (LUA->IsType(-1, Type::Number)) {
            info.radiance.y = static_cast<float>(LUA->GetNumber(-1));
        }
        LUA->Pop();
        
        LUA->GetField(-1, "z");
        if (LUA->IsType(-1, Type::Number)) {
            info.radiance.z = static_cast<float>(LUA->GetNumber(-1));
        }
        LUA->Pop();
    }
    LUA->Pop();
    
    return info;
}

// Helper function to extract LightInfoSphereEXT from Lua table
static remix::LightInfoSphereEXT LuaToSphereInfo(ILuaBase* LUA, int index) {
    remix::LightInfoSphereEXT info;
    
    if (!LUA->IsType(index, Type::Table)) {
        LUA->ThrowError("Expected table for SphereInfo");
        return info;
    }
    
    // Get position
    LUA->GetField(index, "position");
    if (LUA->IsType(-1, Type::Table)) {
        LUA->GetField(-1, "x");
        if (LUA->IsType(-1, Type::Number)) {
            info.position.x = static_cast<float>(LUA->GetNumber(-1));
        }
        LUA->Pop();
        
        LUA->GetField(-1, "y");
        if (LUA->IsType(-1, Type::Number)) {
            info.position.y = static_cast<float>(LUA->GetNumber(-1));
        }
        LUA->Pop();
        
        LUA->GetField(-1, "z");
        if (LUA->IsType(-1, Type::Number)) {
            info.position.z = static_cast<float>(LUA->GetNumber(-1));
        }
        LUA->Pop();
    }
    LUA->Pop();
    
    // Get radius
    LUA->GetField(index, "radius");
    if (LUA->IsType(-1, Type::Number)) {
        info.radius = static_cast<float>(LUA->GetNumber(-1));
    }
    LUA->Pop();
    
    // Get shaping (optional)
    LUA->GetField(index, "shaping");
    if (LUA->IsType(-1, Type::Table)) {
        remix::LightInfoLightShaping shaping;
        
        // Direction
        LUA->GetField(-1, "direction");
        if (LUA->IsType(-1, Type::Table)) {
            LUA->GetField(-1, "x");
            if (LUA->IsType(-1, Type::Number)) {
                shaping.direction.x = static_cast<float>(LUA->GetNumber(-1));
            }
            LUA->Pop();
            
            LUA->GetField(-1, "y");
            if (LUA->IsType(-1, Type::Number)) {
                shaping.direction.y = static_cast<float>(LUA->GetNumber(-1));
            }
            LUA->Pop();
            
            LUA->GetField(-1, "z");
            if (LUA->IsType(-1, Type::Number)) {
                shaping.direction.z = static_cast<float>(LUA->GetNumber(-1));
            }
            LUA->Pop();
        }
        LUA->Pop();
        
        // Cone angle
        LUA->GetField(-1, "coneAngleDegrees");
        if (LUA->IsType(-1, Type::Number)) {
            shaping.coneAngleDegrees = static_cast<float>(LUA->GetNumber(-1));
        }
        LUA->Pop();
        
        // Cone softness
        LUA->GetField(-1, "coneSoftness");
        if (LUA->IsType(-1, Type::Number)) {
            shaping.coneSoftness = static_cast<float>(LUA->GetNumber(-1));
        }
        LUA->Pop();
        
        // Focus exponent
        LUA->GetField(-1, "focusExponent");
        if (LUA->IsType(-1, Type::Number)) {
            shaping.focusExponent = static_cast<float>(LUA->GetNumber(-1));
        }
        LUA->Pop();
        
        info.set_shaping(shaping);
    }
    LUA->Pop();
    
    // Get volumetric radiance scale
    LUA->GetField(index, "volumetricRadianceScale");
    if (LUA->IsType(-1, Type::Number)) {
        info.volumetricRadianceScale = static_cast<float>(LUA->GetNumber(-1));
    }
    LUA->Pop();
    
    return info;
}

// Helper function to extract LightInfoRectEXT from Lua table
static remix::LightInfoRectEXT LuaToRectInfo(ILuaBase* LUA, int index) {
    remix::LightInfoRectEXT info;
    
    if (!LUA->IsType(index, Type::Table)) {
        LUA->ThrowError("Expected table for RectInfo");
        return info;
    }
    
    // Get position
    LUA->GetField(index, "position");
    if (LUA->IsType(-1, Type::Table)) {
        LUA->GetField(-1, "x");
        if (LUA->IsType(-1, Type::Number)) {
            info.position.x = static_cast<float>(LUA->GetNumber(-1));
        }
        LUA->Pop();
        
        LUA->GetField(-1, "y");
        if (LUA->IsType(-1, Type::Number)) {
            info.position.y = static_cast<float>(LUA->GetNumber(-1));
        }
        LUA->Pop();
        
        LUA->GetField(-1, "z");
        if (LUA->IsType(-1, Type::Number)) {
            info.position.z = static_cast<float>(LUA->GetNumber(-1));
        }
        LUA->Pop();
    }
    LUA->Pop();
    
    // Get X axis
    LUA->GetField(index, "xAxis");
    if (LUA->IsType(-1, Type::Table)) {
        LUA->GetField(-1, "x");
        if (LUA->IsType(-1, Type::Number)) {
            info.xAxis.x = static_cast<float>(LUA->GetNumber(-1));
        }
        LUA->Pop();
        
        LUA->GetField(-1, "y");
        if (LUA->IsType(-1, Type::Number)) {
            info.xAxis.y = static_cast<float>(LUA->GetNumber(-1));
        }
        LUA->Pop();
        
        LUA->GetField(-1, "z");
        if (LUA->IsType(-1, Type::Number)) {
            info.xAxis.z = static_cast<float>(LUA->GetNumber(-1));
        }
        LUA->Pop();
    }
    LUA->Pop();
    
    // Get Y axis
    LUA->GetField(index, "yAxis");
    if (LUA->IsType(-1, Type::Table)) {
        LUA->GetField(-1, "x");
        if (LUA->IsType(-1, Type::Number)) {
            info.yAxis.x = static_cast<float>(LUA->GetNumber(-1));
        }
        LUA->Pop();
        
        LUA->GetField(-1, "y");
        if (LUA->IsType(-1, Type::Number)) {
            info.yAxis.y = static_cast<float>(LUA->GetNumber(-1));
        }
        LUA->Pop();
        
        LUA->GetField(-1, "z");
        if (LUA->IsType(-1, Type::Number)) {
            info.yAxis.z = static_cast<float>(LUA->GetNumber(-1));
        }
        LUA->Pop();
    }
    LUA->Pop();
    
    // Get direction
    LUA->GetField(index, "direction");
    if (LUA->IsType(-1, Type::Table)) {
        LUA->GetField(-1, "x");
        if (LUA->IsType(-1, Type::Number)) {
            info.direction.x = static_cast<float>(LUA->GetNumber(-1));
        }
        LUA->Pop();
        
        LUA->GetField(-1, "y");
        if (LUA->IsType(-1, Type::Number)) {
            info.direction.y = static_cast<float>(LUA->GetNumber(-1));
        }
        LUA->Pop();
        
        LUA->GetField(-1, "z");
        if (LUA->IsType(-1, Type::Number)) {
            info.direction.z = static_cast<float>(LUA->GetNumber(-1));
        }
        LUA->Pop();
    }
    LUA->Pop();
    
    // Get sizes
    LUA->GetField(index, "xSize");
    if (LUA->IsType(-1, Type::Number)) {
        info.xSize = static_cast<float>(LUA->GetNumber(-1));
    }
    LUA->Pop();
    
    LUA->GetField(index, "ySize");
    if (LUA->IsType(-1, Type::Number)) {
        info.ySize = static_cast<float>(LUA->GetNumber(-1));
    }
    LUA->Pop();
    
    // Get volumetric radiance scale
    LUA->GetField(index, "volumetricRadianceScale");
    if (LUA->IsType(-1, Type::Number)) {
        info.volumetricRadianceScale = static_cast<float>(LUA->GetNumber(-1));
    }
    LUA->Pop();
    
    return info;
}

// Helper function to extract LightInfoDiskEXT from Lua table
static remix::LightInfoDiskEXT LuaToDiskInfo(ILuaBase* LUA, int index) {
    remix::LightInfoDiskEXT info;
    
    if (!LUA->IsType(index, Type::Table)) {
        LUA->ThrowError("Expected table for DiskInfo");
        return info;
    }
    
    // Similar to rect, but with radii instead of sizes
    // Get position
    LUA->GetField(index, "position");
    if (LUA->IsType(-1, Type::Table)) {
        LUA->GetField(-1, "x");
        if (LUA->IsType(-1, Type::Number)) {
            info.position.x = static_cast<float>(LUA->GetNumber(-1));
        }
        LUA->Pop();
        
        LUA->GetField(-1, "y");
        if (LUA->IsType(-1, Type::Number)) {
            info.position.y = static_cast<float>(LUA->GetNumber(-1));
        }
        LUA->Pop();
        
        LUA->GetField(-1, "z");
        if (LUA->IsType(-1, Type::Number)) {
            info.position.z = static_cast<float>(LUA->GetNumber(-1));
        }
        LUA->Pop();
    }
    LUA->Pop();
    
    // Get X axis
    LUA->GetField(index, "xAxis");
    if (LUA->IsType(-1, Type::Table)) {
        LUA->GetField(-1, "x");
        if (LUA->IsType(-1, Type::Number)) {
            info.xAxis.x = static_cast<float>(LUA->GetNumber(-1));
        }
        LUA->Pop();
        
        LUA->GetField(-1, "y");
        if (LUA->IsType(-1, Type::Number)) {
            info.xAxis.y = static_cast<float>(LUA->GetNumber(-1));
        }
        LUA->Pop();
        
        LUA->GetField(-1, "z");
        if (LUA->IsType(-1, Type::Number)) {
            info.xAxis.z = static_cast<float>(LUA->GetNumber(-1));
        }
        LUA->Pop();
    }
    LUA->Pop();
    
    // Get Y axis
    LUA->GetField(index, "yAxis");
    if (LUA->IsType(-1, Type::Table)) {
        LUA->GetField(-1, "x");
        if (LUA->IsType(-1, Type::Number)) {
            info.yAxis.x = static_cast<float>(LUA->GetNumber(-1));
        }
        LUA->Pop();
        
        LUA->GetField(-1, "y");
        if (LUA->IsType(-1, Type::Number)) {
            info.yAxis.y = static_cast<float>(LUA->GetNumber(-1));
        }
        LUA->Pop();
        
        LUA->GetField(-1, "z");
        if (LUA->IsType(-1, Type::Number)) {
            info.yAxis.z = static_cast<float>(LUA->GetNumber(-1));
        }
        LUA->Pop();
    }
    LUA->Pop();
    
    // Get direction
    LUA->GetField(index, "direction");
    if (LUA->IsType(-1, Type::Table)) {
        LUA->GetField(-1, "x");
        if (LUA->IsType(-1, Type::Number)) {
            info.direction.x = static_cast<float>(LUA->GetNumber(-1));
        }
        LUA->Pop();
        
        LUA->GetField(-1, "y");
        if (LUA->IsType(-1, Type::Number)) {
            info.direction.y = static_cast<float>(LUA->GetNumber(-1));
        }
        LUA->Pop();
        
        LUA->GetField(-1, "z");
        if (LUA->IsType(-1, Type::Number)) {
            info.direction.z = static_cast<float>(LUA->GetNumber(-1));
        }
        LUA->Pop();
    }
    LUA->Pop();
    
    // Get radii
    LUA->GetField(index, "xRadius");
    if (LUA->IsType(-1, Type::Number)) {
        info.xRadius = static_cast<float>(LUA->GetNumber(-1));
    }
    LUA->Pop();
    
    LUA->GetField(index, "yRadius");
    if (LUA->IsType(-1, Type::Number)) {
        info.yRadius = static_cast<float>(LUA->GetNumber(-1));
    }
    LUA->Pop();
    
    // Get volumetric radiance scale
    LUA->GetField(index, "volumetricRadianceScale");
    if (LUA->IsType(-1, Type::Number)) {
        info.volumetricRadianceScale = static_cast<float>(LUA->GetNumber(-1));
    }
    LUA->Pop();
    
    return info;
}

// Helper function to extract LightInfoDistantEXT from Lua table
static remix::LightInfoDistantEXT LuaToDistantInfo(ILuaBase* LUA, int index) {
    remix::LightInfoDistantEXT info;
    
    if (!LUA->IsType(index, Type::Table)) {
        LUA->ThrowError("Expected table for DistantInfo");
        return info;
    }
    
    // Get direction
    LUA->GetField(index, "direction");
    if (LUA->IsType(-1, Type::Table)) {
        LUA->GetField(-1, "x");
        if (LUA->IsType(-1, Type::Number)) {
            info.direction.x = static_cast<float>(LUA->GetNumber(-1));
        }
        LUA->Pop();
        
        LUA->GetField(-1, "y");
        if (LUA->IsType(-1, Type::Number)) {
            info.direction.y = static_cast<float>(LUA->GetNumber(-1));
        }
        LUA->Pop();
        
        LUA->GetField(-1, "z");
        if (LUA->IsType(-1, Type::Number)) {
            info.direction.z = static_cast<float>(LUA->GetNumber(-1));
        }
        LUA->Pop();
    }
    LUA->Pop();
    
    // Get angular diameter
    LUA->GetField(index, "angularDiameterDegrees");
    if (LUA->IsType(-1, Type::Number)) {
        info.angularDiameterDegrees = static_cast<float>(LUA->GetNumber(-1));
    }
    LUA->Pop();
    
    // Get volumetric radiance scale
    LUA->GetField(index, "volumetricRadianceScale");
    if (LUA->IsType(-1, Type::Number)) {
        info.volumetricRadianceScale = static_cast<float>(LUA->GetNumber(-1));
    }
    LUA->Pop();
    
    return info;
}

// Lua function: RemixLight.CreateSphere(baseInfo, sphereInfo, entityID)
LUA_FUNCTION(RemixLight_CreateSphere) {
    if (!LUA->IsType(1, Type::Table)) {
        LUA->ThrowError("Expected table for base light info");
        return 0;
    }
    
    if (!LUA->IsType(2, Type::Table)) {
        LUA->ThrowError("Expected table for sphere info");
        return 0;
    }
    
    remix::LightInfo baseInfo = LuaToLightInfo(LUA, 1);
    remix::LightInfoSphereEXT sphereInfo = LuaToSphereInfo(LUA, 2);
    
    uint64_t entityID = 0;
    if (LUA->IsType(3, Type::Number)) {
        entityID = static_cast<uint64_t>(LUA->GetNumber(3));
    }
    
    auto& lightManager = RemixAPI::Instance().GetLightManager();
    uint64_t lightId = lightManager.CreateSphereLight(baseInfo, sphereInfo, entityID);
    
    LUA->PushNumber(static_cast<double>(lightId));
    return 1;
}

// Lua function: RemixLight.UpdateSphere(baseInfo, sphereInfo, lightId)
LUA_FUNCTION(RemixLight_UpdateSphere) {
    if (!LUA->IsType(1, Type::Table)) {
        LUA->ThrowError("Expected table for base light info");
        return 0;
    }
    if (!LUA->IsType(2, Type::Table)) {
        LUA->ThrowError("Expected table for sphere info");
        return 0;
    }
    if (!LUA->IsType(3, Type::Number)) {
        LUA->ThrowError("Expected number for light ID");
        return 0;
    }
    remix::LightInfo baseInfo = LuaToLightInfo(LUA, 1);
    remix::LightInfoSphereEXT sphereInfo = LuaToSphereInfo(LUA, 2);
    uint64_t lightId = static_cast<uint64_t>(LUA->GetNumber(3));
    bool ok = RemixAPI::Instance().GetLightManager().UpdateSphereLight(lightId, baseInfo, sphereInfo);
    LUA->PushBool(ok);
    return 1;
}

// Lua function: RemixLight.CreateRect(baseInfo, rectInfo, entityID)
LUA_FUNCTION(RemixLight_CreateRect) {
    if (!LUA->IsType(1, Type::Table)) {
        LUA->ThrowError("Expected table for base light info");
        return 0;
    }
    
    if (!LUA->IsType(2, Type::Table)) {
        LUA->ThrowError("Expected table for rect info");
        return 0;
    }
    
    remix::LightInfo baseInfo = LuaToLightInfo(LUA, 1);
    remix::LightInfoRectEXT rectInfo = LuaToRectInfo(LUA, 2);
    
    uint64_t entityID = 0;
    if (LUA->IsType(3, Type::Number)) {
        entityID = static_cast<uint64_t>(LUA->GetNumber(3));
    }
    
    auto& lightManager = RemixAPI::Instance().GetLightManager();
    uint64_t lightId = lightManager.CreateRectLight(baseInfo, rectInfo, entityID);
    
    LUA->PushNumber(static_cast<double>(lightId));
    return 1;
}

// Lua function: RemixLight.UpdateRect(baseInfo, rectInfo, lightId)
LUA_FUNCTION(RemixLight_UpdateRect) {
    if (!LUA->IsType(1, Type::Table)) {
        LUA->ThrowError("Expected table for base light info");
        return 0;
    }
    if (!LUA->IsType(2, Type::Table)) {
        LUA->ThrowError("Expected table for rect info");
        return 0;
    }
    if (!LUA->IsType(3, Type::Number)) {
        LUA->ThrowError("Expected number for light ID");
        return 0;
    }
    remix::LightInfo baseInfo = LuaToLightInfo(LUA, 1);
    remix::LightInfoRectEXT rectInfo = LuaToRectInfo(LUA, 2);
    uint64_t lightId = static_cast<uint64_t>(LUA->GetNumber(3));
    bool ok = RemixAPI::Instance().GetLightManager().UpdateRectLight(lightId, baseInfo, rectInfo);
    LUA->PushBool(ok);
    return 1;
}

// Lua function: RemixLight.CreateDisk(baseInfo, diskInfo, entityID)
LUA_FUNCTION(RemixLight_CreateDisk) {
    if (!LUA->IsType(1, Type::Table)) {
        LUA->ThrowError("Expected table for base light info");
        return 0;
    }
    
    if (!LUA->IsType(2, Type::Table)) {
        LUA->ThrowError("Expected table for disk info");
        return 0;
    }
    
    remix::LightInfo baseInfo = LuaToLightInfo(LUA, 1);
    remix::LightInfoDiskEXT diskInfo = LuaToDiskInfo(LUA, 2);
    
    uint64_t entityID = 0;
    if (LUA->IsType(3, Type::Number)) {
        entityID = static_cast<uint64_t>(LUA->GetNumber(3));
    }
    
    auto& lightManager = RemixAPI::Instance().GetLightManager();
    uint64_t lightId = lightManager.CreateDiskLight(baseInfo, diskInfo, entityID);
    
    LUA->PushNumber(static_cast<double>(lightId));
    return 1;
}

// Lua function: RemixLight.UpdateDisk(baseInfo, diskInfo, lightId)
LUA_FUNCTION(RemixLight_UpdateDisk) {
    if (!LUA->IsType(1, Type::Table)) {
        LUA->ThrowError("Expected table for base light info");
        return 0;
    }
    if (!LUA->IsType(2, Type::Table)) {
        LUA->ThrowError("Expected table for disk info");
        return 0;
    }
    if (!LUA->IsType(3, Type::Number)) {
        LUA->ThrowError("Expected number for light ID");
        return 0;
    }
    remix::LightInfo baseInfo = LuaToLightInfo(LUA, 1);
    remix::LightInfoDiskEXT diskInfo = LuaToDiskInfo(LUA, 2);
    uint64_t lightId = static_cast<uint64_t>(LUA->GetNumber(3));
    bool ok = RemixAPI::Instance().GetLightManager().UpdateDiskLight(lightId, baseInfo, diskInfo);
    LUA->PushBool(ok);
    return 1;
}

// Lua function: RemixLight.CreateDistant(baseInfo, distantInfo, entityID)
LUA_FUNCTION(RemixLight_CreateDistant) {
    if (!LUA->IsType(1, Type::Table)) {
        LUA->ThrowError("Expected table for base light info");
        return 0;
    }
    
    if (!LUA->IsType(2, Type::Table)) {
        LUA->ThrowError("Expected table for distant info");
        return 0;
    }
    
    remix::LightInfo baseInfo = LuaToLightInfo(LUA, 1);
    remix::LightInfoDistantEXT distantInfo = LuaToDistantInfo(LUA, 2);
    
    uint64_t entityID = 0;
    if (LUA->IsType(3, Type::Number)) {
        entityID = static_cast<uint64_t>(LUA->GetNumber(3));
    }
    
    auto& lightManager = RemixAPI::Instance().GetLightManager();
    uint64_t lightId = lightManager.CreateDistantLight(baseInfo, distantInfo, entityID);
    
    LUA->PushNumber(static_cast<double>(lightId));
    return 1;
}

// Lua function: RemixLight.UpdateDistant(baseInfo, distantInfo, lightId)
LUA_FUNCTION(RemixLight_UpdateDistant) {
    if (!LUA->IsType(1, Type::Table)) {
        LUA->ThrowError("Expected table for base light info");
        return 0;
    }
    if (!LUA->IsType(2, Type::Table)) {
        LUA->ThrowError("Expected table for distant info");
        return 0;
    }
    if (!LUA->IsType(3, Type::Number)) {
        LUA->ThrowError("Expected number for light ID");
        return 0;
    }
    remix::LightInfo baseInfo = LuaToLightInfo(LUA, 1);
    remix::LightInfoDistantEXT distantInfo = LuaToDistantInfo(LUA, 2);
    uint64_t lightId = static_cast<uint64_t>(LUA->GetNumber(3));
    bool ok = RemixAPI::Instance().GetLightManager().UpdateDistantLight(lightId, baseInfo, distantInfo);
    LUA->PushBool(ok);
    return 1;
}

// Lua function: RemixLight.DestroyLight(lightId)
LUA_FUNCTION(RemixLight_DestroyLight) {
    if (!LUA->IsType(1, Type::Number)) {
        LUA->ThrowError("Expected number for light ID");
        return 0;
    }
    
    uint64_t lightId = static_cast<uint64_t>(LUA->GetNumber(1));
    
    auto& lightManager = RemixAPI::Instance().GetLightManager();
    bool result = lightManager.DestroyLight(lightId);
    
    LUA->PushBool(result);
    return 1;
}

// Lua function: RemixLight.HasLight(lightId)
LUA_FUNCTION(RemixLight_HasLight) {
    if (!LUA->IsType(1, Type::Number)) {
        LUA->ThrowError("Expected number for light ID");
        return 0;
    }
    
    uint64_t lightId = static_cast<uint64_t>(LUA->GetNumber(1));
    
    auto& lightManager = RemixAPI::Instance().GetLightManager();
    bool result = lightManager.HasLight(lightId);
    
    LUA->PushBool(result);
    return 1;
}

// Lua function: RemixLight.HasLightForEntity(entityID)
LUA_FUNCTION(RemixLight_HasLightForEntity) {
    if (!LUA->IsType(1, Type::Number)) {
        LUA->ThrowError("Expected number for entity ID");
        return 0;
    }
    
    uint64_t entityID = static_cast<uint64_t>(LUA->GetNumber(1));
    
    auto& lightManager = RemixAPI::Instance().GetLightManager();
    bool result = lightManager.HasLightForEntity(entityID);
    
    LUA->PushBool(result);
    return 1;
}

// Lua function: RemixLight.DestroyLightsForEntity(entityID)
LUA_FUNCTION(RemixLight_DestroyLightsForEntity) {
    if (!LUA->IsType(1, Type::Number)) {
        LUA->ThrowError("Expected number for entity ID");
        return 0;
    }
    
    uint64_t entityID = static_cast<uint64_t>(LUA->GetNumber(1));
    
    auto& lightManager = RemixAPI::Instance().GetLightManager();
    lightManager.DestroyLightsForEntity(entityID);
    
    LUA->PushBool(true);
    return 1;
}

// Lua function: RemixLight.GetLightCount()
LUA_FUNCTION(RemixLight_GetLightCount) {
    auto& lightManager = RemixAPI::Instance().GetLightManager();
    size_t count = lightManager.GetLightCount();
    
    LUA->PushNumber(static_cast<double>(count));
    return 1;
}

// Lua function: RemixLight.ClearAllLights()
LUA_FUNCTION(RemixLight_ClearAllLights) {
    auto& lightManager = RemixAPI::Instance().GetLightManager();
    lightManager.ClearAllLights();
    
    LUA->PushBool(true);
    return 1;
}

// Initialize Light Manager Lua bindings
void LightManager::InitializeLuaBindings() {
    if (!m_lua) return;
    
    // Get the global table
    m_lua->PushSpecial(GarrysMod::Lua::SPECIAL_GLOB);
    
    // Create RemixLight table
    m_lua->CreateTable();
    
    // Light creation functions
    m_lua->PushCFunction(RemixLight_CreateSphere);
    m_lua->SetField(-2, "CreateSphere");
    m_lua->PushCFunction(RemixLight_UpdateSphere);
    m_lua->SetField(-2, "UpdateSphere");
    
    m_lua->PushCFunction(RemixLight_CreateRect);
    m_lua->SetField(-2, "CreateRect");
    m_lua->PushCFunction(RemixLight_UpdateRect);
    m_lua->SetField(-2, "UpdateRect");
    
    m_lua->PushCFunction(RemixLight_CreateDisk);
    m_lua->SetField(-2, "CreateDisk");
    m_lua->PushCFunction(RemixLight_UpdateDisk);
    m_lua->SetField(-2, "UpdateDisk");
    
    m_lua->PushCFunction(RemixLight_CreateDistant);
    m_lua->SetField(-2, "CreateDistant");
    m_lua->PushCFunction(RemixLight_UpdateDistant);
    m_lua->SetField(-2, "UpdateDistant");
    
    // Light management functions
    m_lua->PushCFunction(RemixLight_DestroyLight);
    m_lua->SetField(-2, "DestroyLight");
    
    m_lua->PushCFunction(RemixLight_HasLight);
    m_lua->SetField(-2, "HasLight");
    
    // Entity-based functions
    m_lua->PushCFunction(RemixLight_HasLightForEntity);
    m_lua->SetField(-2, "HasLightForEntity");
    
    m_lua->PushCFunction(RemixLight_DestroyLightsForEntity);
    m_lua->SetField(-2, "DestroyLightsForEntity");
    
    // Utility functions
    m_lua->PushCFunction(RemixLight_GetLightCount);
    m_lua->SetField(-2, "GetLightCount");
    
    m_lua->PushCFunction(RemixLight_ClearAllLights);
    m_lua->SetField(-2, "ClearAllLights");

    // No per-frame submission needed with internal auto-instancing
    
    // Set the table as a global field
    m_lua->SetField(-2, "RemixLight");
    
    // Pop the global table
    m_lua->Pop();
    
    Msg("[LightManager] Lua bindings initialized\n");
}

} // namespace RemixAPI

#endif // _WIN64 