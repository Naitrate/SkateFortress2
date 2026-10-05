//========= tf2-skate ============//
//
// Purpose: Remappable Skate 3 controls.
//
//          Skating used to ride on TF2's own commands (+jump pushed, +use
//          stepped off...), so remapping one changed the other. Each skate
//          action now has its own key and controller button, stored in
//          archived cvars (cl_skate_key_<action>, cl_skate_pad_<action>) and
//          read straight from the input system while skating. The usercmd
//          still carries them as the IN_* bits the sidecar's virtual pad
//          expects (skate-engine sidecar/pad.rs).
//
//          "Skate 3 controls" dialog: skate_controls, or Options > Advanced
//          > Skate 3. Click a slot, then press the key / mouse button /
//          controller button; Esc cancels, Backspace clears.
//
//=============================================================================//
#include "cbase.h"
#include "tf_skate_controls.h"
#include "usercmd.h"
#include "in_buttons.h"
#include "inputsystem/iinputsystem.h"
#include <vgui_controls/Frame.h>
#include <vgui_controls/Label.h>
#include <vgui_controls/Button.h>
#include <vgui/ISurface.h>
#include <vgui/IVGui.h>
#include <vgui/IInput.h>
#include <vgui/IPanel.h>
#include "ienginevgui.h"

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

struct SkateAction_t
{
	const char		*pszLabel;
	ConVar			*pKey;		// keyboard / mouse
	ButtonCode_t	nKeyDefault;
	ConVar			*pPad;		// controller button, or NULL (stick actions)
	ButtonCode_t	nPadDefault;
	int				nInButton;	// IN_* bit, or 0 for the left stick
	int				nAxis;		// 0 none, 1 lean (forward), 2 steer (side)
	float			flSign;
};

#define SKATE_KEY( name, def, help )	static ConVar cl_skate_key_##name( "cl_skate_key_" #name, def, FCVAR_ARCHIVE, "Skate 3 control (keyboard/mouse): " help )
#define SKATE_PAD( name, def, help )	static ConVar cl_skate_pad_##name( "cl_skate_pad_" #name, def, FCVAR_ARCHIVE, "Skate 3 control (controller): " help )

SKATE_KEY( lean_forward, "W", "lean forward (left stick up)" );
SKATE_KEY( lean_back, "S", "lean back (left stick down)" );
SKATE_KEY( steer_left, "A", "steer left (left stick left)" );
SKATE_KEY( steer_right, "D", "steer right (left stick right)" );
SKATE_KEY( push, "SPACE", "push (A)" );
SKATE_KEY( x, "CTRL", "X" );
SKATE_KEY( brake, "R", "brake / powerslide (B)" );
SKATE_KEY( board, "E", "step off / on the board (Y)" );
SKATE_KEY( grab_left, "MOUSE1", "left grab (left trigger)" );
SKATE_KEY( grab_right, "MOUSE2", "right grab (right trigger)" );
SKATE_KEY( lb, "SHIFT", "left bumper" );
SKATE_KEY( rb, "MOUSE3", "right bumper" );
SKATE_KEY( l3, "ALT", "left stick click" );
SKATE_PAD( push, "A_BUTTON", "push (A)" );
SKATE_PAD( x, "X_BUTTON", "X" );
SKATE_PAD( brake, "B_BUTTON", "brake / powerslide (B)" );
SKATE_PAD( board, "Y_BUTTON", "step off / on the board (Y)" );
SKATE_PAD( grab_left, "Z AXIS POS", "left grab (left trigger)" );
SKATE_PAD( grab_right, "Z AXIS NEG", "right grab (right trigger)" );
SKATE_PAD( lb, "L_SHOULDER", "left bumper" );
SKATE_PAD( rb, "R_SHOULDER", "right bumper" );
SKATE_PAD( l3, "STICK2", "left stick click" );

// The flick stick (mouse as Skate's right stick). Sent to the server as
// userinfo; the simulation applies them to this player only.
static ConVar cl_skate_mouse_gain( "cl_skate_mouse_gain", "0.02", FCVAR_ARCHIVE | FCVAR_USERINFO, "Skate flick stick: stick deflection per mouse count (higher = shorter flicks).", true, 0.002f, true, 0.2f );
static ConVar cl_skate_mouse_decay( "cl_skate_mouse_decay", "0.7", FCVAR_ARCHIVE | FCVAR_USERINFO, "Skate flick stick: how much deflection carries into the next tick (higher = smoother, slower to recentre).", true, 0.0f, true, 0.95f );

