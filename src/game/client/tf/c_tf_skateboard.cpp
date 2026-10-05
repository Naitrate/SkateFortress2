//========= tf2-skate ============//
//
// Purpose: Procedural skateboard for skating players. See c_tf_skateboard.h.
//
//=============================================================================//
#include "cbase.h"
#include "c_tf_skateboard.h"
#include "c_tf_player.h"
#include "materialsystem/imesh.h"
#include "model_types.h"
#include "materialsystem/imaterialvar.h"
#include "KeyValues.h"
#include "soundenvelope.h"
#include "physics_shared.h"
#include "decals.h"
#include "filesystem.h"
#include "utldict.h"
#include "utlbuffer.h"

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

// Deck-space tuning while the Skate deck axes are being matched to Source.
ConVar cl_skate_board_yaw( "cl_skate_board_yaw", "0", 0, "Extra yaw (degrees) applied to the drawn skateboard." );
ConVar cl_skate_board_z( "cl_skate_board_z", "0", 0, "Vertical offset (units) of the drawn skateboard from the simulated deck body." );

// Board dimensions in Hammer units (1 unit ~ 1 inch): an 8" x 32" deck.
static const float DECK_HALF_LENGTH = 16.0f;
static const float DECK_HALF_WIDTH = 4.0f;
static const float DECK_HALF_THICKNESS = 0.3f;
static const float TRUCK_OFFSET = 10.5f;
static const float WHEEL_RADIUS = 1.1f;

//-----------------------------------------------------------------------------
// Skate 3's own board (models/skate/board.skbd, written by
// the in-game Skate 3 setup, libskate3's converter). Without it
// the board is drawn as boxes.
//-----------------------------------------------------------------------------
struct SkateBoardVertex_t
{
	Vector	pos;
	Vector	normal;
	float	uv[2];
};

struct SkateBoardPart_t
{
	CMaterialReference					material;
	CUtlVector< SkateBoardVertex_t >	verts;
	CUtlVector< unsigned short >		indices;
};

static CUtlVector< SkateBoardPart_t * > s_BoardParts;
static Vector s_vecBoardMins, s_vecBoardMaxs;

static void UnloadBoardModel()
{
	s_BoardParts.PurgeAndDeleteElements();
}

static void LoadBoardModel()
{
	UnloadBoardModel();
	CUtlBuffer buf;
	if ( !g_pFullFileSystem->ReadFile( "models/skate/board.skbd", "GAME", buf ) )
		return;
	char szMagic[4];
	buf.Get( szMagic, 4 );
	if ( V_memcmp( szMagic, "SKBD", 4 ) || buf.GetUnsignedInt() != 1 )
	{
		Warning( "[skate] models/skate/board.skbd: unknown format\n" );
		return;
	}
	s_vecBoardMins.Init( FLT_MAX, FLT_MAX, FLT_MAX );
	s_vecBoardMaxs.Init( -FLT_MAX, -FLT_MAX, -FLT_MAX );
	unsigned int nParts = buf.GetUnsignedInt();
	for ( unsigned int i = 0; i < nParts && buf.IsValid(); ++i )
	{
		char szMaterial[ MAX_PATH ];
		unsigned int nName = buf.GetUnsignedInt();
		if ( nName >= sizeof( szMaterial ) )
			break;
		buf.Get( szMaterial, nName );
		szMaterial[ nName ] = '\0';
		SkateBoardPart_t *pPart = new SkateBoardPart_t;
		s_BoardParts.AddToTail( pPart );
		pPart->material.Init( szMaterial, TEXTURE_GROUP_MODEL );
		unsigned int nVerts = buf.GetUnsignedInt();
		if ( nVerts > 65535 )
			break;
		pPart->verts.SetCount( nVerts );
		buf.Get( pPart->verts.Base(), nVerts * sizeof( SkateBoardVertex_t ) );
		unsigned int nIndices = buf.GetUnsignedInt();
		if ( nIndices > 3 * 65535 )
			break;
		pPart->indices.SetCount( nIndices );
		buf.Get( pPart->indices.Base(), nIndices * sizeof( unsigned short ) );
		for ( int v = 0; v < pPart->verts.Count(); ++v )
		{
			VectorMin( s_vecBoardMins, pPart->verts[v].pos, s_vecBoardMins );
			VectorMax( s_vecBoardMaxs, pPart->verts[v].pos, s_vecBoardMaxs );
		}
		for ( int k = 0; k < pPart->indices.Count(); ++k )
		{
			if ( pPart->indices[k] >= nVerts )
				pPart->indices[k] = 0;
		}
	}
	if ( !buf.IsValid() )
	{
		Warning( "[skate] models/skate/board.skbd is truncated\n" );
		UnloadBoardModel();
		return;
	}
	DevMsg( "[skate] board model: %d parts\n", s_BoardParts.Count() );
}

C_TFSkateboard *C_TFSkateboard::CreateFollowing( C_BaseEntity *pFollow )
{
	C_TFSkateboard *pBoard = Create( NULL );
	if ( pBoard )
	{
		// A child of the prop, so it's drawn wherever the prop's
		// interpolated transform puts it this frame.
		pBoard->m_hFollow = pFollow;
		pBoard->SetParent( pFollow );
		pBoard->SetLocalOrigin( vec3_origin );
		pBoard->SetLocalAngles( vec3_angle );
	}
	return pBoard;
}

