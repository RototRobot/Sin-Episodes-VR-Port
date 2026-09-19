// Two-handed weapons: bring the off hand up to the foregrip and the gun points
// along the line between your hands.
//
// ---- THE BLEND IS ONE VECTOR ------------------------------------------------
//
// One-handed, the gun's direction comes from the weapon hand's own orientation
// through `ComposeWeaponAngles` -- controller pose plus the fixed
// controller-to-weapon tilt. Two-handed, that is the wrong source: a real rifle
// points wherever the two grips put it, and the rear hand's wrist angle stops
// mattering almost entirely.
//
// So when the grip is engaged the direction becomes
//
//     forward = normalize( offHand - weaponHand )
//
// in ROOM space, for the same reason everything else here is: both hands are
// already expressed in it, a snap turn moves neither relative to the other, and
// there is nothing to convert.
//
// ROLL is deliberately NOT taken from that vector -- a line between two points
// has no roll. It keeps coming from the weapon hand's wrist, so canting the gun
// still works while both hands are on it.
//
// ---- THE AIM AND THE MODEL MUST USE THE SAME ANGLES -------------------------
//
// This is the trap the handover already records at a cost of one session:
// applying the weapon tilt to the model but not to the aim put the shots 58
// degrees off the gun. The same applies here, more sharply, because the blend
// moves the direction by however far apart the player's hands are.
//
// So the blend is not applied here. It is handed to `VRCamera`, and BOTH the aim
// and the viewmodel ask `VRCamera::EffectiveWeaponAngles` for the angles in
// force. There is exactly one answer to "where is the gun pointing", and neither
// caller can accidentally get a different one.
//
// ---- WHERE THE FOREGRIP IS ---------------------------------------------------
//
// A point in the WEAPON HAND's own frame, per weapon, because a rifle's foregrip
// is most of an arm's length forward of the trigger while a pistol's support hand
// cups just below and ahead of the firing hand. Same shape as the per-weapon
// viewmodel offsets, keyed on the same model name from `IVModelInfo`, with the
// global values as the fallback for anything not tuned.
//
// The axes are the CONTROLLER's and do not agree with the player's -- a
// controller's forward runs along the handle, which reads as up-and-down through
// a headset. See hand_marker.h, where the same confusion is spelled out.
//
// ---- HYSTERESIS --------------------------------------------------------------
//
// Engage and release use different radii. With one threshold, a hand hovering at
// the boundary flickers the grip on and off several times a second, and since the
// grip changes where the gun POINTS that is far worse than a flickering
// indicator -- it would fight the player's aim.
#pragma once

#include <windows.h>
#include <math.h>
#include "../sdk/source_interfaces.h"
#include "../vr/vr_backend.h"
#include "../../common/log.h"

namespace sinvr {

struct GripOffsets
{
	char key[32] = { 0 };
	float forward = 0.0f;
	float right = 0.0f;
	float up = 0.0f;
	// How much authority the hand-to-hand line has. See TwoHandedSettings.
	float blend = 1.0f;
	bool configured = false;   // false = use the global values
};

// ---- HOW THE OFF HAND TAKES HOLD -------------------------------------------
//
// Three ways, because they suit different people and different weapons and
// there is no single right answer.
//
//   kAuto    proximity alone. Bring the off hand to the foregrip and it takes
//            hold; move it away and it lets go. Nothing to press, works on
//            every controller including Vive wands. The original behaviour and
//            still the default.
//
//   kToggle  press the OFF HAND's grip inside the zone to take hold, press
//            again to let go. Best for long stretches of two-handed shooting,
//            because nothing has to be held down.
//
//   kHold    the off hand's grip must stay pressed. Closest to actually
//            gripping a rifle, and it cannot be left latched by accident.
//
// In BOTH button modes the radius only gates the grab. Once hold is taken, the
// button is the only thing that releases it -- moving the hand away does not.
// That is the point of choosing a button mode: the player decides when the gun
// is held, not the geometry.
//
// The button is the OFF hand's grip -- the right hand in left-handed mode, the
// left hand in right-handed mode -- and it is free because the three stick
// profiles bind NextWeapon on both grips while only the weapon hand's read is
// used. See VRInputState::offHandGrip.
//
// **Vive wands cannot use kToggle or kHold.** Their grips carry Use and Reload
// and NextWeapon sits on the application menu button, so there is no free grip
// to press. kAuto is the mode for wands, and it is why kAuto stays the default.
enum TwoHandedMode
{
	kTwoHandedAuto = 0,
	kTwoHandedToggle,
	kTwoHandedHold,
};

inline const char* TwoHandedModeName( int m )
{
	switch ( m )
	{
		case kTwoHandedToggle: return "toggle";
		case kTwoHandedHold:   return "hold";
		default:               return "auto";
	}
}

struct TwoHandedSettings
{
	// Off by default: it changes where the gun points, which is the most
	// disruptive thing this mod can do without warning.
	bool enabled = false;

