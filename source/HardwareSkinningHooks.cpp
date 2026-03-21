// Hardware Skinning Hooks for RTX Remix
// Forces MESHGROUP_IS_HWSKINNED flag and sets bone transforms via D3D9 fixed-function API

#define HWSKIN_PATCHES

#if defined(HWSKIN_PATCHES) && defined(_WIN64)

#include "HardwareSkinningHooks.h"
#include "e_utils.h"  
#include "cbase.h"
#include "optimize.h"
#include "TinyMathLib.h"
#include "globalconvars.h"
#include "memory_patcher.h"
#include <tier0/dbg.h>
#include <d3d9.h>

// Debug logging - controlled at runtime via r_hwskin_debug convar
static void HWSkinDebugPrint(const char* format, ...) {
    // Check convar at runtime
    if (!GlobalConvars::r_hwskin_debug || !GlobalConvars::r_hwskin_debug->GetBool()) {
        return;
    }
    
    char buffer[1024];
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    buffer[sizeof(buffer) - 1] = '\0';
    
    // Output to Visual Studio debug output and game console
    OutputDebugStringA(buffer);
    Msg("%s", buffer);
}

// Always-on debug print (ignores convar, for critical messages)
static void HWSkinDebugPrintAlways(const char* format, ...) {
    char buffer[1024];
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    buffer[sizeof(buffer) - 1] = '\0';
    OutputDebugStringA(buffer);
    Msg("%s", buffer);
}

#define HWSKIN_DBG(...) HWSkinDebugPrint(__VA_ARGS__)
#define HWSKIN_DBG_ALWAYS(...) HWSkinDebugPrintAlways(__VA_ARGS__)

// D3D9 World Matrix transform type macro
// D3DTS_WORLDMATRIX(index) = 256 + index
#ifndef D3DTS_WORLDMATRIX
#define D3DTS_WORLDMATRIX(index) (D3DTRANSFORMSTATETYPE)(256 + (index))
#endif

// Global variables for bone data handling
BoneData_t g_BONEDATA = { -1, {}, false };
static bool g_bInRenderFinal = false;
static matrix3x4_t g_modelToWorld;
static bool g_bHaveModelToWorld = false;

// D3D9 device for setting bone transforms
static IDirect3DDevice9* g_pD3DDevice = nullptr;


// ============================================================================
// DrawIndexedPrimitive (DIP) vtable hook for per-strip bone updates
// ============================================================================
typedef HRESULT (STDMETHODCALLTYPE* DrawIndexedPrimitive_t)(
    IDirect3DDevice9* pDevice,
    D3DPRIMITIVETYPE PrimitiveType,
    INT BaseVertexIndex,
    UINT MinVertexIndex,
    UINT NumVertices,
    UINT StartIndex,
    UINT PrimitiveCount);

static DrawIndexedPrimitive_t g_OriginalDIP = nullptr;

// Forward declarations for DIP hook
struct ModelBoneRemapCache;
static void ComputeFinalBoneMatrix(int skeletonBoneIndex, studiohdr_t* pStudioHdr,
                                    matrix3x4_t* m_PoseToWorld, int numBones,
                                    const matrix3x4_t& worldToModel, bool haveModelToWorld,
                                    matrix3x4_t& finalMatrix);
static void ConvertBoneToD3DMatrix(const matrix3x4_t& finalMatrix, D3DMATRIX& d3dMatrix);

// Forward declaration for DIP hook function (defined later in the file)
static HRESULT STDMETHODCALLTYPE Hook_DrawIndexedPrimitive(
    IDirect3DDevice9* pDevice, D3DPRIMITIVETYPE PrimitiveType,
    INT BaseVertexIndex, UINT MinVertexIndex, UINT NumVertices,
    UINT StartIndex, UINT PrimitiveCount);

// Helper functions for DIP vtable hook installation/removal.
// These must be separate functions because MSVC forbids mixing __try/__except (SEH)
// with C++ try/catch in the same function (C2712/C2713).
static bool InstallDIPVtableHook(IDirect3DDevice9* pDevice) {
    __try {
        void** vtable = *(void***)pDevice;
        const int DIP_VTABLE_INDEX = 82;
        
        g_OriginalDIP = (DrawIndexedPrimitive_t)vtable[DIP_VTABLE_INDEX];
        
        DWORD oldProtect;
        if (VirtualProtect(&vtable[DIP_VTABLE_INDEX], sizeof(void*), PAGE_READWRITE, &oldProtect)) {
            vtable[DIP_VTABLE_INDEX] = (void*)Hook_DrawIndexedPrimitive;
            VirtualProtect(&vtable[DIP_VTABLE_INDEX], sizeof(void*), oldProtect, &oldProtect);
            return true;
        } else {
            g_OriginalDIP = nullptr;
            return false;
        }
    }
    __except(EXCEPTION_EXECUTE_HANDLER) {
        g_OriginalDIP = nullptr;
        return false;
    }
}

static void RestoreDIPVtableHook(IDirect3DDevice9* pDevice) {
    __try {
        void** vtable = *(void***)pDevice;
        const int DIP_VTABLE_INDEX = 82;
        DWORD oldProtect;
        if (VirtualProtect(&vtable[DIP_VTABLE_INDEX], sizeof(void*), PAGE_READWRITE, &oldProtect)) {
            vtable[DIP_VTABLE_INDEX] = (void*)g_OriginalDIP;
            VirtualProtect(&vtable[DIP_VTABLE_INDEX], sizeof(void*), oldProtect, &oldProtect);
        }
    }
    __except(EXCEPTION_EXECUTE_HANDLER) {
        // Best effort - nothing more we can do
    }
}

// Per-strip draw state: populated before calling the original R_StudioDrawGroupHWSkin
// for multi-strip mesh groups, consumed by the DIP hook to apply per-strip bone changes.
struct PerStripDrawState {
    bool active;                    // true = we're inside a multi-strip draw
    int currentStripIndex;          // which DIP call we're on (0-based)
    int totalStrips;                // total strips in this mesh group
    studiomeshgroup_t* pGroup;      // current mesh group
    ModelBoneRemapCache* cache;     // current model's bone remap cache
    studiohdr_t* pStudioHdr;
    matrix3x4_t* poseToWorld;
    int numBones;
    matrix3x4_t worldToModel;
    bool haveModelToWorld;
};
static PerStripDrawState g_PerStripState = {};

// Thread-local storage for transformed bone matrices
static matrix3x4_t g_transformedBones[512];

// studiomeshgroup_t flags
#define MESHGROUP_IS_FLEXED         0x1
#define MESHGROUP_IS_HWSKINNED      0x2
#define MESHGROUP_IS_DELTA_FLEXED   0x4

// StripGroupFlags_t from optimize.h
#define STRIPGROUP_IS_FLEXED        0x01
#define STRIPGROUP_IS_HWSKINNED     0x02
#define STRIPGROUP_IS_DELTA_FLEXED  0x04

// Maximum bones supported by D3D9 indexed vertex blending
#define MAX_D3D9_BONES 256

// ============================================================================
// Function pointer typedefs for x64
// ============================================================================

typedef void* (__fastcall* F_StudioCreateSingleMesh)(
    void* _this,
    int* pStudioHdr,
    __int64 pStudioLodData,
    int* pMesh,
    int* pVtxMesh,
    int numBones,
    __int64 pMeshData,
    unsigned short* pColorMeshID
);

// GMod's R_StudioDrawGroupHWSkin has 9 parameters (different from Source SDK's 5)
// Based on IDA reverse engineering of studiorender.dll
typedef __int64 (__fastcall* F_StudioDrawGroupHWSkin)(
    void* _this,                    // CStudioRenderContext*
    void* pRenderContext,           // IMatRenderContext*
    __int64 bodyPartInfo,           // Body part info / mesh index
    studiomeshgroup_t* pGroup,      // Mesh group pointer
    int lighting,                   // StudioModelLighting_t
    float r_blend,                  // Alpha blend factor (0.0-1.0) - NOT numBones!
    void* pMaterial,                // IMaterial*
    unsigned int flags,             // Render flags
    void* pColorMeshInfo            // ColorMeshInfo_t* or nullptr
);

// GMod's R_StudioRenderFinal has 9 parameters (different from Source SDK's 11)
// Based on IDA reverse engineering of studiorender.dll
typedef __int64 (__fastcall* F_StudioRenderFinal)(
    void* _this,                    // CStudioRenderContext*
    void* pRenderContext,           // IMatRenderContext*
    int skin,                       // Skin index
    __int64 pBodyPartInfo,          // BodyPartInfo_t* or similar
    void* pClientEntity,            // IClientEntity*
    void* ppMaterials,              // IMaterial**
    int pMaterialFlags,             // Material flags (was int* in SDK)
    unsigned int boneMask,          // Bone mask
    void* pColorMeshes              // ColorMeshInfo_t*
);

// R_StudioDrawEyeball - handles special eye mesh rendering
// In Source SDK: CStudioRender::R_StudioDrawEyeball(IMatRenderContext*, mstudiomesh_t*,
//                studiomeshdata_t*, StudioModelLighting_t, IMaterial*, int)
typedef int (__fastcall* F_StudioDrawEyeball)(
    void* _this,                    // CStudioRender*
    void* pRenderContext,           // IMatRenderContext*
    void* pmesh,                    // mstudiomesh_t*
    void* pMeshData,                // studiomeshdata_t*
    int lighting,                   // StudioModelLighting_t (0=HW, 1=SW, 2=MOUTH)
    void* pMaterial,                // IMaterial*
    int lod                         // LOD level
);

