// VR runtime abstraction.
//
// The concrete backend is OpenVR, because SiN is a 32-bit process and no
// mainstream OpenXR runtime ships a 32-bit component (SteamVR installs
// steamxr_win64.json only). Keeping this interface means an OpenXR backend --
// which would need a 64-bit helper process owning the session -- can be dropped
// in later without touching the camera code.
//
// Nothing in this header includes openvr.h, so the rest of the mod does not
// depend on the OpenVR SDK.
#pragma once

#include <stdint.h>
#include "../sdk/source_interfaces.h"

namespace sinvr {

// Source is ~1 unit per inch (the player is 72 units tall = 6 ft), so a metre of
// real movement is ~39.37 units. Tunable: it is effectively the world-scale
// knob, and a value that is "correct" is not always the one that feels right.
constexpr float kSourceUnitsPerMeter = 39.37f;

struct HmdPose
{
	QAngle angles;    // Source convention, degrees: pitch(+down), yaw, roll
	Vector position;  // Source convention, units, relative to the play space origin
	bool valid = false;
};

// Physical hands, never roles. Which one holds the weapon is a separate
// question -- see WeaponHand() -- because it flips with `left_handed` and
// conflating the two makes every log line ambiguous.
enum Hand
{
	kHandLeft = 0,
	kHandRight = 1,
	kHandCount = 2,
};

// Same frame and units as HmdPose: Source convention, room space, relative to
// the play space origin. Rotating one into world space is the same yaw the head
// uses, so VRCamera can do both with one mapping.
struct ControllerPose
{
	QAngle angles;
	Vector position;

	// Linear velocity, SAME frame and units as `position`: Source axes, room
	// space, units per second.
	//
	// Taken from the runtime rather than derived. TrackedDevicePose_t carries
	// vVelocity alongside the transform in the array we already fetch every
	// frame, so this costs nothing -- and it is better than a per-frame position
	// difference, which divides tracking noise by a small dt and amplifies it
	// exactly when the hand is moving fastest. That is the moment a gesture
	// detector cares about most.
	Vector velocity = { 0.0f, 0.0f, 0.0f };

	bool valid = false;
};

// One frame of controller input, already reduced to what the game needs.
//
// Split deliberately into "held" and "pressed":
//
//   held    -> Source's +command / -command pairs, which must be issued on the
//              EDGE and left alone in between. Issuing "+attack" every frame
//              works but issuing "-attack" every frame does not.
//   pressed -> rising edge only, for one-shot console commands (invnext,
//              impulse 100) where repeating while held would fire continuously.
//
// Doing that reduction here rather than in the game layer keeps the OpenVR
// action-state details (bState vs bChanged) out of everything downstream.
struct VRInputState
{
	// -1..1, deadzoned. moveY is forward-positive, turnX is right-positive.
	float moveX = 0.0f;
	float moveY = 0.0f;
	float turnX = 0.0f;
	float turnY = 0.0f;

	bool attack = false;      // held
	bool attack2 = false;     // held
	bool jump = false;        // held
	bool use = false;         // held
	bool reload = false;      // held
	bool crouch = false;      // held
	bool grenade = false;     // held -- +grenade, SiN's own "g" bind

	bool nextWeapon = false;  // pressed

	// ---- THE OFF HAND'S GRIP -----------------------------------------------
	//
	// The SAME action as nextWeapon, read from the OTHER hand.
	//
	// The three stick profiles bind NextWeapon on both grips and the weapon
	// hand's read is restricted to its own device, so the off hand's grip has
	// been bound and completely inert. That makes it free real estate for the
	// two-handed grip without touching the manifest or asking anyone to rebind
	// -- which is the same argument that put holster draws on the grip in the
	// first place.
	//
	// Both edges are carried because the two grip modes want different things:
	// `hold` wants the held state, `toggle` wants the press.
	//
	// NOT available on Vive wands, whose grips carry Use and Reload -- there is
	// no free grip there and NextWeapon lives on the application menu button.
	bool offHandGrip = false;         // held
	bool offHandGripPressed = false;  // pressed

	bool prevWeapon = false;  // pressed
	bool flashlight = false;  // pressed
	bool menu = false;        // pressed
	bool recenter = false;    // pressed

	// False when the runtime has no action manifest loaded, no controllers are
	// on, or IVRInput was unavailable. Everything above is zeroed in that case,
	// so a caller may use it unconditionally.
	bool valid = false;
};

struct VRBackendSettings
{
	// Overridden from sinvr.cfg so it can be tuned on the VR machine without a
	// rebuild.
	float worldScale = kSourceUnitsPerMeter;

	// Submitting frames requires being the scene application. Until the
	// submission path is proven, running as a non-scene app avoids the
	// compositor flagging us unresponsive for never submitting.
	bool sceneApp = false;

	// Load the action manifest and read controllers. Off means the mod never
	// touches IVRInput, which is the clean way to fall back to mouse+keyboard.
	bool controllerInput = true;

