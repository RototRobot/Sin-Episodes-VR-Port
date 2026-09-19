#pragma once

#include <windows.h>
#include "../vr/vr_backend.h"
#include "../sdk/view_render.h"
#include "menu_panel.h"

struct IDirect3DDevice9;
struct IDirect3DSurface9;

namespace sinvr {

// Per-eye frustum nudge, in tangent units at the near plane. Small numbers:
// 0.01 is roughly half a degree of shift.
struct EyeAdjust
{
	float x = 0.0f;
	float y = 0.0f;
};

struct StereoSettings
{
	bool enabled = true;

	// Numpad live alignment. Off by default -- it steals numpad keys from the
	// game, so it is opt-in for a tuning session.
	bool liveAdjust = false;

	EyeAdjust initialAdjust[2];

	// Replace the game's symmetric monitor projection with the headset's
	// asymmetric per-eye frustum. Separable from the eye offset on purpose: if
	// the projection maths is wrong the view goes strange in an obvious way,
	// and turning this off isolates it from the eye-separation maths.
	bool perEyeProjection = true;

	// Set CViewSetup::fovViewmodel equal to the world fov.
	//
	// ---- THIS IS THE MUZZLE FLASH FIX, AND IT IS NOT ABOUT THE FOV ----------
	//
	// Source draws the viewmodel at a NARROWER fov than the world (54 here
	// against our ~100), so a point measured on the viewmodel does not land
	// where the viewmodel is drawn. c_baseviewmodel.cpp's
	// FormatViewModelAttachment corrects for that, and every first-person
	// muzzle flash goes through it -- the flash is a CLocalSpaceEmitter created
	// with FLE_VIEWMODEL, so each particle is pushed through this on its way to
	// the screen:
	//
	//     factor = tan(fov/2) / tan(fovViewmodel/2)
	//     tmp    = flashWorldPos - viewSetup.origin        <- relative to the EYE
	//     lateral components of tmp *= factor              <- depth is NOT scaled
	//     flashWorldPos = viewSetup.origin + reassembled
	//
	// On a monitor the gun sits near the middle of the screen, so its lateral
	// distance from the eye axis is small, the correction is small, and it is
	// CORRECT -- it is what makes the flash line up with a squashed viewmodel.
	//
	// In VR the gun is out at the player's HAND, a long way off the eye axis,
	// and that distance gets multiplied by 2.34. The flash is thrown away from
	// the gun by a displacement that GROWS with how far the hand is from the
	// eye line and CHANGES as the hand moves -- which is exactly how it was
	// reported: "as I rotate the gun left and right the muzzle flash moves from
	// its relative position to the rifle".
	//
	// We do not need the correction at all: the projection is replaced in every
	// PerspectiveX of the pass, the viewmodel's included, so the viewmodel is
	// NOT drawn squashed here. There is nothing to compensate for. Making the
	// two fovs equal drives factor to exactly 1.0, at which point
	// FormatViewModelAttachment reassembles the vector it was given and is a
	// no-op -- so the flash lands on the attachment.
	//
	// Note this is a fix by ARITHMETIC, not by hooking: the function is a free
	// function in client.dll and not virtual, so intercepting it would need a
	// byte signature. Feeding it operands that make it the identity needs one
	// line, in a struct we already own and write every frame.
	bool matchViewmodelFov = true;

	// ---- GIVE THE VIEWMODEL THE WORLD'S DEPTH ------------------------------
	//
	// Source renders the gun into the FRONT SLICE of the depth buffer --
	// DepthRange(0, 0.1) in CViewRender::DrawViewModels, whose own comment is
	// "HACK HACK: Munge the depth range to prevent view model from poking into
	// walls". Measured on hardware 2026-09-05: one SetViewport per eye pass at
	// 0.000..0.100, carrying exactly two draws, v_magnum and v_hands.
	//
	// So the gun cannot lose a depth test to anything at any distance. It never
	// clips into walls and it is never occluded by them -- one behaviour, both
	// symptoms, and it reads as the gun floating on a plane of its own.
	//
	// That hack solves a FLAT-FPS problem we do not have. There the viewmodel
	// sits a few units from the camera and would clip the near plane; in VR the
	// gun is at real arm's length, well clear of it.
	//
	// This flag is BOTH halves of undoing it, because either alone is wrong:
	//
	//   * CViewSetup's zNearViewmodel/zFarViewmodel are matched to the world's,
	//     here, so both passes build their projection on the SAME depth curve.
	//   * The 0.1 slab is widened back to the full range at SetViewport --
	//     see draw_probe.h.
	//
	// Restoring the range while the curves still differ would put the gun and
	// the world in one buffer measuring depth differently, and a gun at 20
	// units would not compare correctly against geometry at 20 units. One
	// setting, not two -- the same trap menu_depth_bias set.
	bool viewmodelDepth = true;

