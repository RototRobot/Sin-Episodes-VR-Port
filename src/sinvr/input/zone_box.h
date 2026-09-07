// Body-relative zones, as boxes you can see.
//
// Melee, the holsters and arcade reload all ask the same question -- "is the
// weapon hand at a particular place on the player's body" -- and each answered
// it with its own set of loose thresholds: a height here, a lateral distance
// there, some bounded on one side only. That was workable while the numbers were
// found by logging, but it is impossible to SHOW, and a threshold with no upper
// bound has no shape to draw.
//
// So they are all boxes now: a centre and a size, per axis. Two consequences,
// both wanted:
//
//   * a box can be drawn, which is the point -- see ZoneDebugDraw
//   * a zone is bounded on every side, where several used to be open-ended
//
// The DEFAULTS reproduce the old thresholds exactly, with generous extents on
// the axes that used to be unbounded, so converting changes nothing until a box
// is deliberately moved. That mattered: melee was calibrated on hardware at 2
// swings / 2 fires / 0 false positives and holster draws were working, and this
// is not a change anybody asked to pay for with a re-tune.
//
// ---- THE FRAME ---------------------------------------------------------------
//
// Room space, relative to the HEAD, with only the head's YAW applied -- the same
// frame melee_gesture.h and holster_zones.h already used, and for the same
// reasons. Heights are measured DOWN FROM THE HMD so a zone is the same reach
// for a tall player and a short one, and only yaw is applied because the play
// space floor is the world floor however the head is pitched: a player looking
// down must not rotate their own holsters out from under their hand.
#pragma once

#include <math.h>
#include "../sdk/source_interfaces.h"

namespace sinvr {

struct ZoneBox
{
	// Centre, relative to the head.
	//   forward  + is the way the player faces
	//   lateral  + is to the player's RIGHT
	//   up       + is above the head, so body zones are negative
	float forward = 0.0f;
	float lateral = 0.0f;
	float up = 0.0f;

	// FULL size along each axis, not half-extents -- so the config reads like a
	// physical object rather than something to halve in your head. A size of 0
	// on any axis disables the zone entirely, which is the honest way to say
	// "off" for a box.
	float sizeForward = 48.0f;
	float sizeLateral = 48.0f;
	float sizeUp = 30.0f;

	bool Valid() const
	{
		return sizeForward > 0.0f && sizeLateral > 0.0f && sizeUp > 0.0f;
	}

	bool Contains( float fwd, float lat, float upv ) const
	{
		if ( !Valid() )
			return false;
		return fabsf( fwd - forward ) * 2.0f <= sizeForward &&
			   fabsf( lat - lateral ) * 2.0f <= sizeLateral &&
			   fabsf( upv - up ) * 2.0f <= sizeUp;
	}
};

// Resolve a hand's world position into the head-relative, head-yaw frame the
// boxes are expressed in. One implementation, shared -- holster_zones.h,
// melee_gesture.h and arcade_reload.h each had their own copy of this and they
// have to agree exactly or a box means a different place to each of them.
inline void ToBodyFrame( const Vector& handPos, const Vector& headPos, float headYawDeg,
						 float& forward, float& lateral, float& up )
{
	const float yawRad = headYawDeg * 0.01745329252f;
	const float cy = cosf( yawRad );
	const float sy = sinf( yawRad );

	const float dx = handPos.x - headPos.x;
	const float dy = handPos.y - headPos.y;

	// Source's forward and right at zero pitch and roll. Note `right` points
	// along -Y at zero yaw, which is why the lateral term is a subtraction --
	// see AngleVectors in stereo.cpp, where the same sign convention is spelled
	// out at length.
	forward = dx * cy + dy * sy;
	lateral = dx * sy - dy * cy;
	up = handPos.z - headPos.z;
}

} // namespace sinvr
