//========= tf2-skate ============//
//
// Purpose: Skateboard driving for bots (nav mesh routes, steering, tricks).
//
//=============================================================================//
#ifndef TF_BOT_SKATE_H
#define TF_BOT_SKATE_H
#ifdef _WIN32
#pragma once
#endif

class CTFPlayer;
struct SkateInput_t;

// Forget the route and tricks, e.g. on a new skater.
void TFBotSkateReset( CTFPlayer *pBot );
// Replace a skating bot's usercmd-derived input with the driver's.
void TFBotSkateInput( CTFPlayer *pBot, SkateInput_t &input );
// Every frame: remounts a bot that hopped off to get unstuck.
void TFBotSkateThink( CTFPlayer *pBot );

#endif // TF_BOT_SKATE_H