	// Multiplier on the eye separation. 0 collapses to mono, which is a quick
	// way to tell "stereo is wrong" from "everything is wrong".
	float eyeSeparationScale = 1.0f;

	// Per-eye shift applied to 2D/HUD drawing so screen-space elements converge
	// instead of inheriting the frustum asymmetry. 1.0 converges at infinity,
	// 0 disables the correction entirely.
	float hudConvergence = 1.0f;

	// Keep the body ANCHOR off menus. The anchor replaces the ortho window to
	// put the HUD elsewhere in the view, and a menu moved away from where the
	// cursor hit-tests it is unclickable. This does NOT disable the per-eye
	// convergence shift -- that one leaves the mean screen position alone and
	// is what stops the menu sitting on the player's face. See menuConvergence.
	bool skipHudShiftInMenus = true;

	// Per-eye convergence for menus, separately from the HUD's.
	//
	// A 2D element drawn at the same screen position in both eyes is NOT flat
	// -- each eye's frustum is asymmetric in the opposite direction, so it
	// inherits the full divergence between them (~23 degrees here) and the
	// eyes resolve it as something inches away. 1.0 converges it at infinity,
	// which for a UI panel is comfortable; 0 restores the old unconverged
	// behaviour if this ever turns out to move hit-testing after all.
	float menuConvergence = 1.0f;

	// Shrink menus into a comfortable part of the view.
	//
	// A menu is drawn across the whole backbuffer, and the backbuffer covers the
	// whole RENDERED frustum -- about 124 degrees here against the ~75 a monitor
	// would give it. So the panel is not merely close, it is enormous: it spans
	// more than the player's comfortable field of view and they have to turn
	// their head to read one corner of it.
	//
	// This widens the ortho window, which draws everything in it smaller -- the
	// same mechanism hud_anchor_scale uses. 1.0 is untouched; 0.6 puts the panel
	// in the middle 60 percent.
	//
	// It is ONLY safe because the pointer divides by the same number. Shrinking
	// where a menu draws without moving the cursor to match is precisely what
	// makes buttons unclickable, so menu_scale is read once and handed to both.
	float menuScale = 1.0f;

	// Pin the menu to a WORLD direction instead of to the headset.
	//
	// A 2D overlay is screen-space, so it is welded to the player's face: turn
	// your head and it comes with you, which makes a large panel impossible to
	// take in -- you can never look AT part of it, only past all of it.
	//
	// With this on, the panel is projected per eye from a world position
	// captured when the menu OPENED, so it stays where it was put and the player
	// can look around it. Being projected from a world vector also gives it real
	// per-eye parallax, so it converges at its actual distance instead of at the
	// single compromise distance the flat shift can express.
	//
	// This MOVES the panel, which is the thing that breaks hit-testing -- so it
	// is only safe because the pointer is handed the same placement and inverts
	// it. See MenuPointer::Update.
	// How the flattened menu recovers its layering. See the notes in
	// ApplyMenuPanelProjection: VGUI layers by a z position within one draw
	// call, and collapsing that onto a single plane is what made the load and
	// options screens z-fight while the quit box, drawn alone, did not.
	//
	// menuDepthScale maps one unit of the HUD's own z onto Source units along
	// the panel normal. menuDepthStep additionally separates successive Ortho
	// CALLS. Raise either if the shimmer persists; both are far below what
	// reads as the panel having thickness.
	float menuDepthScale = 0.25f;
	float menuDepthStep = 0.05f;

	bool menuAnchor = true;
	float menuAnchorDistance = 150.0f;   // Source units in front of the head

	// Number of eye passes to trace in detail at startup, to show how many
	// distinct views Source sets up per pass.
	int tracePasses = 2;

	// Relax the engine's frustum culling during eye passes. The engine culls
	// against the game's symmetric FOV, which is narrower than the headset's
	// per-eye frustum, so geometry vanishes at the edges of vision.
	//
	// Master switch. It only reaches client renderables -- IVEngineClient has no
	// equivalent for world brush surfaces, which is why widening the engine's own
	// FOV (engine_fov) matters more than any of this.
	bool relaxCulling = true;

	// The four tests, separately, because they fail differently and forcing all
	// of them is not free.
	//
	// The frustum tests genuinely have to be relaxed: the engine's frustum is
	// symmetric and narrower than the eye's. The other three are visibility
	// rather than clipping -- forcing them true drags in entities from rooms the
	// player cannot see, and those are what show through a hole in the world as
	// "pieces of somewhere else". Once the engine's frustum is wide enough these
	// can be turned off one at a time to find out which are still needed.
	bool relaxFrustum = true;    // CullBox, IsBoxVisible
	bool relaxArea = true;       // DoesBoxTouchAreaFrustum
	bool relaxPvs = true;        // IsBoxInViewCluster
	bool relaxOcclusion = true;  // IsOccluded

