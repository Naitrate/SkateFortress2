//========= tf2-skate ============//
//
// Purpose: CTFPlayer side of skating. The "skate_toggle" client command spawns
//          a Skate 3 skater in the sidecar where the player stands; every
//          usercmd then steps it and copies its pose into the player.
//
//=============================================================================//
#include "cbase.h"
#include "tf_player.h"
#include "tf_skate_sidecar.h"
#include "igamemovement.h"
#include "world.h"
#include "in_buttons.h"
#include "collisionutils.h"
#include "tf_skate_pose.h"
#include "tf_bot_skate.h"

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

ConVar skate_difficulty( "skate_difficulty", "normal", FCVAR_GAMEDLL | FCVAR_NOTIFY | FCVAR_ARCHIVE, "Skate 3 physics mode for skaters spawned on this server: easy, normal, hardcore or motorized." );
ConVar skate_hall_of_meat( "skate_hall_of_meat", "1", FCVAR_GAMEDLL | FCVAR_NOTIFY | FCVAR_ARCHIVE, "Hall of Meat: bones a skater breaks in a bail cost health (skate_meat_*), with a crack for each." );
ConVar skate_meat_bone_damage( "skate_meat_bone_damage", "6", FCVAR_GAMEDLL | FCVAR_NOTIFY, "Hall of Meat: damage per broken bone." );
ConVar skate_meat_neck_damage( "skate_meat_neck_damage", "40", FCVAR_GAMEDLL | FCVAR_NOTIFY, "Hall of Meat: damage for breaking the neck or skull (instead of skate_meat_bone_damage)." );
ConVar skate_shove( "skate_shove", "1", FCVAR_GAMEDLL | FCVAR_NOTIFY, "Skaters can shove players in front of them (the Shove skate control)." );
ConVar skate_shove_range( "skate_shove_range", "80", FCVAR_GAMEDLL | FCVAR_NOTIFY, "How far a shove reaches (units, centre to centre)." );
ConVar skate_shove_force( "skate_shove_force", "450", FCVAR_GAMEDLL | FCVAR_NOTIFY, "A shove's push (units/s), plus half the shover's speed toward the target." );
ConVar skate_shove_damage( "skate_shove_damage", "10", FCVAR_GAMEDLL | FCVAR_NOTIFY, "Damage a shove does to enemies." );
ConVar skate_shove_bail_speed( "skate_shove_bail_speed", "550", FCVAR_GAMEDLL | FCVAR_NOTIFY, "A shoved skater bails when the push is at least this strong (units/s)." );
ConVar skate_shove_cooldown( "skate_shove_cooldown", "0.6", FCVAR_GAMEDLL | FCVAR_NOTIFY, "Seconds between a skater's shoves." );
ConVar skate_debug( "skate_debug", "0", FCVAR_GAMEDLL, "Print skater state changes." );
ConVar skate_collide_players( "skate_collide_players", "1", FCVAR_GAMEDLL, "Skaters knock back and hurt enemies they run into, and kill players they land on." );
ConVar skate_collide_teammates( "skate_collide_teammates", "0", FCVAR_GAMEDLL, "Skaters also bump teammates (knockback only, no damage)." );
ConVar skate_ram_speed( "skate_ram_speed", "250", FCVAR_GAMEDLL, "Closing speed (units/s) at which a skater knocks a player back." );
ConVar skate_ram_bail_speed( "skate_ram_bail_speed", "650", FCVAR_GAMEDLL, "Closing speed (units/s) at which the skater wipes out from the collision." );
ConVar skate_stomp_speed( "skate_stomp_speed", "300", FCVAR_GAMEDLL, "Falling speed (units/s) at which landing on a player's head kills them." );
ConVar skate_hitboxes( "skate_hitboxes", "1", FCVAR_GAMEDLL, "Pose skating players' hitboxes like the drawn skater (0: TF2's standing hitboxes)." );
ConVar skate_bot_pipelined( "skate_bot_pipelined", "1", FCVAR_GAMEDLL, "Step skating bots in parallel (one usercmd of latency) instead of one after another." );
ConVar skate_bot_drive( "skate_bot_drive", "1", FCVAR_GAMEDLL, "Skating bots follow nav mesh routes and do tricks (tf_bot_skate.cpp). 0: their TF AI movement goes straight to the board." );

