//========= tf2-skate ============//
//
// Purpose: Runs the Skate 3 simulation in-process: loads bin/linux64/
//          libskate3.so and sends it request frames, documented in
//          skate-engine/crates/skate-game/src/sidecar/protocol.rs (C
//          interface: sidecar/ffi.rs). Shared by server.so (the real
//          skaters) and client.so (the local player's predicted skater).
//
//=============================================================================//
#include "cbase.h"
#include "tf_skate_sidecar.h"
#include "filesystem.h"
#include "utlbuffer.h"
#include "physics_shared.h"
#include "vcollide.h"
#include "engine/IStaticPropMgr.h"
#include "bspfile.h"
#include "tier1/lzmaDecoder.h"
#include "tier1/checksum_crc.h"
#include "usercmd.h"
#ifdef CLIENT_DLL
#include "cdll_client_int.h"
#endif

#include "tf_skate_library.h"

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

// Replicated: the client builds its predicted skater's world with the
// server's scale.
ConVar skate_world_scale( "skate_world_scale", "0.0254", FCVAR_REPLICATED, "Metres per Hammer unit used when converting the map for the Skate simulation." );

enum
{
	SKATE_MSG_HELLO = 1,
	SKATE_MSG_WORLD = 2,
	SKATE_MSG_SPAWN = 3,
	SKATE_MSG_STEP = 4,
	SKATE_MSG_DESPAWN = 5,
	SKATE_MSG_COPY = 6,
	SKATE_MSG_POLL = 7,
};
static const unsigned int SKATE_PROTOCOL_VERSION = 9;
// STEP reply state while the sidecar is still building the skater.
static const unsigned int SKATE_STATE_LOADING_WIRE = 0xFFFFFFFFu;

void SkateInputFromCmd( const CUserCmd *pCmd, SkateInput_t &input )
{
	input.nButtons = pCmd->buttons;
	// cl_forwardspeed / cl_sidespeed default to 450 for a full key press.
	input.flForward = clamp( pCmd->forwardmove / 450.0f, -1.0f, 1.0f );
	input.flSide = clamp( pCmd->sidemove / 450.0f, -1.0f, 1.0f );
	input.flMouseX = (float)pCmd->mousedx;
	input.flMouseY = (float)pCmd->mousedy;
	input.nFlags = 0;
}

CTFSkateSidecar &TFSkateSidecar()
{
	static CTFSkateSidecar s_Sidecar;
	return s_Sidecar;
}

CTFSkateSidecar::CTFSkateSidecar()
	: m_pLibrary( NULL ), m_pHandle( NULL ), m_pfnRequest( NULL ), m_pfnFree( NULL ), m_pfnClose( NULL )
{
	m_szWorld[0] = '\0';
	m_szAssets[0] = '\0';
	m_nWorldCRC = 0;
	m_flNextPrepare = 0.0;
	m_bPreparing = false;
	m_hPrepareThread = NULL;
	m_pPendingWorld = NULL;
	m_szPendingWorld[0] = '\0';
	m_szPrepareError[0] = '\0';
}

void CTFSkateSidecar::LevelShutdown()
{
	IsReady();	// finish a background load first
	// The simulation drops every skater when it receives a new world; the
	// next spawn resends the map.
	m_szWorld[0] = '\0';
	m_nWorldCRC = 0;
}

void CTFSkateSidecar::Disconnect()
{
	if ( m_pHandle && m_pfnClose )
	{
		m_pfnClose( m_pHandle );
	}
	m_pHandle = NULL;
	m_szWorld[0] = '\0';
	m_nWorldCRC = 0;
}

static const char *SkateMapName()
{
#ifdef GAME_DLL
	return STRING( gpGlobals->mapname );
#else
	static char s_szMap[ MAX_PATH ];
	V_FileBase( engine->GetLevelName(), s_szMap, sizeof( s_szMap ) );
	return s_szMap;
#endif
}

