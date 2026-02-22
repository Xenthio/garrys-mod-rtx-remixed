//========= Copyright Valve Corporation, All rights reserved. ============//
//
// Purpose: Fixed function eye renderer (DX9 fixed function pipeline)
//
// $Header: $
// $NoKeywords: $
//=============================================================================//

#include "BaseVSShader.h"
#ifdef _WIN32
#include <Windows.h>
#endif

typedef void (*SetEyeTexGenStateFn)(int enable);
static SetEyeTexGenStateFn s_pfnSetEyeTexGenState = nullptr;
static bool s_bTexGenLookupDone = false;

static void EnsureTexGenHelper() {
	if (s_bTexGenLookupDone)
		return;
	s_bTexGenLookupDone = true;

#ifdef _WIN64
	HMODULE hMod = GetModuleHandleA("gmcl_RTXFixesBinary_win64.dll");
#else
	HMODULE hMod = GetModuleHandleA("gmcl_RTXFixesBinary_win32.dll");
#endif
	if (hMod) {
		s_pfnSetEyeTexGenState =
			(SetEyeTexGenStateFn)GetProcAddress(hMod, "RTX_SetEyeTexGenState");
	}
	if (s_pfnSetEyeTexGenState) {
		Msg("[Eyes_dx9] Found RTX_SetEyeTexGenState helper\n");
	} else {
		Warning("[Eyes_dx9] RTX_SetEyeTexGenState not found - iris/glint may "
				"be invisible\n");
	}
}

#include "tier0/dbg.h"
#include "mathlib/vmatrix.h"

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

DEFINE_FALLBACK_SHADER(eyes, Eyes_dx9)

BEGIN_VS_SHADER(Eyes_dx9, "Help for Eyes")

BEGIN_SHADER_PARAMS
	SHADER_PARAM(IRIS, SHADER_PARAM_TYPE_TEXTURE, "shadertest/BaseTexture",
				 "iris texture")
	SHADER_PARAM(IRISFRAME, SHADER_PARAM_TYPE_INTEGER, "0",
				 "frame for the iris texture")
	SHADER_PARAM(GLINT, SHADER_PARAM_TYPE_TEXTURE, "shadertest/BaseTexture",
				 "glint texture")
	SHADER_PARAM(GLINTFRAME, SHADER_PARAM_TYPE_INTEGER, "0",
				 "frame for the glint texture")
	SHADER_PARAM(DILATION, SHADER_PARAM_TYPE_FLOAT, "0.0", "pupil dilation")
	SHADER_PARAM(GLOSSINESS, SHADER_PARAM_TYPE_FLOAT, "1.0", "glossiness")

	SHADER_PARAM(IRISU, SHADER_PARAM_TYPE_VEC4, "[0 0 0 0]",
				 "U projection vector for iris")
	SHADER_PARAM(IRISV, SHADER_PARAM_TYPE_VEC4, "[0 0 0 0]",
				 "V projection vector for iris")
	SHADER_PARAM(GLINTU, SHADER_PARAM_TYPE_VEC4, "[0 0 0 0]",
				 "U projection vector for glint")
	SHADER_PARAM(GLINTV, SHADER_PARAM_TYPE_VEC4, "[0 0 0 0]",
				 "V projection vector for glint")

	SHADER_PARAM(EYEORIGIN, SHADER_PARAM_TYPE_VEC3, "[0 0 0]",
				 "origin of the eyes")
	SHADER_PARAM(EYEUP, SHADER_PARAM_TYPE_VEC3, "[0 0 1]",
				 "up vector of the eyes")
	SHADER_PARAM(EYERIGHT, SHADER_PARAM_TYPE_VEC3, "[1 0 0]",
				 "right vector of the eyes")

	SHADER_PARAM(SELFILLUMTINT, SHADER_PARAM_TYPE_COLOR, "[1 1 1]",
				 "Self-illumination tint for iris")
END_SHADER_PARAMS

SHADER_INIT_PARAMS() {
	SET_FLAGS(MATERIAL_VAR_MODEL);
	SET_FLAGS2(MATERIAL_VAR2_SUPPORTS_HW_SKINNING);
	SET_FLAGS2(MATERIAL_VAR2_LIGHTING_VERTEX_LIT);

	if (!params[DILATION]->IsDefined()) params[DILATION]->SetFloatValue(0.0f);
	if (!params[GLOSSINESS]->IsDefined())
		params[GLOSSINESS]->SetFloatValue(1.0f);
	if (!params[SELFILLUMTINT]->IsDefined())
		params[SELFILLUMTINT]->SetVecValue(1.0f, 1.0f, 1.0f);

	if (params[IRIS]->IsTexture()) {
		SET_FLAGS(MATERIAL_VAR_SELFILLUM);
	}
}

