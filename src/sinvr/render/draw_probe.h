#pragma once
//-----------------------------------------------------------------------------
// THE SAVE/LOAD Z-FIGHTING FIX -- the draw-call layer.
//
// Two things live here, and they were built in that order: the MEASUREMENT
// (step 1, always on) and the FIX it selected (step 2, keyed by
// menu_depth_fix). The measurement stays because it is how the fix is
// verified -- it reports the engine's own depth state, sampled before the fix
// touches anything.
//
// ---- STEP 1: WHAT WAS MEASURED, AND WHY -------------------------------------
//
// Two questions the fix depended on and that were, at the time, only BELIEVED:
//
//   1. CAN WE TELL A MENU DRAW FROM A WORLD DRAW?
//      The discriminator is "the projection currently loaded is our menu-panel
//      quad".
//
//      ANSWERED YES, 2026-09-04, and emphatically. Main-menu Load reported
//      166-185 menu draws per eye pass and pause-menu Load 183-188, while
//      ordinary gameplay reported EXACTLY ZERO across six heartbeats -- about
//      5500 eye passes and 1.5 million draws with no false positive at all.
//
//      Also settled: only DrawIndexedPrimitive ever fires. DP, DPUP and DIPUP
//      were zero in every phase. All four stay hooked anyway; the cost is a
//      branch and it removes the ambiguity permanently.
//
//   2. WHICH depth state is actually causing the tie?
//      ZENABLE, ZWRITEENABLE and ZFUNC read back at every menu draw, because
//      the plausible answers needed DIFFERENT fixes:
//
//        ZFUNC = LESS      -> coplanar draws FAIL outright, the later panel is
//                             rejected per-pixel, and the fix is ZFUNC.
//        ZFUNC = LESSEQUAL -> ties PASS, so the shimmer is depth-interpolation
//                             precision across the perspective quad, and the fix
//                             is ZENABLE.
//
//      ANSWERED LESSEQUAL, everywhere, which killed the ZFUNC branch outright.
//      See the step 2 block below for what it left standing.
//
// ---- WHY THIS LAYER, WHEN OverrideDepthEnable FAILED ------------------------
//
// The handover records two reasons attempt 1 could not work. BOTH are properties
// of WHERE it hooked, not of the fix -- forcing the depth test off fixed the
// main-menu shimmer on the first try.
//
//   * OverrideDepthEnable has NO GETTER, so "restoring" it could never put back
//     a value it was unable to read. It clobbered a global the engine also
//     writes, which is why the corruption was eye-asymmetric while our own
//     set/clear counts balanced perfectly at 8047/8047.
//   * We knew when the menu STARTED drawing and had no hook for when it
//     STOPPED, so any state we set outlived its scope by construction.
//
// D3D9 answers both. GetRenderState EXISTS -- look_arrow.h and menu_cursor.h
// already save and restore D3DRS_SCISSORTESTENABLE exactly this way -- and a
// draw-call hook IS the "when does it stop" hook: the scope of a change shrinks
// to a single call, so it cannot leak even in principle. A wrong discriminator
// would then cost one mis-drawn quad that self-corrects on the next draw, not a
// corrupted frame.
//
// ---- SLOTS ------------------------------------------------------------------
//
// Verified by counting the pure virtuals in the Windows SDK d3d9.h rather than
// from memory, and cross-checked against this codebase: the same count puts
// Reset at 16 and Present at 17, which is exactly what d3d9_present_hook.cpp
// already ships and runs. All four draw entry points are hooked and counted
// SEPARATELY, because "menu=0" would otherwise be ambiguous between "the
// discriminator is wrong" and "VGUI draws through an entry point we did not
// hook".
//-----------------------------------------------------------------------------

#include <windows.h>
#include <d3d9.h>
#include <string.h>

#include "../hooks/vtable_hook.h"
#include "../../common/log.h"

