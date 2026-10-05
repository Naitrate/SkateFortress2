//========= tf2-skate ============//
//
// Purpose: Skateboard driving for bots ("skate_bots 1").
//
//          A bot on a board ignores its TF AI movement and is driven here:
//          - Route: A* over the nav mesh with a skateboard cost (no ladders,
//            jump or crouch areas, no climbing more than a curb, stairs only
//            downhill, narrow areas and enemy spawn rooms avoided) to a far,
//            open area, re-planned on arrival or when stuck.
//          - Steering: pure pursuit on a lookahead point along the route's
//            portal points, with speed matched to how sharp the turn is
//            (push with A, brake with B), plus hull feelers that steer away
//            from walls and brake before them.
//          - Tricks: real Skate flick gestures on the right stick (sent as an
//            absolute stick, like a controller) on clear straight runs and
//            over curbs, with grabs in the air.
//          - Recovery: stuck against something, the bot hops off and back on
//            facing its route (a fresh skater spawn).
//
//          Stick and button meanings were measured on the sidecar
//          (tools/sidecar_smoke.py style probes): side +1 turns right, A
//          pushes, B brakes, Y steps off; down-then-up is an Ollie, down then
//          up-right a Kickflip, up-left a Heelflip, an arc a Pop Shuvit.
//
//=============================================================================//
#include "cbase.h"
#include "tf_player.h"
#include "tf_bot_skate.h"
#include "tf_skate_sidecar.h"
#include "in_buttons.h"
#include "nav_mesh.h"
#include "nav_pathfind.h"
#include "nav_mesh/tf_nav_area.h"

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

ConVar skate_bot_debug( "skate_bot_debug", "0", FCVAR_CHEAT, "Draw skating bots' routes, lookahead points and wall feelers." );
ConVar skate_bot_tricks( "skate_bot_tricks", "1", FCVAR_GAMEDLL, "Skating bots do flip tricks and grabs." );
ConVar skate_bot_speed( "skate_bot_speed", "260", FCVAR_GAMEDLL, "Skating bots' cruising speed on straights (units/s)." );

static const int SKATE_STATE_RIDING_MIN = 100;
static const int SKATE_STATE_AIR_MIN = 200;
static const int SKATE_STATE_GRIND_MIN = 400;
static const int SKATE_STATE_OFFBOARD_MIN = 500;

static const float SKATE_STEP_HEIGHT = 18.0f;		// curbs an ollie clears
static const float SKATE_MAX_DROP = 220.0f;		// higher drops end in a bail
static const float SKATE_GESTURE_STEP = 0.03f;		// seconds per stick position
static const int SKATE_MAX_WAYPOINTS = 64;

//-----------------------------------------------------------------------------
// Route cost for a skateboard.
//-----------------------------------------------------------------------------
class CSkatePathCost
{
public:
	CSkatePathCost( int nTeam ) : m_nTeam( nTeam ) {}

	float operator()( CNavArea *area, CNavArea *fromArea, const CNavLadder *ladder, const CFuncElevator *elevator, float length )
	{
		if ( !fromArea )
			return 0.0f;
		if ( ladder || elevator || ( area->GetAttributes() & ( NAV_MESH_JUMP | NAV_MESH_CROUCH ) ) )
			return -1.0f;

		CTFNavArea *tfArea = static_cast< CTFNavArea * >( area );
		if ( tfArea->HasAttributeTF( TF_NAV_BLOCKED ) )
			return -1.0f;
		// Respawn room visualizers stop the other team.
		if ( ( m_nTeam == TF_TEAM_RED && tfArea->HasAttributeTF( TF_NAV_SPAWN_ROOM_BLUE ) )
			|| ( m_nTeam == TF_TEAM_BLUE && tfArea->HasAttributeTF( TF_NAV_SPAWN_ROOM_RED ) ) )
			return -1.0f;

		float flDist = length > 0.0f ? length : ( area->GetCenter() - fromArea->GetCenter() ).Length();
		float flCost = flDist;

		float flRise = fromArea->ComputeAdjacentConnectionHeightChange( area );
		if ( flRise > SKATE_STEP_HEIGHT )
			return -1.0f;
		if ( flRise > 4.0f )
			flCost += 4.0f * flDist;					// needs an ollie
		if ( flRise < -SKATE_MAX_DROP )
			return -1.0f;
		if ( flRise < -64.0f )
			flCost += 2.0f * flDist;					// big drop: rough landing

		if ( area->GetAttributes() & NAV_MESH_STAIRS )
		{
			if ( flRise > 0.0f )
				return -1.0f;
			flCost += 3.0f * flDist;
		}
		if ( area->IsUnderwater() )
			flCost += 10.0f * flDist;

		// A board turns wide: tight spaces are slow and bumpy.
		float flWidth = MIN( area->GetSizeX(), area->GetSizeY() );
		if ( flWidth < 48.0f )
			flCost += 2.0f * flDist;

		return fromArea->GetCostSoFar() + flCost;
	}

private:
	int m_nTeam;
};

