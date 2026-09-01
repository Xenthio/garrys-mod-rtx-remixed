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
#include <cmath>

// Debug logging - controlled at runtime via r_hwskin_debug convar
static bool HardwareSkinningDebugEnabled() {
    return GlobalConvars::r_hwskin_debug &&
        GlobalConvars::r_hwskin_debug->GetBool();
}

static char HWSkinAsciiLower(char value) {
    return (value >= 'A' && value <= 'Z')
        ? static_cast<char>(value - 'A' + 'a')
        : value;
}

static bool HWSkinTokenMatchesModel(
    const char* modelName,
    const char* tokenBegin,
    const char* tokenEnd)
{
    while (tokenBegin < tokenEnd &&
           static_cast<unsigned char>(*tokenBegin) <= ' ') {
        tokenBegin++;
    }
    while (tokenEnd > tokenBegin &&
           static_cast<unsigned char>(tokenEnd[-1]) <= ' ') {
        tokenEnd--;
    }

    const size_t tokenLength = static_cast<size_t>(tokenEnd - tokenBegin);
    if (tokenLength == 0) {
        return false;
    }
    if (tokenLength == 1 && tokenBegin[0] == '*') {
        return true;
    }

    for (const char* candidate = modelName; *candidate; candidate++) {
        size_t offset = 0;
        while (offset < tokenLength && candidate[offset] &&
               HWSkinAsciiLower(candidate[offset]) ==
                   HWSkinAsciiLower(tokenBegin[offset])) {
            offset++;
        }
        if (offset == tokenLength) {
            return true;
        }
    }
    return false;
}

// Detailed traces are intentionally stricter than the ordinary debug stream:
// both r_hwskin_debug and a matching comma-separated model filter are required.
// An empty filter therefore cannot flood condebug during ordinary play.
static bool CopyHardwareSkinningTraceModelName(
    const studiohdr_t* pStudioHdr,
    char* modelName,
    size_t modelNameCapacity)
{
    if (!pStudioHdr || !modelName || modelNameCapacity == 0) {
        return false;
    }

    modelName[0] = '\0';
    const size_t bytesToCopy =
        modelNameCapacity - 1 < sizeof(pStudioHdr->name)
            ? modelNameCapacity - 1
            : sizeof(pStudioHdr->name);
    SIZE_T bytesRead = 0;
    const void* sourceName = reinterpret_cast<const void*>(
        reinterpret_cast<uintptr_t>(pStudioHdr) +
        offsetof(studiohdr_t, name));
    if (!ReadProcessMemory(
            GetCurrentProcess(), sourceName, modelName,
            bytesToCopy, &bytesRead) ||
        bytesRead != bytesToCopy) {
        modelName[0] = '\0';
        return false;
    }

    modelName[bytesToCopy] = '\0';
    return modelName[0] != '\0';
}

static bool HardwareSkinningTraceMatchesModel(
    const studiohdr_t* pStudioHdr,
    char* modelName,
    size_t modelNameCapacity)
{
    if (!HardwareSkinningDebugEnabled() || !pStudioHdr ||
        !GlobalConvars::r_hwskin_debug_filter) {
        return false;
    }

    const char* filter = GlobalConvars::r_hwskin_debug_filter->GetString();
    if (!filter || !filter[0] ||
        !CopyHardwareSkinningTraceModelName(
            pStudioHdr, modelName, modelNameCapacity)) {
        return false;
    }

    const char* tokenBegin = filter;
    for (const char* cursor = filter;; cursor++) {
        if (*cursor == ',' || *cursor == '\0') {
            if (HWSkinTokenMatchesModel(modelName, tokenBegin, cursor)) {
                return true;
            }
            if (*cursor == '\0') {
                break;
            }
            tokenBegin = cursor + 1;
        }
    }
    return false;
}