//-----------------------------------------------------------------------------
// Multiplayer testing on one machine: "skate_bots 1" puts every bot on a
// board, so remote-player interpolation, animation and sounds can be seen.
//-----------------------------------------------------------------------------
CON_COMMAND_F( skate_bots, "skate_bots <0|1>: start or stop skating on every bot.", FCVAR_GAMEDLL )
{
	// Server console, or the listen-server host.
	CBasePlayer *pCaller = UTIL_GetCommandClient();
	if ( pCaller && !( engine->IsDedicatedServer() == false && pCaller == UTIL_GetListenServerHost() ) && !sv_cheats->GetBool() )
	{
		ClientPrint( pCaller, HUD_PRINTCONSOLE, "skate_bots: only the server host (or sv_cheats 1) can do that\n" );
		return;
	}
	bool bOn = args.ArgC() < 2 || atoi( args[1] ) != 0;
	int nChanged = 0;
	for ( int i = 1; i <= gpGlobals->maxClients; ++i )
	{
		CTFPlayer *pPlayer = ToTFPlayer( UTIL_PlayerByIndex( i ) );
		if ( !pPlayer || !pPlayer->IsBot() || pPlayer->m_Shared.InCond( TF_COND_SKATING ) == bOn )
			continue;
		if ( !bOn )
		{
			pPlayer->m_Shared.RemoveCond( TF_COND_SKATING );
			++nChanged;
			continue;
		}
		char szError[ 256 ];
		if ( pPlayer->StartSkating( szError, sizeof( szError ) ) )
			++nChanged;
		else
			Msg( "[skate] %s: %s\n", pPlayer->GetPlayerName(), szError );
	}
	Msg( "[skate] %d bots %s skating\n", nChanged, bOn ? "started" : "stopped" );
}

bool CTFPlayer::StartSkating( char *pszError, int nErrorSize )
{
	if ( m_Shared.InCond( TF_COND_SKATING ) )
	{
		V_snprintf( pszError, nErrorSize, "Already skating" );
		return false;
	}
	if ( !IsAlive() || m_Shared.InCond( TF_COND_TAUNTING ) || m_Shared.InCond( TF_COND_HALLOWEEN_KART ) || IsObserver() )
	{
		V_snprintf( pszError, nErrorSize, "Can't start skating right now" );
		return false;
	}
	Vector vecOrigin = GetAbsOrigin();
	float flYaw = EyeAngles()[ YAW ];
	// The skater carries on at the player's velocity: start skating mid rocket
	// jump and land on the board.
	Vector vecVelocity = GetAbsVelocity();
	if ( !TFSkateSidecar().Spawn( entindex(), vecOrigin, flYaw, skate_difficulty.GetString(), vecVelocity, pszError, nErrorSize ) )
		return false;
	m_vecSkateSpawnVelocity = vecVelocity;
	m_vecSkateImpulse.Init();
	// The owner's client spawns the same skater to predict it.
	m_nSkateSpawnSerial = m_nSkateSpawnSerial + 1;
	m_vecSkateSpawnOrigin = vecOrigin;
	m_flSkateSpawnYaw = flYaw;
	V_strncpy( m_szSkateDifficulty.GetForModify(), skate_difficulty.GetString(), 16 );
	m_nSkateWorldCRC = (int)TFSkateSidecar().GetWorldCRC();
	m_nSkateStartCmd = 0;
	m_nSkateAckCmd = 0;
	m_nSkateBreaksSeen = 0;
	m_flSkateShoveTime = -100.0f;
	m_flSkateNextShove = 0.0f;
	m_nSkateBail = 0;
	m_nSkateBrokenBones = 0;
	m_nSkateBailScore = 0;

	m_nSkateState = 0;
	m_angSkateBody = QAngle( 0, EyeAngles()[ YAW ], 0 );
	m_vecSkateDeckOffset = vec3_origin;
	m_angSkateDeck = m_angSkateBody;
	m_vecSkateCameraOffset = vec3_origin;
	m_angSkateCamera = EyeAngles();
	// The Skate camera sits behind the player, so draw their world model
	// (and hide the viewmodel) without relying on the third-person camera.
	SetForceLocalDraw( true );
	m_vecSkateOrigin = GetAbsOrigin();
	m_flSkateTime = gpGlobals->curtime;
	m_flSkateFallSpeed = 0.0f;
	m_vecSkateVelocity = GetAbsVelocity();
	m_bSkateBailNext = false;
	for ( int i = 0; i <= MAX_PLAYERS; ++i )
	{
		m_flSkateLastBump[i] = 0.0f;
	}
	m_Shared.AddCond( TF_COND_SKATING );
	if ( IsBot() )
	{
		TFBotSkateReset( this );
	}
	ClientPrint( this, HUD_PRINTCENTER,
		"SKATING  -  W/S lean, A/D steer, SHIFT push, MOUSE2 brake\n"
		"Mouse back then forward: ollie.  Flick other ways: flip tricks\n"
		"Q/E grab, MOUSE1 step off board, K stop skating" );
	return true;
}

