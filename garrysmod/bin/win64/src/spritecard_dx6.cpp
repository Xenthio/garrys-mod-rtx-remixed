//========= Copyright Valve Corporation, All rights reserved. ============//
//
// Purpose: Fixed-function fallback for Spritecard shader.
//
// The DX9 Spritecard uses VS/PS for frame blending, rotation, depth
// feathering, dual sequences, etc.  This DX6 fallback renders each
// particle as a simple textured alpha-blended quad using the FF pipeline,
// matching the DX9 version's blending logic as closely as possible.
//
// $NoKeywords: $
//=============================================================================//

#include "BaseVSShader.h"

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

DEFINE_FALLBACK_SHADER(Spritecard, Spritecard_DX6)

BEGIN_VS_SHADER_FLAGS(Spritecard_DX6,
	"Spritecard fixed-function fallback for RTX", SHADER_NOT_EDITABLE)

BEGIN_SHADER_PARAMS
	SHADER_PARAM(ORIENTATION, SHADER_PARAM_TYPE_INTEGER, "0",
		"0 = screen-aligned, 1 = z-aligned, 2 = ground-aligned")
	SHADER_PARAM(ADDBASETEXTURE2, SHADER_PARAM_TYPE_FLOAT, "0.0",
		"amount to blend second texture into frame by")
	SHADER_PARAM(OVERBRIGHTFACTOR, SHADER_PARAM_TYPE_FLOAT, "1.0",
		"overbright factor for HDR effects")
	SHADER_PARAM(ADDOVERBLEND, SHADER_PARAM_TYPE_INTEGER, "0",
		"use ONE:INVSRCALPHA blending")
	SHADER_PARAM(ADDSELF, SHADER_PARAM_TYPE_FLOAT, "0.0",
		"amount of base texture to additively blend in")
	SHADER_PARAM(BLENDFRAMES, SHADER_PARAM_TYPE_BOOL, "1",
		"smooth blend between animated frames")
	SHADER_PARAM(DEPTHBLEND, SHADER_PARAM_TYPE_INTEGER, "0",
		"fade at intersection boundaries (stubbed)")
	SHADER_PARAM(DEPTHBLENDSCALE, SHADER_PARAM_TYPE_FLOAT, "50.0",
		"depth blend scale (stubbed)")
	SHADER_PARAM(DUALSEQUENCE, SHADER_PARAM_TYPE_INTEGER, "0",
		"blend two animated sequences (stubbed)")
	SHADER_PARAM(SEQUENCE_BLEND_MODE, SHADER_PARAM_TYPE_INTEGER, "0",
		"dual sequence blend mode (stubbed)")
	SHADER_PARAM(MAXLUMFRAMEBLEND1, SHADER_PARAM_TYPE_INTEGER, "0",
		"max luminance frame blend seq1 (stubbed)")
	SHADER_PARAM(MAXLUMFRAMEBLEND2, SHADER_PARAM_TYPE_INTEGER, "0",
		"max luminance frame blend seq2 (stubbed)")
	SHADER_PARAM(RAMPTEXTURE, SHADER_PARAM_TYPE_TEXTURE, "",
		"color ramp texture (stubbed)")
	SHADER_PARAM(ZOOMANIMATESEQ2, SHADER_PARAM_TYPE_FLOAT, "1.0",
		"zoom animation for seq2 (stubbed)")
	SHADER_PARAM(EXTRACTGREENALPHA, SHADER_PARAM_TYPE_INTEGER, "0",
		"extract green/alpha channels (stubbed)")
	SHADER_PARAM(USEINSTANCING, SHADER_PARAM_TYPE_BOOL, "0",
		"GPU instancing (stubbed)")
	SHADER_PARAM(SPLINETYPE, SHADER_PARAM_TYPE_INTEGER, "0",
		"spline type (stubbed)")
	SHADER_PARAM(MINSIZE, SHADER_PARAM_TYPE_FLOAT, "0.0",
		"minimum screen fractional size")
	SHADER_PARAM(STARTFADESIZE, SHADER_PARAM_TYPE_FLOAT, "10.0",
		"screen size to start fading")
	SHADER_PARAM(ENDFADESIZE, SHADER_PARAM_TYPE_FLOAT, "20.0",
		"screen size to finish fading")
	SHADER_PARAM(MAXSIZE, SHADER_PARAM_TYPE_FLOAT, "20.0",
		"maximum screen fractional size")
	SHADER_PARAM(MAXDISTANCE, SHADER_PARAM_TYPE_FLOAT, "100000.0",
		"maximum draw distance")
	SHADER_PARAM(FARFADEINTERVAL, SHADER_PARAM_TYPE_FLOAT, "400.0",
		"far-fade interval")
END_SHADER_PARAMS

