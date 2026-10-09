//========= tf2-skate ============//
//
// Purpose: The Skate 3 simulation, loaded from libskate3.so beside this
//          client.so / server.so (skate-engine/crates/skate-game/src/sidecar).
//          The server steps one skater per skating player from its usercmds;
//          the client runs its own copy of the local player's skater to
//          predict it (c_tf_skate_predict.cpp). Both build the same world
//          from the map, so the same inputs give the same skating.
//
//=============================================================================//
#ifndef TF_SKATE_SIDECAR_H
#define TF_SKATE_SIDECAR_H
#ifdef _WIN32
#pragma once
#endif

#include "tf_skate_shared.h"
#include "tier0/threadtools.h"

class CUtlBuffer;
class CUserCmd;

struct SkateStepResult_t
{
	int		nState;		// SKATE_STATE_LOADING, or native PhysicalStateId (100 ground, 200 air, 400+ grind, 500+ off board, ...)
	int		nTicks;		// 60 Hz native ticks this step ran
	Vector	vecOrigin;
	QAngle	angBody;
	Vector	vecVelocity;
	Vector	vecDeckOrigin;
	QAngle	angDeck;
	Vector	vecCameraOrigin;
	QAngle	angCamera;
	float	flCameraFov;
	Vector	vecJoints[ SKATE_JOINT_COUNT ];	// root frame, see tf_skate_shared.h

	// Skate's own trick scoring.
	int		nTrickSeq;		// increments per recognized trick
	char	szTrick[ 64 ];	// display name ("Kickflip")
	float	flTrickScore;
	float	flLineScore;	// current line (combo), lost on a bail
	float	flMultiplier;
	float	flTotalScore;	// banked lines
	int		nScoreFlags;	// SKATE_SCORE_*

	// Hall of Meat (the engine judges each bail by its ragdoll's impacts).
	int		nBail;			// increments when a bail starts
	int		nBrokenBones;	// this bail's broken bones, bit per SKATE_BONE_*
	float	flBailScore;	// this bail's score
	int		nBreaks;		// increments per broken bone
	int		nLastBreak;		// the latest broken bone
};

struct SkateInput_t
{
	int		nButtons;	// IN_* bits
	float	flForward;	// -1..1
	float	flSide;		// -1..1, right positive
	float	flMouseX;	// mouse counts since the last usercmd
	float	flMouseY;
	int		nFlags;		// SKATE_STEP_*
	Vector	vecImpulse;	// velocity added before this step (units/s): explosions, knockback
};

// The skate input of one usercmd (flags and impulse left 0). Server and client both use
// this, so the client's predicted skater gets exactly the server's input.
void SkateInputFromCmd( const CUserCmd *pCmd, SkateInput_t &input );

#define SKATE_STATE_LOADING			( -1 )	// Step result while the skater is still being built
#define SKATE_STATE_WIPEOUT			300		// ragdoll bail

// Riding, grinding, standing or planted: Source's ground entity is the world.
inline bool SkateStateIsGrounded( int nState )
{
	return ( nState >= 100 && nState < 200 )	// riding on the ground
		|| ( nState >= 400 && nState < 500 )	// grinds
		|| nState == 500 || nState == 502		// on foot
		|| ( nState >= 600 && nState < 700 );	// plants
}

#define SKATE_STEP_FORCE_WIPEOUT	( 1 << 0 )	// bail now (ragdoll), e.g. deep water
#define SKATE_STEP_PIPELINED		( 1 << 1 )	// get last step's result; this one runs in parallel (bots)

class CTFSkateSidecar
{
public:
	CTFSkateSidecar();

	// Loads the simulation and sends the map ahead of the first spawn, so
	// pressing the skate key doesn't pay for it. Silent if Skate 3 data
	// isn't set up yet.
	void Prepare();
	// The same in the background (the client's predictor): the map is read
	// on this thread, then the slow loading happens on a worker thread.
	// IsReady() says when it's done; GetWorldCRC() identifies the world.
	void PrepareAsync();
	bool IsReady();
	bool IsPreparing() const { return m_bPreparing; }
	unsigned int GetWorldCRC() const { return m_nWorldCRC; }

	// Starts building skater `id`; Step reports SKATE_STATE_LOADING until it
	// exists (Poll says without stepping). Fails with a reason.
	// vecVelocity: the player's velocity, so a skater started mid-air (a
	// rocket jump) keeps flying.
	bool Spawn( int id, const Vector &vecOrigin, float flYaw, const char *pszDifficulty, const Vector &vecVelocity, char *pszError, int nErrorSize );
	void Despawn( int id );
	bool Step( int id, float flDeltaTime, const SkateInput_t &input, float flMouseGain, float flMouseDecay, SkateStepResult_t &result, char *pszError, int nErrorSize );
	// 1 loaded, 0 still loading, -1 failed.
	int Poll( int id, char *pszError, int nErrorSize );
	// Skater `dst` becomes an exact copy of `src` (created if missing).
	bool Copy( int src, int dst, char *pszError, int nErrorSize );

	// Map change: the simulation must receive the new world before the next spawn.
	void LevelShutdown();

private:
	bool EnsureConnected( char *pszError, int nErrorSize );
	bool EnsureWorld( char *pszError, int nErrorSize );
	bool Request( unsigned char nType, const CUtlBuffer &payload, CUtlBuffer &reply, char *pszError, int nErrorSize );
	void Disconnect();
	static uintp PrepareThread( void *pParam );

	typedef int ( *RequestFn )( void *, unsigned char, const unsigned char *, size_t, unsigned char **, size_t * );
	typedef void ( *FreeFn )( unsigned char *, size_t );
	typedef void ( *CloseFn )( void * );

	void		*m_pLibrary;	// dlopen handle of libskate3.so
	void		*m_pHandle;		// skate3_open's simulation (shared data + skaters)
	RequestFn	m_pfnRequest;
	FreeFn		m_pfnFree;
	CloseFn		m_pfnClose;
	char		m_szWorld[ MAX_PATH ];
	char		m_szAssets[ MAX_PATH ];	// data folder the simulation loaded
	unsigned int m_nWorldCRC;			// of the WORLD request this map sent
	double		m_flNextPrepare;		// Prepare() backs off after a failure

	// PrepareAsync's worker: the map name and payload it's loading.
	volatile bool	m_bPreparing;
	ThreadHandle_t	m_hPrepareThread;
	CUtlBuffer		*m_pPendingWorld;
	char			m_szPendingWorld[ MAX_PATH ];
	char			m_szPrepareError[ 512 ];
};

CTFSkateSidecar &TFSkateSidecar();

#endif // TF_SKATE_SIDECAR_H