// Called from OnRemoveSkating, so every way out of the condition (toggle,
// death, round reset) releases the sidecar skater.
extern void TFSkateDropBoard( CTFPlayer *pOwner, const Vector &vecOrigin, const QAngle &angAngles, const Vector &vecVelocity );

void CTFPlayer::StopSkating()
{
	if ( !IsAlive() )
	{
		// Killed on the board: it flies on as a physics object.
		TFSkateDropBoard( this, GetAbsOrigin() + m_vecSkateDeckOffset, m_angSkateDeck, m_vecSkateVelocity );
	}
	TFSkateSidecar().Despawn( entindex() );
	SetForceLocalDraw( false );
	if ( IsAlive() )
	{
		// Face where the skater was heading; velocity is already the skater's.
		SnapEyeAngles( QAngle( 0, m_angSkateBody.Get()[ YAW ], 0 ) );
	}
	m_nSkateState = 0;
	m_vecSkateCameraOffset = vec3_origin;
}

void CTFPlayer::SkateMove( float flDeltaTime, const SkateInput_t &playerInput, CMoveData *pMove )
{
	SkateInput_t input = playerInput;
	// Knockback that arrived since the last step (ApplyAbsVelocityImpulse).
	input.vecImpulse = m_vecSkateImpulse;
	// Bots step in parallel in the sidecar, one usercmd behind. Players keep
	// synchronous steps so their own controls have no added latency.
	if ( IsBot() && skate_bot_pipelined.GetBool() )
	{
		input.nFlags |= SKATE_STEP_PIPELINED;
	}
	if ( IsBot() && skate_bot_drive.GetBool() )
	{
		TFBotSkateInput( this, input );
	}

	// Each player's own flick-stick feel (client cvars sent as userinfo; bots
	// and unset values send 0, which keeps the defaults).
	const char *pszGain = IsBot() ? NULL : engine->GetClientConVarValue( entindex(), "cl_skate_mouse_gain" );
	const char *pszDecay = IsBot() ? NULL : engine->GetClientConVarValue( entindex(), "cl_skate_mouse_decay" );
	float flGain = pszGain && *pszGain ? V_atof( pszGain ) : 0.0f;
	float flDecay = pszDecay && *pszDecay ? V_atof( pszDecay ) : 0.0f;

	SkateStepResult_t result;
	char szError[ 512 ];
	if ( !TFSkateSidecar().Step( entindex(), flDeltaTime, input, flGain, flDecay, result, szError, sizeof( szError ) ) )
	{
		ClientPrint( this, HUD_PRINTTALK, UTIL_VarArgs( "[skate] %s", szError ) );
		Warning( "[skate] %s: %s\n", GetPlayerName(), szError );
		m_Shared.RemoveCond( TF_COND_SKATING );
		return;
	}
	if ( result.nState == SKATE_STATE_LOADING )
	{
		// A few ticks while the simulation builds the skater: hold still where
		// it will appear (it starts with the velocity the player had). Keep
		// any knockback for its first step.
		pMove->m_vecVelocity = vec3_origin;
		m_flSkateTime = gpGlobals->curtime;
		return;
	}

	// What the owner's predicted skater needs to follow along: the usercmds
	// this skater ran, and any bail the server forced on it.
	CUserCmd *pCmd = GetCurrentCommand();
	int nCommand = pCmd ? pCmd->command_number : 0;
	if ( !m_nSkateStartCmd )
	{
		m_nSkateStartCmd = nCommand;
	}
	m_nSkateAckCmd = nCommand;
	m_vecSkateImpulse.Init();
	int nForced = input.nFlags & SKATE_STEP_FORCE_WIPEOUT;
	if ( nForced || input.vecImpulse != vec3_origin )
	{
		int nSlot = m_nSkateFlagCount % SKATE_FLAG_HISTORY;
		m_nSkateFlagCmd.Set( nSlot, nCommand );
		m_nSkateFlagBits.Set( nSlot, nForced );
		m_vecSkateFlagImpulse.Set( nSlot, input.vecImpulse );
		m_nSkateFlagCount = m_nSkateFlagCount + 1;
	}

	if ( skate_debug.GetBool() && result.nState != m_nSkateState )
	{
		Msg( "[skate] %s state %d -> %d at %.0f %.0f %.0f speed %.0f\n", GetPlayerName(), m_nSkateState.Get(), result.nState,
			result.vecOrigin.x, result.vecOrigin.y, result.vecOrigin.z, result.vecVelocity.Length() );
	}

	// A hard bail hurts like a fall; TF's thresholds, scaled to max health.
	if ( result.nState == SKATE_STATE_WIPEOUT && m_nSkateState != SKATE_STATE_WIPEOUT && m_flSkateFallSpeed > PLAYER_MAX_SAFE_FALL_SPEED )
	{
		float flDamage = ( m_flSkateFallSpeed - PLAYER_MAX_SAFE_FALL_SPEED ) * DAMAGE_FOR_FALL_SPEED * GetMaxHealth() / 100.0f;
		CBaseEntity *pWorld = GetWorldEntity();
		TakeDamage( CTakeDamageInfo( pWorld, pWorld, flDamage, DMG_FALL ) );
		if ( !IsAlive() || !m_Shared.InCond( TF_COND_SKATING ) )
			return;
	}
	m_flSkateFallSpeed = MAX( 0.0f, -result.vecVelocity.z );

	// Hall of Meat: each bone the bail broke since the last step hurts, and
	// cracks. The engine breaks a bone once per bail (Injury in sidecar/mod.rs).
	m_nSkateBail = result.nBail;
	m_nSkateBrokenBones = result.nBrokenBones;
	m_nSkateBailScore = (int)result.flBailScore;
	int nNewBreaks = result.nBreaks - m_nSkateBreaksSeen;
	m_nSkateBreaksSeen = result.nBreaks;
	if ( nNewBreaks > 0 && skate_hall_of_meat.GetBool() )
	{
		bool bNeck = result.nLastBreak == SKATE_BONE_NECK || result.nLastBreak == SKATE_BONE_SKULL;
		EmitSound( bNeck ? "Halloween.HammerImpactBloodyBoneCrunch" : "Flesh.Break" );
		float flDamage = ( nNewBreaks - ( bNeck ? 1 : 0 ) ) * skate_meat_bone_damage.GetFloat() + ( bNeck ? skate_meat_neck_damage.GetFloat() : 0.0f );
		CBaseEntity *pWorld = GetWorldEntity();
		TakeDamage( CTakeDamageInfo( pWorld, pWorld, flDamage, DMG_FALL ) );
		if ( !IsAlive() || !m_Shared.InCond( TF_COND_SKATING ) )
			return;
	}

	pMove->SetAbsOrigin( result.vecOrigin );
	m_vecSkateOrigin = result.vecOrigin;
	m_flSkateTime = gpGlobals->curtime;
	pMove->m_vecVelocity = result.vecVelocity;
	m_vecSkateVelocity = result.vecVelocity;
	SetGroundEntity( SkateStateIsGrounded( result.nState ) ? GetWorldEntity() : NULL );

	m_nSkateState = result.nState;
	SkateCollidePlayers();
	m_angSkateBody = result.angBody;
	m_vecSkateDeckOffset = result.vecDeckOrigin - result.vecOrigin;
	m_angSkateDeck = result.angDeck;
	m_nSkateTrickSeq = result.nTrickSeq;
	if ( V_strcmp( m_szSkateTrick.Get(), result.szTrick ) )
	{
		V_strncpy( m_szSkateTrick.GetForModify(), result.szTrick, 64 );
	}
	m_nSkateTrickScore = (int)result.flTrickScore;
	m_nSkateLineScore = (int)result.flLineScore;
	m_flSkateMultiplier = result.flMultiplier;
	m_nSkateTotalScore = (int)result.flTotalScore;
	m_nSkateScoreFlags = result.nScoreFlags;
	for ( int i = 0; i < SKATE_JOINT_COUNT; ++i )
	{
		Vector vecJoint = result.vecJoints[i];
		for ( int axis = 0; axis < 3; ++axis )
		{
			vecJoint[axis] = clamp( vecJoint[axis], -SKATE_JOINT_RANGE, SKATE_JOINT_RANGE );
		}
		m_vecSkateJoints.Set( i, vecJoint );
	}
	if ( result.flCameraFov > 0.0f )
	{
		m_vecSkateCameraOffset = result.vecCameraOrigin - result.vecOrigin;
		m_angSkateCamera = result.angCamera;
	}
}

