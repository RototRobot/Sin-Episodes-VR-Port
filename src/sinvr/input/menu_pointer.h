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
#include <string.h>
#include "../vr/vr_backend.h"
#include "../vr_camera.h"
#include "../sdk/debug_overlay.h"
#include "../sdk/vgui_input.h"
#include "../render/menu_panel.h"
#include "../render/d3d9_present_hook.h"
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

	// ---- DIRECT: TELL THE GAME WINDOW, DO NOT MOVE THE CURSOR -------------
	//
	// true  = post WM_MOUSEMOVE / WM_LBUTTONDOWN / WM_LBUTTONUP straight to the
	//         game window, with the position in the message.
	// false = the old route: SetCursorPos, then a SendInput click.
	//
	// The old route has two faults this one cannot have. Windows will not put
	// its cursor outside the virtual desktop, and with vr_allow_oversize_window
	// the game window is taller than the desktop -- measured, clicks landing up
	// to 712 px short. And SendInput delivers to whatever window is topmost at
	// the cursor, which stopped being the game the moment the mod had a window
	// of its own.
	//
	// CORRECTED 2026-09-11. This first rested on "no module imports
	// GetCursorPos", which was true of the six modules checked and false of the
	// game: engine, vgui2, vguimatsurface and GameUI all import g_pVCR from
	// tier0.dll -- Source's record/playback layer -- and it is tier0 that
	// imports GetCursorPos, ScreenToClient and GetKeyState. VGUI POLLS the real
	// cursor through it every frame, which is why posted moves on their own
	// produced no hover at all. DirectShield answers that poll with the
	// pointer's position while a menu is up.
	bool direct = true;

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

		// ---- TWO SPACES, WHICH USED TO BE ONE -----------------------------
		//
		// VGUI hit-tests in the ENGINE's screen space, which is the backbuffer.
		// The OS cursor lives in the WINDOW's client space. Those were the same
		// number for every build until the desktop mirror was allowed to be
		// smaller than the render (ConfigureDesktopWindow), and after that,
		// mapping both from GetClientRect puts every click short of the drawn
		// cursor by the shrink factor -- zero error at the top-left corner
		// growing to the full error at the bottom-right, which is the shape of
		// bug that reads as "the menu is nearly right".
		//
		// So the fraction is computed once and spent twice, once in each space.
		// They are identical numbers whenever the mirror has not been shrunk,
		// so this costs nothing in the ordinary case.
		unsigned int rw = 0, rh = 0;
		D3D9RenderSize( rw, rh );
		if ( rw == 0 || rh == 0 )
		{
			// Before the first Present there is no backbuffer to ask about, and
			// until one exists the window still IS the render.
			rw = (unsigned int)w;
			rh = (unsigned int)h;
		}

		// hy is already in screen-fraction orientation (fb > ft means +y is
		// DOWN the screen), so it is not flipped again here.
		const float fx = hx * 0.5f + 0.5f;
		const float fy = hy * 0.5f + 0.5f;

		// Engine space -- this is the one that decides what was clicked.
		POINT pv;
		pv.x = ClampAxis( (LONG)( fx * (float)rw ), (int)rw );
		pv.y = ClampAxis( (LONG)( fy * (float)rh ), (int)rh );

		// Client space -- where Windows will actually put the pointer, and so
		// which window SendInput's click gets delivered to. It has to be inside
		// the game window or the click lands on whatever is behind it.
		POINT p;
		p.x = ClampAxis( (LONG)( fx * (float)w ), w );
		p.y = ClampAxis( (LONG)( fy * (float)h ), h );

		// The engine-space point is the one worth reporting: it is what the hit
		// test used.
		m_clientX = pv.x;
		m_clientY = pv.y;

		// ---- DIRECT: A MESSAGE TO THE GAME WINDOW, NO OS CURSOR ------------
		//
		// The position goes in the message's lParam, in the game window's own
		// client coordinates, and Windows never places a cursor -- so there is
		// no desktop edge to clamp against, and nothing on top of the game
		// window can intercept it. `pv` is the engine-space point, which is
		// also the game window's client space: the game window is never resized
		// (see the desktop mirror in d3d9_present_hook.cpp for why it must not
		// be), so the two are the same numbers.
		//
		// The OS cursor is deliberately NOT moved as well. A cursor move is an
		// INPUT message, and input messages are retrieved after posted ones --
		// so wherever Windows clamped it to, that position would arrive second
		// and overwrite the correct one. Moving it would reintroduce the exact
		// defect this route exists to remove.
		//
		// The VGUI slot writer is bypassed too: it existed only to dodge the
		// same clamp by a different door, and its slot check fails on this
		// build anyway.
		if ( m_settings.direct )
		{
			LogWindowRectOnce( wnd );

			// The shield goes on the first time the pointer drives. See
			// DirectShield: it is what stops anything else's cursor moves from
			// overwriting the VR pointer's.
			const bool shielded = InstallShield( wnd );

			m_postWnd = wnd;
			m_postX = pv.x;
			m_postY = pv.y;

			DirectShield& shield = Shield();
			shield.pointerX = pv.x;
			shield.pointerY = pv.y;
			shield.lastPostTick = GetTickCount();

			// The same point in SCREEN coordinates, for the poll. ClientToScreen
			// is plain arithmetic and is NOT clamped to the desktop, which is the
			// whole reason this reaches rows SetCursorPos never could: tier0
			// hands it straight to ScreenToClient, which undoes it exactly.
			POINT screen = { pv.x, pv.y };
			ClientToScreen( wnd, &screen );
			shield.pointerScreen = screen;
			InstallPoll();

			// MK_LBUTTON while held, so a drag -- a scrollbar, a slider -- reads
			// as a drag and not as a hover. The tag marks the message as ours
			// for the shield, which strips it before the game sees it; it is
			// only ever set when the shield is there to strip it.
			const WPARAM buttons = ( m_clickHeld ? MK_LBUTTON : 0 ) |
								   ( shielded ? (WPARAM)kOurTag : 0 );
			if ( PostMessageW( wnd, WM_MOUSEMOVE, buttons,
							   MAKELPARAM( (WORD)pv.x, (WORD)pv.y ) ) )
			{
				++m_moves;
				++m_posted;
				m_lastReason = "";
			}
			else
			{
				m_lastReason = "PostMessage WM_MOUSEMOVE refused";
			}

			if ( m_settings.click )
				DriveClick( vr.Input().attack );
			return;
		}

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
		const bool viaVgui = m_vguiInput.SetCursorPos( pv.x, pv.y );

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

		// The menu is gone: stop answering the cursor poll and stop shielding
		// NOW, not kDrivingMs later. Outside a menu the poll is the engine's
		// own, and a stale pointer position would read as a mouse movement.
		DirectShield& shield = Shield();
		shield.lastPostTick = 0;
		shield.clickHeld = false;
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
		if ( m_settings.direct )
		{
			Log( "pointer: cursor via DIRECT window messages to %p | %u move(s) "
				 "posted | no OS cursor involved, so nothing is clamped and "
				 "nothing on top of the game window can intercept a click",
				 m_postWnd, m_posted );

			// Whether anything else was talking over the pointer. oursSeen far
			// below m_posted would mean messages lost on the way; foreign moves
			// are what killed hover before the shield existed.
			const DirectShield& shield = Shield();
			if ( shield.installed )
				Log( "pointer shield: %u of our message(s) arrived | dropped %u "
					 "foreign move(s) and %u foreign button message(s) while the "
					 "VR pointer was driving%s",
					 shield.oursSeen, shield.foreignMoves, shield.foreignButtons,
					 shield.haveForeign ? " -- the first few are logged in full above"
										: "" );
			else
				Log( "pointer shield: %s", shield.failed
					 ? "FAILED to install -- anything else moving the cursor still "
					   "overwrites the VR pointer"
					 : "not installed yet (no menu frame has driven the pointer)" );

			// THE number that decides this. Zero answered while the pointer has
			// been posting means VGUI does not poll through tier0 after all.
			if ( shield.pollInstalled )
				Log( "pointer poll: answered %u cursor poll(s) with the pointer, "
					 "passed %u through | answered %u VK_LBUTTON read(s)%s",
					 shield.pollsAnswered, shield.pollsPassed, shield.keysAnswered,
					 ( shield.pollsAnswered == 0 && m_posted > 0 )
						 ? "  <-- ZERO while the pointer drove: VGUI is not "
						   "polling through tier0 after all"
						 : "" );
			else
				Log( "pointer poll: %s", shield.pollFailed
					 ? "FAILED -- see the warning above; hover cannot follow the "
					   "pointer"
					 : "not installed yet" );
		}
		else
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

		SendButton( held );
		if ( held )
			++m_clicks;
	}

	// One button edge, by whichever route is configured -- the single place
	// that knows there are two, so a press and its release can never go by
	// different routes and leave VGUI holding a button nobody will release.
	//
	// Direct mode posts only DOWN and UP, never WM_LBUTTONDBLCLK: Windows
	// synthesises double-clicks from real input only. That is a side benefit,
	// not the purpose -- a double press is what toggled the ComboBox shut in
	// the report that clickDebounceMs was written for.
	void SendButton( bool down )
	{
		if ( m_settings.direct )
		{
			if ( !m_postWnd || !IsWindow( m_postWnd ) )
				return;             // no position ever sent -- nothing to press
			Shield().clickHeld = down;
			PostMessageW( m_postWnd, down ? WM_LBUTTONDOWN : WM_LBUTTONUP,
						  ( down ? MK_LBUTTON : 0 ) |
							  ( Shield().installed ? (WPARAM)kOurTag : 0 ),
						  MAKELPARAM( (WORD)m_postX, (WORD)m_postY ) );
			return;
		}

		INPUT in = {};
		in.type = INPUT_MOUSE;
		in.mi.dwFlags = down ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP;
		SendInput( 1, &in, sizeof( in ) );
	}

	// ---- THE SHIELD: WHILE THE VR POINTER DRIVES, IT IS THE ONLY MOUSE ----
	//
	// MEASURED AFTERWARDS, and the reading below turned out WRONG: over 5,681
	// of the pointer's own messages the shield dropped zero foreign ones.
	// Nothing was talking over the pointer. The game was not listening to
	// moves at all -- it polls the cursor through tier0 (see Hook_GetCursorPos
	// below). The shield is kept because it is cheap and because it is the
	// instrument that ruled its own theory out; if anything ever does talk over
	// the pointer, it will be dropped and logged.
	//
	// MEASURED 2026-09-11, desktop at 1080p: 759 moves posted, 11 clicks sent,
	// the main menu's Quit clicked -- and NOTHING highlighted, not even items in
	// the part of the menu that is on the desktop, and the confirm dialog that
	// followed could not be clicked at all.
	//
	// That combination has one reading. Something else is sending the game
	// window REAL cursor moves at some other position. Windows retrieves posted
	// messages before input messages, so each frame the pointer's move is read
	// first and a real one lands on top of it: hover never survives long enough
	// to be drawn. A main-menu item fires on the PRESS, whose message carries
	// its own position, so it still works. A dialog button fires on the RELEASE,
	// and only if it is still armed -- and a real move in between takes the
	// cursor off it and disarms it. Quit worked; its dialog did not.
	//
	// Who sends them is not known yet: engine, vgui2, vguimatsurface and GameUI
	// all import SetCursorPos. The first few dropped are logged in full, with
	// the real cursor's position and the client centre, which is enough to tell
	// an engine recentre from a VGUI cursor sync.
	//
	// So while the pointer is driving, the game window hears only the pointer.
	// Our own messages carry a tag in a wParam bit MK_* never uses; the shield
	// strips it before passing them on, so the game sees ordinary messages.
	// Anything UNtagged in the mouse range while the pointer drove within the
	// last kDrivingMs is dropped. Nothing is dropped at any other time, so the
	// desk mouse works normally once the controller stops pointing at a menu.
	//
	// Tagged rather than matched by position, deliberately: a move is posted
	// every frame and read a frame later, so "is this at the pointer's current
	// position?" would call the pointer's own previous move foreign and drop it.
	//
	// The subclass is permanent once installed. It sits above DXVK's hook, and
	// DXVK only restores a window proc when its own is on top, so it will not
	// cut this one out -- and DXVK's swapchain window is the mirror now, so the
	// game window's DXVK entry is not torn down while the game runs.
	struct DirectShield
	{
		WNDPROC prev = nullptr;
		bool unicode = false;
		bool installed = false;
		bool failed = false;
		HWND wnd = nullptr;

		DWORD lastPostTick = 0;
		int pointerX = 0;
		int pointerY = 0;

		unsigned int oursSeen = 0;
		unsigned int foreignMoves = 0;
		unsigned int foreignButtons = 0;
		bool haveForeign = false;
		unsigned int reported = 0;

		// The poll: tier0's imports of GetCursorPos and GetKeyState, redirected
		// while the pointer drives a menu.
		POINT pointerScreen = { 0, 0 };
		bool clickHeld = false;
		bool pollInstalled = false;
		bool pollFailed = false;
		void* realGetCursorPos = nullptr;
		void* realGetKeyState = nullptr;
		unsigned int pollsAnswered = 0;
		unsigned int pollsPassed = 0;
		unsigned int keysAnswered = 0;
	};

	enum : unsigned long
	{
		kOurTag = 0x40000000UL,   // a wParam bit MK_* never uses
		kDrivingMs = 150,         // the pointer posts every frame; ~13 frames
	};

	static DirectShield& Shield()
	{
		static DirectShield s;
		return s;
	}

	static bool InstallShield( HWND wnd )
	{
		DirectShield& s = Shield();
		if ( s.installed )
			return s.wnd == wnd;
		if ( s.failed || !wnd )
			return false;

		s.unicode = ( IsWindowUnicode( wnd ) != FALSE );
		const LONG_PTR prev = s.unicode
			? SetWindowLongPtrW( wnd, GWLP_WNDPROC, (LONG_PTR)&ShieldProc )
			: SetWindowLongPtrA( wnd, GWLP_WNDPROC, (LONG_PTR)&ShieldProc );
		if ( !prev )
		{
			s.failed = true;
			LogWarn( "pointer shield: could not subclass the game window (err=%lu) "
					 "-- the pointer still posts, but anything else moving the "
					 "cursor can overwrite it", (unsigned long)GetLastError() );
			return false;
		}

		s.prev = (WNDPROC)prev;
		s.wnd = wnd;
		s.installed = true;
		Log( "pointer shield: installed on the game window %p -- while the VR "
			 "pointer drives, the game hears only the pointer", wnd );
		return true;
	}

	static void NoteForeign( DirectShield& s, HWND wnd, UINT msg, LPARAM lp )
	{
		s.haveForeign = true;
		if ( s.reported >= 6 )
			return;
		++s.reported;

		POINT real = {};
		const bool haveReal = ( GetCursorPos( &real ) != FALSE ) &&
							  ( ScreenToClient( wnd, &real ) != FALSE );
		RECT rc = {};
		GetClientRect( wnd, &rc );

		Log( "pointer shield: dropped a foreign %s at client (%d,%d) -- the VR "
			 "pointer is at (%d,%d); real Windows cursor at client %s(%ld,%ld); "
			 "client centre (%ld,%ld)",
			 ( msg == WM_MOUSEMOVE ) ? "WM_MOUSEMOVE" : "mouse button message",
			 (int)(short)LOWORD( lp ), (int)(short)HIWORD( lp ),
			 s.pointerX, s.pointerY,
			 haveReal ? "" : "(unknown) ", real.x, real.y,
			 rc.right / 2, rc.bottom / 2 );
	}

	// ---- THE POLL: WHERE VGUI ACTUALLY LEARNS THE CURSOR POSITION --------
	//
	// Found 2026-09-11, by scanning ALL 42 of the game's modules instead of the
	// six that looked relevant. engine, vgui2, vguimatsurface and GameUI never
	// import GetCursorPos; they import g_pVCR from tier0.dll, and tier0 imports
	// GetCursorPos, ScreenToClient and GetKeyState. That is Source's VCR layer:
	// every such call is routed through tier0 so a session can be recorded and
	// replayed. So VGUI POLLS the real cursor, once a frame, through tier0.
	//
	// That one fact explains every earlier result. Posted moves produced no
	// hover because hover is not driven by moves. The shield caught nothing
	// because nothing was fighting. Clicks landed wherever the real cursor sat,
	// not where the controller pointed -- which is how the Options menu got a
	// click that reset the video mode to 720x480.
	//
	// The fix is surgical: tier0's OWN import table entries are redirected, so
	// only calls that go through tier0 are affected. The mod's own GetCursorPos
	// calls, DXVK's and Steam's are not. While the pointer drives a menu, the
	// poll is answered with the pointer's position in screen coordinates --
	// unclamped, because it comes from ClientToScreen rather than from where
	// Windows was willing to put a cursor -- and tier0's ScreenToClient turns it
	// straight back into the pointer's client position. At every other moment
	// both calls go through to Windows untouched; Release() shuts it off on the
	// first frame without a menu.
	static bool Driving( const DirectShield& s )
	{
		return s.lastPostTick && ( GetTickCount() - s.lastPostTick ) < (DWORD)kDrivingMs;
	}

	static BOOL WINAPI Hook_GetCursorPos( LPPOINT pt )
	{
		DirectShield& s = Shield();
		if ( pt && Driving( s ) )
		{
			*pt = s.pointerScreen;
			++s.pollsAnswered;
			return TRUE;
		}
		++s.pollsPassed;
		using Fn = BOOL( WINAPI* )( LPPOINT );
		return s.realGetCursorPos ? reinterpret_cast<Fn>( s.realGetCursorPos )( pt )
								  : FALSE;
	}

	static SHORT WINAPI Hook_GetKeyState( int vk )
	{
		DirectShield& s = Shield();
		using Fn = SHORT( WINAPI* )( int );
		const SHORT real = s.realGetKeyState
			? reinterpret_cast<Fn>( s.realGetKeyState )( vk ) : 0;

		// Only the left button, only while the pointer drives. Posted button
		// messages do not update the key-state table, so without this a
		// "is the button still down?" check would disagree with the press it
		// just received. Every other key goes through untouched.
		if ( vk == VK_LBUTTON && Driving( s ) )
		{
			++s.keysAnswered;
			return (SHORT)( ( s.clickHeld ? 0x8000 : 0 ) | ( real & 1 ) );
		}
		return real;
	}

	// Redirect one imported function in one module's import table. `original`
	// is written BEFORE the slot, so a call on another thread can never reach
	// the replacement while the original is still unknown. Matches by name, or
	// by bound address when the module carries no name table.
	static bool PatchImport( HMODULE module, const char* dll, const char* func,
							 void* replacement, void** original )
	{
		BYTE* base = reinterpret_cast<BYTE*>( module );
		const IMAGE_DOS_HEADER* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>( base );
		if ( dos->e_magic != IMAGE_DOS_SIGNATURE )
			return false;
		const IMAGE_NT_HEADERS* nt =
			reinterpret_cast<const IMAGE_NT_HEADERS*>( base + dos->e_lfanew );
		if ( nt->Signature != IMAGE_NT_SIGNATURE )
			return false;
		const IMAGE_DATA_DIRECTORY& dir =
			nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
		if ( !dir.VirtualAddress )
			return false;

		HMODULE target = GetModuleHandleA( dll );
		void* bound = target ? reinterpret_cast<void*>( GetProcAddress( target, func ) )
							 : nullptr;

		for ( const IMAGE_IMPORT_DESCRIPTOR* imp =
				  reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>( base + dir.VirtualAddress );
			  imp->Name; ++imp )
		{
			if ( _stricmp( reinterpret_cast<const char*>( base + imp->Name ), dll ) != 0 )
				continue;

			IMAGE_THUNK_DATA* slots =
				reinterpret_cast<IMAGE_THUNK_DATA*>( base + imp->FirstThunk );
			const IMAGE_THUNK_DATA* names = imp->OriginalFirstThunk
				? reinterpret_cast<const IMAGE_THUNK_DATA*>( base + imp->OriginalFirstThunk )
				: nullptr;

			for ( int i = 0; slots[i].u1.Function; ++i )
			{
				bool match = false;
				if ( names )
				{
					if ( !IMAGE_SNAP_BY_ORDINAL( names[i].u1.Ordinal ) )
					{
						const IMAGE_IMPORT_BY_NAME* by =
							reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(
								base + names[i].u1.AddressOfData );
						match = ( strcmp( reinterpret_cast<const char*>( by->Name ),
										  func ) == 0 );
					}
				}
				else if ( bound )
				{
					match = ( reinterpret_cast<void*>( slots[i].u1.Function ) == bound );
				}
				if ( !match )
					continue;

				DWORD old = 0;
				if ( !VirtualProtect( &slots[i].u1.Function, sizeof( slots[i].u1.Function ),
									  PAGE_READWRITE, &old ) )
					return false;
				*original = reinterpret_cast<void*>( slots[i].u1.Function );
				slots[i].u1.Function = reinterpret_cast<ULONG_PTR>( replacement );
				VirtualProtect( &slots[i].u1.Function, sizeof( slots[i].u1.Function ),
								old, &old );
				return true;
			}
		}
		return false;
	}

	static void InstallPoll()
	{
		DirectShield& s = Shield();
		if ( s.pollInstalled || s.pollFailed )
			return;

		HMODULE tier0 = GetModuleHandleA( "tier0.dll" );
		if ( !tier0 )
		{
			s.pollFailed = true;
			LogWarn( "pointer poll: tier0.dll is not loaded -- the menu's cursor "
					 "poll cannot be answered, so hover will not follow the "
					 "pointer" );
			return;
		}

		const bool cursor = PatchImport( tier0, "user32.dll", "GetCursorPos",
										 reinterpret_cast<void*>( &Hook_GetCursorPos ),
										 &s.realGetCursorPos );
		const bool keys = PatchImport( tier0, "user32.dll", "GetKeyState",
									   reinterpret_cast<void*>( &Hook_GetKeyState ),
									   &s.realGetKeyState );
		if ( !cursor )
		{
			s.pollFailed = true;
			LogWarn( "pointer poll: could not find tier0's import of GetCursorPos "
					 "-- hover will not follow the pointer" );
			return;
		}

		s.pollInstalled = true;
		Log( "pointer poll: tier0's GetCursorPos%s now answered with the VR "
			 "pointer while a menu is up (originals %p / %p) -- the call VGUI "
			 "reaches through g_pVCR every frame",
			 keys ? " and GetKeyState(VK_LBUTTON) are" : " is",
			 s.realGetCursorPos, s.realGetKeyState );
	}

	static LRESULT CALLBACK ShieldProc( HWND wnd, UINT msg, WPARAM wp, LPARAM lp )
	{
		DirectShield& s = Shield();
		const WNDPROC prev = s.prev;
		const bool unicode = s.unicode;

		if ( msg >= WM_MOUSEMOVE && msg <= WM_MBUTTONDBLCLK )
		{
			if ( wp & (WPARAM)kOurTag )
			{
				wp &= ~(WPARAM)kOurTag;
				++s.oursSeen;
			}
			else if ( s.lastPostTick &&
					  ( GetTickCount() - s.lastPostTick ) < (DWORD)kDrivingMs )
			{
				if ( msg == WM_MOUSEMOVE )
					++s.foreignMoves;
				else
					++s.foreignButtons;
				NoteForeign( s, wnd, msg, lp );
				return 0;
			}
		}
		else if ( msg == WM_NCDESTROY )
		{
			// The window's last message: nothing calls through this proc for it
			// again, so forget it rather than keep a stale handle.
			s.installed = false;
			s.wnd = nullptr;
		}

		return unicode ? CallWindowProcW( prev, wnd, msg, wp, lp )
					   : CallWindowProcA( prev, wnd, msg, wp, lp );
	}

	// ---- WHERE IS THE GAME WINDOW, REALLY? --------------------------------
	//
	// The clamp was measured at 712 px when the size difference alone predicts
	// 232, which says the window is not where it was assumed to be -- most
	// likely centred, and so hanging off the TOP as well as the bottom. This
	// reports the client area in screen space against the virtual desktop,
	// once, so the overhang is a number rather than an inference.
	//
	// ClientToScreen is plain arithmetic and is not clamped, which is what makes
	// it usable for this.
	void LogWindowRectOnce( HWND wnd )
	{
		if ( m_rectLogged || !wnd )
			return;
		m_rectLogged = true;

		RECT rc = {};
		if ( !GetClientRect( wnd, &rc ) )
			return;
		POINT tl = { 0, 0 };
		POINT br = { rc.right, rc.bottom };
		if ( !ClientToScreen( wnd, &tl ) || !ClientToScreen( wnd, &br ) )
			return;

		const int vx = GetSystemMetrics( SM_XVIRTUALSCREEN );
		const int vy = GetSystemMetrics( SM_YVIRTUALSCREEN );
		const int vw = GetSystemMetrics( SM_CXVIRTUALSCREEN );
		const int vh = GetSystemMetrics( SM_CYVIRTUALSCREEN );

		const long offTop = ( tl.y < vy ) ? ( vy - tl.y ) : 0;
		const long offBottom = ( br.y > vy + vh ) ? ( br.y - ( vy + vh ) ) : 0;
		const long offLeft = ( tl.x < vx ) ? ( vx - tl.x ) : 0;
		const long offRight = ( br.x > vx + vw ) ? ( br.x - ( vx + vw ) ) : 0;

		Log( "pointer: game client area on screen (%ld,%ld)-(%ld,%ld), virtual "
			 "desktop (%d,%d) %dx%d -- off the top %ld, bottom %ld, left %ld, "
			 "right %ld px. %s",
			 tl.x, tl.y, br.x, br.y, vx, vy, vw, vh,
			 offTop, offBottom, offLeft, offRight,
			 ( offTop | offBottom | offLeft | offRight )
				 ? "The old cursor route could not reach those rows; the direct "
				   "route does not use the cursor, so it can."
				 : "All of it is on the desktop." );
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

		SendButton( false );
	}

	// The game's own top-level window, found by process rather than by class
	// name or title -- both of which are the game's to change. Cached, and
	// re-found if it ever goes away.
	// The Present hook knows which window the game actually renders into,
	// because it is handed it. That is authoritative; the enumeration below is
	// only a fallback for before the first Present.
	HWND GameWindow()
	{
		if ( m_window && IsWindow( m_window ) )
			return m_window;

		// Ask the Present hook first. It does not search for the window, it is
		// GIVEN it, so it cannot pick the wrong one -- which the enumeration
		// below can, now that the mod makes a window of its own.
		m_window = D3D9GameWindow();
		if ( !m_window )
			EnumWindows( &MenuPointer::EnumProc, reinterpret_cast<LPARAM>( this ) );

		if ( m_window && !m_windowLogged )
		{
			m_windowLogged = true;
			RECT rc = {};
			GetClientRect( m_window, &rc );
			Log( "pointer: game window %p, client %ldx%ld (mirror is %p) -- "
				 "found %s", m_window, rc.right - rc.left, rc.bottom - rc.top,
				 D3D9MirrorWindow(),
				 D3D9GameWindow() ? "from the Present hook" : "by enumeration" );
		}
		return m_window;
	}

	// Clamp into [0, extent) -- one axis, in whichever space the caller is
	// working in. Shared so the engine-space and client-space points cannot
	// drift into two different clamping rules.
	static LONG ClampAxis( LONG v, int extent )
	{
		if ( extent <= 0 )
			return 0;
		if ( v < 0 )
			return 0;
		if ( v >= extent )
			return (LONG)( extent - 1 );
		return v;
	}

	static BOOL CALLBACK EnumProc( HWND wnd, LPARAM param )
	{
		DWORD pid = 0;
		GetWindowThreadProcessId( wnd, &pid );
		if ( pid != GetCurrentProcessId() )
			return TRUE;
		if ( !IsWindowVisible( wnd ) )
			return TRUE;

		// ---- NOT OUR OWN MIRROR -------------------------------------------
		//
		// The desktop mirror is a visible window of this process and is far
		// larger than the 200 px filter below, so it matches every test this
		// function makes. Worse, EnumWindows walks in Z ORDER and the mirror
		// sits on top, so it is found FIRST -- the cursor would be placed in
		// its client area and the click delivered to it, which is a menu that
		// cannot be operated at all.
		if ( wnd == D3D9MirrorWindow() )
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

	// Direct route: where the last move was posted, so the button edges that
	// follow land on the same point and go to the same window.
	HWND m_postWnd = nullptr;
	int m_postX = 0;
	int m_postY = 0;
	unsigned int m_posted = 0;
	bool m_rectLogged = false;

	const char* m_lastReason = "";
};

} // namespace sinvr