	// Log every engage and release with the distance that caused it.
	bool debug = false;

	// auto / toggle / hold. See TwoHandedMode.
	int mode = kTwoHandedAuto;

	// How close the off hand must come to the foregrip point to engage, and how
	// far it must go to let go again. Release MUST be the larger.
	//
	// `radius` applies in EVERY mode -- it is what stops a grip pressed with the
	// hand by your side swinging the weapon round to point at your hip.
	//
	// `releaseRadius` applies ONLY to kAuto. In kToggle and kHold the button is
	// the only thing that lets go, so there is nothing for a release radius to
	// do; see the mode switch in Update.
	float radius = 6.0f;
	float releaseRadius = 9.0f;

	// The foregrip, in the weapon hand's own frame. Global fallback; per-weapon
	// values override it.
	float gripForward = 0.0f;
	float gripRight = 0.0f;
	float gripUp = 7.5f;

	// How much the hand-to-hand line steers the gun, 0..1.
	//
	// 1 is a rifle: the off hand sits well forward on the weapon, so the line
	// between the hands IS the barrel and the rear wrist stops mattering.
	//
	// 0 is a pistol, and it is not a cop-out. A pistol's support hand wraps
	// BESIDE and BELOW the firing hand rather than in front of it, so the
	// hand-to-hand vector points down and sideways and has nothing to do with
	// where the barrel is aimed -- letting it steer sends the gun somewhere the
	// player never pointed. The support hand is still detected, it just gets no
	// vote on direction.
	//
	// Anything between is a weighted average of the two forward vectors, which
	// is the right shape for a weapon gripped somewhere in between.
	float blend = 1.0f;
};

class TwoHanded
{
public:
	void SetSettings( const TwoHandedSettings& s )
	{
		m_settings = s;
		if ( m_settings.releaseRadius <= m_settings.radius )
			m_settings.releaseRadius = m_settings.radius + 2.0f;
	}
	const TwoHandedSettings& Settings() const { return m_settings; }

	// Per-weapon foregrip, from config at startup. Same keying as the viewmodel
	// offsets so one weapon means one name everywhere.
	void SetModelGrip( const char* key, float fwd, float right, float up,
					  float blend )
	{
		const int i = FindOrAdd( key );
		if ( i < 0 )
			return;
		m_models[i].forward = fwd;
		m_models[i].right = right;
		m_models[i].up = up;
		m_models[i].blend = blend;
		m_models[i].configured = true;
	}

	// Whether the player is actually holding a weapon. Pushed in rather than
	// derived from the model key: the key needs IVModelInfo to have resolved a
	// NAME, and "we could not identify the gun" must not read as "there is no
	// gun". See ViewModelAnim::WeaponInHand().
	void SetWeaponInHand( bool held ) { m_weaponInHand = held; }
	bool WeaponInHand() const { return m_weaponInHand; }

