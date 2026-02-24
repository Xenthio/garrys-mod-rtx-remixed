//========= Copyright Valve Corporation, All rights reserved. ============//
//
// Purpose: Fixed-function fallback for EyeRefract shader.
//
// EyeRefract normally uses VS/PS for cornea refraction, parallax, and
// ambient occlusion.  Since GMod RTX runs at DX6 level where those are
// unavailable, this shader renders the eye with the same multi-pass
// fixed-function approach as Eyes_dx9:
//
//   Pass 1 (sclera) : $AmbientOcclTexture  (or $BaseTexture fallback)
//   Pass 2 (iris)   : $Iris projected via $IrisU / $IrisV  with TexGen
//
// All EyeRefract-specific parameters are declared so the material loads
// without errors, but the advanced shader features are silently ignored.
//
// $Header: $
// $NoKeywords: $
//=============================================================================//

#include "BaseVSShader.h"
#include "tier0/dbg.h"
#include "mathlib/vmatrix.h"

#ifdef _WIN32
#include <Windows.h>
#endif

typedef void (*SetEyeTexGenStateFn)(int enable);
static SetEyeTexGenStateFn s_pfnSetEyeTexGenState_ER = nullptr;
static bool s_bTexGenLookupDone_ER = false;

static void EnsureTexGenHelper_ER() {
	if (s_bTexGenLookupDone_ER)
		return;
	s_bTexGenLookupDone_ER = true;

#ifdef _WIN64
	HMODULE hMod = GetModuleHandleA("gmcl_RTXFixesBinary_win64.dll");
#else
	HMODULE hMod = GetModuleHandleA("gmcl_RTXFixesBinary_win32.dll");
#endif
	if (hMod) {
		s_pfnSetEyeTexGenState_ER =
			(SetEyeTexGenStateFn)GetProcAddress(hMod, "RTX_SetEyeTexGenState");
	}
	if (s_pfnSetEyeTexGenState_ER) {
		Msg("[EyeRefract_ff] Found RTX_SetEyeTexGenState helper\n");
	} else {
		Warning("[EyeRefract_ff] RTX_SetEyeTexGenState not found - iris may "
				"be invisible\n");
	}
}

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

DEFINE_FALLBACK_SHADER(EyeRefract, EyeRefract_ff)

BEGIN_VS_SHADER(EyeRefract_ff, "EyeRefract fixed-function fallback for RTX")

BEGIN_SHADER_PARAMS
	// Core eye params (shared with Eyes shader)
	SHADER_PARAM(IRIS, SHADER_PARAM_TYPE_TEXTURE, "shadertest/BaseTexture",
				 "iris texture")
	SHADER_PARAM(IRISFRAME, SHADER_PARAM_TYPE_INTEGER, "0",
				 "frame for the iris texture")
	SHADER_PARAM(IRISU, SHADER_PARAM_TYPE_VEC4, "[0 0 0 0]",
				 "U projection vector for iris")
	SHADER_PARAM(IRISV, SHADER_PARAM_TYPE_VEC4, "[0 0 0 0]",
				 "V projection vector for iris")
	SHADER_PARAM(EYEORIGIN, SHADER_PARAM_TYPE_VEC3, "[0 0 0]",
				 "origin for the eyes")
	SHADER_PARAM(DILATION, SHADER_PARAM_TYPE_FLOAT, "0",
				 "Pupil dilation (0 is none, 1 is maximal)")
	SHADER_PARAM(GLOSSINESS, SHADER_PARAM_TYPE_FLOAT, "1",
				 "Glossiness of eye")

	// EyeRefract-specific params (accepted but not used in FF path)
	SHADER_PARAM(CORNEATEXTURE, SHADER_PARAM_TYPE_TEXTURE,
				 "shadertest/BaseTexture", "cornea normal map texture")
	SHADER_PARAM(AMBIENTOCCLTEXTURE, SHADER_PARAM_TYPE_TEXTURE,
				 "shadertest/BaseTexture", "ambient occlusion texture")
	SHADER_PARAM(ENVMAP, SHADER_PARAM_TYPE_TEXTURE,
				 "shadertest/shadertest_env", "envmap")
	SHADER_PARAM(SPHERETEXKILLCOMBO, SHADER_PARAM_TYPE_BOOL, "1",
				 "texkill pixels not on sphere")
	SHADER_PARAM(RAYTRACESPHERE, SHADER_PARAM_TYPE_BOOL, "1",
				 "Raytrace sphere")
	SHADER_PARAM(PARALLAXSTRENGTH, SHADER_PARAM_TYPE_FLOAT, "1",
				 "Parallax strength")
	SHADER_PARAM(CORNEABUMPSTRENGTH, SHADER_PARAM_TYPE_FLOAT, "1",
				 "Cornea bump strength")
	SHADER_PARAM(AMBIENTOCCLCOLOR, SHADER_PARAM_TYPE_VEC3, "[1 1 1]",
				 "Ambient occlusion color")
	SHADER_PARAM(EYEBALLRADIUS, SHADER_PARAM_TYPE_FLOAT, "0",
				 "Eyeball radius for ray casting")
	SHADER_PARAM(INTRO, SHADER_PARAM_TYPE_BOOL, "0",
				 "is eyes in the ep1 intro")
	SHADER_PARAM(ENTITYORIGIN, SHADER_PARAM_TYPE_VEC3, "0.0",
				 "center of the model in world space")
	SHADER_PARAM(WARPPARAM, SHADER_PARAM_TYPE_FLOAT, "0.0",
				 "animation param between 0 and 1")
	SHADER_PARAM(LIGHTWARPTEXTURE, SHADER_PARAM_TYPE_TEXTURE,
				 "shadertest/BaseTexture",
				 "1D ramp texture for tinting scalar diffuse term")

	// Cloak pass params (accepted, not rendered)
	SHADER_PARAM(CLOAKPASSENABLED, SHADER_PARAM_TYPE_BOOL, "0",
				 "Enables cloak render in a second pass")
	SHADER_PARAM(CLOAKFACTOR, SHADER_PARAM_TYPE_FLOAT, "0.0", "")
	SHADER_PARAM(CLOAKCOLORTINT, SHADER_PARAM_TYPE_COLOR, "[1 1 1]",
				 "Cloak color tint")
	SHADER_PARAM(REFRACTAMOUNT, SHADER_PARAM_TYPE_FLOAT, "2", "")

	// Emissive scroll pass params (accepted, not rendered)
	SHADER_PARAM(EMISSIVEBLENDENABLED, SHADER_PARAM_TYPE_BOOL, "0",
				 "Enable emissive blend pass")
	SHADER_PARAM(EMISSIVEBLENDSCROLLVECTOR, SHADER_PARAM_TYPE_VEC2,
				 "[0.11 0.124]", "Emissive scroll vec")
	SHADER_PARAM(EMISSIVEBLENDSTRENGTH, SHADER_PARAM_TYPE_FLOAT, "1.0",
				 "Emissive blend strength")
	SHADER_PARAM(EMISSIVEBLENDTEXTURE, SHADER_PARAM_TYPE_TEXTURE, "",
				 "self-illumination map")
	SHADER_PARAM(EMISSIVEBLENDTINT, SHADER_PARAM_TYPE_COLOR, "[1 1 1]",
				 "Self-illumination tint")
	SHADER_PARAM(EMISSIVEBLENDFLOWTEXTURE, SHADER_PARAM_TYPE_TEXTURE, "",
				 "flow map")