//-----------------------------------------------------------------------------
// Per-bot driving state.
//-----------------------------------------------------------------------------
struct SkateBotGesture_t
{
	const char	*pszName;
	int			nSteps;
	float		vecStick[ 8 ][ 2 ];	// right stick, up positive
	float		flWeight;
};

static const float C = 0.7071f;
static const SkateBotGesture_t s_Gestures[] =
{
	{ "ollie",		5, { { 0, -1 }, { 0, -1 }, { 0, -1 }, { 0, 1 }, { 0, 1 } }, 3.0f },
	{ "nollie",		5, { { 0, 1 }, { 0, 1 }, { 0, 1 }, { 0, -1 }, { 0, -1 } }, 1.0f },
	{ "kickflip",	5, { { 0, -1 }, { 0, -1 }, { 0, -1 }, { C, C }, { C, C } }, 2.0f },
	{ "heelflip",	5, { { 0, -1 }, { 0, -1 }, { 0, -1 }, { -C, C }, { -C, C } }, 2.0f },
	{ "shuvit",		6, { { 0, -1 }, { 0, -1 }, { -C, -C }, { -1, 0 }, { -C, C }, { 0, 1 } }, 1.5f },
};

struct SkateBotState_t
{
	void Reset()
	{
		nWaypoints = 0;
		nNextWaypoint = 0;
		flReplanTime = 0.0f;
		flStuckSince = -1.0f;
		flNextTrick = gpGlobals->curtime + RandomFloat( 2.0f, 4.0f );
		pGesture = NULL;
		flGestureStart = 0.0f;
		nGrabButton = 0;
		flGrabUntil = 0.0f;
		flPushUntil = 0.0f;
		flRemountAt = 0.0f;
		flRemountYaw = 0.0f;
		flOffBoardSince = -1.0f;
		vecGoal = vec3_origin;
	}

	Vector	vecWaypoints[ SKATE_MAX_WAYPOINTS ];
	int		nWaypoints;
	int		nNextWaypoint;
	Vector	vecGoal;
	float	flReplanTime;
	float	flStuckSince;
	float	flNextTrick;
	const SkateBotGesture_t *pGesture;
	float	flGestureStart;
	int		nGrabButton;
	float	flGrabUntil;
	float	flPushUntil;
	float	flRemountAt;		// > 0: hopped off to unstick; get back on then
	float	flRemountYaw;
	float	flOffBoardSince;
};

static SkateBotState_t s_BotState[ MAX_PLAYERS + 1 ];

static SkateBotState_t &StateFor( CTFPlayer *pBot )
{
	return s_BotState[ clamp( pBot->entindex(), 0, MAX_PLAYERS ) ];
}

//-----------------------------------------------------------------------------
// Route planning.
//-----------------------------------------------------------------------------

// A far, roomy area the bot can reach on a board, or NULL.
static CNavArea *PickGoalArea( CTFPlayer *pBot, CNavArea *pStart )
{
	if ( TheNavAreas.Count() == 0 )
		return NULL;
	CNavArea *pBest = NULL;
	float flBestScore = -1.0f;
	for ( int i = 0; i < 24; ++i )
	{
		CNavArea *pArea = TheNavAreas[ RandomInt( 0, TheNavAreas.Count() - 1 ) ];
		if ( pArea == pStart || ( pArea->GetAttributes() & ( NAV_MESH_JUMP | NAV_MESH_CROUCH | NAV_MESH_STAIRS ) ) || pArea->IsUnderwater() )
			continue;
		float flDist = ( pArea->GetCenter() - pBot->GetAbsOrigin() ).Length();
		if ( flDist < 600.0f )
			continue;
		// Prefer open floor at a medium distance: good skating, short replans.
		float flSize = MIN( pArea->GetSizeX(), pArea->GetSizeY() );
		float flScore = MIN( flSize, 300.0f ) + RandomFloat( 0.0f, 200.0f ) - fabsf( flDist - 2000.0f ) * 0.1f;
		if ( flScore > flBestScore )
		{
			flBestScore = flScore;
			pBest = pArea;
		}
	}
	return pBest;
}