	// Drive the client's own CViewSetup instead of patching the matrices it
	// produces. This is the difference between asking the engine for a wider
	// frustum through cvars and simply setting `fov`, and between offsetting
	// every view matrix that goes past (and needing a ring buffer to spot the
	// ones that are re-loads of our own) and setting `origin` once per eye.
	//
	// Falls back to the matrix path automatically if the view cannot be found or
	// fails its sanity check, so a wrong offset degrades rather than corrupts.
	bool ownViewSetup = true;

	// Warn in the heartbeat when a single frame's eye passes exceed this. The
	// driver's own limit is ~2000 ms, at which point it resets the device; the
	// point of a much lower threshold is to see the ramp before the cliff.
	float slowFrameMs = 500.0f;

	// Safety factor on the FOV we ask the engine to cull with. The engine's
	// frustum apex sits at the head centre while each eye renders from ~1.25
	// units to the side, so an exact fit still clips; and a BSP node that only
	// just touches the frustum is cheap to keep and expensive to lose.
	float engineFovMargin = 1.15f;

	// Render both eyes at a single frozen engine time.
	//
	// OFF by default: enabling it dropped the game from 450 frames per 5s to 7
	// and then hung. Something inside Source's render path evidently waits on
	// engine time advancing, so pinning it stalls the frame rather than merely
	// making both eyes consistent. Kept as an option only for experimentation.
	bool freezeTime = false;

	float zNear = 3.0f;    // Source units
	float zFar = 28800.0f;
};

// Renders the scene once per eye and copies each result into its own D3D9
// surface, ready for submission.
//
// The engine is asked to render twice per frame; the per-eye difference is
// applied by intercepting IMaterialSystem's view and projection matrix calls
// while each pass is in flight.
// Source shows the OS cursor for menus and hides it during gameplay, which is
// the only signal that distinguishes "a menu is up" from "in gameplay" --
// IsInGame() cannot, because the main menu runs a background map and reports
// true there.
//
// Sampled once per frame at the top of View_Render and pushed into the
// renderer via SetUiVisible. Deliberately ONE definition and one caller: two
// samplers in a frame can straddle a menu opening and disagree, and the
// camera and the Ortho hook must not take different views of that.
inline bool IsInteractiveUiVisible()
{
	CURSORINFO ci = {};
	ci.cbSize = sizeof( ci );
	if ( !GetCursorInfo( &ci ) )
		return false;
	return ( ci.flags & CURSOR_SHOWING ) != 0;
}

class StereoRenderer
{
public:
	bool Init( void* materialSystem, void* engineClient, IVRBackend* vr,
			   const StereoSettings& settings );
	float HudConvergence() const { return m_settings.hudConvergence; }
	float MenuConvergence() const { return m_settings.menuConvergence; }
	float MenuScale() const { return m_settings.menuScale; }
	float MenuDepthScale() const { return m_settings.menuDepthScale; }
	float MenuDepthStep() const { return m_settings.menuDepthStep; }

	// ---- the menu as a world quad ------------------------------------------
	//
	// Pushed in once per frame. The renderer draws the panel from this and the
	// pointer hit-tests against the SAME struct, which is the whole point -- the
	// previous design had two routes to the panel's position and they could not
	// be made to agree.
	void SetMenuPanel( const MenuPanelGeometry& g, const Vector& f,
					   const Vector& r, const Vector& u )
	{
		m_menuPanel = g;
		m_menuHeadForward = f;
		m_menuHeadRight = r;
		m_menuHeadUp = u;
		m_haveMenuHeadBasis = true;
	}
	void ClearMenuPanel() { m_menuPanel.valid = false; }
	const MenuPanelGeometry& MenuPanel() const { return m_menuPanel; }
	bool MenuHeadBasis( Vector& f, Vector& r, Vector& u ) const
	{
		f = m_menuHeadForward; r = m_menuHeadRight; u = m_menuHeadUp;
		return m_haveMenuHeadBasis;
	}
	// The HUD's real aspect, measured from the ortho window it is drawn with.
	// 0 until a menu has been seen; the panel falls back to square then.
	float MenuHudAspect() const;
	void NoteMenuPanelDraw() { ++m_menuPanelDraws; }
	unsigned int MenuPanelDraws() const { return m_menuPanelDraws; }
	bool MenuAnchored() const { return m_settings.menuAnchor; }
	float MenuAnchorDistance() const { return m_settings.menuAnchorDistance; }
	// Where the menu panel is pinned, as a vector FROM THE HEAD to the panel --
	// the same convention HudAnchor uses, because the projection below subtracts
	// this eye's own offset from it. Head-relative rather than absolute is what
	// makes 6DoF work: lean, and the vector changes, so the panel stays put in
	// the world instead of coming with you.
	void SetMenuAnchor( const Vector& headToPanel, const Vector& f,
						const Vector& r, const Vector& u, bool valid )
	{
		m_menuAnchor = headToPanel;
		m_menuForward = f;
		m_menuRight = r;
		m_menuUp = u;
		m_haveMenuAnchor = valid;
	}
	bool MenuAnchor( Vector& headToPanel, Vector& f, Vector& r, Vector& u ) const
	{
		headToPanel = m_menuAnchor;
		f = m_menuForward;
		r = m_menuRight;
		u = m_menuUp;
		return m_haveMenuAnchor;
	}