// The converted Skate 3 data: <mod>/skate3_data/assets, written by the
// in-game setup (libskate3's converter). SKATE3_ASSET_ROOT overrides it.
static bool FindAssetRoot( char *pszRoot, int nSize )
{
	if ( getenv( "SKATE3_ASSET_ROOT" ) && *getenv( "SKATE3_ASSET_ROOT" ) )
	{
		V_strncpy( pszRoot, getenv( "SKATE3_ASSET_ROOT" ), nSize );
		return true;
	}
	char szGameDir[ MAX_PATH ];
#ifdef GAME_DLL
	engine->GetGameDir( szGameDir, sizeof( szGameDir ) );
#else
	V_strncpy( szGameDir, engine->GetGameDirectory(), sizeof( szGameDir ) );
#endif
	V_snprintf( pszRoot, nSize, "%s/skate3_data/assets", szGameDir );
	char szManifest[ MAX_PATH ];
	V_snprintf( szManifest, sizeof( szManifest ), "%s/private/game.json", pszRoot );
	return SkateFileReadable( szManifest );
}

// Loads libskate3.so / skate3.dll (beside this server / client module) and
// Skate 3's data.
bool CTFSkateSidecar::EnsureConnected( char *pszError, int nErrorSize )
{
	char szAssets[ MAX_PATH ];
	bool bHaveAssets = FindAssetRoot( szAssets, sizeof( szAssets ) );
	if ( m_pHandle )
	{
		// Setup converted new data since it was loaded: start over with it.
		if ( !bHaveAssets || !V_strcmp( szAssets, m_szAssets ) )
			return true;
		Msg( "[skate] Skate 3 data moved to %s; reloading\n", szAssets );
		Disconnect();
	}

	typedef void *( *OpenFn )( const char *, char *, size_t );
	typedef unsigned int ( *VersionFn )();
	if ( !m_pLibrary )
	{
		// libskate3.so / skate3.dll, in the same folder as this module.
		char szPath[ MAX_PATH ];
		m_pLibrary = SkateLibraryLoad( "skate3", szPath, sizeof( szPath ), pszError, nErrorSize );
		if ( !m_pLibrary )
		{
			V_strncat( pszError, " (build it with ./build-skate-lib.sh)", nErrorSize );
			return false;
		}
		VersionFn pfnVersion = (VersionFn)SkateLibrarySymbol( m_pLibrary, "skate3_protocol_version" );
		m_pfnRequest = (RequestFn)SkateLibrarySymbol( m_pLibrary, "skate3_request" );
		m_pfnFree = (FreeFn)SkateLibrarySymbol( m_pLibrary, "skate3_free" );
		m_pfnClose = (CloseFn)SkateLibrarySymbol( m_pLibrary, "skate3_close" );
		if ( !pfnVersion || !m_pfnRequest || !m_pfnFree || !m_pfnClose || !SkateLibrarySymbol( m_pLibrary, "skate3_open" ) )
		{
			V_snprintf( pszError, nErrorSize, "%s is missing skate3_* functions", szPath );
			SkateLibraryClose( m_pLibrary );
			m_pLibrary = NULL;
			return false;
		}
		if ( pfnVersion() != SKATE_PROTOCOL_VERSION )
		{
			V_snprintf( pszError, nErrorSize, "%s speaks protocol %u, this game needs %u; rebuild both", szPath, pfnVersion(), SKATE_PROTOCOL_VERSION );
			SkateLibraryClose( m_pLibrary );
			m_pLibrary = NULL;
			return false;
		}
		Msg( "[skate] loaded %s\n", szPath );
	}

	if ( !bHaveAssets )
	{
		V_snprintf( pszError, nErrorSize, "Skate 3 data isn't set up yet: Options > Advanced > Skate 3 setup (or the skate_setup command)." );
		return false;
	}
	OpenFn pfnOpen = (OpenFn)SkateLibrarySymbol( m_pLibrary, "skate3_open" );
	char szOpenError[ 512 ];
	szOpenError[0] = '\0';
	double flStart = Plat_FloatTime();
	m_pHandle = pfnOpen( szAssets, szOpenError, sizeof( szOpenError ) );
	if ( !m_pHandle )
	{
		V_snprintf( pszError, nErrorSize, "Skate 3 data in %s didn't load: %s", szAssets, szOpenError );
		return false;
	}
	Msg( "[skate] Skate 3 data loaded from %s in %.1fs\n", szAssets, Plat_FloatTime() - flStart );
	V_strncpy( m_szAssets, szAssets, sizeof( m_szAssets ) );
	m_szWorld[0] = '\0';
	return true;
}

