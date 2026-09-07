// Stereo rendering.
//
// Source 2004 has no concept of stereo, so we make the engine render the scene
// twice and change what it sees between passes. The interception points are
// IMaterialSystem's matrix calls, whose vtable slots were established by
// disassembling a live process -- see source_interfaces.h, and note that the
// slot numbering is NOT the SDK's.
//
// Per frame:
//     for eye in {left, right}:
//         select eye
//         run the engine's original View_Render     <- matrix hooks fire here
//         copy the backbuffer into that eye's surface
//     submit both surfaces
//
// Copying out of the backbuffer rather than binding an ITexture render target
// avoids needing IMaterialSystem::CreateRenderTargetTexture, whose slot is still
// unidentified. It costs two full-screen blits per frame.

#include "stereo.h"

#include <d3d9.h>
#include <math.h>
#include <string.h>

#include "../sdk/source_interfaces.h"
#include "../hooks/vtable_hook.h"
#include "../../common/log.h"
#include "d3d9_present_hook.h"
#include "draw_probe.h"

namespace sinvr {
namespace {

StereoRenderer g_stereo;

VTableHook g_matrixModeHook;
VTableHook g_loadMatrixHook;
VTableHook g_perspectiveHook;

void* g_materialSystem = nullptr;

// Which matrix the material system is currently addressing. MATERIAL_VIEW = 0,
// MATERIAL_PROJECTION = 1 (imaterialsystem.h:68-71).
int g_matrixMode = -1;

// Guards against our own LoadMatrix calls re-entering the hook.
bool g_inOurLoad = false;

// Double-buffered: the compositor reads asynchronously after Submit returns, so
// writing straight back into the surface it is still sampling stalls it. The
// symptom was a progressive frame-rate collapse over tens of seconds ending in a
// GPU timeout, rather than an immediate failure.
constexpr int kBufferCount = 2;
IDirect3DSurface9* g_eyeSurface[kBufferCount][kEyeCount] = {};
uint32_t g_eyeWidth = 0;
uint32_t g_eyeHeight = 0;
int g_bufferIndex = 0;

//-----------------------------------------------------------------------------
// Source's VMatrix: 4x4 floats, row-major storage, column-vector convention
// (v' = M * v). Row 0 is the X axis of the transform.
//-----------------------------------------------------------------------------
struct VMatrix
{
	float m[4][4];
};

using MatrixModeFn = void( __fastcall* )( void* thisptr, void* edx, int mode );
using LoadMatrixFn = void( __fastcall* )( void* thisptr, void* edx, const VMatrix* matrix );
using PerspectiveXFn = void( __fastcall* )( void* thisptr, void* edx,
											double fovx, double aspect,
											double zNear, double zFar );
using OrthoFn = void( __fastcall* )( void* thisptr, void* edx,
									 double left, double top, double right,
									 double bottom, double zNear, double zFar );
using EngineTimeFn = float( __fastcall* )( void* thisptr, void* edx );
using BindFn = void( __fastcall* )( void* thisptr, void* edx,
									void* material, void* proxyData );

MatrixModeFn g_originalMatrixMode = nullptr;
LoadMatrixFn g_originalLoadMatrix = nullptr;
PerspectiveXFn g_originalPerspectiveX = nullptr;
OrthoFn g_originalOrtho = nullptr;
EngineTimeFn g_originalEngineTime = nullptr;
BindFn g_originalBind = nullptr;

VTableHook g_orthoHook;
VTableHook g_bindHook;
VTableHook g_engineTimeHook;
VTableHook g_cullBoxHook;
VTableHook g_isBoxVisibleHook;
VTableHook g_isBoxInViewClusterHook;
VTableHook g_areaFrustumHook;
VTableHook g_isOccludedHook;

using CullBoxFn = bool( __fastcall* )( void* thisptr, void* edx, const void* mins, const void* maxs );
using IsBoxVisibleFn = int( __fastcall* )( void* thisptr, void* edx, const void* mins, const void* maxs );
using AreaFrustumFn = bool( __fastcall* )( void* thisptr, void* edx, const void* mins,
										   const void* maxs, int area );
using IsOccludedFn = bool( __fastcall* )( void* thisptr, void* edx, const void* mins, const void* maxs );

CullBoxFn g_originalCullBox = nullptr;
IsBoxVisibleFn g_originalIsBoxVisible = nullptr;
IsBoxVisibleFn g_originalIsBoxInViewCluster = nullptr;
AreaFrustumFn g_originalAreaFrustum = nullptr;
IsOccludedFn g_originalIsOccluded = nullptr;

// Tested and rescued are counted separately for every test. Only counting
// rescues made "cluster=0 occluder=0" unreadable: it could equally mean the test
// never rejects anything or that the slot we hooked is never called at all, and
// those call for opposite next steps.
volatile long g_countAreaTested = 0;
volatile long g_countAreaRescued = 0;
volatile long g_countClusterTested = 0;
volatile long g_countClusterRescued = 0;
volatile long g_countOccluderTested = 0;
volatile long g_countOccluderRescued = 0;
volatile long g_countVisTested = 0;
volatile long g_countVisRescued = 0;

bool g_cullOverride = false;
bool g_relaxFrustum = true;
bool g_relaxArea = true;
bool g_relaxPvs = true;
bool g_relaxOcclusion = true;
volatile long g_countCullRescued = 0;
volatile long g_countCullTested = 0;

// Frozen for the duration of a frame's eye passes.
float g_frozenTime = 0.0f;
bool g_timeFrozen = false;

// True while an interactive menu is up. Sampled once per frame rather than per
// Ortho call, which happens several times a pass.
bool g_cursorVisible = false;

// ---- distinct ortho windows seen while a menu is up -------------------------
//
// The question this answers: the main menu's cursor lines up and the SUBMENUS'
// do not. If every panel is drawn through one ortho window, our transform hits
// them all identically and a per-panel misalignment is impossible -- so a second
// window in this list IS the explanation, and its numbers say what to do about
// it. If there is only ever one, the cause is elsewhere and this rules out a
// whole family of theories in one run.
struct OrthoWindow
{
	double l, t, r, b;
	// zNear/zFar are the ONLY other data this hook receives. Recorded because
	// the extents turned out not to separate the HUD from a full-screen fade --
	// both draw through l=0 t=0 r=2206 b=2160 -- so if anything at this hook can
	// tell them apart it is these two, and they cost nothing to capture.
	double zn, zf;
	unsigned int count;
};
OrthoWindow g_menuOrtho[8] = {};
int g_menuOrthoCount = 0;

// Buckets for the per-eye-pass Ortho call count. The last one is "this many or
// more", so a runaway count is visible rather than lost.
const int kOrthoHistogramBuckets = 12;

// ---- EVERY DISTINCT PROJECTION THE ENGINE ASKS FOR IN AN EYE PASS -------
//
// Several views go through PerspectiveX per pass and they are not the same
// thing: the world, the viewmodel at its own FOV, and any render-to-texture
// camera the map has -- an in-game monitor, a security screen, a mirror.
//
// Until now the only thing recorded was the FIRST one, on the assumption it is
// the world. That assumption is already known to be wrong in some areas (the
// `stereo frustum: SHORT` false alarms), and it made RT cameras invisible --
// which is why the monitor's broken stereo went unexplained.
struct ProjRecord
{
	float fov, aspect;
	unsigned int count, kept;
};
ProjRecord g_projSeen[10] = {};
int g_projSeenCount = 0;
volatile long g_countRtProjectionsKept = 0;

void NoteProjection( float fov, float aspect, bool kept )
{
	for ( int i = 0; i < g_projSeenCount; ++i )
	{
		if ( g_projSeen[i].fov == fov && g_projSeen[i].aspect == aspect )
		{
			if ( kept ) ++g_projSeen[i].kept; else ++g_projSeen[i].count;
			return;
		}
	}
	if ( g_projSeenCount >= 10 )
		return;
	g_projSeen[g_projSeenCount].fov = fov;
	g_projSeen[g_projSeenCount].aspect = aspect;
	g_projSeen[g_projSeenCount].count = kept ? 0u : 1u;
	g_projSeen[g_projSeenCount].kept = kept ? 1u : 0u;
	++g_projSeenCount;
}

// Reset at the top of every eye pass. See the depth stagger in
// ApplyMenuPanelProjection -- both eyes must start from the same index or
// the two would disagree about how far away each panel is, which reads as
// the panel swimming in depth.
int g_menuOrthoDepthIndex = 0;
// step and scale are configurable -- see MenuDepthStep / MenuDepthScale

// The HUD's own shape, from the ortho window it is actually drawn with.
//
// Measured rather than assumed, and the difference matters: this game draws its
// menus in an 1800x2124 space while the window client is 1200x1416. Taking the
// aspect from the window would stretch the panel by the ratio between them.
//
// The widest recorded window is the real one -- the others are a normalised
// 0..1 full-screen quad and the same space offset by half a pixel.
float MenuHudAspectFromOrtho()
{
	double bestSpan = 0.0, aspect = 0.0;
	for ( int i = 0; i < g_menuOrthoCount; ++i )
	{
		const double w = g_menuOrtho[i].r - g_menuOrtho[i].l;
		const double h = g_menuOrtho[i].b - g_menuOrtho[i].t;
		if ( w > bestSpan && w > 1.0 && h != 0.0 )
		{
			bestSpan = w;
			aspect = ( h < 0.0 ? -h : h ) / w;
		}
	}
	return (float)aspect;
}

// ---- THE SAME RECORD, FOR GAMEPLAY --------------------------------------
//
// The menu recorder above is gated on the cursor being visible, so it can only
// ever see menus -- which means the one thing it structurally CANNOT see is a
// full-screen effect during play. That is the case now being diagnosed: with
// hud_anchor = body the Ortho hook rewrites the projection for EVERYTHING drawn
// through it, so a fade to white is moved and shrunk along with the HUD and
// appears as a panel off to one side instead of covering the view. Section 9 of
// the handover predicted this exactly and left the fix undone for want of a
// real measurement. This is that measurement.
//
// The proposed fix is to pass FULL-SCREEN draws through untouched, told apart
// from HUD draws by their ortho extents -- but that assumption is unverified,
// and the menu data already hints it may be wrong: menus are drawn in a
// full-screen 0,0..2206,2160 window too. If the HUD and the fade share extents,
// extents cannot separate them and the fix has to be something else.
//
// The CALL COUNT is what will settle it. A HUD element is drawn every frame and
// will show hundreds of thousands of calls; a fade lasts under a second and
// will show a small burst. Two windows with wildly different counts are two
// different things even if their numbers look similar.
OrthoWindow g_gameOrtho[8] = {};
int g_gameOrthoCount = 0;

// Per-eye-pass Ortho call count, and what it says about a fade being up.
// See the block at the top of the eye pass for the reasoning.
volatile long g_orthoCallsThisPass = 0;
bool g_fadeActive = false;
unsigned int g_fadeHoldUntilMs = 0;
unsigned int g_fadeEpisodes = 0;
// Held briefly past the last elevated pass so the count dipping for a frame
// mid-fade cannot flicker the anchor back on.
const unsigned int kFadeHoldMs = 250;
unsigned int g_orthoPassHistogram[kOrthoHistogramBuckets] = {};

//-----------------------------------------------------------------------------
// WHAT is being drawn, by material name, and in WHICH ortho window.
//
// Ortho says which coordinate space a draw is in. It cannot say what the draw
// IS -- and the fade and the HUD were measured to be byte-identical in every
// argument Ortho receives, extents and z alike, so that avenue is closed.
// IMaterialSystem::Bind is the next thing the engine calls, and it carries the
// one piece of information that must differ: the material.
//
// Keyed on (material, ortho window) because the pairing is the answer. "This
// material appears in the HUD's window" is the statement we need; a material
// list on its own would not separate the HUD from the world.
//
// DELTAS ARE THE POINT, not totals. A cumulative count cannot answer "is it
// happening now" -- that is the lesson the fade hunt already paid for once, and
// it is why each entry keeps the count as of the previous heartbeat. A fade
// material sits at delta 0 during normal play and spikes for the one heartbeat
// that contains the flash, which is exactly the signature that identified the
// fade's ortho window in the first place.
struct MaterialUse
{
	char name[48];
	int window;              // index into g_gameOrtho, -1 if unknown
	unsigned int count;
	unsigned int lastCount;  // as of the previous heartbeat
	bool wasIdle;            // delta was zero at the previous heartbeat
};

// Large, because the filtering ideas all failed and we now record everything.
// A saturated table silently drops exactly the rare material we are hunting,
// which is the worst possible failure for this diagnostic -- so the cap is set
// well above the ~100 distinct materials a scene was measured to use.
const int kMaterialUseMax = 256;
MaterialUse g_materialUse[kMaterialUseMax] = {};
int g_materialUseCount = 0;
bool g_materialTrace = false;

// ---- DEPTH-TEST OVERRIDE: DEAD END, PROVED. DO NOT REBUILD IT. -------------
//
// Forcing the depth test off via IMaterialSystem::OverrideDepthEnable (slot 126)
// DOES fix the save/load shimmer -- VGUI is a painter, coplanar draws are normal
// for it, and removing depth testing restores the ordering it was designed
// around. It worked in the main menu on the first try.
//
// It cannot be made safe, and this is not a matter of more careful clearing.
// **OverrideDepthEnable HAS NO GETTER.** There is no GetOverrideDepthEnable
// anywhere in the interface, so the current value cannot be read. "Restoring" it
// means calling (false, false), which does not put back a previous value -- it
// forcibly DISABLES the override, whatever the engine had set for its own
// purposes.
//
// Measured, with a detector built specifically to settle it: 8047 set / 8047
// cleared and ZERO leaks at pass start, on both eyes, WHILE the corruption was
// visible. Our bookkeeping was perfect. It was also irrelevant, because it
// tracks only OUR calls and cannot see the engine's.
//
// That also explains the eye asymmetry that started this hunt: the engine's own
// use of the global is not identical across the two passes, so which eye gets
// corrupted depends on where our calls land relative to its.
//
// THE GENERAL RULE, which is worth more than this feature: do not save/restore a
// global you do not own unless you can READ IT BACK. Balanced set/clear counts
// prove nothing about a value someone else is also writing.
//
// The route that remains is a per-draw tie-breaker -- local, self-limiting, and
// it cannot leak. Bind (slot 49) is already hooked and proved.

// ---- DEPTH-TEST OVERRIDE: BUILT, TESTED ON HARDWARE, REVERTED -------------
//
// Do not rebuild this without reading the write-up in HANDOVER.md first.
//
// The idea was sound and the diagnosis behind it still stands: VGUI is a
// painter, coplanar draws are normal in its world, and giving its output a
// perspective projection turns depth testing ON and makes every overlap fight.
// Forcing the depth test off via IMaterialSystem::OverrideDepthEnable (slot 126,
// proved and still documented in source_interfaces.h) fixed the save/load
// shimmer in the main menu on the first try.
//
// It was reverted because the override is GLOBAL RENDER STATE and we cannot say
// when the menu's draws end. It could be turned on at the menu's Ortho call and
// never reliably turned off: clearing at the next Ortho, then additionally at
// both ends of every eye pass, still leaked into the LEFT EYE's 3D render --
// world geometry through walls, and the main menu's character model rendering
// inside-out. Each fix revealed another path.
//
// The structural problem, and why more clears will not help: we control when
// the menu STARTS drawing and have no hook for when it STOPS, so any state we
// enable there outlives its scope by construction.
//
// The remaining route is a per-draw tie-breaker instead of a global state
// change -- local, self-limiting, and it cannot leak. IMaterialSystem::Bind
// (slot 49) is already hooked and proved.
// Which ortho window the most recent Ortho call selected. Draws belong to it
// until the next Ortho call, which is exactly how the engine's 2D state works.
int g_currentOrthoWindow = -1;
// Set when a material name comes back as something that is not a string, so the
// failure is reported once rather than filling the log or being mistaken for
// "the fade uses no material".
unsigned int g_materialNameFailures = 0;

// Is the ortho window currently in force the one the HUD and the fade share?
//
// TWO gates were tried before this and both failed, which is worth recording
// because both look obviously correct:
//
//   1. "record everything" -- Ortho sets the window and it STAYS set, so every
//      3D draw afterwards was attributed to whichever window was last. The
//      table filled with blade's head and particle sprites.
//   2. "record only between Ortho and the next PerspectiveX" -- the engine does
//      not always leave 2D through PerspectiveX; a projection can arrive by
//      LoadMatrix instead. The flag stuck ON and the world materials came back.
//
// So gate on the WINDOW, which is the one thing already measured and trusted.
// The handover proved the fade draws in `l=0 t=0 r=W b=H zn=-99999 zf=99999` --
// the HUD's own window -- and that is the only place either of them can be.
// The 1x1 window and the zn=-1 VGUI window are neither, and are excluded by
// exactly the two properties that distinguish them.
bool CurrentWindowIsHudShaped()
{
	const int w = g_currentOrthoWindow;
	if ( w < 0 || w >= g_gameOrthoCount )
		return false;
	const OrthoWindow& o = g_gameOrtho[w];
	return ( o.r - o.l ) > 2.0 && ( o.b - o.t ) > 2.0 && o.zn <= -9999.0;
}

// IMaterial::GetName() is vtable slot 0 on that interface. NOT assumed: the
// result is validated as printable ASCII below before it is ever used as a
// string, because a wrong slot here would hand us a garbage pointer and the
// first thing we would do with it is read it as text.
const char* MaterialName( void* material )
{
	if ( !material )
		return nullptr;

	const char* name = nullptr;
	__try
	{
		void** vt = *reinterpret_cast<void***>( material );
		if ( !vt )
			return nullptr;
		using GetNameFn = const char*( __thiscall* )( void* );
		name = reinterpret_cast<GetNameFn>( vt[0] )( material );
		if ( !name )
			return nullptr;

		// Must look like a material path: printable, non-empty, bounded.
		for ( int i = 0; i < 64; ++i )
		{
			const unsigned char c = (unsigned char)name[i];
			if ( c == 0 )
				return i > 0 ? name : nullptr;
			if ( c < 0x20 || c > 0x7E )
			{
				++g_materialNameFailures;
				return nullptr;
			}
		}
		++g_materialNameFailures;   // unterminated within 64 bytes
		return nullptr;
	}
	__except ( EXCEPTION_EXECUTE_HANDLER )
	{
		++g_materialNameFailures;
		return nullptr;
	}
}

void NoteMaterial( const char* name, int window )
{
	if ( !name )
		return;

	for ( int i = 0; i < g_materialUseCount; ++i )
	{
		if ( g_materialUse[i].window == window &&
			 g_materialUse[i].name[0] == name[0] &&
			 strcmp( g_materialUse[i].name, name ) == 0 )
		{
			++g_materialUse[i].count;
			return;
		}
	}
	if ( g_materialUseCount >= kMaterialUseMax )
		return;

	MaterialUse& e = g_materialUse[g_materialUseCount];
	strncpy_s( e.name, sizeof( e.name ), name, _TRUNCATE );
	e.window = window;
	e.count = 1;
	e.lastCount = 0;
	e.wasIdle = true;
	++g_materialUseCount;
}

void NoteGameOrtho( double l, double t, double r, double b, double zn, double zf )
{
	for ( int i = 0; i < g_gameOrthoCount; ++i )
	{
		if ( g_gameOrtho[i].l == l && g_gameOrtho[i].t == t &&
			 g_gameOrtho[i].r == r && g_gameOrtho[i].b == b &&
			 g_gameOrtho[i].zn == zn && g_gameOrtho[i].zf == zf )
		{
			++g_gameOrtho[i].count;
			// Draws belong to this window until the next Ortho call.
			g_currentOrthoWindow = i;
			return;
		}
	}
	if ( g_gameOrthoCount >= 8 )
		return;
	g_gameOrtho[g_gameOrthoCount].l = l;
	g_gameOrtho[g_gameOrthoCount].t = t;
	g_gameOrtho[g_gameOrthoCount].r = r;
	g_gameOrtho[g_gameOrthoCount].b = b;
	g_gameOrtho[g_gameOrthoCount].zn = zn;
	g_gameOrtho[g_gameOrthoCount].zf = zf;
	g_gameOrtho[g_gameOrthoCount].count = 1;
	g_currentOrthoWindow = g_gameOrthoCount;
	++g_gameOrthoCount;
}

void NoteMenuOrtho( double l, double t, double r, double b )
{
	for ( int i = 0; i < g_menuOrthoCount; ++i )
	{
		if ( g_menuOrtho[i].l == l && g_menuOrtho[i].t == t &&
			 g_menuOrtho[i].r == r && g_menuOrtho[i].b == b )
		{
			++g_menuOrtho[i].count;
			return;
		}
	}
	if ( g_menuOrthoCount >= 8 )
		return;
	g_menuOrtho[g_menuOrthoCount].l = l;
	g_menuOrtho[g_menuOrthoCount].t = t;
	g_menuOrtho[g_menuOrthoCount].r = r;
	g_menuOrtho[g_menuOrthoCount].b = b;
	g_menuOrtho[g_menuOrthoCount].count = 1;
	++g_menuOrthoCount;
}
volatile long g_countOrthoSkippedForMenu = 0;
volatile long g_countViewOffsetApplied = 0;
volatile long g_countViewOffsetReentrant = 0;
volatile long g_countFreshViewsLeft = 0;
volatile long g_countFreshViewsRight = 0;

//-----------------------------------------------------------------------------
// Pre-multiply a view matrix by a translation expressed in view space.
//
//     newView = T * oldView
//
// The eye offset has to be applied *after* the world-to-view rotation, which
// for a column-vector convention means pre-multiplying. Because a translation
// only touches the last column, this reduces to adding the offset scaled by the
// existing rows -- no full matrix multiply needed.
//-----------------------------------------------------------------------------
void TranslateViewSpace( VMatrix& view, float right, float up, float back )
{
	view.m[0][3] += right;
	view.m[1][3] += up;
	view.m[2][3] += back;
}

//-----------------------------------------------------------------------------
// Source reads the current view matrix back and re-loads it several times per
// pass. Each re-load already carries the eye offset we applied on the way in,
// so offsetting again accumulates: a trace showed the view translation marching
// 250.77 -> 252.02 -> 253.27 ... in steps of exactly the 1.25-unit eye offset,
// seven times per eye, in opposite directions per eye. That is what made
// geometry drift apart over the frame -- worst on whatever is drawn last, such
// as the viewmodel.
//
// So remember what we produced, and when the engine hands the same matrix back,
// pass it straight through.
//-----------------------------------------------------------------------------
// A ring, not a single slot. Source interleaves several views per pass (main
// scene, skybox, viewmodel), so remembering only the most recent result means an
// earlier view coming back around looks "fresh" and gets offset a second time.
// The left/right imbalance in the logs -- 12763 vs 4298 fresh views -- is that
// mis-detection, not genuinely mono content.
constexpr int kOffsetHistory = 8;
VMatrix g_offsetResults[kOffsetHistory] = {};
int g_offsetResultCount = 0;
int g_offsetResultNext = 0;

bool MatricesNearlyEqual( const VMatrix& a, const VMatrix& b )
{
	// Loose enough to survive a round trip through the engine's own storage,
	// tight enough that a genuinely different view never matches.
	constexpr float kEpsilon = 0.001f;
	for ( int row = 0; row < 4; ++row )
	{
		for ( int col = 0; col < 4; ++col )
		{
			float diff = a.m[row][col] - b.m[row][col];
			if ( diff > kEpsilon || diff < -kEpsilon )
				return false;
		}
	}
	return true;
}

bool IsOneOfOurResults( const VMatrix& m )
{
	for ( int i = 0; i < g_offsetResultCount; ++i )
	{
		if ( MatricesNearlyEqual( m, g_offsetResults[i] ) )
			return true;
	}
	return false;
}

void RememberOffsetResult( const VMatrix& m )
{
	g_offsetResults[g_offsetResultNext] = m;
	g_offsetResultNext = ( g_offsetResultNext + 1 ) % kOffsetHistory;
	if ( g_offsetResultCount < kOffsetHistory )
		++g_offsetResultCount;
}

void ClearOffsetHistory()
{
	g_offsetResultCount = 0;
	g_offsetResultNext = 0;
}

//-----------------------------------------------------------------------------
// Asymmetric perspective projection, D3D convention (depth 0..1, looking down
// -Z). Built from OpenVR's raw frustum tangents rather than a symmetric FOV,
// which is the whole point: each eye sees further toward its own side, and a
// symmetric monitor projection gets that wrong in a way that reads as bad scale.
//-----------------------------------------------------------------------------
// Widen (or heighten) the eye's frustum so it fits the render target's aspect
// while still containing the eye's real view, and report the sub-rectangle that
// maps back to it.
//
// Squashing a 16:9 render into a near-square eye texture scales the frustum's
// *asymmetry* by the wrong factor, and because each eye is asymmetric in the
// opposite direction, the two images shift apart -- double vision. Extending
// instead of scaling keeps every angle exact; the bounds then crop away the
// extra. Correct at any game resolution, the only cost being wasted pixels, so a
// game aspect nearer the headset's wastes fewer.
void ComputeEyeFrustum( const EyeParams& e, float renderAspect,
						const EyeAdjust& adjust,
						float& outLeft, float& outRight, float& outTop, float& outBottom,
						EyeBounds& outBounds )
{
	// Live nudges shift the whole frustum; used to converge the eyes by hand.
	const float l = e.tanLeft + adjust.x;
	const float r = e.tanRight + adjust.x;
	const float t = e.tanTop + adjust.y;
	const float b = e.tanBottom + adjust.y;

	outLeft = l;
	outRight = r;
	outTop = t;
	outBottom = b;
	outBounds = EyeBounds();

	if ( renderAspect <= 0.01f )
		return;

	const float halfW = ( r - l ) * 0.5f;
	const float halfH = ( b - t ) * 0.5f;
	const float centreX = ( l + r ) * 0.5f;
	const float centreY = ( t + b ) * 0.5f;
	const float eyeAspect = ( halfH > 0.0f ) ? ( halfW / halfH ) : renderAspect;

	if ( renderAspect > eyeAspect )
	{
		// Render is wider than the eye needs: extend horizontally, crop in u.
		const float newHalfW = halfH * renderAspect;
		outLeft = centreX - newHalfW;
		outRight = centreX + newHalfW;

		const float span = outRight - outLeft;
		outBounds.uMin = ( l - outLeft ) / span;
		outBounds.uMax = ( r - outLeft ) / span;
	}
	else
	{
		// Render is taller: extend vertically, crop in v.
		const float newHalfH = halfW / renderAspect;
		outTop = centreY - newHalfH;
		outBottom = centreY + newHalfH;

		const float span = outBottom - outTop;
		outBounds.vMin = ( t - outTop ) / span;
		outBounds.vMax = ( b - outTop ) / span;
	}
}

void BuildProjection( float l, float r, float t, float b, float zNear, float zFar,
					  VMatrix& out )
{
	const float idx = 1.0f / ( r - l );
	const float idy = 1.0f / ( b - t );
	const float sx = r + l;
	const float sy = b + t;

	memset( &out, 0, sizeof( out ) );
	out.m[0][0] = 2.0f * idx;
	out.m[0][2] = sx * idx;
	out.m[1][1] = 2.0f * idy;
	out.m[1][2] = sy * idy;
	out.m[2][2] = zFar / ( zNear - zFar );
	out.m[2][3] = zFar * zNear / ( zNear - zFar );
	out.m[3][2] = -1.0f;
}

//-----------------------------------------------------------------------------
// Hooks
//-----------------------------------------------------------------------------
void __fastcall Detour_MatrixMode( void* thisptr, void* edx, int mode )
{
	g_matrixMode = mode;
	g_originalMatrixMode( thisptr, edx, mode );
}

// Call counters. Which of these move tells us how the engine actually sets its
// matrices, which is not something the SDK header can answer.
volatile long g_countLoadView = 0;
volatile long g_countLoadProj = 0;
volatile long g_countPerspectiveX = 0;
volatile long g_countProjReplaced = 0;
// Reset at the top of every eye pass, so the main scene's projection can be told
// apart from the viewmodel's and any render-to-texture views.
volatile long g_perspectiveThisPass = 0;
volatile long g_countOrtho = 0;
volatile long g_countOrthoShifted = 0;
volatile long g_countTimeFrozen = 0;

// Builds this eye's projection and records the crop that goes with it.
// The menu on a world quad: hand the engine P_eye x A instead of an ortho.
//
// A maps HUD pixel coordinates onto the quad, expressed in this eye's VIEW
// space. Source's 2D path leaves the VIEW and MODEL matrices as identity, so
// folding everything into the projection is enough -- no extra hooks, and
// nothing captured from a previous call that could go stale.
//
// Returns false if the geometry is not up yet, in which case the caller falls
// back to the ordinary ortho path rather than drawing nothing.
bool ApplyMenuPanelProjection( void* thisptr, double left, double top,
							   double right, double bottom )
{
	const EyeParams& eye = g_stereo.CurrentEyeParams();
	if ( !eye.valid )
		return false;

	const MenuPanelGeometry& g = g_stereo.MenuPanel();
	if ( !g.valid )
		return false;

	Vector hf, hr, hu;
	if ( !g_stereo.MenuHeadBasis( hf, hr, hu ) )
		return false;

	const double spanX = right - left;
	const double spanY = bottom - top;
	if ( fabs( spanX ) < 0.0001 || fabs( spanY ) < 0.0001 )
		return false;

	// This eye's world offset from the head. Same expression the anchored HUD
	// uses: OpenVR's eye space and Source's view space share a basis.
	const float ex = hr.x * eye.offsetRight + hu.x * eye.offsetUp - hf.x * eye.offsetBack;
	const float ey = hr.y * eye.offsetRight + hu.y * eye.offsetUp - hf.y * eye.offsetBack;
	const float ez = hr.z * eye.offsetRight + hu.z * eye.offsetUp - hf.z * eye.offsetBack;

	// Quad centre relative to THIS eye, in world axes...
	const Vector c = { g.centre.x - g.headWorld.x - ex,
					   g.centre.y - g.headWorld.y - ey,
					   g.centre.z - g.headWorld.z - ez };

	// ...then into view space. Viewer looks down -Z, hence the negated forward.
	// ---- DEPTH STAGGER: draw order becomes depth order --------------------
	//
	// A 2D system layers by draw order with depth testing off. Give it a real
	// perspective and every panel lands on ONE plane at ONE depth, so overlapping
	// panels are coplanar and z-fight -- which is why the load and options
	// screens shimmered while the quit box, sitting alone, did not.
	//
	// Each Ortho call within a pass nudges its plane a hair closer to the eye, so
	// a panel drawn later is nearer and wins cleanly. The step is tiny and the
	// count per pass is small (about eighteen), so the total is under a unit at
	// 150 -- far too little to see, far more than enough to separate depths.
	const float nudge = (float)g_menuOrthoDepthIndex * g_stereo.MenuDepthStep();
	++g_menuOrthoDepthIndex;

	const Vector cn = { c.x - g.normal.x * nudge,
						c.y - g.normal.y * nudge,
						c.z - g.normal.z * nudge };

	const float cx = MenuDot( cn, hr );
	const float cy = MenuDot( cn, hu );
	const float cz = -MenuDot( cn, hf );

	// The quad's own axes in view space.
	const float rx = MenuDot( g.right, hr ), ry = MenuDot( g.right, hu ), rz = -MenuDot( g.right, hf );
	const float ux = MenuDot( g.up, hr ),    uy = MenuDot( g.up, hu ),    uz = -MenuDot( g.up, hf );

	// HUD pixel -> offset along the quad. HUD y grows DOWNWARD, so the up axis
	// takes a negative scale.
	const double su = ( g.halfWidth * 2.0 ) / spanX;
	const double ou = -( left / spanX + 0.5 ) * ( g.halfWidth * 2.0 );
	const double sv = -( g.halfHeight * 2.0 ) / spanY;
	const double ov = ( top / spanY + 0.5 ) * ( g.halfHeight * 2.0 );

	// ---- KEEP THE HUD'S OWN Z ---------------------------------------------
	//
	// The first attempt staggered each Ortho CALL a little nearer, on the theory
	// that panels are drawn in separate calls. They are not -- the submenus kept
	// z-fighting, which says the main menu and the dialog over it share a call
	// and were therefore still landing on one plane.
	//
	// VGUI layers within a call using a Z POSITION, and this matrix was throwing
	// it away: column 2 was zeroed, so every layer collapsed onto the same
	// depth. Mapping it back onto the panel's normal restores the separation the
	// 2D system already had, using the engine's own layering rather than a
	// guess about draw order.
	//
	// The scale is tiny on purpose: enough to separate depths, far too little to
	// see as the panel having thickness.
	//
	// ---- AND THE SIGN MATTERS, WHICH COST A ROUND TO NOTICE ----------------
	//
	// VGUI's convention, from the SDK, is not a guess:
	//
	//     Panel.h:  void SetZPos(int z);
	//               // sets Z ordering - lower numbers are always behind higher z's
	//     Menu.cpp: SetZPos(1);            <- a dropdown puts itself in FRONT
	//     viewport: SetZPos(-20);          // "send it to the back"
	//
	// So HIGHER z is NEARER THE VIEWER. `g.normal` points out of the quad
	// TOWARD the head, so mapping z onto +normal is what puts a higher z nearer.
	//
	// It was mapped onto -normal, which put every higher z FURTHER AWAY. That
	// still separated the depths, so it fixed the z-fighting it was written for
	// and looked like a success -- and it silently inverted the layering of
	// everything that overlaps. The symptoms took a while to connect:
	//
	//   * the "Advanced" dialog opened BEHIND the options dialog it came from
	//   * dropdown menus never appeared at all, because Menu's z of 1 put it
	//     behind the very panel it drops out of
	//
	// A separation bug and an ordering bug wearing one sign. Fixing the sign
	// keeps the separation exactly as it was -- only the direction changes.
	const float nz = g_stereo.MenuDepthScale();
	const float zx = g.normal.x * nz, zy = g.normal.y * nz, zz = g.normal.z * nz;
	const float zvx = MenuDot( Vector{ zx, zy, zz }, hr );
	const float zvy = MenuDot( Vector{ zx, zy, zz }, hu );
	const float zvz = -MenuDot( Vector{ zx, zy, zz }, hf );

	VMatrix a;
	memset( &a, 0, sizeof( a ) );
	a.m[0][0] = (float)( rx * su ); a.m[0][1] = (float)( ux * sv ); a.m[0][2] = zvx;
	a.m[1][0] = (float)( ry * su ); a.m[1][1] = (float)( uy * sv ); a.m[1][2] = zvy;
	a.m[2][0] = (float)( rz * su ); a.m[2][1] = (float)( uz * sv ); a.m[2][2] = zvz;
	a.m[0][3] = (float)( cx + rx * ou + ux * ov );
	a.m[1][3] = (float)( cy + ry * ou + uy * ov );
	a.m[2][3] = (float)( cz + rz * ou + uz * ov );
	a.m[3][3] = 1.0f;

	float fl, fr, ft, fb;
	EyeBounds bounds;
	ComputeEyeFrustum( eye, g_stereo.RenderAspect(), g_stereo.CurrentAdjust(),
					   fl, fr, ft, fb, bounds );

	VMatrix proj;
	BuildProjection( fl, fr, ft, fb, g_stereo.ZNear(), g_stereo.ZFar(), proj );

	// P x A, row-major, column vectors.
	VMatrix out;
	for ( int i = 0; i < 4; ++i )
		for ( int j = 0; j < 4; ++j )
			out.m[i][j] = proj.m[i][0] * a.m[0][j] + proj.m[i][1] * a.m[1][j] +
						  proj.m[i][2] * a.m[2][j] + proj.m[i][3] * a.m[3][j];

	g_inOurLoad = true;
	VCall<matsys_slot::kMatrixMode, void, int>( thisptr, 1 /* MATERIAL_PROJECTION */ );
	VCall<matsys_slot::kLoadMatrixVMatrix, void, const VMatrix*>( thisptr, &out );
	g_inOurLoad = false;

	g_stereo.NoteMenuPanelDraw();

	// Everything drawn from here until the next projection change is the menu,
	// on our quad. That claim is exactly what the probe is measuring.
	Probe().SetMenuProjection( true );
	return true;
}

void ApplyEyeProjection( void* thisptr )
{
	const EyeParams& eye = g_stereo.CurrentEyeParams();
	if ( !eye.valid )
		return;

	float l, r, t, b;
	EyeBounds bounds;
	ComputeEyeFrustum( eye, g_stereo.RenderAspect(), g_stereo.CurrentAdjust(),
					   l, r, t, b, bounds );
	g_stereo.SetEyeBounds( g_stereo.CurrentEye(), bounds );

	VMatrix proj;
	BuildProjection( l, r, t, b, g_stereo.ZNear(), g_stereo.ZFar(), proj );

	g_inOurLoad = true;
	VCall<matsys_slot::kMatrixMode, void, int>( thisptr, 1 /* MATERIAL_PROJECTION */ );
	VCall<matsys_slot::kLoadMatrixVMatrix, void, const VMatrix*>( thisptr, &proj );
	g_inOurLoad = false;

	Probe().SetMenuProjection( false );
	InterlockedIncrement( &g_countProjReplaced );
}

void __fastcall Detour_LoadMatrix( void* thisptr, void* edx, const VMatrix* matrix )
{
	if ( g_inOurLoad || !matrix || !g_stereo.InEyePass() )
	{
		g_originalLoadMatrix( thisptr, edx, matrix );
		return;
	}

	// The projection can arrive either through PerspectiveX or as a matrix
	// loaded directly. Source 2004 uses the latter, so both are handled --
	// missing this path is why per-eye projection silently did nothing.
	if ( g_matrixMode == 1 /* MATERIAL_PROJECTION */ )
	{
		InterlockedIncrement( &g_countLoadProj );
		Probe().SetMenuProjection( false );

		if ( g_stereo.UsePerEyeProjection() && g_stereo.CurrentEyeParams().valid )
		{
			ApplyEyeProjection( thisptr );
			return; // ours replaces theirs
		}

		g_originalLoadMatrix( thisptr, edx, matrix );
		return;
	}

	if ( g_matrixMode != 0 /* MATERIAL_VIEW */ )
	{
		g_originalLoadMatrix( thisptr, edx, matrix );
		return;
	}

	InterlockedIncrement( &g_countLoadView );

	// When we own CViewSetup, the eye offset is already in the origin the engine
	// built this matrix from -- and in every derived view too. Offsetting here as
	// well would double it.
	//
	// This is the whole point of the change. The code below can only guess which
	// matrices are "ours coming back" by remembering the last eight results, and
	// this engine sets up more distinct views per pass than that: a run showed
	// 9.04 offsets applied per frame against an expected 2.00, and left/right
	// fresh-view counts of 13208 against 1642. Every one of that excess is a view
	// offset a second time, which is exactly the wrong disparity on whatever it
	// drew.
	if ( g_stereo.OwnsViewSetup() )
	{
		if ( g_stereo.TracePassesRemaining() > 0 )
			Log( "  [trace] eye %d LoadMatrix(VIEW) in=(%.2f %.2f %.2f)  <- view owned, "
				 "passing through", g_stereo.CurrentEye(),
				 matrix->m[0][3], matrix->m[1][3], matrix->m[2][3] );

		g_originalLoadMatrix( thisptr, edx, matrix );
		return;
	}

	// One-shot structural trace. Source sets up several views per eye pass --
	// main scene, 3D skybox, viewmodel and so on -- and we currently apply the
	// same eye offset to every one of them. Which is almost certainly wrong for
	// some: the 3D skybox in particular is rendered at a reduced world scale, so
	// a full-size offset over-separates it. Logging the sequence shows how many
	// distinct views there really are and what distinguishes them.
	if ( g_stereo.TracePassesRemaining() > 0 )
		Log( "  [trace] eye %d LoadMatrix(VIEW) in=(%.2f %.2f %.2f)%s",
			 g_stereo.CurrentEye(), matrix->m[0][3], matrix->m[1][3], matrix->m[2][3],
			 IsOneOfOurResults( *matrix ) ? "  <- re-load of ours, passing through"
										  : "  <- fresh view, offsetting" );

	const EyeParams& eye = g_stereo.CurrentEyeParams();
	if ( !eye.valid )
	{
		g_originalLoadMatrix( thisptr, edx, matrix );
		return;
	}

	// Already ours -- the engine is re-loading what we handed it. Offsetting a
	// second time is what caused the per-frame drift.
	if ( IsOneOfOurResults( *matrix ) )
	{
		InterlockedIncrement( &g_countViewOffsetReentrant );
		g_originalLoadMatrix( thisptr, edx, matrix );
		return;
	}

	VMatrix view = *matrix;
	TranslateViewSpace( view,
						-eye.offsetRight * g_stereo.EyeSeparationScale(),
						-eye.offsetUp * g_stereo.EyeSeparationScale(),
						-eye.offsetBack * g_stereo.EyeSeparationScale() );

	RememberOffsetResult( view );
	InterlockedIncrement( &g_countViewOffsetApplied );

	// Per-eye tally. A view set up in one eye's pass but not the other renders
	// identical content to both, so it has no depth -- the likely explanation
	// for objects that look flat. The first trace showed exactly that: eye 0 had
	// a view at (1.41, 10.35, -851.87) that eye 1 never received.
	InterlockedIncrement( g_stereo.CurrentEye() == kEyeLeft ? &g_countFreshViewsLeft
														   : &g_countFreshViewsRight );

	g_inOurLoad = true;
	g_originalLoadMatrix( thisptr, edx, &view );
	g_inOurLoad = false;
}

void __fastcall Detour_PerspectiveX( void* thisptr, void* edx, double fovx,
									 double aspect, double zNear, double zFar )
{
	InterlockedIncrement( &g_countPerspectiveX );

	// Record before deciding what to do with it. These two numbers are the whole
	// of what the engine culls the world with, and this is the only place they
	// are visible to us -- the frustum itself is built by angle inside
	// engine.dll and never passes through the material system.
	//
	// Several views go through here per pass and they do NOT share a FOV: the
	// main scene comes first, the viewmodel last at viewmodel_fov. Only the first
	// says anything about world culling.
	if ( g_stereo.InEyePass() )
	{
		const long ordinal = InterlockedIncrement( &g_perspectiveThisPass );
		g_stereo.NoteEngineProjection( (float)fovx, (float)aspect, ordinal == 1 );
	}

	if ( g_stereo.TracePassesRemaining() > 0 && g_stereo.InEyePass() )
		Log( "  [trace] eye %d PerspectiveX fov=%.2f aspect=%.3f near=%.2f far=%.1f",
			 g_stereo.CurrentEye(), fovx, aspect, zNear, zFar );

	if ( g_stereo.InEyePass() )
		NoteProjection( (float)fovx, (float)aspect, false );

	// Ahead of every early return below, not beside ApplyEyeProjection: the RT
	// camera and invalid-eye paths pass through without ever reaching it.
	Probe().SetMenuProjection( false );

	if ( !g_stereo.InEyePass() || !g_stereo.UsePerEyeProjection() )
	{
		g_originalPerspectiveX( thisptr, edx, fovx, aspect, zNear, zFar );
		return;
	}

	// ---- RENDER-TO-TEXTURE CAMERAS MUST KEEP THEIR OWN PROJECTION --------
	//
	// The in-game monitors (the one on the car dashboard in the intro) are
	// rendered by an RT camera into a texture, and that texture is then mapped
	// onto a surface in the world. The STEREO comes from the surface -- both
	// eyes look at the same quad from slightly different places, which is what
	// makes it sit in the room.
	//
	// The texture itself must be IDENTICAL in both eyes. It is a picture on a
	// screen, not a window: a real monitor does not show each of your eyes a
	// different image.
	//
	// Replacing the projection here gave the RT camera THIS EYE's asymmetric
	// frustum, so each eye rendered the monitor's contents from a different
	// virtual viewpoint. The two pictures then disagreed by an amount unrelated
	// to the surface's depth, and the eyes could not fuse them -- reported as
	// "the monitor gives two different images per eye that don't converge",
	// and visible in a side-by-side capture as the subject framed differently
	// in each eye.
	//
	// Told apart by ASPECT. The world view and the viewmodel both render into
	// the headset's target and share its aspect; an RT camera renders into its
	// own texture and carries that texture's aspect instead. The viewmodel must
	// keep being replaced -- viewmodel_fov_match depends on it -- so this
	// cannot key off FOV, which the viewmodel also differs on.
	if ( g_stereo.SkipRtProjections() )
	{
		const float renderAspect = g_stereo.RenderAspect();
		if ( renderAspect > 0.0f &&
			 fabs( aspect - (double)renderAspect ) > (double)g_stereo.RtAspectTolerance() )
		{
			InterlockedIncrement( &g_countRtProjectionsKept );
			NoteProjection( (float)fovx, (float)aspect, true );
			g_originalPerspectiveX( thisptr, edx, fovx, aspect, zNear, zFar );
			return;
		}
	}

	const EyeParams& eye = g_stereo.CurrentEyeParams();
	if ( !eye.valid )
	{
		g_originalPerspectiveX( thisptr, edx, fovx, aspect, zNear, zFar );
		return;
	}

	// Remember the depth range the engine wanted, so the replacement projection
	// keeps the game's own near/far rather than an invented one.
	g_stereo.SetDepthRange( (float)zNear, (float)zFar );

	// Replace rather than adjust: PerspectiveX can only express a symmetric
	// frustum, so the headset's projection is loaded directly instead.
	ApplyEyeProjection( thisptr );
}

//-----------------------------------------------------------------------------
// The HUD is drawn in 2D at the same texture position for both eyes, but each
// eye's frustum is asymmetric in the opposite direction -- its centre sits at
// (l+r)/2, roughly +-0.2 in tangent units on this headset, about 11 degrees
// each way. A 2D overlay therefore inherits the full ~23 degrees of divergence
// and the crosshair appears in a different place per eye.
//
// Correct 3D geometry depends on that asymmetry, so the frustum must not
// change; instead the orthographic window is shifted by the opposite amount, so
// screen-space elements land on the same world direction in both eyes.
//-----------------------------------------------------------------------------
// IMaterialSystem::Bind, slot 49 -- see source_interfaces.h for how that index
// was proved.
//
// PURELY OBSERVATIONAL. It records a name and calls through, always, on every
// path. Nothing downstream sees a different material, and the trace being off
// costs one bool test. This is deliberate: the hook exists to answer a question
// about what the engine draws, and a diagnostic that changes what the engine
// draws cannot answer it.
void __fastcall Detour_Bind( void* thisptr, void* edx, void* material,
							 void* proxyData )
{
	// Only while an eye pass is drawing the game's own 2D, and never for our
	// own matrix loads -- the same two guards the Ortho recorder uses, so the
	// two diagnostics are describing the same draws.
	// NO window or 2D filter. Three attempts to narrow this failed, the last
	// one hardest: Bind is called only when the material CHANGES, so a draw in
	// the HUD's window using an already-bound material makes no call at all.
	// The ortho window at Bind time is therefore NOT the window the draw
	// belongs to, and pairing them recorded zero HUD materials.
	//
	// So record everything and let the DELTA do the separating, which is what
	// identified the fade's ortho window in the first place. The window field is
	// kept as a hint only -- do not trust it as attribution.
	// Classified on EVERY bind, not only inside an eye pass: a bind that
	// happened outside one would otherwise leave a stale name standing, and the
	// depth fix would classify the next draw from the wrong material.
	const char* name = g_inOurLoad ? nullptr : MaterialName( material );
	if ( !g_inOurLoad )
		Probe().SetCurrentMaterial( name );

	if ( g_materialTrace && g_stereo.InEyePass() && !g_inOurLoad )
		NoteMaterial( name, g_currentOrthoWindow );

	if ( g_originalBind )
		g_originalBind( thisptr, edx, material, proxyData );
}

void __fastcall Detour_Ortho( void* thisptr, void* edx, double left, double top,
							  double right, double bottom, double zNear, double zFar )
{
	InterlockedIncrement( &g_countOrtho );

	// Cleared for EVERY ortho, before deciding which kind this is. A menu ortho
	// followed by a HUD ortho would otherwise leave the flag set across draws
	// that are not the menu's.
	Probe().SetMenuProjection( false );
	Probe().NoteOrthoCall();

	// ---- MENUS GET CONVERGENCE, BUT NEVER THE ANCHOR ----------------------
	//
	// These two were skipped together for menus and they are not the same risk.
	//
	// The ANCHOR genuinely moves the panel: it replaces the ortho window outright
	// to put the HUD somewhere else in the player's view. Do that to a menu and
	// its buttons stop being where the cursor hit-tests them -- unclickable, and
	// that is what SkipHudShiftInMenus was written to prevent.
	//
	// The CONVERGENCE shift does not move it. It shifts each eye by the OPPOSITE
	// amount -- this eye's own frustum centre -- so the two eyes land on the same
	// world direction. The mean screen position is unchanged, which is exactly
	// the property hit-testing depends on. Skipping it is what left the menu
	// drawn at identical screen coordinates in both eyes: with per-eye asymmetric
	// frustums that is not "flat", it is ~23 degrees of divergence, which the
	// eyes resolve as an object right in front of the face. That is the
	// "too close to be visible" report.
	//
	// So: converge menus, never anchor them.
	const bool inMenu = g_cursorVisible;
	if ( !inMenu )
	{
		NoteGameOrtho( left, top, right, bottom, zNear, zFar );
		if ( g_stereo.InEyePass() && !g_inOurLoad )
			InterlockedIncrement( &g_orthoCallsThisPass );
	}
	if ( inMenu )
	{
		NoteMenuOrtho( left, top, right, bottom );

		// The menu as real geometry. Replaces the orthographic projection
		// outright, so every 2D draw the menu makes lands on a quad standing in
		// the world -- with real perspective, real per-eye parallax and a real
		// distance. Falls back to the flat path if the quad is not up yet.
		if ( g_stereo.InEyePass() && !g_inOurLoad &&
			 ApplyMenuPanelProjection( thisptr, left, top, right, bottom ) )
			return;
	}
	const float convergence = inMenu ? g_stereo.MenuConvergence()
									 : g_stereo.HudConvergence();

	if ( g_inOurLoad || !g_stereo.InEyePass() || convergence == 0.0f )
	{
		if ( inMenu )
			InterlockedIncrement( &g_countOrthoSkippedForMenu );
		g_originalOrtho( thisptr, edx, left, top, right, bottom, zNear, zFar );
		return;
	}

	const EyeParams& eye = g_stereo.CurrentEyeParams();
	if ( !eye.valid )
	{
		g_originalOrtho( thisptr, edx, left, top, right, bottom, zNear, zFar );
		return;
	}

	// Where this eye's view centre sits, as a fraction of the rendered frustum.
	float l, r, t, b;
	EyeBounds bounds;
	ComputeEyeFrustum( eye, g_stereo.RenderAspect(), g_stereo.CurrentAdjust(),
					   l, r, t, b, bounds );

	const float centreX = ( eye.tanLeft + eye.tanRight ) * 0.5f;
	const float span = r - l;
	if ( span <= 0.0001f )
	{
		g_originalOrtho( thisptr, edx, left, top, right, bottom, zNear, zFar );
		return;
	}

	double shiftX = ( centreX / span ) * ( right - left ) * convergence;
	double shiftY = 0.0;

	// ---- the anchored HUD ------------------------------------------------
	//
	// Projected HERE, per eye, from a WORLD VECTOR -- not handed in as a screen
	// position. Subtracting THIS eye's own offset before projecting is what
	// gives the panel real parallax, so it converges at the distance it is
	// actually placed at.
	//
	// The first version passed pre-computed tangents, which gave both eyes the
	// same screen position -- a panel at infinity however near it was meant to
	// be. That is exactly what 'the convergence breaks when it gets close' was.
	Vector anchor, hf, hr, hu;
	// Which anchor applies, if any.
	//
	// On a menu it is the MENU's own world anchor -- captured when the menu
	// opened and held there, so the panel stays where the player was facing
	// instead of riding their head. The body HUD anchor is never used on a
	// menu: it follows head yaw by design, which is the behaviour being
	// escaped, and it is placed for a HUD rather than for something to read.
	//
	// Both routes move the panel, which is what breaks hit-testing -- and both
	// are safe only because the pointer is handed the resulting placement and
	// inverts it. NoteHudPlacement below is what carries it across.
	bool haveAnchor = false;
	if ( inMenu )
	{
		if ( g_stereo.MenuAnchored() && g_stereo.MenuAnchor( anchor, hf, hr, hu ) )
			haveAnchor = true;
	}
	else if ( g_stereo.HudAnchored() && !g_fadeActive &&
			  !g_stereo.HudAnchorSuspended( GetTickCount() ) &&
			  g_stereo.HudAnchor( anchor, hf, hr, hu ) )
	{
		haveAnchor = true;
	}

	if ( haveAnchor )
	{
		// Head -> this eye, in world space. OpenVR's eye space is +X right,
		// +Y up, +Z back -- the same basis Source uses for view space.
		const float ex = hr.x * eye.offsetRight + hu.x * eye.offsetUp - hf.x * eye.offsetBack;
		const float ey = hr.y * eye.offsetRight + hu.y * eye.offsetUp - hf.y * eye.offsetBack;
		const float ez = hr.z * eye.offsetRight + hu.z * eye.offsetUp - hf.z * eye.offsetBack;

		const float ax = anchor.x - ex;
		const float ay = anchor.y - ey;
		const float az = anchor.z - ez;

		const float depth = ax * hf.x + ay * hf.y + az * hf.z;
		const float spanY = b - t;
		if ( depth > 1.0f && span > 0.0001f && spanY != 0.0f )
		{
			const float sx = g_stereo.HudInvertX() ? -1.0f : 1.0f;
			const float sy = g_stereo.HudInvertY() ? 1.0f : -1.0f;
			const float tanX = sx * ( ax * hr.x + ay * hr.y + az * hr.z ) / depth;
			const float tanY = sy * ( ax * hu.x + ay * hu.y + az * hu.z ) / depth;

			float fx = ( tanX - l ) / span;
			float fy = ( tanY - t ) / spanY;
			// Never clamp a menu into view. Clamping is right for a HUD, which
			// must stay reachable; for a panel deliberately pinned in the world
			// it would defeat the whole point -- the player could never look
			// away from it, which is the behaviour being escaped.
			if ( !inMenu && g_stereo.HudClamp() )
			{
				// A margin, not 0..1: the HUD has width, so pinning its CENTRE to
				// the edge still pushes half of it off screen.
				fx = fx < 0.15f ? 0.15f : ( fx > 0.85f ? 0.85f : fx );
				fy = fy < 0.15f ? 0.15f : ( fy > 0.85f ? 0.85f : fy );
			}

			// Position AND scale in one, by choosing the ortho window outright
			// rather than nudging the existing one.
			//
			// Ortho's arguments are the 2D coordinate range mapped across the
			// viewport, so a WIDER range makes everything drawn in it smaller. To
			// put the HUD's own centre at screen fraction (fx, fy) in a window of
			// size W, the left edge has to be cx - fx*W -- which collapses to the
			// untouched window when scale is 1 and fx is 0.5, so the identity case
			// costs nothing.
			//
			// The per-eye parallax computed above IS the convergence, so this
			// REPLACES the fixed hud_convergence shift rather than adding to it --
			// applying both would converge the panel twice.
			// A menu takes menu_scale; the HUD takes its own. Same key the
			// pointer divides by, so the buttons stay under the cross.
			const float rawScale = inMenu ? g_stereo.MenuScale() : g_stereo.HudScale();
			const double scale = ( rawScale > 0.01f ) ? (double)rawScale : 1.0;
			const double w = ( right - left ) / scale;
			const double h = ( bottom - top ) / scale;
			const double cx = ( left + right ) * 0.5;
			const double cy = ( top + bottom ) * 0.5;
			const double nl = cx - fx * w;
			const double nt = cy - fy * h;

			g_stereo.NoteHudPlacement( fx, fy );
			InterlockedIncrement( &g_countOrthoShifted );
			g_originalOrtho( thisptr, edx, nl, nt, nl + w, nt + h, zNear, zFar );
			return;
		}
	}

	// Menu scaling. A WIDER ortho window draws its contents smaller, so this
	// shrinks the panel about the screen centre without moving where its centre
	// is -- and the pointer divides by the same number, so hit-testing follows.
	const double mscale = ( inMenu && g_stereo.MenuScale() > 0.05f )
		? (double)g_stereo.MenuScale() : 1.0;
	if ( mscale != 1.0 )
	{
		const double mw = ( right - left ) / mscale;
		const double mh = ( bottom - top ) / mscale;
		const double mcx = ( left + right ) * 0.5;
		const double mcy = ( top + bottom ) * 0.5;
		InterlockedIncrement( &g_countOrthoShifted );
		g_originalOrtho( thisptr, edx,
						 mcx - mw * 0.5 + shiftX, mcy - mh * 0.5 + shiftY,
						 mcx + mw * 0.5 + shiftX, mcy + mh * 0.5 + shiftY,
						 zNear, zFar );
		return;
	}

	InterlockedIncrement( &g_countOrthoShifted );
	g_originalOrtho( thisptr, edx, left + shiftX, top + shiftY,
			 right + shiftX, bottom + shiftY, zNear, zFar );
}

//-----------------------------------------------------------------------------
// Both eye passes must see the same instant. Source samples engine time while
// rendering, so without this the second eye gets a slightly later time than the
// first and anything time-driven -- sprites, animated materials, effects --
// lands in a different place per eye, which reads as jitter.
//-----------------------------------------------------------------------------
float __fastcall Detour_EngineTime( void* thisptr, void* edx )
{
	if ( g_timeFrozen && g_stereo.InEyePass() )
	{
		InterlockedIncrement( &g_countTimeFrozen );
		return g_frozenTime;
	}
	return g_originalEngineTime( thisptr, edx );
}

//-----------------------------------------------------------------------------
// Frustum culling override.
//
// The engine tests against a frustum built from the game's symmetric FOV on a
// 4:3 view. The frustum we actually render is wider, and asymmetric in opposite
// directions per eye, so geometry near the edges -- and especially toward each
// eye's outer side -- gets culled while still being visible. Answering "not
// culled" during the eye passes trades some overdraw for correctness, which is
// a good trade on hardware this far ahead of a 2006 engine.
//
// Only active inside an eye pass, so the engine's own culling still applies to
// anything drawn outside stereo rendering.
//-----------------------------------------------------------------------------
// These reach client renderables only. World brush surfaces are culled inside
// engine.dll against a frustum IVEngineClient does not expose, so no amount of
// relaxing here can bring a missing ceiling back -- only a wider engine FOV can.
// That asymmetry is diagnostic in itself: when a door frame draws in front of a
// hole where the wall should be, the door is an entity these rescued and the
// wall is world geometry they could not.
bool __fastcall Detour_CullBox( void* thisptr, void* edx, const void* mins, const void* maxs )
{
	InterlockedIncrement( &g_countCullTested );

	if ( g_cullOverride && g_relaxFrustum && g_stereo.InEyePass() )
	{
		// Only count the ones the engine would actually have thrown away.
		if ( g_originalCullBox( thisptr, edx, mins, maxs ) )
			InterlockedIncrement( &g_countCullRescued );
		return false;
	}

	return g_originalCullBox( thisptr, edx, mins, maxs );
}

int __fastcall Detour_IsBoxVisible( void* thisptr, void* edx, const void* mins, const void* maxs )
{
	InterlockedIncrement( &g_countVisTested );

	if ( g_cullOverride && g_relaxFrustum && g_stereo.InEyePass() )
	{
		if ( !g_originalIsBoxVisible( thisptr, edx, mins, maxs ) )
			InterlockedIncrement( &g_countVisRescued );
		return 1;
	}

	return g_originalIsBoxVisible( thisptr, edx, mins, maxs );
}

// PVS cluster test. Rejecting here means the geometry is not merely clipped, it
// is never submitted -- which is what leaves a hole showing another room.
//
// The PVS is computed from the view origin, not the view direction, so it cannot
// be responsible for anything that changes as the head turns. Relaxing it is
// cheap insurance rather than a fix, and it is one of the two that can put an
// entity from an unseen room into the frame.
int __fastcall Detour_IsBoxInViewCluster( void* thisptr, void* edx, const void* mins, const void* maxs )
{
	InterlockedIncrement( &g_countClusterTested );

	if ( g_cullOverride && g_relaxPvs && g_stereo.InEyePass() )
	{
		if ( !g_originalIsBoxInViewCluster( thisptr, edx, mins, maxs ) )
			InterlockedIncrement( &g_countClusterRescued );
		return 1;
	}
	return g_originalIsBoxInViewCluster( thisptr, edx, mins, maxs );
}

// Area portal frustum -- direction-dependent, so it genuinely does need relaxing
// for a wider VR view. Note this counts *entities* tested against an area, not
// the engine's own decision about which areas to open; that one happens in
// engine.dll, is computed in the engine's screen space, and follows the same FOV.
bool __fastcall Detour_DoesBoxTouchAreaFrustum( void* thisptr, void* edx, const void* mins,
												const void* maxs, int area )
{
	InterlockedIncrement( &g_countAreaTested );

	if ( g_cullOverride && g_relaxArea && g_stereo.InEyePass() )
	{
		if ( !g_originalAreaFrustum( thisptr, edx, mins, maxs, area ) )
			InterlockedIncrement( &g_countAreaRescued );
		return true;
	}
	return g_originalAreaFrustum( thisptr, edx, mins, maxs, area );
}

bool __fastcall Detour_IsOccluded( void* thisptr, void* edx, const void* mins, const void* maxs )
{
	InterlockedIncrement( &g_countOccluderTested );

	if ( g_cullOverride && g_relaxOcclusion && g_stereo.InEyePass() )
	{
		if ( g_originalIsOccluded( thisptr, edx, mins, maxs ) )
			InterlockedIncrement( &g_countOccluderRescued );
		return false;
	}
	return g_originalIsOccluded( thisptr, edx, mins, maxs );
}

} // namespace

//-----------------------------------------------------------------------------
StereoRenderer& Stereo()
{
	return g_stereo;
}

// Defined out here, not beside the flag: g_materialTrace lives in this
// file's anonymous namespace, and a class member cannot be defined inside
// one. It can still SEE it from anywhere later in the same file.
void StereoRenderer::SetMaterialTrace( bool on ) { g_materialTrace = on; }

bool StereoRenderer::InstallMatrixHooks( void* materialSystem )
{
	g_originalMatrixMode = reinterpret_cast<MatrixModeFn>(
		g_matrixModeHook.Install( materialSystem, matsys_slot::kMatrixMode, &Detour_MatrixMode ) );

	g_originalLoadMatrix = reinterpret_cast<LoadMatrixFn>(
		g_loadMatrixHook.Install( materialSystem, matsys_slot::kLoadMatrixVMatrix, &Detour_LoadMatrix ) );

	g_originalPerspectiveX = reinterpret_cast<PerspectiveXFn>(
		g_perspectiveHook.Install( materialSystem, matsys_slot::kPerspectiveX, &Detour_PerspectiveX ) );

	if ( !g_originalMatrixMode || !g_originalLoadMatrix || !g_originalPerspectiveX )
	{
		LogError( "stereo: failed to hook material system matrices "
				  "(MatrixMode=%p LoadMatrix=%p PerspectiveX=%p)",
				  g_originalMatrixMode, g_originalLoadMatrix, g_originalPerspectiveX );
		return false;
	}

	g_originalOrtho = reinterpret_cast<OrthoFn>(
		g_orthoHook.Install( materialSystem, matsys_slot::kOrtho, &Detour_Ortho ) );
	if ( !g_originalOrtho )
		LogWarn( "stereo: failed to hook Ortho(%d) -- HUD will diverge between eyes",
				 matsys_slot::kOrtho );

	// Bind is installed unconditionally so the trace can be switched on at
	// runtime, but it does nothing at all until g_materialTrace is set. A hook
	// that is only present when a flag is on cannot be turned on mid-session,
	// and this is exactly the sort of thing wanted DURING a flash, not before.
	g_originalBind = reinterpret_cast<BindFn>(
		g_bindHook.Install( materialSystem, matsys_slot::kBind, &Detour_Bind ) );
	if ( !g_originalBind )
		LogWarn( "stereo: failed to hook Bind(%d) -- the material trace is dead, "
				 "but nothing else is affected", matsys_slot::kBind );

	Log( "stereo: hooked IMaterialSystem MatrixMode(%d) LoadMatrix(%d) PerspectiveX(%d) Ortho(%d) Bind(%d)",
		 matsys_slot::kMatrixMode, matsys_slot::kLoadMatrixVMatrix,
		 matsys_slot::kPerspectiveX, matsys_slot::kOrtho, matsys_slot::kBind );
	return true;
}

bool StereoRenderer::InstallTimeHook( void* engineClient )
{
	if ( !engineClient )
		return false;

	g_originalEngineTime = reinterpret_cast<EngineTimeFn>(
		g_engineTimeHook.Install( engineClient, engine_slot::kTime, &Detour_EngineTime ) );

	if ( !g_originalEngineTime )
	{
		LogWarn( "stereo: failed to hook IVEngineClient::Time(%d) -- eyes may see "
				 "different instants, showing up as jitter", engine_slot::kTime );
		return false;
	}

	Log( "stereo: hooked IVEngineClient::Time (slot %d) to freeze time across eye passes",
		 engine_slot::kTime );
	return true;
}

bool StereoRenderer::InstallCullingHooks( void* engineClient )
{
	if ( !engineClient )
		return false;

	g_originalCullBox = reinterpret_cast<CullBoxFn>(
		g_cullBoxHook.Install( engineClient, engine_slot::kCullBox, &Detour_CullBox ) );
	g_originalIsBoxVisible = reinterpret_cast<IsBoxVisibleFn>(
		g_isBoxVisibleHook.Install( engineClient, engine_slot::kIsBoxVisible,
									&Detour_IsBoxVisible ) );

	if ( !g_originalCullBox || !g_originalIsBoxVisible )
	{
		LogWarn( "stereo: failed to hook culling (CullBox=%p IsBoxVisible=%p) -- "
				 "geometry will keep disappearing at the edges of the VR frustum",
				 g_originalCullBox, g_originalIsBoxVisible );
		return false;
	}

	// The visibility tests that actually drop whole rooms. Individually optional
	// -- a failure to hook one still leaves the others useful.
	g_originalIsBoxInViewCluster = reinterpret_cast<IsBoxVisibleFn>(
		g_isBoxInViewClusterHook.Install( engineClient, engine_slot::kIsBoxInViewCluster,
										  &Detour_IsBoxInViewCluster ) );
	g_originalAreaFrustum = reinterpret_cast<AreaFrustumFn>(
		g_areaFrustumHook.Install( engineClient, engine_slot::kDoesBoxTouchAreaFrustum,
								   &Detour_DoesBoxTouchAreaFrustum ) );
	g_originalIsOccluded = reinterpret_cast<IsOccludedFn>(
		g_isOccludedHook.Install( engineClient, engine_slot::kIsOccluded, &Detour_IsOccluded ) );

	g_cullOverride = true;
	g_relaxFrustum = m_settings.relaxFrustum;
	g_relaxArea = m_settings.relaxArea;
	g_relaxPvs = m_settings.relaxPvs;
	g_relaxOcclusion = m_settings.relaxOcclusion;

	Log( "stereo: hooked IVEngineClient CullBox(%d) IsBoxVisible(%d) "
		 "IsBoxInViewCluster(%d)=%s DoesBoxTouchAreaFrustum(%d)=%s IsOccluded(%d)=%s",
		 engine_slot::kCullBox, engine_slot::kIsBoxVisible,
		 engine_slot::kIsBoxInViewCluster, g_originalIsBoxInViewCluster ? "ok" : "FAILED",
		 engine_slot::kDoesBoxTouchAreaFrustum, g_originalAreaFrustum ? "ok" : "FAILED",
		 engine_slot::kIsOccluded, g_originalIsOccluded ? "ok" : "FAILED" );
	return true;
}

bool StereoRenderer::Init( void* materialSystem, void* engineClient, IVRBackend* vr,
						   const StereoSettings& settings )
{
	m_vr = vr;
	m_settings = settings;
	m_engineClient = engineClient;

	if ( !materialSystem )
	{
		LogError( "stereo: no IMaterialSystem, stereo unavailable" );
		return false;
	}
	if ( !vr || !vr->IsReady() )
	{
		LogError( "stereo: VR backend not ready, stereo unavailable" );
		return false;
	}

	g_materialSystem = materialSystem;

	// Sanity-check the vtable before hooking into it. A wrong slot count means
	// the indices came from somewhere untrustworthy.
	Log( "stereo: material system %p, using verified slot map "
		 "(SetRenderTarget=%d MatrixMode=%d LoadMatrix=%d PerspectiveX=%d)",
		 materialSystem, matsys_slot::kSetRenderTarget, matsys_slot::kMatrixMode,
		 matsys_slot::kLoadMatrixVMatrix, matsys_slot::kPerspectiveX );

	if ( !InstallMatrixHooks( materialSystem ) )
		return false;

	if ( m_settings.freezeTime )
		InstallTimeHook( engineClient );

	if ( m_settings.relaxCulling )
		InstallCullingHooks( engineClient );

	m_adjust[kEyeLeft] = m_settings.initialAdjust[kEyeLeft];
	m_adjust[kEyeRight] = m_settings.initialAdjust[kEyeRight];
	m_tracePasses = m_settings.tracePasses;
	m_ready = true;

	Log( "stereo: ready (perEyeProjection=%d separation=%.2f liveAdjust=%d "
		 "hudConvergence=%.2f freezeTime=%d)",
		 m_settings.perEyeProjection ? 1 : 0, m_settings.eyeSeparationScale,
		 m_settings.liveAdjust ? 1 : 0, m_settings.hudConvergence,
		 m_settings.freezeTime ? 1 : 0 );
	Log( "stereo: initial eye offsets L(%.4f,%.4f) R(%.4f,%.4f)",
		 m_adjust[kEyeLeft].x, m_adjust[kEyeLeft].y,
		 m_adjust[kEyeRight].x, m_adjust[kEyeRight].y );
	return true;
}

void StereoRenderer::Shutdown()
{
	g_matrixModeHook.Remove();
	g_loadMatrixHook.Remove();
	g_perspectiveHook.Remove();
	g_orthoHook.Remove();
	g_bindHook.Remove();
	g_engineTimeHook.Remove();
	g_cullBoxHook.Remove();
	g_isBoxVisibleHook.Remove();
	g_isBoxInViewClusterHook.Remove();
	g_areaFrustumHook.Remove();
	g_isOccludedHook.Remove();
	g_cullOverride = false;

	ReleaseEyeSurfaces();
	m_ready = false;
}

void StereoRenderer::ReleaseEyeSurfaces()
{
	for ( int b = 0; b < kBufferCount; ++b )
	{
		for ( int i = 0; i < kEyeCount; ++i )
		{
			if ( g_eyeSurface[b][i] )
			{
				g_eyeSurface[b][i]->Release();
				g_eyeSurface[b][i] = nullptr;
			}
		}
	}
	Log( "stereo: eye surfaces released" );
}

bool StereoRenderer::CreateEyeSurfaces( IDirect3DDevice9* device )
{
	if ( !device )
		return false;

	if ( g_eyeSurface[0][0] && g_eyeSurface[kBufferCount - 1][kEyeCount - 1] )
		return true;

	// Match the backbuffer exactly. The scene is rendered there, so a same-size
	// eye surface makes the copy a straight 1:1 blit -- StretchRect into a
	// differently shaped target distorts the frustum's asymmetry and pushes the
	// two eyes out of alignment.
	IDirect3DSurface9* backBuffer = nullptr;
	if ( FAILED( device->GetBackBuffer( 0, 0, D3DBACKBUFFER_TYPE_MONO, &backBuffer ) )
		 || !backBuffer )
	{
		LogError( "stereo: could not query backbuffer for eye surface sizing" );
		return false;
	}

	D3DSURFACE_DESC desc = {};
	backBuffer->GetDesc( &desc );
	backBuffer->Release();

	// ---- IS THE BACKBUFFER ACTUALLY MULTISAMPLED? -----------------------
	//
	// We read this desc anyway and never reported the one field that answers
	// "is the game's MSAA setting reaching the image we submit". Without it,
	// edge aliasing has two indistinguishable explanations -- AA not applied,
	// or AA applied and then thrown away by a resample -- and no way to choose.
	//
	// The eye surfaces below are deliberately D3DMULTISAMPLE_NONE: a resolved
	// copy is what the compositor wants, and StretchRect from a multisampled
	// source performs that resolve. So a non-zero type here is fine and
	// expected; ZERO means the game is not multisampling at all, whatever its
	// menu says.
	Log( "stereo: backbuffer %ux%u fmt=%d multisample=%d quality=%lu -- %s",
		 desc.Width, desc.Height, (int)desc.Format,
		 (int)desc.MultiSampleType, (unsigned long)desc.MultiSampleQuality,
		 desc.MultiSampleType == D3DMULTISAMPLE_NONE
			 ? "NO MSAA on the backbuffer"
			 : "MSAA present, resolved by the eye copy" );

	g_eyeWidth = desc.Width;
	g_eyeHeight = desc.Height;
	m_renderAspect = ( desc.Height > 0 ) ? ( (float)desc.Width / (float)desc.Height ) : 0.0f;

	for ( int b = 0; b < kBufferCount; ++b )
	{
		for ( int i = 0; i < kEyeCount; ++i )
		{
			HRESULT hr = device->CreateRenderTarget( g_eyeWidth, g_eyeHeight,
													 desc.Format, D3DMULTISAMPLE_NONE, 0,
													 FALSE, &g_eyeSurface[b][i], nullptr );
			if ( FAILED( hr ) || !g_eyeSurface[b][i] )
			{
				LogError( "stereo: CreateRenderTarget %ux%u (buffer %d, eye %d) failed (hr=0x%08lX)",
						  g_eyeWidth, g_eyeHeight, b, i, (unsigned long)hr );
				ReleaseEyeSurfaces();
				return false;
			}
		}
	}

	Log( "stereo: eye surfaces %ux%u (aspect %.3f), %d buffers, matching backbuffer",
		 g_eyeWidth, g_eyeHeight, m_renderAspect, kBufferCount );
	return true;
}

IDirect3DSurface9* StereoRenderer::EyeSurface( int eye ) const
{
	if ( eye < 0 || eye >= kEyeCount )
		return nullptr;
	return g_eyeSurface[g_bufferIndex][eye];
}

const EyeParams& StereoRenderer::CurrentEyeParams() const
{
	static EyeParams invalid;
	if ( !m_vr )
		return invalid;
	return m_vr->GetEyeParams( m_currentEye );
}

//-----------------------------------------------------------------------------
// Live alignment. Values are logged on every change in a form that can be
// pasted straight into sinvr.cfg, so a tuning session ends with something
// durable rather than a feeling.
//-----------------------------------------------------------------------------
void StereoRenderer::SelectEye( int eye )
{
	m_selectedEye = ( eye == kEyeRight ) ? kEyeRight : kEyeLeft;
	Log( "adjust: selected %s eye (x=%.4f y=%.4f, step=%.4f)",
		 m_selectedEye == kEyeLeft ? "LEFT" : "RIGHT",
		 m_adjust[m_selectedEye].x, m_adjust[m_selectedEye].y, m_adjustStep );
}

void StereoRenderer::NudgeSelectedEye( float dx, float dy )
{
	EyeAdjust& a = m_adjust[m_selectedEye];
	a.x += dx * m_adjustStep;
	a.y += dy * m_adjustStep;

	Log( "adjust: %s eye  x=%.4f y=%.4f   [cfg] %s_eye_offset_x = %.4f / _y = %.4f",
		 m_selectedEye == kEyeLeft ? "LEFT" : "RIGHT", a.x, a.y,
		 m_selectedEye == kEyeLeft ? "left" : "right", a.x, a.y );
}

void StereoRenderer::ResetSelectedEye()
{
	m_adjust[m_selectedEye] = EyeAdjust();
	Log( "adjust: %s eye reset to 0", m_selectedEye == kEyeLeft ? "LEFT" : "RIGHT" );
}

void StereoRenderer::ChangeStep( float factor )
{
	m_adjustStep *= factor;
	if ( m_adjustStep < 0.0001f )
		m_adjustStep = 0.0001f;
	if ( m_adjustStep > 0.5f )
		m_adjustStep = 0.5f;
	Log( "adjust: step = %.4f", m_adjustStep );
}

void StereoRenderer::SetAdjust( int eye, float x, float y )
{
	int e = ( eye == kEyeRight ) ? kEyeRight : kEyeLeft;
	m_adjust[e].x = x;
	m_adjust[e].y = y;
}

void StereoRenderer::LogMatrixPathStats() const
{
	Log( "stereo paths: LoadMatrix(VIEW)=%ld LoadMatrix(PROJ)=%ld PerspectiveX=%ld "
		 "projReplaced=%ld | bounds L u[%.3f..%.3f] R u[%.3f..%.3f] aspect=%.3f",
		 g_countLoadView, g_countLoadProj, g_countPerspectiveX, g_countProjReplaced,
		 m_bounds[kEyeLeft].uMin, m_bounds[kEyeLeft].uMax,
		 m_bounds[kEyeRight].uMin, m_bounds[kEyeRight].uMax,
		 m_renderAspect );

	Log( "stereo hud/time: Ortho=%ld shifted=%ld skippedForMenu=%ld timeFrozen=%ld "
		 "(convergence=%.2f cursorVisible=%d)",
		 g_countOrtho, g_countOrthoShifted, g_countOrthoSkippedForMenu,
		 g_countTimeFrozen, m_settings.hudConvergence, g_cursorVisible ? 1 : 0 );

	// One window means every menu panel is transformed identically, so a
	// per-panel misalignment cannot come from here. More than one means the
	// submenus are drawn in their own coordinate space, and the pointer's
	// inverse -- which assumes one -- is wrong for exactly those panels.
	if ( g_menuOrthoCount > 0 )
	{
		Log( "menu ortho: %d distinct window(s) seen while a menu was up%s",
			 g_menuOrthoCount,
			 g_menuOrthoCount == 1
				 ? " -- one space, so every panel gets the same transform"
				 : " -- MORE THAN ONE, so submenus are drawn in a different space"
				   " and the pointer's inverse is wrong for them" );
		for ( int i = 0; i < g_menuOrthoCount; ++i )
			Log( "menu ortho:   [%d] l=%.1f t=%.1f r=%.1f b=%.1f  (%u calls)",
				 i, g_menuOrtho[i].l, g_menuOrtho[i].t,
				 g_menuOrtho[i].r, g_menuOrtho[i].b, g_menuOrtho[i].count );
	}

	// GAMEPLAY windows. This is what the anchored-HUD full-screen-effect
	// problem needs: a fade to white goes through this same hook and is moved
	// and shrunk along with the HUD, so it lands off to one side of the view.
	//
	// Read the CALL COUNTS, not just the extents. The HUD is drawn every frame
	// and will be in the hundreds of thousands; a fade lasts well under a
	// second, so it shows up as a window with a few hundred calls at most. If a
	// low-count window has DIFFERENT extents from the high-count ones, extents
	// can separate them and the fix is to pass that shape through untouched. If
	// its extents MATCH the HUD's, they cannot, and the fade has to be told
	// apart some other way.
	// Every distinct projection in an eye pass, and whether we left it alone.
	// An entry with kept > 0 is a render-to-texture camera -- an in-game
	// monitor, a mirror -- keeping its own projection so the texture comes out
	// IDENTICAL in both eyes. A monitor that shows each eye a different picture
	// cannot be fused; the stereo has to come from the surface it is drawn on.
	if ( g_projSeenCount > 0 )
	{
		Log( "stereo projections: %d distinct view(s) per eye pass | render "
			 "aspect %.3f, RT tolerance %.3f",
			 g_projSeenCount, RenderAspect(), m_rtAspectTolerance );
		for ( int i = 0; i < g_projSeenCount; ++i )
			Log( "stereo projections:   fov=%.2f aspect=%.3f  replaced=%u kept=%u%s",
				 g_projSeen[i].fov, g_projSeen[i].aspect,
				 g_projSeen[i].count, g_projSeen[i].kept,
				 g_projSeen[i].kept > 0
					 ? "   <- RT camera, left alone (in-game screen/mirror)"
					 : "" );
	}

	if ( g_gameOrthoCount > 0 )
	{
		// The distribution of per-eye-pass call counts. This is what the fade
		// threshold must be set from: the HUD's own steady figure is the tall
		// bucket, and a fade shows as a smaller bucket above it. A threshold
		// that lands inside the tall bucket would unanchor the HUD constantly.
		{
			char hist[256];
			int used = 0;
			for ( int i = 0; i < kOrthoHistogramBuckets; ++i )
			{
				if ( g_orthoPassHistogram[i] == 0 )
					continue;
				const int n = _snprintf_s( hist + used, sizeof( hist ) - used,
										   _TRUNCATE, "%s%d:%u",
										   used ? "  " : "", i,
										   g_orthoPassHistogram[i] );
				if ( n <= 0 )
					break;
				used += n;
			}
			Log( "fade detect: threshold %d calls/pass | %u fade episode(s) | "
				 "currently %s | per-pass distribution %s",
				 m_fullscreenFadeCalls, g_fadeEpisodes,
				 g_fadeActive ? "FADING (anchor off)" : "idle",
				 used ? hist : "(none)" );
			Log( "fade detect: read the distribution -- the TALL bucket is the "
				 "HUD drawing normally and the threshold must sit ABOVE it. A "
				 "threshold inside it unanchors the HUD during ordinary play; "
				 "one too high misses the fade entirely." );
		}

		if ( g_materialTrace )
		{
			if ( !g_originalBind )
			{
				LogWarn( "material trace: ON but Bind was never hooked -- nothing "
						 "is being recorded" );
			}
			else if ( g_materialUseCount == 0 )
			{
				LogWarn( "material trace: ON, Bind hooked, but ZERO materials "
						 "recorded. Either no 2D drew this pass, or slot %d is not "
						 "Bind after all -- check the name validation warning below.",
						 matsys_slot::kBind );
			}
			else
			{
				Log( "material trace: %d (material, ortho window) pair(s). "
					 "DELTA is the column that matters -- a fade material sits at "
					 "0 through normal play and spikes for the one heartbeat that "
					 "contains the flash. Match `window` against the game ortho "
					 "list below; the HUD's window is the high-count one.",
					 g_materialUseCount );
				// TWO PASSES, and the first one is the answer.
				//
				// A material that was idle and has STARTED drawing is exactly
				// the fade's signature: nothing during normal play, then a burst
				// for the one heartbeat containing the flash. Those are printed
				// first and marked, so the line that matters cannot be lost in a
				// hundred rows of HUD sprites drawing at their usual rate.
				int started = 0;
				for ( int i = 0; i < g_materialUseCount; ++i )
				{
					MaterialUse& e = g_materialUse[i];
					const unsigned int delta = e.count - e.lastCount;
					if ( e.wasIdle && delta > 0 )
					{
						Log( "material trace:   *** STARTED ***  delta %6u  "
							 "total %8u  (window hint %d)  %s",
							 delta, e.count, e.window, e.name );
						++started;
					}
				}
				if ( started == 0 )
					Log( "material trace:   (nothing started drawing this "
						 "heartbeat -- baseline)" );

				for ( int i = 0; i < g_materialUseCount; ++i )
				{
					MaterialUse& e = g_materialUse[i];
					const unsigned int delta = e.count - e.lastCount;
					if ( delta > 0 && !e.wasIdle )
						Log( "material trace:   steady  delta %6u  total %8u  %s",
							 delta, e.count, e.name );
					e.wasIdle = ( delta == 0 );
					e.lastCount = e.count;
				}
			}
			// Slot 49 is PROVEN Bind -- the names above are real Source material
			// paths. So a handful of unreadable names is not a wrong slot; it is
			// a material torn down between the engine's call and our read, which
			// is expected on another thread. Reported as a COUNT so a genuine
			// problem still stands out: thousands here, with names that look
			// like noise, would mean the slot moved in a game update.
			if ( g_materialNameFailures )
				Log( "material trace: %u name read(s) failed. A few is normal -- a "
					 "material can be destroyed between Bind and our read. Thousands, "
					 "with unreadable names above, would mean slot %d is no longer Bind.",
					 g_materialNameFailures, matsys_slot::kBind );
		}

		Log( "game ortho: %d distinct window(s) seen during PLAY -- a rare "
			 "low-count window here is a transient full-screen effect (a fade, "
			 "a damage flash); the high-count ones are the HUD",
			 g_gameOrthoCount );
		for ( int i = 0; i < g_gameOrthoCount; ++i )
		{
			const double w = g_gameOrtho[i].r - g_gameOrtho[i].l;
			const double h = g_gameOrtho[i].b - g_gameOrtho[i].t;
			Log( "game ortho:   [%d] l=%.1f t=%.1f r=%.1f b=%.1f "
				 "zn=%.3f zf=%.3f  (%.0f x %.0f)  %u calls",
				 i, g_gameOrtho[i].l, g_gameOrtho[i].t, g_gameOrtho[i].r,
				 g_gameOrtho[i].b, g_gameOrtho[i].zn, g_gameOrtho[i].zf,
				 w, ( h < 0.0 ? -h : h ), g_gameOrtho[i].count );
		}
	}

	// Healthy is roughly 2 applications per frame (one per eye). Anything much
	// higher means the re-load detector is missing cases and the offset is
	// accumulating again.
	Log( "stereo view offset: applied=%ld reentrantSkipped=%ld (%.2f applied per frame)",
		 g_countViewOffsetApplied, g_countViewOffsetReentrant,
		 m_eyeFrames ? (double)g_countViewOffsetApplied / (double)m_eyeFrames : 0.0 );

	// Should be equal. Any excess in one eye is content rendered from a view the
	// other eye never got, which reaches both eyes identically and so appears
	// flat.
	Log( "stereo views/eye: left=%ld right=%ld (imbalance=%ld)",
		 g_countFreshViewsLeft, g_countFreshViewsRight,
		 g_countFreshViewsLeft - g_countFreshViewsRight );

	// Which test was actually throwing geometry away. Both halves of each pair
	// matter: "0 rescued" out of thousands tested means the test never rejects
	// anything, while "0 rescued out of 0 tested" means the slot we hooked is
	// never called and the index needs re-deriving. The old single-number form
	// could not tell those apart.
	Log( "stereo culling: relax master=%d frustum=%d area=%d pvs=%d occlusion=%d",
		 g_cullOverride ? 1 : 0, g_relaxFrustum ? 1 : 0, g_relaxArea ? 1 : 0,
		 g_relaxPvs ? 1 : 0, g_relaxOcclusion ? 1 : 0 );
	Log( "stereo culling: cullbox %ld/%ld  isboxvisible %ld/%ld  pvs %ld/%ld  "
		 "area %ld/%ld  occluder %ld/%ld  (rescued/tested)",
		 g_countCullRescued, g_countCullTested,
		 g_countVisRescued, g_countVisTested,
		 g_countClusterRescued, g_countClusterTested,
		 g_countAreaRescued, g_countAreaTested,
		 g_countOccluderRescued, g_countOccluderTested );

	// A tested count of zero is worth naming, but it is not automatically a bug:
	// this engine really does route almost everything through CullBox, and some
	// of these are only reached by entities that straddle an area boundary. It
	// matters because kDoesBoxTouchAreaFrustum(59) and kIsOccluded(70) were
	// derived from the SDK's +1 shift and never verified by disassembly, so a
	// permanent zero is the one symptom that would look identical to a wrong
	// index. Anything with a healthy call count has effectively verified itself.
	if ( g_countVisTested == 0 || g_countClusterTested == 0 ||
		 g_countAreaTested == 0 || g_countOccluderTested == 0 )
		Log( "stereo culling: never called ->%s%s%s%s. Not necessarily wrong -- but "
			 "these slots were derived from the SDK's +1 shift, so a permanent zero "
			 "is indistinguishable from a wrong index.",
			 g_countVisTested == 0 ? " IsBoxVisible(34)" : "",
			 g_countClusterTested == 0 ? " IsBoxInViewCluster(35)" : "",
			 g_countAreaTested == 0 ? " DoesBoxTouchAreaFrustum(59)" : "",
			 g_countOccluderTested == 0 ? " IsOccluded(70)" : "" );

	LogFrustumCoverage();
	LogFrameTiming();

	if ( g_countOrtho > 0 && g_countOrthoShifted == 0 && m_settings.hudConvergence != 0.0f )
		LogWarn( "stereo: Ortho is called but never during an eye pass -- the HUD is "
				 "drawn outside the stereo passes, so it cannot be converged here" );

	if ( g_countProjReplaced == 0 )
	{
		LogWarn( "stereo: the per-eye projection was NEVER applied. Neither "
				 "LoadMatrix(PROJECTION) nor PerspectiveX fired during an eye "
				 "pass, so both eyes are still using the game's own frustum." );
	}
}

//-----------------------------------------------------------------------------
// The engine's culling frustum, and whether it covers what we render.
//
// Source builds its world frustum with GeneratePerspectiveFrustum -- from the
// view setup's origin, angles, FOV and aspect, by angle. It never looks at the
// projection matrix, which is why substituting ours changes the picture and
// changes nothing about what the engine decides to submit. World brush surfaces
// outside that frustum are simply not drawn, and no IVEngineClient hook can
// bring them back; only a wider engine FOV can.
//
// The frustum is symmetric about the view axis:
//     halfTanX = tan(fov / 2)
//     halfTanY = tan(fov / 2) / aspect
// while ours is asymmetric per eye and extended sideways to the render target's
// shape, so it sticks out furthest on each eye's *outer* edge. Fitting one
// inside the other is what the numbers below are for.
//-----------------------------------------------------------------------------
namespace {

constexpr float kDegPerRad = 57.2957795130823f;

// Fraction of the span [lo,hi] that falls outside [-limit,+limit].
float FractionOutside( float lo, float hi, float limit )
{
	const float span = hi - lo;
	if ( span <= 0.0001f || limit <= 0.0f )
		return 0.0f;

	float outside = 0.0f;
	if ( lo < -limit )
		outside += ( -limit ) - lo;
	if ( hi > limit )
		outside += hi - limit;
	return outside / span;
}

float LargerOf( float a, float b ) { return a > b ? a : b; }

// Wall clock on the render thread. QPC rather than timeGetTime because the
// interesting range here is single frames, not seconds.
double NowMs()
{
	static double s_msPerTick = 0.0;
	if ( s_msPerTick == 0.0 )
	{
		LARGE_INTEGER freq = {};
		QueryPerformanceFrequency( &freq );
		s_msPerTick = ( freq.QuadPart > 0 ) ? ( 1000.0 / (double)freq.QuadPart ) : 0.0;
	}
	LARGE_INTEGER now = {};
	QueryPerformanceCounter( &now );
	return (double)now.QuadPart * s_msPerTick;
}

// Source's basis from a QAngle, matching mathlib's AngleVectors exactly --
// including the sign conventions, which are not the obvious ones: pitch is
// positive *downwards* and `right` points along -Y at zero yaw.
void AngleVectors( const QAngle& angles, Vector& forward, Vector& right, Vector& up )
{
	const float pitch = angles.x / kDegPerRad;
	const float yaw   = angles.y / kDegPerRad;
	const float roll  = angles.z / kDegPerRad;

	const float sp = sinf( pitch ), cp = cosf( pitch );
	const float sy = sinf( yaw ),   cy = cosf( yaw );
	const float sr = sinf( roll ),  cr = cosf( roll );

	forward.x = cp * cy;
	forward.y = cp * sy;
	forward.z = -sp;

	right.x = -sr * sp * cy + cr * sy;
	right.y = -sr * sp * sy - cr * cy;
	right.z = -sr * cp;

	up.x = cr * sp * cy + sr * sy;
	up.y = cr * sp * sy - sr * cy;
	up.z = cr * cp;
}

// Source's ScaleFOVByWidthRatio (mathlib). CViewRender::Render applies this to
// CViewSetup::fov on every call, so anything we write has to be pre-divided by
// it -- and, more importantly, re-written before each eye, because it mutates
// the field in place and would otherwise widen the second eye's frustum.
float ScaleFovByWidthRatio( float fovDegrees, float ratio )
{
	const float t = tanf( fovDegrees * 0.5f / kDegPerRad ) * ratio;
	return 2.0f * atanf( t ) * kDegPerRad;
}

} // namespace

void StereoRenderer::NoteEngineProjection( float fovX, float aspect, bool firstOfPass )
{
	if ( fovX <= 0.0f || fovX >= 180.0f )
		return;

	if ( firstOfPass )
	{
		m_observedFovX = fovX;
		m_observedFovMin = fovX;
		m_observedFovMax = fovX;

		// Only the main scene's aspect is meaningful; the viewmodel and RT views
		// have their own.
		if ( aspect > 0.01f )
			m_observedAspect = aspect;
		return;
	}

	if ( fovX < m_observedFovMin )
		m_observedFovMin = fovX;
	if ( fovX > m_observedFovMax )
		m_observedFovMax = fovX;
}

float StereoRenderer::EngineCullAspect() const
{
	return ( m_observedAspect > 0.01f ) ? m_observedAspect : m_renderAspect;
}

bool StereoRenderer::EyeFrustumExtents( int eye, float& l, float& r, float& t, float& b ) const
{
	if ( !m_vr || m_renderAspect <= 0.01f )
		return false;

	const int e = ( eye == kEyeRight ) ? kEyeRight : kEyeLeft;
	const EyeParams& params = m_vr->GetEyeParams( e );
	if ( !params.valid )
		return false;

	EyeBounds bounds;
	ComputeEyeFrustum( params, m_renderAspect, m_adjust[e], l, r, t, b, bounds );
	return true;
}

float StereoRenderer::MenuHudAspect() const { return MenuHudAspectFromOrtho(); }

void StereoRenderer::BackbufferSize( unsigned int& w, unsigned int& h ) const
{
	w = g_eyeWidth;
	h = g_eyeHeight;
}

//-----------------------------------------------------------------------------
// A world point -> a pixel in this eye's backbuffer.
//
// Deliberately built from the SAME pieces as ApplyMenuPanelProjection: the head
// basis pushed in with SetMenuPanel, this eye's offset from the head, and the
// frustum from ComputeEyeFrustum/BuildProjection. A point lying on the menu quad
// therefore lands exactly where the quad drew it, in both eyes, with the correct
// parallax -- and it cannot drift out of agreement with the picture, because
// there is only one set of maths.
//
// The handover's sketch for this proposed mapping the pointer's -1..1 panel
// fraction to pixels and then offsetting each eye "by the same parallax". That
// works only if the offset is derived correctly, which is the same class of
// hand-matched transform that made four earlier menu rounds fail. Projecting the
// world point removes the question.
//-----------------------------------------------------------------------------
bool StereoRenderer::ProjectWorldToBackbuffer( const Vector& world, int eye,
											   float& outX, float& outY ) const
{
	outX = 0.0f;
	outY = 0.0f;

	if ( eye < 0 || eye >= kEyeCount || g_eyeWidth == 0 || g_eyeHeight == 0 )
		return false;
	if ( !m_haveMenuHeadBasis || !m_vr )
		return false;

	const EyeParams& e = m_vr->GetEyeParams( eye );
	if ( !e.valid )
		return false;

	const Vector& hf = m_menuHeadForward;
	const Vector& hr = m_menuHeadRight;
	const Vector& hu = m_menuHeadUp;

	// This eye's world offset from the head -- the same expression the panel
	// projection and the anchored HUD use. OpenVR's eye space and Source's view
	// space share a basis, so these drop straight in.
	const float ex = hr.x * e.offsetRight + hu.x * e.offsetUp - hf.x * e.offsetBack;
	const float ey = hr.y * e.offsetRight + hu.y * e.offsetUp - hf.y * e.offsetBack;
	const float ez = hr.z * e.offsetRight + hu.z * e.offsetUp - hf.z * e.offsetBack;

	// The point relative to THIS eye, in world axes, then into view space.
	// Viewer looks down -Z, hence the negated forward.
	const Vector c = { world.x - m_menuPanel.headWorld.x - ex,
					   world.y - m_menuPanel.headWorld.y - ey,
					   world.z - m_menuPanel.headWorld.z - ez };

	const float vx = MenuDot( c, hr );
	const float vy = MenuDot( c, hu );
	const float vz = -MenuDot( c, hf );

	// Behind the eye, or on the plane: there is no pixel for it.
	if ( vz >= -0.001f )
		return false;

	float fl, fr, ft, fb;
	EyeBounds bounds;
	ComputeEyeFrustum( e, m_renderAspect, m_adjust[eye], fl, fr, ft, fb, bounds );

	VMatrix proj;
	BuildProjection( fl, fr, ft, fb, m_settings.zNear, m_settings.zFar, proj );

	// Clip space. Row-major storage, column vectors: clip = P * v.
	const float cx = proj.m[0][0] * vx + proj.m[0][2] * vz;
	const float cy = proj.m[1][1] * vy + proj.m[1][2] * vz;
	const float cw = -vz;              // m[3][2] = -1, so w = -z

	if ( cw <= 0.0001f )
		return false;

	// NDC, then the D3D viewport transform: x right, y DOWN the screen.
	const float ndcX = cx / cw;
	const float ndcY = cy / cw;

	outX = ( ndcX * 0.5f + 0.5f ) * (float)g_eyeWidth;
	outY = ( 0.5f - ndcY * 0.5f ) * (float)g_eyeHeight;
	return true;
}

bool StereoRenderer::RenderedHalfTangents( float& outX, float& outY ) const
{
	outX = 0.0f;
	outY = 0.0f;

	bool any = false;
	for ( int eye = 0; eye < kEyeCount; ++eye )
	{
		float l, r, t, b;
		if ( !EyeFrustumExtents( eye, l, r, t, b ) )
			continue;

		outX = LargerOf( outX, LargerOf( fabsf( l ), fabsf( r ) ) );
		outY = LargerOf( outY, LargerOf( fabsf( t ), fabsf( b ) ) );
		any = true;
	}
	return any;
}

float StereoRenderer::RequiredEngineFovX() const
{
	float hx, hy;
	if ( !RenderedHalfTangents( hx, hy ) )
		return 0.0f;

	const float aspect = EngineCullAspect();
	if ( aspect <= 0.01f )
		return 0.0f;

	// One number has to satisfy both axes, and the vertical one is the horizontal
	// divided by aspect -- so the vertical requirement has to be scaled back up
	// before they can be compared.
	float need = LargerOf( hx, hy * aspect ) * m_settings.engineFovMargin;

	float fov = 2.0f * atanf( need ) * kDegPerRad;

	// A canted eye points away from head-forward, and the engine's frustum is
	// symmetric about head-forward -- so it has to be widened by the cant on top
	// of the tangents, or the outer edge of a canted eye falls outside it. Zero
	// on parallel-display headsets.
	float maxCantX = 0.0f;
	float maxCantY = 0.0f;
	if ( m_vr )
	{
		for ( int eye = 0; eye < kEyeCount; ++eye )
		{
			const EyeParams& e = m_vr->GetEyeParams( eye );
			if ( !e.valid || !e.canted )
				continue;
			maxCantX = LargerOf( maxCantX, fabsf( e.yawOffset ) );
			maxCantY = LargerOf( maxCantY, fabsf( e.pitchOffset ) );
		}
	}
	fov += 2.0f * LargerOf( maxCantX, maxCantY * aspect );

	// Well short of the degenerate 180: an absurd margin must not be able to
	// produce a frustum the engine cannot build.
	if ( fov > 170.0f )
	{
		// Very wide headsets can genuinely need more than the engine can express.
		// Say so rather than silently under-covering, because the symptom is
		// missing world geometry and the cause would not be obvious.
		LogWarn( "stereo frustum: the rendered view needs an engine fov of %.0f, which "
				 "is beyond what the engine can build. Clamped to 170 -- expect world "
				 "geometry to be culled at the extreme edges. Reduce engine_fov_margin.", fov );
		fov = 170.0f;
	}
	return fov;
}

float StereoRenderer::FallbackEngineFovX() const
{
	if ( !m_vr )
		return 0.0f;

	// Raw tangents, before the extension to the render target's aspect -- that is
	// the part that needs a backbuffer we do not have yet. Cant is included
	// because a canted eye points away from head-forward regardless of aspect.
	float halfTan = 0.0f;
	float cant = 0.0f;
	bool any = false;

	for ( int eye = 0; eye < kEyeCount; ++eye )
	{
		const EyeParams& e = m_vr->GetEyeParams( eye );
		if ( !e.valid )
			continue;

		halfTan = LargerOf( halfTan, LargerOf( fabsf( e.tanLeft ), fabsf( e.tanRight ) ) );
		halfTan = LargerOf( halfTan, LargerOf( fabsf( e.tanTop ), fabsf( e.tanBottom ) ) );
		if ( e.canted )
			cant = LargerOf( cant, LargerOf( fabsf( e.yawOffset ), fabsf( e.pitchOffset ) ) );
		any = true;
	}

	if ( !any || halfTan <= 0.0f )
		return 0.0f;

	float fov = 2.0f * atanf( halfTan * m_settings.engineFovMargin ) * kDegPerRad
			  + 2.0f * cant;
	if ( fov > 170.0f )
		fov = 170.0f;
	return fov;
}

void StereoRenderer::LogFrustumCoverage() const
{
	float hx, hy;
	if ( !RenderedHalfTangents( hx, hy ) )
	{
		Log( "stereo frustum: eye parameters or render aspect not up yet" );
		return;
	}

	const float required = RequiredEngineFovX();
	const float fov = m_observedFovX;
	const float aspect = EngineCullAspect();

	if ( fov <= 0.0f )
	{
		Log( "stereo frustum: render reaches halfTan x=%.3f y=%.3f, so the engine "
			 "needs fov >= %.0f -- but no PerspectiveX has been seen during an eye "
			 "pass yet, so its actual fov is unknown", hx, hy, required );
		return;
	}

	const float engX = tanf( fov * 0.5f / kDegPerRad );
	const float engY = engX / aspect;

	Log( "stereo frustum: engine world fov=%.1f aspect=%.3f -> halfTan x=%.3f y=%.3f | "
		 "render reaches x=%.3f y=%.3f | required engine_fov=%.0f | other views this "
		 "pass %.1f..%.1f (the low one is viewmodel_fov, not world culling)",
		 fov, aspect, engX, engY, hx, hy, required,
		 m_observedFovMin, m_observedFovMax );

	// Measured per eye against that eye's real span, because the rendered frustum
	// is off-centre: the left eye overhangs on the left and the right eye on the
	// right, by the same amount. Averaging or using a half-extent would hide it.
	float worstX = 0.0f;
	float worstY = 0.0f;
	for ( int eye = 0; eye < kEyeCount; ++eye )
	{
		float l, r, t, b;
		if ( !EyeFrustumExtents( eye, l, r, t, b ) )
			continue;
		worstX = LargerOf( worstX, FractionOutside( l, r, engX ) );
		worstY = LargerOf( worstY, FractionOutside( t, b, engY ) );
	}

	// The muzzle-flash factor, reported as a NUMBER rather than as "the fix is
	// on". This is the exact quantity c_baseviewmodel.cpp's
	// FormatViewModelAttachment multiplies the flash's offset-from-the-eye by,
	// so 1.000 means it is the identity and the flash sits on the attachment.
	// Anything else is the multiplier by which the flash is thrown off the gun,
	// and it is worth seeing even when the feature is on -- if fovViewmodel is
	// ever set from somewhere else after us, this line is where it shows up.
	{
		const float wx = tanf( m_lastViewFov * 0.5f / kDegPerRad );
		const float vx = tanf( m_lastViewFovViewmodel * 0.5f / kDegPerRad );
		const float factor = ( vx > 0.0001f ) ? ( wx / vx ) : 0.0f;
		Log( "viewmodel fov: world=%.1f viewmodel=%.1f -> muzzle-flash factor %.3f%s",
			 m_lastViewFov, m_lastViewFovViewmodel, factor,
			 ( factor > 0.99f && factor < 1.01f )
				 ? " (identity -- the flash lands on the attachment)"
				 : " -- FormatViewModelAttachment is scaling the flash's offset from"
				   " the eye by this, so it will not sit on the gun" );
	}

	if ( worstX <= 0.0f && worstY <= 0.0f )
	{
		Log( "stereo frustum: OK -- the engine's frustum contains everything we "
			 "render (spare x=%.1f%% y=%.1f%%)",
			 ( engX / hx - 1.0f ) * 100.0f, ( engY / hy - 1.0f ) * 100.0f );
		return;
	}

	LogWarn( "stereo frustum: SHORT -- the engine culls the world outside %.1f%% of "
			 "image width and %.1f%% of image height. World brush surfaces there are "
			 "never submitted, so ceilings, upper walls and edge geometry are missing "
			 "while entities in front of them still draw. Raise engine_fov to %.0f "
			 "(engine currently reports %.1f).",
			 worstX * 100.0f, worstY * 100.0f, required, fov );
}

//-----------------------------------------------------------------------------
// View ownership.
//
// Everything here writes into the client's own state, so nothing is written
// until the values we read back are the ones we can predict independently.
//-----------------------------------------------------------------------------
bool StereoRenderer::BindViewSetup( void* viewRenderFn )
{
	if ( !m_settings.ownViewSetup )
	{
		Log( "view: ownership disabled by config, staying on the matrix path" );
		return false;
	}

	void** viewGlobal = FindViewGlobal( viewRenderFn );
	if ( !viewGlobal )
	{
		// Rate-limited: the retry runs at frame rate, and an unbounded warning
	// here produced hundreds of identical lines that buried everything else
	// in the log.
	// Its own flag, not m_viewBindAttempts: that counter is incremented far
	// below, after the value dump, so it is still 0 on this path and would
	// never suppress anything.
	if ( !m_viewFindWarned )
	{
		m_viewFindWarned = true;
		LogWarn( "view: could not find the `view` global in View_Render(%p) -- the "
				 "mov ecx,[imm32] / call [edx+0x10] sequence was not there. Staying on "
				 "the matrix path.", viewRenderFn );
	}
		return false;
	}

	void* viewRender = *viewGlobal;
	if ( !viewRender )
	{
		LogWarn( "view: `view` global at %p is null", (void*)viewGlobal );
		return false;
	}

	CViewSetup* vs = nullptr;
	__try
	{
		vs = VCall<viewrender_slot::kGetViewSetup, CViewSetup*>( viewRender );
	}
	__except ( EXCEPTION_EXECUTE_HANDLER )
	{
		LogError( "view: GetViewSetup(slot %d) faulted -- slot map is wrong, refusing "
				  "to touch the view", viewrender_slot::kGetViewSetup );
		return false;
	}

	if ( !vs )
	{
		LogWarn( "view: GetViewSetup returned null" );
		return false;
	}

	// GetViewSetup is `lea eax,[ecx+0x0C]`, so this must be exactly that.
	if ( reinterpret_cast<BYTE*>( vs ) != reinterpret_cast<BYTE*>( viewRender ) + 0x0C )
		LogWarn( "view: GetViewSetup returned %p, expected view+0x0C = %p. Continuing, "
				 "but the CViewSetup offsets came from a build where it was +0x0C.",
				 (void*)vs, (void*)( reinterpret_cast<BYTE*>( viewRender ) + 0x0C ) );

	// Sanity-check before trusting the layout. These are values whose plausible
	// range we know independently: a wrong struct offset shows up here as
	// nonsense rather than as a subtle rendering fault three hours later.
	//
	// NOT READY is a different answer from WRONG, and conflating them cost a
	// session. Before the first map the view is legitimately 0x0 while every
	// other field is already correct, so a single `sane` flag reported a
	// layout mismatch that did not exist -- and, being latched, kept the mod
	// on the matrix path for the whole run. That is the same mistake
	// GetMaxEntities caused in the viewmodel work: a not-ready interface must
	// DEFER, never latch a refusal.
	//
	// So the dimensions are judged separately from everything else. Width and
	// height are the only fields that are meaningless before a map; if they
	// are the ONLY thing wrong, this is simply too early.
	bool layoutBad = false;
	bool notReady = false;
	__try
	{
		if ( vs->width <= 0 || vs->height <= 0 )
			notReady = true;
		else if ( vs->width > 16384 || vs->height > 16384 )
			layoutBad = true;
		if ( vs->zNear <= 0.0f || vs->zNear > 200.0f )
			layoutBad = true;
		if ( vs->zFar <= vs->zNear || vs->zFar > 1.0e7f )
			layoutBad = true;
		if ( vs->fov <= 1.0f || vs->fov >= 179.0f )
			layoutBad = true;
	}
	__except ( EXCEPTION_EXECUTE_HANDLER )
	{
		layoutBad = true;
	}

	// Forced, for testing the recovery -- see SetViewBindForcedRetries. Applied
	// AFTER the real checks so a genuine layout mismatch still reports itself
	// rather than being masked by the test.
	if ( !layoutBad && m_viewBindForce > 0 &&
		 (int)m_viewBindAttempts <= m_viewBindForce )
	{
		notReady = true;
		// 0, not 1: this block runs BEFORE the counter is incremented, so the
		// first call sees 0. With ==1 the line never printed at all, which is
		// how a forced run could be mistaken for an ordinary one.
		if ( m_viewBindAttempts == 0 )
			Log( "view: FORCING %d not-ready bind attempts (test only) -- the "
					  "view really is %dx%d. Expect ownership to arrive late and "
					  "the mod to run on the matrix path until it does.",
					  m_viewBindForce, vs->width, vs->height );
	}

	// Logged on the first attempt and again on the one that succeeds, not on
	// every frame in between -- the retry runs at frame rate.
	// `notReady` must already carry the forced value here -- the block above
	// runs first for exactly that reason. With it below, every retry looked
	// like a success to this guard and dumped the values again: 120 identical
	// blocks in the one mode whose whole job is to be readable.
	if ( m_viewBindAttempts == 0 || ( !notReady && !layoutBad ) )
	{
		Log( "view: IViewRender=%p (global at %p) CViewSetup=%p", viewRender,
			 (void*)viewGlobal, (void*)vs );
		Log( "view: %dx%d fov=%.2f fovViewmodel=%.2f zNear=%.2f zFar=%.1f "
			 "origin=(%.1f %.1f %.1f) angles=(%.2f %.2f %.2f)",
			 vs->width, vs->height, vs->fov, vs->fovViewmodel, vs->zNear, vs->zFar,
			 vs->origin.x, vs->origin.y, vs->origin.z,
			 vs->angles.x, vs->angles.y, vs->angles.z );
	}
	++m_viewBindAttempts;

	if ( layoutBad )
	{
		LogError( "view: those values are not plausible, so the CViewSetup layout does "
				  "not match this build. Refusing to write to it; staying on the matrix "
				  "path." );
		m_viewBindGaveUp = true;
		return false;
	}

	if ( notReady )
	{
		// Too early, not wrong. Retried from the frame loop; the only thing
		// missing is a loaded map to give the view a size.
		if ( m_viewBindAttempts == 1 )
			Log( "view: the view is still %dx%d -- no map loaded yet. Everything "
					  "else about the layout checks out, so this is too early rather "
					  "than wrong; retrying every frame until it has a size.",
					  vs->width, vs->height );
		// The FUNCTION, not the interface.
		//
		// BindViewSetup takes the View_Render function and disassembles it to
		// find the `view` global. By this point `viewRender` is the IViewRender
		// INSTANCE that search produced -- storing that made every retry hand an
		// object to FindViewGlobal, which scanned it as code and reported the
		// instruction pattern missing. The retry could never have worked, and it
		// took forcing the failure to find out.
		m_pendingViewRender = viewRenderFn;
		return false;
	}

	m_viewRender = viewRender;
	m_viewSetup = vs;

	if ( m_engineClient )
	{
		const float aspect = EngineClient( m_engineClient ).GetScreenAspectRatio();
		if ( aspect > 0.1f && aspect < 10.0f )
			m_screenAspect = aspect;
	}

	Log( "view: ownership active after %u attempt(s) -- per-eye fov and origin "
		 "now come from CViewSetup, not from cvars and view-matrix patching "
		 "(screen aspect %.4f)",
		 m_viewBindAttempts, m_screenAspect );
	m_pendingViewRender = nullptr;
	return true;
}

// Called every frame from the View_Render hook until the view has a size.
//
// Costs one predictable branch once bound, which is the price of never
// again losing a whole session to a refusal that was taken 200 ms too
// early. Gives up permanently only on a REAL layout mismatch.
void StereoRenderer::RetryViewBind()
{
	if ( m_viewSetup || m_viewBindGaveUp || !m_pendingViewRender )
		return;
	BindViewSetup( m_pendingViewRender );
}

float StereoRenderer::EngineCullFovPreScale() const

{
	const float required = RequiredEngineFovX();
	if ( required <= 0.0f )
		return 0.0f;

	// CViewRender::Render does
	//     m_View.fov = ScaleFOVByWidthRatio( m_View.fov, GetScreenAspectRatio() / (4/3) )
	// before anything reads it, so undo that here. At 4:3 the ratio is 1 and this
	// is the identity -- which is the case on the VR rig, so this is protection
	// against a non-4:3 window rather than something load-bearing today.
	const float ratio = m_screenAspect / ( 4.0f / 3.0f );
	if ( ratio <= 0.01f )
		return required;

	return ScaleFovByWidthRatio( required, 1.0f / ratio );
}

void StereoRenderer::ApplyEyeToViewSetup( CViewSetup& vs, const CViewSetup& base, int eye ) const
{
	// Always start from `base`. Render rescales fov in place, so leaving the
	// previous eye's value in there would hand the second eye a wider frustum
	// than the first -- silently, and only at non-4:3.
	vs.fov = base.fov;
	vs.fovViewmodel = base.fovViewmodel;

	const float cullFov = EngineCullFovPreScale();
	if ( cullFov > 0.0f )
		vs.fov = cullFov;

	// Drive FormatViewModelAttachment's factor to exactly 1.0 so it stops
	// throwing the muzzle flash away from the gun. See matchViewmodelFov in
	// stereo.h for the whole mechanism -- in short, that function scales the
	// flash's offset FROM THE EYE by tan(fov/2)/tan(fovViewmodel/2), which is
	// a small correct correction on a monitor and a 2.3x error in VR, where
	// the gun is nowhere near the eye axis. We replace the viewmodel's
	// projection anyway, so there is no squash left to correct for.
	if ( m_settings.matchViewmodelFov )
		vs.fovViewmodel = vs.fov;

	// ---- AND THE SAME FOR DEPTH ----------------------------------------
	//
	// Half of undoing the viewmodel depth slab; the other half widens the
	// range itself at SetViewport. See viewmodelDepth in stereo.h for why
	// neither half works alone.
	//
	// Written HERE rather than detected later, and that is the point: the
	// engine then calls PerspectiveX for the viewmodel with the world's own
	// near and far, so SetDepthRange, ApplyEyeProjection and everything else
	// downstream become consistent without any of them having to know the
	// viewmodel is special. Taken from `base` for the same reason fov is --
	// Render rescales in place, and the previous eye's value must not leak
	// into this one.
	if ( m_settings.viewmodelDepth )
	{
		vs.zNearViewmodel = base.zNear;
		vs.zFarViewmodel = base.zFar;
	}

	// Recorded as WRITTEN, so the heartbeat reports what the engine was actually
	// handed rather than what the setting asked for.
	m_lastViewFov = vs.fov;
	m_lastViewFovViewmodel = vs.fovViewmodel;

	// 6DoF. The head's world-space displacement from its recentre reference,
	// computed by VRCamera because that is what owns the room->game yaw. Added
	// before the per-eye offset so it applies whether or not eye params are up,
	// and so everything the engine derives downstream -- 3D skybox, viewmodel,
	// water, monitors -- comes from an origin that already has it in.
	//
	// This is the whole of positional tracking: owning CViewSetup turned it into
	// one addition. It is NOT collided -- leaning into a wall puts the camera
	// inside it. Portal 2 VR's VR::TraceEye is the fix and is not done yet; the
	// max-offset clamp is what keeps that bounded in the meantime.
	vs.origin.x = base.origin.x + m_positionalOffset.x;
	vs.origin.y = base.origin.y + m_positionalOffset.y;
	vs.origin.z = base.origin.z + m_positionalOffset.z;
	vs.m_vUnreflectedOrigin = vs.origin;

	// Head orientation for this pass. Normally the engine's own view angles;
	// under `aim_source = controller` the engine's are the WEAPON's, so the
	// override carries the head's and everything below -- the eye offset basis,
	// the frustum, and any cant -- has to use it rather than base.angles.
	const QAngle headAngles = m_haveViewAngleOverride ? m_viewAngleOverride : base.angles;
	vs.angles = headAngles;

	if ( !m_vr )
		return;

	const EyeParams& e = m_vr->GetEyeParams( eye );
	if ( !e.valid )
		return;

	const Vector headOrigin = vs.origin;

	// The whole eye offset, in one place. This is what the LoadMatrix detour and
	// its eight-slot re-load ring existed to approximate; here the engine derives
	// every downstream view -- 3D skybox, viewmodel, water, monitors -- from an
	// origin that is already correct, instead of us trying to catch each one on
	// its way to the material system and guess whether we had seen it before.
	Vector forward, right, up;
	AngleVectors( headAngles, forward, right, up );

	const float s = m_settings.eyeSeparationScale;
	const float dr = e.offsetRight * s;
	const float du = e.offsetUp * s;
	const float db = e.offsetBack * s;

	vs.origin.x = headOrigin.x + right.x * dr + up.x * du - forward.x * db;
	vs.origin.y = headOrigin.y + right.y * dr + up.y * du - forward.y * db;
	vs.origin.z = headOrigin.z + right.z * dr + up.z * du - forward.z * db;

	vs.m_vUnreflectedOrigin = vs.origin;

	// Canted displays: the panel is physically angled outward, so this eye's
	// frustum tangents are expressed about its own axis, not about head-forward.
	// Owning CViewSetup is what makes this expressible at all -- per-eye angles
	// were simply not available when the eye offset lived in a view matrix.
	//
	// Strictly a no-op on parallel-display headsets, which is nearly all of them,
	// so the untested path cannot affect the common case.
	//
	// The composition is additive rather than a proper rotation compose, which is
	// exact for yaw-only cant with the head level and approximate once the head
	// is rolled. Good enough to be much better than ignoring it; flagged in the
	// log so it is not mistaken for validated.
	if ( e.canted )
	{
		vs.angles.x = headAngles.x + e.pitchOffset;
		vs.angles.y = headAngles.y + e.yawOffset;
		vs.angles.z = headAngles.z + e.rollOffset;
	}
}

//-----------------------------------------------------------------------------
// What a frame actually costs.
//
// The failure this exists for is a GPU timeout: the driver resets the device
// after a single submission takes too long (2 seconds by default), DXVK reports
// VK_ERROR_DEVICE_LOST, and the render thread parks forever on a fence that will
// never signal. From inside the process that looks like a hang with no
// exception -- so the only warning available is the shape of the frames leading
// up to it, and an average frame time hides exactly those.
//-----------------------------------------------------------------------------
void StereoRenderer::LogFrameTiming() const
{
	if ( m_eyeMsSamples == 0 )
	{
		Log( "stereo frame cost: no eye passes completed in this interval -- either "
			 "not rendering, or already wedged" );
		return;
	}

	const double avg = m_eyeMsTotal / (double)m_eyeMsSamples;

	Log( "stereo frame cost: eye passes avg=%.1f ms max=%.1f ms over %llu frames "
		 "(%.0f fps at that average) | slow(>100ms)=%lu | worst ever %.1f ms",
		 avg, m_eyeMsMax, m_eyeMsSamples, avg > 0.0 ? 1000.0 / avg : 0.0,
		 m_slowFrames, m_worstEverMs );

	// The threshold is well below the driver's, because by the time a single
	// frame is near two seconds the reset has usually already happened.
	if ( m_settings.slowFrameMs > 0.0f && m_eyeMsMax > (double)m_settings.slowFrameMs )
		LogWarn( "stereo frame cost: a frame took %.0f ms. The driver resets the device "
				 "at around 2000 ms, which is the VK_ERROR_DEVICE_LOST / "
				 "LiveKernelEvent 0x141 crash. Reduce the work: "
				 "engine_portals_open_all=0 first, then engine_fov_margin, then "
				 "relax_cull_pvs=0 and relax_cull_area=0.", m_eyeMsMax );

	// Roll the interval over so the next line describes the next five seconds,
	// not the whole session.
	m_eyeMsTotal = 0.0;
	m_eyeMsMax = 0.0;
	m_eyeMsSamples = 0;
	m_slowFrames = 0;
}

void StereoRenderer::RenderBothEyes( void ( *renderOnce )( void* ctx ), void* ctx )
{
	if ( !m_ready || !renderOnce )
		return;

	// Alternate buffers so this frame never writes what the compositor is still
	// reading from the last one.
	g_bufferIndex = ( g_bufferIndex + 1 ) % kBufferCount;

	// Pushed in from the top of View_Render, not sampled here: the camera needs
	// the same answer BEFORE the eye passes run, and two GetCursorInfo calls in
	// one frame can straddle a menu opening and disagree with each other.
	g_cursorVisible = m_uiVisible;

	if ( m_tracePasses > 0 )
		Log( "[trace] --- frame %llu, cursorVisible=%d ---", m_eyeFrames, g_cursorVisible ? 1 : 0 );

	// Pin engine time for the pair, so both eyes render the same instant.
	if ( g_originalEngineTime && m_engineClient )
	{
		g_frozenTime = g_originalEngineTime( m_engineClient, nullptr );
		g_timeFrozen = m_settings.freezeTime;
	}

	// Snapshot the client's view before the first pass touches it. Render mutates
	// fov in place and we overwrite origin, so both eyes have to start from the
	// same reference and the client has to get its own view back afterwards --
	// other client code reads GetViewSetup() outside our passes.
	const bool ownView = OwnsViewSetup();
	CViewSetup baseView = {};
	if ( ownView )
		baseView = *m_viewSetup;

	const double framePassStartMs = NowMs();

	for ( int eye = 0; eye < kEyeCount; ++eye )
	{
		m_currentEye = eye;
		// Both eyes must stagger identically, or each would place a given panel
		// at a different depth and the disagreement would read as the menu
		// swimming. Reset per PASS, not per frame.
		g_menuOrthoDepthIndex = 0;

		// Measurement only. Clears the draw probe's menu flag so a pass can
		// never inherit the previous one's, and counts the pass so its draw
		// totals can be reported per pass rather than per heartbeat.
		Probe().NoteEyePass();

		// ---- FADE DETECTION, BY CALL COUNT --------------------------------
		//
		// The fade cannot be identified from Detour_Ortho's ARGUMENTS. Measured
		// on hardware: it draws through l=0 t=0 r=2206 b=2160 zn=-99999
		// zf=99999, byte-identical to the HUD's own window. Extents and z were
		// both checked and both failed, which is the whole of what that hook
		// receives.
		//
		// What it does leave is a COUNT. The HUD draws that window ~2 times per
		// eye pass; through a fade it is ~4. So an elevated count means a
		// full-screen wash is being drawn somewhere in this pass, even though
		// we cannot say which of the calls it is.
		//
		// Acting on the PREVIOUS pass's count is deliberate. By the time the
		// extra call arrives, the earlier ones in the same pass have already
		// gone through -- so the first pass of a fade is still misplaced and
		// every one after it is not. At 90 fps against a fade lasting hundreds
		// of milliseconds that is one frame in sixty, which is invisible; and
		// the alternative is predicting the future.
		//
		// This is what the transition-time workaround could not do: the game
		// fades to white BEFORE it teleports, so a trigger on the teleport
		// always misses the first half. This one keys off the fade itself.
		//
		// No feedback loop -- suspending the HUD anchor does not change how
		// many times the engine calls Ortho, so the detector cannot re-arm
		// itself. That is the test the divergence latch failed.
		{
			const long n = InterlockedExchange( &g_orthoCallsThisPass, 0 );
			if ( n >= 0 && n < kOrthoHistogramBuckets )
				++g_orthoPassHistogram[n];
			else if ( n >= kOrthoHistogramBuckets )
				++g_orthoPassHistogram[kOrthoHistogramBuckets - 1];

			if ( m_fullscreenFadeCalls > 0 && n >= m_fullscreenFadeCalls )
			{
				if ( !g_fadeActive )
					++g_fadeEpisodes;
				g_fadeActive = true;
				g_fadeHoldUntilMs = GetTickCount() + kFadeHoldMs;
			}
			else if ( g_fadeActive &&
					  (int)( GetTickCount() - g_fadeHoldUntilMs ) >= 0 )
			{
				g_fadeActive = false;
			}
		}
		m_inEyePass = true;

		if ( ownView )
			ApplyEyeToViewSetup( *m_viewSetup, baseView, eye );

		// Each eye starts fresh: the other eye's results must never be mistaken
		// for re-loads of this one's.
		ClearOffsetHistory();
		InterlockedExchange( &g_perspectiveThisPass, 0 );

		// Anything that must exist in BOTH eyes gets re-submitted here rather
		// than once per frame. See SetPreEyePass.
		if ( m_preEyePass )
			m_preEyePass( eye );

		renderOnce( ctx );

		m_inEyePass = false;


		// ---- ON TOP OF THE FINISHED IMAGE, BEFORE IT IS CAPTURED -----------
		//
		// The engine has now drawn everything for this eye, VGUI included, and
		// the backbuffer still holds it. Anything drawn here is therefore above
		// the menu and is copied into the eye surface on the next line.
		//
		// This is the only point in the frame where that is true, which is why
		// the menu cursor lives here rather than anywhere more convenient.
		if ( m_postEyePass )
			m_postEyePass( eye );

		// Snapshot the backbuffer before the next pass overwrites it.
		CaptureEye( eye );
	}

	if ( ownView )
		*m_viewSetup = baseView;

	// Both eye passes, end to end. A driver reset is not a cliff -- it is the far
	// end of a ramp, so record the worst frame rather than the average.
	{
		const double ms = NowMs() - framePassStartMs;
		m_eyeMsTotal += ms;
		++m_eyeMsSamples;
		if ( ms > m_eyeMsMax )
			m_eyeMsMax = ms;
		if ( ms > m_worstEverMs )
			m_worstEverMs = ms;
		if ( ms > 100.0 )
			++m_slowFrames;
	}

	g_timeFrozen = false;

	if ( m_tracePasses > 0 )
		--m_tracePasses;

	++m_eyeFrames;
	if ( m_eyeFrames == 1 )
		Log( "stereo: first stereo frame rendered" );
}

} // namespace sinvr