namespace sinvr {

// IDirect3DDevice9 vtable indices, fixed by the COM ABI. See the header note.
constexpr int kSlotSetRenderTarget = 37;
constexpr int kSlotSetViewport = 47;
constexpr int kSlotDrawPrimitive = 81;
constexpr int kSlotDrawIndexedPrimitive = 82;
constexpr int kSlotDrawPrimitiveUP = 83;
constexpr int kSlotDrawIndexedPrimitiveUP = 84;

enum DrawEntry
{
	kDrawPrim = 0,
	kDrawIndexed,
	kDrawPrimUP,
	kDrawIndexedUP,
	kDrawEntryCount
};

inline const char* ZFuncName( DWORD f )
{
	switch ( f )
	{
		case D3DCMP_NEVER:        return "NEVER";
		case D3DCMP_LESS:         return "LESS";
		case D3DCMP_EQUAL:        return "EQUAL";
		case D3DCMP_LESSEQUAL:    return "LESSEQUAL";
		case D3DCMP_GREATER:      return "GREATER";
		case D3DCMP_NOTEQUAL:     return "NOTEQUAL";
		case D3DCMP_GREATEREQUAL: return "GREATEREQUAL";
		case D3DCMP_ALWAYS:       return "ALWAYS";
		default:                  return "?";
	}
}

// One distinct (ZENABLE, ZWRITEENABLE, ZFUNC) combination and how often it was
// seen. A table rather than a single sample: if the menu draws through more than
// one depth configuration, that is itself the answer and a single sample would
// hide it.
struct DepthSample
{
	DWORD zEnable = 0;
	DWORD zWrite = 0;
	DWORD zFunc = 0;
	unsigned int count = 0;
};

constexpr int kMaxDepthSamples = 12;

// Every 256th non-menu draw is sampled too. Not because the world's depth state
// is interesting, but as a control: if GetRenderState were failing or returning
// a constant, the menu table alone could not tell us. Two tables that differ
// prove the readback works.
constexpr unsigned int kWorldSampleStride = 256;

// ---- THE INDEPENDENT CHECK ON "IS THIS REALLY A MENU DRAW" ------------------
//
// The ZWRITE split says the discriminator is catching geometry, but that is an
// inference from one number. The material NAME says it outright: a draw using
// vgui/white is a UI fill and a draw using a world or model material is not.
//
// IMaterialSystem::Bind is already hooked, already proved, and already purely
// observational. Its known limitation -- it fires only when the material
// CHANGES -- is not a limitation here. "The most recently bound material" is
// exactly the material a draw uses. That earlier failure was about attributing
// materials to ORTHO WINDOWS, which is a different and genuinely broken idea.
//
// Recorded split by whether the draw writes depth, so if mode 3 is wrong the
// next step needs no further blind run: the table will name what is being
// caught and what is being left alone.
struct MenuMaterialUse
{
	char name[64] = {};
	unsigned int writers = 0;
	unsigned int nonWriters = 0;
};

constexpr int kMaxMaterials = 24;

// ---- THE VIEWMODEL DEPTH SLAB -----------------------------------------------
//
// Measuring, not fixing. The question is why the gun never clips into walls and
// never gets occluded by them -- reported as the gun sitting "on a different
// plane of reality", which is a very good description of what is suspected.
//
// Source's CViewRender::DrawViewModels compresses the viewmodel into the FRONT
// SLICE of the depth buffer before drawing it, and says so:
//
//     // HACK HACK: Munge the depth range to prevent view model from poking
//     // into walls, etc. Force clipped down range
//     pRenderContext->DepthRange( 0.0f, 0.1f );
//
// The world occupies 0..1 and the gun 0..0.1, so the gun cannot lose a depth
// test to anything at any distance. That is ONE behaviour producing both
// symptoms, not two bugs.
//
// D3D9 has no separate depth-range call -- it is D3DVIEWPORT9's MinZ/MaxZ -- so
// it arrives at SetViewport, slot 47, verified against the SDK header the same
// way the draw slots were.
//
// WHAT WOULD CONFIRM IT: a distinct 0.0..0.1 range appearing a couple of times
// per eye pass, with a small number of draws under it, and WEAPON materials on
// those draws. The material column is what makes this proof rather than a
// coincidence -- and it is the check that was missing when ZWRITE was read as
// meaning UI.
//
// WHAT WOULD COMPLICATE IT: anything ELSE using a compressed range. That is
// what the table is for.
struct ViewportUse
{
	float minZ = 0.0f;
	float maxZ = 0.0f;
	unsigned long width = 0;
	unsigned long height = 0;
	unsigned int sets = 0;      // times SetViewport asked for this range
	unsigned int draws = 0;     // draws made while it was the live range
};

constexpr int kMaxViewports = 8;

// ---- WHERE THE WORLD IS ACTUALLY DRAWN --------------------------------------
//
// Edge shimmer survived BOTH native resolution and MSAA x4, which kills the two
// obvious explanations at once. The backbuffer really is multisample=4 -- that
// was measured. But the backbuffer being multisampled says nothing about where
// the SCENE was rendered.
//
// Source draws through intermediate render targets whenever post-processing is
// involved (HDR, bloom, refraction, water). Those are created by the engine and
// are typically NOT multisampled. If the world lands in one of those and is then
// blitted to the multisampled backbuffer, MSAA x4 is anti-aliasing a
// FULL-SCREEN QUAD -- doing nothing, at full cost, while the menu reports it as
// on.
//
// That would explain the symptom precisely, and it is not something any amount
// of resolution or sample count can fix. It is also cheap to settle: record
// every render target the game binds, with its multisample type, and count the
// draws landing in each. The one with the overwhelming majority of draws is the
// scene.
struct RenderTargetUse
{
	unsigned long width = 0;
	unsigned long height = 0;
	DWORD format = 0;
	DWORD multisample = 0;
	unsigned int sets = 0;
	unsigned int draws = 0;
	char firstMaterial[64] = {};
};

constexpr int kMaxRenderTargets = 12;

// ---- TELLING THE VIEWMODEL SLAB FROM THE OTHER ONE --------------------------
//
// Measured 2026-09-05, and the run answered the "what would complicate this"
// question with something real but harmless -- there are TWO compressed ranges:
//
//   0.000..0.100  sets=920   draws=1840  v_magnum, v_hands      <- the viewmodel
//   0.000..0.010  sets=1840  draws=3682  engine/occlusionproxy  <- DO NOT TOUCH
//
// 920 sets is exactly one per eye pass and 1840 draws exactly two per set: the
// gun and the hands. The other is Source's occlusion-query system drawing
// invisible proxy boxes, and widening ITS range would break occlusion culling.
//
// They differ by a factor of ten, so a bracket separates them with an enormous
// margin -- no float equality, and no need to be clever. Anything at full range
// (the 256x256 and 128x128 render-target cameras in the same log) is untouched
// because it never enters this bracket at all.
inline bool IsViewmodelSlab( const D3DVIEWPORT9& vp )
{
	return vp.MaxZ > 0.05f && vp.MaxZ < 0.50f;
}

//-----------------------------------------------------------------------------
// STEP 2 -- THE FIX, chosen by what step 1 measured on 2026-09-04.
//
// The measurement killed one hypothesis outright and produced a better one.
//
// ZFUNC IS LESSEQUAL EVERYWHERE -- every sample, both menus, world draws too.
// So coplanar draws are not being REJECTED; ties pass and the painter's order
// survives. The shimmer is therefore depth-INTERPOLATION precision: two
// overlapping panels on a quad in perspective rasterise to depths that differ
// by an ulp or two per pixel, so some pixels fail LESSEQUAL and some do not.
// That is a stipple, which is exactly what a shimmer is.
//
// AND THE FLAGGED DRAWS WRITE DEPTH -- which was read as "the menu is stamping
// depth and fighting itself". That reading was WRONG, and the wrongness is the
// whole lesson of the next section: a draw that writes depth is geometry. The
// numbers below are real, the interpretation under them was not.
//
// Draws flagged as menu run in three different configurations, and the mix
// INVERTS between the two menus:
//
//                                  main menu      pause menu
//     ZENABLE on, ZWRITE ON          ~25%           ~78%
//     ZENABLE on, ZWRITE off         ~64%           ~21%
//     ZENABLE off                    ~11%           ~0.8%
//
// (Measured over four heartbeats each, ~170k menu draws per 5s interval; the
// three rows are stable to within a percent across repeats.)
//
// That inversion is real and it still explains why attempt 1 fixed the main
// menu on the first try and broke the pause menu -- but the reason is simpler
// than "different depth configurations". The pause menu has far more GEOMETRY
// behind it than the main menu does, so it had far more to lose.
//
// Two modes, because the measurement supports two different minimal fixes and
// arguing about them is more expensive than shipping both behind one key:
//
//   1  ZWRITEENABLE = FALSE.  Menu draws stop writing depth, so menu panels
//      stop fighting EACH OTHER, and the depth test is left intact. Minimal --
//      but it makes every panel test against the WORLD's depth instead of the
//      menu's own, so geometry nearer than the panel could clip it.
//
//   2  ZENABLE = FALSE.  No test, no write: pure painter's algorithm, which is
//      what VGUI was designed for and what flat mode actually does. Ordering
//      falls back to draw order, which already encodes VGUI's z-order because
//      PaintTraverse walks it. This is the default.
//
// ---- WHY THIS CANNOT REPEAT ATTEMPT 1 ---------------------------------------
//
// Two properties, and both are structural rather than careful:
//
//   * WE RESTORE A VALUE WE READ. GetRenderState returns the real current
//     value, so the restore puts back what was actually there. That is the
//     precise thing OverrideDepthEnable could not do -- with no getter, its
//     "restore" forcibly disabled a global the engine also writes.
//
//   * THE SCOPE IS ONE CALL. Set before, restore after, in the same detour.
//     There is no span for it to leak across, so the whole class of failure
//     that produced "world geometry through walls, left eye only" cannot
//     occur. A wrong discriminator costs one mis-drawn quad, not a frame --
//     and step 1 measured the discriminator at ZERO false positives across
//     ~1.5 million gameplay draws.
//-----------------------------------------------------------------------------
// ---- WHAT MODES 1 AND 2 GOT WRONG, tested 2026-09-04 ------------------------
//
// Mode 2 fixed the load-screen shimmer and reproduced attempt 1's damage
// EXACTLY: main-menu character model wrong in the left eye, pause-menu
// transparency wrong in one eye, world geometry through walls.
//
// That result is worth more than the fix it failed to be, because a per-draw
// guard that reads and restores CANNOT LEAK -- and the damage came back
// identical anyway. So the handover's explanation, that attempt 1 leaked state
// out of its scope, is DISPROVED. Scope was never the problem.
//
// The problem is that we were disabling depth on draws that need it. The
// discriminator answers "was the menu projection loaded", and in the main menu
// and the pause menu the 3D content -- character model, world -- is drawn WHILE
// a menu is up. Those draws were being caught too.
//
// And the measured table already said so, if read properly. ZWRITE=TRUE means a
// draw is STAMPING THE DEPTH BUFFER. That is geometry behaviour, not 2D UI
// behaviour; a VGUI fill has no reason to write depth. So the ~78% of
// "menu" draws in the pause menu that write depth were never menu draws --
// they are the world, and 25% in the main menu are the character model.
//
// Which is also why the corruption tracked the 3D content in both attempts, and
// why mode 1 would have broken it too: BOTH modes touched the depth writers.
//
//   3  Disable the depth TEST, but ONLY for menu draws that do not write depth.
//      Depth WRITERS are left completely untouched, so geometry cannot be
//      affected by construction -- which is exactly the failure being fixed.
//
//      It also matches the shimmer's mechanism rather than working around it.
//      Overlapping UI panels that do not write depth can only be fighting
//      something that DOES; stopping the non-writers from testing removes the
//      comparison entirely, while the writers keep stamping depth as before.
//   4  Disable the depth test for menu draws whose BOUND MATERIAL is a UI
//      material. This is the one that works, and it is the first version of
//      this fix built on measurement rather than inference.
enum MenuDepthFixMode
{
	kFixOff = 0,
	kFixNoZWrite = 1,
	kFixNoZTest = 2,
	kFixNonWriters = 3,
	kFixUiOnly = 4,
	kFixUiBias = 5
};

// ---- WHY MODE 4 IS NOT THE END, tested 2026-09-04 ---------------------------
//
// Mode 4 fixed the shimmer and left the geometry clean in both eyes. One defect
// remained: the Load dialog drew BEHIND the pause menu's text.
//
// That is the predicted cost of switching the test off, and it identifies what
// the depth test was actually FOR. VGUI's SetZPos puts a popup in front of its
// parent, menu_depth_scale maps that z onto the panel normal, and the DEPTH
// TEST is what enforced it -- the dialog is drawn EARLIER than the menu text
// and was winning on depth, not on order. Remove the test and draw order is all
// that is left, and draw order disagrees.
//
// So the test has to stay and the tie has to go instead. That is mode 5, and it
// is the "per-draw tie-breaker" the handover proposed as the remaining route --
// now with a discriminator precise enough to aim it.
//
//   5  Keep the depth test. Give each successive UI draw a slightly NEARER
//      depth bias, so coplanar children of one window can never tie, while
//      window-to-window separation still comes from menu_depth_scale.
//
// ---- THE RAMP AND THE GAP ARE ONE SETTING, NOT TWO --------------------------
//
// The tie-breaker must be big enough to beat depth-interpolation error and
// SMALLER THAN THE WINDOW SEPARATION IT SITS INSIDE. Miss that second half and
// the ramp carries a later-drawn panel straight past the window in front of it,
// which is a worse bug than the shimmer.
//
// Measured 2026-09-04, first try at menu_depth_bias = 8.0:
//
//     most UI draws in one ortho call   93          (from the probe)
//     ramp                              93 x 8   = 744 depth units
//     window separation at scale 0.25            ~ 500 depth units
//
// The ramp was LARGER than the gap, so the pause menu's text -- drawn later
// than the Load dialog -- climbed past it and the dialog stayed behind. The
// scroll bar is the same effect at the tail of the ramp.
//
// Both knobs move that comparison, so they are set together:
//
//     menu_depth_scale 0.25 -> 2.0    separation ~500 -> ~4000 units
//     menu_depth_bias  8.0  -> 3.0    ramp       744  ->  ~280 units
//
// which leaves the ramp at roughly 7% of the gap. menu_depth_scale is a
// physical displacement of the panel along its own normal, so 2.0 world units
// at a 150-unit viewing distance is about a 1% size change -- invisible, and
// nothing like the 50.0 that once pushed the windows visibly away.
//
// The probe reports the ramp in depth units every heartbeat so this comparison
// never has to be reconstructed from memory again.
//
// UNITS: D3DRS_DEPTHBIAS is a float in NORMALIZED depth, which DXVK multiplies
// by the depth buffer's r-value on the way to Vulkan (d3d9_device.cpp,
// m_depthBiasScale). Checked in the DXVK we actually ship rather than assumed:
// the D3D9 and Vulkan conventions differ by a factor of 2^24, so the wrong one
// gives either no effect whatsoever or a catastrophe. The config value is in
// DEPTH BUFFER UNITS and converted here, so it reads as what it is.
constexpr float kDepthUnit = 1.0f / 16777216.0f;   // one ULP of a D24 buffer

// ---- WHAT THE MATERIAL LOG SETTLED, 2026-09-04 ------------------------------
//
// Mode 3 was the worst of both: the rendering damage stayed AND the shimmer came
// back. That is not a near miss, it is a clean falsification -- and the material
// table printed on the same run says exactly why. ZWRITE does not split UI from
// geometry. It splits OPAQUE from TRANSLUCENT, and both halves contain both.
//
//   writes=6453  nowrite=821    vgui/white          <- UI, and it WRITES
//   writes=30751 nowrite=0      __font              <- UI, always writes
//   writes=2115  nowrite=0      __gui textu         <- UI, always writes
//   writes=705   nowrite=0      vgui/hud/800corner1 <- UI, always writes
//   writes=464   nowrite=0      warehouse/se1_brick_02        <- world
//   writes=465   nowrite=0      maps/se1_docks01/...          <- world
//   writes=820   nowrite=0      engine/writez                 <- engine
//   writes=3704  nowrite=0      (none)                        <- no material
//   writes=0     nowrite=926    decals/wallstain01a           <- DECAL
//   writes=0     nowrite=926    decals/rendershadow           <- DECAL
//   writes=0     nowrite=1230   se1_decals/se1_pothole_decal_01
//
// So mode 3 hit every DECAL and missed every piece of UI. Decals are
// translucent world geometry drawn with ZWRITE off -- switching their depth
// test off draws them straight through walls, which is precisely the
// "world geometry appears through walls" report, in both attempts.
//
// And `decals/rendershadow` is in that list, which is why the main-menu
// character model looked wrong: its shadow was being drawn without a depth
// test.
//
// The lesson, and it is the second one this bug has taught about inference:
// ZWRITE describes HOW a draw uses the depth buffer, not WHAT it is drawing.
// Only the material name answers what. Ask the question you actually mean.

class DrawProbe
{
public:
	bool Install( IDirect3DDevice9* device );

