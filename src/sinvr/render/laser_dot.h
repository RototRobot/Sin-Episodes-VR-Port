// A crosshair in the world, where the bullet is going.
//
// ---- IT DOES NOT COME FROM THE CONTROLLER ------------------------------------
//
// The obvious mental model -- cast a ray from the gun in your hand -- draws a dot
// where bullets do NOT go. Source fires from the player's EYE:
//
//     CBaseCombatWeapon   info.m_vecSrc = pPlayer->Weapon_ShootPosition()
//     CBasePlayer         Weapon_ShootPosition() -> EyePosition()
//     direction           AngleVectors( EyeAngles() + m_vecPunchAngle + autoaim )
//
// That is the same fact `aim_convergence_distance` exists to paper over: aiming
// down the barrel gives a ray PARALLEL to it, displaced sideways by the
// eye-to-gun distance, so the shot lands consistently off to one side. The
// convergence rotates the eye ray so the two lines MEET at one chosen distance.
//
// So the crosshair is drawn on the eye ray, which is the bullet's ray. Drawing it
// on the barrel's ray would look more convincing and be wrong.
//
// ---- WHY IT IS A WORLD-SPACE MARKER AND NOT A MOVED CROSSHAIR ----------------
//
// From the eye, every point along the ray projects to the SAME screen position,
// so tracing buys nothing in 2D -- a repositioned crosshair would sit at exactly
// the same pixel whether the wall is 2 m away or 200. What it buys is DEPTH.
//
// A 2D element drawn at one screen position is at infinity in stereo, however
// near it is meant to look. That is not a theory: the hand-mounted HUD was built,
// played and shelved for precisely this, and the body anchor works because it
// projects a WORLD vector per eye. A crosshair has the same requirement and more
// urgently, because it sits exactly where the player's eyes are converged.
//
// `CHudCrosshair` can in fact draw off-centre -- it projects
// `m_curViewAngles + m_vecCrossHairOffsetAngle` when that offset is non-zero, and
// SetCrosshairAngle is its setter -- so reusing the game's own crosshair TEXTURE
// looked attractive. It is still a 2D element at a screen position, so it would
// sit at infinity, and Valve's own comment on that block reads "this code is
// wrong". Getting the texture onto a world-space quad instead means the material
// system and a mesh, which is the raw-D3D9 dead end wearing a different hat.
//
// So the SHAPE is reproduced rather than the texture: four arms with a gap in the
// middle, which is what the graphic is anyway, drawn as world-space lines through
// the interface already proven by the zone boxes.
//
// ---- AND AUTOAIM ------------------------------------------------------------
//
// `ShouldAutoaim()` is true in singleplayer below hard skill, and
// `GetAutoaimVector( AUTOAIM_5DEGREES )` bends the shot by up to 5 degrees toward
// a target. It is server-side and depends on entity positions, so the crosshair
// cannot show it. It shows where you are AIMING; below `skill 3` the bullet may
// bend off it toward something nearby. That is the game being helpful, not the
// crosshair being wrong.
#pragma once

#include <windows.h>
#include <math.h>
#include "../sdk/source_interfaces.h"
#include "../sdk/debug_overlay.h"
#include "../sdk/engine_trace.h"
#include "../../common/log.h"

