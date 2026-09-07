// Melee by gesture -- swing the weapon hand down and the game swings with you.
//
// SiN binds melee to "+melee" / "-melee" (client.dll registers the pair;
// SE1/cfg/config_default.cfg binds it to Q). It is a HELD command, not an
// impulse, so firing it means asserting it and releasing it a moment later --
// not one ClientCmd. weapon_melee.txt confirms what the animation is: it borrows
// v_assault_rifle.mdl, i.e. the player strikes downward with the handle of
// whatever they are holding. So the gesture to detect is the one the game is
// already going to play: a fast downward swing of the weapon hand, ending across
// the body.
//
// ---- DETECTION IS THE EASY HALF ---------------------------------------------
//
// Anything that lowers the weapon hand is a candidate: dropping your arm to
// rest, reaching for the numpad, reloading, gesturing while someone talks. Four
// independent gates have to agree before this fires, and the point of having
// four is that no single one has to be tight enough to be annoying:
//
//   height   the swing must START high -- near head height. This alone rejects
//            almost every idle arm movement, because those start low.
//   speed    a minimum downward speed, from the runtime's own velocity.
//   travel   a minimum downward DISTANCE, so a fast twitch is not a swing.
//   shape    the net path must be mostly downward, and optionally mostly
//            INWARD -- right hand swinging to the player's left, or the mirror
//            of that when left_handed is set, which is the motion the animation
//            actually performs.
//
// plus a refractory period, so one swing is one melee.
//
// ---- ROOM SPACE, NOT WORLD SPACE --------------------------------------------
//
// Everything here works in room space, relative to the head's own yaw. That is
// not just cheaper than converting to world space, it is more correct:
//
//   * "Down" is identical in both frames -- the room floor IS the world floor,
//     since VRCamera's room->world mapping is a pure yaw rotation. There is
//     nothing to gain by converting.
//   * A SNAP TURN moves the hand's world position by metres in a single frame
//     while the hand has not moved at all. In world space that is an enormous
//     phantom velocity arriving exactly as a spike -- which is what a swing
//     detector is looking for. In room space a snap turn is invisible, which is
//     what it should be.
//   * "Across the body" needs the player's own facing, and world axes are as
//     arbitrary as room axes for that. The head's yaw is the frame that means
//     it, and both devices are already expressed in it.
//
// ---- MEASURE FIRST, THEN SET THRESHOLDS -------------------------------------
//
// The five numbers below cannot be derived, only measured on a real player with
// real controllers. So the detector runs even when `melee_gesture = 0`, as long
// as `melee_debug = 1`: it evaluates every candidate swing, logs what it
// measured and which gate rejected it, and says whether it WOULD have fired --
// without any chance of swinging in-game while calibrating. Do that first, read
// the numbers, then set the thresholds and turn it on.
#pragma once

#include <windows.h>
#include <math.h>
#include "../sdk/source_interfaces.h"
#include "../vr/vr_backend.h"
#include "zone_box.h"
#include "../../common/log.h"

namespace sinvr {

struct MeleeGestureSettings
{
	// Off by default. It adds a way to attack, which is a gameplay change, and
	// the thresholds below are starting points rather than measured values.
	bool enabled = false;

	// Evaluate and log candidate swings even when disabled. This is the
	// calibration mode -- see the header comment.
	bool debug = false;

	// Minimum downward speed, Source units per second, for a motion to be
	// considered the start of a swing. ~40 units/s is a metre per second.
	float minSpeed = 55.0f;

	// Minimum downward distance travelled, in units (~inches), before it fires.
	// A gun-butt swing from head height to about waist is on the order of 20.
	// Measured on this rig: the largest travel ever recorded before firing was
	// 22.2 units against a threshold of 20, so the old default fired at roughly
	// 90% of the swing -- at the bottom, which is exactly where the player does
	// not want the hit to come from.
	//
	// Lowered to fire early in the swing instead. The other three gates are
	// untouched and carry the false-positive load: speed peaks at 317 u/s
	// against a 55 gate, and downness at 0.98 against 0.55.
	float minTravel = 12.0f;

	// Where the hand must be for a swing to START. Was a bare height gate
	// (-16, i.e. roughly chin-to-shoulder, which a raised weapon hand clears
	// and a resting one does not); it is now a box, so it can be drawn and so
	// a swing that begins somewhere implausible sideways can be excluded too.
	//
	// Null means no start constraint at all rather than no swings -- a
	// missing box must not silently disable a calibrated feature.
	const ZoneBox* startZone = nullptr;

