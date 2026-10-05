//========= tf2-skate ============//
//
// Purpose: Client prediction for the local player's skater. See
//          c_tf_skate_predict.cpp.
//
//=============================================================================//
#ifndef C_TF_SKATE_PREDICT_H
#define C_TF_SKATE_PREDICT_H
#ifdef _WIN32
#pragma once
#endif

#include "tf_skate_sidecar.h"

class C_TFPlayer;
class CUserCmd;

class CTFSkatePredictor
{
public:
	CTFSkatePredictor();

	// Every usercmd, the first time prediction runs it (skating or not): its
	// skate input, kept so the skater can replay from the server's start.
	void Capture( C_TFPlayer *pPlayer, const CUserCmd *pCmd );
	// In prediction, while skating: the predicted skater's result for this
	// command. False while not predicting (the server's playback is drawn).
	bool Move( C_TFPlayer *pPlayer, const CUserCmd *pCmd, bool bFirstTime, SkateStepResult_t &result );
	// Once a frame for the local player: start, catch up, and check the
	// predicted skater against the server.
	void Update( C_TFPlayer *pPlayer );
	// The predicted skater drawn this frame: between the last two predicted
	// commands, like Source's own predicted entities.
	bool GetRenderState( SkateStepResult_t &out );
	bool IsActive() const { return m_eState == STATE_ACTIVE; }
	void Reset( const char *pszWhy = NULL );
	void LevelShutdown();

private:
	enum EState
	{
		STATE_IDLE,			// not skating, or prediction off
		STATE_PREPARING,	// waiting for the simulation and world
		STATE_SPAWNING,		// our skater is loading
		STATE_WAITING,		// for the server's first step
		STATE_CATCHUP,		// replaying commands since the server's first step
		STATE_ACTIVE,
		STATE_FAILED,		// lost sync: server playback until the next spawn
	};
	enum { RECORDS = 512 };
	struct Record_t
	{
		int					nCommand;		// 0 = empty
		SkateInput_t		input;			// nFlags: what the predicted skater was given
		float				flMouseGain;
		float				flMouseDecay;
		bool				bDeepWater;		// at capture, for guessing the water bail
		bool				bPredicted;		// result is valid
		int					nServerFlags;	// what the server used (from its flag history)
		SkateStepResult_t	result;
	};

	Record_t *Find( int nCommand );
	bool Fill( int nCommand );	// from the engine's usercmd history if Capture missed it
	bool StepPredicted( int nCommand );
	void Confirm( C_TFPlayer *pPlayer, int nBudget );
	void ReadServerFlags( C_TFPlayer *pPlayer );
	void Fail( const char *pszFormat, ... );
	int GuessFlags( int nCommand );

	EState		m_eState;
	int			m_nSerial;			// server's spawn count we're following
	int			m_nStartCommand;	// the server's first step
	int			m_nPredictedTo;		// last command the predicted skater ran
	int			m_nConfirmedTo;		// last command the confirmed skater ran
	int			m_nLatest;			// last captured command
	int			m_nFlagCount;		// server flag history read so far
	bool		m_bSpawned;
	Record_t	m_Records[ RECORDS ];
};

CTFSkatePredictor &TFSkatePredictor();

#endif // C_TF_SKATE_PREDICT_H
