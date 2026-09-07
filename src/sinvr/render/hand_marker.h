// A box where each hand is.
//
// This is the thing DEAD END #1 was originally trying to do, and it works now
// that the overlay rules are understood: clear the list at the top of every eye
// pass, and submit inside each pass rather than once per frame. See
// `debug_overlay.h` for why -- the short version is that `duration 0` outlives a
// frame, so the old copy was still there at the old position.
//
// ---- WHAT IT IS FOR ----------------------------------------------------------
//
// Two-handed weapons. The maths is a blend: when the off hand is gripping near
// the foregrip, the weapon's forward should come from the HAND-TO-HAND vector
// rather than from the weapon hand's own orientation. None of that can be
// reasoned about, let alone tuned, while the off hand is invisible -- there is no
// way to tell "my hand is not where I think it is" from "the blend is wrong".
//
// So this is an instrument first and a feature second. It draws:
//
//   * a box at the hand, ROTATED to the controller's orientation, so the pivot
//     is visible and not just the position
//   * a short line along the controller's forward axis, because a small rotated
//     cube reads as almost the same cube at any angle, and the direction the
//     hand points is the whole question for a foregrip
//
// ---- IT USES THE VIEWMODEL'S BASE, NOT THE EYE'S -----------------------------
//
// The base origin handed in must be the one the VIEWMODEL is built from -- the
// engine's view origin plus the duck compensation -- and not the raw eye. The
// viewmodel needed that fix because the engine's base is already ducked while the
// hand offset is measured from the standing recentre reference; a marker on the
// raw base would sink through the floor when the player crouched, in exactly the
// way the gun used to. Sharing the base is what keeps the box on the gun.
#pragma once

#include <windows.h>
#include <math.h>
#include "../sdk/source_interfaces.h"
#include "../sdk/debug_overlay.h"
#include "../vr/vr_backend.h"
#include "../vr_camera.h"
#include "../../common/log.h"

namespace sinvr {

enum HandMarkerMode
{
	kHandMarkerOff = 0,
	kHandMarkerOffHand = 1,   // the empty hand -- what two-handed grip needs
	kHandMarkerBoth = 2,      // both, for comparing one against the other

	// Off hand always; weapon hand ONLY while the player is unarmed.
	//
	// The default, and the reason it exists: with a gun in hand the weapon
	// itself shows where that hand is, far better than a box floating over it.
	// With NO gun -- the whole intro, and every stretch after a weapon is taken
	// away -- nothing marks the primary hand at all and it simply vanishes.
	// Same argument the grip already makes for hiding the off-hand marker.
	kHandMarkerAuto = 3,
};

struct HandMarkerSettings
{
	HandMarkerMode mode = kHandMarkerOff;

	// Half-extent of the box, in units (~inches).
	//
	// Small on purpose now that it is a permanent fixture rather than a tuning
	// aid: it marks where the hand is without covering what is behind it.
	float size = 0.75f;

	// Length of the forward whisker, in units. Longer than the box on purpose:
	// it is the part that answers "which way is this hand pointing".
	float forwardLength = 6.0f;

	// Where the box sits relative to the TRACKED POSE, in the controller's own
	// frame.
	//
	// OpenVR's controller pose is not the middle of the controller. It is a
	// defined reference point, and on the hardware this was tuned against it
	// sits roughly 1.5 units ABOVE the visual centre -- near the top of the
	// handle. A box drawn straight on the pose therefore hovers at the top of
	// the controller rather than around it, which is what was reported.
	//
	// Applied in the CONTROLLER's frame, not the world's, because the offset is
	// a fixed property of the hardware -- it has to rotate with the hand or it
	// would drift off as soon as the wrist turned.
	//
	// Worth knowing beyond cosmetics: a two-handed grip pivots about the
	// TRACKED pose, so this offset is exactly the discrepancy between where the
	// maths thinks the hand is and where the player feels it.
	// AXIS NAMES ARE THE CONTROLLER'S, NOT THE PLAYER'S, and they do not
	// agree. A controller's tracked forward runs along the HANDLE, and held
	// naturally that reads as up-and-down from inside the headset -- the same
	// ~58 degree discrepancy viewmodel_angle_pitch exists to correct for the
	// weapon. So `offsetUp` visibly moves the box forwards, which is
	// bewildering until the axes are named out loud:
	//
	//   offsetForward -> along the WHISKER. This is the one that looks like
	//                    up and down, and the one that centres the box on the
	//                    controller.
	//   offsetUp      -> perpendicular to the handle; reads as forward/back.
	//   offsetRight   -> across the handle.
	//
	// The whisker is drawn along offsetForward's axis precisely so this is
	// visible rather than something to work out by trial.
	float offsetForward = -1.5f;
	float offsetRight = 0.0f;
	float offsetUp = 0.0f;
};

class HandMarker
{
public:
	void SetSettings( const HandMarkerSettings& s ) { m_settings = s; }
	const HandMarkerSettings& Settings() const { return m_settings; }