	void SetEnabled( bool on ) { m_enabled = on; }
	bool Enabled() const { return m_enabled; }
	bool Installed() const { return m_installed; }

	// Fed by Detour_Bind in stereo.cpp on every material change. Copied
	// rather than kept as a pointer: the engine owns that string and we read
	// it later, on another thread, in Report().
	void SetCurrentMaterial( const char* name )
	{
		if ( !name )
		{
			m_material[0] = 0;
			m_currentIsUi = false;
			return;
		}
		int i = 0;
		for ( ; i < (int)sizeof( m_material ) - 1 && name[i]; ++i )
			m_material[i] = name[i];
		m_material[i] = 0;
		m_currentIsUi = IsUiMaterial( m_material );
	}

	bool CurrentIsUi() const { return m_currentIsUi; }

	// Reset per ORTHO CALL, not per pass: see the bound argument above.
	void NoteOrthoCall()
	{
		if ( m_uiDrawIndex > m_maxUiPerOrtho )
			m_maxUiPerOrtho = m_uiDrawIndex;
		m_uiDrawIndex = 0;
		++m_orthoCalls;
	}

	unsigned int NextBiasIndex() { return m_uiDrawIndex++; }
	void SetBiasStep( float units ) { m_biasStep = units * kDepthUnit; }
	float BiasStep() const { return m_biasStep; }