static bool PlanRoute( CTFPlayer *pBot, SkateBotState_t &state )
{
	state.nWaypoints = 0;
	state.nNextWaypoint = 0;
	state.flReplanTime = gpGlobals->curtime + 12.0f;

	CNavArea *pStart = TheNavMesh->GetNearestNavArea( pBot->GetAbsOrigin(), false, 300.0f, true );
	if ( !pStart )
		return false;

	for ( int nTry = 0; nTry < 4; ++nTry )
	{
		CNavArea *pGoal = PickGoalArea( pBot, pStart );
		if ( !pGoal )
			return false;
		CSkatePathCost cost( pBot->GetTeamNumber() );
		CNavArea *pClosest = NULL;
		Vector vecGoal = pGoal->GetCenter();
		NavAreaBuildPath( pStart, pGoal, &vecGoal, cost, &pClosest, 6000.0f, pBot->GetTeamNumber() );
		CNavArea *pEnd = pClosest ? pClosest : pGoal;
		if ( pEnd == pStart || ( pEnd->GetCenter() - pStart->GetCenter() ).Length() < 400.0f )
			continue;

		// Walk back from the end, collecting portal centres, then reverse.
		Vector vecPoints[ SKATE_MAX_WAYPOINTS ];
		int nPoints = 0;
		vecPoints[ nPoints++ ] = pEnd->GetCenter();
		for ( CNavArea *pArea = pEnd; pArea->GetParent() && nPoints < SKATE_MAX_WAYPOINTS; pArea = pArea->GetParent() )
		{
			NavTraverseType how = pArea->GetParentHow();
			Vector vecPortal = pArea->GetCenter();
			if ( how < NUM_DIRECTIONS )
			{
				float flHalfWidth;
				pArea->GetParent()->ComputePortal( pArea, (NavDirType)how, &vecPortal, &flHalfWidth );
			}
			vecPoints[ nPoints++ ] = vecPortal;
		}
		// Many points are only a few units apart; keep ones worth steering to.
		for ( int i = nPoints - 1; i >= 0 && state.nWaypoints < SKATE_MAX_WAYPOINTS; --i )
		{
			if ( state.nWaypoints && ( vecPoints[i] - state.vecWaypoints[ state.nWaypoints - 1 ] ).Length2D() < 48.0f && i != 0 )
				continue;
			state.vecWaypoints[ state.nWaypoints++ ] = vecPoints[i];
		}
		state.vecGoal = pEnd->GetCenter();
		return state.nWaypoints > 0;
	}
	return false;
}

//-----------------------------------------------------------------------------
// Steering helpers.
//-----------------------------------------------------------------------------

// Free distance along a yaw from the skater, at board height (above curbs).
static float FeelerDistance( CTFPlayer *pBot, float flYaw, float flRange, float flHeight, trace_t *pTrace = NULL )
{
	Vector vecDir;
	AngleVectors( QAngle( 0, flYaw, 0 ), &vecDir );
	Vector vecStart = pBot->GetAbsOrigin() + Vector( 0, 0, flHeight );
	trace_t tr;
	UTIL_TraceHull( vecStart, vecStart + vecDir * flRange, Vector( -12, -12, 0 ), Vector( 12, 12, 16 ), MASK_PLAYERSOLID, pBot, COLLISION_GROUP_PLAYER_MOVEMENT, &tr );
	if ( skate_bot_debug.GetBool() )
	{
		NDebugOverlay::Line( vecStart, tr.endpos, tr.DidHit() ? 255 : 0, tr.DidHit() ? 0 : 255, 0, true, 0.1f );
	}
	if ( pTrace )
		*pTrace = tr;
	return tr.fraction * flRange;
}