// Runs one request in the simulation. On success `reply` is positioned after
// the ok byte; on failure the simulation's reason is copied to pszError.
bool CTFSkateSidecar::Request( unsigned char nType, const CUtlBuffer &payload, CUtlBuffer &reply, char *pszError, int nErrorSize )
{
	if ( !m_pHandle )
	{
		V_snprintf( pszError, nErrorSize, "Skate 3 simulation isn't loaded" );
		return false;
	}
	unsigned char *pReply = NULL;
	size_t nReplySize = 0;
	if ( m_pfnRequest( m_pHandle, nType, (const unsigned char *)payload.Base(), payload.TellPut(), &pReply, &nReplySize ) != 0 || !pReply || nReplySize < 1 )
	{
		if ( pReply )
			m_pfnFree( pReply, nReplySize );
		V_snprintf( pszError, nErrorSize, "Skate 3 simulation rejected the request" );
		return false;
	}
	reply.Clear();
	reply.Put( pReply, (int)nReplySize );
	m_pfnFree( pReply, nReplySize );

	if ( reply.GetUnsignedChar() == 0 )
	{
		int nMessage = reply.GetInt();
		nMessage = clamp( nMessage, 0, MIN( nErrorSize - 1, reply.GetBytesRemaining() ) );
		reply.Get( pszError, nMessage );
		pszError[ nMessage ] = '\0';
		return false;
	}
	return true;
}

static void PutString( CUtlBuffer &buf, const char *psz )
{
	int nLength = V_strlen( psz );
	buf.PutInt( nLength );
	buf.Put( psz, nLength );
}

//-----------------------------------------------------------------------------
// The map's entities, read from the BSP's entity lump. Server and client both
// read them from the file (the client doesn't receive every entity), so they
// build the same world.
//-----------------------------------------------------------------------------
struct SkateMapEntity_t
{
	CUtlVector< CUtlString > keys;
	CUtlVector< CUtlString > values;

	const char *Get( const char *pszKey, const char *pszDefault = "" ) const
	{
		FOR_EACH_VEC( keys, i )
		{
			if ( !V_stricmp( keys[i].Get(), pszKey ) )
				return values[i].Get();
		}
		return pszDefault;
	}
	Vector GetVector( const char *pszKey ) const
	{
		Vector v( 0, 0, 0 );
		sscanf( Get( pszKey, "0 0 0" ), "%f %f %f", &v.x, &v.y, &v.z );
		return v;
	}
};

static bool ReadEntityLump( const CUtlBuffer &bsp, CUtlVector< char > &text )
{
	if ( bsp.TellPut() < (int)sizeof( dheader_t ) )
		return false;
	const dheader_t *pHeader = (const dheader_t *)bsp.Base();
	const lump_t &lump = pHeader->lumps[ LUMP_ENTITIES ];
	if ( lump.fileofs < 0 || lump.filelen <= 0 || lump.fileofs + lump.filelen > bsp.TellPut() )
		return false;
	unsigned char *pLump = (unsigned char *)bsp.Base() + lump.fileofs;
	if ( lump.uncompressedSize && CLZMA::IsCompressed( pLump ) )
	{
		unsigned int nSize = CLZMA::GetActualSize( pLump );
		text.SetCount( nSize + 1 );
		if ( CLZMA::Uncompress( pLump, (unsigned char *)text.Base() ) != nSize )
			return false;
		text[ nSize ] = '\0';
		return true;
	}
	text.SetCount( lump.filelen + 1 );
	V_memcpy( text.Base(), pLump, lump.filelen );
	text[ lump.filelen ] = '\0';
	return true;
}

static void ParseEntities( const char *pszText, CUtlVector< SkateMapEntity_t > &entities )
{
	const char *p = pszText;
	SkateMapEntity_t *pEntity = NULL;
	char szToken[ 2 ][ 1024 ];
	int nPart = 0;
	while ( *p )
	{
		if ( *p == '{' )
		{
			pEntity = &entities[ entities.AddToTail() ];
			nPart = 0;
			++p;
		}
		else if ( *p == '}' )
		{
			pEntity = NULL;
			++p;
		}
		else if ( *p == '"' )
		{
			const char *pEnd = strchr( p + 1, '"' );
			if ( !pEnd )
				break;
			int nLength = MIN( (int)( pEnd - p - 1 ), (int)sizeof( szToken[0] ) - 1 );
			V_memcpy( szToken[ nPart ], p + 1, nLength );
			szToken[ nPart ][ nLength ] = '\0';
			if ( ++nPart == 2 )
			{
				if ( pEntity )
				{
					pEntity->keys.AddToTail( CUtlString( szToken[0] ) );
					pEntity->values.AddToTail( CUtlString( szToken[1] ) );
				}
				nPart = 0;
			}
			p = pEnd + 1;
		}
		else
		{
			++p;
		}
	}
}

