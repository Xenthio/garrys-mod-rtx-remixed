#ifdef _WIN64

#include "d3d9_texture_tracker.h"
#include <tier0/dbg.h>
#include <Windows.h>
#include <materialsystem/imaterialsystem.h>
#include <materialsystem/imaterial.h>
#include <materialsystem/imaterialvar.h>
#include <materialsystem/itexture.h>
#include <filesystem.h>
#include <texture_group_names.h>
#include <algorithm>
#include <atomic>
#include <functional>
#include <cctype>
#include <cstring>
#include <remix/remix.h>
#include "material_pipeline/material_pipeline.h"
#include "material_pipeline/material_filter.h"
#include "material_pipeline/auto_categorisation/auto_categorisation.h"

// Global material system pointer (from module.cpp)
extern IMaterialSystem* materials;
extern remix::Interface* g_remix;

// Local filesystem pointer - we'll initialize this ourselves
static IFileSystem* s_pFileSystem = nullptr;

namespace {

// Render state must be thread-local. Source can issue Bind calls on a queued
// main-thread context while IDirect3DDevice9::SetTexture runs on the render
// thread; sharing one "current material" between them lets unrelated draws
// overwrite each other's identity.
struct ThreadMaterialState {
    uint64_t generation = 0;
    std::string materialName;
    IMaterial* material = nullptr;
    bool bindPending = false;
    bool pipelineEligible = false;

    // D3D9 stage at which this material's $detail texture is expected:
    //   0 = none, 1 = Stage 1, 2 = Stage 2, 3 = separate Stage 0/1 overlay.
    int detailStage = 0;
    bool hasBaseTexture2 = false;
    IDirect3DTexture9* stage0Texture = nullptr;
};

thread_local ThreadMaterialState s_threadMaterialState;
std::atomic<uint64_t> s_materialStateGeneration{1};

static ThreadMaterialState& GetThreadMaterialState() {
    ThreadMaterialState& state = s_threadMaterialState;
    const uint64_t generation =
        s_materialStateGeneration.load(std::memory_order_acquire);
    if (state.generation != generation) {
        state = ThreadMaterialState{};
        state.generation = generation;
    }
    return state;
}

static void InvalidateThreadMaterialStates() {
    s_materialStateGeneration.fetch_add(1, std::memory_order_acq_rel);
    // Reset this caller immediately; other threads reset lazily the next time
    // they enter a Bind/SetTexture hook.
    s_threadMaterialState = ThreadMaterialState{};
}

static std::string NormalizeTextureName(const char* name) {
    std::string normalized = name ? name : "";
    std::transform(normalized.begin(), normalized.end(), normalized.begin(),
        [](unsigned char c) {
            return c == '\\' ? '/' : static_cast<char>(std::tolower(c));
        });
    return normalized;
}

static bool IsMaterialPageAlias(
    IMaterial* boundMaterial,
    const std::string& queriedName)
{
    if (!boundMaterial || !boundMaterial->InMaterialPage()) {
        return false;
    }

    IMaterial* materialPage = boundMaterial->GetMaterialPage();
    if (!materialPage) {
        return false;
    }

    return NormalizeTextureName(materialPage->GetName()) == queriedName;
}

static bool GetMaterialTexture(
    IMaterial* material,
    const char* variableName,
    ITexture** outTexture)
{
    if (outTexture) {
        *outTexture = nullptr;
    }
    if (!material || !variableName) {
        return false;
    }

    bool found = false;
    IMaterialVar* variable = material->FindVar(variableName, &found, false);
    if (!found || !variable) {
        return false;
    }

    ITexture* texture = variable->GetTextureValue();
    if (!texture || texture->IsError()) {
        return false;
    }

    // Shaders auto-fill an unset $basetexture with the shared built-in error
    // texture. That pointer is a valid, successfully-loaded resource, so
    // IsError() reports false for it - name comparison is the only signal.
    const char* textureName = texture->GetName();
    if (textureName && _stricmp(textureName, "error") == 0) {
        return false;
    }

    if (outTexture) {
        *outTexture = texture;
    }
    return true;
}

} // namespace

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

D3D9TextureTracker::TextureSnapshot::~TextureSnapshot() {
    Reset();
}

D3D9TextureTracker::TextureSnapshot::TextureSnapshot(TextureSnapshot&& other) noexcept {
    m_textures.swap(other.m_textures);
}

D3D9TextureTracker::TextureSnapshot&
D3D9TextureTracker::TextureSnapshot::operator=(TextureSnapshot&& other) noexcept {
    if (this != &other) {
        Reset();
        m_textures.swap(other.m_textures);
    }
    return *this;
}

void D3D9TextureTracker::TextureSnapshot::Reset() {
    for (auto* texture : m_textures) {
        if (texture) {
            texture->Release();
        }
    }
    m_textures.clear();
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
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        
        // Release all texture references before clearing
        for (auto& entry : m_textureCache) {
            for (auto* tex : entry.second) {
                if (tex) {
                    tex->Release();
                }
            }
        }
        m_textureCache.clear();
        m_textureSourceIdentities.clear();
        
        // Clear pending categorizations
        for (auto& pending : m_pendingCategories) {
            if (pending.texture) {
                pending.texture->Release();
            }
        }
        m_pendingCategories.clear();

        for (auto& pending : m_pendingBSPHashes) {
            if (pending.texture) {
                pending.texture->Release();
            }
        }
        m_pendingBSPHashes.clear();
        m_pendingHashResolution.clear();

        m_detailTextureCache.clear();
        m_hashToMaterials.clear();
        m_hashToCategoryFlags.clear();
        m_bspWorldMaterials.clear();
        m_intrinsicDecalMaterials.clear();
        m_contestedDecalHashes.clear();
        m_worldTextureNames.clear();
        
        InvalidateThreadMaterialStates();
    }
    
    m_pDevice = nullptr;
    m_pRenderContext = nullptr;
    m_pOriginalSetTexture = nullptr;
    m_pOriginalBind = nullptr;
    m_bInitialized = false;

    Msg("[D3D9TextureTracker] Shutdown complete\n");
}

void D3D9TextureTracker::EnsureBindHook() {
    // This is called from Hook_SetTexture which runs on the RENDER THREAD.
    // The initial hook in Initialize() may have hooked the queued context (main thread),
    // but we need to hook the immediate context (render thread) for the hook to fire
    // during actual rendering.
    if (!materials) return;
    
    IMatRenderContext* currentContext = materials->GetRenderContext();
    if (!currentContext) return;
    
    if (currentContext != m_pRenderContext) {
        // Different context than what we hooked! This likely means Initialize() hooked
        // the queued context but rendering uses the immediate context.
        IMatRenderContext* oldContext = m_pRenderContext;
        
        // Restore the old hook if we had one
        if (m_pRenderContext && m_pOriginalBind) {
            VTableHook::HookVTableFunction(m_pRenderContext, 7, m_pOriginalBind);
            m_pOriginalBind = nullptr;
        }
        
        m_pRenderContext = currentContext;
        Bind_t newOriginal = reinterpret_cast<Bind_t>(
            VTableHook::HookVTableFunction(m_pRenderContext, 7, &Hook_Bind)
        );
        
        if (newOriginal) {
            // Only update m_pOriginalBind if we got a valid pointer AND it's not our own hook
            if (newOriginal != &Hook_Bind) {
                m_pOriginalBind = newOriginal;
                Msg("[D3D9TextureTracker] Re-hooked IMatRenderContext::Bind on new context %p (was %p)\n", 
                    currentContext, oldContext);
            } else {
                // Already hooked (same vtable shared between instances)
                // Restore since we don't need a double-hook
                VTableHook::HookVTableFunction(m_pRenderContext, 7, newOriginal);
            }
        } else {
            Warning("[D3D9TextureTracker] Failed to re-hook Bind on new context!\n");
        }
    }
}

void D3D9TextureTracker::SetCurrentMaterial(IMaterial* pMaterial) {
    ThreadMaterialState& state = GetThreadMaterialState();
    state.material = pMaterial;
    state.bindPending = pMaterial != nullptr;
    // Reset per-draw-call state so the previous material's Stage 0 texture
    // doesn't bleed into the overlay-pass detection of the next material.
    state.stage0Texture = nullptr;
    state.detailStage = 0;
    state.hasBaseTexture2 = false;
    state.pipelineEligible = false;
    
    if (pMaterial) {
        const char* name = pMaterial->GetName();
        if (name) {
            state.materialName = NormalizeTextureName(name);
        } else {
            state.materialName.clear();
        }

        ITexture* baseTexture2 = nullptr;
        state.hasBaseTexture2 =
            GetMaterialTexture(pMaterial, "$basetexture2", &baseTexture2);

        const char* textureGroup = pMaterial->GetTextureGroupName();
        const bool isVGUITextureGroup =
            textureGroup && std::strcmp(textureGroup, TEXTURE_GROUP_VGUI) == 0;
        state.pipelineEligible =
            !isVGUITextureGroup &&
            !MaterialPipeline::MaterialFilter::IsNonSceneMaterialName(
                state.materialName);
    } else {
        state.materialName.clear();
    }
}

IDirect3DTexture9* D3D9TextureTracker::GetTextureForMaterial(const char* materialName) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
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