// Function pointers for trampolines
static F_StudioCreateSingleMesh R_StudioCreateSingleMesh_Original = nullptr;
static F_StudioDrawGroupHWSkin R_StudioDrawGroupHWSkin_Original = nullptr;
static F_StudioRenderFinal R_StudioRenderFinal_Original = nullptr;
static F_StudioDrawEyeball R_StudioDrawEyeball_Original = nullptr;

// ============================================================================
// D3D9 Fixed-Function Bone Transform Helper
// Sets bone transforms via D3D9 SetTransform for RTX Remix to pick up
// ============================================================================
// Identity matrix for clearing bone slots
static const D3DMATRIX g_IdentityMatrix = {
    1.0f, 0.0f, 0.0f, 0.0f,
    0.0f, 1.0f, 0.0f, 0.0f,
    0.0f, 0.0f, 1.0f, 0.0f,
    0.0f, 0.0f, 0.0f, 1.0f
};

// Track max bones we've set to know how many to clear
static int g_MaxBonesEverSet = 0;

// ============================================================================
// Per-Model Bone Remap Cache
//
// Source Engine bone state changes are INCREMENTAL across mesh groups of the
// same model. The first mesh group sets up all bone mappings, and subsequent
// mesh groups only record what CHANGED. The engine maintains an internal bone
// array that accumulates these changes.
//
// Problem: our bone matrices are set via D3D9 WORLDMATRIX slots (global state).
// When different models draw between mesh groups of the same model, the
// intermediate model's bones OVERWRITE the WORLDMATRIX slots. When the original
// model's next mesh group draws with only incremental changes, the overwritten
// slots have the wrong model's bones.
//
// Solution: maintain a per-model bone remap cache (hwID → skeletonBoneID)
// that accumulates across all mesh groups. On each draw, we apply the FULL
// accumulated mapping, not just the incremental changes from the current mesh
// group's strip data.
//
// The mapping is a static property of the model's optimized mesh data, so
// the cache is persistent and grows to full size after the first frame.
// ============================================================================
struct ModelBoneRemapCache {
    studiohdr_t* pStudioHdr;      // which model this cache belongs to
    int remapTable[MAX_D3D9_BONES]; // hwID -> skeletonBoneID (-1 = not mapped)
    int maxHwID;                    // highest hwID + 1 (for iteration)
    bool active;                    // whether this slot is in use
};

static const int MAX_CACHED_MODELS = 32;
static ModelBoneRemapCache g_BoneRemapCaches[MAX_CACHED_MODELS];
static int g_NumActiveCaches = 0;

// Find or create a bone remap cache for the given model.
// The cache persists across frames since the hwID→boneID mapping is a static
// property of the model's optimized mesh data and doesn't change.
static ModelBoneRemapCache* GetOrCreateBoneRemapCache(studiohdr_t* pStudioHdr) {
    // Search for existing cache
    for (int i = 0; i < g_NumActiveCaches; i++) {
        if (g_BoneRemapCaches[i].active && g_BoneRemapCaches[i].pStudioHdr == pStudioHdr) {
            return &g_BoneRemapCaches[i];
        }
    }
    
    // Create new cache
    if (g_NumActiveCaches < MAX_CACHED_MODELS) {
        ModelBoneRemapCache* cache = &g_BoneRemapCaches[g_NumActiveCaches++];
        cache->pStudioHdr = pStudioHdr;
        cache->maxHwID = 0;
        cache->active = true;
        memset(cache->remapTable, -1, sizeof(cache->remapTable));
        return cache;
    }
    
    // Cache full - reuse oldest slot (slot 0)
    ModelBoneRemapCache* cache = &g_BoneRemapCaches[0];
    cache->pStudioHdr = pStudioHdr;
    cache->maxHwID = 0;
    cache->active = true;
    memset(cache->remapTable, -1, sizeof(cache->remapTable));
    return cache;
}

// Helper: convert a matrix3x4_t to D3DMATRIX using current convar settings
static void ConvertBoneToD3DMatrix(const matrix3x4_t& finalMatrix, D3DMATRIX& d3dMatrix) {
    int transposeMode = GlobalConvars::r_hwskin_transpose ? GlobalConvars::r_hwskin_transpose->GetInt() : 1;
    
    if (transposeMode == 2) {
        // Identity
        d3dMatrix._11 = 1.0f; d3dMatrix._12 = 0.0f; d3dMatrix._13 = 0.0f; d3dMatrix._14 = 0.0f;
        d3dMatrix._21 = 0.0f; d3dMatrix._22 = 1.0f; d3dMatrix._23 = 0.0f; d3dMatrix._24 = 0.0f;
        d3dMatrix._31 = 0.0f; d3dMatrix._32 = 0.0f; d3dMatrix._33 = 1.0f; d3dMatrix._34 = 0.0f;
        d3dMatrix._41 = 0.0f; d3dMatrix._42 = 0.0f; d3dMatrix._43 = 0.0f; d3dMatrix._44 = 1.0f;
    } else if (transposeMode == 4) {
        // Direct copy - translation in column 4
        d3dMatrix._11 = finalMatrix[0][0]; d3dMatrix._12 = finalMatrix[0][1]; d3dMatrix._13 = finalMatrix[0][2]; d3dMatrix._14 = finalMatrix[0][3];
        d3dMatrix._21 = finalMatrix[1][0]; d3dMatrix._22 = finalMatrix[1][1]; d3dMatrix._23 = finalMatrix[1][2]; d3dMatrix._24 = finalMatrix[1][3];
        d3dMatrix._31 = finalMatrix[2][0]; d3dMatrix._32 = finalMatrix[2][1]; d3dMatrix._33 = finalMatrix[2][2]; d3dMatrix._34 = finalMatrix[2][3];
        d3dMatrix._41 = 0.0f;             d3dMatrix._42 = 0.0f;             d3dMatrix._43 = 0.0f;             d3dMatrix._44 = 1.0f;
    } else if (transposeMode == 5) {
        // Rotation only (zero translation)
        d3dMatrix._11 = finalMatrix[0][0]; d3dMatrix._12 = finalMatrix[1][0]; d3dMatrix._13 = finalMatrix[2][0]; d3dMatrix._14 = 0.0f;
        d3dMatrix._21 = finalMatrix[0][1]; d3dMatrix._22 = finalMatrix[1][1]; d3dMatrix._23 = finalMatrix[2][1]; d3dMatrix._24 = 0.0f;
        d3dMatrix._31 = finalMatrix[0][2]; d3dMatrix._32 = finalMatrix[1][2]; d3dMatrix._33 = finalMatrix[2][2]; d3dMatrix._34 = 0.0f;
        d3dMatrix._41 = 0.0f;             d3dMatrix._42 = 0.0f;             d3dMatrix._43 = 0.0f;             d3dMatrix._44 = 1.0f;
    } else if (transposeMode == 6) {
        // Translation only
        d3dMatrix._11 = 1.0f; d3dMatrix._12 = 0.0f; d3dMatrix._13 = 0.0f; d3dMatrix._14 = 0.0f;
        d3dMatrix._21 = 0.0f; d3dMatrix._22 = 1.0f; d3dMatrix._23 = 0.0f; d3dMatrix._24 = 0.0f;
        d3dMatrix._31 = 0.0f; d3dMatrix._32 = 0.0f; d3dMatrix._33 = 1.0f; d3dMatrix._34 = 0.0f;
        d3dMatrix._41 = finalMatrix[0][3]; d3dMatrix._42 = finalMatrix[1][3]; d3dMatrix._43 = finalMatrix[2][3]; d3dMatrix._44 = 1.0f;
    } else if (transposeMode == 1) {
        // Transpose rotation, translation in row 4 (DEFAULT)
        d3dMatrix._11 = finalMatrix[0][0]; d3dMatrix._12 = finalMatrix[1][0]; d3dMatrix._13 = finalMatrix[2][0]; d3dMatrix._14 = 0.0f;
        d3dMatrix._21 = finalMatrix[0][1]; d3dMatrix._22 = finalMatrix[1][1]; d3dMatrix._23 = finalMatrix[2][1]; d3dMatrix._24 = 0.0f;
        d3dMatrix._31 = finalMatrix[0][2]; d3dMatrix._32 = finalMatrix[1][2]; d3dMatrix._33 = finalMatrix[2][2]; d3dMatrix._34 = 0.0f;
        d3dMatrix._41 = finalMatrix[0][3]; d3dMatrix._42 = finalMatrix[1][3]; d3dMatrix._43 = finalMatrix[2][3]; d3dMatrix._44 = 1.0f;
    } else {
        // No transpose, translation in row 4
        d3dMatrix._11 = finalMatrix[0][0]; d3dMatrix._12 = finalMatrix[0][1]; d3dMatrix._13 = finalMatrix[0][2]; d3dMatrix._14 = 0.0f;
        d3dMatrix._21 = finalMatrix[1][0]; d3dMatrix._22 = finalMatrix[1][1]; d3dMatrix._23 = finalMatrix[1][2]; d3dMatrix._24 = 0.0f;
        d3dMatrix._31 = finalMatrix[2][0]; d3dMatrix._32 = finalMatrix[2][1]; d3dMatrix._33 = finalMatrix[2][2]; d3dMatrix._34 = 0.0f;
        d3dMatrix._41 = finalMatrix[0][3]; d3dMatrix._42 = finalMatrix[1][3]; d3dMatrix._43 = finalMatrix[2][3]; d3dMatrix._44 = 1.0f;
    }
}

