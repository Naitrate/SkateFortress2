//========= tf2-skate ============//
//
// Purpose: "Skate 3 setup" panel: pick the Skate 3 disc (ISO, default.xex or
//          extracted folder), then convert in-game. Everything it makes goes
//          into this mod folder (skate3_data/, sound/skate/, models/skate/,
//          materials/models/skate/), so there's nothing to configure; the disc
//          used is remembered in cl_skate_setup_source.
//
//          The conversion runs inside libskate3.so (skate3_setup_start /
//          skate3_setup_status, see skate-engine sidecar/setup), which reads
//          the disc directly and decodes the sounds with the bundled
//          libvgmstream.so. Files and folders are picked with Source's own
//          VGUI file dialog (as the spray importer uses), so nothing outside
//          the game is needed on any platform.
//
//          Opens by itself at the main menu until Skate 3 data is set up
//          (first run), and from the skate_setup command (Advanced Options >
//          Skate 3).
//
//=============================================================================//
#include "cbase.h"
#include <vgui_controls/Frame.h>
#include <vgui_controls/Label.h>
#include <vgui_controls/TextEntry.h>
#include <vgui_controls/Button.h>
#include <vgui_controls/ProgressBar.h>
#include <vgui_controls/FileOpenDialog.h>
#include <vgui/ISurface.h>
#include <vgui/IVGui.h>
#include "ienginevgui.h"
#include "filesystem.h"
#include "igamesystem.h"
#include "c_tf_skateboard.h"

#include "tf_skate_library.h"

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"


enum SkateSetupState_t
{
	SKATE_SETUP_IDLE = 0,
	SKATE_SETUP_RUNNING = 1,
	SKATE_SETUP_DONE = 2,
	SKATE_SETUP_FAILED = 3,
};

//-----------------------------------------------------------------------------
// libskate3.so / skate3.dll, beside this client module.
//-----------------------------------------------------------------------------
typedef int ( *SetupStartFn )( const char *, const char * );
typedef int ( *SetupStatusFn )( char *, size_t, float * );
static SetupStartFn s_pfnSetupStart;
static SetupStatusFn s_pfnSetupStatus;

static bool LoadSkateLibrary( char *pszError, int nErrorSize )
{
	if ( s_pfnSetupStart && s_pfnSetupStatus )
		return true;
	char szPath[ MAX_PATH ];
	void *pLibrary = SkateLibraryLoad( "skate3", szPath, sizeof( szPath ), pszError, nErrorSize );
	if ( !pLibrary )
		return false;
	s_pfnSetupStart = (SetupStartFn)SkateLibrarySymbol( pLibrary, "skate3_setup_start" );
	s_pfnSetupStatus = (SetupStatusFn)SkateLibrarySymbol( pLibrary, "skate3_setup_status" );
	if ( !s_pfnSetupStart || !s_pfnSetupStatus )
	{
		V_snprintf( pszError, nErrorSize, "%s is too old (no skate3_setup_*); rebuild it", szPath );
		return false;
	}
	return true;
}

// The disc the last conversion used, to prefill the panel (saved with the
// game's other settings in config.cfg).
static ConVar cl_skate_setup_source( "cl_skate_setup_source", "", FCVAR_ARCHIVE | FCVAR_DONTRECORD, "The Skate 3 disc the in-game setup last converted (prefills skate_setup)." );

// True when converted data is in the mod folder (what server.so loads).
static bool SkateDataReady()
{
	return g_pFullFileSystem->FileExists( "skate3_data/assets/private/game.json", "MOD" );
}

class CTFSkateSetupDialog : public vgui::Frame
{
	DECLARE_CLASS_SIMPLE( CTFSkateSetupDialog, vgui::Frame );
public:
	CTFSkateSetupDialog( vgui::VPANEL parent, bool bFirstRun );

	virtual void OnTick() OVERRIDE;
	virtual void OnCommand( const char *pszCommand ) OVERRIDE;
	virtual void PerformLayout() OVERRIDE;
	virtual void ApplySchemeSettings( vgui::IScheme *pScheme ) OVERRIDE;

private:
	void Browse();
	void StartConversion();
	MESSAGE_FUNC_PARAMS( OnFileSelected, "FileSelected", pParams );

	vgui::Label			*m_pIntro;
	vgui::Label			*m_pSourceLabel;
	vgui::TextEntry		*m_pSource;
	vgui::Button		*m_pBrowseSource;
	vgui::ProgressBar	*m_pProgress;
	vgui::Label			*m_pStatus;
	vgui::Button		*m_pConvert;
	vgui::Button		*m_pCloseButton;