//-----------------------------------------------------------------------------
// Hitboxes: TF2 poses the skeleton around the player's facing; turn that onto
// the skater's body frame, then retarget it to the Skate pose exactly as the
// client draws it.
//-----------------------------------------------------------------------------
void CTFPlayer::SetupBones( matrix3x4_t *pBoneToWorld, int boneMask )
{
	BaseClass::SetupBones( pBoneToWorld, boneMask );
	if ( !skate_hitboxes.GetBool() || !m_Shared.InCond( TF_COND_SKATING ) || !IsAlive() )
		return;
	CStudioHdr *hdr = GetModelPtr();
	if ( !hdr )
		return;

	matrix3x4_t absToWorld, bodyToWorld, worldToAbs, delta, moved;
	AngleMatrix( GetAbsAngles(), GetAbsOrigin(), absToWorld );
	AngleMatrix( m_angSkateBody.Get(), GetAbsOrigin(), bodyToWorld );
	MatrixInvert( absToWorld, worldToAbs );
	ConcatTransforms( bodyToWorld, worldToAbs, delta );
	for ( int i = 0; i < hdr->numbones(); ++i )
	{
		if ( hdr->boneFlags( i ) & boneMask )
		{
			ConcatTransforms( delta, pBoneToWorld[i], moved );
			pBoneToWorld[i] = moved;
		}
	}
	Vector vecJoints[ SKATE_JOINT_COUNT ];
	for ( int i = 0; i < SKATE_JOINT_COUNT; ++i )
	{
		vecJoints[i] = m_vecSkateJoints[i];
	}
	SkateApplyShove( vecJoints, bodyToWorld, m_vecSkateShoveDir, SkateShoveAmount( gpGlobals->curtime - m_flSkateShoveTime ) );
	SkateRetargetBones( hdr, pBoneToWorld, vecJoints, bodyToWorld, m_SkateRig, boneMask );
}

