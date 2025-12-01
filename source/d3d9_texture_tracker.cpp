#ifdef _WIN64

#include "d3d9_texture_tracker.h"
#include <tier0/dbg.h>
#include <Windows.h>
#include <materialsystem/imaterialsystem.h>
#include <materialsystem/imaterial.h>
#include <algorithm>
#include <functional>

// Global material system pointer (from module.cpp)
extern IMaterialSystem* materials;

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
        m_currentMaterial.clear();
    }
    
    m_pDevice = nullptr;
    m_pRenderContext = nullptr;
    m_pOriginalSetTexture = nullptr;
    m_pOriginalBind = nullptr;
    m_bInitialized = false;

    Msg("[D3D9TextureTracker] Shutdown complete\n");
}

void D3D9TextureTracker::SetCurrentMaterial(const char* materialName) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (materialName && materialName[0]) {
        // Normalize to lowercase to handle case-insensitive Source Engine names
        std::string lowerName = materialName;
        std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(), 
            [](unsigned char c){ return std::tolower(c); });
        m_currentMaterial = lowerName;
    } else {
        m_currentMaterial.clear();
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
        if (Stage == 0 && pTexture && !tracker.m_currentMaterial.empty()) {
            // Msg("[D3D9TextureTracker] SetTexture(0, %p) for '%s'\n", pTexture, tracker.m_currentMaterial.c_str());
        }
#endif

        // For now, let's just track ALL textures at stage 0 with a generic key
        // We'll use the texture pointer itself as a way to identify it
        if (Stage == 0 && pTexture) {
            // Check if this is a 2D texture (not cube/volume)
            D3DRESOURCETYPE resType = pTexture->GetType();
            if (resType == D3DRTYPE_TEXTURE) {
                IDirect3DTexture9* p2DTexture = static_cast<IDirect3DTexture9*>(pTexture);
                
                // If we have a current material name, use it
                if (!tracker.m_currentMaterial.empty()) {
                    auto& textures = tracker.m_textureCache[tracker.m_currentMaterial];
                    
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
                        // AddRef to keep the texture alive while we reference it
                        p2DTexture->AddRef();
                        textures.push_back(p2DTexture);
                        // Re-enable logging for debugging texture capture issues
                        Msg("[D3D9TextureTracker] NEW texture variant #%zu: 0x%p for '%s'\n", 
                            textures.size(), p2DTexture, tracker.m_currentMaterial.c_str());
                    }
                }
                // No else block needed - we silently ignore untracked textures now
            }
        }
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
    
    if (pMaterial) {
        const char* name = pMaterial->GetName();
        tracker.SetCurrentMaterial(name);
    } else {
        tracker.SetCurrentMaterial(nullptr);
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

#endif // _WIN64

