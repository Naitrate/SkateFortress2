//========= Copyright Valve Corporation, All rights reserved. ============//
//
// Purpose: 
//
// $Workfile:     $
// $Date:         $
// $NoKeywords: $
//=============================================================================//

// C callable material system interface for the utils.

#include "materialsystem/imaterialsystem.h"
#include "materialsystem/imaterial.h"
#include "materialsystem/imaterialvar.h"
#include <cmdlib.h>
#include "utilmatlib.h"
#include "tier0/dbg.h"
#ifdef _WIN32
#include <windows.h>
#endif
#include "filesystem.h"
#include "materialsystem/materialsystem_config.h"
#include "mathlib/mathlib.h"

#ifndef POSIX

void LoadMaterialSystemInterface( CreateInterfaceFn fileSystemFactory )
{
	if( g_pMaterialSystem )
		return;
	
	// materialsystem.dll should be in the path, it's in bin along with vbsp.
	const char *pDllName = "materialsystem.dll";
	CSysModule *materialSystemDLLHInst;
	materialSystemDLLHInst = g_pFullFileSystem->LoadModule( pDllName );
	if( !materialSystemDLLHInst )
	{
		Error( "Can't load MaterialSystem.dll\n" );
	}

	CreateInterfaceFn clientFactory = Sys_GetFactory( materialSystemDLLHInst );
	if ( clientFactory )
	{
		g_pMaterialSystem = (IMaterialSystem *)clientFactory( MATERIAL_SYSTEM_INTERFACE_VERSION, NULL );
		if ( !g_pMaterialSystem )
		{
			Error( "Could not get the material system interface from materialsystem.dll (" __FILE__ ")" );
		}
	}
	else
	{
		Error( "Could not find factory interface in library MaterialSystem.dll" );
	}

	if (!g_pMaterialSystem->Init( "shaderapiempty.dll", 0, fileSystemFactory ))
	{
		Error( "Could not start the empty shader (shaderapiempty.dll)!" );
	}
}

void InitMaterialSystem( const char *materialBaseDirPath, CreateInterfaceFn fileSystemFactory )
{
	LoadMaterialSystemInterface( fileSystemFactory );
	MaterialSystem_Config_t config;
	g_pMaterialSystem->OverrideConfig( config, false );
}

void ShutdownMaterialSystem( )
{
	if ( g_pMaterialSystem )
	{
		g_pMaterialSystem->Shutdown();
		g_pMaterialSystem = NULL;
	}
}

MaterialSystemMaterial_t FindMaterial( const char *materialName, bool *pFound, bool bComplain )
{
	IMaterial *pMat = g_pMaterialSystem->FindMaterial( materialName, TEXTURE_GROUP_OTHER, bComplain );
	MaterialSystemMaterial_t matHandle = pMat;
	
	if ( pFound )
	{
		*pFound = true;
		if ( IsErrorMaterial( pMat ) )
			*pFound = false;
	}

	return matHandle;
}

void GetMaterialDimensions( MaterialSystemMaterial_t materialHandle, int *width, int *height )
{
	PreviewImageRetVal_t retVal;
	ImageFormat dummyImageFormat;
	IMaterial *material = ( IMaterial * )materialHandle;
	bool translucent;
	retVal = material->GetPreviewImageProperties( width, height, &dummyImageFormat, &translucent );
	if (retVal != MATERIAL_PREVIEW_IMAGE_OK ) 
	{
#if 0
		if (retVal == MATERIAL_PREVIEW_IMAGE_BAD ) 
		{
			Error( "problem getting preview image for %s", 
				g_pMaterialSystem->GetMaterialName( materialInfo[matID].materialHandle ) );
		}
#else
		*width = 128;
		*height = 128;
#endif
	}
}

void GetMaterialReflectivity( MaterialSystemMaterial_t materialHandle, float *reflectivityVect )
{
	IMaterial *material = ( IMaterial * )materialHandle;
	const IMaterialVar *reflectivityVar;

	bool found;
	reflectivityVar = material->FindVar( "$reflectivity", &found, false );
	if( !found )
	{
		Vector tmp;
		material->GetReflectivity( tmp );
		VectorCopy( tmp.Base(), reflectivityVect );
	}
	else
	{
		reflectivityVar->GetVecValue( reflectivityVect, 3 );
	}
}

int GetMaterialShaderPropertyBool( MaterialSystemMaterial_t materialHandle, int propID )
{
	IMaterial *material = ( IMaterial * )materialHandle;
	switch( propID )
	{
	case UTILMATLIB_NEEDS_BUMPED_LIGHTMAPS:
		return material->GetPropertyFlag( MATERIAL_PROPERTY_NEEDS_BUMPED_LIGHTMAPS );

	case UTILMATLIB_NEEDS_LIGHTMAP:
		return material->GetPropertyFlag( MATERIAL_PROPERTY_NEEDS_LIGHTMAP );

	default:
		Assert( 0 );
		return 0;
	}
}

