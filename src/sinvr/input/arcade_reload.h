// Arcade reloading -- drop the weapon hand to your waist and the gun reloads.
//
// ---- WHY THIS IS A POSITION DETECTOR AND MELEE IS A VELOCITY ONE ------------
//
// Both gestures are "the weapon hand goes down", and telling them apart was
// flagged in the handover as the real work here. Tuning two velocity detectors
// against each other would have been a losing fight, because a melee swing ENDS
// inside the reload zone: it starts at or above melee_arm_height (-16) and
// travels at least melee_min_travel (20), so it finishes at -36 or lower --
// well past this gesture's gate.
//
// So they are different KINDS of detector rather than one detector with two sets
// of numbers:
//
//   melee    a fast downward SWING                  -- velocity, fires in motion
//   reload   the hand ARRIVING and staying at rest  -- position, fires on dwell
//
// A swing passing through the waist fails the reload test because it is moving;
// a hand lowered to the waist fails the melee test because it is slow and starts
// low. That separation is structural, so neither has to be tuned tightly to
// avoid the other -- which is the property that makes both survivable.
//
// Three interlocks back it up, in decreasing order of how much work they do:
//
//   * maxSpeed -- the inverse of melee's minSpeed. The hand must be nearly
//                 still, which is what the decelerating tail of a swing fails.
//   * dwell    -- it must STAY there. A swing rebounds back up.
//   * the melee detector is asked outright whether it is busy, so the two
//     cannot both claim one motion even if the thresholds are set badly.
//
// ---- AND THE HIP HOLSTER ZONE ----------------------------------------------
//
// holster_hip_height (-22 by default) draws slot1 when the player grips at the
// waist, which is the same place this gesture lives. The default gate here is
// DELIBERATELY LOWER (-26): a reach to the holster stops at the hip, a reload
// goes past it. The overlap is reduced by the numbers rather than by a special
// case, and a player who wants them further apart has a knob at each end.
//
// They can still both happen -- reach deep, dwell, and you reload the gun you
// are holding before gripping to draw another. That is wasteful rather than
// broken, and it is reported instead of hidden.
//
// ---- ROOM SPACE, HEAD-RELATIVE HEIGHT --------------------------------------
//
// The same frame as melee_gesture.h and holster_zones.h, for the same reasons:
// heights measured DOWN FROM THE HMD so they follow the player's height instead
// of assuming one, and room space so a snap turn -- which moves the hand metres
// in world space without the player moving at all -- is invisible. The full
// argument is in melee_gesture.h.
//
// ---- MEASURE FIRST -----------------------------------------------------------
//
// Like melee, the detector runs with `arcade_reload = 0` and
// `arcade_reload_debug = 1`: it evaluates and logs every candidate and says
// whether it WOULD have fired, without ever issuing a reload. The heartbeat
// reports the lowest the hand reached this session, which is the number the
// height gate should actually be set from.
#pragma once

#include <windows.h>
#include <math.h>
#include "../sdk/source_interfaces.h"
#include "../vr/vr_backend.h"
#include "zone_box.h"
#include "../../common/log.h"

namespace sinvr {

struct ArcadeReloadSettings
{
	// Off by default: it unbinds the reload button, which is a control change
	// nobody should get without asking for it.
	bool enabled = false;

	// Evaluate and log candidates without ever reloading. Calibration mode.
	bool debug = false;

	// Where the hand has to be. Was a bare height threshold (-26, below the
	// hip holster's -22 so a reach to the holster stops short of it); it is
	// now a box so it can be drawn, and so the overlap with the hip holster
	// is something you can see instead of something you infer.
	//
	// Null disables the gesture rather than firing everywhere: an absent
	// zone must fail closed here, because this one unbinds a button.
	const ZoneBox* zone = nullptr;

	// How long it must stay there. This is what a melee swing fails: a swing
	// passes through the zone and rebounds, it does not settle in it.
	float dwellSeconds = 0.12f;

