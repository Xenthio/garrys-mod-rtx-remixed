#ifdef _WIN64
#include "remixapi.h"
#include "material_lua_bindings.h"
#include <tier0/dbg.h>
#include <materialsystem/imaterialsystem.h>
#include <materialsystem/imaterial.h>
#include <materialsystem/imaterialvar.h>
#include <materialsystem/itexture.h>
#include <d3d9.h>
#include <Windows.h>
#include <mutex>
#include <unordered_set>
#include "../d3d9_texture_tracker.h"
#include "../globalconvars.h"
#include "../material_pipeline/auto_categorisation/auto_categorisation.h"
#include "../material_pipeline/material_pipeline.h"

using namespace GarrysMod::Lua;

// Forward declarations for global symbols (defined in module.cpp)
extern IMaterialSystem* materials;
extern remix::Interface* g_remix;

namespace RemixAPI {

// Helper function to extract MaterialInfo from Lua table
static remix::MaterialInfo LuaToMaterialInfo(ILuaBase* LUA, int index) {
    remix::MaterialInfo info;
    
    if (!LUA->IsType(index, Type::Table)) {
        LUA->ThrowError("Expected table for MaterialInfo");
        return info;
    }
    
    // Get hash (CRITICAL: This must be set to non-zero value!)
#ifdef _DEBUG
    Msg("[MaterialInfo] Looking for hash field at stack index %d\n", index);
    Msg("[MaterialInfo] Table check: %d\n", LUA->IsType(index, Type::Table));
#endif
    
    LUA->GetField(index, "hash");
    
#ifdef _DEBUG
    Msg("[MaterialInfo] After GetField, type at -1: %d (Number=%d)\n", LUA->GetType(-1), Type::Number);
#endif
    
    if (LUA->IsType(-1, Type::Number)) {
        double hashValue = LUA->GetNumber(-1);
        info.hash = static_cast<uint64_t>(hashValue);
#ifdef _DEBUG
        Msg("[MaterialInfo] Extracted hash from Lua (number): %.0f -> %llu\n", hashValue, info.hash);
#endif
    } else if (LUA->IsType(-1, Type::String)) {
        const char* hashStr = LUA->GetString(-1);
        if (hashStr) {
            // Handle 0x prefix if present
            if (hashStr[0] == '0' && (hashStr[1] == 'x' || hashStr[1] == 'X')) {
                info.hash = std::strtoull(hashStr, nullptr, 16);
            } else {
                info.hash = std::strtoull(hashStr, nullptr, 10);
            }
#ifdef _DEBUG
            Msg("[MaterialInfo] Extracted hash from Lua (string): %s -> %llu\n", hashStr, info.hash);
#endif
        }
    } else {
#ifdef _DEBUG
        Msg("[MaterialInfo] WARNING: No hash field found in material table!\n");
        Msg("[MaterialInfo] Type at stack -1: %d\n", LUA->GetType(-1));
#endif
    }
    LUA->Pop();
    
    // Get albedo texture (skip if empty string)
    LUA->GetField(index, "albedoTexture");
    if (LUA->IsType(-1, Type::String)) {
        std::string texture = LUA->GetString(-1);
        // If texture is empty string, don't set it, BUT if we have a hash, 
        // we usually want to allow the hash to drive the texture lookup in Remix.
        // However, if we are explicitly defining a material for a mesh that should use
        // a captured texture, we might need to set albedoTexture to the captured texture's path?
        // No, Remix uses the hash to identify the texture.
        if (!texture.empty()) {
            info.set_albedoTexture(texture);
        }
    }
    LUA->Pop();
    
    // Get normal texture (skip if empty string)
    LUA->GetField(index, "normalTexture");
    if (LUA->IsType(-1, Type::String)) {
        std::string texture = LUA->GetString(-1);
        if (!texture.empty()) {
            info.set_normalTexture(texture);
        }
    }
    LUA->Pop();
    
    // Get tangent texture (skip if empty string)
    LUA->GetField(index, "tangentTexture");
    if (LUA->IsType(-1, Type::String)) {
        std::string texture = LUA->GetString(-1);
        if (!texture.empty()) {
            info.set_tangentTexture(texture);
        }
    }
    LUA->Pop();
    
    // Get emissive texture (skip if empty string)
    LUA->GetField(index, "emissiveTexture");
    if (LUA->IsType(-1, Type::String)) {
        std::string texture = LUA->GetString(-1);
        if (!texture.empty()) {
            info.set_emissiveTexture(texture);
        }
    }
    LUA->Pop();
    
    // Get emissive intensity
    LUA->GetField(index, "emissiveIntensity");
    if (LUA->IsType(-1, Type::Number)) {
        info.emissiveIntensity = static_cast<float>(LUA->GetNumber(-1));
    }
    LUA->Pop();
    
    // Get emissive color constant
    LUA->GetField(index, "emissiveColorConstant");
    if (LUA->IsType(-1, Type::Table)) {
        LUA->GetField(-1, "x");
        if (LUA->IsType(-1, Type::Number)) {
            info.emissiveColorConstant.x = static_cast<float>(LUA->GetNumber(-1));
        }
        LUA->Pop();
        
        LUA->GetField(-1, "y");
        if (LUA->IsType(-1, Type::Number)) {
            info.emissiveColorConstant.y = static_cast<float>(LUA->GetNumber(-1));
        }
        LUA->Pop();
        
        LUA->GetField(-1, "z");
        if (LUA->IsType(-1, Type::Number)) {
            info.emissiveColorConstant.z = static_cast<float>(LUA->GetNumber(-1));
        }
        LUA->Pop();
    }
    LUA->Pop();
    
    // Get sprite sheet properties
    LUA->GetField(index, "spriteSheetRow");
    if (LUA->IsType(-1, Type::Number)) {
        info.spriteSheetRow = static_cast<uint8_t>(LUA->GetNumber(-1));
    }
    LUA->Pop();
    
    LUA->GetField(index, "spriteSheetCol");
    if (LUA->IsType(-1, Type::Number)) {
        info.spriteSheetCol = static_cast<uint8_t>(LUA->GetNumber(-1));
    }
    LUA->Pop();
    
    LUA->GetField(index, "spriteSheetFps");
    if (LUA->IsType(-1, Type::Number)) {
        info.spriteSheetFps = static_cast<uint8_t>(LUA->GetNumber(-1));
    }
    LUA->Pop();
    
    // Get filtering and wrap modes
    LUA->GetField(index, "filterMode");
    if (LUA->IsType(-1, Type::Number)) {
        info.filterMode = static_cast<uint8_t>(LUA->GetNumber(-1));
    }
    LUA->Pop();
    
    LUA->GetField(index, "wrapModeU");
    if (LUA->IsType(-1, Type::Number)) {
        info.wrapModeU = static_cast<uint8_t>(LUA->GetNumber(-1));
    }
    LUA->Pop();
    
    LUA->GetField(index, "wrapModeV");
    if (LUA->IsType(-1, Type::Number)) {
        info.wrapModeV = static_cast<uint8_t>(LUA->GetNumber(-1));
    }
    LUA->Pop();
    
    return info;
}

// Helper function to extract MaterialInfoOpaqueEXT from Lua table
static remix::MaterialInfoOpaqueEXT LuaToMaterialInfoOpaqueEXT(ILuaBase* LUA, int index) {
    remix::MaterialInfoOpaqueEXT info;
    
    if (!LUA->IsType(index, Type::Table)) {
        LUA->ThrowError("Expected table for MaterialInfoOpaqueEXT");
        return info;
    }
    
    // Get roughness texture (skip if empty string)
    LUA->GetField(index, "roughnessTexture");
    if (LUA->IsType(-1, Type::String)) {
        std::string texture = LUA->GetString(-1);
        if (!texture.empty()) {
            info.set_roughnessTexture(texture);
        }
    }
    LUA->Pop();
    
    // Get metallic texture (skip if empty string)
    LUA->GetField(index, "metallicTexture");
    if (LUA->IsType(-1, Type::String)) {
        std::string texture = LUA->GetString(-1);
        if (!texture.empty()) {
            info.set_metallicTexture(texture);
        }
    }
    LUA->Pop();
    
    // Get height texture (skip if empty string)
    LUA->GetField(index, "heightTexture");
    if (LUA->IsType(-1, Type::String)) {
        std::string texture = LUA->GetString(-1);
        if (!texture.empty()) {
            info.set_heightTexture(texture);
        }
    }
    LUA->Pop();
    
    // Get anisotropy
    LUA->GetField(index, "anisotropy");
    if (LUA->IsType(-1, Type::Number)) {
        info.anisotropy = static_cast<float>(LUA->GetNumber(-1));
    }
    LUA->Pop();
    
    // Get albedo constant
    LUA->GetField(index, "albedoConstant");
    if (LUA->IsType(-1, Type::Table)) {
        LUA->GetField(-1, "x");
        if (LUA->IsType(-1, Type::Number)) {
            info.albedoConstant.x = static_cast<float>(LUA->GetNumber(-1));
        }
        LUA->Pop();
        
        LUA->GetField(-1, "y");
        if (LUA->IsType(-1, Type::Number)) {
            info.albedoConstant.y = static_cast<float>(LUA->GetNumber(-1));
        }
        LUA->Pop();
        
        LUA->GetField(-1, "z");
        if (LUA->IsType(-1, Type::Number)) {
            info.albedoConstant.z = static_cast<float>(LUA->GetNumber(-1));
        }
        LUA->Pop();
    }
    LUA->Pop();
    
    // Get opacity constant
    LUA->GetField(index, "opacityConstant");
    if (LUA->IsType(-1, Type::Number)) {
        info.opacityConstant = static_cast<float>(LUA->GetNumber(-1));
    }
    LUA->Pop();
    
    // Get roughness constant
    LUA->GetField(index, "roughnessConstant");
    if (LUA->IsType(-1, Type::Number)) {
        info.roughnessConstant = static_cast<float>(LUA->GetNumber(-1));
    }
    LUA->Pop();
    
    // Get metallic constant
    LUA->GetField(index, "metallicConstant");
    if (LUA->IsType(-1, Type::Number)) {
        info.metallicConstant = static_cast<float>(LUA->GetNumber(-1));
    }
    LUA->Pop();
    
    // Get thin film thickness (optional)
    LUA->GetField(index, "thinFilmThickness");
    if (LUA->IsType(-1, Type::Number)) {
        info.set_thinFilmThickness(static_cast<float>(LUA->GetNumber(-1)));
    }
    LUA->Pop();
    
    // Get blend type (optional)
    LUA->GetField(index, "blendType");
    if (LUA->IsType(-1, Type::Number)) {
        info.set_blendType(static_cast<int>(LUA->GetNumber(-1)));
    }
    LUA->Pop();
    
    // Get boolean flags
    LUA->GetField(index, "alphaIsThinFilmThickness");
    if (LUA->IsType(-1, Type::Bool)) {
        info.alphaIsThinFilmThickness = LUA->GetBool(-1);
    }
    LUA->Pop();
    
    LUA->GetField(index, "useDrawCallAlphaState");
    if (LUA->IsType(-1, Type::Bool)) {
        info.useDrawCallAlphaState = LUA->GetBool(-1);
    }
    LUA->Pop();
    
    LUA->GetField(index, "invertedBlend");
    if (LUA->IsType(-1, Type::Bool)) {
        info.invertedBlend = LUA->GetBool(-1);
    }
    LUA->Pop();
    
    // Get alpha test properties
    LUA->GetField(index, "alphaTestType");
    if (LUA->IsType(-1, Type::Number)) {
        info.alphaTestType = static_cast<int>(LUA->GetNumber(-1));
    }
    LUA->Pop();
    
    LUA->GetField(index, "alphaReferenceValue");
    if (LUA->IsType(-1, Type::Number)) {
        info.alphaReferenceValue = static_cast<uint8_t>(LUA->GetNumber(-1));
    }
    LUA->Pop();
    
    // Get displacement properties
    LUA->GetField(index, "displaceIn");
    if (LUA->IsType(-1, Type::Number)) {
        info.displaceIn = static_cast<float>(LUA->GetNumber(-1));
    }
    LUA->Pop();
    
    LUA->GetField(index, "displaceOut");
    if (LUA->IsType(-1, Type::Number)) {
        info.displaceOut = static_cast<float>(LUA->GetNumber(-1));
    }
    LUA->Pop();
    
    return info;
}

// Lua function: RemixMaterial.CreateMaterial(name, materialInfo)
LUA_FUNCTION(RemixMaterial_CreateMaterial) {
    if (!LUA->IsType(1, Type::String)) {
        LUA->ThrowError("Expected string for material name");
        return 0;
    }
    
    if (!LUA->IsType(2, Type::Table)) {
        LUA->ThrowError("Expected table for material info");
        return 0;
    }
    
    std::string name = LUA->GetString(1);
    remix::MaterialInfo info = LuaToMaterialInfo(LUA, 2);
    
    auto& materialManager = RemixAPI::Instance().GetMaterialManager();
    uint64_t materialId = materialManager.CreateMaterial(name, info);
    
    LUA->PushNumber(static_cast<double>(materialId));
    return 1;
}

// Lua function: RemixMaterial.CreateOpaqueMaterial(name, materialInfo, opaqueInfo)
LUA_FUNCTION(RemixMaterial_CreateOpaqueMaterial) {
    if (!LUA->IsType(1, Type::String)) {
        LUA->ThrowError("Expected string for material name");
        return 0;
    }
    
    if (!LUA->IsType(2, Type::Table)) {
        LUA->ThrowError("Expected table for material info");
        return 0;
    }
    
    if (!LUA->IsType(3, Type::Table)) {
        LUA->ThrowError("Expected table for opaque info");
        return 0;
    }
    
    std::string name = LUA->GetString(1);
    remix::MaterialInfo info = LuaToMaterialInfo(LUA, 2);
    remix::MaterialInfoOpaqueEXT opaqueInfo = LuaToMaterialInfoOpaqueEXT(LUA, 3);
    
    auto& materialManager = RemixAPI::Instance().GetMaterialManager();
    uint64_t materialId = materialManager.CreateOpaqueMaterial(name, info, opaqueInfo);
    
    LUA->PushNumber(static_cast<double>(materialId));
    return 1;
}

// Lua function: RemixMaterial.DestroyMaterial(materialId)
LUA_FUNCTION(RemixMaterial_DestroyMaterial) {
    if (!LUA->IsType(1, Type::Number)) {
        LUA->ThrowError("Expected number for material ID");
        return 0;
    }
    
    uint64_t materialId = static_cast<uint64_t>(LUA->GetNumber(1));
    
    auto& materialManager = RemixAPI::Instance().GetMaterialManager();
    bool result = materialManager.DestroyMaterial(materialId);
    
    LUA->PushBool(result);
    return 1;
}

// Lua function: RemixMaterial.HasMaterial(materialId)
LUA_FUNCTION(RemixMaterial_HasMaterial) {
    if (!LUA->IsType(1, Type::Number)) {
        LUA->ThrowError("Expected number for material ID");
        return 0;
    }
    
    uint64_t materialId = static_cast<uint64_t>(LUA->GetNumber(1));
    
    auto& materialManager = RemixAPI::Instance().GetMaterialManager();
    bool result = materialManager.HasMaterial(materialId);
    
    LUA->PushBool(result);
    return 1;
}

// Helper: Manually track a material (call this before rendering with a material)
LUA_FUNCTION(RemixMaterial_TrackMaterial) {
    if (!LUA->IsType(1, Type::String)) {
        LUA->ThrowError("Expected string for material name");
        return 0;
    }
    
    const char* materialName = LUA->GetString(1);
    
    if (GlobalConvars::r_remix_material_debug && GlobalConvars::r_remix_material_debug->GetBool())
        Msg("[RemixMaterial] TrackMaterial: Attempting to track '%s'\n", materialName);
    
    // Try to find and "touch" the material to trigger loading
    if (materials) {
        IMaterial* pMaterial = materials->FindMaterial(materialName, TEXTURE_GROUP_MODEL);
        if (pMaterial && !pMaterial->IsErrorMaterial()) {
            // Get the base texture var
            bool bFound;
            IMaterialVar* pVar = pMaterial->FindVar("$basetexture", &bFound, false);
            if (bFound && pVar) {
                ITexture* pTex = pVar->GetTextureValue();
                if (pTex) {
                    // Force download to GPU
                    pTex->Download();
                    if (GlobalConvars::r_remix_material_debug && GlobalConvars::r_remix_material_debug->GetBool())
                        Msg("[RemixMaterial] TrackMaterial: Triggered texture download for '%s'\n", pTex->GetName());
                } else {
                    Warning("[RemixMaterial] TrackMaterial: Texture is null for '%s'\n", materialName);
                }
            } else {
                Warning("[RemixMaterial] TrackMaterial: No $basetexture found for '%s'\n", materialName);
            }
        } else {
            Warning("[RemixMaterial] TrackMaterial: Material '%s' not found or is error material\n", materialName);
        }
    }

    // Do not bind and draw the material here. In queued rendering mode the Lua
    // call runs on the main thread while SetTexture runs later on the render
    // thread; forcing hundreds of 1x1 draws during BSP categorization allowed a
    // material name from one thread to be paired with another draw's texture.
    // Download() is enough to make the resource resident. The persistent Lua
    // pending queue will categorize it when a real draw provides a verified hash.
    
    LUA->PushBool(true);
    return 1;
}

// Lua function: RemixMaterial.GetTextureHash(materialName)
// Returns the Remix texture hash for a Source Engine material's base texture
LUA_FUNCTION(RemixMaterial_GetTextureHash) {
    if (!LUA->IsType(1, Type::String)) {
        LUA->ThrowError("Expected string for material name");
        return 0;
    }
    
    const char* materialName = LUA->GetString(1);
    
    // Check Remix API is initialized
    if (!g_remix) {
        Warning("[RemixMaterial] GetTextureHash: Remix API not initialized\n");
        LUA->PushNumber(0);
        return 1;
    }
    
    // Get all texture variants for this material
    auto variants =
        D3D9TextureTracker::Instance().GetTextureVariantsForMaterial(materialName);
    
    if (variants.empty()) {
        // Quiet warning to avoid spam
        // Warning("[RemixMaterial] GetTextureHash: Material '%s' not found in texture cache\n", materialName);
        LUA->PushNumber(0);
        return 1;
    }
    
    // Msg("[RemixMaterial] GetTextureHash: Found %zu texture variant(s) for '%s'\n", variants->size(), materialName);
    
    // Try all variants and collect their hashes
    uint64_t firstValidHash = 0;
    for (size_t i = 0; i < variants.size(); ++i) {
        IDirect3DTexture9* d3dTexture = variants[i];
        
        if (!d3dTexture) {
            continue;
        }

        // The tracker snapshot owns a COM reference for the duration of this
        // function, so the pointer cannot disappear while Remix reads its hash.
        auto result = g_remix->dxvk_GetTextureHash(d3dTexture);
        
        if (result) {
            uint64_t hash = result.value();
            // Msg("[RemixMaterial]   Variant %zu (0x%p): Hash = 0x%llX\n", i, d3dTexture, hash);
            
            if (firstValidHash == 0) {
                firstValidHash = hash;
            }
        }
    }
    
    if (firstValidHash == 0) {
        Warning("[RemixMaterial] GetTextureHash: No valid hashes found for '%s'\n", materialName);
        LUA->PushNumber(0);
        return 1;
    }
    
    // Return the first valid hash (for now)
    // Msg("[RemixMaterial] GetTextureHash: Returning hash 0x%llX for '%s'\n", firstValidHash, materialName);
    
    LUA->PushNumber(static_cast<double>(firstValidHash));
    
    // Push string version as second return value
    char hashStr[32];
    sprintf_s(hashStr, "0x%llX", firstValidHash);
    LUA->PushString(hashStr);
    
    return 2; // Return 2 values
}

// Lua function: RemixMaterial.GetAllTextureHashes(materialName)
// Returns ALL texture hashes for a material (handles multiple variants)
// Returns: table of hash strings
LUA_FUNCTION(RemixMaterial_GetAllTextureHashes) {
    if (!LUA->IsType(1, Type::String)) {
        LUA->ThrowError("Expected string for material name");
        return 0;
    }
    
    const char* materialName = LUA->GetString(1);
    
    if (!g_remix) {
        LUA->CreateTable(); // Return empty table
        return 1;
    }
    
    auto variants =
        D3D9TextureTracker::Instance().GetTextureVariantsForMaterial(materialName);
    
    if (variants.empty()) {
        LUA->CreateTable(); // Return empty table
        return 1;
    }
    
    // Create result table
    LUA->CreateTable();
    int idx = 1;
    
    for (size_t i = 0; i < variants.size(); ++i) {
        IDirect3DTexture9* d3dTexture = variants[i];
        
        if (!d3dTexture) continue;
        
        auto result = g_remix->dxvk_GetTextureHash(d3dTexture);
        if (result) {
            uint64_t hash = result.value();
            if (hash != 0) {
                char hashStr[32];
                sprintf_s(hashStr, "0x%llX", hash);

                LUA->PushNumber(idx);
                LUA->PushString(hashStr);
                LUA->SetTable(-3);
                idx++;
            }
        }
    }
    
    return 1;
}

// Lua function: RemixMaterial.FindMaterialByHash(textureHash)
// Reverse lookup: Returns all material names that match the given texture hash
// Input: textureHash (number or hex string like "0xABCD1234")
// Returns: table of material names
LUA_FUNCTION(RemixMaterial_FindMaterialByHash) {
    uint64_t queryHash = 0;
    
    // Parse hash from either number or string
    if (LUA->IsType(1, Type::Number)) {
        queryHash = static_cast<uint64_t>(LUA->GetNumber(1));
    } else if (LUA->IsType(1, Type::String)) {
        const char* hashStr = LUA->GetString(1);
        // Parse hex string (with or without 0x prefix)
        if (strncmp(hashStr, "0x", 2) == 0 || strncmp(hashStr, "0X", 2) == 0) {
            sscanf_s(hashStr + 2, "%llx", &queryHash);
        } else {
            sscanf_s(hashStr, "%llx", &queryHash);
        }
    } else {
        LUA->ThrowError("Expected number or hex string for texture hash");
        return 0;
    }
    
    if (queryHash == 0) {
        Warning("[RemixMaterial] FindMaterialByHash: Invalid hash (0)\n");
        LUA->CreateTable();
        return 1;
    }
    
    // Check Remix API is initialized
    if (!g_remix) {
        Warning("[RemixMaterial] FindMaterialByHash: Remix API not initialized\n");
        LUA->CreateTable();
        return 1;
    }
    
    Msg("[RemixMaterial] FindMaterialByHash: Searching for hash 0x%llX...\n", queryHash);
    
    // Get all cached materials
    std::vector<std::string> allMaterials = D3D9TextureTracker::Instance().GetCachedMaterials();
    std::vector<std::string> matchingMaterials;
    
    // Check each material's texture hash
    for (const auto& materialName : allMaterials) {
        auto variants =
            D3D9TextureTracker::Instance().GetTextureVariantsForMaterial(materialName.c_str());
        
        if (variants.empty()) {
            continue;
        }
        
        // Check all texture variants for this material
        for (IDirect3DTexture9* d3dTexture : variants) {
            if (!d3dTexture) {
                continue;
            }
            
            // The snapshot keeps every pointer alive during this lookup.
            auto result = g_remix->dxvk_GetTextureHash(d3dTexture);
            if (result) {
                uint64_t hash = result.value();
                if (hash == queryHash) {
                    matchingMaterials.push_back(materialName);
                    Msg("[RemixMaterial]   Found match: '%s' (hash 0x%llX)\n", materialName.c_str(), hash);
                    break; // Found a match, no need to check other variants
                }
            }
        }
    }
    
    // Return results as Lua table
    LUA->CreateTable();
    for (size_t i = 0; i < matchingMaterials.size(); ++i) {
        LUA->PushNumber(static_cast<double>(i + 1)); // Lua arrays are 1-indexed
        LUA->PushString(matchingMaterials[i].c_str());
        LUA->SetTable(-3);
    }
    
    if (matchingMaterials.empty()) {
        Msg("[RemixMaterial] FindMaterialByHash: No materials found with hash 0x%llX\n", queryHash);
        Msg("[RemixMaterial]   Tip: Make sure the texture has been rendered and is in the cache\n");
    } else {
        Msg("[RemixMaterial] FindMaterialByHash: Found %zu matching material(s)\n", matchingMaterials.size());
    }
    
    return 1;
}


// Lua function: RemixMaterial.GetCachedMaterials()
// Returns a table of all material names currently in the texture tracker cache
LUA_FUNCTION(RemixMaterial_GetCachedMaterials) {
    // Check Remix API is initialized
    if (!g_remix) {
        Warning("[RemixMaterial] GetCachedMaterials: Remix API not initialized\n");
        LUA->CreateTable();
        return 1;
    }
    
    std::vector<std::string> materials = D3D9TextureTracker::Instance().GetCachedMaterials();
    
    LUA->CreateTable();
    for (size_t i = 0; i < materials.size(); ++i) {
        LUA->PushNumber(static_cast<double>(i + 1)); // Lua arrays are 1-indexed
        LUA->PushString(materials[i].c_str());
        LUA->SetTable(-3);
    }
    
    return 1;
}

// Helper: Convert category flags to Remix option names
static std::vector<const char*> GetRemixCategoryOptions(uint32_t categoryFlags) {
    std::vector<const char*> options;
    
    // Based on remixapi_InstanceCategoryBit enum and our Lua constants
    // NOTE: We intentionally skip bit 0 (WORLD_UI) and bit 1 (WORLD_MATTE) - not used
    if (categoryFlags & (1 << 2))  options.push_back("rtx.skyBoxTextures");  // SKY
    if (categoryFlags & (1 << 3))  options.push_back("rtx.ignoreTextures");  // IGNORE
    if (categoryFlags & (1 << 9))  options.push_back("rtx.hideInstanceTextures");  // HIDDEN
    if (categoryFlags & (1 << 10)) options.push_back("rtx.particleTextures");  // PARTICLE
    if (categoryFlags & (1 << 11)) options.push_back("rtx.beamTextures");  // BEAM
    if (categoryFlags & (1 << 12)) options.push_back("rtx.decalTextures");  // DECAL_STATIC
    if (categoryFlags & (1 << 17)) options.push_back("rtx.terrainTextures");  // TERRAIN
    if (categoryFlags & (1 << 18)) options.push_back("rtx.animatedWaterTextures");  // ANIMATED_WATER
    if (categoryFlags & (1 << 19)) options.push_back("rtx.playerModelTextures");  // THIRD_PERSON_PLAYER_MODEL
    if (categoryFlags & (1 << 24)) options.push_back("rtx.legacyEmissiveTextures");  // LEGACY_EMISSIVE
    
    return options;
}

// Lua function: RemixMaterial.SetHashCategory(textureHash, categoryFlags)
LUA_FUNCTION(RemixMaterial_SetHashCategory) {
    if (!g_remix) {
        LUA->ThrowError("RemixMaterial.SetHashCategory: Remix API not initialized");
        return 0;
    }
    
    if (!LUA->IsType(1, Type::Number) && !LUA->IsType(1, Type::String)) {
        LUA->ThrowError("RemixMaterial.SetHashCategory: Expected number or string for texture hash");
        return 0;
    }
    
    if (!LUA->IsType(2, Type::Number)) {
        LUA->ThrowError("RemixMaterial.SetHashCategory: Expected number for category flags");
        return 0;
    }
    
    uint64_t textureHash;
    if (LUA->IsType(1, Type::String)) {
        const char* hashStr = LUA->GetString(1);
        textureHash = std::strtoull(hashStr, nullptr, 16);
    } else {
        textureHash = static_cast<uint64_t>(LUA->GetNumber(1));
    }
    
    uint32_t categoryFlags = static_cast<uint32_t>(LUA->GetNumber(2));
    
    // Convert hash to string for Remix API
    char hashStr[32];
    sprintf_s(hashStr, "0x%llX", textureHash);
    
    // Get the Remix category option names from the flags
    auto options = GetRemixCategoryOptions(categoryFlags);
    
    // Add hash to each relevant Remix category
    bool success = true;
    for (const char* option : options) {
        auto result = g_remix->AddTextureHash(option, hashStr);
        if (!result) {
            Warning("[RemixMaterial] Failed to add hash %s to category %s: error code %d\n", 
                    hashStr, option, static_cast<int>(result.status()));
            success = false;
        }
    }
    
    // DECAL_STATIC (world geometry) takes priority over PARTICLE.
    // When a hash is assigned to world geometry, remove any prior particle tag so
    // RTX Remix doesn't see the hash in both rtx.particleTextures and rtx.decalTextures.
    if (categoryFlags & 0x1000) { // DECAL_STATIC
        uint32_t existingFlags = 0;
        if (D3D9TextureTracker::Instance().GetHashCategoryFlags(textureHash, &existingFlags)) {
            if (existingFlags & 0x400) { // PARTICLE
                g_remix->RemoveTextureHash("rtx.particleTextures", hashStr);
                // Merge: carry over all existing flags except PARTICLE, then OR in new flags
                categoryFlags = (existingFlags & ~0x400u) | categoryFlags;
            }
        }
    }
    
    // Always store locally so GetHashCategory reflects the intent regardless of
    // whether the Remix API accepted the call this frame.
    D3D9TextureTracker::Instance().SetHashCategoryFlags(textureHash, categoryFlags);

    // Lua-driven scans and the C++ pipeline must obey the same global-hash
    // collision rule. In particular, do not let an emissive material make a
    // non-emissive material glow when both resolve to the same Remix hash.
    MaterialPipeline::AutoCategorisation::ReconcileHashCategories(textureHash);
    
    LUA->PushBool(success);
    return 1;
}

// Lua function: RemixMaterial.RemoveHashCategory(textureHash)
// Removes the category mapping for a texture hash
LUA_FUNCTION(RemixMaterial_RemoveHashCategory) {
    if (!g_remix) {
        LUA->ThrowError("RemixMaterial.RemoveHashCategory: Remix API not initialized");
        return 0;
    }
    
    if (!LUA->IsType(1, Type::Number) && !LUA->IsType(1, Type::String)) {
        LUA->ThrowError("Expected number or string for texture hash");
        return 0;
    }
    
    uint64_t textureHash = 0;
    if (LUA->IsType(1, Type::String)) {
        const char* hashStr = LUA->GetString(1);
        if (hashStr) {
            if (hashStr[0] == '0' && (hashStr[1] == 'x' || hashStr[1] == 'X')) {
                textureHash = std::strtoull(hashStr, nullptr, 16);
            } else {
                textureHash = std::strtoull(hashStr, nullptr, 10);
            }
        }
    } else {
        textureHash = static_cast<uint64_t>(LUA->GetNumber(1));
    }
    
    // Get current category flags to know which Remix options to remove from
    uint32_t categoryFlags = 0;
    bool hadCategories = D3D9TextureTracker::Instance().GetHashCategoryFlags(textureHash, &categoryFlags);
    
    // Convert hash to string for Remix API
    char hashStr[32];
    sprintf_s(hashStr, "0x%llX", textureHash);
    
    // Remove from Remix categories if we had any
    if (hadCategories && categoryFlags != 0) {
        auto options = GetRemixCategoryOptions(categoryFlags);
        
        for (const char* option : options) {
            auto result = g_remix->RemoveTextureHash(option, hashStr);
            if (!result) {
                Warning("[RemixMaterial] Failed to remove hash %s from category %s: error code %d\n", 
                        hashStr, option, static_cast<int>(result.status()));
            }
        }
    }
    
    // Remove from local tracking
    D3D9TextureTracker::Instance().RemoveHashCategoryFlags(textureHash);
    
    LUA->PushBool(true);
    return 1;
}

// Lua function: RemixMaterial.ClearHashCategories()
// Clears all hash-to-category mappings
LUA_FUNCTION(RemixMaterial_ClearHashCategories) {
    D3D9TextureTracker::Instance().ClearHashCategoryMappings();
    LUA->PushBool(true);
    return 1;
}

// Lua function: RemixMaterial.GetHashCategory(textureHash)
// Gets the category flags for a texture hash (returns nil if not found)
LUA_FUNCTION(RemixMaterial_GetHashCategory) {
    if (!LUA->IsType(1, Type::Number) && !LUA->IsType(1, Type::String)) {
        LUA->ThrowError("Expected number or string for texture hash");
        return 0;
    }
    
    uint64_t textureHash = 0;
    if (LUA->IsType(1, Type::String)) {
        const char* hashStr = LUA->GetString(1);
        if (hashStr) {
            if (hashStr[0] == '0' && (hashStr[1] == 'x' || hashStr[1] == 'X')) {
                textureHash = std::strtoull(hashStr, nullptr, 16);
            } else {
                textureHash = std::strtoull(hashStr, nullptr, 10);
            }
        }
    } else {
        textureHash = static_cast<uint64_t>(LUA->GetNumber(1));
    }
    
    uint32_t categoryFlags = 0;
    if (D3D9TextureTracker::Instance().GetHashCategoryFlags(textureHash, &categoryFlags)) {
        LUA->PushNumber(static_cast<double>(categoryFlags));
        return 1;
    }
    
    return 0; // nil
}

// Lua function: RemixMaterial.ClearTextureCache()
// Clears the D3D9 texture tracker cache
LUA_FUNCTION(RemixMaterial_ClearTextureCache) {
    auto& pipeline = MaterialPipeline::Pipeline::Instance();
    if (pipeline.IsInitialized()) {
        pipeline.ClearMapState();
    } else {
        D3D9TextureTracker::Instance().ClearCache();
    }
    LUA->PushBool(true);
    return 1;
}

// Lua function: RemixMaterial.RetryPendingCategories()
// Retries categorization for textures that returned hash=0
// Returns number of textures successfully categorized
LUA_FUNCTION(RemixMaterial_RetryPendingCategories) {
    int count = D3D9TextureTracker::Instance().RetryPendingCategories();
    LUA->PushNumber(count);
    return 1;
}

// Lua function: RemixMaterial.GetPendingCount()
// Returns the number of textures waiting for categorization
LUA_FUNCTION(RemixMaterial_GetPendingCount) {
    size_t count = D3D9TextureTracker::Instance().GetPendingCount();
    LUA->PushNumber(static_cast<double>(count));
    return 1;
}

// Lua function: RemixMaterial.RescanAllMaterials()
// Re-scans all cached materials and applies categories (emissive, etc.)
// Useful after code changes or to catch materials that were cached before detection was added
LUA_FUNCTION(RemixMaterial_RescanAllMaterials) {
    int count = D3D9TextureTracker::Instance().RescanAllMaterials();
    LUA->PushNumber(count);
    return 1;
}

// Lua function: RemixMaterial.RecheckWorldTextures()
// Re-checks all cached materials against the world texture list
// Useful after SetWorldTextureList is called to categorize materials that rendered before the list was loaded
LUA_FUNCTION(RemixMaterial_RecheckWorldTextures) {
    int count = D3D9TextureTracker::Instance().RecheckWorldTextures();
    LUA->PushNumber(count);
    return 1;
}

// Lua function: RemixMaterial.DumpAllTextureHashes()
// Returns a table of all tracked textures with their current hashes
// Format: { { material = "name", texture = "0xPTR", hash = "0xHASH" }, ... }
LUA_FUNCTION(RemixMaterial_DumpAllTextureHashes) {
    auto& tracker = D3D9TextureTracker::Instance();
    auto dump = tracker.DumpAllTextureHashes();
    
    // Create Lua table
    LUA->CreateTable();
    int idx = 1;
    
    for (const auto& entry : dump) {
        const std::string& materialName = std::get<0>(entry);
        void* texturePtr = std::get<1>(entry);
        uint64_t hash = std::get<2>(entry);
        
        LUA->PushNumber(idx);
        LUA->CreateTable();
        
        LUA->PushString("material");
        LUA->PushString(materialName.c_str());
        LUA->SetTable(-3);
        
        LUA->PushString("texture");
        char ptrStr[32];
        sprintf_s(ptrStr, "0x%p", texturePtr);
        LUA->PushString(ptrStr);
        LUA->SetTable(-3);
        
        LUA->PushString("hash");
        if (hash != 0) {
            char hashStr[32];
            sprintf_s(hashStr, "0x%llX", hash);
            LUA->PushString(hashStr);
        } else {
            LUA->PushString("0x0");
        }
        LUA->SetTable(-3);
        
        LUA->SetTable(-3);
        idx++;
    }
    
    return 1;
}

// Lua function: RemixMaterial.FindTexturesByName(searchName)
LUA_FUNCTION(RemixMaterial_FindTexturesByName) {
    if (!LUA->IsType(1, Type::String)) {
        LUA->ThrowError("RemixMaterial.FindTexturesByName: Expected string for search name");
        return 0;
    }
    
    const char* searchName = LUA->GetString(1);
    std::string searchLower = searchName;
    std::transform(searchLower.begin(), searchLower.end(), searchLower.begin(), ::tolower);
    
    // Search D3D9TextureTracker for matching textures
    auto& tracker = D3D9TextureTracker::Instance();
    auto results = tracker.FindTexturesByName(searchLower);
    
    // Create Lua table with results
    LUA->CreateTable();
    int idx = 1;
    
    for (const auto& result : results) {
        LUA->PushNumber(idx);
        LUA->CreateTable();
        
        LUA->PushString("name");
        LUA->PushString(result.first.c_str());
        LUA->SetTable(-3);
        
        LUA->PushString("hash");
        LUA->PushNumber(static_cast<double>(result.second));
        LUA->SetTable(-3);
        
        LUA->SetTable(-3);
        idx++;
    }
    
    return 1;
}

// Lua function: RemixMaterial.RegisterBSPWorldMaterial(materialName)
// Registers a material as BSP world geometry in the reverse hash-collision guard.
// Should be called for every BSP world brush material, including translucent ones
// that receive no Remix category.  When the material's texture hash is later
// encountered with a PARTICLE tag (hash collision), the tag is suppressed.
LUA_FUNCTION(RemixMaterial_RegisterBSPWorldMaterial) {
    if (!LUA->IsType(1, Type::String)) {
        LUA->ThrowError("RemixMaterial.RegisterBSPWorldMaterial: Expected string material name");
        return 0;
    }
    const char* materialName = LUA->GetString(1);
    if (materialName && materialName[0] != '\0') {
        D3D9TextureTracker::Instance().RegisterBSPWorldMaterial(materialName);
    }
    return 0;
}

// Lua function: RemixMaterial.ClearBSPWorldMaterials()
// Clears the BSP world material registry.  Call on map change before re-running
// the BSP scan so stale entries from the previous map do not persist.
LUA_FUNCTION(RemixMaterial_ClearBSPWorldMaterials) {
    D3D9TextureTracker::Instance().ClearBSPWorldMaterials();
    return 0;
}

// Lua function: RemixMaterial.SetWorldTextureList(textureTable)
// Accepts a Lua table of texture names from BSP parsing
// These will be automatically marked as DECAL_STATIC (world geometry) when rendered
LUA_FUNCTION(RemixMaterial_SetWorldTextureList) {
    if (!LUA->IsType(1, Type::Table)) {
        LUA->ThrowError("RemixMaterial.SetWorldTextureList: Expected table of texture names");
        return 0;
    }
    
    std::vector<std::string> textureNames;
    textureNames.reserve(1024); // Pre-allocate to avoid reallocations
    
    Msg("[RemixMaterial] SetWorldTextureList: Starting table iteration...\n");
    
    // Iterate through the Lua table
    LUA->PushNil(); // First key
    int count = 0;
    while (LUA->Next(1) != 0) {
        // Key is at -2, value is at -1
        if (LUA->IsType(-1, Type::String)) {
            const char* textureName = LUA->GetString(-1);
            if (textureName && textureName[0] != '\0') {
                textureNames.push_back(textureName);
                count++;
            }
        }
        LUA->Pop(1); // Explicitly pop 1 value, keep key for next iteration
    }
    
    Msg("[RemixMaterial] SetWorldTextureList: Parsed %d texture names from Lua table\n", count);
    
    // Pass to D3D9TextureTracker
    if (!textureNames.empty()) {
        D3D9TextureTracker::Instance().SetWorldTextureNames(textureNames);
        Msg("[RemixMaterial] SetWorldTextureList: Sent to D3D9TextureTracker\n");
    } else {
        Warning("[RemixMaterial] SetWorldTextureList: No valid texture names found in table\n");
    }
    
    LUA->PushBool(true);
    return 1;
}

// Lua function: RemixMaterial.ClearWorldTextureList()
// Clears the world texture list (useful for map changes)
LUA_FUNCTION(RemixMaterial_ClearWorldTextureList) {
    D3D9TextureTracker::Instance().ClearWorldTextureNames();
    LUA->PushBool(true);
    return 1;
}

// Lua function: RemixMaterial.SetParticleCategorization(enabled)
// Enable or disable automatic particle categorization
LUA_FUNCTION(RemixMaterial_SetParticleCategorization) {
    if (!LUA->IsType(1, Type::Bool)) {
        LUA->ThrowError("RemixMaterial.SetParticleCategorization: Expected boolean argument");
        return 0;
    }
    
    bool enabled = LUA->GetBool(1);
    D3D9TextureTracker::Instance().SetParticleCategorization(enabled);
    
    LUA->PushBool(true);
    return 1;
}

// Lua function: RemixMaterial.SetDecalCategorization(enabled)
// Enable or disable automatic decal categorization
LUA_FUNCTION(RemixMaterial_SetDecalCategorization) {
    if (!LUA->IsType(1, Type::Bool)) {
        LUA->ThrowError("RemixMaterial.SetDecalCategorization: Expected boolean argument");
        return 0;
    }
    
    bool enabled = LUA->GetBool(1);
    D3D9TextureTracker::Instance().SetDecalCategorization(enabled);
    
    LUA->PushBool(true);
    return 1;
}

// Lua function: RemixMaterial.SetEmissiveCategorization(enabled)
// Enable or disable automatic emissive categorization
LUA_FUNCTION(RemixMaterial_SetEmissiveCategorization) {
    if (!LUA->IsType(1, Type::Bool)) {
        LUA->ThrowError("RemixMaterial.SetEmissiveCategorization: Expected boolean argument");
        return 0;
    }
    
    bool enabled = LUA->GetBool(1);
    D3D9TextureTracker::Instance().SetEmissiveCategorization(enabled);
    
    LUA->PushBool(true);
    return 1;
}

// Lua function: RemixMaterial.SetAutoCategorization(enabled)
// Enable or disable ALL automatic categorization (master switch)
LUA_FUNCTION(RemixMaterial_SetAutoCategorization) {
    if (!LUA->IsType(1, Type::Bool)) {
        LUA->ThrowError("RemixMaterial.SetAutoCategorization: Expected boolean argument");
        return 0;
    }
    
    bool enabled = LUA->GetBool(1);
    D3D9TextureTracker::Instance().SetAutoCategorization(enabled);
    
    LUA->PushBool(true);
    return 1;
}

// Lua function: RemixMaterial.SetDebugOutput(enabled)
// Enable or disable debug output for categorization
LUA_FUNCTION(RemixMaterial_SetDebugOutput) {
    if (!LUA->IsType(1, Type::Bool)) {
        LUA->ThrowError("RemixMaterial.SetDebugOutput: Expected boolean argument");
        return 0;
    }
    
    bool enabled = LUA->GetBool(1);
    D3D9TextureTracker::Instance().SetDebugOutput(enabled);
    
    LUA->PushBool(true);
    return 1;
}

// =========================================================================
// Force-Albedo Emission Helpers
// =========================================================================
// Shared by Lua and C++ AutoCategorisation. Access through the global helper
// functions below so updates cannot race or overwrite another path's hashes.
static std::unordered_set<uint64_t> s_forceAlbedoHashes;
static std::mutex s_forceAlbedoMutex;

// Caller must hold s_forceAlbedoMutex.
static void UpdateForceAlbedoConfig() {
    if (!g_remix) return;
    
    std::string value;
    for (uint64_t hash : s_forceAlbedoHashes) {
        if (!value.empty()) value += ',';
        char buf[32];
        snprintf(buf, sizeof(buf), "0x%llX", (unsigned long long)hash);
        value += buf;
    }
    g_remix->SetConfigVariable("rtx.legacyEmissiveForceAlbedoString", value.c_str());
}

// Lua function: RemixMaterial.AddForceAlbedoHash(hashStr)
// Registers a texture hash for force-albedo emission mode so that Remix emits
// full albedo colour even when the texture has no alpha mask channel.
// Call this for every hash belonging to an UnlitGeneric or UnlitTwoTexture material.
LUA_FUNCTION(RemixMaterial_AddForceAlbedoHash) {
    if (!g_remix) {
        LUA->ThrowError("RemixMaterial.AddForceAlbedoHash: Remix API not initialized");
        return 0;
    }
    
    if (!LUA->IsType(1, Type::String) && !LUA->IsType(1, Type::Number)) {
        LUA->ThrowError("RemixMaterial.AddForceAlbedoHash: Expected string or number for hash");
        return 0;
    }
    
    uint64_t hash = 0;
    if (LUA->IsType(1, Type::String)) {
        hash = std::strtoull(LUA->GetString(1), nullptr, 0); // handles 0x prefix
    } else {
        hash = static_cast<uint64_t>(LUA->GetNumber(1));
    }
    
    if (hash == 0) {
        LUA->PushBool(false);
        return 1;
    }
    
    LUA->PushBool(::AddForceAlbedoHashCpp(hash));
    return 1;
}

// Lua function: RemixMaterial.ClearForceAlbedoHashes()
// Resets the force-albedo list. Call on map change before re-scanning.
LUA_FUNCTION(RemixMaterial_ClearForceAlbedoHashes) {
    ::ClearForceAlbedoHashesCpp();
    LUA->PushBool(true);
    return 1;
}

// Initialize Material Manager Lua bindings
void MaterialManager::InitializeLuaBindings() {
    if (!m_lua) return;
    
    // Get the global table
    m_lua->PushSpecial(GarrysMod::Lua::SPECIAL_GLOB);
    
    // Create RemixMaterial table
    m_lua->CreateTable();
    
    // Add functions to the table
    m_lua->PushCFunction(RemixMaterial_CreateMaterial);
    m_lua->SetField(-2, "CreateMaterial");
    
    m_lua->PushCFunction(RemixMaterial_CreateOpaqueMaterial);
    m_lua->SetField(-2, "CreateOpaqueMaterial");
    
    m_lua->PushCFunction(RemixMaterial_DestroyMaterial);
    m_lua->SetField(-2, "DestroyMaterial");
    
    m_lua->PushCFunction(RemixMaterial_HasMaterial);
    m_lua->SetField(-2, "HasMaterial");
    
    m_lua->PushCFunction(RemixMaterial_GetTextureHash);
    m_lua->SetField(-2, "GetTextureHash");
    
    m_lua->PushCFunction(RemixMaterial_GetAllTextureHashes);
    m_lua->SetField(-2, "GetAllTextureHashes");
    
    m_lua->PushCFunction(RemixMaterial_FindMaterialByHash);
    m_lua->SetField(-2, "FindMaterialByHash");
    
    m_lua->PushCFunction(RemixMaterial_TrackMaterial);
    m_lua->SetField(-2, "TrackMaterial");

    m_lua->PushCFunction(RemixMaterial_GetCachedMaterials);
    m_lua->SetField(-2, "GetCachedMaterials");
    
    m_lua->PushCFunction(RemixMaterial_SetHashCategory);
    m_lua->SetField(-2, "SetHashCategory");
    
    m_lua->PushCFunction(RemixMaterial_RemoveHashCategory);
    m_lua->SetField(-2, "RemoveHashCategory");
    
    m_lua->PushCFunction(RemixMaterial_ClearHashCategories);
    m_lua->SetField(-2, "ClearHashCategories");
    
    m_lua->PushCFunction(RemixMaterial_GetHashCategory);
    m_lua->SetField(-2, "GetHashCategory");
    
    m_lua->PushCFunction(RemixMaterial_FindTexturesByName);
    m_lua->SetField(-2, "FindTexturesByName");
    
    m_lua->PushCFunction(RemixMaterial_ClearTextureCache);
    m_lua->SetField(-2, "ClearTextureCache");
    
    m_lua->PushCFunction(RemixMaterial_RetryPendingCategories);
    m_lua->SetField(-2, "RetryPendingCategories");
    
    m_lua->PushCFunction(RemixMaterial_GetPendingCount);
    m_lua->SetField(-2, "GetPendingCount");
    
    m_lua->PushCFunction(RemixMaterial_RescanAllMaterials);
    m_lua->SetField(-2, "RescanAllMaterials");
    
    m_lua->PushCFunction(RemixMaterial_RecheckWorldTextures);
    m_lua->SetField(-2, "RecheckWorldTextures");
    
    m_lua->PushCFunction(RemixMaterial_DumpAllTextureHashes);
    m_lua->SetField(-2, "DumpAllTextureHashes");
    
    m_lua->PushCFunction(RemixMaterial_RegisterBSPWorldMaterial);
    m_lua->SetField(-2, "RegisterBSPWorldMaterial");

    m_lua->PushCFunction(RemixMaterial_ClearBSPWorldMaterials);
    m_lua->SetField(-2, "ClearBSPWorldMaterials");

    m_lua->PushCFunction(RemixMaterial_SetWorldTextureList);
    m_lua->SetField(-2, "SetWorldTextureList");
    
    m_lua->PushCFunction(RemixMaterial_ClearWorldTextureList);
    m_lua->SetField(-2, "ClearWorldTextureList");
    
    m_lua->PushCFunction(RemixMaterial_SetParticleCategorization);
    m_lua->SetField(-2, "SetParticleCategorization");
    
    m_lua->PushCFunction(RemixMaterial_SetDecalCategorization);
    m_lua->SetField(-2, "SetDecalCategorization");
    
    m_lua->PushCFunction(RemixMaterial_SetEmissiveCategorization);
    m_lua->SetField(-2, "SetEmissiveCategorization");
    
    m_lua->PushCFunction(RemixMaterial_SetAutoCategorization);
    m_lua->SetField(-2, "SetAutoCategorization");
    
    m_lua->PushCFunction(RemixMaterial_SetDebugOutput);
    m_lua->SetField(-2, "SetDebugOutput");
    
    m_lua->PushCFunction(RemixMaterial_AddForceAlbedoHash);
    m_lua->SetField(-2, "AddForceAlbedoHash");
    
    m_lua->PushCFunction(RemixMaterial_ClearForceAlbedoHashes);
    m_lua->SetField(-2, "ClearForceAlbedoHashes");
    
    // Set the table as the global "RemixMaterial"
    m_lua->SetField(-2, "RemixMaterial");
    
    // Pop global table
    m_lua->Pop();
    
    Msg("[MaterialManager] Lua bindings initialized\n");
}

} // namespace RemixAPI

bool AddForceAlbedoHashCpp(uint64_t hash) {
    if (!g_remix || hash == 0) return false;

    std::lock_guard<std::mutex> lock(RemixAPI::s_forceAlbedoMutex);
    if (RemixAPI::s_forceAlbedoHashes.insert(hash).second) {
        RemixAPI::UpdateForceAlbedoConfig();
    }
    return true;
}

bool RemoveForceAlbedoHashCpp(uint64_t hash) {
    std::lock_guard<std::mutex> lock(RemixAPI::s_forceAlbedoMutex);
    if (!RemixAPI::s_forceAlbedoHashes.erase(hash)) {
        return false;
    }
    RemixAPI::UpdateForceAlbedoConfig();
    return true;
}

void ClearForceAlbedoHashesCpp() {
    std::lock_guard<std::mutex> lock(RemixAPI::s_forceAlbedoMutex);
    RemixAPI::s_forceAlbedoHashes.clear();
    RemixAPI::UpdateForceAlbedoConfig();
}

#endif // _WIN64
