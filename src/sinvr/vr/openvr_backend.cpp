// OpenVR backend.
//
// openvr_api.dll is loaded at runtime rather than linked, so a missing or broken
// SteamVR install leaves the game perfectly playable in flatscreen instead of
// failing to start. openvr.h is included only for the interface definitions and
// constants -- none of its inline helpers (VR_Init, VRSystem(), ...) are used,
// because those rely on module-local context that only exists when you link
// against the import library.

#include <windows.h>
#include <math.h>
#include <stdio.h>

#include "vr_backend.h"
#include "../../common/log.h"

// openvr.h declares the C entry points as dllimport. We never reference them
// directly -- only through GetProcAddress -- so nothing needs to be linked.
#include <openvr.h>

namespace sinvr {
namespace {

constexpr float kRadToDeg = 57.2957795131f;

//-----------------------------------------------------------------------------
// OpenVR pose -> Source pose.
//
// OpenVR: right-handed, metres, +Y up, -Z forward.
// Source: +X forward, +Y left, +Z up, ~inches.
//
// Column 2 of the tracking matrix is the device's +Z, which points *backward*
// out of the HMD. So the Source-space forward vector is:
//
//     fwd.x = -openvr.z = m[2][2]
//     fwd.y = -openvr.x = m[0][2]
//     fwd.z = -m[1][2]      (Source up is OpenVR +Y, and we negate for backward)
//
// Source yaw is atan2(fwd.y, fwd.x) and pitch is -asin(fwd.z), which reduces to
// the two expressions below. This matches the conversion used by L4D2VR and
// Portal 2 VR on the same engine family.
//-----------------------------------------------------------------------------
void MatrixToSourcePose( const vr::HmdMatrix34_t& m, float scale,
						 QAngle& angles, Vector& position )
{
	angles.x = asinf( m.m[1][2] ) * kRadToDeg;                  // pitch
	angles.y = atan2f( m.m[0][2], m.m[2][2] ) * kRadToDeg;      // yaw
	angles.z = atan2f( -m.m[1][0], m.m[1][1] ) * kRadToDeg;     // roll

	position.x = -m.m[2][3] * scale;
	position.y = -m.m[0][3] * scale;
	position.z = m.m[1][3] * scale;
}

// Same axis mapping for a free vector.
//
// vVelocity is expressed in the tracking space the transform's translation
// lives in, so it converts with exactly the columns used above -- x from -z,
// y from -x, z from +y -- and the metres-to-units scale becomes metres/s to
// units/s. Kept as its own function rather than inlined so the two can never
// disagree about which axis is which.
void HmdVectorToSource( const vr::HmdVector3_t& v, float scale, Vector& out )
{
	out.x = -v.v[2] * scale;
	out.y = -v.v[0] * scale;
	out.z = v.v[1] * scale;
}

//-----------------------------------------------------------------------------
// A bare EVRInitError number in a log from another machine is close to useless.
//-----------------------------------------------------------------------------
const char* InitErrorName( vr::EVRInitError e )
{
	switch ( e )
	{
		case vr::VRInitError_None:                        return "None";
		case vr::VRInitError_Init_InstallationNotFound:   return "Init_InstallationNotFound";
		case vr::VRInitError_Init_InstallationCorrupt:    return "Init_InstallationCorrupt";
		case vr::VRInitError_Init_VRClientDLLNotFound:    return "Init_VRClientDLLNotFound";
		case vr::VRInitError_Init_FileNotFound:           return "Init_FileNotFound";
		case vr::VRInitError_Init_FactoryNotFound:        return "Init_FactoryNotFound";
		case vr::VRInitError_Init_InterfaceNotFound:      return "Init_InterfaceNotFound";
		case vr::VRInitError_Init_InvalidInterface:       return "Init_InvalidInterface";
		case vr::VRInitError_Init_HmdNotFound:            return "Init_HmdNotFound (no headset connected)";
		case vr::VRInitError_Init_NotInitialized:         return "Init_NotInitialized";
		case vr::VRInitError_Init_PathRegistryNotFound:   return "Init_PathRegistryNotFound";
		case vr::VRInitError_Init_NoConfigPath:           return "Init_NoConfigPath";
		case vr::VRInitError_Init_NoLogPath:              return "Init_NoLogPath";
		case vr::VRInitError_Init_VRMonitorNotFound:      return "Init_VRMonitorNotFound";
		case vr::VRInitError_Init_HmdNotFoundPresenceFailed: return "Init_HmdNotFoundPresenceFailed";
		case vr::VRInitError_Init_AlreadyRunning:         return "Init_AlreadyRunning";
		case vr::VRInitError_Init_NoServerForBackgroundApp: return "Init_NoServerForBackgroundApp";
		case vr::VRInitError_Init_NotSupportedWithCompositor: return "Init_NotSupportedWithCompositor";
		case vr::VRInitError_Init_VRDashboardNotFound:    return "Init_VRDashboardNotFound";
		default:                                          return "<unmapped>";
	}
}

// Defined below, used by methods declared before it.
const char* CompositorErrorName( vr::EVRCompositorError e );

//-----------------------------------------------------------------------------
class OpenVRBackend : public IVRBackend
{
public:
	bool Init( const VRBackendSettings& settings ) override;
	void Shutdown() override;
	bool Update() override;

	const HmdPose& Hmd() const override { return m_hmd; }
	const VRInputState& Input() const override { return m_inputState; }

	const ControllerPose& Controller( int hand ) const override
	{
		return m_hands[( hand == kHandRight ) ? kHandRight : kHandLeft];
	}
	void RecommendedRenderTargetSize( unsigned int& w, unsigned int& h ) const override
	{
		w = m_recommendedW;
		h = m_recommendedH;
	}

	const ControllerPose& WeaponHand() const override
	{
		return m_hands[m_settings.leftHanded ? kHandLeft : kHandRight];
	}
	const ControllerPose& OffHand() const override
	{
		return m_hands[m_settings.leftHanded ? kHandRight : kHandLeft];
	}
	bool IsLeftHanded() const override { return m_settings.leftHanded; }
	bool ThumbsticksSwapped() const override
	{
		return m_settings.leftHanded != m_settings.swapThumbsticks;
	}
	bool StickClicksSwapped() const override
	{
		return ThumbsticksSwapped() && m_stickClicksResolved;
	}
	const char* StickClickName( int hand ) const override
	{
		if ( !m_stickClicksResolved )
			return "(not resolved)";
		return ( hand == kHandRight ) ? m_stickClickName[kHandRight]
				                              : m_stickClickName[kHandLeft];
	}
	bool IsReady() const override { return m_system != nullptr; }
	const char* Name() const override { return "OpenVR"; }
	const char* LastError() const override { return m_error; }

	bool CanSubmit() const override { return m_compositor != nullptr; }
	bool BeginFrame() override;
	void SetUseCompositorPoses( bool on ) override { m_useCompositorPoses = on; }
	bool HavePoses() const override { return m_haveRenderPoses; }
	void PredictionStats( float& mn, float& mx, float& avg ) const override
	{
		mn = ( m_predCount ? m_predMin : 0.0f );
		mx = ( m_predCount ? m_predMax : 0.0f );
		avg = ( m_predCount ? m_predSum / (float)m_predCount : 0.0f );
	}
	void PreSubmit() override;
	void PostSubmit() override;
	bool SubmitBothEyes( const VulkanTextureDesc& tex ) override;
	bool SubmitEye( int eye, const VulkanTextureDesc& tex, const EyeBounds& bounds ) override;

	const EyeParams& GetEyeParams( int eye ) const override
	{
		return m_eyes[( eye == kEyeRight ) ? kEyeRight : kEyeLeft];
	}

private:
	float PredictedSecondsToPhotons();
	void LogHeadsetInfo();
	// Measures the lens mask so the prize is known before the machinery is
	// built. See the definition.
	void LogHiddenAreaMesh();
	void CacheEyeParams();

	EyeParams m_eyes[kEyeCount];
	ControllerPose m_hands[kHandCount];

	using PFN_InitInternal2 = uint32_t( VR_CALLTYPE* )( vr::EVRInitError*, vr::EVRApplicationType, const char* );
	using PFN_ShutdownInternal = void( VR_CALLTYPE* )();
	using PFN_GetGenericInterface = void*( VR_CALLTYPE* )( const char*, vr::EVRInitError* );
	using PFN_IsHmdPresent = bool( VR_CALLTYPE* )();
	using PFN_IsRuntimeInstalled = bool( VR_CALLTYPE* )();

	HMODULE m_dll = nullptr;
	PFN_InitInternal2 m_InitInternal2 = nullptr;
	PFN_ShutdownInternal m_ShutdownInternal = nullptr;
	PFN_GetGenericInterface m_GetGenericInterface = nullptr;
	PFN_IsHmdPresent m_IsHmdPresent = nullptr;
	PFN_IsRuntimeInstalled m_IsRuntimeInstalled = nullptr;

	vr::IVRSystem* m_system = nullptr;
	vr::IVRCompositor* m_compositor = nullptr;

	// ---- controller input --------------------------------------------------
	bool InitInput();
	void UpdateInput();

