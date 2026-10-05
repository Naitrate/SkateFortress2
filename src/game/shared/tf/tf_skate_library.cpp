//========= tf2-skate ============//
//
// Purpose: Platform code for tf_skate_library.h. Built without the
//          precompiled header (see client_tf.vpc / server_tf.vpc), so the
//          system headers come first, as in tier1/interface.cpp.
//
//=============================================================================//
#if defined( _WIN32 )
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <io.h>
#else
#include <dlfcn.h>
#include <unistd.h>
#endif

#include "tf_skate_library.h"
#include "tier1/strtools.h"

#if defined( _WIN32 )
static const char *s_pszPrefix = "";
static const char *s_pszSuffix = ".dll";
#else
static const char *s_pszPrefix = "lib";
static const char *s_pszSuffix = ".so";
#endif

// The folder of the module (client or server) this code is linked into.
static bool OwnFolder( char *pszFolder, int nSize )
{
#if defined( _WIN32 )
	HMODULE hModule = NULL;
	if ( !GetModuleHandleExA( GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
							  (LPCSTR)&OwnFolder, &hModule ) )
		return false;
	DWORD nLength = GetModuleFileNameA( hModule, pszFolder, (DWORD)nSize );
	if ( nLength == 0 || nLength >= (DWORD)nSize )
		return false;
#else
	Dl_info info;
	if ( !dladdr( (void *)&OwnFolder, &info ) || !info.dli_fname )
		return false;
	V_strncpy( pszFolder, info.dli_fname, nSize );
#endif
	V_StripFilename( pszFolder );
	return true;
}

void *SkateLibraryLoad( const char *pszStem, char *pszPath, int nPathSize, char *pszError, int nErrorSize )
{
	pszPath[0] = '\0';
	if ( !OwnFolder( pszPath, nPathSize ) )
	{
		V_snprintf( pszError, nErrorSize, "Can't locate this game module to find %s%s%s", s_pszPrefix, pszStem, s_pszSuffix );
		return NULL;
	}
	char szFile[ 128 ];
	V_snprintf( szFile, sizeof( szFile ), "%s%s%s", s_pszPrefix, pszStem, s_pszSuffix );
	V_AppendSlash( pszPath, nPathSize );
	V_strncat( pszPath, szFile, nPathSize );
	V_FixSlashes( pszPath );

#if defined( _WIN32 )
	// Altered search path: the library's own dependencies are found beside it.
	HMODULE hLibrary = LoadLibraryExA( pszPath, NULL, LOAD_WITH_ALTERED_SEARCH_PATH );
	if ( !hLibrary )
	{
		DWORD nError = GetLastError();
		char szMessage[ 256 ] = "";
		FormatMessageA( FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, nError, 0, szMessage, sizeof( szMessage ), NULL );
		V_StripTrailingWhitespace( szMessage );
		V_snprintf( pszError, nErrorSize, "Couldn't load %s: %s (error %lu)", pszPath, szMessage, (unsigned long)nError );
		return NULL;
	}
	return (void *)hLibrary;
#else
	void *pLibrary = dlopen( pszPath, RTLD_NOW | RTLD_LOCAL );
	if ( !pLibrary )
	{
		V_snprintf( pszError, nErrorSize, "Couldn't load %s: %s", pszPath, dlerror() );
		return NULL;
	}
	return pLibrary;
#endif
}

void *SkateLibrarySymbol( void *pLibrary, const char *pszName )
{
	if ( !pLibrary )
		return NULL;
#if defined( _WIN32 )
	return (void *)GetProcAddress( (HMODULE)pLibrary, pszName );
#else
	return dlsym( pLibrary, pszName );
#endif
}

void SkateLibraryClose( void *pLibrary )
{
	if ( !pLibrary )
		return;
#if defined( _WIN32 )
	FreeLibrary( (HMODULE)pLibrary );
#else
	dlclose( pLibrary );
#endif
}

bool SkateFileReadable( const char *pszPath )
{
#if defined( _WIN32 )
	return _access( pszPath, 4 ) == 0;
#else
	return access( pszPath, R_OK ) == 0;
#endif
}