	// World-space head displacement for 6DoF, pushed in once per frame from the
	// View_Render hook rather than pulled, so the renderer keeps no dependency
	// on VRCamera. Zero is the safe value and the default, so a frame that
	// arrives before the camera has run simply renders without positional
	// tracking rather than at a stale offset.
	void SetPositionalOffset( const Vector& v ) { m_positionalOffset = v; }
	const Vector& PositionalOffset() const { return m_positionalOffset; }

	// Head angles to render from, when they differ from the engine's view
	// angles. They only differ under `aim_source = controller`: the engine gets
	// the weapon hand's direction so the shot goes where the gun points, and the
	// eyes must keep the head's or the picture would swing with the gun.
	//
	// Pushed in per frame and cleared when not in use, so "no override" is a
	// state rather than a stale value.
	void SetViewAngleOverride( const QAngle& a )
	{
		m_viewAngleOverride = a;
		m_haveViewAngleOverride = true;
	}
	void ClearViewAngleOverride() { m_haveViewAngleOverride = false; }

	// ---- UI mode -----------------------------------------------------------
	//
	// "A menu is up." Source shows the OS cursor for menus and hides it during
	// gameplay, which is the only signal available that does not depend on
	// IsInGame() -- and IsInGame() is useless here, because the main menu runs
	// a BACKGROUND MAP and reports true. That one fact is why every gameplay
	// system was running on the menu: melee, holsters, the arcade reload, the
	// laser dot and the aim decoupling were all live behind a gate that was
	// never closed.
	//
	// Sampled ONCE per frame, at the top of View_Render, and pushed in here --
	// rather than each consumer calling GetCursorInfo for itself. Two samplers
	// would be two sources of truth that disagree on the frame a menu opens.
	void SetUiVisible( bool on ) { m_uiVisible = on; }
	bool UiVisible() const { return m_uiVisible; }
	bool SkipHudShiftInMenus() const { return m_settings.skipHudShiftInMenus; }
	int TracePassesRemaining() const { return m_tracePasses; }
	void Shutdown();

	bool Ready() const { return m_ready; }

	// ---- where the HUD is anchored -------------------------------------
	//
	// Given as the world-space vector from the HEAD to the anchor point,
	// plus the head's basis. NOT as screen tangents, and that difference is
	// the whole reason stereo works now: each eye subtracts its OWN offset
	// before projecting, so the two eyes see the panel from slightly
	// different positions and it converges at its real distance.
	//
	// Passing pre-computed tangents -- the first attempt -- gave both eyes
	// the same screen position, which is a panel at infinity however close
	// it is meant to be. That is what 'convergence breaks up close' was.
	void SetHudAnchor( const Vector& headToAnchor, const Vector& forward,
			  const Vector& right, const Vector& up )
	{
		m_hudAnchor = headToAnchor;
		m_hudForward = forward;
		m_hudRight = right;
		m_hudUp = up;
		m_haveHudAnchor = true;
	}
	void ClearHudAnchor() { m_haveHudAnchor = false; }
	bool HudAnchor( Vector& headToAnchor, Vector& f, Vector& r, Vector& u ) const
	{
		headToAnchor = m_hudAnchor;
		f = m_hudForward;
		r = m_hudRight;
		u = m_hudUp;
		return m_haveHudAnchor;
	}

