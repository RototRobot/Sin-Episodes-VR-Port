#pragma once

// vgui::IInput -- setting the menu cursor WITHOUT going through the OS.
//
// ---- WHY THIS EXISTS ------------------------------------------------------
//
// The menu pointer used to place the cursor with Win32 `SetCursorPos`, which
// cannot put the cursor outside the virtual desktop. That was invisible until
// `vr_allow_oversize_window` let the game window grow past the monitor:
//
//     virtual desktop 3840x2160      game client 2444x2392
//
//     asked y=2159  ->  got y=2159
//     asked y=2200  ->  got y=2159   clamped by 41
//     asked y=2391  ->  got y=2159   clamped by 232
//
// So the bottom 232 rows of the menu were unreachable. The D3D9 cursor is
// drawn by PROJECTING a world point into the backbuffer and is not clamped by
// anything, so the drawn cursor and the actual click drifted apart -- by zero
// over most of the screen and by the full overhang at the bottom. Reported
// from the headset as "the cursor no longer lines up with where I click".
//
// VGUI keeps its own cursor position in its own coordinate space. Setting it
// there has no desktop to be clamped to. The SDK's own `vgui_int.cpp` drives
// the cursor through exactly this call, so this is the intended route.
//
// ---- SLOTS ARE VERIFIED, NOT TRUSTED --------------------------------------
//
// SDK 2004's IInput.h puts SetCursorPos at 5 and GetCursorPos at 6, and this
// game exports `VGUI_Input005` -- the same version string the header declares.
// That is encouraging and is NOT proof: Ritual shifted vtable slots in four
// other interfaces this project uses, and `IMaterialSystem`'s shift is not even
// uniform across the class.
//
// So the layout is proved with the READ before anything is written through it:
// call GetCursorPos (slot 6) and compare it against the OS cursor converted to
// client space. Those agree only if slot 6 really is GetCursorPos, and if the
// pair is where the header says, slot 5 is SetCursorPos. Until that check
// passes the writer stays disabled and the caller keeps the old path.
//
// A read is used for the probe deliberately: calling a WRONG slot that takes
// two ints could be anything at all, including something that alters state.

#include "source_interfaces.h"
#include "../render/d3d9_present_hook.h"

#include <windows.h>

namespace sinvr {

namespace vgui_input_slot {
// From SDK 2004 public/vgui/IInput.h, counted from the top of the class.
constexpr int kSetCursorPos = 5;
constexpr int kGetCursorPos = 6;
}

class VGuiInput
{
public:
	// `vgui2.dll` is loaded by the engine long before any map. Binding is
	// deferred rather than latched: a failure here must not be permanent,
	// because a not-ready interface that latches a refusal has cost this
	// project two separate sessions.
	bool Bind()
	{
		if ( m_iface )
			return true;

		// ---- OFF UNLESS EXPLICITLY ASKED FOR --------------------------------
		//
		// This crashed the game at launch. The slot check below passed on
		// GARBAGE (see the note on Verify), so slot 5 was then called with two
		// ints on a vtable where it is not SetCursorPos, and the process died
		// before reaching the menu.
		//
		// The route is not disproved -- the CHECK was wrong, not necessarily
		// the premise -- but it must never again be on by default while
		// unproven. Set SINVR_VGUI_CURSOR=1 to try it deliberately.
		if ( !m_gateChecked )
		{
			m_gateChecked = true;
			char buf[8] = {};
			m_gateOpen = ( GetEnvironmentVariableA( "SINVR_VGUI_CURSOR", buf,
													sizeof( buf ) ) > 0 &&
						   buf[0] == '1' );
			if ( !m_gateOpen )
				Log( "vgui cursor: OFF (default). The menu cursor uses Win32 "
					 "SetCursorPos, which cannot reach past the desktop edge -- "
					 "so with a window taller than your display, clicks below "
					 "that edge land short of the drawn cursor. Set "
					 "SINVR_VGUI_CURSOR=1 to try the VGUI route." );
		}
		if ( !m_gateOpen )
			return false;

		m_iface = GetInterface( "vgui2.dll", "VGUI_Input005" );
		if ( !m_iface )
		{
			++m_bindAttempts;
			return false;
		}

		Log( "vgui input: VGUI_Input005 at %p after %u attempt(s) -- slots "
			 "unproven until the first cursor read", m_iface, m_bindAttempts + 1 );
		return true;
	}

