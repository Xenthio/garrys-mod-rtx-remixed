#include "tier1/convar.h"
#include <GarrysMod/FactoryLoader.hpp>
#include <GarrysMod/Lua/LuaShared.h>
#include <GarrysMod/Lua/LuaConVars.h>
#include <tier0/dbg.h>
#include "interfaces/interfaces.h"
#include "globalconvars.h"

static SourceSDK::FactoryLoader loader_lua_shared("lua_shared");
static GarrysMod::Lua::ILuaShared* lua_shared = nullptr;
static GarrysMod::Lua::ILuaConVars* m_pLuaConVars;
 
ConVar* GlobalConvars::r_forcenovis;
ConVar* GlobalConvars::c_frustumcull;
ConVar* GlobalConvars::r_worldnodenocull;
ConVar* GlobalConvars::r_forcehwlight;
ConVar* GlobalConvars::rtx_force_static_lighting;
ConVar* GlobalConvars::r_forcehwskin;
ConVar* GlobalConvars::r_hwskin_debug;
ConVar* GlobalConvars::r_hwskin_setbones;
ConVar* GlobalConvars::r_hwskin_transform_chain;
ConVar* GlobalConvars::r_hwskin_transpose;
ConVar* GlobalConvars::r_hwskin_clear;
// Helper: try ILuaConVars first, fall back to cvar->FindVar()
static ConVar* CreateOrFindConVar(const char* name, const char* defaultVal, const char* help, int flags = FCVAR_ARCHIVE) {
	ConVar* cv = nullptr;

	if (m_pLuaConVars) {
		cv = m_pLuaConVars->CreateConVar(name, defaultVal, help, flags);
	}

	if (!cv && cvar) {
		cv = cvar->FindVar(name);
	}

	if (cv) {
		Msg("[RTX Fixes 2] %s convar ready\n", name);
	} else {
		Warning("[RTX Fixes 2] Failed to create or find convar '%s'\n", name);
	}
	return cv;
}

void GlobalConvars::InitialiseConVars() {
	m_pLuaConVars = loader_lua_shared.GetInterface<GarrysMod::Lua::ILuaConVars>(GMOD_LUACONVARS_INTERFACE);
	if (!m_pLuaConVars) {
		Warning("[RTX Fixes 2] Failed to get ILuaConVars interface, will try cvar->FindVar() fallback\n");
	}

	r_forcenovis = CreateOrFindConVar("r_forcenovis", "1", "Force disable vis");
	c_frustumcull = CreateOrFindConVar("c_frustumcull", "0", "Force frustum culling");
	r_worldnodenocull = CreateOrFindConVar("r_worldnodenocull", "0", "Force world node nocull");
	r_forcehwlight = CreateOrFindConVar("r_forcehwlight", "0", "Force LIGHTING_HARDWARE");
	rtx_force_static_lighting = CreateOrFindConVar("rtx_force_static_lighting", "1", "Force all models to use static lighting for RTX");
	r_forcehwskin = CreateOrFindConVar("r_forcehwskin", "1", "Force hardware skinning for all models (RTX Remix)");
	r_hwskin_debug = CreateOrFindConVar("r_hwskin_debug", "0", "Enable debug logging for hardware skinning");
	r_hwskin_setbones = CreateOrFindConVar("r_hwskin_setbones", "1", "Enable D3D9 bone matrix setting (set to 0 to disable for debugging)");
	r_hwskin_transform_chain = CreateOrFindConVar("r_hwskin_transform_chain", "0", "Use full poseToBone transform chain (0 = direct passthrough, 1 = full chain)");
	r_hwskin_transpose = CreateOrFindConVar("r_hwskin_transpose", "1", "Transpose matrices for D3D9 (0 = no transpose, 1 = transpose)");
	r_hwskin_clear = CreateOrFindConVar("r_hwskin_clear", "1", "Clear matrices to identity before setting (0 = no, 1 = yes)");
}