#pragma once
//-----------------------------------------------------------------------------
// SEEING THE AIM, RATHER THAN INFERRING IT FROM A DOT.
//
// Zeroing the sights was proving hard to tune, and the reason is that a single
// crosshair shows you the ANSWER without showing you the MECHANISM. When the
// dot is off you cannot tell whether the barrel is pointing somewhere else, or
// the shot is converging at the wrong distance, or both -- and those want
// different knobs. So the tuner was a search with one bit of feedback.
//
// ONE line, because there is now one ray.
//
// This file used to draw three -- the model axis, the bore, and the shot from
// the eye -- because those were three different rays that had to be reconciled
// by tuning, and seeing their relationship was the only way to tell an angular
// error from a lateral one.
//
// shot_from_gun removed the problem rather than making it easier to see. The
// shot leaves the muzzle and travels down the barrel, so the model axis, the
// bore and the shot are the same line. Drawing two more on top of it was
// clutter that implied a distinction that no longer exists.
//
// What remains is drawn from the SHOOT ORIGIN handed to the trace rewrite,
// along the angles actually written to the engine. It is not an illustration
// of the shot; it IS the shot. If it leaves the wrong part of the gun, that is
// exactly what the lateral / up / forward knobs are for -- and being able to
// see the start point is what makes those tunable at all.
//-----------------------------------------------------------------------------

#include <windows.h>
#include <stdio.h>

#include "../sdk/source_interfaces.h"
#include "../sdk/debug_overlay.h"
#include "../../common/log.h"

namespace sinvr {

struct AimDebugSettings
{
	bool enabled = false;

	// How far to draw each line. The barrel line stops at what it hits; this
	// bounds it when it hits nothing.
	float length = 600.0f;

	// Screen position for the axis readout, 0..1.
	float textX = 0.35f;
	float textY = 0.75f;
};

class AimDebug
{
public:
	void SetSettings( const AimDebugSettings& s ) { m_settings = s; }
	bool Enabled() const { return m_settings.enabled; }
	bool Bind( void* overlayIface ) { return m_overlay.Bind( overlayIface ); }
	bool Ready() const { return m_overlay.Valid(); }

	// Called once per frame, before the eye passes, with everything already
	// computed by the aim path. Nothing is derived a second time here -- a
	// visualiser that recomputes what it is drawing can disagree with it, and
	// then it is showing you its own arithmetic rather than the game's.
	// PREPARE / SUBMIT, like the zone boxes and the laser dot -- and for the
	// same measured reason recorded beside them: the overlay list is CLEARED at
	// the top of every eye pass, so anything added once per frame is drawn in
	// one eye and missing from the other.
	void Prepare( const Vector& muzzle, const Vector& barrelForward,
				  const Vector& target, float convergence, bool measured )
	{
		m_muzzle = muzzle;
		m_forward = barrelForward;
		m_target = target;
		m_lastConvergence = convergence;
		m_lastMeasured = measured;
		m_have = true;

	}

	void Submit()
	{
		if ( !m_settings.enabled || !m_overlay.Valid() || !m_have )
			return;

		const Vector& muzzle = m_muzzle;
		const Vector& barrelForward = m_forward;
		const Vector& target = m_target;

		// ---- THE SHOT, green -----------------------------------------------
		//
		// From the point the trace rewrite fires from, along the angles the
		// engine was given. Tuned by eye against the gun itself: it should
		// leave the muzzle and run down the barrel.
		const Vector barrelEnd = {
			muzzle.x + barrelForward.x * m_settings.length,
			muzzle.y + barrelForward.y * m_settings.length,
			muzzle.z + barrelForward.z * m_settings.length };
		m_overlay.Line( muzzle, barrelEnd, 40, 255, 40, true, 0.0f );

		// ---- WHAT THE SHOT WILL HIT, white ----------------------------------
		//
		// A cross where the traced barrel ray lands. With one ray this is
		// simply the point of impact, which makes "am I pointing at that" a
		// thing to look at rather than estimate.
		const float k = 4.0f;
		m_overlay.Line( Vector{ target.x - k, target.y, target.z },
						Vector{ target.x + k, target.y, target.z },
						255, 255, 255, true, 0.0f );
		m_overlay.Line( Vector{ target.x, target.y - k, target.z },
						Vector{ target.x, target.y + k, target.z },
						255, 255, 255, true, 0.0f );
		m_overlay.Line( Vector{ target.x, target.y, target.z - k },
						Vector{ target.x, target.y, target.z + k },
						255, 255, 255, true, 0.0f );

		++m_frames;
	}

	// The readout. Separate from Draw because it wants the TUNER's state, which
	// the aim path knows nothing about.
	void DrawText( const char* weapon, const char* axis,
				   float yaw, float pitch, float right, float up, float fwd )
	{
		if ( !m_settings.enabled || !m_overlay.Valid() )
			return;

		char line1[192], line2[192];

		// The active axis first and in capitals: it is the one thing the player
		// needs at a glance while both hands are on the controllers.
		_snprintf_s( line1, sizeof( line1 ), _TRUNCATE,
					 "AXIS: %s        [ ' cycles ]   [ , . adjust ]   [ / save ]",
					 axis ? axis : "?" );

		_snprintf_s( line2, sizeof( line2 ), _TRUNCATE,
					 "%s   yaw %+.2f   pitch %+.2f   lateral %+.2f   up %+.2f   "
					 "fwd %+.2f",
					 weapon ? weapon : "<no weapon>", yaw, pitch, right, up, fwd );

		// The impact range, which is the only number left worth showing: it
		// says whether the white cross is where you think it is.
		char line3[192];
		_snprintf_s( line3, sizeof( line3 ), _TRUNCATE,
					 "impact at %.0f units (%s)   |   GREEN is the shot -- it "
					 "should leave the muzzle and run down the barrel",
					 m_lastConvergence, m_lastMeasured ? "traced" : "no hit" );

		m_overlay.ScreenText( m_settings.textX, m_settings.textY, line1,
							  255, 220, 64, 255, 0.0f );
		m_overlay.ScreenText( m_settings.textX, m_settings.textY + 0.035f, line2,
							  200, 200, 200, 255, 0.0f );
		m_overlay.ScreenText( m_settings.textX, m_settings.textY + 0.070f, line3,
							  160, 200, 160, 255, 0.0f );
	}

	void LogState() const
	{
		if ( !m_settings.enabled )
			return;
		Log( "aim debug: %s | %u frame(s) drawn | screen text %s",
			 m_overlay.Valid() ? "overlay BOUND" : "overlay NOT bound -- nothing drawn",
			 m_frames,
			 m_overlay.ScreenTextFaulted() ? "FAULTED (wrong slot?)" : "ok" );
	}

private:
	AimDebugSettings m_settings;
	DebugOverlay m_overlay;
	Vector m_muzzle = { 0.0f, 0.0f, 0.0f };
	Vector m_forward = { 1.0f, 0.0f, 0.0f };
	Vector m_target = { 0.0f, 0.0f, 0.0f };
	bool m_have = false;
	float m_lastConvergence = 0.0f;
	bool m_lastMeasured = false;
	unsigned int m_frames = 0;
};

} // namespace sinvr