	// The hand must be slower than this, in units/second -- the inverse of
	// melee's minSpeed (55). A swing's tail is still moving well above it.
	//
	// If the runtime does not populate vVelocity this reads 0 and the gate
	// always passes, so the gesture degrades to position-and-dwell rather than
	// stopping working. Melee has no such fallback, which is why it warns and
	// this does not.
	float maxSpeed = 30.0f;

	// The hand must leave the box and rise this far above its TOP before
	// another reload can fire. Relative to the box rather than absolute, so
	// moving the box carries its hysteresis with it -- an absolute height
	// would silently stop rearming the moment the box was dragged past it.
	float rearmAbove = 8.0f;

	// Refractory period after a reload fires, in seconds. Longer than melee's
	// because a reload takes real time in-game and repeating it is pointless.
	float cooldown = 1.5f;

	// How long "+reload" stays asserted. Same reasoning as melee's: the engine
	// samples key state into a usercmd once per frame, and a press that goes
	// down and up inside one frame can be missed entirely.
	float holdSeconds = 0.12f;
};

class ArcadeReload
{
public:
	void SetSettings( const ArcadeReloadSettings& s ) { m_settings = s; }
	const ArcadeReloadSettings& Settings() const { return m_settings; }

	// True when the gesture owns the reload command, so GameInput knows to stop
	// driving it from the button. Only when actually enabled -- in debug-only
	// mode the button must keep working or the player cannot reload at all
	// while calibrating.
	bool OwnsReloadCommand() const { return m_settings.enabled; }