	vr::IVRInput* m_input = nullptr;
	vr::VRActionSetHandle_t m_actionSet = vr::k_ulInvalidActionSetHandle;
	VRInputState m_inputState;

	struct Actions
	{
		vr::VRActionHandle_t move = vr::k_ulInvalidActionHandle;
		vr::VRActionHandle_t turn = vr::k_ulInvalidActionHandle;
		vr::VRActionHandle_t attack = vr::k_ulInvalidActionHandle;
		vr::VRActionHandle_t attack2 = vr::k_ulInvalidActionHandle;
		vr::VRActionHandle_t jump = vr::k_ulInvalidActionHandle;
		vr::VRActionHandle_t use = vr::k_ulInvalidActionHandle;
		vr::VRActionHandle_t reload = vr::k_ulInvalidActionHandle;
		vr::VRActionHandle_t crouch = vr::k_ulInvalidActionHandle;
		vr::VRActionHandle_t nextWeapon = vr::k_ulInvalidActionHandle;
		vr::VRActionHandle_t prevWeapon = vr::k_ulInvalidActionHandle;
		// (the two per-hand DEVICE handles live in m_handSource, below --
		//  they are input sources, not actions)
		vr::VRActionHandle_t flashlight = vr::k_ulInvalidActionHandle;
		vr::VRActionHandle_t menu = vr::k_ulInvalidActionHandle;
		vr::VRActionHandle_t recenter = vr::k_ulInvalidActionHandle;
		vr::VRActionHandle_t grenade = vr::k_ulInvalidActionHandle;
	} m_actions;

	// Which ACTION sits on each stick's click, discovered from the live
	// bindings rather than assumed. See DiscoverStickClicks.
	void DiscoverStickClicks();
	vr::VRActionHandle_t m_stickClick[2] = { vr::k_ulInvalidActionHandle,
				                                         vr::k_ulInvalidActionHandle };
	char m_stickClickName[2][32] = { "", "" };
	bool m_stickClicksResolved = false;

	// The two hands as INPUT SOURCES, indexed by kHandLeft / kHandRight.
	//
	// Device handles, not action handles. Passed as `ulRestrictedToDevice` to
	// ask "did THIS hand press it", which is how a control follows the weapon
	// hand without the binding file having to know which hand that is.
	// Invalid if the runtime would not give them, in which case the handed
	// reads fall back to either hand rather than to nothing.
	vr::VRInputValueHandle_t m_handSource[2] = { vr::k_ulInvalidInputValueHandle,
												 vr::k_ulInvalidInputValueHandle };

	// The runtime's recommended per-eye size, cached at startup so the mod can
	// report whether the launcher's -w/-h actually matched it.
	unsigned int m_recommendedW = 0;
	unsigned int m_recommendedH = 0;
	bool m_stickClicksLogged = false;
	unsigned int m_stickDiscoveryTries = 0;
	unsigned long long m_submitCount = 0;
	unsigned long long m_frameCount = 0;
	int m_submitErrorsLogged = 0;
	int m_waitErrorsLogged = 0;
	bool m_explicitTiming = false;

	// ---- THE COMPOSITOR'S OWN POSES, WHICH WERE BEING THROWN AWAY --------
	//
	// WaitGetPoses already returns renderPoses -- the poses the compositor has
	// predicted for the frame that is about to be drawn, on ITS cadence. They
	// were stack locals in BeginFrame and discarded, while Update() went and
	// asked for tracking a second time with a prediction interval of our own.
	//
	// Two routes to one quantity, and the wrong one was being rendered from.
	// That is lesson 25 in a new place, and here the second route is not merely
	// redundant, it is NOISY -- see PredictedSecondsToPhotons.
	vr::TrackedDevicePose_t m_renderPoses[vr::k_unMaxTrackedDeviceCount] = {};
	bool m_haveRenderPoses = false;
	bool m_useCompositorPoses = true;