static SkateAction_t s_Actions[] =
{
	{ "Lean forward",				&cl_skate_key_lean_forward,	KEY_W,		NULL,							BUTTON_CODE_INVALID,			0,			1,  1.0f },
	{ "Lean back",					&cl_skate_key_lean_back,	KEY_S,		NULL,							BUTTON_CODE_INVALID,			0,			1, -1.0f },
	{ "Steer left",					&cl_skate_key_steer_left,	KEY_A,		NULL,							BUTTON_CODE_INVALID,			0,			2, -1.0f },
	{ "Steer right",				&cl_skate_key_steer_right,	KEY_D,		NULL,							BUTTON_CODE_INVALID,			0,			2,  1.0f },
	{ "Push (A)",					&cl_skate_key_push,			KEY_SPACE,	&cl_skate_pad_push,				KEY_XBUTTON_A,					IN_JUMP,	0, 0 },
	{ "Brake / powerslide (B)",		&cl_skate_key_brake,		KEY_R,		&cl_skate_pad_brake,			KEY_XBUTTON_B,					IN_RELOAD,	0, 0 },
	{ "X",							&cl_skate_key_x,			KEY_LCONTROL, &cl_skate_pad_x,				KEY_XBUTTON_X,					IN_DUCK,	0, 0 },
	{ "Step off / on board (Y)",	&cl_skate_key_board,		KEY_E,		&cl_skate_pad_board,			KEY_XBUTTON_Y,					IN_USE,		0, 0 },
	{ "Left grab (LT)",				&cl_skate_key_grab_left,	MOUSE_LEFT,	&cl_skate_pad_grab_left,		KEY_XBUTTON_LTRIGGER,			IN_ATTACK,	0, 0 },
	{ "Right grab (RT)",			&cl_skate_key_grab_right,	MOUSE_RIGHT, &cl_skate_pad_grab_right,		KEY_XBUTTON_RTRIGGER,			IN_ATTACK2,	0, 0 },
	{ "Left bumper (LB)",			&cl_skate_key_lb,			KEY_LSHIFT,	&cl_skate_pad_lb,				KEY_XBUTTON_LEFT_SHOULDER,		IN_SPEED,	0, 0 },
	{ "Right bumper (RB)",			&cl_skate_key_rb,			MOUSE_MIDDLE, &cl_skate_pad_rb,				KEY_XBUTTON_RIGHT_SHOULDER,		IN_ATTACK3,	0, 0 },
	{ "Left stick click (L3)",		&cl_skate_key_l3,			KEY_LALT,	&cl_skate_pad_l3,				KEY_XBUTTON_STICK2,				IN_WALK,	0, 0 },
};

// A control's button: its cvar, the built-in default for a name the input
// system doesn't know, or nothing when cleared ("").
static ButtonCode_t ResolveButton( ConVar *pVar, ButtonCode_t nDefault )
{
	if ( !pVar )
		return BUTTON_CODE_INVALID;
	const char *pszName = pVar->GetString();
	if ( !pszName[0] )
		return BUTTON_CODE_INVALID;
	ButtonCode_t code = g_pInputSystem ? g_pInputSystem->StringToButtonCode( pszName ) : BUTTON_CODE_INVALID;
	if ( code == BUTTON_CODE_INVALID && !V_stricmp( pszName, pVar->GetDefault() ) )
		code = nDefault;
	return code;
}

static bool IsDown( ButtonCode_t code )
{
	return code != BUTTON_CODE_INVALID && g_pInputSystem && g_pInputSystem->IsButtonDown( code );
}

// Skate keys only count while playing: not over the console, menus, or a
// text box with focus (chat).
static bool KeysAreForTheGame()
{
	if ( !engine->IsActiveApp() || engine->Con_IsVisible() || enginevgui->IsGameUIVisible() || vgui::surface()->IsCursorVisible() )
		return false;
	vgui::VPANEL focus = vgui::input()->GetFocus();
	return !focus || !V_stristr( vgui::ipanel()->GetClassName( focus ), "Entry" );
}

int SkateControlsAllButtons()
{
	int nAll = 0;
	for ( int i = 0; i < ARRAYSIZE( s_Actions ); ++i )
		nAll |= s_Actions[i].nInButton;
	return nAll;
}

int SkateControlsPadButtons()
{
	int nButtons = 0;
	for ( int i = 0; i < ARRAYSIZE( s_Actions ); ++i )
	{
		if ( IsDown( ResolveButton( s_Actions[i].pPad, s_Actions[i].nPadDefault ) ) )
			nButtons |= s_Actions[i].nInButton;
	}
	return nButtons;
}