C_TFSkateboard *C_TFSkateboard::Create( C_TFPlayer *pOwner )
{
	C_TFSkateboard *pBoard = new C_TFSkateboard;
	if ( !pBoard->InitializeAsClientEntity( NULL, RENDER_GROUP_OPAQUE_ENTITY ) )
	{
		pBoard->Release();
		return NULL;
	}

	pBoard->m_hOwner = pOwner;
	pBoard->m_hFollow = NULL;
	pBoard->m_pRolling = NULL;
	pBoard->m_pszRolling = NULL;
	pBoard->m_flNextSurfaceCheck = 0.0f;
	pBoard->m_nLastState = pOwner ? pOwner->GetSkateState() : 0;
	pBoard->m_flLastSpeed = 0.0f;
	pBoard->m_flAirFallSpeed = 0.0f;
	pBoard->m_angLastDeck = pOwner ? pOwner->GetSkateDeckAngles() : vec3_angle;
	pBoard->m_flNextFlip = 0.0f;
	pBoard->m_flNextImpact = 0.0f;
	pBoard->m_vecLastOwnerVelocity = vec3_origin;
	pBoard->m_flNextBodyImpact = 0.0f;
	pBoard->m_pGrind = NULL;
	pBoard->m_pSkid = NULL;
	pBoard->m_chSurface = CHAR_TEX_CONCRETE;
	pBoard->m_flAirStart = 0.0f;
	pBoard->m_szGrindLoop[0] = '\0';

	KeyValues *pKeys = new KeyValues( "UnlitGeneric" );
	pKeys->SetString( "$basetexture", "vgui/white" );
	pKeys->SetInt( "$vertexcolor", 1 );
	pKeys->SetInt( "$nocull", 1 );
	pBoard->m_Material.Init( materials->CreateMaterial( "__tf_skateboard", pKeys ) );

	pBoard->UpdateTransform();
	pBoard->SetNextClientThink( CLIENT_THINK_ALWAYS );
	return pBoard;
}

void C_TFSkateboard::Destroy()
{
	StopRolling();
	UpdateLoop( m_pGrind, NULL, false, 0, 0 );
	UpdateLoop( m_pSkid, NULL, false, 0, 0 );
	m_Material.Shutdown();
	Release();
}

void C_TFSkateboard::UpdateTransform()
{
	if ( m_hFollow.Get() )
		return;	// parented to it
	C_TFPlayer *pOwner = m_hOwner.Get();
	if ( !pOwner )
		return;

	QAngle angDeck = pOwner->GetSkateDeckAngles();
	angDeck[ YAW ] += cl_skate_board_yaw.GetFloat();
	Vector vecUp;
	AngleVectors( angDeck, NULL, NULL, &vecUp );
	SetAbsOrigin( pOwner->GetAbsOrigin() + pOwner->GetSkateDeckOffset() + vecUp * cl_skate_board_z.GetFloat() );
	SetAbsAngles( angDeck );
}

void C_TFSkateboard::ClientThink()
{
	UpdateTransform();
	if ( m_hFollow.Get() )
		return;	// a dropped board: physics impact sounds come from the server
	UpdateSound();
	UpdateEvents();
}

ConVar cl_skate_roll_volume( "cl_skate_roll_volume", "0.8", FCVAR_ARCHIVE, "Volume of the Skate 3 wheel rolling loop at full speed." );

void C_TFSkateboard::StopRolling()
{
	if ( m_pRolling )
	{
		CSoundEnvelopeController::GetController().SoundDestroy( m_pRolling );
		m_pRolling = NULL;
	}
	m_pszRolling = NULL;
}

// Skate 3 grain recording for the surface under the board.
static const char *SkateRollingSound( char chMaterial )
{
	switch ( chMaterial )
	{
	case CHAR_TEX_METAL:
	case CHAR_TEX_VENT:
	case CHAR_TEX_GRATE:
		return "skate/roll_metal_smooth_hard.wav";
	case CHAR_TEX_WOOD:
		return "skate/roll_wood_ramp_hard.wav";
	case CHAR_TEX_DIRT:
	case CHAR_TEX_SAND:
	case CHAR_TEX_FOLIAGE:
		return "skate/roll_concrete_aggregate_soft.wav";
	case CHAR_TEX_TILE:
		return "skate/roll_concrete_smooth_soft.wav";
	case CHAR_TEX_CONCRETE:
		return "skate/roll_concrete_smooth_hard.wav";
	default:
		return "skate/roll_asphalt_smooth_hard.wav";
	}
}

