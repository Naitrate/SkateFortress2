//========= tf2-skate ============//
//
// Purpose: Skate 3 pose -> TF2 class skeleton, shared by the client (what is
//          drawn) and the server (hitboxes for hit detection), so shots land
//          where the skater is drawn.
//
//          Direction-based retargeting: each TF2 limb bone is turned (minimal
//          rotation) to point where the matching Skate bone points, and the
//          pelvis/chest are turned so the frame spanned by their joints (hips
//          or shoulders, plus the spine) matches. TF2 keeps its own bone
//          lengths and twist; everything else follows by FK.
//
//=============================================================================//
#include "cbase.h"
#include "tf_skate_pose.h"
#include "bone_setup.h"

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

enum ESkateRigBone
{
	RIG_PELVIS, RIG_SPINE_0, RIG_SPINE_1, RIG_SPINE_2, RIG_SPINE_3, RIG_NECK, RIG_HEAD,
	RIG_UPPERARM_L, RIG_LOWERARM_L, RIG_HAND_L, RIG_UPPERARM_R, RIG_LOWERARM_R, RIG_HAND_R,
	RIG_HIP_L, RIG_KNEE_L, RIG_FOOT_L, RIG_TOE_L, RIG_HIP_R, RIG_KNEE_R, RIG_FOOT_R, RIG_TOE_R,
	RIG_COUNT
};

const char *s_pszSkateRigBones[ RIG_COUNT ] =
{
	"bip_pelvis", "bip_spine_0", "bip_spine_1", "bip_spine_2", "bip_spine_3", "bip_neck", "bip_head",
	"bip_upperArm_L", "bip_lowerArm_L", "bip_hand_L", "bip_upperArm_R", "bip_lowerArm_R", "bip_hand_R",
	"bip_hip_L", "bip_knee_L", "bip_foot_L", "bip_toe_L", "bip_hip_R", "bip_knee_R", "bip_foot_R", "bip_toe_R",
};

// A TF2 bone aimed at its child along the Skate bone between two joints.
struct SkateSegment_t
{
	int nBone;
	int nChild;
	int nFrom;
	int nTo;
};

static const SkateSegment_t s_SkateSegments[] =
{
	{ RIG_UPPERARM_L, RIG_LOWERARM_L, SKATE_JOINT_LEFT_ARM, SKATE_JOINT_LEFT_FOREARM },
	{ RIG_LOWERARM_L, RIG_HAND_L, SKATE_JOINT_LEFT_FOREARM, SKATE_JOINT_LEFT_HAND },
	{ RIG_UPPERARM_R, RIG_LOWERARM_R, SKATE_JOINT_RIGHT_ARM, SKATE_JOINT_RIGHT_FOREARM },
	{ RIG_LOWERARM_R, RIG_HAND_R, SKATE_JOINT_RIGHT_FOREARM, SKATE_JOINT_RIGHT_HAND },
	{ RIG_HIP_L, RIG_KNEE_L, SKATE_JOINT_LEFT_UPLEG, SKATE_JOINT_LEFT_LEG },
	{ RIG_KNEE_L, RIG_FOOT_L, SKATE_JOINT_LEFT_LEG, SKATE_JOINT_LEFT_FOOT },
	{ RIG_FOOT_L, RIG_TOE_L, SKATE_JOINT_LEFT_FOOT, SKATE_JOINT_LEFT_TOE },
	{ RIG_HIP_R, RIG_KNEE_R, SKATE_JOINT_RIGHT_UPLEG, SKATE_JOINT_RIGHT_LEG },
	{ RIG_KNEE_R, RIG_FOOT_R, SKATE_JOINT_RIGHT_LEG, SKATE_JOINT_RIGHT_FOOT },
	{ RIG_FOOT_R, RIG_TOE_R, SKATE_JOINT_RIGHT_FOOT, SKATE_JOINT_RIGHT_TOE },
	{ RIG_NECK, RIG_HEAD, SKATE_JOINT_NECK, SKATE_JOINT_HEAD },
};

// Pre-multiply a bone's rotation by `rotation`, keeping its origin.
static void SkateRotateBone( matrix3x4_t &bone, const matrix3x4_t &rotation )
{
	Vector vecOrigin;
	MatrixPosition( bone, vecOrigin );
	matrix3x4_t out;
	ConcatTransforms( rotation, bone, out );
	MatrixSetColumn( vecOrigin, 3, out );
	bone = out;
}

static void SkateRotationBetween( Vector vecFrom, Vector vecTo, matrix3x4_t &out )
{
	SetIdentityMatrix( out );
	if ( vecFrom.NormalizeInPlace() < 1e-3f || vecTo.NormalizeInPlace() < 1e-3f )
		return;
	Vector vecAxis = CrossProduct( vecFrom, vecTo );
	float flSin = vecAxis.NormalizeInPlace();
	if ( flSin < 1e-5f )
		return;
	Quaternion q;
	AxisAngleQuaternion( vecAxis, RAD2DEG( atan2f( flSin, DotProduct( vecFrom, vecTo ) ) ), q );
	QuaternionMatrix( q, out );
}

