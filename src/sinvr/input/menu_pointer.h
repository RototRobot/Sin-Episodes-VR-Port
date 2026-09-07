// Pointing at the game's menus with a controller.
//
// ---- WHY THIS SHAPE ---------------------------------------------------------
//
// SiN's menus are VGUI, driven by the OS cursor. `hud_shift_skip_in_menus`
// exists because moving where a menu DRAWS without moving its hit-testing makes
// the buttons unclickable -- so the two halves cannot be done independently, and
// the cursor is the half that has to come first. Drive the cursor and verify a
// click lands; only then is it safe to move the panel.
//
// The menu is drawn at the SAME backbuffer position for both eyes (every Ortho
// call during a menu goes through unshifted -- see Detour_Ortho), which means it
// is screen-locked: it follows the player's head and holds still relative to it.
// That is the property this module leans on. A button's screen position is fixed
// relative to the HEAD, so "where is the controller pointing, as seen from the
// head" maps directly onto a backbuffer pixel, and no world geometry, trace or
// panel placement is involved.
//
//     pointing direction (room space)
//       -> expressed in the HEAD's basis          right/up/forward
//       -> divided by the RENDERED half-tangents  normalised -1..1
//       -> backbuffer pixel -> screen pixel -> SetCursorPos
//
// The half-tangents come from StereoRenderer::RenderedHalfTangents, which is the
// same frustum the image is actually rendered with -- not the game's own FOV,
// which no longer describes what the player sees.
//
// ---- WHAT THIS DELIBERATELY DOES NOT DO -------------------------------------
//
// No trace, no world panel, no depth. The menu is still a 2D overlay at the
// wrong convergence; this makes it USABLE, not correct. Moving it onto a world
// quad is a separate job and this module is the prerequisite for it, because
// once the cursor is driven from the controller the hit-testing follows the
// pointer rather than the panel.
#pragma once

#include <windows.h>
#include <math.h>
#include "../vr/vr_backend.h"
#include "../vr_camera.h"
#include "../sdk/debug_overlay.h"
#include "../sdk/vgui_input.h"
#include "../render/menu_panel.h"
#include "../../common/log.h"

namespace sinvr {

struct MenuPointerSettings
{
	bool enabled = true;

	// Which hand points. Defaults to the weapon hand so it matches the hand the
	// player already aims with -- pointing and aiming being different hands is
	// the sort of thing that is only discovered mid-menu.
	bool useOffHand = false;

	// Synthesise a left click from the trigger. Separable from the pointing so
	// the mapping can be verified before anything starts clicking buttons --
	// a mis-mapped pointer that also clicks can start a new game on its own.
	bool click = true;

	// The controller-to-pointer tilt, degrees. A controller's tracked axis is
	// nowhere near the direction it feels like it points: the same ~58 degrees
	// that viewmodel_angle_pitch exists to correct. Pointing uses the same
	// correction so that "point at it" and "aim at it" are the same gesture.
	float pitch = 58.0f;
	float yaw = 0.0f;

	// 0 = raw. Higher is steadier and laggier; a menu pointer wants a little,
	// because a button is a small target and the hand is not a mouse.
	float smoothing = 0.35f;

	// ---- WHY A SYNTHESISED CLICK NEEDS DEBOUNCING -------------------------
	//
	// A second press arriving within this many milliseconds of the last one is
	// dropped. 0 disables it.
	//
	// It exists because of what a DOUBLE press does to a VGUI ComboBox, which
	// is not what it does to a button:
	//
	//   Button    OnMousePressed twice -> the command fires twice. Usually
	//             harmless and always invisible, so nothing looks wrong.
	//   ComboBox  OnMouseDoublePressed calls OnMousePressed, which calls
	//             DoClick(), and DoClick() TOGGLES -- "menu is already
	//             visible, hide the menu". So the dropdown opens and shuts
	//             again within one frame and the player sees nothing happen.
	//
	// That asymmetry is the whole reported symptom: "I can click buttons but
	// dropdowns do not expand." The pointer was firing two presses per trigger
	// pull -- visible in the heartbeat as a click counter climbing in twos --
	// and only the combo boxes could show it.
	//
	// 100 ms is far longer than a chattering trigger and far shorter than a
	// deliberate double-click, so nothing a player means to do is lost.
	int clickDebounceMs = 100;

