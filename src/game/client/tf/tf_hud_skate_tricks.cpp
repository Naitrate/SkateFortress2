//========= tf2-skate ============//
//
// Purpose: Skate 3 style trick feed. Shows the tricks of the current line with
//          their points, the line score and multiplier, how the line ended
//          (landed clean/sketchy or bailed), and the banked total. All scoring
//          comes from Skate's own ScoreModule in the sidecar. Also air stats
//          (time, distance, height) measured here from the skater's state,
//          and the current speed.
//
//=============================================================================//
#include "cbase.h"
#include "hudelement.h"
#include "iclientmode.h"
#include "c_tf_player.h"
#include "tf_skate_shared.h"
#include <vgui/ISurface.h>
#include <vgui/ILocalize.h>
#include <vgui_controls/Panel.h>

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

ConVar cl_skate_hud( "cl_skate_hud", "1", FCVAR_ARCHIVE, "Show the Skate trick and score feed." );
ConVar cl_skate_hud_air( "cl_skate_hud_air", "1", FCVAR_ARCHIVE, "Show air time, distance and height, and the current speed, while skating." );
ConVar cl_skate_hud_units( "cl_skate_hud_units", "0", FCVAR_ARCHIVE, "Skate HUD units: 0 metres and km/h, 1 feet and mph." );

static const float SKATE_HUD_AIR_TIME = 3.0f;
static const float SKATE_HUD_MIN_AIR = 0.25f;	// shorter hops aren't worth a line

static const float SKATE_HUD_RESULT_TIME = 2.5f;
static const float SKATE_HUD_ENTRY_FADE = 6.0f;
static const int SKATE_HUD_MAX_ENTRIES = 6;

class CHudSkateTricks : public CHudElement, public vgui::Panel
{
	DECLARE_CLASS_SIMPLE( CHudSkateTricks, vgui::Panel );
public:
	CHudSkateTricks( const char *pElementName );

	virtual bool ShouldDraw() OVERRIDE;
	virtual void ApplySchemeSettings( vgui::IScheme *pScheme ) OVERRIDE;
	virtual void OnThink() OVERRIDE;
	virtual void Paint() OVERRIDE;

private:
	struct Entry_t
	{
		wchar_t	szName[ 64 ];
		int		nScore;
		float	flTime;
	};

	void Track( C_TFPlayer *pPlayer );
	void DrawText( vgui::HFont font, int x, int y, const wchar_t *pszText, Color color, bool bCenter );

	CUtlVector< Entry_t >	m_Entries;
	int		m_nLastSeq;
	int		m_nLastLine;
	int		m_nLineMultiplied;
	float	m_flLastMultiplier;
	int		m_nLastTotal;
	float	m_flResultTime;
	bool	m_bResultBailed;
	int		m_nResultPoints;
	int		m_nResultFlags;
	bool	m_bTracking;

	// Air: from takeoff to landing (native states 200..299 are airborne).
	bool	m_bAirborne;
	float	m_flAirStart;
	Vector	m_vecAirStart;
	float	m_flAirPeak;		// highest z this air
	float	m_flAirResultTime;	// when the last air landed
	float	m_flAirSeconds, m_flAirDistance, m_flAirHeight, m_flAirDrop;
	bool	m_bAirBest;
	float	m_flBestAirSeconds;

	void TrackAir( C_TFPlayer *pPlayer );
	void FormatLength( float flUnits, wchar_t *pszOut, int nOutChars );

	// Hall of Meat: the latest bail's score and broken bones, shown until a
	// few seconds after its last change.
	void TrackMeat( C_TFPlayer *pPlayer );
	void PaintMeat( int w, int h );
	int		m_nMeatBail;
	int		m_nMeatScore;
	int		m_nMeatBones;
	float	m_flMeatTime;

	vgui::HFont m_hNameFont;
	vgui::HFont m_hLineFont;
	vgui::HFont m_hSmallFont;
};

DECLARE_HUDELEMENT( CHudSkateTricks );