	vgui::DHANDLE< vgui::FileOpenDialog > m_hFileDialog;
	int		m_nLastState;
	vgui::HFont	m_hFont;

	// Layout metrics, all derived from the font (fonts scale with the
	// screen resolution; fixed pixel sizes don't).
	int		Line() const;
	int		ButtonWide() const;
	int		IntroTall( int nWide ) const;
};

static vgui::DHANDLE< CTFSkateSetupDialog > g_hSkateSetupDialog;

CTFSkateSetupDialog::CTFSkateSetupDialog( vgui::VPANEL parent, bool bFirstRun )
	: BaseClass( NULL, "SkateSetupDialog" )
{
	SetParent( parent );
	// The menus' own SourceScheme (as the Options dialog uses), so fonts and
	// sizes match it at every resolution; load it sized to the menu panel
	// only if the menus haven't yet.
	vgui::HScheme hScheme = vgui::scheme()->GetScheme( "SourceScheme" );
	if ( !hScheme || hScheme == vgui::scheme()->GetDefaultScheme() )
	{
		hScheme = vgui::scheme()->LoadSchemeFromFileEx( parent, "resource/SourceScheme.res", "SourceScheme" );
	}
	SetScheme( hScheme );
	SetTitle( bFirstRun ? "Welcome to TF2 Skate: Skate 3 setup" : "Skate 3 setup", true );
	SetSizeable( false );
	SetMoveable( true );
	SetDeleteSelfOnClose( true );

	m_pIntro = new vgui::Label( this, "Intro",
		"TF2 Skate uses Skate 3's skater physics, animations, sounds and board from your own copy of the game. "
		"Pick your Xbox 360 Skate 3 disc image (ISO), its default.xex, or the extracted disc folder. "
		"The converted data (about 270 MB) goes into the mod folder. This takes a few seconds and only happens once." );
	m_pIntro->SetWrap( true );
	m_pIntro->SetContentAlignment( vgui::Label::a_northwest );
	m_pSourceLabel = new vgui::Label( this, "SourceLabel", "Skate 3 disc (ISO, default.xex or folder):" );
	m_pSource = new vgui::TextEntry( this, "Source" );
	m_pBrowseSource = new vgui::Button( this, "BrowseSource", "Browse...", this, "browse_source" );
	m_pProgress = new vgui::ProgressBar( this, "Progress" );
	m_pProgress->SetVisible( false );
	m_pStatus = new vgui::Label( this, "Status", "" );
	m_pStatus->SetWrap( true );
	m_pStatus->SetContentAlignment( vgui::Label::a_northwest );
	m_pConvert = new vgui::Button( this, "Convert", "Convert", this, "convert" );
	m_pCloseButton = new vgui::Button( this, "CloseButton", bFirstRun ? "Later" : "Close", this, "Close" );
	m_nLastState = -1;

	// Prefill the disc from the last conversion.
	m_pSource->SetText( cl_skate_setup_source.GetString() );
	m_pStatus->SetText( SkateDataReady() ? "Skate 3 data is set up. Convert again to redo it."
										 : "Skate 3 data isn't set up yet. Choose your disc and press Convert." );
	m_hFont = vgui::INVALID_FONT;

	vgui::ivgui()->AddTickSignal( GetVPanel(), 100 );
}

int CTFSkateSetupDialog::Line() const
{
	return MAX( 8, vgui::surface()->GetFontTall( m_hFont ) );
}

// Wide enough for the longest button label.
int CTFSkateSetupDialog::ButtonWide() const
{
	int nWidest = 0;
	for ( const char *pszLabel : { "Browse...", "Convert", "Later", "Close" } )
	{
		wchar_t wszLabel[ 32 ];
		V_UTF8ToUnicode( pszLabel, wszLabel, sizeof( wszLabel ) );
		int nWide, nTall;
		vgui::surface()->GetTextSize( m_hFont, wszLabel, nWide, nTall );
		nWidest = MAX( nWidest, nWide );
	}
	return nWidest + 2 * Line();
}

// Height of the wrapped intro paragraph at a given width.
int CTFSkateSetupDialog::IntroTall( int nWide ) const
{
	wchar_t wszText[ 1024 ];
	m_pIntro->GetText( wszText, sizeof( wszText ) );
	int nTextWide, nTextTall;
	vgui::surface()->GetTextSize( m_hFont, wszText, nTextWide, nTextTall );
	int nLines = nTextWide / MAX( 1, nWide - Line() ) + 2;	// word wrap slack
	return nLines * ( Line() + 2 );
}

