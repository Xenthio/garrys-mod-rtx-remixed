#pragma once
#include <Windows.h>
#include "studio.h"
#include "e_utils.h"
#include <d3d9.h>
#include "mathlib/vmatrix.h" 

// Forward declarations
namespace OptimizedModel {
    struct StripGroupHeader_t;
}

class IMaterial;

// prop_fixes.cpp already owns the R_StudioDrawDynamicMesh detour. Route that
// detour through the hardware-skinning module so facial flexes can share the
// same hook instead of installing two MinHook patches on one function.
using HWSkinDrawDynamicMeshFn = int* (__fastcall*)(
    void* pStudioRender,
    void* pRenderContext,
    void* pMesh,
    studiomeshgroup_t* pGroup,
    int lighting,
    float blend,
    IMaterial* pMaterial,
    int lod);

int* HardwareSkinning_DrawDynamicMesh(
    void* pStudioRender,
    void* pRenderContext,
    void* pMesh,
    studiomeshgroup_t* pGroup,
    int lighting,
    float blend,
    IMaterial* pMaterial,
    int lod,
    HWSkinDrawDynamicMeshFn original);

// prop_fixes.cpp owns the one R_StudioDrawDynamicMesh hook. Publish its
// trampoline so the already-installed static hardware draw hook can rebuild a
// forced eye through the flexed dynamic path without double-hooking the static
// routine.
void HardwareSkinning_SetDynamicMeshOriginal(
    HWSkinDrawDynamicMeshFn original);

// True only while R_StudioDrawEyeball is submitting an eye that has been
// explicitly routed to the hardware-skinning path. The existing hardware draw
// detour uses this to replace the eye's outer static delta-flex draw with the
// dynamic flex builder while retaining GPU bone weights.
bool HardwareSkinning_IsForcedEyeDraw();

class HardwareSkinningHooks {
public:
    static HardwareSkinningHooks& Instance() {
        static HardwareSkinningHooks instance;
        return instance;
    }

    void Initialize();
    void Shutdown();
    
    // Check if hardware skinning is enabled
    bool IsEnabled() const { return m_bEnabled; }
    void SetEnabled(bool enabled) { m_bEnabled = enabled; }
    
    // Check if bone export to Remix is enabled
    bool IsBoneExportEnabled() const { return m_bBoneExportEnabled; }
    void SetBoneExportEnabled(bool enabled) { m_bBoneExportEnabled = enabled; }

private:
    HardwareSkinningHooks() : m_bEnabled(false), m_bBoneExportEnabled(false), m_bInitialized(false) {}
    
    
    bool m_bEnabled;
    bool m_bBoneExportEnabled;
    bool m_bInitialized;
};

// Global data structure for bone data (used to pass bone transforms to Remix)
struct BoneData_t {
    int bone_count;
    VMatrix bone_matrices[512];
    bool active;  // Whether bone data is valid for current frame
};

extern BoneData_t g_BONEDATA;