	// Every UI material observed in the log starts with one of two prefixes and
	// nothing else does: "vgui/" covers vgui/white and the vgui/hud/* set,
	// "__" covers Source's generated __font and __gui surfaces. World, decal,
	// engine and unbound draws all fall outside both.
	static bool IsUiMaterial( const char* n )
	{
		if ( !n || !n[0] )
			return false;
		if ( n[0] == '_' && n[1] == '_' )
			return true;
		const char* p = "vgui/";
		for ( int i = 0; p[i]; ++i )
		{
			char c = n[i];
			if ( c >= 'A' && c <= 'Z' )
				c = (char)( c - 'A' + 'a' );
			if ( c != p[i] )
				return false;
		}
		return true;
	}

	void SetViewmodelDepth( bool on ) { m_viewmodelDepth = on; }
	bool ViewmodelDepth() const { return m_viewmodelDepth; }
	void NoteSlabWidened() { ++m_slabWidened; }

	void SetFixMode( int mode ) { m_fixMode = mode; }
	int FixMode() const { return m_fixMode; }
	bool MenuProjection() const { return m_menuProj; }
	void NoteFixApplied() { ++m_fixApplied; }

	// ---- THE DISCRIMINATOR ------------------------------------------------
	//
	// Three writers in stereo.cpp, and between them they cover every path that
	// can change the projection:
	//
	//   NoteEyePass()             -- eye pass start, beside g_menuOrthoDepthIndex
	//   SetMenuProjection(true)   -- ApplyMenuPanelProjection, on success only
	//   SetMenuProjection(false)  -- top of Detour_Ortho, and ApplyEyeProjection
	//
	// Clearing at the TOP of Detour_Ortho matters: a menu ortho followed by a
	// HUD ortho would otherwise leave the flag set over draws that are not the
	// menu's. Setting it only on SUCCESS matters for the same reason -- the
	// fallback path draws through a plain ortho, which is not our quad.
	void SetMenuProjection( bool on ) { m_menuProj = on; }

