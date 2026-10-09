//========= tf2-skate ============//
//
// Purpose: Remappable Skate 3 controls (keyboard/mouse and controller), kept
//          apart from TF2's binds. See tf_skate_controls.cpp.
//
//=============================================================================//
#ifndef TF_SKATE_CONTROLS_H
#define TF_SKATE_CONTROLS_H
#ifdef _WIN32
#pragma once
#endif

class CUserCmd;

// While skating: replace the skate buttons and the lean/steer movement of
// `cmd` with the skate keys' state (keyboard and mouse).
void SkateControlsApplyKeys( CUserCmd *cmd );

// The IN_* bits of the skate actions currently held on the controller, and
// every skate action bit (to clear first).
int SkateControlsPadButtons();
int SkateControlsAllButtons();

#endif // TF_SKATE_CONTROLS_H
