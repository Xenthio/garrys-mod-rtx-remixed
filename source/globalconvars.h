#pragma once

#include "tier1/convar.h"

class GlobalConvars
{
public:
	static ConVar* r_forcenovis;
	static ConVar* c_frustumcull;
	static ConVar* r_worldnodenocull;
	static ConVar* r_forcehwlight;
	static ConVar* rtx_force_static_lighting;
	static ConVar* r_forcehwskin;      // Force hardware skinning for all models
	static ConVar* r_hwskin_debug;     // Debug logging for hardware skinning
	static ConVar* r_hwskin_setbones;  // Enable D3D9 bone matrix setting (0 to disable for debugging)
	static ConVar* r_hwskin_transform_chain;  // Use full transform chain (0 = direct passthrough)
	static ConVar* r_hwskin_transpose; // Transpose matrices for D3D9 (0 = no, 1 = yes)
	static ConVar* r_hwskin_clear;     // Clear matrices to identity before setting (0 = no, 1 = yes)
	static void InitialiseConVars();
}; 