	void SetHudAnchored( bool on ) { m_hudAnchored = on; }
	// ---- THE ANCHOR IS SUSPENDED ACROSS A TRANSITION --------------------
	//
	// A fade to white goes through the SAME Ortho hook as the HUD, and with
	// hud_anchor = body it is moved and shrunk exactly like the HUD -- so it
	// lands as a panel off to one side instead of covering the view.
	//
	// It cannot be told apart at the hook. Measured on hardware: the fade draws
	// through l=0 t=0 r=2206 b=2160, which is the HUD's own window, identified
	// by its call count DOUBLING during each flash while the other two windows
	// stayed flat. Section 9's proposal to separate them by ortho extents is
	// therefore not available.
	//
	// So this separates them by TIME instead. Every flash in the intro
	// coincides with a level change or an in-map teleport, both of which are
	// now detected reliably and neither of which the HUD cares about. The
	// anchor is dropped for a moment either side, the fade draws in plain
	// screen space and covers the view, and the HUD comes back afterwards.
	//
	// Losing the anchor for a second during a white-out costs nothing: the HUD
	// is not readable through it anyway.
	// How many Ortho calls in one eye pass mean a full-screen wash is up. The
	// HUD alone is ~2; a fade takes it to ~4. 0 disables the detector.
	// Measured, not guessed -- the heartbeat prints the whole distribution.
	// Leave render-to-texture cameras' projections alone. See Detour_PerspectiveX.
	void SetSkipRtProjections( bool on ) { m_skipRtProjections = on; }
	bool SkipRtProjections() const { return m_skipRtProjections; }
	void SetRtAspectTolerance( float t ) { m_rtAspectTolerance = t; }
	float RtAspectTolerance() const { return m_rtAspectTolerance; }

	void SetFullscreenFadeCalls( int n ) { m_fullscreenFadeCalls = n; }
	// Record material names per ortho window. Observational only -- see
	// Detour_Bind. Live-switchable, because it is wanted DURING a flash.
	void SetMaterialTrace( bool on );
	int FullscreenFadeCalls() const { return m_fullscreenFadeCalls; }

	void SuspendHudAnchor( unsigned int untilMs ) { m_hudAnchorSuspendUntilMs = untilMs; }
	bool HudAnchorSuspended( unsigned int nowMs ) const
	{
		return m_hudAnchorSuspendUntilMs != 0 &&
			   (int)( nowMs - m_hudAnchorSuspendUntilMs ) < 0;
	}
	unsigned int HudAnchorSuspendUntil() const { return m_hudAnchorSuspendUntilMs; }

	bool HudAnchored() const { return m_hudAnchored; }
	// Below 1 shrinks the HUD on screen: the ortho window is widened, so what
	// is drawn inside it covers less of the view.
	void SetHudScale( float s ) { m_hudScale = s; }
	float HudScale() const { return m_hudScale; }
	void SetHudClamp( bool on ) { m_hudClamp = on; }
	bool HudClamp() const { return m_hudClamp; }
	void SetHudInvert( bool x, bool y ) { m_hudInvertX = x; m_hudInvertY = y; }
	bool HudInvertX() const { return m_hudInvertX; }
	bool HudInvertY() const { return m_hudInvertY; }
	// Recorded PER EYE, so the two differ by the panel's parallax. The mean of
	// the pair is the monocular centre -- which is what hit-testing wants, and
	// what the pointer inverts. Averaging the renderer's own output is
	// deliberate: re-deriving it would mean reproducing the invert flags and the
	// tangent-space sign conventions in a second place, and the first thing that
	// would go wrong is a menu that is visibly fine and silently unclickable.
	void NoteHudPlacement( float fx, float fy )
	{
		m_hudFxPrev = m_hudFx;
		m_hudFyPrev = m_hudFy;
		m_hudFx = fx;
		m_hudFy = fy;
		++m_hudPlacements;
	}
	// Mean of the last two eye placements. Valid one frame late, which on a
	// panel that is pinned in the world is not observable.
	bool MenuPlacement( float& fx, float& fy ) const
	{
		if ( m_hudPlacements < 2 )
			return false;
		fx = ( m_hudFx + m_hudFxPrev ) * 0.5f;
		fy = ( m_hudFy + m_hudFyPrev ) * 0.5f;
		return true;
	}
	unsigned int HudPlacements() const { return m_hudPlacements; }
	float HudFx() const { return m_hudFx; }
	float HudFy() const { return m_hudFy; }

	// Called from the View_Render detour. `renderOnce` runs the engine's
	// original render for the currently selected eye.
	void RenderBothEyes( void ( *renderOnce )( void* ctx ), void* ctx );

	// Eye surfaces, valid after the first successful frame.
	IDirect3DSurface9* EyeSurface( int eye ) const;
	bool CreateEyeSurfaces( IDirect3DDevice9* device );

	int CurrentEye() const { return m_currentEye; }
	bool InEyePass() const { return m_inEyePass; }

	// Used by the matrix hooks, which run on the render thread mid-pass.
	const EyeParams& CurrentEyeParams() const;
	float EyeSeparationScale() const { return m_settings.eyeSeparationScale; }
	bool UsePerEyeProjection() const { return m_settings.perEyeProjection; }

	// Aspect of the surface actually being rendered into (the game's
	// backbuffer), not the headset's per-eye aspect.
	float RenderAspect() const { return m_renderAspect; }

