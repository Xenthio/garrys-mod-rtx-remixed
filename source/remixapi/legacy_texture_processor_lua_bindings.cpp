#ifdef _WIN64

#include "legacy_texture_processor.h"
#include <GarrysMod/Lua/Interface.h>
#include <tier0/dbg.h>

using namespace GarrysMod::Lua;

namespace LegacyTextureProcessor {

// =========================================================================
// Lua Function Implementations
// =========================================================================

LUA_FUNCTION(LegacyTextureProcessor_Initialize) {
    // If already initialized (by C++ during RemixAPI init), return success
    if (TextureProcessor::Instance().IsInitialized()) {
        LUA->PushBool(true);
        return 1;
    }
    
    // Not initialized yet - need g_remix to initialize
    extern remix::Interface* g_remix;
    if (!g_remix) {
        LUA->PushBool(false);
        return 1;
    }
    
    bool result = TextureProcessor::Instance().Initialize(g_remix);
    LUA->PushBool(result);
    return 1;
}

LUA_FUNCTION(LegacyTextureProcessor_IsInitialized) {
    LUA->PushBool(TextureProcessor::Instance().IsInitialized());
    return 1;
}

LUA_FUNCTION(LegacyTextureProcessor_ProcessAllMaterials) {
    int count = TextureProcessor::Instance().ProcessAllTrackedMaterials();
    LUA->PushNumber(count);
    return 1;
}

LUA_FUNCTION(LegacyTextureProcessor_ProcessMaterialsBatch) {
    int maxBatch = 5; // Default batch size
    if (LUA->IsType(1, Type::Number)) {
        maxBatch = (int)LUA->GetNumber(1);
    }
    
    int count = TextureProcessor::Instance().ProcessTrackedMaterialsBatch(maxBatch);
    LUA->PushNumber(count);
    return 1;
}

LUA_FUNCTION(LegacyTextureProcessor_SetAutoProcessing) {
    if (!LUA->IsType(1, Type::Bool)) {
        LUA->ThrowError("Expected boolean for auto processing");
        return 0;
    }
    
    TextureProcessor::Instance().SetAutoProcessing(LUA->GetBool(1));
    return 0;
}

LUA_FUNCTION(LegacyTextureProcessor_SetDebugOutput) {
    if (!LUA->IsType(1, Type::Bool)) {
        LUA->ThrowError("Expected boolean for debug output");
        return 0;
    }
    
    TextureProcessor::Instance().SetDebugOutput(LUA->GetBool(1));
    return 0;
}

LUA_FUNCTION(LegacyTextureProcessor_SetMetallicGeneration) {
    if (!LUA->IsType(1, Type::Bool)) {
        LUA->ThrowError("Expected boolean for metallic generation");
        return 0;
    }
    
    bool enabled = LUA->GetBool(1);
    TextureProcessor::Instance().SetMetallicGeneration(enabled);
    
    if (enabled) {
        Msg("[LegacyTextureProcessor] Experimental metallic generation ENABLED\n");
        Msg("[LegacyTextureProcessor] WARNING: This may cause dark envmap materials to appear black.\n");
        Msg("[LegacyTextureProcessor] In PBR, metallic surfaces reflect their base color - black base = no reflections.\n");
    } else {
        Msg("[LegacyTextureProcessor] Metallic generation DISABLED (default)\n");
        Msg("[LegacyTextureProcessor] Dark envmap materials will use low roughness for reflections instead.\n");
    }
    return 0;
}

LUA_FUNCTION(LegacyTextureProcessor_IsMetallicGenerationEnabled) {
    LUA->PushBool(TextureProcessor::Instance().IsMetallicGenerationEnabled());
    return 1;
}

LUA_FUNCTION(LegacyTextureProcessor_SetAutoDiscover) {
    if (!LUA->IsType(1, Type::Bool)) {
        LUA->ThrowError("Expected boolean for auto-discover");
        return 0;
    }
    
    bool enabled = LUA->GetBool(1);
    TextureProcessor::Instance().SetAutoDiscover(enabled);
    
    if (enabled) {
        Msg("[LegacyTextureProcessor] Texture auto-discovery ENABLED (default)\n");
        Msg("[LegacyTextureProcessor] Will search for companion textures like _normal, _mask, _spec\n");
    } else {
        Msg("[LegacyTextureProcessor] Texture auto-discovery DISABLED\n");
        Msg("[LegacyTextureProcessor] Only explicitly referenced textures will be used\n");
    }
    return 0;
}

LUA_FUNCTION(LegacyTextureProcessor_IsAutoDiscoverEnabled) {
    LUA->PushBool(TextureProcessor::Instance().IsAutoDiscoverEnabled());
    return 1;
}

LUA_FUNCTION(LegacyTextureProcessor_SetParseCommentedProperties) {
    if (!LUA->IsType(1, Type::Bool)) {
        LUA->ThrowError("Expected boolean for parse commented properties");
        return 0;
    }
    
    bool enabled = LUA->GetBool(1);
    TextureProcessor::Instance().SetParseCommentedProperties(enabled);
    
    if (enabled) {
        Msg("[LegacyTextureProcessor] Parsing commented-out VMT properties ENABLED\n");
        Msg("[LegacyTextureProcessor] Will parse // commented properties like $envmap, $normalmapalphaenvmapmask\n");
        Msg("[LegacyTextureProcessor] Useful for maps where these were disabled for vanilla Source performance\n");
    } else {
        Msg("[LegacyTextureProcessor] Parsing commented-out VMT properties DISABLED (default)\n");
        Msg("[LegacyTextureProcessor] Respects author intent - commented properties will be ignored\n");
    }
    return 0;
}

LUA_FUNCTION(LegacyTextureProcessor_IsParseCommentedPropertiesEnabled) {
    LUA->PushBool(TextureProcessor::Instance().IsParseCommentedPropertiesEnabled());
    return 1;
}

LUA_FUNCTION(LegacyTextureProcessor_GetStats) {
    auto stats = TextureProcessor::Instance().GetStats();
    
    LUA->CreateTable();
    
    LUA->PushNumber(stats.materialsProcessed);
    LUA->SetField(-2, "materialsProcessed");
    
    LUA->PushNumber(stats.texturesUploaded);
    LUA->SetField(-2, "texturesUploaded");
    
    LUA->PushNumber(stats.materialsWithNormals);
    LUA->SetField(-2, "materialsWithNormals");
    
    LUA->PushNumber(stats.materialsWithRoughness);
    LUA->SetField(-2, "materialsWithRoughness");
    
    LUA->PushNumber(stats.failedConversions);
    LUA->SetField(-2, "failedConversions");
    
    return 1;
}

LUA_FUNCTION(LegacyTextureProcessor_ClearCache) {
    TextureProcessor::Instance().ClearCache();
    return 0;
}

LUA_FUNCTION(LegacyTextureProcessor_ConvertTexture) {
    if (!LUA->IsType(1, Type::String)) {
        LUA->ThrowError("Expected string for texture path");
        return 0;
    }
    
    const char* path = LUA->GetString(1);
    bool isNormalMap = LUA->IsType(2, Type::Bool) ? LUA->GetBool(2) : false;
    
    uint64_t hash = TextureProcessor::Instance().ConvertAndUploadTexture(path, isNormalMap);
    
    // Return hash as string to preserve precision
    char hashStr[32];
    sprintf_s(hashStr, "0x%llX", hash);
    LUA->PushString(hashStr);
    
    return 1;
}

LUA_FUNCTION(LegacyTextureProcessor_InspectMaterial) {
    if (!LUA->IsType(1, Type::String)) {
        LUA->ThrowError("Expected string for material name");
        return 0;
    }
    
    const char* matName = LUA->GetString(1);
    
    MaterialPBRProperties props;
    if (!TextureProcessor::Instance().ExtractMaterialPBR(matName, props)) {
        LUA->PushNil();
        return 1;
    }
    
    // Create a table with material properties
    LUA->CreateTable();
    
    LUA->PushString(props.materialName.c_str());
    LUA->SetField(-2, "materialName");
    
    LUA->PushString(props.shaderName.c_str());
    LUA->SetField(-2, "shaderName");
    
    LUA->PushString(props.baseTexturePath.c_str());
    LUA->SetField(-2, "baseTexture");
    
    if (props.hasBumpMap) {
        LUA->PushString(props.bumpMapPath.c_str());
        LUA->SetField(-2, "bumpMap");
    }
    
    if (props.hasEnvMapMask) {
        LUA->PushString(props.envMapMaskPath.c_str());
        LUA->SetField(-2, "envMapMask");
    }
    
    if (props.hasPhongExponentTexture) {
        LUA->PushString(props.phongExponentTexturePath.c_str());
        LUA->SetField(-2, "phongExponentTexture");
    }
    
    LUA->PushBool(props.hasPhong);
    LUA->SetField(-2, "hasPhong");
    
    if (props.hasPhong) {
        LUA->PushNumber(props.phongExponent);
        LUA->SetField(-2, "phongExponent");
        
        LUA->PushNumber(props.phongBoost);
        LUA->SetField(-2, "phongBoost");
    }
    
    LUA->PushNumber(props.roughness);
    LUA->SetField(-2, "roughness");
    
    LUA->PushNumber(props.metallic);
    LUA->SetField(-2, "metallic");
    
    LUA->PushBool(props.isSelfIllum);
    LUA->SetField(-2, "isSelfIllum");
    
    LUA->PushBool(props.isTranslucent);
    LUA->SetField(-2, "isTranslucent");
    
    LUA->PushBool(props.isGlass);
    LUA->SetField(-2, "isGlass");
    
    LUA->PushBool(props.isRefractShader);
    LUA->SetField(-2, "isRefractShader");
    
    // Community PBR formats
    LUA->PushBool(props.isExoPBR);
    LUA->SetField(-2, "isExoPBR");
    
    LUA->PushBool(props.isGPBR);
    LUA->SetField(-2, "isGPBR");
    
    LUA->PushBool(props.isBFTPseudoPBR);
    LUA->SetField(-2, "isBFTPseudoPBR");
    
    LUA->PushBool(props.isMWBPBR);
    LUA->SetField(-2, "isMWBPBR");
    
    // ExoPBR-specific
    if (props.isExoPBR) {
        if (props.hasARMTexture) {
            LUA->PushString(props.armTexturePath.c_str());
            LUA->SetField(-2, "armTexture");
        }
        if (props.hasExoNormal) {
            LUA->PushString(props.exoNormalPath.c_str());
            LUA->SetField(-2, "exoNormal");
        }
        if (props.hasEmissionTexture) {
            LUA->PushString(props.emissionTexturePath.c_str());
            LUA->SetField(-2, "emissionTexture");
        }
        if (props.hasEmissionScale) {
            LUA->PushNumber(props.emissionScale);
            LUA->SetField(-2, "emissionScale");
        }
    }
    
    // GPBR-specific
    if (props.isGPBR) {
        if (props.hasMRAOTexture) {
            LUA->PushString(props.mraoTexturePath.c_str());
            LUA->SetField(-2, "mraoTexture");
        }
        if (props.hasGPBREmission) {
            LUA->PushString(props.gpbrEmissionPath.c_str());
            LUA->SetField(-2, "gpbrEmission");
        }
    }
    
    // BFT-specific
    if (props.isBFTPseudoPBR) {
        LUA->PushBool(props.isBFTMetallicLayer);
        LUA->SetField(-2, "isBFTMetallicLayer");
        
        if (props.hasBFTExponentTexture) {
            LUA->PushString(props.bftExponentTexturePath.c_str());
            LUA->SetField(-2, "bftExponentTexture");
        }
    }
    
    // Auto-discovered textures
    if (props.hasDiscoveredNormal) {
        LUA->PushString(props.discoveredNormalPath.c_str());
        LUA->SetField(-2, "discoveredNormal");
    }
    if (props.hasDiscoveredHeight) {
        LUA->PushString(props.discoveredHeightPath.c_str());
        LUA->SetField(-2, "discoveredHeight");
    }
    if (props.hasDiscoveredMask) {
        LUA->PushString(props.discoveredMaskPath.c_str());
        LUA->SetField(-2, "discoveredMask");
    }
    if (props.hasDiscoveredAO) {
        LUA->PushString(props.discoveredAOPath.c_str());
        LUA->SetField(-2, "discoveredAO");
    }
    
    return 1;
}

LUA_FUNCTION(LegacyTextureProcessor_SetOutputDirectory) {
    if (!LUA->IsType(1, Type::String)) {
        LUA->ThrowError("Expected string for output directory");
        return 0;
    }
    
    const char* path = LUA->GetString(1);
    TextureProcessor::Instance().SetOutputDirectory(path);
    return 0;
}

LUA_FUNCTION(LegacyTextureProcessor_GetOutputDirectory) {
    std::string dir = TextureProcessor::Instance().GetOutputDirectory();
    LUA->PushString(dir.c_str());
    return 1;
}

LUA_FUNCTION(LegacyTextureProcessor_ProcessSingleMaterial) {
    if (!LUA->IsType(1, Type::String)) {
        LUA->ThrowError("Expected string for material name");
        return 0;
    }
    
    const char* matName = LUA->GetString(1);
    bool result = TextureProcessor::Instance().ProcessSingleMaterial(matName);
    LUA->PushBool(result);
    return 1;
}

LUA_FUNCTION(LegacyTextureProcessor_WriteUSDAIfNeeded) {
    TextureProcessor::Instance().WriteUSDAIfNeeded();
    return 0;
}

LUA_FUNCTION(LegacyTextureProcessor_NeedsUSDAUpdate) {
    bool needs = TextureProcessor::Instance().NeedsUSDAUpdate();
    LUA->PushBool(needs);
    return 1;
}

// Background processing functions

LUA_FUNCTION(LegacyTextureProcessor_QueueMaterialsForProcessing) {
    int count = TextureProcessor::Instance().QueueMaterialsForProcessing();
    LUA->PushNumber(count);
    return 1;
}

LUA_FUNCTION(LegacyTextureProcessor_IsProcessingInBackground) {
    bool isProcessing = TextureProcessor::Instance().IsProcessingInBackground();
    LUA->PushBool(isProcessing);
    return 1;
}

LUA_FUNCTION(LegacyTextureProcessor_GetQueuedMaterialCount) {
    size_t count = TextureProcessor::Instance().GetQueuedMaterialCount();
    LUA->PushNumber((double)count);
    return 1;
}

LUA_FUNCTION(LegacyTextureProcessor_GetLastProcessedCount) {
    int count = TextureProcessor::Instance().GetLastProcessedCount();
    LUA->PushNumber(count);
    return 1;
}

LUA_FUNCTION(LegacyTextureProcessor_AppendToUSDAAsync) {
    TextureProcessor::Instance().AppendToUSDAAsync();
    return 0;
}

// =========================================================================
// Lua Bindings Initialization
// =========================================================================

void InitializeLegacyTextureProcessorLuaBindings(GarrysMod::Lua::ILuaBase* LUA) {
    // Create LegacyTextureProcessor table
    LUA->PushSpecial(SPECIAL_GLOB);
    LUA->CreateTable();
    
    LUA->PushCFunction(LegacyTextureProcessor_Initialize);
    LUA->SetField(-2, "Initialize");
    
    LUA->PushCFunction(LegacyTextureProcessor_IsInitialized);
    LUA->SetField(-2, "IsInitialized");
    
    LUA->PushCFunction(LegacyTextureProcessor_ProcessAllMaterials);
    LUA->SetField(-2, "ProcessAllMaterials");
    
    LUA->PushCFunction(LegacyTextureProcessor_ProcessMaterialsBatch);
    LUA->SetField(-2, "ProcessMaterialsBatch");
    
    LUA->PushCFunction(LegacyTextureProcessor_ProcessSingleMaterial);
    LUA->SetField(-2, "ProcessSingleMaterial");
    
    LUA->PushCFunction(LegacyTextureProcessor_SetAutoProcessing);
    LUA->SetField(-2, "SetAutoProcessing");
    
    LUA->PushCFunction(LegacyTextureProcessor_SetDebugOutput);
    LUA->SetField(-2, "SetDebugOutput");
    
    LUA->PushCFunction(LegacyTextureProcessor_SetMetallicGeneration);
    LUA->SetField(-2, "SetMetallicGeneration");
    
    LUA->PushCFunction(LegacyTextureProcessor_IsMetallicGenerationEnabled);
    LUA->SetField(-2, "IsMetallicGenerationEnabled");
    
    LUA->PushCFunction(LegacyTextureProcessor_SetAutoDiscover);
    LUA->SetField(-2, "SetAutoDiscover");
    
    LUA->PushCFunction(LegacyTextureProcessor_IsAutoDiscoverEnabled);
    LUA->SetField(-2, "IsAutoDiscoverEnabled");
    
    LUA->PushCFunction(LegacyTextureProcessor_SetParseCommentedProperties);
    LUA->SetField(-2, "SetParseCommentedProperties");
    
    LUA->PushCFunction(LegacyTextureProcessor_IsParseCommentedPropertiesEnabled);
    LUA->SetField(-2, "IsParseCommentedPropertiesEnabled");
    
    LUA->PushCFunction(LegacyTextureProcessor_GetStats);
    LUA->SetField(-2, "GetStats");
    
    LUA->PushCFunction(LegacyTextureProcessor_ClearCache);
    LUA->SetField(-2, "ClearCache");
    
    LUA->PushCFunction(LegacyTextureProcessor_ConvertTexture);
    LUA->SetField(-2, "ConvertTexture");
    
    LUA->PushCFunction(LegacyTextureProcessor_InspectMaterial);
    LUA->SetField(-2, "InspectMaterial");
    
    LUA->PushCFunction(LegacyTextureProcessor_SetOutputDirectory);
    LUA->SetField(-2, "SetOutputDirectory");
    
    LUA->PushCFunction(LegacyTextureProcessor_GetOutputDirectory);
    LUA->SetField(-2, "GetOutputDirectory");
    
    LUA->PushCFunction(LegacyTextureProcessor_WriteUSDAIfNeeded);
    LUA->SetField(-2, "WriteUSDAIfNeeded");
    
    LUA->PushCFunction(LegacyTextureProcessor_NeedsUSDAUpdate);
    LUA->SetField(-2, "NeedsUSDAUpdate");
    
    // Background processing
    LUA->PushCFunction(LegacyTextureProcessor_QueueMaterialsForProcessing);
    LUA->SetField(-2, "QueueMaterialsForProcessing");
    
    LUA->PushCFunction(LegacyTextureProcessor_IsProcessingInBackground);
    LUA->SetField(-2, "IsProcessingInBackground");
    
    LUA->PushCFunction(LegacyTextureProcessor_GetQueuedMaterialCount);
    LUA->SetField(-2, "GetQueuedMaterialCount");
    
    LUA->PushCFunction(LegacyTextureProcessor_GetLastProcessedCount);
    LUA->SetField(-2, "GetLastProcessedCount");
    
    LUA->PushCFunction(LegacyTextureProcessor_AppendToUSDAAsync);
    LUA->SetField(-2, "AppendToUSDAAsync");
    
    LUA->SetField(-2, "LegacyTextureProcessor");
    
    // Also create an alias as VTFConverter for backwards compatibility
    LUA->CreateTable();
    
    LUA->PushCFunction(LegacyTextureProcessor_Initialize);
    LUA->SetField(-2, "Initialize");
    
    LUA->PushCFunction(LegacyTextureProcessor_IsInitialized);
    LUA->SetField(-2, "IsInitialized");
    
    LUA->PushCFunction(LegacyTextureProcessor_ProcessAllMaterials);
    LUA->SetField(-2, "ProcessAllMaterials");
    
    LUA->PushCFunction(LegacyTextureProcessor_ProcessMaterialsBatch);
    LUA->SetField(-2, "ProcessMaterialsBatch");
    
    LUA->PushCFunction(LegacyTextureProcessor_ProcessSingleMaterial);
    LUA->SetField(-2, "ProcessSingleMaterial");
    
    LUA->PushCFunction(LegacyTextureProcessor_SetAutoProcessing);
    LUA->SetField(-2, "SetAutoProcessing");
    
    LUA->PushCFunction(LegacyTextureProcessor_SetDebugOutput);
    LUA->SetField(-2, "SetDebugOutput");
    
    LUA->PushCFunction(LegacyTextureProcessor_SetMetallicGeneration);
    LUA->SetField(-2, "SetMetallicGeneration");
    
    LUA->PushCFunction(LegacyTextureProcessor_IsMetallicGenerationEnabled);
    LUA->SetField(-2, "IsMetallicGenerationEnabled");
    
    LUA->PushCFunction(LegacyTextureProcessor_SetAutoDiscover);
    LUA->SetField(-2, "SetAutoDiscover");
    
    LUA->PushCFunction(LegacyTextureProcessor_IsAutoDiscoverEnabled);
    LUA->SetField(-2, "IsAutoDiscoverEnabled");
    
    LUA->PushCFunction(LegacyTextureProcessor_SetParseCommentedProperties);
    LUA->SetField(-2, "SetParseCommentedProperties");
    
    LUA->PushCFunction(LegacyTextureProcessor_IsParseCommentedPropertiesEnabled);
    LUA->SetField(-2, "IsParseCommentedPropertiesEnabled");
    
    LUA->PushCFunction(LegacyTextureProcessor_GetStats);
    LUA->SetField(-2, "GetStats");
    
    LUA->PushCFunction(LegacyTextureProcessor_ClearCache);
    LUA->SetField(-2, "ClearCache");
    
    LUA->PushCFunction(LegacyTextureProcessor_ConvertTexture);
    LUA->SetField(-2, "ConvertTexture");
    
    LUA->PushCFunction(LegacyTextureProcessor_InspectMaterial);
    LUA->SetField(-2, "InspectMaterial");
    
    LUA->PushCFunction(LegacyTextureProcessor_SetOutputDirectory);
    LUA->SetField(-2, "SetOutputDirectory");
    
    LUA->PushCFunction(LegacyTextureProcessor_GetOutputDirectory);
    LUA->SetField(-2, "GetOutputDirectory");
    
    LUA->PushCFunction(LegacyTextureProcessor_WriteUSDAIfNeeded);
    LUA->SetField(-2, "WriteUSDAIfNeeded");
    
    LUA->PushCFunction(LegacyTextureProcessor_NeedsUSDAUpdate);
    LUA->SetField(-2, "NeedsUSDAUpdate");
    
    // Background processing
    LUA->PushCFunction(LegacyTextureProcessor_QueueMaterialsForProcessing);
    LUA->SetField(-2, "QueueMaterialsForProcessing");
    
    LUA->PushCFunction(LegacyTextureProcessor_IsProcessingInBackground);
    LUA->SetField(-2, "IsProcessingInBackground");
    
    LUA->PushCFunction(LegacyTextureProcessor_GetQueuedMaterialCount);
    LUA->SetField(-2, "GetQueuedMaterialCount");
    
    LUA->PushCFunction(LegacyTextureProcessor_GetLastProcessedCount);
    LUA->SetField(-2, "GetLastProcessedCount");
    
    LUA->PushCFunction(LegacyTextureProcessor_AppendToUSDAAsync);
    LUA->SetField(-2, "AppendToUSDAAsync");
    
    LUA->SetField(-2, "VTFConverter");
    LUA->Pop();
    
    Msg("[LegacyTextureProcessor] Lua bindings initialized\n");
}

} // namespace LegacyTextureProcessor

#endif // _WIN64
