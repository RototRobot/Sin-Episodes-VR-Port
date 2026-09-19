// The menu cursor, drawn by us in D3D9, on top of the finished eye image.
//
// ---- WHY THIS EXISTS AT ALL --------------------------------------------------
//
// Three cursors were tried before this one and each died to the same fact about
// the ENGINE's draw order rather than to anything wrong with the cursor:
//
//   the OS cursor        Windows composites it onto the DESKTOP, never into the
//                        D3D backbuffer we submit. Invisible in VR wherever it
//                        is put -- not a bug, a fact about the compositor.
//   IVDebugOverlay       drawn by the engine BEFORE VGUI, so it is always behind
//                        the menu -- and while an in-game PAUSE menu is up the
//                        engine does not draw the overlay list AT ALL. Proved by
//                        pointing a second, unrelated consumer of the same
//                        interface (the zone boxes) at the same state and
//                        watching both vanish together.
//   AddScreenTextOverlay same overlay system, same state, same death.
//
// So the cursor cannot be handed to the engine in any form. It has to be drawn
// after the engine has finished, which is exactly what this does: the stereo
// loop calls Draw() at the END of each eye pass, once the engine has drawn the
// world, the HUD and VGUI, and BEFORE the backbuffer is copied into that eye's
// surface. Whatever is put there is on top of everything and is submitted with
// the frame.
//
// ---- WHY THE POSITION IS PROJECTED, NOT MAPPED -------------------------------
//
// The obvious implementation takes the pointer's -1..1 position across the panel,
// scales it to backbuffer pixels, and nudges each eye sideways by "the same
// parallax". That is a hand-matched transform, and hand-matched transforms are
// what killed four earlier rounds of menu work -- the picture and the position
// came through different maths and could not be made to agree.
//
// Instead the cursor's WORLD position on the quad is projected through the SAME
// matrices the panel is drawn with (StereoRenderer::ProjectWorldToBackbuffer).
// A point on the panel therefore lands where the panel drew it, in both eyes,
// with the right parallax, by construction. There is nothing to tune and nothing
// that can drift.
//
// ---- WHY IT CANNOT TOUCH GAMEPLAY -------------------------------------------
//
// This is deliberately the most inert thing in the project:
//
//   * Draw() returns immediately unless ARMED, and arming happens only inside
//     the `uiVisible` branch. Leaving a menu disarms it in the same place the
//     pointer is released.
//   * It touches no engine interface. No matrices, no view, no CViewSetup, no
//     entity. The 6DoF path (VRCamera::UpdatePositionalOffset) is nowhere near
//     it and cannot be reached from here.
//   * Every device state it changes -- render target, viewport, scissor -- is
//     saved and restored around the draw, so the engine's next pass sees a
//     device it left. `Clear` cannot corrupt geometry or shader state the way a
//     mesh draw can; it is one call with no pipeline setup.
//   * A failure at any step returns without drawing. There is no path where a
//     bad cursor costs a frame.
#pragma once

#include <d3d9.h>
#include "stereo.h"
#include "d3d9_present_hook.h"
#include "../../common/log.h"

namespace sinvr {

struct MenuCursorSettings
{
	// Master switch. Off falls back to the world-space overlay cross, which
	// works on the main menu and not in a pause menu -- i.e. the old behaviour.
	bool enabled = true;

	// Arm half-length and bar thickness, in BACKBUFFER pixels.
	//
	// Pixels rather than world units because this is drawn in screen space, and
	// because the backbuffer is 1800x2124 here while the window client is
	// 1200x1416 -- a size in window pixels would be 1.5x wrong. Same trap
	// MenuHudAspect exists for.
	int size = 16;
	int thickness = 4;

	// Dark border drawn around the bright cross, in pixels. 0 disables it.
	//
	// Not decoration: a single-colour cursor disappears against a menu of the
	// same colour, and SiN's menus are light on dark in some screens and dark on
	// light in others. An outlined cross is legible on both.
	int outline = 2;

	int r = 255;
	int g = 240;
	int b = 64;
};

class MenuCursor
{
public:
	void SetSettings( const MenuCursorSettings& s ) { m_settings = s; }
	const MenuCursorSettings& Settings() const { return m_settings; }