	// Net path shape at the moment travel is met, as fractions of the total
	// displacement, 0..1.
	//
	//   downness  how much of the movement was straight down
	//   inward    how much was across the body -- to the player's LEFT for a
	//             right-handed swing, mirrored when left_handed is on
	//
	// inward defaults to 0 (not required) because it is the gate most likely to
	// reject a real swing from a player who does not swing across as far as the
	// animation does. Raise it if lowering the arm is triggering melees.
	float minDownness = 0.55f;
	float minInward = 0.0f;

	// A swing that has not covered `minTravel` within this many seconds is not a
	// swing, it is an arm being lowered.
	float maxDuration = 0.7f;

	// Refractory period after a melee fires. One swing must be one melee.
	float cooldown = 0.8f;

	// How long "+melee" stays asserted. It has to survive long enough for the
	// engine to sample it into at least one usercmd, and a couple of frames is
	// not guaranteed when the frame rate dips.
	float holdSeconds = 0.12f;

	// ---- WHERE THE SWING LANDS --------------------------------------------
	//
	// Aim the melee along the player's GAZE rather than along the weapon hand.
	//
	// On by default because the alternative is not really a choice: the engine
	// resolves the hit along the view angles, aim decoupling makes those the
	// hand's, and a downward swing therefore always connects with the floor.
	bool aimForward = true;

	// How long past the button release to keep holding the gaze aim, in
	// seconds. The engine resolves the attack a few frames after "+melee" goes
	// down, and if the aim reverts before then the swing lands along the hand
	// after all -- which is the bug, arriving late.
	float aimTailSeconds = 0.25f;
};

class MeleeGesture
{
public:
	void SetSettings( const MeleeGestureSettings& s ) { m_settings = s; }
	const MeleeGestureSettings& Settings() const { return m_settings; }