// Orthonormal frame with x along `right` and z as close to `up` as possible.
static bool SkateFrame( Vector vecRight, const Vector &vecUp, matrix3x4_t &out )
{
	if ( vecRight.NormalizeInPlace() < 1e-3f )
		return false;
	Vector vecZ = vecUp - vecRight * DotProduct( vecUp, vecRight );
	if ( vecZ.NormalizeInPlace() < 1e-3f )
		return false;
	out.Init( vecRight, CrossProduct( vecZ, vecRight ), vecZ, vec3_origin );
	return true;
}

// Rotation taking frame `current` onto frame `target`, applied by fraction t.
static void SkateFrameDelta( const matrix3x4_t &target, const matrix3x4_t &current, float t, matrix3x4_t &out )
{
	matrix3x4_t inverse, delta;
	MatrixInvert( current, inverse );
	ConcatTransforms( target, inverse, delta );
	if ( t >= 1.0f )
	{
		out = delta;
		return;
	}
	Quaternion q, identity( 0, 0, 0, 1 ), partial;
	MatrixQuaternion( delta, q );
	QuaternionSlerp( identity, q, t, partial );
	QuaternionMatrix( partial, out );
}

bool SkateRetargetBones( CStudioHdr *hdr, matrix3x4_t *pBones, const Vector *J, const matrix3x4_t &rootToWorld, SkateRigCache_t &cache, int boneMask )
{
	if ( J[ SKATE_JOINT_HIPS ].LengthSqr() < 1.0f )
		return false;
	if ( cache.pModel != hdr->GetRenderHdr() )
	{
		cache.pModel = hdr->GetRenderHdr();
		for ( int i = 0; i < RIG_COUNT; ++i )
		{
			cache.nBones[i] = Studio_BoneIndexByName( hdr, s_pszSkateRigBones[i] );
		}
	}
	const int *pRig = cache.nBones;
	for ( int i = 0; i < RIG_COUNT; ++i )
	{
		if ( pRig[i] < 0 )
			return false;	// Not a class rig (custom model); leave TF2's pose alone.
	}

	const int nBones = hdr->numbones();
	CUtlVector< matrix3x4_t > old, cur;
	CUtlVector< int > role;
	old.SetCount( nBones );
	cur.SetCount( nBones );
	role.SetCount( nBones );
	for ( int i = 0; i < nBones; ++i )
	{
		old[i] = pBones[i];
		role[i] = -1;
	}
	for ( int r = 0; r < RIG_COUNT; ++r )
	{
		role[ pRig[r] ] = r;
	}

	// Skate joints in world space, scaled so the legs match this class.
	Vector vecHipL, vecKneeL, vecFootL;
	MatrixPosition( old[ pRig[RIG_HIP_L] ], vecHipL );
	MatrixPosition( old[ pRig[RIG_KNEE_L] ], vecKneeL );
	MatrixPosition( old[ pRig[RIG_FOOT_L] ], vecFootL );
	float flClassLeg = ( vecHipL - vecKneeL ).Length() + ( vecKneeL - vecFootL ).Length();
	float flSkateLeg = ( J[SKATE_JOINT_LEFT_UPLEG] - J[SKATE_JOINT_LEFT_LEG] ).Length() + ( J[SKATE_JOINT_LEFT_LEG] - J[SKATE_JOINT_LEFT_FOOT] ).Length();
	float flScale = flSkateLeg > 1.0f ? clamp( flClassLeg / flSkateLeg, 0.5f, 2.0f ) : 1.0f;
	Vector W[ SKATE_JOINT_COUNT ];
	for ( int j = 0; j < SKATE_JOINT_COUNT; ++j )
	{
		VectorTransform( J[j] * flScale, rootToWorld, W[j] );
	}

	// World position of `desc` if bone i keeps its animated relation to it.
	auto Measure = [&]( int i, int nRigDesc ) -> Vector
	{
		matrix3x4_t inverse;
		MatrixInvert( old[i], inverse );
		Vector vecDesc, vecLocal, vecWorld;
		MatrixPosition( old[ pRig[nRigDesc] ], vecDesc );
		VectorTransform( vecDesc, inverse, vecLocal );
		VectorTransform( vecLocal, cur[i], vecWorld );
		return vecWorld;
	};

	matrix3x4_t chestTarget;
	bool bChest = SkateFrame( W[SKATE_JOINT_RIGHT_ARM] - W[SKATE_JOINT_LEFT_ARM], W[SKATE_JOINT_NECK] - W[SKATE_JOINT_CHEST], chestTarget );

	for ( int i = 0; i < nBones; ++i )
	{
		int nParent = hdr->boneParent( i );
		if ( nParent < 0 )
		{
			cur[i] = old[i];
		}
		else
		{
			matrix3x4_t inverse, local;
			MatrixInvert( old[ nParent ], inverse );
			ConcatTransforms( inverse, old[i], local );
			ConcatTransforms( cur[ nParent ], local, cur[i] );
		}

		Vector vecOrigin;
		MatrixPosition( cur[i], vecOrigin );
		matrix3x4_t rotation;
		switch ( role[i] )
		{
		case RIG_PELVIS:
		{
			matrix3x4_t target, current;
			if ( SkateFrame( W[SKATE_JOINT_RIGHT_UPLEG] - W[SKATE_JOINT_LEFT_UPLEG], W[SKATE_JOINT_SPINE] - W[SKATE_JOINT_HIPS], target ) &&
				 SkateFrame( Measure( i, RIG_HIP_R ) - Measure( i, RIG_HIP_L ), Measure( i, RIG_SPINE_1 ) - vecOrigin, current ) )
			{
				SkateFrameDelta( target, current, 1.0f, rotation );
				SkateRotateBone( cur[i], rotation );
			}
			MatrixSetColumn( W[SKATE_JOINT_HIPS], 3, cur[i] );
			break;
		}
		case RIG_SPINE_0:
		case RIG_SPINE_1:
		case RIG_SPINE_2:
		{
			// Spread the chest's turn over three spine bones.
			matrix3x4_t current;
			if ( bChest && SkateFrame( Measure( i, RIG_UPPERARM_R ) - Measure( i, RIG_UPPERARM_L ), Measure( i, RIG_NECK ) - Measure( i, RIG_SPINE_3 ), current ) )
			{
				SkateFrameDelta( chestTarget, current, 1.0f / ( RIG_SPINE_2 - role[i] + 1 ), rotation );
				SkateRotateBone( cur[i], rotation );
			}
			break;
		}
		default:
			for ( int s = 0; s < ARRAYSIZE( s_SkateSegments ); ++s )
			{
				const SkateSegment_t &seg = s_SkateSegments[s];
				if ( seg.nBone != role[i] )
					continue;
				SkateRotationBetween( Measure( i, seg.nChild ) - vecOrigin, W[ seg.nTo ] - W[ seg.nFrom ], rotation );
				SkateRotateBone( cur[i], rotation );
				break;
			}
			break;
		}
	}

	for ( int i = 0; i < nBones; ++i )
	{
		if ( hdr->boneFlags( i ) & boneMask )
		{
			pBones[i] = cur[i];
		}
	}
	return true;
}

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------