	// Draw a marker where the pointer is aiming.
	//
	// REQUIRED to be able to use the menu at all, and the reason is not obvious:
	// the OS cursor is composited by WINDOWS onto the desktop, not drawn into
	// the D3D backbuffer -- and the backbuffer is what gets submitted to the
	// headset. So the real cursor is genuinely invisible in VR no matter where
	// it is put. This draws our own, in the world, along the pointing ray.
	bool marker = true;
	float markerDistance = 120.0f;   // Source units from the head
	float markerSize = 3.0f;

	// Must match menu_scale, so that pointing at a button that has been drawn
	// SHRUNK still moves the cursor to where that button hit-tests. Held here
	// rather than read twice, because the two disagreeing is silently
	// unclickable menus -- the exact failure hud_shift_skip_in_menus existed
	// to avoid.
	float menuScale = 1.0f;

	// Draw the cross as a SCREEN overlay rather than a world one.
	//
	// A world marker is drawn in the world pass; a menu is 2D and is drawn after
	// it. So the world cross is always BEHIND the panel, which is exactly the
	// "cursor appears behind the submenus" report -- and it is worst on the
	// small centred dialogs, which is where a pointer matters most. The screen
	// overlay goes through the mat-system surface instead, in the same 2D layer
	// the menu is drawn in.
	bool screenMarker = false;

	bool debug = false;
};

class MenuPointer
{
public:
	void SetSettings( const MenuPointerSettings& s ) { m_settings = s; }
	// Room space and world space differ by a pure yaw. The camera owns that
	// number; taking a second copy of the derivation is how they drift.
	void SetRoomToWorldYaw( float deg ) { m_roomToWorldYaw = deg; }
	const MenuPointerSettings& Settings() const { return m_settings; }