	bool Valid() const { return m_iface != nullptr; }
	bool Verified() const { return m_verified; }
	bool VerifyFailed() const { return m_verifyFailed; }

	// ---- THE PROOF --------------------------------------------------------
	//
	// Read VGUI's cursor and compare it against the OS cursor in client space.
	// They track each other in normal operation because VGUI's position comes
	// from the same mouse the OS is tracking -- so agreement means slot 6 is
	// GetCursorPos, and disagreement by a large margin means the slot is wrong
	// and nothing should be written.
	//
	// `tolerance` is generous on purpose. The two are sampled at slightly
	// different moments and a moving mouse legitimately differs by a few
	// pixels; what this has to separate is "a few pixels apart" from "an
	// arbitrary integer out of the wrong vtable slot".
	bool Verify( HWND window, int tolerance = 64 )
	{
		if ( m_verified )
			return true;
		if ( !m_iface || !window )
			return false;

		POINT os = {};
		if ( !GetCursorPos( &os ) || !ScreenToClient( window, &os ) )
			return false;

		// Only meaningful while the OS cursor is genuinely inside the client
		// area. Outside it the two are not describing the same thing and a
		// mismatch would prove nothing -- exactly the sort of test that cannot
		// produce the positive it is looking for.
		RECT rc = {};
		if ( !GetClientRect( window, &rc ) )
			return false;
		if ( os.x < 0 || os.y < 0 || os.x >= rc.right || os.y >= rc.bottom )
			return false;

		// ---- THE CHECK THAT WAS WRONG, AND HOW ---------------------------
		//
		// This block previously reported CONFIRMED on a call that wrote
		// NOTHING, and the game then crashed calling slot 5. Two mistakes,
		// both worth keeping written down:
		//
		//  1. The sentinel was never tested. The callee left both values at
		//     -2147483647, which the log printed verbatim -- so the evidence
		//     that the call had failed was on screen and unexamined.
		//  2. The comparison OVERFLOWED. `os.x - vx` with vx = -2147483647 is
		//     2147484934, which does not fit in an int, wraps negative, and
		//     sails through `dx <= tolerance`. The guard rubber-stamped the
		//     exact garbage it existed to catch.
		//
		// So: seed with a sentinel, require it to have been overwritten, do the
		// arithmetic in 64 bits, and bound the values to something a cursor
		// could actually be before believing any of it.
		const int kSentinelX = -1431655766;   // 0xAAAAAAAA
		const int kSentinelY = -1431655766;
		int vx = kSentinelX, vy = kSentinelY;
		GetCursorPosRaw( vx, vy );

		++m_verifyAttempts;

		if ( vx == kSentinelX && vy == kSentinelY )
		{
			if ( !m_verifyFailed )
			{
				m_verifyFailed = true;
				LogWarn( "vgui cursor: slot %d did NOT write the output values -- "
						 "they came back as the sentinel this code seeded them "
						 "with. That is not GetCursorPos on this build, so slot "
						 "%d is not SetCursorPos either and nothing will be "
						 "called through it.",
						 vgui_input_slot::kGetCursorPos,
						 vgui_input_slot::kSetCursorPos );
			}
			return false;
		}

		// A cursor cannot be at +-2 billion. Reject implausible values outright
		// rather than letting them into a subtraction.
		if ( vx < -32768 || vx > 65535 || vy < -32768 || vy > 65535 )
		{
			if ( !m_verifyFailed )
			{
				m_verifyFailed = true;
				LogWarn( "vgui cursor: slot %d returned (%d %d), which is not a "
						 "plausible cursor position. Wrong slot; nothing will be "
						 "written through slot %d.",
						 vgui_input_slot::kGetCursorPos, vx, vy,
						 vgui_input_slot::kSetCursorPos );
			}
			return false;
		}

		// ---- CLIENT SPACE IS NOT ALWAYS ENGINE SPACE ---------------------
		//
		// VGUI reports in the ENGINE's screen space, which is the backbuffer.
		// `os` is in the WINDOW's client space. Those are the same number only
		// while the desktop mirror is the size of the render; once the mirror
		// is shrunk (ConfigureDesktopWindow) they differ by the shrink factor.
		//
		// BOTH readings are accepted, deliberately, because which one is right
		// depends on who wrote VGUI's cursor last -- and this check runs before
		// that is settled. Untouched, VGUI holds what the engine copied out of
		// a WM_MOUSEMOVE, which is raw client coordinates. Once the menu
		// pointer starts writing, it holds engine coordinates. Insisting on
		// either one alone would fail a perfectly good pair of slots and
		// disable the writer for exactly the reason it exists.
		//
		// This is not a loosened test. Both candidates are derived from the
		// same OS reading, so a wrong slot still has to land on one of two
		// specific numbers rather than anywhere inside a widened band -- and
		// when the mirror has not been shrunk the two candidates are the same
		// number, so the check is bit-for-bit what it always was.
		unsigned int rw = 0, rh = 0;
		D3D9RenderSize( rw, rh );

		long long wantX = os.x, wantY = os.y;
		long long tolX = tolerance, tolY = tolerance;
		bool engineSpace = false;

		if ( rw && rh && rc.right > 0 && rc.bottom > 0 &&
			 ( (LONG)rw != rc.right || (LONG)rh != rc.bottom ) )
		{
			const double sx = (double)rw / (double)rc.right;
			const double sy = (double)rh / (double)rc.bottom;
			const long long scaledX = (long long)( (double)os.x * sx + 0.5 );
			const long long scaledY = (long long)( (double)os.y * sy + 0.5 );

			// One client pixel is sx engine pixels, so a reading exact to the
			// pixel in client space can be sx out in engine space.
			const long long stolX = (long long)( (double)tolerance * sx ) + 1;
			const long long stolY = (long long)( (double)tolerance * sy ) + 1;

			const long long dsx = ( vx > scaledX ) ? vx - scaledX : scaledX - vx;
			const long long dsy = ( vy > scaledY ) ? vy - scaledY : scaledY - vy;
			if ( dsx <= stolX && dsy <= stolY )
			{
				wantX = scaledX;  wantY = scaledY;
				tolX = stolX;     tolY = stolY;
				engineSpace = true;
			}
		}

		const long long dx = ( vx > wantX ) ? vx - wantX : wantX - vx;
		const long long dy = ( vy > wantY ) ? vy - wantY : wantY - vy;
		if ( dx <= tolX && dy <= tolY )
		{
			m_verified = true;
			Log( "vgui input: slots CONFIRMED after %u sample(s) -- VGUI reports "
				 "the cursor at (%d %d), the OS at (%d %d) in client space, "
				 "matched in %s. Slot %d is GetCursorPos and slot %d is "
				 "SetCursorPos; the menu cursor can now be placed outside the "
				 "desktop.",
				 m_verifyAttempts, vx, vy, os.x, os.y,
				 engineSpace ? "ENGINE space (the desktop mirror is smaller than "
							   "the render)"
							 : "client space",
				 vgui_input_slot::kGetCursorPos, vgui_input_slot::kSetCursorPos );
			return true;
		}

		// One bad sample is not a verdict -- the cursor may simply have moved
		// between the two reads. Several in a row is.
		if ( m_verifyAttempts >= 30 && !m_verifyFailed )
		{
			m_verifyFailed = true;
			LogWarn( "vgui input: slot check FAILED over %u sample(s) -- VGUI says "
					 "(%d %d), the OS says (%d %d). Slot %d is not GetCursorPos on "
					 "this build, so nothing will be written through slot %d. The "
					 "menu cursor falls back to Win32 SetCursorPos, which cannot "
					 "reach past the desktop edge.",
					 m_verifyAttempts, vx, vy, os.x, os.y,
					 vgui_input_slot::kGetCursorPos, vgui_input_slot::kSetCursorPos );
		}
		return false;
	}