	const EyeBounds& GetEyeBounds( int eye ) const
	{
		return m_bounds[( eye == kEyeRight ) ? kEyeRight : kEyeLeft];
	}
	void SetEyeBounds( int eye, const EyeBounds& b )
	{
		m_bounds[( eye == kEyeRight ) ? kEyeRight : kEyeLeft] = b;
	}

	// --- live alignment, driven from the numpad ---
	const EyeAdjust& CurrentAdjust() const { return m_adjust[m_currentEye]; }
	void SelectEye( int eye );
	void NudgeSelectedEye( float dx, float dy );
	void ResetSelectedEye();
	void ChangeStep( float factor );
	int SelectedEye() const { return m_selectedEye; }
	float AdjustStep() const { return m_adjustStep; }
	void SetAdjust( int eye, float x, float y );

	// Default-pool surfaces do not survive a device reset. Source resets on
	// level load, so they must be dropped and rebuilt.
	void ReleaseEyeSurfaces();

	bool LiveAdjustEnabled() const { return m_settings.liveAdjust; }

	float ZNear() const { return m_settings.zNear; }
	float ZFar() const { return m_settings.zFar; }
	void SetDepthRange( float zNear, float zFar )
	{
		if ( zNear > 0.0f && zFar > zNear )
		{
			m_settings.zNear = zNear;
			m_settings.zFar = zFar;
		}
	}

	// Diagnostic: how the engine actually sets its matrices.
	void LogMatrixPathStats() const;

	//-------------------------------------------------------------------------
	// The engine's culling frustum.
	//
	// Source builds its world-culling frustum from the view setup's FOV and
	// aspect, by angle -- not from the projection matrix we substitute. So the
	// matrix override changes the picture and changes nothing about what the
	// engine decides to draw. PerspectiveX is the only place those two numbers
	// pass through our hands, which makes it the sole readback for "what is the
	// engine actually culling with right now".
	//-------------------------------------------------------------------------
	// `firstOfPass` marks the main scene's projection. Source sets that one first
	// (the 3D skybox shares its FOV) and the viewmodel's last, with its own much
	// narrower viewmodel_fov -- 54 in this engine. Recording whichever came last
	// made the log report 54 as "the engine's fov" while the world was correctly
	// culling at 123, which reads as a total failure when nothing is wrong.
	void NoteEngineProjection( float fovX, float aspect, bool firstOfPass );
	float ObservedEngineFovX() const { return m_observedFovX; }
	float ObservedEngineAspect() const { return m_observedAspect; }

	// Half-tangents the rendered frustum really reaches, taken over both eyes and
	// including the extension to the render target's aspect. False if the eye
	// parameters are not up yet.
	bool RenderedHalfTangents( float& outX, float& outY ) const;

	// A WORLD point -> a pixel in this eye's backbuffer image.
	//
	// Uses the SAME head basis, eye offset and projection that the menu panel is
	// drawn with, so a point on the panel lands exactly where the panel drew it
	// -- in both eyes, with the right parallax, by construction rather than by
	// tuning. That property is the whole reason this lives here next to
	// ApplyMenuPanelProjection instead of being re-derived by the caller: every
	// earlier menu round failed because the picture and the position came
	// through different maths.
	//
	// `eye` selects the eye; the head basis is the one pushed in with
	// SetMenuPanel. Returns false if the basis is not up, the eye params are
	// invalid, or the point is at/behind the eye plane.
	bool ProjectWorldToBackbuffer( const Vector& world, int eye,
								   float& outX, float& outY ) const;

	// Backbuffer size in pixels, as measured from the real surface at eye-surface
	// creation. 0 until then. NOT the window client size -- those differ here
	// (1800x2124 against 1200x1416) and using the wrong one puts the cursor in
	// the wrong place. Same trap MenuHudAspect exists for.
	void BackbufferSize( unsigned int& w, unsigned int& h ) const;

	// Smallest engine FOV whose frustum contains everything we render, with
	// engineFovMargin applied. 0 if it cannot be computed yet.
	float RequiredEngineFovX() const;

	// The same, from the headset's raw frustum alone, for the frames before the
	// render target's aspect is known. Omits the aspect extension, so it can be
	// slightly narrow -- but it is derived from whatever headset is attached
	// rather than being a constant picked on one particular device.
	float FallbackEngineFovX() const;

	// One line saying whether the engine is currently culling away parts of the
	// image we are rendering, and by how much.
	void LogFrustumCoverage() const;

	// What the eye passes actually cost. Resets its interval accumulators, so it
	// reports the worst frame *since the last call* rather than since startup --
	// an average hides exactly the frames that matter here.
	void LogFrameTiming() const;

	//-------------------------------------------------------------------------
	// View ownership.
	//
	// `viewRenderFn` is IBaseClientDLL::View_Render, which we already hook; the
	// `view` global is read out of its code rather than hard-coded. Everything
	// is sanity-checked against values we can predict before anything is
	// written, because this hands us a raw pointer into the client's state.
	//-------------------------------------------------------------------------
	bool BindViewSetup( void* viewRenderFn );