	// Called only while a menu is up. `tanX`/`tanY` are the rendered frustum's
	// half-tangents; without them there is no mapping and the pointer stays put.
	// `fl/fr/ft/fb` are the MEAN of the two eyes' rendered frusta, in tangent
	// units -- the mapping between a backbuffer pixel and the direction the
	// player fuses. `anchorTanX/Y` are the panel centre's tangents in the head's
	// frame, or 0 when the menu is not anchored.
	//
	// Both the pointing direction AND the panel centre are turned into screen
	// fractions by the SAME two lines below. That is deliberate: the previous
	// version normalised the pointing by RenderedHalfTangents and took the panel
	// centre from the renderer's own per-eye placement, so the two came through
	// different maths and could not be made to agree. One function, one mapping,
	// no sign conventions duplicated anywhere.
	// ---- hit-tested against the PANEL, not against the screen --------------
	//
	// The pointing direction is intersected with the quad the renderer is
	// drawing the menu on, and the crossing point IS the cursor position. No
	// frustum tangents, no per-eye placement, no scale to invert -- all of which
	// were sources of error that only showed up on small targets.
	//
	// It also cannot drift out of agreement with the picture, because the quad
	// it traces against is the same struct the renderer draws from.
	void UpdateOnPanel( IVRBackend& vr, const MenuPanelGeometry& panel )
	{
		if ( !m_settings.enabled )
			return;

		++m_frames;

		if ( !panel.valid )
		{
			m_lastReason = "menu panel geometry not up yet";
			return;
		}

		const HmdPose& hmd = vr.Hmd();
		const ControllerPose& hand = m_settings.useOffHand ? vr.OffHand() : vr.WeaponHand();
		if ( !hand.valid )
		{
			// Do NOT recentre the cursor or fall back to the head. A controller
			// that has gone to sleep should leave the pointer where the player
			// last put it, not fling it to the middle of the screen.
			m_lastReason = "pointing hand not tracked";
			ReleaseClick();
			return;
		}

		// Where the controller points, in WORLD space, with the same tilt
		// correction the weapon gets -- composed as a rotation rather than
		// added, because adding Euler components sends it through the pitch
		// singularity. That cost this project a session on the weapon.
		//
		// The controller pose is in room space and the panel is in world space,
		// but the two differ by a pure YAW, so the pointing direction is rotated
		// by the same body yaw the camera composes the view from. Nothing else
		// is needed: pitch and roll are shared between the frames.
		const QAngle aimed = ComposeWeaponAngles( hand.angles, m_settings.pitch,
												  m_settings.yaw, 0.0f );
		QAngle world = aimed;
		world.y = NormalizeAngle( aimed.y + m_roomToWorldYaw );

		Vector dir, pr, pu;
		AnglesToBasis( world, dir, pr, pu );

		// Straight at the quad the renderer is drawing on. The crossing point IS
		// the cursor -- there is no screen mapping in this at all, which is what
		// makes it immune to the frustum and placement conventions that the
		// previous versions kept getting wrong.
		float u = 0.0f, v = 0.0f;
		Vector hitWorld;
		if ( !MenuPanelHit( panel, dir, u, v, hitWorld ) )
		{
			m_lastReason = "not pointing at the panel";
			return;
		}

		m_offScreen = ( u < 0.0f || u > 1.0f || v < 0.0f || v > 1.0f );

		// -1..1 about the panel's middle, so the smoothing and the clamping stay
		// in one symmetric space.
		float sx = Clamp1( u * 2.0f - 1.0f );
		float sy = Clamp1( v * 2.0f - 1.0f );

		// Exponential smoothing on the NORMALISED position, before it becomes
		// pixels, so the amount of smoothing does not depend on resolution.
		if ( m_haveLast && m_settings.smoothing > 0.0f )
		{
			const float a = 1.0f - Clamp01( m_settings.smoothing );
			sx = m_lastX + ( sx - m_lastX ) * a;
			sy = m_lastY + ( sy - m_lastY ) * a;
		}
		m_lastX = sx;
		m_lastY = sy;
		m_haveLast = true;

		// The marker goes ON the quad, at the smoothed position -- so the cross
		// is a picture of where the cursor IS, not of where the hand points.
		m_markerWorld.x = panel.centre.x + panel.right.x * ( sx * panel.halfWidth )
										 - panel.up.x * ( sy * panel.halfHeight );
		m_markerWorld.y = panel.centre.y + panel.right.y * ( sx * panel.halfWidth )
										 - panel.up.y * ( sy * panel.halfHeight );
		m_markerWorld.z = panel.centre.z + panel.right.z * ( sx * panel.halfWidth )
										 - panel.up.z * ( sy * panel.halfHeight );
		m_markerReady = m_settings.marker && m_overlay.Valid();

		// NOTHING to invert any more.
		//
		// The panel's position, size and orientation are already accounted for
		// by tracing against it, so the fraction across the quad IS the fraction
		// across the HUD. Every transform the old version had to undo -- the
		// screen scale, the panel placement, the frustum normalisation -- has
		// gone, and with it every chance of the picture and the hit-test
		// disagreeing.
		const float hx = sx;
		const float hy = sy;
		m_panelFx = 0.5f;
		m_panelFy = 0.5f;

		HWND wnd = GameWindow();
		if ( !wnd )
		{
			m_lastReason = "game window not found";
			return;
		}

		RECT rc = {};
		if ( !GetClientRect( wnd, &rc ) || rc.right <= rc.left || rc.bottom <= rc.top )
		{
			m_lastReason = "client rect unavailable";
			return;
		}

		const int w = rc.right - rc.left;
		const int h = rc.bottom - rc.top;

		// hy is already in screen-fraction orientation (fb > ft means +y is
		// DOWN the screen), so it is not flipped again here.
		POINT p;
		p.x = (LONG)( ( hx * 0.5f + 0.5f ) * (float)w );
		p.y = (LONG)( ( hy * 0.5f + 0.5f ) * (float)h );
		if ( p.x < 0 ) p.x = 0;
		if ( p.y < 0 ) p.y = 0;
		if ( p.x >= w ) p.x = w - 1;
		if ( p.y >= h ) p.y = h - 1;

		m_clientX = p.x;
		m_clientY = p.y;

		// ---- VGUI FIRST, THE OS SECOND ------------------------------------
		//
		// Win32 SetCursorPos cannot place the cursor outside the virtual
		// desktop, and with vr_allow_oversize_window the client area is larger
		// than the desktop -- measured, 2444x2392 against 3840x2160, so the
		// bottom 232 rows were unreachable and every click there landed short
		// of the drawn cursor. VGUI keeps its own cursor in its own space and
		// has no desktop to clamp against.
		//
		// The OS cursor is still moved afterwards, on purpose. It is what the
		// engine's own mouse handling and any hover state outside VGUI read,
		// and clamped-but-close is strictly better there than not moved at
		// all. VGUI's copy is the one that decides the hit test.
		m_vguiInput.Bind();
		if ( m_vguiInput.Valid() && !m_vguiInput.Verified() &&
			 !m_vguiInput.VerifyFailed() )
			m_vguiInput.Verify( wnd );

		m_vguiInput.ProbeClamp( wnd );
		const bool viaVgui = m_vguiInput.SetCursorPos( p.x, p.y );

		if ( !ClientToScreen( wnd, &p ) )
		{
			m_lastReason = "ClientToScreen failed";
			return;
		}

		// How far Windows had to move the request to keep it on the desktop.
		// Reported rather than inferred: this is the exact quantity that was
		// silently wrong before, and a zero here is what says the menu is
		// clickable to its bottom edge.
		const bool osOk = ( SetCursorPos( p.x, p.y ) != FALSE );
		POINT got = {};
		if ( GetCursorPos( &got ) )
		{
			const int ddx = ( got.x > p.x ) ? ( got.x - p.x ) : ( p.x - got.x );
			const int ddy = ( got.y > p.y ) ? ( got.y - p.y ) : ( p.y - got.y );
			const int worst = ( ddx > ddy ) ? ddx : ddy;
			if ( worst > m_osClampWorst )
				m_osClampWorst = worst;
			if ( worst > 1 )
				++m_osClamped;
		}

		if ( viaVgui || osOk )
		{
			++m_moves;
			m_lastReason = "";
		}
		else
		{
			m_lastReason = "SetCursorPos refused";
		}

		if ( m_settings.click )
			DriveClick( vr.Input().attack );
	}

