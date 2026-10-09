//========= Copyright Valve Corporation, All rights reserved. ============//
//
// Purpose: tf2-skate: entry point for the Linux vrad executable. Windows runs
// vrad_launcher, which loads vrad_dll and calls the same VRAD_Main.
//
//=============================================================================//

#include "tier0/icommandline.h"

int VRAD_Main( int argc, char **argv );

int main( int argc, char **argv )
{
	// As vrad_launcher does: the file system reads -game from here.
	CommandLine()->CreateCmdLine( argc, argv );
	return VRAD_Main( argc, argv );
}