namespace sinvr {

// ---- THE GAME'S OWN RETICLES, TAKEN FROM THE GAME'S OWN FONT ---------------
//
// SiN does not draw its crosshairs from a texture. Each weapon script names a
// CHARACTER in a font -- `SE1/resource/SE1Crosshairs.ttf` -- and the HUD prints
// that glyph:
//
//     weapon_magnum.txt         crosshair_normal -> "A"
//     weapon_scattergun.txt                      -> "B"
//     weapon_assault_rifle.txt                   -> "C"
//
// So the shapes below are not eyeballed from a screenshot. The TTF's `glyf`
// outlines for A, B and C were read directly and reduced to their elements:
// every contour in those three glyphs is an axis-aligned rounded rectangle,
// which is to say a DASH or a DOT. Normalised into a common frame (the glyph
// centre at the origin, the outer dashes at +/-1), that is the whole reticle.
//
//   A  a single dot
//   B  four dashes, top / bottom / left / right
//   C  the same four, plus four inner ticks and a centre dot
//
// > The wings either side of the crosshair in-game are NOT part of it. They are
// > `crosshair_quickinfo_left/right_full` -- separate glyphs showing clip state.
// > Reproducing them faithfully would paint an ammo gauge onto a wall, at
// > whatever range the player happens to be aiming, which is worse than not
// > having one.
//
// What stands in for them is a pair of BRACKETS, one shape per weapon, chosen
// to echo the silhouette without claiming to mean anything:
//
//     magnum          tortoise-shell   U+3018 / U+3019
//     scattergun      lenticular       U+3010 / U+3011
//     assault rifle   double angle     U+300A / U+300B
//
// They are decoration and are honest about it. The alternative -- an ammo
// readout welded to the impact point -- would be information in the wrong place.
//
// ---- ONE DELIBERATE DEPARTURE FROM THE FONT --------------------------------
//
// The centre dot is drawn at 0.150 where the glyph's own is 0.064. That is not
// taste, it is the medium: the font's dot is a FILLED 27-unit square and reads
// solid at HUD scale, while ours is hairlines. At the true size it all but
// vanishes -- and on the magnum that dot is the ENTIRE reticle. It is drawn as
// a box plus a cross for the same reason, so the outline reads as a mark rather
// than as four thin strokes.
// A plain segment list, in normalised glyph space with the reticle centre at
// the origin and the glyph's outer dashes at +/-1. Segments rather than
// axis-aligned dashes because the side markers below need diagonals, and one
// primitive that covers everything beats three that nearly do.
struct ReticleSeg
{
	float x0, y0, x1, y1;
};

struct Reticle
{
	const ReticleSeg* segs;
	int count;
	int r, g, b;
};

struct LaserDotSettings
{
	// Off by default: it draws something in the world that was not there before.
	bool enabled = false;

	// Distance along the bullet ray when nothing is hit, in units. 0 means "use
	// the aim convergence distance", which is where the shot and the barrel
	// actually coincide -- so the crosshair doubles as a way to see whether that
	// convergence is set correctly.
	float distance = 0.0f;

	// Arm length in units AT 250 UNITS OF RANGE, scaled with the real distance so
	// the crosshair keeps a constant APPARENT size. Without that a mark on a far
	// wall shrinks to nothing and one on a near wall covers what you are aiming
	// at.
	float size = 1.2f;

	// Fraction of the arm left empty in the middle, so the target stays visible
	// through the crosshair. 0 draws a solid plus.
	float gap = 0.35f;

	// Land on the first thing the ray hits instead of floating at `distance`.
	bool trace = true;

	// How far to look when tracing.
	float maxDistance = 4096.0f;

	// Draw through walls. Off is correct -- a mark on a wall should be hidden by
	// anything in front of it -- and on is the escape hatch if it turns out to
	// z-fight with the surface it is sitting on.
	bool throughWalls = false;

	// How many times the crosshair is submitted per frame.
	//
	// This is the ghosting experiment, and a setting rather than a constant
	// so the next value can be tried without a rebuild.
	//
	// The mod renders the scene TWICE and used to submit overlays ONCE, with
	// duration 0. If the engine's overlay list is consumed or cleared by the
	// first eye pass, the second eye never receives it -- and a mark present
	// in ONE EYE ONLY is exactly what ghosting looks like. One submission per
	// pass would then fix it outright.
	//
	// If instead both submissions are drawn in both passes, the lines simply
	// overdraw in the same place and nothing changes -- so the experiment
	// cannot make anything worse, and 1 restores the original behaviour.
	int submits = 2;
	
	// Colour each submission differently: 1st as configured, then green,
	// blue, yellow. This is what makes a FAILED experiment still worth a run,
	// because it separates outcomes that look identical otherwise:
	//
	//   each eye a DIFFERENT colour  -> the list is consumed per pass, and
	//                                   one submit per pass is the fix
	//   both eyes show BOTH colours  -> both draw in both passes, so the
	//                                   ghosting is something else
	//   only the FIRST colour at all -> only one pass draws overlays, and the
	//                                   overlay path cannot do stereo
	bool debugTint = false;
	