CHudSkateTricks::CHudSkateTricks( const char *pElementName )
	: CHudElement( pElementName ), BaseClass( NULL, "HudSkateTricks" )
{
	SetParent( g_pClientMode->GetViewport() );
	SetHiddenBits( HIDEHUD_MISCSTATUS );
	m_nLastSeq = -1;
	m_nLastLine = 0;
	m_nLineMultiplied = 0;
	m_flLastMultiplier = 1.0f;
	m_nLastTotal = 0;
	m_flResultTime = -100.0f;
	m_bResultBailed = false;
	m_nResultPoints = 0;
	m_nResultFlags = 0;
	m_bTracking = false;
	m_bAirborne = false;
	m_flAirStart = m_flAirPeak = 0.0f;
	m_flAirResultTime = -100.0f;
	m_flAirSeconds = m_flAirDistance = m_flAirHeight = m_flAirDrop = 0.0f;
	m_bAirBest = false;
	m_flBestAirSeconds = 0.0f;
	m_nMeatBail = m_nMeatScore = m_nMeatBones = 0;
	m_flMeatTime = -100.0f;
	m_hNameFont = m_hLineFont = m_hSmallFont = vgui::INVALID_FONT;
}

void CHudSkateTricks::ApplySchemeSettings( vgui::IScheme *pScheme )
{
	BaseClass::ApplySchemeSettings( pScheme );
	SetPaintBackgroundEnabled( false );
	m_hNameFont = pScheme->GetFont( "HudFontMediumBold", true );
	m_hLineFont = pScheme->GetFont( "HudFontBiggerBold", true );
	m_hSmallFont = pScheme->GetFont( "HudFontSmallBold", true );
}

bool CHudSkateTricks::ShouldDraw()
{
	C_TFPlayer *pPlayer = C_TFPlayer::GetLocalTFPlayer();
	if ( !cl_skate_hud.GetBool() || !pPlayer )
		return false;
	bool bSkating = pPlayer->m_Shared.InCond( TF_COND_SKATING );
	if ( !bSkating && gpGlobals->curtime - m_flResultTime > SKATE_HUD_RESULT_TIME )
		return false;
	return CHudElement::ShouldDraw();
}

void CHudSkateTricks::OnThink()
{
	// No HudLayout entry: cover the viewport.
	int w, h;
	GetParent()->GetSize( w, h );
	SetBounds( 0, 0, w, h );

	C_TFPlayer *pPlayer = C_TFPlayer::GetLocalTFPlayer();
	if ( pPlayer && pPlayer->m_Shared.InCond( TF_COND_SKATING ) )
	{
		Track( pPlayer );
		TrackAir( pPlayer );
		TrackMeat( pPlayer );
	}
	else if ( m_bTracking )
	{
		// Stopping mid-line: whatever was showing just clears.
		m_bTracking = false;
		m_Entries.RemoveAll();
		m_nLastLine = 0;
		m_bAirborne = false;
	}
}

void CHudSkateTricks::TrackAir( C_TFPlayer *pPlayer )
{
	int nState = pPlayer->GetSkateState();
	bool bAirborne = nState >= 200 && nState < 300;
	const Vector &vecOrigin = pPlayer->GetAbsOrigin();
	if ( bAirborne && !m_bAirborne )
	{
		m_flAirStart = gpGlobals->curtime;
		m_vecAirStart = vecOrigin;
		m_flAirPeak = vecOrigin.z;
	}
	else if ( bAirborne )
	{
		m_flAirPeak = MAX( m_flAirPeak, vecOrigin.z );
	}
	else if ( m_bAirborne )
	{
		float flSeconds = gpGlobals->curtime - m_flAirStart;
		// Landed (riding or grinding); a bail mid-air doesn't count.
		if ( flSeconds >= SKATE_HUD_MIN_AIR && nState != 300 )
		{
			m_flAirSeconds = flSeconds;
			m_flAirDistance = ( vecOrigin - m_vecAirStart ).Length2D();
			m_flAirHeight = m_flAirPeak - m_vecAirStart.z;
			m_flAirDrop = m_vecAirStart.z - vecOrigin.z;
			m_bAirBest = flSeconds > m_flBestAirSeconds;
			m_flBestAirSeconds = MAX( m_flBestAirSeconds, flSeconds );
			m_flAirResultTime = gpGlobals->curtime;
		}
	}
	m_bAirborne = bAirborne;
}

