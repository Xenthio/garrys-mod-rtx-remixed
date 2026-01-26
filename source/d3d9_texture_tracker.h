#pragma once

#ifdef _WIN64

#include <d3d9.h>
#include <unordered_map>
#include <unordered_set>
#include <string>
#include <vector>
#include <mutex>
#include <cstddef>

// Forward declarations
class IMaterial;
class IMatRenderContext;

// D3D9 Texture Tracker
// Hooks IDirect3DDevice9::SetTexture to track which textures are used by which materials
class D3D9TextureTracker {
public:
    static D3D9TextureTracker& Instance();

    // Initialize the tracker with the D3D9 device
    bool Initialize(IDirect3DDevice9Ex* pDevice);
    
    // Shutdown and remove hooks
    void Shutdown();

    // Track a material being rendered
    void SetCurrentMaterial(IMaterial* pMaterial);
    
    // Check and apply automatic categories (particles, emissive, sky, water, etc.)
    void CheckAndApplyCategories(IDirect3DTexture9* pTexture);
    
    // Apply category flags to a texture hash (used by CheckAndApply and Retry)
    void ApplyCategoryToHash(uint64_t hash, uint32_t categoryFlags, const char* materialName);
    
    // Re-scan all cached materials and apply categories
    // This is useful after code changes or to catch materials that were cached before detection was added
    int RescanAllMaterials();
    
    // Re-check all cached materials for world texture categorization
    // Useful after SetWorldTextureNames is called to categorize materials that rendered before the list was loaded
    int RecheckWorldTextures();
    
    // Get the D3D9 texture for a material (returns null if not found)
    IDirect3DTexture9* GetTextureForMaterial(const char* materialName);
    
    // Get all texture variants for a material
    const std::vector<IDirect3DTexture9*>* GetTextureVariantsForMaterial(const char* materialName);

    // Clear the cache (useful for map changes)
    void ClearCache();

    // Get cache statistics
    size_t GetCacheSize() const { return m_textureCache.size(); }

    // Get all cached materials
    std::vector<std::string> GetCachedMaterials() const {
        std::vector<std::string> materials;
        materials.reserve(m_textureCache.size());
        for (const auto& pair : m_textureCache) {
            materials.push_back(pair.first);
        }
        return materials;
    }

    // Hash-to-Category mapping system
    void SetHashCategoryFlags(uint64_t textureHash, uint32_t categoryFlags);
    void RemoveHashCategoryFlags(uint64_t textureHash);
    void ClearHashCategoryMappings();
    bool GetHashCategoryFlags(uint64_t textureHash, uint32_t* outCategoryFlags) const;
    
    // Get category flags for a material based on its texture hash
    bool GetMaterialCategoryFlags(const char* materialName, uint32_t* outCategoryFlags) const;
    
    // Find textures by partial name match (returns name->hash pairs)
    std::vector<std::pair<std::string, uint64_t>> FindTexturesByName(const std::string& searchName) const;
    
    // Retry categorization for pending textures (those that returned hash=0)
    // Returns number of textures successfully categorized
    int RetryPendingCategories();
    
    // Get count of pending textures
    size_t GetPendingCount() const { return m_pendingCategories.size(); }
    
    // Dump all tracked textures with their hashes (for debugging)
    // Returns vector of (materialName, texturePtr, hash) tuples
    std::vector<std::tuple<std::string, void*, uint64_t>> DumpAllTextureHashes() const;
    
    // Set the list of world texture names (from BSP parsing)
    // These will be marked as DECAL_STATIC when rendered
    void SetWorldTextureNames(const std::vector<std::string>& textureNames);
    
    // Clear the world texture list (for map changes)
    void ClearWorldTextureNames();
    
    // Check if a material is in the world texture list
    bool IsWorldTexture(const std::string& materialName) const;
    
    // Enable/disable automatic particle categorization
    void SetParticleCategorization(bool enabled);
    
    // Enable/disable automatic decal categorization
    void SetDecalCategorization(bool enabled);
    
    // Enable/disable automatic emissive categorization
    void SetEmissiveCategorization(bool enabled);
    
    // Enable/disable ALL automatic categorization (master switch)
    void SetAutoCategorization(bool enabled);
    
    // Enable/disable debug output
    void SetDebugOutput(bool enabled);
    
