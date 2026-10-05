//========= tf2-skate ============//
//
// Purpose: Client prediction for the local player's skater.
//
//   The server simulates every skater (tf_player_skate.cpp). Its result
//   reaches the player a round trip after they pressed anything, so online
//   the board would answer late. The Skate simulation is deterministic (the
//   same inputs from the same start give the same result, byte for byte:
//   tools/skate_determinism.py), so the client runs its own copy of the
//   local skater from the same usercmds and draws that instead:
//
//   - skater 1 (predicted) runs each new usercmd the moment it's made;
//   - skater 2 (confirmed) follows a little behind, running only commands
//     the server has acknowledged, with the server's own flags.
//
//   The only input the client can't know in advance is a server-forced
//   wipeout (slamming into a player, deep water); the server reports those
//   per command. When one differs from what was predicted, skater 1 is put
//   back to a copy of skater 2 and replays the commands since. Skater 2's
//   position is checked against the server's at every acknowledged command;
//   if they ever differ the prediction has lost sync, and the server's
//   playback is drawn until the next time the player starts skating.
//
//=============================================================================//
#include "cbase.h"
#include "c_tf_skate_predict.h"
#include "c_tf_player.h"
#include "iinput.h"
#include "usercmd.h"
#include "prediction.h"
#include "inetchannelinfo.h"
#include "igamesystem.h"

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

ConVar cl_skate_predict( "cl_skate_predict", "1", FCVAR_ARCHIVE, "Predict your own skater: 0 off (draw the server's), 1 on other people's servers, 2 always (also when hosting; for testing with net_fakelag)." );
ConVar cl_skate_predict_debug( "cl_skate_predict_debug", "0", 0, "Print skate prediction events (rewinds, sync checks)." );

// Skater ids in the client's own simulation.
enum
{
	SKATER_PREDICTED = 1,
	SKATER_CONFIRMED = 2,
};

// Commands stepped per frame while catching up, and confirmed per frame.
static const int CATCHUP_BUDGET = 8;
static const int CONFIRM_BUDGET = 16;
// Server and client positions agree exactly when in sync; this only allows
// for the network's float encoding.
static const float SYNC_TOLERANCE = 0.01f;

CTFSkatePredictor &TFSkatePredictor()
{
	static CTFSkatePredictor s_Predictor;
	return s_Predictor;
}

// A new map: the simulation needs the new world, and the predictor starts over.
class CTFSkatePredictSystem : public CAutoGameSystem
{
public:
	CTFSkatePredictSystem() : CAutoGameSystem( "CTFSkatePredictSystem" ) {}
	virtual void LevelShutdownPreEntity() OVERRIDE
	{
		TFSkatePredictor().LevelShutdown();
		TFSkateSidecar().LevelShutdown();
	}
};
static CTFSkatePredictSystem s_SkatePredictSystem;

void CTFSkatePredictor::LevelShutdown()
{
	Reset( "level change" );
	m_nSerial = -1;
	m_nLatest = 0;
	V_memset( m_Records, 0, sizeof( m_Records ) );
}

CTFSkatePredictor::CTFSkatePredictor()
{
	V_memset( m_Records, 0, sizeof( m_Records ) );
	m_eState = STATE_IDLE;
	m_nSerial = -1;
	m_nStartCommand = 0;
	m_nPredictedTo = 0;
	m_nConfirmedTo = 0;
	m_nLatest = 0;
	m_nFlagCount = 0;
	m_bSpawned = false;
}

static bool PredictionWanted()
{
	if ( engine->IsPlayingDemo() )
		return false;
	switch ( cl_skate_predict.GetInt() )
	{
	case 0:
		return false;
	case 1:
	{
		// Hosting has no round trip to hide.
		INetChannelInfo *pChannel = engine->GetNetChannelInfo();
		return pChannel && !pChannel->IsLoopback();
	}
	default:
		return true;
	}
}

void CTFSkatePredictor::Reset( const char *pszWhy )
{
	if ( m_bSpawned )
	{
		TFSkateSidecar().Despawn( SKATER_PREDICTED );
		TFSkateSidecar().Despawn( SKATER_CONFIRMED );
		m_bSpawned = false;
	}
	if ( pszWhy && cl_skate_predict_debug.GetBool() )
	{
		Msg( "[skate predict] reset: %s\n", pszWhy );
	}
	m_eState = STATE_IDLE;
	m_nStartCommand = 0;
	m_nPredictedTo = 0;
	m_nConfirmedTo = 0;
}