	bool Bind( void* debugOverlayIface ) { return m_overlay.Bind( debugOverlayIface ); }
	// Raw evidence for "is it being drawn at all", kept separate from the
	// pretty log line so the two cannot drift.
	bool OverlayBound() const { return m_overlay.Valid(); }
	unsigned int OverlayLines() const { return m_overlay.BoxesDrawn(); }
	// ---- TWO DIFFERENT QUESTIONS, AND THEY MUST NOT SHARE A FLAG ----------
	//
	// MarkerWorld answers "should the OVERLAY cross be drawn", because
	// m_markerReady folds in both `marker` and whether IVDebugOverlay bound.
	// CursorWorld answers "does the pointer HAVE a position on the panel".
	//
	// They were the same call for one build and it cost a whole test run: the
	// D3D9 cursor armed itself off MarkerWorld, the overlay marker was
	// deliberately switched off so the two cursors would not both draw, and so
	// the D3D9 cursor was never armed at all. It reported `idle (no menu)` on a
	// menu, which is exactly the wrong thing to go looking for.
	//
	// A consumer that draws the cursor ITSELF wants CursorWorld. Only the
	// overlay path wants MarkerWorld.
	bool MarkerWorld( Vector& out ) const { out = m_markerWorld; return m_markerReady; }
	bool CursorWorld( Vector& out ) const { out = m_markerWorld; return m_haveLast; }
	bool HavePosition() const { return m_haveLast; }

	// The cursor route, reported as EFFECT. `clamped 0` is the claim that the
	// menu is clickable all the way to its bottom edge; anything else is the
	// old defect still present.
	bool VguiCursorActive() const { return m_vguiInput.Verified(); }
	bool VguiCursorBound() const { return m_vguiInput.Valid(); }
	bool VguiCursorFailed() const { return m_vguiInput.VerifyFailed(); }
	unsigned int VguiCursorWrites() const { return m_vguiInput.Writes(); }
	unsigned int OsClampedCount() const { return m_osClamped; }
	int OsClampWorst() const { return m_osClampWorst; }
	bool MarkerReady() const { return m_settings.enabled && m_settings.marker && m_overlay.Valid(); }

