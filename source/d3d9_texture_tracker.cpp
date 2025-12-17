#ifdef _WIN64

#include "d3d9_texture_tracker.h"
#include <tier0/dbg.h>
#include <Windows.h>
#include <materialsystem/imaterialsystem.h>
#include <materialsystem/imaterial.h>
#include <materialsystem/imaterialvar.h>
#include <filesystem.h>
#include <algorithm>
#include <functional>
#include <cctype>
#include <remix/remix.h>

// Global material system pointer (from module.cpp)
extern IMaterialSystem* materials;
extern remix::Interface* g_remix;

// Local filesystem pointer - we'll initialize this ourselves
static IFileSystem* s_pFileSystem = nullptr;

// Helper to get filesystem interface
static IFileSystem* GetFileSystem() {
    if (s_pFileSystem) return s_pFileSystem;
    
    // Try to get the filesystem interface from the engine
    HMODULE fsModule = GetModuleHandle("filesystem_stdio.dll");
    if (!fsModule) {
        fsModule = GetModuleHandle("filesystem.dll");
    }
    
    if (fsModule) {
        typedef void* (*CreateInterfaceFn)(const char* name, int* returnCode);
        CreateInterfaceFn createInterface = (CreateInterfaceFn)GetProcAddress(fsModule, "CreateInterface");
        if (createInterface) {
            s_pFileSystem = (IFileSystem*)createInterface("VFileSystem022", nullptr);
            if (!s_pFileSystem) {
                // Try older interface versions
                s_pFileSystem = (IFileSystem*)createInterface("VFileSystem021", nullptr);
            }
            if (!s_pFileSystem) {
                s_pFileSystem = (IFileSystem*)createInterface("VFileSystem017", nullptr);
            }
        }
    }
    
    return s_pFileSystem;
}

// Simple vtable hook implementation
namespace VTableHook {
    // Get vtable pointer from object
    inline void** GetVTable(void* pObject) {
        return *reinterpret_cast<void***>(pObject);
    }

    // Hook a vtable entry
    inline void* HookVTableFunction(void* pObject, size_t index, void* pNewFunc) {
        void** vtable = GetVTable(pObject);
        
        // Make vtable writable
        DWORD oldProtect;
        if (!VirtualProtect(&vtable[index], sizeof(void*), PAGE_READWRITE, &oldProtect)) {
            Warning("[D3D9TextureTracker] Failed to make vtable writable\n");
            return nullptr;
        }

        // Swap function pointer
        void* pOriginal = vtable[index];
        vtable[index] = pNewFunc;

        // Restore protection
        VirtualProtect(&vtable[index], sizeof(void*), oldProtect, &oldProtect);

        return pOriginal;
    }
}

// Singleton instance
D3D9TextureTracker& D3D9TextureTracker::Instance() {
    static D3D9TextureTracker instance;
    return instance;
}

D3D9TextureTracker::~D3D9TextureTracker() {
    Shutdown();
}

bool D3D9TextureTracker::Initialize(IDirect3DDevice9Ex* pDevice) {
    if (m_bInitialized) {
        Warning("[D3D9TextureTracker] Already initialized!\n");
        return false;
    }

    if (!pDevice) {
        Warning("[D3D9TextureTracker] Invalid device pointer!\n");
        return false;
    }

    m_pDevice = pDevice;

    // Hook SetTexture (vtable index 65 for IDirect3DDevice9)
    // SetTexture is method #65 in the IDirect3DDevice9 vtable
    m_pOriginalSetTexture = reinterpret_cast<SetTexture_t>(
        VTableHook::HookVTableFunction(pDevice, 65, &Hook_SetTexture)
    );

    if (!m_pOriginalSetTexture) {
        Warning("[D3D9TextureTracker] Failed to hook SetTexture!\n");
        m_pDevice = nullptr;
        return false;
    }

    // Hook IMatRenderContext::Bind (index 7)
    if (materials) {
        m_pRenderContext = materials->GetRenderContext();
        if (m_pRenderContext) {
            m_pOriginalBind = reinterpret_cast<Bind_t>(
                VTableHook::HookVTableFunction(m_pRenderContext, 7, &Hook_Bind)
            );
            
            if (!m_pOriginalBind) {
                Warning("[D3D9TextureTracker] Failed to hook Bind!\n");
            } else {
                Msg("[D3D9TextureTracker] Hooked IMatRenderContext::Bind\n");
            }
        } else {
            Warning("[D3D9TextureTracker] Failed to get RenderContext!\n");
        }
    } else {
        Warning("[D3D9TextureTracker] Material system not available for Bind hook!\n");
    }

    m_bInitialized = true;
    Msg("[D3D9TextureTracker] Initialized successfully\n");
    
    return true;
}

