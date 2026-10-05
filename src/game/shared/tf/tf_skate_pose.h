//========= tf2-skate ============//
//
// Purpose: Skate 3 pose retargeting onto TF2 class skeletons (client and
//          server). See tf_skate_pose.cpp.
//
//=============================================================================//
#ifndef TF_SKATE_POSE_H
#define TF_SKATE_POSE_H
#ifdef _WIN32
#pragma once
#endif

#include "tf_skate_shared.h"

class CStudioHdr;
struct studiohdr_t;

// Bone indices of the rig bones for one model, looked up once.
struct SkateRigCache_t
{
	SkateRigCache_t() : pModel( NULL ) {}
	const studiohdr_t	*pModel;
	int					nBones[ 32 ];
};

// Poses `pBones` (world space, hdr->numbones() of them, as TF2 animated them)
// like the skater: `pJoints` are the SKATE_JOINT_COUNT joints in the root
// frame `rootToWorld`. Only bones in `boneMask` are written. False if there
// is no pose yet or the model isn't a TF2 class rig.
bool SkateRetargetBones( CStudioHdr *hdr, matrix3x4_t *pBones, const Vector *pJoints, const matrix3x4_t &rootToWorld, SkateRigCache_t &cache, int boneMask );

#endif // TF_SKATE_POSE_H