	// Once per frame, after VRCamera::Apply so the poses are this frame's.
	void Update( const EngineClient& engine, IVRBackend& vr )
	{
		const DWORD now = GetTickCount();

		// Release first and unconditionally: a held "+melee" must come back up
		// even if everything below decides to bail this frame.
		ReleaseIfDue( engine, now );

		if ( !m_settings.enabled && !m_settings.debug )
			return;

		const ControllerPose& hand = vr.WeaponHand();
		const HmdPose& head = vr.Hmd();
		if ( !hand.valid || !head.valid )
		{
			// Tracking gone: abandon any candidate rather than measuring across
			// the gap, which would read as one enormous instant swing.
			if ( m_state == kSwinging )
				Reject( "tracking lost", now );
			m_rearmNeeded = true;
			return;
		}

		// Menus are driven by the OS cursor, and a player pointing at a menu is
		// waving the controller around. Nothing good comes of melee attacks
		// there.
		if ( InteractiveUiVisible() )
		{
			if ( m_state == kSwinging )
				Reject( "menu opened", now );
			m_rearmNeeded = true;
			return;
		}

		// The head's own yaw is the body frame. Only yaw: the play space floor
		// is the world floor however the head is tilted, so pitching to look
		// down must not rotate what "down" means.
		const float yawRad = head.angles.y * 0.01745329252f;
		const float cy = cosf( yawRad );
		const float sy = sinf( yawRad );
		// Source's right vector at zero pitch and roll.
		const float rightX = sy;
		const float rightY = -cy;

		const float speed = sqrtf( hand.velocity.x * hand.velocity.x +
								   hand.velocity.y * hand.velocity.y +
								   hand.velocity.z * hand.velocity.z );
		const float downSpeed = -hand.velocity.z;
		const float handHeight = hand.position.z - head.position.z;

		if ( speed > m_sessionPeakSpeed )
			m_sessionPeakSpeed = speed;

		// Every gate here is downstream of the runtime actually filling in
		// vVelocity. If it does not, nothing ever fires and the symptom is
		// indistinguishable from swinging too slowly -- so say so outright
		// rather than leaving a zero in the heartbeat to be interpreted.
		if ( !m_velocityChecked )
		{
			const float moved = fabsf( hand.position.x - m_lastPos.x )
					 + fabsf( hand.position.y - m_lastPos.y )
					 + fabsf( hand.position.z - m_lastPos.z );
			if ( m_haveLastPos && moved > 0.5f )
			{
				if ( speed > 0.0f )
				{
					m_velocityChecked = true;
				}
				else if ( ++m_zeroVelocityFrames > 90 )
				{
					m_velocityChecked = true;
					LogWarn( "melee: the runtime reports ZERO controller velocity "
						 "while the hand is moving. Gesture detection cannot "
						 "work on this driver -- vVelocity is unpopulated. "
						 "Nothing else is affected." );
				}
			}
			m_lastPos = hand.position;
			m_haveLastPos = true;
		}

		switch ( m_state )
		{
			case kIdle:
			{
				// Hysteresis. After any candidate ends -- fired or rejected --
				// the hand has to slow down before another can begin. Without
				// it, the tail of one swing immediately starts the next, which
				// double-fires on a single motion and fills the log.
				if ( m_rearmNeeded )
				{
					if ( downSpeed < m_settings.minSpeed * 0.5f )
						m_rearmNeeded = false;
					return;
				}

				if ( (DWORD)( now - m_lastFireMs ) < (DWORD)( m_settings.cooldown * 1000.0f ) &&
					 m_lastFireMs != 0 )
					return;

				if ( downSpeed < m_settings.minSpeed )
					return;

				// Started outside the start box. Counted rather than silent:
				// "nothing happens when I swing" and "my swings start outside
				// the box" are the same experience and different fixes.
				float sfwd = 0.0f, slat = 0.0f, sup = 0.0f;
				ToBodyFrame( hand.position, head.position, head.angles.y,
						 sfwd, slat, sup );
				if ( m_settings.startZone &&
					 !m_settings.startZone->Contains( sfwd, slat, sup ) )
				{
					++m_rejectLow;
					m_rearmNeeded = true;
					// Rate-limited: an arm swinging while the player walks crosses
					// the speed gate often, and one line per occurrence buries the
					// real swings. The counter still records every one.
					if ( m_settings.debug && (DWORD)( now - m_lastLowLogMs ) > 2000u )
					{
						m_lastLowLogMs = now;
						Log( "melee: candidate ignored -- started outside the start "
							 "box at fwd=%.1f lat=%.1f up=%.1f (down %.0f u/s)",
							 sfwd, slat, sup, downSpeed );
					}
					return;
				}

				m_state = kSwinging;
				m_startPos = hand.position;
				m_startMs = now;
				m_startHeight = handHeight;
				m_peakSpeed = speed;
				return;
			}

			case kSwinging:
			{
				if ( speed > m_peakSpeed )
					m_peakSpeed = speed;

				const float dx = hand.position.x - m_startPos.x;
				const float dy = hand.position.y - m_startPos.y;
				const float drop = m_startPos.z - hand.position.z;
				const float elapsed = (float)( now - m_startMs ) / 1000.0f;

				const float dist = sqrtf( dx * dx + dy * dy + drop * drop );
				const float downness = ( dist > 0.001f ) ? ( drop / dist ) : 0.0f;

				// Across the body: to the player's LEFT for a right-handed
				// swing, and the mirror of that when the weapon is in the left
				// hand. Resolved from IsLeftHanded rather than from which
				// physical controller it is, so this follows the same single
				// definition of "weapon hand" as everything else.
				const float lateral = dx * rightX + dy * rightY;
				const float inwardSign = vr.IsLeftHanded() ? 1.0f : -1.0f;
				const float inward = ( dist > 0.001f ) ? ( lateral * inwardSign / dist ) : 0.0f;

				if ( drop > m_sessionPeakTravel )
					m_sessionPeakTravel = drop;

				m_lastDrop = drop;
				m_lastDownness = downness;
				m_lastInward = inward;
				m_lastDuration = elapsed;

				if ( drop >= m_settings.minTravel )
				{
					if ( downness < m_settings.minDownness )
					{
						++m_rejectShape;
						Report( "not downward enough", now, downness, inward, drop, elapsed );
						return;
					}
					if ( inward < m_settings.minInward )
					{
						++m_rejectShape;
						Report( "not across the body", now, downness, inward, drop, elapsed );
						return;
					}

					Fire( engine, now, downness, inward, drop, elapsed );
					return;
				}

				if ( elapsed > m_settings.maxDuration )
				{
					++m_rejectSlow;
					Report( "too slow", now, downness, inward, drop, elapsed );
					return;
				}

				// Reversed before it got there. Tolerance rather than zero: the
				// bottom of a real swing decelerates through zero and a noisy
				// sample either side of it should not abort a swing that has
				// already travelled most of the way.
				if ( -downSpeed > m_settings.minSpeed * 0.25f )
				{
					++m_rejectReversed;
					Report( "reversed", now, downness, inward, drop, elapsed );
					return;
				}
				return;
			}
		}
	}

