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
static ConVar* cv_skip_world_draw = nullptr;

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
	cv_skip_world_draw    = CreatePatchConVar("rtx_patch_skip_world_draw", "0", "Skip engine world surface drawing (Shader_DrawChains) so overlays/decals still render");

	Msg("[PatchManager] ConVar registration complete\n");
}

// ===================================================================
//      PATCH DEFINITIONS
// ===================================================================

static void RegisterCullingPatches() {
	auto& pm = PatchManager::Instance();

	// R_CullBox is compiled into both engine.dll and client.dll. The primary
	// locator identifies the current function entry exactly. The body fallback
	// tolerates small prologue/stack-size changes, then searches backwards for
	// the validated "sub rsp" instruction that still marks the entry point.
	static const char cullBoxEntrySig[] =
		"48 83 EC 48 0F 10 22 33 C0 48 8D 51 20 41 0F 10 28";
	static const char cullBoxBodySig[] =
		"0F 10 22 33 C0 48 8D 51 20 41 0F 10 28 "
		"0F 29 74 24 30";
	const std::vector<uint8_t> expectedCullBoxEntry = {
		0x48, 0x83, 0xEC // sub rsp, imm8
	};
	const std::vector<PatchLocator> cullBoxLocators = {
		{
			"current R_CullBox entry",
			std::string(cullBoxEntrySig, sizeof(cullBoxEntrySig) - 1),
			0,
			0,
			{0x48, 0x83, 0xEC, 0x48, 0x0F, 0x10, 0x22}
		},
		{
			"stable R_CullBox body",
			std::string(cullBoxBodySig, sizeof(cullBoxBodySig) - 1),
			-4,
			8,
			expectedCullBoxEntry
		}
	};

	// engine.dll patches (from applypatch.py patches64)
	{
		// c_frustumcull - uses sse instructions in 64bit
		pm.RegisterPatchWithFallbacks(
			"rtx_patch_frustumcull_engine",
			"engine.dll",
			cullBoxLocators,
			{0x31, 0xC0, 0xC3}); // xor eax,eax; ret
	}
	{
		// brush entity backfaces
		// The body fallback occurs in several related functions, but only this
		// one has a JNZ two bytes before it. PatchManager requires that this
		// expected target validation yields exactly one executable address.
		static const char currentSig[] =
			"85 F6 75 ? F3 0F 10 15 ? ? ? ? "
			"F3 0F 59 51 04 F3 0F 10 05 ? ? ? ?";
		static const char stableBodySig[] =
			"F3 0F 10 15 ? ? ? ? F3 0F 59 51 04 "
			"F3 0F 10 05 ? ? ? ? F3 0F 59 01";
		const std::vector<PatchLocator> locators = {
			{
				"current brush backface branch",
				std::string(currentSig, sizeof(currentSig) - 1),
				2,
				0,
				{0x75} // jnz short
			},
			{
				"stable brush lighting body",
				std::string(stableBodySig, sizeof(stableBodySig) - 1),
				-2,
				4,
				{0x75} // jnz short
			}
		};
		pm.RegisterPatchWithFallbacks(
			"rtx_patch_brush_backfaces",
			"engine.dll",
			locators,
			{0xEB}); // jmp short (unconditional)
	}
	{
		// world backfaces #1
		// PATCH DOCS: in R_DrawLeaf, search for "CBitVec invalid set bitNum" string 
		// Working as of 2026-02-14, cross reference gmod as of that date if you need to refind
		static const char currentSig[] =
			"4D 85 F6 7E ? 8B EF 0F 1F 80 ? ? ? ? "
			"48 8B 05 ? ? ? ? 49 8B 0C EC";
		static const char stableBodySig[] =
			"8B EF 0F 1F 80 ? ? ? ? 48 8B 05 ? ? ? ? "
			"49 8B 0C EC 48 2B 88 E8 00 00 00";
		const std::vector<PatchLocator> locators = {
			{
				"current R_DrawLeaf backface branch #1",
				std::string(currentSig, sizeof(currentSig) - 1),
				3,
				0,
				{0x7E} // jle short
			},
			{
				"stable R_DrawLeaf body after branch #1",
				std::string(stableBodySig, sizeof(stableBodySig) - 1),
				-2,
				4,
				{0x7E} // jle short
			}
		};
		pm.RegisterPatchWithFallbacks(
			"rtx_patch_world_backfaces1",
			"engine.dll",
			locators,
			{0xEB}); // jmp short (unconditional)
	}
	{
		// world backfaces #2
		// PATCH DOCS: in R_DrawLeaf, search for "CBitVec invalid set bitNum" string, but a little lower in the function
		// Working as of 2026-02-14, cross reference gmod as of that date if you need to refind
		static const char currentSig[] =
			"41 85 D1 75 ? F7 45 00 00 02 00 00 75 ? "
			"48 8B 45 ? F3 0F 10 15 ? ? ? ?";
		static const char stableBodySig[] =
			"48 8B 45 ? F3 0F 10 15 ? ? ? ? "
			"F3 0F 10 05 ? ? ? ? F3 0F 10 0D ? ? ? ?";
		const std::vector<PatchLocator> locators = {
			{
				"current R_DrawLeaf backface branch #2",
				std::string(currentSig, sizeof(currentSig) - 1),
				12,
				0,
				{0x75} // jnz short
			},
			{
				"stable R_DrawLeaf body after branch #2",
				std::string(stableBodySig, sizeof(stableBodySig) - 1),
				-2,
				4,
				{0x75} // jnz short
			}
		};
		pm.RegisterPatchWithFallbacks(
			"rtx_patch_world_backfaces2",
			"engine.dll",
			locators,
			{0xEB}); // jmp short (unconditional)
	}
	{
		// R_CullNode. The body fallback allows the stack allocation and RIP
		// displacement to move while requiring a nearby function-entry prologue.
		static const char currentSig[] =
			"48 83 EC 48 80 3D ? ? ? ? 00 48 8B D1 "
			"0F 29 74 24 30 0F 29 7C 24 20";
		static const char stableBodySig[] =
			"48 8B D1 0F 29 74 24 30 0F 29 7C 24 20 "
			"44 0F 29 44 24 10";
		const std::vector<PatchLocator> locators = {
			{
				"current R_CullNode entry",
				std::string(currentSig, sizeof(currentSig) - 1),
				0,
				0,
				{0x48, 0x83, 0xEC, 0x48}
			},
			{
				"stable R_CullNode body",
				std::string(stableBodySig, sizeof(stableBodySig) - 1),
				-11,
				8,
				{0x48, 0x83, 0xEC}
			}
		};
		pm.RegisterPatchWithFallbacks(
			"rtx_patch_cullnode",
			"engine.dll",
			locators,
			{0x31, 0xC0, 0xC3}); // xor eax,eax; ret
	}

	{
		// Shader_DrawChains - skip world surface polygon drawing so overlays/decals still render.
		// Garry's Mod changed the third argument from an 8-bit bool to a 32-bit
		// value in 2026, changing movzx ebp,r8b into mov ebp,r8d. Keep exact
		// signatures for both layouts, then fall back to a stable body signature.
		// The body fallback searches backwards for the validated function prologue
		// so another small argument/prologue change fails safely or still resolves.
		static const char currentSig[] =
			"48 83 EC 60 48 89 6C 24 70 41 8B E8";
		static const char legacySig[] =
			"48 83 EC 60 48 89 6C 24 70 41 0F B6 E8";
		static const char stableBodySig[] =
			"4C 89 64 24 58 4D 8B E1 4C 89 6C 24 50 "
			"4C 89 74 24 48 4C 8B F1";
		const std::vector<uint8_t> expectedEntry = {
			0x40, 0x53,                   // push rbx
			0x48, 0x83, 0xEC, 0x60       // sub rsp, 60h
		};
		const std::vector<PatchLocator> locators = {
			{
				"current 32-bit draw-mode parameter",
				std::string(currentSig, sizeof(currentSig) - 1),
				-2,
				0,
				expectedEntry
			},
			{
				"legacy 8-bit draw-mode parameter",
				std::string(legacySig, sizeof(legacySig) - 1),
				-2,
				0,
				expectedEntry
			},
			{
				"stable Shader_DrawChains body",
				std::string(stableBodySig, sizeof(stableBodySig) - 1),
				-15,
				8,
				expectedEntry
			}
		};

		pm.RegisterPatchWithFallbacks(
			"rtx_patch_skip_world_draw",
			"engine.dll",
			locators,
			{0xC3}); // ret
	}

	// client.dll patches (from applypatch.py patches64)
	{
		// c_frustumcull
		pm.RegisterPatchWithFallbacks(
			"rtx_patch_frustumcull_client",
			"client.dll",
			cullBoxLocators,
			{0x31, 0xC0, 0xC3}); // xor eax,eax; ret
	}
	{
		// r_forcenovis [getter] - force return 1
		// The neighborhood fallback tolerates the member offset changing by
		// identifying the adjacent bool setter/getter and following int getter.
		static const char currentSig[] =
			"0F B6 81 54 03 00 00 C3";
		static const char stableNeighborhoodSig[] =
			"C6 81 ? ? ? ? 01 C3 "
			"CC CC CC CC CC CC CC CC "
			"0F B6 81 ? ? ? ? C3 "
			"CC CC CC CC CC CC CC CC "
			"8B 81 ? ? ? ? C3";
		const std::vector<PatchLocator> locators = {
			{
				"current ShouldForceNoVis getter",
				std::string(currentSig, sizeof(currentSig) - 1),
				0,
				0,
				{0x0F, 0xB6, 0x81}
			},
			{
				"stable ShouldForceNoVis accessor neighborhood",
				std::string(
					stableNeighborhoodSig,
					sizeof(stableNeighborhoodSig) - 1),
				16,
				0,
				{0x0F, 0xB6, 0x81}
			}
		};
		pm.RegisterPatchWithFallbacks(
			"rtx_patch_forcenovis",
			"client.dll",
			locators,
			{0xB0, 0x01, 0xC3}); // mov al,1; ret
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
	if (cv_skip_world_draw && cv_skip_world_draw->GetBool()) pm.ApplyPatch("rtx_patch_skip_world_draw");
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
	SyncPatch("rtx_patch_skip_world_draw", cv_skip_world_draw);
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
