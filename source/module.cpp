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
#include "material_pipeline/material_pipeline.h"

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

namespace {
    ConCommand* g_flushTexturesCommand = nullptr;
    ConCommand* g_vramStatsCommand = nullptr;
    ICvar* g_commandCvar = nullptr;
    bool g_moduleClosing = false;

    void PrintVramStats() {
        remixapi_VramStats stats = {};
        if (!RemixAPI::RemixAPI::Instance().GetResourceManager().GetVramStats(stats)) {
            Warning("[RTX Texture Cleanup] Unable to read Remix VRAM stats\n");
            return;
        }

        constexpr double bytesPerMiB = 1024.0 * 1024.0;
        Msg("[RTX VRAM] driver %.1f / %.1f MiB, DXVK allocated %.1f MiB, used %.1f MiB, retained %.1f MiB\n",
            stats.driverAllocatedBytes / bytesPerMiB,
            stats.driverBudgetBytes / bytesPerMiB,
            stats.totalAllocatedBytes / bytesPerMiB,
            stats.totalUsedBytes / bytesPerMiB,
            stats.poolRetainedBytes / bytesPerMiB);
        Msg("[RTX VRAM] replacement textures %.1f MiB, geometry %.1f MiB, buffers %.1f MiB, AS %.1f MiB, OMM %.1f MiB, render targets %.1f MiB, texture cache %u\n",
            stats.usedMaterialTextureBytes / bytesPerMiB,
            stats.usedReplacementGeometryBytes / bytesPerMiB,
            stats.usedBufferBytes / bytesPerMiB,
            stats.usedAccelerationStructureBytes / bytesPerMiB,
            stats.usedOpacityMicromapBytes / bytesPerMiB,
            stats.usedRenderTargetBytes / bytesPerMiB,
            stats.forkTextureCacheCount);
        Msg("[RTX VRAM] D3D9 app textures %.1f MiB, D3D9 app buffers %.1f MiB\n",
            stats.usedAppTextureBytes / bytesPerMiB,
            stats.usedAppBufferBytes / bytesPerMiB);
    }

    bool PerformTextureCleanup(const char* reason, bool honorMapExitConvar) {
        if (g_moduleClosing) {
            return false;
        }
        if (honorMapExitConvar
            && GlobalConvars::rtx_flush_textures_on_map_exit
            && !GlobalConvars::rtx_flush_textures_on_map_exit->GetBool()) {
            Msg("[RTX Texture Cleanup] Skipping map-exit purge because rtx_flush_textures_on_map_exit is disabled\n");
            return false;
        }

        Msg("[RTX Texture Cleanup] Starting cleanup (%s)\n", reason);
        Msg("[RTX Texture Cleanup] VRAM before Source material purge:\n");
        PrintVramStats();

        // Lua ShutDown can overlap queued material work. Drain it before releasing
        // pipeline or Source references; otherwise an in-flight shader draw can use
        // a material while UncacheAllMaterials tears it down.
        if (materials) {
            materials->Flush(true);
        }

        // Release pipeline-owned D3D9 references before asking Source to discard
        // every cached material and its texture references. Materials are lazily
        // precached again when Source binds them in the next map.
        MaterialPipeline::Pipeline::Instance().ClearMapState();
        if (materials) {
            materials->UncacheAllMaterials();
            Msg("[RTX Texture Cleanup] Source material cache fully uncached\n");
        } else {
            Warning("[RTX Texture Cleanup] Material system is unavailable; Source texture cleanup was skipped\n");
        }

        const bool requested = RemixAPI::RemixAPI::Instance().GetResourceManager().ForceCleanup();
        if (requested) {
            Msg("[RTX Texture Cleanup] Renderer purge queued for the next render-thread tick\n");
        }
        return requested;
    }

    void FlushTexturesCommand(const CCommand&) {
        PerformTextureCleanup("manual console command", false);
    }

    void VramStatsCommand(const CCommand&) {
        PrintVramStats();
    }