D3D9TextureTracker::TextureSnapshot
D3D9TextureTracker::GetTextureVariantsForMaterial(const char* materialName) {
    TextureSnapshot snapshot;
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (!materialName || !materialName[0]) {
        return snapshot;
    }

    // Normalize lookup key
    std::string lowerName = materialName;
    std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(), 
        [](unsigned char c){ return std::tolower(c); });

    auto it = m_textureCache.find(lowerName);
    if (it != m_textureCache.end()) {
        snapshot.m_textures.reserve(it->second.size());
        for (auto* texture : it->second) {
            if (texture) {
                texture->AddRef();
                snapshot.m_textures.push_back(texture);
            }
        }
    }

    return snapshot;
}

std::vector<std::string> D3D9TextureTracker::GetCachedMaterials() const {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    std::vector<std::string> materialNames;
    materialNames.reserve(m_textureCache.size());
    for (const auto& entry : m_textureCache) {
        materialNames.push_back(entry.first);
    }
    return materialNames;
}

size_t D3D9TextureTracker::GetCacheSize() const {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    return m_textureCache.size();
}

size_t D3D9TextureTracker::GetTextureVariantCount(const char* materialName) const {
    if (!materialName || !materialName[0]) {
        return 0;
    }
    std::string lowerName = materialName;
    std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(), 
        [](unsigned char c){ return std::tolower(c); });
    
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    auto it = m_textureCache.find(lowerName);
    return it != m_textureCache.end() ? it->second.size() : 0;
}

bool D3D9TextureTracker::ValidateTextureAssociation(
    IMaterial* material,
    DWORD stage,
    IDirect3DTexture9* texture,
    const std::string& materialName)
{
    if (!material || !texture || (stage != 0 && stage != 1)) {
        return false;
    }

    // Material-page subrects render from their parent atlas. Validate Stage 0
    // against that atlas while retaining the subrect's material name for
    // categorization (notably decals/concrete/shot*_subrect).
    IMaterial* textureMaterial = material;
    if (stage == 0 && material->InMaterialPage()) {
        IMaterial* materialPage = material->GetMaterialPage();
        if (materialPage) {
            textureMaterial = materialPage;
        }
    }

    const char* variableName = stage == 0 ? "$basetexture" : "$basetexture2";
    ITexture* expectedTexture = nullptr;
    if (!GetMaterialTexture(textureMaterial, variableName, &expectedTexture)) {
        // Envmap-only chrome materials ($envmapsphere overlays) have no
        // $basetexture - their Stage 0 bind is the 2D spheremap itself.
        const bool envmapFallback =
            stage == 0 &&
            GetMaterialTexture(textureMaterial, "$envmap", &expectedTexture);
        if (!envmapFallback) {
            if (m_enableDebugOutput) {
                Msg("[D3D9TextureTracker] Rejected Stage %lu texture 0x%p for '%s': no valid %s\n",
                    stage, texture, materialName.c_str(), variableName);
            }
            return false;
        }
        variableName = "$envmap";
    }

    D3DSURFACE_DESC description = {};
    if (FAILED(texture->GetLevelDesc(0, &description))) {
        if (m_enableDebugOutput) {
            Msg("[D3D9TextureTracker] Rejected Stage %lu texture 0x%p for '%s': GetLevelDesc failed\n",
                stage, texture, materialName.c_str());
        }
        return false;
    }

    const int expectedWidth = expectedTexture->GetActualWidth();
    const int expectedHeight = expectedTexture->GetActualHeight();
    if ((expectedWidth > 0 && description.Width != static_cast<UINT>(expectedWidth)) ||
        (expectedHeight > 0 && description.Height != static_cast<UINT>(expectedHeight))) {
        if (m_enableDebugOutput) {
            Msg("[D3D9TextureTracker] Rejected Stage %lu texture 0x%p for '%s': "
                "%ux%u does not match %s '%s' (%dx%d)\n",
                stage, texture, materialName.c_str(),
                description.Width, description.Height,
                variableName, expectedTexture->GetName(),
                expectedWidth, expectedHeight);
        }
        return false;
    }

    const std::string expectedIdentity =
        NormalizeTextureName(expectedTexture->GetName());
    if (expectedIdentity.empty()) {
        return false;
    }

    auto identityIt = m_textureSourceIdentities.find(texture);
    if (identityIt != m_textureSourceIdentities.end() &&
        identityIt->second != expectedIdentity) {
        if (m_enableDebugOutput) {
            Msg("[D3D9TextureTracker] Rejected texture identity mismatch for 0x%p: "
                "'%s' expects '%s', pointer was already verified as '%s'\n",
                texture, materialName.c_str(), expectedIdentity.c_str(),
                identityIt->second.c_str());
        }
        return false;
    }

    m_textureSourceIdentities[texture] = expectedIdentity;
    return true;
}

size_t D3D9TextureTracker::InvalidateMaterialCache(const char* materialName) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (!materialName || !materialName[0]) {
        return 0;
    }
    
    // Normalize lookup key (same as other parts of this tracker)
    std::string lowerName = materialName;
    std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(), 
        [](unsigned char c){ return std::tolower(c); });
    
    // Remove common prefixes/suffixes to match how names are stored in cache
    if (lowerName.find("materials/") == 0) {
        lowerName = lowerName.substr(10); // Remove "materials/"
    }
    if (lowerName.size() > 4 && lowerName.substr(lowerName.size() - 4) == ".vmt") {
        lowerName = lowerName.substr(0, lowerName.size() - 4); // Remove ".vmt"
    }
    
    size_t totalCount = 0;
    
    // Helper to invalidate a specific cache key (release textures and erase entry)
    auto invalidateKey = [this, &totalCount](const std::string& key) {
        auto it = m_textureCache.find(key);
        if (it == m_textureCache.end()) {
            return;
        }

        totalCount += it->second.size();

        // Release texture references
        for (auto* tex : it->second) {
            if (tex) {
                m_textureSourceIdentities.erase(tex);
                tex->Release();
            }
        }

        // Remove from cache
        m_textureCache.erase(it);
    };

    // Invalidate base material entry
    invalidateKey(lowerName);

    // Also invalidate displacement/stage1 variant, if present
    std::string stage1Name = lowerName + "_stage1";
    invalidateKey(stage1Name);

    if (totalCount == 0) {
        return 0;
    }

    // A runtime material update may retain the same material name while changing
    // its Source texture variables. Force every hook thread to rediscover them.
    InvalidateThreadMaterialStates();
    
    if (m_enableDebugOutput) {
        Msg("[D3D9TextureTracker] Invalidated cache for '%s' and variants (%zu textures)\n", materialName, totalCount);
    }
    
    return totalCount;
}

void D3D9TextureTracker::ClearCache() {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    
    // Release all texture references before clearing
    for (auto& entry : m_textureCache) {
        for (auto* tex : entry.second) {
            if (tex) {
                tex->Release();
            }
        }
    }
    m_textureCache.clear();
    m_textureSourceIdentities.clear();
    m_detailTextureCache.clear();
    m_hashToMaterials.clear();
    m_bspWorldMaterials.clear();
    m_intrinsicDecalMaterials.clear();
    m_contestedDecalHashes.clear();
    InvalidateThreadMaterialStates();
    
    // Also clear pending categorizations
    for (auto& pending : m_pendingCategories) {
        if (pending.texture) {
            pending.texture->Release();
        }
    }
    m_pendingCategories.clear();

    for (auto& pending : m_pendingBSPHashes) {
        if (pending.texture) {
            pending.texture->Release();
        }
    }
    m_pendingBSPHashes.clear();

    // Pending hash-resolution entries share their AddRef with m_textureCache
    // (already released above), so we just clear the list without extra Release.
    m_pendingHashResolution.clear();
    
    Msg("[D3D9TextureTracker] Cache cleared\n");
}

