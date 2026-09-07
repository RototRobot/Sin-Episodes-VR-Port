// Physical crouching -- duck in the room, duck in the game.
//
// ---- THE DOUBLE-LOWERING PROBLEM, AND WHY IT IS NOT SOLVED BY SUPPRESSION ----
//
// Triggering "+duck" from a physical crouch is the easy half. The trap is that
// the two lowerings STACK: 6DoF already drops the view by however far the head
// really moved, and the engine's duck then drops it again by its own fixed
// amount, so a 20-unit crouch puts the camera 56 units down and the player is
// looking at the floor from their own knees.
//
// The obvious fix -- suppress our own vertical offset while ducked -- trades one
// wrong answer for another. The view would sit at the engine's canonical duck
// height regardless of how far the player actually crouched, so a shallow crouch
// would drop the view further than the head moved and a deep one not far enough,
// and crossing the threshold would SNAP the view by the difference.
//
// So it is done the other way round: the engine's duck is CANCELLED in the view
// and kept everywhere else.
//
//     view = base.origin (already ducked by the engine)
//          + head displacement            (6DoF, what the player really did)
//          + duck compensation            (exactly undoes the engine's drop)
//
// The result is that the camera tracks the player's real head, always, while the
// engine's duck still does the things only it can do: shrink the collision hull
// so they fit under the gap, and tell the game they are crouching. Nothing here
// has to guess a crouch height, because the compensation is read from the
// engine's own m_vecViewOffset rather than assumed to be Source's 64/28.
//
// ---- ONLY WHEN THE DUCK WAS PHYSICAL ----------------------------------------
//
// Compensation must NOT apply to a duck the player asked for with the button.
// Pressing crouch while standing up straight should lower the view -- that is
// the whole point of the button -- and compensating it would make the button do
// nothing visible.
//
// So the compensation is latched to the CAUSE, not to the state: it turns on
// when a physical crouch starts, and off only once the engine's duck has fully
// released. Latching on the release matters as much as on the start. Standing
// up crosses the threshold instantly while the engine's un-duck ramps over
// roughly half a second, so dropping the compensation the moment the head came
// up would snap the view by the whole duck height. Following the engine down to
// zero instead makes it ramp out exactly in step.
//
// ---- MEASURED AGAINST THE PLAYER'S OWN HEIGHT -------------------------------
//
// The threshold is a FRACTION of standing height, not a distance, so it fits a
// tall player and a short one without either of them touching a config file.
// Standing height is the head height at the last recentre -- the same moment the
// 6DoF reference is captured, so there is no second calibration step and no way
// for the two to disagree.
//
// The corollary is worth knowing: recentre while STANDING. Recentring in a chair
// calibrates a seated player as standing, and then nothing will ever trigger.
// The log prints the height it measured for exactly this reason.
#pragma once

#include <windows.h>
#include <math.h>
#include "../sdk/source_interfaces.h"
#include "../vr/vr_backend.h"
#include "../../common/log.h"

namespace sinvr {

// How many consecutive unchanged samples mean the engine's view offset has
// finished ramping. Three is ~33 ms at 90 fps -- longer than a frame of noise,
// far shorter than a duck ramp.
constexpr int kSettledFramesNeeded = 3;

struct PhysicalCrouchSettings
{
	// Off by default: it changes how the player moves, and someone who only
	// wants the button should get exactly what they had before.
	bool enabled = false;

	// Log every transition and what it measured.
	bool debug = false;

	// Fraction of standing head height below which the player is crouching.
	// 0.70 is a real crouch rather than a lean over a desk.
	float triggerFraction = 0.70f;

	// Must come back above this to stand again. The gap is hysteresis: a head
	// hovering exactly on one threshold would otherwise stutter the duck on and
	// off several times a second.
	float releaseFraction = 0.77f;

	// Cancel the engine's duck in the view, so the camera tracks the real head.
	// Off means the two lowerings stack -- see the header. Kept as a switch
	// because it is the one part of this that could be wrong on a game whose
	// duck does not work the way Source's does, and turning it off is a cheaper
	// diagnosis than a rebuild.
	bool compensate = true;
};

class PhysicalCrouch
{
public:
	void SetSettings( const PhysicalCrouchSettings& s ) { m_settings = s; }
	const PhysicalCrouchSettings& Settings() const { return m_settings; }