END_SHADER_PARAMS

SHADER_INIT_PARAMS() {
	static bool s_initLogged = false;
	if (!s_initLogged) {
		Msg("[EyeRefract_ff] SHADER_INIT_PARAMS: %s\n",
			pMaterialName ? pMaterialName : "unknown");
		s_initLogged = true;
	}

	SET_FLAGS(MATERIAL_VAR_MODEL);
	SET_FLAGS2(MATERIAL_VAR2_SUPPORTS_HW_SKINNING);
	SET_FLAGS2(MATERIAL_VAR2_LIGHTING_VERTEX_LIT);

	if (!params[DILATION]->IsDefined()) params[DILATION]->SetFloatValue(0.0f);
	if (!params[GLOSSINESS]->IsDefined())
		params[GLOSSINESS]->SetFloatValue(1.0f);
	if (!params[CLOAKPASSENABLED]->IsDefined())
		params[CLOAKPASSENABLED]->SetIntValue(0);
	if (!params[EMISSIVEBLENDENABLED]->IsDefined())
		params[EMISSIVEBLENDENABLED]->SetIntValue(0);

	if (params[IRIS]->IsTexture()) {
		SET_FLAGS(MATERIAL_VAR_SELFILLUM);
	}
}

SHADER_INIT {
	// Match the original EyeRefract_dx9 texture loading flags.
	// TEXTUREFLAGS_SRGB is critical: without it, DXVK-Remix may create
	// the D3D texture in a linear format and lose color channel data.
	if (params[AMBIENTOCCLTEXTURE]->IsDefined())
		LoadTexture(AMBIENTOCCLTEXTURE, TEXTUREFLAGS_SRGB);
	if (params[BASETEXTURE]->IsDefined())
		LoadTexture(BASETEXTURE, TEXTUREFLAGS_SRGB);
	if (params[IRIS]->IsDefined())
		LoadTexture(IRIS, TEXTUREFLAGS_SRGB | TEXTUREFLAGS_CLAMPS | TEXTUREFLAGS_CLAMPT);
}

// UV scale for the EyeRefract iris projection.  The original EyeRefract
// pixel shader applies parallax / sphere-raytrace adjustments that change
// the effective iris size.  This constant compensates in the FF fallback.
// 0.5 = iris covers twice as much of the eye (UVs compressed toward center).
#define EYEREFRACT_IRIS_UV_SCALE 0.5f

