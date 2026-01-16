//#define HWSKIN_PATCHES
#include "GarrysMod/Lua/Interface.h" 
#include "e_utils.h"  
#include "iclientrenderable.h"
#include "materialsystem/imaterialsystem.h"
#include "materialsystem/materialsystem_config.h"
#include "interfaces/interfaces.h"  
#include "prop_fixes.h"
#include <memory_patcher.h>
#include <globalconvars.h>
#include "icliententity.h" // Add this include to resolve the incomplete type error
#include <c_baseanimating.h>
#include "engine/ivmodelinfo.h"
#include <tier0/dbg.h>
#include <cstdio>
#include <cstdarg>

// Debug output function that works in Release builds
// Uses OutputDebugStringA which shows in VS Output window when debugging
static void DebugPrintVS(const char* format, ...) {
    char buffer[1024];
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    buffer[sizeof(buffer) - 1] = '\0';
    OutputDebugStringA(buffer);
}

// Use this macro for debug output that MUST show in VS Output window
// (Msg/Warning/Error from Source SDK may not output in Release builds)
#define DBG_PRINT(...) DebugPrintVS(__VA_ARGS__)

// Fallback definitions for Msg/Warning/Error if not defined by Source SDK
#ifndef Msg
#define Msg(...) DebugPrintVS(__VA_ARGS__)
#endif
#ifndef Warning
#define Warning(...) DebugPrintVS(__VA_ARGS__)
#endif
#ifndef Error
#define Error(...) DebugPrintVS(__VA_ARGS__)
#endif

using namespace GarrysMod::Lua;

IVModelInfo* pModelInfo = nullptr;
static StudioRenderConfig_t s_StudioRenderConfig;

// Debug logging control - set to 1 to enable verbose logging, 0 to disable
// Set to 2 for EXTREMELY verbose logging (every single call)
#define PROP_FIXES_DEBUG_LOGGING 0

#if PROP_FIXES_DEBUG_LOGGING
static int g_debugCallCount = 0;
static const int DEBUG_LOG_INTERVAL = 100; // Only log every N calls to reduce spam

// Circular buffer to track recent model names for crash diagnosis
// We COPY the strings because the original pointers may become invalid!
static const int RECENT_MODELS_COUNT = 8;
static const int MAX_MODEL_NAME_LEN = 128;
static char g_recentModels[RECENT_MODELS_COUNT][MAX_MODEL_NAME_LEN] = { {0} };
static int g_recentModelIndex = 0;
static bool g_recentModelsInitialized[RECENT_MODELS_COUNT] = { false };

static void TrackRecentModel(const char* modelName) {
    if (modelName) {
        // Safely copy the string (don't trust the source pointer length)
        __try {
            strncpy_s(g_recentModels[g_recentModelIndex], MAX_MODEL_NAME_LEN, modelName, MAX_MODEL_NAME_LEN - 1);
            g_recentModels[g_recentModelIndex][MAX_MODEL_NAME_LEN - 1] = '\0';  // Ensure null termination
            g_recentModelsInitialized[g_recentModelIndex] = true;
        }
        __except(EXCEPTION_EXECUTE_HANDLER) {
            // If copying fails, mark as invalid
            strcpy_s(g_recentModels[g_recentModelIndex], MAX_MODEL_NAME_LEN, "<invalid ptr>");
            g_recentModelsInitialized[g_recentModelIndex] = true;
        }
        g_recentModelIndex = (g_recentModelIndex + 1) % RECENT_MODELS_COUNT;
    }
}
static void DumpRecentModels() {
    for (int i = 0; i < RECENT_MODELS_COUNT; i++) {
        int idx = (g_recentModelIndex + i) % RECENT_MODELS_COUNT;
        if (g_recentModelsInitialized[idx]) {
            Msg("[PropFixes DEBUG]   [%d] %s\n", i, g_recentModels[idx]);
        }
    }
}