	// ---- WHY A MARKER IS NOT OPTIONAL --------------------------------------
	//
	// The OS cursor is composited by WINDOWS onto the desktop. It is not drawn
	// into the D3D backbuffer, and the backbuffer is what is submitted to the
	// headset -- so the real cursor is invisible in VR wherever it is put. That
	// is not a bug in the pointer; it is what "I cannot see the cursor" always
	// was, and no amount of moving it would have helped.
	//
	// So we draw our own. Placed along the SAME direction the cursor position
	// was computed from, at a fixed distance from the head, which puts it at
	// exactly the screen position the cursor occupies -- by construction, not by
	// tuning, because both come from the one pair of tangents.
	//
	// The marker is placed ON THE QUAD inside UpdateOnPanel now, so there is
	// no separate preparation step and no second way for it to disagree with
	// the cursor.

	// Once per EYE PASS, after the overlay list has been cleared -- a duration-0
	// overlay lives about a TICK, so anything submitted once per frame ghosts.
	void Submit()
	{
		// The screen cross first: it goes through the mat-system surface, which
		// is the layer the menu itself is drawn in, so it is not buried by the
		// panel the way a world marker is.
		if ( m_settings.enabled && m_settings.screenMarker && m_overlay.Valid()
			 && m_haveLast )
		{
			const float fx = m_lastX * 0.5f + 0.5f;
			const float fy = 0.5f - m_lastY * 0.5f;
			const int g = m_clickHeld ? 64 : 255;
			m_overlay.ScreenText( fx, fy, "+", 255, g, 32, 255, 0.0f );
			++m_screenSubmissions;
		}

		if ( !m_markerReady )
			return;

		const float h = m_settings.markerSize;
		// A cross rather than a box: at this distance a small box is a dot, and
		// which pixel it is centred on is the whole question.
		const Vector a1 = { m_markerWorld.x - h, m_markerWorld.y, m_markerWorld.z };
		const Vector b1 = { m_markerWorld.x + h, m_markerWorld.y, m_markerWorld.z };
		const Vector a2 = { m_markerWorld.x, m_markerWorld.y - h, m_markerWorld.z };
		const Vector b2 = { m_markerWorld.x, m_markerWorld.y + h, m_markerWorld.z };
		const Vector a3 = { m_markerWorld.x, m_markerWorld.y, m_markerWorld.z - h };
		const Vector b3 = { m_markerWorld.x, m_markerWorld.y, m_markerWorld.z + h };

		const int g = m_clickHeld ? 64 : 255;
		const int b = m_clickHeld ? 64 : 32;
		// noDepthTest TRUE. The panel is drawn as UI and ignores the depth
		// buffer, but the cross is world geometry and does not -- so in a real
		// level any wall nearer than the panel swallowed it. That is why it
		// showed on the main menu, whose background map is wide open, and
		// vanished the moment a pause menu came up indoors.
		m_overlay.Line( a1, b1, 255, g, b, true, 0.0f );
		m_overlay.Line( a2, b2, 255, g, b, true, 0.0f );
		m_overlay.Line( a3, b3, 255, g, b, true, 0.0f );
		++m_submissions;
	}

	// Leaving the menu, or losing the hand, must not strand a held button.
	void Release()
	{
		ReleaseClick();
		m_haveLast = false;
		m_markerReady = false;
	}