// Hammer units (1 unit = 1 inch at skate_world_scale 0.0254) as m or ft.
static const float SKATE_HUD_MEAT_TIME = 5.0f;
static void SkateFormatPoints( int nPoints, wchar_t *pszOut, int nOutChars );

void CHudSkateTricks::TrackMeat( C_TFPlayer *pPlayer )
{
	if ( pPlayer->m_nSkateBail != m_nMeatBail )
	{
		m_nMeatBail = pPlayer->m_nSkateBail;
		m_nMeatScore = m_nMeatBones = 0;
	}
	if ( pPlayer->m_nSkateBailScore != m_nMeatScore || pPlayer->m_nSkateBrokenBones != m_nMeatBones )
	{
		m_nMeatScore = pPlayer->m_nSkateBailScore;
		m_nMeatBones = pPlayer->m_nSkateBrokenBones;
		m_flMeatTime = gpGlobals->curtime;
	}
}

void CHudSkateTricks::PaintMeat( int w, int h )
{
	float flAge = gpGlobals->curtime - m_flMeatTime;
	if ( m_nMeatScore <= 0 || flAge > SKATE_HUD_MEAT_TIME )
		return;
	int nAlpha = (int)( 255 * clamp( ( SKATE_HUD_MEAT_TIME - flAge ) / 0.75f, 0.0f, 1.0f ) );
	int x = (int)( w * 0.03f );
	int y = (int)( h * 0.42f );
	wchar_t szText[ 128 ], szPoints[ 48 ];
	DrawText( m_hLineFont, x, y, L"HALL OF MEAT", Color( 235, 80, 60, nAlpha ), false );
	y += vgui::surface()->GetFontTall( m_hLineFont ) + 2;
	SkateFormatPoints( m_nMeatScore, szPoints, ARRAYSIZE( szPoints ) );
	V_snwprintf( szText, ARRAYSIZE( szText ), L"$%ls", szPoints );
	DrawText( m_hNameFont, x, y, szText, Color( 255, 215, 90, nAlpha ), false );
	y += vgui::surface()->GetFontTall( m_hNameFont ) + 4;

	int nSmallTall = vgui::surface()->GetFontTall( m_hSmallFont );
	int nShown = 0, nBroken = 0;
	for ( int i = 1; i < SKATE_BONE_COUNT; ++i )
	{
		if ( !( m_nMeatBones & ( 1 << i ) ) )
			continue;
		++nBroken;
		if ( nShown < 8 )
		{
			V_snwprintf( szText, ARRAYSIZE( szText ), L"broken %hs", g_pszSkateBoneNames[i] );
			DrawText( m_hSmallFont, x, y, szText, Color( 255, 255, 255, nAlpha ), false );
			y += nSmallTall + 1;
			++nShown;
		}
	}
	if ( nBroken > nShown )
	{
		V_snwprintf( szText, ARRAYSIZE( szText ), L"and %d more", nBroken - nShown );
		DrawText( m_hSmallFont, x, y, szText, Color( 255, 255, 255, nAlpha ), false );
	}
}

void CHudSkateTricks::FormatLength( float flUnits, wchar_t *pszOut, int nOutChars )
{
	if ( cl_skate_hud_units.GetBool() )
		V_snwprintf( pszOut, nOutChars, L"%.1f ft", flUnits / 12.0f );
	else
		V_snwprintf( pszOut, nOutChars, L"%.1f m", flUnits * 0.0254f );
}