void D3D9TextureTracker::Shutdown() {
    if (!m_bInitialized) {
        return;
    }

    // Restore original functions
    if (m_pDevice && m_pOriginalSetTexture) {
        VTableHook::HookVTableFunction(m_pDevice, 65, m_pOriginalSetTexture);
    }

    if (m_pRenderContext && m_pOriginalBind) {
        VTableHook::HookVTableFunction(m_pRenderContext, 7, m_pOriginalBind);
    }

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        
        // Release all texture references before clearing
        for (auto& entry : m_textureCache) {
            for (auto* tex : entry.second) {
                if (tex) {
                    tex->Release();
                }
            }
        }
        m_textureCache.clear();
        
        // Clear pending categorizations
        for (auto& pending : m_pendingCategories) {
            if (pending.texture) {
                pending.texture->Release();
            }
        }
        m_pendingCategories.clear();
        
        m_currentMaterialName.clear();
        m_currentMaterial = nullptr;
    }
    
    m_pDevice = nullptr;
    m_pRenderContext = nullptr;
    m_pOriginalSetTexture = nullptr;
    m_pOriginalBind = nullptr;
    m_bInitialized = false;

    Msg("[D3D9TextureTracker] Shutdown complete\n");
}

void D3D9TextureTracker::SetCurrentMaterial(IMaterial* pMaterial) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_currentMaterial = pMaterial;
    
    if (pMaterial) {
        const char* name = pMaterial->GetName();
        if (name) {
            // Normalize to lowercase to handle case-insensitive Source Engine names
            std::string lowerName = name;
            std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(), 
                [](unsigned char c){ return std::tolower(c); });
            m_currentMaterialName = lowerName;
        } else {
            m_currentMaterialName.clear();
        }
    } else {
        m_currentMaterialName.clear();
    }
}

IDirect3DTexture9* D3D9TextureTracker::GetTextureForMaterial(const char* materialName) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!materialName || !materialName[0]) {
        return nullptr;
    }

    // Normalize lookup key
    std::string lowerName = materialName;
    std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(), 
        [](unsigned char c){ return std::tolower(c); });

    auto it = m_textureCache.find(lowerName);
    if (it != m_textureCache.end() && !it->second.empty()) {
        // Return the first texture variant
        // TODO: We might want to try all variants and see which one has a valid hash
        return it->second[0];
    }

    return nullptr;
}

const std::vector<IDirect3DTexture9*>* D3D9TextureTracker::GetTextureVariantsForMaterial(const char* materialName) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!materialName || !materialName[0]) {
        return nullptr;
    }

    // Normalize lookup key
    std::string lowerName = materialName;
    std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(), 
        [](unsigned char c){ return std::tolower(c); });

    auto it = m_textureCache.find(lowerName);
    if (it != m_textureCache.end()) {
        return &it->second;
    }

    return nullptr;
}

void D3D9TextureTracker::ClearCache() {
    std::lock_guard<std::mutex> lock(m_mutex);
    
    // Release all texture references before clearing
    for (auto& entry : m_textureCache) {
        for (auto* tex : entry.second) {
            if (tex) {
                tex->Release();
            }
        }
    }
    m_textureCache.clear();
    
    // Also clear pending categorizations
    for (auto& pending : m_pendingCategories) {
        if (pending.texture) {
            pending.texture->Release();
        }
    }
    m_pendingCategories.clear();
    
    Msg("[D3D9TextureTracker] Cache cleared\n");
}