// Point `flLookahead` units along the route past the bot's current waypoint.
static Vector LookaheadPoint( CTFPlayer *pBot, SkateBotState_t &state, float flLookahead )
{
	const Vector &vecOrigin = pBot->GetAbsOrigin();
	// Drop waypoints we've reached or passed.
	while ( state.nNextWaypoint < state.nWaypoints - 1 )
	{
		const Vector &a = state.vecWaypoints[ state.nNextWaypoint ];
		const Vector &b = state.vecWaypoints[ state.nNextWaypoint + 1 ];
		Vector2D toA = ( a - vecOrigin ).AsVector2D();
		Vector2D segment = ( b - a ).AsVector2D();
		if ( toA.Length() < 72.0f || toA.Dot( segment ) < 0.0f )
			++state.nNextWaypoint;
		else
			break;
	}
	Vector vecPoint = state.vecWaypoints[ state.nNextWaypoint ];
	float flLeft = flLookahead - ( vecPoint - vecOrigin ).Length2D();
	for ( int i = state.nNextWaypoint; i < state.nWaypoints - 1 && flLeft > 0.0f; ++i )
	{
		Vector vecSegment = state.vecWaypoints[ i + 1 ] - state.vecWaypoints[ i ];
		float flLength = vecSegment.Length2D();
		if ( flLength >= flLeft )
		{
			vecPoint = state.vecWaypoints[ i ] + vecSegment * ( flLeft / MAX( flLength, 1.0f ) );
			break;
		}
		vecPoint = state.vecWaypoints[ i + 1 ];
		flLeft -= flLength;
	}
	return vecPoint;
}

//-----------------------------------------------------------------------------
// Tricks.
//-----------------------------------------------------------------------------
static void StartGesture( SkateBotState_t &state, const SkateBotGesture_t *pGesture )
{
	state.pGesture = pGesture;
	state.flGestureStart = gpGlobals->curtime;
	// Sometimes follow the pop with a grab: hold a trigger once in the air.
	if ( skate_bot_tricks.GetBool() && RandomFloat() < 0.35f )
	{
		state.nGrabButton = RandomInt( 0, 1 ) ? IN_ATTACK : IN_ATTACK2;
		state.flGrabUntil = gpGlobals->curtime + 0.75f;
	}
	else
	{
		state.nGrabButton = 0;
	}
}

static const SkateBotGesture_t *RandomGesture()
{
	float flTotal = 0.0f;
	for ( int i = 0; i < ARRAYSIZE( s_Gestures ); ++i )
		flTotal += s_Gestures[i].flWeight;
	float flPick = RandomFloat( 0.0f, flTotal );
	for ( int i = 0; i < ARRAYSIZE( s_Gestures ); ++i )
	{
		flPick -= s_Gestures[i].flWeight;
		if ( flPick <= 0.0f )
			return &s_Gestures[i];
	}
	return &s_Gestures[0];
}

//-----------------------------------------------------------------------------
// Public entry points.
//-----------------------------------------------------------------------------
void TFBotSkateReset( CTFPlayer *pBot )
{
	StateFor( pBot ).Reset();
}