	// Called once per frame while a menu is up, with the cursor's world position
	// on the panel -- the same Vector the overlay marker used, so the two cannot
	// disagree about where the cursor is.
	void Arm( const Vector& world )
	{
		m_world = world;
		m_armed = true;
	}

	// Called wherever the menu goes away. Deliberately separate from Arm so the
	// caller cannot forget: the pointer's Release() and this sit together.
	void Disarm() { m_armed = false; }

	bool Armed() const { return m_armed; }

	// ---- ONCE PER EYE PASS, AT THE END -------------------------------------
	//
	// Per PASS, not per frame. A single draw would reach whichever eye happened
	// to render after it and the cursor would be visible to one eye only -- the
	// same rule the debug overlays follow, for a different reason.
	void Draw( int eye )
	{
		if ( !m_settings.enabled || !m_armed )
			return;

		IDirect3DDevice9* device = D3D9Device();
		if ( !device )
			return;

		unsigned int bbW = 0, bbH = 0;
		Stereo().BackbufferSize( bbW, bbH );
		if ( bbW == 0 || bbH == 0 )
			return;

		float px = 0.0f, py = 0.0f;
		if ( !Stereo().ProjectWorldToBackbuffer( m_world, eye, px, py ) )
		{
			++m_notProjected;
			return;
		}

		// The rects, built once and reused for both the outline and the fill.
		const int half = m_settings.size > 0 ? m_settings.size : 1;
		const int thick = m_settings.thickness > 0 ? m_settings.thickness : 1;
		const int out = m_settings.outline > 0 ? m_settings.outline : 0;

		const int cx = (int)( px + 0.5f );
		const int cy = (int)( py + 0.5f );

		IDirect3DSurface9* backBuffer = nullptr;
		if ( FAILED( device->GetBackBuffer( 0, 0, D3DBACKBUFFER_TYPE_MONO, &backBuffer ) )
			 || !backBuffer )
			return;

		// ---- SAVE EVERYTHING WE ARE ABOUT TO CHANGE ------------------------
		//
		// The engine owns this device and is mid-frame. Nothing here may be
		// left different from how it was found.
		//
		// The render target is set explicitly rather than assumed: SetRenderTarget
		// is a known slot but is NOT hooked, so nothing in this project actually
		// proves what is bound at the end of a pass. Binding it ourselves makes
		// the question moot instead of guessed.
		//
		// Scissor and viewport are saved because `Clear` with rects is clipped by
		// BOTH. A cursor that silently fails to draw because the engine left a
		// scissor rect somewhere else would look exactly like a projection bug,
		// and would be debugged as one.
		IDirect3DSurface9* savedTarget = nullptr;
		device->GetRenderTarget( 0, &savedTarget );

		D3DVIEWPORT9 savedViewport = {};
		const bool haveViewport = SUCCEEDED( device->GetViewport( &savedViewport ) );

		DWORD savedScissor = FALSE;
		device->GetRenderState( D3DRS_SCISSORTESTENABLE, &savedScissor );

		// ---- ONLY REBIND IF WE HAVE TO --------------------------------------
		//
		// In the ordinary case the backbuffer IS already the bound target at the
		// end of a pass, and rebinding it is both pointless and the one call
		// here that can fail: D3D9 refuses SetRenderTarget when the currently
		// bound depth-stencil is smaller than the new target, and the backbuffer
		// is the largest surface around. Comparing first removes that failure
		// mode from the common path entirely.
		const bool needRebind = ( savedTarget != backBuffer );
		bool bound = !needRebind;
		if ( needRebind )
			bound = SUCCEEDED( device->SetRenderTarget( 0, backBuffer ) );

		if ( needRebind && !bound && !m_rebindLogged )
		{
			m_rebindLogged = true;
			LogWarn( "menu cursor (d3d9): the backbuffer was not the bound target "
					 "and rebinding it was refused -- most likely a depth-stencil "
					 "smaller than the backbuffer. The cursor cannot draw this "
					 "frame; nothing else is affected." );
		}

		bool drew = false;
		if ( bound )
		{
			D3DVIEWPORT9 full = { 0, 0, bbW, bbH, 0.0f, 1.0f };
			device->SetViewport( &full );
			device->SetRenderState( D3DRS_SCISSORTESTENABLE, FALSE );

			// Outline first, then the bright cross inside it. Two Clear calls
			// rather than four: Clear takes an array of rects but one colour.
			if ( out > 0 )
			{
				D3DRECT o[2];
				MakeCross( o, cx, cy, half + out, thick + out * 2, bbW, bbH );
				device->Clear( 2, o, D3DCLEAR_TARGET,
							   D3DCOLOR_XRGB( 0, 0, 0 ), 1.0f, 0 );
			}

			D3DRECT c[2];
			MakeCross( c, cx, cy, half, thick, bbW, bbH );
			drew = SUCCEEDED( device->Clear( 2, c, D3DCLEAR_TARGET,
											 D3DCOLOR_XRGB( m_settings.r,
															m_settings.g,
															m_settings.b ),
											 1.0f, 0 ) );
		}

		// ---- PUT IT ALL BACK -----------------------------------------------
		//
		// Render target FIRST, then the viewport: setting a target resets the
		// viewport to that surface's full extent, so restoring in the other
		// order would silently throw the saved viewport away.
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

		if ( drew )
		{
			++m_draws;
			m_lastX = cx;
			m_lastY = cy;
		}
		else
		{
			++m_failed;
		}
	}