SHADER_INIT_PARAMS() {
	if (!params[MAXDISTANCE]->IsDefined())
		params[MAXDISTANCE]->SetFloatValue(100000.0f);
	if (!params[FARFADEINTERVAL]->IsDefined())
		params[FARFADEINTERVAL]->SetFloatValue(400.0f);
	if (!params[MAXSIZE]->IsDefined())
		params[MAXSIZE]->SetFloatValue(20.0f);
	if (!params[ENDFADESIZE]->IsDefined())
		params[ENDFADESIZE]->SetFloatValue(20.0f);
	if (!params[STARTFADESIZE]->IsDefined())
		params[STARTFADESIZE]->SetFloatValue(10.0f);
	if (!params[OVERBRIGHTFACTOR]->IsDefined())
		params[OVERBRIGHTFACTOR]->SetFloatValue(1.0f);
	if (!params[ADDBASETEXTURE2]->IsDefined())
		params[ADDBASETEXTURE2]->SetFloatValue(0.0f);
	if (!params[ADDSELF]->IsDefined())
		params[ADDSELF]->SetFloatValue(0.0f);
	if (!params[BLENDFRAMES]->IsDefined())
		params[BLENDFRAMES]->SetIntValue(1);
	if (!params[DEPTHBLEND]->IsDefined())
		params[DEPTHBLEND]->SetIntValue(0);
	if (!params[DUALSEQUENCE]->IsDefined())
		params[DUALSEQUENCE]->SetIntValue(0);
	if (!params[MAXLUMFRAMEBLEND1]->IsDefined())
		params[MAXLUMFRAMEBLEND1]->SetIntValue(0);
	if (!params[MAXLUMFRAMEBLEND2]->IsDefined())
		params[MAXLUMFRAMEBLEND2]->SetIntValue(0);
	if (!params[EXTRACTGREENALPHA]->IsDefined())
		params[EXTRACTGREENALPHA]->SetIntValue(0);
	if (!params[ADDOVERBLEND]->IsDefined())
		params[ADDOVERBLEND]->SetIntValue(0);
	if (!params[USEINSTANCING]->IsDefined())
		params[USEINSTANCING]->SetIntValue(0);
	if (!params[SPLINETYPE]->IsDefined())
		params[SPLINETYPE]->SetIntValue(0);

	SET_FLAGS(MATERIAL_VAR_NO_DEBUG_OVERRIDE);
	SET_FLAGS(MATERIAL_VAR_VERTEXCOLOR);
	SET_FLAGS(MATERIAL_VAR_VERTEXALPHA);
}

SHADER_INIT {
	if (params[BASETEXTURE]->IsDefined())
		LoadTexture(BASETEXTURE, TEXTUREFLAGS_SRGB);
}

SHADER_DRAW {
	bool bAddOverBlend =
		params[ADDOVERBLEND]->IsDefined() &&
		params[ADDOVERBLEND]->GetIntValue() != 0;
	bool bAddSelf =
		params[ADDSELF]->IsDefined() &&
		params[ADDSELF]->GetFloatValue() != 0.0f;
	bool bAdditive2nd =
		params[ADDBASETEXTURE2]->IsDefined() &&
		params[ADDBASETEXTURE2]->GetFloatValue() != 0.0f;

	SHADOW_STATE {
		pShaderShadow->EnableCulling(false);
		pShaderShadow->EnableAlphaWrites(false);
		pShaderShadow->EnableDepthWrites(false);
		pShaderShadow->EnableTexture(SHADER_SAMPLER0, true);

		// Blending logic matching the DX9 Spritecard shader exactly.
		if (bAdditive2nd || bAddOverBlend || bAddSelf) {
			pShaderShadow->EnableBlending(true);
			pShaderShadow->BlendFunc(SHADER_BLEND_ONE,
									 SHADER_BLEND_ONE_MINUS_SRC_ALPHA);
		} else if (IS_FLAG_SET(MATERIAL_VAR_ADDITIVE)) {
			pShaderShadow->EnableBlending(true);
			pShaderShadow->BlendFunc(SHADER_BLEND_SRC_ALPHA,
									 SHADER_BLEND_ONE);
		} else {
			pShaderShadow->EnableBlending(true);
			pShaderShadow->BlendFunc(SHADER_BLEND_SRC_ALPHA,
									 SHADER_BLEND_ONE_MINUS_SRC_ALPHA);
		}

		// Alpha test: kill fully-transparent fragments.
		// Disabled for additive modes (same as DX9).
		if (bAdditive2nd || bAddSelf) {
			pShaderShadow->EnableAlphaTest(false);
		} else {
			pShaderShadow->EnableAlphaTest(true);
			pShaderShadow->AlphaFunc(SHADER_ALPHAFUNC_GREATER, 0.01f);
		}

		pShaderShadow->DrawFlags(SHADER_DRAW_POSITION |
								 SHADER_DRAW_TEXCOORD0 |
								 SHADER_DRAW_COLOR);

		if (IS_FLAG_SET(MATERIAL_VAR_ADDITIVE) || bAddOverBlend ||
			bAddSelf || bAdditive2nd) {
			FogToBlack();
		} else {
			FogToFogColor();
		}
	}

	DYNAMIC_STATE {
		BindTexture(SHADER_SAMPLER0, BASETEXTURE, FRAME);
	}

	Draw();
}
END_SHADER