void SkateControlsApplyKeys( CUserCmd *cmd )
{
	int nButtons = 0;
	float flAxes[3] = { 0, 0, 0 };
	if ( KeysAreForTheGame() )
	{
		for ( int i = 0; i < ARRAYSIZE( s_Actions ); ++i )
		{
			const SkateAction_t &action = s_Actions[i];
			if ( !IsDown( ResolveButton( action.pKey, action.nKeyDefault ) ) )
				continue;
			nButtons |= action.nInButton;
			flAxes[ action.nAxis ] += action.flSign;
		}
	}
	// TF2's binds for these bits (+jump, +use...) don't skate any more.
	cmd->buttons = ( cmd->buttons & ~SkateControlsAllButtons() ) | nButtons;
	cmd->forwardmove = clamp( flAxes[1], -1.0f, 1.0f ) * 450.0f;
	cmd->sidemove = clamp( flAxes[2], -1.0f, 1.0f ) * 450.0f;
}

//-----------------------------------------------------------------------------
// "Skate 3 controls" dialog.
//-----------------------------------------------------------------------------
class CTFSkateControlsDialog : public vgui::Frame
{
	DECLARE_CLASS_SIMPLE( CTFSkateControlsDialog, vgui::Frame );
public:
	CTFSkateControlsDialog( vgui::VPANEL parent );

	virtual void OnTick() OVERRIDE;
	virtual void OnCommand( const char *pszCommand ) OVERRIDE;
	virtual void PerformLayout() OVERRIDE;
	virtual void ApplySchemeSettings( vgui::IScheme *pScheme ) OVERRIDE;

private:
	void Refresh();
	void StopCapture( const char *pszStatus );

	vgui::Label		*m_pHeaderAction;
	vgui::Label		*m_pHeaderKey;
	vgui::Label		*m_pHeaderPad;
	vgui::Label		*m_pLabels[ ARRAYSIZE( s_Actions ) ];
	vgui::Button	*m_pKeys[ ARRAYSIZE( s_Actions ) ];
	vgui::Button	*m_pPads[ ARRAYSIZE( s_Actions ) ];
	vgui::Label		*m_pStatus;
	vgui::Button	*m_pDefaults;
	vgui::Button	*m_pCloseButton;
	vgui::HFont		m_hFont;

	int		m_nCapture;			// action index being assigned, or -1
	bool	m_bCapturePad;
	bool	m_bWaitForRelease;	// ignore the click that started capturing
};

static vgui::DHANDLE< CTFSkateControlsDialog > g_hSkateControlsDialog;

CTFSkateControlsDialog::CTFSkateControlsDialog( vgui::VPANEL parent )
	: BaseClass( NULL, "SkateControlsDialog" )
{
	SetParent( parent );
	vgui::HScheme hScheme = vgui::scheme()->GetScheme( "SourceScheme" );
	if ( !hScheme || hScheme == vgui::scheme()->GetDefaultScheme() )
		hScheme = vgui::scheme()->LoadSchemeFromFileEx( parent, "resource/SourceScheme.res", "SourceScheme" );
	SetScheme( hScheme );
	SetTitle( "Skate 3 controls", true );
	SetSizeable( false );
	SetMoveable( true );
	SetDeleteSelfOnClose( true );

	m_pHeaderAction = new vgui::Label( this, "HeaderAction", "Skate action" );
	m_pHeaderKey = new vgui::Label( this, "HeaderKey", "Keyboard / mouse" );
	m_pHeaderPad = new vgui::Label( this, "HeaderPad", "Controller" );
	for ( int i = 0; i < ARRAYSIZE( s_Actions ); ++i )
	{
		m_pLabels[i] = new vgui::Label( this, "Action", s_Actions[i].pszLabel );
		m_pKeys[i] = new vgui::Button( this, "Key", "", this, CFmtStr( "key %d", i ) );
		m_pPads[i] = new vgui::Button( this, "Pad", "", this, CFmtStr( "pad %d", i ) );
		m_pPads[i]->SetVisible( s_Actions[i].pPad != NULL );
	}
	m_pStatus = new vgui::Label( this, "Status", "Click a slot, then press the key or button to use. The mouse still flicks tricks." );
	m_pDefaults = new vgui::Button( this, "Defaults", "Reset to defaults", this, "defaults" );
	m_pCloseButton = new vgui::Button( this, "CloseButton", "Close", this, "Close" );
	m_hFont = vgui::INVALID_FONT;
	m_nCapture = -1;
	m_bCapturePad = false;
	m_bWaitForRelease = false;
	Refresh();
	vgui::ivgui()->AddTickSignal( GetVPanel(), 15 );
}

