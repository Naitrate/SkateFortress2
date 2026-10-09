//========= tf2-skate ============//
//
// Purpose: Skate 3 joints sent for retargeting onto TF2 class rigs. Order must
//          match RETARGET_JOINTS in skate-engine/crates/skate-game/src/sidecar/mod.rs.
//          Positions are in the skater root's frame (x forward, y left, z up),
//          in Hammer units.
//
//=============================================================================//
#ifndef TF_SKATE_SHARED_H
#define TF_SKATE_SHARED_H
#ifdef _WIN32
#pragma once
#endif

enum ESkateJoint
{
	SKATE_JOINT_HIPS = 0,
	SKATE_JOINT_SPINE,
	SKATE_JOINT_CHEST,			// SPINE3
	SKATE_JOINT_NECK,
	SKATE_JOINT_HEAD,
	SKATE_JOINT_LEFT_ARM,		// shoulder joint
	SKATE_JOINT_LEFT_FOREARM,	// elbow
	SKATE_JOINT_LEFT_HAND,		// wrist
	SKATE_JOINT_RIGHT_ARM,
	SKATE_JOINT_RIGHT_FOREARM,
	SKATE_JOINT_RIGHT_HAND,
	SKATE_JOINT_LEFT_UPLEG,		// hip joint
	SKATE_JOINT_LEFT_LEG,		// knee
	SKATE_JOINT_LEFT_FOOT,		// ankle
	SKATE_JOINT_LEFT_TOE,
	SKATE_JOINT_RIGHT_UPLEG,
	SKATE_JOINT_RIGHT_LEG,
	SKATE_JOINT_RIGHT_FOOT,
	SKATE_JOINT_RIGHT_TOE,

	SKATE_JOINT_COUNT
};

// Trick scoring flags (m_nSkateScoreFlags).
#define SKATE_SCORE_CLEAN		( 1 << 0 )	// last landing was clean
#define SKATE_SCORE_SKETCHY		( 1 << 1 )	// last landing was sketchy
#define SKATE_SCORE_ACTIVE		( 1 << 2 )	// a trick sequence is in progress

// Networked joint range (units from the skater root) and precision.
#define SKATE_JOINT_RANGE	128.0f
#define SKATE_JOINT_BITS	13

// Server-forced step flags (bails) the owner's predicted skater is told
// about: the last few, by usercmd number.
#define SKATE_FLAG_HISTORY	8

// Hall of Meat: the rider's physical bones, in the engine's PHYS_TPOSE order
// (bit n of the broken-bones mask is bone n; 0 is the board root).
#define SKATE_BONE_COUNT	24
#define SKATE_BONE_SKULL	1
#define SKATE_BONE_NECK		2
static const char *const g_pszSkateBoneNames[ SKATE_BONE_COUNT ] =
{
	"board", "skull", "neck", "left wrist", "left forearm", "left humerus", "left collarbone",
	"right wrist", "right forearm", "right humerus", "right collarbone", "upper back", "ribs",
	"spine", "lower back", "left toes", "left ankle", "left shin", "left femur", "right toes",
	"right ankle", "right shin", "right femur", "pelvis",
};

#endif // TF_SKATE_SHARED_H