// Hooked SetTexture function
HRESULT STDMETHODCALLTYPE D3D9TextureTracker::Hook_SetTexture(
    IDirect3DDevice9* pDevice,
    DWORD Stage,
    IDirect3DBaseTexture9* pTexture)
{
    D3D9TextureTracker& tracker = Instance();

    {
        std::lock_guard<std::mutex> lock(tracker.m_mutex);

        // Always log if we have a current material to help debug
#ifdef _DEBUG
        if (Stage == 0 && pTexture && !tracker.m_currentMaterialName.empty()) {
            // Msg("[D3D9TextureTracker] SetTexture(0, %p) for '%s'\n", pTexture, tracker.m_currentMaterialName.c_str());
        }
#endif

        // DEBUG: Log all texture stages for displacement materials
        if (pTexture && !tracker.m_currentMaterialName.empty()) {
            std::string lowerName = tracker.m_currentMaterialName;
            std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(), 
                [](unsigned char c){ return std::tolower(c); });
            
            if (lowerName.find("blend_") != std::string::npos || 
                lowerName.find("_wvt_patch") != std::string::npos) {
                static int dispSetTexCount = 0;
                dispSetTexCount++;
                if (dispSetTexCount <= 20) {
                    Msg("[D3D9TextureTracker] SetTexture(Stage=%d, 0x%p) for displacement '%s'\n", 
                        Stage, pTexture, tracker.m_currentMaterialName.c_str());
                }
            }
        }

        // For now, let's just track ALL textures at stage 0 with a generic key
        // We'll use the texture pointer itself as a way to identify it
        // ALSO track Stage 1 for displacement materials (they use multi-stage blending)
        if ((Stage == 0 || Stage == 1) && pTexture) {
            // Check if this is a 2D texture (not cube/volume)
            D3DRESOURCETYPE resType = pTexture->GetType();
            if (resType == D3DRTYPE_TEXTURE) {
                IDirect3DTexture9* p2DTexture = static_cast<IDirect3DTexture9*>(pTexture);
                
                // If we have a current material name, use it
                if (!tracker.m_currentMaterialName.empty()) {
                    // For Stage 1 textures, append "_stage1" to the material name to track separately
                    std::string trackingName = tracker.m_currentMaterialName;
                    if (Stage == 1) {
                        trackingName += "_stage1";
                    }
                    
                    auto& textures = tracker.m_textureCache[trackingName];
                    
                    // Check if we've seen this texture before
                    bool found = false;
                    for (auto* tex : textures) {
                        if (tex == p2DTexture) {
                            found = true;
                            break;
                        }
                    }
                    
                    // Only log when we discover a NEW variant
                    if (!found) {
                        // Get the hash immediately to see what Remix thinks
                        uint64_t hash = 0;
                        if (g_remix) {
                            auto result = g_remix->dxvk_GetTextureHash(p2DTexture);
                            if (result) {
                                hash = result.value();
                            }
                        }
                        
                        // AddRef to keep the texture alive while we reference it
                        p2DTexture->AddRef();
                        textures.push_back(p2DTexture);
                        // Re-enable logging for debugging texture capture issues
                        Msg("[D3D9TextureTracker] NEW texture variant #%zu: 0x%p for '%s'%s (hash: 0x%llX)\n", 
                            textures.size(), p2DTexture, tracker.m_currentMaterialName.c_str(), 
                            Stage == 1 ? " [STAGE1]" : "", hash);
                            
                        // Apply automatic categorization logic (Particles, Emissive)
                        // Only for Stage 0 to avoid double-categorization
                        if (Stage == 0) {
                            tracker.CheckAndApplyCategories(p2DTexture);
                        }
                    }
                } else {
                    // DEBUG: Log textures that are set without a material name
                    // This helps diagnose why some hashes aren't being tracked
                    static int unmatchedCount = 0;
                    unmatchedCount++;
                    if (unmatchedCount % 1000 == 0) {
                        Msg("[D3D9TextureTracker] WARNING: %d textures set without material name (0x%p)\n", 
                            unmatchedCount, p2DTexture);
                    }
                    
                    // Try to get the hash and log it for debugging
                    if (g_remix && unmatchedCount % 1000 == 0) {
                        auto result = g_remix->dxvk_GetTextureHash(p2DTexture);
                        if (result && result.value() != 0) {
                            Msg("[D3D9TextureTracker] Untracked texture hash: 0x%llX\n", result.value());
                        }
                    }
                }
            }
        }
    }

    // Periodically retry pending categorizations
    // We do this here because SetTexture is called frequently during rendering
    static int setTextureCallCount = 0;
    setTextureCallCount++;
    if (setTextureCallCount % 500 == 0 && !tracker.m_pendingCategories.empty()) {
        tracker.RetryPendingCategories();
    }

    // Call original function
    if (tracker.m_pOriginalSetTexture) {
        return tracker.m_pOriginalSetTexture(pDevice, Stage, pTexture);
    }

    return D3D_OK;
}

// Hooked Bind function
void D3D9TextureTracker::Hook_Bind(IMatRenderContext* pContext, IMaterial* pMaterial, void* proxyData) {
    D3D9TextureTracker& tracker = Instance();
    
    tracker.SetCurrentMaterial(pMaterial);
    
    // DEBUG: Log displacement materials to see if they're being bound
    if (pMaterial) {
        const char* name = pMaterial->GetName();
        if (name) {
            std::string lowerName = name;
            std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(), 
                [](unsigned char c){ return std::tolower(c); });
            
            // Log if it's a displacement blend material
            if (lowerName.find("blend_") != std::string::npos || 
                lowerName.find("_wvt_patch") != std::string::npos) {
                static int dispBindCount = 0;
                dispBindCount++;
                if (dispBindCount <= 10) {
                    Msg("[D3D9TextureTracker] Bind() called for displacement: '%s'\n", name);
                }
            }
        }
    }
    
    if (tracker.m_pOriginalBind) {
        tracker.m_pOriginalBind(pContext, pMaterial, proxyData);
    }
}

// Hash-to-Category mapping implementation
void D3D9TextureTracker::SetHashCategoryFlags(uint64_t textureHash, uint32_t categoryFlags) {
    m_hashToCategoryFlags[textureHash] = categoryFlags;
    Msg("[D3D9TextureTracker] Set category flags 0x%X for hash 0x%llX\n", categoryFlags, textureHash);
}

void D3D9TextureTracker::RemoveHashCategoryFlags(uint64_t textureHash) {
    auto it = m_hashToCategoryFlags.find(textureHash);
    if (it != m_hashToCategoryFlags.end()) {
        m_hashToCategoryFlags.erase(it);
        Msg("[D3D9TextureTracker] Removed category mapping for hash 0x%llX\n", textureHash);
    }
}

void D3D9TextureTracker::ClearHashCategoryMappings() {
    size_t count = m_hashToCategoryFlags.size();
    m_hashToCategoryFlags.clear();
    Msg("[D3D9TextureTracker] Cleared %zu hash-to-category mappings\n", count);
}