	// Called when VR stops driving the game, for the same reason GameInput has
	// one: leaving a held command asserted while the player takes the headset
	// off is its own kind of bug.
	void Release( const EngineClient& engine )
	{
		if ( m_held && engine.Valid() )
		{
			engine.ClientCmd( "-melee\n" );
			m_held = false;
		}
	}

	void LogState() const
	{
		if ( !m_settings.enabled && !m_settings.debug )
			return;

		Log( "melee: gesture %s fired=%u rejected[outside box=%u shape=%u slow=%u reversed=%u] "
			 "| thresholds speed=%.0f travel=%.0f down=%.2f inward=%.2f",
			 m_settings.enabled ? "ON" : "OFF (measuring only)",
			 m_fired, m_rejectLow, m_rejectShape, m_rejectSlow, m_rejectReversed,
			 m_settings.minSpeed, m_settings.minTravel,
			 m_settings.minDownness, m_settings.minInward );

		// The session maxima are the calibration numbers: they say what this
		// player's arm actually produces, which is the only basis on which the
		// thresholds above can be set honestly.
		Log( "melee: best seen this session peak=%.0f u/s travel=%.1f u | "
			 "last candidate travel=%.1fu down=%.2f inward=%.2f dur=%.2fs",
			 m_sessionPeakSpeed, m_sessionPeakTravel,
			 m_lastDrop, m_lastDownness, m_lastInward, m_lastDuration );
	}

	unsigned int Fired() const { return m_fired; }

	// True while the swing should be aimed along the player's gaze. Read once
	// per frame by the caller and pushed into the camera -- the gesture does not
	// reach into the camera itself, for the same reason nothing else here does.
	bool AimForward() const
	{
		if ( !m_settings.enabled || !m_settings.aimForward || m_aimUntilMs == 0 )
			return false;
		return (DWORD)( GetTickCount() - m_aimUntilMs ) > 0x80000000u;
	}

	// Is a swing being evaluated, or was one just issued? Asked by the
	// arcade-reload gesture, which shares the bottom of a melee swing's
	// path and must not claim the same motion.
	//
	// Deliberately conservative on both ends: a candidate in progress
	// counts even if it goes on to be rejected, because the hand is
	// travelling for melee reasons either way; and the window after a
	// fire is melee's own cooldown, so retuning that setting keeps the
	// two detectors consistent without a second number to maintain.
	bool Busy( DWORD now ) const
	{
		if ( !m_settings.enabled && !m_settings.debug )
			return false;
		if ( m_state == kSwinging )
			return true;
		if ( m_lastFireMs == 0 )
			return false;
		return (DWORD)( now - m_lastFireMs ) <
				   (DWORD)( m_settings.cooldown * 1000.0f );
	}

	// "A melee is happening RIGHT NOW", which is a different question from
	// Busy() and the one the reload interlock actually wants.
	//
	// Busy() folds in melee's own COOLDOWN (0.8s), which exists to stop a
	// second swing -- a reason that has nothing to do with reloading. Using it
	// as the reload interlock meant one swing suppressed the reload for the
	// whole cooldown, and a swing ENDS in the reload zone by construction
	// (start at or above melee_arm_height, travel melee_min_travel down), so
	// the hand was sitting exactly where a reload is performed while reload was
	// switched off. That is the "I lowered the gun and it meleed instead of
	// reloading" report.
	//
	// The window here is the button hold plus the aim tail, because that is how
	// long the ATTACK takes to resolve: the engine reads the melee a few frames
	// after "+melee" goes down, which is the whole reason aimTailSeconds
	// exists. Issuing "+reload" inside it would land on a weapon mid-swing and
	// be dropped by the engine -- the same silent failure in a new place.
	//
	// Measured in time rather than off m_held so it reads identically in
	// calibration mode (melee_gesture = 0), where the button is never pressed
	// but every candidate is still evaluated.
	bool Active( DWORD now ) const
	{
		if ( !m_settings.enabled && !m_settings.debug )
			return false;
		if ( m_state == kSwinging )
			return true;
		if ( m_lastFireMs == 0 )
			return false;
		const float window = m_settings.holdSeconds + m_settings.aimTailSeconds;
		return (DWORD)( now - m_lastFireMs ) < (DWORD)( window * 1000.0f );
	}

private:
	enum State
	{
		kIdle = 0,
		kSwinging = 1,
	};