	// ---- DOES THIS ACTUALLY BYPASS THE DESKTOP CLAMP? --------------------
	//
	// The whole point of this class is that VGUI keeps its cursor in its own
	// space. That is an ASSUMPTION until measured: if `IInput::SetCursorPos`
	// merely forwards to the Win32 call, it is clamped exactly as before and
	// this class is an elaborate no-op. The SDK ships vgui2's controls but not
	// its core input, so the implementation cannot be read -- only tested.
	//
	// The test writes a position BELOW the desktop and reads it back. VGUI
	// returning what was written means it holds its own state and the clamp is
	// bypassed; VGUI returning a smaller number means it went through Windows
	// and nothing has been gained.
	//
	// One shot, and it restores the position it found. Run only while a menu is
	// up, where the pointer is already moving the cursor every frame.
	void ProbeClamp( HWND window )
	{
		if ( m_probed || !m_verified || !window )
			return;
		m_probed = true;

		RECT rc = {};
		if ( !GetClientRect( window, &rc ) || rc.bottom <= 0 )
			return;

		const int deskH = GetSystemMetrics( SM_CYVIRTUALSCREEN );
		const int deskTop = GetSystemMetrics( SM_YVIRTUALSCREEN );

		int wasX = 0, wasY = 0;
		GetCursorPosRaw( wasX, wasY );

		// The bottom row of the client area. Interesting only when the window
		// really does overhang the desktop -- otherwise there is nothing to
		// clamp and the probe cannot distinguish the two cases, so say that
		// rather than reporting a meaningless pass.
		POINT origin = { 0, rc.bottom - 1 };
		const bool haveScreen = ( ClientToScreen( window, &origin ) != FALSE );
		const bool overhangs = haveScreen && ( origin.y >= deskTop + deskH );

		const int probeY = rc.bottom - 1;
		VCall<vgui_input_slot::kSetCursorPos, void, int, int>( m_iface, wasX, probeY );

		int gotX = 0, gotY = 0;
		GetCursorPosRaw( gotX, gotY );

		VCall<vgui_input_slot::kSetCursorPos, void, int, int>( m_iface, wasX, wasY );

		// Same discipline as Verify: a readback has to be PLAUSIBLE before it
		// is allowed to mean anything. The first version accepted 1920800504 as
		// proof the clamp was bypassed, because it only asked whether the
		// number was large enough.
		const bool plausible = ( gotY >= -32768 && gotY <= 65535 &&
								 gotX >= -32768 && gotX <= 65535 );
		m_bypassesClamp = plausible && ( gotY >= probeY - 2 );
		if ( !plausible )
		{
			LogWarn( "vgui cursor: probe read back (%d %d), which is not a cursor "
					 "position -- the slots are wrong and this route is unusable "
					 "on this build.", gotX, gotY );
			return;
		}

		if ( !overhangs )
		{
			Log( "vgui cursor: probe INCONCLUSIVE -- the %ldx%ld client fits on "
				 "the desktop, so nothing would be clamped either way. Wrote "
				 "y=%d, read back y=%d. This becomes meaningful only with "
				 "vr_allow_oversize_window on a window taller than the display.",
				 rc.right, rc.bottom, probeY, gotY );
			return;
		}

		if ( m_bypassesClamp )
			Log( "vgui cursor: BYPASSES the desktop clamp -- wrote y=%d into a "
				 "%ld-tall client on a %d-tall desktop and read back y=%d. The "
				 "menu is clickable to its bottom edge.",
				 probeY, rc.bottom, deskH, gotY );
		else
			LogWarn( "vgui cursor: DOES NOT bypass the clamp -- wrote y=%d and read "
					 "back y=%d, so IInput::SetCursorPos forwards to Windows and is "
					 "clamped like the old path. The bottom %ld row(s) of the menu "
					 "remain unclickable. Lower the SteamVR resolution until the "
					 "per-eye height fits the desktop, or raise the desktop with "
					 "DSR/VSR.",
					 probeY, gotY, rc.bottom - deskH );
	}

	bool Probed() const { return m_probed; }
	bool BypassesClamp() const { return m_bypassesClamp; }

	// Place the cursor in VGUI's own space. Refuses until the layout is proved.
	bool SetCursorPos( int x, int y )
	{
		if ( !m_iface || !m_verified )
			return false;
		VCall<vgui_input_slot::kSetCursorPos, void, int, int>( m_iface, x, y );
		++m_writes;
		return true;
	}

	void GetCursorPosRaw( int& x, int& y ) const
	{
		if ( !m_iface )
			return;
		VCall<vgui_input_slot::kGetCursorPos, void, int&, int&>( m_iface, x, y );
	}

	unsigned int Writes() const { return m_writes; }

private:
	void* m_iface = nullptr;
	bool m_verified = false;
	bool m_verifyFailed = false;
	unsigned int m_bindAttempts = 0;
	unsigned int m_verifyAttempts = 0;
	unsigned int m_writes = 0;
	bool m_gateChecked = false;
	bool m_gateOpen = false;
	bool m_probed = false;
	bool m_bypassesClamp = false;
};

} // namespace sinvr