void C_TFSkateboard::UpdateSound()
{
	C_TFPlayer *pOwner = m_hOwner.Get();
	if ( !pOwner || !pOwner->IsAlive() || pOwner->IsDormant() )
	{
		StopRolling();
		return;
	}

	// Ground riding and slides roll; air, grinds, bails and walking don't.
	int nState = pOwner->GetSkateState();
	bool bRolling = nState >= 100 && nState < 200;

	if ( gpGlobals->curtime >= m_flNextSurfaceCheck )
	{
		m_flNextSurfaceCheck = gpGlobals->curtime + 0.2f;
		trace_t tr;
		UTIL_TraceLine( GetAbsOrigin() + Vector( 0, 0, 4 ), GetAbsOrigin() - Vector( 0, 0, 24 ), MASK_SOLID_BRUSHONLY, pOwner, COLLISION_GROUP_NONE, &tr );
		surfacedata_t *pSurface = tr.DidHit() ? physprops->GetSurfaceData( tr.surface.surfaceProps ) : NULL;
		if ( pSurface )
		{
			m_chSurface = pSurface->game.material;
		}
		const char *pszWanted = SkateRollingSound( pSurface ? pSurface->game.material : CHAR_TEX_CONCRETE );
		if ( tr.DidHit() && pszWanted != m_pszRolling )
		{
			StopRolling();
			m_pszRolling = pszWanted;
			enginesound->PrecacheSound( pszWanted, true );
			CPASAttenuationFilter filter( pOwner, ATTN_NORM );
			CSoundEnvelopeController &controller = CSoundEnvelopeController::GetController();
			m_pRolling = controller.SoundCreate( filter, pOwner->entindex(), CHAN_STATIC, pszWanted, ATTN_NORM );
			controller.Play( m_pRolling, 0.0f, 100 );
		}
	}

	if ( m_pRolling )
	{
		Vector vecVelocity;
		pOwner->EstimateAbsVelocity( vecVelocity );
		float flSpeed = vecVelocity.Length();
		float flVolume = bRolling ? clamp( flSpeed / 350.0f, 0.0f, 1.0f ) * cl_skate_roll_volume.GetFloat() : 0.0f;
		float flPitch = clamp( 75.0f + flSpeed * 0.08f, 75.0f, 135.0f );
		CSoundEnvelopeController &controller = CSoundEnvelopeController::GetController();
		controller.SoundChangeVolume( m_pRolling, flVolume, 0.08f );
		controller.SoundChangePitch( m_pRolling, flPitch, 0.08f );
	}
}

bool C_TFSkateboard::ShouldDraw()
{
	if ( C_BaseEntity *pFollow = m_hFollow.Get() )
		return !pFollow->IsDormant();
	C_TFPlayer *pOwner = m_hOwner.Get();
	return pOwner && pOwner->IsAlive() && !pOwner->IsDormant();
}

void C_TFSkateboard::GetRenderBounds( Vector &mins, Vector &maxs )
{
	if ( s_BoardParts.Count() )
	{
		mins = s_vecBoardMins - Vector( 1, 1, 1 );
		maxs = s_vecBoardMaxs + Vector( 1, 1, 1 );
		return;
	}
	mins.Init( -DECK_HALF_LENGTH - 2, -DECK_HALF_WIDTH - 2, -6 );
	maxs.Init( DECK_HALF_LENGTH + 2, DECK_HALF_WIDTH + 2, 4 );
}

// One box in deck space (x forward, y left, z up), shaded by a fixed light.
static void AddBox( CMeshBuilder &builder, const matrix3x4_t &toWorld, const Vector &center, const Vector &half, const Vector &color )
{
	static const Vector s_Light = Vector( 0.4f, 0.3f, 0.86f );
	static const int s_Faces[6][4][3] =
	{
		{ { 1,-1,-1 }, { 1, 1,-1 }, { 1, 1, 1 }, { 1,-1, 1 } },
		{ {-1, 1,-1 }, {-1,-1,-1 }, {-1,-1, 1 }, {-1, 1, 1 } },
		{ {-1, 1,-1 }, {-1, 1, 1 }, { 1, 1, 1 }, { 1, 1,-1 } },
		{ {-1,-1, 1 }, {-1,-1,-1 }, { 1,-1,-1 }, { 1,-1, 1 } },
		{ {-1,-1, 1 }, { 1,-1, 1 }, { 1, 1, 1 }, {-1, 1, 1 } },
		{ {-1, 1,-1 }, { 1, 1,-1 }, { 1,-1,-1 }, {-1,-1,-1 } },
	};
	static const Vector s_Normals[6] =
	{
		Vector( 1, 0, 0 ), Vector( -1, 0, 0 ), Vector( 0, 1, 0 ),
		Vector( 0, -1, 0 ), Vector( 0, 0, 1 ), Vector( 0, 0, -1 ),
	};

	for ( int f = 0; f < 6; ++f )
	{
		Vector vecNormal;
		VectorRotate( s_Normals[f], toWorld, vecNormal );
		float flShade = 0.45f + 0.55f * MAX( 0.0f, DotProduct( vecNormal, s_Light ) );
		unsigned char r = (unsigned char)clamp( color.x * flShade, 0.0f, 255.0f );
		unsigned char g = (unsigned char)clamp( color.y * flShade, 0.0f, 255.0f );
		unsigned char b = (unsigned char)clamp( color.z * flShade, 0.0f, 255.0f );
		for ( int v = 0; v < 4; ++v )
		{
			Vector vecLocal( center.x + half.x * s_Faces[f][v][0],
							 center.y + half.y * s_Faces[f][v][1],
							 center.z + half.z * s_Faces[f][v][2] );
			Vector vecWorld;
			VectorTransform( vecLocal, toWorld, vecWorld );
			builder.Position3fv( vecWorld.Base() );
			builder.Color4ub( r, g, b, 255 );
			builder.TexCoord2f( 0, 0.5f, 0.5f );
			builder.AdvanceVertex();
		}
	}
}