// Hooked SetTexture function
HRESULT STDMETHODCALLTYPE D3D9TextureTracker::Hook_SetTexture(
    IDirect3DDevice9* pDevice,
    DWORD Stage,
    IDirect3DBaseTexture9* pTexture)
{
    D3D9TextureTracker& tracker = Instance();
    ThreadMaterialState& drawState = GetThreadMaterialState();
    bool materialIdentityConflict = false;
    uint64_t hashToReconcile = 0;

    // Cross-check the Bind hook against GetCurrentMaterial on Stage 0. Most
    // disagreements mean the queued and immediate material contexts are out of
    // phase, so rejecting the observation is safer than permanently assigning
    // the texture to either name.
    //
    // Material-page subrects are the intentional exception. Source binds the
    // logical subrect material (for example, a bullet-hole material) while
    // GetCurrentMaterial returns its shared atlas page. Keep the more specific
    // bound identity when Source confirms that exact parent relationship.
    if (Stage == 0 && pTexture && materials) {
        IMatRenderContext* ctx = materials->GetRenderContext();
        if (ctx) {
            IMaterial* pCurMat = ctx->GetCurrentMaterial();
            if (pCurMat) {
                const char* name = pCurMat->GetName();
                if (name && name[0]) {
                    const std::string queriedName = NormalizeTextureName(name);
                    const bool namesDisagree =
                        !drawState.materialName.empty() &&
                        drawState.materialName != queriedName;
                    const bool materialPageAlias =
                        drawState.bindPending &&
                        namesDisagree &&
                        IsMaterialPageAlias(drawState.material, queriedName);

                    if (drawState.bindPending &&
                        namesDisagree &&
                        !materialPageAlias) {
                        materialIdentityConflict = true;
                        if (tracker.m_enableDebugOutput) {
                            Msg("[D3D9TextureTracker] Rejected Stage 0 identity disagreement: "
                                "Bind='%s', GetCurrentMaterial='%s', texture=0x%p\n",
                                drawState.materialName.c_str(), queriedName.c_str(), pTexture);
                        }
                    }
                    if (namesDisagree && !materialPageAlias) {
                        tracker.SetCurrentMaterial(pCurMat);
                    } else if (!namesDisagree) {
                        // GetCurrentMaterial is queried for every Stage 0 pass.
                        // Do not reset the per-draw detail state when it merely
                        // confirms the material already supplied by Hook_Bind.
                        drawState.material = pCurMat;
                    }
                }
            }
        }
        drawState.bindPending = false;
    }

    {
        std::lock_guard<std::recursive_mutex> lock(tracker.m_mutex);

        // Always log if we have a current material to help debug
#ifdef _DEBUG
        if (Stage == 0 && pTexture && !drawState.materialName.empty()) {
            // Msg("[D3D9TextureTracker] SetTexture(0, %p) for '%s'\n", pTexture, drawState.materialName.c_str());
        }
#endif

        // DEBUG: Log all texture stages for displacement materials
        if (pTexture && !drawState.materialName.empty()) {
            std::string lowerName = drawState.materialName;
            std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(), 
                [](unsigned char c){ return std::tolower(c); });
            
            if (lowerName.find("blend_") != std::string::npos || 
                lowerName.find("_wvt_patch") != std::string::npos) {
                if (tracker.m_enableDebugOutput) {
                    static int dispSetTexCount = 0;
                    dispSetTexCount++;
                    if (dispSetTexCount <= 20) {
                        Msg("[D3D9TextureTracker] SetTexture(Stage=%d, 0x%p) for displacement '%s'\n", 
                            Stage, pTexture, drawState.materialName.c_str());
                    }
                }
            }
        }

        // Stage 1 is only a material texture when the VMT actually declares
        // $basetexture2. For ordinary materials it is a lightmap, bump/detail
        // texture, or another shader input and must never inherit Stage 0's category.
        const bool shouldTrackMaterialTexture =
            drawState.pipelineEligible &&
            (Stage == 0 || (Stage == 1 && drawState.hasBaseTexture2));

        // Remember every Stage 0 pointer, including rejected auxiliary passes.
        // A following Stage 1 bind of the same pointer is the signal used to
        // identify LightmappedGeneric's separate $detail overlay pass.
        if (drawState.pipelineEligible && Stage == 0 && pTexture &&
            pTexture->GetType() == D3DRTYPE_TEXTURE) {
            drawState.stage0Texture =
                static_cast<IDirect3DTexture9*>(pTexture);
        }

        if (shouldTrackMaterialTexture && !materialIdentityConflict && pTexture) {
            // Check if this is a 2D texture (not cube/volume)
            D3DRESOURCETYPE resType = pTexture->GetType();
            if (resType == D3DRTYPE_TEXTURE) {
                IDirect3DTexture9* p2DTexture = static_cast<IDirect3DTexture9*>(pTexture);
                
                // If we have a current material name, use it
                if (!drawState.materialName.empty() &&
                    tracker.ValidateTextureAssociation(
                        drawState.material, Stage, p2DTexture, drawState.materialName)) {
                    // For Stage 1 textures, append "_stage1" to the material name to track separately
                    std::string trackingName = drawState.materialName;
                    if (Stage == 1) {
                        trackingName += "_stage1";
                    }

                    // At Stage 0: determine which D3D9 stage the $detail texture will be
                    // bound at for this material (0 = no detail).  The result is cached so
                    // FindVar / GetShaderName are only called once per unique material name.
                    //
                    // detailStage values:
                    //   0 = no $detail
                    //   1 = VertexLitGeneric/UnlitGeneric: detail at Stage 1
                    //   2 = LightmappedGeneric (no bumpmap): detail at Stage 2
                    //   3 = LightmappedGeneric + $bumpmap: detail is rendered in a separate
                    //       overlay pass where the engine calls SetTexture(0/1, detail).
                    //       The normal Stage 2 path is not used in this case.
                    if (Stage == 0) {
                        auto cacheIt = tracker.m_detailTextureCache.find(drawState.materialName);
                        if (cacheIt != tracker.m_detailTextureCache.end()) {
                            drawState.detailStage = cacheIt->second;
                        } else {
                            int detailStage = 0;
                            if (drawState.material) {
                                bool found = false;
                                IMaterialVar* pDetailVar = drawState.material->FindVar("$detail", &found, false);
                                // GetStringValue() returns "" for resolved texture vars; use
                                // GetTextureValue() which is reliable for both loaded and
                                // not-yet-loaded textures.
                                bool hasDetail = found && pDetailVar &&
                                    pDetailVar->GetTextureValue() &&
                                    !pDetailVar->GetTextureValue()->IsError();
                                if (hasDetail) {
                                    const char* shaderName = drawState.material->GetShaderName();
                                    if (shaderName) {
                                        std::string lowerShader = shaderName;
                                        std::transform(lowerShader.begin(), lowerShader.end(),
                                            lowerShader.begin(), [](unsigned char c){ return std::tolower(c); });
                                        if (lowerShader.find("vertexlitgeneric") == 0 ||
                                            lowerShader.find("unlitgeneric") == 0) {
                                            // VertexLitGeneric / UnlitGeneric: no lightmap, detail at Stage 1.
                                            detailStage = 1;
                                        } else {
                                            // LightmappedGeneric (and similar): check whether the
                                            // shader occupies Stage 2 for something other than $detail.
                                            //
                                            // $bumpmap → normal map at Stage 2; detail in overlay pass.
                                            // $basetexture2 → second base at Stage 1, lightmap shifts
                                            //   to Stage 2; detail also rendered in overlay pass.
                                            //
                                            // In both cases set detailStage=3 so we detect the detail
                                            // via the Stage 0/Stage 1 co-binding heuristic.
                                            bool foundBump = false;
                                            IMaterialVar* pBumpVar = drawState.material->FindVar("$bumpmap", &foundBump, false);
                                            bool hasBump = foundBump && pBumpVar &&
                                                pBumpVar->GetTextureValue() &&
                                                !pBumpVar->GetTextureValue()->IsError();

                                            detailStage =
                                                (hasBump || drawState.hasBaseTexture2) ? 3 : 2;
                                        }
                                    } else {
                                        detailStage = 2; // safe default
                                    }
                                }
                            }
                            tracker.m_detailTextureCache[drawState.materialName] = detailStage;
                            drawState.detailStage = detailStage;
                        }
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
                        if (tracker.m_enableDebugOutput) {
                            Msg("[D3D9TextureTracker] NEW texture variant #%zu: 0x%p for '%s'%s (hash: 0x%llX)\n", 
                                textures.size(), p2DTexture, drawState.materialName.c_str(),
                                Stage == 1 ? " [STAGE1]" : "", hash);
                        }

                        // Record intrinsic decal identity before hash resolution.
                        // This preserves $decal/shader-based materials whose first
                        // Remix hash is zero and gets resolved asynchronously.
                        std::string lowerMatName;
                        bool isIntrinsicDecal = false;
                        if (Stage == 0) {
                            lowerMatName = drawState.materialName;
                            std::transform(
                                lowerMatName.begin(), lowerMatName.end(),
                                lowerMatName.begin(),
                                [](unsigned char c){ return std::tolower(c); });
                            isIntrinsicDecal =
                                MaterialPipeline::AutoCategorisation::IsIntrinsicDecalMaterial(
                                    drawState.materialName, drawState.material);
                            if (isIntrinsicDecal) {
                                tracker.m_intrinsicDecalMaterials.insert(lowerMatName);
                            }
                        }
                            
                        // Reverse map: record hash → materialName for collision detection.
                        // Also handle stale category tags caused by hash collisions between
                        // BSP world materials and non-world materials (models, etc.).
                        if (Stage == 0 && hash != 0) {
                            tracker.m_hashToMaterials[hash].insert(drawState.materialName);
                            hashToReconcile = hash;

                            using namespace MaterialPipeline::AutoCategorisation::CategoryFlags;

                            // Query BOTH flag maps.  PARTICLE/DECAL_STATIC applied via
                            // AutoCategorisation::ApplyToHash live in its own s_hashToCategoryFlags
                            // and are NOT mirrored into the tracker's m_hashToCategoryFlags.
                            // Checking only the tracker map means we silently miss those tags.
                            uint32_t existingFlags = 0;
                            tracker.GetHashCategoryFlags(hash, &existingFlags);
                            uint32_t acFlags = 0;
                            MaterialPipeline::AutoCategorisation::GetHashCategoryFlags(hash, &acFlags);
                            uint32_t allExistingFlags = existingFlags | acFlags;

                            if (tracker.m_bspWorldMaterials.count(lowerMatName)) {
                                // BSP world material: remove any stale PARTICLE tag that was
                                // applied before the BSP scan identified this as world geometry.
                                if (allExistingFlags & PARTICLE) {
                                    char hashStr[32];
                                    sprintf_s(hashStr, "0x%llX", hash);
                                    g_remix->RemoveTextureHash("rtx.particleTextures", hashStr);
                                    tracker.SetHashCategoryFlags(hash, existingFlags & ~PARTICLE);
                                    MaterialPipeline::AutoCategorisation::SetHashCategoryFlags(
                                        hash, acFlags & ~PARTICLE);
                                    if (tracker.m_enableDebugOutput) {
                                        Msg("[D3D9TextureTracker] Removed stale PARTICLE for BSP world material '%s' (hash %s)\n",
                                            drawState.materialName.c_str(), hashStr);
                                    }
                                }
                            }

                            // Separate from the PARTICLE check: if this material is neither an
                            // intrinsic decal nor in the world-texture DECAL_STATIC list, and the
                            // hash already carries DECAL_STATIC, mark it permanently contested.
                            // Multiple intrinsic decal materials are allowed to share an atlas.
                            if (!isIntrinsicDecal &&
                                !tracker.IsWorldTexture(drawState.materialName)) {
                                if (allExistingFlags & DECAL_STATIC) {
                                    char hashStr[32];
                                    sprintf_s(hashStr, "0x%llX", hash);
                                    tracker.m_contestedDecalHashes.insert(hash);
                                    g_remix->RemoveTextureHash("rtx.decalTextures", hashStr);
                                    tracker.SetHashCategoryFlags(hash, existingFlags & ~DECAL_STATIC);
                                    MaterialPipeline::AutoCategorisation::SetHashCategoryFlags(
                                        hash, acFlags & ~DECAL_STATIC);
                                    if (tracker.m_enableDebugOutput) {
                                        Msg("[D3D9TextureTracker] Hash %s contested: removed DECAL_STATIC for non-decal material '%s'\n",
                                            hashStr, drawState.materialName.c_str());
                                    }
                                }
                            }
                        }

                        // When a Stage 0 BSP world material variant has hash=0 (RTX Remix hasn't
                        // processed the texture yet), queue it for deferred hash resolution.
                        // BSP world materials with no detectable category (e.g. translucent brushes)
                        // are never added to the normal pending-categories queue, so this is the
                        // only mechanism that lets us retroactively remove a stale PARTICLE tag
                        // once the hash is resolved and we discover the collision.
                        if (Stage == 0 && hash == 0) {
                            std::string lowerForBSP = drawState.materialName;
                            std::transform(lowerForBSP.begin(), lowerForBSP.end(),
                                lowerForBSP.begin(), [](unsigned char c){ return std::tolower(c); });
                            if (tracker.m_bspWorldMaterials.count(lowerForBSP)) {
                                p2DTexture->AddRef();
                                tracker.m_pendingBSPHashes.push_back({p2DTexture, drawState.materialName});
                            }
                        }

                        // Notify MaterialPipeline of new material for unified processing
                        // Only for Stage 0 to avoid double-processing
                        // The pipeline handles: ShaderFixes → HashCollisionFixer → AutoCategorisation → ToPBR
                        if (Stage == 0) {
                            if (hash != 0) {
                                // Hash is ready — notify the pipeline immediately.
                                MaterialPipeline::Pipeline::OnNewMaterialDetected(
                                    drawState.materialName, hash, p2DTexture);

                                // For animated textures: apply any already-detected category flags
                                // to new variants as they appear (no need for re-detection).
                                if (textures.size() > 1) {
                                    MaterialPipeline::AutoCategorisation::ApplyKnownCategoryToTexture(
                                        drawState.materialName, p2DTexture);
                                }
                            } else {
                                // Remix hasn't processed this texture yet — defer pipeline
                                // notification until RetryPendingHashResolution resolves
                                // a valid non-zero hash.  The pointer is kept alive by
                                // m_textureCache; no extra AddRef is needed here.
                                tracker.m_pendingHashResolution.push_back(
                                    {p2DTexture, drawState.materialName});

                                // For animated textures: if this material was already
                                // categorized via a prior frame, fast-apply the stored
                                // flags to this variant through the pending-category
                                // queue so it resolves independently of the pipeline.
                                // This mirrors what the hash!=0 path does synchronously
                                // via ApplyKnownCategoryToTexture when textures.size()>1.
                                if (textures.size() > 1) {
                                    MaterialPipeline::AutoCategorisation::ApplyKnownCategoryToTexture(
                                        drawState.materialName, p2DTexture);
                                }
                            }
                        }

                        // VertexLitGeneric + $detail: Stage 1 is the detail texture.
                        // Apply IGNORED so RTX Remix skips it.
                        if (Stage == 1 && drawState.detailStage == 1 && g_remix) {
                            using namespace MaterialPipeline::AutoCategorisation::CategoryFlags;
                            if (hash != 0) {
                                uint32_t existingFlags = 0;
                                tracker.GetHashCategoryFlags(hash, &existingFlags);
                                // Apply whenever not already IGNORED — ApplyCategoryToHash will
                                // also remove from rtx.decalTextures if DECAL_STATIC was set.
                                if (!(existingFlags & IGNORED)) {
                                    if (tracker.m_enableDebugOutput) {
                                        Msg("[D3D9TextureTracker] Ignoring detail texture (Stage1/VLG) for '%s' (hash 0x%llX)\n",
                                            drawState.materialName.c_str(), hash);
                                    }
                                    tracker.ApplyCategoryToHash(hash, IGNORED, trackingName.c_str());
                                }
                            } else {
                                // Hash not ready — queue for retry with its own ref
                                bool alreadyPending = false;
                                for (const auto& pc : tracker.m_pendingCategories) {
                                    if (pc.texture == p2DTexture && pc.categoryFlags == IGNORED) {
                                        alreadyPending = true;
                                        break;
                                    }
                                }
                                if (!alreadyPending) {
                                    p2DTexture->AddRef();
                                    PendingCategory pc;
                                    pc.texture = p2DTexture;
                                    pc.materialName = trackingName + "_detail";
                                    pc.categoryFlags = IGNORED;
                                    tracker.m_pendingCategories.push_back(pc);
                                }
                            }
                        }
                    }
                } else {
                    // DEBUG: Log textures that are set without a material name
                    if (drawState.materialName.empty() &&
                        tracker.m_enableDebugOutput) {
                        // DEBUG: Log unmatched textures occasionally
                        static int unmatchedCount = 0;
                        unmatchedCount++;
                        if (unmatchedCount % 1000 == 0) {
                            Msg("[D3D9TextureTracker] WARNING: %d textures set without material name (0x%p)\n", 
                                unmatchedCount, p2DTexture);
                        }
                        
                        // Try to get hash for debugging
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

        // VertexLitGeneric / UnlitGeneric + $detail uses Stage 1 for the detail
        // sampler. It is intentionally handled outside the material-variant cache:
        // Stage 1 is not a second base texture and must only receive IGNORED.
        if (Stage == 1 && drawState.detailStage == 1 && pTexture && g_remix &&
            pTexture->GetType() == D3DRTYPE_TEXTURE) {
            using namespace MaterialPipeline::AutoCategorisation::CategoryFlags;
            IDirect3DTexture9* detailTexture =
                static_cast<IDirect3DTexture9*>(pTexture);
            auto result = g_remix->dxvk_GetTextureHash(detailTexture);
            if (result && result.value() != 0) {
                const uint64_t hash = result.value();
                uint32_t existingFlags = 0;
                tracker.GetHashCategoryFlags(hash, &existingFlags);
                if (!(existingFlags & IGNORED)) {
                    if (tracker.m_enableDebugOutput) {
                        Msg("[D3D9TextureTracker] Ignoring Stage 1 detail for '%s' "
                            "(hash 0x%llX)\n",
                            drawState.materialName.c_str(), hash);
                    }
                    tracker.ApplyCategoryToHash(
                        hash, IGNORED, drawState.materialName.c_str());
                }
            } else if (result) {
                bool alreadyPending = false;
                for (const auto& pending : tracker.m_pendingCategories) {
                    if (pending.texture == detailTexture &&
                        pending.categoryFlags == IGNORED) {
                        alreadyPending = true;
                        break;
                    }
                }
                if (!alreadyPending) {
                    detailTexture->AddRef();
                    tracker.m_pendingCategories.push_back({
                        detailTexture,
                        drawState.materialName + "_detail",
                        IGNORED
                    });
                }
            }
        }

        // LightmappedGeneric + $bumpmap + $detail (detailStage==3):
        // The engine renders $detail in a separate overlay pass using two consecutive
        // SetTexture calls:  SetTexture(0, detail)  then  SetTexture(1, detail).
        // The reliable signal is that Stage 1 receives the SAME D3D9 pointer as Stage 0.
        // We check this every frame (not just for new variants) so that IGNORED persists
        // even after AutoCat's pending-retry may have re-added the hash to rtx.decalTextures.
        if (Stage == 1 && drawState.detailStage == 3 &&
            pTexture && drawState.stage0Texture && g_remix) {
            D3DRESOURCETYPE resType = pTexture->GetType();
            if (resType == D3DRTYPE_TEXTURE) {
                IDirect3DTexture9* p2DTexture = static_cast<IDirect3DTexture9*>(pTexture);
                if (p2DTexture == drawState.stage0Texture) {
                    using namespace MaterialPipeline::AutoCategorisation::CategoryFlags;
                    auto result = g_remix->dxvk_GetTextureHash(p2DTexture);
                    if (result && result.value() != 0) {
                        uint64_t hash = result.value();
                        uint32_t existingFlags = 0;
                        tracker.GetHashCategoryFlags(hash, &existingFlags);
                        if (!(existingFlags & IGNORED)) {
                            if (tracker.m_enableDebugOutput) {
                                Msg("[D3D9TextureTracker] Ignoring detail overlay texture (Stage1/LMG+bump) for '%s' (hash 0x%llX)\n",
                                    drawState.materialName.c_str(), hash);
                            }
                            tracker.ApplyCategoryToHash(hash, IGNORED, drawState.materialName.c_str());
                        }
                    } else if (result) {
                        // Hash not ready — queue for retry
                        bool alreadyPending = false;
                        for (const auto& pc : tracker.m_pendingCategories) {
                            if (pc.texture == p2DTexture && pc.categoryFlags == IGNORED) {
                                alreadyPending = true;
                                break;
                            }
                        }
                        if (!alreadyPending) {
                            p2DTexture->AddRef();
                            PendingCategory pc;
                            pc.texture = p2DTexture;
                            pc.materialName = drawState.materialName + "_detail_overlay";
                            pc.categoryFlags = IGNORED;
                            tracker.m_pendingCategories.push_back(pc);
                        }
                    }
                }
            }
        }

        // Stage 2: detail texture for LightmappedGeneric — mark as IGNORED so RTX Remix skips it.
        // LightmappedGeneric binds: Stage 0 = base, Stage 1 = lightmap, Stage 2 = $detail.
        // We only fire when Stage 0 determined the detail is at Stage 2 (LightmappedGeneric path).
        if (Stage == 2 && pTexture && drawState.detailStage == 2 && g_remix) {
            D3DRESOURCETYPE resType = pTexture->GetType();
            if (resType == D3DRTYPE_TEXTURE) {
                IDirect3DTexture9* p2DTexture = static_cast<IDirect3DTexture9*>(pTexture);
                using namespace MaterialPipeline::AutoCategorisation::CategoryFlags;

                auto result = g_remix->dxvk_GetTextureHash(p2DTexture);
                if (result && result.value() != 0) {
                    uint64_t hash = result.value();
                    uint32_t existingFlags = 0;
                    tracker.GetHashCategoryFlags(hash, &existingFlags);
                    // Apply whenever not already IGNORED — ApplyCategoryToHash will
                    // also remove from rtx.decalTextures if DECAL_STATIC was set.
                    if (!(existingFlags & IGNORED)) {
                        if (tracker.m_enableDebugOutput) {
                            Msg("[D3D9TextureTracker] Ignoring detail texture for '%s' (hash 0x%llX)\n",
                                drawState.materialName.c_str(), hash);
                        }
                        tracker.ApplyCategoryToHash(hash, IGNORED, drawState.materialName.c_str());
                    }
                } else if (result) {
                    // Hash not ready yet — queue for retry
                    bool alreadyPending = false;
                    for (const auto& pending : tracker.m_pendingCategories) {
                        if (pending.texture == p2DTexture) { alreadyPending = true; break; }
                    }
                    if (!alreadyPending) {
                        p2DTexture->AddRef();
                        PendingCategory pc;
                        pc.texture = p2DTexture;
                        pc.materialName = drawState.materialName + "_detail";
                        pc.categoryFlags = IGNORED;
                        tracker.m_pendingCategories.push_back(pc);
                    }
                }
            }
        }
    }

    if (hashToReconcile != 0) {
        MaterialPipeline::AutoCategorisation::ReconcileHashCategories(
            hashToReconcile);
    }

    // Periodically check if the render context changed and re-hook Bind if needed.
    // This fixes the case where the initial vtable hook was applied to a different
    // IMatRenderContext instance than the one used during actual rendering.
    static int setTextureCallCount = 0;
    setTextureCallCount++;
    if (setTextureCallCount % 500 == 1) {
        tracker.EnsureBindHook();
    }

    // Periodically retry pending categorizations
    // We do this here because SetTexture is called frequently during rendering
    // This ensures categories are applied when texture hashes become available
    // NOTE: Reduced from 500 to 100 calls for more aggressive retry, especially
    // for particle effects that may have many texture variants with delayed hashing
    if (setTextureCallCount % 100 == 0) {
        // Resolve textures whose hash was 0 at first discovery — must run before
        // the pipeline retry so that the correct hash reaches DetectAndApply.
        if (!tracker.m_pendingHashResolution.empty()) {
            tracker.RetryPendingHashResolution();
        }

        // Retry AutoCategorisation pending queue (from pipeline processing)
        MaterialPipeline::AutoCategorisation::RetryPendingCategories();
        
        // Also retry tracker's own pending queue (legacy path)
        if (!tracker.m_pendingCategories.empty()) {
            tracker.RetryPendingCategories();
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
                if (tracker.m_enableDebugOutput) {
                    static int dispBindCount = 0;
                    dispBindCount++;
                    if (dispBindCount <= 10) {
                        Msg("[D3D9TextureTracker] Bind() called for displacement: '%s'\n", name);
                    }
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
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    // Merge rather than overwrite: category producers are independent (the
    // world smart-mark must not erase a water/particle bit detected by
    // Stage 3). RemoveHashCategoryFlags exists for explicit clearing.
    auto it = m_hashToCategoryFlags.find(textureHash);
    if (it != m_hashToCategoryFlags.end()) {
        it->second |= categoryFlags;
    } else {
        m_hashToCategoryFlags[textureHash] = categoryFlags;
    }
    if (m_enableDebugOutput) {
        Msg("[D3D9TextureTracker] Set category flags 0x%X for hash 0x%llX\n", categoryFlags, textureHash);
    }
}

void D3D9TextureTracker::RemoveHashCategoryFlags(uint64_t textureHash) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    auto it = m_hashToCategoryFlags.find(textureHash);
    if (it != m_hashToCategoryFlags.end()) {
        m_hashToCategoryFlags.erase(it);
        if (m_enableDebugOutput) {
            Msg("[D3D9TextureTracker] Removed category mapping for hash 0x%llX\n", textureHash);
        }
    }
}

void D3D9TextureTracker::ClearHashCategoryMappings() {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    size_t count = m_hashToCategoryFlags.size();
    m_hashToCategoryFlags.clear();
    if (m_enableDebugOutput) {
        Msg("[D3D9TextureTracker] Cleared %zu hash-to-category mappings\n", count);
    }
}

bool D3D9TextureTracker::GetHashCategoryFlags(uint64_t textureHash, uint32_t* outCategoryFlags) const {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
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
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
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

// NOTE: CheckAndApplyCategories was removed - all categorisation now goes through
// MaterialPipeline::AutoCategorisation::DetectAndApply() for unified logic.

// Helper function to apply category flags to a texture hash
void D3D9TextureTracker::ApplyCategoryToHash(uint64_t hash, uint32_t categoryFlags, const char* materialName) {
    if (!g_remix || hash == 0 || categoryFlags == 0) return;
    
    // Category flag constants - use values from AutoCategorisation for consistency
    using namespace MaterialPipeline::AutoCategorisation::CategoryFlags;
    
    // Strip WORLD_UI flag if present - it should never be applied
    if (categoryFlags & WORLD_UI) {
        Msg("[D3D9] WARNING: Removing WORLD_UI flag from '%s' (flags: 0x%X)\n", materialName, categoryFlags);
        categoryFlags &= ~WORLD_UI;  // Remove the WORLD_UI bit
    }
    
    char hashStr[32];
    sprintf_s(hashStr, "0x%llX", hash);
    
    // Water is never a decal: the world smart-mark applies DECAL_STATIC to
    // every world face including water planes, and decal handling breaks the
    // translucent water surface. ANIMATED_WATER wins regardless of which
    // flag arrives first.
    {
        uint32_t trackedFlags = 0;
        GetHashCategoryFlags(hash, &trackedFlags);
        if ((categoryFlags | trackedFlags) & ANIMATED_WATER) {
            if (categoryFlags & DECAL_STATIC) {
                categoryFlags &= ~DECAL_STATIC;
                if (m_enableDebugOutput) {
                    Msg("[D3D9] Skipping DECAL_STATIC for water hash %s ('%s')\n", hashStr, materialName);
                }
            }
            if (categoryFlags & ANIMATED_WATER) {
                g_remix->RemoveTextureHash("rtx.decalTextures", hashStr);
            }
        }
    }
    
    // Map category flags to Remix API texture lists
    // NOTE: WORLD_UI is intentionally omitted - we don't use it
    if (categoryFlags & SKY) {
        g_remix->AddTextureHash("rtx.skyBoxTextures", hashStr);
        if (m_enableDebugOutput) {
            Msg("[D3D9] Categorized SKY: '%s' -> %s\n", materialName, hashStr);
        }
    }
    if (categoryFlags & IGNORED) {
        g_remix->AddTextureHash("rtx.ignoreTextures", hashStr);
        // IGNORED takes precedence over DECAL: a detail texture that was already
        // recorded as world geometry must be removed from the decal list so RTX
        // Remix doesn't try to use it as a decal surface.  This happens because
        // Source Engine re-binds the detail texture at Stage 0 in a separate detail
        // overlay pass, causing it to be collected by the world-geometry scan.
        g_remix->RemoveTextureHash("rtx.decalTextures", hashStr);
        if (m_enableDebugOutput) {
            Msg("[D3D9] Categorized IGNORE: '%s' -> %s\n", materialName, hashStr);
        }
    }
    if (categoryFlags & PARTICLE) {
        // Guard: never tag a hash as particle if a BSP world material shares it.
        // This protects translucent/excluded world brushes (registered via
        // RegisterBSPWorldMaterial) when PARTICLE reaches this path through
        // RetryPendingCategories, bypassing the AutoCategorisation::ApplyToHash guard.
        if (IsAnyBSPWorldMaterialForHash(hash)) {
            categoryFlags &= ~PARTICLE;
            if (m_enableDebugOutput) {
                Msg("[D3D9] Skipping PARTICLE for hash %s ('%s'): shares hash with BSP world material\n",
                    hashStr, materialName);
            }
        } else {
            g_remix->AddTextureHash("rtx.particleTextures", hashStr);
            if (m_enableDebugOutput) {
                Msg("[D3D9] Categorized PARTICLE: '%s' -> %s\n", materialName, hashStr);
            }
        }
    }
    if (categoryFlags & DECAL_STATIC) {
        if (m_contestedDecalHashes.count(hash)) {
            // Hash is shared with a non-world material; suppress DECAL_STATIC permanently.
            categoryFlags &= ~DECAL_STATIC;
            if (m_enableDebugOutput) {
                Msg("[D3D9] Skipping DECAL_STATIC for contested hash %s ('%s')\n", hashStr, materialName);
            }
        } else {
            g_remix->AddTextureHash("rtx.decalTextures", hashStr);
            if (m_enableDebugOutput) {
                Msg("[D3D9] Categorized DECAL: '%s' -> %s\n", materialName, hashStr);
            }
        }
    }
    if (categoryFlags & ANIMATED_WATER) {
        g_remix->AddTextureHash("rtx.animatedWaterTextures", hashStr);
        if (m_enableDebugOutput) {
            Msg("[D3D9] Categorized WATER: '%s' -> %s\n", materialName, hashStr);
        }
    }
    if (categoryFlags & EMISSIVE) {
        g_remix->AddTextureHash("rtx.legacyEmissiveTextures", hashStr);
        // Model/prop textures commonly resolve their hash late (through this
        // deferred pending-category path), so force-albedo must be applied
        // here too - not just in AutoCategorisation::ApplyToHash's immediate
        // path - or unlit materials without a real alpha mask stay dark.
        MaterialPipeline::AutoCategorisation::ApplyForceAlbedoIfNeeded(hash, materialName);
        if (m_enableDebugOutput) {
            Msg("[D3D9] Categorized EMISSIVE: '%s' -> %s\n", materialName, hashStr);
        }
    }
    
    // Update local tracking.
    // When IGNORED is applied it wins over DECAL: clear any stale DECAL flags so
    // GetHashCategory accurately reflects that this texture is being ignored.
    // ANIMATED_WATER likewise clears DECAL_STATIC (water is never a decal).
    uint32_t currentFlags = 0;
    GetHashCategoryFlags(hash, &currentFlags);
    uint32_t newFlags = currentFlags | categoryFlags;
    if (newFlags & IGNORED) {
        newFlags &= ~(DECAL_STATIC | DECAL_DYNAMIC);
    }
    if (newFlags & ANIMATED_WATER) {
        newFlags &= ~DECAL_STATIC;
    }
    SetHashCategoryFlags(hash, newFlags);
}

int D3D9TextureTracker::RetryPendingHashResolution() {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (!g_remix || m_pendingHashResolution.empty()) return 0;

    using namespace MaterialPipeline::AutoCategorisation::CategoryFlags;

    std::vector<PendingHashResolution> stillPending;
    int resolvedCount = 0;

    for (auto& pending : m_pendingHashResolution) {
        auto result = g_remix->dxvk_GetTextureHash(pending.texture);
        if (!result || result.value() == 0) {
            stillPending.push_back(pending);
            continue;
        }

        uint64_t hash = result.value();

        // Insert into the hash→materials reverse map.
        m_hashToMaterials[hash].insert(pending.materialName);
        MaterialPipeline::AutoCategorisation::ReconcileHashCategories(hash);

        // Run the same collision-detection logic that the !found path applies
        // for immediately-available hashes.
        std::string lowerMatName = pending.materialName;
        std::transform(lowerMatName.begin(), lowerMatName.end(),
            lowerMatName.begin(), [](unsigned char c){ return std::tolower(c); });
        const bool isIntrinsicDecal =
            m_intrinsicDecalMaterials.count(lowerMatName) != 0 ||
            MaterialPipeline::AutoCategorisation::IsIntrinsicDecalMaterial(
                pending.materialName, nullptr);
        if (isIntrinsicDecal) {
            m_intrinsicDecalMaterials.insert(lowerMatName);
        }

        uint32_t existingFlags = 0;
        GetHashCategoryFlags(hash, &existingFlags);
        uint32_t acFlags = 0;
        MaterialPipeline::AutoCategorisation::GetHashCategoryFlags(hash, &acFlags);
        uint32_t allExistingFlags = existingFlags | acFlags;

        if (m_bspWorldMaterials.count(lowerMatName)) {
            if (allExistingFlags & PARTICLE) {
                char hashStr[32];
                sprintf_s(hashStr, "0x%llX", hash);
                g_remix->RemoveTextureHash("rtx.particleTextures", hashStr);
                SetHashCategoryFlags(hash, existingFlags & ~PARTICLE);
                MaterialPipeline::AutoCategorisation::SetHashCategoryFlags(
                    hash, acFlags & ~PARTICLE);
                if (m_enableDebugOutput) {
                    Msg("[D3D9TextureTracker] RetryHashResolution: Removed stale PARTICLE for BSP material '%s' (hash %s)\n",
                        pending.materialName.c_str(), hashStr);
                }
            }
        }

        if (!isIntrinsicDecal && !IsWorldTexture(pending.materialName)) {
            if (allExistingFlags & DECAL_STATIC) {
                char hashStr[32];
                sprintf_s(hashStr, "0x%llX", hash);
                m_contestedDecalHashes.insert(hash);
                g_remix->RemoveTextureHash("rtx.decalTextures", hashStr);
                SetHashCategoryFlags(hash, existingFlags & ~DECAL_STATIC);
                MaterialPipeline::AutoCategorisation::SetHashCategoryFlags(
                    hash, acFlags & ~DECAL_STATIC);
                if (m_enableDebugOutput) {
                    Msg("[D3D9TextureTracker] RetryHashResolution: Hash %s contested, removed DECAL_STATIC for '%s'\n",
                        hashStr, pending.materialName.c_str());
                }
            }
        }

        // If this material was already categorized via another variant (e.g. an
        // animated frame that had a non-zero hash earlier), apply the stored flags
        // to this frame's hash immediately so we don't have to wait for the async
        // pipeline.  This mirrors the ApplyKnownCategoryToTexture call that the
        // normal hash!=0 path makes when textures.size()>1.
        MaterialPipeline::AutoCategorisation::ApplyKnownCategoryToTexture(
            pending.materialName, pending.texture);

        // Notify the pipeline for full detection / any categories not yet stored.
        MaterialPipeline::Pipeline::OnNewMaterialDetected(pending.materialName, hash, pending.texture);

        if (m_enableDebugOutput) {
            Msg("[D3D9TextureTracker] RetryHashResolution: Resolved hash 0x%llX for '%s'\n",
                hash, pending.materialName.c_str());
        }
        resolvedCount++;
    }

    m_pendingHashResolution = std::move(stillPending);

    if (resolvedCount > 0 && m_enableDebugOutput) {
        Msg("[D3D9TextureTracker] RetryPendingHashResolution: %d resolved, %zu still pending\n",
            resolvedCount, m_pendingHashResolution.size());
    }

    return resolvedCount;
}

int D3D9TextureTracker::RetryPendingCategories() {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (!g_remix) return 0;
    
    if (m_pendingCategories.empty()) return 0;
    
    // Check if auto-categorization is enabled
    if (!m_enableAutoCategorization) {
        // Clear pending queue and release references since categorization is disabled
        for (auto& pending : m_pendingCategories) {
            pending.texture->Release();
        }
        m_pendingCategories.clear();
        return 0;
    }
    
    // Use unified constants from AutoCategorisation
    using namespace MaterialPipeline::AutoCategorisation::CategoryFlags;
    
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
        
        // Filter category flags based on enable settings
        uint32_t filteredFlags = pending.categoryFlags;
        
        if (!m_enableParticleCategorization) {
            filteredFlags &= ~PARTICLE;
        }
        if (!m_enableDecalCategorization) {
            filteredFlags &= ~DECAL_STATIC;
        }
        if (!m_enableEmissiveCategorization) {
            filteredFlags &= ~EMISSIVE;
        }
        
        // If no categories remain after filtering, skip this material
        if (filteredFlags == 0) {
            pending.texture->Release();
            continue;
        }
        
        // Got a valid hash! Log it for debugging
        if (m_enableDebugOutput) {
            Msg("[D3D9TextureTracker] RetryPending: Got hash 0x%llX for '%s' (texture 0x%p)\n",
                hash, pending.materialName.c_str(), pending.texture);
        }
        
        // Apply filtered categories using the helper function
        ApplyCategoryToHash(hash, filteredFlags, pending.materialName.c_str());
        
        // Release our reference
        pending.texture->Release();
        successCount++;
    }
    
    m_pendingCategories = std::move(stillPending);
    
    if (successCount > 0 && m_enableDebugOutput) {
        Msg("[D3D9] RetryPendingCategories: %d categorized, %zu still pending\n", 
            successCount, m_pendingCategories.size());
    }

    // Process deferred BSP world material hash resolution.
    // These are textures whose hash was 0 at discovery time; once resolved we update
    // m_hashToMaterials and remove any stale PARTICLE tag (applied before the collision
    // was detected) from both the Remix API and both flag maps.
    if (!m_pendingBSPHashes.empty()) {
        std::vector<PendingBSPHash> stillPendingBSP;
        for (auto& pending : m_pendingBSPHashes) {
            auto result = g_remix->dxvk_GetTextureHash(pending.texture);
            if (!result || result.value() == 0) {
                stillPendingBSP.push_back(pending);
                continue;
            }
            uint64_t hash = result.value();

            std::string lowerName = pending.materialName;
            std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(),
                [](unsigned char c){ return std::tolower(c); });

            m_hashToMaterials[hash].insert(lowerName);

            uint32_t existingFlags = 0;
            GetHashCategoryFlags(hash, &existingFlags);
            uint32_t acFlags = 0;
            MaterialPipeline::AutoCategorisation::GetHashCategoryFlags(hash, &acFlags);
            if ((existingFlags | acFlags) & PARTICLE) {
                char hashStr[32];
                sprintf_s(hashStr, "0x%llX", hash);
                g_remix->RemoveTextureHash("rtx.particleTextures", hashStr);
                SetHashCategoryFlags(hash, existingFlags & ~PARTICLE);
                MaterialPipeline::AutoCategorisation::SetHashCategoryFlags(hash, acFlags & ~PARTICLE);
                if (m_enableDebugOutput) {
                    Msg("[D3D9TextureTracker] RetryBSP: Removed stale PARTICLE for BSP world material '%s' (hash %s)\n",
                        pending.materialName.c_str(), hashStr);
                }
            }

            pending.texture->Release();
        }
        m_pendingBSPHashes = std::move(stillPendingBSP);
    }

    return successCount;
}

int D3D9TextureTracker::RescanAllMaterials() {
    if (!g_remix || !materials) return 0;

    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    
    // Check if auto-categorization is enabled
    if (!m_enableAutoCategorization) {
        Msg("[D3D9] RescanAllMaterials: Auto-categorization is disabled, skipping rescan\n");
        return 0;
    }
    
    Msg("[D3D9] RescanAllMaterials: Scanning %zu cached materials through AutoCategorisation...\n", m_textureCache.size());
    
    int categorizedCount = 0;
    
    for (const auto& entry : m_textureCache) {
        const std::string& materialName = entry.first;
        const std::vector<IDirect3DTexture9*>& textures = entry.second;
        
        if (textures.empty()) continue;
        
        // Skip internal materials
        if (materialName.find("__") == 0) continue;
        
        // Get the material for AutoCategorisation
        IMaterial* mat = materials->FindMaterial(materialName.c_str(), TEXTURE_GROUP_OTHER, false);
        if (mat && mat->IsErrorMaterial()) {
            mat = nullptr;
        }
        
        // Use AutoCategorisation to detect and apply categories
        // This delegates to the unified categorisation logic in material_pipeline
        for (auto* tex : textures) {
            uint32_t categoryFlags = MaterialPipeline::AutoCategorisation::DetectAndApply(materialName, mat, tex);
            if (categoryFlags != 0) {
                categorizedCount++;
                break; // Only need one variant per material
            }
        }
    }
    
    Msg("[D3D9] RescanAllMaterials: Done! %d materials categorized through AutoCategorisation\n", categorizedCount);
    
    return categorizedCount;
}

int D3D9TextureTracker::RecheckWorldTextures() {
    if (!g_remix) return 0;
    
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    
    if (m_worldTextureNames.empty()) {
        Msg("[D3D9TextureTracker] RecheckWorldTextures: World texture list is empty\n");
        return 0;
    }
    
    Msg("[D3D9TextureTracker] RecheckWorldTextures: Checking %zu cached materials against %zu world textures...\n",
        m_textureCache.size(), m_worldTextureNames.size());
    
    // Use unified constant from AutoCategorisation
    using namespace MaterialPipeline::AutoCategorisation::CategoryFlags;
    
    int categorizedCount = 0;
    int matchCount = 0;
    
    for (const auto& entry : m_textureCache) {
        const std::string& materialName = entry.first;
        const std::vector<IDirect3DTexture9*>& textures = entry.second;
        
        if (textures.empty()) continue;
        
        // Skip internal materials
        if (materialName.find("__") == 0 || materialName.find("vgui") == 0) continue;
        
        // Normalize material name (inline version to avoid mutex deadlock)
        std::string lowerName = materialName;
        std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(),
            [](unsigned char c){ return std::tolower(c); });
        
        // Remove prefixes
        if (lowerName.find("materials/") == 0) {
            lowerName = lowerName.substr(10);
        }
        if (lowerName.size() > 4 && lowerName.substr(lowerName.size() - 4) == ".vmt") {
            lowerName = lowerName.substr(0, lowerName.size() - 4);
        }
        
        // IMPORTANT: Remove _stage1 suffix for displacement materials
        // BSP lists "material_name" but engine tracks "material_name_stage1"
        if (lowerName.size() > 7 && lowerName.substr(lowerName.size() - 7) == "_stage1") {
            lowerName = lowerName.substr(0, lowerName.size() - 7);
        }
        
        // Check if this material is in the world texture list
        bool isWorldTexture = m_worldTextureNames.find(lowerName) != m_worldTextureNames.end();
        
        if (!isWorldTexture) continue;
        
        matchCount++;
        
        // Get hash for ALL texture variants (displacement materials have multiple stages)
        int variantsCategorized = 0;
        for (auto* tex : textures) {
            auto result = g_remix->dxvk_GetTextureHash(tex);
            if (result && result.value() != 0) {
                uint64_t hash = result.value();
                
                // Check if already categorized
                uint32_t existingFlags = 0;
                if (GetHashCategoryFlags(hash, &existingFlags) && existingFlags != 0) {
                    // Already categorized, skip this variant
                    continue;
                }
                
                // Apply world geometry category using AutoCategorisation constant
                if (m_enableDebugOutput) {
                    Msg("[D3D9TextureTracker] RecheckWorldTextures: Categorizing '%s' variant %d (hash 0x%llX) as DECAL_STATIC\n",
                        materialName.c_str(), variantsCategorized + 1, hash);
                }
                ApplyCategoryToHash(hash, DECAL_STATIC, materialName.c_str());
                categorizedCount++;
                variantsCategorized++;
            }
        }
        
        if (m_enableDebugOutput && variantsCategorized > 1) {
            Msg("[D3D9TextureTracker]   -> Categorized %d texture variants for '%s'\n", 
                variantsCategorized, materialName.c_str());
        }
    }
    
    Msg("[D3D9TextureTracker] RecheckWorldTextures: Found %d world texture matches, categorized %d\n",
        matchCount, categorizedCount);
    
    return categorizedCount;
}

std::vector<std::tuple<std::string, void*, uint64_t>> D3D9TextureTracker::DumpAllTextureHashes() const {
    std::vector<std::tuple<std::string, void*, uint64_t>> result;
    
    if (!g_remix) return result;
    
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    
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

// Set world texture names from BSP parsing
void D3D9TextureTracker::SetWorldTextureNames(const std::vector<std::string>& textureNames) {
    if (textureNames.empty()) {
        Warning("[D3D9TextureTracker] SetWorldTextureNames: Empty texture list\n");
        return;
    }
    
    Msg("[D3D9TextureTracker] SetWorldTextureNames: Processing %zu textures...\n", textureNames.size());
    
    try {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        
        m_worldTextureNames.clear();
        m_bspWorldMaterials.clear();
        
        for (const auto& name : textureNames) {
            if (name.empty()) continue;
            
            // Normalize to lowercase for case-insensitive matching
            std::string lowerName = name;
            std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(),
                [](unsigned char c){ return std::tolower(c); });
            
            // Remove common prefixes/suffixes
            if (lowerName.find("materials/") == 0) {
                lowerName = lowerName.substr(10); // Remove "materials/"
            }
            if (lowerName.size() > 4 && lowerName.substr(lowerName.size() - 4) == ".vmt") {
                lowerName = lowerName.substr(0, lowerName.size() - 4); // Remove ".vmt"
            }
            
            // IMPORTANT: Remove _stage1 suffix for displacement materials
            // BSP lists "material_name" but engine tracks "material_name_stage1"
            if (lowerName.size() > 7 && lowerName.substr(lowerName.size() - 7) == "_stage1") {
                lowerName = lowerName.substr(0, lowerName.size() - 7);
            }
            
            if (!lowerName.empty()) {
                m_worldTextureNames.insert(lowerName);
                // The NikNaks list contains every material owned by the BSP,
                // including static-prop material slots. Keep the reverse
                // collision guard in sync with the real-time world list so a
                // late texture observation cannot strip DECAL_STATIC again.
                m_bspWorldMaterials.insert(lowerName);
            }
        }
        
        Msg("[D3D9TextureTracker] Loaded %zu world texture names for categorization\n", m_worldTextureNames.size());
    } catch (const std::exception& e) {
        Warning("[D3D9TextureTracker] SetWorldTextureNames: Exception: %s\n", e.what());
    }
    
    // Forward to AutoCategorisation so the pipeline's DetectCategory can identify
    // world textures during normal material processing (IsWorldTexture check)
    MaterialPipeline::AutoCategorisation::SetWorldTextureNames(textureNames);
}

// Clear world texture list
void D3D9TextureTracker::ClearWorldTextureNames() {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    m_worldTextureNames.clear();
    Msg("[D3D9TextureTracker] Cleared world texture list\n");
    
    // Keep AutoCategorisation in sync
    MaterialPipeline::AutoCategorisation::ClearWorldTextureNames();
}

// =========================================================================
// BSP World Material Registry (reverse hash-collision guard)
// =========================================================================

void D3D9TextureTracker::RegisterBSPWorldMaterial(const std::string& materialName) {
    if (materialName.empty()) return;

    std::lock_guard<std::recursive_mutex> lock(m_mutex);

    // Normalize to lowercase
    std::string lowerName = materialName;
    std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(),
        [](unsigned char c){ return std::tolower(c); });

    m_bspWorldMaterials.insert(lowerName);

    if (!g_remix) return;

    using namespace MaterialPipeline::AutoCategorisation::CategoryFlags;

    // Back-fill the reverse map with any variants already in the texture cache.
    // If any of those hashes were incorrectly claimed as PARTICLE before the BSP
    // scan ran, remove the stale tag from the Remix API and from our local map.
    auto cacheIt = m_textureCache.find(materialName);
    if (cacheIt == m_textureCache.end()) {
        // Try lowercase variant
        cacheIt = m_textureCache.find(lowerName);
    }
    if (cacheIt != m_textureCache.end()) {
        for (auto* tex : cacheIt->second) {
            if (!tex) continue;
            auto result = g_remix->dxvk_GetTextureHash(tex);
            if (!result) continue;
            uint64_t hash = result.value();
            if (hash == 0) {
                // Hash not yet assigned by RTX Remix — queue for deferred resolution.
                tex->AddRef();
                m_pendingBSPHashes.push_back({tex, materialName});
                continue;
            }

            m_hashToMaterials[hash].insert(lowerName);

            uint32_t existingFlags = 0;
            GetHashCategoryFlags(hash, &existingFlags);
            uint32_t acFlags = 0;
            MaterialPipeline::AutoCategorisation::GetHashCategoryFlags(hash, &acFlags);
            if ((existingFlags | acFlags) & PARTICLE) {
                char hashStr[32];
                sprintf_s(hashStr, "0x%llX", hash);
                g_remix->RemoveTextureHash("rtx.particleTextures", hashStr);
                SetHashCategoryFlags(hash, existingFlags & ~PARTICLE);
                MaterialPipeline::AutoCategorisation::SetHashCategoryFlags(hash, acFlags & ~PARTICLE);
                if (m_enableDebugOutput) {
                    Msg("[D3D9TextureTracker] RegisterBSPWorldMaterial: Removed stale PARTICLE for '%s' (hash %s)\n",
                        materialName.c_str(), hashStr);
                }
            }
        }
    }
}

void D3D9TextureTracker::ClearBSPWorldMaterials() {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    m_bspWorldMaterials.clear();
    m_contestedDecalHashes.clear();
    if (m_enableDebugOutput) {
        Msg("[D3D9TextureTracker] Cleared BSP world material registry\n");
    }
}

bool D3D9TextureTracker::IsAnyBSPWorldMaterialForHash(uint64_t hash) const {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    auto it = m_hashToMaterials.find(hash);
    if (it == m_hashToMaterials.end()) return false;
    for (const auto& name : it->second) {
        if (m_bspWorldMaterials.count(name)) return true;
    }
    return false;
}

std::vector<std::string>
D3D9TextureTracker::GetMaterialsForHash(uint64_t hash) const {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    std::vector<std::string> materialNames;
    auto it = m_hashToMaterials.find(hash);
    if (it == m_hashToMaterials.end()) {
        return materialNames;
    }

    materialNames.reserve(it->second.size());
    for (const auto& name : it->second) {
        materialNames.push_back(name);
    }
    return materialNames;
}

bool D3D9TextureTracker::HasNonBSPWorldMaterialForHash(uint64_t hash) const {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    auto it = m_hashToMaterials.find(hash);
    if (it == m_hashToMaterials.end()) return false;
    for (const auto& name : it->second) {
        std::string lower = name;
        std::transform(lower.begin(), lower.end(), lower.begin(),
            [](unsigned char c){ return std::tolower(c); });
        if (!m_bspWorldMaterials.count(lower)) return true;
    }
    return false;
}

bool D3D9TextureTracker::HasMaterialNotInWorldListForHash(uint64_t hash) const {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (m_worldTextureNames.empty()) return false;
    auto it = m_hashToMaterials.find(hash);
    if (it == m_hashToMaterials.end()) return false;
    for (const auto& rawName : it->second) {
        // Normalize the same way IsWorldTexture does.
        std::string lower = rawName;
        std::transform(lower.begin(), lower.end(), lower.begin(),
            [](unsigned char c){ return std::tolower(c); });
        if (lower.find("materials/") == 0) lower = lower.substr(10);
        if (lower.size() > 4 && lower.substr(lower.size() - 4) == ".vmt")
            lower = lower.substr(0, lower.size() - 4);
        if (lower.size() > 7 && lower.substr(lower.size() - 7) == "_stage1")
            lower = lower.substr(0, lower.size() - 7);
        const bool isWorldDecal =
            m_worldTextureNames.find(lower) != m_worldTextureNames.end();
        const bool isIntrinsicDecal =
            m_intrinsicDecalMaterials.find(lower) !=
            m_intrinsicDecalMaterials.end();
        if (!isWorldDecal && !isIntrinsicDecal) return true;
    }
    return false;
}

void D3D9TextureTracker::MarkHashContested(uint64_t hash) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    m_contestedDecalHashes.insert(hash);
}

bool D3D9TextureTracker::IsHashContested(uint64_t hash) const {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    return m_contestedDecalHashes.count(hash) != 0;
}

// Enable or disable automatic particle categorization
void D3D9TextureTracker::SetParticleCategorization(bool enabled) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    m_enableParticleCategorization = enabled;
    Msg("[D3D9TextureTracker] Particle categorization %s\n", enabled ? "enabled" : "disabled");
}