void CTFSkateSetupDialog::ApplySchemeSettings( vgui::IScheme *pScheme )
{
	BaseClass::ApplySchemeSettings( pScheme );

	// The same small font as the Options dialog's lists, at this resolution.
	m_hFont = pScheme->GetFont( "DefaultSmall", IsProportional() );
	if ( m_hFont == vgui::INVALID_FONT )
		m_hFont = pScheme->GetFont( "Default", IsProportional() );
	vgui::Label *pLabels[] = { m_pIntro, m_pSourceLabel, m_pStatus, m_pBrowseSource, m_pConvert, m_pCloseButton };
	for ( vgui::Label *pLabel : pLabels )
	{
		pLabel->SetFont( m_hFont );
	}
	m_pSource->SetFont( m_hFont );

	// Size the frame to its contents: ~46 text lines wide (capped to the
	// screen), and as tall as the rows below the title bar need.
	int nScreenWide, nScreenTall;
	vgui::surface()->GetScreenSize( nScreenWide, nScreenTall );
	int nLine = Line(), nPad = nLine / 2;
	int nWide = MIN( nScreenWide * 9 / 10, nLine * 46 );
	SetSize( nWide, nScreenTall );	// provisional, to read the client area
	int x, y, w, h;
	GetClientArea( x, y, w, h );
	int nRow = nLine + nPad;
	int nContent = nPad + IntroTall( w - 2 * nPad ) + nPad	// intro
		+ nLine + nRow + nPad								// disc label + entry
		+ nLine + nPad										// progress
		+ 3 * ( nLine + 2 ) + nPad							// status
		+ nRow + nPad;										// buttons
	SetSize( nWide, MIN( nScreenTall * 9 / 10, y + nContent + nPad ) );
	MoveToCenterOfScreen();
}

void CTFSkateSetupDialog::PerformLayout()
{
	BaseClass::PerformLayout();
	if ( m_hFont == vgui::INVALID_FONT )
		return;
	int x, y, w, h;
	GetClientArea( x, y, w, h );
	const int nLine = Line(), nPad = nLine / 2, nRow = nLine + nPad, nButton = ButtonWide();
	const int nLeft = x + nPad, nInner = w - 2 * nPad;
	int nY = y + nPad;

	int nIntro = IntroTall( nInner );
	m_pIntro->SetBounds( nLeft, nY, nInner, nIntro );
	nY += nIntro + nPad;
	m_pSourceLabel->SetBounds( nLeft, nY, nInner, nLine + 2 );
	nY += nLine + 2;
	m_pSource->SetBounds( nLeft, nY, nInner - nButton - nPad, nRow );
	m_pBrowseSource->SetBounds( nLeft + nInner - nButton, nY, nButton, nRow );
	nY += nRow + nPad;
	m_pProgress->SetBounds( nLeft, nY, nInner, nLine );
	nY += nLine + nPad;

	int nButtonsY = y + h - nPad - nRow;
	m_pStatus->SetBounds( nLeft, nY, nInner, MAX( nLine, nButtonsY - nPad - nY ) );
	m_pCloseButton->SetBounds( nLeft + nInner - nButton, nButtonsY, nButton, nRow );
	m_pConvert->SetBounds( nLeft + nInner - 2 * nButton - nPad, nButtonsY, nButton, nRow );
}

// Opens Source's file dialog at the current choice's folder (else home).
void CTFSkateSetupDialog::Browse()
{
	if ( m_hFileDialog.Get() )
	{
		m_hFileDialog->MarkForDeletion();
	}
	vgui::FileOpenDialog *pDialog = new vgui::FileOpenDialog( this, "Choose your Skate 3 disc (ISO or default.xex)", vgui::FOD_OPEN );
	pDialog->AddFilter( "*.iso;*.ISO;default.xex", "Skate 3 disc (*.iso, default.xex)", true );
	pDialog->AddFilter( "*.*", "All files (*.*)", false );

	char szStart[ MAX_PATH ];
	m_pSource->GetText( szStart, sizeof( szStart ) );
	if ( szStart[0] )
	{
		V_StripFilename( szStart );
	}
	if ( !szStart[0] )
	{
#ifdef _WIN32
		V_strncpy( szStart, "C:\\", sizeof( szStart ) );
#else
		V_strncpy( szStart, getenv( "HOME" ) ? getenv( "HOME" ) : "/", sizeof( szStart ) );
#endif
	}
	pDialog->SetStartDirectory( szStart );
	pDialog->AddActionSignalTarget( this );
	pDialog->DoModal( false );
	pDialog->Activate();
	m_hFileDialog = pDialog;
}