// Helper: compute the final bone matrix for a given skeleton bone index
static void ComputeFinalBoneMatrix(int skeletonBoneIndex, studiohdr_t* pStudioHdr,
                                    matrix3x4_t* m_PoseToWorld, int numBones,
                                    const matrix3x4_t& worldToModel, bool haveModelToWorld,
                                    matrix3x4_t& finalMatrix) {
    int transformMode = GlobalConvars::r_hwskin_transform_chain ? 
                        GlobalConvars::r_hwskin_transform_chain->GetInt() : 0;
    
    if (transformMode == 1 && haveModelToWorld && numBones > 1) {
        TinyMathLib_ConcatTransforms(worldToModel, m_PoseToWorld[skeletonBoneIndex], finalMatrix);
    } else if (transformMode == 2 && haveModelToWorld && numBones > 1 && pStudioHdr) {
        mstudiobone_t* bdata = pStudioHdr->pBone(skeletonBoneIndex);
        matrix3x4_t poseToBoneInverse;
        TinyMathLib_MatrixInverseTR(bdata->poseToBone, poseToBoneInverse);
        
        matrix3x4_t temp_pBoneToWorld;
        TinyMathLib_ConcatTransforms(m_PoseToWorld[skeletonBoneIndex], poseToBoneInverse, temp_pBoneToWorld);
        
        matrix3x4_t new_BoneToWorld;
        TinyMathLib_ConcatTransforms(worldToModel, temp_pBoneToWorld, new_BoneToWorld);
        
        TinyMathLib_ConcatTransforms(new_BoneToWorld, bdata->poseToBone, finalMatrix);
    } else {
        memcpy(&finalMatrix, &m_PoseToWorld[skeletonBoneIndex], sizeof(matrix3x4_t));
    }
}

static void SetD3D9BoneTransforms(IDirect3DDevice9* pDevice, studiohdr_t* pStudioHdr,
                                   matrix3x4_t* m_PoseToWorld, int numBones,
                                   const matrix3x4_t& modelToWorld, bool haveModelToWorld,
                                   studiomeshgroup_t* pGroup) {
    if (!pDevice || !m_PoseToWorld || numBones <= 0) {
        return;
    }
    
    // Clamp to D3D9 limit
    if (numBones > MAX_D3D9_BONES) {
        numBones = MAX_D3D9_BONES;
    }
    
    if (numBones > g_MaxBonesEverSet) {
        g_MaxBonesEverSet = numBones;
    }
    
    // Enable indexed vertex blending for multi-bone models.
    // RTX Remix requires D3DRS_VERTEXBLEND to be non-DISABLE to process skinning data.
    if (numBones > 1) {
        pDevice->SetRenderState(D3DRS_INDEXEDVERTEXBLENDENABLE, TRUE);
        pDevice->SetRenderState(D3DRS_VERTEXBLEND, D3DVBF_2WEIGHTS);
    } else {
        pDevice->SetRenderState(D3DRS_INDEXEDVERTEXBLENDENABLE, FALSE);
        pDevice->SetRenderState(D3DRS_VERTEXBLEND, D3DVBF_DISABLE);
    }

    matrix3x4_t worldToModel;
    if (haveModelToWorld && numBones > 1 && pStudioHdr) {
        TinyMathLib_MatrixInvert(modelToWorld, worldToModel);
    }

    // =========================================================================
    // BONE REMAPPING with Per-Model Cache
    //
    // Source Engine bone state changes are INCREMENTAL across mesh groups of the
    // same model. The first mesh group sets up all bone mappings; subsequent mesh
    // groups only record what CHANGED. The engine maintains an internal bone array
    // that accumulates these changes.
    //
    // Problem: D3D9 WORLDMATRIX slots are GLOBAL. When different models draw
    // between mesh groups of the same model, the intermediate model's bones
    // OVERWRITE the WORLDMATRIX slots. The next mesh group's incremental changes
    // can't fix hardware IDs that weren't in its strip data.
    //
    // Solution: maintain a per-model bone remap CACHE (hwID -> skeletonBoneID).
    // The cache accumulates mappings across all mesh groups. On each draw, we
    // apply ALL cached mappings to D3D9, ensuring the full accumulated state is
    // restored even after other models have drawn.
    //
    // The mapping is a STATIC property of the model's optimized mesh data - it
    // doesn't change between frames. Only the bone poses (m_PoseToWorld) change.
    // So the cache is permanent and grows to full size after the first frame.
    // =========================================================================

    int bonesSetCount = 0;
    bool usedStripRemap = false;
    int newStateChanges = 0;
    
    if (pGroup && pGroup->m_pStripData && pGroup->m_NumStrips > 0 && numBones > 1) {
        // Get or create the bone remap cache for this model
        ModelBoneRemapCache* cache = GetOrCreateBoneRemapCache(pStudioHdr);
        
        // =====================================================================
        // Phase 1: Process ONLY the first strip's bone state changes.
        //
        // For multi-strip mesh groups, the engine draws strips in order:
        // strip 1 first, then strip 2, etc. Each strip's bone state changes
        // are incremental from the previous strip. Strip 2 may remap some
        // hardware IDs to DIFFERENT skeleton bones than strip 1.
        //
        // Since we can't intercept between per-strip draws (the original
        // function handles the per-strip loop internally), we set D3D9 bones
        // once before the draw. By using only strip 1's mapping, strip 1's
        // vertices (the majority) get correct bones. Strip 2's vertices that
        // reference remapped IDs will use strip 1's mapping instead - slightly
        // wrong for those few bones but much better than the reverse.
        // =====================================================================
        {
            OptimizedModel::StripHeader_t* pStrip = &pGroup->m_pStripData[0];
            for (int k = 0; k < pStrip->numBoneStateChanges; ++k) {
                OptimizedModel::BoneStateChangeHeader_t* pStateChange = pStrip->pBoneStateChange(k);
                if (pStateChange->newBoneID < 0)
                    break;
                
                int hwID = pStateChange->hardwareID;
                int boneID = pStateChange->newBoneID;
                
                if (hwID < 0 || hwID >= MAX_D3D9_BONES || boneID >= numBones)
                    continue;
                
                // Add/update the mapping in the cache
                if (cache->remapTable[hwID] != boneID) {
                    cache->remapTable[hwID] = boneID;
                    newStateChanges++;
                }
                
                if (hwID + 1 > cache->maxHwID) {
                    cache->maxHwID = hwID + 1;
                }
            }
        }
        
        // Apply the accumulated cache to D3D9.
        // This re-applies ALL cached mappings to restore any that were overwritten
        // by intermediate models' draws. At this point, the cache reflects:
        //   - all previous mesh groups' accumulated state
        //   - this mesh group's strip 1 bone state changes
        // This gives correct bones for strip 1's vertices (the majority).
        for (int hwID = 0; hwID < cache->maxHwID; ++hwID) {
            int boneID = cache->remapTable[hwID];
            if (boneID < 0 || boneID >= numBones)
                continue;
            
            matrix3x4_t finalMatrix;
            ComputeFinalBoneMatrix(boneID, pStudioHdr, m_PoseToWorld, numBones,
                                   worldToModel, haveModelToWorld, finalMatrix);
            
            D3DMATRIX d3dMatrix;
            ConvertBoneToD3DMatrix(finalMatrix, d3dMatrix);
            pDevice->SetTransform(D3DTS_WORLDMATRIX(hwID), &d3dMatrix);
            
            bonesSetCount++;
            usedStripRemap = true;
        }
        
        // NOTE: Phase 2 (processing strip 2+ into cache) is now DEFERRED to
        // after the draw call returns, in Hook_StudioDrawGroupHWSkin. This is
        // because the DIP hook handles per-strip bone updates incrementally
        // during the draw. The cache update for future mesh groups happens after.
        
        // Track max hardware ID for clearing
        if (cache->maxHwID > g_MaxBonesEverSet) {
            g_MaxBonesEverSet = cache->maxHwID;
        }
        
        // Log bone remap info (rate-limited)
        static int remapLogCount = 0;
        if (usedStripRemap && (remapLogCount++ < 5 || (remapLogCount % 2000) == 0)) {
            HWSKIN_DBG("[HWSkin] BoneRemap: %d strips%s, %d new + %d cached = %d D3D9 applied, numBones=%d (call #%d)\n",
                pGroup->m_NumStrips,
                (pGroup->m_NumStrips > 1 && g_OriginalDIP) ? " (DIP hook active)" : "",
                newStateChanges, bonesSetCount - newStateChanges,
                bonesSetCount, numBones, remapLogCount);
        }
    }
    
    if (!usedStripRemap) {
        // Fallback: no strip data available, set bones at skeleton indices.
        // This path is used for single-bone models or if strip data is missing.
        for (int i = 0; i < numBones; i++) {
            matrix3x4_t finalMatrix;
            ComputeFinalBoneMatrix(i, pStudioHdr, m_PoseToWorld, numBones,
                                   worldToModel, haveModelToWorld, finalMatrix);
            
            D3DMATRIX d3dMatrix;
            ConvertBoneToD3DMatrix(finalMatrix, d3dMatrix);
            pDevice->SetTransform(D3DTS_WORLDMATRIX(i), &d3dMatrix);
            bonesSetCount++;
        }
    }
    
    // Debug: log first bone matrix (rate-limited)
    static int bone0LogCount = 0;
    bone0LogCount++;
    if (bone0LogCount <= 3 || (bone0LogCount % 1000) == 0) {
        D3DMATRIX check;
        if (SUCCEEDED(pDevice->GetTransform(D3DTS_WORLDMATRIX(0), &check))) {
            HWSKIN_DBG("[HWSkin] Bone0 src m_PoseToWorld:\n");
            HWSKIN_DBG("  [%.3f, %.3f, %.3f, %.3f]\n", 
                m_PoseToWorld[0][0][0], m_PoseToWorld[0][0][1], m_PoseToWorld[0][0][2], m_PoseToWorld[0][0][3]);
            HWSKIN_DBG("  [%.3f, %.3f, %.3f, %.3f]\n", 
                m_PoseToWorld[0][1][0], m_PoseToWorld[0][1][1], m_PoseToWorld[0][1][2], m_PoseToWorld[0][1][3]);
            HWSKIN_DBG("  [%.3f, %.3f, %.3f, %.3f]\n", 
                m_PoseToWorld[0][2][0], m_PoseToWorld[0][2][1], m_PoseToWorld[0][2][2], m_PoseToWorld[0][2][3]);
            HWSKIN_DBG("[HWSkin] WORLDMATRIX(0) D3D (remap=%s):\n", usedStripRemap ? "yes" : "no");
            HWSKIN_DBG("  [%.3f, %.3f, %.3f, %.3f]\n", check._11, check._12, check._13, check._14);
            HWSKIN_DBG("  [%.3f, %.3f, %.3f, %.3f]\n", check._21, check._22, check._23, check._24);
            HWSKIN_DBG("  [%.3f, %.3f, %.3f, %.3f]\n", check._31, check._32, check._33, check._34);
            HWSKIN_DBG("  [%.3f, %.3f, %.3f, %.3f]\n", check._41, check._42, check._43, check._44);
        }
    }
    
    // Debug logging (rate-limited to avoid spam)
    static int logCounter = 0;
    if ((logCounter++ % 500) == 0) {
        HWSKIN_DBG("[HWSkin] Set %d bone transforms via D3D9 SetTransform (call #%d)\n", 
            bonesSetCount, logCounter);
    }
}