	// Which physical hand holds the weapon. SiN's viewmodel is right-handed and
	// this does not change that yet -- for now it decides which controller is
	// the aim hand and which is the movement hand. L4D2VR does the same thing by
	// swapping the two device indices at the source, which is the right shape:
	// one swap, and everything downstream follows without knowing about it.
	bool leftHanded = false;

	// Swap the two thumbsticks, independently of handedness.
	//
	// `leftHanded` mirrors EVERY pair at once -- triggers, face buttons,
	// grips and sticks -- which leaves a left-handed player no way to ask
	// for the mirrored triggers but their movement thumb where it started.
	// This is that second axis, and it COMPOSES with handedness by XOR
	// rather than overriding it: left-handed plus swapped puts movement
	// back on the left stick while everything else stays mirrored.
	bool swapThumbsticks = false;

	// ---- submission safety (2026-09-13) -- see OpenVRBackend --------------
	bool pauseSubmitOnStandby = true;   // vr_pause_submit_on_standby
	bool guardSubmit = true;            // vr_submit_guard
	bool checkOutputDevice = true;      // vr_submit_check_gpu
};

enum Eye
{
	kEyeLeft = 0,
	kEyeRight = 1,
	kEyeCount = 2,
};

// Sub-rectangle of the submitted texture belonging to one eye, in 0..1 texture
// coordinates.
//
// The game renders into its own backbuffer, whose aspect almost never matches
// the headset's near-square per-eye aspect. Rather than distorting the image to
// fit, the scene is rendered with a frustum that *contains* the eye's, and these
// bounds crop back to the part that is actually the eye's view. Geometrically
// exact at any game resolution -- the only cost is the rendered pixels that get
// cropped away, which is why a game resolution closer to the headset's aspect
// wastes less.
struct EyeBounds
{
	float uMin = 0.0f;
	float vMin = 0.0f;
	float uMax = 1.0f;
	float vMax = 1.0f;
};

// Everything needed to render one eye.
struct EyeParams
{
	// Raw projection tangents at the near plane, from GetProjectionRaw. The
	// frustum is asymmetric -- each eye sees further towards its own side --
	// which is exactly what a symmetric monitor projection gets wrong.
	float tanLeft = -1.0f;
	float tanRight = 1.0f;
	float tanTop = -1.0f;
	float tanBottom = 1.0f;

	// Eye position relative to head centre, in Source units.
	// OpenVR's eye space is +X right, +Y up, +Z back -- the same basis Source
	// uses for view space, so these drop straight into a view-space translation
	// with no axis juggling.
	float offsetRight = 0.0f;
	float offsetUp = 0.0f;
	float offsetBack = 0.0f;

	// Angular offset of this eye from head-forward, Source convention, degrees.
	//
	// Zero on every headset with parallel displays, which is most of them -- G2,
	// Index, Quest, Vive. Non-zero on **canted-display** designs (Pimax, StarVR,
	// some Bigscreen Beyond configurations), where each panel is physically
	// angled outward and the runtime's asymmetric tangents are expressed about
	// that canted axis rather than about head-forward.
	//
	// GetEyeToHeadTransform carries this in its 3x3 rotation, which we used to
	// discard entirely -- reading only the translation in column 3. On a canted
	// headset that renders each eye along the wrong axis.
	float pitchOffset = 0.0f;
	float yawOffset = 0.0f;
	float rollOffset = 0.0f;

	// True when the rotation above is meaningfully non-identity.
	bool canted = false;

	bool valid = false;
};

// A Vulkan image to hand the compositor. Layout matches D3D9VRTextureDesc and
// vr::VRVulkanTextureData_t so no translation is needed anywhere in the chain.
struct VulkanTextureDesc
{
	uint64_t image;
	void*    device;
	void*    physicalDevice;
	void*    instance;
	void*    queue;
	uint32_t queueFamilyIndex;
	uint32_t width;
	uint32_t height;
	uint32_t format;
	uint32_t sampleCount;
};

class IVRBackend
{
public:
	virtual ~IVRBackend() {}

	// Brings the runtime up. False means no VR this session; the mod must stay
	// out of the way rather than take the game down with it.
	virtual bool Init( const VRBackendSettings& settings ) = 0;
	virtual void Shutdown() = 0;

	// Polls fresh poses. Called once per rendered frame.
	virtual bool Update() = 0;

	virtual const HmdPose& Hmd() const = 0;

	// Refreshed by Update(), so it is one frame's worth and safe to read once.
	virtual const VRInputState& Input() const = 0;

	// Physical hand poses. Read from the same pose array the HMD comes from, so
	// they cost nothing extra.
	virtual const ControllerPose& Controller( int hand ) const = 0;

	// Role accessors, which is what gameplay code should use. These are the only
	// place `left_handed` is interpreted, so nothing downstream has to remember
	// which way round it is.
	virtual const ControllerPose& WeaponHand() const = 0;
	virtual const ControllerPose& OffHand() const = 0;
	virtual bool IsLeftHanded() const = 0;

