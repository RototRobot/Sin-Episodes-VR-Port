// IViewRender and CViewSetup -- the client's own view, which is what the engine
// actually renders and culls from.
//
// Why this matters: everything else in this mod patches the *result* of the
// view setup (the matrices the material system is handed) and therefore has to
// fight the engine for the things the view setup decides -- the culling FOV via
// cvars, the eye offset via a ring buffer that tries to spot re-loads, and no way
// at all to tell the main scene from a render-to-texture view. Owning CViewSetup
// replaces all of that with three assignments.
//
// Both reference mods do exactly this; see the README. They signature-scan
// CViewRender::RenderView because in their engines it is not reachable
// virtually. In SiN it does not need scanning at all -- see below.
#pragma once

#include <windows.h>

#include "source_interfaces.h"

namespace sinvr {

//-----------------------------------------------------------------------------
// IViewRender -- the `view` global in client.dll.
//
// !! Verified against a live process on 2026-08-03 (client.dll @ 0x24000000).
// !! The vtable has **29** slots; SDK 2004's iviewrender.h declares 23. Ritual
// !! appended six at 23-28, so slots 0-22 are the SDK's, unshifted -- the same
// !! append-at-the-end pattern as IBaseClientDLL. Everything we need is below
// !! the append point.
//
// Evidence, read out of the live process:
//
//   CHLClient::View_Render (IBaseClientDLL slot 23) @ client+0xFDAD0 ends with
//
//       8B 0D 60 70 37 24     mov  ecx, [0x24377060]   ; the `view` global
//       8B 11                 mov  edx, [ecx]          ; vtable
//       50                    push eax                 ; rect
//       FF 52 10              call [edx+0x10]          ; slot 4
//       C2 04 00              ret  4
//
//   which is verbatim the SDK's
//       void CHLClient::View_Render( vrect_t *rect ) { ... view->Render( rect ); }
//   and pins Render to slot 4.
//
//   Slot 11 @ client+0x1D4EC0 is
//       8D 41 0C   lea eax, [ecx+0x0C]
//       C3         ret
//   i.e. GetViewSetup() returning `this + 0x0C`, so CViewRender::m_View sits at
//   offset 0x0C. Slot 14 @ client+0x1D4E80 is `mov eax,[ecx+0x2DC]; ret`, the
//   trivial FrameNumber() getter -- a control confirming the numbering.
//
// **CViewRender::Render calls RenderView( m_View, ... ) directly, not through
// the vtable** (SDK view.cpp:979). Hooking slot 5 would therefore miss the main
// scene entirely and only catch timerefresh/envmap. Slot 4 is the one that is
// genuinely reached virtually.
//-----------------------------------------------------------------------------
namespace viewrender_slot {
enum
{
	kOnRenderStart = 3,   // iviewrender.h:73
	kRender        = 4,   // iviewrender.h:76  VERIFIED  <- called from View_Render
	kRenderView    = 5,   // iviewrender.h:78  (direct-called internally; see above)
	kGetDrawFlags  = 6,
	kGetFrustum    = 9,
	kGetViewSetup  = 11,  // iviewrender.h:94  VERIFIED  returns this+0x0C
	kFrameNumber   = 14,  // VERIFIED  mov eax,[ecx+0x2DC]

