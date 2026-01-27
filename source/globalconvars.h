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
	static void InitialiseConVars();
}; 