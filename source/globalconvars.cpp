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
ConVar* GlobalConvars::rtx_hash_collision_detection;
ConVar* GlobalConvars::rtx_fix_solid_color_collisions;
void GlobalConvars::InitialiseConVars() {
	m_pLuaConVars = loader_lua_shared.GetInterface<GarrysMod::Lua::ILuaConVars>(GMOD_LUACONVARS_INTERFACE);
	if (!m_pLuaConVars) {
		Error("[RTX Fixes 2] Failed to get ILuaConVars interface\n");
		return;
	}

	r_forcenovis = m_pLuaConVars->CreateConVar("r_forcenovis", "1", "Force disable vis", FCVAR_ARCHIVE);
	if (!r_forcenovis) { r_forcenovis = cvar->FindVar("r_forcenovis"); }
	if (!r_forcenovis) { Error("[RTX Fixes 2] Failed to create r_forcenovis convar\n"); }
	else { Msg("[RTX Fixes 2] r_forcenovis convar created\n"); }


	c_frustumcull = m_pLuaConVars->CreateConVar("c_frustumcull", "0", "Force frustum culling", FCVAR_ARCHIVE);
	if (!c_frustumcull) { c_frustumcull = cvar->FindVar("c_frustumcull"); }
	if (!c_frustumcull) { Error("[RTX Fixes 2] Failed to create c_frustumcull convar\n"); }
	else { Msg("[RTX Fixes 2] c_frustumcull convar created\n"); }


	r_worldnodenocull = m_pLuaConVars->CreateConVar("r_worldnodenocull", "0", "Force world node nocull", FCVAR_ARCHIVE);
	if (!r_worldnodenocull) { r_worldnodenocull = cvar->FindVar("r_worldnodenocull"); }
	if (!r_worldnodenocull) { Error("[RTX Fixes 2] Failed to create r_worldnodenocull convar\n"); }
	else { Msg("[RTX Fixes 2] r_worldnodenocull convar created\n"); }


	r_forcehwlight = m_pLuaConVars->CreateConVar("r_forcehwlight", "0", "Force LIGHTING_HARDWARE", FCVAR_ARCHIVE);
	if (!r_forcehwlight) { r_forcehwlight = cvar->FindVar("r_forcehwlight"); }
	if (!r_forcehwlight) { Error("[RTX Fixes 2] Failed to create r_forcehwlight convar\n"); }
	else { Msg("[RTX Fixes 2] r_forcehwlight convar created\n"); }

	rtx_force_static_lighting = m_pLuaConVars->CreateConVar("rtx_force_static_lighting", "1", "Force all models to use static lighting for RTX", FCVAR_ARCHIVE);
	if (!rtx_force_static_lighting) { rtx_force_static_lighting = cvar->FindVar("rtx_force_static_lighting"); }
	if (!rtx_force_static_lighting) { Error("[RTX Fixes 2] Failed to create rtx_force_static_lighting convar\n"); }
	else { Msg("[RTX Fixes 2] rtx_force_static_lighting convar created\n"); }

	rtx_hash_collision_detection = m_pLuaConVars->CreateConVar("rtx_hash_collision_detection", "1", "Enable detection and logging of texture hash collisions (textures with identical content/dimensions)", FCVAR_ARCHIVE);
	if (!rtx_hash_collision_detection) { rtx_hash_collision_detection = cvar->FindVar("rtx_hash_collision_detection"); }
	if (!rtx_hash_collision_detection) { Error("[RTX Fixes 2] Failed to create rtx_hash_collision_detection convar\n"); }
	else { Msg("[RTX Fixes 2] rtx_hash_collision_detection convar created\n"); }

	rtx_fix_solid_color_collisions = m_pLuaConVars->CreateConVar("rtx_fix_solid_color_collisions", "1", "Fix texture hash collisions by creating modified runtime textures for solid-color materials", FCVAR_ARCHIVE);
	if (!rtx_fix_solid_color_collisions) { rtx_fix_solid_color_collisions = cvar->FindVar("rtx_fix_solid_color_collisions"); }
	if (!rtx_fix_solid_color_collisions) { Error("[RTX Fixes 2] Failed to create rtx_fix_solid_color_collisions convar\n"); }
	else { Msg("[RTX Fixes 2] rtx_fix_solid_color_collisions convar created\n"); }
}