int GetMaterialShaderPropertyInt( MaterialSystemMaterial_t materialHandle, int propID )
{
	IMaterial *material = ( IMaterial * )materialHandle;
	switch( propID )
	{
	case UTILMATLIB_OPACITY:
		if (material->IsTranslucent())
			return UTILMATLIB_TRANSLUCENT;
		if (material->IsAlphaTested())
			return UTILMATLIB_ALPHATEST;
		return UTILMATLIB_OPAQUE;

	default:
		Assert( 0 );
		return 0;
	}
}

const char *GetMaterialVar( MaterialSystemMaterial_t materialHandle, const char *propertyName )
{
	IMaterial *material = ( IMaterial * )materialHandle;
	IMaterialVar *var;
	bool found;
	var = material->FindVar( propertyName, &found, false );
	if( found )
	{
		return var->GetStringValue();
	}
	else
	{
		return NULL;
	}
}

const char *GetMaterialShaderName( MaterialSystemMaterial_t materialHandle )
{
	IMaterial *material = ( IMaterial * )materialHandle;
	return material->GetShaderName();
}

#else // POSIX

//-----------------------------------------------------------------------------
// tf2-skate: the Linux build of the map tools reads materials itself. The
// Linux material system doesn't start without the engine around it, and the
// compilers only need a material's variables (%compile* flags, $translucent,
// $basetexture...), its shader name, and its base texture's size and
// reflectivity: all in the .vmt and the .vtf header.
//-----------------------------------------------------------------------------
#include "KeyValues.h"
#include "tier1/utldict.h"


struct ToolMaterial_t
{
	KeyValues	*pKeys;		// the shader block (patches resolved)
	int			nWidth, nHeight;
	float		flReflectivity[3];
};

static CUtlDict< ToolMaterial_t *, int > s_Materials;
static ToolMaterial_t s_ErrorMaterial;

static KeyValues *LoadVMT( const char *pszName, int nDepth = 0 )
{
	char szPath[ MAX_PATH ];
	V_snprintf( szPath, sizeof( szPath ), "materials/%s.vmt", pszName );
	V_FixSlashes( szPath );
	KeyValues *pKeys = new KeyValues( "vmt" );
	if ( !pKeys->LoadFromFile( g_pFullFileSystem, szPath, "GAME" ) )
	{
		pKeys->deleteThis();
		return NULL;
	}
	// "patch": an include with keys replaced or inserted.
	if ( !V_stricmp( pKeys->GetName(), "patch" ) && nDepth < 8 )
	{
		char szInclude[ MAX_PATH ];
		V_strncpy( szInclude, pKeys->GetString( "include" ), sizeof( szInclude ) );
		V_StripExtension( szInclude, szInclude, sizeof( szInclude ) );
		const char *pszInclude = szInclude;
		if ( !V_strnicmp( pszInclude, "materials/", 10 ) || !V_strnicmp( pszInclude, "materials\\", 10 ) )
			pszInclude += 10;
		KeyValues *pBase = LoadVMT( pszInclude, nDepth + 1 );
		if ( pBase )
		{
			for ( const char *pszBlock : { "insert", "replace" } )
			{
				KeyValues *pBlock = pKeys->FindKey( pszBlock );
				for ( KeyValues *pKey = pBlock ? pBlock->GetFirstSubKey() : NULL; pKey; pKey = pKey->GetNextKey() )
				{
					pBase->SetString( pKey->GetName(), pKey->GetString() );
				}
			}
		}
		pKeys->deleteThis();
		return pBase;
	}
	return pKeys;
}

static void ReadVTFHeader( ToolMaterial_t *pMaterial )
{
	pMaterial->nWidth = pMaterial->nHeight = 128;
	pMaterial->flReflectivity[0] = pMaterial->flReflectivity[1] = pMaterial->flReflectivity[2] = 0.18f;
	const char *pszTexture = pMaterial->pKeys->GetString( "$basetexture", NULL );
	if ( !pszTexture || !*pszTexture )
		return;
	char szPath[ MAX_PATH ];
	V_snprintf( szPath, sizeof( szPath ), "materials/%s.vtf", pszTexture );
	V_FixSlashes( szPath );
	FileHandle_t hFile = g_pFullFileSystem->Open( szPath, "rb", "GAME" );
	if ( !hFile )
		return;
	unsigned char header[ 48 ];
	int nRead = g_pFullFileSystem->Read( header, sizeof( header ), hFile );
	g_pFullFileSystem->Close( hFile );
	if ( nRead < 44 || V_memcmp( header, "VTF", 4 ) )
		return;
	// VTFFileHeader_t: width, height at 16; reflectivity (3 floats) at 32.
	unsigned short nWidth, nHeight;
	V_memcpy( &nWidth, header + 16, 2 );
	V_memcpy( &nHeight, header + 18, 2 );
	pMaterial->nWidth = nWidth;
	pMaterial->nHeight = nHeight;
	V_memcpy( pMaterial->flReflectivity, header + 32, sizeof( pMaterial->flReflectivity ) );
}