void CTFSkatePredictor::Fail( const char *pszFormat, ... )
{
	char szMessage[ 512 ];
	va_list args;
	va_start( args, pszFormat );
	V_vsnprintf( szMessage, sizeof( szMessage ), pszFormat, args );
	va_end( args );
	Msg( "[skate predict] %s; showing the server's skater until you next start skating\n", szMessage );
	if ( m_bSpawned )
	{
		TFSkateSidecar().Despawn( SKATER_PREDICTED );
		TFSkateSidecar().Despawn( SKATER_CONFIRMED );
		m_bSpawned = false;
	}
	m_eState = STATE_FAILED;
}

CTFSkatePredictor::Record_t *CTFSkatePredictor::Find( int nCommand )
{
	if ( nCommand <= 0 )
		return NULL;
	Record_t &record = m_Records[ nCommand % RECORDS ];
	return record.nCommand == nCommand ? &record : NULL;
}

static float UserCvar( const char *pszName )
{
	// Parsed like the server parses the userinfo copy it receives.
	ConVarRef cvar( pszName );
	const char *pszValue = cvar.IsValid() ? cvar.GetString() : "";
	return *pszValue ? V_atof( pszValue ) : 0.0f;
}

void CTFSkatePredictor::Capture( C_TFPlayer *pPlayer, const CUserCmd *pCmd )
{
	Record_t &record = m_Records[ pCmd->command_number % RECORDS ];
	record.nCommand = pCmd->command_number;
	SkateInputFromCmd( pCmd, record.input );
	record.flMouseGain = UserCvar( "cl_skate_mouse_gain" );
	record.flMouseDecay = UserCvar( "cl_skate_mouse_decay" );
	record.bDeepWater = pPlayer->GetWaterLevel() >= WL_Waist;
	record.bPredicted = false;
	record.nServerFlags = 0;
	m_nLatest = MAX( m_nLatest, pCmd->command_number );
}

// A command prediction never ran the first time (a hitch, or before we
// started capturing): take it from the engine's recent usercmds.
bool CTFSkatePredictor::Fill( int nCommand )
{
	if ( Find( nCommand ) )
		return true;
	CUserCmd *pCmd = ::input->GetUserCmd( nCommand );
	C_TFPlayer *pPlayer = C_TFPlayer::GetLocalTFPlayer();
	if ( !pCmd || !pPlayer )
		return false;
	Capture( pPlayer, pCmd );
	return true;
}

// The predicted skater's flags for a command: the server's when it has told
// us, otherwise the same deep-water rule the server applies.
int CTFSkatePredictor::GuessFlags( int nCommand )
{
	Record_t *pRecord = Find( nCommand );
	if ( !pRecord )
		return 0;
	if ( nCommand <= m_nConfirmedTo )
		return pRecord->nServerFlags;
	Record_t *pPrevious = Find( nCommand - 1 );
	int nPreviousState = pPrevious && pPrevious->bPredicted ? pPrevious->result.nState : 0;
	return ( pRecord->bDeepWater && nPreviousState != SKATE_STATE_WIPEOUT ) ? SKATE_STEP_FORCE_WIPEOUT : 0;
}

bool CTFSkatePredictor::StepPredicted( int nCommand )
{
	if ( !Fill( nCommand ) )
	{
		Fail( "usercmd %d is gone", nCommand );
		return false;
	}
	Record_t *pRecord = Find( nCommand );
	pRecord->input.nFlags = GuessFlags( nCommand );
	char szError[ 256 ];
	if ( !TFSkateSidecar().Step( SKATER_PREDICTED, TICK_INTERVAL, pRecord->input, pRecord->flMouseGain, pRecord->flMouseDecay, pRecord->result, szError, sizeof( szError ) ) )
	{
		Fail( "step failed: %s", szError );
		return false;
	}
	pRecord->bPredicted = true;
	m_nPredictedTo = nCommand;
	return true;
}

void CTFSkatePredictor::ReadServerFlags( C_TFPlayer *pPlayer )
{
	int nCount = pPlayer->m_nSkateFlagCount;
	if ( nCount - m_nFlagCount > SKATE_FLAG_HISTORY )
	{
		Fail( "missed %d server bail events", nCount - m_nFlagCount - SKATE_FLAG_HISTORY );
		return;
	}
	for ( int i = m_nFlagCount; i < nCount; ++i )
	{
		int nSlot = i % SKATE_FLAG_HISTORY;
		Record_t *pRecord = Find( pPlayer->m_nSkateFlagCmd[ nSlot ] );
		if ( pRecord )
		{
			pRecord->nServerFlags = pPlayer->m_nSkateFlagBits[ nSlot ];
		}
	}
	m_nFlagCount = nCount;
}

