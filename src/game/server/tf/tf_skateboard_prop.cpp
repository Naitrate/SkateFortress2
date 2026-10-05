//========= tf2-skate ============//
//
// Purpose: A dead skater's board, dropped as a physics object (like a dropped
//          weapon). A wooden box the size of the board for VPhysics; clients
//          draw Skate 3's board on it (C_TFSkateboardProp). Moving fast enough
//          into an enemy, it hurts them, or kills them, on its owner's behalf.
//
//=============================================================================//
#include "cbase.h"
#include "tf_player.h"
#include "physics_shared.h"
#include "vphysics_interface.h"
#include "vcollide_parse.h"

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

ConVar skate_board_drop( "skate_board_drop", "1", FCVAR_GAMEDLL, "Drop the board as a physics object when a skater dies." );
ConVar skate_board_lifetime( "skate_board_lifetime", "20", FCVAR_GAMEDLL, "Seconds a dropped board stays before it's removed." );
ConVar skate_board_hurt_speed( "skate_board_hurt_speed", "350", FCVAR_GAMEDLL, "A dropped board moving faster than this (units/s) hurts enemies it hits." );
ConVar skate_board_kill_speed( "skate_board_kill_speed", "700", FCVAR_GAMEDLL, "A dropped board moving faster than this (units/s) kills enemies it hits." );

// The board's extent around the deck centre, matching the drawn mesh.
static const Vector s_vecBoardMins( -17.8f, -4.5f, -4.1f );
static const Vector s_vecBoardMaxs( 17.8f, 4.5f, 0.9f );

class CTFSkateboardProp : public CBaseEntity
{
public:
	DECLARE_CLASS( CTFSkateboardProp, CBaseEntity );
	DECLARE_SERVERCLASS();
	DECLARE_DATADESC();

	static CTFSkateboardProp *Create( CTFPlayer *pOwner, const Vector &vecOrigin, const QAngle &angAngles, const Vector &vecVelocity, const AngularImpulse &angSpin );

	virtual void Spawn() OVERRIDE;
	virtual int UpdateTransmitState() OVERRIDE { return SetTransmitState( FL_EDICT_PVSCHECK ); }
	virtual void VPhysicsCollision( int index, gamevcollisionevent_t *pEvent ) OVERRIDE;
	void RemoveThink();

	CHandle< CTFPlayer >	m_hOwner;
	float					m_flNextHit;
};

LINK_ENTITY_TO_CLASS( tf_skateboard_prop, CTFSkateboardProp );

IMPLEMENT_SERVERCLASS_ST( CTFSkateboardProp, DT_TFSkateboardProp )
END_SEND_TABLE()

BEGIN_DATADESC( CTFSkateboardProp )
	DEFINE_THINKFUNC( RemoveThink ),
END_DATADESC()

CTFSkateboardProp *CTFSkateboardProp::Create( CTFPlayer *pOwner, const Vector &vecOrigin, const QAngle &angAngles, const Vector &vecVelocity, const AngularImpulse &angSpin )
{
	CTFSkateboardProp *pBoard = static_cast< CTFSkateboardProp * >( CreateEntityByName( "tf_skateboard_prop" ) );
	if ( !pBoard )
		return NULL;
	pBoard->m_hOwner = pOwner;
	pBoard->SetAbsOrigin( vecOrigin );
	pBoard->SetAbsAngles( angAngles );
	DispatchSpawn( pBoard );

	// A wooden box, so impacts sound and slide like wood.
	solid_t solid;
	PhysGetDefaultAABBSolid( solid );
	Vector vecSize = s_vecBoardMaxs - s_vecBoardMins;
	solid.params.volume = vecSize.x * vecSize.y * vecSize.z;
	solid.params.mass = 3.0f;
	V_strncpy( solid.surfaceprop, "wood_plank", sizeof( solid.surfaceprop ) );
	CPhysCollide *pCollide = PhysCreateBbox( s_vecBoardMins, s_vecBoardMaxs );
	IPhysicsObject *pPhysics = pCollide ? PhysModelCreateCustom( pBoard, pCollide, vecOrigin, angAngles, "tf_skateboard_prop", false, &solid ) : NULL;
	if ( !pPhysics )
	{
		UTIL_Remove( pBoard );
		return NULL;
	}
	pBoard->VPhysicsSetObject( pPhysics );
	pBoard->SetMoveType( MOVETYPE_VPHYSICS );
	Vector vecLinear = vecVelocity;
	AngularImpulse angSpinCopy = angSpin;
	pPhysics->SetVelocity( &vecLinear, &angSpinCopy );
	pPhysics->Wake();
	return pBoard;
}

void CTFSkateboardProp::Spawn()
{
	BaseClass::Spawn();
	SetSolid( SOLID_BBOX );
	AddSolidFlags( FSOLID_NOT_STANDABLE );
	SetCollisionGroup( COLLISION_GROUP_NONE );
	SetSize( s_vecBoardMins, s_vecBoardMaxs );
	m_flNextHit = 0.0f;
	SetThink( &CTFSkateboardProp::RemoveThink );
	SetNextThink( gpGlobals->curtime + skate_board_lifetime.GetFloat() );
}

void CTFSkateboardProp::RemoveThink()
{
	UTIL_Remove( this );
}

void CTFSkateboardProp::VPhysicsCollision( int index, gamevcollisionevent_t *pEvent )
{
	BaseClass::VPhysicsCollision( index, pEvent );

	CTFPlayer *pVictim = ToTFPlayer( pEvent->pEntities[ !index ] );
	float flSpeed = pEvent->preVelocity[ index ].Length();
	if ( !pVictim || !pVictim->IsAlive() || gpGlobals->curtime < m_flNextHit || flSpeed < skate_board_hurt_speed.GetFloat() )
		return;
	CTFPlayer *pOwner = m_hOwner.Get();
	if ( pOwner && pOwner->GetTeamNumber() == pVictim->GetTeamNumber() && pOwner != pVictim )
		return;	// teammates only get bumped

	float flKill = skate_board_kill_speed.GetFloat();
	float flDamage = flSpeed >= flKill ? pVictim->GetMaxHealth() * 4.0f
									   : RemapValClamped( flSpeed, skate_board_hurt_speed.GetFloat(), flKill, 15.0f, 90.0f );
	CBaseEntity *pAttacker = pOwner ? static_cast< CBaseEntity * >( pOwner ) : this;
	CTakeDamageInfo info( this, pAttacker, flDamage, DMG_CLUB );
	Vector vecForce = pEvent->preVelocity[ index ];
	info.SetDamageForce( vecForce * 40.0f );
	info.SetDamagePosition( GetAbsOrigin() );
	pVictim->TakeDamage( info );
	m_flNextHit = gpGlobals->curtime + 0.25f;
}

// Called by CTFPlayer::StopSkating when the skater died.
void TFSkateDropBoard( CTFPlayer *pOwner, const Vector &vecOrigin, const QAngle &angAngles, const Vector &vecVelocity )
{
	if ( !skate_board_drop.GetBool() )
		return;
	AngularImpulse angSpin( RandomFloat( -360, 360 ), RandomFloat( -360, 360 ), RandomFloat( -540, 540 ) );
	CTFSkateboardProp::Create( pOwner, vecOrigin, angAngles, vecVelocity, angSpin );
}