	// Once per frame, BEFORE VRCamera::Apply, so the view built this frame
	// already carries the compensation.
	//
	// `standingHeight` is the head height at the last recentre, in Source units
	// above the play-space floor. `viewOffsetZ` is the engine's current
	// m_vecViewOffset.z, or a negative number when it could not be read.
	void Update( IVRBackend& vr, float standingHeight, bool haveStandingHeight,
				 float viewOffsetZ )
	{
		if ( !m_settings.enabled )
		{
			m_crouching = false;
			m_compensation = 0.0f;
			m_compensating = false;
			return;
		}

		const HmdPose& head = vr.Hmd();

		// ---- WHAT "STANDING" MEANS, AND WHY THE MAXIMUM WAS WRONG ---------
		//
		// This used to take the LARGEST view offset ever seen, to survive a
		// player who was already crouching when the map loaded. It survives that
		// and nothing else: a maximum can only ever go UP, so a single transient
		// spike -- a level load, a scripted sequence, anything that lifts the
		// view for a moment -- is latched forever.
		//
		// Measured on this build: 69.2 latched as "standing" against a real
		// standing offset of 64.0, leaving a permanent 5.2u compensation. And
		// because `ducked` could then never fall back to ~0, the compensating
		// flag never released either, so the view stayed pushed up after the
		// player had stood back up. One bad sample, two stuck states.
		//
		// The fix is to sample it when the player is DEMONSTRABLY STANDING
		// rather than to take a maximum: head at full height and not crouching.
		// That handles the already-crouching-at-load case just as well -- it
		// corrects the moment they stand -- and it cannot be poisoned, because
		// the next genuine standing frame overwrites it.
		//
		// ---- BUT NOT WHILE IT IS STILL MOVING ------------------------------
		//
		// "Standing" by head height arrives BEFORE the engine has finished its
		// stand-up ramp: the head clears the release fraction in one motion
		// while m_vecViewOffset[2] is still lerping from ducked back to
		// standing. Sampling there latches a mid-ramp value -- around 45
		// against a true 64 -- which collapses the reference, drops the
		// compensation out from under the player, and reads as the view POPPING
		// upward as they stand.
		//
		// So the value has to be STILL as well as standing. A settled view
		// offset does not change at all frame to frame; a ramping one changes
		// every frame. Requiring a few identical samples separates them without
		// needing to know anything about the engine's ramp rate.
		const bool standingNow = haveStandingHeight && standingHeight > 1.0f &&
								 head.valid && !m_crouching &&
								 ( head.position.z / standingHeight ) >= m_settings.releaseFraction;
		const bool settled = ( viewOffsetZ >= 0.0f ) &&
							 ( fabsf( viewOffsetZ - m_prevViewOffsetZ ) < 0.05f );

		if ( standingNow && settled )
		{
			if ( m_settledFrames < kSettledFramesNeeded )
				++m_settledFrames;
			if ( m_settledFrames >= kSettledFramesNeeded )
				m_standingViewOffsetZ = viewOffsetZ;
		}
		else
		{
			m_settledFrames = 0;
		}

		// Bootstrap: before any pose is known there is nothing to compare
		// against, and a zero reference would mean no compensation at all.
		if ( m_standingViewOffsetZ <= 0.0f && viewOffsetZ >= 0.0f )
			m_standingViewOffsetZ = viewOffsetZ;

		m_prevViewOffsetZ = viewOffsetZ;

		const bool haveViewOffset = ( viewOffsetZ >= 0.0f );

		if ( !head.valid || !haveStandingHeight || standingHeight <= 1.0f )
		{
			// No reference: do not duck, and release anything held. A crouch
			// asserted while tracking is lost would stay asserted.
			m_crouching = false;
			UpdateCompensation( viewOffsetZ, haveViewOffset );
			return;
		}

		m_standingHeight = standingHeight;
		m_headHeight = head.position.z;
		m_fraction = m_headHeight / standingHeight;

		const bool was = m_crouching;
		if ( m_crouching )
		{
			if ( m_fraction >= m_settings.releaseFraction )
				m_crouching = false;
		}
		else
		{
			if ( m_fraction <= m_settings.triggerFraction )
				m_crouching = true;
		}

		if ( m_crouching != was )
		{
			if ( m_crouching )
			{
				++m_crouches;
				m_compensating = true;   // latched: see the header
			}
			if ( m_settings.debug )
				Log( "crouch: %s -- head at %.1fu of %.1fu standing (%.0f%%), "
					 "gates trigger %.0f%% release %.0f%%",
					 m_crouching ? "CROUCHING" : "standing",
					 m_headHeight, m_standingHeight, m_fraction * 100.0f,
					 m_settings.triggerFraction * 100.0f,
					 m_settings.releaseFraction * 100.0f );
		}

		UpdateCompensation( viewOffsetZ, haveViewOffset );
	}