void CHudSkateTricks::Track( C_TFPlayer *pPlayer )
{
	if ( !m_bTracking )
	{
		// Adopt the current counters without inventing a trick or result.
		m_bTracking = true;
		m_nLastSeq = pPlayer->m_nSkateTrickSeq;
		m_nLastLine = pPlayer->m_nSkateLineScore;
		m_nLastTotal = pPlayer->m_nSkateTotalScore;
		m_Entries.RemoveAll();
	}

	if ( pPlayer->m_nSkateTrickSeq != m_nLastSeq )
	{
		m_nLastSeq = pPlayer->m_nSkateTrickSeq;
		if ( pPlayer->m_szSkateTrick[0] )
		{
			if ( m_Entries.Count() >= SKATE_HUD_MAX_ENTRIES )
				m_Entries.Remove( 0 );
			Entry_t &entry = m_Entries[ m_Entries.AddToTail() ];
			g_pVGuiLocalize->ConvertANSIToUnicode( pPlayer->m_szSkateTrick, entry.szName, sizeof( entry.szName ) );
			entry.nScore = pPlayer->m_nSkateTrickScore;
			entry.flTime = gpGlobals->curtime;
			// A new line started after a shown result.
			if ( gpGlobals->curtime - m_flResultTime < SKATE_HUD_RESULT_TIME && m_Entries.Count() > 1 && m_Entries[0].flTime < m_flResultTime )
			{
				Entry_t keep = entry;
				m_Entries.RemoveAll();
				m_Entries.AddToTail( keep );
			}
			m_flResultTime = -100.0f;
		}
	}
	else if ( m_Entries.Count() )
	{
		// The trick's points settle while it is still being performed.
		Entry_t &last = m_Entries.Tail();
		if ( pPlayer->m_nSkateTrickScore > last.nScore )
		{
			last.nScore = pPlayer->m_nSkateTrickScore;
			last.flTime = gpGlobals->curtime;
		}
	}

	int nLine = pPlayer->m_nSkateLineScore;
	int nTotal = pPlayer->m_nSkateTotalScore;
	if ( nLine > 0 )
	{
		m_flLastMultiplier = MAX( 1.0f, pPlayer->m_flSkateMultiplier );
		m_nLineMultiplied = nLine;
	}
	if ( m_nLastLine > 0 && nLine == 0 )
	{
		// The line ended: banked into the total, or lost to a bail.
		m_flResultTime = gpGlobals->curtime;
		m_nResultPoints = nTotal - m_nLastTotal;
		m_bResultBailed = m_nResultPoints <= 0;
		m_nResultFlags = pPlayer->m_nSkateScoreFlags;
	}
	m_nLastLine = nLine;
	m_nLastTotal = nTotal;
}

void CHudSkateTricks::DrawText( vgui::HFont font, int x, int y, const wchar_t *pszText, Color color, bool bCenter )
{
	int tw, th;
	vgui::surface()->GetTextSize( font, pszText, tw, th );
	if ( bCenter )
		x -= tw / 2;
	vgui::surface()->DrawSetTextFont( font );
	vgui::surface()->DrawSetTextColor( Color( 0, 0, 0, color.a() * 3 / 4 ) );
	vgui::surface()->DrawSetTextPos( x + 2, y + 2 );
	vgui::surface()->DrawPrintText( pszText, V_wcslen( pszText ) );
	vgui::surface()->DrawSetTextColor( color );
	vgui::surface()->DrawSetTextPos( x, y );
	vgui::surface()->DrawPrintText( pszText, V_wcslen( pszText ) );
}

static void SkateFormatPoints( int nPoints, wchar_t *pszOut, int nOutChars )
{
	char szDigits[ 32 ];
	V_snprintf( szDigits, sizeof( szDigits ), "%d", abs( nPoints ) );
	char szGrouped[ 48 ];
	int nLen = V_strlen( szDigits ), o = 0;
	if ( nPoints < 0 )
		szGrouped[ o++ ] = '-';
	for ( int i = 0; i < nLen; ++i )
	{
		szGrouped[ o++ ] = szDigits[i];
		if ( ( nLen - i - 1 ) % 3 == 0 && i != nLen - 1 )
			szGrouped[ o++ ] = ',';
	}
	szGrouped[ o ] = '\0';
	g_pVGuiLocalize->ConvertANSIToUnicode( szGrouped, pszOut, nOutChars * sizeof( wchar_t ) );
}