// Unlit materials, so light the board here: the world's light at the board
// (ambient plus direct) scaled by a fixed key light on each vertex normal.
void C_TFSkateboard::DrawSkate3Board( const matrix3x4_t &toWorld )
{
	static const Vector s_Light = Vector( 0.4f, 0.3f, 0.86f );
	Vector vecLight = engine->GetLightForPoint( GetAbsOrigin() + Vector( 0, 0, 8 ), true );
	// Lightmap values are linear and often dim; lift them like the studio renderer's ambient.
	for ( int c = 0; c < 3; ++c )
	{
		vecLight[c] = clamp( sqrtf( MAX( vecLight[c], 0.0f ) ) * 1.1f, 0.12f, 1.0f );
	}

	// An ubercharged rider's board glows like the rider: the player's
	// invulnerability material instead of the board's own, as TF2 forces it
	// onto the player's model and items. Its proxies read the player.
	IMaterial *pOverride = NULL;
	C_TFPlayer *pOwner = m_hOwner.Get();
	if ( pOwner && pOwner->m_Shared.IsInvulnerable() && pOwner->GetInvulnMaterialRef()->IsValid() )
	{
		pOverride = *pOwner->GetInvulnMaterialRef();
		modelrender->SetupLighting( GetAbsOrigin() );
	}

	CMatRenderContextPtr pRenderContext( materials );
	for ( int i = 0; i < s_BoardParts.Count(); ++i )
	{
		SkateBoardPart_t *pPart = s_BoardParts[i];
		if ( !pPart->material.IsValid() || pPart->indices.Count() == 0 )
			continue;
		if ( pOverride )
		{
			pRenderContext->Bind( pOverride, static_cast< IClientRenderable * >( pOwner ) );
		}
		else
		{
			pRenderContext->Bind( pPart->material );
		}
		IMesh *pMesh = pRenderContext->GetDynamicMesh();
		CMeshBuilder builder;
		builder.Begin( pMesh, MATERIAL_TRIANGLES, pPart->verts.Count(), pPart->indices.Count() );
		for ( int v = 0; v < pPart->verts.Count(); ++v )
		{
			const SkateBoardVertex_t &vert = pPart->verts[v];
			Vector vecWorld, vecNormal;
			VectorTransform( vert.pos, toWorld, vecWorld );
			VectorRotate( vert.normal, toWorld, vecNormal );
			float flShade = 0.55f + 0.45f * MAX( 0.0f, DotProduct( vecNormal, s_Light ) );
			builder.Position3fv( vecWorld.Base() );
			if ( pOverride )
			{
				// The lit, bump-mapped override needs a normal and a tangent
				// (along the board, square to the normal).
				Vector vecAlong( toWorld[0][0], toWorld[1][0], toWorld[2][0] );
				Vector vecTangent = vecAlong - vecNormal * DotProduct( vecAlong, vecNormal );
				if ( vecTangent.NormalizeInPlace() < 0.001f )
				{
					vecTangent.Init( toWorld[0][1], toWorld[1][1], toWorld[2][1] );
				}
				float flTangent[4] = { vecTangent.x, vecTangent.y, vecTangent.z, 1.0f };
				builder.Normal3fv( vecNormal.Base() );
				builder.TangentS3fv( vecTangent.Base() );
				builder.UserData( flTangent );
			}
			builder.Color4ub( (unsigned char)( 255 * clamp( vecLight.x * flShade, 0.0f, 1.0f ) ),
							  (unsigned char)( 255 * clamp( vecLight.y * flShade, 0.0f, 1.0f ) ),
							  (unsigned char)( 255 * clamp( vecLight.z * flShade, 0.0f, 1.0f ) ), 255 );
			builder.TexCoord2fv( 0, vert.uv );
			builder.AdvanceVertex();
		}
		for ( int k = 0; k < pPart->indices.Count(); ++k )
		{
			builder.FastIndex( pPart->indices[k] );
		}
		builder.End();
		pMesh->Draw();
	}
}

int C_TFSkateboard::DrawModel( int flags )
{
	if ( !( flags & STUDIO_RENDER ) || !m_Material.IsValid() )
		return 0;

	matrix3x4_t toWorld;
	AngleMatrix( GetAbsAngles(), GetAbsOrigin(), toWorld );
	if ( s_BoardParts.Count() )
	{
		DrawSkate3Board( toWorld );
		return 1;
	}

	CMatRenderContextPtr pRenderContext( materials );
	pRenderContext->Bind( m_Material );
	IMesh *pMesh = pRenderContext->GetDynamicMesh();

	const int nBoxes = 1 + 2 + 4;
	CMeshBuilder builder;
	builder.Begin( pMesh, MATERIAL_QUADS, nBoxes * 6 );

	// Deck: grip on top would need a second box; one dark box reads fine.
	AddBox( builder, toWorld, vec3_origin, Vector( DECK_HALF_LENGTH, DECK_HALF_WIDTH, DECK_HALF_THICKNESS ), Vector( 45, 40, 38 ) );

	for ( int i = -1; i <= 1; i += 2 )
	{
		const float x = i * TRUCK_OFFSET;
		// Truck hanger.
		AddBox( builder, toWorld, Vector( x, 0, -1.4f ), Vector( 0.6f, 3.4f, 0.5f ), Vector( 190, 190, 200 ) );
		// Wheels.
		for ( int j = -1; j <= 1; j += 2 )
		{
			AddBox( builder, toWorld, Vector( x, j * 3.6f, -WHEEL_RADIUS - 0.9f ), Vector( WHEEL_RADIUS, 0.8f, WHEEL_RADIUS ), Vector( 240, 235, 215 ) );
		}
	}

	builder.End();
	pMesh->Draw();
	return 1;
}

//-----------------------------------------------------------------------------
// Skate 3 event sounds.
//
// sound/skate/events.txt maps event names to WAVs (written by
// tools/skate_sound_picker.py):
//   "SkateSounds" { "pop" { "wave" "skate/banks/Skate_Collisions/012.wav" ... } }
// One-shot events pick a random wave; "grind" and "skid" loop their first.
//-----------------------------------------------------------------------------
ConVar cl_skate_sfx_volume( "cl_skate_sfx_volume", "1.0", FCVAR_ARCHIVE, "Volume of Skate 3 pop/land/bail/grind sounds." );
ConVar cl_skate_sfx_debug( "cl_skate_sfx_debug", "0", 0, "Print skate sound events as they fire." );