void CTFSkateSetupDialog::OnFileSelected( KeyValues *pParams )
{
	const char *pszPath = pParams->GetString( "fullpath" );
	if ( *pszPath )
	{
		m_pSource->SetText( pszPath );
	}
}

void CTFSkateSetupDialog::StartConversion()
{
	char szError[ 512 ];
	if ( !LoadSkateLibrary( szError, sizeof( szError ) ) )
	{
		m_pStatus->SetText( szError );
		return;
	}
	char szSource[ MAX_PATH ];
	m_pSource->GetText( szSource, sizeof( szSource ) );
	if ( !szSource[0] )
	{
		m_pStatus->SetText( "Choose your Skate 3 disc first." );
		return;
	}
	if ( s_pfnSetupStart( szSource, engine->GetGameDirectory() ) != 0 )
	{
		m_pStatus->SetText( "A conversion is already running." );
		return;
	}
	cl_skate_setup_source.SetValue( szSource );
	m_pProgress->SetProgress( 0.0f );
	m_pProgress->SetVisible( true );
	m_pStatus->SetText( "Starting..." );
}

void CTFSkateSetupDialog::OnCommand( const char *pszCommand )
{
	if ( !V_stricmp( pszCommand, "browse_source" ) )
	{
		Browse();
		return;
	}
	if ( !V_stricmp( pszCommand, "convert" ) )
	{
		StartConversion();
		return;
	}
	BaseClass::OnCommand( pszCommand );
}

void CTFSkateSetupDialog::OnTick()
{
	BaseClass::OnTick();
	if ( !s_pfnSetupStatus )
		return;

	char szMessage[ 1024 ];
	float flProgress = 0.0f;
	int nState = s_pfnSetupStatus( szMessage, sizeof( szMessage ), &flProgress );
	bool bRunning = nState == SKATE_SETUP_RUNNING;
	m_pBrowseSource->SetEnabled( !bRunning );
	m_pConvert->SetEnabled( !bRunning );
	m_pCloseButton->SetEnabled( !bRunning );
	if ( nState == SKATE_SETUP_IDLE )
		return;

	m_pProgress->SetVisible( true );
	m_pProgress->SetProgress( flProgress );
	if ( nState == SKATE_SETUP_DONE )
	{
		char szText[ 1280 ];
		V_snprintf( szText, sizeof( szText ), "Ready! %s\nStart a map and press K (BACK on a controller) to skate.", szMessage );
		m_pStatus->SetText( szText );
		m_pCloseButton->SetText( "Close" );
		if ( m_nLastState != SKATE_SETUP_DONE && engine->IsInGame() )
		{
			// Pick up the new sounds and board right away if a map is loaded.
			C_TFSkateboard::LevelInitAssets();
		}
	}
	else if ( nState == SKATE_SETUP_FAILED )
	{
		char szText[ 1280 ];
		V_snprintf( szText, sizeof( szText ), "Setup failed: %s", szMessage );
		m_pStatus->SetText( szText );
	}
	else
	{
		m_pStatus->SetText( szMessage );
	}
	m_nLastState = nState;
}

static void OpenSkateSetupDialog( bool bFirstRun )
{
	if ( !g_hSkateSetupDialog.Get() )
	{
		g_hSkateSetupDialog = new CTFSkateSetupDialog( enginevgui->GetPanel( PANEL_GAMEUIDLL ), bFirstRun );
	}
	g_hSkateSetupDialog->Activate();
}

CON_COMMAND( skate_setup, "Open the Skate 3 setup panel (convert your Skate 3 disc)." )
{
	OpenSkateSetupDialog( false );
}

//-----------------------------------------------------------------------------
// First run: once the main menu has settled, open the panel if no Skate 3
// data is set up yet.
//-----------------------------------------------------------------------------
class CSkateSetupPrompt : public CAutoGameSystemPerFrame
{
public:
	CSkateSetupPrompt() : CAutoGameSystemPerFrame( "CSkateSetupPrompt" ), m_flStart( -1.0 ), m_bDone( false ) {}

	virtual void Update( float frametime ) OVERRIDE
	{
		if ( m_bDone )
			return;
		if ( m_flStart < 0.0 )
			m_flStart = Plat_FloatTime();
		if ( Plat_FloatTime() - m_flStart < 2.0 || engine->IsInGame() || engine->IsConnected() )
			return;
		m_bDone = true;
		if ( !SkateDataReady() )
		{
			OpenSkateSetupDialog( true );
		}
	}

private:
	double	m_flStart;
	bool	m_bDone;
};
static CSkateSetupPrompt s_SkateSetupPrompt;