	int r = 255, g = 32, b = 32;

	// Draw the current weapon's own reticle instead of the plain cross, in the
	// colour the game uses for it. 0 restores the four-arm cross and the r/g/b
	// above, which is also what any weapon with no entry of its own gets.
	bool perWeapon = true;
};

class LaserDot
{
public:
	void SetSettings( const LaserDotSettings& s ) { m_settings = s; }
	const LaserDotSettings& Settings() const { return m_settings; }

	bool Bind( void* debugOverlayIface ) { return m_overlay.Bind( debugOverlayIface ); }
	bool BindTrace( void* engineTraceIface ) { return m_trace.Bind( engineTraceIface ); }
	bool Ready() const { return m_settings.enabled && m_overlay.Valid(); }

	// Drop every overlay the engine still holds, including last frame's. Called
	// once at the top of each eye pass, BEFORE anything submits.
	void ClearOverlays() { m_overlay.Clear(); }

	// Once per frame from the View_Render hook, BEFORE the eye passes, so the
	// marker is part of the world both passes render and gets each eye's
	// projection -- which is what makes it converge at its real distance.
	//
	// `eyeOrigin` must be the ENGINE's view origin -- the base, without the 6DoF
	// offset -- because that is where the engine fires from. Using the VR eye
	// would put it on a ray the bullet does not travel.
	void Draw( const Vector& eyeOrigin, const QAngle& aimAngles, float convergence,
			   void* localPlayer )
	{
		m_drawn = false;
		m_hitSomething = false;
		if ( !Ready() )
			return;

		float dist = m_settings.distance;
		if ( dist <= 0.0f )
			dist = ( convergence > 0.0f ) ? convergence : 250.0f;

		// Source's forward, matching AngleVectors exactly -- pitch is positive
		// DOWNWARDS, which is the sign convention that catches everyone.
		const float pitch = aimAngles.x * 0.01745329252f;
		const float yaw = aimAngles.y * 0.01745329252f;
		const float sp = sinf( pitch ), cp = cosf( pitch );
		const float sy = sinf( yaw ), cy = cosf( yaw );
		const Vector fwd = { cp * cy, cp * sy, -sp };

		// Trace first, fall back to the fixed distance. The fallback is not
		// decoration: if the interface is unavailable, or its structs do not match
		// this build, the crosshair still shows the aim -- it just stops sticking.
		if ( m_settings.trace && m_trace.Valid() )
		{
			const float reach = m_settings.maxDistance;
			const Vector far_ = { eyeOrigin.x + fwd.x * reach,
								  eyeOrigin.y + fwd.y * reach,
								  eyeOrigin.z + fwd.z * reach };
			Vector hit = { 0.0f, 0.0f, 0.0f };
			float fraction = 1.0f;
			if ( m_trace.Ray( eyeOrigin, far_, localPlayer, hit, fraction ) )
			{
				// A hit at essentially zero range is the ray striking the player
				// it started inside, which means the skip filter did not
				// recognise them -- see engine_trace.h. Counted and ignored
				// rather than drawn on the player's own face.
				if ( fraction * reach < 8.0f )
				{
					++m_selfHits;
				}
				else
				{
					m_point = hit;
					m_hitSomething = true;
					dist = fraction * reach;
				}
			}
		}

		if ( !m_hitSomething )
		{
			m_point.x = eyeOrigin.x + fwd.x * dist;
			m_point.y = eyeOrigin.y + fwd.y * dist;
			m_point.z = eyeOrigin.z + fwd.z * dist;
		}

		// The GEOMETRY is not submitted here. Compute once, submit inside each
		// eye pass -- see Submit() -- because a single submission reaches only
		// the first pass.
		m_fwd = fwd;
		m_drawn = true;
		m_lastDistance = dist;
		++m_frames;
	}