SHADER_INIT {
	if (params[BASETEXTURE]->IsDefined())
		LoadTexture(BASETEXTURE, TEXTUREFLAGS_SRGB);
	if (params[IRIS]->IsDefined())
		LoadTexture(IRIS, TEXTUREFLAGS_SRGB | TEXTUREFLAGS_CLAMPS | TEXTUREFLAGS_CLAMPT);
	if (params[GLINT]->IsDefined())
		LoadTexture(GLINT, TEXTUREFLAGS_CLAMPS | TEXTUREFLAGS_CLAMPT);
}

static void SetTextureTransform(IMaterialVar** params,
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

	float mat[16] = {uView[0], vView[0], 0.0f,    0.0f,    uView[1], vView[1],
					 0.0f,     0.0f,     uView[2], vView[2], 1.0f,    0.0f,
					 uView[3], vView[3], 0.0f,     1.0f};

	pShaderAPI->MatrixMode(textureTransform);
	pShaderAPI->LoadMatrix(mat);
}

SHADER_DRAW {
	// =================================================================
	// Pass 1: Sclera (base texture)
	// =================================================================
	SHADOW_STATE {
		SET_FLAGS2(MATERIAL_VAR2_LIGHTING_VERTEX_LIT);

		pShaderShadow->EnableTexture(SHADER_SAMPLER0, true);
		pShaderShadow->OverbrightValue(SHADER_TEXTURE_STAGE0, OVERBRIGHT);
		pShaderShadow->DrawFlags(SHADER_DRAW_POSITION | SHADER_DRAW_COLOR |
								 SHADER_DRAW_TEXCOORD0);
		FogToFogColor();
		SetDefaultBlendingShadowState(BASETEXTURE, true);
	}

	DYNAMIC_STATE {
		BindTexture(SHADER_SAMPLER0, BASETEXTURE, FRAME);
	}

	Draw();

	// =================================================================
	// Pass 2: Iris - projected via camera-space TexGen + texture matrix
	// =================================================================
	if (params[IRIS]->IsTexture()) {
		SHADOW_STATE {
			pShaderShadow->EnableTexture(SHADER_SAMPLER0, true);
			pShaderShadow->OverbrightValue(SHADER_TEXTURE_STAGE0, OVERBRIGHT);
			pShaderShadow->DrawFlags(SHADER_DRAW_POSITION | SHADER_DRAW_COLOR);

			pShaderShadow->EnableTexGen(SHADER_TEXTURE_STAGE0, true);
			pShaderShadow->TexGen(SHADER_TEXTURE_STAGE0,
								  SHADER_TEXGENPARAM_EYE_LINEAR);

			pShaderShadow->EnableBlending(true);
			pShaderShadow->BlendFunc(SHADER_BLEND_SRC_ALPHA,
									 SHADER_BLEND_ONE_MINUS_SRC_ALPHA);
			FogToFogColor();
		}

		DYNAMIC_STATE {
			BindTexture(SHADER_SAMPLER0, IRIS, IRISFRAME);
			SetTextureTransform(params, pShaderAPI, MATERIAL_TEXTURE0, IRISU,
								IRISV);
			EnsureTexGenHelper();
			if (s_pfnSetEyeTexGenState)
				s_pfnSetEyeTexGenState(1);
		}

		Draw();

		if (!IsSnapshotting() && s_pfnSetEyeTexGenState)
			s_pfnSetEyeTexGenState(0);
	}

	// =================================================================
	// Pass 3: Glint - small additive highlight (optional)
	// =================================================================
	if (params[GLINT]->IsTexture()) {
		SHADOW_STATE {
			pShaderShadow->EnableTexture(SHADER_SAMPLER0, true);
			pShaderShadow->EnableConstantColor(true);
			pShaderShadow->EnableDepthWrites(false);
			pShaderShadow->EnableBlending(true);
			pShaderShadow->BlendFunc(SHADER_BLEND_SRC_ALPHA, SHADER_BLEND_ONE);

			pShaderShadow->EnableTexGen(SHADER_TEXTURE_STAGE0, true);
			pShaderShadow->TexGen(SHADER_TEXTURE_STAGE0,
								  SHADER_TEXGENPARAM_EYE_LINEAR);

			pShaderShadow->DrawFlags(SHADER_DRAW_POSITION);
			FogToBlack();
		}

		DYNAMIC_STATE {
			BindTexture(SHADER_SAMPLER0, GLINT, GLINTFRAME);
			SetTextureTransform(params, pShaderAPI, MATERIAL_TEXTURE0, GLINTU,
								GLINTV);
			EnsureTexGenHelper();
			if (s_pfnSetEyeTexGenState)
				s_pfnSetEyeTexGenState(1);
		}

		Draw();

		if (!IsSnapshotting() && s_pfnSetEyeTexGenState)
			s_pfnSetEyeTexGenState(0);
	}
}
END_SHADER