// Runs the confirmed skater over the commands the server has acknowledged,
// with the server's flags; rewinds the predicted skater where they differ
// from its guess, and checks the result against the server's position.
void CTFSkatePredictor::Confirm( C_TFPlayer *pPlayer, int nBudget )
{
	ReadServerFlags( pPlayer );
	if ( m_eState == STATE_FAILED )
		return;
	int nAck = pPlayer->m_nSkateAckCmd;
	int nRewindFrom = 0;
	while ( m_nConfirmedTo < MIN( nAck, m_nPredictedTo ) && nBudget-- > 0 )
	{
		int nCommand = m_nConfirmedTo + 1;
		Record_t *pRecord = Find( nCommand );
		if ( !pRecord || !pRecord->bPredicted )
		{
			Fail( "usercmd %d is gone", nCommand );
			return;
		}
		SkateInput_t input = pRecord->input;
		input.nFlags = pRecord->nServerFlags;
		SkateStepResult_t confirmed;
		char szError[ 256 ];
		if ( !TFSkateSidecar().Step( SKATER_CONFIRMED, TICK_INTERVAL, input, pRecord->flMouseGain, pRecord->flMouseDecay, confirmed, szError, sizeof( szError ) ) )
		{
			Fail( "step failed: %s", szError );
			return;
		}
		m_nConfirmedTo = nCommand;
		if ( pRecord->input.nFlags != pRecord->nServerFlags )
		{
			// Guessed wrong: this command's answer is the confirmed one, and
			// everything after it must be replayed from here.
			pRecord->input.nFlags = pRecord->nServerFlags;
			pRecord->result = confirmed;
			nRewindFrom = nCommand;
		}
		if ( nCommand == nAck )
		{
			float flError = ( confirmed.vecOrigin - pPlayer->m_vecSkateOrigin ).Length();
			if ( flError > SYNC_TOLERANCE )
			{
				Fail( "lost sync at usercmd %d (%.2f units from the server)", nCommand, flError );
				return;
			}
			if ( cl_skate_predict_debug.GetInt() > 1 )
			{
				Msg( "[skate predict] usercmd %d in sync (state %d)\n", nCommand, confirmed.nState );
			}
		}
	}

	if ( nRewindFrom )
	{
		char szError[ 256 ];
		if ( !TFSkateSidecar().Copy( SKATER_CONFIRMED, SKATER_PREDICTED, szError, sizeof( szError ) ) )
		{
			Fail( "rewind failed: %s", szError );
			return;
		}
		// The predicted skater is now at m_nConfirmedTo; replay the rest.
		int nReplayTo = m_nPredictedTo;
		m_nPredictedTo = m_nConfirmedTo;
		for ( int nCommand = m_nConfirmedTo + 1; nCommand <= nReplayTo; ++nCommand )
		{
			if ( !StepPredicted( nCommand ) )
				return;
		}
		if ( cl_skate_predict_debug.GetBool() )
		{
			Msg( "[skate predict] server bail at usercmd %d: rewound and replayed %d commands\n", nRewindFrom, nReplayTo - m_nConfirmedTo );
		}
	}
}