	// Called once per EYE PASS, from StereoRenderer's pre-pass hook.
	//
	// Submitting once per frame put the crosshair in one eye only: the engine
	// draws its overlay list during the first pass and clears it, so the
	// second pass finds nothing. A monocular mark looks correct while the
	// head is still and swims the moment it moves, which is exactly how the
	// ghosting presented -- and why submitting twice BEFORE both passes did
	// not help: both copies landed in the same pass and were cleared together.
	void Submit( int eye )
	{
		if ( !m_drawn || !Ready() )
			return;

		int n = m_settings.submits;
		if ( n < 1 ) n = 1;
		if ( n > 4 ) n = 4;
		for ( int i = 0; i < n; ++i )
			DrawCrosshair( m_fwd, m_lastDistance, i, eye );
		++m_submissions;
	}

	void LogState() const
	{
		if ( !m_settings.enabled )
			return;

		if ( !m_overlay.Valid() )
		{
			LogWarn( "laser: enabled but VDebugOverlay003 is not bound -- nothing "
					 "can be drawn" );
			return;
		}

		// Effect, not intent. FLOATING is the one that matters: it means the trace
		// is not landing and the crosshair has quietly gone back to being a
		// fixed-distance marker, which looks almost right and is not.
		Log( "laser: crosshair %s at %.0f units, %s | point (%.1f %.1f %.1f) | "
			 "%u traces over %u frames | %d submit(s)/pass, %u passes%s%s",
			 m_drawn ? "ON" : "ON but not drawn this frame",
			 m_lastDistance,
			 m_hitSomething ? "ON GEOMETRY"
							: ( m_settings.trace ? "FLOATING (trace found nothing)"
												 : "floating (tracing off)" ),
			 m_point.x, m_point.y, m_point.z,
			 m_trace.Traces(), m_frames, m_settings.submits,
			 m_submissions,
			 m_settings.debugTint ? " TINTED" : "",
			 m_selfHits ? "  <- some traces hit the player: the skip filter is not "
						  "matching, see engine_trace.h" : "" );
	}

