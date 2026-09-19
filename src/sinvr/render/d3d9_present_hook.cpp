// Phase 4a -- get the game's frame into the headset.
//
// Two problems to solve:
//
// 1. We need the game's IDirect3DDevice9, and Source never hands it to us.
//    Solution: create a throwaway device of our own, read its vtable, and hook
//    IDirect3DDevice9::Present there. COM vtables are per-class, not
//    per-instance, so the game's device goes through the same slot -- and the
//    `this` pointer it passes us *is* the device we were looking for. The
//    throwaway device is released immediately; the vtable is static and
//    outlives it.
//
// 2. The compositor cannot take a D3D9 surface. Solution: our DXVK fork exports
//    Direct3DCreateVR9, which yields the VkImage behind any D3D9 surface. See
//    dxvk-patch/.
//
// Phase 4a submits the same backbuffer to both eyes: no stereo, but it proves
// the whole DXVK -> Vulkan -> compositor chain before the engine's render loop
// is touched.

#include "d3d9_present_hook.h"

#include <d3d9.h>
#include "d3d9_vr_iface.h"
#include "stereo.h"
#include "../../common/log.h"
#include "../../common/crash_handler.h"
#include "../hooks/vtable_hook.h"
#include "draw_probe.h"

namespace sinvr {
namespace {

// IDirect3DDevice9 vtable indices, fixed by the COM ABI.
constexpr int kResetSlot = 16;
constexpr int kPresentSlot = 17;

using PresentFn = HRESULT( __stdcall* )( IDirect3DDevice9*, const RECT*, const RECT*,
										 HWND, const RGNDATA* );
using ResetFn = HRESULT( __stdcall* )( IDirect3DDevice9*, D3DPRESENT_PARAMETERS* );
using Direct3DCreate9Fn = IDirect3D9*( __stdcall* )( UINT );

VTableHook g_presentHook;
VTableHook g_resetHook;
PresentFn g_originalPresent = nullptr;
ResetFn g_originalReset = nullptr;

IVRBackend* g_vr = nullptr;
IDirect3DVR9Mirror* g_d3dvr = nullptr;
IDirect3DDevice9* g_device = nullptr;
Direct3DCreateVR9Fn g_createVR = nullptr;

bool g_submitEnabled = true;
bool g_vrIfaceAttempted = false;
unsigned long long g_presentCount = 0;
DWORD g_presentThreadId = 0;

//-----------------------------------------------------------------------------
// Failure containment.
//
// A GPU timeout is survivable. What is not survivable is what we were doing
// afterwards: continuing to call BeginFrame and to hand the compositor VkImage
// handles belonging to a device the driver has already destroyed, once per
// Present, indefinitely. A crash log showed 27 consecutive
// VK_ERROR_DEVICE_LOST, which is 27 rounds of that, and the session that
// followed ended in a kernel bugcheck.
//
// We cannot fix a driver from user mode. We can refuse to keep feeding it.
//-----------------------------------------------------------------------------
bool g_deviceLost = false;
bool g_submitDisabledBySafety = false;
int g_consecutiveSubmitFailures = 0;
constexpr int kMaxConsecutiveSubmitFailures = 8;

void DisableSubmissionForSafety( const char* why )
{
	if ( g_submitDisabledBySafety )
		return;

	g_submitDisabledBySafety = true;
	Breadcrumb( "submission DISABLED: %s", why );
	LogError( "d3d9: VR submission DISABLED -- %s. The game keeps running on the "
			  "monitor; the headset will freeze on its last frame. This is "
			  "deliberate: submitting into a broken device is how a recoverable "
			  "timeout becomes a driver or system crash. Restart the game to retry.",
			  why );
}

//-----------------------------------------------------------------------------
// Bind IDirect3DVR9 to the device the game is actually using. Deferred to the
// first Present because that is the first moment we know which device that is.
//-----------------------------------------------------------------------------
void EnsureVRInterface( IDirect3DDevice9* device )
{
	if ( g_vrIfaceAttempted )
		return;
	g_vrIfaceAttempted = true;

	g_device = device;

	HMODULE d3d9 = GetModuleHandleA( "d3d9.dll" );
	if ( !d3d9 )
	{
		LogError( "d3d9: module not loaded -- cannot bind IDirect3DVR9" );
		return;
	}

	wchar_t path[MAX_PATH] = { 0 };
	GetModuleFileNameW( d3d9, path, MAX_PATH );
	Log( "d3d9: module at %p (%S)", d3d9, path );

	g_createVR = (Direct3DCreateVR9Fn)GetProcAddress( d3d9, "Direct3DCreateVR9" );
	if ( !g_createVR )
	{
		LogError( "d3d9: Direct3DCreateVR9 not exported -- this is not the SiN VR "
				  "DXVK build. Copy build\\d3d9.dll next to SinEpisodes.exe." );
		return;
	}

	HRESULT hr = g_createVR( device, &g_d3dvr );
	if ( FAILED( hr ) || !g_d3dvr )
	{
		LogError( "d3d9: Direct3DCreateVR9 failed (hr=0x%08lX)", (unsigned long)hr );
		g_d3dvr = nullptr;
		return;
	}

	Log( "d3d9: IDirect3DVR9 bound, device=%p", device );

	// Eye surfaces need the real device, so they are created here rather than
	// at stereo init.
	if ( Stereo().Ready() )
		Stereo().CreateEyeSurfaces( device );
}

// Copies the just-rendered backbuffer into the current eye's surface. Called
// once per eye pass, from the View_Render detour.
void CaptureEyeImpl( int eye )
{
	if ( !g_device || !Stereo().Ready() )
		return;

	IDirect3DSurface9* target = Stereo().EyeSurface( eye );
	if ( !target )
		return;

	IDirect3DSurface9* backBuffer = nullptr;
	if ( FAILED( g_device->GetBackBuffer( 0, 0, D3DBACKBUFFER_TYPE_MONO, &backBuffer ) )
		 || !backBuffer )
		return;

	// ---- HOW MUCH OF THE BACKBUFFER DID THE ENGINE ACTUALLY DRAW? -------
	//
	// The copy below takes the WHOLE backbuffer, on the assumption that the
	// engine filled the whole backbuffer. That assumption has never been
	// checked, and it is the first thing to doubt after a window resize: if the
	// engine follows WM_SIZE by shrinking its viewport, the scene lands in a
	// corner of a larger surface, the copy takes the empty part with it, and
	// the headset gets a picture whose framing no longer matches the projection
	// it was drawn with -- which looks like broken rendering AND broken
	// tracking while every other log line stays perfectly normal.
	//
	// Reported once per run, and unconditionally, because it is just as worth
	// knowing when nothing has been resized: it turns an assumption into a
	// measurement.
	{
		static bool loggedViewport = false;
		if ( !loggedViewport )
		{
			loggedViewport = true;
			D3DVIEWPORT9 vp = {};
			unsigned int bw = 0, bh = 0;
			Stereo().BackbufferSize( bw, bh );
			if ( SUCCEEDED( g_device->GetViewport( &vp ) ) )
			{
				const bool covers = ( vp.X == 0 && vp.Y == 0 &&
									  vp.Width == bw && vp.Height == bh );
				Log( "d3d9: engine viewport at capture = %lux%lu at (%lu,%lu), "
					 "backbuffer %ux%u -- %s",
					 (unsigned long)vp.Width, (unsigned long)vp.Height,
					 (unsigned long)vp.X, (unsigned long)vp.Y, bw, bh,
					 covers ? "covers it, so the whole-surface copy is right"
							: "DOES NOT COVER IT. The eye copy is taking "
							  "undrawn pixels and the headset image will be "
							  "framed wrongly." );
			}
		}
	}

	// StretchRect handles any size difference between the backbuffer and the
	// per-eye target. In practice there is none -- the eye surfaces are created
	// to match the backbuffer exactly.
	HRESULT hr = g_device->StretchRect( backBuffer, nullptr, target, nullptr, D3DTEXF_LINEAR );
	backBuffer->Release();

	static bool loggedFailure = false;
	if ( FAILED( hr ) && !loggedFailure )
	{
		LogError( "d3d9: StretchRect to eye %d failed (hr=0x%08lX)", eye, (unsigned long)hr );
		loggedFailure = true;
	}
}

//-----------------------------------------------------------------------------
// Pull the backbuffer's Vulkan handles and hand them to the compositor.
//-----------------------------------------------------------------------------
// Stereo: submit the two eye surfaces the render passes filled in.
bool SubmitStereo()
{
	if ( !Stereo().Ready() || Stereo().EyeFrameCount() == 0 )
		return false;

	IDirect3DSurface9* surfaces[kEyeCount] = { nullptr, nullptr };
	VulkanTextureDesc textures[kEyeCount];

	for ( int eye = 0; eye < kEyeCount; ++eye )
	{
		surfaces[eye] = Stereo().EyeSurface( eye );
		if ( !surfaces[eye] )
			return false;

		// waitResourceIdle=FALSE: the queue lock below plus the compositor's own
		// synchronisation already order this. Waiting here as well meant a full
		// CPU stall per eye per frame, which is what let contention build until
		// the GPU timed out.
		g_d3dvr->TransferSurface( surfaces[eye], FALSE );

		D3D9VRTextureDesc desc = {};
		if ( FAILED( g_d3dvr->GetVRDesc( surfaces[eye], &desc ) ) )
		{
			LogWarn( "d3d9: GetVRDesc failed for eye %d", eye );
			return false;
		}
		memcpy( &textures[eye], &desc, sizeof( textures[eye] ) );
	}

	// ---- NOTHING INCOMPLETE GOES TO THE RUNTIME ---------------------------
	//
	// The 2026-09-11 crash was a read of NULL-plus-0x1E0 inside NVIDIA's driver,
	// made by SteamVR while it worked on our texture. Whether a null handle in
	// this description was that NULL is not known -- but it is the one thing on
	// our side of the call that could be, and refusing it costs nothing.
	for ( int eye = 0; eye < kEyeCount; ++eye )
	{
		const VulkanTextureDesc& t = textures[eye];
		if ( !t.image || !t.device || !t.physicalDevice || !t.instance || !t.queue ||
			 !t.width || !t.height || !t.format )
		{
			static unsigned int logged = 0;
			if ( logged++ < 5 )
			{
				LogWarn( "d3d9: eye %d texture description incomplete -- image=%llu "
						 "device=%p physical=%p instance=%p queue=%p %ux%u fmt=%u. "
						 "Frame NOT submitted.",
						 eye, (unsigned long long)t.image, t.device, t.physicalDevice,
						 t.instance, t.queue, t.width, t.height, t.format );
				Breadcrumb( "frame skipped: eye %d texture description incomplete", eye );
			}
			return true;   // handled -- skipped, and NOT handed to the mono fallback
		}
	}

	// Once, on the first frame: is SteamVR on the GPU DXVK chose? See the backend.
	if ( !g_vr->CheckOutputDevice( textures[0] ) )
		return true;

	// One lock for the pair, not one per eye. LockSubmissionQueue drains the CS
	// thread, so doing it twice per frame doubled an already expensive sync.
	//
	// PreSubmit/PostSubmit are the explicit-timing handshake and also touch the
	// Vulkan queue, so they belong inside the same lock as the submits.
	g_d3dvr->LockSubmissionQueue();

	bool allSubmitted = true;
	g_vr->PreSubmit();
	for ( int eye = 0; eye < kEyeCount; ++eye )
	{
		if ( !g_vr->SubmitEye( eye, textures[eye], Stereo().GetEyeBounds( eye ) ) )
			allSubmitted = false;
	}
	g_vr->PostSubmit();

	g_d3dvr->UnlockSubmissionQueue();

	// The compositor refusing a frame is normal and transient -- losing focus to
	// the dashboard does it. A long unbroken run of refusals is not transient,
	// and retrying forever is what we are trying to stop doing.
	if ( allSubmitted )
	{
		g_consecutiveSubmitFailures = 0;
	}
	else if ( ++g_consecutiveSubmitFailures >= kMaxConsecutiveSubmitFailures )
	{
		char why[128];
		_snprintf_s( why, sizeof( why ), _TRUNCATE,
					 "%d consecutive eye submissions failed", g_consecutiveSubmitFailures );
		DisableSubmissionForSafety( why );
	}

	return true;
}

void SubmitFrame( IDirect3DDevice9* device )
{
	if ( !g_d3dvr || !g_vr || !g_vr->CanSubmit() )
		return;

	// Real stereo when the eye passes have produced surfaces; otherwise fall
	// back to the mono path so a stereo failure still leaves a usable image.
	if ( SubmitStereo() )
		return;

	IDirect3DSurface9* backBuffer = nullptr;
	HRESULT hr = device->GetBackBuffer( 0, 0, D3DBACKBUFFER_TYPE_MONO, &backBuffer );
	if ( FAILED( hr ) || !backBuffer )
	{
		LogWarn( "d3d9: GetBackBuffer failed (hr=0x%08lX)", (unsigned long)hr );
		return;
	}

	// The compositor wants TRANSFER_SRC_OPTIMAL.
	g_d3dvr->TransferSurface( backBuffer, TRUE );

	D3D9VRTextureDesc desc = {};
	hr = g_d3dvr->GetVRDesc( backBuffer, &desc );
	backBuffer->Release();

	if ( FAILED( hr ) )
	{
		LogWarn( "d3d9: GetVRDesc failed (hr=0x%08lX)", (unsigned long)hr );
		return;
	}

	VulkanTextureDesc tex;
	memcpy( &tex, &desc, sizeof( tex ) );

	// VkQueue is externally synchronised and DXVK's submit thread uses it too,
	// so it has to be held across the submit or the two race.
	g_d3dvr->LockSubmissionQueue();
	g_vr->SubmitBothEyes( tex );
	g_d3dvr->UnlockSubmissionQueue();
}

//-----------------------------------------------------------------------------
// THE DESKTOP MIRROR -- A WINDOW OF OUR OWN, NOT THE GAME'S
//
// With vr_allow_oversize_window the game window is the headset's per-eye size,
// which on most monitors is larger than the screen: it hangs off the bottom and
// right, there is no reachable title bar, and what it shows is a magnified
// corner of the frame rather than a picture of it. Every other PC VR title
// shows a small mirror instead.
//
// ---- TWO THINGS THAT DO NOT WORK, BOTH MEASURED ---------------------------
//
// 1. RESIZING THE GAME WINDOW. Tested 2026-09-10. DXVK scales on present --
//    D3D9SwapChainEx::Present reads its source rect from BackBufferWidth and
//    its destination rect from the window, every frame -- so the mirror really
//    did shrink while the render stayed full size, and every log line the mod
//    writes stayed clean. The headset image broke anyway. The draw probe found
//    why: with the window at 2092x2048 the ENGINE began setting a viewport of
//    exactly 2092x2048 into a 2444x2392 render target, ~1 per eye pass.
//
// 2. HIDING THAT RESIZE BEHIND A WINDOW SUBCLASS. Tested the same day. The
//    subclass worked -- it swallowed the notification, and the client rect
//    changed -- and the engine set the 2092x2048 viewport regardless, 796 times
//    in the first interval alone. So Source does not cache the size it is told;
//    it ASKS the window. Message interception cannot help, and no variation on
//    it will.
//
// The constraint that survives both: THE GAME WINDOW'S CLIENT RECT MUST NEVER
// CHANGE. Anything that resizes it corrupts the render, whatever the engine is
// or is not told about it.
//
// ---- WHAT WORKS INSTEAD ---------------------------------------------------
//
// Present into a different window. IDirect3DDevice9::Present takes an
// hDestWindowOverride, we already receive it in the detour, and DXVK honours
// it properly -- D3D9SwapChainEx::Present switches m_window to the override,
// UpdateWindowCtx builds a presenter for it (kept in a per-window map, so this
// costs one creation, not one per frame), and UpdatePresentRegion then takes
// its destination rect from THAT window's size.
//
// So the game window is never touched at all: same size, same position, same
// client rect, nothing for the engine to query wrongly. The frame is scaled
// into a small window of ours instead. What killed both earlier attempts is
// absent by construction rather than worked around.
//
// The mirror window is created on the thread that calls Present, which is also
// the thread that owns the game window (measured: both 3696). The engine's own
// message pump therefore services it and we need no thread and no pump.
//
// ---- AND THE BIG WINDOW IS STILL THERE ------------------------------------
//
// Presenting elsewhere does not remove the game window from the desktop; it
// just stops being where the picture goes. vr_desktop_window_hide makes it
// invisible with a layered alpha of 0, which changes neither its size nor its
// position -- so GetClientRect still answers with the render size and
// ClientToScreen still maps the menu cursor exactly as it does today. It is a
// separate switch from the mirror on purpose: the two can fail independently
// and each is one launch to check.
//-----------------------------------------------------------------------------

bool g_desktopFit = false;
int g_desktopHeight = 0;
bool g_desktopHide = false;

HWND g_gameWindow = nullptr;
HWND g_mirrorWindow = nullptr;
bool g_mirrorAttempted = false;
bool g_gameWindowHidden = false;
unsigned int g_renderW = 0;
unsigned int g_renderH = 0;
int g_mirrorW = 0;
int g_mirrorH = 0;

constexpr int kMinMirrorW = 480;
constexpr int kMinMirrorH = 270;
const wchar_t* const kMirrorClass = L"SinVRDesktopMirror";

// The client size the mirror should have, or false if there is no sensible one.
bool WantedMirrorSize( HWND reference, int renderW, int renderH, int& outW, int& outH )
{
	if ( renderW <= 0 || renderH <= 0 )
		return false;

	// An explicit height wins and is NOT clamped to the desktop: someone who
	// names a mirror size has a reason, and second-guessing it would be the mod
	// arguing with the player about their own monitor.
	if ( g_desktopHeight > 0 )
	{
		outH = g_desktopHeight;
		outW = (int)( ( (double)renderW / (double)renderH ) * (double)outH + 0.5 );
		if ( outW < kMinMirrorW ) outW = kMinMirrorW;
		if ( outH < kMinMirrorH ) outH = kMinMirrorH;
		return true;
	}

	if ( !g_desktopFit )
		return false;

	// The WORK area, not the screen: the taskbar is the difference between a
	// window that is fully visible and one whose bottom edge is covered.
	// MONITOR_DEFAULTTONEAREST keeps this right on the monitor the game
	// actually opened on, which is not necessarily the primary.
	RECT work = { 0, 0, GetSystemMetrics( SM_CXSCREEN ), GetSystemMetrics( SM_CYSCREEN ) };
	MONITORINFO mi = {};
	mi.cbSize = sizeof( mi );
	HMONITOR mon = MonitorFromWindow( reference, MONITOR_DEFAULTTONEAREST );
	if ( mon && GetMonitorInfoA( mon, &mi ) )
		work = mi.rcWork;

	// Room for our own frame, and a margin so the mirror does not sit edge to
	// edge with the screen.
	RECT frame = { 0, 0, 0, 0 };
	AdjustWindowRectEx( &frame, WS_OVERLAPPEDWINDOW, FALSE, WS_EX_NOACTIVATE );
	const LONG maxW = ( work.right - work.left ) - ( frame.right - frame.left ) - 32;
	const LONG maxH = ( work.bottom - work.top ) - ( frame.bottom - frame.top ) - 32;
	if ( maxW <= 0 || maxH <= 0 )
		return false;

	// ONE scale factor on both axes, the same discipline as the launcher's
	// clamp. The mirror is a picture of what the headset is seeing, and a
	// squashed one misreports it -- which matters most to whoever is watching
	// the monitor to judge whether something is wrong.
	double shrink = 1.0;
	const double byW = (double)maxW / (double)renderW;
	const double byH = (double)maxH / (double)renderH;
	if ( byW < shrink ) shrink = byW;
	if ( byH < shrink ) shrink = byH;

	outW = (int)( (double)renderW * shrink );
	outH = (int)( (double)renderH * shrink );
	if ( outW < kMinMirrorW ) outW = kMinMirrorW;
	if ( outH < kMinMirrorH ) outH = kMinMirrorH;
	return true;
}

LRESULT CALLBACK MirrorWndProc( HWND wnd, UINT msg, WPARAM wp, LPARAM lp )
{
	switch ( msg )
	{
	case WM_CLOSE:
		// Closing the mirror must never close the GAME. Hide it instead; the
		// present path falls back to the game window on the next frame and the
		// headset is unaffected either way.
		ShowWindow( wnd, SW_HIDE );
		Log( "d3d9: desktop mirror closed by the user -- presenting to the game "
			 "window again. The headset is unaffected." );
		return 0;

	case WM_ERASEBKGND:
		// The present covers every pixel. Erasing first only flickers.
		return 1;

	case WM_NCHITTEST:
	{
		// ---- THE PICTURE IS NOT A CONTROL --------------------------------
		//
		// MEASURED: the VR menu pointer places the OS cursor in the GAME
		// window's client coordinates and then clicks with SendInput, which
		// delivers to whatever window is topmost at that point. The mirror sits
		// on top of the game window and covers most of it -- 2043x2000 at
		// (887,16) over a game window at 2444x2392 -- so every menu click
		// inside the mirror's rectangle was swallowed by the mirror, and only
		// the ones landing outside it reached the game. That is exactly the
		// shape of "some buttons work and some do not".
		//
		// HTTRANSPARENT passes the hit through to the window underneath, which
		// is the game. Nothing else changes: the click still goes through the
		// normal path and still lands where the pointer put the cursor.
		//
		// The NON-client area is deliberately left alone. Returning
		// HTTRANSPARENT for the whole window (or setting WS_EX_TRANSPARENT,
		// which is the blunt version of this) would also kill the title bar,
		// leaving a mirror that cannot be moved or closed. Only the picture is
		// inert; the frame still behaves like a window.
		const LRESULT hit = DefWindowProcW( wnd, msg, wp, lp );
		return ( hit == HTCLIENT ) ? HTTRANSPARENT : hit;
	}

	default:
		break;
	}

	return DefWindowProcW( wnd, msg, wp, lp );
}

bool CreateMirrorWindow( HWND reference, int clientW, int clientH )
{
	if ( g_mirrorWindow )
		return true;
	if ( g_mirrorAttempted )
		return false;
	g_mirrorAttempted = true;

	HINSTANCE inst = GetModuleHandleW( nullptr );

	WNDCLASSEXW wc = {};
	wc.cbSize = sizeof( wc );
	wc.lpfnWndProc = MirrorWndProc;
	wc.hInstance = inst;
	// IDC_ARROW is MAKEINTRESOURCE, which is the ANSI flavour in this
	// build. The value is an integer resource id either way.
	wc.hCursor = LoadCursorW( nullptr, (LPCWSTR)IDC_ARROW );
	// A SYSTEM COLOUR index + 1, not a real brush handle -- that is the one
	// form WNDCLASS accepts without a GDI call, and GetStockObject would
	// pull gdi32 into the link for a brush that is never used anyway:
	// WM_ERASEBKGND is answered above and the present covers every pixel.
	wc.hbrBackground = (HBRUSH)( COLOR_WINDOWTEXT + 1 );
	wc.lpszClassName = kMirrorClass;

	if ( !RegisterClassExW( &wc ) )
	{
		const DWORD err = GetLastError();
		if ( err != ERROR_CLASS_ALREADY_EXISTS )
		{
			LogWarn( "d3d9: could not register the mirror window class (err=%lu) "
					 "-- no desktop mirror this run.", (unsigned long)err );
			return false;
		}
	}

	// WS_EX_NOACTIVATE: clicking the mirror must not take focus away from the
	// game, or the next keypress goes to a window that does nothing with it.
	//
	// WS_EX_TOOLWINDOW: and it must not appear in Alt+Tab or the taskbar
	// either. MEASURED -- without it the mirror is an ordinary top-level window
	// and Alt+Tab offers it as if it were the game. Picking it activates
	// nothing, because of NOACTIVATE, so the game never regains focus: the
	// music stops on the way out and does not come back on the way in. That is
	// the symptom that identified this.
	//
	// NOACTIVATE alone does not remove a window from Alt+Tab. TOOLWINDOW is
	// what does, and the two are needed together.
	const DWORD style = WS_OVERLAPPEDWINDOW;
	const DWORD exStyle = WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW;

	RECT outer = { 0, 0, clientW, clientH };
	AdjustWindowRectEx( &outer, style, FALSE, exStyle );
	const int outerW = outer.right - outer.left;
	const int outerH = outer.bottom - outer.top;

	int x = CW_USEDEFAULT, y = CW_USEDEFAULT;
	MONITORINFO mi = {};
	mi.cbSize = sizeof( mi );
	HMONITOR mon = MonitorFromWindow( reference, MONITOR_DEFAULTTONEAREST );
	if ( mon && GetMonitorInfoA( mon, &mi ) )
	{
		x = mi.rcWork.left + ( ( mi.rcWork.right - mi.rcWork.left ) - outerW ) / 2;
		y = mi.rcWork.top + ( ( mi.rcWork.bottom - mi.rcWork.top ) - outerH ) / 2;
	}

	g_mirrorWindow = CreateWindowExW( exStyle, kMirrorClass, L"SiN VR -- desktop view",
									  style, x, y, outerW, outerH,
									  nullptr, nullptr, inst, nullptr );
	if ( !g_mirrorWindow )
	{
		LogWarn( "d3d9: CreateWindowEx for the desktop mirror failed (err=%lu) "
				 "-- presenting to the game window as before.",
				 (unsigned long)GetLastError() );
		return false;
	}

	// SHOWNOACTIVATE, not SHOW: appearing must not steal focus from the game.
	ShowWindow( g_mirrorWindow, SW_SHOWNOACTIVATE );

	g_mirrorW = clientW;
	g_mirrorH = clientH;

	Log( "d3d9: desktop mirror window %dx%d at (%d,%d) -- Present is redirected "
		 "into it with hDestWindowOverride, so DXVK scales the frame down on the "
		 "way out. The GAME window is untouched at %ux%u, which is the whole "
		 "point: nothing the engine can query has changed.",
		 clientW, clientH, x, y, g_renderW, g_renderH );
	return true;
}

// Invisible, but the same size and in the same place. Layered alpha rather than
// SetWindowRgn or SW_HIDE, both of which stop the window being hit-tested and so
// break the menu cursor's SendInput click.
//
// ---- AND THE ALPHA IS 1, NOT 0 -------------------------------------------
//
// This was wrong in the first version and it cost a test run. Hit testing of a
// layered window follows its transparency: areas that are colour-keyed OR WHOSE
// ALPHA IS ZERO let mouse messages through to whatever is underneath. So
// alpha 0 does not merely hide the game window, it makes it click-through --
// and every menu click fell past it onto the desktop.
//
// 1/255 is 0.4% opacity. It is invisible in practice, doubly so because the mod
// no longer presents to this window at all, so there is nothing being drawn
// into it to show through. But it is not zero, which is the whole point: the
// window is still hit-tested and still receives the click.
void HideGameWindow( HWND wnd )
{
	if ( !wnd || g_gameWindowHidden || !g_desktopHide )
		return;

	const LONG_PTR ex = GetWindowLongPtrW( wnd, GWL_EXSTYLE );
	if ( !SetWindowLongPtrW( wnd, GWL_EXSTYLE, ex | WS_EX_LAYERED ) )
	{
		LogWarn( "d3d9: could not make the game window layered (err=%lu) -- it "
				 "stays visible behind the mirror.", (unsigned long)GetLastError() );
		return;
	}

	constexpr BYTE kNearlyInvisible = 1;   // NOT 0 -- see above
	if ( !SetLayeredWindowAttributes( wnd, 0, kNearlyInvisible, LWA_ALPHA ) )
	{
		LogWarn( "d3d9: SetLayeredWindowAttributes failed (err=%lu) -- the game "
				 "window stays visible behind the mirror.",
				 (unsigned long)GetLastError() );
		SetWindowLongPtrW( wnd, GWL_EXSTYLE, ex );
		return;
	}

	g_gameWindowHidden = true;

	RECT rc = {};
	GetClientRect( wnd, &rc );
	Log( "d3d9: game window hidden (layered, alpha %d of 255 -- not 0, which "
		 "would make it click-through). Its client rect is still %ldx%ld, so the "
		 "engine sees no change and the menu cursor still maps into it.",
		 (int)kNearlyInvisible, rc.right - rc.left, rc.bottom - rc.top );
}

// ---- THE MOUSE RE-CENTRE POINT MUST BE ON THE MONITOR ----------------------
//
// MEASURED 2026-09-13, desktop at 1920x1080: shots landed ~10 degrees ABOVE the
// laser dot while the aim the mod wrote to the engine was right. The game
// window's client area ran (3,26)-(2447,2418), so its centre sat at y=1222 --
// 143 px below the bottom of the desktop.
//
// Source's in-game mouse look puts the cursor back on that centre every frame
// and reads how far it moved since. Windows will not put a cursor off the
// desktop, so the re-centre landed on the bottom row instead, and the next read
// saw the mouse 143 px "up" from centre -- every frame, with nobody touching
// it. 143 px x m_pitch 0.022 x sensitivity 3 = 9.4 degrees, added to the
// usercmd AFTER the mod had written the controller's aim. The laser dot is drawn
// by the mod from the gun, so it stayed right while the bullets did not.
//
// It takes a render taller than the desktop AND a desktop short enough to put
// the centre off it, which is why it only surfaced once the desktop went to
// 1080p. On a 1440p desktop the same window's centre is on screen.
//
// The fix MOVES the window -- never resizes it; the mirror section above says
// why the size must not change -- so its centre sits in the middle of its
// monitor. Hidden, that is invisible; not hidden, the visible part becomes the
// middle of the frame instead of its top-left corner.
static bool g_keepCentred = true;
static unsigned int g_centreMoves = 0;

static void KeepMouseCentreOnScreen( HWND wnd )
{
	if ( !g_keepCentred || !wnd )
		return;

	RECT rc = {};
	if ( !GetClientRect( wnd, &rc ) )
		return;
	POINT centre = { ( rc.right - rc.left ) / 2, ( rc.bottom - rc.top ) / 2 };
	if ( !ClientToScreen( wnd, &centre ) )
		return;

	MONITORINFO mi = {};
	mi.cbSize = sizeof( mi );
	const HMONITOR mon = MonitorFromWindow( wnd, MONITOR_DEFAULTTOPRIMARY );
	if ( !mon || !GetMonitorInfoW( mon, &mi ) )
		return;
	const RECT& m = mi.rcMonitor;

	// Inside by a pixel: rcMonitor's right and bottom are exclusive, and the
	// last row is exactly where Windows parks a clamped cursor.
	if ( centre.x > m.left && centre.x < m.right - 1 &&
		 centre.y > m.top && centre.y < m.bottom - 1 )
		return;

	const int dx = (int)( ( m.left + m.right ) / 2 - centre.x );
	const int dy = (int)( ( m.top + m.bottom ) / 2 - centre.y );

	RECT wr = {};
	if ( !GetWindowRect( wnd, &wr ) )
		return;

	++g_centreMoves;
	if ( !SetWindowPos( wnd, nullptr, wr.left + dx, wr.top + dy, 0, 0,
						SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER ) )
	{
		if ( g_centreMoves <= 3 )
			LogWarn( "d3d9: could not move the game window (err=%lu) -- its centre "
					 "stays off the monitor, and the mouse re-centre keeps pushing "
					 "the aim", (unsigned long)GetLastError() );
		return;
	}

	if ( g_centreMoves <= 3 )
		Log( "d3d9: game window MOVED by (%d,%d) -- its centre was at (%ld,%ld), off "
			 "the monitor (%ld,%ld)-(%ld,%ld). Source re-centres the mouse there every "
			 "frame; Windows clamped it and the game read that as the mouse moving, "
			 "which put shots above the laser dot. Size unchanged at %ldx%ld.",
			 dx, dy, centre.x, centre.y, m.left, m.top, m.right, m.bottom,
			 rc.right - rc.left, rc.bottom - rc.top );
	else if ( g_centreMoves == 10 )
		LogWarn( "d3d9: the game window has had to be moved back on screen 10 times "
				 "-- something keeps putting it back. Moves continue, unlogged." );
}

void ApplyDesktopMirror( HWND window )
{
	if ( !window )
		return;

	// Sized against the BACKBUFFER, which is what DXVK uses as its source rect.
	// Zero until the eye surfaces are built, and there is nothing to size
	// against before then.
	unsigned int rw = 0, rh = 0;
	Stereo().BackbufferSize( rw, rh );
	if ( rw == 0 || rh == 0 )
		return;

	g_gameWindow = window;
	g_renderW = rw;
	g_renderH = rh;

	// Before the mirror's early-out: this is about the MOUSE, not the picture,
	// and a player with the mirror off has the same off-screen centre.
	KeepMouseCentreOnScreen( window );

	if ( !g_desktopFit && g_desktopHeight <= 0 )
		return;

	int wantW = 0, wantH = 0;
	if ( !WantedMirrorSize( window, (int)rw, (int)rh, wantW, wantH ) )
		return;

	if ( CreateMirrorWindow( window, wantW, wantH ) )
	{
		HideGameWindow( window );

		// The mirror appears without activating, but the act of creating a
		// top-level window is enough to disturb who is foreground. Put the game
		// back explicitly and once: it is the window that owns the input, and a
		// game that is not foreground stops its own audio.
		SetForegroundWindow( window );
		SetActiveWindow( window );
	}
}

// Where Present should put the picture. The mirror when we have a visible one,
// the game window otherwise -- including after the user closes the mirror,
// which must degrade to the old behaviour rather than to no picture at all.
HWND PresentTarget( HWND fromPresent )
{
	if ( g_mirrorWindow )
	{
		if ( !IsWindow( g_mirrorWindow ) )
			g_mirrorWindow = nullptr;
		else if ( IsWindowVisible( g_mirrorWindow ) )
			return g_mirrorWindow;
	}
	return fromPresent;
}

// Present hands us hDestWindowOverride, which is null in the ordinary case.
// The device's creation parameters carry the window Source actually made.
HWND ResolveGameWindow( IDirect3DDevice9* device, HWND fromPresent )
{
	if ( fromPresent )
		return fromPresent;
	if ( g_gameWindow )
		return g_gameWindow;
	if ( !device )
		return nullptr;

	D3DDEVICE_CREATION_PARAMETERS cp = {};
	if ( FAILED( device->GetCreationParameters( &cp ) ) )
		return nullptr;
	return cp.hFocusWindow;
}

//-----------------------------------------------------------------------------
// Device reset. CreateRenderTarget always allocates in D3DPOOL_DEFAULT, and
// default-pool resources do not survive a reset -- they must be released before
// and recreated after. Source resets on level transitions, so skipping this
// leaves us blitting into surfaces the driver no longer owns, which is how the
// GPU ends up timing out (TDR / VK_ERROR_DEVICE_LOST) rather than failing
// cleanly.
//-----------------------------------------------------------------------------
HRESULT __stdcall Detour_Reset( IDirect3DDevice9* device, D3DPRESENT_PARAMETERS* params )
{
	Log( "d3d9: device reset -- releasing eye surfaces" );
	Breadcrumb( "d3d9: device reset" );
	Stereo().ReleaseEyeSurfaces();

	// No guard against a reset caused by our own resize any more: the game
	// window is never resized. See the section above for why that is the one
	// thing this feature must not do.
	HRESULT hr = g_originalReset( device, params );

	if ( SUCCEEDED( hr ) )
	{
		Log( "d3d9: device reset ok, rebuilding eye surfaces" );
		Breadcrumb( "d3d9: device reset ok" );
		Stereo().CreateEyeSurfaces( device );
		ApplyDesktopMirror( g_gameWindow );
	}
	else
	{
		LogError( "d3d9: device reset failed (hr=0x%08lX)", (unsigned long)hr );
		Breadcrumb( "d3d9: device reset FAILED (hr=0x%08lX)", (unsigned long)hr );
	}
	return hr;
}

HRESULT __stdcall Detour_Present( IDirect3DDevice9* device, const RECT* src, const RECT* dest,
								  HWND window, const RGNDATA* dirty )
{
	++g_presentCount;
	g_presentThreadId = GetCurrentThreadId();

	if ( g_presentCount == 1 )
	{
		Log( "d3d9: Present hook is live (device=%p, tid=%lu)", device, g_presentThreadId );

		// Which rectangles the game passes decides whether shrinking the
		// desktop window stays on the fast path. DXVK treats a pDestRect that
		// is not the whole window as a partial copy and falls back to a GDI
		// StretchBlt -- correct, but far slower than the Vulkan blit. Source
		// passes null for both, and this says so out loud rather than leaving
		// it as an assumption.
		Log( "d3d9: Present rects -- src=%s dest=%s%s",
			 src ? "SET" : "null", dest ? "SET" : "null",
			 dest ? "  <-- a dest rect puts DXVK on its GDI fallback; the "
					"desktop mirror will be slow" : "" );
	}

	EnsureVRInterface( device );

	// Once the eye surfaces exist there is a render size to size the mirror
	// against. Frame 1, then every 64 -- creation is one-shot, so the repeat is
	// only there to catch a first frame that ran before the surfaces existed.
	if ( ( g_presentCount & 0x3F ) == 1 )
		ApplyDesktopMirror( ResolveGameWindow( device, window ) );

	if ( g_submitEnabled && !g_submitDisabledBySafety && g_vr && g_vr->CanSubmit() )
	{
		// Never submit from a device the driver has taken away. TestCooperativeLevel
		// is the only reliable way to ask, and DXVK implements it faithfully:
		// D3DERR_DEVICELOST after a TDR, D3DERR_DEVICENOTRESET once it can be
		// recovered. Detour_Reset handles the recovery; until then we stay out of
		// the way rather than resubmitting into the wreckage.
		const HRESULT coop = device->TestCooperativeLevel();
		if ( FAILED( coop ) )
		{
			if ( !g_deviceLost )
			{
				g_deviceLost = true;
				LogError( "d3d9: device unusable (TestCooperativeLevel=0x%08lX) -- "
						  "suspending VR submission until it resets. Check the DXVK log "
						  "for VK_ERROR_DEVICE_LOST; this is a GPU timeout (TDR).",
						  (unsigned long)coop );
			}
		}
		else
		{
			if ( g_deviceLost )
			{
				g_deviceLost = false;
				g_consecutiveSubmitFailures = 0;
				Log( "d3d9: device usable again -- resuming VR submission" );
			}

			// ---- SUBMIT FIRST, THEN WaitGetPoses --------------------------
			//
			// WaitGetPoses must still happen EVERY frame -- skipping it is what
			// left the compositor refusing us with DoNotHaveFocus, because
			// registering as a scene app is not enough and the app has to be in
			// the frame loop for focus to transfer away from SteamVR Home. That
			// is unchanged. Only the ORDER within the frame moved.
			//
			// The compositor attributes a Submit to the poses returned by the
			// MOST RECENT WaitGetPoses, and it reprojects the submitted image
			// from them. Calling WaitGetPoses first meant handing it this
			// frame's image while it held NEXT frame's poses -- so every frame
			// was reprojected through one frame of head motion that had not
			// happened yet.
			//
			// That error is a ROTATION, and it is why the symptom was so
			// specific: stick turning rotates the world and is baked into the
			// image, so reprojection never touches it and it looked perfect.
			// Physical head rotation is exactly what reprojection corrects, so
			// it took the whole error. "Only physical head rotation judders,
			// the thumbstick is fine" localised this to the submit path when
			// nothing in the pose values could.
			//
			// Submitting first hands the image over while the compositor still
			// holds the poses it was actually drawn with. WaitGetPoses then
			// fetches the next frame's, which Update() picks up at the top of
			// the following frame -- the pairing the pose cache already assumes.
			if ( !g_vr->HavePoses() )
				g_vr->BeginFrame();     // first frame: nothing to submit against yet

			// Held back while the headset sleeps, while recovering from a caught
			// fault, or on a GPU mismatch -- see the backend. The frame loop below
			// runs either way: WaitGetPoses every frame is what keeps scene focus.
			if ( !g_vr->SubmitPaused() )
				SubmitFrame( device );
			g_vr->BeginFrame();
		}
	}

	// ---- THE ONLY LINE THAT MOVES THE PICTURE ---------------------------
	//
	// hDestWindowOverride. DXVK switches its swapchain's target window to this
	// and takes the present destination rect from ITS size, so the frame is
	// scaled into the mirror while the backbuffer, the eye surfaces and the
	// image already submitted to the compositor stay at full resolution. When
	// there is no mirror this is exactly the window Present was called with.
	return g_originalPresent( device, src, dest, PresentTarget( window ), dirty );
}

//-----------------------------------------------------------------------------
// Throwaway device, created only to read the shared vtable.
//-----------------------------------------------------------------------------
bool HookPresentViaTemporaryDevice()
{
	HMODULE d3d9 = GetModuleHandleA( "d3d9.dll" );
	if ( !d3d9 )
	{
		LogError( "d3d9: not loaded yet, cannot hook Present" );
		return false;
	}

	auto create = (Direct3DCreate9Fn)GetProcAddress( d3d9, "Direct3DCreate9" );
	if ( !create )
	{
		LogError( "d3d9: Direct3DCreate9 not found" );
		return false;
	}

	IDirect3D9* d3d = create( D3D_SDK_VERSION );
	if ( !d3d )
	{
		LogError( "d3d9: Direct3DCreate9 returned null" );
		return false;
	}

	D3DPRESENT_PARAMETERS pp = {};
	pp.Windowed = TRUE;
	pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
	pp.BackBufferFormat = D3DFMT_UNKNOWN;
	pp.hDeviceWindow = GetDesktopWindow();

	IDirect3DDevice9* dummy = nullptr;
	HRESULT hr = d3d->CreateDevice( D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, pp.hDeviceWindow,
									D3DCREATE_SOFTWARE_VERTEXPROCESSING | D3DCREATE_NOWINDOWCHANGES,
									&pp, &dummy );
	if ( FAILED( hr ) || !dummy )
	{
		LogError( "d3d9: temporary device creation failed (hr=0x%08lX)", (unsigned long)hr );
		d3d->Release();
		return false;
	}

	g_originalPresent = reinterpret_cast<PresentFn>(
		g_presentHook.Install( dummy, kPresentSlot, &Detour_Present ) );
	g_originalReset = reinterpret_cast<ResetFn>(
		g_resetHook.Install( dummy, kResetSlot, &Detour_Reset ) );

	// Measurement only -- counts draws and reads back depth state, changes
	// nothing. Shares this throwaway device because the vtable is per-class.
	Probe().Install( dummy );

	// The vtable is per-class and static, so it survives these releases.
	dummy->Release();
	d3d->Release();

	if ( !g_originalPresent )
	{
		LogError( "d3d9: failed to hook Present (slot %d)", kPresentSlot );
		return false;
	}

	Log( "d3d9: hooked Present (slot %d, orig=%p) and Reset (slot %d, orig=%p)",
		 kPresentSlot, g_originalPresent, kResetSlot, g_originalReset );
	return true;
}

} // namespace

bool InstallD3D9PresentHook( IVRBackend* backend, bool submitEnabled )
{
	g_vr = backend;
	g_submitEnabled = submitEnabled;
	return HookPresentViaTemporaryDevice();
}

void CaptureEye( int eye )
{
	CaptureEyeImpl( eye );
}

IDirect3DDevice9* D3D9Device()
{
	return g_device;
}

unsigned long long D3D9PresentCount()
{
	return g_presentCount;
}

unsigned long D3D9PresentThreadId()
{
	return g_presentThreadId;
}

HWND D3D9GameWindow()
{
	return g_gameWindow;
}

HWND D3D9MirrorWindow()
{
	return g_mirrorWindow;
}

void ConfigureDesktopWindow( bool fit, int height, bool hideGameWindow, bool keepCentred )
{
	g_desktopFit = fit;
	g_desktopHeight = height;
	g_desktopHide = hideGameWindow;
	g_keepCentred = keepCentred;
	Log( "d3d9: desktop mirror -- fit=%d height=%d hide_game_window=%d "
		 "game_window_centred=%d",
		 fit ? 1 : 0, height, hideGameWindow ? 1 : 0, keepCentred ? 1 : 0 );
}

void D3D9RenderSize( unsigned int& w, unsigned int& h )
{
	Stereo().BackbufferSize( w, h );
}

bool D3D9VRInterfaceBound()
{
	return g_d3dvr != nullptr;
}

} // namespace sinvr