#if PROP_FIXES_DEBUG_LOGGING == 2
// Extremely verbose - log every call
#define DEBUG_LOG_SKINLIGHTING(...) do { \
    Msg("[PropFixes DEBUG] "); \
    Msg(__VA_ARGS__); \
} while(0)
#else
// Normal verbose - log every N calls
#define DEBUG_LOG_SKINLIGHTING(...) do { \
    if (g_debugCallCount % DEBUG_LOG_INTERVAL == 0) { \
        Msg("[PropFixes DEBUG] "); \
        Msg(__VA_ARGS__); \
    } \
} while(0)
#endif

// ALWAYS version logs every time but does NOT dump models (safer in exception contexts)
#define DEBUG_LOG_SKINLIGHTING_ALWAYS(...) do { \
    Msg("[PropFixes DEBUG] "); \
    Msg(__VA_ARGS__); \
} while(0)

// Explicit function to dump recent models - call this when safe to do so
#define DEBUG_DUMP_RECENT_MODELS() DumpRecentModels()
#else
#define DEBUG_LOG_SKINLIGHTING(...) ((void)0)
#define DEBUG_LOG_SKINLIGHTING_ALWAYS(...) ((void)0)
#define DEBUG_DUMP_RECENT_MODELS() ((void)0)
#define TrackRecentModel(x) ((void)0)
#endif

// Set to 1 to enable bone-count filtering (skip HW lighting on multi-bone models)
// Set to 0 to just use the convar override without any model inspection
#define PROP_FIXES_ENABLE_BONE_FILTERING 1

// Re-entrancy guard - prevents crashes when GetModel() triggers more rendering
static thread_local bool g_inSkinLightingHook = false;

// Thread-local to pass lighting decision from SetupSkinAndLighting to DrawDynamicMesh
// -1 = not set, 0 = LIGHTING_HARDWARE, 1+ = software lighting
static thread_local int g_desiredLightingForMesh = -1;