	// True while the player is physically crouched. ORed with the crouch button
	// by GameInput, so the two never fight over "+duck".
	bool Crouching() const { return m_crouching; }

	// Units to add back to the view's Z to cancel the engine's duck. Zero unless
	// a physical crouch caused it.
	float ViewCompensation() const { return m_compensation; }

	void LogState() const
	{
		if ( !m_settings.enabled )
			return;

		Log( "crouch: physical %s | head %.1fu of %.1fu standing (%.0f%%) -> %s "
			 "| crouches=%u",
			 m_settings.compensate ? "ON" : "ON (compensation OFF)",
			 m_headHeight, m_standingHeight, m_fraction * 100.0f,
			 m_crouching ? "CROUCHING" : "standing", m_crouches );

		// Reported as effect, not intent. A compensation stuck at 0 while
		// crouching means m_vecViewOffset was never readable, and the symptom of
		// that is the double-lowering this whole class exists to prevent -- which
		// from inside the headset just looks like "crouching is too low".
		if ( m_standingViewOffsetZ <= 0.0f )
			LogWarn( "crouch: the engine's view offset has never been read, so "
					 "its duck cannot be cancelled -- expect the view to drop "
					 "TWICE when crouching. See physical_crouch_compensate." );
		else
			Log( "crouch: engine view offset %.1fu standing, compensation %.1fu "
				 "now (%s)", m_standingViewOffsetZ, m_compensation,
				 m_compensating ? "active" : "idle" );
	}

	unsigned int Crouches() const { return m_crouches; }

private:
	// The compensation follows the ENGINE, not the player: it ramps in and out
	// with m_vecViewOffset, which is what makes the transitions smooth without
	// any smoothing of our own.
	void UpdateCompensation( float viewOffsetZ, bool haveViewOffset )
	{
		if ( !m_settings.compensate || !haveViewOffset ||
			 m_standingViewOffsetZ <= 0.0f )
		{
			m_compensation = 0.0f;
			return;
		}

		const float ducked = m_standingViewOffsetZ - viewOffsetZ;

		// Latched off once the engine has finished standing back up, so the
		// compensation ramps out in step with the duck rather than vanishing the
		// instant the head clears the threshold.
		//
		// ---- AND THE TOLERANCE IS THE SIZE OF THE POP ----------------------
		//
		// This was briefly widened to 2.0 to work around a standing reference
		// that could never return to zero. That fixed the stuck latch and
		// created a visible one: compensation jumps straight from 2.0 to 0 at
		// the moment it releases, and two units of view is a perceptible snap.
		//
		// Whatever this threshold is, it is also the discontinuity -- so it
		// belongs just above float noise, and the reference has to be right
		// enough to reach it. Fixing the reference is what allows that, which is
		// why the two changes are one fix and not two.
		if ( !m_crouching && ducked <= 0.25f )
			m_compensating = false;

		m_compensation = ( m_compensating && ducked > 0.0f ) ? ducked : 0.0f;
	}

	PhysicalCrouchSettings m_settings;

	bool m_crouching = false;
	bool m_compensating = false;
	float m_compensation = 0.0f;

	float m_standingHeight = 0.0f;
	float m_headHeight = 0.0f;
	float m_fraction = 1.0f;
	float m_standingViewOffsetZ = 0.0f;
	// For the settled test above -- a ramping view offset changes every frame,
	// a settled one does not change at all.
	float m_prevViewOffsetZ = -1.0f;
	int m_settledFrames = 0;

	unsigned int m_crouches = 0;
};

} // namespace sinvr
