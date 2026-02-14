#ifdef _WIN64

#include "culling_patches.h"
#include "patch_manager.h"
#include "e_utils.h"
#include "tier1/convar.h"
#include <GarrysMod/FactoryLoader.hpp>
#include <GarrysMod/InterfacePointers.hpp>
#include <GarrysMod/Lua/Interface.h>
#include <GarrysMod/Lua/LuaShared.h>
#include <GarrysMod/Lua/LuaConVars.h>
#include "interfaces/interfaces.h"
#include <tier0/dbg.h>

// ===================================================================
//      CONVAR REGISTRATION (via ILuaConVars, like garrys-mod-rtx-remixed)
// ===================================================================

static SourceSDK::FactoryLoader loader_lua_shared("lua_shared");
static GarrysMod::Lua::ILuaConVars* g_pLuaConVars = nullptr;

static ConVar* cv_frustumcull_engine = nullptr;
static ConVar* cv_brush_backfaces = nullptr;
static ConVar* cv_world_backfaces1 = nullptr;
static ConVar* cv_world_backfaces2 = nullptr;
static ConVar* cv_cullnode = nullptr;
static ConVar* cv_frustumcull_client = nullptr;
static ConVar* cv_forcenovis = nullptr;

static ConVar* CreatePatchConVar(const char* name, const char* defaultVal, const char* help) {
	ConVar* cv = nullptr;

	if (g_pLuaConVars) {
		cv = g_pLuaConVars->CreateConVar(name, defaultVal, help, FCVAR_ARCHIVE);
	}

	if (!cv && cvar) {
		cv = cvar->FindVar(name);
	}

	// Last resort: construct + register directly via ICvar
	if (!cv && cvar) {
		cv = new ConVar(name, defaultVal, FCVAR_ARCHIVE, help);
		cvar->RegisterConCommand(cv);
	}

	if (cv) {
		Msg("[PatchManager] ConVar '%s' ready\n", name);
	} else {
		Warning("[PatchManager] Failed to create ConVar '%s'\n", name);
	}
	return cv;
}

static void RegisterPatchConVars() {
	g_pLuaConVars = loader_lua_shared.GetInterface<GarrysMod::Lua::ILuaConVars>(GMOD_LUACONVARS_INTERFACE);
	if (!g_pLuaConVars) {
		Warning("[PatchManager] Failed to get ILuaConVars interface (lua_shared)\n");
		// Continue anyway - patches will still work, just no ConVar toggling
	} else {
		Msg("[PatchManager] Got ILuaConVars interface\n");
	}

	cv_frustumcull_engine = CreatePatchConVar("rtx_patch_frustumcull_engine", "1", "Disable engine frustum culling");
	cv_brush_backfaces    = CreatePatchConVar("rtx_patch_brush_backfaces", "1", "Force render brush entity backfaces");
	cv_world_backfaces1   = CreatePatchConVar("rtx_patch_world_backfaces1", "1", "Force render world backfaces #1");
	cv_world_backfaces2   = CreatePatchConVar("rtx_patch_world_backfaces2", "1", "Force render world backfaces #2");
	cv_cullnode           = CreatePatchConVar("rtx_patch_cullnode", "1", "Disable R_CullNode BSP culling");
	cv_frustumcull_client = CreatePatchConVar("rtx_patch_frustumcull_client", "1", "Disable client frustum culling");
	cv_forcenovis         = CreatePatchConVar("rtx_patch_forcenovis", "1", "Force r_novis (disable PVS culling)");

	Msg("[PatchManager] ConVar registration complete\n");
}

// ===================================================================
//      PATCH DEFINITIONS
// ===================================================================