//-----------------------------------------------------------------------------
// Collisions with other players. The Skate simulation only knows the map, so
// players are handled here after each step: landing on someone's head kills
// them (a boots stomp), and running into someone knocks them away, hurting
// enemies; a hard enough hit makes the skater bail too. Teammates pass
// through, as in TF2, unless skate_collide_teammates is on.
//-----------------------------------------------------------------------------
void CTFPlayer::SkateCollidePlayers()
{
	if ( !skate_collide_players.GetBool() || !IsAlive() )
		return;
	const Vector vecMins = GetAbsOrigin() + VEC_HULL_MIN_SCALED( this ) - Vector( 2, 2, 2 );
	const Vector vecMaxs = GetAbsOrigin() + VEC_HULL_MAX_SCALED( this ) + Vector( 2, 2, 2 );
	const Vector &vecMine = m_vecSkateVelocity;

	for ( int i = 1; i <= gpGlobals->maxClients; ++i )
	{
		CTFPlayer *pOther = ToTFPlayer( UTIL_PlayerByIndex( i ) );
		if ( !pOther || pOther == this || !pOther->IsAlive() || pOther->IsObserver() )
			continue;
		bool bEnemy = pOther->GetTeamNumber() != GetTeamNumber();
		if ( !bEnemy && !skate_collide_teammates.GetBool() )
			continue;
		if ( gpGlobals->curtime - m_flSkateLastBump[i] < 0.5f )
			continue;
		const Vector vecOtherMins = pOther->GetAbsOrigin() + VEC_HULL_MIN_SCALED( pOther );
		const Vector vecOtherMaxs = pOther->GetAbsOrigin() + VEC_HULL_MAX_SCALED( pOther );
		if ( !IsBoxIntersectingBox( vecMins, vecMaxs, vecOtherMins, vecOtherMaxs ) )
			continue;

		Vector vecRelative = vecMine - pOther->GetAbsVelocity();
		Vector vecToOther = pOther->WorldSpaceCenter() - WorldSpaceCenter();
		vecToOther.z = 0.0f;
		if ( vecToOther.NormalizeInPlace() < 1.0f )
		{
			vecToOther = vecMine;
			vecToOther.z = 0.0f;
			vecToOther.NormalizeInPlace();
		}

		// Stomp: coming down onto their head.
		float flHeadHeight = vecOtherMins.z + ( vecOtherMaxs.z - vecOtherMins.z ) * 0.6f;
		if ( bEnemy && -vecMine.z >= skate_stomp_speed.GetFloat() && GetAbsOrigin().z >= flHeadHeight )
		{
			CTakeDamageInfo info( this, this, pOther->GetMaxHealth() * 4.0f, DMG_FALL );
			info.SetDamageCustom( TF_DMG_CUSTOM_BOOTS_STOMP );
			info.SetDamagePosition( pOther->EyePosition() );
			pOther->TakeDamage( info );
			pOther->EmitSound( "Weapon_Mantreads.Impact" );
			m_flSkateLastBump[i] = gpGlobals->curtime;
			continue;
		}

		// Ram: how fast the skater closes on them.
		float flClosing = DotProduct( vecRelative, vecToOther );
		if ( flClosing < skate_ram_speed.GetFloat() )
			continue;
		Vector vecPush = vecToOther * flClosing * 0.9f + Vector( 0, 0, 220.0f );
		pOther->ApplyAbsVelocityImpulse( vecPush );
		if ( bEnemy )
		{
			float flDamage = RemapValClamped( flClosing, skate_ram_speed.GetFloat(), 1000.0f, 8.0f, 75.0f );
			CTakeDamageInfo info( this, this, flDamage, DMG_CLUB );
			info.SetDamageForce( vecPush * 50.0f );
			info.SetDamagePosition( pOther->WorldSpaceCenter() );
			pOther->TakeDamage( info );
		}
		pOther->EmitSound( "Flesh.ImpactHard" );
		if ( flClosing >= skate_ram_bail_speed.GetFloat() )
		{
			m_bSkateBailNext = true;
		}
		m_flSkateLastBump[i] = gpGlobals->curtime;
	}
}