//-----------------------------------------------------------------------------
// Collision the BSP's world model doesn't hold: static props (their .phy
// models) and solid func_brush entities. Triangles in world space, wound out
// of the solid.
//-----------------------------------------------------------------------------
static void AddCollideTriangles( const CPhysCollide *pCollide, const matrix3x4_t &toWorld, CUtlVector< Vector > &tris )
{
	Vector *pVerts = NULL;
	int nVerts = physcollision->CreateDebugMesh( pCollide, &pVerts );
	if ( nVerts < 3 || !pVerts )
		return;
	// The debug mesh is closed convex pieces with one consistent winding; its
	// signed volume says whether that winding points out of the solid.
	float flVolume = 0.0f;
	for ( int i = 0; i + 2 < nVerts; i += 3 )
	{
		flVolume += DotProduct( pVerts[i], CrossProduct( pVerts[i + 1], pVerts[i + 2] ) );
	}
	bool bFlip = flVolume < 0.0f;
	for ( int i = 0; i + 2 < nVerts; i += 3 )
	{
		Vector a, b, c;
		VectorTransform( pVerts[i], toWorld, a );
		VectorTransform( pVerts[i + 1], toWorld, b );
		VectorTransform( pVerts[i + 2], toWorld, c );
		tris.AddToTail( a );
		tris.AddToTail( bFlip ? c : b );
		tris.AddToTail( bFlip ? b : c );
	}
	physcollision->DestroyDebugMesh( nVerts, pVerts );
}

static void AddModelTriangles( const model_t *pModel, const matrix3x4_t &toWorld, CUtlVector< Vector > &tris )
{
	vcollide_t *pVCollide = pModel ? modelinfo->GetVCollide( pModel ) : NULL;
	if ( !pVCollide )
		return;
	for ( int i = 0; i < pVCollide->solidCount; ++i )
	{
		AddCollideTriangles( pVCollide->solids[i], toWorld, tris );
	}
}

static int CollectExtraCollision( const CUtlVector< SkateMapEntity_t > &entities, CUtlVector< Vector > &tris )
{
	int nSources = 0;
	CUtlVector< ICollideable * > props;
	staticpropmgr->GetAllStaticProps( &props );
	FOR_EACH_VEC( props, i )
	{
		ICollideable *pProp = props[i];
		if ( !pProp || pProp->GetSolid() != SOLID_VPHYSICS )
			continue;
		int nBefore = tris.Count();
		AddModelTriangles( pProp->GetCollisionModel(), pProp->CollisionToWorldTransform(), tris );
		nSources += tris.Count() > nBefore;
	}

	// Static, solid brush entities as the map places them. Doors, platforms
	// and other movers are left out: Skate's world is static.
	FOR_EACH_VEC( entities, i )
	{
		const SkateMapEntity_t &ent = entities[i];
		if ( V_stricmp( ent.Get( "classname" ), "func_brush" ) )
			continue;
		int nSolidity = atoi( ent.Get( "Solidity", "0" ) );	// 0 toggle, 1 never, 2 always
		if ( nSolidity == 1 || ( nSolidity == 0 && atoi( ent.Get( "StartDisabled", "0" ) ) ) )
			continue;
		const char *pszModel = ent.Get( "model" );
		if ( pszModel[0] != '*' )
			continue;
		int nModel = modelinfo->GetModelIndex( pszModel );
		const model_t *pModel = nModel >= 0 ? modelinfo->GetModel( nModel ) : NULL;
		Vector vecAngles = ent.GetVector( "angles" );
		matrix3x4_t toWorld;
		AngleMatrix( QAngle( vecAngles.x, vecAngles.y, vecAngles.z ), ent.GetVector( "origin" ), toWorld );
		int nBefore = tris.Count();
		AddModelTriangles( pModel, toWorld, tris );
		nSources += tris.Count() > nBefore;
	}
	return nSources;
}