static CUtlDict< CUtlVector< CUtlString > *, int > s_SkateSounds;
static bool s_bSkateSoundsLoaded = false;

static void LoadSkateSounds()
{
	s_bSkateSoundsLoaded = true;
	s_SkateSounds.PurgeAndDeleteElements();
	KeyValues *pKV = new KeyValues( "SkateSounds" );
	if ( pKV->LoadFromFile( g_pFullFileSystem, "sound/skate/events.txt", "GAME" ) )
	{
		for ( KeyValues *pEvent = pKV->GetFirstTrueSubKey(); pEvent; pEvent = pEvent->GetNextTrueSubKey() )
		{
			CUtlVector< CUtlString > *pWaves = new CUtlVector< CUtlString >;
			for ( KeyValues *pWave = pEvent->GetFirstValue(); pWave; pWave = pWave->GetNextValue() )
			{
				if ( !V_stricmp( pWave->GetName(), "wave" ) && pWave->GetString()[0] )
				{
					// Source finds sounds by lowercase path on Linux.
					char szWave[ MAX_PATH ];
					V_strncpy( szWave, pWave->GetString(), sizeof( szWave ) );
					V_strlower( szWave );
					pWaves->AddToTail( szWave );
					enginesound->PrecacheSound( szWave, true );
				}
			}
			s_SkateSounds.Insert( pEvent->GetName(), pWaves );
		}
	}
	pKV->deleteThis();
	DevMsg( "[skate] %d sound events loaded\n", s_SkateSounds.Count() );
}

CON_COMMAND( skate_reload_sounds, "Reload sound/skate/events.txt." )
{
	LoadSkateSounds();
}

static const char *SkateEventWave( const char *pszEvent, bool bRandom )
{
	if ( !s_bSkateSoundsLoaded )
		LoadSkateSounds();
	int i = s_SkateSounds.Find( pszEvent );
	if ( i == s_SkateSounds.InvalidIndex() || s_SkateSounds[i]->Count() == 0 )
		return NULL;
	CUtlVector< CUtlString > &waves = *s_SkateSounds[i];
	return waves[ bRandom ? RandomInt( 0, waves.Count() - 1 ) : 0 ].String();
}

static void PlaySkateEvent( C_BaseEntity *pSource, const Vector &vecOrigin, const char *pszEvent, float flVolume )
{
	const char *pszWave = SkateEventWave( pszEvent, true );
	if ( cl_skate_sfx_debug.GetBool() )
		Msg( "[skate] %s -> %s (%.2f)\n", pszEvent, pszWave ? pszWave : "<unmapped>", flVolume );
	if ( !pszWave )
		return;
	CPASAttenuationFilter filter( vecOrigin, ATTN_NORM );
	EmitSound_t params;
	params.m_pSoundName = pszWave;
	params.m_flVolume = clamp( flVolume * cl_skate_sfx_volume.GetFloat(), 0.0f, 1.0f );
	params.m_SoundLevel = SNDLVL_80dB;
	params.m_nChannel = CHAN_AUTO;
	params.m_pOrigin = &vecOrigin;
	params.m_nPitch = RandomInt( 95, 105 );
	params.m_nFlags = SND_CHANGE_PITCH;
	C_BaseEntity::EmitSound( filter, pSource->entindex(), params );
}

//-----------------------------------------------------------------------------
// Skate 3's own event table (sound/skate/skate3_events.txt, written by
// the in-game Skate 3 setup from the reverse-engineered Splicer data).
// An event has variants (one is picked); a variant has layers that all play
// together; each layer picks one sample with its own gain, pitch, delay and
// chance. Events missing here fall back to the picker mapping (events.txt).
//-----------------------------------------------------------------------------
struct Skate3Sample_t
{
	CUtlString	wave;
	float		gain, pitch, delay, chance;
};
typedef CUtlVector< Skate3Sample_t > Skate3Layer_t;
typedef CUtlVector< Skate3Layer_t > Skate3Variant_t;
typedef CUtlVector< Skate3Variant_t > Skate3Event_t;

static CUtlDict< Skate3Event_t *, int > s_Skate3Events;
static bool s_bSkate3Loaded = false;

static void LoadSkate3Events()
{
	s_bSkate3Loaded = true;
	s_Skate3Events.PurgeAndDeleteElements();
	KeyValues *pKV = new KeyValues( "Skate3Sounds" );
	if ( pKV->LoadFromFile( g_pFullFileSystem, "sound/skate/skate3_events.txt", "GAME" ) )
	{
		for ( KeyValues *pEvent = pKV->GetFirstTrueSubKey(); pEvent; pEvent = pEvent->GetNextTrueSubKey() )
		{
			Skate3Event_t *pVariants = new Skate3Event_t;
			for ( KeyValues *pVariant = pEvent->GetFirstTrueSubKey(); pVariant; pVariant = pVariant->GetNextTrueSubKey() )
			{
				Skate3Variant_t &variant = pVariants->Element( pVariants->AddToTail() );
				for ( KeyValues *pLayer = pVariant->GetFirstTrueSubKey(); pLayer; pLayer = pLayer->GetNextTrueSubKey() )
				{
					Skate3Layer_t &layer = variant[ variant.AddToTail() ];
					for ( KeyValues *pSample = pLayer->GetFirstTrueSubKey(); pSample; pSample = pSample->GetNextTrueSubKey() )
					{
						Skate3Sample_t &sample = layer[ layer.AddToTail() ];
						char szWave[ MAX_PATH ];
						V_strncpy( szWave, pSample->GetString( "wave" ), sizeof( szWave ) );
						V_strlower( szWave );	// Source finds sounds by lowercase path on Linux
						sample.wave = szWave;
						sample.gain = pSample->GetFloat( "gain", 1.0f );
						sample.pitch = pSample->GetFloat( "pitch", 1.0f );
						sample.delay = pSample->GetFloat( "delay", 0.0f );
						sample.chance = pSample->GetFloat( "chance", 1.0f );
						enginesound->PrecacheSound( sample.wave.String(), true );
					}
				}
			}
			s_Skate3Events.Insert( pEvent->GetName(), pVariants );
		}
	}
	pKV->deleteThis();
	DevMsg( "[skate] %d Skate 3 sound events loaded\n", s_Skate3Events.Count() );
}