	// Re-attempt a bind that was too early. See RetryViewBind -- before the
	// first map the view is legitimately 0x0, and treating that as a layout
	// mismatch drops the mod onto the matrix path for the whole run.
	void RetryViewBind();

	// Called immediately before each eye pass, with the eye index.
	//
	// This exists for anything that has to be RE-SUBMITTED per pass rather
	// than once per frame. Debug overlays are the case that forced it: the
	// engine appears to draw its overlay list during the first pass and clear
	// it, so a mark submitted once before both passes reaches ONE EYE ONLY --
	// which looks fine while the head is still and swims as soon as it moves.
	typedef void ( *PreEyePassFn )( int eye );
	void SetPreEyePass( PreEyePassFn fn ) { m_preEyePass = fn; }

	// Runs at the END of an eye pass, after the engine has drawn everything --
	// world, HUD and VGUI -- and BEFORE the backbuffer is copied into that eye's
	// surface. So anything drawn here sits on top of the finished image and is
	// submitted with it.
	//
	// That is the one place a menu cursor can go. Debug overlays are drawn by
	// the engine BEFORE VGUI, and not at all while a pause menu is up, so no
	// overlay-based cursor can ever appear over a menu. This hook sidesteps the
	// engine's draw order entirely rather than fighting it.
	typedef void ( *PostEyePassFn )( int eye );
	void SetPostEyePass( PostEyePassFn fn ) { m_postEyePass = fn; }

	// Pretend the view is not ready for the first N bind attempts.
	//
	// The retry path exists because the real bind can happen before the
	// first map, when the view is legitimately 0x0 -- but that is a RACE,
	// and the run that confirmed the fix bound on attempt 1 because the map
	// happened to be loaded. Waiting for a race to recur is not a test.
	// This forces the failure so the recovery can be exercised on demand.
	void SetViewBindForcedRetries( int n ) { m_viewBindForce = n; }
	bool OwnsViewSetup() const { return m_viewSetup != nullptr && m_settings.ownViewSetup; }
	const CViewSetup* ViewSetup() const { return m_viewSetup; }

private:
	float m_renderAspect = 0.0f;
	EyeBounds m_bounds[2];
	EyeAdjust m_adjust[2];
	int m_selectedEye = 0;
	float m_adjustStep = 0.01f;
	int m_tracePasses = 0;

public:

	unsigned long long EyeFrameCount() const { return m_eyeFrames; }

public:
	// The MEAN of the two eyes' rendered frusta, in tangent units.
	//
	// This is the mapping between a backbuffer pixel and a direction AS THE
	// PLAYER PERCEIVES IT. A 2D element sits at the same pixel in both eyes, and
	// each eye's frustum is asymmetric the opposite way, so the direction the
	// player fuses is the average of the two -- which is what a pointer has to
	// aim along.
	//
	// NOT the same thing as RenderedHalfTangents, and confusing the two is a
	// real error rather than a rounding one: that returns max(|l|,|r|) over both
	// eyes, i.e. how far the frustum reaches at its OUTER edge, which for an
	// asymmetric frustum is not half its span. Normalising a tangent by it is
	// near enough at the centre of the screen and progressively wrong towards
	// the edges -- so a pointer built on it hits big central buttons and misses
	// small ones, which is exactly how it presented.
	bool MeanFrustumExtents( float& l, float& r, float& t, float& b ) const
	{
		float ll[2], rr[2], tt[2], bb[2];
		int n = 0;
		for ( int eye = 0; eye < kEyeCount; ++eye )
			if ( EyeFrustumExtents( eye, ll[n], rr[n], tt[n], bb[n] ) )
				++n;
		if ( n == 0 )
			return false;
		l = r = t = b = 0.0f;
		for ( int i = 0; i < n; ++i ) { l += ll[i]; r += rr[i]; t += tt[i]; b += bb[i]; }
		l /= n; r /= n; t /= n; b /= n;
		return ( r - l ) > 0.0001f && ( b - t ) != 0.0f;
	}

private:
	bool InstallMatrixHooks( void* materialSystem );
	bool InstallTimeHook( void* engineClient );
	bool InstallCullingHooks( void* engineClient );

	// One eye's frustum as actually rendered, after the extension to the render
	// target's aspect. False if that eye's parameters are not up yet.
	bool EyeFrustumExtents( int eye, float& l, float& r, float& t, float& b ) const;

	// Point the client's view setup at one eye, starting from `base` every time.
	void ApplyEyeToViewSetup( CViewSetup& vs, const CViewSetup& base, int eye ) const;

