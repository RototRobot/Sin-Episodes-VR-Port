// The five body zones in one place, the code that draws them, and the live
// tuner that positions them.
//
// Holding them together rather than leaving each one inside the feature that
// uses it is what makes them positionable: the boxes have to be drawn from a
// single list in a single frame, they have to agree on the body frame exactly,
// and while they are being tuned the interesting question is where they sit
// RELATIVE TO EACH OTHER -- the reload box against the hip holster it overlaps,
// the melee start box against both.
//
// The features still own their behaviour. This owns only the geometry.
//
// ---- TUNING: SNAP, DON'T NUDGE -----------------------------------------------
//
// The nudge keys exist, but the key that matters is SNAP TO HAND: hold the
// weapon hand where the zone should be and press numpad `.`, and the box centres
// there. Nudging a box you cannot see the numbers for is guesswork; snapping
// sets it from the player's actual reach, which is the same argument that made
// the melee thresholds work -- the informative quantity is what a real arm did,
// not a blind increment.
#pragma once

#include <windows.h>
#include <string.h>
#include "zone_box.h"
#include "../sdk/debug_overlay.h"
#include "../vr/vr_backend.h"
#include "../../common/log.h"

namespace sinvr {

enum ZoneId
{
	kZoneHolsterHip = 0,   // the pistol -- slot1
	kZoneHolsterLeft,      // upper body, player's left
	kZoneHolsterRight,     // upper body, player's right
	kZoneMeleeStart,       // where a swing must BEGIN
	kZoneReload,           // waist, arcade reloading
	kZoneCount
};

struct ZoneStyle
{
	const char* name;
	const char* configPrefix;
	int r, g, b;
};

// Colours are fixed rather than configurable: they exist to tell five boxes
// apart for a few sessions, and a colour that has to be looked up in a config
// file defeats that.
inline const ZoneStyle& ZoneStyleFor( int id )
{
	static const ZoneStyle styles[kZoneCount] = {
		{ "holster hip (pistol)", "zone_holster_hip",   160,  32, 240 },  // purple
		{ "holster left",         "zone_holster_left",   64, 128, 255 },  // blue
		{ "holster right",        "zone_holster_right", 255, 160,  32 },  // orange
		{ "melee start",          "zone_melee",          32, 224,  64 },  // green
		{ "reload",               "zone_reload",        240,  48,  48 },  // red
	};
	return styles[( id >= 0 && id < kZoneCount ) ? id : 0];
}

class ZoneSet
{
public:
	ZoneBox& Get( int id ) { return m_boxes[Clamp( id )]; }
	const ZoneBox& Get( int id ) const { return m_boxes[Clamp( id )]; }

	void SetVisible( bool v ) { m_visible = v; }
	bool Visible() const { return m_visible; }

	bool BindOverlay( void* raw ) { return m_overlay.Bind( raw ); }

	// See DebugOverlay::Clear. Harmless when nothing is bound.
	void ClearOverlays() { m_overlay.Clear(); }

	// Remember the loaded values so numpad 5 has something to reset TO. Called
	// once, after the config is read: the baseline is what the player started
	// this session with, not the built-in defaults, so a reset undoes this
	// session's fiddling rather than a previous session's tuning.
	void CaptureBaseline()
	{
		for ( int i = 0; i < kZoneCount; ++i )
			m_baseline[i] = m_boxes[i];
	}

	// ---- live tuning -------------------------------------------------------

	void SetAdjustEnabled( bool v ) { m_adjust = v; }
	bool AdjustEnabled() const { return m_adjust; }

	void SelectNext()
	{
		m_selected = ( m_selected + 1 ) % kZoneCount;
		LogSelection( "selected" );
	}

	void ToggleMode()
	{
		m_sizeMode = !m_sizeMode;
		LogSelection( m_sizeMode ? "now editing SIZE" : "now editing POSITION" );
	}

	void ScaleStep( float factor )
	{
		m_step *= factor;
		if ( m_step < 0.25f ) m_step = 0.25f;
		if ( m_step > 16.0f ) m_step = 16.0f;
		Log( "zones: step %.2f units", m_step );
	}

