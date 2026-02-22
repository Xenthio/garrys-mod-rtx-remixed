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
    
    // Hook objects
    Detouring::Hook m_StudioDrawGroupHWSkin_hook;
    Detouring::Hook m_StudioCreateSingleMesh_hook;
    Detouring::Hook m_StudioRenderFinal_hook;
    Detouring::Hook m_StudioDrawEyeball_hook;
    
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