	// Once per frame, after VRCamera::Apply so the poses are this frame's, and
	// after MeleeGesture::Update so `meleeBusy` describes this frame.
	void Update( const EngineClient& engine, IVRBackend& vr, bool meleeBusy )
	{
		const DWORD now = GetTickCount();

		// Release first and unconditionally: a held "+reload" must come back up
		// even if everything below bails this frame.
		ReleaseIfDue( engine, now );

		if ( !m_settings.enabled && !m_settings.debug )
			return;

		const ControllerPose& hand = vr.WeaponHand();
		const HmdPose& head = vr.Hmd();
		if ( !hand.valid || !head.valid )
		{
			// Abandon any dwell rather than measuring across the gap.
			m_atWaistSinceMs = 0;
			m_rearmNeeded = true;
			return;
		}

		// Menus are driven by the OS cursor, and a player pointing at a menu is
		// waving the controller around at whatever height suits them.
		if ( InteractiveUiVisible() )
		{
			m_atWaistSinceMs = 0;
			m_rearmNeeded = true;
			return;
		}

		float fwd = 0.0f, lat = 0.0f, handHeight = 0.0f;
		ToBodyFrame( hand.position, head.position, head.angles.y,
				 fwd, lat, handHeight );
		const ZoneBox* zone = m_settings.zone;
		if ( !zone || !zone->Valid() )
			return;
		const float zoneTop = zone->up + zone->sizeUp * 0.5f;
		const bool inZone = zone->Contains( fwd, lat, handHeight );
		const float speed = sqrtf( hand.velocity.x * hand.velocity.x +
								   hand.velocity.y * hand.velocity.y +
								   hand.velocity.z * hand.velocity.z );

		if ( handHeight < m_sessionLowest )
			m_sessionLowest = handHeight;

		// ---- hysteresis ----------------------------------------------------
		// The hand has to come back up before another reload is possible. This
		// is what stops an arm resting at the player's side from reloading on
		// repeat, and it is checked before everything else because while it is
		// pending nothing else can matter.
		if ( m_rearmNeeded )
		{
			if ( handHeight > zoneTop + m_settings.rearmAbove )
				m_rearmNeeded = false;
			m_atWaistSinceMs = 0;
			return;
		}

		if ( m_lastFireMs != 0 &&
			 (DWORD)( now - m_lastFireMs ) < (DWORD)( m_settings.cooldown * 1000.0f ) )
		{
			m_atWaistSinceMs = 0;
			return;
		}

		// ---- the melee interlock -------------------------------------------
		// Asked rather than inferred. The height and speed gates already
		// separate the two, but a player who has retuned melee's thresholds
		// should not be able to make both fire on one motion -- and a swing in
		// progress is exactly the case where the hand is about to arrive at the
		// waist for reasons that are not a reload.
		if ( meleeBusy )
		{
			// Counted once per EPISODE, not once per frame. The first version
			// incremented every frame melee was busy, so one swing plus its 0.8s
			// cooldown read as ~47 rejections at 90 fps and looked like a
			// systematic problem. That is the same mistake the animation
			// suppressor made -- 285 log lines for one reload -- and the same
			// fix: compare against the previous state, not against nothing.
			if ( !m_meleeWasBusy )
				++m_rejectMelee;
			m_meleeWasBusy = true;
			m_atWaistSinceMs = 0;

			// NOT m_rearmNeeded. Aborting the DWELL is right -- the hand is
			// moving for melee reasons and must not bank dwell time towards a
			// reload. Demanding a RE-ARM was not: it required the hand back up
			// above the box before a reload could even begin, so a swing that
			// ended at the waist -- which every swing does, by construction --
			// cost the player the reload they were about to perform, and they
			// had to raise the gun to chest height and lower it again.
			//
			// The dwell timer is the interlock. It restarts from zero here, so
			// the hand still has to ARRIVE and STAY once the melee has
			// resolved; a swing that rebounds up never accumulates it.
			return;
		}
		if ( m_meleeWasBusy )
			m_meleeClearedMs = now;
		m_meleeWasBusy = false;

		// ---- the gesture ---------------------------------------------------
		if ( !inZone )
		{
			// Left the zone before the dwell completed. Counted, because "I put
			// my hand down and nothing happened" is a real report and it is
			// usually this: the hand came back up too soon.
			if ( m_atWaistSinceMs != 0 )
			{
				++m_rejectShortDwell;
				const float held = (float)( now - m_atWaistSinceMs ) / 1000.0f;
				if ( m_settings.debug )
					Log( "reload: left the zone after %.2fs, dwell needs %.2fs "
						 "(lowest this attempt %.1fu, box centre %.1fu)",
						 held, m_settings.dwellSeconds, m_attemptLowest,
						 zone->up );
				m_atWaistSinceMs = 0;
			}
			return;
		}

		// Below the gate. Moving too fast is a swing passing through, not a
		// hand being placed -- restart the dwell rather than reject outright,
		// since the same motion may still settle here.
		if ( speed > m_settings.maxSpeed )
		{
			if ( m_atWaistSinceMs != 0 )
				++m_rejectFast;
			m_atWaistSinceMs = 0;
			m_lastRejectSpeed = speed;
			return;
		}

		if ( m_atWaistSinceMs == 0 )
		{
			m_atWaistSinceMs = now;
			m_attemptLowest = handHeight;
			return;
		}
		if ( handHeight < m_attemptLowest )
			m_attemptLowest = handHeight;

		const float dwell = (float)( now - m_atWaistSinceMs ) / 1000.0f;
		if ( dwell >= m_settings.dwellSeconds )
			Fire( engine, now, handHeight, dwell, speed );
	}

	// Called when VR stops driving the game, for the same reason GameInput and
	// MeleeGesture have one.
	void Release( const EngineClient& engine )
	{
		if ( m_held && engine.Valid() )
		{
			engine.ClientCmd( "-reload\n" );
			m_held = false;
		}
	}