//-----------------------------------------------------------------------------
// skate_rail: a grind rail for Hammer maps. Place a chain of skate_rail nodes
// along a rail, ledge or coping, each "target"-ing the next (like path_track);
// a chain that loops back on itself is a closed rail. They become Skate 3
// rail splines in the simulation. See skate.fgd.
//-----------------------------------------------------------------------------
#ifdef GAME_DLL
class CSkateRailNode : public CPointEntity
{
public:
	DECLARE_CLASS( CSkateRailNode, CPointEntity );
};
LINK_ENTITY_TO_CLASS( skate_rail, CSkateRailNode );
#endif

struct SkateRail_t
{
	CUtlString		name;
	bool			bClosed;
	CUtlVector< Vector > points;
};

static void CollectRails( const CUtlVector< SkateMapEntity_t > &entities, CUtlVector< SkateRail_t > &rails )
{
	CUtlVector< const SkateMapEntity_t * > nodes;
	FOR_EACH_VEC( entities, i )
	{
		if ( !V_stricmp( entities[i].Get( "classname" ), "skate_rail" ) )
			nodes.AddToTail( &entities[i] );
	}
	CUtlVector< int > next, targeted;
	next.SetCount( nodes.Count() );
	targeted.SetCount( nodes.Count() );
	FOR_EACH_VEC( nodes, i )
	{
		next[i] = -1;
		targeted[i] = 0;
	}
	FOR_EACH_VEC( nodes, i )
	{
		const char *pszTarget = nodes[i]->Get( "target" );
		if ( !*pszTarget )
			continue;
		FOR_EACH_VEC( nodes, j )
		{
			if ( j != i && !V_stricmp( nodes[j]->Get( "targetname" ), pszTarget ) )
			{
				next[i] = j;
				targeted[j]++;
				break;
			}
		}
	}

	CUtlVector< bool > visited;
	visited.SetCount( nodes.Count() );
	FOR_EACH_VEC( visited, i )
		visited[i] = false;
	// Open rails start at a node nothing targets; what's left are loops.
	for ( int pass = 0; pass < 2; ++pass )
	{
		FOR_EACH_VEC( nodes, start )
		{
			if ( visited[ start ] || ( pass == 0 && targeted[ start ] ) )
				continue;
			SkateRail_t &rail = rails[ rails.AddToTail() ];
			rail.bClosed = false;
			const char *pszName = nodes[ start ]->Get( "targetname" );
			rail.name = *pszName ? pszName : CFmtStr( "skate_rail_%d", rails.Count() ).Get();
			for ( int i = start; i >= 0; i = next[i] )
			{
				if ( visited[i] )
				{
					rail.bClosed = ( i == start );
					break;
				}
				visited[i] = true;
				rail.points.AddToTail( nodes[i]->GetVector( "origin" ) );
			}
			if ( rail.points.Count() < 2 )
			{
#ifdef GAME_DLL
				Vector vecAt = nodes[ start ]->GetVector( "origin" );
				Warning( "[skate] skate_rail \"%s\" at %.0f %.0f %.0f leads nowhere; give it a target\n", rail.name.Get(), vecAt.x, vecAt.y, vecAt.z );
#endif
				rails.RemoveMultipleFromTail( 1 );
			}
		}
	}
}