//-----------------------------------------------------------------------------
// Map load: parse the event tables and load every wave now. Done lazily on
// the first board, it stalled the frame the player started skating.
//-----------------------------------------------------------------------------
void C_TFSkateboard::LevelInitAssets()
{
	LoadBoardModel();
	C_BaseEntity::PrecacheScriptSound( "Flesh.ImpactHard" );
	C_BaseEntity::PrecacheScriptSound( "Flesh.ImpactSoft" );
	LoadSkateSounds();
	LoadSkate3Events();
	static const char chMaterials[] = { CHAR_TEX_METAL, CHAR_TEX_WOOD, CHAR_TEX_DIRT, CHAR_TEX_TILE, CHAR_TEX_CONCRETE, 0 };
	for ( int i = 0; i < ARRAYSIZE( chMaterials ); ++i )
	{
		enginesound->PrecacheSound( SkateRollingSound( chMaterials[i] ), true );
	}
}

static Skate3Event_t *FindSkate3Event( const char *pszEvent )
{
	if ( !s_bSkate3Loaded )
		LoadSkate3Events();
	int i = s_Skate3Events.Find( pszEvent );
	if ( i == s_Skate3Events.InvalidIndex() || s_Skate3Events[i]->Count() == 0 )
		return NULL;
	return s_Skate3Events[i];
}

// Plays every layer of one random variant. Returns false if the event is unknown.
static bool PlaySkate3Event( C_BaseEntity *pSource, const Vector &vecOrigin, const char *pszEvent, float flVolume )
{
	Skate3Event_t *pEvent = FindSkate3Event( pszEvent );
	if ( !pEvent )
		return false;
	const Skate3Variant_t &variant = pEvent->Element( RandomInt( 0, pEvent->Count() - 1 ) );
	if ( cl_skate_sfx_debug.GetBool() )
		Msg( "[skate] %s (Skate 3 table, %d layers)\n", pszEvent, variant.Count() );
	CPASAttenuationFilter filter( vecOrigin, ATTN_NORM );
	for ( int l = 0; l < variant.Count(); ++l )
	{
		const Skate3Layer_t &layer = variant[l];
		if ( layer.Count() == 0 )
			continue;
		const Skate3Sample_t &sample = layer[ RandomInt( 0, layer.Count() - 1 ) ];
		if ( RandomFloat( 0.0f, 1.0f ) > sample.chance )
			continue;
		EmitSound_t params;
		params.m_pSoundName = sample.wave.String();
		params.m_flVolume = clamp( sample.gain * flVolume * cl_skate_sfx_volume.GetFloat(), 0.0f, 1.0f );
		params.m_SoundLevel = SNDLVL_80dB;
		params.m_nChannel = CHAN_AUTO;
		params.m_pOrigin = &vecOrigin;
		params.m_nPitch = clamp( (int)( 100.0f * sample.pitch ), 30, 250 );
		params.m_nFlags = SND_CHANGE_PITCH;
		if ( sample.delay > 0.0f )
		{
			params.m_flSoundTime = gpGlobals->curtime + sample.delay;
		}
		C_BaseEntity::EmitSound( filter, pSource->entindex(), params );
	}
	return true;
}

// A loop's wave: first layer of a random variant (loops have 'cue' points).
static const char *Skate3LoopWave( const char *pszEvent )
{
	Skate3Event_t *pEvent = FindSkate3Event( pszEvent );
	if ( !pEvent )
		return NULL;
	const Skate3Variant_t &variant = pEvent->Element( RandomInt( 0, pEvent->Count() - 1 ) );
	if ( variant.Count() == 0 || variant[0].Count() == 0 )
		return NULL;
	return variant[0][ RandomInt( 0, variant[0].Count() - 1 ) ].wave.String();
}

CON_COMMAND( skate_reload_skate3_sounds, "Reload sound/skate/skate3_events.txt." )
{
	LoadSkate3Events();
}

// Skate 3 first, then the picker mapping.
static void PlaySkateSound( C_BaseEntity *pSource, const Vector &vecOrigin, const char *pszSkate3, const char *pszFallback, float flVolume )
{
	if ( pszSkate3 && PlaySkate3Event( pSource, vecOrigin, pszSkate3, flVolume ) )
		return;
	if ( pszFallback )
		PlaySkateEvent( pSource, vecOrigin, pszFallback, flVolume );
}