bool D3D9TextureTracker::GetHashCategoryFlags(uint64_t textureHash, uint32_t* outCategoryFlags) const {
    auto it = m_hashToCategoryFlags.find(textureHash);
    if (it != m_hashToCategoryFlags.end()) {
        if (outCategoryFlags) {
            *outCategoryFlags = it->second;
        }
        return true;
    }
    return false;
}

bool D3D9TextureTracker::GetMaterialCategoryFlags(const char* materialName, uint32_t* outCategoryFlags) const {
    // This requires the Remix API to get the hash from the material
    // For now, return false - this will be called from Lua with the hash
    return false;
}

std::vector<std::pair<std::string, uint64_t>> D3D9TextureTracker::FindTexturesByName(const std::string& searchName) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<std::pair<std::string, uint64_t>> results;
    
    // Search through all tracked materials
    for (const auto& entry : m_textureCache) {
        const std::string& materialName = entry.first;
        std::string materialLower = materialName;
        std::transform(materialLower.begin(), materialLower.end(), materialLower.begin(), ::tolower);
        
        // Check if the search term is in the material name
        if (materialLower.find(searchName) != std::string::npos) {
            // Calculate hash for this material/texture
            // Use std::hash for now - we just need to identify textures
            std::hash<std::string> hasher;
            uint64_t hash = hasher(materialName);
            results.push_back({materialName, hash});
        }
    }
    
    return results;
}

// Helper: Check if VMT file contains "$selfillum" "1" or "$selfillum" 1
// This is a fallback when IMaterial::FindVar doesn't expose the parameter
static bool CheckVMTForSelfillum(const std::string& materialName, bool debug = false) {
    IFileSystem* fs = GetFileSystem();
    if (!fs) {
        if (debug) Msg("[D3D9] CheckVMT: Could not get filesystem interface\n");
        return false;
    }
    
    // Build VMT path
    std::string vmtPath = "materials/" + materialName + ".vmt";
    
    // Try to open the file
    FileHandle_t file = fs->Open(vmtPath.c_str(), "rb", "GAME");
    if (!file) {
        // Try without .vmt extension (in case materialName already has it)
        vmtPath = "materials/" + materialName;
        file = fs->Open(vmtPath.c_str(), "rb", "GAME");
        if (!file) {
            if (debug) Msg("[D3D9] CheckVMT: Could not open '%s'\n", vmtPath.c_str());
            return false;
        }
    }
    
    // Read file content
    int fileSize = fs->Size(file);
    if (fileSize <= 0 || fileSize > 65536) { // Sanity check
        if (debug) Msg("[D3D9] CheckVMT: Invalid file size %d\n", fileSize);
        fs->Close(file);
        return false;
    }
    
    std::string content;
    content.resize(fileSize);
    int bytesRead = fs->Read(&content[0], fileSize, file);
    fs->Close(file);
    
    if (bytesRead <= 0) {
        if (debug) Msg("[D3D9] CheckVMT: Read failed\n");
        return false;
    }
    
    if (debug) Msg("[D3D9] CheckVMT: Read %d bytes from '%s'\n", bytesRead, vmtPath.c_str());
    
    // Parse line by line to handle comments properly
    // Split content into lines
    std::vector<std::string> lines;
    size_t lineStart = 0;
    for (size_t i = 0; i <= content.size(); ++i) {
        if (i == content.size() || content[i] == '\n' || content[i] == '\r') {
            if (i > lineStart) {
                lines.push_back(content.substr(lineStart, i - lineStart));
            }
            lineStart = i + 1;
        }
    }
    
    // Look for $selfillum followed by 1, excluding commented lines
    for (const auto& line : lines) {
        std::string lowerLine = line;
        std::transform(lowerLine.begin(), lowerLine.end(), lowerLine.begin(), ::tolower);
        
        // Skip empty lines
        if (lowerLine.empty()) continue;
        
        // Skip leading whitespace to check for comments
        size_t firstChar = lowerLine.find_first_not_of(" \t");
        if (firstChar == std::string::npos) continue;
        
        // Skip commented lines
        if (firstChar + 1 < lowerLine.size() && 
            lowerLine[firstChar] == '/' && lowerLine[firstChar + 1] == '/') {
            continue;
        }
        
        // Look for $selfillum with word boundary
        size_t pos = 0;
        while ((pos = lowerLine.find("$selfillum", pos)) != std::string::npos) {
            // Check if it's followed by whitespace, quote, or end of string (not 'tint', 'mask', etc.)
            size_t endPos = pos + 10; // length of "$selfillum"
            if (endPos >= lowerLine.size() || 
                lowerLine[endPos] == ' ' || lowerLine[endPos] == '\t' || 
                lowerLine[endPos] == '"' || lowerLine[endPos] == '\'' ||
                lowerLine[endPos] == '\r' || lowerLine[endPos] == '\n') {
                
                // Found $selfillum, now check if value is 1
                pos = endPos;
                
                // Skip whitespace/quotes
                while (pos < lowerLine.size() && 
                       (lowerLine[pos] == ' ' || lowerLine[pos] == '\t' || 
                        lowerLine[pos] == '"' || lowerLine[pos] == '\'')) {
                    pos++;
                }
                
                // Check if next character is '1'
                if (pos < lowerLine.size() && lowerLine[pos] == '1') {
                    if (debug) Msg("[D3D9] CheckVMT: Found active '$selfillum 1' on line: %s\n", line.c_str());
                    return true;
                }
                
                break; // Found $selfillum but not set to 1
            }
            pos++; // Move forward and keep searching
        }
    }
    
    if (debug) Msg("[D3D9] CheckVMT: '$selfillum 1' not found (or commented out)\n");
    return false;
}