	// Called from the SetViewport detour. Finds or adds this depth range and
	// makes it current.
	void NoteViewport( float minZ, float maxZ, unsigned long w, unsigned long h )
	{
		if ( !m_enabled )
			return;

		for ( int i = 0; i < m_viewportCount; ++i )
		{
			if ( m_viewports[i].minZ == minZ && m_viewports[i].maxZ == maxZ &&
				 m_viewports[i].width == w && m_viewports[i].height == h )
			{
				++m_viewports[i].sets;
				m_viewportCur = i;
				return;
			}
		}

		if ( m_viewportCount >= kMaxViewports )
		{
			m_viewportCur = -1;
			return;
		}

		ViewportUse& v = m_viewports[m_viewportCount];
		v.minZ = minZ;
		v.maxZ = maxZ;
		v.width = w;
		v.height = h;
		v.sets = 1;
		v.draws = 0;
		m_viewportCur = m_viewportCount;
		++m_viewportCount;
	}

	// Called from the SetRenderTarget detour with the surface's description.
	void NoteRenderTarget( unsigned long w, unsigned long h, DWORD fmt, DWORD ms )
	{
		if ( !m_enabled )
			return;

		for ( int i = 0; i < m_rtCount; ++i )
		{
			if ( m_rts[i].width == w && m_rts[i].height == h &&
				 m_rts[i].format == fmt && m_rts[i].multisample == ms )
			{
				++m_rts[i].sets;
				m_rtCur = i;
				return;
			}
		}

		if ( m_rtCount >= kMaxRenderTargets )
		{
			m_rtCur = -1;
			return;
		}

		RenderTargetUse& rt = m_rts[m_rtCount];
		rt.width = w;
		rt.height = h;
		rt.format = fmt;
		rt.multisample = ms;
		rt.sets = 1;
		rt.draws = 0;
		rt.firstMaterial[0] = 0;
		m_rtCur = m_rtCount;
		++m_rtCount;
	}

	void NoteEyePass()
	{
		m_menuProj = false;
		++m_passes;
	}

	// Render thread only, and on EVERY draw the game makes -- so the early-out
	// ordering is deliberate: one bool test for the overwhelming majority of
	// calls, and no device round trip unless the draw is a menu draw.
	void NoteDraw( IDirect3DDevice9* device, int entry )
	{
		if ( !m_enabled )
			return;

		++m_total[entry];

		// Which render target this draw lands in -- see RenderTargetUse.
		if ( m_rtCur >= 0 && m_rtCur < m_rtCount )
		{
			RenderTargetUse& rt = m_rts[m_rtCur];
			++rt.draws;
			if ( !rt.firstMaterial[0] && m_material[0] )
			{
				int k = 0;
				for ( ; k < (int)sizeof( rt.firstMaterial ) - 1 && m_material[k]; ++k )
					rt.firstMaterial[k] = m_material[k];
				rt.firstMaterial[k] = 0;
			}
		}

		// Which depth slab this draw lands in. Recorded for every draw, not
		// only compressed ones, so the ordinary 0..1 range gives the count to
		// compare against.
		if ( m_viewportCur >= 0 && m_viewportCur < m_viewportCount )
		{
			++m_viewports[m_viewportCur].draws;
			if ( m_viewports[m_viewportCur].maxZ < 0.999f )
				NoteSlabMaterial();
		}

		if ( !m_menuProj )
		{
			if ( ++m_worldStride >= kWorldSampleStride )
			{
				m_worldStride = 0;
				Sample( device, m_world, m_worldCount, nullptr );
			}
			return;
		}

		++m_menu[entry];
		DWORD zWrite = 0;
		Sample( device, m_depth, m_depthCount, &zWrite );
		NoteMaterial( zWrite != 0 );
	}

	// Heartbeat thread. Counters are written on the render thread without
	// interlocks: a diagnostic that loses one draw in ten thousand to a race
	// still answers "hundreds vs none", which is the whole question.
	void Report()
	{
		if ( !m_enabled || !m_installed )
			return;

		unsigned int total = 0, menu = 0;
		for ( int i = 0; i < kDrawEntryCount; ++i )
		{
			total += m_total[i];
			menu += m_menu[i];
		}

		const unsigned int passes = m_passes ? m_passes : 1;

		Log( "draw probe: %u eye passes | total=%u (DP %u / DIP %u / DPUP %u / DIPUP %u)"
			 " | menu=%u (DP %u / DIP %u / DPUP %u / DIPUP %u)"
			 " | per pass: total %u, menu %u | fix=%d applied=%u",
			 m_passes, total,
			 m_total[kDrawPrim], m_total[kDrawIndexed],
			 m_total[kDrawPrimUP], m_total[kDrawIndexedUP],
			 menu,
			 m_menu[kDrawPrim], m_menu[kDrawIndexed],
			 m_menu[kDrawPrimUP], m_menu[kDrawIndexedUP],
			 total / passes, menu / passes,
			 m_fixMode, m_fixApplied );

		for ( int i = 0; i < m_depthCount; ++i )
			LogSample( "menu ", m_depth[i] );
		for ( int i = 0; i < m_worldCount; ++i )
			LogSample( "world", m_world[i] );

		// The column that matters is which side each material falls on. A UI
		// material appearing under "writes" would mean the split is not the
		// clean geometry/UI divide it looks like.
		if ( m_viewmodelDepth )
			Log( "draw probe: viewmodel depth slab WIDENED to full range %u time(s) "
				 "this interval (expect ~1 per eye pass)", m_slabWidened );

		for ( int i = 0; i < m_rtCount; ++i )
			Log( "draw probe: RT %lux%-5lu fmt=%-3lu MSAA=%lu  sets=%-6u draws=%-7u %s%s",
				 m_rts[i].width, m_rts[i].height,
				 (unsigned long)m_rts[i].format, (unsigned long)m_rts[i].multisample,
				 m_rts[i].sets, m_rts[i].draws,
				 m_rts[i].firstMaterial[0] ? m_rts[i].firstMaterial : "(none)",
				 m_rts[i].multisample == 0 ? "   <== NO MSAA" : "" );

		for ( int i = 0; i < m_viewportCount; ++i )
			Log( "draw probe: viewport %lux%lu  depth %.3f..%.3f  sets=%u draws=%u%s",
				 m_viewports[i].width, m_viewports[i].height,
				 m_viewports[i].minZ, m_viewports[i].maxZ,
				 m_viewports[i].sets, m_viewports[i].draws,
				 m_viewports[i].maxZ < 0.999f ? "   <== COMPRESSED SLAB" : "" );

		for ( int i = 0; i < m_slabCount; ++i )
			Log( "draw probe: slab material  x%-7u %s",
				 m_slab[i].writers, m_slab[i].name );

		// Answers "do the dialog and the menu share an Ortho call" -- an
		// inference the handover made from a failed experiment and never
		// measured, and the bias bound depends on it.
		// The ramp, in depth-buffer units, beside the count that produced it.
		// This is the number that has to stay under the window separation --
		// see "THE RAMP AND THE GAP ARE ONE SETTING" above.
		Log( "draw probe: ortho calls=%u | most UI draws in one call=%u | "
			 "ramp=%.0f depth units (step %.1f)",
			 m_orthoCalls, m_maxUiPerOrtho,
			 (double)m_maxUiPerOrtho * (double)( m_biasStep / kDepthUnit ),
			 (double)( m_biasStep / kDepthUnit ) );

		for ( int i = 0; i < m_materialCount; ++i )
			Log( "draw probe: menu material  writes=%-7u nowrite=%-7u  %s",
				 m_materials[i].writers, m_materials[i].nonWriters,
				 m_materials[i].name );

		// Delta, not cumulative. A running total cannot answer "is it happening
		// NOW", and the whole experiment is a comparison between one heartbeat
		// with a dialog open and one without.
		Reset();
	}

private:
	static void LogSample( const char* what, const DepthSample& s )
	{
		Log( "draw probe: %s depth  ZENABLE=%-5s ZWRITE=%-5s ZFUNC=%-12s  x%u",
			 what,
			 s.zEnable ? "TRUE" : "FALSE",
			 s.zWrite ? "TRUE" : "FALSE",
			 ZFuncName( s.zFunc ), s.count );
	}