	// The FOV to write into CViewSetup so that, *after* the engine's own
	// widescreen rescale, the culling frustum ends up at RequiredEngineFovX().
	float EngineCullFovPreScale() const;

	// Aspect the engine culls with: whatever it last handed to PerspectiveX,
	// falling back to the render target's until we have seen one.
	float EngineCullAspect() const;

	void* m_engineClient = nullptr;

	// Written from the render thread inside the PerspectiveX hook, read by the
	// heartbeat thread. Plain floats: a torn read costs a wrong diagnostic line,
	// never a wrong render.
	float m_observedFovX = 0.0f;      // main scene -- the one that culls the world
	float m_observedAspect = 0.0f;
	float m_observedFovMin = 0.0f;    // across the whole pass, for cross-checking
	float m_observedFovMax = 0.0f;

	// The client's live view. Null until BindViewSetup succeeds; never written
	// unless OwnsViewSetup().
	Vector m_positionalOffset = { 0.0f, 0.0f, 0.0f };
	QAngle m_viewAngleOverride = { 0.0f, 0.0f, 0.0f };
	// The last fov pair actually written into CViewSetup. Mutable because
	// ApplyEyeToViewSetup is const and this is a record of what it did, not
	// state it steers by. Feeds the heartbeat's muzzle-flash factor.
	mutable float m_lastViewFov = 0.0f;
	mutable float m_lastViewFovViewmodel = 0.0f;

	bool m_haveViewAngleOverride = false;
	CViewSetup* m_viewSetup = nullptr;
	void* m_pendingViewRender = nullptr;
	PreEyePassFn m_preEyePass = nullptr;
	PostEyePassFn m_postEyePass = nullptr;
	unsigned int m_viewBindAttempts = 0;
	int m_viewBindForce = 0;
	bool m_viewFindWarned = false;
	bool m_viewBindGaveUp = false;
	void* m_viewRender = nullptr;

	// engine->GetScreenAspectRatio(), needed to undo the widescreen rescale the
	// engine applies to CViewSetup::fov inside CViewRender::Render.
	float m_screenAspect = 4.0f / 3.0f;

	// Cost of the eye passes, in wall-clock milliseconds on the render thread.
	// That includes time blocked inside DXVK when the GPU is behind, which is
	// what makes it a usable proxy for "how close is this frame to a TDR".
	// Mutable so the heartbeat can roll the interval over from a const method.
	mutable double m_eyeMsTotal = 0.0;
	mutable double m_eyeMsMax = 0.0;
	mutable unsigned long long m_eyeMsSamples = 0;
	mutable unsigned long m_slowFrames = 0;
	mutable double m_worstEverMs = 0.0;

	IVRBackend* m_vr = nullptr;
	StereoSettings m_settings;
	bool m_ready = false;
	Vector m_hudAnchor = { 0.0f, 0.0f, 0.0f };
	Vector m_hudForward = { 1.0f, 0.0f, 0.0f };
	Vector m_hudRight = { 0.0f, -1.0f, 0.0f };
	Vector m_hudUp = { 0.0f, 0.0f, 1.0f };
	bool m_haveHudAnchor = false;
	bool m_hudAnchored = false;
	unsigned int m_hudAnchorSuspendUntilMs = 0;
	int m_fullscreenFadeCalls = 3;
	bool m_skipRtProjections = true;
	float m_rtAspectTolerance = 0.02f;
	bool m_hudInvertX = false;
	bool m_hudInvertY = false;
	float m_hudFx = 0.5f;
	float m_hudFxPrev = 0.5f;
	float m_hudFyPrev = 0.5f;
	float m_hudFy = 0.5f;
	unsigned int m_hudPlacements = 0;
	bool m_hudClamp = true;
	float m_hudScale = 1.0f;
	bool m_inEyePass = false;

	// A menu is up. See SetUiVisible.
	bool m_uiVisible = false;
	Vector m_menuAnchor = { 0.0f, 0.0f, 0.0f };
	Vector m_menuForward = { 1.0f, 0.0f, 0.0f };
	Vector m_menuRight = { 0.0f, -1.0f, 0.0f };
	Vector m_menuUp = { 0.0f, 0.0f, 1.0f };
	bool m_haveMenuAnchor = false;
	MenuPanelGeometry m_menuPanel;
	Vector m_menuHeadForward = { 1.0f, 0.0f, 0.0f };
	Vector m_menuHeadRight = { 0.0f, -1.0f, 0.0f };
	Vector m_menuHeadUp = { 0.0f, 0.0f, 1.0f };
	bool m_haveMenuHeadBasis = false;
	unsigned int m_menuPanelDraws = 0;
	int m_currentEye = 0;
	unsigned long long m_eyeFrames = 0;
};

StereoRenderer& Stereo();

} // namespace sinvr
