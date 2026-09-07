// IVDebugOverlay -- the engine's own wireframe box drawing, used to make the
// body zones visible while they are being positioned.
//
// ---- THIS IS A KNOWN DEAD END, DELIBERATELY RETRIED --------------------------
//
// HANDOVER.md records an earlier attempt to draw a tracked box with this
// interface. It worked in the sense that the box followed the hand, and failed
// in the sense that it DREW TWICE -- a second copy that lagged only when the
// player walked, never when the hand moved. A counter proved the mod added
// exactly one box per frame, so the duplication was the engine's: overlays are
// drawn outside our per-eye setup.
//
// It is being retried for two reasons rather than out of forgetfulness:
//
//   1. This is scaffolding with a short life. The boxes exist to position the
//      zones over a few sessions and then come out; the zones themselves are
//      invisible and do not depend on any of this. Even a doubled box says
//      where a zone is, which is most of what it is for.
//   2. The mod now OWNS CViewSetup, which it did not when that attempt was
//      made. That is exactly what invalidated the other rendering dead end
//      (raw D3D9 capturing the wrong view matrix), so it is at least possible
//      the picture has changed.
//
// The instrumentation from last time is kept: BoxesDrawn() counts what we
// actually submitted, so "there are two boxes" can be attributed to the engine
// or to us without guessing. If it doubles again, that is a confirmed second
// data point and the answer is the raw-D3D9 path, not more attempts here.
//
// ---- SLOTS -------------------------------------------------------------------
//
// AddBoxOverlay is slot 1 in the SDK's IVDebugOverlay. Unlike the four
// interfaces the mod depends on, this one is NOT verified by disassembly --
// Ritual modified every one of those, and there is no reason to assume this one
// escaped. It is called defensively instead: the interface pointer and its
// vtable are range-checked before use, and every failure disables the drawing
// rather than propagating. That is acceptable HERE, and only here, because
// nothing depends on it: the zones work whether or not a box ever appears.
//
// The two same-named overload pairs further down the interface (AddTextOverlay,
// ScreenPosition) would be emitted by MSVC in reverse declaration order, but
// they sit AFTER slot 1 and reversal never moves a slot outside its own
// same-named group -- so AddBoxOverlay is unaffected either way.
#pragma once

#include <windows.h>
#include "source_interfaces.h"
#include "../../common/log.h"

namespace sinvr {

class DebugOverlay
{
public:
	// `raw` is whatever CreateInterface("VDebugOverlay003") returned; null is a
	// normal outcome and simply means no boxes.
	bool Bind( void* raw )
	{
		m_iface = nullptr;
		if ( !raw )
		{
			Log( "zones: VDebugOverlay003 not available -- zone boxes cannot be "
				 "drawn. The zones themselves are unaffected." );
			return false;
		}

		// A vtable with a plausible slot 1. Cheap, and it is the difference
		// between "no boxes" and calling a garbage address inside the engine.
		void** vtable = *reinterpret_cast<void***>( raw );
		if ( IsBadReadPtr( vtable, sizeof( void* ) * 2 ) ||
			 !vtable[kAddBoxOverlay] ||
			 IsBadCodePtr( reinterpret_cast<FARPROC>( vtable[kAddBoxOverlay] ) ) )
		{
			LogWarn( "zones: VDebugOverlay003 vtable slot %d does not look like "
					 "code -- zone boxes disabled", kAddBoxOverlay );
			return false;
		}

		m_iface = raw;

		// Clearing is separately optional. It is the ghosting fix, but it is
		// also the one call here that reaches further than our own overlays,
		// so a slot that does not look like code disables it and leaves the
		// drawing working.
		m_canClear = !IsBadReadPtr( vtable, sizeof( void* ) * ( kClearAllOverlays + 1 ) ) &&
					 vtable[kClearAllOverlays] != nullptr &&
					 !IsBadCodePtr( reinterpret_cast<FARPROC>( vtable[kClearAllOverlays] ) );

		Log( "zones: VDebugOverlay003 bound at %p (AddBoxOverlay = slot %d, "
				 "ClearAllOverlays = slot %d %s)",
			 raw, kAddBoxOverlay, kClearAllOverlays,
			 m_canClear ? "ok" : "UNAVAILABLE -- expect ghosting" );
		return true;
	}

	bool Valid() const { return m_iface != nullptr; }

	// `origin` and `angles` place the box in the world; `mins`/`maxs` are its
	// corners in that rotated frame. Duration 0 means this frame only, which is
	// what makes a moving box track instead of smearing.
	void Box( const Vector& origin, const Vector& mins, const Vector& maxs,
			  const QAngle& angles, int r, int g, int b, int a, float duration )
	{
		if ( !m_iface )
			return;

		using Fn = void( __thiscall* )( void*, const Vector&, const Vector&,
										const Vector&, const QAngle&,
										int, int, int, int, float );
		void** vtable = *reinterpret_cast<void***>( m_iface );
		auto fn = reinterpret_cast<Fn>( vtable[kAddBoxOverlay] );
		fn( m_iface, origin, mins, maxs, angles, r, g, b, a, duration );
		++m_boxesDrawn;
	}