	// EFFECT, not intent, which is the distinction that matters here.
	// `ThumbsticksSwapped` is handedness and the swap composed; the click
	// half can still be false while it is true, because which actions sit
	// on the stick clicks is discovered from the live bindings and is not
	// known until the runtime has loaded them. See DiscoverStickClicks.
	virtual bool ThumbsticksSwapped() const = 0;
	virtual bool StickClicksSwapped() const = 0;
	// Human-readable, for the log: which action each stick click carries.
	virtual const char* StickClickName( int hand ) const = 0;
	virtual bool IsReady() const = 0;
	virtual const char* Name() const = 0;

	// True once the compositor is available and we are the scene application.
	virtual bool CanSubmit() const = 0;

	// Must be called once per frame before submitting.
	//
	// This is what actually claims scene focus: an app that registers as
	// VRApplication_Scene but never enters the compositor's frame loop is
	// refused with VRCompositorError_DoNotHaveFocus, and SteamVR Home keeps the
	// headset. It also blocks for frame pacing, which is what synchronises the
	// game to the display refresh.
	virtual bool BeginFrame() = 0;

	// Explicit timing handshake, required for Vulkan.
	//
	// In the default implicit mode the runtime touches the Vulkan queue inside
	// WaitGetPoses, on whichever thread called it -- while DXVK's submit thread
	// is using the same externally-synchronised VkQueue. PreSubmit/PostSubmit
	// move that work under our control so WaitGetPoses is guaranteed never to
	// touch the queue.
	//
	// PreSubmit must be called immediately before the frame's first queue
	// submission, PostSubmit immediately after the last.
	virtual void PreSubmit() = 0;
	virtual void PostSubmit() = 0;

	// Phase 4a: the same image goes to both eyes, so there is no stereo yet --
	// it proves the DXVK -> Vulkan -> compositor chain in isolation, before the
	// engine's render loop is touched.
	virtual bool SubmitBothEyes( const VulkanTextureDesc& tex ) = 0;

	// Per-eye frustum and offset. Static for a given headset, so fetched once.
	virtual const EyeParams& GetEyeParams( int eye ) const = 0;

	// What the runtime thinks one eye should be rendered at.
	//
	// The mod cannot ACT on this -- the eye surfaces are created to match the
	// game's backbuffer, so the window size is the render resolution and it is
	// fixed before the process starts. sinvr_launcher.exe is what acts on it,
	// by appending -w/-h. This exists so the mod can say whether that worked,
	// which is otherwise only visible by comparing two log lines by eye.
	//
	// 0x0 if the runtime never answered.
	virtual void RecommendedRenderTargetSize( unsigned int& w,
											  unsigned int& h ) const = 0;

	// Per-eye submission, for real stereo. `bounds` selects the sub-rectangle of
	// the texture that actually corresponds to this eye's frustum.
	virtual bool SubmitEye( int eye, const VulkanTextureDesc& tex,
							const EyeBounds& bounds ) = 0;

	// ---- submission safety (2026-09-13) -- see OpenVRBackend --------------
	//
	// True while frames should NOT go to the runtime: the headset is in
	// standby, a fault inside the runtime was just caught, or the compositor's
	// GPU is not the game's. The frame loop (BeginFrame) keeps running.
	virtual bool SubmitPaused() const { return false; }

	// Once, on the first stereo submit: is SteamVR's compositor on the Vulkan
	// GPU DXVK created the game's device on? False only on a confirmed
	// mismatch with vr_submit_check_gpu on.
	virtual bool CheckOutputDevice( const VulkanTextureDesc& ) { return true; }

	// Heartbeat lines: headset activity, scene focus, faults, errors, and the
	// compositor's dropped/reprojected frame counts since the last one.
	virtual void LogSubmitSafety() {}

	// Human-readable reason Init() failed, for the log.
	virtual const char* LastError() const = 0;

	// Take the head pose from the compositor's renderPoses (true, the default)
	// rather than re-querying tracking with a prediction interval of our own.
	// The latter jitters whenever the app is off the display cadence, which
	// shows up as judder on TURNING and not on walking.
	virtual void SetUseCompositorPoses( bool ) {}

	// Whether a WaitGetPoses has ever succeeded. The Present hook submits
	// BEFORE WaitGetPoses so the compositor reprojects from the poses the
	// frame was drawn with -- which needs one priming call on the first frame,
	// when there is nothing to submit against yet.
	virtual bool HavePoses() const { return false; }

	// Prediction-interval spread, for the heartbeat. A wide min..max is the
	// evidence that the old self-predicted path was unstable.
	virtual void PredictionStats( float& mn, float& mx, float& avg ) const
	{
		mn = 0.0f; mx = 0.0f; avg = 0.0f;
	}
};

IVRBackend* CreateOpenVRBackend();

} // namespace sinvr