// The WORLD request for the current map: name, scale, the BSP, extra
// collision triangles, rails.
static bool BuildWorldPayload( const char *pszMap, CUtlBuffer &payload, char *pszError, int nErrorSize )
{
	char szPath[ MAX_PATH ];
	V_snprintf( szPath, sizeof( szPath ), "maps/%s.bsp", pszMap );
	CUtlBuffer bsp;
	if ( !g_pFullFileSystem->ReadFile( szPath, "GAME", bsp ) )
	{
		V_snprintf( pszError, nErrorSize, "Could not read %s for the skate world", szPath );
		return false;
	}
	CUtlVector< SkateMapEntity_t > entities;
	CUtlVector< char > text;
	if ( ReadEntityLump( bsp, text ) )
	{
		ParseEntities( text.Base(), entities );
	}

	PutString( payload, pszMap );
	payload.PutFloat( skate_world_scale.GetFloat() );
	payload.PutInt( bsp.TellPut() );
	payload.Put( bsp.Base(), bsp.TellPut() );

	CUtlVector< Vector > extra;
	int nSources = CollectExtraCollision( entities, extra );
	payload.PutUnsignedInt( extra.Count() / 3 );
	FOR_EACH_VEC( extra, i )
	{
		payload.PutFloat( extra[i].x );
		payload.PutFloat( extra[i].y );
		payload.PutFloat( extra[i].z );
	}

	CUtlVector< SkateRail_t > rails;
	CollectRails( entities, rails );
	payload.PutUnsignedInt( rails.Count() );
	FOR_EACH_VEC( rails, r )
	{
		PutString( payload, rails[r].name.Get() );
		payload.PutUnsignedInt( rails[r].bClosed ? 1 : 0 );
		payload.PutUnsignedInt( rails[r].points.Count() );
		FOR_EACH_VEC( rails[r].points, p )
		{
			payload.PutFloat( rails[r].points[p].x );
			payload.PutFloat( rails[r].points[p].y );
			payload.PutFloat( rails[r].points[p].z );
		}
	}
	DevMsg( "[skate] %s: %d prop/brush-entity collision triangles from %d models, %d rails\n", pszMap, extra.Count() / 3, nSources, rails.Count() );
	return true;
}

static unsigned int PayloadCRC( const CUtlBuffer &payload )
{
	CRC32_t crc;
	CRC32_Init( &crc );
	CRC32_ProcessBuffer( &crc, payload.Base(), payload.TellPut() );
	CRC32_Final( &crc );
	return crc;
}

bool CTFSkateSidecar::EnsureWorld( char *pszError, int nErrorSize )
{
	const char *pszMap = SkateMapName();
	if ( !V_strcmp( m_szWorld, pszMap ) )
		return true;

	CUtlBuffer payload;
	if ( !BuildWorldPayload( pszMap, payload, pszError, nErrorSize ) )
		return false;
	unsigned int nCRC = PayloadCRC( payload );
	CUtlBuffer reply;
	if ( !Request( SKATE_MSG_WORLD, payload, reply, pszError, nErrorSize ) )
		return false;

	char szMessage[ 256 ];
	int nMessage = clamp( reply.GetInt(), 0, (int)sizeof( szMessage ) - 1 );
	reply.Get( szMessage, nMessage );
	szMessage[ nMessage ] = '\0';
	Msg( "[skate] world %s\n", szMessage );

	V_strncpy( m_szWorld, pszMap, sizeof( m_szWorld ) );
	m_nWorldCRC = nCRC;
	return true;
}

void CTFSkateSidecar::Prepare()
{
	// Quietly: skating is optional until someone actually skates. Called
	// on every player spawn, so a failure isn't retried for a while.
	if ( m_bPreparing || ( m_szWorld[0] && m_pHandle ) || Plat_FloatTime() < m_flNextPrepare )
		return;
	char szError[ 512 ];
	if ( !EnsureConnected( szError, sizeof( szError ) ) || !EnsureWorld( szError, sizeof( szError ) ) )
	{
		DevMsg( "[skate] not prepared: %s\n", szError );
		m_flNextPrepare = Plat_FloatTime() + 60.0;
	}
}

//-----------------------------------------------------------------------------
// Background preparation. Loading Skate 3's data and converting the map take
// a second or two; the client does it on a worker thread so the game doesn't
// stall. The map and its props are read here first (engine interfaces belong
// to the main thread); the thread only talks to the library.
//-----------------------------------------------------------------------------
void CTFSkateSidecar::PrepareAsync()
{
	const char *pszMap = SkateMapName();
	if ( m_bPreparing || !*pszMap || ( m_pHandle && !V_strcmp( m_szWorld, pszMap ) ) || Plat_FloatTime() < m_flNextPrepare )
		return;
	char szError[ 512 ];
	// The library itself loads here (fast); skate3_open happens on the thread.
	char szAssets[ MAX_PATH ];
	if ( !FindAssetRoot( szAssets, sizeof( szAssets ) ) )
	{
		m_flNextPrepare = Plat_FloatTime() + 60.0;
		return;
	}
	if ( m_pHandle && V_strcmp( szAssets, m_szAssets ) )
	{
		Disconnect();
	}
	m_pPendingWorld = new CUtlBuffer;
	if ( !BuildWorldPayload( pszMap, *m_pPendingWorld, szError, sizeof( szError ) ) )
	{
		DevMsg( "[skate] not prepared: %s\n", szError );
		delete m_pPendingWorld;
		m_pPendingWorld = NULL;
		m_flNextPrepare = Plat_FloatTime() + 60.0;
		return;
	}
	V_strncpy( m_szPendingWorld, pszMap, sizeof( m_szPendingWorld ) );
	m_szPrepareError[0] = '\0';
	m_bPreparing = true;
	m_hPrepareThread = CreateSimpleThread( PrepareThread, this );
	if ( !m_hPrepareThread )
	{
		m_bPreparing = false;
		delete m_pPendingWorld;
		m_pPendingWorld = NULL;
	}
}

