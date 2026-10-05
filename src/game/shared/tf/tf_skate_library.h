//========= tf2-skate ============//
//
// Purpose: Loading the Skate 3 simulation library (libskate3.so on Linux,
//          skate3.dll on Windows) from beside this client/server module.
//          The platform code lives in tf_skate_library.cpp, which is built
//          without the precompiled header so it can include <windows.h>.
//
//=============================================================================//
#ifndef TF_SKATE_LIBRARY_H
#define TF_SKATE_LIBRARY_H
#ifdef _WIN32
#pragma once
#endif

// Loads the library named `pszStem` ("skate3": libskate3.so / skate3.dll)
// from the folder this module is in. Returns NULL on failure, with the reason
// in pszError. pszPath receives the full path either way (when known).
void *SkateLibraryLoad( const char *pszStem, char *pszPath, int nPathSize, char *pszError, int nErrorSize );
void *SkateLibrarySymbol( void *pLibrary, const char *pszName );
void SkateLibraryClose( void *pLibrary );

// True if a file exists and can be read (outside Source's filesystem).
bool SkateFileReadable( const char *pszPath );

#endif // TF_SKATE_LIBRARY_H