void D3D9TextureTracker::CheckAndApplyCategories(IDirect3DTexture9* pTexture) {
    if (!g_remix) return;
    if (m_currentMaterialName.empty()) return;
    
    // Skip internal/engine materials
    if (m_currentMaterialName.find("__") == 0) return; // __fontpage, __error, etc.
    
    // Lowercase the material name for case-insensitive matching
    std::string lowerName = m_currentMaterialName;
    std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(), ::tolower);
    
    // Category flags - these are internal flags that get mapped to Remix API strings
    // We use the same bit positions as the Lua RemixCategoryManager for consistency
    constexpr uint32_t CAT_SKY            = 0x4;       // bit 2 - rtx.skyBoxTextures
    constexpr uint32_t CAT_IGNORE         = 0x8;       // bit 3 - rtx.ignoreTextures  
    constexpr uint32_t CAT_PARTICLE       = 0x400;     // bit 10 - rtx.particleTextures
    constexpr uint32_t CAT_DECAL_STATIC   = 0x1000;    // bit 12 - rtx.decalTextures
    constexpr uint32_t CAT_ANIMATED_WATER = 0x40000;   // bit 18 - rtx.animatedWaterTextures
    constexpr uint32_t CAT_EMISSIVE       = 0x1000000; // bit 24 - rtx.legacyEmissiveTextures
    
    uint32_t categoryFlags = 0;
    const char* categoryName = nullptr;
    
    // === PRIORITY 1: SKY ===
    // Skybox textures - highest priority
    if (lowerName.find("tools/toolsskybox") != std::string::npos ||
        lowerName.find("skybox/") == 0 ||
        lowerName.find("/skybox/") != std::string::npos) {
        categoryFlags = CAT_SKY;
        categoryName = "SKY";
    }
    
    // === PRIORITY 2: IGNORE ===
    // Nodraw, invisible, clip textures - should be ignored by RTX
    else if (lowerName.find("tools/toolsnodraw") != std::string::npos ||
             lowerName.find("tools/toolsinvisible") != std::string::npos ||
             lowerName.find("tools/toolsclip") != std::string::npos ||
             lowerName.find("tools/toolsplayerclip") != std::string::npos ||
             lowerName.find("tools/toolsnpcclip") != std::string::npos ||
             lowerName.find("tools/toolstrigger") != std::string::npos ||
             lowerName.find("tools/toolsblocklight") != std::string::npos ||
             lowerName.find("tools/toolsareaportal") != std::string::npos ||
             lowerName.find("tools/toolsoccluder") != std::string::npos) {
        categoryFlags = CAT_IGNORE;
        categoryName = "IGNORE";
    }
    
    // === PRIORITY 3: PARTICLES ===
    // Check path prefixes
    else if (lowerName.find("particles/") == 0 || 
             lowerName.find("particle/") == 0 ||
             lowerName.find("effects/") == 0 || 
             lowerName.find("sprites/") == 0 ||
             lowerName.find("/particles/") != std::string::npos ||
             lowerName.find("/particle/") != std::string::npos ||
             lowerName.find("/effects/") != std::string::npos ||
             lowerName.find("/sprites/") != std::string::npos) {
        categoryFlags = CAT_PARTICLE;
        categoryName = "PARTICLE";
    }
    
    // === PRIORITY 4: WATER ===
    else if (lowerName.find("water") != std::string::npos ||
             lowerName.find("slime") != std::string::npos) {
        // Check if it's actually a water shader
        if (m_currentMaterial) {
            const char* shaderName = m_currentMaterial->GetShaderName();
            if (shaderName) {
                std::string s = shaderName;
                std::transform(s.begin(), s.end(), s.begin(), ::tolower);
                if (s.find("water") != std::string::npos || 
                    s.find("refract") != std::string::npos) {
                    categoryFlags = CAT_ANIMATED_WATER;
                    categoryName = "WATER";
                }
            }
        }
        // Fallback: if name contains water but shader isn't water, still mark it
        if (categoryFlags == 0 && lowerName.find("water") != std::string::npos) {
            categoryFlags = CAT_ANIMATED_WATER;
            categoryName = "WATER";
        }
    }
    
    // === PRIORITY 5: DECALS (map overlays, bullet holes, blood) ===
    // IMPORTANT: Exclude light materials (they should be emissive, not decals)
    else if ((lowerName.find("decals/") == 0 ||
             lowerName.find("/decals/") != std::string::npos ||
             lowerName.find("overlay") != std::string::npos ||
             lowerName.find("bulleth") != std::string::npos ||
             lowerName.find("_blood") != std::string::npos ||
             lowerName.find("blood_") != std::string::npos ||
             lowerName.find("/blood") != std::string::npos ||
             lowerName.find("scorch") != std::string::npos) &&
             // Exclude light materials
             lowerName.find("light") == std::string::npos &&
             lowerName.find("/lights/") == std::string::npos &&
             lowerName.find("lights/") != 0) {
        categoryFlags = CAT_DECAL_STATIC;
        categoryName = "DECAL";
    }
    
    // If no category matched yet, check shader-based detection
    if (categoryFlags == 0 && m_currentMaterial) {
        const char* shaderName = m_currentMaterial->GetShaderName();
        if (shaderName) {
            std::string s = shaderName;
            std::transform(s.begin(), s.end(), s.begin(), ::tolower);
            
            // Particle shaders
            if (s.find("sprite") != std::string::npos ||
                s.find("modulate") != std::string::npos ||
                s == "cable") {
                categoryFlags = CAT_PARTICLE;
                categoryName = "PARTICLE";
            }
            // UnlitGeneric with $vertexalpha + $vertexcolor = particle/effect
            else if (s.find("unlitgeneric") != std::string::npos) {
                bool hasVertexAlpha = false;
                bool hasVertexColor = false;
                
                bool foundVA = false;
                IMaterialVar* pVA = m_currentMaterial->FindVar("$vertexalpha", &foundVA, false);
                if (foundVA && pVA && pVA->GetIntValue() == 1) {
                    hasVertexAlpha = true;
                }
                
                bool foundVC = false;
                IMaterialVar* pVC = m_currentMaterial->FindVar("$vertexcolor", &foundVC, false);
                if (foundVC && pVC && pVC->GetIntValue() == 1) {
                    hasVertexColor = true;
                }
                
                if (hasVertexAlpha && hasVertexColor) {
                    categoryFlags = CAT_PARTICLE;
                    categoryName = "PARTICLE";
                }
            }
        }
    }
    
    // === EMISSIVE CHECK (can be combined with other categories) ===
    // IMPORTANT: Skip emissive check if this is a decal (decals should never be emissive)
    bool isDecal = (categoryFlags & CAT_DECAL_STATIC) != 0;
    
    bool isEmissive = false;
    std::string emissiveReason;
    
    // Method 1: Check IMaterial::FindVar for $selfillum
    if (!isDecal && m_currentMaterial) {
        bool found = false;
        IMaterialVar* pVar = m_currentMaterial->FindVar("$selfillum", &found, false);
        if (found && pVar && pVar->GetIntValue() == 1) {
            isEmissive = true;
            emissiveReason = "IMaterial::FindVar($selfillum) = 1";
        }
        
        // Method 2: Check $emissive var (vector)
        if (!isEmissive) {
            bool foundEmissive = false;
            IMaterialVar* pEmissiveVar = m_currentMaterial->FindVar("$emissive", &foundEmissive, false);
            if (foundEmissive && pEmissiveVar) {
                const float* val = pEmissiveVar->GetVecValue();
                if (val && (val[0] > 0.0f || val[1] > 0.0f || val[2] > 0.0f)) {
                    isEmissive = true;
                    emissiveReason = "$emissive vector";
                }
            }
        }
    }
    
    // Method 3: Fallback - read VMT file directly
    // This catches cases where the engine doesn't expose $selfillum via FindVar
    // (common with DX7/DX6 shader fallbacks)
    // SKIP for decals
    if (!isDecal && !isEmissive) {
        if (CheckVMTForSelfillum(m_currentMaterialName)) {
            isEmissive = true;
            emissiveReason = "VMT file $selfillum";
        }
    }
    
    // Method 4: Keyword-based detection for VTF self-illumination
    // These materials use VTF-based self-illumination (no VMT parameter)
    // SKIP for decals
    if (!isDecal && !isEmissive) {
        // Pattern 1: Material name contains "_on" (light fixtures, LEDs, screens, etc.)
        // AND is from known emissive content packs (pkvoidplaces, pb_ prefix)
        // Examples: pkvoidplaces/props/pb_propremake_ceilinglightbase_on
        //           pkvoidplaces/props/pb_pmall_prop_led_pink_on
        bool hasOnSuffix = (lowerName.find("_on") != std::string::npos);
        bool isKnownEmissivePack = (lowerName.find("pkvoidplaces") != std::string::npos ||
                                    lowerName.find("/pb_") != std::string::npos ||
                                    lowerName.find("pb_") == 0);
        
        // Pattern 2: Contains "light" + "_on" anywhere (generic light fixtures)
        bool hasLightOn = (lowerName.find("light") != std::string::npos && 
                          lowerName.find("_on") != std::string::npos);
        
        if ((hasOnSuffix && isKnownEmissivePack) || hasLightOn) {
            isEmissive = true;
            emissiveReason = "Keyword pattern (_on suffix)";
        }
    }
    
    // Combine emissive flag
    if (isEmissive) {
        categoryFlags |= CAT_EMISSIVE;
        if (!categoryName) categoryName = "EMISSIVE";
        
        // Debug: Log when decals are marked as emissive (should never happen now)
        if (categoryFlags & CAT_DECAL_STATIC) {
            Msg("[D3D9] ERROR: Decal '%s' was marked as EMISSIVE (reason: %s) - this should not happen!\n", 
                m_currentMaterialName.c_str(), emissiveReason.c_str());
        }
    }
    
    // If nothing to categorize, skip
    if (categoryFlags == 0) return;
    
    // Get hash
    auto result = g_remix->dxvk_GetTextureHash(pTexture);
    if (!result) return;
    uint64_t hash = result.value();
    
    // If hash is 0, add to pending queue for later retry
    if (hash == 0) {
        // Check if already in pending queue
        bool alreadyPending = false;
        for (const auto& pending : m_pendingCategories) {
            if (pending.texture == pTexture) {
                alreadyPending = true;
                break;
            }
        }
        if (!alreadyPending) {
            pTexture->AddRef(); // Keep texture alive
            m_pendingCategories.push_back({pTexture, m_currentMaterialName, categoryFlags});
        }
        return;
    }
    
    // Check if this hash has already been categorized
    uint32_t existingFlags = 0;
    if (GetHashCategoryFlags(hash, &existingFlags) && existingFlags != 0) {
        // Already categorized, don't re-categorize
        return;
    }
    
    // Apply the category
    ApplyCategoryToHash(hash, categoryFlags, m_currentMaterialName.c_str());
}