	bool Bind( void* debugOverlayIface ) { return m_overlay.Bind( debugOverlayIface ); }
	bool Ready() const
	{
		return m_settings.mode != kHandMarkerOff && m_overlay.Valid();
	}

	// Once per frame. `base` must be the VIEWMODEL's base origin -- see the
	// header -- so the boxes sit on the hands and not below them while crouched.
	// `unarmed` comes from ViewModelAnim::Unarmed(), which reads m_hWeapon.
	//
	// NOT WeaponInHand(), and the distinction cost a build: that one reads
	// m_nModelIndex, which on the intro maps is >= 0 even with no gun, so it
	// reported "armed" for the entire stretch this mode exists to cover. The two
	// questions look identical and are answered by different fields. Unarmed()
	// also reads UNKNOWN as armed, so a missing offset draws nothing rather than
	// putting a box over a gun.
	void Prepare( const Vector& base, IVRBackend& vr, const VRCamera& camera,
				  bool gripEngaged, bool unarmed, unsigned int weaponHandle )
	{
		m_count = 0;
		m_unarmed = unarmed;
		m_weaponHandle = weaponHandle;
		if ( !Ready() )
			return;

		// Hidden while the hand is ON the gun. Once the grip is held the weapon
		// itself shows where that hand is, and far better than a box floating
		// over it -- the marker exists to find the gun, not to sit on it.
		if ( gripEngaged )
			return;

		// Grey rather than a signal colour: this is a quiet permanent marker,
		// and anything brighter competes with the crosshair for attention.
		AddHand( base, camera, vr.OffHand(), 170, 170, 170 );

		if ( ShowWeaponHand() )
			AddHand( base, camera, vr.WeaponHand(), 255, 224, 32 );   // amber
	}

	// Amber, not grey, and deliberately so in `auto` too: the weapon hand's box
	// is the one that comes and goes, and a marker that appears and disappears
	// needs to be obviously a different thing from the one that is always there.
	bool ShowWeaponHand() const
	{
		if ( m_settings.mode == kHandMarkerBoth )
			return true;
		return m_settings.mode == kHandMarkerAuto && m_unarmed;
	}

	// The foregrip point, in ROOM space, and whether the off hand is on it.
	//
	// Drawn small and without the pose offset: this is a point in space, not a
	// controller, so the tracked-pose correction that the hand boxes need does
	// not apply to it.
	void PrepareGrip( const Vector& base, const VRCamera& camera,
					 const Vector& gripRoom, bool engaged )
	{
		m_haveGrip = false;
		if ( !Ready() )
			return;

		const Vector offset = camera.WorldOffsetForRoomPos( gripRoom );
		m_gripWorld.x = base.x + offset.x;
		m_gripWorld.y = base.y + offset.y;
		m_gripWorld.z = base.z + offset.z;
		m_gripEngaged = engaged;
		m_haveGrip = true;
	}

	// Once per EYE PASS, after the overlay list has been cleared.
	void Submit()
	{
		if ( !Ready() )
			return;

		const float h = m_settings.size;
		const Vector mins = { -h, -h, -h };
		const Vector maxs = { h, h, h };

		for ( int i = 0; i < m_count; ++i )
		{
			const Marker& m = m_markers[i];

			// Rotated with the controller, so the box shows the hand's
			// ORIENTATION and not merely where it is.
			m_overlay.Box( m.origin, mins, maxs, m.angles, m.r, m.g, m.b, 96, 0.0f );

			// The whisker. Without it a small rotated cube looks much the same
			// from any angle, and pointing direction is the whole question.
			const Vector tip = { m.origin.x + m.forward.x * m_settings.forwardLength,
								 m.origin.y + m.forward.y * m_settings.forwardLength,
								 m.origin.z + m.forward.z * m_settings.forwardLength };
			m_overlay.Line( m.origin, tip, m.r, m.g, m.b, false, 0.0f );
		}
		// The foregrip. Green when the grip is held, dim white when it is not,
		// so engagement is visible without reading the log.
		if ( m_haveGrip )
		{
			const float gh = m_settings.size * 0.6f;
			const Vector gmins = { -gh, -gh, -gh };
			const Vector gmaxs = { gh, gh, gh };
			const QAngle none = { 0.0f, 0.0f, 0.0f };
			const int gr = m_gripEngaged ? 32 : 200;
			const int gg = m_gripEngaged ? 255 : 200;
			const int gb = m_gripEngaged ? 32 : 200;
			m_overlay.Box( m_gripWorld, gmins, gmaxs, none, gr, gg, gb,
						   m_gripEngaged ? 160 : 64, 0.0f );
		}

		++m_submissions;
	}

