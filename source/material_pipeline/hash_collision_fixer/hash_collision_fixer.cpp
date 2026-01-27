// =========================================================================
// hash_collision_fixer.cpp - Fix RTX Remix hash collisions for solid-color textures
// =========================================================================
// Part of the Material Pipeline - Pre-processing stage
// =========================================================================

#ifdef _WIN64

#include <Windows.h>
#include "hash_collision_fixer.h"
#include "../../vtf_parser.h"
#include <tier0/dbg.h>
#include <filesystem.h>

namespace MaterialPipeline {
namespace HashCollisionFixer {

// =========================================================================
// Internal State
// =========================================================================
static std::mutex s_mutex;
static std::unordered_map<std::string, SolidColorMaterial> s_solidColorMaterials;
static std::unordered_set<std::string> s_fixedMaterials;
static std::unordered_set<std::string> s_checkedMaterials;  // Materials we've already checked
static size_t s_totalDetected = 0;
static size_t s_totalFixed = 0;
static bool s_initialized = false;

// =========================================================================
// Main Interface Implementation
// =========================================================================

void Initialize() {
    std::lock_guard<std::mutex> lock(s_mutex);
    if (s_initialized) return;
    
    s_solidColorMaterials.clear();
    s_fixedMaterials.clear();
    s_checkedMaterials.clear();
    s_totalDetected = 0;
    s_totalFixed = 0;
    s_initialized = true;
    
    Msg("[MaterialPipeline::HashCollisionFixer] Initialized\n");
}

void Shutdown() {
    std::lock_guard<std::mutex> lock(s_mutex);
    s_solidColorMaterials.clear();
    s_fixedMaterials.clear();
    s_checkedMaterials.clear();
    s_initialized = false;
    
    Msg("[MaterialPipeline::HashCollisionFixer] Shutdown\n");
}

bool CheckMaterial(IFileSystem* fileSystem,
                   const std::string& materialName,
                   const std::string& texturePath,
                   bool debugOutput) {
    if (materialName.empty() || texturePath.empty()) {
        return false;
    }
    
    std::lock_guard<std::mutex> lock(s_mutex);
    
    // Skip if already checked
    if (s_checkedMaterials.count(materialName) > 0) {
        return s_solidColorMaterials.count(materialName) > 0 && 
               s_fixedMaterials.count(materialName) == 0;
    }
    
    s_checkedMaterials.insert(materialName);
    
    // Check if the texture is a solid color
    VTFParser::SolidColorResult result = VTFParser::CheckSolidColor(fileSystem, texturePath, debugOutput);
    
    if (result.error) {
        if (debugOutput) {
            Msg("[MaterialPipeline::HashCollisionFixer] Error checking %s: %s\n", 
                materialName.c_str(), result.errorMessage.c_str());
        }
        return false;
    }
    
    if (result.isSolidColor) {
        SolidColorMaterial info;
        info.materialName = materialName;
        info.texturePath = texturePath;
        info.r = result.r;
        info.g = result.g;
        info.b = result.b;
        info.a = result.a;
        info.fixed = false;
        
        s_solidColorMaterials[materialName] = info;
        s_totalDetected++;
        
        if (debugOutput) {
            Msg("[MaterialPipeline::HashCollisionFixer] Detected solid-color material: %s (RGBA: %d,%d,%d,%d)\n",
                materialName.c_str(), result.r, result.g, result.b, result.a);
        }
        
        return true;
    }
    
    return false;
}

std::vector<std::string> GetMaterialsNeedingFix() {
    std::lock_guard<std::mutex> lock(s_mutex);
    std::vector<std::string> result;
    
    for (const auto& pair : s_solidColorMaterials) {
        if (s_fixedMaterials.count(pair.first) == 0) {
            result.push_back(pair.first);
        }
    }
    
    return result;
}

void MarkMaterialFixed(const std::string& materialName) {
    std::lock_guard<std::mutex> lock(s_mutex);
    
    if (s_fixedMaterials.count(materialName) == 0) {
        s_fixedMaterials.insert(materialName);
        s_totalFixed++;
        
        auto it = s_solidColorMaterials.find(materialName);
        if (it != s_solidColorMaterials.end()) {
            it->second.fixed = true;
        }
    }
}

bool IsMaterialFixed(const std::string& materialName) {
    std::lock_guard<std::mutex> lock(s_mutex);
    return s_fixedMaterials.count(materialName) > 0;
}

bool IsSolidColorMaterial(const std::string& materialName) {
    std::lock_guard<std::mutex> lock(s_mutex);
    return s_solidColorMaterials.count(materialName) > 0;
}

bool GetMaterialColor(const std::string& materialName,
                      uint8_t& r, uint8_t& g, uint8_t& b, uint8_t& a) {
    std::lock_guard<std::mutex> lock(s_mutex);
    
    auto it = s_solidColorMaterials.find(materialName);
    if (it != s_solidColorMaterials.end()) {
        r = it->second.r;
        g = it->second.g;
        b = it->second.b;
        a = it->second.a;
        return true;
    }
    
    return false;
}

void Reset() {
    std::lock_guard<std::mutex> lock(s_mutex);
    
    s_solidColorMaterials.clear();
    s_fixedMaterials.clear();
    s_checkedMaterials.clear();
    // Keep totals for statistics
    
    Msg("[MaterialPipeline::HashCollisionFixer] Reset (total detected: %zu, total fixed: %zu)\n",
        s_totalDetected, s_totalFixed);
}

size_t GetTotalDetected() {
    std::lock_guard<std::mutex> lock(s_mutex);
    return s_totalDetected;
}

size_t GetTotalFixed() {
    std::lock_guard<std::mutex> lock(s_mutex);
    return s_totalFixed;
}

size_t GetPendingCount() {
    std::lock_guard<std::mutex> lock(s_mutex);
    size_t pending = 0;
    for (const auto& pair : s_solidColorMaterials) {
        if (s_fixedMaterials.count(pair.first) == 0) {
            pending++;
        }
    }
    return pending;
}

// =========================================================================
// Lua Bindings
// =========================================================================

// Filesystem access - dynamically retrieved from Source Engine
static IFileSystem* s_pFileSystem = nullptr;

static IFileSystem* GetFileSystem() {
    if (s_pFileSystem) return s_pFileSystem;
    
    // Get filesystem interface the same way as other modules
    HMODULE hModule = GetModuleHandleA("filesystem_stdio.dll");
    if (hModule) {
        typedef void* (*CreateInterfaceFn)(const char* pName, int* pReturnCode);
        CreateInterfaceFn createInterface = (CreateInterfaceFn)GetProcAddress(hModule, "CreateInterface");
        if (createInterface) {
            // Try different versions
            s_pFileSystem = (IFileSystem*)createInterface("VFileSystem022", nullptr);
            if (!s_pFileSystem) {
                s_pFileSystem = (IFileSystem*)createInterface("VFileSystem021", nullptr);
            }
            if (!s_pFileSystem) {
                s_pFileSystem = (IFileSystem*)createInterface("VFileSystem017", nullptr);
            }
        }
    }
    
    return s_pFileSystem;
}

// Lua: HashCollisionFixer.CheckMaterial(materialName, texturePath, [debugOutput])
LUA_FUNCTION(HashFixer_CheckMaterial) {
    const char* materialName = LUA->CheckString(1);
    const char* texturePath = LUA->CheckString(2);
    bool debug = LUA->IsType(3, GarrysMod::Lua::Type::Bool) ? LUA->GetBool(3) : false;
    
    IFileSystem* fs = GetFileSystem();
    if (!fs) {
        LUA->PushBool(false);
        return 1;
    }
    
    bool result = CheckMaterial(fs, materialName, texturePath, debug);
    LUA->PushBool(result);
    return 1;
}

// Lua: HashCollisionFixer.GetMaterialsNeedingFix()
LUA_FUNCTION(HashFixer_GetMaterialsNeedingFix) {
    std::vector<std::string> materials = GetMaterialsNeedingFix();
    
    LUA->CreateTable();
    int i = 1;
    for (const auto& mat : materials) {
        LUA->PushNumber(i++);
        LUA->PushString(mat.c_str());
        LUA->SetTable(-3);
    }
    
    return 1;
}

// Lua: HashCollisionFixer.MarkMaterialFixed(materialName)
LUA_FUNCTION(HashFixer_MarkMaterialFixed) {
    const char* materialName = LUA->CheckString(1);
    MarkMaterialFixed(materialName);
    return 0;
}

// Lua: HashCollisionFixer.IsMaterialFixed(materialName)
LUA_FUNCTION(HashFixer_IsMaterialFixed) {
    const char* materialName = LUA->CheckString(1);
    LUA->PushBool(IsMaterialFixed(materialName));
    return 1;
}

// Lua: HashCollisionFixer.IsSolidColorMaterial(materialName)
LUA_FUNCTION(HashFixer_IsSolidColorMaterial) {
    const char* materialName = LUA->CheckString(1);
    LUA->PushBool(IsSolidColorMaterial(materialName));
    return 1;
}

// Lua: HashCollisionFixer.GetMaterialColor(materialName)
LUA_FUNCTION(HashFixer_GetMaterialColor) {
    const char* materialName = LUA->CheckString(1);
    uint8_t r, g, b, a;
    
    if (GetMaterialColor(materialName, r, g, b, a)) {
        LUA->PushNumber(r);
        LUA->PushNumber(g);
        LUA->PushNumber(b);
        LUA->PushNumber(a);
        return 4;
    }
    
    return 0;
}

// Lua: HashCollisionFixer.Reset()
LUA_FUNCTION(HashFixer_Reset) {
    Reset();
    return 0;
}

// Lua: HashCollisionFixer.GetStats()
LUA_FUNCTION(HashFixer_GetStats) {
    LUA->CreateTable();
    
    LUA->PushString("totalDetected");
    LUA->PushNumber(static_cast<double>(GetTotalDetected()));
    LUA->SetTable(-3);
    
    LUA->PushString("totalFixed");
    LUA->PushNumber(static_cast<double>(GetTotalFixed()));
    LUA->SetTable(-3);
    
    LUA->PushString("pending");
    LUA->PushNumber(static_cast<double>(GetPendingCount()));
    LUA->SetTable(-3);
    
    return 1;
}

// Lua: HashCollisionFixer.CheckSolidColor(texturePath, [debugOutput])
LUA_FUNCTION(HashFixer_CheckSolidColor) {
    const char* texturePath = LUA->CheckString(1);
    bool debug = LUA->IsType(2, GarrysMod::Lua::Type::Bool) ? LUA->GetBool(2) : false;
    
    IFileSystem* fs = GetFileSystem();
    if (!fs) {
        LUA->PushBool(false);
        LUA->PushString("Could not get filesystem interface");
        return 2;
    }
    
    VTFParser::SolidColorResult result = VTFParser::CheckSolidColor(fs, texturePath, debug);
    
    if (result.error) {
        LUA->PushBool(false);
        LUA->PushString(result.errorMessage.c_str());
        return 2;
    }
    
    LUA->PushBool(result.isSolidColor);
    if (result.isSolidColor) {
        LUA->PushNumber(result.r);
        LUA->PushNumber(result.g);
        LUA->PushNumber(result.b);
        LUA->PushNumber(result.a);
        return 5;
    }
    
    return 1;
}

void RegisterLuaBindings(GarrysMod::Lua::ILuaBase* LUA) {
    // Create HashCollisionFixer table
    LUA->PushSpecial(GarrysMod::Lua::SPECIAL_GLOB);
    LUA->CreateTable();
    
    LUA->PushString("CheckMaterial");
    LUA->PushCFunction(HashFixer_CheckMaterial);
    LUA->SetTable(-3);
    
    LUA->PushString("GetMaterialsNeedingFix");
    LUA->PushCFunction(HashFixer_GetMaterialsNeedingFix);
    LUA->SetTable(-3);
    
    LUA->PushString("MarkMaterialFixed");
    LUA->PushCFunction(HashFixer_MarkMaterialFixed);
    LUA->SetTable(-3);
    
    LUA->PushString("IsMaterialFixed");
    LUA->PushCFunction(HashFixer_IsMaterialFixed);
    LUA->SetTable(-3);
    
    LUA->PushString("IsSolidColorMaterial");
    LUA->PushCFunction(HashFixer_IsSolidColorMaterial);
    LUA->SetTable(-3);
    
    LUA->PushString("GetMaterialColor");
    LUA->PushCFunction(HashFixer_GetMaterialColor);
    LUA->SetTable(-3);
    
    LUA->PushString("Reset");
    LUA->PushCFunction(HashFixer_Reset);
    LUA->SetTable(-3);
    
    LUA->PushString("GetStats");
    LUA->PushCFunction(HashFixer_GetStats);
    LUA->SetTable(-3);
    
    LUA->PushString("CheckSolidColor");
    LUA->PushCFunction(HashFixer_CheckSolidColor);
    LUA->SetTable(-3);
    
    LUA->SetField(-2, "HashCollisionFixer");
    LUA->Pop();
    
    Msg("[MaterialPipeline::HashCollisionFixer] Lua bindings registered\n");
}

} // namespace HashCollisionFixer
} // namespace MaterialPipeline

#endif // _WIN64