	// A world-space line. This is what draws the crosshair: the game's own
	// crosshair is a small texture, and getting a TEXTURE onto a world quad
	// means the material system and a mesh -- which is the raw-D3D9 dead end
	// wearing a different hat. Four short lines with a gap in the middle are
	// the same shape, cost nothing, and go through the interface already
	// bound and proven by the zone boxes.
	//
	// `noDepthTest` decides whether walls occlude it. A laser dot wants
	// occlusion; a crosshair you must never lose wants none.
	void Line( const Vector& from, const Vector& to,
		   int r, int g, int b, bool noDepthTest, float duration )
	{
		if ( !m_iface )
			return;

		using Fn = void( __thiscall* )( void*, const Vector&, const Vector&,
					  int, int, int, bool, float );
		void** vtable = *reinterpret_cast<void***>( m_iface );
		auto fn = reinterpret_cast<Fn>( vtable[kAddLineOverlay] );
		fn( m_iface, from, to, r, g, b, noDepthTest, duration );
		++m_boxesDrawn;
	}

	// Drop every overlay the engine is currently holding.
	//
	// This is what actually fixes the ghosting, and the reason is worth
	// stating because it was measured rather than guessed. `duration 0` does
	// NOT mean one frame -- it means roughly one TICK, and at 90 fps against
	// a 66 Hz tick that is about 1.4 frames. So last frame's copy is still in
	// the list when this frame renders, at last frame's world position, and
	// you see both. That is why it ghosted only when the VIEW moved: with a
	// still head the stale copy sits exactly on top of the fresh one.
	//
	// Counted per eye on hardware, with each pass tinted and offset:
	//
	//     left  eye (pass 0): red(prev) green(prev) red(this)          = 3
	//     right eye (pass 1): red(prev) green(prev) red(this) green(this) = 4
	//
	// which is exactly what was reported, and rules out both earlier
	// theories: the list is not consumed per pass, and both passes do draw.
	//
	// Clearing immediately before each pass submits leaves each eye holding
	// precisely one copy, at this frame's position.
	void Clear()
	{
		if ( !m_iface || !m_canClear )
			return;
		using Fn = void( __thiscall* )( void* );
		void** vtable = *reinterpret_cast<void***>( m_iface );
		reinterpret_cast<Fn>( vtable[kClearAllOverlays] )( m_iface );
	}

	// What WE submitted. The whole point of keeping this: last time, the
	// question "are there two boxes because we added two?" was answerable only
	// because this counter existed.
	// Text at a SCREEN position, 0..1 across the viewport.
	//
	// Wanted for one reason: this goes through the mat-system surface, i.e. the
	// 2D/VGUI layer, rather than the world pass the boxes and lines are drawn
	// in. A menu is 2D and is drawn AFTER the world, so a world-space marker is
	// always behind it -- which is exactly what "the cursor appears behind the
	// submenus" was. This is the same list, drawn in the other layer.
	void ScreenText( float x, float y, const char* text,
					 int r, int g, int b, int a, float duration )
	{
		if ( !m_iface || !text )
			return;
		void** vtable = *reinterpret_cast<void***>( m_iface );
		if ( !vtable )
			return;
		using Fn = void( __thiscall* )( void*, float, float, float,
									    int, int, int, int, const char* );
		__try
		{
			reinterpret_cast<Fn>( vtable[kAddScreenTextOverlay] )(
				m_iface, x, y, duration, r, g, b, a, text );
		}
		__except ( EXCEPTION_EXECUTE_HANDLER )
		{
			// A wrong slot here draws nothing rather than taking the frame with
			// it. The rest of the overlay use is unaffected.
			m_screenTextFaulted = true;
		}
	}

	bool ScreenTextFaulted() const { return m_screenTextFaulted; }

	unsigned int BoxesDrawn() const { return m_boxesDrawn; }
	void ResetCount() { m_boxesDrawn = 0; }

private:
	enum { kAddBoxOverlay = 1 };
	enum { kAddLineOverlay = 3 };   // AddTriangleOverlay is 2
	// The 15th virtual, so index 14. The two same-named overload pairs above
	// it (AddTextOverlay, ScreenPosition) are emitted by MSVC in reverse, but
	// reversal never moves a slot outside its own group, so this is unmoved.
	enum { kClearAllOverlays = 14 };
	// AddScreenTextOverlay is the 7th virtual. It sits BETWEEN the two
	// AddTextOverlay overloads (4,5) and AddSweptBoxOverlay (7); MSVC reverses
	// same-named groups but never moves a slot out of its own group, so 6 is
	// unmoved by that. Guarded anyway -- this interface is not disassembly
	// verified, and ClearAllOverlays working at 14 is evidence for the ordering
	// but not proof of every slot in between.
	enum { kAddScreenTextOverlay = 6 };

	void* m_iface = nullptr;
	bool m_canClear = false;
	unsigned int m_boxesDrawn = 0;
	bool m_screenTextFaulted = false;
};

} // namespace sinvr