// Build eye-space texture matrix for iris projection (same as Eyes_dx9),
// with an additional UV scale applied around the (0.5, 0.5) center.
static void SetTextureTransformER(IMaterialVar** params,
								  IShaderDynamicAPI* pShaderAPI,
								  MaterialMatrixMode_t textureTransform,
								  int uparam, int vparam) {
	Vector4D uWorld, vWorld;
	params[uparam]->GetVecValue(uWorld.Base(), 4);
	params[vparam]->GetVecValue(vWorld.Base(), 4);

	VMatrix view, invTrans;
	pShaderAPI->GetMatrix(MATERIAL_VIEW, view.m[0]);
	view = view.Transpose();

	view.InverseGeneral(invTrans);
	invTrans = invTrans.Transpose();

	Vector4D uView, vView;
	uView.AsVector3D() = invTrans.VMul3x3(uWorld.AsVector3D());
	vView.AsVector3D() = invTrans.VMul3x3(vWorld.AsVector3D());
	uView[3] =
		uWorld[3] - DotProduct(view.GetTranslation(), uView.AsVector3D());
	vView[3] =
		vWorld[3] - DotProduct(view.GetTranslation(), vView.AsVector3D());

	// Scale UVs around (0.5, 0.5):  UV' = s * UV + 0.5*(1-s)
	const float s = EYEREFRACT_IRIS_UV_SCALE;
	const float offset = 0.5f * (1.0f - s);

	float mat[16] = {uView[0]*s, vView[0]*s, 0.0f,              0.0f,
					 uView[1]*s, vView[1]*s, 0.0f,              0.0f,
					 uView[2]*s, vView[2]*s, 1.0f,              0.0f,
					 uView[3]*s + offset, vView[3]*s + offset, 0.0f, 1.0f};

	pShaderAPI->MatrixMode(textureTransform);
	pShaderAPI->LoadMatrix(mat);
}

SHADER_DRAW {
	// =====================================================================
	// Pass 1: Sclera (fully opaque)
	//
	// Fills the entire eye surface with the AO texture or a white base.
	// Must be fully opaque so RTX Remix treats it as a solid material
	// instead of a translucent one.
	// =====================================================================
	bool hasSclera = (params[AMBIENTOCCLTEXTURE]->IsTexture() ||
					  params[BASETEXTURE]->IsTexture());

	if (hasSclera) {
		SHADOW_STATE {
			SET_FLAGS2(MATERIAL_VAR2_LIGHTING_VERTEX_LIT);

			pShaderShadow->EnableTexture(SHADER_SAMPLER0, true);
			pShaderShadow->OverbrightValue(SHADER_TEXTURE_STAGE0, OVERBRIGHT);
			pShaderShadow->DrawFlags(SHADER_DRAW_POSITION | SHADER_DRAW_COLOR |
									 SHADER_DRAW_TEXCOORD0);

			// Fully opaque -- no blending, no alpha test.
			pShaderShadow->EnableBlending(false);
			pShaderShadow->EnableAlphaTest(false);
			pShaderShadow->EnableAlphaWrites(false);
			FogToFogColor();
		}

		DYNAMIC_STATE {
			if (params[AMBIENTOCCLTEXTURE]->IsTexture())
				BindTexture(SHADER_SAMPLER0, AMBIENTOCCLTEXTURE);
			else
				BindTexture(SHADER_SAMPLER0, BASETEXTURE, FRAME);
		}

		Draw();
	}

	// =====================================================================
	// Pass 2: Iris - projected via camera-space TexGen + texture matrix
	//
	// Alpha-blended over the sclera.  The iris texture's alpha channel
	// masks the iris circle (alpha=1 inside, alpha=0 outside).
	// TEXTUREFLAGS_CLAMPS/CLAMPT on the iris texture prevent tiling
	// when TexGen UVs exceed [0,1].
	// =====================================================================
	if (params[IRIS]->IsTexture()) {
		SHADOW_STATE {
			pShaderShadow->EnableTexture(SHADER_SAMPLER0, true);
			pShaderShadow->OverbrightValue(SHADER_TEXTURE_STAGE0, OVERBRIGHT);
			pShaderShadow->DrawFlags(SHADER_DRAW_POSITION | SHADER_DRAW_COLOR);

			pShaderShadow->EnableTexGen(SHADER_TEXTURE_STAGE0, true);
			pShaderShadow->TexGen(SHADER_TEXTURE_STAGE0,
								  SHADER_TEXGENPARAM_EYE_LINEAR);

			pShaderShadow->EnableBlending(false);
			pShaderShadow->EnableAlphaTest(false);

			FogToFogColor();
		}

		DYNAMIC_STATE {
			BindTexture(SHADER_SAMPLER0, IRIS, IRISFRAME);
			SetTextureTransformER(params, pShaderAPI, MATERIAL_TEXTURE0, IRISU,
								  IRISV);
			EnsureTexGenHelper_ER();
			if (s_pfnSetEyeTexGenState_ER)
				s_pfnSetEyeTexGenState_ER(1);
		}

		Draw();

		if (!IsSnapshotting() && s_pfnSetEyeTexGenState_ER)
			s_pfnSetEyeTexGenState_ER(0);
	}
}
END_SHADER