	// axis: 0 forward, 1 lateral, 2 up. `dir` is +1 or -1.
	void Nudge( int axis, float dir )
	{
		ZoneBox& z = m_boxes[m_selected];
		float* target = nullptr;
		if ( m_sizeMode )
			target = ( axis == 0 ) ? &z.sizeForward
				   : ( axis == 1 ) ? &z.sizeLateral : &z.sizeUp;
		else
			target = ( axis == 0 ) ? &z.forward
				   : ( axis == 1 ) ? &z.lateral : &z.up;

		*target += dir * m_step;

		// A size may not go negative. Zero is meaningful -- ZoneBox::Valid()
		// reads it as "this zone is off" -- so it is allowed to reach zero and
		// stop there rather than being clamped to some small positive number
		// that would look like a working zone nobody can hit.
		if ( m_sizeMode && *target < 0.0f )
			*target = 0.0f;

		LogSelection( "moved" );
	}

	// The important one. Centre the selected zone on the weapon hand, wherever
	// it is right now.
	bool SnapToHand( IVRBackend& vr )
	{
		const ControllerPose& hand = vr.WeaponHand();
		const HmdPose& head = vr.Hmd();
		if ( !hand.valid || !head.valid )
		{
			LogWarn( "zones: cannot snap -- the weapon hand is not tracked" );
			return false;
		}

		ZoneBox& z = m_boxes[m_selected];
		ToBodyFrame( hand.position, head.position, head.angles.y,
					 z.forward, z.lateral, z.up );
		LogSelection( "SNAPPED to the weapon hand" );
		return true;
	}

	// Two presses, like the viewmodel tuner's reset: it is the one key here that
	// destroys work, and it sits between the four direction keys.
	void ResetSelected()
	{
		m_boxes[m_selected] = m_baseline[m_selected];
		LogSelection( "reset to the values this session started with" );
	}

	int Selected() const { return m_selected; }

	// ---- drawing -----------------------------------------------------------

	// Once per frame, from the View_Render hook BEFORE the eye passes.
	//
	// `headWorldOrigin` is the view origin the mod itself computes -- the base
	// CViewSetup origin plus the 6DoF offset -- so the boxes sit on the player's
	// real head rather than on the entity's eye position, which is where those
	// two differ by exactly the amount the player has leaned.
	void Prepare( const Vector& headWorldOrigin, float headWorldYaw )
	{
		m_origin = headWorldOrigin;
		m_yaw = headWorldYaw;
	}

	// Submitted once per EYE PASS, because the overlay list is cleared at the
	// top of each one to drop stale copies -- see DebugOverlay::Clear.
	void Submit()
	{
		const Vector headWorldOrigin = m_origin;
		const float headWorldYaw = m_yaw;
		if ( !m_visible || !m_overlay.Valid() )
			return;

		// Only yaw. Pitching the boxes with the head would tip the player's
		// holsters up as they looked down to find them, which is precisely when
		// they need to be where the body left them.
		const QAngle angles = { 0.0f, headWorldYaw, 0.0f };

		for ( int i = 0; i < kZoneCount; ++i )
		{
			const ZoneBox& z = m_boxes[i];
			if ( !z.Valid() )
				continue;

			const ZoneStyle& s = ZoneStyleFor( i );

			const float hf = z.sizeForward * 0.5f;
			const float hl = z.sizeLateral * 0.5f;
			const float hu = z.sizeUp * 0.5f;

			// Source's local Y is LEFT, while ZoneBox.lateral is RIGHT, so the
			// centre negates. The half-size does not -- the box is symmetric
			// about its own centre, and negating that too would be a no-op that
			// reads as if it meant something.
			const Vector mins = { z.forward - hf, -z.lateral - hl, z.up - hu };
			const Vector maxs = { z.forward + hf, -z.lateral + hl, z.up + hu };

			// The zone being edited is drawn solid, the others faint. Without
			// this the tuner is unusable: five overlapping wireframes and no way
			// to tell which one the numpad is moving.
			const int alpha = ( m_adjust && i == m_selected ) ? 200 : 48;

			// Duration 0 is this frame only. Anything longer smears a box that
			// moves with the player into a trail.
			m_overlay.Box( headWorldOrigin, mins, maxs, angles,
						   s.r, s.g, s.b, alpha, 0.0f );
		}
		++m_frames;
	}

