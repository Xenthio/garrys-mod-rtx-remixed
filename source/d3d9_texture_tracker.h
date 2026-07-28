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
    // Stable, AddRef'd snapshot of a material's tracked D3D textures.
    // The tracker can keep accepting render-thread observations while callers
    // inspect this copy without retaining a pointer into an unordered_map value.
    class TextureSnapshot {
    public:
        TextureSnapshot() = default;
        ~TextureSnapshot();

        TextureSnapshot(const TextureSnapshot&) = delete;
        TextureSnapshot& operator=(const TextureSnapshot&) = delete;
        TextureSnapshot(TextureSnapshot&& other) noexcept;
        TextureSnapshot& operator=(TextureSnapshot&& other) noexcept;

        bool empty() const { return m_textures.empty(); }
        size_t size() const { return m_textures.size(); }
        IDirect3DTexture9* operator[](size_t index) const { return m_textures[index]; }
        std::vector<IDirect3DTexture9*>::const_iterator begin() const { return m_textures.begin(); }
        std::vector<IDirect3DTexture9*>::const_iterator end() const { return m_textures.end(); }
        const std::vector<IDirect3DTexture9*>& Get() const { return m_textures; }

    private:
        friend class D3D9TextureTracker;
        void Reset();
        std::vector<IDirect3DTexture9*> m_textures;
    };

    static D3D9TextureTracker& Instance();

    // Initialize the tracker with the D3D9 device
    bool Initialize(IDirect3DDevice9Ex* pDevice);
    
    // Shutdown and remove hooks
    void Shutdown();

    // Track a material being rendered
    void SetCurrentMaterial(IMaterial* pMaterial);
    
    // NOTE: CheckAndApplyCategories was removed - all categorisation now goes through
    // MaterialPipeline::AutoCategorisation::DetectAndApply() (Stage 3 of the pipeline).
    
    // Apply category flags to a texture hash (used by RecheckWorldTextures and RetryPending)
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
    TextureSnapshot GetTextureVariantsForMaterial(const char* materialName);

    // Invalidate/clear cached textures for a specific material
    // Call this when a material's $basetexture is changed at runtime (e.g., by Lua)
    // Returns the number of textures cleared
    size_t InvalidateMaterialCache(const char* materialName);

    // Clear the cache (useful for map changes)
    void ClearCache();

    // Get cache statistics
    size_t GetCacheSize() const;
    
    // Check if initialized
    bool IsInitialized() const { return m_bInitialized; }

    // Get all cached materials
    std::vector<std::string> GetCachedMaterials() const;

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

    // Retry pipeline notification for textures whose hash was 0 at discovery time.
    // When Remix eventually returns a non-zero hash, we fire OnNewMaterialDetected
    // and update m_hashToMaterials / perform collision detection.
    // Returns the number of entries resolved this call.
    int RetryPendingHashResolution();
    
    // Get count of pending textures
    size_t GetPendingCount() const { return m_pendingCategories.size(); }

    // Get count of textures waiting for their initial hash
    size_t GetPendingHashResolutionCount() const { return m_pendingHashResolution.size(); }
    
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

    // Register a material as BSP world geometry (regardless of category assignment).
    // Populates m_bspWorldMaterials and back-fills m_hashToMaterials from the texture
    // cache for any already-rendered variants; retroactively removes any stale PARTICLE
    // tag if the hash was claimed before the BSP scan ran.
    void RegisterBSPWorldMaterial(const std::string& materialName);

    // Clear the BSP world material registry (call on map change).
    void ClearBSPWorldMaterials();

    // Returns true if any material known to share this hash is registered as BSP
    // world geometry, meaning PARTICLE should not be applied to it.
    bool IsAnyBSPWorldMaterialForHash(uint64_t hash) const;

    // Snapshot all verified Stage 0 material names currently associated with a
    // Remix hash. Used to reject global category bits when the Source materials
    // sharing that hash disagree (notably LEGACY_EMISSIVE).
    std::vector<std::string> GetMaterialsForHash(uint64_t hash) const;

    // Returns true if any material known to share this hash is NOT registered as
    // BSP world geometry, meaning DECAL_STATIC should not be applied to that hash
    // (it would incorrectly tag non-world materials such as model textures).
    bool HasNonBSPWorldMaterialForHash(uint64_t hash) const;

    // Returns true if any material known to share this hash is NOT in the world
    // texture DECAL_STATIC list.  This is the accurate pre-emptive guard: it covers
    // both non-BSP model textures and BSP brushes intentionally excluded from the
    // decal list (e.g. translucent or $nodecal surfaces).
    bool HasMaterialNotInWorldListForHash(uint64_t hash) const;

    // Mark a hash as "contested" for DECAL_STATIC: it is shared between a BSP world
    // brush and at least one non-world material, so DECAL_STATIC must never be applied.
    // This state persists for the lifetime of the map (cleared on ClearCache /
    // ClearBSPWorldMaterials) so that RecheckWorldTextures cannot re-apply the tag.
    void MarkHashContested(uint64_t hash);

    // Returns true if this hash has been permanently blocked from DECAL_STATIC.
    bool IsHashContested(uint64_t hash) const;
    
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