void C_TFSkateboard::UpdateLoop( CSoundPatch *&pPatch, const char *pszEvent, bool bActive, float flVolume, float flPitch )
{
	CSoundEnvelopeController &controller = CSoundEnvelopeController::GetController();
	if ( !bActive || !pszEvent )
	{
		if ( pPatch )
		{
			controller.SoundDestroy( pPatch );
			pPatch = NULL;
		}
		return;
	}
	if ( !pPatch )
	{
		const char *pszWave = Skate3LoopWave( pszEvent );
		if ( !pszWave )
			pszWave = SkateEventWave( pszEvent, false );
		C_TFPlayer *pOwner = m_hOwner.Get();
		if ( !pszWave || !pOwner )
			return;
		CPASAttenuationFilter filter( pOwner, ATTN_NORM );
		pPatch = controller.SoundCreate( filter, pOwner->entindex(), CHAN_STATIC, pszWave, ATTN_NORM );
		controller.Play( pPatch, 0.0f, 100 );
	}
	controller.SoundChangeVolume( pPatch, clamp( flVolume * cl_skate_sfx_volume.GetFloat(), 0.0f, 1.0f ), 0.05f );
	controller.SoundChangePitch( pPatch, flPitch, 0.05f );
}

// Skate 3 grind sounds by surface (its per-surface table at 0x8302D6E8);
// concrete ledges use Skate 3's AEMS scrape system, not yet mapped -> NULL.
static const char *SkateGrindSurface( char chMaterial )
{
	switch ( chMaterial )
	{
	case CHAR_TEX_METAL:	return "metal_round";
	case CHAR_TEX_VENT:		return "metal_sheet";
	case CHAR_TEX_GRATE:	return "metal_coarse";
	case CHAR_TEX_WOOD:		return "wood";
	case CHAR_TEX_PLASTIC:	return "plastic";
	default:				return NULL;
	}
}

static bool SkateGround( int nState )	{ return nState >= 100 && nState < 200; }
static bool SkateAir( int nState )		{ return nState >= 200 && nState < 300; }
static bool SkateGrind( int nState )	{ return nState >= 400 && nState < 500; }
static bool SkateOnFoot( int nState )	{ return nState == 500 || nState == 501 || nState == 502; }

