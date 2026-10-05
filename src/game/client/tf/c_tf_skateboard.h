//========= tf2-skate ============//
//
// Purpose: Client-only skateboard drawn at a skating player's networked deck
//          transform. Procedural geometry, so the mod needs no board model.
//
//=============================================================================//
#ifndef C_TF_SKATEBOARD_H
#define C_TF_SKATEBOARD_H
#ifdef _WIN32
#pragma once
#endif

#include "c_baseentity.h"
#include "materialsystem/MaterialSystemUtil.h"

class C_TFPlayer;

class C_TFSkateboard : public C_BaseEntity
{
	DECLARE_CLASS( C_TFSkateboard, C_BaseEntity );
public:
	static C_TFSkateboard *Create( C_TFPlayer *pOwner );
	// A board that just follows another entity (a dead skater's dropped
	// board, tf_skateboard_prop): drawn, but silent.
	static C_TFSkateboard *CreateFollowing( C_BaseEntity *pFollow );
	static void LevelInitAssets();
	void Destroy();

	virtual void ClientThink() OVERRIDE;
	virtual bool ShouldDraw() OVERRIDE;
	virtual int DrawModel( int flags ) OVERRIDE;
	virtual void GetRenderBounds( Vector &mins, Vector &maxs ) OVERRIDE;
	virtual bool IsTransparent() OVERRIDE { return false; }

private:
	void UpdateTransform();
	void DrawSkate3Board( const matrix3x4_t &toWorld );
	void UpdateSound();
	void StopRolling();
	void UpdateEvents();
	void UpdateLoop( class CSoundPatch *&pPatch, const char *pszEvent, bool bActive, float flVolume, float flPitch );

	CHandle< C_TFPlayer >	m_hOwner;
	EHANDLE					m_hFollow;
	CMaterialReference		m_Material;

	// Skate 3 wheel rolling loop (sound/skate/roll_*.wav), chosen by surface.
	class CSoundPatch		*m_pRolling;
	const char				*m_pszRolling;
	float					m_flNextSurfaceCheck;

	// Event sounds driven by the networked skate state (see events.txt).
	int						m_nLastState;
	float					m_flLastSpeed;
	float					m_flAirFallSpeed;
	QAngle					m_angLastDeck;
	float					m_flNextFlip;
	float					m_flNextImpact;
	// Source's own body impact sounds while the skater ragdolls in a bail.
	Vector					m_vecLastOwnerVelocity;
	float					m_flNextBodyImpact;
	class CSoundPatch		*m_pGrind;
	class CSoundPatch		*m_pSkid;
	char					m_chSurface;	// CHAR_TEX_* under the board
	float					m_flAirStart;
	char					m_szGrindLoop[ 48 ];
};

#endif // C_TF_SKATEBOARD_H