// Reset D3D9 bone state after drawing
static void ResetD3D9BoneState(IDirect3DDevice9* pDevice) {
    if (!pDevice) return;
    
    pDevice->SetRenderState(D3DRS_INDEXEDVERTEXBLENDENABLE, FALSE);
    pDevice->SetRenderState(D3DRS_VERTEXBLEND, D3DVBF_DISABLE);
}

// ============================================================================
// DrawIndexedPrimitive (DIP) Vtable Hook
//
// Intercepts per-strip draw calls inside R_StudioDrawGroupHWSkin.
// For multi-strip mesh groups, the engine draws strip 1, then strip 2, etc.
// Each strip may remap some hardware bone IDs to different skeleton bones.
// This hook applies the incremental bone state changes between strips so
// each strip's vertices reference the correct bone matrices.
//
// For non-skinned draws and single-strip groups, this is a near-zero-cost
// branch on the g_PerStripState.active boolean.
// ============================================================================
static HRESULT STDMETHODCALLTYPE Hook_DrawIndexedPrimitive(
    IDirect3DDevice9* pDevice,
    D3DPRIMITIVETYPE PrimitiveType,
    INT BaseVertexIndex,
    UINT MinVertexIndex,
    UINT NumVertices,
    UINT StartIndex,
    UINT PrimitiveCount)
{
    // Fast path: if we're not in a multi-strip skinned draw, pass through immediately
    if (g_PerStripState.active) {
        int stripIdx = g_PerStripState.currentStripIndex;
        
        // For strip 2+ (index > 0), apply that strip's incremental bone state changes
        if (stripIdx > 0 && stripIdx < g_PerStripState.totalStrips) {
            OptimizedModel::StripHeader_t* pStrip = &g_PerStripState.pGroup->m_pStripData[stripIdx];
            
            for (int k = 0; k < pStrip->numBoneStateChanges; ++k) {
                OptimizedModel::BoneStateChangeHeader_t* pStateChange = pStrip->pBoneStateChange(k);
                if (pStateChange->newBoneID < 0)
                    break;
                
                int hwID = pStateChange->hardwareID;
                int boneID = pStateChange->newBoneID;
                
                if (hwID < 0 || hwID >= MAX_D3D9_BONES || boneID >= g_PerStripState.numBones)
                    continue;
                
                // Compute and apply the bone matrix for this remapped hardware ID
                matrix3x4_t finalMatrix;
                ComputeFinalBoneMatrix(boneID, g_PerStripState.pStudioHdr,
                                       g_PerStripState.poseToWorld, g_PerStripState.numBones,
                                       g_PerStripState.worldToModel, g_PerStripState.haveModelToWorld,
                                       finalMatrix);
                
                D3DMATRIX d3dMatrix;
                ConvertBoneToD3DMatrix(finalMatrix, d3dMatrix);
                pDevice->SetTransform(D3DTS_WORLDMATRIX(hwID), &d3dMatrix);
            }
            
            // Rate-limited logging for per-strip bone updates
            static int perStripLogCount = 0;
            if (perStripLogCount++ < 5 || (perStripLogCount % 5000) == 0) {
                HWSKIN_DBG("[HWSkin] DIP hook: applied strip %d/%d bone changes (%d state changes)\n",
                    stripIdx + 1, g_PerStripState.totalStrips, pStrip->numBoneStateChanges);
            }
        }
        
        // Increment strip counter for next DIP call
        g_PerStripState.currentStripIndex++;
    }
    
    return g_OriginalDIP(pDevice, PrimitiveType, BaseVertexIndex,
                          MinVertexIndex, NumVertices, StartIndex, PrimitiveCount);
}


// ============================================================================
// Exported helper: called from the Eyes_dx9 shader (stdshader_dx6.dll) to
// set/reset D3D9 texture-generation state for the iris & glint passes.
//
// GMod's shaderapidx9.dll is compiled without FIXED_FUNCTION_PIPELINE, so
// IShaderShadow::EnableTexGen / TexGen are no-ops and D3DTSS_TEXCOORDINDEX
// is never set to D3DTSS_TCI_CAMERASPACEPOSITION.  We bridge the gap here
// by writing the state directly on the device from the shader's DYNAMIC_STATE
// block, which is the correct point in the frame to touch per-pass D3D state.
// ============================================================================
extern "C" __declspec(dllexport) void RTX_SetEyeTexGenState(int enable)
{
    if (!g_pD3DDevice) return;

    if (enable) {
        g_pD3DDevice->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX,
                                            D3DTSS_TCI_CAMERASPACEPOSITION);
        g_pD3DDevice->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS,
                                            D3DTTFF_COUNT2);
        // CLAMP prevents the iris texture from tiling when TexGen UVs
        // exceed [0,1] for vertices outside the iris projection area.
        g_pD3DDevice->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
        g_pD3DDevice->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    } else {
        g_pD3DDevice->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, 0);
        g_pD3DDevice->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS,
                                            D3DTTFF_DISABLE);
        g_pD3DDevice->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP);
        g_pD3DDevice->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_WRAP);
    }
}

// ============================================================================
// Hook: R_StudioDrawEyeball
// Intercepts eye mesh rendering to:
//   1) Log diagnostic info (which early-out kills rendering)
//   2) Force LIGHTING_HARDWARE so eyes go through the HW skinning path
//      instead of the software vertex transform path (invisible to RTX Remix)
// ============================================================================
Define_method_Hook(int, R_StudioDrawEyeball, void*,
    void* pRenderContext,
    void* pmesh,
    void* pMeshData,
    int lighting,
    void* pMaterial,
    int lod)
{
    static int eyeCallCount = 0;
    eyeCallCount++;

    bool forceEyeHW = GlobalConvars::r_eyes_hwskin && GlobalConvars::r_eyes_hwskin->GetBool();
    bool forcehwskin = GlobalConvars::r_forcehwskin && GlobalConvars::r_forcehwskin->GetBool();
    bool shouldForce = forceEyeHW && forcehwskin && g_bInRenderFinal;

    if (eyeCallCount <= 10 || (eyeCallCount % 2000) == 0) {
        HWSKIN_DBG_ALWAYS("[HWSkin] R_StudioDrawEyeball #%d: lighting=%d (%s), pMaterial=%p, lod=%d, forceHW=%d\n",
            eyeCallCount, lighting,
            (lighting == 0 ? "HARDWARE" : (lighting == 1 ? "SOFTWARE" : "MOUTH")),
            pMaterial, lod, shouldForce);
    }

    int actualLighting = lighting;
    if (shouldForce) {
        actualLighting = 0; // LIGHTING_HARDWARE
    }

    int result = 0;
    __try {
        result = R_StudioDrawEyeball_trampoline()(_this, pRenderContext, pmesh, pMeshData, actualLighting, pMaterial, lod);
    }
    __except(EXCEPTION_EXECUTE_HANDLER) {
        static int eyeCrashCount = 0;
        if (++eyeCrashCount <= 5) {
            HWSKIN_DBG_ALWAYS("[HWSkin] EXCEPTION in DrawEyeball trampoline (occurrence #%d, code=0x%08X)\n",
                eyeCrashCount, GetExceptionCode());
        }
        // Safety net: ensure TexGen is OFF even on exception, so subsequent
        // draws (head, body, other models) don't get corrupted texcoords.
        RTX_SetEyeTexGenState(0);
        return 0;
    }

    // Safety net: the eye shader *should* reset TexGen after each pass, but
    // if anything goes wrong (early return, exception in shader code, state
    // cache mismatch), force it off here after every DrawEyeball call.
    RTX_SetEyeTexGenState(0);

    if (eyeCallCount <= 10 || (eyeCallCount % 2000) == 0) {
        HWSKIN_DBG_ALWAYS("[HWSkin] R_StudioDrawEyeball #%d: returned %d triangles (lighting: %d->%d)\n",
            eyeCallCount, result, lighting, actualLighting);
        if (result == 0) {
            HWSKIN_DBG_ALWAYS("[HWSkin]   -> 0 triangles! Likely causes: bEyes=false, GetFatVertexData=NULL, or mesh has no groups\n");
        }
    }

    return result;
}