	void SetCurrentModel( const char* key )
	{
		if ( !key )
		{
			m_current = -1;
			return;
		}
		if ( m_current >= 0 && _stricmp( m_models[m_current].key, key ) == 0 )
			return;
		m_current = FindOrAdd( key );
		if ( m_current >= 0 && m_settings.enabled )
		{
			const GripOffsets g = Grip();
			Log( "grip: now holding %s -- foregrip at fwd=%.1f right=%.1f up=%.1f, "
				 "blend %.2f%s",
				 key, g.forward, g.right, g.up, g.blend,
				 m_models[m_current].configured
					 ? "" : " (global default -- not tuned for this weapon)" );
		}
	}

	// Once per frame, after the poses are fresh and BEFORE VRCamera::Apply, so
	// the angles it hands over are this frame's.
	void Update( IVRBackend& vr )
	{
		m_engaged = false;
		m_haveGrip = false;

		if ( !m_settings.enabled && !m_settings.debug )
			return;

		const VRInputState& in = vr.Input();
		const bool gripHeld = in.valid && in.offHandGrip;
		const bool gripPressed = in.valid && in.offHandGripPressed;

		const ControllerPose& weapon = vr.WeaponHand();
		const ControllerPose& off = vr.OffHand();
		if ( !weapon.valid || !off.valid )
		{
			// A grip cannot be held by a hand that is not tracked. Released
			// rather than latched, or the gun would stay pointing along a line
			// to a controller that has gone to sleep.
			if ( m_wasEngaged && m_settings.debug )
				Log( "grip: released -- a hand stopped tracking" );
			m_wasEngaged = false;
			// A latch cannot survive a hand that is gone. Otherwise the gun is
			// still "held" by a controller on the desk, and comes back holding
			// it the moment tracking returns.
			m_latched = false;
			return;
		}

		// ---- NOTHING TO HOLD WITH TWO HANDS --------------------------------
		//
		// There is no gun, so there is no barrel for the hands to line up, and
		// the blend has nothing to mean. Engaging anyway pointed the ENGINE's
		// aim -- and therefore the player's movement basis -- straight down the
		// bare line between two empty hands. Measured on the intro maps: the
		// grip toggled on, latched, and put the aim 90 to 125 degrees away from
		// the head for the rest of the session.
		//
		// Same shape as the tracking case above: released, and the latch
		// dropped, so putting a weapon away cannot leave a grip held on it.
		if ( !m_weaponInHand )
		{
			if ( m_wasEngaged && m_settings.debug )
				Log( "grip: released -- no weapon in hand" );
			m_wasEngaged = false;
			m_latched = false;
			return;
		}

		// ---- the foregrip point, in the weapon hand's frame -----------------
		Vector fwdAxis, rightAxis, upAxis;
		Basis( weapon.angles, fwdAxis, rightAxis, upAxis );

		const GripOffsets g = Grip();
		m_gripRoom.x = weapon.position.x + fwdAxis.x * g.forward
					   + rightAxis.x * g.right + upAxis.x * g.up;
		m_gripRoom.y = weapon.position.y + fwdAxis.y * g.forward
					   + rightAxis.y * g.right + upAxis.y * g.up;
		m_gripRoom.z = weapon.position.z + fwdAxis.z * g.forward
					   + rightAxis.z * g.right + upAxis.z * g.up;
		m_haveGrip = true;

		const float dx = off.position.x - m_gripRoom.x;
		const float dy = off.position.y - m_gripRoom.y;
		const float dz = off.position.z - m_gripRoom.z;
		m_distance = sqrtf( dx * dx + dy * dy + dz * dz );

		// Hysteresis: harder to reach than to keep. Without it the boundary
		// flickers the grip on and off several times a second, and since the
		// grip changes where the gun POINTS that is far worse than a flickering
		// indicator would be.
		const float gate = m_wasEngaged ? m_settings.releaseRadius : m_settings.radius;
		const bool inReach = ( m_distance <= gate );

		// ---- WHO DECIDES, AND WHO ONLY GATES -------------------------------
		//
		// Distance does two different jobs depending on the mode, and
		// conflating them was wrong:
		//
		//   kAuto             distance decides BOTH -- it engages and it
		//                     releases. Nothing else to consult.
		//   kToggle, kHold    distance only GATES THE GRAB. Once the button has
		//                     taken hold, the button is the only thing that
		//                     lets go.
		//
		// The button modes exist precisely so the player is in charge of when
		// the gun is held. Letting the radius release as well would mean a
		// deliberate grip could be broken by the hand drifting -- which is the
		// behaviour the player chose a button mode to escape. So there is no
		// release radius in these two: press again, or let go.
		//
		// The consequence is deliberate and worth stating: hold the grip and
		// move the off hand away, and the gun keeps pointing along the line to
		// it. That is what "still holding it" means, and the way out is the
		// same button that took hold.
		//
		// Grabbing uses the TIGHT radius, not the hysteresis gate. Hysteresis
		// only exists to stop a boundary flickering, and a boundary that is
		// crossed once on a button press cannot flicker.
		bool nowEngaged = false;
		switch ( m_settings.mode )
		{
			case kTwoHandedHold:
				if ( m_latched )
					m_latched = gripHeld;                 // the button, and only it
				else
					m_latched = gripHeld && ( m_distance <= m_settings.radius );
				nowEngaged = m_latched;
				break;

			case kTwoHandedToggle:
				if ( gripPressed )
				{
					if ( m_latched )
						m_latched = false;                // the button, and only it
					else if ( m_distance <= m_settings.radius )
						m_latched = true;
					else if ( m_settings.debug )
						Log( "grip: toggle pressed %.1fu from the foregrip -- too "
							 "far to take hold (needs %.1f)",
							 m_distance, m_settings.radius );
				}
				nowEngaged = m_latched;
				break;

			default:
				// Proximity alone, with hysteresis. No latch to keep.
				m_latched = false;
				nowEngaged = inReach;
				break;
		}

		// ---- the blend ------------------------------------------------------
		//
		// Straight from hand to hand. Note this uses the HAND positions, not the
		// foregrip point: the grip point decides WHETHER the player is holding
		// the gun two-handed, the hands decide where it then points. Using the
		// grip point here would make the gun swing about its own offset.
		const float bx = off.position.x - weapon.position.x;
		const float by = off.position.y - weapon.position.y;
		const float bz = off.position.z - weapon.position.z;
		const float len = sqrtf( bx * bx + by * by + bz * bz );

		if ( nowEngaged && len > 1.0f )
		{
			// Source's convention: pitch is positive DOWNWARDS.
			m_blend.x = -asinf( bz / len ) * 57.2957795130823f;
			m_blend.y = atan2f( by, bx ) * 57.2957795130823f;
			m_blend.z = 0.0f;   // roll comes from the wrist, not from a line
			m_engaged = true;
		}

		if ( m_engaged != m_wasEngaged )
		{
			if ( m_engaged )
				++m_engagements;
			if ( m_settings.debug )
			{
				// Mode-honest: quoting a release radius in a button mode would
				// send anyone tuning this to a number that does nothing.
				char gates[64];
				if ( m_settings.mode == kTwoHandedAuto )
					_snprintf_s( gates, sizeof( gates ), _TRUNCATE,
								 "engage %.1f, release %.1f",
								 m_settings.radius, m_settings.releaseRadius );
				else
					_snprintf_s( gates, sizeof( gates ), _TRUNCATE,
								 "grab within %.1f, released by the button only",
								 m_settings.radius );

				Log( "grip: %s [%s] -- off hand %.1fu from the foregrip (%s), "
					 "hands %.1fu apart%s",
					 m_engaged ? "ENGAGED" : "released",
					 TwoHandedModeName( m_settings.mode ),
					 m_distance, gates, len,
					 m_settings.enabled ? "" : "  [measuring only]" );
			}
			m_wasEngaged = m_engaged;
		}
	}

