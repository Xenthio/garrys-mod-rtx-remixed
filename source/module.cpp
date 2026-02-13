// Hardware skinning patches - enable to force HW skinning and bone export for RTX Remix
#define HWSKIN_PATCHES

#define DELAYIMP_INSECURE_WRITABLE_HOOKS
#ifdef _WIN32
#pragma comment(linker, "/DELAYLOAD:\"tier0.dll\"")
#include <Windows.h>
#include <DelayImp.h>
#endif

#include "GarrysMod/Lua/Interface.h"
#include "cdll_client_int.h"
#include "materialsystem/imaterialsystem.h"
#include <shaderapi/ishaderapi.h>
#include "e_utils.h"
#include <Windows.h>
#include <d3d9.h>
#include "GarrysMod/FactoryLoader.hpp"

// Only include Remix API headers in 64-bit builds
#ifdef _WIN64
#include <remix/remix.h>
#include <remix/remix_c.h>
#include "remixapi/remixapi.h"
#include "d3d9_texture_tracker.h"
#include "patch_manager.h"
#include "culling_patches.h"
#endif // _WIN64

#include "prop_fixes.h" 
#include "HardwareSkinningHooks.h" 
#include <culling_fixes.h>
#include <modelload_fixes.h>
#include <globalconvars.h>
#include "model_draw_hook.h"

#ifdef GMOD_MAIN
extern IMaterialSystem* materials = NULL;
#endif

#ifdef _WIN64
// Global pointers for Remix and Source Engine interfaces
extern IMaterialSystem* materials;
extern IMaterialSystem* materials;
remix::Interface* g_remix = nullptr;
IDirect3DDevice9Ex* g_d3dDevice = nullptr;

// Remix API present auto-instancing callback
typedef remixapi_ErrorCode (REMIXAPI_CALL* PFN_remixapi_AutoInstancePersistentLights)(void);
static PFN_remixapi_AutoInstancePersistentLights g_pfnAutoInstancePersistentLights = nullptr;
static void __stdcall RemixPresentCallback() {
    if (g_pfnAutoInstancePersistentLights) {
        g_pfnAutoInstancePersistentLights();
    }
}

#endif

using namespace GarrysMod::Lua;

// Define a proper LOG_GENERAL replacement function
void DummyLogGeneral(const char* prefix, const char* msg, ...) {
    // Basic implementation
    char buffer[1024];
    va_list args;
    va_start(args, msg);
    vsprintf_s(buffer, sizeof(buffer), msg, args);
    va_end(args);
    Msg("[LOG_GENERAL] %s: %s\n", prefix, buffer);
}

// In your delay load hook:
FARPROC WINAPI MyDelayLoadHook(unsigned dliNotify, PDelayLoadInfo pdli)
{
    if (dliNotify == dliFailGetProc) {
        if (strcmp(pdli->dlp.szProcName, "LOG_GENERAL") == 0) {
            // Return our function instead of a dummy variable
            return (FARPROC)DummyLogGeneral;
        }
    }
    return NULL;
}

// Define the hook variable
__declspec(selectany) PfnDliHook __pfnDliNotifyHook2 = MyDelayLoadHook;

// Lua function implementations for static lighting control
LUA_FUNCTION(SetForceStaticLighting_Lua) {
    try {
        Msg("[gmRTX - Binary Module] SetForceStaticLighting_Lua called\n");
        
        if (!LUA->IsType(1, GarrysMod::Lua::Type::Bool)) {
            LUA->ThrowError("Expected boolean argument for SetForceStaticLighting");
            return 0;
        }
        
        bool enable = LUA->GetBool(1);
        Msg("[gmRTX - Binary Module] Setting force static lighting to: %s\n", enable ? "true" : "false");
        
        // Use the SetForceStaticLighting function which will update both the global var and ConVar
        SetForceStaticLighting(enable);
        return 0;
    }
    catch (...) {
        Error("[gmRTX - Binary Module] Exception in SetForceStaticLighting\n");
        return 0;
    }
}