    // Create a modified copy of a texture with slightly different pixels (for hash collision fix)
    // Returns the new texture and its hash, or nullptr/0 if failed
    // The modification is based on materialName to ensure unique hashes for different materials
    IDirect3DTexture9* CreateModifiedTexture(IDirect3DTexture9* pOriginal, const std::string& materialName, uint64_t* outHash);
    
    // Check for hash collision and fix if possible (returns the fixed hash, or original if no fix needed/possible)
    // This is called automatically when new textures are detected, independent of LegacyTextureProcessor
    uint64_t CheckAndFixHashCollision(IDirect3DTexture9* pTexture, const std::string& materialName, uint64_t originalHash);
    
    // Get hash collision groups: returns a map of hash -> list of material names that share that hash
    // Only returns hashes that have multiple materials (actual collisions)
    std::unordered_map<uint64_t, std::vector<std::string>> GetHashCollisions() const;
    
    // Check if a specific material has a hash collision with another material
    // Returns the other material name(s) if collision exists, empty vector otherwise
    std::vector<std::string> GetMaterialCollisions(const std::string& materialName) const;
    
    // Get the D3D9 device (for texture creation)
    IDirect3DDevice9Ex* GetDevice() const { return m_pDevice; }

private:
    // Pending categorization entry
    struct PendingCategory {
        IDirect3DTexture9* texture;
        std::string materialName;
        uint32_t categoryFlags;  // Combined category flags (SKY, PARTICLE, WATER, etc.)
    };
    D3D9TextureTracker() = default;
    ~D3D9TextureTracker();

    // Prevent copying
    D3D9TextureTracker(const D3D9TextureTracker&) = delete;
    D3D9TextureTracker& operator=(const D3D9TextureTracker&) = delete;

    // Hook functions
    static HRESULT STDMETHODCALLTYPE Hook_SetTexture(
        IDirect3DDevice9* pDevice,
        DWORD Stage,
        IDirect3DBaseTexture9* pTexture);

    static void Hook_Bind(
        IMatRenderContext* pContext,
        IMaterial* pMaterial,
        void* proxyData);

    // Original function pointer
    typedef HRESULT (STDMETHODCALLTYPE *SetTexture_t)(
        IDirect3DDevice9* pDevice,
        DWORD Stage,
        IDirect3DBaseTexture9* pTexture);
    
    typedef void (*Bind_t)(
        IMatRenderContext* pContext,
        IMaterial* pMaterial,
        void* proxyData);

    SetTexture_t m_pOriginalSetTexture = nullptr;
    Bind_t m_pOriginalBind = nullptr;
    
    IDirect3DDevice9Ex* m_pDevice = nullptr;
    IMatRenderContext* m_pRenderContext = nullptr;
    
    // Current material being rendered (set by Bind hooks)
    std::string m_currentMaterialName;
    IMaterial* m_currentMaterial = nullptr;
    
    // Cache: material name -> set of D3D9 textures (materials can have multiple texture variants)
    std::unordered_map<std::string, std::vector<IDirect3DTexture9*>> m_textureCache;
    
    // Hash to category flags mapping
    std::unordered_map<uint64_t, uint32_t> m_hashToCategoryFlags;
    
    // Hash to material names mapping (for collision detection)
    // Maps a hash to ALL materials that use textures with that hash
    std::unordered_map<uint64_t, std::vector<std::string>> m_hashToMaterialNames;
    
    // Modified textures created for collision fixes (keep references to prevent cleanup)
    std::vector<IDirect3DTexture9*> m_modifiedTextures;
    
    // Pending categorizations (textures that returned hash=0)
    std::vector<PendingCategory> m_pendingCategories;
    
    // World texture names from BSP (for DECAL_STATIC marking)
    std::unordered_set<std::string> m_worldTextureNames;
    
    // Category enable flags
    bool m_enableAutoCategorization = true;     // Master switch
    bool m_enableParticleCategorization = true;
    bool m_enableDecalCategorization = true;
    bool m_enableEmissiveCategorization = true;
    
    // Debug output flag
    bool m_enableDebugOutput = false;
    
    // Track whether we're initialized
    bool m_bInitialized = false;
    
    // Mutex for thread safety
    mutable std::mutex m_mutex;
};

#endif // _WIN64