	// True only when the blend should actually be applied. Debug mode measures
	// and reports without ever moving the gun.
	bool Active() const { return m_engaged && m_settings.enabled; }
	const QAngle& BlendedAngles() const { return m_blend; }

	// 0 leaves the gun entirely on the weapon hand; 1 hands it to the line
	// between the hands. Per weapon.
	float BlendWeight() const { return Grip().blend; }

	// Room-space foregrip point, for the marker that makes this tunable.
	bool GripPoint( Vector& out ) const
	{
		if ( !m_haveGrip )
			return false;
		out = m_gripRoom;
		return true;
	}
	bool Engaged() const { return m_engaged; }
	float Distance() const { return m_distance; }

	void LogState() const
	{
		if ( !m_settings.enabled && !m_settings.debug )
			return;

		const GripOffsets g = Grip();
		Log( "grip: two-handed %s | %s | off hand %.1fu from the foregrip "
			 "(engage %.1f release %.1f) | grip fwd=%.1f right=%.1f up=%.1f blend %.2f%s "
			 "| engaged %u times",
			 m_settings.enabled ? "ON" : "OFF (measuring only)",
			 m_weaponInHand
				 ? ( m_engaged ? "HOLDING" : "one-handed" )
				 : "UNARMED -- grip cannot engage",
			 m_distance, m_settings.radius, m_settings.releaseRadius,
			 g.forward, g.right, g.up, g.blend,
			 ( m_current >= 0 && m_models[m_current].configured )
				 ? "" : " (global)",
			 m_engagements );
	}

private:
	// Source's AngleVectors, sign conventions and all: pitch positive downwards,
	// right along -Y at zero yaw.
	static void Basis( const QAngle& a, Vector& fwd, Vector& right, Vector& up )
	{
		const float p = a.x * 0.01745329252f;
		const float y = a.y * 0.01745329252f;
		const float r = a.z * 0.01745329252f;
		const float sp = sinf( p ), cp = cosf( p );
		const float sy = sinf( y ), cy = cosf( y );
		const float sr = sinf( r ), cr = cosf( r );

		fwd = Vector{ cp * cy, cp * sy, -sp };
		right = Vector{ -sr * sp * cy + cr * sy,
						-sr * sp * sy - cr * cy,
						-sr * cp };
		up = Vector{ cr * sp * cy + sr * sy,
					 cr * sp * sy - sr * cy,
					 cr * cp };
	}

