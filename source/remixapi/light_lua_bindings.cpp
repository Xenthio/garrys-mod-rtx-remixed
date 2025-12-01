#ifdef _WIN64
#include "remixapi.h"
#include <tier0/dbg.h>

using namespace GarrysMod::Lua;
// Helpers: push LightInfo to Lua table
static void PushLightInfoToLua(ILuaBase* LUA, const remix::LightInfo& info) {
    LUA->CreateTable();
    LUA->PushNumber(static_cast<double>(info.hash)); LUA->SetField(-2, "hash");
    LUA->CreateTable();
    LUA->PushNumber(info.radiance.x); LUA->SetField(-2, "x");
    LUA->PushNumber(info.radiance.y); LUA->SetField(-2, "y");
    LUA->PushNumber(info.radiance.z); LUA->SetField(-2, "z");
    LUA->SetField(-2, "radiance");
}

static void PushSphereInfoToLua(ILuaBase* LUA, const remix::LightInfoSphereEXT& info) {
    LUA->CreateTable();
    LUA->CreateTable();
    LUA->PushNumber(info.position.x); LUA->SetField(-2, "x");
    LUA->PushNumber(info.position.y); LUA->SetField(-2, "y");
    LUA->PushNumber(info.position.z); LUA->SetField(-2, "z");
    LUA->SetField(-2, "position");
    LUA->PushNumber(info.radius); LUA->SetField(-2, "radius");
    // shaping
    LUA->CreateTable();
    LUA->CreateTable();
    LUA->PushNumber(info.shaping_value.direction.x); LUA->SetField(-2, "x");
    LUA->PushNumber(info.shaping_value.direction.y); LUA->SetField(-2, "y");
    LUA->PushNumber(info.shaping_value.direction.z); LUA->SetField(-2, "z");
    LUA->SetField(-2, "direction");
    LUA->PushNumber(info.shaping_value.coneAngleDegrees); LUA->SetField(-2, "coneAngleDegrees");
    LUA->PushNumber(info.shaping_value.coneSoftness); LUA->SetField(-2, "coneSoftness");
    LUA->PushNumber(info.shaping_value.focusExponent); LUA->SetField(-2, "focusExponent");
    LUA->SetField(-2, "shaping");
    LUA->PushNumber(info.volumetricRadianceScale); LUA->SetField(-2, "volumetricRadianceScale");
}


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
    
    // Get isDynamic flag (optional, defaults to false for static behavior)
    LUA->GetField(index, "isDynamic");
    if (LUA->IsType(-1, Type::Bool)) {
        info.isDynamic = LUA->GetBool(-1);
    }
    LUA->Pop();
    
    // Get ignoreViewModel flag (optional, defaults to false)
    LUA->GetField(index, "ignoreViewModel");
    if (LUA->IsType(-1, Type::Bool)) {
        info.ignoreViewModel = LUA->GetBool(-1);
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

// Helper function to extract LightInfoCylinderEXT from Lua table
static remix::LightInfoCylinderEXT LuaToCylinderInfo(ILuaBase* LUA, int index) {
    remix::LightInfoCylinderEXT info;
    if (!LUA->IsType(index, Type::Table)) { LUA->ThrowError("Expected table for CylinderInfo"); return info; }
    // position
    LUA->GetField(index, "position"); if (LUA->IsType(-1, Type::Table)) { LUA->GetField(-1, "x"); if (LUA->IsType(-1, Type::Number)) info.position.x = (float)LUA->GetNumber(-1); LUA->Pop(); LUA->GetField(-1, "y"); if (LUA->IsType(-1, Type::Number)) info.position.y = (float)LUA->GetNumber(-1); LUA->Pop(); LUA->GetField(-1, "z"); if (LUA->IsType(-1, Type::Number)) info.position.z = (float)LUA->GetNumber(-1); LUA->Pop(); } LUA->Pop();
    // radius
    LUA->GetField(index, "radius"); if (LUA->IsType(-1, Type::Number)) info.radius = (float)LUA->GetNumber(-1); LUA->Pop();
    // axis
    LUA->GetField(index, "axis"); if (LUA->IsType(-1, Type::Table)) { LUA->GetField(-1, "x"); if (LUA->IsType(-1, Type::Number)) info.axis.x = (float)LUA->GetNumber(-1); LUA->Pop(); LUA->GetField(-1, "y"); if (LUA->IsType(-1, Type::Number)) info.axis.y = (float)LUA->GetNumber(-1); LUA->Pop(); LUA->GetField(-1, "z"); if (LUA->IsType(-1, Type::Number)) info.axis.z = (float)LUA->GetNumber(-1); LUA->Pop(); } LUA->Pop();
    // axisLength
    LUA->GetField(index, "axisLength"); if (LUA->IsType(-1, Type::Number)) info.axisLength = (float)LUA->GetNumber(-1); LUA->Pop();
    // volumetricRadianceScale
    LUA->GetField(index, "volumetricRadianceScale"); if (LUA->IsType(-1, Type::Number)) info.volumetricRadianceScale = (float)LUA->GetNumber(-1); LUA->Pop();
    return info;
}

// Helper function to extract LightInfoDomeEXT from Lua table
static remix::LightInfoDomeEXT LuaToDomeInfo(ILuaBase* LUA, int index) {
    remix::LightInfoDomeEXT info;
    if (!LUA->IsType(index, Type::Table)) { LUA->ThrowError("Expected table for DomeInfo"); return info; }
    // transform (3x4 matrix) as { matrix = { {a,b,c,d}, {..}, {..} } } optional; default identity
    // Keep default unless provided
    // colorTexture: string path
    LUA->GetField(index, "colorTexture"); if (LUA->IsType(-1, Type::String)) { std::filesystem::path p = LUA->GetString(-1); info.set_colorTexture(p); } LUA->Pop();
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
    
    if (lightId == 0) {
        Warning("[RemixLight] CreateSphere: Failed to create light (API may not be initialized or handle creation failed)\n");
    }
    
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
    
    if (!ok) {
        Warning("[RemixLight] UpdateSphere: Failed to update light ID %llu (light may not exist or API call failed)\n", lightId);
    }
    
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

// Lua function: RemixLight.CreateCylinder(baseInfo, cylinderInfo, entityID)
LUA_FUNCTION(RemixLight_CreateCylinder) {
    if (!LUA->IsType(1, Type::Table)) { LUA->ThrowError("Expected table for base light info"); return 0; }
    if (!LUA->IsType(2, Type::Table)) { LUA->ThrowError("Expected table for cylinder info"); return 0; }
    remix::LightInfo baseInfo = LuaToLightInfo(LUA, 1);
    remix::LightInfoCylinderEXT cylInfo = LuaToCylinderInfo(LUA, 2);
    uint64_t entityID = 0; if (LUA->IsType(3, Type::Number)) entityID = (uint64_t)LUA->GetNumber(3);
    auto& lm = RemixAPI::Instance().GetLightManager();
    uint64_t id = lm.CreateCylinderLight(baseInfo, cylInfo, entityID);
    LUA->PushNumber((double)id); return 1;
}

// Lua function: RemixLight.UpdateCylinder(baseInfo, cylinderInfo, lightId)
LUA_FUNCTION(RemixLight_UpdateCylinder) {
    if (!LUA->IsType(1, Type::Table)) { LUA->ThrowError("Expected table for base light info"); return 0; }
    if (!LUA->IsType(2, Type::Table)) { LUA->ThrowError("Expected table for cylinder info"); return 0; }
    if (!LUA->IsType(3, Type::Number)) { LUA->ThrowError("Expected number for light ID"); return 0; }
    remix::LightInfo baseInfo = LuaToLightInfo(LUA, 1);
    remix::LightInfoCylinderEXT cylInfo = LuaToCylinderInfo(LUA, 2);
    uint64_t id = (uint64_t)LUA->GetNumber(3);
    bool ok = RemixAPI::Instance().GetLightManager().UpdateCylinderLight(id, baseInfo, cylInfo);
    LUA->PushBool(ok); return 1;
}

// Lua function: RemixLight.CreateDome(baseInfo, domeInfo, entityID)
LUA_FUNCTION(RemixLight_CreateDome) {
    if (!LUA->IsType(1, Type::Table)) { LUA->ThrowError("Expected table for base light info"); return 0; }
    if (!LUA->IsType(2, Type::Table)) { LUA->ThrowError("Expected table for dome info"); return 0; }
    remix::LightInfo baseInfo = LuaToLightInfo(LUA, 1);
    remix::LightInfoDomeEXT domeInfo = LuaToDomeInfo(LUA, 2);
    uint64_t entityID = 0; if (LUA->IsType(3, Type::Number)) entityID = (uint64_t)LUA->GetNumber(3);
    auto& lm = RemixAPI::Instance().GetLightManager();
    uint64_t id = lm.CreateDomeLight(baseInfo, domeInfo, entityID);
    LUA->PushNumber((double)id); return 1;
}

// Lua function: RemixLight.UpdateDome(baseInfo, domeInfo, lightId)
LUA_FUNCTION(RemixLight_UpdateDome) {
    if (!LUA->IsType(1, Type::Table)) { LUA->ThrowError("Expected table for base light info"); return 0; }
    if (!LUA->IsType(2, Type::Table)) { LUA->ThrowError("Expected table for dome info"); return 0; }
    if (!LUA->IsType(3, Type::Number)) { LUA->ThrowError("Expected number for light ID"); return 0; }
    remix::LightInfo baseInfo = LuaToLightInfo(LUA, 1);
    remix::LightInfoDomeEXT domeInfo = LuaToDomeInfo(LUA, 2);
    uint64_t id = (uint64_t)LUA->GetNumber(3);
    bool ok = RemixAPI::Instance().GetLightManager().UpdateDomeLight(id, baseInfo, domeInfo);
    LUA->PushBool(ok); return 1;
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

// Lua function: RemixLight.GetSphereState(lightId) -> baseTable, sphereTable or nil
LUA_FUNCTION(RemixLight_GetSphereState) {
    if (!LUA->IsType(1, Type::Number)) { LUA->ThrowError("Expected number for light ID"); return 0; }
    uint64_t lightId = static_cast<uint64_t>(LUA->GetNumber(1));
    auto& lm = RemixAPI::Instance().GetLightManager();
    remix::LightInfo base{}; remix::LightInfoSphereEXT sphere{};
    if (!lm.GetSphereState(lightId, base, sphere)) { LUA->PushNil(); return 1; }
    PushLightInfoToLua(LUA, base);
    PushSphereInfoToLua(LUA, sphere);
    return 2;
}

// Lua function: RemixLight.GetLightsForEntity(entityID)
LUA_FUNCTION(RemixLight_GetLightsForEntity) {
    if (!LUA->IsType(1, Type::Number)) {
        LUA->ThrowError("Expected number for entity ID");
        return 0;
    }
    uint64_t entityID = static_cast<uint64_t>(LUA->GetNumber(1));
    auto& lm = RemixAPI::Instance().GetLightManager();
    auto ids = lm.GetLightsForEntity(entityID);
    LUA->CreateTable();
    int idx = 1;
    for (auto id : ids) {
        LUA->PushNumber((double)idx++);
        LUA->PushNumber((double)id);
        LUA->SetTable(-3);
    }
    return 1;
}

// Lua function: RemixLight.GetAllLightIds()
LUA_FUNCTION(RemixLight_GetAllLightIds) {
    auto& lm = RemixAPI::Instance().GetLightManager();
    auto ids = lm.GetAllLightIds();
    LUA->CreateTable();
    int idx = 1;
    for (auto id : ids) {
        LUA->PushNumber((double)idx++);
        LUA->PushNumber((double)id);
        LUA->SetTable(-3);
    }
    return 1;
}

// Lua function: RemixLight.UpdateSphereFields(lightId, fields)
// fields can contain { radiance={x,y,z}, position={x,y,z}, radius=number, shaping={direction, coneAngleDegrees, coneSoftness, focusExponent}, volumetricRadianceScale }
LUA_FUNCTION(RemixLight_UpdateSphereFields) {
    if (!LUA->IsType(1, Type::Number)) { LUA->ThrowError("Expected number for light ID"); return 0; }
    if (!LUA->IsType(2, Type::Table)) { LUA->ThrowError("Expected table for fields"); return 0; }
    uint64_t lightId = static_cast<uint64_t>(LUA->GetNumber(1));
    auto& lm = RemixAPI::Instance().GetLightManager();
    remix::LightInfo base{}; remix::LightInfoSphereEXT sphere{};
    if (!lm.GetSphereState(lightId, base, sphere)) { LUA->PushBool(false); return 1; }
    // Merge fields into cached state
    auto mergeVec = [&](int idx, remixapi_Float3D& dst){ LUA->GetField(idx, "x"); if (LUA->IsType(-1, Type::Number)) dst.x = (float)LUA->GetNumber(-1); LUA->Pop(); LUA->GetField(idx, "y"); if (LUA->IsType(-1, Type::Number)) dst.y = (float)LUA->GetNumber(-1); LUA->Pop(); LUA->GetField(idx, "z"); if (LUA->IsType(-1, Type::Number)) dst.z = (float)LUA->GetNumber(-1); LUA->Pop(); };
    // radiance
    LUA->GetField(2, "radiance"); if (LUA->IsType(-1, Type::Table)) { mergeVec(-1, base.radiance); } LUA->Pop();
    // position
    LUA->GetField(2, "position"); if (LUA->IsType(-1, Type::Table)) { mergeVec(-1, sphere.position); } LUA->Pop();
    // radius
    LUA->GetField(2, "radius"); if (LUA->IsType(-1, Type::Number)) { sphere.radius = (float)LUA->GetNumber(-1); } LUA->Pop();
    // volumetricRadianceScale
    LUA->GetField(2, "volumetricRadianceScale"); if (LUA->IsType(-1, Type::Number)) { sphere.volumetricRadianceScale = (float)LUA->GetNumber(-1); } LUA->Pop();
    // shaping
    LUA->GetField(2, "shaping");
    if (LUA->IsType(-1, Type::Table)) {
        remix::LightInfoLightShaping shaping = sphere.shaping_value;
        LUA->GetField(-1, "direction"); if (LUA->IsType(-1, Type::Table)) { mergeVec(-1, shaping.direction); } LUA->Pop();
        LUA->GetField(-1, "coneAngleDegrees"); if (LUA->IsType(-1, Type::Number)) shaping.coneAngleDegrees = (float)LUA->GetNumber(-1); LUA->Pop();
        LUA->GetField(-1, "coneSoftness"); if (LUA->IsType(-1, Type::Number)) shaping.coneSoftness = (float)LUA->GetNumber(-1); LUA->Pop();
        LUA->GetField(-1, "focusExponent"); if (LUA->IsType(-1, Type::Number)) shaping.focusExponent = (float)LUA->GetNumber(-1); LUA->Pop();
        sphere.set_shaping(shaping);
    }
    LUA->Pop();
    bool ok = lm.ApplySphereState(lightId, base, sphere);
    LUA->PushBool(ok);
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
    m_lua->PushCFunction(RemixLight_CreateCylinder);
    m_lua->SetField(-2, "CreateCylinder");
    m_lua->PushCFunction(RemixLight_UpdateCylinder);
    m_lua->SetField(-2, "UpdateCylinder");
    m_lua->PushCFunction(RemixLight_CreateDome);
    m_lua->SetField(-2, "CreateDome");
    m_lua->PushCFunction(RemixLight_UpdateDome);
    m_lua->SetField(-2, "UpdateDome");
    
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
    m_lua->PushCFunction(RemixLight_GetLightsForEntity);
    m_lua->SetField(-2, "GetLightsForEntity");
    m_lua->PushCFunction(RemixLight_GetAllLightIds);
    m_lua->SetField(-2, "GetAllLightIds");
    m_lua->PushCFunction(RemixLight_UpdateSphereFields);
    m_lua->SetField(-2, "UpdateSphereFields");
    
    // Utility functions
    m_lua->PushCFunction(RemixLight_GetLightCount);
    m_lua->SetField(-2, "GetLightCount");
    
    m_lua->PushCFunction(RemixLight_ClearAllLights);
    m_lua->SetField(-2, "ClearAllLights");
    m_lua->PushCFunction(RemixLight_GetSphereState);
    m_lua->SetField(-2, "GetSphereState");

    // No per-frame submission needed with internal auto-instancing
    
    // Set the table as a global field
    m_lua->SetField(-2, "RemixLight");
    
    // Pop the global table
    m_lua->Pop();
    
#ifdef _DEBUG
    Msg("[LightManager] Lua bindings initialized\n");
#endif
}

} // namespace RemixAPI

#endif // _WIN64 