void TFBotSkateInput( CTFPlayer *pBot, SkateInput_t &input )
{
	SkateBotState_t &state = StateFor( pBot );
	const float flNow = gpGlobals->curtime;
	const int nState = pBot->m_nSkateState;

	input.nButtons = 0;
	input.flForward = 0.0f;
	input.flSide = 0.0f;
	input.flMouseX = input.flMouseY = 0.0f;

	Vector vecVelocity = pBot->GetAbsVelocity();
	float flSpeed = vecVelocity.Length2D();
	// Board heading: the way it is rolling, else the deck's nose.
	float flHeading = flSpeed > 40.0f ? RAD2DEG( atan2f( vecVelocity.y, vecVelocity.x ) ) : pBot->m_angSkateDeck.Get()[ YAW ];

	bool bGrounded = nState >= SKATE_STATE_RIDING_MIN && nState < SKATE_STATE_AIR_MIN;
	bool bAirborne = nState >= SKATE_STATE_AIR_MIN && nState < SKATE_STATE_WIPEOUT;
	bool bOffBoard = nState >= SKATE_STATE_OFFBOARD_MIN && nState < 600;
	bool bGrinding = nState >= SKATE_STATE_GRIND_MIN && nState < SKATE_STATE_OFFBOARD_MIN;

	// Off the board by accident (a bail can leave the skater on foot): Y
	// gets back on. Pressed briefly, released, then retried.
	if ( bOffBoard )
	{
		if ( state.flOffBoardSince < 0.0f )
			state.flOffBoardSince = flNow;
		if ( fmodf( flNow - state.flOffBoardSince, 1.5f ) > 0.8f && fmodf( flNow - state.flOffBoardSince, 1.5f ) < 0.9f )
			input.nButtons |= IN_USE;
		return;
	}
	state.flOffBoardSince = -1.0f;

	// Bailing: let Skate's ragdoll and respawn play out, then plan afresh.
	if ( nState == SKATE_STATE_WIPEOUT || nState < SKATE_STATE_RIDING_MIN )
	{
		state.nWaypoints = 0;
		state.pGesture = NULL;
		return;
	}

	if ( state.nWaypoints == 0 || flNow > state.flReplanTime
		|| ( state.nNextWaypoint >= state.nWaypoints - 1 && ( pBot->GetAbsOrigin() - state.vecGoal ).Length2D() < 150.0f ) )
	{
		PlanRoute( pBot, state );
	}

	// --- Steering -----------------------------------------------------------
	float flDesiredSpeed = skate_bot_speed.GetFloat();
	float flTargetYaw = flHeading;
	if ( state.nWaypoints > 0 )
	{
		float flLookahead = clamp( flSpeed * 0.7f, 110.0f, 320.0f );
		Vector vecTarget = LookaheadPoint( pBot, state, flLookahead );
		Vector vecTo = vecTarget - pBot->GetAbsOrigin();
		flTargetYaw = RAD2DEG( atan2f( vecTo.y, vecTo.x ) );
		if ( skate_bot_debug.GetBool() )
		{
			for ( int i = MAX( 0, state.nNextWaypoint - 1 ); i < state.nWaypoints - 1; ++i )
				NDebugOverlay::Line( state.vecWaypoints[i] + Vector( 0, 0, 8 ), state.vecWaypoints[ i + 1 ] + Vector( 0, 0, 8 ), 0, 160, 255, true, 0.1f );
			NDebugOverlay::Cross3D( vecTarget + Vector( 0, 0, 8 ), 12.0f, 255, 255, 0, true, 0.1f );
		}
	}

	// Wall feelers ahead and to each side, at board height (above curbs).
	float flRange = 64.0f + flSpeed * 0.6f;
	trace_t trAhead;
	float flAhead = FeelerDistance( pBot, flHeading, flRange, 20.0f, &trAhead );
	float flLeft = FeelerDistance( pBot, flHeading + 35.0f, flRange * 0.8f, 20.0f );
	float flRight = FeelerDistance( pBot, flHeading - 35.0f, flRange * 0.8f, 20.0f );
	float flAvoid = 0.0f;	// degrees added to the target yaw
	if ( flAhead < flRange )
	{
		// Turn toward whichever side is more open, harder the closer it is.
		float flUrgency = 1.0f - flAhead / flRange;
		flAvoid = ( flLeft >= flRight ? 1.0f : -1.0f ) * 70.0f * flUrgency;
		flDesiredSpeed = MIN( flDesiredSpeed, 60.0f + flAhead * 1.5f );
	}
	else if ( flLeft < flRange * 0.8f || flRight < flRange * 0.8f )
	{
		flAvoid = ( flLeft - flRight ) / ( flRange * 0.8f ) * 25.0f;
	}

	float flError = AngleDiff( flTargetYaw + flAvoid, flHeading );	// + = target is to the left
	// Side +1 steers right (yaw decreasing).
	input.flSide = clamp( -flError / 30.0f, -1.0f, 1.0f );

	// A board can't turn tight at speed: slow for corners.
	float flAbsError = fabsf( flError );
	if ( flAbsError > 25.0f )
		flDesiredSpeed = MIN( flDesiredSpeed, 220.0f - ( flAbsError - 25.0f ) * 2.0f );
	flDesiredSpeed = MAX( flDesiredSpeed, 70.0f );

	if ( bGrounded )
	{
		if ( flSpeed > flDesiredSpeed + 70.0f || ( flAhead < 56.0f && flSpeed > 90.0f ) )
		{
			input.nButtons |= IN_RELOAD;	// B: brake
		}
		else if ( flSpeed < flDesiredSpeed - 25.0f && flAbsError < 60.0f && flNow > state.flPushUntil + 0.25f )
		{
			state.flPushUntil = flNow + 0.3f;	// one push: tap, release, repeat
		}
		if ( flNow < state.flPushUntil )
			input.nButtons |= IN_JUMP;
	}

	// --- Stuck --------------------------------------------------------------
	// Pushing but going nowhere: hop off and back on facing the route.
	if ( bGrounded && flSpeed < 25.0f && ( flAhead < 40.0f || flNow > state.flPushUntil + 1.0f ) )
	{
		if ( state.flStuckSince < 0.0f )
			state.flStuckSince = flNow;
		else if ( flNow - state.flStuckSince > 1.5f )
		{
			state.flStuckSince = -1.0f;
			float flYaw = state.nWaypoints ? flTargetYaw : flHeading + 180.0f;
			if ( flAhead < 40.0f && state.nWaypoints == 0 )
				flYaw = flHeading + ( flLeft >= flRight ? 120.0f : -120.0f );
			pBot->SnapEyeAngles( QAngle( 0, flYaw, 0 ) );
			state.flRemountAt = flNow + 0.3f;
			state.flRemountYaw = flYaw;
			if ( skate_bot_debug.GetBool() )
				Msg( "[skate] %s stuck; remounting facing %.0f\n", pBot->GetPlayerName(), flYaw );
			pBot->m_Shared.RemoveCond( TF_COND_SKATING );
			return;
		}
	}
	else
	{
		state.flStuckSince = -1.0f;
	}

	// --- Tricks -------------------------------------------------------------
	if ( !state.pGesture && bGrounded && skate_bot_tricks.GetBool() )
	{
		// A curb ahead that the board would hit but the body clears: ollie it.
		trace_t trLow;
		float flLow = FeelerDistance( pBot, flHeading, 40.0f + flSpeed * 0.3f, 3.0f, &trLow );
		bool bCurb = flSpeed > 100.0f && flLow < 40.0f + flSpeed * 0.3f && flAhead >= flRange
			&& trLow.plane.normal.z < 0.7f;
		bool bClearRun = flSpeed > 140.0f && flAbsError < 12.0f && flAhead >= flRange
			&& FeelerDistance( pBot, flHeading, flSpeed * 1.2f + 96.0f, 20.0f ) >= flSpeed * 1.2f + 95.0f;
		if ( bCurb )
		{
			StartGesture( state, &s_Gestures[0] );
		}
		else if ( bClearRun && flNow > state.flNextTrick )
		{
			// Only over roughly level ground, so the landing is clean.
			Vector vecDir;
			AngleVectors( QAngle( 0, flHeading, 0 ), &vecDir );
			Vector vecProbe = pBot->GetAbsOrigin() + vecDir * flSpeed * 0.8f + Vector( 0, 0, 32 );
			trace_t trGround;
			UTIL_TraceLine( vecProbe, vecProbe - Vector( 0, 0, 96 ), MASK_PLAYERSOLID, pBot, COLLISION_GROUP_PLAYER_MOVEMENT, &trGround );
			if ( trGround.DidHit() && fabsf( trGround.endpos.z - pBot->GetAbsOrigin().z ) < 24.0f )
			{
				StartGesture( state, RandomGesture() );
				state.flNextTrick = flNow + RandomFloat( 2.5f, 6.0f );
			}
		}
	}

	if ( state.pGesture )
	{
		int nStep = (int)( ( flNow - state.flGestureStart ) / SKATE_GESTURE_STEP );
		if ( nStep < state.pGesture->nSteps )
		{
			// Absolute right stick, as a controller sends it.
			input.nButtons |= IN_BULLRUSH;
			input.flMouseX = state.pGesture->vecStick[ nStep ][0] * 32767.0f;
			input.flMouseY = state.pGesture->vecStick[ nStep ][1] * 32767.0f;
			input.nButtons &= ~( IN_JUMP | IN_RELOAD );
			input.flSide *= 0.3f;	// keep the line while popping
		}
		else
		{
			state.pGesture = NULL;
		}
	}
	if ( state.nGrabButton && bAirborne && flNow < state.flGrabUntil && flNow > state.flGestureStart + 0.2f )
	{
		input.nButtons |= state.nGrabButton;
	}
	else if ( flNow >= state.flGrabUntil )
	{
		state.nGrabButton = 0;
	}
	if ( bAirborne || bGrinding )
	{
		// Pushing or braking mid-air or on a rail would end the trick/grind.
		input.nButtons &= ~( IN_JUMP | IN_RELOAD );
	}
}

void TFBotSkateThink( CTFPlayer *pBot )
{
	SkateBotState_t &state = StateFor( pBot );
	if ( state.flRemountAt > 0.0f && gpGlobals->curtime >= state.flRemountAt )
	{
		state.flRemountAt = 0.0f;
		if ( !pBot->IsAlive() || pBot->m_Shared.InCond( TF_COND_SKATING ) )
			return;
		// The bot's aim code may have turned it meanwhile; the new skater
		// spawns facing the eye yaw.
		pBot->SnapEyeAngles( QAngle( 0, state.flRemountYaw, 0 ) );
		char szError[ 256 ];
		if ( pBot->StartSkating( szError, sizeof( szError ) ) )
		{
		}
		else if ( skate_bot_debug.GetBool() )
		{
			Msg( "[skate] %s couldn't remount: %s\n", pBot->GetPlayerName(), szError );
		}
	}
}