void C_TFSkateboard::UpdateEvents()
{
	C_TFPlayer *pOwner = m_hOwner.Get();
	if ( !pOwner || !pOwner->IsAlive() || pOwner->IsDormant() )
	{
		UpdateLoop( m_pGrind, NULL, false, 0, 0 );
		UpdateLoop( m_pSkid, NULL, false, 0, 0 );
		return;
	}

	int nState = pOwner->GetSkateState();
	Vector vecVelocity;
	pOwner->EstimateAbsVelocity( vecVelocity );
	float flSpeed = vecVelocity.Length();
	const Vector &vecBoard = GetAbsOrigin();

	if ( SkateAir( nState ) )
	{
		m_flAirFallSpeed = MAX( m_flAirFallSpeed, -vecVelocity.z );
	}

	// Bail: the skater's ragdoll hits things. Each sudden change of velocity
	// is a body impact, played with Source's physics flesh sounds (as for
	// ragdolls), harder hits louder; plus one hit as the bail starts.
	{
		float flJolt = ( vecVelocity - m_vecLastOwnerVelocity ).Length();
		bool bStartedBail = nState == 300 && m_nLastState != 300;
		if ( ( nState == 300 || bStartedBail ) && gpGlobals->curtime >= m_flNextBodyImpact )
		{
			float flHit = bStartedBail ? MAX( flJolt, m_vecLastOwnerVelocity.Length() ) : flJolt;
			if ( flHit > 160.0f )
			{
				CPASAttenuationFilter filter( pOwner, "Flesh.ImpactHard" );
				const Vector vecBody = pOwner->WorldSpaceCenter();
				C_BaseEntity::EmitSound( filter, pOwner->entindex(), flHit > 380.0f ? "Flesh.ImpactHard" : "Flesh.ImpactSoft", &vecBody );
				m_flNextBodyImpact = gpGlobals->curtime + 0.12f;
			}
		}
		m_vecLastOwnerVelocity = vecVelocity;
	}

	// Skate 3's surface class: wooden ramps sound hollow (AudioSurfaceMap +8).
	const char *pszClass = ( m_chSurface == CHAR_TEX_WOOD ) ? "wood" : "hard";
	char szEvent[ 64 ];

	if ( nState != m_nLastState )
	{
		int nFrom = m_nLastState;
		if ( SkateGround( nFrom ) && SkateAir( nState ) )
		{
			// Skate 3 picks one of three pop intensities (0x824B9CC8); our
			// stand-in for its pop strength is speed. The rattle layer plays
			// above 4 m/s (157 units/s), as in the original.
			int nTier = flSpeed < 150.0f ? 0 : ( flSpeed < 300.0f ? 1 : 2 );
			V_snprintf( szEvent, sizeof( szEvent ), "pop_%s_%d", pszClass, nTier );
			PlaySkateSound( this, vecBoard, szEvent, "pop", 1.0f );
			if ( flSpeed > 157.0f )
			{
				PlaySkate3Event( this, vecBoard, "takeoff_rattle", 1.0f );
			}
			m_flAirFallSpeed = 0.0f;
			m_flAirStart = gpGlobals->curtime;
		}
		else if ( ( SkateAir( nFrom ) || SkateGrind( nFrom ) ) && SkateGround( nState ) )
		{
			// "Big" landing after 0.75 s of air, as in 0x824BA630.
			bool bBig = SkateAir( nFrom ) && gpGlobals->curtime - m_flAirStart >= 0.75f;
			V_snprintf( szEvent, sizeof( szEvent ), "land_%s%s", pszClass, bBig ? "_big" : "" );
			PlaySkateSound( this, vecBoard, szEvent, bBig ? "land_hard" : "land", 1.0f );
		}
		else if ( nState == 300 )
		{
			PlaySkateEvent( this, vecBoard, "bail", 1.0f );
		}
		else if ( SkateOnFoot( nState ) != SkateOnFoot( nFrom ) )
		{
			PlaySkateEvent( this, vecBoard, "board_step", 0.8f );
		}
		if ( SkateGrind( nState ) && !SkateGrind( nFrom ) )
		{
			const char *pszRail = SkateGrindSurface( m_chSurface );
			V_snprintf( szEvent, sizeof( szEvent ), "grind_start_%s", pszRail ? pszRail : "" );
			PlaySkateSound( this, vecBoard, pszRail ? szEvent : NULL, "grind_start", 1.0f );
		}
		if ( !SkateAir( nState ) )
		{
			m_flAirFallSpeed = 0.0f;
		}
		m_nLastState = nState;
	}

	// A sudden loss of speed while riding: the board hit something.
	float flDeltaTime = MAX( gpGlobals->frametime, 0.001f );
	if ( SkateGround( nState ) && m_flLastSpeed - flSpeed > 900.0f * flDeltaTime && m_flLastSpeed > 120.0f && gpGlobals->curtime >= m_flNextImpact )
	{
		float flHit = m_flLastSpeed - flSpeed;
		V_snprintf( szEvent, sizeof( szEvent ), "impact_%s_%d", pszClass, flHit < 150.0f ? 0 : ( flHit < 300.0f ? 1 : 2 ) );
		PlaySkateSound( this, vecBoard, szEvent, "impact", clamp( flHit / 200.0f, 0.4f, 1.0f ) );
		m_flNextImpact = gpGlobals->curtime + 0.3f;
	}
	m_flLastSpeed = flSpeed;

	// Fast board rotation in the air: a flip or shuvit whoosh.
	const QAngle &angDeck = pOwner->GetSkateDeckAngles();
	float flTurn = fabsf( AngleDiff( angDeck.x, m_angLastDeck.x ) ) + fabsf( AngleDiff( angDeck.y, m_angLastDeck.y ) ) + fabsf( AngleDiff( angDeck.z, m_angLastDeck.z ) );
	m_angLastDeck = angDeck;
	if ( SkateAir( nState ) && flTurn / flDeltaTime > 720.0f && gpGlobals->curtime >= m_flNextFlip )
	{
		PlaySkateEvent( this, vecBoard, "flip", 0.8f );
		m_flNextFlip = gpGlobals->curtime + 0.35f;
	}

	float flPitch = clamp( 85.0f + flSpeed * 0.06f, 85.0f, 125.0f );
	// Grind loop by rail surface; board slides (boardslide, tailslide,
	// darkslide: 400/402/405) use the slide loop, truck grinds the grind loop.
	const char *pszLoop = "grind";
	char szLoop[ 48 ];
	const char *pszRail = SkateGrindSurface( m_chSurface );
	if ( pszRail && SkateGrind( nState ) )
	{
		bool bSlide = nState == 400 || nState == 402 || nState == 405;
		V_snprintf( szLoop, sizeof( szLoop ), "%s_loop_%s", bSlide ? "slide" : "grind", pszRail );
		if ( FindSkate3Event( szLoop ) )
			pszLoop = szLoop;
	}
	if ( V_strcmp( pszLoop, m_szGrindLoop ) )
	{
		UpdateLoop( m_pGrind, NULL, false, 0, 0 );
		V_strncpy( m_szGrindLoop, pszLoop, sizeof( m_szGrindLoop ) );
	}
	UpdateLoop( m_pGrind, m_szGrindLoop, SkateGrind( nState ), clamp( 0.4f + flSpeed / 500.0f, 0.4f, 1.0f ), flPitch );
	UpdateLoop( m_pSkid, "skid", nState == 101, clamp( flSpeed / 300.0f, 0.0f, 1.0f ), 100.0f );
}

//-----------------------------------------------------------------------------
// tf_skateboard_prop: a dead skater's board as a server physics object
// (server/tf/tf_skateboard_prop.cpp). It has no model; a following
// C_TFSkateboard draws Skate 3's board where it is.
//-----------------------------------------------------------------------------
class C_TFSkateboardProp : public C_BaseEntity
{
public:
	DECLARE_CLASS( C_TFSkateboardProp, C_BaseEntity );
	DECLARE_CLIENTCLASS();

	C_TFSkateboardProp() : m_pBoard( NULL ) {}

	virtual void OnDataChanged( DataUpdateType_t type ) OVERRIDE
	{
		BaseClass::OnDataChanged( type );
		if ( type == DATA_UPDATE_CREATED && !m_pBoard )
		{
			m_pBoard = C_TFSkateboard::CreateFollowing( this );
		}
	}

	virtual void UpdateOnRemove() OVERRIDE
	{
		if ( m_pBoard )
		{
			m_pBoard->SetParent( NULL );
			m_pBoard->Destroy();
			m_pBoard = NULL;
		}
		BaseClass::UpdateOnRemove();
	}

	virtual bool ShouldDraw() OVERRIDE { return false; }	// the following board draws
	// Source only interpolates entities with a model; this one has none, but
	// its board child is drawn from its transform.
	virtual bool ShouldInterpolate() OVERRIDE { return true; }

private:
	C_TFSkateboard *m_pBoard;
};

IMPLEMENT_CLIENTCLASS_DT( C_TFSkateboardProp, DT_TFSkateboardProp, CTFSkateboardProp )
END_RECV_TABLE()