void InitMaterialSystem( const char *materialBaseDirPath, CreateInterfaceFn fileSystemFactory )
{
}

void ShutdownMaterialSystem( )
{
}

MaterialSystemMaterial_t FindMaterial( const char *materialName, bool *pFound, bool bComplain )
{
	char szName[ MAX_PATH ];
	V_strncpy( szName, materialName, sizeof( szName ) );
	V_strlower( szName );
	V_FixSlashes( szName, '/' );
	int i = s_Materials.Find( szName );
	ToolMaterial_t *pMaterial = i != s_Materials.InvalidIndex() ? s_Materials[i] : NULL;
	if ( !pMaterial )
	{
		KeyValues *pKeys = LoadVMT( szName );
		if ( pKeys )
		{
			pMaterial = new ToolMaterial_t;
			pMaterial->pKeys = pKeys;
			ReadVTFHeader( pMaterial );
		}
		else if ( bComplain )
		{
			Warning( "Material not found: %s\n", materialName );
		}
		s_Materials.Insert( szName, pMaterial );
	}
	if ( pFound )
		*pFound = pMaterial != NULL;
	if ( !pMaterial )
	{
		if ( !s_ErrorMaterial.pKeys )
		{
			s_ErrorMaterial.pKeys = new KeyValues( "LightmappedGeneric" );
			s_ErrorMaterial.nWidth = s_ErrorMaterial.nHeight = 128;
			s_ErrorMaterial.flReflectivity[0] = s_ErrorMaterial.flReflectivity[1] = s_ErrorMaterial.flReflectivity[2] = 0.18f;
		}
		return &s_ErrorMaterial;
	}
	return pMaterial;
}

void GetMaterialDimensions( MaterialSystemMaterial_t materialHandle, int *width, int *height )
{
	ToolMaterial_t *pMaterial = (ToolMaterial_t *)materialHandle;
	*width = pMaterial->nWidth;
	*height = pMaterial->nHeight;
}

void GetMaterialReflectivity( MaterialSystemMaterial_t materialHandle, float *reflectivityVect )
{
	ToolMaterial_t *pMaterial = (ToolMaterial_t *)materialHandle;
	const char *pszValue = pMaterial->pKeys->GetString( "$reflectivity", NULL );
	if ( pszValue && sscanf( pszValue, " [ %f %f %f", &reflectivityVect[0], &reflectivityVect[1], &reflectivityVect[2] ) == 3 )
		return;
	VectorCopy( pMaterial->flReflectivity, reflectivityVect );
}

static bool ShaderUsesLightmap( const char *pszShader )
{
	return V_stristr( pszShader, "lightmapped" ) || V_stristr( pszShader, "worldvertextransition" ) || V_stristr( pszShader, "worldtwotextureblend" );
}

int GetMaterialShaderPropertyBool( MaterialSystemMaterial_t materialHandle, int propID )
{
	ToolMaterial_t *pMaterial = (ToolMaterial_t *)materialHandle;
	bool bLightmap = ShaderUsesLightmap( pMaterial->pKeys->GetName() );
	switch( propID )
	{
	case UTILMATLIB_NEEDS_BUMPED_LIGHTMAPS:
		return bLightmap && *pMaterial->pKeys->GetString( "$bumpmap" );
	case UTILMATLIB_NEEDS_LIGHTMAP:
		return bLightmap;
	default:
		Assert( 0 );
		return 0;
	}
}

int GetMaterialShaderPropertyInt( MaterialSystemMaterial_t materialHandle, int propID )
{
	ToolMaterial_t *pMaterial = (ToolMaterial_t *)materialHandle;
	switch( propID )
	{
	case UTILMATLIB_OPACITY:
		if ( pMaterial->pKeys->GetInt( "$translucent" ) || pMaterial->pKeys->GetInt( "$additive" ) )
			return UTILMATLIB_TRANSLUCENT;
		if ( pMaterial->pKeys->GetInt( "$alphatest" ) )
			return UTILMATLIB_ALPHATEST;
		return UTILMATLIB_OPAQUE;
	default:
		Assert( 0 );
		return 0;
	}
}

const char *GetMaterialVar( MaterialSystemMaterial_t materialHandle, const char *propertyName )
{
	ToolMaterial_t *pMaterial = (ToolMaterial_t *)materialHandle;
	KeyValues *pKey = pMaterial->pKeys->FindKey( propertyName );
	return pKey ? pKey->GetString() : NULL;
}

const char *GetMaterialShaderName( MaterialSystemMaterial_t materialHandle )
{
	ToolMaterial_t *pMaterial = (ToolMaterial_t *)materialHandle;
	return pMaterial->pKeys->GetName();
}

#endif // POSIX
