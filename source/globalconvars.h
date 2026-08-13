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
	static ConVar* r_hwskin_debug_filter; // Comma-separated model-name filter for detailed traces
	static ConVar* r_hwskin_setbones;  // Enable D3D9 bone matrix setting (0 to disable for debugging)
	static ConVar* r_hwskin_transform_chain;  // Use full transform chain (0 = direct passthrough)
	static ConVar* r_hwskin_transpose; // Transpose matrices for D3D9 (0 = no, 1 = yes)
	static ConVar* r_hwskin_clear;     // Clear matrices to identity before setting (0 = no, 1 = yes)
	static ConVar* r_hwskin_force_flag;  // Force MESHGROUP_IS_HWSKINNED flag on multi-bone models
	static ConVar* r_hwskin_ubyte4;      // Apply UBYTE4 vertex declaration patch for bone indices
	static ConVar* r_hwskin_vtx_hw;      // Use legacy HW VTX ordering instead of HL2 RTX-compatible SW VTX ordering
	static ConVar* r_eyes_hwskin;          // Force eye meshes through HW skinning path for RTX Remix
	static ConVar* r_remix_material_debug; // Verbose logging for RemixMaterial Lua bindings
	static ConVar* rtx_flush_textures_on_map_exit; // Purge Source and Remix texture residency at map shutdown
	static void InitialiseConVars();
};