uintp CTFSkateSidecar::PrepareThread( void *pParam )
{
	CTFSkateSidecar *pThis = (CTFSkateSidecar *)pParam;
	char *pszError = pThis->m_szPrepareError;
	int nErrorSize = sizeof( pThis->m_szPrepareError );
	if ( pThis->EnsureConnected( pszError, nErrorSize ) )
	{
		CUtlBuffer reply;
		if ( pThis->Request( SKATE_MSG_WORLD, *pThis->m_pPendingWorld, reply, pszError, nErrorSize ) )
		{
			pThis->m_nWorldCRC = PayloadCRC( *pThis->m_pPendingWorld );
			V_strncpy( pThis->m_szWorld, pThis->m_szPendingWorld, sizeof( pThis->m_szWorld ) );
			pszError[0] = '\0';
		}
	}
	ThreadMemoryBarrier();
	pThis->m_bPreparing = false;
	return 0;
}

bool CTFSkateSidecar::IsReady()
{
	if ( m_hPrepareThread )
	{
		if ( m_bPreparing )
			return false;
		ThreadJoin( m_hPrepareThread );
		ReleaseThreadHandle( m_hPrepareThread );
		m_hPrepareThread = NULL;
		delete m_pPendingWorld;
		m_pPendingWorld = NULL;
		if ( m_szPrepareError[0] )
		{
			DevMsg( "[skate] not prepared: %s\n", m_szPrepareError );
			m_flNextPrepare = Plat_FloatTime() + 60.0;
		}
		else
		{
			Msg( "[skate] simulation ready for %s\n", m_szWorld );
		}
	}
	return m_pHandle && !V_strcmp( m_szWorld, SkateMapName() );
}

bool CTFSkateSidecar::Spawn( int id, const Vector &vecOrigin, float flYaw, const char *pszDifficulty, char *pszError, int nErrorSize )
{
	if ( m_bPreparing )
	{
		V_snprintf( pszError, nErrorSize, "Skate 3 simulation is still loading" );
		return false;
	}
	if ( !EnsureConnected( pszError, nErrorSize ) || !EnsureWorld( pszError, nErrorSize ) )
		return false;

	CUtlBuffer payload;
	payload.PutUnsignedInt( id );
	payload.PutFloat( vecOrigin.x );
	payload.PutFloat( vecOrigin.y );
	payload.PutFloat( vecOrigin.z );
	payload.PutFloat( flYaw );
	PutString( payload, pszDifficulty );

	CUtlBuffer reply;
	if ( !Request( SKATE_MSG_SPAWN, payload, reply, pszError, nErrorSize ) )
		return false;

	char szMessage[ 256 ];
	int nMessage = clamp( reply.GetInt(), 0, (int)sizeof( szMessage ) - 1 );
	reply.Get( szMessage, nMessage );
	szMessage[ nMessage ] = '\0';
	DevMsg( "[skate] %s\n", szMessage );
	return true;
}

void CTFSkateSidecar::Despawn( int id )
{
	if ( !m_pHandle || m_bPreparing )
		return;
	CUtlBuffer payload;
	payload.PutUnsignedInt( id );
	CUtlBuffer reply;
	char szError[ 256 ];
	if ( !Request( SKATE_MSG_DESPAWN, payload, reply, szError, sizeof( szError ) ) )
	{
		Warning( "[skate] despawn failed: %s\n", szError );
	}
}