// ============================================================================
// Helpers: enable/disable UBYTE4 patch around skinned model creation.
// Separated into their own functions because MSVC forbids C++ object unwinding
// (std::string temporaries) in functions that use SEH (__try/__except).
//
// The UBYTE4 patch is only active during CreateSingleMesh for multi-bone
// models, so only their vertex declarations get UBYTE4. All other models
// keep D3DCOLOR and render normally through Source Engine shaders.
static void EnableUBYTE4Patch() {
    g_MemoryPatcher.EnablePatch("HWSkin_BlendIndices_UBYTE4");
}
static void DisableUBYTE4Patch() {
    g_MemoryPatcher.DisablePatch("HWSkin_BlendIndices_UBYTE4");
}

// ============================================================================
// Hook: R_StudioCreateSingleMesh
// Forces MESHGROUP_IS_HWSKINNED flag on mesh groups during creation
// ============================================================================
Define_method_Hook(void*, R_StudioCreateSingleMesh, void*,
    int* pStudioHdr,
    __int64 pStudioLodData,
    int* pMesh,
    int* pVtxMesh,
    int numBones,
    __int64 pMeshData,
    unsigned short* pColorMeshID)
{
    // Log first call
    static bool s_firstCallLogged = false;
    if (!s_firstCallLogged) {
        HWSKIN_DBG_ALWAYS("[HWSkin] CreateSingleMesh hook HIT! numBones=%d\n", numBones);
        s_firstCallLogged = true;
    }
    
    // NOTE: CreateSingleMesh is only called when models are FIRST loaded.
    // Each sub-patch has its own convar (gated behind r_forcehwskin master switch).
    bool forcehwskin = GlobalConvars::r_forcehwskin && GlobalConvars::r_forcehwskin->GetBool();
    bool wantFlag   = forcehwskin && GlobalConvars::r_hwskin_force_flag && GlobalConvars::r_hwskin_force_flag->GetBool();
    bool wantUBYTE4 = forcehwskin && GlobalConvars::r_hwskin_ubyte4 && GlobalConvars::r_hwskin_ubyte4->GetBool();
    bool needUBYTE4 = (numBones > 1 && wantUBYTE4);
    
    // Enable UBYTE4 patch ONLY during mesh creation for skinned models.
    // This scopes the patch so only multi-bone models get UBYTE4 in their
    // vertex declarations. All other models keep D3DCOLOR and render normally.
    if (needUBYTE4) {
        EnableUBYTE4Patch();
    }
    
    // Call original to create the mesh (vertex declarations are created here)
    void* result = nullptr;
    __try {
        result = R_StudioCreateSingleMesh_trampoline()(_this, pStudioHdr, pStudioLodData, pMesh, pVtxMesh, numBones, pMeshData, pColorMeshID);
    }
    __except(EXCEPTION_EXECUTE_HANDLER) {
        HWSKIN_DBG_ALWAYS("[HWSkin] CRASH in CreateSingleMesh trampoline!\n");
        if (needUBYTE4) DisableUBYTE4Patch();
        return nullptr;
    }
    
    // Disable UBYTE4 patch after mesh creation
    if (needUBYTE4) {
        DisableUBYTE4Patch();
    }
    
    // Force MESHGROUP_IS_HWSKINNED flag on mesh groups with bones (only when enabled)
    if (numBones > 1 && wantFlag) {
        __try {
            if (!pMeshData) {
                HWSKIN_DBG("[HWSkin] CreateSingleMesh: pMeshData is null!\n");
                return result;
            }
            
            // studiomeshdata_t layout (x64):
            // +0x00: int m_NumGroup
            // +0x08: studiomeshgroup_t* m_pMeshGroup
            int numGroups = *(int*)pMeshData;
            studiomeshgroup_t* pGroups = *(studiomeshgroup_t**)(pMeshData + 8);
            
            HWSKIN_DBG("[HWSkin] CreateSingleMesh: numGroups=%d, pGroups=%p, numBones=%d\n", 
                numGroups, pGroups, numBones);
            
            if (!pGroups || numGroups <= 0 || numGroups > 100) {
                HWSKIN_DBG("[HWSkin] CreateSingleMesh: invalid groups (numGroups=%d, pGroups=%p)\n", 
                    numGroups, pGroups);
                return result;
            }
            
			int forcedCount = 0;
			for (int i = 0; i < numGroups; i++) {
				studiomeshgroup_t* pGroup = &pGroups[i];
				
				HWSKIN_DBG("[HWSkin] Group %d: flags=0x%X (flexed=%d, hwskinned=%d)\n", 
					i, pGroup->m_Flags,
					!!(pGroup->m_Flags & MESHGROUP_IS_FLEXED),
					!!(pGroup->m_Flags & MESHGROUP_IS_HWSKINNED));
				
				// Force hardware skinning flag on ALL groups including flexed ones.
				// Eye mesh groups are typically flexed (for eyelid animation) and were
				// previously skipped here, which prevented them from ever reaching the
				// R_StudioDrawGroupHWSkin hook for proper D3D9 bone transform output.
				if (!(pGroup->m_Flags & MESHGROUP_IS_HWSKINNED)) {
					pGroup->m_Flags |= MESHGROUP_IS_HWSKINNED;
					forcedCount++;
				}
			}
            
            if (forcedCount > 0) {
                HWSKIN_DBG("[HWSkin] Forced MESHGROUP_IS_HWSKINNED on %d/%d groups (numBones=%d)\n", 
                    forcedCount, numGroups, numBones);
            } else {
                HWSKIN_DBG("[HWSkin] No groups needed forcing (all already HW skinned or flexed)\n");
            }
        }
        __except(EXCEPTION_EXECUTE_HANDLER) {
            HWSKIN_DBG_ALWAYS("[HWSkin] EXCEPTION in CreateSingleMesh flag forcing!\n");
        }
    }
    
    return result;
}