void CTFSkatePredictor::Update( C_TFPlayer *pPlayer )
{
	bool bSkating = pPlayer->m_Shared.InCond( TF_COND_SKATING ) && pPlayer->IsAlive();
	if ( !bSkating || !PredictionWanted() )
	{
		if ( m_eState != STATE_IDLE )
		{
			Reset( bSkating ? "prediction turned off" : "stopped skating" );
			m_nSerial = -1;
		}
		return;
	}

	CTFSkateSidecar &sim = TFSkateSidecar();
	if ( pPlayer->m_nSkateSpawnSerial != m_nSerial )
	{
		// A new skater on the server: follow it from the start.
		Reset( "new skater" );
		m_nSerial = pPlayer->m_nSkateSpawnSerial;
		m_nFlagCount = pPlayer->m_nSkateFlagCount;
		m_eState = STATE_PREPARING;
	}

	switch ( m_eState )
	{
	case STATE_PREPARING:
	{
		if ( !sim.IsReady() )
		{
			sim.PrepareAsync();
			return;
		}
		if ( (unsigned int)pPlayer->m_nSkateWorldCRC != sim.GetWorldCRC() )
		{
			Fail( "the server's skate world differs from ours (%08x vs %08x)", (unsigned int)pPlayer->m_nSkateWorldCRC, sim.GetWorldCRC() );
			return;
		}
		char szError[ 256 ];
		if ( !sim.Spawn( SKATER_PREDICTED, pPlayer->m_vecSkateSpawnOrigin, pPlayer->m_flSkateSpawnYaw, pPlayer->m_szSkateDifficulty, szError, sizeof( szError ) ) )
		{
			Fail( "spawn failed: %s", szError );
			return;
		}
		m_bSpawned = true;
		m_eState = STATE_SPAWNING;
		return;
	}
	case STATE_SPAWNING:
	{
		char szError[ 256 ];
		int nLoaded = sim.Poll( SKATER_PREDICTED, szError, sizeof( szError ) );
		if ( nLoaded < 0 )
		{
			Fail( "skater didn't load: %s", szError );
			return;
		}
		if ( nLoaded == 0 )
			return;
		// Both start from the freshly spawned skater, as the server's does.
		if ( !sim.Copy( SKATER_PREDICTED, SKATER_CONFIRMED, szError, sizeof( szError ) ) )
		{
			Fail( "copy failed: %s", szError );
			return;
		}
		m_eState = STATE_WAITING;
		// fall through
	}
	case STATE_WAITING:
		if ( pPlayer->m_nSkateStartCmd <= 0 )
			return;
		m_nStartCommand = pPlayer->m_nSkateStartCmd;
		m_nPredictedTo = m_nConfirmedTo = m_nStartCommand - 1;
		m_eState = STATE_CATCHUP;
		if ( cl_skate_predict_debug.GetBool() )
		{
			Msg( "[skate predict] server started at usercmd %d; catching up to %d\n", m_nStartCommand, m_nLatest );
		}
		// fall through
	case STATE_CATCHUP:
	{
		for ( int i = 0; i < CATCHUP_BUDGET && m_nPredictedTo < m_nLatest; ++i )
		{
			if ( !StepPredicted( m_nPredictedTo + 1 ) )
				return;
		}
		Confirm( pPlayer, CONFIRM_BUDGET );
		if ( m_eState == STATE_CATCHUP && m_nPredictedTo >= m_nLatest )
		{
			m_eState = STATE_ACTIVE;
			if ( cl_skate_predict_debug.GetBool() )
			{
				Msg( "[skate predict] predicting from usercmd %d\n", m_nPredictedTo );
			}
		}
		return;
	}
	case STATE_ACTIVE:
		Confirm( pPlayer, CONFIRM_BUDGET );
		return;
	default:
		return;
	}
}

bool CTFSkatePredictor::Move( C_TFPlayer *pPlayer, const CUserCmd *pCmd, bool bFirstTime, SkateStepResult_t &result )
{
	if ( m_eState != STATE_ACTIVE || !pCmd )
		return false;
	int nCommand = pCmd->command_number;
	if ( bFirstTime )
	{
		// New commands (any a hitch skipped first).
		while ( m_nPredictedTo < nCommand )
		{
			if ( !StepPredicted( m_nPredictedTo + 1 ) )
				return false;
		}
	}
	Record_t *pRecord = Find( nCommand );
	if ( !pRecord || !pRecord->bPredicted || nCommand < m_nStartCommand )
		return false;
	result = pRecord->result;
	return true;
}

static void LerpAngles( const QAngle &a, const QAngle &b, float t, QAngle &out )
{
	for ( int i = 0; i < 3; ++i )
	{
		out[i] = a[i] + AngleDiff( b[i], a[i] ) * t;
	}
}

bool CTFSkatePredictor::GetRenderState( SkateStepResult_t &out )
{
	if ( m_eState != STATE_ACTIVE )
		return false;
	Record_t *pTo = Find( m_nPredictedTo );
	Record_t *pFrom = Find( m_nPredictedTo - 1 );
	if ( !pTo || !pTo->bPredicted )
		return false;
	out = pTo->result;
	if ( !pFrom || !pFrom->bPredicted || m_nPredictedTo - 1 < m_nStartCommand )
		return true;
	// Predicted entities are drawn interpolation_amount of the way from the
	// previous predicted tick to the latest one.
	float t = clamp( gpGlobals->interpolation_amount, 0.0f, 1.0f );
	const SkateStepResult_t &a = pFrom->result;
	const SkateStepResult_t &b = pTo->result;
	out.vecOrigin = Lerp( t, a.vecOrigin, b.vecOrigin );
	LerpAngles( a.angBody, b.angBody, t, out.angBody );
	out.vecVelocity = Lerp( t, a.vecVelocity, b.vecVelocity );
	out.vecDeckOrigin = Lerp( t, a.vecDeckOrigin, b.vecDeckOrigin );
	LerpAngles( a.angDeck, b.angDeck, t, out.angDeck );
	out.vecCameraOrigin = Lerp( t, a.vecCameraOrigin, b.vecCameraOrigin );
	LerpAngles( a.angCamera, b.angCamera, t, out.angCamera );
	for ( int j = 0; j < SKATE_JOINT_COUNT; ++j )
	{
		out.vecJoints[j] = Lerp( t, a.vecJoints[j], b.vecJoints[j] );
	}
	return true;
}