void CTFSkateControlsDialog::Refresh()
{
	for ( int i = 0; i < ARRAYSIZE( s_Actions ); ++i )
	{
		const SkateAction_t &action = s_Actions[i];
		ButtonCode_t key = ResolveButton( action.pKey, action.nKeyDefault );
		ButtonCode_t pad = ResolveButton( action.pPad, action.nPadDefault );
		bool bKey = m_nCapture == i && !m_bCapturePad, bPad = m_nCapture == i && m_bCapturePad;
		m_pKeys[i]->SetText( bKey ? "press a key..." : key == BUTTON_CODE_INVALID ? "(none)" : g_pInputSystem->ButtonCodeToString( key ) );
		m_pPads[i]->SetText( bPad ? "press a button..." : pad == BUTTON_CODE_INVALID ? "(none)" : g_pInputSystem->ButtonCodeToString( pad ) );
	}
}

void CTFSkateControlsDialog::StopCapture( const char *pszStatus )
{
	m_nCapture = -1;
	m_pStatus->SetText( pszStatus );
	Refresh();
}

void CTFSkateControlsDialog::OnCommand( const char *pszCommand )
{
	int nIndex;
	if ( sscanf( pszCommand, "key %d", &nIndex ) == 1 || sscanf( pszCommand, "pad %d", &nIndex ) == 1 )
	{
		m_nCapture = clamp( nIndex, 0, ARRAYSIZE( s_Actions ) - 1 );
		m_bCapturePad = pszCommand[0] == 'p';
		m_bWaitForRelease = true;
		m_pStatus->SetText( m_bCapturePad ? "Press a controller button (Esc cancels, Backspace clears)."
										  : "Press a key or mouse button (Esc cancels, Backspace clears)." );
		Refresh();
		return;
	}
	if ( !V_stricmp( pszCommand, "defaults" ) )
	{
		for ( int i = 0; i < ARRAYSIZE( s_Actions ); ++i )
		{
			s_Actions[i].pKey->Revert();
			if ( s_Actions[i].pPad )
				s_Actions[i].pPad->Revert();
		}
		StopCapture( "Back to the default controls." );
		return;
	}
	BaseClass::OnCommand( pszCommand );
}

// While assigning, watch the input system for the next button pressed.
void CTFSkateControlsDialog::OnTick()
{
	BaseClass::OnTick();
	if ( m_nCapture < 0 || !g_pInputSystem )
		return;

	ButtonCode_t first = m_bCapturePad ? JOYSTICK_FIRST : KEY_FIRST;
	ButtonCode_t last = m_bCapturePad ? JOYSTICK_LAST : MOUSE_LAST;
	ButtonCode_t pressed = BUTTON_CODE_INVALID;
	bool bAnyDown = false;
	for ( int code = first; code <= last; ++code )
	{
		if ( code == KEY_NONE || code == MOUSE_WHEEL_UP || code == MOUSE_WHEEL_DOWN )
			continue;
		if ( g_pInputSystem->IsButtonDown( (ButtonCode_t)code ) )
		{
			bAnyDown = true;
			if ( pressed == BUTTON_CODE_INVALID )
				pressed = (ButtonCode_t)code;
		}
	}
	// Escape and Backspace work in both columns.
	bool bEscape = g_pInputSystem->IsButtonDown( KEY_ESCAPE );
	bool bClear = g_pInputSystem->IsButtonDown( KEY_BACKSPACE ) || g_pInputSystem->IsButtonDown( KEY_DELETE );
	if ( m_bWaitForRelease )
	{
		if ( !bAnyDown && !bEscape && !bClear )
			m_bWaitForRelease = false;
		return;
	}
	const SkateAction_t &action = s_Actions[ m_nCapture ];
	ConVar *pVar = m_bCapturePad ? action.pPad : action.pKey;
	if ( bEscape )
	{
		StopCapture( "Cancelled." );
	}
	else if ( bClear )
	{
		pVar->SetValue( "" );
		StopCapture( CFmtStr( "%s: cleared.", action.pszLabel ) );
	}
	else if ( pressed != BUTTON_CODE_INVALID )
	{
		pVar->SetValue( g_pInputSystem->ButtonCodeToString( pressed ) );
		StopCapture( CFmtStr( "%s: %s.", action.pszLabel, g_pInputSystem->ButtonCodeToString( pressed ) ) );
	}
}