// Helper function to apply category flags to a texture hash
void D3D9TextureTracker::ApplyCategoryToHash(uint64_t hash, uint32_t categoryFlags, const char* materialName) {
    if (!g_remix || hash == 0 || categoryFlags == 0) return;
    
    // Category flag constants (same as in CheckAndApplyCategories)
    constexpr uint32_t CAT_WORLD_UI       = 0x1;       // UNUSED - do not apply
    constexpr uint32_t CAT_SKY            = 0x4;
    constexpr uint32_t CAT_IGNORE         = 0x8;
    constexpr uint32_t CAT_PARTICLE       = 0x400;
    constexpr uint32_t CAT_DECAL_STATIC   = 0x1000;
    constexpr uint32_t CAT_ANIMATED_WATER = 0x40000;
    constexpr uint32_t CAT_EMISSIVE       = 0x1000000;
    
    // Strip WORLD_UI flag if present - it should never be applied
    if (categoryFlags & CAT_WORLD_UI) {
        Msg("[D3D9] WARNING: Removing WORLD_UI flag from '%s' (flags: 0x%X)\n", materialName, categoryFlags);
        categoryFlags &= ~CAT_WORLD_UI;  // Remove the WORLD_UI bit
    }
    
    char hashStr[32];
    sprintf_s(hashStr, "0x%llX", hash);
    
    // Map category flags to Remix API texture lists
    // NOTE: WORLD_UI is intentionally omitted - we don't use it
    if (categoryFlags & CAT_SKY) {
        g_remix->AddTextureHash("rtx.skyBoxTextures", hashStr);
        Msg("[D3D9] Categorized SKY: '%s' -> %s\n", materialName, hashStr);
    }
    if (categoryFlags & CAT_IGNORE) {
        g_remix->AddTextureHash("rtx.ignoreTextures", hashStr);
        Msg("[D3D9] Categorized IGNORE: '%s' -> %s\n", materialName, hashStr);
    }
    if (categoryFlags & CAT_PARTICLE) {
        g_remix->AddTextureHash("rtx.particleTextures", hashStr);
        Msg("[D3D9] Categorized PARTICLE: '%s' -> %s\n", materialName, hashStr);
    }
    if (categoryFlags & CAT_DECAL_STATIC) {
        g_remix->AddTextureHash("rtx.decalTextures", hashStr);
        Msg("[D3D9] Categorized DECAL: '%s' -> %s\n", materialName, hashStr);
    }
    if (categoryFlags & CAT_ANIMATED_WATER) {
        g_remix->AddTextureHash("rtx.animatedWaterTextures", hashStr);
        Msg("[D3D9] Categorized WATER: '%s' -> %s\n", materialName, hashStr);
    }
    if (categoryFlags & CAT_EMISSIVE) {
        g_remix->AddTextureHash("rtx.legacyEmissiveTextures", hashStr);
        Msg("[D3D9] Categorized EMISSIVE: '%s' -> %s\n", materialName, hashStr);
    }
    
    // Update local tracking (without WORLD_UI bit)
    uint32_t currentFlags = 0;
    GetHashCategoryFlags(hash, &currentFlags);
    SetHashCategoryFlags(hash, currentFlags | categoryFlags);
}

