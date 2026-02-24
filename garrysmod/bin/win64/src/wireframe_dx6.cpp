//========= Copyright Valve Corporation, All rights reserved. ============//
//
// Purpose: DX6 fixed-function wireframe shader backport.
//
// $Header: $
// $NoKeywords: $
//=============================================================================//

#include "shaderlib/cshader.h"

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

DEFINE_FALLBACK_SHADER( Wireframe, Wireframe_DX6 )
DEFINE_FALLBACK_SHADER( Wireframe_DX9, Wireframe_DX6 )
DEFINE_FALLBACK_SHADER( Wireframe_DX8, Wireframe_DX6 )

BEGIN_SHADER( Wireframe_DX6,
			  "Help for Wireframe_DX6" )

	BEGIN_SHADER_PARAMS
	END_SHADER_PARAMS

	SHADER_INIT_PARAMS()
	{
		SET_FLAGS( MATERIAL_VAR_NO_DEBUG_OVERRIDE );
		SET_FLAGS( MATERIAL_VAR_NOFOG );
		SET_FLAGS( MATERIAL_VAR_WIREFRAME );
	}

	SHADER_INIT
	{
		if (params[BASETEXTURE]->IsDefined())
		{
			LoadTexture( BASETEXTURE );
		}
	}

	SHADER_DRAW
	{
		SHADOW_STATE
		{
			pShaderShadow->PolyMode( SHADER_POLYMODEFACE_FRONT_AND_BACK, SHADER_POLYMODE_LINE );
			// Bind a texture so RTX Remix tracks this draw call.
			// Without a bound texture, Remix ignores the geometry entirely.
			pShaderShadow->EnableTexture( SHADER_SAMPLER0, true );
			pShaderShadow->OverbrightValue( SHADER_TEXTURE_STAGE0, 1.0f );
			SetModulationShadowState();
			SetNormalBlendingShadowState();
			pShaderShadow->DrawFlags( SHADER_DRAW_POSITION | SHADER_DRAW_COLOR | SHADER_DRAW_TEXCOORD0 );
			FogToFogColor();
		}
		DYNAMIC_STATE
		{
			// Use the base texture if defined, otherwise fall back to the engine white texture.
			if ( params[BASETEXTURE]->IsTexture() )
				BindTexture( SHADER_SAMPLER0, BASETEXTURE, FRAME );
			else
				pShaderAPI->BindStandardTexture( SHADER_SAMPLER0, TEXTURE_WHITE );
			SetModulationDynamicState();
		}
		Draw();
	}
END_SHADER