	// ---- THE THREE GLYPHS, AS LINE PRIMITIVES ---------------------------
	//
	// Generated from SE1Crosshairs.ttf rather than typed by hand. Colours are
	// the ones the HUD shows for each weapon.
	static const Reticle* ReticleFor( const char* model )
	{
		// glyph 'A' -- 6 segment(s) -- plus tortoise-shell side markers
		static const ReticleSeg kMagnum[] = {
			{ -0.143f, -0.009f, +0.157f, -0.009f },
			{ +0.007f, -0.159f, +0.007f, +0.141f },
			{ -0.143f, -0.159f, +0.157f, -0.159f },
			{ +0.157f, -0.159f, +0.157f, +0.141f },
			{ +0.157f, +0.141f, -0.143f, +0.141f },
			{ -0.143f, +0.141f, -0.143f, -0.159f },
			{ -1.780f, -0.780f, -1.780f, +0.780f },
			{ -1.650f, -0.624f, -1.650f, +0.624f },
			{ -1.380f, +0.780f, -1.780f, +0.780f },
			{ -1.380f, -0.780f, -1.780f, -0.780f },
			{ +1.780f, -0.780f, +1.780f, +0.780f },
			{ +1.650f, -0.624f, +1.650f, +0.624f },
			{ +1.380f, +0.780f, +1.780f, +0.780f },
			{ +1.380f, -0.780f, +1.780f, -0.780f },
		};
		// 14 segments total

		// glyph 'B' -- 4 segment(s) -- plus lenticular side markers
		static const ReticleSeg kScattergun[] = {
			{ -0.942f, -0.268f, -0.942f, +0.255f },
			{ +0.920f, -0.268f, +0.920f, +0.255f },
			{ -0.259f, +0.932f, +0.268f, +0.932f },
			{ -0.259f, -0.934f, +0.268f, -0.934f },
			{ -1.780f, -0.780f, -1.780f, +0.780f },
			{ -1.690f, -0.780f, -1.690f, +0.780f },
			{ -1.380f, +0.780f, -1.780f, +0.780f },
			{ -1.380f, -0.780f, -1.780f, -0.780f },
			{ +1.780f, -0.780f, +1.780f, +0.780f },
			{ +1.690f, -0.780f, +1.690f, +0.780f },
			{ +1.380f, +0.780f, +1.780f, +0.780f },
			{ +1.380f, -0.780f, +1.780f, -0.780f },
		};
		// 12 segments total

		// glyph 'C' -- 14 segment(s) -- plus double-angle side markers
		static const ReticleSeg kRifle[] = {
			{ +0.918f, -0.268f, +0.918f, +0.277f },
			{ -0.944f, -0.268f, -0.944f, +0.255f },
			{ +0.002f, +0.432f, +0.002f, +0.786f },
			{ +0.002f, -0.786f, +0.002f, -0.432f },
			{ -0.259f, +0.918f, +0.268f, +0.918f },
			{ -0.259f, -0.925f, +0.268f, -0.925f },
			{ -0.143f, -0.009f, +0.157f, -0.009f },
			{ +0.007f, -0.159f, +0.007f, +0.141f },
			{ -0.143f, -0.159f, +0.157f, -0.159f },
			{ +0.157f, -0.159f, +0.157f, +0.141f },
			{ +0.157f, +0.141f, -0.143f, +0.141f },
			{ -0.143f, +0.141f, -0.143f, -0.159f },
			{ -0.818f, -0.011f, -0.441f, -0.011f },
			{ +0.436f, -0.007f, +0.768f, -0.007f },
			{ -1.380f, +0.780f, -1.780f, +0.000f },
			{ -1.780f, +0.000f, -1.380f, -0.780f },
			{ -1.080f, +0.671f, -1.480f, +0.000f },
			{ -1.480f, +0.000f, -1.080f, -0.671f },
			{ +1.380f, +0.780f, +1.780f, +0.000f },
			{ +1.780f, +0.000f, +1.380f, -0.780f },
			{ +1.080f, +0.671f, +1.480f, +0.000f },
			{ +1.480f, +0.000f, +1.080f, -0.671f },
		};
		// 22 segments total

		static const Reticle kMagnumR     = { kMagnum,     14, 150, 235, 235 };
		static const Reticle kScattergunR = { kScattergun, 12, 130, 245, 130 };
		static const Reticle kRifleR      = { kRifle,      22, 240, 205,  70 };

		if ( !model || !model[0] )
			return nullptr;
		if ( _stricmp( model, "v_magnum" ) == 0 )        return &kMagnumR;
		if ( _stricmp( model, "v_scattergun" ) == 0 )    return &kScattergunR;
		if ( _stricmp( model, "v_assault_rifle" ) == 0 ) return &kRifleR;
		return nullptr;   // anything else keeps the plain cross
	}

public:
	// Which weapon is in hand. Pushed in per frame rather than looked up here,
	// for the same reason everything else in this project is pushed: a renderer
	// that reaches for the viewmodel can disagree with the viewmodel.
	void SetWeapon( const char* modelKey )
	{
		m_reticle = m_settings.perWeapon ? ReticleFor( modelKey ) : nullptr;
	}

private:
	// Four arms with a gap in the middle, facing the eye.
	//
	// BILLBOARDED, not axis-aligned. A cross drawn on world axes collapses to a
	// line when you look along one of them -- which is exactly when you are aiming
	// at something. Building the arms from vectors perpendicular to the view ray
	// keeps it square-on from anywhere.
	void DrawCrosshair( const Vector& fwd, float dist, int submission, int eye )
	{
		// A right vector perpendicular to the ray, using world up as the
		// reference. When the ray IS vertical that degenerates, and then any
		// perpendicular will do.
		Vector right = { fwd.y, -fwd.x, 0.0f };
		float len = sqrtf( right.x * right.x + right.y * right.y );
		if ( len < 0.001f )
		{
			right = Vector{ 1.0f, 0.0f, 0.0f };
			len = 1.0f;
		}
		right.x /= len;
		right.y /= len;

		// up = right x forward, unit already since both are.
		const Vector up = { right.y * fwd.z - right.z * fwd.y,
							right.z * fwd.x - right.x * fwd.z,
							right.x * fwd.y - right.y * fwd.x };

		// Constant apparent size: the arm grows with range.
		const float arm = m_settings.size * ( dist / 250.0f );
		float inner = arm * m_settings.gap;
		if ( inner < 0.0f ) inner = 0.0f;
		if ( inner > arm ) inner = arm;

		// Every submission is the same colour unless we are diagnosing, so an
		// overdraw is invisible rather than a rainbow.
		//
		// The tint is now keyed on the EYE rather than the submission index,
		// and shifts the copy sideways as well as recolouring it. The first
		// version did neither: identical geometry in the same place means
		// whichever is drawn last simply covers the other, so "all one
		// colour" could not be told apart from "only one was drawn". A
		// diagnostic that cannot produce the positive it is looking for is
		// worse than none.
		int cr = m_settings.r, cg = m_settings.g, cb = m_settings.b;
		float sideways = 0.0f;
		if ( m_settings.debugTint )
		{
			static const int tint[2][3] = { { 255, 32, 32 },     // eye 0 red
			                                { 32, 255, 32 } };   // eye 1 green
			const int e = ( eye == 1 ) ? 1 : 0;
			cr = tint[e][0]; cg = tint[e][1]; cb = tint[e][2];
			// Offset so both copies are visible at once if both reach an eye.
			sideways = ( e == 0 ) ? -1.0f : 1.0f;
		}

		// Pulled slightly toward the eye so it does not z-fight with the surface
		// it is sitting on, which is the normal case once it is traced onto a
		// wall.
		const float lift = 0.5f;
		const float sx = sideways * arm;
		const Vector c = { m_point.x - fwd.x * lift + right.x * sx,
						   m_point.y - fwd.y * lift + right.y * sx,
						   m_point.z - fwd.z * lift + right.z * sx };

		// ---- THE WEAPON'S OWN RETICLE ---------------------------------
		//
		// The glyph is defined in a flat normalised frame, so drawing it is
		// just mapping (x,y) onto the billboard's right/up. `arm` is the same
		// distance-scaled size the plain cross uses, so both keep a constant
		// APPARENT size and the setting means the same thing either way.
		if ( m_reticle && !m_settings.debugTint )
		{
			// Mapping a flat segment list onto the billboard is the whole of
			// it: (x,y) becomes right*x + up*y. `arm` is the same
			// distance-scaled size the plain cross uses, so the setting means
			// the same thing whichever shape is drawn and both keep a constant
			// APPARENT size.
			for ( int i = 0; i < m_reticle->count; ++i )
			{
				const ReticleSeg& sg = m_reticle->segs[i];
				const float ax = sg.x0 * arm, ay = sg.y0 * arm;
				const float bx = sg.x1 * arm, by = sg.y1 * arm;

				const Vector from = { c.x + right.x * ax + up.x * ay,
									  c.y + right.y * ax + up.y * ay,
									  c.z + right.z * ax + up.z * ay };
				const Vector to   = { c.x + right.x * bx + up.x * by,
									  c.y + right.y * bx + up.y * by,
									  c.z + right.z * bx + up.z * by };
				m_overlay.Line( from, to, m_reticle->r, m_reticle->g,
								m_reticle->b, m_settings.throughWalls, 0.0f );
			}
			return;
		}

		const Vector axes[2] = { right, up };
		for ( int a = 0; a < 2; ++a )
		{
			for ( int sign = -1; sign <= 1; sign += 2 )
			{
				const float i = (float)sign * inner;
				const float o = (float)sign * arm;
				const Vector from = { c.x + axes[a].x * i,
									  c.y + axes[a].y * i,
									  c.z + axes[a].z * i };
				const Vector to = { c.x + axes[a].x * o,
									c.y + axes[a].y * o,
									c.z + axes[a].z * o };
				m_overlay.Line( from, to, cr, cg, cb,
								m_settings.throughWalls, 0.0f );
			}
		}
	}

	LaserDotSettings m_settings;
	DebugOverlay m_overlay;
	const Reticle* m_reticle = nullptr;
	EngineTrace m_trace;

	Vector m_point = { 0.0f, 0.0f, 0.0f };
	bool m_drawn = false;
	bool m_hitSomething = false;
	float m_lastDistance = 0.0f;
	unsigned int m_frames = 0;
	unsigned int m_selfHits = 0;
	unsigned int m_submissions = 0;
	Vector m_fwd = { 1.0f, 0.0f, 0.0f };
};

} // namespace sinvr
