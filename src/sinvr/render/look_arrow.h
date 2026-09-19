// "Look over there" -- a pulsing arrow toward what a cutscene is showing.
//
// ---- WHY THIS EXISTS ---------------------------------------------------------
//
// `cutscene_camera = head` stops a scripted camera rotating the player's view,
// which is the comfortable choice. It has one cost: if the script pans to show
// you something, your head is still pointing wherever you left it.
//
// The mod turns the player to face the scene ONCE when it begins (see
// VRCamera's cutscene alignment) and then points at it for as long as they are
// looking away. Between them the player keeps control and still knows where the
// scene is.
//
// ---- THE MISTAKE THIS WAS REBUILT TO FIX -------------------------------------
//
// The first version chose its axis by comparing Euler components:
//
//     m_horizontal = ( fabsf( yawError ) >= fabsf( pitchError ) )
//
// which is wrong in a way that only shows up on a steeply pitched target, and
// SiN's opening cutscene is exactly that. Measured there: target
// (pitch -87, yaw -42) against a head at (pitch -5, yaw 180) gives a yaw error
// of 138 and a pitch error of 82 -- so it chose HORIZONTAL and pointed sideways
// at something almost directly overhead.
//
// **At the zenith, yaw is degenerate.** No comparison of Euler components can
// know that. It is the same trap this project records for COMPOSING angles,
// applied to comparing them, and it cost a whole round of "the angle source must
// be wrong" before anyone questioned the arithmetic.
//
// So the direction now arrives as a genuine screen-space vector from
// VRCamera::LookError -- the target's forward expressed in the head's own basis
// -- and the arrow simply points along it. Correct at any pitch, including
// straight up, and it can point diagonally, which the axis-picking version could
// not.
//
// ---- WHY IT IS DRAWN THE WAY IT IS -------------------------------------------
//
// Same post-eye-pass D3D9 layer as the menu cursor: after the engine has drawn
// everything, before the eye image is captured. No material system, no vertex
// buffer, no shader.
//
// The shape is a chevron built from small squares, because `Clear` fills
// axis-aligned rectangles and nothing else. Pulsing is BRIGHTNESS rather than
// alpha for the same reason -- `Clear` writes opaque colour and cannot blend.
#pragma once

#include <d3d9.h>
#include <math.h>
#include "stereo.h"
#include "d3d9_present_hook.h"
#include "../../common/log.h"

namespace sinvr {

struct LookArrowSettings
{
	bool enabled = true;

	// ---- "CLOSE ENOUGH" IS A FRACTION OF THE VIEW, NOT A FIXED ANGLE -------
	//
	// The first version used a flat 15 degrees, which is far stricter than it
	// sounds: a VR eye renders something like 110 degrees across, so a subject
	// 30 degrees off the gaze centre is still comfortably ON SCREEN and plainly
	// visible. The arrow was demanding the player centre the scene when they
	// could already see it perfectly well.
	//
	// So the threshold is derived from the frustum actually being rendered.
	// `fovFraction` is how far out toward the edge of the view a target may sit
	// and still count as seen -- 0.7 means "anywhere in the middle 70% of the
	// way to the edge". That adapts to the headset for free, which a fixed angle
	// cannot: a wide-FOV headset should be more forgiving than a narrow one.
	//
	// The two degree values below are floors, for the case where the frustum is
	// not known yet. They are not the normal path.
	float fovFraction = 0.85f;
	float hideDegrees = 25.0f;
	float fadeDegrees = 60.0f;

	// Chevron half-size and bar thickness, in BACKBUFFER pixels -- not window
	// pixels, which are a different number on a scaled display.
	int size = 80;
	int thickness = 16;

	// How far from the centre of the image the arrow sits, as a fraction of the
	// smaller dimension. Far enough not to sit over the subject, near enough to
	// be in the comfortable part of the lens.
	float offset = 0.22f;

	// Pulses per second. Slow: a hint, not an alarm.
	float pulseHz = 1.4f;

	int r = 255;
	int g = 210;
	int b = 60;
};

class LookArrow
{
public:
	void SetSettings( const LookArrowSettings& s ) { m_settings = s; }
	const LookArrowSettings& Settings() const { return m_settings; }