int D3D9TextureTracker::RetryPendingCategories() {
    if (!g_remix) return 0;
    
    if (m_pendingCategories.empty()) return 0;
    
    int successCount = 0;
    std::vector<PendingCategory> stillPending;
    
    for (auto& pending : m_pendingCategories) {
        auto result = g_remix->dxvk_GetTextureHash(pending.texture);
        if (!result) {
            stillPending.push_back(pending);
            continue;
        }
        
        uint64_t hash = result.value();
        if (hash == 0) {
            stillPending.push_back(pending);
            continue;
        }
        
        // Got a valid hash! Log it for debugging
        Msg("[D3D9TextureTracker] RetryPending: Got hash 0x%llX for '%s' (texture 0x%p)\n",
            hash, pending.materialName.c_str(), pending.texture);
        
        // Apply categories using the helper function
        ApplyCategoryToHash(hash, pending.categoryFlags, pending.materialName.c_str());
        
        // Release our reference
        pending.texture->Release();
        successCount++;
    }
    
    m_pendingCategories = std::move(stillPending);
    
    if (successCount > 0) {
        Msg("[D3D9] RetryPendingCategories: %d categorized, %zu still pending\n", 
            successCount, m_pendingCategories.size());
    }
    
    return successCount;
}

int D3D9TextureTracker::RescanAllMaterials() {
    if (!g_remix || !materials) return 0;
    
    Msg("[D3D9] RescanAllMaterials: Scanning %zu cached materials...\n", m_textureCache.size());
    
    // Category flag constants
    constexpr uint32_t CAT_SKY            = 0x4;
    constexpr uint32_t CAT_IGNORE         = 0x8;
    constexpr uint32_t CAT_PARTICLE       = 0x400;
    constexpr uint32_t CAT_DECAL_STATIC   = 0x1000;
    constexpr uint32_t CAT_ANIMATED_WATER = 0x40000;
    constexpr uint32_t CAT_EMISSIVE       = 0x1000000;
    
    int categorizedCount = 0;
    int emissiveCount = 0;
    
    for (const auto& entry : m_textureCache) {
        const std::string& materialName = entry.first;
        const std::vector<IDirect3DTexture9*>& textures = entry.second;
        
        if (textures.empty()) continue;
        
        // Skip internal materials
        if (materialName.find("__") == 0) continue;
        
        // Lowercase for matching
        std::string lowerName = materialName;
        std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(), ::tolower);
        
        // Get the material
        IMaterial* mat = materials->FindMaterial(materialName.c_str(), TEXTURE_GROUP_OTHER, false);
        
        // Skip decals - they should never be emissive
        bool isDecalMaterial = (lowerName.find("decal") != std::string::npos || 
                                lowerName.find("overlay") != std::string::npos);
        
        if (isDecalMaterial) {
            continue; // Don't even check decals for emissive
        }
        
        // Check for emissive using VMT file fallback
        bool isEmissive = false;
        
        // Method 1: IMaterial::FindVar
        if (mat && !mat->IsErrorMaterial()) {
            bool found = false;
            IMaterialVar* pVar = mat->FindVar("$selfillum", &found, false);
            if (found && pVar && pVar->GetIntValue() == 1) {
                isEmissive = true;
            }
        }
        
        // Method 2: VMT file fallback (with debug output for first few)
        if (!isEmissive) {
            isEmissive = CheckVMTForSelfillum(materialName, emissiveCount < 3); // Debug first 3
        }
        
        if (isEmissive) {
            emissiveCount++;
            
            // Debug: Show which material is being marked (first 10)
            if (emissiveCount <= 10) {
                std::string lowerCheck = materialName;
                std::transform(lowerCheck.begin(), lowerCheck.end(), lowerCheck.begin(), ::tolower);
                bool isDecal = (lowerCheck.find("decal") != std::string::npos || 
                               lowerCheck.find("overlay") != std::string::npos);
                if (isDecal) {
                    Msg("[D3D9] RescanAllMaterials: DECAL marked as emissive: '%s'\n", materialName.c_str());
                } else {
                    Msg("[D3D9] RescanAllMaterials: Emissive material #%d: '%s'\n", emissiveCount, materialName.c_str());
                }
            }
            
            // Get hash for first texture variant
            for (auto* tex : textures) {
                auto result = g_remix->dxvk_GetTextureHash(tex);
                if (result && result.value() != 0) {
                    ApplyCategoryToHash(result.value(), CAT_EMISSIVE, materialName.c_str());
                    categorizedCount++;
                    break; // Only need to categorize once per material
                }
            }
        }
    }
    
    Msg("[D3D9] RescanAllMaterials: Found %d emissive materials, categorized %d\n", 
        emissiveCount, categorizedCount);
    
    return categorizedCount;
}

std::vector<std::tuple<std::string, void*, uint64_t>> D3D9TextureTracker::DumpAllTextureHashes() const {
    std::vector<std::tuple<std::string, void*, uint64_t>> result;
    
    if (!g_remix) return result;
    
    std::lock_guard<std::mutex> lock(m_mutex);
    
    for (const auto& entry : m_textureCache) {
        const std::string& materialName = entry.first;
        const std::vector<IDirect3DTexture9*>& textures = entry.second;
        
        for (auto* tex : textures) {
            if (!tex) continue;
            
            auto hashResult = g_remix->dxvk_GetTextureHash(tex);
            uint64_t hash = hashResult ? hashResult.value() : 0;
            
            result.push_back(std::make_tuple(materialName, (void*)tex, hash));
        }
    }
    
    return result;
}

#endif // _WIN64