	void LogState() const
	{
		if ( !m_settings.enabled )
		{
			Log( "pointer: OFF -- the menu cannot be clicked with a controller" );
			return;
		}

		Log( "pointer: %s | screen fraction (%.2f %.2f) -> client (%d %d) | %u moves "
			 "over %u menu frames | clicks %u%s%s",
			 m_moves ? "driving the cursor" : "NOT MOVING THE CURSOR",
			 m_lastX * 0.5f + 0.5f, m_lastY * 0.5f + 0.5f,
			 m_clientX, m_clientY, m_moves, m_frames, m_clicks,
			 m_offScreen ? " | pointing OFF the panel" : "",
			 ( m_lastReason && m_lastReason[0] ) ? " | " : "" );

		if ( m_lastReason && m_lastReason[0] )
			Log( "pointer: last refusal -- %s", m_lastReason );

		// ---- WHICH ROUTE PLACED THE CURSOR, AND WAS IT CLAMPED ------------
		//
		// `clamped 0` is the whole claim: the click lands where the cursor is
		// drawn, right down to the bottom edge of the menu. A non-zero worst
		// value is the exact pixel error the player sees, and it appears only
		// once the window is bigger than the desktop.
		Log( "pointer: cursor via %s | %u vgui write(s) | OS clamped on %u move(s), "
			 "worst %d px%s",
			 m_vguiInput.Verified() ? "VGUI (unclamped)"
				 : ( m_vguiInput.VerifyFailed()
					 ? "Win32 ONLY -- vgui slot check failed"
					 : ( m_vguiInput.Valid() ? "Win32 (vgui verifying...)"
										     : "Win32 (vgui not bound)" ) ),
			 m_vguiInput.Writes(), m_osClamped, m_osClampWorst,
			 ( m_osClamped > 0 && !m_vguiInput.Verified() )
				 ? " <- the OS cursor cannot reach past the desktop edge, so "
				   "clicks below it land short of the drawn cursor"
				 : "" );

		// The number that decides whether the trigger is chattering. A VGUI
		// ComboBox toggles on every press, so two presses per pull open and
		// shut the dropdown and the player sees nothing at all.
		Log( "pointer: clicks %u sent, %u suppressed as chatter (debounce %d ms) "
			 "| closest gap between presses %d ms%s",
			 m_clicks, m_suppressed, m_settings.clickDebounceMs, m_minGapMs,
			 ( m_suppressed > 0 )
				 ? "  <-- the trigger IS double-firing; this is what stopped "
				   "dropdown menus opening"
				 : ( m_minGapMs > 0 && m_minGapMs < 250 )
					 ? "  <-- presses this close together are unlikely to be "
					   "deliberate; raise menu_click_debounce_ms" : "" );

		// The OS cursor cannot appear in the headset -- Windows composites it
		// onto the desktop, never into the backbuffer we submit. So this marker
		// IS the pointer as far as the player is concerned.
		Log( "pointer: world marker %s (%u) | screen marker %s (%u)%s",
			 !m_settings.marker ? "off"
				 : ( m_overlay.Valid() ? ( m_markerReady ? "drawn" : "no direction yet" )
									   : "NO DEBUG OVERLAY" ),
			 m_submissions,
			 !m_settings.screenMarker ? "off"
				 : ( m_overlay.Valid() ? "drawn" : "NO DEBUG OVERLAY" ),
			 m_screenSubmissions,
			 m_overlay.ScreenTextFaulted()
				 ? "  <-- AddScreenTextOverlay FAULTED, slot 6 is wrong on this build" : "" );
		Log( "pointer: panel centre at (%.2f %.2f) scale %.2f -- the cursor is mapped "
			 "through exactly that, so a moved or shrunk menu stays clickable",
			 m_panelFx, m_panelFy, m_settings.menuScale );
	}

private:
	static float Clamp1( float v ) { return v < -1.0f ? -1.0f : ( v > 1.0f ? 1.0f : v ); }
	static float Clamp01( float v ) { return v < 0.0f ? 0.0f : ( v > 0.95f ? 0.95f : v ); }