// Hook: R_StudioDrawGroupHWSkin
// GMod version takes 9 parameters (different from Source SDK's 5)
// Sets bone transforms via D3D9 fixed-function API for Remix to capture
// ============================================================================
Define_method_Hook(__int64, R_StudioDrawGroupHWSkin, void*,
    void* pRenderContext,
    __int64 bodyPartInfo,
    studiomeshgroup_t* pGroup,
    int lighting,
    float r_blend,
    void* pMaterial,
    unsigned int flags,
    void* pColorMeshInfo)
{
    // Log first call
    static bool s_firstCallLogged = false;
    if (!s_firstCallLogged) {
        HWSKIN_DBG_ALWAYS("[HWSkin] DrawGroupHWSkin hook HIT! lighting=%d, r_blend=%.2f, pGroup=%p\n", lighting, r_blend, pGroup);
        s_firstCallLogged = true;
    }

    // Defensive: ensure TexGen is OFF before any non-eye body-part draw.
    // Eye rendering (DrawEyeball) sets D3DTSS_TCI_CAMERASPACEPOSITION for the
    // iris/glint passes and should reset it, but if the shader or DrawEyeball
    // hook didn't run the reset (crash, early-out, engine state cache), this
    // catches the leak before it corrupts head/body textures.
    if (g_pD3DDevice) {
        RTX_SetEyeTexGenState(0);
    }
    
    // Check if bone export is enabled and we have a D3D device
    bool forcehwskin = GlobalConvars::r_forcehwskin && GlobalConvars::r_forcehwskin->GetBool();
    bool shouldSetBones = g_bInRenderFinal && g_pD3DDevice && forcehwskin;
    bool forcedHwLighting = false; // Will be set to true for skinned meshes
    
    // Diagnostic logging (rate limited)
    static int totalCallCount = 0;
    totalCallCount++;
    if (forcehwskin && (totalCallCount % 500) == 1) {
        HWSKIN_DBG("[HWSkin] DrawGroupHWSkin #%d: g_bInRenderFinal=%d, g_pD3DDevice=%p, shouldSetBones=%d\n",
            totalCallCount, g_bInRenderFinal, g_pD3DDevice, shouldSetBones);
    }
    
    bool bonesSet = false;
    studiohdr_t* hookStudioHdr = nullptr;    // Captured for per-strip DIP hook
    matrix3x4_t* hookPoseToWorld = nullptr;  // Captured for per-strip DIP hook
    bool hookDoSetBones = false;             // Whether bones were actually set
    
    if (shouldSetBones) {
        __try {
            // CStudioRenderContext layout (x64) - found via IDA reverse engineering:
            // +0x108 (264): studiohdr_t* m_pStudioHdr
            // +0xE8 (232): matrix3x4_t* m_PoseToWorld (array of bone matrices)
            studiohdr_t* pStudioHdr = *(studiohdr_t**)((uintptr_t)_this + 264);
            matrix3x4_t* m_PoseToWorld = *(matrix3x4_t**)((uintptr_t)_this + 232);
            
            // Log what we found
            if ((totalCallCount % 500) == 1) {
                HWSKIN_DBG("[HWSkin] _this=%p, pStudioHdr=%p, m_PoseToWorld=%p\n",
                    _this, pStudioHdr, m_PoseToWorld);
                if (pStudioHdr) {
                    HWSKIN_DBG("[HWSkin] numbones=%d\n", pStudioHdr->numbones);
                }
            }
            
            if (pStudioHdr && m_PoseToWorld && pStudioHdr->numbones > 1 && pStudioHdr->numbones <= 512) {
                // Set bone transforms via D3D9 fixed-function API
                // Key: TRANSPOSE the matrices for D3D9 multi-bone format
                // If we have modelToWorld, apply full transform
                
                // r_hwskin_setbones ConVar controls whether we actually set D3D9 bone matrices
                // Set to 0 to disable bone setting, or a specific bone count to only set for that model
                // -1 or 0 = disabled, 1 = all models, >1 = only models with exactly that many bones
                int setBonesMode = GlobalConvars::r_hwskin_setbones ? GlobalConvars::r_hwskin_setbones->GetInt() : 0;
                
                bool doSetBones = false;
                if (setBonesMode == 1) {
                    // Mode 1: set bones for all models
                    doSetBones = true;
                } else if (setBonesMode > 1) {
                    // Mode N>1: only set bones for models with exactly N bones
                    doSetBones = (pStudioHdr->numbones == setBonesMode);
                }
                // Mode 0 or -1: don't set bones
                
                if (doSetBones) {
                    // Debug: Check what D3DTS_WORLD is currently set to
                    // Remix uses VIEW * WORLDMATRIX[i] for skinned, so WORLD is NOT used
                    // But Source might expect WORLD to contain model-to-world
                    static int worldMatrixLogCount = 0;
                    worldMatrixLogCount++;
                    if (worldMatrixLogCount <= 5 || (worldMatrixLogCount % 1000) == 0) {
                        D3DMATRIX currentWorld;
                        if (SUCCEEDED(g_pD3DDevice->GetTransform(D3DTS_WORLD, &currentWorld))) {
                            HWSKIN_DBG("[HWSkin] D3DTS_WORLD matrix:\n");
                            HWSKIN_DBG("  [%.3f, %.3f, %.3f, %.3f]\n", currentWorld._11, currentWorld._12, currentWorld._13, currentWorld._14);
                            HWSKIN_DBG("  [%.3f, %.3f, %.3f, %.3f]\n", currentWorld._21, currentWorld._22, currentWorld._23, currentWorld._24);
                            HWSKIN_DBG("  [%.3f, %.3f, %.3f, %.3f]\n", currentWorld._31, currentWorld._32, currentWorld._33, currentWorld._34);
                            HWSKIN_DBG("  [%.3f, %.3f, %.3f, %.3f]\n", currentWorld._41, currentWorld._42, currentWorld._43, currentWorld._44);
                        }
                    }
                    
                    SetD3D9BoneTransforms(g_pD3DDevice, pStudioHdr, m_PoseToWorld, pStudioHdr->numbones,
                                          g_modelToWorld, g_bHaveModelToWorld, pGroup);
                    
                    // Capture context for per-strip DIP hook
                    hookStudioHdr = pStudioHdr;
                    hookPoseToWorld = m_PoseToWorld;
                    hookDoSetBones = true;
                }
                bonesSet = true;
                
                // Store in g_BONEDATA for debugging/other systems
                g_BONEDATA.bone_count = pStudioHdr->numbones;
                g_BONEDATA.active = true;
                
                // Force hardware lighting for skinned meshes - this ensures the engine
                // uses the HW skinning code path rather than CPU skinning
                // LIGHTING_HARDWARE = 0
                forcedHwLighting = true;
                
                // Debug logging - always log first few, then rate-limit
                static int successCount = 0;
                successCount++;
                if (successCount <= 5 || (successCount % 500) == 1) {
                    HWSKIN_DBG("[HWSkin] SUCCESS #%d: Set %d bones via D3D9, lighting=%d->0 (forced HW), pGroup=%p\n", 
                        successCount, pStudioHdr->numbones, lighting, pGroup);
                    
                    // Log first bone matrix for verification
                    if (successCount <= 3) {
                        HWSKIN_DBG("[HWSkin] Bone0 matrix: [%.2f, %.2f, %.2f, %.2f]\n",
                            m_PoseToWorld[0][0][0], m_PoseToWorld[0][0][1], m_PoseToWorld[0][0][2], m_PoseToWorld[0][0][3]);
                    }
                }
            } else {
                // Log why we skipped
                if ((totalCallCount % 500) == 1) {
                    HWSKIN_DBG("[HWSkin] SKIPPED: pStudioHdr=%p, m_PoseToWorld=%p, numbones=%d\n",
                        pStudioHdr, m_PoseToWorld, pStudioHdr ? pStudioHdr->numbones : -1);
                }
            }
        }
        __except(EXCEPTION_EXECUTE_HANDLER) {
            HWSKIN_DBG_ALWAYS("[HWSkin] EXCEPTION reading bone data from _this!\n");
            g_BONEDATA.bone_count = -1;
            g_BONEDATA.active = false;
        }
    }
    
    // Determine actual lighting to use
    // Force LIGHTING_HARDWARE (0) for skinned meshes to ensure HW skinning path
    int actualLighting = (forcedHwLighting) ? 0 : lighting;
    
    // =========================================================================
    // Per-strip DIP hook activation
    //
    // For multi-strip mesh groups, activate the DIP hook to apply incremental
    // bone state changes between each strip's DrawIndexedPrimitive call.
    // This ensures every strip's vertices reference the correct bone matrices.
    // =========================================================================
    bool perStripActive = false;
    if (hookDoSetBones && g_OriginalDIP && pGroup && pGroup->m_NumStrips > 1) {
        g_PerStripState.active = true;
        g_PerStripState.currentStripIndex = 0;
        g_PerStripState.totalStrips = pGroup->m_NumStrips;
        g_PerStripState.pGroup = pGroup;
        g_PerStripState.cache = GetOrCreateBoneRemapCache(hookStudioHdr);
        g_PerStripState.pStudioHdr = hookStudioHdr;
        g_PerStripState.poseToWorld = hookPoseToWorld;
        g_PerStripState.numBones = hookStudioHdr->numbones;
        g_PerStripState.haveModelToWorld = g_bHaveModelToWorld;
        if (g_bHaveModelToWorld && hookStudioHdr->numbones > 1) {
            TinyMathLib_MatrixInvert(g_modelToWorld, g_PerStripState.worldToModel);
        }
        perStripActive = true;
    }
    
    // Call original draw function
    __int64 result = 0;
    __try {
        result = R_StudioDrawGroupHWSkin_trampoline()(_this, pRenderContext, bodyPartInfo, pGroup, actualLighting, r_blend, pMaterial, flags, pColorMeshInfo);
    }
    __except(EXCEPTION_EXECUTE_HANDLER) {
        HWSKIN_DBG_ALWAYS("[HWSkin] CRASH in DrawGroupHWSkin trampoline!\n");
    }
    
    // Deactivate per-strip DIP hook
    if (perStripActive) {
        g_PerStripState.active = false;
    }
    
    // =========================================================================
    // Deferred Phase 2: Update cache with strip 2+'s bone state changes
    //
    // Now that the draw is complete and the DIP hook has handled per-strip
    // bone updates during rendering, process strip 2+'s bone state changes
    // into the cache. This ensures future mesh groups (which are incremental
    // from the last strip of this group) have the correct base state.
    // =========================================================================
    if (hookDoSetBones && pGroup && pGroup->m_pStripData && pGroup->m_NumStrips > 1) {
        ModelBoneRemapCache* cache = GetOrCreateBoneRemapCache(hookStudioHdr);
        int numBones = hookStudioHdr->numbones;
        if (numBones > MAX_D3D9_BONES) numBones = MAX_D3D9_BONES;
        
        for (int s = 1; s < pGroup->m_NumStrips; ++s) {
            OptimizedModel::StripHeader_t* pStrip = &pGroup->m_pStripData[s];
            for (int k = 0; k < pStrip->numBoneStateChanges; ++k) {
                OptimizedModel::BoneStateChangeHeader_t* pStateChange = pStrip->pBoneStateChange(k);
                if (pStateChange->newBoneID < 0)
                    break;
                
                int hwID = pStateChange->hardwareID;
                int boneID = pStateChange->newBoneID;
                
                if (hwID < 0 || hwID >= MAX_D3D9_BONES || boneID >= numBones)
                    continue;
                
                cache->remapTable[hwID] = boneID;
                
                if (hwID + 1 > cache->maxHwID) {
                    cache->maxHwID = hwID + 1;
                }
            }
        }
    }
    
    // Reset D3D9 bone state after drawing
    if (bonesSet) {
        ResetD3D9BoneState(g_pD3DDevice);
        g_BONEDATA.active = false;
    }
    
    return result;
}