// Enable or disable automatic decal categorization
void D3D9TextureTracker::SetDecalCategorization(bool enabled) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    m_enableDecalCategorization = enabled;
    Msg("[D3D9TextureTracker] Decal categorization %s\n", enabled ? "enabled" : "disabled");
}

// Enable or disable automatic emissive categorization
void D3D9TextureTracker::SetEmissiveCategorization(bool enabled) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    m_enableEmissiveCategorization = enabled;
    Msg("[D3D9TextureTracker] Emissive categorization %s\n", enabled ? "enabled" : "disabled");
}

// Enable or disable ALL automatic categorization (master switch)
void D3D9TextureTracker::SetAutoCategorization(bool enabled) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    m_enableAutoCategorization = enabled;
    
    // If disabling, clear pending queue to prevent delayed categorization
    if (!enabled && !m_pendingCategories.empty()) {
        size_t count = m_pendingCategories.size();
        for (auto& pending : m_pendingCategories) {
            pending.texture->Release();
        }
        m_pendingCategories.clear();
        Msg("[D3D9TextureTracker] Cleared %zu pending categorizations\n", count);
    }
    
    Msg("[D3D9TextureTracker] Auto-categorization %s\n", enabled ? "enabled" : "disabled");
}

// Enable or disable debug output
void D3D9TextureTracker::SetDebugOutput(bool enabled) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    m_enableDebugOutput = enabled;
    Msg("[D3D9TextureTracker] Debug output %s\n", enabled ? "enabled" : "disabled");
}

// Check if material is a world texture
bool D3D9TextureTracker::IsWorldTexture(const std::string& materialName) const {
    if (materialName.empty()) return false;
    
    try {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        
        // Return false if world texture list hasn't been set yet
        if (m_worldTextureNames.empty()) return false;
        
        // Normalize material name
        std::string lowerName = materialName;
        std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(),
            [](unsigned char c){ return std::tolower(c); });
        
        // Remove prefixes
        if (lowerName.find("materials/") == 0) {
            lowerName = lowerName.substr(10);
        }
        if (lowerName.size() > 4 && lowerName.substr(lowerName.size() - 4) == ".vmt") {
            lowerName = lowerName.substr(0, lowerName.size() - 4);
        }
        
        // IMPORTANT: Remove _stage1 suffix for displacement materials
        // BSP lists "material_name" but engine tracks "material_name_stage1"
        if (lowerName.size() > 7 && lowerName.substr(lowerName.size() - 7) == "_stage1") {
            lowerName = lowerName.substr(0, lowerName.size() - 7);
        }
        
        return m_worldTextureNames.find(lowerName) != m_worldTextureNames.end();
    } catch (...) {
        // Silently fail - better than crashing during rendering
        return false;
    }
}

#endif // _WIN64