//-----------------------------------------------------------------------------
// Shove (the Shove skate control, IN_GRENADE2): push the nearest player in
// front of the skater. Skate 3's own shove animation waits on its pedestrian
// probe, which the engine hasn't recovered, so the mod pushes the target here
// and draws the arms itself (SkateApplyShove). Pushes go through
// ApplyGenericPushbackImpulse, so a shoved skater's simulation gets them.
//-----------------------------------------------------------------------------
void CTFPlayer::SkateShove()
{
	if ( !skate_shove.GetBool() || !IsAlive() || gpGlobals->curtime < m_flSkateNextShove )
		return;
	m_flSkateNextShove = gpGlobals->curtime + skate_shove_cooldown.GetFloat();

	Vector vecForward;
	AngleVectors( QAngle( 0, m_angSkateBody.Get()[ YAW ], 0 ), &vecForward );
	const Vector vecCenter = WorldSpaceCenter();
	const float flRange = skate_shove_range.GetFloat();
	CTFPlayer *pTarget = NULL;
	float flBest = FLT_MAX;
	Vector vecToTarget = vecForward;
	for ( int i = 1; i <= gpGlobals->maxClients; ++i )
	{
		CTFPlayer *pOther = ToTFPlayer( UTIL_PlayerByIndex( i ) );
		if ( !pOther || pOther == this || !pOther->IsAlive() || pOther->IsObserver() )
			continue;
		if ( pOther->GetTeamNumber() == GetTeamNumber() && !skate_collide_teammates.GetBool() )
			continue;
		Vector vecTo = pOther->WorldSpaceCenter() - vecCenter;
		if ( fabsf( vecTo.z ) > 64.0f )
			continue;
		Vector vecFlat( vecTo.x, vecTo.y, 0.0f );
		float flDist = vecFlat.NormalizeInPlace();
		if ( flDist > flRange || flDist >= flBest || DotProduct( vecFlat, vecForward ) < 0.3f )
			continue;
		trace_t tr;
		UTIL_TraceLine( vecCenter, pOther->WorldSpaceCenter(), MASK_SOLID_BRUSHONLY, this, COLLISION_GROUP_NONE, &tr );
		if ( tr.fraction < 1.0f )
			continue;
		pTarget = pOther;
		flBest = flDist;
		vecToTarget = vecFlat;
	}

	m_flSkateShoveTime = gpGlobals->curtime;
	m_vecSkateShoveDir = vecToTarget;
	if ( !pTarget )
	{
		EmitSound( "Weapon_Fist.Miss" );
		return;
	}

	// Harder the faster the shover closes on them.
	float flClosing = MAX( 0.0f, DotProduct( m_vecSkateVelocity - pTarget->GetAbsVelocity(), vecToTarget ) );
	float flPush = skate_shove_force.GetFloat() + flClosing * 0.5f;
	Vector vecPush = vecToTarget * flPush + Vector( 0, 0, 200.0f );
	pTarget->ApplyGenericPushbackImpulse( vecPush, this );
	if ( pTarget->m_Shared.InCond( TF_COND_SKATING ) && flPush >= skate_shove_bail_speed.GetFloat() )
	{
		pTarget->m_bSkateBailNext = true;
	}
	if ( pTarget->GetTeamNumber() != GetTeamNumber() && skate_shove_damage.GetFloat() > 0.0f )
	{
		CTakeDamageInfo info( this, this, skate_shove_damage.GetFloat(), DMG_CLUB );
		info.SetDamageForce( vecPush * 50.0f );
		info.SetDamagePosition( pTarget->WorldSpaceCenter() );
		pTarget->TakeDamage( info );
	}
	pTarget->EmitSound( "Weapon_Fist.HitFlesh" );
}