LUA_FUNCTION(GetForceStaticLighting_Lua) {
    try {
        Msg("[gmRTX - Binary Module] GetForceStaticLighting_Lua called\n");
        
        // Use the GetForceStaticLighting function which checks ConVar first
        bool enabled = GetForceStaticLighting();
        Msg("[gmRTX - Binary Module] Current force static lighting state: %s\n", enabled ? "true" : "false");
        LUA->PushBool(enabled);
        return 1;
    }
    catch (...) {
        Error("[gmRTX - Binary Module] Exception in GetForceStaticLighting\n");
        LUA->PushBool(false);
        return 1;
    }
}

LUA_FUNCTION(SetModelDrawHookEnabled_Lua) {
    try {
        Msg("[gmRTX - Binary Module] SetModelDrawHookEnabled_Lua called\n");
        
        if (!LUA->IsType(1, GarrysMod::Lua::Type::Bool)) {
            LUA->ThrowError("Expected boolean argument for SetModelDrawHookEnabled");
            return 0;
        }
        
        bool enable = LUA->GetBool(1);
        Msg("[gmRTX - Binary Module] Setting model draw hook enabled to: %s\n", enable ? "true" : "false");
        SetModelDrawHookEnabled(enable);
        return 0;
    }
    catch (...) {
        Error("[gmRTX - Binary Module] Exception in SetModelDrawHookEnabled\n");
        return 0;
    }
}