private:
    // Pending categorization entry
    struct PendingCategory {
        IDirect3DTexture9* texture;
        std::string materialName;
        uint32_t categoryFlags;  // Combined category flags (SKY, PARTICLE, WATER, etc.)
    };

    // Pending BSP hash resolution entry.
    // When a BSP world material's new texture variant has hash=0 (RTX Remix hasn't
    // assigned it yet), we queue it here so RetryPendingCategories can resolve the
    // hash later and retroactively remove any stale PARTICLE tag.
    struct PendingBSPHash {
        IDirect3DTexture9* texture;
        std::string materialName;
    };

    // Deferred pipeline-notification entry.
    // When a new Stage 0 texture variant is discovered but dxvk_GetTextureHash
    // returns 0 (Remix hasn't processed the texture yet), we queue the pointer here
    // instead of calling OnNewMaterialDetected immediately.  RetryPendingHashResolution
    // polls until a non-zero hash is available, then fires the notification.
    // The texture pointer is already AddRef'd by m_textureCache; no extra AddRef needed.
    struct PendingHashResolution {
        IDirect3DTexture9* texture;
        std::string materialName;
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

    // Re-hook Bind if the render context instance has changed
    void EnsureBindHook();

    // Verify that a bound D3D texture agrees with the Source material sampler
    // we intend to track ($basetexture for Stage 0, $basetexture2 for Stage 1).
    // Must be called while m_mutex is held.
    bool ValidateTextureAssociation(
        IMaterial* material,
        DWORD stage,
        IDirect3DTexture9* texture,
        const std::string& materialName);

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
    
    // Per-material cache: maps material name -> detail stage (0/1/2/3).
    // Avoids calling FindVar + GetShaderName on every Stage 0 call for the same material.
    std::unordered_map<std::string, int> m_detailTextureCache;

    // Cache: material name -> set of D3D9 textures (materials can have multiple texture variants)
    std::unordered_map<std::string, std::vector<IDirect3DTexture9*>> m_textureCache;

    // The Source texture identity expected for each accepted D3D pointer.
    // One Source texture may be shared by many VMTs, but the same live D3D
    // pointer must never silently change from one Source texture to another.
    std::unordered_map<IDirect3DTexture9*, std::string> m_textureSourceIdentities;

    // Reverse map: texture hash -> set of material names that use it (Stage 0 only).
    // Populated as new Stage 0 variants are discovered in Hook_SetTexture.
    std::unordered_map<uint64_t, std::unordered_set<std::string>> m_hashToMaterials;

    // Set of lowercase material names that the Lua BSP scan identified as world brushes,
    // regardless of whether they were assigned a Remix category.
    std::unordered_set<std::string> m_bspWorldMaterials;

    // Hashes permanently blocked from DECAL_STATIC because they are shared between
    // BSP world geometry and at least one non-world material (e.g. a model texture).
    // Cleared on map change alongside m_bspWorldMaterials.
    std::unordered_set<uint64_t> m_contestedDecalHashes;
    
    // Hash to category flags mapping
    std::unordered_map<uint64_t, uint32_t> m_hashToCategoryFlags;
    
    // Pending categorizations (textures that returned hash=0)
    std::vector<PendingCategory> m_pendingCategories;

    // BSP world material textures whose hash was 0 at discovery time.
    // Resolved by RetryPendingCategories; on success, updates m_hashToMaterials
    // and removes any stale PARTICLE tag from the resolved hash.
    std::vector<PendingBSPHash> m_pendingBSPHashes;

    // Materials whose Stage 0 texture had hash=0 at first discovery.
    // OnNewMaterialDetected is deferred until Remix provides a valid hash.
    // No extra AddRef: the pointer is already kept alive by m_textureCache.
    std::vector<PendingHashResolution> m_pendingHashResolution;
    
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
    mutable std::recursive_mutex m_mutex;
};

#endif // _WIN64