	void LogState() const
	{
		if ( !m_settings.enabled )
		{
			Log( "menu cursor (d3d9): OFF -- falling back to the world overlay "
				 "cross, which cannot appear in a PAUSE menu at all" );
			return;
		}

		Log( "menu cursor (d3d9): %s | last pixel (%d %d) of %s | %u draws, "
			 "%u not projected, %u refused",
			 m_armed ? "armed" : "idle (no menu)",
			 m_lastX, m_lastY,
			 BackbufferText(),
			 m_draws, m_notProjected, m_failed );

		if ( m_draws == 0 && m_armed )
			Log( "menu cursor (d3d9): armed but never drawn -- if 'not projected' "
				 "is climbing the panel geometry or head basis is not reaching "
				 "the renderer; if 'refused' is climbing the device rejected "
				 "Clear and the rect or the render target is wrong" );
	}

private:
	// A plus sign as two rects, clamped to the target. Shared by the outline and
	// the fill so the two cannot describe different shapes.
	static void MakeCross( D3DRECT* out, int cx, int cy, int half, int thick,
						   unsigned int w, unsigned int h )
	{
		const int ht = thick / 2 > 0 ? thick / 2 : 1;

		// Horizontal bar, then vertical.
		out[0].x1 = cx - half;  out[0].x2 = cx + half;
		out[0].y1 = cy - ht;    out[0].y2 = cy + ht;

		out[1].x1 = cx - ht;    out[1].x2 = cx + ht;
		out[1].y1 = cy - half;  out[1].y2 = cy + half;

		for ( int i = 0; i < 2; ++i )
			Clamp( out[i], w, h );
	}

	// Clear rejects a rect that leaves the surface, and an inverted one draws
	// nothing -- so a cursor at the very edge of the panel must be trimmed
	// rather than passed through.
	static void Clamp( D3DRECT& r, unsigned int w, unsigned int h )
	{
		if ( r.x1 < 0 ) r.x1 = 0;
		if ( r.y1 < 0 ) r.y1 = 0;
		if ( r.x2 > (LONG)w ) r.x2 = (LONG)w;
		if ( r.y2 > (LONG)h ) r.y2 = (LONG)h;
		if ( r.x2 <= r.x1 ) r.x2 = r.x1 + ( r.x1 < (LONG)w ? 1 : 0 );
		if ( r.y2 <= r.y1 ) r.y2 = r.y1 + ( r.y1 < (LONG)h ? 1 : 0 );
	}

	const char* BackbufferText() const
	{
		static char buf[32];
		unsigned int w = 0, h = 0;
		Stereo().BackbufferSize( w, h );
		_snprintf_s( buf, sizeof( buf ), _TRUNCATE, "%ux%u", w, h );
		return buf;
	}

	MenuCursorSettings m_settings;

	Vector m_world = { 0.0f, 0.0f, 0.0f };
	bool m_armed = false;

	int m_lastX = 0;
	int m_lastY = 0;
	unsigned int m_draws = 0;
	unsigned int m_notProjected = 0;
	unsigned int m_failed = 0;
	bool m_rebindLogged = false;
};

} // namespace sinvr