void CTFSkateControlsDialog::ApplySchemeSettings( vgui::IScheme *pScheme )
{
	BaseClass::ApplySchemeSettings( pScheme );
	m_hFont = pScheme->GetFont( "DefaultSmall", IsProportional() );
	if ( m_hFont == vgui::INVALID_FONT )
		m_hFont = pScheme->GetFont( "Default", IsProportional() );
	m_pHeaderAction->SetFont( m_hFont );
	m_pHeaderKey->SetFont( m_hFont );
	m_pHeaderPad->SetFont( m_hFont );
	m_pStatus->SetFont( m_hFont );
	m_pDefaults->SetFont( m_hFont );
	m_pCloseButton->SetFont( m_hFont );
	for ( int i = 0; i < ARRAYSIZE( s_Actions ); ++i )
	{
		m_pLabels[i]->SetFont( m_hFont );
		m_pKeys[i]->SetFont( m_hFont );
		m_pPads[i]->SetFont( m_hFont );
	}

	// Sized from the font: a header, one row per action, status and buttons.
	int nScreenWide, nScreenTall;
	vgui::surface()->GetScreenSize( nScreenWide, nScreenTall );
	int nLine = MAX( 8, vgui::surface()->GetFontTall( m_hFont ) ), nPad = nLine / 2, nRow = nLine + nPad;
	int nWide = MIN( nScreenWide * 9 / 10, nLine * 34 );
	SetSize( nWide, nScreenTall );
	int x, y, w, h;
	GetClientArea( x, y, w, h );
	int nContent = nPad + nLine + nPad + ARRAYSIZE( s_Actions ) * ( nRow + 2 ) + nPad + 2 * ( nLine + 2 ) + nPad + nRow + nPad;
	SetSize( nWide, MIN( nScreenTall * 9 / 10, y + nContent + nPad ) );
	MoveToCenterOfScreen();
}

void CTFSkateControlsDialog::PerformLayout()
{
	BaseClass::PerformLayout();
	if ( m_hFont == vgui::INVALID_FONT )
		return;
	int x, y, w, h;
	GetClientArea( x, y, w, h );
	const int nLine = MAX( 8, vgui::surface()->GetFontTall( m_hFont ) ), nPad = nLine / 2, nRow = nLine + nPad;
	const int nLeft = x + nPad, nInner = w - 2 * nPad;
	const int nColumn = ( nInner - nPad ) * 3 / 10;		// key and pad columns
	const int nLabelWide = nInner - 2 * nColumn - 2 * nPad;
	const int nKeyX = nLeft + nLabelWide + nPad, nPadX = nKeyX + nColumn + nPad;
	int nY = y + nPad;
	m_pHeaderAction->SetBounds( nLeft, nY, nLabelWide, nLine + 2 );
	m_pHeaderKey->SetBounds( nKeyX, nY, nColumn, nLine + 2 );
	m_pHeaderPad->SetBounds( nPadX, nY, nColumn, nLine + 2 );
	nY += nLine + nPad;
	for ( int i = 0; i < ARRAYSIZE( s_Actions ); ++i )
	{
		m_pLabels[i]->SetBounds( nLeft, nY, nLabelWide, nRow );
		m_pKeys[i]->SetBounds( nKeyX, nY, nColumn, nRow );
		m_pPads[i]->SetBounds( nPadX, nY, nColumn, nRow );
		nY += nRow + 2;
	}
	int nButtonsY = y + h - nPad - nRow;
	m_pStatus->SetBounds( nLeft, nY + nPad, nInner, MAX( nLine, nButtonsY - nY - 2 * nPad ) );
	m_pStatus->SetWrap( true );
	m_pStatus->SetContentAlignment( vgui::Label::a_northwest );
	m_pCloseButton->SetBounds( nLeft + nInner - nColumn / 2 - nPad, nButtonsY, nColumn / 2 + nPad, nRow );
	m_pDefaults->SetBounds( nLeft, nButtonsY, nColumn, nRow );
}

CON_COMMAND( skate_controls, "Remap the Skate 3 controls (keyboard, mouse and controller)." )
{
	if ( !g_hSkateControlsDialog.Get() )
	{
		g_hSkateControlsDialog = new CTFSkateControlsDialog( enginevgui->GetPanel( PANEL_GAMEUIDLL ) );
	}
	g_hSkateControlsDialog->Activate();
}