// Helper function to get bone count from a renderable (returns -1 on error)
// Also returns model name via outModelName if provided (for debugging)
static int GetBoneCountForRenderable(IClientRenderable* pClientRenderable, IVModelInfo* pModelInfo, const char** outModelName = nullptr) {
    if (outModelName) *outModelName = nullptr;
    
    if (!pClientRenderable || !pModelInfo) {
        return -1;
    }
    
    // Validate the pointer
    __try {
        if (((uintptr_t)pClientRenderable < 0x10000) || 
            ((uintptr_t)pClientRenderable > 0x7FFFFFFFFFFF) || 
            ((uintptr_t)pClientRenderable & 0x7) != 0) {
            return -1;
        }
        
        void** vtable = *(void***)pClientRenderable;
        if (!vtable || ((uintptr_t)vtable < 0x10000) || 
            ((uintptr_t)vtable > 0x7FFFFFFFFFFF)) {
            return -1;
        }
            }
            __except(EXCEPTION_EXECUTE_HANDLER) {
        return -1;
            }
            
    // Get the model
                const model_t* mdl = nullptr;
                __try {
                    mdl = pClientRenderable->GetModel();
                }
                __except(EXCEPTION_EXECUTE_HANDLER) {
        return -1;
                }

                if (!mdl) {
        return 0;  // No model = treat as simple
    }
    
    // Get model name for debugging
    if (outModelName) {
                        __try {
            *outModelName = pModelInfo->GetModelName(mdl);
                        }
                        __except(EXCEPTION_EXECUTE_HANDLER) {
            *outModelName = nullptr;
                    }
                }

    // Get studio header
                    const studiohdr_t* pStudioHdr = nullptr;
                    __try {
                        pStudioHdr = pModelInfo->GetStudiomodel(mdl);
                    }
                    __except(EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
    
    if (!pStudioHdr) {
        return 0;  // No studio header = treat as simple (might be brush model)
    }
    
    return pStudioHdr->numbones;
                    }

// Debug counters for bone count distribution
#if PROP_FIXES_DEBUG_LOGGING
static int g_boneCountStats[5] = {0};  // [-1/error, 0, 1, 2+, reentrant]
static int g_lastStatsDump = 0;
static bool g_firstCallLogged = false;
#endif

Define_method_Hook(IMaterial*, R_StudioSetupSkinAndLighting, void*, IMatRenderContext* pRenderContext, int index, IMaterial** ppMaterials, int materialFlags,
    IClientRenderable* pClientRenderable, void* pColorMeshes, int &lighting)
{ 
#if PROP_FIXES_DEBUG_LOGGING
    g_debugCallCount++;
    
    // Log once on first call to confirm hook is working
    if (!g_firstCallLogged) {
        g_firstCallLogged = true;
        DBG_PRINT("[PropFixes] pModelInfo = %p\n", pModelInfo);
        DBG_PRINT("[PropFixes] pClientRenderable = %p\n", pClientRenderable);
        DBG_PRINT("[PropFixes] BONE_FILTERING = %d\n", PROP_FIXES_ENABLE_BONE_FILTERING);
#ifdef _WIN64
        DBG_PRINT("[PropFixes] r_forcehwlight convar = %p\n", GlobalConvars::r_forcehwlight);
#endif
    }
#endif

    // Re-entrancy guard - if we're already in this hook, just call trampoline
    if (g_inSkinLightingHook) {
#if PROP_FIXES_DEBUG_LOGGING
        g_boneCountStats[4]++;  // reentrant
#endif
        return R_StudioSetupSkinAndLighting_trampoline()(_this, pRenderContext, index, ppMaterials, materialFlags, pClientRenderable, pColorMeshes, lighting);
    }
    
    // Set re-entrancy guard
    g_inSkinLightingHook = true;
    
    // CRITICAL: Call the trampoline FIRST - it determines and outputs the lighting value
    // The caller uses this output for subsequent DrawDynamicMesh calls
    IMaterial* pMaterial = R_StudioSetupSkinAndLighting_trampoline()(_this, pRenderContext, index, ppMaterials, materialFlags, pClientRenderable, pColorMeshes, lighting);
    
    // Now we can override the lighting OUTPUT after the trampoline has run
    // This is the pattern that worked in the old code
    
#ifdef _WIN64
    // Handle global convar override if it exists
    if (GlobalConvars::r_forcehwlight && GlobalConvars::r_forcehwlight->GetBool()) {
        // Force HW lighting on everything - user requested via convar
                        lighting = 0; // LIGHTING_HARDWARE 
        g_desiredLightingForMesh = 0;
        g_inSkinLightingHook = false;
        return pMaterial;
    }
#endif

#if PROP_FIXES_ENABLE_BONE_FILTERING
    // Only force LIGHTING_HARDWARE on simple models (single bone or no bones)
    const char* modelName = nullptr;
    int boneCount = GetBoneCountForRenderable(pClientRenderable, pModelInfo, &modelName);
    
#if PROP_FIXES_DEBUG_LOGGING
    // Track bone count distribution
    if (boneCount < 0) g_boneCountStats[0]++;
    else if (boneCount == 0) g_boneCountStats[1]++;
    else if (boneCount == 1) g_boneCountStats[2]++;
    else g_boneCountStats[3]++;
    
    // Dump stats every 1000 calls
    if (g_debugCallCount - g_lastStatsDump >= 1000) {
        g_lastStatsDump = g_debugCallCount;
        DBG_PRINT("[PropFixes] Stats after %d calls: errors=%d, 0bones=%d, 1bone=%d, 2+bones=%d, reentrant=%d\n",
            g_debugCallCount, g_boneCountStats[0], g_boneCountStats[1], g_boneCountStats[2], g_boneCountStats[3], g_boneCountStats[4]);
    }
    
    // Log individual calls occasionally
    if (g_debugCallCount % 200 == 0 && modelName) {
        DBG_PRINT("[PropFixes] Call #%d: model=%s, bones=%d, trampoline_lighting=%d\n", 
            g_debugCallCount, modelName, boneCount, lighting);
    }
#endif
    
    if (boneCount >= 0 && boneCount <= 1) {
        // Simple model - override lighting AFTER trampoline (this is what makes it work!)
#if PROP_FIXES_DEBUG_LOGGING
        if (lighting != 0 && g_debugCallCount % 200 == 0) {
            DBG_PRINT("[PropFixes] Overriding lighting %d -> 0 for simple model (bones=%d)\n", lighting, boneCount);
        }
#endif
        lighting = 0; // LIGHTING_HARDWARE
        g_desiredLightingForMesh = 0;
    } else {
        // Multi-bone model or error - keep trampoline's lighting decision
        g_desiredLightingForMesh = lighting;
    }
#else
    g_desiredLightingForMesh = lighting;
#endif // PROP_FIXES_ENABLE_BONE_FILTERING

#ifdef HWSKIN_PATCHES
    // Always force hardware lighting when specifically enabled by define
    lighting = 0; // LIGHTING_HARDWARE 
    g_desiredLightingForMesh = 0;
#endif

    g_inSkinLightingHook = false;
    return pMaterial;
}

#if PROP_FIXES_DEBUG_LOGGING
static int g_dynamicMeshCallCount = 0;
#endif

Define_method_Hook(int*, R_StudioDrawDynamicMesh, void*, IMatRenderContext* pRenderContext, mstudiomesh_t* pmesh,
	studiomeshgroup_t* pGroup, int lighting,
	float r_blend, IMaterial* pMaterial, int lod)
{ 
	// Use the lighting value set by SetupSkinAndLighting if available
	int actualLighting = lighting;
	if (g_desiredLightingForMesh >= 0) {
		actualLighting = g_desiredLightingForMesh;
	}
	
#ifdef _WIN64
	// Also check convar override
	if (GlobalConvars::r_forcehwlight && GlobalConvars::r_forcehwlight->GetBool()) {
		actualLighting = 0; // LIGHTING_HARDWARE
	}
#endif

#if PROP_FIXES_DEBUG_LOGGING
	g_dynamicMeshCallCount++;
	if (g_dynamicMeshCallCount % DEBUG_LOG_INTERVAL == 0) {
		DBG_PRINT("[PropFixes DEBUG] DrawDynamicMesh #%d: lighting=%d->%d, pmesh=%p, pMaterial=%p\n",
			g_dynamicMeshCallCount, lighting, actualLighting, pmesh, pMaterial);
	}
#endif

	int* returncode = nullptr;
	__try {
		returncode = R_StudioDrawDynamicMesh_trampoline()(_this, pRenderContext, pmesh, pGroup, actualLighting, r_blend, pMaterial, lod);
	}
	__except(EXCEPTION_EXECUTE_HANDLER) {
		// Crash in trampoline - just return nullptr silently
		return nullptr;
	}
	
	return returncode;
}
 
void ModelRenderHooks::Initialize() {
	try { 

		// config stuff
		//Sys_LoadInterface CRASHES for some reason on win32?????
#ifdef _WIN32
		Msg("[PropFixes] Loading studiorender.dll...\n");

		HMODULE studiorenderLib = LoadLibraryA("studiorender.dll");
		if (!studiorenderLib) {
			Warning("[PropFixes] FAILED to load studiorender.dll: error code %d\n", GetLastError());
			return;
		}
		Msg("[PropFixes] studiorender.dll loaded at %p\n", studiorenderLib);

		using CreateInterfaceFn = void* (*)(const char* pName, int* pReturnCode);
		CreateInterfaceFn createInterface = (CreateInterfaceFn)GetProcAddress(studiorenderLib, "CreateInterface");
		if (!createInterface) {
			Warning("[PropFixes] Could not get CreateInterface from studiorender.dll\n");
			return;
		}
		Msg("[PropFixes] Got CreateInterface from studiorender.dll\n");
		
		g_pStudioRender = (IStudioRender*)createInterface(STUDIO_RENDER_INTERFACE_VERSION, nullptr);
		Msg("[PropFixes] g_pStudioRender = %p (interface: %s)\n", g_pStudioRender, STUDIO_RENDER_INTERFACE_VERSION);

		Msg("[PropFixes] Loading engine.dll...\n");
		HMODULE engineLib = LoadLibraryA("engine.dll");
		if (!engineLib) {
			Warning("[PropFixes] FAILED to load engine.dll: error code %d\n", GetLastError());
			return;
		}
		Msg("[PropFixes] engine.dll loaded at %p\n", engineLib);

		CreateInterfaceFn createEngineInterface = (CreateInterfaceFn)GetProcAddress(engineLib, "CreateInterface");
		if (!createEngineInterface) {
			Warning("[PropFixes] Could not get CreateInterface from engine.dll\n");
			return;
		}
		
		pModelInfo = (IVModelInfo*)createEngineInterface(VMODELINFO_CLIENT_INTERFACE_VERSION, nullptr);
		if (!pModelInfo) {
			Warning("[PropFixes] Could not get IVModelInfo interface\n");
			return;
		}
		Msg("[PropFixes] pModelInfo = %p (interface: %s)\n", pModelInfo, VMODELINFO_CLIENT_INTERFACE_VERSION);
#else
		Msg("[PropFixes] Loading studiorender (64-bit path)...\n");
		if (!Sys_LoadInterface("studiorender", STUDIO_RENDER_INTERFACE_VERSION, NULL, (void**)&g_pStudioRender))
			Warning("[PropFixes] Could not load studiorender interface");
		Msg("[PropFixes] g_pStudioRender = %p\n", g_pStudioRender);

		if (!Sys_LoadInterface("engine", VMODELINFO_CLIENT_INTERFACE_VERSION, NULL, (void**)&pModelInfo))
			Warning("[PropFixes] Could not load IVModelInfo interface");
		Msg("[PropFixes] pModelInfo = %p\n", pModelInfo);
#endif

#ifdef _WIN32
		// Use direct vtable call to avoid calling convention issues
		if (g_pStudioRender) {
			Msg("[PropFixes] Configuring StudioRender...\n");
			typedef void(__thiscall* GetConfigFn)(void*, StudioRenderConfig_t&);
			void** vtable = *reinterpret_cast<void***>(g_pStudioRender);
			Msg("[PropFixes] StudioRender vtable at %p\n", vtable);
			
			GetConfigFn GetConfig = reinterpret_cast<GetConfigFn>(vtable[9]); // GetCurrentConfig at index 9
			Msg("[PropFixes] GetCurrentConfig at vtable[9] = %p\n", GetConfig);
			GetConfig(g_pStudioRender, s_StudioRenderConfig);
			Msg("[PropFixes] Got current config\n");

			s_StudioRenderConfig.bSoftwareSkin = false;
			s_StudioRenderConfig.bSoftwareLighting = false;
			s_StudioRenderConfig.bDrawNormals = false;
			s_StudioRenderConfig.bDrawTangentFrame = false;
			//s_StudioRenderConfig.bFlex = false;

			// Similarly for UpdateConfig
			typedef void(__thiscall* UpdateConfigFn)(void*, const StudioRenderConfig_t&);
			UpdateConfigFn UpdateConfig = reinterpret_cast<UpdateConfigFn>(vtable[8]); // UpdateConfig at index 8
			Msg("[PropFixes] UpdateConfig at vtable[8] = %p\n", UpdateConfig);
			UpdateConfig(g_pStudioRender, s_StudioRenderConfig);
			Msg("[PropFixes] StudioRender config updated (bSoftwareSkin=false, bSoftwareLighting=false)\n");
		}
#else
		// 64-bit code remains unchanged
		if (g_pStudioRender) {
			Msg("[PropFixes] Configuring StudioRender (64-bit path)...\n");
			g_pStudioRender->GetCurrentConfig(s_StudioRenderConfig);
			s_StudioRenderConfig.bSoftwareSkin = false;
			s_StudioRenderConfig.bSoftwareLighting = false;
			s_StudioRenderConfig.bDrawNormals = false;
			s_StudioRenderConfig.bDrawTangentFrame = false;
			//s_StudioRenderConfig.bFlex = false;
			g_pStudioRender->UpdateConfig(s_StudioRenderConfig);
			Msg("[PropFixes] StudioRender config updated\n");
		}
#endif

		// end config stuff

		Msg("[PropFixes] Setting up function hooks...\n");
		auto studiorenderdll = GetModuleHandleA("studiorender.dll");
		if (!studiorenderdll) { 
			Msg("[PropFixes] ERROR: studiorender.dll module handle is NULL!\n"); 
			return;
		}
		Msg("[PropFixes] studiorender.dll module at %p\n", studiorenderdll);

#ifdef _WIN64
		static const char R_StudioSetupSkinAndLighting_sign[] = "48 89 54 24 ? 48 89 4C 24 ? 55 56";
		Msg("[PropFixes] Using 64-bit signature for R_StudioSetupSkinAndLighting\n");
#else
		static const char R_StudioSetupSkinAndLighting_sign[] = "55 8B EC 83 EC 18 8B C1";
		Msg("[PropFixes] Using 32-bit signature for R_StudioSetupSkinAndLighting\n");
#endif
#ifdef _WIN64
		static const char R_StudioDrawDynamicMesh_sign[] = "40 55 53 57 41 54 41 55 41 56 41 57";
		Msg("[PropFixes] Using 64-bit signature for R_StudioDrawDynamicMesh\n");
#else
		static const char R_StudioDrawDynamicMesh_sign[] = "55 8B EC 81 EC F8 01 00 00";
		Msg("[PropFixes] Using 32-bit signature for R_StudioDrawDynamicMesh\n");
#endif
#ifdef _WIN64
		static const char R_StudioDrawStaticMesh_sign[] = "40 55 53 56 57 41 54 41 55 41 56 41 57 48 8D AC 24";
#else
		static const char R_StudioDrawStaticMesh_sign[] = "55 8B EC 81 EC EC 01 00 00 83 7D";
#endif

		Msg("[PropFixes] Scanning for R_StudioSetupSkinAndLighting...\n");
		auto R_StudioSetupSkinAndLighting = ScanSign(studiorenderdll, R_StudioSetupSkinAndLighting_sign, sizeof(R_StudioSetupSkinAndLighting_sign) - 1);
		Msg("[PropFixes] R_StudioSetupSkinAndLighting found at: %p\n", R_StudioSetupSkinAndLighting);
		
		Msg("[PropFixes] Scanning for R_StudioDrawDynamicMesh...\n");
		auto R_StudioDrawDynamicMesh = ScanSign(studiorenderdll, R_StudioDrawDynamicMesh_sign, sizeof(R_StudioDrawDynamicMesh_sign) - 1);
		Msg("[PropFixes] R_StudioDrawDynamicMesh found at: %p\n", R_StudioDrawDynamicMesh);

		if (!R_StudioSetupSkinAndLighting) { 
			Msg("[PropFixes] ERROR: R_StudioSetupSkinAndLighting signature not found!\n"); 
			return; 
		}
		if (!R_StudioDrawDynamicMesh) { 
			Msg("[PropFixes] ERROR: R_StudioDrawDynamicMesh signature not found!\n"); 
			return; 
		}

		Msg("[PropFixes] Installing hook for R_StudioSetupSkinAndLighting...\n");
		Setup_Hook(R_StudioSetupSkinAndLighting, R_StudioSetupSkinAndLighting)
		Msg("[PropFixes] Hook installed for R_StudioSetupSkinAndLighting\n");
		
		Msg("[PropFixes] Installing hook for R_StudioDrawDynamicMesh...\n");
		Setup_Hook(R_StudioDrawDynamicMesh, R_StudioDrawDynamicMesh)
		Msg("[PropFixes] Hook installed for R_StudioDrawDynamicMesh\n");

		//MaterialSystem_Config_t cfg = materials->GetCurrentConfigForVideoCard();
		//cfg.bSoftwareLighting = false;
		//materials->OverrideConfig(cfg, true);

		HMODULE studiorenderModule = GetModuleHandleEx("studiorender.dll");

		// hardware skin patch 1, override pColorMeshes 
		// can fatally crash, disabled

#ifdef _WIN64
		//g_MemoryPatcher.FindAndPatch(
		//	"ForceHardwareSkinning1",
		//	studiorenderModule,
		//	"75 ?? 48 8B 41 ?? F6 40",
		//	"90",
		//	"Force models to use Hardware Skinning (1/2)"
		//);
#else
		//g_MemoryPatcher.FindAndPatch(
		//	"ForceHardwareSkinning1",
		//	studiorenderModule,
		//	"75 ?? 8B 46 ?? F6 40",
		//	"90",
		//	"Force models to use Hardware Skinning (1/2)"
		//);
#endif


#ifdef _WIN64
		// hardware skin patch 2, overrides the first jnz to jump after pColorMeshes is checked
		//g_MemoryPatcher.FindAndPatch(
		//	"ForceHardwareSkinning2",
		//	studiorenderModule,
		//	"75 ?? F6 40 ?? ?? 75",
		//	"EB",
		//	"Force models to use Hardware Skinning (2/2)"
		//);
#endif

#ifdef _WIN64
		//g_MemoryPatcher.FindAndPatch(
		//	"ForceStaticModel1",
		//	studiorenderModule,
		//	"75 ?? 84 C0 75",
		//	"90",
		//	"Force models to use static meshes (1/2)"
		//);
#else
		//g_MemoryPatcher.FindAndPatch(
		//	"ForceStaticModel1",
		//	studiorenderModule,
		//	"75 ?? 84 C9 75 ?? D9 45",
		//	"90",
		//	"Force models to use static meshes (1/2)"
		//);
#endif

#ifdef _WIN64
		//g_MemoryPatcher.FindAndPatch(
		//	"ForceStaticModel2",
		//	studiorenderModule,
		//	"75 ?? 8B 85 ?? ?? ?? ?? 33 F6",
		//	"90",
		//	"Force models to use static meshes (2/2)"
		//);
#else
		//g_MemoryPatcher.FindAndPatch(
		//	"ForceStaticModel2",
		//	studiorenderModule,
		//	"75 ?? D9 45 ?? 6A 00",
		//	"90",
		//	"Force models to use static meshes (2/2)"
		//);
#endif

	}
	catch (...) {
		Msg("[Prop Fixes] Exception in ModelRenderHooks::Initialize\n");
	}
}

// Old Shutdown Code

// void ModelRenderHooks::Shutdown() { 
// 	// Existing shutdown code  
// 	R_StudioSetupSkinAndLighting_hook.Disable();
// 	R_StudioDrawDynamicMesh_hook.Disable();
// #ifdef _WIN64
// 	//g_MemoryPatcher.DisablePatch("ForceHardwareSkinning2");
// 	//g_MemoryPatcher.DisablePatch("ForceStaticModel1");
// #endif

// 	// Log shutdown completion
// 	Msg("[Prop Fixes] Shutdown complete\n");
// }

void ModelRenderHooks::Shutdown() { 
    try {
        // Safely disable hooks
        if (R_StudioSetupSkinAndLighting_hook.IsEnabled())
            R_StudioSetupSkinAndLighting_hook.Disable();
        
        if (R_StudioDrawDynamicMesh_hook.IsEnabled())
            R_StudioDrawDynamicMesh_hook.Disable();
        
        // Reset interface pointers
        g_pStudioRender = nullptr;
        pModelInfo = nullptr;
        
        Msg("[Prop Fixes] Shutdown complete\n");
    }
    catch (...) {
        Error("[Prop Fixes] Exception during shutdown\n");
    }
}