static void RegisterCullingPatches() {
	auto& pm = PatchManager::Instance();

	// engine.dll patches (from applypatch.py patches64)
	{
		// c_frustumcull - uses sse instructions in 64bit
		static const char sig[] = "48 83 EC 48 0F 10";
		pm.RegisterPatch("rtx_patch_frustumcull_engine", "engine.dll",
			sig, sizeof(sig) - 1, 0, {0x31, 0xC0, 0xC3}); // xor eax,eax; ret
	}
	{
		// brush entity backfaces
		static const char sig[] = "75 3C F3 0F 10";
		pm.RegisterPatch("rtx_patch_brush_backfaces", "engine.dll",
			sig, sizeof(sig) - 1, 0, {0xEB}); // jmp (unconditional)
	}
	{
		// world backfaces #1
		// PATCH DOCS: in R_DrawLeaf, search for "CBitVec invalid set bitNum" string 
		// Working as of 2026-02-14, cross reference gmod as of that date if you need to refind
		static const char sig[] = "7E ? 8B EF";
		pm.RegisterPatch("rtx_patch_world_backfaces1", "engine.dll",
			sig, sizeof(sig) - 1, 0, {0xEB}); // jmp (unconditional)
	}
	{
		// world backfaces #2
		// PATCH DOCS: in R_DrawLeaf, search for "CBitVec invalid set bitNum" string, but a little lower in the function
		// Working as of 2026-02-14, cross reference gmod as of that date if you need to refind
		static const char sig[] = "75 ? 48 8B 45 ? F3 0F 10 15";
		pm.RegisterPatch("rtx_patch_world_backfaces2", "engine.dll",
			sig, sizeof(sig) - 1, 0, {0xEB}); // jmp (unconditional)
	}
	{
		// R_CullNode - wildcard bytes use single ? per byte in ScanSign format
		static const char sig[] = "48 83 EC 48 80 3D ? ? ? ? ? 48";
		pm.RegisterPatch("rtx_patch_cullnode", "engine.dll",
			sig, sizeof(sig) - 1, 0, {0x31, 0xC0, 0xC3}); // xor eax,eax; ret
	}

	// client.dll patches (from applypatch.py patches64)
	{
		// c_frustumcull
		static const char sig[] = "48 83 EC 48 0F 10 22";
		pm.RegisterPatch("rtx_patch_frustumcull_client", "client.dll",
			sig, sizeof(sig) - 1, 0, {0x31, 0xC0, 0xC3}); // xor eax,eax; ret
	}
	{
		// r_forcenovis [getter] - force return 1
		static const char sig[] = "0F B6 81 54";
		pm.RegisterPatch("rtx_patch_forcenovis", "client.dll",
			sig, sizeof(sig) - 1, 0, {0xB0, 0x01, 0xC3}); // mov al,1; ret
	}

	pm.ResolveAll();
}

// ===================================================================
//      APPLY INITIAL PATCHES
// ===================================================================

static void ApplyInitialPatches() {
	auto& pm = PatchManager::Instance();

	// Apply patches based on ConVar values (default to enabled if ConVar failed to create)
	if (!cv_frustumcull_engine || cv_frustumcull_engine->GetBool()) pm.ApplyPatch("rtx_patch_frustumcull_engine");
	if (!cv_brush_backfaces || cv_brush_backfaces->GetBool()) pm.ApplyPatch("rtx_patch_brush_backfaces");
	if (!cv_world_backfaces1 || cv_world_backfaces1->GetBool()) pm.ApplyPatch("rtx_patch_world_backfaces1");
	if (!cv_world_backfaces2 || cv_world_backfaces2->GetBool()) pm.ApplyPatch("rtx_patch_world_backfaces2");
	if (!cv_cullnode || cv_cullnode->GetBool()) pm.ApplyPatch("rtx_patch_cullnode");
	if (!cv_frustumcull_client || cv_frustumcull_client->GetBool()) pm.ApplyPatch("rtx_patch_frustumcull_client");
	if (!cv_forcenovis || cv_forcenovis->GetBool()) pm.ApplyPatch("rtx_patch_forcenovis");
}

// ===================================================================
//      SYNC PATCHES TO CONVAR VALUES (runtime toggling)
// ===================================================================

static void SyncPatch(const char* name, ConVar* cv) {
	auto& pm = PatchManager::Instance();
	if (!cv) return; // No ConVar, leave patch as-is

	bool wantEnabled = cv->GetBool();
	bool isApplied = pm.IsApplied(name);

	if (wantEnabled && !isApplied) {
		pm.ApplyPatch(name);
	} else if (!wantEnabled && isApplied) {
		pm.RestorePatch(name);
	}
}

void SyncCullingPatches() {
	SyncPatch("rtx_patch_frustumcull_engine", cv_frustumcull_engine);
	SyncPatch("rtx_patch_brush_backfaces", cv_brush_backfaces);
	SyncPatch("rtx_patch_world_backfaces1", cv_world_backfaces1);
	SyncPatch("rtx_patch_world_backfaces2", cv_world_backfaces2);
	SyncPatch("rtx_patch_cullnode", cv_cullnode);
	SyncPatch("rtx_patch_frustumcull_client", cv_frustumcull_client);
	SyncPatch("rtx_patch_forcenovis", cv_forcenovis);
}

// ===================================================================
//      LUA-CALLABLE FUNCTIONS
// ===================================================================

LUA_FUNCTION_STATIC(RTX_SyncPatches) {
	SyncCullingPatches();
	return 0;
}

void RegisterCullingPatchLuaFunctions(GarrysMod::Lua::ILuaBase* LUA) {
	LUA->PushSpecial(GarrysMod::Lua::SPECIAL_GLOB);
		LUA->PushCFunction(RTX_SyncPatches);
		LUA->SetField(-2, "RTX_SyncPatches");
	LUA->Pop();
}

// ===================================================================
//      PUBLIC ENTRY POINT
// ===================================================================

void InitCullingPatches() {
	RegisterPatchConVars();
	RegisterCullingPatches();
	ApplyInitialPatches();
}

#endif // _WIN64