	GripOffsets Grip() const
	{
		if ( m_current >= 0 && m_models[m_current].configured )
			return m_models[m_current];
		GripOffsets out;
		out.forward = m_settings.gripForward;
		out.right = m_settings.gripRight;
		out.up = m_settings.gripUp;
		out.blend = m_settings.blend;
		return out;
	}

	int FindOrAdd( const char* key )
	{
		if ( !key )
			return -1;
		for ( int i = 0; i < m_modelCount; ++i )
			if ( _stricmp( m_models[i].key, key ) == 0 )
				return i;
		if ( m_modelCount >= (int)( sizeof( m_models ) / sizeof( m_models[0] ) ) )
			return -1;
		const int i = m_modelCount++;
		strncpy_s( m_models[i].key, key, _TRUNCATE );
		return i;
	}

	TwoHandedSettings m_settings;

	GripOffsets m_models[8];
	int m_modelCount = 0;
	int m_current = -1;
	// Assumed true until told otherwise, so a build where the signal never
	// arrives behaves as it always did rather than losing the grip silently.
	bool m_weaponInHand = true;

	bool m_engaged = false;
	bool m_wasEngaged = false;
	bool m_haveGrip = false;
	// Button-driven intent, for kToggle and kHold. Unused by kAuto.
	bool m_latched = false;
	Vector m_gripRoom = { 0.0f, 0.0f, 0.0f };
	QAngle m_blend = { 0.0f, 0.0f, 0.0f };
	float m_distance = 0.0f;
	unsigned int m_engagements = 0;
};

} // namespace sinvr