    void RegisterTextureCleanupCommands() {
        g_commandCvar = cvar;
        if (!g_commandCvar) {
            static SourceSDK::FactoryLoader vstdlibLoader("vstdlib");
            g_commandCvar = vstdlibLoader.GetInterface<ICvar>(CVAR_INTERFACE_VERSION);
        }
        if (!g_commandCvar) {
            Warning("[RTX Texture Cleanup] Failed to resolve %s; console commands were not registered\n",
                CVAR_INTERFACE_VERSION);
            return;
        }

        if (!g_commandCvar->FindCommand("rtx_flush_textures")) {
            g_flushTexturesCommand = new ConCommand(
                "rtx_flush_textures", FlushTexturesCommand,
                "Fully uncache Source materials and purge texture VRAM; textures reload lazily and may hitch");
            g_commandCvar->RegisterConCommand(g_flushTexturesCommand);
        }
        if (!g_commandCvar->FindCommand("rtx_vram_stats")) {
            g_vramStatsCommand = new ConCommand(
                "rtx_vram_stats", VramStatsCommand,
                "Print RTX Remix VRAM usage by category");
            g_commandCvar->RegisterConCommand(g_vramStatsCommand);
        }
        Msg("[RTX Texture Cleanup] Console commands registered through %s\n",
            CVAR_INTERFACE_VERSION);
    }

    void UnregisterTextureCleanupCommands() {
        if (g_commandCvar && g_flushTexturesCommand) {
            g_commandCvar->UnregisterConCommand(g_flushTexturesCommand);
            delete g_flushTexturesCommand;
            g_flushTexturesCommand = nullptr;
        }
        if (g_commandCvar && g_vramStatsCommand) {
            g_commandCvar->UnregisterConCommand(g_vramStatsCommand);
            delete g_vramStatsCommand;
            g_vramStatsCommand = nullptr;
        }
        g_commandCvar = nullptr;
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

#ifdef _WIN64
LUA_FUNCTION(ReloadModelsAfterHardwareSkinningSettings_Lua) {
    try {
        ModelLoadHooks::Instance().ReloadModelsAfterSettings();
        return 0;
    }
    catch (...) {
        Error("[gmRTX - Model Load Fixes] Exception while reloading models after hardware-skinning settings\n");
        return 0;
    }
}

LUA_FUNCTION(CleanupMapTextures_Lua) {
    try {
        LUA->PushBool(PerformTextureCleanup("client Lua ShutDown", true));
        return 1;
    }
    catch (...) {
        Error("[RTX Texture Cleanup] Exception in map-exit cleanup\n");
        LUA->PushBool(false);
        return 1;
    }
}
#endif

GMOD_MODULE_OPEN() { 
    try {
#ifdef _WIN64
        g_moduleClosing = false;
#endif
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
        RegisterTextureCleanupCommands();
#endif //_WIN64

#if defined(HWSKIN_PATCHES) && defined(_WIN64)
        HardwareSkinningHooks::Instance().Initialize();
#endif

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

#ifdef _WIN64
        Msg("[gmRTX - Binary Module] Registering RTX_CleanupMapTextures Lua function...\n");
        LUA->PushCFunction(CleanupMapTextures_Lua);
        LUA->SetField(-2, "RTX_CleanupMapTextures");

        Msg("[gmRTX - Binary Module] Registering RTX_ReloadModelsAfterHardwareSkinningSettings Lua function...\n");
        LUA->PushCFunction(ReloadModelsAfterHardwareSkinningSettings_Lua);
        LUA->SetField(-2, "RTX_ReloadModelsAfterHardwareSkinningSettings");
#endif

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
        g_moduleClosing = true;
        UnregisterTextureCleanupCommands();

        // Restore all runtime patches before shutdown
        PatchManager::Instance().RestoreAll();

        RemixAPI::RemixAPI::Instance().Shutdown();
        g_d3dDevice = nullptr;
#endif // _WIN64

#if defined(HWSKIN_PATCHES) && defined(_WIN64)
        HardwareSkinningHooks::Instance().Shutdown();
#endif

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