	void LogState( IVRBackend* vr ) const
	{
		if ( m_settings.mode == kHandMarkerOff )
			return;

		if ( !m_overlay.Valid() )
		{
			LogWarn( "hands: markers enabled but VDebugOverlay003 is not bound" );
			return;
		}

		// Reports the EFFECT. A marker count of 0 with the feature on means the
		// hand is not tracked, which from inside the headset is indistinguishable
		// from the drawing being broken.
		const bool offValid = vr && vr->OffHand().valid;
		const char* modeName = ( m_settings.mode == kHandMarkerBoth ) ? "BOTH"
							 : ( m_settings.mode == kHandMarkerAuto ) ? "auto"
							 : "off hand";
		// In `auto` the weapon-hand box is expected to come and go, so report
		// the reason it is not drawn. Otherwise "1 drawn" reads as a fault.
		//
		// The RAW HANDLE is printed because the first version of this mode read
		// the wrong field and silently never showed the box. "hidden" and "the
		// signal is broken" are indistinguishable from inside the headset, and
		// 0xFFFFFFFF vs a plausible handle value tells them apart instantly.
		Log( "hands: markers %s | %d drawn | off hand %s | weapon hand %s "
			 "(m_hWeapon=0x%08X) | %u passes",
			 modeName, m_count, offValid ? "tracked" : "NOT TRACKED",
			 ShowWeaponHand() ? "SHOWN, unarmed" : "hidden, armed or unknown",
			 m_weaponHandle, m_submissions );
	}

private:
	struct Marker
	{
		Vector origin;
		QAngle angles;
		Vector forward;
		int r, g, b;
	};

	void AddHand( const Vector& base, const VRCamera& camera,
				  const ControllerPose& hand, int r, int g, int b )
	{
		if ( !hand.valid || m_count >= 2 )
			return;

		// Exactly the mapping the viewmodel uses. Sharing it is not tidiness:
		// if the marker and the gun used different ones they would drift apart
		// as the player turned, and the marker's whole job is to be believable.
		const Vector offset = camera.WorldOffsetForRoomPos( hand.position );

		Marker& m = m_markers[m_count++];
		m.origin.x = base.x + offset.x;
		m.origin.y = base.y + offset.y;
		m.origin.z = base.z + offset.z;

		// No weapon tilt applied. This is where the CONTROLLER points, which is
		// what a foregrip blend has to be reasoned about in -- the
		// controller-to-weapon tilt is a separate transform and mixing it in
		// here would hide the thing being measured.
		m.angles.x = hand.angles.x;
		m.angles.y = camera.RoomYawToWorld( hand.angles.y );
		m.angles.z = hand.angles.z;

		// Source's full basis, roll included -- the offset below is applied in
		// this frame, so getting `up` wrong would move the box sideways as the
		// wrist rolled.
		const float pitch = m.angles.x * 0.01745329252f;
		const float yaw = m.angles.y * 0.01745329252f;
		const float roll = m.angles.z * 0.01745329252f;
		const float sp = sinf( pitch ), cp = cosf( pitch );
		const float sy = sinf( yaw ), cy = cosf( yaw );
		const float sr = sinf( roll ), cr = cosf( roll );

		m.forward = Vector{ cp * cy, cp * sy, -sp };
		const Vector right = { -sr * sp * cy + cr * sy,
					   -sr * sp * sy - cr * cy,
					   -sr * cp };
		const Vector up = { cr * sp * cy + sr * sy,
				cr * sp * sy - sr * cy,
				cr * cp };

		// Shift off the tracked pose onto the controller itself.
		m.origin.x += m.forward.x * m_settings.offsetForward
				  + right.x * m_settings.offsetRight
				  + up.x * m_settings.offsetUp;
		m.origin.y += m.forward.y * m_settings.offsetForward
				  + right.y * m_settings.offsetRight
				  + up.y * m_settings.offsetUp;
		m.origin.z += m.forward.z * m_settings.offsetForward
				  + right.z * m_settings.offsetRight
				  + up.z * m_settings.offsetUp;

		m.r = r;
		m.g = g;
		m.b = b;
	}

	HandMarkerSettings m_settings;
	DebugOverlay m_overlay;

	Marker m_markers[2];
	int m_count = 0;
	// Last frame's answer from ViewModelAnim::Unarmed(). Cached so LogState can
	// explain an absent weapon-hand box rather than just omit it.
	bool m_unarmed = false;
	// Raw m_hWeapon, carried only so LogState can tell "no gun" apart from
	// "we read the wrong field".
	unsigned int m_weaponHandle = 0xFFFFFFFFu;
	unsigned int m_submissions = 0;
	Vector m_gripWorld = { 0.0f, 0.0f, 0.0f };
	bool m_haveGrip = false;
	bool m_gripEngaged = false;
};

} // namespace sinvr