	static void Sample( IDirect3DDevice9* device, DepthSample* table, int& count,
						DWORD* outZWrite )
	{
		if ( !device )
			return;

		DWORD zEnable = 0, zWrite = 0, zFunc = 0;
		device->GetRenderState( D3DRS_ZENABLE, &zEnable );
		device->GetRenderState( D3DRS_ZWRITEENABLE, &zWrite );
		device->GetRenderState( D3DRS_ZFUNC, &zFunc );

		if ( outZWrite )
			*outZWrite = zWrite;

		for ( int i = 0; i < count; ++i )
		{
			if ( table[i].zEnable == zEnable && table[i].zWrite == zWrite &&
				 table[i].zFunc == zFunc )
			{
				++table[i].count;
				return;
			}
		}

		if ( count >= kMaxDepthSamples )
			return;

		table[count].zEnable = zEnable;
		table[count].zWrite = zWrite;
		table[count].zFunc = zFunc;
		table[count].count = 1;
		++count;
	}

	// Materials drawn while a COMPRESSED depth range is live. If these are
	// weapon models the diagnosis is settled; if they are something else
	// entirely the fix has to be aimed differently.
	void NoteSlabMaterial()
	{
		const char* name = m_material[0] ? m_material : "(none)";
		for ( int i = 0; i < m_slabCount; ++i )
		{
			int j = 0;
			for ( ; m_slab[i].name[j] && m_slab[i].name[j] == name[j]; ++j )
				;
			if ( m_slab[i].name[j] == name[j] )
			{
				++m_slab[i].writers;
				return;
			}
		}
		if ( m_slabCount >= kMaxMaterials )
			return;
		MenuMaterialUse& u = m_slab[m_slabCount];
		int k = 0;
		for ( ; k < (int)sizeof( u.name ) - 1 && name[k]; ++k )
			u.name[k] = name[k];
		u.name[k] = 0;
		u.writers = 1;
		u.nonWriters = 0;
		++m_slabCount;
	}

	void NoteMaterial( bool writesDepth )
	{
		const char* name = m_material[0] ? m_material : "(none)";

		for ( int i = 0; i < m_materialCount; ++i )
		{
			int j = 0;
			for ( ; m_materials[i].name[j] && m_materials[i].name[j] == name[j]; ++j )
				;
			if ( m_materials[i].name[j] == name[j] )
			{
				if ( writesDepth ) ++m_materials[i].writers;
				else               ++m_materials[i].nonWriters;
				return;
			}
		}

		if ( m_materialCount >= kMaxMaterials )
			return;

		MenuMaterialUse& u = m_materials[m_materialCount];
		int k = 0;
		for ( ; k < (int)sizeof( u.name ) - 1 && name[k]; ++k )
			u.name[k] = name[k];
		u.name[k] = 0;
		u.writers = writesDepth ? 1u : 0u;
		u.nonWriters = writesDepth ? 0u : 1u;
		++m_materialCount;
	}

	void Reset()
	{
		for ( int i = 0; i < kDrawEntryCount; ++i )
		{
			m_total[i] = 0;
			m_menu[i] = 0;
		}
		m_depthCount = 0;
		m_worldCount = 0;
		m_passes = 0;
		m_fixApplied = 0;
		m_materialCount = 0;
		m_orthoCalls = 0;
		m_maxUiPerOrtho = 0;
		m_viewportCount = 0;
		m_viewportCur = -1;
		m_rtCount = 0;
		m_rtCur = -1;
		m_slabCount = 0;
		m_slabWidened = 0;
	}

	bool m_enabled = true;
	bool m_installed = false;
	bool m_menuProj = false;
	int m_fixMode = kFixOff;
	unsigned int m_fixApplied = 0;

	unsigned int m_total[kDrawEntryCount] = {};
	unsigned int m_menu[kDrawEntryCount] = {};
	unsigned int m_passes = 0;
	unsigned int m_worldStride = 0;

	DepthSample m_depth[kMaxDepthSamples] = {};
	int m_depthCount = 0;
	DepthSample m_world[kMaxDepthSamples] = {};
	int m_worldCount = 0;

	char m_material[64] = {};
	bool m_currentIsUi = false;
	unsigned int m_uiDrawIndex = 0;
	unsigned int m_maxUiPerOrtho = 0;
	unsigned int m_orthoCalls = 0;
	float m_biasStep = 8.0f * kDepthUnit;
	MenuMaterialUse m_materials[kMaxMaterials] = {};
	int m_materialCount = 0;