	// Once per frame. `right` / `up` are a UNIT screen direction toward the
	// target -- +right is to the player's right, +up is up the screen -- and
	// `degrees` is the true angle between where they are looking and where the
	// scene is. All three come from VRCamera::LookError.
	void Update( bool active, float right, float up, float degrees )
	{
		m_armed = false;
		m_degrees = degrees;
		if ( !m_settings.enabled || !active )
			return;

		// Thresholds from the rendered frustum where it is known, falling back
		// to the configured floors otherwise.
		float hide = m_settings.hideDegrees;
		float fade = m_settings.fadeDegrees;
		{
			float tanX = 0.0f, tanY = 0.0f;
			if ( Stereo().RenderedHalfTangents( tanX, tanY ) && tanY > 0.01f )
			{
				// The VERTICAL half-angle, because it is the smaller of the two
				// and therefore the one that actually limits what can be seen.
				const float halfFov = atanf( tanY ) * 57.2957795130823f;
				const float derived = m_settings.fovFraction * halfFov;
				if ( derived > hide )
					hide = derived;
				// Full strength once the target is past the edge of the view --
				// at that point it genuinely cannot be seen at all.
				const float derivedFade = halfFov * 1.15f;
				if ( derivedFade > fade )
					fade = derivedFade;
			}
		}
		m_hideUsed = hide;

		if ( degrees <= hide )
			return;                          // close enough; say nothing

		const float span = ( fade > hide ) ? ( fade - hide ) : 1.0f;
		m_strength = ( degrees - hide ) / span;
		if ( m_strength > 1.0f ) m_strength = 1.0f;
		if ( m_strength < 0.0f ) m_strength = 0.0f;

		const float len = sqrtf( right * right + up * up );
		if ( len < 0.0001f )
			return;                          // dead ahead or dead behind

		m_dirX = right / len;
		m_dirY = up / len;
		m_armed = true;
	}

	void Disarm() { m_armed = false; }

	// Once per EYE PASS. A single draw would reach one eye only.
	void Draw( int eye )
	{
		if ( !m_armed )
			return;

		IDirect3DDevice9* device = D3D9Device();
		if ( !device )
			return;

		unsigned int bbW = 0, bbH = 0;
		Stereo().BackbufferSize( bbW, bbH );
		if ( bbW == 0 || bbH == 0 )
			return;

		// Performance counter, not engine time: engine time is deliberately
		// frozen across the eye pair, so a pulse driven from it would step once
		// per frame PAIR rather than run smoothly.
		const float t = NowSeconds();
		const float pulse = 0.55f + 0.45f * sinf( t * m_settings.pulseHz * 6.2831853f );
		const float bright = m_strength * pulse;

		const int r = (int)( m_settings.r * bright );
		const int g = (int)( m_settings.g * bright );
		const int b = (int)( m_settings.b * bright );

		const int size = (int)( m_settings.size * ( 0.55f + 0.45f * m_strength ) );
		const int thick = m_settings.thickness > 0 ? m_settings.thickness : 1;
		if ( size < thick )
			return;

		// Screen y grows DOWNWARD, so the up component is negated once, here.
		const float radius = m_settings.offset *
							 (float)( bbW < bbH ? bbW : bbH );
		const int cx = (int)bbW / 2 + (int)( m_dirX * radius );
		const int cy = (int)bbH / 2 - (int)( m_dirY * radius );

		D3DRECT rects[kMaxSteps];
		const int count = BuildChevron( rects, cx, cy, size, thick, bbW, bbH );
		if ( count <= 0 )
			return;

		IDirect3DSurface9* backBuffer = nullptr;
		if ( FAILED( device->GetBackBuffer( 0, 0, D3DBACKBUFFER_TYPE_MONO, &backBuffer ) )
			 || !backBuffer )
			return;

		IDirect3DSurface9* savedTarget = nullptr;
		device->GetRenderTarget( 0, &savedTarget );

		D3DVIEWPORT9 savedViewport = {};
		const bool haveViewport = SUCCEEDED( device->GetViewport( &savedViewport ) );

		DWORD savedScissor = FALSE;
		device->GetRenderState( D3DRS_SCISSORTESTENABLE, &savedScissor );

		const bool needRebind = ( savedTarget != backBuffer );
		bool bound = !needRebind;
		if ( needRebind )
			bound = SUCCEEDED( device->SetRenderTarget( 0, backBuffer ) );

		if ( bound )
		{
			D3DVIEWPORT9 full = { 0, 0, bbW, bbH, 0.0f, 1.0f };
			device->SetViewport( &full );
			device->SetRenderState( D3DRS_SCISSORTESTENABLE, FALSE );

			device->Clear( (DWORD)count, rects, D3DCLEAR_TARGET,
						   D3DCOLOR_XRGB( r, g, b ), 1.0f, 0 );
			++m_draws;
		}

		if ( savedTarget )
		{
			if ( needRebind && bound )
				device->SetRenderTarget( 0, savedTarget );
			savedTarget->Release();
		}
		if ( haveViewport )
			device->SetViewport( &savedViewport );
		device->SetRenderState( D3DRS_SCISSORTESTENABLE, savedScissor );

		backBuffer->Release();
	}

