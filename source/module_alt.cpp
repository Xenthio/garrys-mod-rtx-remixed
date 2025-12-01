//#define HWSKIN_PATCHES

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
#endif // _WIN64

#ifdef GMOD_MAIN
extern IMaterialSystem* materials = NULL;
#endif

#ifdef _WIN64
// Global pointers for Remix and Source Engine interfaces
IMaterialSystem* materials = nullptr;
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

        #endif // _WIN64

        // Register Lua functions
        LUA->PushSpecial(GarrysMod::Lua::SPECIAL_GLOB); 

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
        RemixAPI::RemixAPI::Instance().Shutdown();
        g_d3dDevice = nullptr;

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