	// ---- reporting ---------------------------------------------------------

	void LogState( IVRBackend* vr ) const
	{
		if ( !m_visible && !m_adjust )
			return;

		char inside[256];
		inside[0] = 0;
		float fwd = 0.0f, lat = 0.0f, up = 0.0f;
		bool haveHand = false;

		if ( vr )
		{
			const ControllerPose& hand = vr->WeaponHand();
			const HmdPose& head = vr->Hmd();
			if ( hand.valid && head.valid )
			{
				ToBodyFrame( hand.position, head.position, head.angles.y, fwd, lat, up );
				haveHand = true;
				for ( int i = 0; i < kZoneCount; ++i )
				{
					if ( !m_boxes[i].Contains( fwd, lat, up ) )
						continue;
					if ( inside[0] )
						strcat_s( inside, ", " );
					strcat_s( inside, ZoneStyleFor( i ).name );
				}
			}
		}

		if ( m_visible )
			Log( "zones: boxes VISIBLE (purple=hip/pistol blue=left orange=right "
				 "green=melee red=reload) | we drew %u boxes over %u frames",
				 m_overlay.BoxesDrawn(), m_frames );

		if ( m_adjust )
			Log( "zones: TUNING %s, editing %s, step %.2f | numpad 1 next zone, "
				 "7 pos/size, . snap to hand, 0 dump, 5 5 reset",
				 ZoneStyleFor( m_selected ).name,
				 m_sizeMode ? "SIZE" : "POSITION", m_step );

		if ( haveHand )
			Log( "zones: weapon hand at forward=%.1f lateral=%.1f up=%.1f -> %s",
				 fwd, lat, up, inside[0] ? inside : "no zone" );
		else
			Log( "zones: weapon hand not tracked" );
	}

	// Prints every box as cfg lines, ready to paste. The same idea as the
	// viewmodel tuner's numpad-0: the values that matter are the ones the player
	// arrived at in the headset, and retyping them from a log by hand is how
	// they get transcribed wrong.
	void LogAsConfig() const
	{
		Log( "zones: ---- paste into sinvr.cfg ----" );
		for ( int i = 0; i < kZoneCount; ++i )
		{
			const ZoneBox& z = m_boxes[i];
			const ZoneStyle& s = ZoneStyleFor( i );
			Log( "%s_forward = %.1f", s.configPrefix, z.forward );
			Log( "%s_lateral = %.1f", s.configPrefix, z.lateral );
			Log( "%s_up = %.1f", s.configPrefix, z.up );
			Log( "%s_size_forward = %.1f", s.configPrefix, z.sizeForward );
			Log( "%s_size_lateral = %.1f", s.configPrefix, z.sizeLateral );
			Log( "%s_size_up = %.1f", s.configPrefix, z.sizeUp );
		}
		Log( "zones: ---- end ----" );
	}

private:
	static int Clamp( int id ) { return ( id >= 0 && id < kZoneCount ) ? id : 0; }

	void LogSelection( const char* what ) const
	{
		const ZoneBox& z = m_boxes[m_selected];
		Log( "zones: %s -- %s | pos fwd=%.1f lat=%.1f up=%.1f | "
			 "size fwd=%.1f lat=%.1f up=%.1f | editing %s, step %.2f",
			 ZoneStyleFor( m_selected ).name, what,
			 z.forward, z.lateral, z.up,
			 z.sizeForward, z.sizeLateral, z.sizeUp,
			 m_sizeMode ? "SIZE" : "POSITION", m_step );
	}

	ZoneBox m_boxes[kZoneCount];
	ZoneBox m_baseline[kZoneCount];
	DebugOverlay m_overlay;

	bool m_visible = false;
	Vector m_origin = { 0.0f, 0.0f, 0.0f };
	float m_yaw = 0.0f;
	bool m_adjust = false;
	int m_selected = 0;
	bool m_sizeMode = false;
	float m_step = 2.0f;
	unsigned int m_frames = 0;
};

} // namespace sinvr