// ============================================================================
// Hook: R_StudioRenderFinal
// GMod version takes 9 parameters (different from Source SDK's 11)
// Sets up frame state for bone capture and captures modelToWorld transform
// ============================================================================
Define_method_Hook(__int64, R_StudioRenderFinal, void*,
    void* pRenderContext,
    int skin,
    __int64 pBodyPartInfo,
    void* pClientEntity,
    void* ppMaterials,
    int pMaterialFlags,
    unsigned int boneMask,
    void* pColorMeshes)
{
    // ALWAYS log first call to confirm hook is working
    static bool s_firstCallLogged = false;
    if (!s_firstCallLogged) {
        HWSKIN_DBG_ALWAYS("[HWSkin] RenderFinal hook HIT! skin=%d, boneMask=0x%X, pClientEntity=%p\n", 
            skin, boneMask, pClientEntity);
        s_firstCallLogged = true;
    }
    
    // Set flag to indicate we're in the render final path
    g_bInRenderFinal = true;
    g_bHaveModelToWorld = false;
    
    // Capture modelToWorld from pClientEntity using direct memory offset
    // NOTE: Vtable-based IClientRenderable::RenderableToWorldTransform() crashes in GMod 64-bit
    // due to vtable layout differences. Instead, we use direct memory offset.
    // 
    // In 32-bit Source: offset 1228 (0x4CC) for m_rgflCoordinateFrame in C_BaseEntity
    // In 64-bit GMod: Offset 1016 (0x3F8) found via IDA datamap analysis
    //   - Datamap at 0x180881538 shows: string "m_rgflCoordinateFrame", offset 0x3F8, size 12 floats
    //
    g_bHaveModelToWorld = false;
    
    if (pClientEntity) {
        __try {
            const int ICLIENTRENDERABLE_OFFSET_X64 = 8;
            uintptr_t pBaseEntity = (uintptr_t)pClientEntity - ICLIENTRENDERABLE_OFFSET_X64;
            
            // m_rgflCoordinateFrame offset from C_BaseEntity
            const int OFFSET_COORDINATE_FRAME_X64 = 1016; // 0x3F8 from IDA datamap
            
            matrix3x4_t* pCoordinateFrame = (matrix3x4_t*)(pBaseEntity + OFFSET_COORDINATE_FRAME_X64);
            
            // Sanity check: rotation values should be -1 to 1, translation should be reasonable
            float checkVal = (*pCoordinateFrame)[0][0];
            float checkTrans = (*pCoordinateFrame)[0][3]; // X translation
            
            // Check both rotation (should be -1 to 1) and translation (should be reasonable world coords)
            bool rotationValid = (checkVal >= -1.1f && checkVal <= 1.1f);
            bool translationValid = (checkTrans > -100000.0f && checkTrans < 100000.0f);
            
            if (rotationValid && translationValid) {
                memcpy(&g_modelToWorld, pCoordinateFrame, sizeof(matrix3x4_t));
                g_bHaveModelToWorld = true;
                
                static bool s_firstLogged = false;
                if (!s_firstLogged) {
                    HWSKIN_DBG("[HWSkin] Got modelToWorld from pBaseEntity (pClientEntity-%d)+%d:\n",
                        ICLIENTRENDERABLE_OFFSET_X64, OFFSET_COORDINATE_FRAME_X64);
                    HWSKIN_DBG("[HWSkin]   Row0: [%.3f, %.3f, %.3f, %.3f]\n",
                        g_modelToWorld[0][0], g_modelToWorld[0][1], g_modelToWorld[0][2], g_modelToWorld[0][3]);
                    HWSKIN_DBG("[HWSkin]   Row1: [%.3f, %.3f, %.3f, %.3f]\n",
                        g_modelToWorld[1][0], g_modelToWorld[1][1], g_modelToWorld[1][2], g_modelToWorld[1][3]);
                    HWSKIN_DBG("[HWSkin]   Row2: [%.3f, %.3f, %.3f, %.3f]\n",
                        g_modelToWorld[2][0], g_modelToWorld[2][1], g_modelToWorld[2][2], g_modelToWorld[2][3]);
                    s_firstLogged = true;
                }
            } else {
                static bool s_badOffsetLogged = false;
                if (!s_badOffsetLogged) {
                    HWSKIN_DBG("[HWSkin] Bad offset? pBaseEntity+%d: rot[0][0]=%.2f, trans[0][3]=%.2f\n",
                        OFFSET_COORDINATE_FRAME_X64, checkVal, checkTrans);
                    s_badOffsetLogged = true;
                }
            }
        }
        __except(EXCEPTION_EXECUTE_HANDLER) {
            static bool s_exceptionLogged = false;
            if (!s_exceptionLogged) {
                HWSKIN_DBG_ALWAYS("[HWSkin] Exception reading m_rgflCoordinateFrame from pClientEntity!\n");
                s_exceptionLogged = true;
            }
        }
    }
    
    __int64 result = 0;
    __try {
        result = R_StudioRenderFinal_trampoline()(_this, pRenderContext, skin, pBodyPartInfo, 
            pClientEntity, ppMaterials, pMaterialFlags, boneMask, pColorMeshes);
    }
    __except(EXCEPTION_EXECUTE_HANDLER) {
        static int crashCount = 0;
        if (++crashCount <= 5) {
            HWSKIN_DBG_ALWAYS("[HWSkin] Exception in RenderFinal trampoline (occurrence #%d)\n", crashCount);
        }
    }
    
    g_bInRenderFinal = false;
    g_bHaveModelToWorld = false;
    
    return result;
}