	void DriveClick( bool held )
	{
		if ( held == m_clickHeld )
			return;
		m_clickHeld = held;

		if ( held )
		{
			const DWORD now = GetTickCount();
			const DWORD since = m_lastPressMs ? ( now - m_lastPressMs ) : 0xFFFFFFFF;

			// Measured whether or not it is acted on, so the log can say
			// what the trigger is really doing rather than what the fix
			// assumed. A gap of a few ms is chatter; a real double-click is
			// hundreds.
			m_lastGapMs = ( since == 0xFFFFFFFF ) ? 0 : (int)since;
			if ( m_lastGapMs > 0 && ( m_minGapMs == 0 || m_lastGapMs < m_minGapMs ) )
				m_minGapMs = m_lastGapMs;

			if ( m_settings.clickDebounceMs > 0 &&
				 since < (DWORD)m_settings.clickDebounceMs )
			{
				// Swallow the press AND the release that will follow it, so
				// the button state stays paired -- a lone LEFTUP would be a
				// release VGUI never saw a press for.
				m_clickSuppressed = true;
				++m_suppressed;
				return;
			}

			m_lastPressMs = now;
		}
		else if ( m_clickSuppressed )
		{
			m_clickSuppressed = false;
			return;
		}

		INPUT in = {};
		in.type = INPUT_MOUSE;
		in.mi.dwFlags = held ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP;
		SendInput( 1, &in, sizeof( in ) );
		if ( held )
			++m_clicks;
	}

	void ReleaseClick()
	{
		if ( !m_clickHeld )
			return;
		m_clickHeld = false;

		// A suppressed press was never sent, so there is nothing to release.
		if ( m_clickSuppressed )
		{
			m_clickSuppressed = false;
			return;
		}

		INPUT in = {};
		in.type = INPUT_MOUSE;
		in.mi.dwFlags = MOUSEEVENTF_LEFTUP;
		SendInput( 1, &in, sizeof( in ) );
	}

	// The game's own top-level window, found by process rather than by class
	// name or title -- both of which are the game's to change. Cached, and
	// re-found if it ever goes away.
	HWND GameWindow()
	{
		if ( m_window && IsWindow( m_window ) )
			return m_window;

		m_window = nullptr;
		EnumWindows( &MenuPointer::EnumProc, reinterpret_cast<LPARAM>( this ) );
		if ( m_window && !m_windowLogged )
		{
			m_windowLogged = true;
			Log( "pointer: game window %p", m_window );
		}
		return m_window;
	}

	static BOOL CALLBACK EnumProc( HWND wnd, LPARAM param )
	{
		DWORD pid = 0;
		GetWindowThreadProcessId( wnd, &pid );
		if ( pid != GetCurrentProcessId() )
			return TRUE;
		if ( !IsWindowVisible( wnd ) )
			return TRUE;

		RECT rc = {};
		if ( !GetClientRect( wnd, &rc ) )
			return TRUE;
		// Skip the tool windows a game creates alongside the real one.
		if ( rc.right - rc.left < 200 || rc.bottom - rc.top < 200 )
			return TRUE;

		reinterpret_cast<MenuPointer*>( param )->m_window = wnd;
		return FALSE;
	}

	MenuPointerSettings m_settings;
	DebugOverlay m_overlay;

	float m_roomToWorldYaw = 0.0f;
	float m_tanX = 0.0f;
	float m_tanY = 0.0f;
	bool m_haveDir = false;
	Vector m_markerWorld = { 0.0f, 0.0f, 0.0f };
	bool m_markerReady = false;
	unsigned int m_submissions = 0;
	unsigned int m_screenSubmissions = 0;
	float m_panelFx = 0.5f;
	float m_panelFy = 0.5f;

	HWND m_window = nullptr;
	bool m_windowLogged = false;

	float m_lastX = 0.0f;
	float m_lastY = 0.0f;
	bool m_haveLast = false;
	bool m_offScreen = false;
	bool m_clickHeld = false;
	bool m_clickSuppressed = false;
	DWORD m_lastPressMs = 0;
	int m_lastGapMs = 0;
	int m_minGapMs = 0;
	unsigned int m_suppressed = 0;

	int m_clientX = 0;
	int m_clientY = 0;

	unsigned int m_frames = 0;
	unsigned int m_moves = 0;
	// VGUI's own cursor, which the desktop cannot clamp. See vgui_input.h.
	VGuiInput m_vguiInput;
	// How often, and how badly, Windows had to pull the OS cursor back onto
	// the desktop. Non-zero means the window is bigger than the display and
	// the OS route alone would mis-click by this many pixels.
	unsigned int m_osClamped = 0;
	int m_osClampWorst = 0;
	unsigned int m_clicks = 0;

	const char* m_lastReason = "";
};

} // namespace sinvr