static void HWSkinDebugPrint(const char* format, ...) {
    // Check convar at runtime
    if (!HardwareSkinningDebugEnabled()) {
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
#define HWSKIN_ERROR(...) HWSkinDebugPrintAlways(__VA_ARGS__)

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

// Per-draw skinning state: populated before calling the original
// R_StudioDrawGroupHWSkin and consumed by the DIP hook. In addition to applying
// incremental changes for multi-strip meshes, the hook restores hardware bone
// slot 0 at the final draw boundary. D3DTS_WORLDMATRIX(0) aliases D3DTS_WORLD,
// which Source updates while preparing the draw after our initial bone upload.
struct PerStripDrawState {
    bool active;                    // true = we're inside a skinned draw
    int currentStripIndex;          // which DIP call we're on (0-based)
    int totalStrips;                // total strips in this mesh group
    studiomeshgroup_t* pGroup;      // current mesh group
    ModelBoneRemapCache* cache;     // current model's bone remap cache
    studiohdr_t* pStudioHdr;
    matrix3x4_t* poseToWorld;
    int numBones;
    matrix3x4_t worldToModel;
    bool haveModelToWorld;
    bool skeletonGlobalBoneIDs;     // dynamic SW-VTX flex vertices use skeleton IDs directly
    bool rigidFlexOutput;           // dynamic eye format has no blend-weight stream
    int slotZeroBoneOverride;       // SW VTX flex groups rigidly bind to this bone
};
static PerStripDrawState g_PerStripState = {};

// R_StudioProcessFlexedMesh is called from inside R_StudioDrawDynamicMesh and
// does not receive the runtime mesh group. Keep the active group scoped to the
// dynamic draw so the flex-builder hook can distinguish SW VTX groups that we
// promoted from ordinary Source DX9 flex groups.
static thread_local studiomeshgroup_t* g_pCurrentDynamicFlexGroup = nullptr;

// The fixed-function Eyes_dx9 shader cannot consume Source's static
// delta-flex stream. Keep a scoped marker around forced hardware eye draws so
// the existing hardware draw detour can redirect only their outer static calls
// through the dynamic flex builder that already powers hardware-skinned faces.
static thread_local int g_forcedEyeDrawDepth = 0;

bool HardwareSkinning_IsForcedEyeDraw() {
    return g_forcedEyeDrawDepth > 0;
}

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

// Preserve the optimized SW VTX skin selection for every flex vertex. The VTX
// can intentionally discard or reorder influences from the raw VVD (G-Man's
// delta-flex face does this heavily), so copying the VVD's complete influence
// list is not equivalent to the mesh Source actually renders.
struct FlexVertexSkinSelection {
    unsigned char boneWeightIndex[MAX_NUM_BONES_PER_VERT];
    unsigned char numBones;
    unsigned char boneID[MAX_NUM_BONES_PER_VERT];
};

struct FlexGroupBoneBinding {
    studiomeshgroup_t* pGroup;
    int slotZeroBoneID;
    FlexVertexSkinSelection* pVertexSkin;
    int numVertices;
    bool hardwareBoneIDs;
    bool rigidSlotZero;
    unsigned char state; // 0 = empty, 1 = active, 2 = tombstone
};

static const int MAX_FLEX_GROUP_BONE_BINDINGS = 8192;
static FlexGroupBoneBinding g_FlexGroupBoneBindings[MAX_FLEX_GROUP_BONE_BINDINGS] = {};

static int GetFlexGroupBoneBindingHash(studiomeshgroup_t* pGroup) {
    return static_cast<int>(
        (reinterpret_cast<uintptr_t>(pGroup) >> 4) &
        (MAX_FLEX_GROUP_BONE_BINDINGS - 1));
}

static void FreeFlexGroupVertexSkin(FlexGroupBoneBinding& binding) {
    if (binding.pVertexSkin) {
        HeapFree(GetProcessHeap(), 0, binding.pVertexSkin);
        binding.pVertexSkin = nullptr;
    }
    binding.numVertices = 0;
}

static void ClearFlexGroupBoneBinding(studiomeshgroup_t* pGroup) {
    if (!pGroup) {
        return;
    }

    const int start = GetFlexGroupBoneBindingHash(pGroup);
    for (int probe = 0; probe < MAX_FLEX_GROUP_BONE_BINDINGS; probe++) {
        FlexGroupBoneBinding& binding =
            g_FlexGroupBoneBindings[(start + probe) &
                                    (MAX_FLEX_GROUP_BONE_BINDINGS - 1)];
        if (binding.state == 0) {
            return;
        }
        if (binding.state == 1 && binding.pGroup == pGroup) {
            FreeFlexGroupVertexSkin(binding);
            binding.state = 2;
            binding.pGroup = nullptr;
            binding.slotZeroBoneID = -1;
            binding.hardwareBoneIDs = false;
            binding.rigidSlotZero = false;
            return;
        }
    }
}

static void SetFlexGroupBoneBinding(
    studiomeshgroup_t* pGroup,
    int slotZeroBoneID,
    FlexVertexSkinSelection* pVertexSkin,
    int numVertices,
    bool hardwareBoneIDs,
    bool rigidSlotZero = false,
    bool markerOnly = false)
{
    if (!pGroup || slotZeroBoneID < 0 ||
        slotZeroBoneID >= MAX_D3D9_BONES ||
        (!rigidSlotZero && !markerOnly &&
         (!pVertexSkin || numVertices <= 0))) {
        if (pVertexSkin) {
            HeapFree(GetProcessHeap(), 0, pVertexSkin);
        }
        return;
    }

    const int start = GetFlexGroupBoneBindingHash(pGroup);
    int firstTombstone = -1;
    for (int probe = 0; probe < MAX_FLEX_GROUP_BONE_BINDINGS; probe++) {
        const int slot =
            (start + probe) & (MAX_FLEX_GROUP_BONE_BINDINGS - 1);
        FlexGroupBoneBinding& binding = g_FlexGroupBoneBindings[slot];
        if (binding.state == 1 && binding.pGroup == pGroup) {
            FreeFlexGroupVertexSkin(binding);
            binding.slotZeroBoneID = slotZeroBoneID;
            binding.pVertexSkin = pVertexSkin;
            binding.numVertices = numVertices;
            binding.hardwareBoneIDs = hardwareBoneIDs;
            binding.rigidSlotZero = rigidSlotZero;
            return;
        }
        if (binding.state == 2 && firstTombstone < 0) {
            firstTombstone = slot;
        }
        if (binding.state == 0) {
            const int target = (firstTombstone >= 0) ? firstTombstone : slot;
            g_FlexGroupBoneBindings[target].pGroup = pGroup;
            g_FlexGroupBoneBindings[target].slotZeroBoneID = slotZeroBoneID;
            g_FlexGroupBoneBindings[target].pVertexSkin = pVertexSkin;
            g_FlexGroupBoneBindings[target].numVertices = numVertices;
            g_FlexGroupBoneBindings[target].hardwareBoneIDs =
                hardwareBoneIDs;
            g_FlexGroupBoneBindings[target].rigidSlotZero =
                rigidSlotZero;
            g_FlexGroupBoneBindings[target].state = 1;
            return;
        }
    }

    if (firstTombstone >= 0) {
        g_FlexGroupBoneBindings[firstTombstone].pGroup = pGroup;
        g_FlexGroupBoneBindings[firstTombstone].slotZeroBoneID = slotZeroBoneID;
        g_FlexGroupBoneBindings[firstTombstone].pVertexSkin = pVertexSkin;
        g_FlexGroupBoneBindings[firstTombstone].numVertices = numVertices;
        g_FlexGroupBoneBindings[firstTombstone].hardwareBoneIDs =
            hardwareBoneIDs;
        g_FlexGroupBoneBindings[firstTombstone].rigidSlotZero =
            rigidSlotZero;
        g_FlexGroupBoneBindings[firstTombstone].state = 1;
        return;
    }

    HeapFree(GetProcessHeap(), 0, pVertexSkin);
    HWSKIN_ERROR("[HWSkin] SW flex-group bone-binding table is full\n");
}

static FlexGroupBoneBinding* FindFlexGroupBoneBindingRecord(
    studiomeshgroup_t* pGroup)
{
    if (!pGroup) {
        return nullptr;
    }

    const int start = GetFlexGroupBoneBindingHash(pGroup);
    for (int probe = 0; probe < MAX_FLEX_GROUP_BONE_BINDINGS; probe++) {
        FlexGroupBoneBinding& binding =
            g_FlexGroupBoneBindings[(start + probe) &
                                    (MAX_FLEX_GROUP_BONE_BINDINGS - 1)];
        if (binding.state == 0) {
            return nullptr;
        }
        if (binding.state == 1 && binding.pGroup == pGroup) {
            return &binding;
        }
    }

    return nullptr;
}

static int FindFlexGroupBoneBinding(studiomeshgroup_t* pGroup) {
    FlexGroupBoneBinding* binding =
        FindFlexGroupBoneBindingRecord(pGroup);
    if (binding &&
        (binding->rigidSlotZero || !binding->hardwareBoneIDs)) {
        return binding->slotZeroBoneID;
    }
    return -1;
}

static bool FlexGroupUsesSkeletonGlobalBoneIDs(studiomeshgroup_t* pGroup) {
    FlexGroupBoneBinding* binding =
        FindFlexGroupBoneBindingRecord(pGroup);
    return binding && !binding->rigidSlotZero &&
           !binding->hardwareBoneIDs;
}

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

// Source's hardware facial-flex builder receives CMeshBuilder by reference
// (a pointer in the x64 ABI). The stock implementation writes every generated
// vertex as weight 1.0 / hardware bone slot 0, losing the original neck/head
// blends. GetFatVertexData is the internal helper used by that same function
// to resolve the active VVD vertex stream.
typedef void* (__fastcall* F_GetFatVertexData)(
    mstudiomesh_t* pMesh,
    studiohdr_t* pStudioHdr);
static F_GetFatVertexData GetFatVertexData_Original = nullptr;

// Function pointers for trampolines
static F_StudioCreateSingleMesh R_StudioCreateSingleMesh_Original = nullptr;
static F_StudioDrawGroupHWSkin R_StudioDrawGroupHWSkin_Original = nullptr;
static F_StudioRenderFinal R_StudioRenderFinal_Original = nullptr;
static F_StudioDrawEyeball R_StudioDrawEyeball_Original = nullptr;
static HWSkinDrawDynamicMeshFn g_RStudioDrawDynamicMeshOriginal = nullptr;
static thread_local int g_forcedEyeDynamicRedirectDepth = 0;

void HardwareSkinning_SetDynamicMeshOriginal(
    HWSkinDrawDynamicMeshFn original)
{
    g_RStudioDrawDynamicMeshOriginal = original;
}

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
    const bool skeletonGlobalBoneIDs =
        FlexGroupUsesSkeletonGlobalBoneIDs(pGroup);
    
    if (!skeletonGlobalBoneIDs && pGroup && pGroup->m_pStripData &&
        pGroup->m_NumStrips > 0 && numBones > 1) {
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

    // Source's dynamically generated flex vertices all reference hardware slot
    // 0. For SW VTX compatibility that slot must point at the flex group's
    // dominant attachment bone (normally the head), not skeleton root bone 0.
    const int flexSlotZeroBoneID = FindFlexGroupBoneBinding(pGroup);
    if (!skeletonGlobalBoneIDs && flexSlotZeroBoneID >= 0 &&
        flexSlotZeroBoneID < numBones) {
        matrix3x4_t finalMatrix;
        ComputeFinalBoneMatrix(flexSlotZeroBoneID, pStudioHdr, m_PoseToWorld,
                               numBones, worldToModel, haveModelToWorld,
                               finalMatrix);

        D3DMATRIX d3dMatrix;
        ConvertBoneToD3DMatrix(finalMatrix, d3dMatrix);
        pDevice->SetTransform(D3DTS_WORLDMATRIX(0), &d3dMatrix);
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
// Intercepts draw calls inside R_StudioDrawGroupHWSkin.
// For multi-strip mesh groups, the engine draws strip 1, then strip 2, etc.
// Each strip may remap some hardware bone IDs to different skeleton bones.
// This hook applies the incremental bone state changes between strips so
// each strip's vertices reference the correct bone matrices.
//
// It also restores hardware slot 0 immediately before every skinned DIP.
// D3DTS_WORLDMATRIX(0) and D3DTS_WORLD are the same D3D9 transform state, so
// Source's ordinary world-matrix setup otherwise replaces bone 0 with identity
// after SetD3D9BoneTransforms. Vertices blended with bone 0 then stretch toward
// the world origin.
//
// Non-skinned draws take only the g_PerStripState.active fast-path branch.
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
    // Fast path: if we're not in a skinned draw, pass through immediately.
    if (g_PerStripState.active) {
        int stripIdx = g_PerStripState.currentStripIndex;

        // Some EyeRefract models rebuild their delta-flexed eye into a dynamic
        // vertex format with no blend-weight elements. They still need the
        // flexed positions, but D3D9 must submit them as a rigid draw attached
        // to the eye/head bone rather than as indexed vertices rooted at bone
        // zero. Reassert this at the final DIP boundary after Source's state
        // setup has run.
        if (g_PerStripState.rigidFlexOutput) {
            pDevice->SetRenderState(
                D3DRS_INDEXEDVERTEXBLENDENABLE, FALSE);
            pDevice->SetRenderState(
                D3DRS_VERTEXBLEND, D3DVBF_DISABLE);
        }

        // Promoted software-VTX flex meshes retain global skeleton bone IDs in
        // their dynamic vertex stream. Source still replays the strip's legacy
        // hardware-local bone-state changes immediately before Draw(), which
        // overwrites those global slots. Restore the complete current skeleton
        // palette at the final D3D9 boundary so the face and body use the same
        // pose in motion as well as at rest.
        if (g_PerStripState.skeletonGlobalBoneIDs) {
            const int boneCount =
                g_PerStripState.numBones < MAX_D3D9_BONES
                    ? g_PerStripState.numBones
                    : MAX_D3D9_BONES;
            for (int boneID = 0; boneID < boneCount; boneID++) {
                matrix3x4_t finalMatrix;
                ComputeFinalBoneMatrix(
                    boneID, g_PerStripState.pStudioHdr,
                    g_PerStripState.poseToWorld, g_PerStripState.numBones,
                    g_PerStripState.worldToModel,
                    g_PerStripState.haveModelToWorld, finalMatrix);

                D3DMATRIX d3dMatrix;
                ConvertBoneToD3DMatrix(finalMatrix, d3dMatrix);
                pDevice->SetTransform(
                    D3DTS_WORLDMATRIX(boneID), &d3dMatrix);
            }
        }
        
        // For strip 2+ (index > 0), apply that strip's incremental bone state changes
        if (!g_PerStripState.skeletonGlobalBoneIDs &&
            stripIdx > 0 && stripIdx < g_PerStripState.totalStrips) {
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

                // Keep the accumulated mapping current so slot 0 below uses
                // this strip's mapping rather than the previous strip's.
                if (g_PerStripState.cache) {
                    g_PerStripState.cache->remapTable[hwID] = boneID;
                    if (hwID + 1 > g_PerStripState.cache->maxHwID) {
                        g_PerStripState.cache->maxHwID = hwID + 1;
                    }
                }
            }
            
            // Rate-limited logging for per-strip bone updates
            static int perStripLogCount = 0;
            if (perStripLogCount++ < 5 || (perStripLogCount % 5000) == 0) {
                HWSKIN_DBG("[HWSkin] DIP hook: applied strip %d/%d bone changes (%d state changes)\n",
                    stripIdx + 1, g_PerStripState.totalStrips, pStrip->numBoneStateChanges);
            }
        }

        // Restore hardware bone slot 0 after Source has committed its ordinary
        // world transform for this DIP. SW VTX data uses global skeleton IDs,
        // while legacy HW VTX data can map hardware slot 0 through the model's
        // accumulated bone-state table.
        if (g_PerStripState.rigidFlexOutput ||
            !g_PerStripState.skeletonGlobalBoneIDs) {
            int slotZeroBoneID = g_PerStripState.slotZeroBoneOverride;
            if (!g_PerStripState.skeletonGlobalBoneIDs &&
                slotZeroBoneID < 0 &&
                g_PerStripState.cache &&
                g_PerStripState.cache->maxHwID > 0 &&
                g_PerStripState.cache->remapTable[0] >= 0) {
                slotZeroBoneID = g_PerStripState.cache->remapTable[0];
            }
            if (slotZeroBoneID < 0) {
                slotZeroBoneID = 0;
            }

            if (slotZeroBoneID >= 0 &&
                slotZeroBoneID < g_PerStripState.numBones) {
                matrix3x4_t finalMatrix;
                ComputeFinalBoneMatrix(
                    slotZeroBoneID, g_PerStripState.pStudioHdr,
                    g_PerStripState.poseToWorld, g_PerStripState.numBones,
                    g_PerStripState.worldToModel,
                    g_PerStripState.haveModelToWorld, finalMatrix);

                D3DMATRIX d3dMatrix;
                ConvertBoneToD3DMatrix(finalMatrix, d3dMatrix);
                pDevice->SetTransform(
                    D3DTS_WORLDMATRIX(0), &d3dMatrix);
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
// set/reset D3D9 texture-generation state for the combined sclera/iris pass.
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

    // Stage 0 is the sclera and must retain the model's regular UV0 stream.
    // Stage 1 is the projected iris. The previous implementation forced both
    // stages through camera-space TexGen, which produced the same clipped iris
    // boundary in raster mode regardless of the skinning path.
    g_pD3DDevice->SetTextureStageState(
        0, D3DTSS_TEXCOORDINDEX, 0);
    g_pD3DDevice->SetTextureStageState(
        0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
    g_pD3DDevice->SetSamplerState(
        0, D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP);
    g_pD3DDevice->SetSamplerState(
        0, D3DSAMP_ADDRESSV, D3DTADDRESS_WRAP);

    if (enable) {
        g_pD3DDevice->SetTextureStageState(
            1, D3DTSS_TEXCOORDINDEX,
            D3DTSS_TCI_CAMERASPACEPOSITION);
        g_pD3DDevice->SetTextureStageState(
            1, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_COUNT2);
        // CLAMP prevents the projected iris from tiling outside [0,1].
        g_pD3DDevice->SetSamplerState(
            1, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
        g_pD3DDevice->SetSamplerState(
            1, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    } else {
        g_pD3DDevice->SetTextureStageState(
            1, D3DTSS_TEXCOORDINDEX, 1);
        g_pD3DDevice->SetTextureStageState(
            1, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
        g_pD3DDevice->SetSamplerState(
            1, D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP);
        g_pD3DDevice->SetSamplerState(
            1, D3DSAMP_ADDRESSV, D3DTADDRESS_WRAP);
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

    // Log the first draw of each concrete model/eye pair while diagnostics are
    // enabled.  The old global "first ten calls" sample was normally consumed
    // by menu/player-model eyes before a mounted TF2 model was spawned, which
    // made a missing EyeRefract draw indistinguishable from a shader failure.
    // Pointer identity is sufficient here: studio headers and materials stay
    // stable for the lifetime of a loaded model, and this is diagnostics only.
    struct LoggedEyeDraw {
        studiohdr_t* studioHdr;
        IMaterial* material;
        int meshID;
    };
    static LoggedEyeDraw loggedEyeDraws[256] = {};
    static int loggedEyeDrawCount = 0;

    studiohdr_t* studioHdr = nullptr;
    mstudiomesh_t* studioMesh = reinterpret_cast<mstudiomesh_t*>(pmesh);
    IMaterial* material = reinterpret_cast<IMaterial*>(pMaterial);
    bool logThisEye = false;
    int groupCount = 0;
    unsigned int groupFlags = 0;

    if (HardwareSkinningDebugEnabled()) {
        __try {
            studioHdr = *reinterpret_cast<studiohdr_t**>(
                reinterpret_cast<uintptr_t>(_this) + 0x108);

            bool alreadyLogged = false;
            for (int i = 0; i < loggedEyeDrawCount; i++) {
                if (loggedEyeDraws[i].studioHdr == studioHdr &&
                    loggedEyeDraws[i].material == material &&
                    loggedEyeDraws[i].meshID ==
                        (studioMesh ? studioMesh->meshid : -1)) {
                    alreadyLogged = true;
                    break;
                }
            }

            if (!alreadyLogged && loggedEyeDrawCount < 256) {
                loggedEyeDraws[loggedEyeDrawCount++] = {
                    studioHdr,
                    material,
                    studioMesh ? studioMesh->meshid : -1
                };
                logThisEye = true;

                if (pMeshData) {
                    groupCount = *reinterpret_cast<int*>(pMeshData);
                    studiomeshgroup_t* groups =
                        *reinterpret_cast<studiomeshgroup_t**>(
                            reinterpret_cast<uintptr_t>(pMeshData) + 8);
                    if (!groups || groupCount < 0 || groupCount > 100) {
                        groupCount = 0;
                    } else {
                        for (int i = 0; i < groupCount; i++) {
                            groupFlags |= groups[i].m_Flags;
                        }
                    }
                }

                HWSKIN_DBG(
                    "[HWSkin] Eye draw route: model=%s material=%p "
                    "mesh=%d eye=%d verts=%d flexes=%d "
                    "groups=%d flags=0x%X lighting=%d forceHW=%d\n",
                    studioHdr ? studioHdr->name : "unknown",
                    material,
                    studioMesh ? studioMesh->meshid : -1,
                    studioMesh ? studioMesh->materialparam : -1,
                    studioMesh ? studioMesh->numvertices : -1,
                    studioMesh ? studioMesh->numflexes : -1,
                    groupCount, groupFlags, lighting,
                    (GlobalConvars::r_eyes_hwskin &&
                     GlobalConvars::r_eyes_hwskin->GetBool() &&
                     GlobalConvars::r_forcehwskin &&
                     GlobalConvars::r_forcehwskin->GetBool() &&
                     g_bInRenderFinal));
            }
        }
        __except(EXCEPTION_EXECUTE_HANDLER) {
            HWSKIN_ERROR(
                "[HWSkin] Exception while collecting eye draw diagnostics\n");
            studioHdr = nullptr;
            studioMesh = nullptr;
            material = nullptr;
            logThisEye = false;
        }
    }

    bool forceEyeHW = GlobalConvars::r_eyes_hwskin && GlobalConvars::r_eyes_hwskin->GetBool();
    bool forcehwskin = GlobalConvars::r_forcehwskin && GlobalConvars::r_forcehwskin->GetBool();
    bool shouldForce = forceEyeHW && forcehwskin && g_bInRenderFinal;

    if (eyeCallCount <= 10 || (eyeCallCount % 2000) == 0) {
        HWSKIN_DBG("[HWSkin] R_StudioDrawEyeball #%d: lighting=%d (%s), pMaterial=%p, lod=%d, forceHW=%d\n",
            eyeCallCount, lighting,
            (lighting == 0 ? "HARDWARE" : (lighting == 1 ? "SOFTWARE" : "MOUTH")),
            pMaterial, lod, shouldForce);
    }

    int actualLighting = lighting;
    if (shouldForce) {
        actualLighting = 0; // LIGHTING_HARDWARE
    }

    int result = 0;
    const int previousForcedEyeDrawDepth = g_forcedEyeDrawDepth;
    if (shouldForce) {
        g_forcedEyeDrawDepth++;
    }
    __try {
        result = R_StudioDrawEyeball_trampoline()(_this, pRenderContext, pmesh, pMeshData, actualLighting, pMaterial, lod);
    }
    __except(EXCEPTION_EXECUTE_HANDLER) {
        g_forcedEyeDrawDepth = previousForcedEyeDrawDepth;
        static int eyeCrashCount = 0;
        if (++eyeCrashCount <= 5) {
            HWSKIN_ERROR("[HWSkin] EXCEPTION in DrawEyeball trampoline (occurrence #%d, code=0x%08X)\n",
                eyeCrashCount, GetExceptionCode());
        }
        // Safety net: ensure TexGen is OFF even on exception, so subsequent
        // draws (head, body, other models) don't get corrupted texcoords.
        RTX_SetEyeTexGenState(0);
        return 0;
    }
    g_forcedEyeDrawDepth = previousForcedEyeDrawDepth;

    // Safety net: the eye shader *should* reset TexGen after each pass, but
    // if anything goes wrong (early return, exception in shader code, state
    // cache mismatch), force it off here after every DrawEyeball call.
    RTX_SetEyeTexGenState(0);

    if (eyeCallCount <= 10 || (eyeCallCount % 2000) == 0) {
        HWSKIN_DBG("[HWSkin] R_StudioDrawEyeball #%d: returned %d triangles (lighting: %d->%d)\n",
            eyeCallCount, result, lighting, actualLighting);
        if (result == 0) {
            HWSKIN_DBG("[HWSkin]   -> 0 triangles! Likely causes: bEyes=false, GetFatVertexData=NULL, or mesh has no groups\n");
        }
    }

    if (logThisEye) {
        HWSKIN_DBG(
            "[HWSkin] Eye draw result: model=%s material=%p mesh=%d "
            "triangles=%d lighting=%d->%d\n",
            studioHdr ? studioHdr->name : "unknown",
            material,
            studioMesh ? studioMesh->meshid : -1,
            result, lighting, actualLighting);
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
    g_MemoryPatcher.SetVerbose(GlobalConvars::r_hwskin_debug && GlobalConvars::r_hwskin_debug->GetBool());
    g_MemoryPatcher.EnablePatch("HWSkin_BlendIndices_UBYTE4");
}
static void DisableUBYTE4Patch() {
    g_MemoryPatcher.SetVerbose(GlobalConvars::r_hwskin_debug && GlobalConvars::r_hwskin_debug->GetBool());
    g_MemoryPatcher.DisablePatch("HWSkin_BlendIndices_UBYTE4");
}

// Software VTX files use the topology/order captured by Half-Life 2 RTX, but
// their strip groups are marked for CPU skinning. Temporarily promote those
// groups before Source builds the static mesh. When no matching SW companion
// exists, the loader supplies DX90 data here instead; its delta-only facial
// groups also need a temporary compatibility flag before mesh creation.
struct StripGroupFlagBackup {
    OptimizedModel::StripGroupHeader_t* pStripGroup;
    unsigned char flags;
    int slotZeroBoneID;
    bool forcedDynamicFlex;
};

static const int MAX_VTX_STRIP_GROUPS = 32;

static OptimizedModel::StripGroupHeader_t* GetVtxStripGroup(
    OptimizedModel::MeshHeader_t* pMeshHeader,
    const studiohdr_t* pStudioHdr,
    int groupIndex)
{
    if (!pMeshHeader || !pStudioHdr || groupIndex < 0 ||
        groupIndex >= pMeshHeader->numStripGroups ||
        pMeshHeader->stripGroupHeaderOffset <= 0) {
        return nullptr;
    }

    // GMod's MDL v49+ VTX group header has an eight-byte extension that is
    // absent from the public SDK structure.
    const int stripGroupHeaderSize =
        (pStudioHdr->version >= 49) ? 0x21 : 0x19;
    return reinterpret_cast<OptimizedModel::StripGroupHeader_t*>(
        reinterpret_cast<unsigned char*>(pMeshHeader) +
        pMeshHeader->stripGroupHeaderOffset +
        groupIndex * stripGroupHeaderSize);
}

static unsigned long long HWSkinTraceHashByte(
    unsigned long long hash,
    unsigned char value)
{
    return (hash ^ value) * 1099511628211ULL;
}

static void TraceVtxMeshForHardwareSkinning(
    studiohdr_t* pStudioHdr,
    mstudiomesh_t* pStudioMesh,
    int* pVtxMesh,
    int requestedBoneCount,
    bool useSoftwareVtx,
    bool forceHardwareSkinning,
    bool wantFlag,
    bool wantUbyte4,
    const StripGroupFlagBackup* backups,
    int backupCount)
{
    char modelName[sizeof(pStudioHdr->name) + 1] = {};
    if (!HardwareSkinningTraceMatchesModel(
            pStudioHdr, modelName, sizeof(modelName))) {
        return;
    }

    __try {
        OptimizedModel::MeshHeader_t* pMeshHeader =
            reinterpret_cast<OptimizedModel::MeshHeader_t*>(pVtxMesh);
        const int meshID = pStudioMesh ? pStudioMesh->meshid : -1;
        const int materialType = pStudioMesh ? pStudioMesh->materialtype : -1;
        const int groupCount = pMeshHeader ? pMeshHeader->numStripGroups : 0;

        HWSKIN_DBG(
            "[HWSkin-Trace] model_begin model=\"%s\" checksum=0x%08X "
            "mdlVersion=%d headerBones=%d requestedBones=%d mesh=%d "
            "materialType=%d vtxSource=%s force=%d wantFlag=%d "
            "wantUbyte4=%d groups=%d\n",
            modelName,
            static_cast<unsigned int>(pStudioHdr->checksum),
            pStudioHdr->version, pStudioHdr->numbones,
            requestedBoneCount, meshID, materialType,
            useSoftwareVtx ? "software" : "hardware",
            forceHardwareSkinning, wantFlag, wantUbyte4, groupCount);

        if (!pMeshHeader || groupCount <= 0 ||
            groupCount > MAX_VTX_STRIP_GROUPS) {
            HWSKIN_DBG(
                "[HWSkin-Trace] model_error model=\"%s\" mesh=%d "
                "reason=invalid_vtx_groups groups=%d\n",
                modelName, meshID, groupCount);
            return;
        }

        for (int groupIndex = 0; groupIndex < groupCount; groupIndex++) {
            OptimizedModel::StripGroupHeader_t* pGroup =
                GetVtxStripGroup(pMeshHeader, pStudioHdr, groupIndex);
            if (!pGroup) {
                continue;
            }

            const unsigned int flagsBefore =
                groupIndex < backupCount
                    ? backups[groupIndex].flags
                    : pGroup->flags;
            const unsigned int flagsAfter = pGroup->flags;
            unsigned long long blendIndexHash = 14695981039346656037ULL;
            bool usedBoneIDs[MAX_D3D9_BONES] = {};
            int usedBoneCount = 0;
            int minBoneID = MAX_D3D9_BONES;
            int maxBoneID = -1;

            if (pGroup->numVerts > 0 && pGroup->numVerts <= 1000000 &&
                pGroup->vertOffset > 0) {
                for (int vertexIndex = 0;
                     vertexIndex < pGroup->numVerts;
                     vertexIndex++) {
                    const OptimizedModel::Vertex_t* pVertex =
                        pGroup->pVertex(vertexIndex);
                    int influenceCount = pVertex->numBones;
                    if (influenceCount > MAX_NUM_BONES_PER_VERT) {
                        influenceCount = MAX_NUM_BONES_PER_VERT;
                    }
                    blendIndexHash = HWSkinTraceHashByte(
                        blendIndexHash,
                        static_cast<unsigned char>(influenceCount));
                    for (int influence = 0;
                         influence < influenceCount;
                         influence++) {
                        const unsigned char rawBoneID =
                            static_cast<unsigned char>(
                                pVertex->boneID[influence]);
                        blendIndexHash = HWSkinTraceHashByte(
                            blendIndexHash, rawBoneID);
                        const int boneID =
                            static_cast<signed char>(rawBoneID);
                        if (boneID >= 0 && boneID < MAX_D3D9_BONES &&
                            !usedBoneIDs[boneID]) {
                            usedBoneIDs[boneID] = true;
                            usedBoneCount++;
                            if (boneID < minBoneID) minBoneID = boneID;
                            if (boneID > maxBoneID) maxBoneID = boneID;
                        }
                    }
                }
            }

            HWSKIN_DBG(
                "[HWSkin-Trace] vtx_group model=\"%s\" mesh=%d group=%d "
                "flagsBefore=0x%02X flagsAfter=0x%02X promoted=%d "
                "vertices=%d indices=%d strips=%d boneIndexDomain=%s "
                "blendIndexHash=0x%016llX usedBoneCount=%d "
                "usedBoneRange=%d..%d\n",
                modelName, meshID, groupIndex,
                flagsBefore, flagsAfter,
                !(flagsBefore & STRIPGROUP_IS_HWSKINNED) &&
                    !!(flagsAfter & STRIPGROUP_IS_HWSKINNED),
                pGroup->numVerts, pGroup->numIndices,
                pGroup->numStrips,
                (flagsBefore & STRIPGROUP_IS_HWSKINNED)
                    ? "hardware-local"
                    : "skeleton-global",
                blendIndexHash, usedBoneCount,
                usedBoneCount ? minBoneID : -1,
                usedBoneCount ? maxBoneID : -1);

            for (int boneID = 0; boneID < MAX_D3D9_BONES; boneID++) {
                if (usedBoneIDs[boneID]) {
                    HWSKIN_DBG(
                        "[HWSkin-Trace] bone_index model=\"%s\" mesh=%d "
                        "group=%d id=%d\n",
                        modelName, meshID, groupIndex, boneID);
                }
            }

            if (pGroup->numStrips <= 0 || pGroup->numStrips > 4096 ||
                pGroup->stripOffset <= 0) {
                continue;
            }
            for (int stripIndex = 0;
                 stripIndex < pGroup->numStrips;
                 stripIndex++) {
                OptimizedModel::StripHeader_t* pStrip =
                    pGroup->pStrip(stripIndex);
                HWSKIN_DBG(
                    "[HWSkin-Trace] strip model=\"%s\" mesh=%d group=%d "
                    "strip=%d flags=0x%02X indices=%d indexOffset=%d "
                    "vertices=%d vertexOffset=%d bones=%d "
                    "stateChanges=%d\n",
                    modelName, meshID, groupIndex, stripIndex,
                    static_cast<unsigned int>(pStrip->flags),
                    pStrip->numIndices, pStrip->indexOffset,
                    pStrip->numVerts, pStrip->vertOffset,
                    pStrip->numBones, pStrip->numBoneStateChanges);

                if (pStrip->numBoneStateChanges < 0 ||
                    pStrip->numBoneStateChanges > 1024 ||
                    pStrip->boneStateChangeOffset <= 0) {
                    continue;
                }
                for (int changeIndex = 0;
                     changeIndex < pStrip->numBoneStateChanges;
                     changeIndex++) {
                    const OptimizedModel::BoneStateChangeHeader_t* pChange =
                        pStrip->pBoneStateChange(changeIndex);
                    HWSKIN_DBG(
                        "[HWSkin-Trace] palette_change model=\"%s\" mesh=%d "
                        "group=%d strip=%d change=%d hardwareID=%d "
                        "newBoneID=%d\n",
                        modelName, meshID, groupIndex,
                        stripIndex, changeIndex, pChange->hardwareID,
                        pChange->newBoneID);
                }
            }
        }
    }
    __except(EXCEPTION_EXECUTE_HANDLER) {
        HWSKIN_DBG(
            "[HWSkin-Trace] model_error model=\"%s\" "
            "reason=exception_while_reading_vtx\n",
            modelName[0] ? modelName : "<unreadable>");
    }
}

static void TraceRuntimeGroupsForHardwareSkinning(
    studiohdr_t* pStudioHdr,
    mstudiomesh_t* pStudioMesh,
    __int64 pMeshData)
{
    char modelName[sizeof(pStudioHdr->name) + 1] = {};
    if (!HardwareSkinningTraceMatchesModel(
            pStudioHdr, modelName, sizeof(modelName)) ||
        !pMeshData) {
        return;
    }

    __try {
        const int numGroups = *reinterpret_cast<int*>(pMeshData);
        studiomeshgroup_t* pGroups =
            *reinterpret_cast<studiomeshgroup_t**>(pMeshData + 8);
        const int meshID = pStudioMesh ? pStudioMesh->meshid : -1;
        if (!pGroups || numGroups <= 0 || numGroups > 100) {
            HWSKIN_DBG(
                "[HWSkin-Trace] model_error model=\"%s\" mesh=%d "
                "reason=invalid_runtime_groups groups=%d\n",
                modelName, meshID, numGroups);
            return;
        }

        for (int groupIndex = 0; groupIndex < numGroups; groupIndex++) {
            studiomeshgroup_t* pGroup = &pGroups[groupIndex];
            FlexGroupBoneBinding* pBinding =
                FindFlexGroupBoneBindingRecord(pGroup);
            const char* bindingMode = "strip-palette";
            int bindingBone = -1;
            int cachedVertices = 0;
            if (pBinding) {
                bindingBone = pBinding->slotZeroBoneID;
                cachedVertices = pBinding->numVertices;
                if (pBinding->rigidSlotZero) {
                    bindingMode = "rigid-slot-zero";
                } else if (pBinding->hardwareBoneIDs) {
                    bindingMode = "hardware-local";
                } else {
                    bindingMode = "skeleton-global";
                }
            }

            HWSKIN_DBG(
                "[HWSkin-Trace] runtime_group model=\"%s\" mesh=%d "
                "group=%d flags=0x%X vertices=%d strips=%d "
                "bindingMode=%s bindingBone=%d cachedSkinVertices=%d\n",
                modelName, meshID, groupIndex,
                pGroup->m_Flags, pGroup->m_NumVertices,
                pGroup->m_NumStrips, bindingMode, bindingBone,
                cachedVertices);
        }
        HWSKIN_DBG(
            "[HWSkin-Trace] model_end model=\"%s\" mesh=%d groups=%d\n",
            modelName, meshID, numGroups);
    }
    __except(EXCEPTION_EXECUTE_HANDLER) {
        HWSKIN_DBG(
            "[HWSkin-Trace] model_error model=\"%s\" "
            "reason=exception_while_reading_runtime_groups\n",
            modelName[0] ? modelName : "<unreadable>");
    }
}

struct HardwareSkinDrawTraceRecord {
    studiohdr_t* pStudioHdr;
    studiomeshgroup_t* pGroup;
    OptimizedModel::StripHeader_t* pStripData;
    int checksum;
    unsigned char phase;
};

static const int MAX_HWSKIN_DRAW_TRACE_RECORDS = 8192;
static HardwareSkinDrawTraceRecord
    g_HardwareSkinDrawTraceRecords[MAX_HWSKIN_DRAW_TRACE_RECORDS] = {};
static int g_HardwareSkinDrawTraceWriteIndex = 0;
static int g_HardwareSkinDrawTraceRecordCount = 0;

static bool ClaimHardwareSkinDrawTrace(
    studiohdr_t* pStudioHdr,
    studiomeshgroup_t* pGroup,
    OptimizedModel::StripHeader_t* pStripData,
    int checksum,
    unsigned char phase)
{
    if (!pStudioHdr || !pGroup) {
        return false;
    }

    for (int i = 0; i < g_HardwareSkinDrawTraceRecordCount; i++) {
        const HardwareSkinDrawTraceRecord& record =
            g_HardwareSkinDrawTraceRecords[i];
        if (record.pStudioHdr == pStudioHdr &&
            record.pGroup == pGroup &&
            record.pStripData == pStripData &&
            record.checksum == checksum &&
            record.phase == phase) {
            return false;
        }
    }

    int recordIndex = 0;
    if (g_HardwareSkinDrawTraceRecordCount <
        MAX_HWSKIN_DRAW_TRACE_RECORDS) {
        recordIndex = g_HardwareSkinDrawTraceRecordCount++;
    } else {
        recordIndex = g_HardwareSkinDrawTraceWriteIndex++ %
            MAX_HWSKIN_DRAW_TRACE_RECORDS;
    }
    HardwareSkinDrawTraceRecord& record =
        g_HardwareSkinDrawTraceRecords[recordIndex];
    record.pStudioHdr = pStudioHdr;
    record.pGroup = pGroup;
    record.pStripData = pStripData;
    record.checksum = checksum;
    record.phase = phase;
    return true;
}

static void TraceHardwareSkinningDraw(
    studiohdr_t* pStudioHdr,
    studiomeshgroup_t* pGroup,
    const char* phase,
    unsigned char phaseID,
    int setBonesMode,
    bool bonesUploaded,
    bool perStripActive)
{
    char modelName[sizeof(pStudioHdr->name) + 1] = {};
    if (!HardwareSkinningTraceMatchesModel(
            pStudioHdr, modelName, sizeof(modelName)) ||
        !pGroup) {
        return;
    }

    __try {
        const int checksum = pStudioHdr->checksum;
        OptimizedModel::StripHeader_t* pStripData =
            pGroup->m_pStripData;
        if (!ClaimHardwareSkinDrawTrace(
                pStudioHdr, pGroup, pStripData,
                checksum, phaseID)) {
            return;
        }

        FlexGroupBoneBinding* pBinding =
            FindFlexGroupBoneBindingRecord(pGroup);
        const char* bindingMode = "strip-palette";
        int bindingBone = -1;
        int cachedSkinVertices = 0;
        bool skeletonGlobalBoneIDs = false;
        bool rigidSlotZero = false;
        if (pBinding) {
            bindingBone = pBinding->slotZeroBoneID;
            cachedSkinVertices = pBinding->numVertices;
            skeletonGlobalBoneIDs = !pBinding->hardwareBoneIDs;
            rigidSlotZero = pBinding->rigidSlotZero;
            if (pBinding->rigidSlotZero) {
                bindingMode = "rigid-slot-zero";
            } else if (pBinding->hardwareBoneIDs) {
                bindingMode = "hardware-local";
            } else {
                bindingMode = "skeleton-global";
            }
        }

        HWSKIN_DBG(
            "[HWSkin-Trace] draw_binding model=\"%s\" checksum=0x%08X "
            "phase=%s groupPtr=%p stripData=%p flags=0x%X vertices=%d "
            "strips=%d setBonesMode=%d bonesUploaded=%d perStripActive=%d "
            "bindingMode=%s bindingBone=%d cachedSkinVertices=%d "
            "skeletonGlobalBoneIDs=%d rigidSlotZero=%d modelToWorld=%d\n",
            modelName,
            static_cast<unsigned int>(checksum), phase,
            pGroup, pStripData, pGroup->m_Flags,
            pGroup->m_NumVertices, pGroup->m_NumStrips,
            setBonesMode, bonesUploaded, perStripActive,
            bindingMode, bindingBone, cachedSkinVertices,
            skeletonGlobalBoneIDs, rigidSlotZero,
            g_bHaveModelToWorld);

        if (!pGroup->m_pStripData || pGroup->m_NumStrips <= 0 ||
            pGroup->m_NumStrips > 4096) {
            return;
        }
        for (int stripIndex = 0;
             stripIndex < pGroup->m_NumStrips;
             stripIndex++) {
            const OptimizedModel::StripHeader_t* pStrip =
                &pGroup->m_pStripData[stripIndex];
            HWSKIN_DBG(
                "[HWSkin-Trace] draw_strip model=\"%s\" phase=%s "
                "groupPtr=%p strip=%d flags=0x%02X vertices=%d "
                "indices=%d bones=%d stateChanges=%d\n",
                modelName, phase, pGroup, stripIndex,
                static_cast<unsigned int>(pStrip->flags),
                pStrip->numVerts, pStrip->numIndices,
                pStrip->numBones, pStrip->numBoneStateChanges);
        }
    }
    __except(EXCEPTION_EXECUTE_HANDLER) {
        HWSKIN_DBG(
            "[HWSkin-Trace] draw_error model=\"%s\" phase=%s "
            "reason=exception_while_reading_draw_binding\n",
            modelName[0] ? modelName : "<unreadable>", phase);
    }
}

static int PromoteVtxStripGroupsForHardwareSkinning(
    int* pVtxMesh,
    studiohdr_t* pStudioHdr,
    mstudiomesh_t* pStudioMesh,
    bool promoteHardwareSkinning,
    StripGroupFlagBackup* backups,
    int backupCapacity)
{
    __try {
        if (!pVtxMesh || !pStudioHdr || !backups || backupCapacity <= 0) {
            return 0;
        }

        OptimizedModel::MeshHeader_t* pMeshHeader =
            reinterpret_cast<OptimizedModel::MeshHeader_t*>(pVtxMesh);
        const int numStripGroups = pMeshHeader->numStripGroups;
        if (numStripGroups <= 0 || numStripGroups > backupCapacity ||
            pMeshHeader->stripGroupHeaderOffset <= 0) {
            HWSKIN_ERROR(
                "[HWSkin] Invalid VTX mesh header (groups=%d, offset=%d)\n",
                numStripGroups, pMeshHeader->stripGroupHeaderOffset);
            return 0;
        }

        int promotedCount = 0;
        int forcedDynamicFlexCount = 0;
        // GMod extended StripGroupHeader_t by eight bytes for MDL v49+.
        // The fields used here retain their original offsets, but pointer
        // arithmetic through the SDK's 25-byte type would land on the wrong
        // group after group zero.
        const int stripGroupHeaderSize =
            (pStudioHdr->version >= 49) ? 0x21 : 0x19;
        unsigned char* pStripGroupBase =
            reinterpret_cast<unsigned char*>(pMeshHeader) +
            pMeshHeader->stripGroupHeaderOffset;

        for (int i = 0; i < numStripGroups; i++) {
            OptimizedModel::StripGroupHeader_t* pStripGroup =
                reinterpret_cast<OptimizedModel::StripGroupHeader_t*>(
                    pStripGroupBase + i * stripGroupHeaderSize);
            backups[i].pStripGroup = pStripGroup;
            backups[i].flags = pStripGroup->flags;
            backups[i].slotZeroBoneID = -1;
            backups[i].forcedDynamicFlex = false;

            // GMod's v49 StudioMDL can omit the legacy FLEXED bit when it
            // emits only DX90 optimized data. Those groups retain the newer
            // DELTA_FLEXED bit, which is normally consumed by a vertex shader,
            // but Remix's fixed-function hardware-skinning path never applies
            // that shader morph. A matching SW VTX already carries FLEXED, so
            // the delta-only combination identifies the DX90 fallback that
            // must be rebuilt by Source's dynamic facial-flex path instead.
            const bool forceDynamicFlex =
                pStudioMesh && pStudioMesh->numflexes > 0 &&
                (pStripGroup->flags & STRIPGROUP_IS_DELTA_FLEXED) &&
                !(pStripGroup->flags & STRIPGROUP_IS_FLEXED);
            backups[i].forcedDynamicFlex = forceDynamicFlex;

            // The old flex-HW path rewrites every dynamic vertex to bone slot
            // zero. Preserve the group's dominant original primary bone/slot
            // for that fallback; the binding record below separately tracks
            // whether the VTX uses global SW IDs or hardware-local DX IDs.
            // This is exact for normal rigid face groups and is the least-lossy
            // fallback for old mixed neck/face groups that Source's own flex
            // HW path cannot represent fully.
            if (((pStripGroup->flags & STRIPGROUP_IS_FLEXED) ||
                 forceDynamicFlex) &&
                pStripGroup->numVerts > 0 &&
                pStripGroup->numVerts < 100000 &&
                pStripGroup->vertOffset > 0) {
                int primaryBoneCounts[MAX_D3D9_BONES] = {};
                const int maxBoneID =
                    (pStudioHdr->numbones < MAX_D3D9_BONES)
                    ? pStudioHdr->numbones
                    : MAX_D3D9_BONES;

                for (int vertexIndex = 0;
                     vertexIndex < pStripGroup->numVerts;
                     vertexIndex++) {
                    const OptimizedModel::Vertex_t* pVertex =
                        pStripGroup->pVertex(vertexIndex);
                    if (!pVertex || pVertex->numBones == 0) {
                        continue;
                    }

                    const int boneID = static_cast<signed char>(
                        pVertex->boneID[0]);
                    if (boneID >= 0 && boneID < maxBoneID) {
                        primaryBoneCounts[boneID]++;
                    }
                }

                int dominantBoneID = -1;
                int dominantBoneCount = 0;
                for (int boneID = 0; boneID < maxBoneID; boneID++) {
                    if (primaryBoneCounts[boneID] > dominantBoneCount) {
                        dominantBoneID = boneID;
                        dominantBoneCount = primaryBoneCounts[boneID];
                    }
                }
                backups[i].slotZeroBoneID = dominantBoneID;

                static int vtxFlexBindingLogCount = 0;
                if (HardwareSkinningDebugEnabled() &&
                    vtxFlexBindingLogCount++ < 64) {
                    HWSKIN_DBG(
                        "[HWSkin] VTX flex source: group=%d/%d "
                        "vertices=%d slot 0 -> bone %d (%d primary vertices)\n",
                        i + 1, numStripGroups, pStripGroup->numVerts,
                        dominantBoneID, dominantBoneCount);
                }
            }

            if (forceDynamicFlex) {
                pStripGroup->flags |= STRIPGROUP_IS_FLEXED;
                forcedDynamicFlexCount++;
            }

            if (promoteHardwareSkinning &&
                !(pStripGroup->flags & STRIPGROUP_IS_HWSKINNED)) {
                pStripGroup->flags |= STRIPGROUP_IS_HWSKINNED;
                promotedCount++;
            }
        }

        HWSKIN_DBG(
            "[HWSkin] Prepared %d/%d VTX strip groups for static GPU mesh "
            "creation; forced %d delta-only facial groups through the dynamic "
            "flex path\n",
            promotedCount, numStripGroups, forcedDynamicFlexCount);
        return numStripGroups;
    }
    __except(EXCEPTION_EXECUTE_HANDLER) {
        HWSKIN_ERROR("[HWSkin] Exception while promoting VTX strip groups\n");
        return 0;
    }
}

static void RestoreVtxStripGroupFlags(StripGroupFlagBackup* backups, int backupCount) {
    __try {
        for (int i = 0; i < backupCount; i++) {
            if (backups[i].pStripGroup) {
                backups[i].pStripGroup->flags = backups[i].flags;
            }
        }
    }
    __except(EXCEPTION_EXECUTE_HANDLER) {
        HWSKIN_ERROR("[HWSkin] Exception while restoring VTX strip-group flags\n");
    }
}

static void PreserveForcedDynamicFlexGroupFlags(
    __int64 pMeshData,
    const StripGroupFlagBackup* backups,
    int backupCount)
{
    __try {
        if (!pMeshData || !backups || backupCount <= 0) {
            return;
        }

        const int numGroups = *reinterpret_cast<int*>(pMeshData);
        studiomeshgroup_t* pGroups = *reinterpret_cast<studiomeshgroup_t**>(
            pMeshData + 8);
        if (!pGroups || numGroups <= 0 || numGroups > 100) {
            return;
        }

        const int groupsToPreserve =
            (numGroups < backupCount) ? numGroups : backupCount;
        for (int groupIndex = 0;
             groupIndex < groupsToPreserve;
             groupIndex++) {
            if (backups[groupIndex].forcedDynamicFlex) {
                pGroups[groupIndex].m_Flags |= MESHGROUP_IS_FLEXED;
            }
        }
    }
    __except(EXCEPTION_EXECUTE_HANDLER) {
        HWSKIN_ERROR(
            "[HWSkin] Exception while preserving dynamic flex-group flags\n");
    }
}

static void CachePromotedFlexGroupBoneBindings(
    __int64 pMeshData,
    const StripGroupFlagBackup* backups,
    int backupCount)
{
    __try {
        if (!pMeshData || !backups || backupCount <= 0) {
            return;
        }

        // studiomeshdata_t layout (x64):
        // +0x00: int m_NumGroup
        // +0x08: studiomeshgroup_t* m_pMeshGroup
        const int numGroups = *(int*)pMeshData;
        studiomeshgroup_t* pGroups = *(studiomeshgroup_t**)(pMeshData + 8);

        if (!pGroups || numGroups <= 0 || numGroups > 100) {
            return;
        }

        // The mesh-group allocation can be reused after a map/model purge. Clear
        // every newly created group first so a stale face binding cannot leak to
        // an unrelated body mesh at the same address.
        for (int i = 0; i < numGroups; i++) {
            ClearFlexGroupBoneBinding(&pGroups[i]);
        }

        const int groupsToCache =
            (numGroups < backupCount) ? numGroups : backupCount;
        for (int groupIndex = 0; groupIndex < groupsToCache; groupIndex++) {
            studiomeshgroup_t* pGroup = &pGroups[groupIndex];
            OptimizedModel::StripGroupHeader_t* pStripGroup =
                backups[groupIndex].pStripGroup;

            // CreateSingleMesh copies a promoted SW-VTX group's global
            // skeleton bone IDs directly into the GPU vertex stream.  Record
            // that layout for every promoted group, not only flex groups.
            // Otherwise ordinary body/torso meshes are drawn with the legacy
            // hardware-local strip palette and their vertices follow unrelated
            // bones.  Flex groups replace this marker below with their complete
            // per-vertex skin selection.
            if (!(backups[groupIndex].flags &
                  STRIPGROUP_IS_HWSKINNED)) {
                SetFlexGroupBoneBinding(
                    pGroup, 0, nullptr, 0, false, false, true);
            }

            if (!(pGroup->m_Flags & MESHGROUP_IS_FLEXED) ||
                backups[groupIndex].slotZeroBoneID < 0 || !pStripGroup ||
                pStripGroup->numVerts <= 0 ||
                pStripGroup->numVerts > 100000 ||
                pGroup->m_NumVertices != pStripGroup->numVerts) {
                continue;
            }

            const SIZE_T skinBytes =
                sizeof(FlexVertexSkinSelection) *
                static_cast<SIZE_T>(pStripGroup->numVerts);
            FlexVertexSkinSelection* pVertexSkin =
                static_cast<FlexVertexSkinSelection*>(
                    HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, skinBytes));
            if (!pVertexSkin) {
                HWSKIN_ERROR(
                    "[HWSkin] Unable to allocate %llu bytes for flex VTX skin cache\n",
                    static_cast<unsigned long long>(skinBytes));
                continue;
            }

            int oneBoneVertices = 0;
            int blendedVertices = 0;
            for (int vertexIndex = 0;
                 vertexIndex < pStripGroup->numVerts;
                 vertexIndex++) {
                const OptimizedModel::Vertex_t* pSourceVertex =
                    pStripGroup->pVertex(vertexIndex);
                FlexVertexSkinSelection& destination =
                    pVertexSkin[vertexIndex];
                destination.numBones = pSourceVertex->numBones;
                if (destination.numBones > MAX_NUM_BONES_PER_VERT) {
                    destination.numBones = MAX_NUM_BONES_PER_VERT;
                }
                for (int influence = 0;
                     influence < MAX_NUM_BONES_PER_VERT;
                     influence++) {
                    destination.boneWeightIndex[influence] =
                        pSourceVertex->boneWeightIndex[influence];
                    destination.boneID[influence] =
                        static_cast<unsigned char>(
                            pSourceVertex->boneID[influence]);
                }

                if (destination.numBones == 1) {
                    oneBoneVertices++;
                } else if (destination.numBones > 1) {
                    blendedVertices++;
                }
            }

            SetFlexGroupBoneBinding(
                pGroup, backups[groupIndex].slotZeroBoneID,
                pVertexSkin, pStripGroup->numVerts,
                (backups[groupIndex].flags &
                 STRIPGROUP_IS_HWSKINNED) != 0);
            static int flexBindingLogCount = 0;
            if (HardwareSkinningDebugEnabled() &&
                flexBindingLogCount++ < 64) {
                HWSKIN_DBG(
                    "[HWSkin] Flex binding cached: group=%d/%d %p, "
                    "slot 0 -> bone %d, exact VTX skin=%d "
                    "(one=%d blended=%d layout=%s)\n",
                    groupIndex + 1, numGroups, pGroup,
                    backups[groupIndex].slotZeroBoneID,
                    pStripGroup->numVerts, oneBoneVertices,
                    blendedVertices,
                    (backups[groupIndex].flags &
                     STRIPGROUP_IS_HWSKINNED)
                        ? "hardware-local"
                        : "skeleton-global");
            }
        }
    }
    __except(EXCEPTION_EXECUTE_HANDLER) {
        HWSKIN_ERROR(
            "[HWSkin] Exception while caching promoted flex-group bone bindings\n");
    }
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
    if (HardwareSkinningDebugEnabled() && !s_firstCallLogged) {
        HWSKIN_DBG("[HWSkin] CreateSingleMesh hook HIT! numBones=%d\n", numBones);
        s_firstCallLogged = true;
    }
    
    // NOTE: CreateSingleMesh is only called when models are FIRST loaded.
    // Each sub-patch has its own convar (gated behind r_forcehwskin master switch).
    bool forcehwskin = GlobalConvars::r_forcehwskin && GlobalConvars::r_forcehwskin->GetBool();
    bool wantFlag   = forcehwskin && GlobalConvars::r_hwskin_force_flag && GlobalConvars::r_hwskin_force_flag->GetBool();
    bool wantUBYTE4 = forcehwskin && GlobalConvars::r_hwskin_ubyte4 && GlobalConvars::r_hwskin_ubyte4->GetBool();
    bool useSoftwareVtx = forcehwskin && GlobalConvars::r_hwskin_vtx_hw &&
                          !GlobalConvars::r_hwskin_vtx_hw->GetBool();
    bool needUBYTE4 = (numBones > 1 && wantUBYTE4);

    StripGroupFlagBackup stripGroupBackups[MAX_VTX_STRIP_GROUPS] = {};
    int stripGroupBackupCount = 0;
    mstudiomesh_t* studioMesh = reinterpret_cast<mstudiomesh_t*>(pMesh);
    const bool needsDeltaFlexCompatibility =
        numBones > 1 && studioMesh && studioMesh->numflexes > 0;
    if (numBones > 1 &&
        (useSoftwareVtx || needsDeltaFlexCompatibility)) {
        stripGroupBackupCount = PromoteVtxStripGroupsForHardwareSkinning(
            pVtxMesh, reinterpret_cast<studiohdr_t*>(pStudioHdr),
            studioMesh, useSoftwareVtx,
            stripGroupBackups, MAX_VTX_STRIP_GROUPS);
    }

    TraceVtxMeshForHardwareSkinning(
        reinterpret_cast<studiohdr_t*>(pStudioHdr),
        reinterpret_cast<mstudiomesh_t*>(pMesh),
        pVtxMesh, numBones, useSoftwareVtx, forcehwskin,
        wantFlag, wantUBYTE4, stripGroupBackups,
        stripGroupBackupCount);
    
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
        HWSKIN_ERROR("[HWSkin] CRASH in CreateSingleMesh trampoline!\n");
        if (stripGroupBackupCount > 0) {
            RestoreVtxStripGroupFlags(stripGroupBackups, stripGroupBackupCount);
        }
        if (needUBYTE4) DisableUBYTE4Patch();
        return nullptr;
    }

    if (stripGroupBackupCount > 0) {
        RestoreVtxStripGroupFlags(stripGroupBackups, stripGroupBackupCount);
    }
    
    // Disable UBYTE4 patch after mesh creation
    if (needUBYTE4) {
        DisableUBYTE4Patch();
    }

    // Delta-only facial groups are a StudioMDL compatibility issue, not a
    // force-hardware-skinning feature. Keep them on Source's dynamic flex path
    // even when r_forcehwskin is disabled; in that mode the stock dynamic draw
    // performs the deformation without any of our hardware-state overrides.
    if (stripGroupBackupCount > 0) {
        PreserveForcedDynamicFlexGroupFlags(
            pMeshData, stripGroupBackups, stripGroupBackupCount);
    }

    if (forcehwskin && stripGroupBackupCount > 0) {
        CachePromotedFlexGroupBoneBindings(
            pMeshData, stripGroupBackups, stripGroupBackupCount);
    }

    // HL2 eyeballs can carry both the legacy FLEXED bit and the newer
    // DELTA_FLEXED bit. FLEXED forces Source's manually transformed dynamic
    // path even after we have built valid hardware-skinned vertex data, which
    // leaves Remix without stable bind-pose geometry and makes eyes disappear.
    //
    // DELTA_FLEXED is different: Source's hardware path consumes its morph
    // stream to keep the eyeball surface aligned with the animated eyelids.
    // Preserve that bit and clear only the legacy software-flex routing bit.
    // Otherwise G-Man/Alyx render the undeformed base eyeball and the eyelids
    // visibly intersect it at some gaze angles, even in raster mode.
    __try {
        mstudiomesh_t* studioMesh =
            reinterpret_cast<mstudiomesh_t*>(pMesh);
        studiohdr_t* studioHdr =
            reinterpret_cast<studiohdr_t*>(pStudioHdr);
        if (forcehwskin && studioMesh && studioHdr &&
            studioMesh->materialtype == 1 &&
            pMeshData) {
            const int numGroups = *reinterpret_cast<int*>(pMeshData);
            studiomeshgroup_t* groups =
                *reinterpret_cast<studiomeshgroup_t**>(pMeshData + 8);
            if (groups && numGroups > 0 && numGroups <= 100) {
                int declaredEyeBone = -1;
                mstudiomodel_t* studioModel = studioMesh->pModel();
                if (studioModel && studioMesh->materialparam >= 0 &&
                    studioMesh->materialparam < studioModel->numeyeballs) {
                    declaredEyeBone = studioModel->pEyeball(
                        studioMesh->materialparam)->bone;
                    if (declaredEyeBone < 0 ||
                        declaredEyeBone >= studioHdr->numbones) {
                        declaredEyeBone = -1;
                    }
                }

                int promotedEyeGroups = 0;
                int preservedDeltaFlexGroups = 0;
                int rigidEyeGroupsBound = 0;
                for (int groupIndex = 0;
                     groupIndex < numGroups;
                     groupIndex++) {
                    studiomeshgroup_t& group = groups[groupIndex];
                    const bool hasHardwareData =
                        (group.m_Flags & MESHGROUP_IS_HWSKINNED) != 0;
                    const bool hasLegacyFlex =
                        (group.m_Flags & MESHGROUP_IS_FLEXED) != 0;
                    const bool hasDeltaFlex =
                        (group.m_Flags & MESHGROUP_IS_DELTA_FLEXED) != 0;
                    if (hasHardwareData && hasDeltaFlex) {
                        preservedDeltaFlexGroups++;
                    }
                    if (hasHardwareData && hasLegacyFlex) {
                        group.m_Flags &= ~MESHGROUP_IS_FLEXED;
                        promotedEyeGroups++;
                    }

                    // A few SW-VTX eyes are already marked as HW data but use
                    // global skeleton IDs and emit a rigid one-bone strip with
                    // no BoneStateChange mapping. Fixed-function skinning then
                    // ignores the vertex's bone index and leaves WORLD slot 0
                    // attached to the model root. Cache the model-declared eye
                    // bone only for that exact case. Explicitly remapped and
                    // flexed eye groups retain their existing paths.
                    bool needsRigidEyeBinding =
                        declaredEyeBone >= 0 && hasHardwareData &&
                        !hasLegacyFlex && !hasDeltaFlex &&
                        group.m_pStripData && group.m_NumStrips > 0;
                    if (needsRigidEyeBinding) {
                        for (int stripIndex = 0;
                             stripIndex < group.m_NumStrips;
                             stripIndex++) {
                            OptimizedModel::StripHeader_t* pStrip =
                                &group.m_pStripData[stripIndex];
                            if (pStrip->numBones != 1 ||
                                pStrip->numBoneStateChanges != 0) {
                                needsRigidEyeBinding = false;
                                break;
                            }
                        }
                    }
                    if (needsRigidEyeBinding) {
                        SetFlexGroupBoneBinding(
                            &group, declaredEyeBone, nullptr, 0,
                            false, true);
                        rigidEyeGroupsBound++;
                    }
                }

                static int eyePromotionLogCount = 0;
                if (HardwareSkinningDebugEnabled() &&
                    (promotedEyeGroups > 0 || rigidEyeGroupsBound > 0) &&
                    eyePromotionLogCount++ < 64) {
                    HWSKIN_DBG(
                        "[HWSkin] Eye promoted to static HW: "
                        "model=%s mesh=%d eye=%d groups=%d "
                        "deltaFlexPreserved=%d rigidBone=%d "
                        "rigidGroups=%d\n",
                        studioHdr->name, studioMesh->meshid,
                        studioMesh->materialparam,
                        promotedEyeGroups, preservedDeltaFlexGroups,
                        declaredEyeBone, rigidEyeGroupsBound);
                }
            }
        }
    }
    __except(EXCEPTION_EXECUTE_HANDLER) {
        HWSKIN_ERROR(
            "[HWSkin] Exception while promoting an eye mesh\n");
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
				
				// Force the hardware flag on every group. Generic legacy flex groups
				// are handled by the dynamic-flex wrapper; eye-specific legacy routing
				// is resolved above only when a real hardware mesh was built.
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
            HWSKIN_ERROR("[HWSkin] EXCEPTION in CreateSingleMesh flag forcing!\n");
        }
    }
    
    TraceRuntimeGroupsForHardwareSkinning(
        reinterpret_cast<studiohdr_t*>(pStudioHdr),
        reinterpret_cast<mstudiomesh_t*>(pMesh), pMeshData);

    return result;
}

// ============================================================================
// Hook: R_StudioProcessFlexedMesh
//
// The stock Source implementation copies the flexed position and normal into
// the dynamic mesh, then hard-codes every vertex to weight 1.0 on hardware
// matrix slot 0. That was sufficient for old rigid facial meshes, but it loses
// the original multi-bone head/neck weights and creates a seam against a truly
// hardware-skinned body. Let Source generate its facial morph positions first,
// then put the source VVD bone weights and global bone IDs back into those same
// dynamic vertices.
// ============================================================================
static int RestoreFlexVertexSkinning(
    void* pStudioRender,
    mstudiomesh_t* pMesh,
    CMeshBuilder* pMeshBuilder,
    int numVertices,
    unsigned short* pGroupToMesh)
{
    __try {
        studiomeshgroup_t* pGroup = g_pCurrentDynamicFlexGroup;
        FlexGroupBoneBinding* pBinding =
            FindFlexGroupBoneBindingRecord(pGroup);
        const int fallbackBoneID = pBinding
            ? pBinding->slotZeroBoneID
            : -1;
        if (!pStudioRender || !pMesh || !pMeshBuilder || !pGroupToMesh ||
            !pGroup || fallbackBoneID < 0 || !GetFatVertexData_Original ||
            !pBinding->pVertexSkin || pBinding->numVertices != numVertices ||
            numVertices <= 0 || numVertices > 100000) {
            return 0;
        }

        studiohdr_t* pStudioHdr =
            *(studiohdr_t**)((uintptr_t)pStudioRender + 0x108);
        if (!pStudioHdr || pStudioHdr->numbones <= 1 ||
            pStudioHdr->numbones > MAX_D3D9_BONES) {
            return 0;
        }

        // EyeRefract's dynamic eye mesh can be declared without any blend
        // weights. BoneWeight/BoneMatrix writes cannot add a stream that is
        // absent from the declaration, so keep Source's flexed positions and
        // route the rigid draw through the optimized VTX group's dominant
        // attachment bone instead.
        if (HardwareSkinning_IsForcedEyeDraw() &&
            pMeshBuilder->NumBoneWeights() <= 0) {
            if (g_PerStripState.active &&
                g_PerStripState.pGroup == pGroup) {
                g_PerStripState.rigidFlexOutput = true;
                g_PerStripState.slotZeroBoneOverride =
                    pBinding->hardwareBoneIDs ? -1 : fallbackBoneID;
            }

            static int rigidEyeFlexLogCount = 0;
            if (HardwareSkinningDebugEnabled() &&
                rigidEyeFlexLogCount++ < 64) {
                HWSKIN_DBG(
                    "[HWSkin] Rigid eye flex output: group=%p "
                    "slot0=%d layout=%s vertices=%d\n",
                    pGroup, fallbackBoneID,
                    pBinding->hardwareBoneIDs
                        ? "hardware-local"
                        : "skeleton-global",
                    numVertices);
            }
            return numVertices;
        }

        // Active GMod x64 runtime layout (verified against the instructions in
        // this exact function): GetFatVertexData returns pMesh + 0x30. Its
        // model-vertex-data pointer is at +0x24; that structure contains the
        // VVD vertex base at +0x00, while the model's byte offset is at -0x18.
        // The mesh-local vertex offset is stored 0x24 bytes before the returned
        // pointer. mstudiovertex_t remains the standard 48-byte Source layout.
        unsigned char* pMeshVertexData =
            static_cast<unsigned char*>(
                GetFatVertexData_Original(pMesh, pStudioHdr));
        if (!pMeshVertexData || sizeof(mstudiovertex_t) != 48) {
            return 0;
        }

        unsigned char* pModelVertexData =
            *(unsigned char**)(pMeshVertexData + 0x24);
        const int meshVertexOffset = *(int*)(pMeshVertexData - 0x24);
        if (!pModelVertexData || meshVertexOffset < 0) {
            return 0;
        }

        unsigned char* pVvdVertexBase = *(unsigned char**)pModelVertexData;
        const int modelVertexByteOffset = *(int*)(pModelVertexData - 0x18);
        if (!pVvdVertexBase || modelVertexByteOffset < 0 ||
            (modelVertexByteOffset % sizeof(mstudiovertex_t)) != 0) {
            return 0;
        }

        mstudiovertex_t* pVertices = reinterpret_cast<mstudiovertex_t*>(
            pVvdVertexBase + modelVertexByteOffset) + meshVertexOffset;
        const int meshVertexCount = *(int*)((unsigned char*)pMesh + 0x08);
        if (meshVertexCount <= 0 || meshVertexCount > 1000000) {
            return 0;
        }

        int restoredVertices = 0;
        int blendedVertices = 0;
        int minBoneID = MAX_D3D9_BONES;
        int maxBoneID = -1;

        for (int vertexIndex = 0; vertexIndex < numVertices; vertexIndex++) {
            const int meshVertexIndex = pGroupToMesh[vertexIndex];
            if (meshVertexIndex < 0 || meshVertexIndex >= meshVertexCount) {
                continue;
            }

            const mstudioboneweight_t& sourceWeights =
                pVertices[meshVertexIndex].m_BoneWeights;
            const FlexVertexSkinSelection& optimizedSkin =
                pBinding->pVertexSkin[vertexIndex];
            int sourceBoneCount = optimizedSkin.numBones;
            if (sourceBoneCount > MAX_NUM_BONES_PER_VERT) {
                sourceBoneCount = MAX_NUM_BONES_PER_VERT;
            }

            float weights[MAX_NUM_BONES_PER_VERT] = {};
            int boneIDs[MAX_NUM_BONES_PER_VERT] = {};
            int validBoneCount = 0;
            float totalWeight = 0.0f;

            for (int influence = 0;
                 influence < sourceBoneCount;
                 influence++) {
                const int weightIndex =
                    optimizedSkin.boneWeightIndex[influence];
                const int boneID = optimizedSkin.boneID[influence];
                if (weightIndex < 0 ||
                    weightIndex >= MAX_NUM_BONES_PER_VERT) {
                    continue;
                }
                const float weight = sourceWeights.weight[weightIndex];
                if (boneID < 0 || boneID >= pStudioHdr->numbones ||
                    !(weight >= 0.0f) || weight > 1.0001f) {
                    continue;
                }

                boneIDs[validBoneCount] = boneID;
                weights[validBoneCount] = weight;
                totalWeight += weight;
                validBoneCount++;
            }

            if (validBoneCount == 0 || totalWeight <= 0.000001f) {
                validBoneCount = 1;
                boneIDs[0] = fallbackBoneID;
                weights[0] = 1.0f;
                totalWeight = 1.0f;
            }

            const float inverseTotalWeight = 1.0f / totalWeight;
            for (int influence = 0;
                 influence < validBoneCount;
                 influence++) {
                weights[influence] *= inverseTotalWeight;
                if (boneIDs[influence] < minBoneID) {
                    minBoneID = boneIDs[influence];
                }
                if (boneIDs[influence] > maxBoneID) {
                    maxBoneID = boneIDs[influence];
                }
            }

            pMeshBuilder->SelectVertex(vertexIndex);
            for (int influence = 0; influence < 4; influence++) {
                pMeshBuilder->BoneWeight(
                    influence,
                    influence < validBoneCount ? weights[influence] : 0.0f);
                pMeshBuilder->BoneMatrix(
                    influence,
                    influence < validBoneCount
                        ? boneIDs[influence]
                        : boneIDs[0]);
            }

            restoredVertices++;
            if (validBoneCount > 1) {
                blendedVertices++;
            }
        }

        if (restoredVertices > 0 && g_PerStripState.active &&
            g_PerStripState.pGroup == pGroup) {
            // SW VTX stores global skeleton IDs, while a legacy DX9 VTX stores
            // hardware-local slots whose skeleton mapping lives in the strip's
            // bone-state changes. Preserve that distinction at the DIP boundary
            // instead of accidentally treating local slots 0..N as root bones.
            g_PerStripState.slotZeroBoneOverride =
                pBinding->hardwareBoneIDs ? -1 : 0;
        }

        static int restoreLogCount = 0;
        if (HardwareSkinningDebugEnabled() && restoredVertices > 0 &&
            restoreLogCount++ < 64) {
            HWSKIN_DBG(
                "[HWSkin] Flex vertex skin restored: group=%p "
                "vertices=%d/%d blended=%d bones=%d..%d "
                "storedWeights=%d layout=%s\n",
                pGroup, restoredVertices, numVertices, blendedVertices,
                minBoneID, maxBoneID, pMeshBuilder->NumBoneWeights(),
                pBinding->hardwareBoneIDs
                    ? "hardware-local"
                    : "skeleton-global");
        }

        return restoredVertices;
    }
    __except(EXCEPTION_EXECUTE_HANDLER) {
        HWSKIN_ERROR(
            "[HWSkin] Exception while restoring flex vertex skinning\n");
        return 0;
    }
}

Define_method_Hook(void, R_StudioProcessFlexedMesh, void*,
    mstudiomesh_t* pMesh,
    CMeshBuilder* pMeshBuilder,
    int numVertices,
    unsigned short* pGroupToMesh)
{
    R_StudioProcessFlexedMesh_trampoline()(
        _this, pMesh, pMeshBuilder, numVertices, pGroupToMesh);

    RestoreFlexVertexSkinning(
        _this, pMesh, pMeshBuilder, numVertices, pGroupToMesh);
}

// ============================================================================
// Hook: R_StudioDrawDynamicMesh
//
// Old-style facial flexes are rebuilt into a dynamic vertex buffer every frame.
// Source normally routes that buffer through R_StudioDrawGroupSWSkin whenever
// software-skin renderer/material flags are present, which bakes the skeleton
// pose into positions and leaves Remix with no bones. For flex groups only,
// temporarily suppress those fallback flags and carry our D3D9 bone state into
// the separate dynamic-HW draw wrapper.
// ============================================================================
int* HardwareSkinning_DrawDynamicMesh(
    void* _this,
    void* pRenderContext,
    void* pMesh,
    studiomeshgroup_t* pGroup,
    int lighting,
    float r_blend,
    IMaterial* pMaterial,
    int lod,
    HWSkinDrawDynamicMeshFn original)
{
    if (!original) {
        return nullptr;
    }

    const bool forcehwskin =
        GlobalConvars::r_forcehwskin &&
        GlobalConvars::r_forcehwskin->GetBool();
    const bool forceFlexHardware =
        forcehwskin && g_bInRenderFinal && pGroup &&
        (pGroup->m_Flags & MESHGROUP_IS_FLEXED);

    if (!forceFlexHardware) {
        return original(
            _this, pRenderContext, pMesh, pGroup, lighting, r_blend,
            pMaterial, lod);
    }

    unsigned char* pRenderConfig = nullptr;
    unsigned char oldConfig24 = 0;
    unsigned char oldConfig25 = 0;
    bool configChanged = false;
    bool materialNeededSoftwareSkinning = false;
    bool materialFlagChanged = false;
    bool perStripActive = false;
    studiomeshgroup_t* previousDynamicFlexGroup =
        g_pCurrentDynamicFlexGroup;
    int* result = nullptr;

    __try {
        // CStudioRender::m_pRC is at +0x08 in the active x64 build. The
        // relevant config bits are read directly by R_StudioDrawDynamicMesh:
        //   config[0x24] bit 1: software skin
        //   config[0x25] bits 0/1/3: normals, tangent frame, software lighting
        pRenderConfig = *(unsigned char**)((uintptr_t)_this + 0x08);
        if (pRenderConfig) {
            oldConfig24 = pRenderConfig[0x24];
            oldConfig25 = pRenderConfig[0x25];
            pRenderConfig[0x24] &= static_cast<unsigned char>(~0x02);
            pRenderConfig[0x25] &= static_cast<unsigned char>(~0x0B);
            configChanged = true;
        }

        if (pMaterial) {
            materialNeededSoftwareSkinning = pMaterial->NeedsSoftwareSkinning();
            if (materialNeededSoftwareSkinning) {
                pMaterial->SetMaterialVarFlag(
                    MATERIAL_VAR_NEEDS_SOFTWARE_SKINNING, false);
                materialFlagChanged = true;
            }
        }

        pGroup->m_Flags |= MESHGROUP_IS_HWSKINNED;

        studiohdr_t* pStudioHdr =
            *(studiohdr_t**)((uintptr_t)_this + 0x108);
        matrix3x4_t* pPoseToWorld =
            *(matrix3x4_t**)((uintptr_t)_this + 0xE8);

        if (g_pD3DDevice && pStudioHdr && pPoseToWorld &&
            pStudioHdr->numbones > 1 && pStudioHdr->numbones <= 512) {
            const int setBonesMode = GlobalConvars::r_hwskin_setbones
                ? GlobalConvars::r_hwskin_setbones->GetInt()
                : 0;
            const bool doSetBones =
                setBonesMode == 1 ||
                (setBonesMode > 1 && pStudioHdr->numbones == setBonesMode);

            if (doSetBones) {
                SetD3D9BoneTransforms(
                    g_pD3DDevice, pStudioHdr, pPoseToWorld,
                    pStudioHdr->numbones, g_modelToWorld,
                    g_bHaveModelToWorld, pGroup);

                if (g_OriginalDIP && pGroup->m_pStripData &&
                    pGroup->m_NumStrips > 0) {
                    g_PerStripState.active = true;
                    g_PerStripState.currentStripIndex = 0;
                    g_PerStripState.totalStrips = pGroup->m_NumStrips;
                    g_PerStripState.pGroup = pGroup;
                    g_PerStripState.cache =
                        GetOrCreateBoneRemapCache(pStudioHdr);
                    g_PerStripState.pStudioHdr = pStudioHdr;
                    g_PerStripState.poseToWorld = pPoseToWorld;
                    g_PerStripState.numBones = pStudioHdr->numbones;
                    g_PerStripState.haveModelToWorld =
                        g_bHaveModelToWorld;
                    g_PerStripState.skeletonGlobalBoneIDs =
                        FlexGroupUsesSkeletonGlobalBoneIDs(pGroup);
                    g_PerStripState.rigidFlexOutput = false;
                    g_PerStripState.slotZeroBoneOverride =
                        g_PerStripState.skeletonGlobalBoneIDs
                            ? 0
                            : FindFlexGroupBoneBinding(pGroup);
                    if (g_bHaveModelToWorld) {
                        TinyMathLib_MatrixInvert(
                            g_modelToWorld, g_PerStripState.worldToModel);
                    }
                    perStripActive = true;
                }
            }

            TraceHardwareSkinningDraw(
                pStudioHdr, pGroup, "dynamic", 1,
                setBonesMode, doSetBones, perStripActive);

            static int dynamicFlexLogCount = 0;
            if (HardwareSkinningDebugEnabled() &&
                dynamicFlexLogCount++ < 64) {
                HWSKIN_DBG(
                    "[HWSkin] Dynamic flex forced HW: group=%p flags=0x%X "
                    "lighting=%d config24=0x%02X config25=0x%02X "
                    "materialSW=%d slot0Override=%d bones=%d\n",
                    pGroup, pGroup->m_Flags, lighting, oldConfig24,
                    oldConfig25, materialNeededSoftwareSkinning,
                    FindFlexGroupBoneBinding(pGroup), pStudioHdr->numbones);
            }
        }

        g_pCurrentDynamicFlexGroup = pGroup;
        result = original(
            _this, pRenderContext, pMesh, pGroup, 0, r_blend,
            pMaterial, lod);
    }
    __except(EXCEPTION_EXECUTE_HANDLER) {
        HWSKIN_ERROR(
            "[HWSkin] Exception while forcing dynamic flex hardware skinning\n");
    }

    g_pCurrentDynamicFlexGroup = previousDynamicFlexGroup;

    if (perStripActive) {
        g_PerStripState.active = false;
    }

    __try {
        if (materialFlagChanged && pMaterial) {
            pMaterial->SetMaterialVarFlag(
                MATERIAL_VAR_NEEDS_SOFTWARE_SKINNING, true);
        }
        if (configChanged && pRenderConfig) {
            pRenderConfig[0x24] = oldConfig24;
            pRenderConfig[0x25] = oldConfig25;
        }
    }
    __except(EXCEPTION_EXECUTE_HANDLER) {
        HWSKIN_ERROR(
            "[HWSkin] Exception while restoring dynamic flex render state\n");
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
    if (HardwareSkinningDebugEnabled() && !s_firstCallLogged) {
        HWSKIN_DBG("[HWSkin] DrawGroupHWSkin hook HIT! lighting=%d, r_blend=%.2f, pGroup=%p\n", lighting, r_blend, pGroup);
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

    // This function is also Source's outer static-mesh submission routine.
    // A stock vertex shader consumes its delta-flex stream, but our fixed-
    // function Eyes_dx9 shader cannot. Rebuild only the outer forced eye draw
    // as a dynamic flex mesh. The dynamic renderer re-enters this hook for its
    // final HW submission, so the depth guard lets that inner call continue
    // normally and avoids recursion.
    if (HardwareSkinning_IsForcedEyeDraw() &&
        g_forcedEyeDynamicRedirectDepth == 0 && pGroup &&
        g_RStudioDrawDynamicMeshOriginal) {
        const unsigned int originalGroupFlags = pGroup->m_Flags;
        __int64 redirectedResult = 0;
        __try {
            pGroup->m_Flags |=
                MESHGROUP_IS_FLEXED | MESHGROUP_IS_HWSKINNED;
            g_forcedEyeDynamicRedirectDepth++;
            int* dynamicResult = HardwareSkinning_DrawDynamicMesh(
                _this, pRenderContext,
                reinterpret_cast<void*>(bodyPartInfo), pGroup, 0, r_blend,
                reinterpret_cast<IMaterial*>(pMaterial),
                static_cast<int>(flags),
                g_RStudioDrawDynamicMeshOriginal);
            redirectedResult = static_cast<__int64>(
                reinterpret_cast<intptr_t>(dynamicResult));
        }
        __finally {
            g_forcedEyeDynamicRedirectDepth = 0;
            pGroup->m_Flags = originalGroupFlags;
        }

        static int redirectedEyeDrawCount = 0;
        if (HardwareSkinningDebugEnabled() &&
            redirectedEyeDrawCount++ < 32) {
            HWSKIN_DBG(
                "[HWSkin] Eye static delta-flex redirected to dynamic HW: "
                "group=%p flags=0x%X triangles=%lld\n",
                pGroup, originalGroupFlags,
                static_cast<long long>(redirectedResult));
        }
        return redirectedResult;
    }

    // CMDLCache::Flush(MDLCACHE_FLUSH_STUDIOHWDATA) can leave a mesh group in
    // the short interval between destroying its bookkeeping arrays and
    // rebuilding the hardware data.  GMod's stock draw helper does not guard
    // m_pUniqueTris: studiorender.dll+0x11270 unconditionally adds
    // m_pUniqueTris[stripIndex] after submitting each strip.  The array is
    // used only for the returned performance counter, so supply conservative
    // temporary counts when the drawable mesh/strip data are otherwise valid.
    constexpr int MAX_TRANSIENT_DRAW_STRIPS = 1024;
    int transientUniqueTris[MAX_TRANSIENT_DRAW_STRIPS];
    int* originalUniqueTris = nullptr;
    bool usingTransientUniqueTris = false;

    __try {
        if (!pGroup || !pGroup->m_pMesh || pGroup->m_NumStrips < 0 ||
            pGroup->m_NumStrips > MAX_TRANSIENT_DRAW_STRIPS ||
            (pGroup->m_NumStrips > 0 && !pGroup->m_pStripData)) {
            static int incompleteGroupLogCount = 0;
            if (HardwareSkinningDebugEnabled() &&
                incompleteGroupLogCount++ < 32) {
                HWSKIN_DBG(
                    "[HWSkin] Skipping incomplete HW mesh group: "
                    "group=%p mesh=%p strips=%d stripData=%p\n",
                    pGroup, pGroup ? pGroup->m_pMesh : nullptr,
                    pGroup ? pGroup->m_NumStrips : -1,
                    pGroup ? pGroup->m_pStripData : nullptr);
            }
            return 0;
        }

        originalUniqueTris = pGroup->m_pUniqueTris;
        if (pGroup->m_NumStrips > 0 && !originalUniqueTris) {
            for (int stripIndex = 0;
                 stripIndex < pGroup->m_NumStrips; ++stripIndex) {
                const OptimizedModel::StripHeader_t& strip =
                    pGroup->m_pStripData[stripIndex];
                transientUniqueTris[stripIndex] =
                    (strip.flags & OptimizedModel::STRIP_IS_TRISTRIP)
                        ? ((strip.numIndices > 2) ? strip.numIndices - 2 : 0)
                        : ((strip.numIndices > 0) ? strip.numIndices / 3 : 0);
            }
            pGroup->m_pUniqueTris = transientUniqueTris;
            usingTransientUniqueTris = true;

            static int transientStatsLogCount = 0;
            if (HardwareSkinningDebugEnabled() &&
                transientStatsLogCount++ < 32) {
                HWSKIN_DBG(
                    "[HWSkin] Supplying transient triangle statistics for "
                    "HW mesh group %p (%d strips)\n",
                    pGroup, pGroup->m_NumStrips);
            }
        }
    }
    __except(EXCEPTION_EXECUTE_HANDLER) {
        HWSKIN_ERROR(
            "[HWSkin] Skipping unreadable HW mesh group before draw\n");
        return 0;
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
    studiohdr_t* traceStudioHdr = nullptr;
    const int traceSetBonesMode = GlobalConvars::r_hwskin_setbones
        ? GlobalConvars::r_hwskin_setbones->GetInt()
        : 0;
    
    if (shouldSetBones) {
        __try {
            // CStudioRenderContext layout (x64) - found via IDA reverse engineering:
            // +0x108 (264): studiohdr_t* m_pStudioHdr
            // +0xE8 (232): matrix3x4_t* m_PoseToWorld (array of bone matrices)
            studiohdr_t* pStudioHdr = *(studiohdr_t**)((uintptr_t)_this + 264);
            matrix3x4_t* m_PoseToWorld = *(matrix3x4_t**)((uintptr_t)_this + 232);
            traceStudioHdr = pStudioHdr;
            
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
                int setBonesMode = traceSetBonesMode;
                
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
            HWSKIN_ERROR("[HWSkin] EXCEPTION reading bone data from _this!\n");
            g_BONEDATA.bone_count = -1;
            g_BONEDATA.active = false;
        }
    }
    
    // Determine actual lighting to use
    // Force LIGHTING_HARDWARE (0) for skinned meshes to ensure HW skinning path
    int actualLighting = (forcedHwLighting) ? 0 : lighting;
    
    // =========================================================================
    // Skinned DIP hook activation
    //
    // Activate this for every skinned mesh, including single-strip SW VTX
    // meshes, so hardware slot 0 is restored at the actual DIP boundary.
    // Multi-strip groups additionally receive their incremental remaps.
    // =========================================================================
    bool perStripActive = false;
    if (hookDoSetBones && g_OriginalDIP && pGroup &&
        pGroup->m_pStripData && pGroup->m_NumStrips > 0) {
        // A forced eye draw first enters HardwareSkinning_DrawDynamicMesh so
        // Source can build its delta-flexed vertices.  If that dynamic vertex
        // declaration has no blend stream, RestoreFlexVertexSkinning records a
        // rigid attachment to the eye/head bone in g_PerStripState.  The
        // dynamic renderer then re-enters this HW draw hook for the actual
        // submission.  Preserve that pending attachment here instead of
        // replacing it with the ordinary skeleton-global slot-zero default
        // (the model root).
        const bool pendingRigidFlexOutput =
            g_PerStripState.active &&
            g_PerStripState.pGroup == pGroup &&
            g_PerStripState.rigidFlexOutput;
        const int pendingRigidFlexBone =
            pendingRigidFlexOutput
                ? g_PerStripState.slotZeroBoneOverride
                : -1;

        g_PerStripState.active = true;
        g_PerStripState.currentStripIndex = 0;
        g_PerStripState.totalStrips = pGroup->m_NumStrips;
        g_PerStripState.pGroup = pGroup;
        g_PerStripState.cache = GetOrCreateBoneRemapCache(hookStudioHdr);
        g_PerStripState.pStudioHdr = hookStudioHdr;
        g_PerStripState.poseToWorld = hookPoseToWorld;
        g_PerStripState.numBones = hookStudioHdr->numbones;
        g_PerStripState.haveModelToWorld = g_bHaveModelToWorld;
        g_PerStripState.skeletonGlobalBoneIDs =
            FlexGroupUsesSkeletonGlobalBoneIDs(pGroup);
        g_PerStripState.rigidFlexOutput = pendingRigidFlexOutput;
        g_PerStripState.slotZeroBoneOverride =
            pendingRigidFlexOutput
                ? pendingRigidFlexBone
                : (g_PerStripState.skeletonGlobalBoneIDs
                    ? 0
                    : FindFlexGroupBoneBinding(pGroup));
        if (pendingRigidFlexOutput) {
            static int rigidEyeHandoffLogCount = 0;
            if (HardwareSkinningDebugEnabled() &&
                rigidEyeHandoffLogCount++ < 64) {
                HWSKIN_DBG(
                    "[HWSkin] Rigid eye flex handoff preserved: "
                    "model=%s group=%p bone=%d\n",
                    hookStudioHdr->pszName(), pGroup,
                    pendingRigidFlexBone);
            }
        }
        if (pGroup->m_Flags & MESHGROUP_IS_FLEXED) {
            static int flexDrawLogCount = 0;
            if (HardwareSkinningDebugEnabled() &&
                flexDrawLogCount++ < 64) {
                HWSKIN_DBG(
                    "[HWSkin] Flex HW draw: model=%s group=%p flags=0x%X "
                    "slot0Override=%d vertices=%d strips=%d\n",
                    hookStudioHdr->pszName(), pGroup, pGroup->m_Flags,
                    g_PerStripState.slotZeroBoneOverride,
                    pGroup->m_NumVertices, pGroup->m_NumStrips);
            }
        }
        if (g_bHaveModelToWorld && hookStudioHdr->numbones > 1) {
            TinyMathLib_MatrixInvert(g_modelToWorld, g_PerStripState.worldToModel);
        }
        perStripActive = true;
    }

    TraceHardwareSkinningDraw(
        traceStudioHdr, pGroup, "static", 2,
        traceSetBonesMode, hookDoSetBones, perStripActive);
    
    // Call original draw function
    __int64 result = 0;
    __try {
        result = R_StudioDrawGroupHWSkin_trampoline()(_this, pRenderContext, bodyPartInfo, pGroup, actualLighting, r_blend, pMaterial, flags, pColorMeshInfo);
    }
    __except(EXCEPTION_EXECUTE_HANDLER) {
        HWSKIN_ERROR("[HWSkin] CRASH in DrawGroupHWSkin trampoline!\n");
    }

    if (usingTransientUniqueTris) {
        // The group belongs to the engine; never leave it pointing at our
        // stack-backed fallback after this draw returns.
        pGroup->m_pUniqueTris = originalUniqueTris;
    }
    
    // Deactivate the skinned DIP hook.
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
    if (HardwareSkinningDebugEnabled() && !s_firstCallLogged) {
        HWSKIN_DBG("[HWSkin] RenderFinal hook HIT! skin=%d, boneMask=0x%X, pClientEntity=%p\n",
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
        const uintptr_t ICLIENTRENDERABLE_OFFSET_X64 = 8;
        const uintptr_t OFFSET_COORDINATE_FRAME_X64 = 1016; // 0x3F8 from IDA datamap
        const uintptr_t pBaseEntity =
            (uintptr_t)pClientEntity - ICLIENTRENDERABLE_OFFSET_X64;
        const void* pCoordinateFrame =
            (const void*)(pBaseEntity + OFFSET_COORDINATE_FRAME_X64);

        // Some loading-screen and transient renderables are not C_BaseEntity
        // objects even though the engine passes them through this parameter.
        // Reading the assumed coordinate-frame offset directly raises a
        // first-chance access violation for those objects. ReadProcessMemory
        // validates the complete range without faulting this render thread.
        matrix3x4_t candidateModelToWorld;
        SIZE_T bytesRead = 0;
        const bool copied = ReadProcessMemory(
            GetCurrentProcess(), pCoordinateFrame, &candidateModelToWorld,
            sizeof(candidateModelToWorld), &bytesRead) != FALSE &&
            bytesRead == sizeof(candidateModelToWorld);

        bool matrixFinite = copied;
        for (int row = 0; matrixFinite && row < 3; ++row) {
            for (int column = 0; column < 4; ++column) {
                if (!std::isfinite(candidateModelToWorld[row][column])) {
                    matrixFinite = false;
                    break;
                }
            }
        }

        if (matrixFinite) {
            // Sanity check: rotation values should be -1 to 1, translation
            // should be reasonable world coordinates.
            const float checkVal = candidateModelToWorld[0][0];
            const float checkTrans = candidateModelToWorld[0][3];
            const bool rotationValid =
                checkVal >= -1.1f && checkVal <= 1.1f;
            const bool translationValid =
                checkTrans > -100000.0f && checkTrans < 100000.0f;

            if (rotationValid && translationValid) {
                memcpy(&g_modelToWorld, &candidateModelToWorld,
                    sizeof(g_modelToWorld));
                g_bHaveModelToWorld = true;

                static bool s_firstLogged = false;
                if (!s_firstLogged) {
                    HWSKIN_DBG("[HWSkin] Got modelToWorld from pBaseEntity (pClientEntity-%llu)+%llu:\n",
                        (unsigned long long)ICLIENTRENDERABLE_OFFSET_X64,
                        (unsigned long long)OFFSET_COORDINATE_FRAME_X64);
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
                    HWSKIN_DBG("[HWSkin] Bad offset? pBaseEntity+%llu: rot[0][0]=%.2f, trans[0][3]=%.2f\n",
                        (unsigned long long)OFFSET_COORDINATE_FRAME_X64,
                        checkVal, checkTrans);
                    s_badOffsetLogged = true;
                }
            }
        } else {
            static bool s_unreadableLogged = false;
            if (!s_unreadableLogged) {
                HWSKIN_DBG("[HWSkin] Skipping unreadable/non-entity renderable %p\n",
                    pClientEntity);
                s_unreadableLogged = true;
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
            HWSKIN_ERROR("[HWSkin] Exception in RenderFinal trampoline (occurrence #%d)\n", crashCount);
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
        HWSKIN_DBG("[Hardware Skinning] Already initialized\n");
            return;
        }

    try {
        HWSKIN_DBG("[Hardware Skinning] Initializing...\n");

        // Get D3D9 device for setting bone transforms
        g_pD3DDevice = static_cast<IDirect3DDevice9*>(FindD3D9Device());
        if (!g_pD3DDevice) {
            Warning("[Hardware Skinning] Failed to get D3D9 device - bone transforms will not be set\n");
            // Continue anyway - the hooks can still work, just without D3D9 bone transform output
        } else {
            HWSKIN_DBG("[Hardware Skinning] D3D9 device acquired for bone transform output\n");
            
            // ================================================================
            // Install DrawIndexedPrimitive vtable hook for per-strip bone updates
            // ================================================================
            if (InstallDIPVtableHook(g_pD3DDevice)) {
                HWSKIN_DBG("[Hardware Skinning] Installed DrawIndexedPrimitive vtable hook for per-strip bones\n");
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
                    HWSKIN_DBG("[Hardware Skinning] UBYTE4 patch registered (enabled per-model during mesh creation)\n");
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
        HWSKIN_DBG("[Hardware Skinning] Found studiorender.dll at %p\n", studiorenderdll);

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

        // R_StudioProcessFlexedMesh. The call at +0x25 is the function's own
        // GetFatVertexData helper, which we reuse after the original builder
        // has emitted its morphed positions and normals.
        static const char ProcessFlexedMesh_sign[] =
            "48 89 5C 24 18 48 89 74 24 20 57 48 83 EC 20 "
            "48 8B C2 49 63 F1 48 8B 91 08 01 00 00 48 8B F9 "
            "48 8B C8 49 8B D8 E8 ? ? ? ? 48 85 C0";
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
        HWSKIN_DBG("[Hardware Skinning] R_StudioCreateSingleMesh: %p (offset: 0x%llX)\n", CreateSingleMesh_addr, CreateSingleMesh_offset);

        auto DrawGroupHWSkin_addr = ScanSign(studiorenderdll, DrawGroupHWSkin_sign, sizeof(DrawGroupHWSkin_sign) - 1);
        uintptr_t DrawGroupHWSkin_offset = DrawGroupHWSkin_addr ? ((uintptr_t)DrawGroupHWSkin_addr - moduleBase) : 0;
        HWSKIN_DBG("[Hardware Skinning] R_StudioDrawGroupHWSkin: %p (offset: 0x%llX)\n", DrawGroupHWSkin_addr, DrawGroupHWSkin_offset);
        
        auto RenderFinal_addr = ScanSign(studiorenderdll, RenderFinal_sign, sizeof(RenderFinal_sign) - 1);
        uintptr_t RenderFinal_offset = RenderFinal_addr ? ((uintptr_t)RenderFinal_addr - moduleBase) : 0;
        HWSKIN_DBG("[Hardware Skinning] R_StudioRenderFinal: %p (offset: 0x%llX)\n", RenderFinal_addr, RenderFinal_offset);

        auto ProcessFlexedMesh_addr = ScanSign(
            studiorenderdll, ProcessFlexedMesh_sign,
            sizeof(ProcessFlexedMesh_sign) - 1);
        uintptr_t ProcessFlexedMesh_offset = ProcessFlexedMesh_addr
            ? ((uintptr_t)ProcessFlexedMesh_addr - moduleBase)
            : 0;
        HWSKIN_DBG("[Hardware Skinning] R_StudioProcessFlexedMesh: %p "
            "(offset: 0x%llX)\n",
            ProcessFlexedMesh_addr, ProcessFlexedMesh_offset);

        void* GetFatVertexData_addr = nullptr;
        if (ProcessFlexedMesh_addr) {
            unsigned char* getFatCall =
                (unsigned char*)ProcessFlexedMesh_addr + 0x25;
            if (getFatCall[0] == 0xE8) {
                const int relativeTarget = *(int*)(getFatCall + 1);
                GetFatVertexData_addr =
                    getFatCall + 5 + relativeTarget;
            }
        }
        HWSKIN_DBG("[Hardware Skinning] GetFatVertexData: %p (from flex call)\n",
            GetFatVertexData_addr);

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
        HWSKIN_DBG("[Hardware Skinning] R_StudioDrawEyeball: %p (offset: 0x%llX)\n", DrawEyeball_addr, DrawEyeball_offset);
        
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
        if (ProcessFlexedMesh_addr &&
            ProcessFlexedMesh_offset < MIN_VALID_OFFSET) {
            Warning("[Hardware Skinning] R_StudioProcessFlexedMesh matched "
                    "PE header - invalid!\n");
            ProcessFlexedMesh_addr = nullptr;
            GetFatVertexData_addr = nullptr;
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
        if (!ProcessFlexedMesh_addr || !GetFatVertexData_addr) {
            Warning("[Hardware Skinning] R_StudioProcessFlexedMesh or its "
                    "vertex-data helper not found!\n");
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
        GetFatVertexData_Original =
            (F_GetFatVertexData)GetFatVertexData_addr;

        // Set up all hooks with corrected GMod signatures (9 params for DrawGroupHWSkin/RenderFinal)
        Setup_Hook(R_StudioCreateSingleMesh, CreateSingleMesh_addr);
        Setup_Hook(R_StudioProcessFlexedMesh, ProcessFlexedMesh_addr);
        Setup_Hook(R_StudioDrawGroupHWSkin, DrawGroupHWSkin_addr);
        Setup_Hook(R_StudioRenderFinal, RenderFinal_addr);

        if (DrawEyeball_addr) {
            Setup_Hook(R_StudioDrawEyeball, DrawEyeball_addr);
            HWSKIN_DBG("[Hardware Skinning] R_StudioDrawEyeball hook installed - HW lighting force active\n");
        } else {
            Warning("[Hardware Skinning] R_StudioDrawEyeball not found - eye hook disabled. Eyes may not render in RTX Remix.\n");
            Warning("[Hardware Skinning]   To fix: find R_StudioDrawEyeball signature in IDA (search for \"$glint\" string ref)\n");
        }

        m_bInitialized = true;
        m_bEnabled = true;
        
        HWSKIN_DBG("[Hardware Skinning] Successfully initialized - bone transforms via D3D9 fixed-function API\n");
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
        HWSKIN_DBG("[Hardware Skinning] Shutting down...\n");

        // Restore DrawIndexedPrimitive vtable hook
        if (g_OriginalDIP && g_pD3DDevice) {
            RestoreDIPVtableHook(g_pD3DDevice);
            HWSKIN_DBG("[Hardware Skinning] Restored original DrawIndexedPrimitive vtable entry\n");
            g_OriginalDIP = nullptr;
        }
        g_PerStripState.active = false;

        // Restore shaderapidx9.dll UBYTE4 patch
        if (g_MemoryPatcher.DoesPatchExist("HWSkin_BlendIndices_UBYTE4")) {
            g_MemoryPatcher.DisablePatch("HWSkin_BlendIndices_UBYTE4");
            HWSKIN_DBG("[Hardware Skinning] Restored original bone index declaration type\n");
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
        R_StudioProcessFlexedMesh_hook.Destroy();
        R_StudioDrawGroupHWSkin_hook.Destroy();
        R_StudioRenderFinal_hook.Destroy();
        if (R_StudioDrawEyeball_Original) {
            R_StudioDrawEyeball_hook.Destroy();
        }

        // Reset global state
        g_BONEDATA.bone_count = -1;
        g_BONEDATA.active = false;
        g_bInRenderFinal = false;
        g_pCurrentDynamicFlexGroup = nullptr;
        GetFatVertexData_Original = nullptr;
        g_pD3DDevice = nullptr;
        g_MaxBonesEverSet = 0;
        g_NumActiveCaches = 0;
        for (int bindingIndex = 0;
             bindingIndex < MAX_FLEX_GROUP_BONE_BINDINGS;
             bindingIndex++) {
            FreeFlexGroupVertexSkin(
                g_FlexGroupBoneBindings[bindingIndex]);
        }
        memset(g_FlexGroupBoneBindings, 0, sizeof(g_FlexGroupBoneBindings));
        memset(g_HardwareSkinDrawTraceRecords, 0,
            sizeof(g_HardwareSkinDrawTraceRecords));
        g_HardwareSkinDrawTraceWriteIndex = 0;
        g_HardwareSkinDrawTraceRecordCount = 0;
        m_bEnabled = false;
        m_bInitialized = false;

        HWSKIN_DBG("[Hardware Skinning] Shutdown complete\n");
    }
    catch (...) {
        Warning("[Hardware Skinning] Exception during shutdown\n");
    }
}

#endif // HWSKIN_PATCHES && _WIN64