// ============================================================================
// Initialization and Shutdown
// ============================================================================
void HardwareSkinningHooks::Initialize() {
    if (m_bInitialized) {
        Msg("[Hardware Skinning] Already initialized\n");
            return;
        }

    try {
        Msg("[Hardware Skinning] Initializing...\n");

        // Get D3D9 device for setting bone transforms
        g_pD3DDevice = static_cast<IDirect3DDevice9*>(FindD3D9Device());
        if (!g_pD3DDevice) {
            Warning("[Hardware Skinning] Failed to get D3D9 device - bone transforms will not be set\n");
            // Continue anyway - the hooks can still work, just without D3D9 bone transform output
        } else {
            Msg("[Hardware Skinning] D3D9 device acquired for bone transform output\n");
            
            // ================================================================
            // Install DrawIndexedPrimitive vtable hook for per-strip bone updates
            // ================================================================
            if (InstallDIPVtableHook(g_pD3DDevice)) {
                Msg("[Hardware Skinning] Installed DrawIndexedPrimitive vtable hook for per-strip bones\n");
            } else {
                Warning("[Hardware Skinning] Failed to install DIP vtable hook - per-strip bones disabled\n");
            }
        }

        // ====================================================================
        // Patch shaderapidx9.dll: D3DDECLTYPE_D3DCOLOR -> D3DDECLTYPE_UBYTE4
        // for bone indices in ComputeVertexSpec.
        //
        // D3DCOLOR uses BGRA byte ordering which swizzles bone indices
        // (e.g., [0,1,2,3] becomes [2,1,0,3]). RTX Remix reads them raw,
        // so every vertex gets mapped to the wrong bones. UBYTE4 reads
        // the 4 bytes as-is without swizzle.
        //
        // The patch is created here (to capture original bytes) but kept
        // DISABLED. It is only enabled briefly during CreateSingleMesh for
        // multi-bone models, so only their vertex declarations get UBYTE4.
        // All other models keep D3DCOLOR and render normally.
        // ====================================================================
#ifdef _WIN64
        HMODULE shaderApiDll = GetModuleHandleA("shaderapidx9.dll");
        if (shaderApiDll) {
            static const char blendIndicesTypeSig[] = "66 C7 44 C7 05 00 02 88 54 C7 07 C6 44 C7 04 04";
            void* sigAddr = g_MemoryPatcher.FindPatternWildcard(shaderApiDll, blendIndicesTypeSig);
            if (sigAddr) {
                void* patchAddr = (void*)((uintptr_t)sigAddr + 15);
                if (g_MemoryPatcher.CreatePatch("HWSkin_BlendIndices_UBYTE4", patchAddr, "05",
                    "Patch D3DDECLTYPE_D3DCOLOR to D3DDECLTYPE_UBYTE4 for bone indices")) {
                    // Always disable immediately -- the patch is toggled on/off
                    // around CreateSingleMesh calls for skinned models only
                    g_MemoryPatcher.DisablePatch("HWSkin_BlendIndices_UBYTE4");
                    Msg("[Hardware Skinning] UBYTE4 patch registered (enabled per-model during mesh creation)\n");
                } else {
                    Warning("[Hardware Skinning] Failed to register UBYTE4 patch!\n");
                }
            } else {
                Warning("[Hardware Skinning] Could not find BLENDINDICES D3DCOLOR signature in shaderapidx9.dll\n");
            }
        } else {
            Warning("[Hardware Skinning] shaderapidx9.dll not found - UBYTE4 patch skipped\n");
        }
#endif

        // Get studiorender.dll handle
        HMODULE studiorenderdll = GetModuleHandleA("studiorender.dll");
        if (!studiorenderdll) {
            Warning("[Hardware Skinning] studiorender.dll not found\n");
            return;
        }
        Msg("[Hardware Skinning] Found studiorender.dll at %p\n", studiorenderdll);

        // ====================================================================
        // 64-bit signatures from IDA analysis
        // Format: hex string with spaces (ScanSign format)
        // NOTE: Using exact bytes without wildcards for reliability
        // ====================================================================
#ifdef _WIN64
        // R_StudioCreateSingleMesh at offset 0x64300
        static const char CreateSingleMesh_sign[] = "4C 8B DC 4D 89 4B 20 4D 89 43 18 49 89 4B 08 53 55 56 57 41 56 41 57";
        
        // R_StudioDrawGroupHWSkin at offset 0x11F50
        // Unique register save pattern: push rbp, rbx, rsi, rdi, r12, r13, r14, r15 + lea rbp
        static const char DrawGroupHWSkin_sign[] = "40 55 53 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 A8 FD FF FF";
        
        // R_StudioRenderFinal at offset 0x11E40
        // Spill r9 and rdx to shadow space, push rsi/rdi/r15, sub rsp,0A0h, xor r15d
        static const char RenderFinal_sign[] = "4C 89 4C 24 20 48 89 54 24 10 56 57 41 57 48 81 EC A0 00 00 00 45 33 FF 48 8B F2 48 8B F9";
#else
        // 32-bit signatures (kept for reference)
        static const char CreateSingleMesh_sign[] = "55 8B EC 81 EC 00 02 00 00";
        static const char DrawGroupHWSkin_sign[] = "55 8B EC 83 EC 0C 53 8B 5D 00 56";
        static const char RenderFinal_sign[] = "55 8B EC 83 EC 10 53 57";
#endif

        // Minimum valid offset from module base (skip PE header)
        const uintptr_t MIN_VALID_OFFSET = 0x1000;
        uintptr_t moduleBase = (uintptr_t)studiorenderdll;

        // Scan for function addresses
        auto CreateSingleMesh_addr = ScanSign(studiorenderdll, CreateSingleMesh_sign, sizeof(CreateSingleMesh_sign) - 1);
        uintptr_t CreateSingleMesh_offset = CreateSingleMesh_addr ? ((uintptr_t)CreateSingleMesh_addr - moduleBase) : 0;
        Msg("[Hardware Skinning] R_StudioCreateSingleMesh: %p (offset: 0x%llX)\n", CreateSingleMesh_addr, CreateSingleMesh_offset);
        
        auto DrawGroupHWSkin_addr = ScanSign(studiorenderdll, DrawGroupHWSkin_sign, sizeof(DrawGroupHWSkin_sign) - 1);
        uintptr_t DrawGroupHWSkin_offset = DrawGroupHWSkin_addr ? ((uintptr_t)DrawGroupHWSkin_addr - moduleBase) : 0;
        Msg("[Hardware Skinning] R_StudioDrawGroupHWSkin: %p (offset: 0x%llX)\n", DrawGroupHWSkin_addr, DrawGroupHWSkin_offset);
        
        auto RenderFinal_addr = ScanSign(studiorenderdll, RenderFinal_sign, sizeof(RenderFinal_sign) - 1);
        uintptr_t RenderFinal_offset = RenderFinal_addr ? ((uintptr_t)RenderFinal_addr - moduleBase) : 0;
        Msg("[Hardware Skinning] R_StudioRenderFinal: %p (offset: 0x%llX)\n", RenderFinal_addr, RenderFinal_offset);

        // R_StudioDrawEyeball: CStudioRender::R_StudioDrawEyeball handles special eye mesh rendering.
        // Prologue: push rbp/rbx/rsi/rdi/r12-r15, lea rbp,[rsp-3A8h], sub rsp,4A8h
        // Unique stack frame size (0x3A8/0x4A8) distinguishes it from R_StudioDrawGroupHWSkin (0x258/0x538).
#ifdef _WIN64
        static const char DrawEyeball_sign[] = "40 55 53 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 58 FC FF FF 48 81 EC A8 04 00 00";
#else
        static const char DrawEyeball_sign[] = "";
#endif
        void* DrawEyeball_addr = nullptr;
        if (sizeof(DrawEyeball_sign) > 1) {
            DrawEyeball_addr = ScanSign(studiorenderdll, DrawEyeball_sign, sizeof(DrawEyeball_sign) - 1);
        }
        uintptr_t DrawEyeball_offset = DrawEyeball_addr ? ((uintptr_t)DrawEyeball_addr - moduleBase) : 0;
        Msg("[Hardware Skinning] R_StudioDrawEyeball: %p (offset: 0x%llX)\n", DrawEyeball_addr, DrawEyeball_offset);
        
        // Validate addresses are in code section (not PE header)
        if (CreateSingleMesh_addr && CreateSingleMesh_offset < MIN_VALID_OFFSET) {
            Warning("[Hardware Skinning] R_StudioCreateSingleMesh matched PE header - invalid!\n");
            CreateSingleMesh_addr = nullptr;
        }
        if (DrawGroupHWSkin_addr && DrawGroupHWSkin_offset < MIN_VALID_OFFSET) {
            Warning("[Hardware Skinning] R_StudioDrawGroupHWSkin matched PE header - invalid!\n");
            DrawGroupHWSkin_addr = nullptr;
        }
        if (RenderFinal_addr && RenderFinal_offset < MIN_VALID_OFFSET) {
            Warning("[Hardware Skinning] R_StudioRenderFinal matched PE header - invalid!\n");
            RenderFinal_addr = nullptr;
        }
        if (DrawEyeball_addr && DrawEyeball_offset < MIN_VALID_OFFSET) {
            Warning("[Hardware Skinning] R_StudioDrawEyeball matched PE header - invalid!\n");
            DrawEyeball_addr = nullptr;
        }

        // Initialize model-to-world as identity
        memset(&g_modelToWorld, 0, sizeof(g_modelToWorld));
        g_modelToWorld[0][0] = 1.0f;
        g_modelToWorld[1][1] = 1.0f;
        g_modelToWorld[2][2] = 1.0f;

        // Validate signatures were found
        bool allFound = true;
        
        if (!CreateSingleMesh_addr) {
            Warning("[Hardware Skinning] R_StudioCreateSingleMesh not found!\n");
            allFound = false;
        }
        if (!DrawGroupHWSkin_addr) {
            Warning("[Hardware Skinning] R_StudioDrawGroupHWSkin not found!\n");
            allFound = false;
        }
        if (!RenderFinal_addr) {
            Warning("[Hardware Skinning] R_StudioRenderFinal not found!\n");
            allFound = false;
        }
        
        if (!allFound) {
            Warning("[Hardware Skinning] Some signatures not found, hooks disabled\n");
            return;
        }

        // Store original function pointers
        R_StudioCreateSingleMesh_Original = (F_StudioCreateSingleMesh)CreateSingleMesh_addr;
        R_StudioDrawGroupHWSkin_Original = (F_StudioDrawGroupHWSkin)DrawGroupHWSkin_addr;
        R_StudioRenderFinal_Original = (F_StudioRenderFinal)RenderFinal_addr;
        R_StudioDrawEyeball_Original = DrawEyeball_addr ? (F_StudioDrawEyeball)DrawEyeball_addr : nullptr;

        // Set up all hooks with corrected GMod signatures (9 params for DrawGroupHWSkin/RenderFinal)
        Setup_Hook(R_StudioCreateSingleMesh, CreateSingleMesh_addr);
        Setup_Hook(R_StudioDrawGroupHWSkin, DrawGroupHWSkin_addr);
        Setup_Hook(R_StudioRenderFinal, RenderFinal_addr);

        if (DrawEyeball_addr) {
            Setup_Hook(R_StudioDrawEyeball, DrawEyeball_addr);
            Msg("[Hardware Skinning] R_StudioDrawEyeball hook installed - eye rendering diagnostics + HW lighting force active\n");
        } else {
            Warning("[Hardware Skinning] R_StudioDrawEyeball not found - eye hook disabled. Eyes may not render in RTX Remix.\n");
            Warning("[Hardware Skinning]   To fix: find R_StudioDrawEyeball signature in IDA (search for \"$glint\" string ref)\n");
        }

        m_bInitialized = true;
        m_bEnabled = true;
        
        Msg("[Hardware Skinning] Successfully initialized - bone transforms via D3D9 fixed-function API\n");
    }
    catch (...) {
        Warning("[Hardware Skinning] Exception during initialization\n");
    }
}

void HardwareSkinningHooks::Shutdown() {
    if (!m_bInitialized) {
        return;
    }
    
    try {
        Msg("[Hardware Skinning] Shutting down...\n");

        // Restore DrawIndexedPrimitive vtable hook
        if (g_OriginalDIP && g_pD3DDevice) {
            RestoreDIPVtableHook(g_pD3DDevice);
            Msg("[Hardware Skinning] Restored original DrawIndexedPrimitive vtable entry\n");
            g_OriginalDIP = nullptr;
        }
        g_PerStripState.active = false;

        // Restore shaderapidx9.dll UBYTE4 patch
        if (g_MemoryPatcher.DoesPatchExist("HWSkin_BlendIndices_UBYTE4")) {
            g_MemoryPatcher.DisablePatch("HWSkin_BlendIndices_UBYTE4");
            Msg("[Hardware Skinning] Restored original bone index declaration type\n");
        }

        // Destroy all hooks (not just Disable). Destroy() calls MH_RemoveHook +
        // MH_Uninitialize, which fully purges them from MinHook's list so that
        // the next Initialize() can call Create() cleanly whether or not the
        // DLL was actually reloaded by Windows between map changes.
        // NOTE: Setup_Hook uses the file-scope globals created by Define_method_Hook
        // (R_StudioXxx_hook), NOT the m_StudioXxx_hook class members. Calling
        // Disable/Destroy on the class members is a no-op since they are never
        // enabled; we must use the correct globals here.
        R_StudioCreateSingleMesh_hook.Destroy();
        R_StudioDrawGroupHWSkin_hook.Destroy();
        R_StudioRenderFinal_hook.Destroy();
        if (R_StudioDrawEyeball_Original) {
            R_StudioDrawEyeball_hook.Destroy();
        }

        // Reset global state
        g_BONEDATA.bone_count = -1;
        g_BONEDATA.active = false;
        g_bInRenderFinal = false;
        g_pD3DDevice = nullptr;
        g_MaxBonesEverSet = 0;
        g_NumActiveCaches = 0;
        m_bEnabled = false;
        m_bInitialized = false;

        Msg("[Hardware Skinning] Shutdown complete\n");
    }
    catch (...) {
        Warning("[Hardware Skinning] Exception during shutdown\n");
    }
}

#endif // HWSKIN_PATCHES && _WIN64
