// The menu as a real quad in the world.
//
// ---- WHY ---------------------------------------------------------------------
//
// Every menu problem this project hit came from ONE fact: the menu is a 2D
// screen-space overlay. Screen-space means welded to the player's face, means no
// real depth, and means the per-eye "convergence" shift is an approximation of a
// thing that has no distance. Four rounds of fixes were all attempts to make a
// flat overlay behave like an object, and each one added a transform that had to
// be inverted somewhere else to keep the buttons clickable.
//
// This stops treating it as 2D. When the engine asks for an orthographic
// projection, it is handed
//
//     P_eye  x  A
//
// instead, where A maps HUD pixel coordinates onto a quad standing in the world.
// Every 2D draw call the menu makes then lands on that quad with real
// perspective, real per-eye parallax, and a real distance -- without the engine
// knowing anything has changed.
//
// ---- WHY VIEW SPACE, NOT WORLD SPACE -----------------------------------------
//
// The handover's sketch wanted projection x view x model, which needs the eye's
// world-to-view matrix. That matrix exists but only passes through a hook, and
// capturing the right one is its own problem -- several views go through
// LoadMatrix per pass and only the first is the world's.
//
// It is not needed. The panel is placed relative to the HEAD, and the eye's
// offset from the head is already known here, so the quad can be expressed
// directly in the eye's VIEW space and multiplied by the projection alone. One
// matrix, no captured state, nothing to go stale.
//
// Source's view space, confirmed by BuildProjection's `m[3][2] = -1`: +X right,
// +Y up, viewer looking down -Z.
//
// ---- ONE SOURCE OF TRUTH -----------------------------------------------------
//
// The renderer draws the panel from this geometry and the pointer hit-tests
// against the SAME geometry, by ray-plane intersection. That is the whole reason
// this is a shared header rather than two pieces of maths.
//
// The previous design had the renderer place the panel by one route and the
// pointer invert it by another, and they could not be made to agree -- a menu
// that looked fine and was silently unclickable, which is exactly the failure
// `hud_shift_skip_in_menus` was written to prevent. Here there is nothing to
// keep in sync: if the quad moves, both follow, because both read these fields.
#pragma once

#include <math.h>
#include "../sdk/source_interfaces.h"
#include "../vr_camera.h"

namespace sinvr {

struct MenuPanelSettings
{
	// Draw the menu on a world quad instead of as a screen overlay.
	bool enabled = true;

	// Where the quad stands, relative to the direction captured at startup.
	float distance = 150.0f;      // Source units from the head
	float yawOffset = 0.0f;       // degrees, + right / - left
	float heightOffset = 0.0f;    // Source units, + up

	// Width of the quad in Source units. The height follows the HUD's own
	// aspect, so nothing is stretched -- a menu authored for a 1800x2124
	// backbuffer keeps that shape whatever the quad's size.
	float width = 190.0f;

	// Face the quad at the player rather than leaving it a fixed board.
	//
	// A fixed board is the honest "it is a thing in the room" behaviour, and is
	// the default. Billboarding keeps it readable from an angle, which matters
	// if it ends up further to one side than intended.
	bool billboard = false;

	// Tilt the board. Yaw is above; these two turn it about its own axes so a
	// panel that is not square-on to the player can be squared up.
	//
	// These exist because the RIGHT direction is not derivable from anything the
	// mod can read. The main menu's character model and logo are placed in the
	// background map at a direction the engine never exposes -- its view yaw is
	// not the camera's staging, as pinning to it and getting the identical
	// number proved. Someone wearing the headset can see where they should be
	// in a second; no amount of reading the engine gets there.
	float pitch = 0.0f;   // degrees, + tips the top away
	float roll = 0.0f;    // degrees, + rolls clockwise as seen by the player
};

// The quad, in WORLD space. Computed once per frame and read by both the
// renderer and the pointer.
struct MenuPanelGeometry
{
	bool valid = false;

	Vector centre = { 0.0f, 0.0f, 0.0f };   // world position of the quad's middle
	Vector right = { 0.0f, 1.0f, 0.0f };    // unit, along the quad's +X
	Vector up = { 0.0f, 0.0f, 1.0f };       // unit, along the quad's +Y
	Vector normal = { 1.0f, 0.0f, 0.0f };   // unit, out of the quad toward the head

	float halfWidth = 0.0f;
	float halfHeight = 0.0f;