	// Prediction-interval statistics, kept even when the compositor poses are
	// in use, because the SPREAD is the evidence for why this changed.
	float m_predMin = 1e9f, m_predMax = -1e9f, m_predSum = 0.0f;
	unsigned int m_predCount = 0;
	int m_timingErrorsLogged = 0;
	VRBackendSettings m_settings;
	HmdPose m_hmd;
	char m_error[256] = "not initialised";
};

//-----------------------------------------------------------------------------
// openvr_api.dll: prefer the copy shipped beside the game so the mod is
// self-contained and version-stable, then fall back to the normal search path.
//-----------------------------------------------------------------------------
HMODULE LoadOpenVRDll()
{
	wchar_t path[MAX_PATH] = { 0 };
	if ( GetModuleFileNameW( NULL, path, MAX_PATH ) != 0 )
	{
		wchar_t* slash = wcsrchr( path, L'\\' );
		if ( slash )
		{
			wcscpy_s( slash + 1, MAX_PATH - ( slash + 1 - path ), L"openvr_api.dll" );
			HMODULE local = LoadLibraryW( path );
			if ( local )
			{
				Log( "openvr: loaded local openvr_api.dll" );
				return local;
			}
		}
	}

	HMODULE any = LoadLibraryW( L"openvr_api.dll" );
	if ( any )
		Log( "openvr: loaded openvr_api.dll from system search path" );
	return any;
}

bool OpenVRBackend::Init( const VRBackendSettings& settings )
{
	m_settings = settings;
	Log( "openvr: world scale = %.2f Source units per metre", m_settings.worldScale );

	m_dll = LoadOpenVRDll();
	if ( !m_dll )
	{
		strcpy_s( m_error, "openvr_api.dll not found -- copy the 32-bit one next to SinEpisodes.exe" );
		return false;
	}

	m_InitInternal2 = (PFN_InitInternal2)GetProcAddress( m_dll, "VR_InitInternal2" );
	m_ShutdownInternal = (PFN_ShutdownInternal)GetProcAddress( m_dll, "VR_ShutdownInternal" );
	m_GetGenericInterface = (PFN_GetGenericInterface)GetProcAddress( m_dll, "VR_GetGenericInterface" );
	m_IsHmdPresent = (PFN_IsHmdPresent)GetProcAddress( m_dll, "VR_IsHmdPresent" );
	m_IsRuntimeInstalled = (PFN_IsRuntimeInstalled)GetProcAddress( m_dll, "VR_IsRuntimeInstalled" );

	if ( !m_InitInternal2 || !m_GetGenericInterface || !m_ShutdownInternal )
	{
		strcpy_s( m_error, "openvr_api.dll is missing expected exports (wrong architecture?)" );
		return false;
	}

	if ( m_IsRuntimeInstalled && !m_IsRuntimeInstalled() )
	{
		strcpy_s( m_error, "no OpenVR runtime installed" );
		return false;
	}
	if ( m_IsHmdPresent && !m_IsHmdPresent() )
	{
		strcpy_s( m_error, "no HMD detected" );
		return false;
	}

	// Submitting frames requires being the scene application. When we are not
	// submitting, _Other avoids the compositor flagging us unresponsive for
	// never producing a frame.
	const vr::EVRApplicationType appType =
		m_settings.sceneApp ? vr::VRApplication_Scene : vr::VRApplication_Other;
	Log( "openvr: initialising as %s",
		 m_settings.sceneApp ? "VRApplication_Scene" : "VRApplication_Other" );

	vr::EVRInitError err = vr::VRInitError_None;
	m_InitInternal2( &err, appType, nullptr );
	if ( err != vr::VRInitError_None )
	{
		_snprintf_s( m_error, sizeof( m_error ), _TRUNCATE,
					 "VR_InitInternal2 failed: %s (EVRInitError %d)",
					 InitErrorName( err ), (int)err );
		return false;
	}

	m_system = (vr::IVRSystem*)m_GetGenericInterface( vr::IVRSystem_Version, &err );
	if ( !m_system || err != vr::VRInitError_None )
	{
		_snprintf_s( m_error, sizeof( m_error ), _TRUNCATE,
					 "%s unavailable: %s (%d) -- SteamVR may be older than this SDK",
					 vr::IVRSystem_Version, InitErrorName( err ), (int)err );
		m_ShutdownInternal();
		m_system = nullptr;
		return false;
	}

	// The compositor is a separate interface and is only useful to a scene app.
	if ( m_settings.sceneApp )
	{
		vr::EVRInitError cerr = vr::VRInitError_None;
		m_compositor = (vr::IVRCompositor*)m_GetGenericInterface( vr::IVRCompositor_Version, &cerr );
		if ( !m_compositor )
			LogError( "openvr: %s unavailable: %s (%d) -- cannot submit frames",
					  vr::IVRCompositor_Version, InitErrorName( cerr ), (int)cerr );
		else
		{
			Log( "openvr: compositor ready (%s)", vr::IVRCompositor_Version );

			// Vulkan requires this. Without it the runtime accesses the Vulkan
			// queue from inside WaitGetPoses, on the render thread, while DXVK's
			// submit thread is using the same externally-synchronised VkQueue.
			// openvr.h:3832 -- with Explicit_ApplicationPerformsPostPresentHandoff
			// "WaitGetPoses is guaranteed not to access the queue", provided we
			// call PostPresentHandoff ourselves, which PostSubmit does.
			m_compositor->SetExplicitTimingMode(
				vr::VRCompositorTimingMode_Explicit_ApplicationPerformsPostPresentHandoff );
			m_explicitTiming = true;
			Log( "openvr: explicit timing mode enabled (app performs post-present handoff)" );
		}
	}

	strcpy_s( m_error, "none" );
	Log( "openvr: initialised, %s", vr::IVRSystem_Version );
	LogHeadsetInfo();
	CacheEyeParams();

	// Non-fatal on purpose: head tracking and stereo are worth having even with
	// no controllers bound, so a missing manifest degrades to "no controller
	// input" rather than "no VR".
	if ( m_settings.controllerInput )
		InitInput();
	else
		Log( "openvr: controller input disabled by config" );

	return true;
}

//-----------------------------------------------------------------------------
// Eye frustums and offsets are fixed for a given headset and IPD setting, so
// they are read once rather than every frame.
//-----------------------------------------------------------------------------
void OpenVRBackend::CacheEyeParams()
{
	uint32_t w = 0, h = 0;
	m_system->GetRecommendedRenderTargetSize( &w, &h );

	// Reported, not applied. The eye surfaces are created to match the game's
	// BACKBUFFER exactly (see StereoRenderer::CreateEyeSurfaces), so the per-eye
	// resolution is the game's window resolution and nothing here can change it.
	//
	// There used to be a `render_scale` setting that computed a size from this
	// recommendation. It was dead: the value it produced had no call sites, so
	// the only thing it changed was a log line claiming a resolution the mod was
	// not rendering at. Removed rather than left to mislead -- the real lever is
	// the game's -w/-h, and the aspect worth matching is this one.
	Log( "openvr: runtime recommends %ux%u per eye (aspect %.3f). Actual per-eye "
		 "resolution is the game's window size -- set it with -w/-h, and match "
		 "this aspect to stop the frustum extension cropping what you render.",
		 w, h, h > 0 ? (float)w / (float)h : 0.0f );

	for ( int eye = 0; eye < kEyeCount; ++eye )
	{
		vr::EVREye vrEye = ( eye == kEyeLeft ) ? vr::Eye_Left : vr::Eye_Right;
		EyeParams& p = m_eyes[eye];

		m_system->GetProjectionRaw( vrEye, &p.tanLeft, &p.tanRight, &p.tanTop, &p.tanBottom );

		vr::HmdMatrix34_t eyeToHead = m_system->GetEyeToHeadTransform( vrEye );

		// Column 3 is the eye's translation from head centre, in metres.
		// OpenVR eye space is +X right, +Y up, +Z back, matching Source's view
		// space basis, so these need scaling but no reordering.
		p.offsetRight = eyeToHead.m[0][3] * m_settings.worldScale;
		p.offsetUp    = eyeToHead.m[1][3] * m_settings.worldScale;
		p.offsetBack  = eyeToHead.m[2][3] * m_settings.worldScale;

		// The 3x3 rotation, which this used to throw away. Reusing
		// MatrixToSourcePose means no new sign conventions to get wrong: the
		// eye-to-head transform is in the same OpenVR head space as a tracking
		// pose, and an identity rotation yields exactly (0,0,0) through it.
		QAngle eyeAngles = { 0.0f, 0.0f, 0.0f };
		Vector ignored = { 0.0f, 0.0f, 0.0f };
		MatrixToSourcePose( eyeToHead, m_settings.worldScale, eyeAngles, ignored );

		p.pitchOffset = eyeAngles.x;
		p.yawOffset   = eyeAngles.y;
		p.rollOffset  = eyeAngles.z;

		// ---- TWO DIFFERENT THINGS LIVE IN THIS ROTATION -----------------
		//
		// The original reading was that a non-identity eye rotation means a
		// deliberately CANTED panel -- Pimax, StarVR, some Bigscreen Beyond
		// configurations. A Reverb G2 proved that wrong on 2026-08-24: it
		// reports pitch=0.02 yaw=0.11 roll=-0.08, which is not a cant. It is
		// per-device FACTORY CALIBRATION, and WMR headsets all carry some.
		//
		// Applying it is correct either way -- the runtime is stating how the
		// eye is oriented and there is no reason to argue -- so kCantEpsilon
		// stays where it is and the rotation is still used.
		//
		// What was wrong was the REPORT. Anything over a tenth of a degree
		// raised a warning saying the path had never been validated and to
		// suspect it if the eyes did not fuse, which for a tenth of a degree is
		// alarm about nothing. Below kCantVisible it is now stated as the
		// calibration it is.
		constexpr float kCantEpsilon = 0.1f;
		p.canted = ( fabsf( p.pitchOffset ) > kCantEpsilon )
				|| ( fabsf( p.yawOffset )   > kCantEpsilon )
				|| ( fabsf( p.rollOffset )  > kCantEpsilon );

		// Where a rotation stops being calibration and starts being geometry
		// worth looking at. A real canted design is several degrees; nothing
		// under a degree changes what the player sees.
		constexpr float kCantVisible = 1.0f;
		const bool cantIsLarge = ( fabsf( p.pitchOffset ) > kCantVisible )
							  || ( fabsf( p.yawOffset )   > kCantVisible )
							  || ( fabsf( p.rollOffset )  > kCantVisible );

		p.valid = true;

		Log( "openvr: eye %d tan(l=%.4f r=%.4f t=%.4f b=%.4f) offset(right=%.2f up=%.2f back=%.2f units)",
			 eye, p.tanLeft, p.tanRight, p.tanTop, p.tanBottom,
			 p.offsetRight, p.offsetUp, p.offsetBack );

		if ( p.canted && cantIsLarge )
			LogWarn( "openvr: eye %d has a CANTED display: pitch=%.2f yaw=%.2f roll=%.2f "
					 "degrees from head-forward. Per-eye view angles are applied for this, "
					 "but the composition is additive and has only been exercised at a "
					 "fraction of a degree -- if the two eyes do not fuse, this is the "
					 "first thing to suspect.",
					 eye, p.pitchOffset, p.yawOffset, p.rollOffset );
		else if ( p.canted )
			Log( "openvr: eye %d carries a small per-eye rotation: pitch=%.2f yaw=%.2f "
				 "roll=%.2f degrees. That is factory CALIBRATION, not a canted panel -- "
				 "normal on WMR headsets, applied, and far too small to see. Confirmed "
				 "fusing on a Reverb G2.",
				 eye, p.pitchOffset, p.yawOffset, p.rollOffset );
		else
			Log( "openvr: eye %d display is parallel to head-forward (no cant)", eye );
	}

	// IPD is the horizontal separation of the two eyes.
	float ipdUnits = m_eyes[kEyeRight].offsetRight - m_eyes[kEyeLeft].offsetRight;
	Log( "openvr: IPD = %.2f Source units (%.1f mm at %.2f units/m)",
		 ipdUnits, ipdUnits / m_settings.worldScale * 1000.0f, m_settings.worldScale );
}

bool OpenVRBackend::SubmitEye( int eye, const VulkanTextureDesc& tex, const EyeBounds& bounds )
{
	if ( !m_compositor )
		return false;

	vr::VRVulkanTextureData_t vkData;
	memcpy( &vkData, &tex, sizeof( vkData ) );

	vr::Texture_t texture;
	texture.handle = &vkData;
	texture.eType = vr::TextureType_Vulkan;
	texture.eColorSpace = vr::ColorSpace_Auto;

	vr::VRTextureBounds_t vrBounds;
	vrBounds.uMin = bounds.uMin;
	vrBounds.vMin = bounds.vMin;
	vrBounds.uMax = bounds.uMax;
	vrBounds.vMax = bounds.vMax;

	static bool loggedBounds = false;
	if ( !loggedBounds )
	{
		Log( "openvr: eye %d bounds u[%.4f..%.4f] v[%.4f..%.4f]",
			 eye, bounds.uMin, bounds.uMax, bounds.vMin, bounds.vMax );
		if ( eye == kEyeRight )
			loggedBounds = true;
	}

	vr::EVREye vrEye = ( eye == kEyeRight ) ? vr::Eye_Right : vr::Eye_Left;
	vr::EVRCompositorError err = m_compositor->Submit( vrEye, &texture, &vrBounds,
													   vr::Submit_Default );

	// AlreadySubmitted is not a failure. It means this compositor frame has
	// already taken an image for this eye, which happens whenever Present runs
	// more than once per WaitGetPoses. Reporting it as failure made the caller's
	// consecutive-failure safety counter trip on entirely healthy frames.
	if ( err == vr::VRCompositorError_AlreadySubmitted )
		return true;

	if ( err != vr::VRCompositorError_None )
	{
		if ( m_submitErrorsLogged < 10 )
		{
			LogError( "openvr: SubmitEye(%d) failed: %s (%d) image=%llu %ux%u fmt=%u",
					  eye, CompositorErrorName( err ), (int)err,
					  (unsigned long long)tex.image, tex.width, tex.height, tex.format );
			if ( ++m_submitErrorsLogged == 10 )
				LogError( "openvr: further Submit errors suppressed" );
		}
		return false;
	}

	if ( eye == kEyeRight )
	{
		++m_submitCount;
		if ( m_submitCount == 1 )
			Log( "openvr: first successful stereo pair submitted (%ux%u)",
				 tex.width, tex.height );
	}
	return true;
}

//-----------------------------------------------------------------------------
const char* CompositorErrorName( vr::EVRCompositorError e )
{
	switch ( e )
	{
		case vr::VRCompositorError_None:                   return "None";
		case vr::VRCompositorError_RequestFailed:          return "RequestFailed";
		case vr::VRCompositorError_IncompatibleVersion:    return "IncompatibleVersion";
		case vr::VRCompositorError_DoNotHaveFocus:         return "DoNotHaveFocus";
		case vr::VRCompositorError_InvalidTexture:         return "InvalidTexture";
		case vr::VRCompositorError_IsNotSceneApplication:  return "IsNotSceneApplication";
		case vr::VRCompositorError_TextureIsOnWrongDevice: return "TextureIsOnWrongDevice";
		case vr::VRCompositorError_TextureUsesUnsupportedFormat: return "TextureUsesUnsupportedFormat";
		case vr::VRCompositorError_SharedTexturesNotSupported: return "SharedTexturesNotSupported";
		case vr::VRCompositorError_IndexOutOfRange:        return "IndexOutOfRange";
		case vr::VRCompositorError_AlreadySubmitted:       return "AlreadySubmitted";
		case vr::VRCompositorError_InvalidBounds:          return "InvalidBounds";
		case vr::VRCompositorError_AlreadySet:             return "AlreadySet";
		default:                                           return "<unmapped>";
	}
}

//-----------------------------------------------------------------------------
// Enter the compositor's frame loop. Blocks until it is time to render.
//-----------------------------------------------------------------------------
bool OpenVRBackend::BeginFrame()
{
	if ( !m_compositor )
		return false;

	vr::TrackedDevicePose_t renderPoses[vr::k_unMaxTrackedDeviceCount];
	vr::TrackedDevicePose_t gamePoses[vr::k_unMaxTrackedDeviceCount];

	vr::EVRCompositorError err = m_compositor->WaitGetPoses(
		renderPoses, vr::k_unMaxTrackedDeviceCount,
		gamePoses, vr::k_unMaxTrackedDeviceCount );

	++m_frameCount;

	if ( err != vr::VRCompositorError_None )
	{
		if ( m_waitErrorsLogged < 10 )
		{
			LogError( "openvr: WaitGetPoses failed: %s (%d)", CompositorErrorName( err ), (int)err );
			if ( ++m_waitErrorsLogged == 10 )
				LogError( "openvr: further WaitGetPoses errors suppressed" );
		}
		return false;
	}

	// Keep them. WaitGetPoses runs at Present, so what it returns is predicted
	// for the NEXT frame to be drawn -- which is exactly the frame Update()
	// composes at the top of the following Apply(). The pairing is correct as
	// it stands; the poses simply were never carried across.
	memcpy( m_renderPoses, renderPoses, sizeof( m_renderPoses ) );
	m_haveRenderPoses = true;

	if ( m_frameCount == 1 )
		Log( "openvr: entered compositor frame loop -- scene focus should now transfer" );

	return true;
}

//-----------------------------------------------------------------------------
// Explicit timing handshake. Both of these touch the Vulkan queue, so they must
// run inside the same queue lock as the submits themselves.
//-----------------------------------------------------------------------------
void OpenVRBackend::PreSubmit()
{
	if ( !m_compositor || !m_explicitTiming )
		return;

	vr::EVRCompositorError err = m_compositor->SubmitExplicitTimingData();
	if ( err != vr::VRCompositorError_None && m_timingErrorsLogged < 5 )
	{
		LogError( "openvr: SubmitExplicitTimingData failed: %s (%d)",
				  CompositorErrorName( err ), (int)err );
		++m_timingErrorsLogged;
	}
}

void OpenVRBackend::PostSubmit()
{
	if ( !m_compositor || !m_explicitTiming )
		return;

	// Mandatory in this mode -- the runtime is relying on us to do it.
	m_compositor->PostPresentHandoff();
}

//-----------------------------------------------------------------------------
// Hands one Vulkan image to both eyes.
//
// VulkanTextureDesc is laid out to match vr::VRVulkanTextureData_t exactly, so
// this is a straight reinterpret rather than a field-by-field copy -- the
// static_assert below is what keeps that honest.
//-----------------------------------------------------------------------------
bool OpenVRBackend::SubmitBothEyes( const VulkanTextureDesc& tex )
{
	static_assert( sizeof( VulkanTextureDesc ) == sizeof( vr::VRVulkanTextureData_t ),
				   "VulkanTextureDesc must match vr::VRVulkanTextureData_t" );

	if ( !m_compositor )
		return false;

	if ( tex.image == 0 || tex.device == nullptr || tex.queue == nullptr )
	{
		LogWarn( "openvr: refusing to submit an incomplete texture desc" );
		return false;
	}

	vr::VRVulkanTextureData_t vkData;
	memcpy( &vkData, &tex, sizeof( vkData ) );

	vr::Texture_t texture;
	texture.handle = &vkData;
	texture.eType = vr::TextureType_Vulkan;
	texture.eColorSpace = vr::ColorSpace_Auto;

	// Whole image to each eye. Stereo will replace this with per-eye textures.
	vr::EVRCompositorError le = m_compositor->Submit( vr::Eye_Left, &texture, nullptr,
													  vr::Submit_Default );
	vr::EVRCompositorError re = m_compositor->Submit( vr::Eye_Right, &texture, nullptr,
													  vr::Submit_Default );

	++m_submitCount;

	if ( le != vr::VRCompositorError_None || re != vr::VRCompositorError_None )
	{
		// A failing submit fails every frame; logging all of them would bury
		// everything else in the file.
		if ( m_submitErrorsLogged < 10 )
		{
			LogError( "openvr: Submit failed left=%s(%d) right=%s(%d) image=%llu %ux%u fmt=%u samples=%u",
					  CompositorErrorName( le ), (int)le,
					  CompositorErrorName( re ), (int)re,
					  (unsigned long long)tex.image,
					  tex.width, tex.height, tex.format, tex.sampleCount );
			if ( ++m_submitErrorsLogged == 10 )
				LogError( "openvr: further Submit errors suppressed" );
		}
		return false;
	}

	if ( m_submitCount == 1 )
		Log( "openvr: first successful Submit -- image=%llu %ux%u fmt=%u samples=%u",
			 (unsigned long long)tex.image, tex.width, tex.height,
			 tex.format, tex.sampleCount );

	LogTrace( "openvr: submitted frame %llu", m_submitCount );
	return true;
}

void OpenVRBackend::LogHeadsetInfo()
{
	char buf[256] = { 0 };
	vr::ETrackedPropertyError perr = vr::TrackedProp_Success;

	m_system->GetStringTrackedDeviceProperty( vr::k_unTrackedDeviceIndex_Hmd,
											  vr::Prop_TrackingSystemName_String,
											  buf, sizeof( buf ), &perr );
	Log( "openvr: tracking system = %s", perr == vr::TrackedProp_Success ? buf : "?" );

	m_system->GetStringTrackedDeviceProperty( vr::k_unTrackedDeviceIndex_Hmd,
											  vr::Prop_ModelNumber_String,
											  buf, sizeof( buf ), &perr );
	Log( "openvr: headset = %s", perr == vr::TrackedProp_Success ? buf : "?" );

	uint32_t w = 0, h = 0;
	m_system->GetRecommendedRenderTargetSize( &w, &h );
	m_recommendedW = w;
	m_recommendedH = h;
	Log( "openvr: recommended per-eye render target = %ux%u", w, h );

	float hz = m_system->GetFloatTrackedDeviceProperty( vr::k_unTrackedDeviceIndex_Hmd,
														vr::Prop_DisplayFrequency_Float, &perr );
	Log( "openvr: display frequency = %.2f Hz", hz );

	LogHiddenAreaMesh();
}

//-----------------------------------------------------------------------------
// How much of each eye texture is behind the lens mask?
//
// The runtime knows which pixels of the render target the player CANNOT see --
// the corners, cut off by the lens. Drawing them is pure waste, and the usual
// VR optimisation is to write that mesh into the depth buffer before the scene
// so the GPU rejects those fragments early.
//
// This measures the prize before anyone builds the machinery, because the
// machinery is not free: it means writing depth mid-frame, on a device the
// engine owns, and stopping the engine clearing it afterwards. Worth doing for
// 15% of the fill; not worth doing for 2%, and not worth doing at all if the
// frame turns out to be CPU-bound, where saving fragments buys nothing.
//
// Read-only and costs one call per eye at startup.
//-----------------------------------------------------------------------------
void OpenVRBackend::LogHiddenAreaMesh()
{
	if ( !m_system )
		return;

	for ( int e = 0; e < 2; ++e )
	{
		const vr::EVREye eye = ( e == 0 ) ? vr::Eye_Left : vr::Eye_Right;
		vr::HiddenAreaMesh_t mesh =
			m_system->GetHiddenAreaMesh( eye, vr::k_eHiddenAreaMesh_Standard );

		if ( mesh.unTriangleCount == 0 || mesh.pVertexData == nullptr )
		{
			Log( "openvr: hidden area mesh -- %s eye reports NONE. Nothing to "
				 "reclaim here; this headset either has no lens mask or the "
				 "runtime does not expose one.", e == 0 ? "left" : "right" );
			continue;
		}

		// The vertices are in render-target UV space, so the summed triangle
		// area IS the fraction of the eye texture that is never seen.
		double area = 0.0;
		for ( uint32_t t = 0; t < mesh.unTriangleCount; ++t )
		{
			const vr::HmdVector2_t& a = mesh.pVertexData[t * 3 + 0];
			const vr::HmdVector2_t& b = mesh.pVertexData[t * 3 + 1];
			const vr::HmdVector2_t& c = mesh.pVertexData[t * 3 + 2];
			const double cross = ( (double)b.v[0] - a.v[0] ) * ( (double)c.v[1] - a.v[1] )
							   - ( (double)c.v[0] - a.v[0] ) * ( (double)b.v[1] - a.v[1] );
			area += ( cross < 0.0 ? -cross : cross ) * 0.5;
		}

		Log( "openvr: hidden area mesh -- %s eye, %u triangles covering %.1f%% "
			 "of the eye texture. That is the CEILING on what a depth-prepass "
			 "mask could save, and only if the frame is fill-bound.",
			 e == 0 ? "left" : "right", mesh.unTriangleCount, area * 100.0 );
	}
}

//-----------------------------------------------------------------------------
// Standard photon prediction: how far ahead of now the pose should be sampled
// so it is correct at the moment the light actually reaches the eye.
//-----------------------------------------------------------------------------
float OpenVRBackend::PredictedSecondsToPhotons()
{
	vr::ETrackedPropertyError perr = vr::TrackedProp_Success;

	float displayHz = m_system->GetFloatTrackedDeviceProperty(
		vr::k_unTrackedDeviceIndex_Hmd, vr::Prop_DisplayFrequency_Float, &perr );
	if ( perr != vr::TrackedProp_Success || displayHz <= 1.0f )
		return 0.016f; // sane fallback rather than a divide by zero

	float frameDuration = 1.0f / displayHz;

	float secondsSinceVsync = 0.0f;
	m_system->GetTimeSinceLastVsync( &secondsSinceVsync, nullptr );

	float vsyncToPhotons = m_system->GetFloatTrackedDeviceProperty(
		vr::k_unTrackedDeviceIndex_Hmd, vr::Prop_SecondsFromVsyncToPhotons_Float, &perr );
	if ( perr != vr::TrackedProp_Success )
		vsyncToPhotons = 0.0f;

	return frameDuration - secondsSinceVsync + vsyncToPhotons;
}

//-----------------------------------------------------------------------------
// Controller input, via OpenVR's action system.
//
// Actions rather than raw buttons because that is the only way SteamVR will let
// a player rebind anything -- the manifest is what populates the controller
// binding UI. It also means one mapping works across wands, Touch, Index and
// Cosmos without the mod knowing which is attached.
//
// The manifest is loaded from `actions\` beside the exe. SetActionManifestPath
// wants an absolute path and must be called before the first UpdateActionState,
// so this runs during Init.
//-----------------------------------------------------------------------------
bool OpenVRBackend::InitInput()
{
	if ( !m_GetGenericInterface )
		return false;

	vr::EVRInitError err = vr::VRInitError_None;
	m_input = (vr::IVRInput*)m_GetGenericInterface( vr::IVRInput_Version, &err );
	if ( !m_input || err != vr::VRInitError_None )
	{
		LogWarn( "openvr: IVRInput unavailable (%s) -- controller input disabled",
				 InitErrorName( err ) );
		m_input = nullptr;
		return false;
	}

	// <exe dir>\actions\sinvr_actions.json
	wchar_t exe[MAX_PATH] = { 0 };
	GetModuleFileNameW( NULL, exe, MAX_PATH );
	wchar_t* slash = wcsrchr( exe, L'\\' );
	if ( slash )
		*( slash + 1 ) = 0;

	wchar_t manifestW[MAX_PATH] = { 0 };
	wcscpy_s( manifestW, MAX_PATH, exe );
	wcscat_s( manifestW, MAX_PATH, L"actions\\sinvr_actions.json" );

	if ( GetFileAttributesW( manifestW ) == INVALID_FILE_ATTRIBUTES )
	{
		LogWarn( "openvr: no action manifest at %S -- controller input disabled. "
				 "Copy the mod's actions\\ folder next to the exe.", manifestW );
		m_input = nullptr;
		return false;
	}

	char manifest[MAX_PATH * 2] = { 0 };
	WideCharToMultiByte( CP_UTF8, 0, manifestW, -1, manifest, sizeof( manifest ), NULL, NULL );

	vr::EVRInputError ie = m_input->SetActionManifestPath( manifest );
	if ( ie != vr::VRInputError_None )
	{
		LogWarn( "openvr: SetActionManifestPath failed (%d) for %s", (int)ie, manifest );
		m_input = nullptr;
		return false;
	}

	ie = m_input->GetActionSetHandle( "/actions/sinvr", &m_actionSet );
	if ( ie != vr::VRInputError_None || m_actionSet == vr::k_ulInvalidActionSetHandle )
	{
		LogWarn( "openvr: GetActionSetHandle(/actions/sinvr) failed (%d)", (int)ie );
		m_input = nullptr;
		return false;
	}

	// A handle that fails to resolve stays invalid and its action simply never
	// fires, so one typo in the manifest costs that action and nothing else.
	struct { const char* path; vr::VRActionHandle_t* out; } wanted[] = {
		{ "/actions/sinvr/in/Move",       &m_actions.move },
		{ "/actions/sinvr/in/Turn",       &m_actions.turn },
		{ "/actions/sinvr/in/Attack",     &m_actions.attack },
		{ "/actions/sinvr/in/Attack2",    &m_actions.attack2 },
		{ "/actions/sinvr/in/Jump",       &m_actions.jump },
		{ "/actions/sinvr/in/Use",        &m_actions.use },
		{ "/actions/sinvr/in/Reload",     &m_actions.reload },
		{ "/actions/sinvr/in/Crouch",     &m_actions.crouch },
		{ "/actions/sinvr/in/NextWeapon", &m_actions.nextWeapon },
		{ "/actions/sinvr/in/PrevWeapon", &m_actions.prevWeapon },
		{ "/actions/sinvr/in/Flashlight", &m_actions.flashlight },
		{ "/actions/sinvr/in/Grenade", &m_actions.grenade },
		{ "/actions/sinvr/in/Menu",       &m_actions.menu },
		{ "/actions/sinvr/in/Recenter",   &m_actions.recenter },
	};

	int failed = 0;
	for ( const auto& w : wanted )
	{
		if ( m_input->GetActionHandle( w.path, w.out ) != vr::VRInputError_None ||
			 *w.out == vr::k_ulInvalidActionHandle )
		{
			LogWarn( "openvr: action handle failed for %s", w.path );
			++failed;
		}
	}

	// ---- THE TWO HANDS, AS INPUT SOURCES -----------------------------------
	//
	// Not action handles -- DEVICE handles. Passing one to GetDigitalActionData
	// as `ulRestrictedToDevice` asks "is this action active ON THIS HAND", which
	// is the question a handed control actually poses.
	//
	// This is what the grip needed all along. The old answer was to mirror two
	// action HANDLES by handedness, which meant a static binding file had to
	// carry the same control twice under two names -- and when one of those
	// names lost its binding, the mirror swapped in a dead action and the
	// holsters died silently. Restricting by device asks the runtime instead of
	// encoding the answer, so there is nothing to fall out of step.
	if ( m_input->GetInputSourceHandle( "/user/hand/left", &m_handSource[kHandLeft] )
			 != vr::VRInputError_None ||
		 m_input->GetInputSourceHandle( "/user/hand/right", &m_handSource[kHandRight] )
			 != vr::VRInputError_None )
	{
		m_handSource[kHandLeft] = vr::k_ulInvalidInputValueHandle;
		m_handSource[kHandRight] = vr::k_ulInvalidInputValueHandle;
		LogWarn( "openvr: per-hand input sources unavailable -- the holster grip "
				 "will answer to EITHER hand rather than the weapon hand" );
	}

	Log( "openvr: input ready, %d/%d actions bound from %s",
		 (int)( sizeof( wanted ) / sizeof( wanted[0] ) ) - failed,
		 (int)( sizeof( wanted ) / sizeof( wanted[0] ) ), manifest );
	return true;
}

//-----------------------------------------------------------------------------
// Which action does each stick's CLICK carry?
//
// The click on a stick should travel with that stick when the sticks swap.
// WHICH action that is varies by controller, and it is a property of the
// BINDING rather than of the manifest -- so it cannot be hard-coded without
// being wrong somewhere. The shipped files prove the point:
//
//   Knuckles / Touch / Cosmos   left click = Menu    right click = Recenter
//   Vive wands                  left click = Crouch  right click = Jump
//
// Wands have eight physical inputs to cover more actions than that, so
// Recenter is not bound on them at all. A hard-coded Menu/Recenter swap
// would there read Menu from an unbound action and cost the player their
// menu button, to move a click that was never on a stick.
//
// So ask the runtime. GetActionBindingInfo reports the device, input path,
// mode and slot each action is really bound to, which makes "the click on
// the left stick" a question with a real answer on any controller -- and one
// that stays right when a player rebinds in SteamVR, which a hard-coded pair
// would silently invalidate.
//
// DEFERRED, NEVER REFUSED. Bindings are not loaded for the first frames, so
// an early call legitimately returns nothing. This project has already lost
// a session to the opposite habit -- the viewmodel was dead because
// GetMaxEntities was sanity-checked before any map, read 0, and latched a
// permanent refusal. This retries until it succeeds and only then latches.
void OpenVRBackend::DiscoverStickClicks()
{
	if ( m_stickClicksResolved || !m_input )
		return;

	// Bounded so a controller that never reports a stick does not retry
	// forever at 90 Hz. Generous: this is frames, and bindings can take a
	// second or two to arrive after the manifest is set.
	if ( ++m_stickDiscoveryTries > 900 )
	{
		if ( !m_stickClicksLogged )
		{
			m_stickClicksLogged = true;
			Log( "sticks: no stick CLICK found on either hand after %u tries -- "
					  "the sticks will still swap, the clicks will not",
					  m_stickDiscoveryTries );
		}
		return;
	}

	struct Named
	{
		const char* name;
		vr::VRActionHandle_t handle;
	};
	const Named candidates[] = {
		{ "Menu", m_actions.menu },       { "Recenter", m_actions.recenter },
		{ "Crouch", m_actions.crouch },   { "Jump", m_actions.jump },
		{ "Use", m_actions.use },         { "Reload", m_actions.reload },
		{ "NextWeapon", m_actions.nextWeapon },
		{ "PrevWeapon", m_actions.prevWeapon },
		{ "Flashlight", m_actions.flashlight },
		{ "Grenade", m_actions.grenade },
	};

	// Case-insensitive substring. The exact strings the runtime returns are
	// not specified by the header, so match tolerantly and LOG what came
	// back -- reading the header alone has been wrong here before.
	const auto has = []( const char* haystack, const char* needle ) -> bool {
		if ( !haystack || !needle || !*needle )
			return false;
		for ( const char* p = haystack; *p; ++p )
		{
			const char* a = p;
			const char* b = needle;
			while ( *a && *b &&
				  ( *a | 0x20 ) == ( *b | 0x20 ) )
			{
				++a;
				++b;
			}
			if ( !*b )
				return true;
		}
		return false;
	};

	vr::VRActionHandle_t found[2] = { vr::k_ulInvalidActionHandle,
					  vr::k_ulInvalidActionHandle };
	const char* foundName[2] = { "", "" };
	bool sawAnyBinding = false;

	const char* unbound[16] = {};
	int unboundCount = 0;

	for ( const Named& cand : candidates )
	{
		if ( cand.handle == vr::k_ulInvalidActionHandle )
			continue;

		vr::InputBindingInfo_t info[8] = {};
		uint32_t returned = 0;
		if ( m_input->GetActionBindingInfo( cand.handle, info, sizeof( info[0] ),
					                                    8, &returned ) !=
				 vr::VRInputError_None )
			continue;

		// An action DECLARED in the manifest but bound to nothing in the
		// active profile. Collected here because this loop already asks the
		// runtime the exact question, and reported below -- see the note at
		// the report itself for why it is worth a log line.
		if ( returned == 0 && unboundCount < 16 )
			unbound[unboundCount++] = cand.name;

		for ( uint32_t i = 0; i < returned; ++i )
		{
			sawAnyBinding = true;

			// Logged once, in full, because these strings are the evidence
			// for every match below and are not documented anywhere.
			LogDebug( "sticks: %-11s dev='%s' input='%s' mode='%s' slot='%s' type='%s'",
					  cand.name, info[i].rchDevicePathName, info[i].rchInputPathName,
					  info[i].rchModeName, info[i].rchSlotName,
					  info[i].rchInputSourceType );

			// A stick by any of its names, and specifically its CLICK.
			// Trackpads count: on wands that is the stick.
			const bool onStick =
				has( info[i].rchInputPathName, "joystick" ) ||
				has( info[i].rchInputPathName, "thumbstick" ) ||
				has( info[i].rchInputPathName, "trackpad" ) ||
				has( info[i].rchInputSourceType, "joystick" ) ||
				has( info[i].rchInputSourceType, "trackpad" );
			// The stick's PRESS, by whichever activation. A double-click is
			// still the stick being pressed -- matching only "click" would
			// silently fail to find an action bound that way, and the swap
			// takes both clicks or neither, so one miss disables it entirely.
			const bool onPress =
				has( info[i].rchSlotName, "click" ) ||
				has( info[i].rchSlotName, "double" ) ||
				has( info[i].rchSlotName, "long" );
			if ( !onStick || !onPress )
				continue;

			const int hand = has( info[i].rchDevicePathName, "/right" )
					  ? kHandRight
					  : ( has( info[i].rchDevicePathName, "/left" )
						  ? kHandLeft : -1 );
			if ( hand < 0 )
				continue;
			if ( found[hand] == vr::k_ulInvalidActionHandle )
			{
				found[hand] = cand.handle;
				foundName[hand] = cand.name;
			}
		}
	}

	// BOTH, or neither. One stick click swapping into an action the other
	// stick does not carry would put two functions on one button and strand
	// the other -- worse than not swapping at all.
	if ( found[kHandLeft] == vr::k_ulInvalidActionHandle ||
		 found[kHandRight] == vr::k_ulInvalidActionHandle )
	{
		if ( sawAnyBinding && !m_stickClicksLogged && m_stickDiscoveryTries > 120 )
		{
			m_stickClicksLogged = true;
			Log( "sticks: bindings are loaded but only %s stick carries a click "
					  "-- sticks will swap, clicks will not",
					  ( found[kHandLeft] != vr::k_ulInvalidActionHandle ) ? "the left"
						  : ( found[kHandRight] != vr::k_ulInvalidActionHandle )
							  ? "the right" : "neither" );
		}
		return;
	}

	m_stickClick[kHandLeft] = found[kHandLeft];
	m_stickClick[kHandRight] = found[kHandRight];
	strncpy_s( m_stickClickName[kHandLeft], foundName[kHandLeft], _TRUNCATE );
	strncpy_s( m_stickClickName[kHandRight], foundName[kHandRight], _TRUNCATE );
	m_stickClicksResolved = true;
	Log( "sticks: stick clicks resolved from the live bindings -- "
			  "left=%s right=%s (after %u frames)",
			  m_stickClickName[kHandLeft], m_stickClickName[kHandRight],
			  m_stickDiscoveryTries );

	// ---- ACTIONS THE PROFILE BINDS TO NOTHING ------------------------------
	//
	// This exists because an unbound action once cost a whole feature silently.
	// `Crouch` moved off the grip onto the turn stick, its binding was removed
	// from all four profiles, and the handedness mirror `pick(NextWeapon,
	// Crouch)` was left in place -- so in left-handed mode the grip read a
	// permanently dead action and the HOLSTERS stopped working. Nothing failed,
	// nothing logged, and the zones kept reporting themselves as healthy
	// because they were: the button was the part that had gone.
	//
	// A declared-but-unbound action is not automatically a bug -- it is a
	// perfectly normal way to retire one. It IS a bug the moment code still
	// reads it. So this reports rather than warns, and the pairing in
	// UpdateInput is what has to stay honest about it.
	if ( unboundCount > 0 )
	{
		char list[256] = {};
		for ( int i = 0; i < unboundCount; ++i )
		{
			if ( i )
				strncat_s( list, ", ", _TRUNCATE );
			strncat_s( list, unbound[i], _TRUNCATE );
		}
		Log( "bindings: %d action(s) declared in the manifest but bound to "
			 "NOTHING in this profile: %s. That is fine for a retired action "
			 "and a silent dead end for anything still reading it -- check "
			 "UpdateInput does not pick() against these.",
			 unboundCount, list );
	}
}

void OpenVRBackend::UpdateInput()
{
	m_inputState = VRInputState();

	if ( !m_input || m_actionSet == vr::k_ulInvalidActionSetHandle )
		return;

	vr::VRActiveActionSet_t active = {};
	active.ulActionSet = m_actionSet;
	active.ulRestrictedToDevice = vr::k_ulInvalidInputValueHandle;

	const vr::EVRInputError ie =
		m_input->UpdateActionState( &active, sizeof( active ), 1 );
	if ( ie != vr::VRInputError_None )
	{
		LogTrace( "openvr: UpdateActionState failed (%d)", (int)ie );
		return;
	}

	// Held: current state, whatever it is.
	const auto digitalHeld = [&]( vr::VRActionHandle_t h ) -> bool {
		if ( h == vr::k_ulInvalidActionHandle )
			return false;
		vr::InputDigitalActionData_t d = {};
		if ( m_input->GetDigitalActionData( h, &d, sizeof( d ),
											vr::k_ulInvalidInputValueHandle ) !=
			 vr::VRInputError_None )
			return false;
		return d.bActive && d.bState;
	};

	// Pressed: rising edge only. bChanged is relative to the previous
	// UpdateActionState, so this is exactly one true per physical press.
	const auto digitalPressed = [&]( vr::VRActionHandle_t h ) -> bool {
		if ( h == vr::k_ulInvalidActionHandle )
			return false;
		vr::InputDigitalActionData_t d = {};
		if ( m_input->GetDigitalActionData( h, &d, sizeof( d ),
											vr::k_ulInvalidInputValueHandle ) !=
			 vr::VRInputError_None )
			return false;
		return d.bActive && d.bState && d.bChanged;
	};

	// Pressed, but only when the press came from ONE hand.
	//
	// Falls back to either hand if the source handles never resolved, because a
	// control that answers the wrong hand is a nuisance and a control that
	// answers no hand is a dead feature -- and this project has already paid
	// once for latching a refusal instead of degrading.
	const auto digitalPressedOnHand = [&]( vr::VRActionHandle_t h, int hand ) -> bool {
		if ( h == vr::k_ulInvalidActionHandle )
			return false;
		const vr::VRInputValueHandle_t dev =
			( hand == kHandLeft || hand == kHandRight )
				? m_handSource[hand] : vr::k_ulInvalidInputValueHandle;
		vr::InputDigitalActionData_t d = {};
		if ( m_input->GetDigitalActionData( h, &d, sizeof( d ), dev ) !=
			 vr::VRInputError_None )
			return false;
		return d.bActive && d.bState && d.bChanged;
	};

	// Held, restricted to one hand. The `hold` grip mode needs the state, not
	// the edge, and the same fallback rule applies: an unresolved source handle
	// answers either hand rather than none.
	const auto digitalHeldOnHand = [&]( vr::VRActionHandle_t h, int hand ) -> bool {
		if ( h == vr::k_ulInvalidActionHandle )
			return false;
		const vr::VRInputValueHandle_t dev =
			( hand == kHandLeft || hand == kHandRight )
				? m_handSource[hand] : vr::k_ulInvalidInputValueHandle;
		vr::InputDigitalActionData_t d = {};
		if ( m_input->GetDigitalActionData( h, &d, sizeof( d ), dev ) !=
			 vr::VRInputError_None )
			return false;
		return d.bActive && d.bState;
	};

	const auto analog = [&]( vr::VRActionHandle_t h, float& x, float& y ) {
		x = 0.0f;
		y = 0.0f;
		if ( h == vr::k_ulInvalidActionHandle )
			return;
		vr::InputAnalogActionData_t a = {};
		if ( m_input->GetAnalogActionData( h, &a, sizeof( a ),
										   vr::k_ulInvalidInputValueHandle ) !=
			 vr::VRInputError_None )
			return;
		if ( !a.bActive )
			return;
		x = a.x;
		y = a.y;
	};

	// ---- left-handed mirroring ---------------------------------------------
	//
	// The manifest binds every action to one PHYSICAL hand, so swapping which
	// hand aims is only half of being left-handed: the trigger under your
	// shooting finger was still alt-fire, and the stick under your aiming thumb
	// still turned instead of moving.
	//
	// Swap the ACTION HANDLE, not the resulting flag. The first version swapped
	// the booleans afterwards, which quietly mixed two different kinds of value:
	// `use` is a HELD flag driving +use/-use, while `flashlight` is a rising
	// EDGE driving a one-shot impulse. Exchanging those gave a `use` true for a
	// single frame and a `flashlight` that re-fired every frame it was held.
	// Swapping the handle keeps held-vs-pressed attached to the MEANING and
	// only changes which physical button feeds it.
	//
	// Pairs are the buttons that occupy mirrored positions on the two hands.
	const bool lh = m_settings.leftHanded;
	const auto pick = [lh]( vr::VRActionHandle_t rightHanded,
							vr::VRActionHandle_t mirrored ) {
		return lh ? mirrored : rightHanded;
	};

	// ---- thumbstick swap ---------------------------------------------
	//
	// A second, independent axis that COMPOSES with handedness by XOR.
	// `left_handed` mirrors every pair at once, so it already moves the
	// sticks along with the triggers and buttons; this flips the stick
	// group once more on top of that:
	//
	//   left_handed=0 swap=0   move LEFT  turn RIGHT
	//   left_handed=0 swap=1   move RIGHT turn LEFT
	//   left_handed=1 swap=0   move RIGHT turn LEFT
	//   left_handed=1 swap=1   move LEFT  turn RIGHT, triggers still mirrored
	//
	// XOR rather than an override is what makes the two commutative: both
	// readings of "swap the thumbsticks" land on the same control, so
	// neither setting has to know the other exists.
	DiscoverStickClicks();
	const bool stickSwap = ( m_settings.leftHanded != m_settings.swapThumbsticks );
	const auto pickStick = [stickSwap]( vr::VRActionHandle_t rightHanded,
				                                   vr::VRActionHandle_t mirrored ) {
		return stickSwap ? mirrored : rightHanded;
	};

	// The stick CLICKS trade places with their sticks. Applied as a remap
	// of whichever two actions the live bindings put there -- Menu and
	// Recenter on stick controllers, Crouch and Jump on Vive wands -- so
	// this needs no per-controller special case and leaves every action
	// that is not on a stick click exactly where it was. Composes AFTER
	// the handedness mirror: `pick` decides which action a slot reads,
	// then this moves the two stick clicks.
	//
	// A no-op until discovery succeeds, so an unresolved binding costs the
	// click swap and never the button.
	const bool clickSwap = stickSwap && m_stickClicksResolved;
	const auto stickClick = [&]( vr::VRActionHandle_t h ) {
		if ( !clickSwap )
			return h;
		if ( h == m_stickClick[kHandLeft] )
			return m_stickClick[kHandRight];
		if ( h == m_stickClick[kHandRight] )
			return m_stickClick[kHandLeft];
		return h;
	};

	// Stick POSITION needs no discovery: every shipped binding puts Move on
	// the left and Turn on the right, and a player who rebinds that has
	// already chosen their layout.
	analog( pickStick( m_actions.move, m_actions.turn ),
			m_inputState.moveX, m_inputState.moveY );
	analog( pickStick( m_actions.turn, m_actions.move ),
			m_inputState.turnX, m_inputState.turnY );

	// stickClick() wraps every one of these, not just the pair that happens
	// to be on the sticks today: which actions those are is discovered, so
	// naming them here would reintroduce exactly the hard-coding it avoids.
	// It is identity for anything not on a stick click.
	m_inputState.attack = digitalHeld( stickClick( pick( m_actions.attack, m_actions.attack2 ) ) );
	m_inputState.attack2 = digitalHeld( stickClick( pick( m_actions.attack2, m_actions.attack ) ) );
	// Jump and reload travel with the STICK group, not with handedness.
	//
	// Jump belongs under the thumb that is NOT steering -- that is the
	// gamepad convention it inherits (move on the left stick, jump on the
	// right thumb), and it is what makes jumping while running possible at
	// all. Following left_handed alone broke that: a left-handed player who
	// swapped the sticks back got movement and jump on the SAME thumb.
	//
	// So they take stickSwap, which is the composed answer to "which hand
	// steers", rather than lh, which is the answer to "which hand shoots".
	// Use and flashlight deliberately stay on lh: they are not stick-thumb
	// actions and nothing about steering changes where they want to be.
	m_inputState.jump = digitalHeld( stickClick( pickStick( m_actions.jump, m_actions.reload ) ) );
	m_inputState.reload = digitalHeld( stickClick( pickStick( m_actions.reload, m_actions.jump ) ) );
	m_inputState.use = digitalHeld( stickClick( pick( m_actions.use, m_actions.flashlight ) ) );
	// ---- CROUCH AND NEXTWEAPON ARE NOT A PAIR ANY MORE ---------------------
	//
	// They used to be: Crouch sat on the LEFT grip and NextWeapon on the RIGHT,
	// so mirroring them was exactly right -- the grip followed the weapon hand
	// without a manifest change, which is what holster_zones.h is built on.
	//
	// Then crouch moved off the grip onto the turn stick, and the Crouch
	// BINDING was removed from all four profiles while the pair was left in
	// place. `Crouch` is now declared in the manifest and bound in none of them,
	// so with left_handed = 1 the mirror swapped a working action for a
	// permanently dead one:
	//
	//     nextWeapon -> Crouch      unbound  -> the grip did NOTHING,
	//                                           so holsters and invnext died
	//     crouch     -> NextWeapon  right grip -> and squeezing the OFF hand
	//                                           crouched the player
	//
	// Both symptoms, one cause. A pair is only a pair while BOTH halves are
	// bound; mirroring against an unbound action can only ever produce a dead
	// slot, so these now read their own action like PrevWeapon does.
	//
	// The weapon hand keeps its grip because the BINDINGS carry it now -- the
	// three stick profiles bind NextWeapon on both grips, so the action exists
	// on whichever hand is the weapon hand. That is the right place for it:
	// which physical button an action lives on is a binding question, and
	// answering it in code is what produced this bug.
	m_inputState.crouch = digitalHeld( stickClick( m_actions.crouch ) );

	m_inputState.flashlight = digitalPressed( stickClick( pick( m_actions.flashlight, m_actions.use ) ) );

	// ---- THE GRIP FOLLOWS THE WEAPON HAND, BY DEVICE ------------------------
	//
	// Bound on BOTH grips in the manifest and read from ONE of them: whichever
	// hand `left_handed` says holds the weapon. Reaching into a holster zone is
	// done with the weapon hand, so the button that takes the weapon has to be
	// on that same hand -- squeezing the other one is a different gesture.
	//
	// Doing it by device rather than by swapping action handles is the whole
	// lesson from the holster outage: a static binding file cannot know which
	// hand is the weapon hand, so it should not be asked. It binds the control
	// on both, and the runtime is asked which hand pressed it.
	//
	// The off hand's grip is therefore INERT rather than free. If it should do
	// something later, give it its own action restricted to the OTHER hand --
	// the same mechanism, pointed the other way.
	m_inputState.nextWeapon = digitalPressedOnHand(
		stickClick( m_actions.nextWeapon ),
		m_settings.leftHanded ? kHandLeft : kHandRight );

	// The OTHER hand's grip, from the same action. See VRInputState::offHandGrip.
	{
		const int offHand = m_settings.leftHanded ? kHandRight : kHandLeft;
		const vr::VRActionHandle_t grip = stickClick( m_actions.nextWeapon );
		m_inputState.offHandGrip = digitalHeldOnHand( grip, offHand );
		m_inputState.offHandGripPressed = digitalPressedOnHand( grip, offHand );
	}
	// Menu and Recenter take stickClick ALONE -- no pick().
	//
	// They live ON the stick clicks, so the only question is which stick, and
	// stickClick() already answers it from the live bindings. Running them
	// through pick() as well applied handedness a SECOND time: with
	// left_handed = 1 and swap_thumbsticks = 1 the sticks sit in their original
	// arrangement, stickClick correctly did nothing, and pick() then swapped the
	// pair anyway -- so Menu answered the right stick's single click while the
	// left stick's double click silently recentred.
	//
	// It is the same distinction the handover records for jump/reload: an action
	// on a stick follows "which hand STEERS", not "which hand shoots". pick()
	// answers the wrong question for anything living on a thumbstick.
	m_inputState.menu = digitalPressed( stickClick( m_actions.menu ) );
	m_inputState.recenter = digitalPressed( stickClick( m_actions.recenter ) );

	// Unpaired: no mirrored counterpart is bound by default, so they read the
	// same action either way.
	m_inputState.prevWeapon = digitalPressed( stickClick( m_actions.prevWeapon ) );

	// Grenade is deliberately NOT mirrored by handedness, and that is a
	// CONSTRAINT rather than a preference.
	//
	// pick() swaps two action HANDLES, so a mirrored pair only works when its
	// members sit on the same kind of input on opposite hands -- Attack on the
	// right trigger against Attack2 on the left. Pairing Grenade with Attack2
	// would therefore break the moment the two are bound to different input
	// TYPES on different controllers, which they are: bumpers on a Cosmos,
	// trackpads on an Index, a stick gesture on wands. Left-handed mode would
	// then read alt-fire off a trackpad and grenades off a trigger.
	//
	// So grenade is bound SYMMETRICALLY -- both bumpers, both trackpads -- and
	// either hand throws. Symmetry sidesteps the handedness question rather
	// than answering it, and there is no "grenade hand" the way there is a
	// weapon hand.
	//
	// HELD, not pressed: +grenade/-grenade is a command pair the engine holds
	// in its own key state, and SiN's own bind is `bind "g" "+grenade"`. An
	// impulse-style single frame is the mistake lesson 4 in the handover
	// records -- held and pressed are different kinds of value.
	m_inputState.grenade = digitalHeld( stickClick( m_actions.grenade ) );

	m_inputState.valid = true;
}

bool OpenVRBackend::Update()
{
	m_hmd.valid = false;
	if ( !m_system )
		return false;

	// Input first, and unconditionally on the pose result: controllers keep
	// working while the headset pose is momentarily rejected, and a player who
	// cannot press Escape because tracking hiccuped is stuck.
	UpdateInput();

	vr::TrackedDevicePose_t poses[vr::k_unMaxTrackedDeviceCount];

	// ---- WHERE THE HEAD POSE COMES FROM, AND WHY IT CHANGED --------------
	//
	// It used to be GetDeviceToAbsoluteTrackingPose with an interval we worked
	// out ourselves:
	//
	//     frameDuration - secondsSinceVsync + vsyncToPhotons
	//
	// That is only stable if the call lands at the same point in every display
	// frame. This app does not: it free-runs at ~80 fps against a 90 Hz panel,
	// so `secondsSinceVsync` sweeps the whole frame period and the prediction
	// interval jitters with it -- by up to a full frame, every frame.
	//
	// Prediction error scales with VELOCITY, which is why the symptom was so
	// lopsided and so misleading. Walking at a few units per second, a jitter
	// of 11 ms moves the eye by hundredths of a unit: invisible. Turning the
	// head, the same jitter is a large and randomly-varying fraction of a
	// degree, every frame. Reported, correctly, as "walking is fine, turning
	// judders" -- which also rules out frame rate, because dropped frames would
	// hit both equally. That one sentence was worth more than three sessions of
	// heartbeat arithmetic.
	//
	// The compositor's renderPoses have none of this: they are predicted by the
	// runtime, for a specific frame, on its own cadence. We were already asking
	// for them every frame and dropping them on the floor.
	const bool useCompositor = m_useCompositorPoses && m_haveRenderPoses;
	if ( useCompositor )
	{
		memcpy( poses, m_renderPoses, sizeof( poses ) );
	}
	else
	{
		// Fallback, and the old behaviour. Still reachable before the first
		// WaitGetPoses -- and with submission off, when there is no compositor
		// frame loop to take poses from at all.
		m_system->GetDeviceToAbsoluteTrackingPose( vr::TrackingUniverseStanding,
												   PredictedSecondsToPhotons(),
												   poses, vr::k_unMaxTrackedDeviceCount );
	}

	// Measured either way. The spread on this is the whole argument for the
	// change, so it is reported even when it is no longer being used -- a
	// setting's effect and its justification are different claims.
	{
		const float pred = PredictedSecondsToPhotons();
		if ( pred < m_predMin ) m_predMin = pred;
		if ( pred > m_predMax ) m_predMax = pred;
		m_predSum += pred;
		++m_predCount;
	}

	// Hands come out of the same array the HMD does, so this is free. Resolved
	// by ROLE rather than by device index, because indices are assignment order
	// and a controller that sleeps and wakes can come back as a different one.
	for ( int hand = 0; hand < kHandCount; ++hand )
	{
		m_hands[hand] = ControllerPose();

		const vr::ETrackedControllerRole role = ( hand == kHandLeft )
			? vr::TrackedControllerRole_LeftHand
			: vr::TrackedControllerRole_RightHand;

		const vr::TrackedDeviceIndex_t idx =
			m_system->GetTrackedDeviceIndexForControllerRole( role );
		if ( idx == vr::k_unTrackedDeviceIndexInvalid ||
			 idx >= vr::k_unMaxTrackedDeviceCount )
			continue;

		const vr::TrackedDevicePose_t& p = poses[idx];
		if ( !p.bPoseIsValid || p.eTrackingResult != vr::TrackingResult_Running_OK )
			continue;

		MatrixToSourcePose( p.mDeviceToAbsoluteTracking, m_settings.worldScale,
							m_hands[hand].angles, m_hands[hand].position );
		HmdVectorToSource( p.vVelocity, m_settings.worldScale, m_hands[hand].velocity );
		m_hands[hand].valid = true;
	}

	const vr::TrackedDevicePose_t& hmd = poses[vr::k_unTrackedDeviceIndex_Hmd];
	if ( !hmd.bPoseIsValid || hmd.eTrackingResult != vr::TrackingResult_Running_OK )
	{
		LogTrace( "openvr: pose rejected (valid=%d result=%d)",
				  hmd.bPoseIsValid ? 1 : 0, (int)hmd.eTrackingResult );
		return false;
	}

	MatrixToSourcePose( hmd.mDeviceToAbsoluteTracking, m_settings.worldScale,
						m_hmd.angles, m_hmd.position );
	m_hmd.valid = true;

	LogTrace( "openvr: ang=(%.2f %.2f %.2f) pos=(%.2f %.2f %.2f)",
			  m_hmd.angles.x, m_hmd.angles.y, m_hmd.angles.z,
			  m_hmd.position.x, m_hmd.position.y, m_hmd.position.z );
	return true;
}

void OpenVRBackend::Shutdown()
{
	if ( m_system && m_ShutdownInternal )
	{
		m_ShutdownInternal();
		Log( "openvr: shut down" );
	}
	m_system = nullptr;

	if ( m_dll )
	{
		FreeLibrary( m_dll );
		m_dll = nullptr;
	}
}

} // namespace

IVRBackend* CreateOpenVRBackend()
{
	return new OpenVRBackend();
}

} // namespace sinvr
