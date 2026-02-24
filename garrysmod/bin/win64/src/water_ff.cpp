//========= Copyright Valve Corporation, All rights reserved. ============//
//
// Purpose: DX6 fixed-function water backport.
//          Replaces the old Water_DX60 inherited-from-LightmappedGeneric approach.
//          Binds $normalmap as the primary texture so RTX Remix can track the
//          draw call and assign water materials, while $fogcolor provides a tint
//          for vanilla rendering.
//
// $NoKeywords: $
//=============================================================================//

#include "shaderlib/cshader.h"

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

BEGIN_SHADER( Water_DX60, "Help for Water_DX60" )

	BEGIN_SHADER_PARAMS
		SHADER_PARAM( NORMALMAP,               SHADER_PARAM_TYPE_TEXTURE, "dev/water_normal", "normal map (used as base texture in FF)" )
		SHADER_PARAM( BUMPFRAME,               SHADER_PARAM_TYPE_INTEGER, "0", "frame number for $normalmap" )
		SHADER_PARAM( BUMPTRANSFORM,           SHADER_PARAM_TYPE_MATRIX,  "center .5 .5 scale 1 1 rotate 0 translate 0 0", "$normalmap UV transform" )
		SHADER_PARAM( FOGCOLOR,                SHADER_PARAM_TYPE_COLOR,   "[0.2 0.3 0.4]", "water fog / tint color" )
		SHADER_PARAM( ENVMAP,                  SHADER_PARAM_TYPE_TEXTURE, "env_cubemap", "envmap" )
		SHADER_PARAM( ENVMAPFRAME,             SHADER_PARAM_TYPE_INTEGER, "0", "" )
		SHADER_PARAM( ENVMAPTINT,              SHADER_PARAM_TYPE_COLOR,   "[1 1 1]", "envmap tint" )
		SHADER_PARAM( REFLECTTEXTURE,          SHADER_PARAM_TYPE_TEXTURE, "_rt_WaterReflection", "" )
		SHADER_PARAM( REFRACTTEXTURE,          SHADER_PARAM_TYPE_TEXTURE, "_rt_WaterRefraction", "" )
		SHADER_PARAM( REFRACTAMOUNT,           SHADER_PARAM_TYPE_FLOAT,   "0", "" )
		SHADER_PARAM( REFRACTTINT,             SHADER_PARAM_TYPE_COLOR,   "[1 1 1]", "refraction tint" )
		SHADER_PARAM( REFLECTAMOUNT,           SHADER_PARAM_TYPE_FLOAT,   "0.8", "" )
		SHADER_PARAM( REFLECTTINT,             SHADER_PARAM_TYPE_COLOR,   "[1 1 1]", "reflection tint" )
		SHADER_PARAM( SCALE,                   SHADER_PARAM_TYPE_VEC2,    "[1 1]", "" )
		SHADER_PARAM( WATERDEPTH,              SHADER_PARAM_TYPE_FLOAT,   "1", "" )
		SHADER_PARAM( ABOVEWATER,              SHADER_PARAM_TYPE_BOOL,    "1", "" )
		SHADER_PARAM( FORCECHEAP,              SHADER_PARAM_TYPE_INTEGER, "0", "" )
		SHADER_PARAM( FORCEEXPENSIVE,          SHADER_PARAM_TYPE_BOOL,    "0", "" )
		SHADER_PARAM( REFLECTENTITIES,         SHADER_PARAM_TYPE_BOOL,    "0", "" )
		SHADER_PARAM( REFLECTBLENDFACTOR,      SHADER_PARAM_TYPE_FLOAT,   "1.0", "" )
		SHADER_PARAM( NOFRESNEL,               SHADER_PARAM_TYPE_BOOL,    "0", "" )
		SHADER_PARAM( NOLOWENDLIGHTMAP,        SHADER_PARAM_TYPE_BOOL,    "0", "" )
		SHADER_PARAM( CHEAPWATERSTARTDISTANCE, SHADER_PARAM_TYPE_FLOAT,   "500", "" )
		SHADER_PARAM( CHEAPWATERENDDISTANCE,   SHADER_PARAM_TYPE_FLOAT,   "1000", "" )
		SHADER_PARAM( SCROLL1,                 SHADER_PARAM_TYPE_COLOR,   "[0 0 0]", "" )
		SHADER_PARAM( SCROLL2,                 SHADER_PARAM_TYPE_COLOR,   "[0 0 0]", "" )
		SHADER_PARAM( BLURREFRACT,             SHADER_PARAM_TYPE_BOOL,    "0", "" )
		SHADER_PARAM( TIME,                    SHADER_PARAM_TYPE_FLOAT,   "0", "" )
	END_SHADER_PARAMS

	SHADER_INIT_PARAMS()
	{
		if ( !params[FOGCOLOR]->IsDefined() )
			params[FOGCOLOR]->SetVecValue( 0.2f, 0.3f, 0.4f );
		if ( !params[ENVMAPTINT]->IsDefined() )
			params[ENVMAPTINT]->SetVecValue( 1.0f, 1.0f, 1.0f );
		if ( !params[ABOVEWATER]->IsDefined() )
			params[ABOVEWATER]->SetIntValue( 1 );
		// Cheap water flag default
		if ( !params[FORCECHEAP]->IsDefined() )
			params[FORCECHEAP]->SetIntValue( 0 );
		if ( !params[FORCEEXPENSIVE]->IsDefined() )
			params[FORCEEXPENSIVE]->SetIntValue( 0 );
		if ( !params[REFLECTBLENDFACTOR]->IsDefined() )
			params[REFLECTBLENDFACTOR]->SetFloatValue( 1.0f );
	}

	SHADER_INIT
	{
		if ( params[NORMALMAP]->IsDefined() )
			LoadTexture( NORMALMAP );

		if ( params[ENVMAP]->IsDefined() )
		{
			if ( !IS_FLAG_SET( MATERIAL_VAR_ENVMAPSPHERE ) )
				LoadCubeMap( ENVMAP );
			else
				LoadTexture( ENVMAP );

			if ( !g_pHardwareConfig->SupportsCubeMaps() )
				SET_FLAGS( MATERIAL_VAR_ENVMAPSPHERE );
		}
	}

	SHADER_DRAW
	{
		bool bHasNormalmap = params[NORMALMAP]->IsTexture();

		// Primary pass: bind normalmap so Remix can identify and track this material.
		// In vanilla mode, we tint by $fogcolor so water retains its color.
		SHADOW_STATE
		{
			pShaderShadow->EnableTexture( SHADER_SAMPLER0, true );

			SetModulationShadowState();
			SetNormalBlendingShadowState();

			pShaderShadow->DrawFlags( SHADER_DRAW_POSITION | SHADER_DRAW_TEXCOORD0 );
			FogToFogColor();
		}
		DYNAMIC_STATE
		{
			if ( bHasNormalmap )
				BindTexture( SHADER_SAMPLER0, NORMALMAP, BUMPFRAME );
			else
				pShaderAPI->BindStandardTexture( SHADER_SAMPLER0, TEXTURE_WHITE );

			SetFixedFunctionTextureTransform( MATERIAL_TEXTURE0, BUMPTRANSFORM );

			// Tint by $fogcolor to preserve water appearance in vanilla rendering.
			float fogColor[3];
			params[FOGCOLOR]->GetVecValue( fogColor, 3 );
			pShaderAPI->Color3f( fogColor[0], fogColor[1], fogColor[2] );
		}
		Draw();
	}
END_SHADER