void CHudSkateTricks::Paint()
{
	C_TFPlayer *pPlayer = C_TFPlayer::GetLocalTFPlayer();
	if ( !pPlayer )
		return;

	int w, h;
	GetSize( w, h );
	const int cx = w / 2;
	int y = (int)( h * 0.70f );
	const float flNow = gpGlobals->curtime;
	PaintMeat( w, h );
	wchar_t szText[ 128 ], szPoints[ 48 ];

	// Line score / result, the anchor of the feed.
	bool bResult = flNow - m_flResultTime < SKATE_HUD_RESULT_TIME;
	if ( bResult )
	{
		int nAlpha = (int)( 255 * clamp( ( SKATE_HUD_RESULT_TIME - ( flNow - m_flResultTime ) ) / 0.5f, 0.0f, 1.0f ) );
		if ( m_bResultBailed )
		{
			DrawText( m_hLineFont, cx, y, L"BAILED", Color( 235, 80, 60, nAlpha ), true );
		}
		else
		{
			SkateFormatPoints( m_nResultPoints, szPoints, ARRAYSIZE( szPoints ) );
			const wchar_t *pszQuality = ( m_nResultFlags & SKATE_SCORE_CLEAN ) ? L"  CLEAN" : ( ( m_nResultFlags & SKATE_SCORE_SKETCHY ) ? L"  SKETCHY" : L"" );
			V_snwprintf( szText, ARRAYSIZE( szText ), L"LANDED +%ls%ls", szPoints, pszQuality );
			DrawText( m_hLineFont, cx, y, szText, Color( 140, 230, 110, nAlpha ), true );
		}
	}
	else if ( pPlayer->m_nSkateLineScore > 0 )
	{
		SkateFormatPoints( pPlayer->m_nSkateLineScore, szPoints, ARRAYSIZE( szPoints ) );
		if ( m_flLastMultiplier > 1.01f )
			V_snwprintf( szText, ARRAYSIZE( szText ), L"%ls  x%d", szPoints, (int)( m_flLastMultiplier + 0.5f ) );
		else
			V_snwprintf( szText, ARRAYSIZE( szText ), L"%ls", szPoints );
		DrawText( m_hLineFont, cx, y, szText, Color( 255, 215, 90, 255 ), true );
	}

	// Tricks in this line, newest just above the line score.
	int nNameTall = vgui::surface()->GetFontTall( m_hNameFont );
	int ty = y - nNameTall - 4;
	for ( int i = m_Entries.Count() - 1; i >= 0; --i )
	{
		const Entry_t &entry = m_Entries[i];
		float flAge = flNow - entry.flTime;
		if ( !bResult && flAge > SKATE_HUD_ENTRY_FADE )
			continue;
		int nAlpha = bResult ? 255 : (int)( 255 * clamp( ( SKATE_HUD_ENTRY_FADE - flAge ) / 1.0f, 0.0f, 1.0f ) );
		bool bNewest = i == m_Entries.Count() - 1;
		if ( entry.nScore > 0 )
		{
			SkateFormatPoints( entry.nScore, szPoints, ARRAYSIZE( szPoints ) );
			V_snwprintf( szText, ARRAYSIZE( szText ), L"%ls  +%ls", entry.szName, szPoints );
		}
		else
		{
			V_snwprintf( szText, ARRAYSIZE( szText ), L"%ls", entry.szName );
		}
		DrawText( m_hNameFont, cx, ty, szText, bNewest ? Color( 255, 255, 255, nAlpha ) : Color( 210, 210, 210, nAlpha * 3 / 4 ), true );
		ty -= nNameTall + 2;
	}

	// Air: live while airborne, then the result under the line score.
	if ( cl_skate_hud_air.GetBool() && pPlayer->m_Shared.InCond( TF_COND_SKATING ) )
	{
		int ay = y + vgui::surface()->GetFontTall( m_hLineFont ) + 4;
		wchar_t szDistance[ 32 ], szHeight[ 32 ], szDrop[ 32 ];
		if ( m_bAirborne && flNow - m_flAirStart >= SKATE_HUD_MIN_AIR )
		{
			const Vector &vecOrigin = pPlayer->GetAbsOrigin();
			FormatLength( ( vecOrigin - m_vecAirStart ).Length2D(), szDistance, ARRAYSIZE( szDistance ) );
			FormatLength( m_flAirPeak - m_vecAirStart.z, szHeight, ARRAYSIZE( szHeight ) );
			V_snwprintf( szText, ARRAYSIZE( szText ), L"AIR  %.2fs   %ls   %ls high", flNow - m_flAirStart, szDistance, szHeight );
			DrawText( m_hSmallFont, cx, ay, szText, Color( 150, 210, 255, 255 ), true );
		}
		else if ( flNow - m_flAirResultTime < SKATE_HUD_AIR_TIME )
		{
			int nAlpha = (int)( 255 * clamp( ( SKATE_HUD_AIR_TIME - ( flNow - m_flAirResultTime ) ) / 0.5f, 0.0f, 1.0f ) );
			FormatLength( m_flAirDistance, szDistance, ARRAYSIZE( szDistance ) );
			FormatLength( MAX( 0.0f, m_flAirHeight ), szHeight, ARRAYSIZE( szHeight ) );
			FormatLength( m_flAirDrop, szDrop, ARRAYSIZE( szDrop ) );
			V_snwprintf( szText, ARRAYSIZE( szText ), L"AIR  %.2fs   %ls   %ls high%ls%ls%ls", m_flAirSeconds, szDistance, szHeight,
				m_flAirDrop > 24.0f ? L"   " : L"", m_flAirDrop > 24.0f ? szDrop : L"", m_flAirDrop > 24.0f ? L" drop" : L"" );
			DrawText( m_hSmallFont, cx, ay, szText, Color( 150, 210, 255, nAlpha ), true );
			if ( m_bAirBest )
			{
				DrawText( m_hSmallFont, cx, ay + vgui::surface()->GetFontTall( m_hSmallFont ) + 2, L"BEST AIR", Color( 255, 215, 90, nAlpha ), true );
			}
		}
	}

	// Banked total and speed, top right.
	if ( pPlayer->m_Shared.InCond( TF_COND_SKATING ) )
	{
		SkateFormatPoints( pPlayer->m_nSkateTotalScore, szPoints, ARRAYSIZE( szPoints ) );
		V_snwprintf( szText, ARRAYSIZE( szText ), L"SKATE  %ls", szPoints );
		int tw, th;
		vgui::surface()->GetTextSize( m_hSmallFont, szText, tw, th );
		DrawText( m_hSmallFont, w - tw - (int)( w * 0.02f ), (int)( h * 0.12f ), szText, Color( 255, 255, 255, 230 ), false );
		if ( cl_skate_hud_air.GetBool() )
		{
			Vector vecVelocity;
			pPlayer->EstimateAbsVelocity( vecVelocity );
			float flSpeed = vecVelocity.Length2D();	// units/s; 1 unit = 1 inch
			if ( cl_skate_hud_units.GetBool() )
				V_snwprintf( szText, ARRAYSIZE( szText ), L"%.0f MPH", flSpeed * 3600.0f / 63360.0f );
			else
				V_snwprintf( szText, ARRAYSIZE( szText ), L"%.0f KM/H", flSpeed * 0.0254f * 3.6f );
			vgui::surface()->GetTextSize( m_hSmallFont, szText, tw, th );
			DrawText( m_hSmallFont, w - tw - (int)( w * 0.02f ), (int)( h * 0.12f ) + th + 2, szText, Color( 255, 255, 255, 200 ), false );
		}
	}
}