	kSlotCount     = 29,  // SDK 2004 declares 23
};
} // namespace viewrender_slot

//-----------------------------------------------------------------------------
// CViewSetup -- public/view_shared.h.
//
// !! Verified field-by-field against the live process. SiN's matches SDK 2004
// !! exactly; Ritual did not touch this one. Three independent cross-checks,
// !! all read while the game sat in a map at 1920x1080:
//
//   +0x48 angles   = (-18.9734, -108.0725, 0)
//                    vs engine.dll+0x3F51F4 = (-18.9760, -108.0727, 0)
//                    -- the same numbers one frame apart
//   +0x54 zNear    = 7.0
//   +0x58 zFar     = 28377.92
//                    -- exactly what our PerspectiveX trace logs every frame
//   +0x28 fov      = 91.3085, +0x2C fovViewmodel = 68.3818
//                    -- ScaleFOVByWidthRatio(75, (16/9)/(4/3)) = 91.31 and the
//                       same scaling of 54 = 68.38, so the underlying values are
//                       the stock fov 75 and viewmodel_fov 54
//
// NOTE there is **no m_flAspectRatio here**. That field arrives in a later
// Source; this engine takes the aspect from IVEngineClient::GetScreenAspectRatio
// at the top of CViewRender::Render.
//-----------------------------------------------------------------------------
struct CViewSetup
{
	int   context;                        // 0x00
	int   x;                              // 0x04
	int   y;                              // 0x08
	int   width;                          // 0x0C
	int   height;                         // 0x10
	bool  clearColor;                     // 0x14
	bool  clearDepth;                     // 0x15
	bool  bForceClearWholeRenderTarget;   // 0x16
	bool  m_bOrtho;                       // 0x17
	float m_OrthoLeft;                    // 0x18
	float m_OrthoTop;                     // 0x1C
	float m_OrthoRight;                   // 0x20
	float m_OrthoBottom;                  // 0x24
	float fov;                            // 0x28
	float fovViewmodel;                   // 0x2C
	Vector origin;                        // 0x30
	Vector m_vUnreflectedOrigin;          // 0x3C
	QAngle angles;                        // 0x48
	float zNear;                          // 0x54
	float zFar;                           // 0x58
	float zNearViewmodel;                 // 0x5C
	float zFarViewmodel;                  // 0x60
	bool  m_bForceAspectRatio1To1;        // 0x64
	bool  m_bRenderToSubrectOfLargerScreen; // 0x65
	bool  m_bUseRenderTargetAspectRatio;  // 0x66
};

static_assert( sizeof( CViewSetup ) == 0x68, "CViewSetup layout must match the engine's" );
static_assert( offsetof( CViewSetup, fov ) == 0x28, "fov offset" );
static_assert( offsetof( CViewSetup, origin ) == 0x30, "origin offset" );
static_assert( offsetof( CViewSetup, angles ) == 0x48, "angles offset" );
static_assert( offsetof( CViewSetup, zNear ) == 0x54, "zNear offset" );

//-----------------------------------------------------------------------------
// Locate the `view` global by reading the instruction that uses it, rather than
// hard-coding client+0x377060. The RVA is right for this build, but a different
// SiN build would move it and a stale absolute address is the kind of failure
// that looks like a subtle rendering bug instead of an obvious one.
//
// Scans CHLClient::View_Render -- whose address we already have, since we hook
// it -- for:
//
//     8B 0D <imm32>   mov ecx, [view]
//     8B 11           mov edx, [ecx]
//     50              push eax
//     FF 52 10        call [edx+0x10]
//
// Returns the address of the pointer, i.e. `IViewRender**`, or null.
//-----------------------------------------------------------------------------
inline void** FindViewGlobal( const void* viewRenderFn, size_t scanBytes = 128 )
{
	if ( !viewRenderFn )
		return nullptr;

	const BYTE* p = reinterpret_cast<const BYTE*>( viewRenderFn );

	__try
	{
		for ( size_t i = 0; i + 12 <= scanBytes; ++i )
		{
			if ( p[i] != 0x8B || p[i + 1] != 0x0D )
				continue;
			if ( p[i + 6] != 0x8B || p[i + 7] != 0x11 )
				continue;
			if ( p[i + 8] != 0x50 )
				continue;
			if ( p[i + 9] != 0xFF || p[i + 10] != 0x52 )
				continue;

			// The call must be through slot 4; anything else is a different
			// forwarder that happens to share the prologue shape.
			if ( p[i + 11] != viewrender_slot::kRender * 4 )
				continue;

			return *reinterpret_cast<void***>( const_cast<BYTE*>( p + i + 2 ) );
		}
	}
	__except ( EXCEPTION_EXECUTE_HANDLER )
	{
	}
	return nullptr;
}

} // namespace sinvr