	// Source shows the OS cursor for menus and hides it in gameplay. Read
	// directly rather than borrowed from the stereo renderer, which only tracks
	// it inside its own eye passes -- so it would read stale, or permanently
	// false, whenever stereo is off.
	static bool InteractiveUiVisible()
	{
		CURSORINFO ci = {};
		ci.cbSize = sizeof( ci );
		if ( !GetCursorInfo( &ci ) )
			return false;
		return ( ci.flags & CURSOR_SHOWING ) != 0;
	}

	void Fire( const EngineClient& engine, DWORD now,
			   float downness, float inward, float drop, float elapsed )
	{
		++m_fired;
		m_state = kIdle;
		m_rearmNeeded = true;
		m_lastFireMs = now;

		if ( m_settings.enabled && engine.Valid() )
		{
			// A pair, not an impulse. "+melee" alone leaves the button stuck
			// down; the release is scheduled rather than issued immediately
			// because the engine samples key state into a usercmd once per
			// frame, and a press that goes down and up inside one frame can be
			// missed entirely.
			engine.ClientCmd( "+melee\n" );
			m_held = true;
			m_releaseAtMs = now + (DWORD)( m_settings.holdSeconds * 1000.0f );

			// Outlives the button on purpose -- see aimTailSeconds.
			m_aimUntilMs = m_releaseAtMs +
						   (DWORD)( m_settings.aimTailSeconds * 1000.0f );
		}

		Log( "melee: %s -- travel=%.1fu peak=%.0f u/s down=%.2f inward=%.2f "
			 "startH=%.1fu dur=%.2fs",
			 m_settings.enabled ? "SWING" : "would have swung (melee_gesture=0)",
			 drop, m_peakSpeed, downness, inward, m_startHeight, elapsed );
	}

	void Report( const char* why, DWORD now,
				 float downness, float inward, float drop, float elapsed )
	{
		m_state = kIdle;
		m_rearmNeeded = true;
		(void)now;

		if ( !m_settings.debug )
			return;

		// Every measured value, every time, including the ones that passed --
		// a rejection reason on its own says which gate stopped it but not how
		// far off it was, and that difference is the whole tuning job.
		Log( "melee: rejected (%s) -- travel=%.1fu/%.0f peak=%.0f u/s down=%.2f/%.2f "
			 "inward=%.2f/%.2f startH=%.1fu dur=%.2fs/%.2f",
			 why, drop, m_settings.minTravel, m_peakSpeed,
			 downness, m_settings.minDownness, inward, m_settings.minInward,
			 m_startHeight, elapsed, m_settings.maxDuration );
	}

	void Reject( const char* why, DWORD now )
	{
		Report( why, now, m_lastDownness, m_lastInward, m_lastDrop, m_lastDuration );
	}

	void ReleaseIfDue( const EngineClient& engine, DWORD now )
	{
		if ( !m_held )
			return;
		if ( (DWORD)( now - m_releaseAtMs ) > 0x80000000u )
			return;   // not due yet (unsigned wrap-safe compare)
		if ( engine.Valid() )
			engine.ClientCmd( "-melee\n" );
		m_held = false;
	}

	MeleeGestureSettings m_settings;

	State m_state = kIdle;
	bool m_rearmNeeded = false;
	bool m_held = false;
	DWORD m_releaseAtMs = 0;
	DWORD m_startMs = 0;
	DWORD m_lastFireMs = 0;

	Vector m_startPos = { 0.0f, 0.0f, 0.0f };
	float m_startHeight = 0.0f;
	float m_peakSpeed = 0.0f;

	// Diagnostics.
	unsigned int m_fired = 0;
	// End of the gaze-aim window. Wraps safely: the comparison in AimForward
	// is done on the signed difference, not on the raw values.
	DWORD m_aimUntilMs = 0;
	unsigned int m_rejectLow = 0;
	unsigned int m_rejectShape = 0;
	unsigned int m_rejectSlow = 0;
	unsigned int m_rejectReversed = 0;
	float m_sessionPeakSpeed = 0.0f;
	float m_sessionPeakTravel = 0.0f;
	float m_lastDrop = 0.0f;
	float m_lastDownness = 0.0f;
	float m_lastInward = 0.0f;
	float m_lastDuration = 0.0f;
	DWORD m_lastLowLogMs = 0;

	// One-shot check that the runtime populates vVelocity at all.
	Vector m_lastPos = { 0.0f, 0.0f, 0.0f };
	bool m_haveLastPos = false;
	bool m_velocityChecked = false;
	int m_zeroVelocityFrames = 0;
};

} // namespace sinvr