	void LogState() const
	{
		if ( !m_settings.enabled && !m_settings.debug )
			return;

		Log( "reload: arcade %s fired=%u rejected[short dwell=%u moving=%u melee=%u] "
			 "| dwell=%.2fs maxSpeed=%.0f rearm=%.0f above the box",
			 m_settings.enabled ? "ON (button unbound)" : "OFF (measuring only)",
			 m_fired, m_rejectShortDwell, m_rejectFast, m_rejectMelee,
			 m_settings.dwellSeconds, m_settings.maxSpeed,
			 m_settings.rearmAbove );

		// The calibration number. If the hand never gets near the gate, the gate
		// is wrong for this player's arm -- not the gesture.
		Log( "reload: lowest the hand reached this session %.1fu | "
			 "last attempt reached %.1fu, last rejection was moving at %.0f u/s",
			 m_sessionLowest, m_attemptLowest, m_lastRejectSpeed );

		// Reported as EFFECT. "melee=N" alone cannot tell a healthy interlock
		// from the lockout that used to follow it -- both look like a rejection.
		// This says how many of those went on to reload anyway.
		Log( "reload: %u reload(s) followed a swing within 1.5s -- these are the "
			 "ones the old forced re-arm made impossible. melee rejections %u. "
			 "If swings happened this session and this is 0, the interlock is "
			 "still eating reloads.",
			 m_recoveredAfterMelee, m_rejectMelee );
	}

	unsigned int Fired() const { return m_fired; }

private:
	// Source shows the OS cursor for menus and hides it in gameplay. Read
	// directly rather than borrowed from the stereo renderer -- see the same
	// note in melee_gesture.h.
	static bool InteractiveUiVisible()
	{
		CURSORINFO ci = {};
		ci.cbSize = sizeof( ci );
		if ( !GetCursorInfo( &ci ) )
			return false;
		return ( ci.flags & CURSOR_SHOWING ) != 0;
	}

	void Fire( const EngineClient& engine, DWORD now,
			   float handHeight, float dwell, float speed )
	{
		++m_fired;
		m_lastFireMs = now;
		m_atWaistSinceMs = 0;
		m_rearmNeeded = true;

		// Within a second and a half of a melee clearing, this is the swing-
		// then-settle case the interlock used to make impossible.
		if ( m_meleeClearedMs != 0 && (DWORD)( now - m_meleeClearedMs ) < 1500 )
		{
			++m_recoveredAfterMelee;
			m_meleeClearedMs = 0;
		}

		if ( m_settings.enabled && engine.Valid() )
		{
			// A held pair, not an impulse -- same reasoning as melee's.
			engine.ClientCmd( "+reload\n" );
			m_held = true;
			m_releaseAtMs = now + (DWORD)( m_settings.holdSeconds * 1000.0f );
		}

		Log( "reload: %s -- hand at %.1fu dwell=%.2fs speed=%.0f u/s",
			 m_settings.enabled ? "RELOAD" : "would have reloaded (disabled)",
			 handHeight, dwell, speed );
	}

	void ReleaseIfDue( const EngineClient& engine, DWORD now )
	{
		if ( !m_held || m_releaseAtMs == 0 )
			return;
		if ( (long)( now - m_releaseAtMs ) < 0 )
			return;
		if ( engine.Valid() )
			engine.ClientCmd( "-reload\n" );
		m_held = false;
		m_releaseAtMs = 0;
	}

	ArcadeReloadSettings m_settings;

	DWORD m_atWaistSinceMs = 0;
	DWORD m_lastFireMs = 0;
	DWORD m_releaseAtMs = 0;
	bool m_held = false;
	// Starts true so the hand has to be raised once before the first reload --
	// otherwise a player who loads a map with their arm down reloads instantly.
	bool m_rearmNeeded = true;

	unsigned int m_fired = 0;
	unsigned int m_rejectShortDwell = 0;
	unsigned int m_rejectFast = 0;
	unsigned int m_rejectMelee = 0;
	bool m_meleeWasBusy = false;

	// The positive this change exists to produce. Before it, a reload could
	// NEVER fire shortly after a melee -- the forced re-arm made it impossible
	// -- so a non-zero count here is the fix working, and a zero count across a
	// session with swings in it means it is not. A counter that can only report
	// success is worth nothing; this one can report either.
	DWORD m_meleeClearedMs = 0;
	unsigned int m_recoveredAfterMelee = 0;

	float m_sessionLowest = 0.0f;
	float m_attemptLowest = 0.0f;
	float m_lastRejectSpeed = 0.0f;
};

} // namespace sinvr