	RenderTargetUse m_rts[kMaxRenderTargets] = {};
	int m_rtCount = 0;
	int m_rtCur = -1;

	ViewportUse m_viewports[kMaxViewports] = {};
	int m_viewportCount = 0;
	int m_viewportCur = -1;
	MenuMaterialUse m_slab[kMaxMaterials] = {};
	int m_slabCount = 0;
	bool m_viewmodelDepth = true;
	unsigned int m_slabWidened = 0;
};

inline DrawProbe& Probe()
{
	static DrawProbe probe;
	return probe;
}

//-----------------------------------------------------------------------------
// Forces one depth render state for the duration of ONE draw, and puts back the
// value it read. See the mode table above for why this is safe where
// OverrideDepthEnable was not.
//
// Constructed and destroyed inside a single detour, so the forced state exists
// only across the call it was set for. Nothing can observe it, nothing can
// inherit it, and there is no path -- exception, early return, or engine
// callback -- on which the restore is skipped.
//-----------------------------------------------------------------------------
class DepthGuard
{
public:
	explicit DepthGuard( IDirect3DDevice9* device ) : m_device( nullptr )
	{
		DrawProbe& probe = Probe();
		const int mode = probe.FixMode();
		if ( mode == kFixOff || !device || !probe.MenuProjection() )
			return;

		// Modes 4 and 5 both let the material decide, because nothing else
		// reliably does.
		if ( ( mode == kFixUiOnly || mode == kFixUiBias ) && !probe.CurrentIsUi() )
			return;

		// Mode 5 leaves the depth TEST alone and moves the depth instead, so
		// window ordering survives and only the tie is removed.
		if ( mode == kFixUiBias )
		{
			const float bias = -(float)probe.NextBiasIndex() * probe.BiasStep();
			DWORD want = 0;
			memcpy( &want, &bias, sizeof( want ) );

			m_state = D3DRS_DEPTHBIAS;
			device->GetRenderState( m_state, &m_saved );
			if ( m_saved == want )
				return;

			device->SetRenderState( m_state, want );
			m_device = device;
			probe.NoteFixApplied();
			return;
		}

		// Mode 3 leaves every depth WRITER alone. A draw that stamps the depth
		// buffer is geometry -- the character model, the world -- and modes 1
		// and 2 broke exactly those by not making this distinction.
		if ( mode == kFixNonWriters )
		{
			DWORD zWrite = TRUE;
			device->GetRenderState( D3DRS_ZWRITEENABLE, &zWrite );
			if ( zWrite )
				return;
		}

		m_state = ( mode == kFixNoZWrite ) ? D3DRS_ZWRITEENABLE : D3DRS_ZENABLE;
		const DWORD want = ( mode == kFixNoZWrite ) ? (DWORD)FALSE : (DWORD)D3DZB_FALSE;

		device->GetRenderState( m_state, &m_saved );

		// Roughly one menu draw in ten is already in the state we want -- see
		// the ZENABLE=FALSE row of the measured table. Touching those would be
		// two redundant device calls for no change at all.
		if ( m_saved == want )
			return;

		device->SetRenderState( m_state, want );
		m_device = device;
		probe.NoteFixApplied();
	}

	~DepthGuard()
	{
		// The value we READ, not a guess at what it should have been. This one
		// line is the whole difference from attempt 1.
		if ( m_device )
			m_device->SetRenderState( m_state, m_saved );
	}