GMOD_MODULE_OPEN() { 
    try {
        Msg("[gmRTX - Binary Module] - Module loaded!\n"); 

        // Remix initialization is only available in 64-bit builds for now
#ifdef _WIN64
        // Load the Material System interface
        SourceSDK::FactoryLoader materialSystemLoader("materialsystem");
        if (materialSystemLoader.IsValid()) {
            materials = materialSystemLoader.GetInterface<IMaterialSystem>(MATERIAL_SYSTEM_INTERFACE_VERSION);
            if (materials) {
                Msg("[gmRTX - Binary Module] Material system loaded: %s\n", MATERIAL_SYSTEM_INTERFACE_VERSION);
            } else {
                Warning("[gmRTX - Binary Module] Failed to get IMaterialSystem interface\n");
            }
        } else {
            Warning("[gmRTX - Binary Module] Failed to load materialsystem.dll\n");
        }

        // Find Source's D3D9 device
        auto sourceDevice = static_cast<IDirect3DDevice9Ex*>(FindD3D9Device());
        if (!sourceDevice) {
            LUA->ThrowError("[gmRTX - Binary Module] Failed to find D3D9 device");
            return 0;
        }
        
        // Store the device globally for RemixAPI use
        g_d3dDevice = sourceDevice;

        // Initialize D3D9 texture tracker
        if (!D3D9TextureTracker::Instance().Initialize(sourceDevice)) {
            Warning("[gmRTX - Binary Module] Failed to initialize D3D9 texture tracker\n");
        }

        // Initialize Remix
        if (auto interf = remix::lib::loadRemixDllAndInitialize(L"d3d9.dll")) {
            g_remix = new remix::Interface{ *interf };
        }
        else {
            LUA->ThrowError("[gmRTX - Binary Module] - remix::loadRemixDllAndInitialize() failed"); 
        }

        g_remix->dxvk_RegisterD3D9Device(sourceDevice);

        // Initialize the new comprehensive RemixAPI
        if (!RemixAPI::RemixAPI::Instance().Initialize(g_remix, LUA)) {
            LUA->ThrowError("[gmRTX - Binary Module] Failed to initialize RemixAPI");
            return 0;
        }

        // Register native Remix API frame callbacks to submit lights (resolve dynamically)
        {
            typedef remixapi_ErrorCode (REMIXAPI_CALL* PFN_remixapi_RegisterCallbacks)(
                PFN_remixapi_BridgeCallback,
                PFN_remixapi_BridgeCallback,
                PFN_remixapi_BridgeCallback);
            HMODULE hRemix = nullptr;
            if (g_remix && g_remix->m_RemixDLL) {
                hRemix = g_remix->m_RemixDLL;
            }
            if (!hRemix) {
                hRemix = GetModuleHandleA("d3d9.dll");
            }
            if (hRemix) {
                // Resolve optional auto-instancing helper
                g_pfnAutoInstancePersistentLights = reinterpret_cast<PFN_remixapi_AutoInstancePersistentLights>(
                    GetProcAddress(hRemix, "remixapi_AutoInstancePersistentLights"));
                auto pfnRegister = reinterpret_cast<PFN_remixapi_RegisterCallbacks>(
                    GetProcAddress(hRemix, "remixapi_RegisterCallbacks"));
                if (pfnRegister) {
                    // Use present callback to auto-instance all persistent external API lights each frame
                    // Disabled: rely on explicit per-frame submissions from RemixAPI::LightManager
                    PFN_remixapi_BridgeCallback presentCb = nullptr;
                    pfnRegister(nullptr, nullptr, presentCb);
                } else {
                    Msg("[gmRTX - Binary Module] remixapi_RegisterCallbacks not found in d3d9.dll, skipping callback registration.\n");
                }
            } else {
                Msg("[gmRTX - Binary Module] d3d9.dll not loaded yet, skipping callback registration.\n");
            }
        }

        // Configure RTX settings through the new API
        auto& configManager = RemixAPI::RemixAPI::Instance().GetConfigManager();
        configManager.SetConfigVariable("rtx.enableAdvancedMode", "1");
        configManager.SetConfigVariable("rtx.fallbackLightMode", "0");

        // Register and apply runtime culling patches
        InitCullingPatches();
        RegisterCullingPatchLuaFunctions(LUA);

        // Set resource limits
        RemixAPI::RemixAPI::Instance().GetResourceManager().SetMemoryLimits(256, 1024);

        GlobalConvars::InitialiseConVars();
#endif //_WIN64

#ifdef HWSKIN_PATCHES
        HardwareSkinningHooks::Instance().Initialize();

#endif //HWSKIN_PATCHES

        ModelRenderHooks::Instance().Initialize();
        ModelLoadHooks::Instance().Initialize();
        //ModelDrawHook::Instance().Initialize();

        // Register Lua functions
        LUA->PushSpecial(GarrysMod::Lua::SPECIAL_GLOB); 

        // Static lighting control functions
        Msg("[gmRTX - Binary Module] Registering SetForceStaticLighting Lua function...\n");
        LUA->PushCFunction(SetForceStaticLighting_Lua);
        LUA->SetField(-2, "SetForceStaticLighting");

        Msg("[gmRTX - Binary Module] Registering GetForceStaticLighting Lua function...\n");
        LUA->PushCFunction(GetForceStaticLighting_Lua);
        LUA->SetField(-2, "GetForceStaticLighting");

        //Msg("[gmRTX - Binary Module] Registering SetModelDrawHookEnabled Lua function...\n");
        //LUA->PushCFunction(SetModelDrawHookEnabled_Lua);
        //LUA->SetField(-2, "SetModelDrawHookEnabled");

        // Only register Remix-related Lua functions in 64-bit builds
        #ifdef _WIN64
            // The new RemixAPI is already initialized above, which initializes
            // the unified MaterialPipeline (including HashCollisionFixer)
            // No need for separate HashCollisionFixer initialization here
        #endif // _WIN64    

        LUA->Pop();

        Msg("[gmRTX - Binary Module] Module initialization completed successfully!\n");
        return 0;
    }
    catch (...) {
        Error("[gmRTX - Binary Module] Exception in module initialization\n");
        return 0;
    }
}

GMOD_MODULE_CLOSE() {
    try {
        Msg("[gmRTX - Binary Module] Shutting down module...\n");

#ifdef _WIN64
        // Restore all runtime patches before shutdown
        PatchManager::Instance().RestoreAll();

        RemixAPI::RemixAPI::Instance().Shutdown();
        g_d3dDevice = nullptr;
#endif // _WIN64

#ifdef HWSKIN_PATCHES
        HardwareSkinningHooks::Instance().Shutdown();
#endif //HWSKIN_PATCHES

       ModelRenderHooks::Instance().Shutdown();
       ModelLoadHooks::Instance().Shutdown();
       //ModelDrawHook::Instance().Shutdown();

#ifdef _WIN64
        if (g_remix) {
            delete g_remix;
            g_remix = nullptr;
        }
#endif

        Msg("[gmRTX - Binary Module] Module shutdown complete\n");
        return 0;
    }
    catch (...) {
        Error("[gmRTX - Binary Module] Exception in module shutdown\n");
        return 0;
    }
}