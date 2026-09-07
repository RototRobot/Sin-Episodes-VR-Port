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

	// StretchRect handles any size difference between the backbuffer and the
	// per-eye target. In practice there is none -- the eye surfaces are created
	// to match the backbuffer exactly, so the game window size IS the per-eye
	// resolution.
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
	Stereo().ReleaseEyeSurfaces();

	HRESULT hr = g_originalReset( device, params );

	if ( SUCCEEDED( hr ) )
	{
		Log( "d3d9: device reset ok, rebuilding eye surfaces" );
		Stereo().CreateEyeSurfaces( device );
	}
	else
	{
		LogError( "d3d9: device reset failed (hr=0x%08lX)", (unsigned long)hr );
	}
	return hr;
}

HRESULT __stdcall Detour_Present( IDirect3DDevice9* device, const RECT* src, const RECT* dest,
								  HWND window, const RGNDATA* dirty )
{
	++g_presentCount;
	g_presentThreadId = GetCurrentThreadId();

	if ( g_presentCount == 1 )
		Log( "d3d9: Present hook is live (device=%p, tid=%lu)", device, g_presentThreadId );

	EnsureVRInterface( device );

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

			SubmitFrame( device );
			g_vr->BeginFrame();
		}
	}

	return g_originalPresent( device, src, dest, window, dirty );
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

bool D3D9VRInterfaceBound()
{
	return g_d3dvr != nullptr;
}

} // namespace sinvr