	// Where the head was when this was built. The pointer traces from here.
	Vector headWorld = { 0.0f, 0.0f, 0.0f };
};

inline Vector MenuCross( const Vector& a, const Vector& b )
{
	return Vector{ a.y * b.z - a.z * b.y,
				   a.z * b.x - a.x * b.z,
				   a.x * b.y - a.y * b.x };
}

inline float MenuDot( const Vector& a, const Vector& b )
{
	return a.x * b.x + a.y * b.y + a.z * b.z;
}

inline bool MenuNormalise( Vector& v )
{
	const float len = sqrtf( v.x * v.x + v.y * v.y + v.z * v.z );
	if ( len < 0.0001f )
		return false;
	v.x /= len; v.y /= len; v.z /= len;
	return true;
}

// Build the quad for this frame.
//
// `anchorYaw` is the world yaw captured at startup (and re-captured on
// recentre); `headWorld` the eye origin including the 6DoF offset; `viewYaw` the
// direction the player is currently looking, used only when billboarding.
inline MenuPanelGeometry BuildMenuPanel( const MenuPanelSettings& s,
										 float anchorYaw, float viewYaw,
										 const Vector& headWorld,
										 float hudAspect )
{
	MenuPanelGeometry g;
	g.headWorld = headWorld;

	if ( !s.enabled || s.distance < 1.0f || s.width < 1.0f )
		return g;

	const float yaw = ( s.billboard ? viewYaw : anchorYaw ) + s.yawOffset;
	const float yr = yaw / kDegPerRadian;

	// Direction from the head out to the panel. Level: a menu opened while
	// looking at the floor should not be pinned to the floor.
	const Vector out = { cosf( yr ), sinf( yr ), 0.0f };

	g.centre.x = headWorld.x + out.x * s.distance;
	g.centre.y = headWorld.y + out.y * s.distance;
	g.centre.z = headWorld.z + s.heightOffset;

	// The quad faces back along `out`, stands upright, and its right is
	// whichever way that leaves. World up is used rather than the head's, so
	// tilting your head does not roll the menu.
	g.normal = Vector{ -out.x, -out.y, -out.z };

	const Vector worldUp = { 0.0f, 0.0f, 1.0f };
	g.right = MenuCross( worldUp, g.normal );
	if ( !MenuNormalise( g.right ) )
		return g;              // looking straight up or down at it: degenerate
	g.up = MenuCross( g.normal, g.right );
	if ( !MenuNormalise( g.up ) )
		return g;

	// Pitch about the board's own right axis, then roll about its normal.
	// Applied as rotations of the basis rather than added to Euler angles --
	// the same reason ComposeWeaponAngles exists, and the same singularity
	// avoided.
	if ( s.pitch != 0.0f )
	{
		g.normal = RotateAboutAxis( g.normal, g.right, s.pitch );
		g.up = RotateAboutAxis( g.up, g.right, s.pitch );
		MenuNormalise( g.normal );
		MenuNormalise( g.up );
	}
	if ( s.roll != 0.0f )
	{
		g.right = RotateAboutAxis( g.right, g.normal, s.roll );
		g.up = RotateAboutAxis( g.up, g.normal, s.roll );
		MenuNormalise( g.right );
		MenuNormalise( g.up );
	}

	g.halfWidth = s.width * 0.5f;
	g.halfHeight = g.halfWidth * ( hudAspect > 0.01f ? hudAspect : 1.0f );
	g.valid = true;
	return g;
}

// Where a ray from the head crosses the quad, as a fraction across it.
//
// Returns false if the ray is parallel to the quad or points away from it. `u`
// runs 0..1 left to right and `v` 0..1 TOP TO BOTTOM, matching HUD coordinates
// rather than the quad's own up axis -- so the caller never has to remember
// which way round it is.
inline bool MenuPanelHit( const MenuPanelGeometry& g, const Vector& dir,
						  float& u, float& v, Vector& hitWorld )
{
	if ( !g.valid )
		return false;

	const float denom = MenuDot( dir, g.normal );
	if ( fabsf( denom ) < 0.0001f )
		return false;

	const Vector toCentre = { g.centre.x - g.headWorld.x,
							  g.centre.y - g.headWorld.y,
							  g.centre.z - g.headWorld.z };
	const float t = MenuDot( toCentre, g.normal ) / denom;
	if ( t <= 0.0f )
		return false;          // the quad is behind the pointing direction

	hitWorld.x = g.headWorld.x + dir.x * t;
	hitWorld.y = g.headWorld.y + dir.y * t;
	hitWorld.z = g.headWorld.z + dir.z * t;

	const Vector local = { hitWorld.x - g.centre.x,
						   hitWorld.y - g.centre.y,
						   hitWorld.z - g.centre.z };

	u = MenuDot( local, g.right ) / ( g.halfWidth * 2.0f ) + 0.5f;
	v = 0.5f - MenuDot( local, g.up ) / ( g.halfHeight * 2.0f );
	return true;
}

} // namespace sinvr