	DepthGuard( const DepthGuard& ) = delete;
	DepthGuard& operator=( const DepthGuard& ) = delete;

private:
	IDirect3DDevice9* m_device;
	D3DRENDERSTATETYPE m_state = D3DRS_ZENABLE;
	DWORD m_saved = 0;
};

//-----------------------------------------------------------------------------
// The detours. Each one counts and calls through -- unconditionally, on every
// path, with no early return that could swallow a draw.
//
// NoteDraw runs BEFORE the guard is constructed, deliberately: the depth tables
// must keep reporting the state the ENGINE chose, not the state we forced, or
// the measurement would just read back our own writes and confirm itself.
//-----------------------------------------------------------------------------

using SetRenderTargetFn = HRESULT( __stdcall* )( IDirect3DDevice9*, DWORD,
												 IDirect3DSurface9* );
using SetViewportFn = HRESULT( __stdcall* )( IDirect3DDevice9*, const D3DVIEWPORT9* );
using DrawPrimitiveFn = HRESULT( __stdcall* )( IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, UINT );
using DrawIndexedPrimitiveFn = HRESULT( __stdcall* )( IDirect3DDevice9*, D3DPRIMITIVETYPE, INT,
													  UINT, UINT, UINT, UINT );
using DrawPrimitiveUPFn = HRESULT( __stdcall* )( IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT,
												 const void*, UINT );
using DrawIndexedPrimitiveUPFn = HRESULT( __stdcall* )( IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT,
														UINT, UINT, const void*, D3DFORMAT,
														const void*, UINT );

inline VTableHook g_setRenderTargetHook;
inline VTableHook g_setViewportHook;
inline VTableHook g_drawPrimHook;
inline VTableHook g_drawIndexedHook;
inline VTableHook g_drawPrimUPHook;
inline VTableHook g_drawIndexedUPHook;

inline SetRenderTargetFn g_originalSetRenderTarget = nullptr;
inline SetViewportFn g_originalSetViewport = nullptr;
inline DrawPrimitiveFn g_originalDrawPrim = nullptr;
inline DrawIndexedPrimitiveFn g_originalDrawIndexed = nullptr;
inline DrawPrimitiveUPFn g_originalDrawPrimUP = nullptr;
inline DrawIndexedPrimitiveUPFn g_originalDrawIndexedUP = nullptr;

// PURELY OBSERVATIONAL. Only target 0 is recorded -- that is the colour buffer
// the scene lands in, and the extra MRT slots are not what this question is
// about.
inline HRESULT __stdcall Detour_SetRenderTarget( IDirect3DDevice9* device, DWORD index,
												 IDirect3DSurface9* surface )
{
	if ( index == 0 && surface )
	{
		D3DSURFACE_DESC d = {};
		if ( SUCCEEDED( surface->GetDesc( &d ) ) )
			Probe().NoteRenderTarget( d.Width, d.Height, (DWORD)d.Format,
									  (DWORD)d.MultiSampleType );
	}
	return g_originalSetRenderTarget( device, index, surface );
}

inline HRESULT __stdcall Detour_SetViewport( IDirect3DDevice9* device,
											 const D3DVIEWPORT9* vp )
{
	if ( !vp )
		return g_originalSetViewport( device, vp );

	// Recorded BEFORE the fix, always. The table has to keep reporting what the
	// ENGINE asked for -- if it logged our substitution the slab would vanish
	// from the log the moment the fix worked, and there would be no way to tell
	// that from the hack having gone away on its own.
	Probe().NoteViewport( vp->MinZ, vp->MaxZ, vp->Width, vp->Height );

	if ( Probe().ViewmodelDepth() && IsViewmodelSlab( *vp ) )
	{
		// A copy. The engine owns that struct and may well read it back.
		D3DVIEWPORT9 full = *vp;
		full.MinZ = 0.0f;
		full.MaxZ = 1.0f;
		Probe().NoteSlabWidened();
		return g_originalSetViewport( device, &full );
	}

	return g_originalSetViewport( device, vp );
}

inline HRESULT __stdcall Detour_DrawPrimitive( IDirect3DDevice9* device, D3DPRIMITIVETYPE type,
											   UINT start, UINT count )
{
	Probe().NoteDraw( device, kDrawPrim );
	DepthGuard guard( device );
	return g_originalDrawPrim( device, type, start, count );
}

inline HRESULT __stdcall Detour_DrawIndexedPrimitive( IDirect3DDevice9* device, D3DPRIMITIVETYPE type,
													  INT baseVertex, UINT minIndex, UINT numVertices,
													  UINT startIndex, UINT primCount )
{
	Probe().NoteDraw( device, kDrawIndexed );
	DepthGuard guard( device );
	return g_originalDrawIndexed( device, type, baseVertex, minIndex, numVertices,
								  startIndex, primCount );
}

inline HRESULT __stdcall Detour_DrawPrimitiveUP( IDirect3DDevice9* device, D3DPRIMITIVETYPE type,
												 UINT primCount, const void* vertices, UINT stride )
{
	Probe().NoteDraw( device, kDrawPrimUP );
	DepthGuard guard( device );
	return g_originalDrawPrimUP( device, type, primCount, vertices, stride );
}

inline HRESULT __stdcall Detour_DrawIndexedPrimitiveUP( IDirect3DDevice9* device, D3DPRIMITIVETYPE type,
													    UINT minIndex, UINT numVertices, UINT primCount,
													    const void* indices, D3DFORMAT indexFormat,
													    const void* vertices, UINT stride )
{
	Probe().NoteDraw( device, kDrawIndexedUP );
	DepthGuard guard( device );
	return g_originalDrawIndexedUP( device, type, minIndex, numVertices, primCount,
									indices, indexFormat, vertices, stride );
}

// Installed off the same throwaway device the Present hook uses -- the vtable is
// per-class and static, so it survives that device's release.
//
// Installed UNCONDITIONALLY, for the reason already recorded for Bind: a hook
// that is only present when a flag is on cannot be turned on mid-session. The
// flag gates the WORK, not the hook.
inline bool DrawProbe::Install( IDirect3DDevice9* device )
{
	if ( !device )
		return false;

	g_originalSetRenderTarget = reinterpret_cast<SetRenderTargetFn>(
		g_setRenderTargetHook.Install( device, kSlotSetRenderTarget,
									   &Detour_SetRenderTarget ) );
	g_originalSetViewport = reinterpret_cast<SetViewportFn>(
		g_setViewportHook.Install( device, kSlotSetViewport, &Detour_SetViewport ) );
	g_originalDrawPrim = reinterpret_cast<DrawPrimitiveFn>(
		g_drawPrimHook.Install( device, kSlotDrawPrimitive, &Detour_DrawPrimitive ) );
	g_originalDrawIndexed = reinterpret_cast<DrawIndexedPrimitiveFn>(
		g_drawIndexedHook.Install( device, kSlotDrawIndexedPrimitive, &Detour_DrawIndexedPrimitive ) );
	g_originalDrawPrimUP = reinterpret_cast<DrawPrimitiveUPFn>(
		g_drawPrimUPHook.Install( device, kSlotDrawPrimitiveUP, &Detour_DrawPrimitiveUP ) );
	g_originalDrawIndexedUP = reinterpret_cast<DrawIndexedPrimitiveUPFn>(
		g_drawIndexedUPHook.Install( device, kSlotDrawIndexedPrimitiveUP, &Detour_DrawIndexedPrimitiveUP ) );

	// All four or none. A partially installed set would count some draws and
	// miss others, and the resulting numbers would look plausible and be wrong.
	if ( !g_originalDrawPrim || !g_originalDrawIndexed || !g_originalDrawPrimUP ||
		 !g_originalDrawIndexedUP || !g_originalSetViewport ||
		 !g_originalSetRenderTarget )
	{
		LogError( "draw probe: failed to hook one or more slots "
				  "(SetViewport=%p DP=%p DIP=%p DPUP=%p DIPUP=%p) -- probe disabled",
				  g_originalSetViewport, g_originalDrawPrim, g_originalDrawIndexed,
				  g_originalDrawPrimUP, g_originalDrawIndexedUP );
		g_setRenderTargetHook.Remove();
		g_setViewportHook.Remove();
		g_drawPrimHook.Remove();
		g_drawIndexedHook.Remove();
		g_drawPrimUPHook.Remove();
		g_drawIndexedUPHook.Remove();
		return false;
	}

	m_installed = true;
	Log( "draw probe: hooked SetViewport(%d) DrawPrimitive(%d) "
		 "DrawIndexedPrimitive(%d) DrawPrimitiveUP(%d) DrawIndexedPrimitiveUP(%d)",
		 kSlotSetViewport, kSlotDrawPrimitive, kSlotDrawIndexedPrimitive,
		 kSlotDrawPrimitiveUP, kSlotDrawIndexedPrimitiveUP );
	return true;
}

} // namespace sinvr