//-----------------------------------------------------------------------------
// Shove: both hands reach out to a point in front of the chest, the elbows
// straightening along the way. The joints are positions, so moving the elbow
// and wrist targets is enough; the retarget turns the arm bones to follow.
//-----------------------------------------------------------------------------
float SkateShoveAmount( float flSinceShove )
{
	if ( flSinceShove < 0.0f || flSinceShove > SKATE_SHOVE_TIME )
		return 0.0f;
	const float flOut = 0.12f, flHold = 0.2f;
	if ( flSinceShove < flOut )
	{
		float x = flSinceShove / flOut;
		return x * x * ( 3.0f - 2.0f * x );
	}
	if ( flSinceShove < flHold )
		return 1.0f;
	float x = ( flSinceShove - flHold ) / ( SKATE_SHOVE_TIME - flHold );
	return 1.0f - x * x * ( 3.0f - 2.0f * x );
}

void SkateApplyShove( Vector *pJoints, const matrix3x4_t &rootToWorld, const Vector &vecDirection, float flAmount )
{
	if ( flAmount <= 0.0f )
		return;
	Vector vecDir;
	VectorIRotate( vecDirection, rootToWorld, vecDir );
	vecDir.z = clamp( vecDir.z, -0.3f, 0.3f );
	if ( vecDir.NormalizeInPlace() < 0.1f )
	{
		vecDir.Init( 1, 0, 0 );
	}
	const Vector vecChest = ( pJoints[ SKATE_JOINT_LEFT_ARM ] + pJoints[ SKATE_JOINT_RIGHT_ARM ] ) * 0.5f;
	static const int s_nArms[2][3] =
	{
		{ SKATE_JOINT_LEFT_ARM, SKATE_JOINT_LEFT_FOREARM, SKATE_JOINT_LEFT_HAND },
		{ SKATE_JOINT_RIGHT_ARM, SKATE_JOINT_RIGHT_FOREARM, SKATE_JOINT_RIGHT_HAND },
	};
	for ( int side = 0; side < 2; ++side )
	{
		const Vector &vecShoulder = pJoints[ s_nArms[side][0] ];
		Vector &vecElbow = pJoints[ s_nArms[side][1] ];
		Vector &vecHand = pJoints[ s_nArms[side][2] ];
		float flUpper = ( vecElbow - vecShoulder ).Length();
		float flReach = ( flUpper + ( vecHand - vecElbow ).Length() ) * 0.95f;
		// Palms a little narrower than the shoulders, at chest height.
		Vector vecTarget = vecChest + vecDir * flReach + ( vecShoulder - vecChest ) * 0.35f;
		Vector vecArm = vecTarget - vecShoulder;
		if ( vecArm.NormalizeInPlace() < 0.01f )
			continue;
		vecElbow = Lerp( flAmount, vecElbow, vecShoulder + vecArm * flUpper * 0.97f );
		vecHand = Lerp( flAmount, vecHand, vecShoulder + vecArm * flReach );
	}
}