	void LogState() const
	{
		if ( !m_settings.enabled )
			return;
		Log( "look arrow: %s | %.0f deg off (hidden below %.0f, from the rendered "
			 "FOV) | screen dir (%+.2f %+.2f) | %u draw(s)",
			 m_armed ? "pointing" : "idle",
			 m_degrees, m_hideUsed, m_dirX, m_dirY, m_draws );
	}

private:
	enum { kMaxSteps = 64 };

	static float NowSeconds()
	{
		LARGE_INTEGER f, c;
		if ( !QueryPerformanceFrequency( &f ) || f.QuadPart == 0 )
			return 0.0f;
		QueryPerformanceCounter( &c );
		return (float)( (double)c.QuadPart / (double)f.QuadPart );
	}

	// A chevron pointing along (m_dirX, -m_dirY) in screen space, as a staircase
	// of squares. Being built from the direction VECTOR rather than from one of
	// four cases is what lets it point diagonally.
	int BuildChevron( D3DRECT* out, int cx, int cy, int size, int thick,
					  unsigned int w, unsigned int h ) const
	{
		const int half = thick / 2 > 0 ? thick / 2 : 1;
		const int steps = size / half;
		const int n = ( steps > kMaxSteps / 2 ) ? kMaxSteps / 2 : steps;
		if ( n <= 0 )
			return 0;

		// Screen-space pointing direction and its perpendicular.
		const float px = m_dirX;
		const float py = -m_dirY;          // screen y grows downward
		const float qx = -py;              // perpendicular
		const float qy = px;

		int count = 0;
		for ( int i = 0; i <= n; ++i )
		{
			const float d = (float)( size * i ) / (float)n;

			for ( int sgn = -1; sgn <= 1; sgn += 2 )
			{
				if ( i == 0 && sgn > 0 )
					continue;               // the tip is one square, not two

				// Tip toward the target, limbs trailing back along the
				// perpendicular -- a ">" rotated to any angle.
				const float fx = (float)cx - px * d + qx * (float)sgn * d;
				const float fy = (float)cy - py * d + qy * (float)sgn * d;

				if ( count >= kMaxSteps )
					break;

				D3DRECT& rc = out[count];
				rc.x1 = (LONG)fx - half;  rc.x2 = (LONG)fx + half;
				rc.y1 = (LONG)fy - half;  rc.y2 = (LONG)fy + half;
				Clamp( rc, w, h );
				if ( rc.x2 > rc.x1 && rc.y2 > rc.y1 )
					++count;
			}
		}
		return count;
	}

	static void Clamp( D3DRECT& r, unsigned int w, unsigned int h )
	{
		if ( r.x1 < 0 ) r.x1 = 0;
		if ( r.y1 < 0 ) r.y1 = 0;
		if ( r.x2 > (LONG)w ) r.x2 = (LONG)w;
		if ( r.y2 > (LONG)h ) r.y2 = (LONG)h;
	}

	LookArrowSettings m_settings;

	bool m_armed = false;
	float m_dirX = 0.0f;
	float m_dirY = 0.0f;
	float m_strength = 0.0f;
	float m_degrees = 0.0f;
	float m_hideUsed = 0.0f;
	unsigned int m_draws = 0;
};

} // namespace sinvr