int CTFSkateSidecar::Poll( int id, char *pszError, int nErrorSize )
{
	if ( m_bPreparing )
		return 0;
	CUtlBuffer payload;
	payload.PutUnsignedInt( id );
	CUtlBuffer reply;
	if ( !Request( SKATE_MSG_POLL, payload, reply, pszError, nErrorSize ) )
		return -1;
	return reply.GetUnsignedInt() ? 1 : 0;
}

bool CTFSkateSidecar::Copy( int src, int dst, char *pszError, int nErrorSize )
{
	if ( m_bPreparing )
	{
		V_snprintf( pszError, nErrorSize, "Skate 3 simulation is still loading" );
		return false;
	}
	CUtlBuffer payload;
	payload.PutUnsignedInt( src );
	payload.PutUnsignedInt( dst );
	CUtlBuffer reply;
	return Request( SKATE_MSG_COPY, payload, reply, pszError, nErrorSize );
}

static void GetVector( CUtlBuffer &buf, Vector &v )
{
	v.x = buf.GetFloat();
	v.y = buf.GetFloat();
	v.z = buf.GetFloat();
}

static void GetAngles( CUtlBuffer &buf, QAngle &a )
{
	a.x = buf.GetFloat();
	a.y = buf.GetFloat();
	a.z = buf.GetFloat();
}

bool CTFSkateSidecar::Step( int id, float flDeltaTime, const SkateInput_t &input, float flMouseGain, float flMouseDecay, SkateStepResult_t &result, char *pszError, int nErrorSize )
{
	if ( !m_pHandle || m_bPreparing )
	{
		V_snprintf( pszError, nErrorSize, "Skate 3 simulation isn't loaded" );
		return false;
	}

	CUtlBuffer payload;
	payload.PutUnsignedInt( id );
	payload.PutFloat( flDeltaTime );
	payload.PutUnsignedInt( (unsigned int)input.nButtons );
	payload.PutFloat( input.flForward );
	payload.PutFloat( input.flSide );
	payload.PutFloat( input.flMouseX );
	payload.PutFloat( input.flMouseY );
	payload.PutUnsignedInt( (unsigned int)input.nFlags );
	// The player's own flick-stick feel (0 keeps the defaults).
	payload.PutFloat( flMouseGain );
	payload.PutFloat( flMouseDecay );

	CUtlBuffer reply;
	if ( !Request( SKATE_MSG_STEP, payload, reply, pszError, nErrorSize ) )
		return false;

	unsigned int nState = reply.GetUnsignedInt();
	if ( nState == SKATE_STATE_LOADING_WIRE )
	{
		result.nState = SKATE_STATE_LOADING;
		result.nTicks = 0;
		return true;
	}
	result.nState = (int)nState;
	result.nTicks = reply.GetInt();
	GetVector( reply, result.vecOrigin );
	GetAngles( reply, result.angBody );
	GetVector( reply, result.vecVelocity );
	GetVector( reply, result.vecDeckOrigin );
	GetAngles( reply, result.angDeck );
	GetVector( reply, result.vecCameraOrigin );
	GetAngles( reply, result.angCamera );
	result.flCameraFov = reply.GetFloat();
	unsigned int nJoints = reply.GetUnsignedInt();
	if ( nJoints != SKATE_JOINT_COUNT )
	{
		V_snprintf( pszError, nErrorSize, "Skate sidecar sent %u joints, expected %d", nJoints, SKATE_JOINT_COUNT );
		return false;
	}
	for ( int i = 0; i < SKATE_JOINT_COUNT; ++i )
	{
		GetVector( reply, result.vecJoints[i] );
	}
	result.nTrickSeq = reply.GetInt();
	int nName = reply.GetInt();
	int nKeep = clamp( nName, 0, (int)sizeof( result.szTrick ) - 1 );
	reply.Get( result.szTrick, nKeep );
	result.szTrick[ nKeep ] = '\0';
	if ( nName > nKeep )
		reply.SeekGet( CUtlBuffer::SEEK_CURRENT, nName - nKeep );
	result.flTrickScore = reply.GetFloat();
	result.flLineScore = reply.GetFloat();
	result.flMultiplier = reply.GetFloat();
	result.flTotalScore = reply.GetFloat();
	result.nScoreFlags = reply.GetInt();
	if ( !reply.IsValid() )
	{
		V_snprintf( pszError, nErrorSize, "Skate sidecar step reply was too short" );
		return false;
	}
	return true;
}
