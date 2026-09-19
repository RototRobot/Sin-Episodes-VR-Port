// SiN VR -- phase 2: head tracking drives the game camera.
//
// Phase 1 established that we can attach, bind the Source interfaces, and that
// the vtable mapping is correct (see source_interfaces.h for the verified slot
// numbers -- SiN's IVEngineClient is NOT stock SDK 2004).
//
// This stage brings up OpenVR and feeds the HMD orientation into
// IVEngineClient::SetViewAngles from inside IBaseClientDLL::View_Render. The
// game still renders mono to the monitor; stereo and frame submission are
// phase 4 and need a DXVK fork.
//
// Hotkeys:  F9 recentre   F10 toggle VR camera   F11 toggle roll
//
// Everything lands in sinvr.log next to SinEpisodes.exe.

#include <windows.h>
#include "../common/log.h"
#include "../common/config.h"
#include "../common/crash_handler.h"
#include "../common/address_space.h"
#include "sdk/source_interfaces.h"
#include "hooks/vtable_hook.h"
#include "vr/vr_backend.h"
#include "vr_camera.h"
#include "input/game_input.h"
#include "input/melee_gesture.h"
#include "input/arcade_reload.h"
#include "input/menu_pointer.h"
#include "render/menu_panel.h"
#include "render/menu_cursor.h"
#include "render/look_arrow.h"
#include "input/physical_crouch.h"
#include "input/two_handed.h"
#include "input/holster_zones.h"
#include "input/zone_set.h"
#include "sdk/client_entity_list.h"
#include "sdk/netprops.h"
#include "sdk/engine_trace.h"
#include "sdk/interface_list.h"
#include "sdk/game_movement.h"
#include "render/viewmodel.h"
#include "render/laser_dot.h"
#include "render/aim_debug.h"
#include "render/hand_marker.h"
#include "render/viewmodel_anim.h"
#include "render/d3d9_present_hook.h"
#include "render/draw_probe.h"
#include "render/stereo.h"

using namespace sinvr;

namespace {

EngineClient g_engine( nullptr );
void* g_client = nullptr;
VTableHook g_viewRenderHook;

IVRBackend* g_vr = nullptr;
VRCamera g_camera;
GameInput g_input;
MeleeGesture g_melee;
ArcadeReload g_arcadeReload;

// Head-vs-scene-camera divergence. See the block after LookError in the frame
// loop for why this is the signal FL_FROZEN and the duration heuristic both
// miss. Measurement only for now -- no behaviour keys off it yet.
float g_lookDivergenceMax = 0.0f;
float g_lookDivergenceMaxUndetected = 0.0f;
unsigned int g_lookDivergedDetected = 0;
unsigned int g_lookDivergedUndetected = 0;
unsigned int g_lookFramesUndetected = 0;

// The divergence LATCH was removed -- it drove turn-to-face, turn-to-face
// collapsed the divergence, and the latch re-fired every time the player looked
// away. See the note in vr_camera.h. The measurement above is kept because it
// is read-only and is still the best evidence about scenes the two surviving
// detectors miss.

// Turn-to-face on a map change. Edge-triggered on the level name, which is the
// property that makes it safe where divergence was not.
char g_lastMapName[128] = { 0 };
bool g_haveSeenAMap = false;
bool g_mapTurnPending = false;
// Which of the two triggers armed the pending turn. The turn itself is one
// shared one-shot, so without this the log asserts "level change" over a
// teleport -- and a log line that states the wrong cause is how three
// misclassifications of SiN's intro survived as long as they did.
const char* g_mapTurnCause = "level change";
// Set for the one frame a new level name first appears. Belt and braces beside
// clearing g_haveLastOrigin: two maps' origins are not comparable, and a level
// change must never be able to present as an in-map teleport.
bool g_mapChangedThisFrame = false;
int g_mapTurnFrames = 0;
unsigned int g_mapTurnCount = 0;
bool g_mapTurnEnabled = true;
int g_mapTurnDelayFrames = 45;
float g_mapTurnMinDegrees = 25.0f;

// How long the HUD anchor is dropped either side of a transition, so the
// game's fade-to-white draws in plain screen space and covers the view instead
// of being moved and shrunk with the HUD. See StereoRenderer::SuspendHudAnchor
// for why this is done by TIME rather than by inspecting the draw.
float g_hudAnchorTransitionMs = 2500.0f;

void SuspendHudAnchorForTransition()
{
	if ( g_hudAnchorTransitionMs <= 0.0f )
		return;
	Stereo().SuspendHudAnchor(
		GetTickCount() + (unsigned int)g_hudAnchorTransitionMs );
}

// Teleport-within-a-map detection. The flash-cut scenes are this, not cutscenes.
bool g_mapTurnOnTeleport = true;
float g_mapTurnTeleportUnits = 400.0f;
Vector g_lastOrigin = { 0.0f, 0.0f, 0.0f };
bool g_haveLastOrigin = false;
unsigned int g_teleportCount = 0;
PhysicalCrouch g_physicalCrouch;
TwoHanded g_twoHanded;
// m_vecViewOffset's offset, found by name. -1 until resolved; the crouch
// compensation degrades to 0 without it rather than guessing Source's 64/28.
int g_viewOffsetOffset = -1;
// m_nWaterLevel's offset, found by name. Swimming needs it; -1 leaves the
// aim pitch alone, which is the pre-existing sink-while-swimming behaviour.
int g_waterLevelOffset = -1;

// ---- FL_FROZEN: THE GAME SAYING "THE PLAYER IS NOT DRIVING" ----------------
//
// A far better cutscene signal than inferring one from how long the engine has
// been rewriting the view angles, and it comes straight from the SDK:
//
//     CTriggerCamera::Enable()            <- point_viewcontrol, the standard
//         EnableControl( FALSE )             Source cutscene camera
//             AddFlag( FL_FROZEN )
//
//     const.h:  #define FL_FROZEN (1<<5)
//     c_baseplayer.cpp:  RecvPropInt( RECVINFO( m_fFlags ) )   <- networked
//
// So the flag is readable on the CLIENT's player entity with the netprop
// machinery already in use for punch angle, water level and view offset.
//
// It is used ALONGSIDE the duration heuristic rather than replacing it. The two
// catch different things: this catches anything that freezes the player, which
// is every point_viewcontrol; the heuristic catches a scripted camera that
// drives the view WITHOUT freezing movement. Either one is enough.
int g_playerFlagsOffset = -1;
constexpr int kFlFrozen = ( 1 << 5 );
int g_lastWaterLevel = 0;
HolsterZones g_holsters;
ZoneSet g_zones;
LaserDot g_laserDot;
AimDebug g_aimDebug;
// Set by sinvr_launcher.exe's -w/-h, reported here. See ReportResolution.
bool g_autoResolution = true;
float g_resolutionScale = 1.0f;
// Read only so the effective-settings block can report it. The LAUNCHER owns
// this one -- it decides the window size before the mod exists -- but a
// launcher-only key is invisible in the place everyone actually looks.
bool g_allowOversizeWindow = false;
// Also the launcher's, and also reported here only so the effective-settings
// block tells the whole story about how the render size was arrived at.
float g_maxRenderHeight = 0.0f;

bool g_menuGating = true;   // see menu_gating in sinvr.cfg

// How many consecutive frames the OS cursor must be visible before menu mode
// latches, and how far the movement stick has to be pushed to overrule it.
//
// At 90 fps six frames is 67 ms -- far too short to notice when opening a menu,
// long enough that a one-frame blip from the mod's own SetCursorPos / SendInput
// cannot latch it. The move threshold sits above the 0.35 deadzone the sticks
// are already filtered by, so it cannot be reached by drift.
constexpr unsigned int kMenuLatchFrames = 6;
constexpr float kMenuEscapeMove = 0.5f;

// Times the liveness test overruled the cursor. A non-zero count is evidence
// for possibility 2 in the handover: the cursor sticks visible after a menu
// closes.
unsigned int g_menuEscapes = 0;

// The engine's view yaw at the PREVIOUS heartbeat. Printed beside the current
// one so a pinned heading is visible in a single log line rather than needing
// two lines five seconds apart to be compared by hand.
float g_lastHeartbeatEngineYaw = 0.0f;
unsigned int g_lastHeartbeatScriptedFrames = 0;
MenuPointer g_menuPointer;

// The cursor, drawn by us in D3D9 on top of the finished eye image. The only
// cursor that can appear over a PAUSE menu -- see render/menu_cursor.h.
MenuCursor g_menuCursor;

// Points at whatever a scripted camera is showing, while the player's head keeps
// the view. See render/look_arrow.h.
LookArrow g_lookArrow;
MenuPanelSettings g_menuPanelSettings;
// Pin the menu to the background map's own camera direction rather than to
// wherever the player happened to be facing. See menu_panel_align_scene.
bool g_menuAlignToScene = true;
MenuPanelGeometry g_menuPanelGeom;
// The menu panel holds one world YAW for the session -- taken at startup and
// re-taken on recentre. Not per menu: re-capturing on every menu open is what
// made it appear to follow the head, because menu transitions blink the cursor.
float g_menuAnchorYaw = 0.0f;
unsigned int g_menuAnchorRecentre = 0;
bool g_menuAnchorHeld = false;
// When the cursor last went away, so a genuine menu OPEN can be told apart from
// the frame-or-two blink that moving between menu screens produces.
DWORD g_menuDownSinceMs = 0;
unsigned int g_menuRepinMs = 750;
unsigned int g_menuRepins = 0;
HandMarker g_handMarker;
ClientEntityList g_entities;
ViewModelDriver g_viewModel;
ViewModelAnimation g_viewModelAnim;
NetProps g_netProps;

// The camera's own trace, deliberately NOT the laser's.
//
// LaserDot owns a private EngineTrace, and borrowing it would tie head
// collision to `laser_dot = 1` -- turning the crosshair off would silently let
// the player walk through walls again. Two instances of a stateless binding
// cost nothing.
EngineTrace g_collisionTrace;

// One-shot: dump server.dll's interfaces the first time a map is up.
bool g_logInterfacesInGame = false;
int g_punchAngleOffset = -1;

// Recoil view lock. SiN's kick reaches the client as already-rotated view
// angles, so "is a shot being fired" is the only available discriminator.
DWORD g_firingUntilMs = 0;
float g_recoilLockTailMs = 250.0f;
float g_recoilCompensation = 0.0f;
Config g_config;
bool g_vrCameraEnabled = true;
// Intent, for the log only -- the backend owns the effect. The "sticks:"
// heartbeat line reports both, because they differ whenever the click half
// could not be resolved from the bindings.
bool g_swapThumbsticks = false;
int g_heartbeatSeconds = 5;
float g_hudAnchorForward = 55.0f;
float g_hudAnchorRight = -6.0f;
float g_hudAnchorUp = -14.0f;
bool g_hudAnchorFollowYaw = true;
// Frames of viewmodel transform tracing left to emit, for the flicker
// investigation. See TraceViewModel below.
int g_vmTraceFrames = 0;
DWORD g_lastDiagnoseMs = 0;
DWORD g_lastInputTick = 0;
double g_lastInputTime = 0.0;

unsigned long long g_frames = 0;

//-----------------------------------------------------------------------------
// The clock the input dt comes from -- and a measurement of the one it replaced.
//
// dt scales the smooth turn rate, so the clock's granularity IS the smoothness.
// It used to come from GetTickCount, which advances in steps of the system
// clock interrupt -- nominally ~15.6 ms -- while a frame at 90 Hz is 11.1 ms.
// A dt read from that is not 0.011 every frame: it is 0 on most frames and
// ~0.0156 on the rest, so the turn rate stalls and lurches several times a
// second, which is what smooth-turn jitter looks like.
//
// That is a reading of the code, though, not a measurement, and this project
// has produced several confident wrong diagnoses. So the old clock is still
// sampled on the same frames and both are reported. The number that decides it
// is `zero=`: a GetTickCount dt of exactly 0 on a frame that demonstrably took
// milliseconds is quantisation and cannot be anything else. At 90 fps the
// expected figure is around 29% (1 - 11.1/15.6). Near 0% means the old clock
// was fine on this machine and the jitter is something else -- in which case do
// not go on believing this fix was the one.
double NowSeconds()
{
	static double s_secPerTick = 0.0;
	if ( s_secPerTick == 0.0 )
	{
		LARGE_INTEGER freq = {};
		QueryPerformanceFrequency( &freq );
		s_secPerTick = ( freq.QuadPart > 0 ) ? ( 1.0 / (double)freq.QuadPart ) : 0.0;
	}
	LARGE_INTEGER now = {};
	QueryPerformanceCounter( &now );
	return (double)now.QuadPart * s_secPerTick;
}

// Both clocks, same frames. Costs two counter reads and a handful of floats.
class InputClockStats
{
public:
	void Note( float qpcDt, float legacyDt )
	{
		// Zero means first frame or a gap we already discarded, not a
		// measurement -- counting those would dilute the very statistic
		// this exists to report.
		if ( qpcDt <= 0.0f )
			return;

		if ( m_frames == 0 )
		{
			m_qpcMin = m_qpcMax = qpcDt;
			m_legacyMin = m_legacyMax = legacyDt;
		}
		else
		{
			if ( qpcDt < m_qpcMin ) m_qpcMin = qpcDt;
			if ( qpcDt > m_qpcMax ) m_qpcMax = qpcDt;
			if ( legacyDt < m_legacyMin ) m_legacyMin = legacyDt;
			if ( legacyDt > m_legacyMax ) m_legacyMax = legacyDt;
		}
		m_qpcSum += qpcDt;
		m_legacySum += legacyDt;
		++m_frames;
		if ( legacyDt == 0.0f )
			++m_legacyZero;

		// The raw sequence, briefly, once per session. An average cannot
		// show quantisation -- both clocks average to the frame time by
		// construction -- but a run of "0.0000 0.0156 0.0156 0.0000"
		// beside a steady 0.0111 shows it at a glance.
		if ( m_logged < kSequenceFrames )
		{
			++m_logged;
			Log( "input clock: frame %u  qpc=%.4fs  GetTickCount=%.4fs%s",
				 m_logged, qpcDt, legacyDt,
				 ( legacyDt == 0.0f ) ? "   <- old clock did not move" : "" );
		}
	}

	void LogState() const
	{
		if ( m_frames == 0 )
		{
			Log( "input clock: no frames sampled yet -- smooth turn has had no dt" );
			return;
		}
		const float pctZero = 100.0f * (float)m_legacyZero / (float)m_frames;
		Log( "input clock: QPC dt avg=%.4f min=%.4f max=%.4f s | "
			 "GetTickCount dt avg=%.4f min=%.4f max=%.4f s, zero on %u/%u "
			 "frames (%.0f%%) | %s",
			 m_qpcSum / (float)m_frames, m_qpcMin, m_qpcMax,
			 m_legacySum / (float)m_frames, m_legacyMin, m_legacyMax,
			 m_legacyZero, m_frames, pctZero,
			 ( pctZero > 5.0f )
				 ? "QUANTISED -- the old clock WAS the smooth-turn jitter"
				 : "old clock was NOT quantised here -- if smooth turn still "
				   "jitters, the cause is elsewhere" );
	}

private:
	static const unsigned int kSequenceFrames = 20;

	unsigned int m_frames = 0;
	unsigned int m_legacyZero = 0;
	unsigned int m_logged = 0;
	float m_qpcMin = 0.0f, m_qpcMax = 0.0f, m_qpcSum = 0.0f;
	float m_legacyMin = 0.0f, m_legacyMax = 0.0f, m_legacySum = 0.0f;
};

InputClockStats g_inputClock;

// Widening the game's own FOV is the only lever we have over the engine's world
// culling. Source builds its BSP frustum planes from the view setup's FOV -- 75
// degrees here -- and nothing exposes those planes. Entities can be rescued via
// CullBox, but world surfaces outside that frustum are never submitted at all,
// which is why the ceiling and upper walls vanish in VR while the flatscreen
// build shows them.
//
// Our projection override replaces the matrix regardless, so a wider engine FOV
// does not change the rendered image -- it only makes the engine keep geometry
// it was throwing away.
//
// <= 0 means work the value out from the headset's own frustum rather than
// guessing: see StereoRenderer::RequiredEngineFovX. The hand-picked 110 that
// used to be the default was never enough -- each eye's frustum is extended
// sideways to the render target's aspect and reaches tan 1.61 on its outer edge,
// where fov 110 only reaches 1.43.
int g_engineFov = 0;
bool g_engineFovIsAuto = true;

// Cheat-flagged cvars are rejected outright unless sv_cheats is on, and
// default_fov is one of them.
bool g_allowCheats = true;
bool g_openAllPortals = true;
// Experimental performance level, 0..3. See where it is read for what each
// level turns off and what it may cost visually.
int g_perfLevel = 0;
bool g_hideCrosshairWhenAiming = true;
bool g_disableOcclusion = true;
bool g_disableVis = false;

// Re-assertion, not a one-shot.
//
// The original bug: the settings were applied once, behind a latch. The first
// frames of a session run on the menu's background map, where IsInGame() is
// already true, so the latch closed there -- and the level load that followed
// put the FOV back to the game's own 75 for the whole play session. The log said
// "requested 110", the trace ended two frames later, and nothing ever mentioned
// it again.
//
// So: re-apply on every entry into a map, and periodically while in one. A
// redundant ClientCmd costs nothing; a missed one costs the ceiling.
bool g_wasInGame = false;
DWORD g_lastEngineTuneMs = 0;
int g_engineTuneIntervalMs = 5000;
int g_engineTuneCount = 0;

// Recorded so the watchdog can go and look at whichever thread stopped moving.
DWORD g_renderThreadId = 0;

//-----------------------------------------------------------------------------
// Edge-triggered hotkey. GetAsyncKeyState is per-frame polling rather than a
// keyboard hook, which keeps us out of the engine's input path entirely.
//-----------------------------------------------------------------------------
// ---- A HOTKEY MUST NOT ANSWER SOMEBODY ELSE'S SHORTCUT ----------------------
//
// This used to be a bare GetAsyncKeyState on a virtual key, which is wrong in
// two ways that only show up when something else is running:
//
//   NO MODIFIERS   Ctrl+F11 and F11 were the same key. Every recording tool
//                  binds something in the F9-F12 range -- Steam, GeForce
//                  Experience, OBS, the Windows Game Bar -- so pressing record
//                  ALSO toggled the VR camera off, and the player got a
//                  recording of the tracking breaking.
//
//   NO FOCUS TEST  GetAsyncKeyState is global. Alt-tab to a browser, press
//                  F10, and the mod still acted on it.
//
// So a hotkey now requires its modifiers to match EXACTLY -- Ctrl+Alt+F10 does
// not fire on Ctrl+F10, and a plain-key binding does not fire when a modifier
// is held -- and only while the game is the foreground window.
//
// Exact matching is the important half. "At least these modifiers" would still
// have collided with Ctrl+F11, which is the case that caused the report.
enum HotKeyMods
{
	kModNone = 0,
	kModCtrl = 1,
	kModAlt = 2,
	kModShift = 4,
};

inline bool GameHasFocus()
{
	DWORD pid = 0;
	GetWindowThreadProcessId( GetForegroundWindow(), &pid );
	return pid == GetCurrentProcessId();
}

class HotKey
{
public:
	explicit HotKey( int vk, int mods = kModNone ) : m_vk( vk ), m_mods( mods ) {}

	// Changed at startup from config, so a player whose recorder uses the same
	// combination can move ours.
	void SetMods( int mods ) { m_mods = mods; }
	int Mods() const { return m_mods; }
	int Key() const { return m_vk; }

	bool Pressed()
	{
		const bool held = ( GetAsyncKeyState( m_vk ) & 0x8000 ) != 0;

		int now = kModNone;
		if ( GetAsyncKeyState( VK_CONTROL ) & 0x8000 ) now |= kModCtrl;
		if ( GetAsyncKeyState( VK_MENU ) & 0x8000 )    now |= kModAlt;
		if ( GetAsyncKeyState( VK_SHIFT ) & 0x8000 )   now |= kModShift;

		const bool down = held && ( now == m_mods ) && GameHasFocus();

		// The edge is tracked on the KEY alone, not on the whole combination.
		// Otherwise releasing Ctrl while still holding F11 re-arms the edge and
		// the action fires a second time on the way out.
		const bool fired = down && !m_wasDown;
		m_wasDown = held;
		return fired;
	}

private:
	int m_vk;
	int m_mods;
	bool m_wasDown = false;
};

inline const char* HotKeyModName( int mods )
{
	switch ( mods )
	{
		case kModCtrl | kModAlt:  return "Ctrl+Alt+";
		case kModCtrl:            return "Ctrl+";
		case kModAlt:             return "Alt+";
		case kModShift:           return "Shift+";
		case kModCtrl | kModShift: return "Ctrl+Shift+";
		default:                  return "";
	}
}

// Ctrl+Alt by default. These three are live for the whole session -- unlike the
// numpad tuners, which are gated behind an adjust mode the player has switched
// on deliberately -- so they are the ones that collide with recording software.
//
// g_keyToggle in particular turns the VR camera OFF, which is as dramatic as it
// sounds and is exactly what a stray F10 was doing.
HotKey g_keyRecenter( VK_F9, kModCtrl | kModAlt );
HotKey g_keyToggle( VK_F10, kModCtrl | kModAlt );
HotKey g_keyRoll( VK_F11, kModCtrl | kModAlt );

// Numpad live eye alignment, opt-in via sinvr.cfg.
//   7 / 9  select left / right eye
//   4 6 8 2  nudge left / right / up / down
//   5      reset selected eye
//   - / +  smaller / larger step
HotKey g_keySelectLeft( VK_NUMPAD7 );
HotKey g_keySelectRight( VK_NUMPAD9 );
HotKey g_keyNudgeLeft( VK_NUMPAD4 );
HotKey g_keyNudgeRight( VK_NUMPAD6 );
HotKey g_keyNudgeUp( VK_NUMPAD8 );
HotKey g_keyNudgeDown( VK_NUMPAD2 );
HotKey g_keyResetEye( VK_NUMPAD5 );
HotKey g_keyStepDown( VK_SUBTRACT );
HotKey g_keyStepUp( VK_ADD );

// Live viewmodel offset tuning, opt-in via viewmodel_adjust_enabled. Shares the
// numpad with the eye alignment above, so only one of the two may be on at a
// time -- startup refuses and says so rather than letting one key do two jobs.
//   8 / 2  forward / back      9 / 3  up / down
//   4 / 6  left / right       5      reset
//   - / +  smaller / larger step      0  print the cfg lines again
HotKey g_keyVmForward( VK_NUMPAD8 );
HotKey g_keyVmBack( VK_NUMPAD2 );
HotKey g_keyVmLeft( VK_NUMPAD4 );
HotKey g_keyVmRight( VK_NUMPAD6 );
HotKey g_keyVmUp( VK_NUMPAD9 );
HotKey g_keyVmDown( VK_NUMPAD3 );
HotKey g_keyVmReset( VK_NUMPAD5 );
HotKey g_keyVmDump( VK_NUMPAD0 );
HotKey g_keyVmMode( VK_NUMPAD7 );   // toggle position <-> angles

// Iron-sight zeroing. Bare keys, like the numpad tuners -- HotKey::Pressed
// tests GameHasFocus, so these cannot reach another application, and SiN takes
// no text input during play.
HotKey g_keyBoreLeft( VK_OEM_COMMA );    // ,
HotKey g_keyBoreRight( VK_OEM_PERIOD );  // .
HotKey g_keyBoreSave( VK_OEM_2 );        // /
HotKey g_keyBoreAxis( VK_OEM_7 );        // '  -- cycles which axis , and . drive
// Own step keys rather than sharing the eye-adjust ones: HotKey::Pressed is
// edge-triggered and CONSUMES the edge, so two objects reading VK_ADD in the
// same frame would work, but two calls on the SAME object would not -- the
// first would swallow it.
HotKey g_keyVmStepDown( VK_SUBTRACT );
HotKey g_keyVmStepUp( VK_ADD );
bool g_viewModelAdjust = false;

// Live body-zone tuning, opt-in via zone_adjust_enabled. Shares the numpad
// with BOTH adjusters above, so at most one of the three may be on.
//
// Numpad 1 and . are the only keys the other two leave free, which is why
// the two zone-specific actions -- pick a zone, snap it to the hand -- are
// the ones that landed there.
//   1      next zone            7      position <-> size
//   8 / 2  forward / back       9 / 3  up / down
//   4 / 6  left / right         .      SNAP to the weapon hand
//   5 5    reset this zone      0      dump all five as cfg lines
//   - / +  smaller / larger step
//
// Separate HotKey objects for the shared VKs, for the reason spelled out
// above g_keyVmStepDown: Pressed() consumes the edge, so two reads of one
// object in a frame would lose it.
HotKey g_keyZoneNext( VK_NUMPAD1 );
HotKey g_keyZoneMode( VK_NUMPAD7 );
HotKey g_keyZoneForward( VK_NUMPAD8 );
HotKey g_keyZoneBack( VK_NUMPAD2 );
HotKey g_keyZoneLeft( VK_NUMPAD4 );
HotKey g_keyZoneRight( VK_NUMPAD6 );
HotKey g_keyZoneUp( VK_NUMPAD9 );
HotKey g_keyZoneDown( VK_NUMPAD3 );
HotKey g_keyZoneReset( VK_NUMPAD5 );
HotKey g_keyZoneDump( VK_NUMPAD0 );
HotKey g_keyZoneSnap( VK_DECIMAL );
HotKey g_keyZoneStepDown( VK_SUBTRACT );
HotKey g_keyZoneStepUp( VK_ADD );
bool g_zoneAdjust = false;
DWORD g_lastZoneResetMs = 0;

// Live MENU PANEL tuning, opt-in via menu_panel_adjust. Shares the numpad with
// the three adjusters above, so at most one of the four may be on.
//
// This one exists because the correct placement is NOT derivable. The menu
// scene's character model and logo sit at a direction the engine does not
// expose -- pinning to its view yaw produced the identical number and moved
// nothing. A player wearing the headset can see where the panel should be in a
// second, so the fastest path is to let them put it there.
//
//   8 / 2  further / nearer      9 / 3  up / down
//   4 / 6  yaw left / right      1      pitch down    7  pitch up
//   .      roll left             5 5    reset all
//   0      dump as cfg lines     - / +  smaller / larger step
HotKey g_keyMpFurther( VK_NUMPAD8 );
HotKey g_keyMpNearer( VK_NUMPAD2 );
HotKey g_keyMpLeft( VK_NUMPAD4 );
HotKey g_keyMpRight( VK_NUMPAD6 );
HotKey g_keyMpUp( VK_NUMPAD9 );
HotKey g_keyMpDown( VK_NUMPAD3 );
HotKey g_keyMpPitchUp( VK_NUMPAD7 );
HotKey g_keyMpPitchDown( VK_NUMPAD1 );
HotKey g_keyMpRoll( VK_DECIMAL );
HotKey g_keyMpReset( VK_NUMPAD5 );
HotKey g_keyMpDump( VK_NUMPAD0 );
HotKey g_keyMpStepDown( VK_SUBTRACT );
HotKey g_keyMpStepUp( VK_ADD );
bool g_menuPanelAdjust = false;
float g_menuPanelStep = 5.0f;
DWORD g_lastMpResetMs = 0;

// Reset (numpad 5) is confirm-on-second-press. See HandleHotKeys.
DWORD g_lastResetPressMs = 0;
constexpr DWORD kResetDoubleClickMs = 600;

//-----------------------------------------------------------------------------
// Module address range, read straight from the PE headers.
//-----------------------------------------------------------------------------
struct ModuleRange
{
	uintptr_t base = 0;
	uintptr_t end = 0;

	bool Contains( const void* p ) const
	{
		uintptr_t a = reinterpret_cast<uintptr_t>( p );
		return a >= base && a < end;
	}
};

ModuleRange GetModuleRange( const char* name )
{
	ModuleRange r;
	HMODULE mod = GetModuleHandleA( name );
	if ( !mod )
		return r;

	auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>( mod );
	if ( dos->e_magic != IMAGE_DOS_SIGNATURE )
		return r;

	auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(
		reinterpret_cast<BYTE*>( mod ) + dos->e_lfanew );
	if ( nt->Signature != IMAGE_NT_SIGNATURE )
		return r;

	r.base = reinterpret_cast<uintptr_t>( mod );
	r.end = r.base + nt->OptionalHeader.SizeOfImage;
	return r;
}

//-----------------------------------------------------------------------------
// Walk a vtable until an entry stops pointing into the owning module. This is
// what caught Ritual's modified IVEngineClient (100 slots, not the SDK's 94).
//-----------------------------------------------------------------------------
int ProbeVTableLength( void* instance, const ModuleRange& owner, int sanityCap = 512 )
{
	if ( !instance || owner.base == 0 )
		return -1;

	void** vtable = VTableOf( instance );
	if ( !owner.Contains( vtable ) )
		return -1;

	int count = 0;
	__try
	{
		while ( count < sanityCap && owner.Contains( vtable[count] ) )
			++count;
	}
	__except ( EXCEPTION_EXECUTE_HANDLER )
	{
	}
	return count;
}

const char* GetRttiName( void* instance )
{
	__try
	{
		void** vtable = VTableOf( instance );
		auto* col = reinterpret_cast<BYTE*>( vtable[-1] );
		if ( !col )
			return "<none>";

		auto* typeDesc = *reinterpret_cast<BYTE**>( col + 12 );
		if ( !typeDesc )
			return "<none>";

		return reinterpret_cast<const char*>( typeDesc + 8 );
	}
	__except ( EXCEPTION_EXECUTE_HANDLER )
	{
		return "<fault>";
	}
}

//-----------------------------------------------------------------------------
// IBaseClientDLL::View_Render detour (slot 23, verified).
// thiscall is reached via fastcall plus a dummy edx.
//-----------------------------------------------------------------------------
using ViewRenderFn = void( __fastcall* )( void* thisptr, void* edx, vrect_t* rect );

//-----------------------------------------------------------------------------
// Write the tuned viewmodel offsets into sinvr.cfg.
//
// The tuner used to only PRINT the cfg lines, on the theory that a tuning
// session should end with something durable to paste. In practice it ends with
// the player taking a headset off, and the first per-weapon session was lost
// exactly that way -- both guns dialled in, game closed, values gone. Printing
// still happens; it is just no longer the only record.
//
// POSITION is saved per weapon, keyed on the model currently held, because
// that is the form that stays correct after a weapon switch. ANGLES are saved
// globally -- they are how a hand holds a gun, not a property of the gun.
//-----------------------------------------------------------------------------
// PER-WEAPON BORE OFFSET, TUNED IN THE HEADSET.
//
// Zeroing the iron sights is a LOOK-AND-ADJUST job: the only instrument that
// works is the player's own eye down the sights, and the value differs per gun
// because the three models carry their sights differently. Guessing it from the
// desk and shipping a build per guess is the slow way round -- the per-weapon
// grips and the HUD anchor both ended up tuned live for the same reason.
//
//   ,  bore LEFT      .  bore RIGHT      /  SAVE the current weapon
//
// Values live PER MODEL and fall back to the global `aim_bore_right` /
// `aim_bore_up` until a weapon has its own. Saving writes
// `aim_bore_right_<model>`, which is an OPTIONAL key -- deliberately absent by
// default and NOT in KnownKeys, exactly like the per-weapon viewmodel offsets.
// Adding it there would make every launch see a missing key.
//-----------------------------------------------------------------------------
struct BoreOffset
{
	char model[32];
	// ---- WHAT right/up/fwd MEAN NOW -----------------------------------
	//
	// They used to be a PARALLAX correction: the shot left the eye, and these
	// nudged the convergence target sideways to compensate for the gun being
	// somewhere else. With shot_from_gun the shot leaves the gun, so there is
	// no parallax left to correct -- and these become what the name always
	// suggested, the MUZZLE's position in the gun's own basis.
	//
	// That is why `fwd` had to be added. As a parallax fudge a forward offset
	// did nothing at all (sliding the target along the aim axis does not change
	// the angle); as a muzzle position it is the axis that matters most,
	// because it is the one that decides how far past a corner the shot starts.
	float right;   // muzzle offset, Source units, + is right
	float up;
	float fwd;
	float yaw;     // angular sight correction, degrees, + is LEFT
	float pitch;   // + is UP
};

// Which axis the , and . keys are driving. Cycled with '.
enum BoreAxis { kBoreYaw = 0, kBoreLateral, kBorePitch, kBoreUp, kBoreForward,
				kBoreAxisCount };

inline const char* BoreAxisName( int a )
{
	switch ( a )
	{
		case kBoreYaw:     return "YAW (deg, + left)";
		case kBoreLateral: return "LATERAL (units, + right)";
		case kBorePitch:   return "PITCH (deg, + up)";
		case kBoreUp:      return "UP (units)";
		default:           return "FORWARD (units, along the barrel)";
	}
}

BoreOffset g_bore[8] = {};
int g_boreCount = 0;
float g_boreGlobalRight = -3.0f;
float g_boreGlobalUp = 0.0f;
float g_boreGlobalFwd = 0.0f;
float g_boreGlobalYaw = 0.0f;
float g_boreGlobalPitch = 0.0f;
float g_boreStep = 0.5f;
float g_boreAngleStep = 0.25f;
int g_boreAxis = kBoreYaw;
bool g_boreAdjust = true;

// Which hand's profile is live. Suffixes the saved keys so BOTH handedness
// tunings coexist in one cfg -- a player who switches hands gets the right
// numbers back rather than the other hand's, and nothing has to declare which
// profile the file was written for. left_handed already says.
bool g_boreLeftHanded = false;
const char* BoreHandSuffix() { return g_boreLeftHanded ? "left" : "right"; }

BoreOffset* FindBore( const char* model, bool create )
{
	if ( !model || !model[0] )
		return nullptr;

	for ( int i = 0; i < g_boreCount; ++i )
		if ( _stricmp( g_bore[i].model, model ) == 0 )
			return &g_bore[i];

	if ( !create || g_boreCount >= (int)( sizeof( g_bore ) / sizeof( g_bore[0] ) ) )
		return nullptr;

	BoreOffset& b = g_bore[g_boreCount++];
	strcpy_s( b.model, sizeof( b.model ), model );
	// Seeded from the global so a first nudge moves from where the player was
	// already aiming, not from zero.
	b.right = g_boreGlobalRight;
	b.up = g_boreGlobalUp;
	b.fwd = g_boreGlobalFwd;
	b.yaw = g_boreGlobalYaw;
	b.pitch = g_boreGlobalPitch;
	return &b;
}

// What the aim should actually use this frame.
void BoreFor( const char* model, float& right, float& up, float& fwd,
			  float& yaw, float& pitch )
{
	const BoreOffset* b = FindBore( model, false );
	right = b ? b->right : g_boreGlobalRight;
	up = b ? b->up : g_boreGlobalUp;
	fwd = b ? b->fwd : g_boreGlobalFwd;
	yaw = b ? b->yaw : g_boreGlobalYaw;
	pitch = b ? b->pitch : g_boreGlobalPitch;
}

// `dir` is -1 or +1; the axis decides what that means and how big a step it is.
void NudgeBore( float dir )
{
	const char* model = g_viewModel.CurrentModelKey();
	BoreOffset* b = FindBore( model, true );
	if ( !b )
	{
		g_boreGlobalRight += dir * g_boreStep;
		Log( "aim bore: global right=%.2f -- no weapon resolved, editing the "
			 "global", g_boreGlobalRight );
		return;
	}

	switch ( g_boreAxis )
	{
		case kBoreYaw:     b->yaw   += dir * g_boreAngleStep; break;
		case kBoreLateral: b->right += dir * g_boreStep;      break;
		case kBorePitch:   b->pitch += dir * g_boreAngleStep; break;
		case kBoreUp:      b->up    += dir * g_boreStep;      break;
		default:           b->fwd   += dir * g_boreStep;      break;
	}

	Log( "aim bore: %-16s yaw=%6.2f pitch=%6.2f right=%7.2f up=%6.2f fwd=%6.2f   [%s]",
		 b->model, b->yaw, b->pitch, b->right, b->up, b->fwd,
		 BoreAxisName( g_boreAxis ) );
}

void CycleBoreAxis()
{
	g_boreAxis = ( g_boreAxis + 1 ) % kBoreAxisCount;
	Log( "aim bore: , and . now drive %s", BoreAxisName( g_boreAxis ) );
}

void SaveBore()
{
	const char* model = g_viewModel.CurrentModelKey();
	const BoreOffset* b = FindBore( model, false );

	char kr[64], ku[64];
	if ( b )
	{
		_snprintf_s( kr, sizeof( kr ), _TRUNCATE, "aim_bore_right_%s_%s",
					 b->model, BoreHandSuffix() );
		_snprintf_s( ku, sizeof( ku ), _TRUNCATE, "aim_bore_up_%s_%s",
					 b->model, BoreHandSuffix() );
	}
	else
	{
		strcpy_s( kr, sizeof( kr ), "aim_bore_right" );
		strcpy_s( ku, sizeof( ku ), "aim_bore_up" );
	}

	char ky[64], kp[64], kf[64];
	if ( b )
	{
		_snprintf_s( ky, sizeof( ky ), _TRUNCATE, "aim_bore_yaw_%s_%s",
					 b->model, BoreHandSuffix() );
		_snprintf_s( kp, sizeof( kp ), _TRUNCATE, "aim_bore_pitch_%s_%s",
					 b->model, BoreHandSuffix() );
		_snprintf_s( kf, sizeof( kf ), _TRUNCATE, "aim_bore_fwd_%s_%s",
					 b->model, BoreHandSuffix() );
	}
	else
	{
		strcpy_s( ky, sizeof( ky ), "aim_bore_yaw" );
		strcpy_s( kp, sizeof( kp ), "aim_bore_pitch" );
		strcpy_s( kf, sizeof( kf ), "aim_bore_fwd" );
	}

	const float r = b ? b->right : g_boreGlobalRight;
	const float u = b ? b->up : g_boreGlobalUp;
	const float fw = b ? b->fwd : g_boreGlobalFwd;
	const float y = b ? b->yaw : g_boreGlobalYaw;
	const float pi = b ? b->pitch : g_boreGlobalPitch;

	char vr[32], vu[32], vy[32], vp[32], vf[32];
	_snprintf_s( vr, sizeof( vr ), _TRUNCATE, "%.2f", r );
	_snprintf_s( vu, sizeof( vu ), _TRUNCATE, "%.2f", u );
	_snprintf_s( vy, sizeof( vy ), _TRUNCATE, "%.2f", y );
	_snprintf_s( vp, sizeof( vp ), _TRUNCATE, "%.2f", pi );
	_snprintf_s( vf, sizeof( vf ), _TRUNCATE, "%.2f", fw );

	// All FIVE together. Saving only the axis last touched would leave the
	// others behind, and the five are one calibration.
	const char* keys[] = { kr, ku, ky, kp, kf };
	const char* values[] = { vr, vu, vy, vp, vf };

	if ( Config::WriteKeys( SiblingPath( L"sinvr.cfg" ), keys, values, 5 ) )
		Log( "aim bore: SAVED %s -- yaw=%.2f pitch=%.2f right=%.2f up=%.2f fwd=%.2f",
			 b ? b->model : "global", y, pi, r, u, fw );
	else
		LogError( "aim bore: could NOT write sinvr.cfg. The only record is this "
				  "line: yaw=%.2f pitch=%.2f right=%.2f up=%.2f fwd=%.2f",
				  y, pi, r, u, fw );
}

//-----------------------------------------------------------------------------
void SaveViewModelOffsets()
{
	const ModelOffsets off = g_viewModel.Eff();
	const char* model = g_viewModel.CurrentModelKey();

	char kf[64], kr[64], ku[64];
	if ( model )
	{
		_snprintf_s( kf, sizeof( kf ), _TRUNCATE, "viewmodel_offset_forward_%s", model );
		_snprintf_s( kr, sizeof( kr ), _TRUNCATE, "viewmodel_offset_right_%s", model );
		_snprintf_s( ku, sizeof( ku ), _TRUNCATE, "viewmodel_offset_up_%s", model );
	}
	else
	{
		// No model resolved -- save the globals, which is what was being
		// edited in that case anyway.
		strcpy_s( kf, sizeof( kf ), "viewmodel_offset_forward" );
		strcpy_s( kr, sizeof( kr ), "viewmodel_offset_right" );
		strcpy_s( ku, sizeof( ku ), "viewmodel_offset_up" );
	}

	char vf[32], vr[32], vu[32], vp[32], vy[32], vro[32];
	_snprintf_s( vf, sizeof( vf ), _TRUNCATE, "%.2f", off.forward );
	_snprintf_s( vr, sizeof( vr ), _TRUNCATE, "%.2f", off.right );
	_snprintf_s( vu, sizeof( vu ), _TRUNCATE, "%.2f", off.up );
	_snprintf_s( vp, sizeof( vp ), _TRUNCATE, "%.2f", g_viewModel.Settings().anglePitch );
	_snprintf_s( vy, sizeof( vy ), _TRUNCATE, "%.2f", g_viewModel.Settings().angleYaw );
	_snprintf_s( vro, sizeof( vro ), _TRUNCATE, "%.2f", g_viewModel.Settings().angleRoll );

	const char* keys[] = { kf, kr, ku, "viewmodel_angle_pitch",
			   "viewmodel_angle_yaw", "viewmodel_angle_roll" };
	const char* values[] = { vf, vr, vu, vp, vy, vro };

	if ( Config::WriteKeys( SiblingPath( L"sinvr.cfg" ), keys, values, 6 ) )
	{
		Log( "viewmodel: SAVED to sinvr.cfg -- %s fwd=%.2f right=%.2f up=%.2f, angles %.2f/%.2f/%.2f",
			 model ? model : "global", off.forward, off.right, off.up,
			 g_viewModel.Settings().anglePitch, g_viewModel.Settings().angleYaw,
			 g_viewModel.Settings().angleRoll );
	}
	else
	{
		LogError( "viewmodel: could NOT write sinvr.cfg -- the values below are the only record" );
		g_viewModel.LogOffsets( "save failed" );
	}
}

// Re-submitted inside every eye pass. See StereoRenderer::SetPreEyePass:
// the engine draws its overlay list during the first pass and clears it, so
// anything submitted once per frame reaches one eye only.
void PreEyePass( int eye )
{
	// CLEAR FIRST, then submit. The engine holds overlays for about a tick,
	// which at 90 fps is longer than a frame -- so without this, last
	// frame's copy is still in the list at last frame's world position and
	// draws alongside this one. That is the ghosting, and it only showed
	// when the view moved because a still head leaves the stale copy sitting
	// exactly on top of the fresh one.
	//
	// Both clears are called because either may be the bound one; each is a
	// no-op when its interface is not, and the second finds nothing left.
	g_laserDot.ClearOverlays();
	g_zones.ClearOverlays();

	g_zones.Submit();
	g_handMarker.Submit();
	g_menuPointer.Submit();
	g_laserDot.Submit( eye );
	g_aimDebug.Submit();
}

// Runs at the END of each eye pass, after the engine has drawn everything and
// before the backbuffer is copied into the eye surface. See
// StereoRenderer::SetPostEyePass.
//
// Exactly one thing happens here, and it is inert unless a menu is up. Anything
// added to this function is drawing ON TOP of the finished frame, which is a
// privilege worth keeping scarce.
// ---- DID THE LAUNCHER'S RESOLUTION ACTUALLY LAND? --------------------------
//
// The game window size IS the per-eye render resolution, and it is chosen before
// this process starts -- so all the mod can do is check the result and say so.
// Worth saying because the failure is silent and expensive: a wrong ASPECT still
// renders correctly (ComputeEyeFrustum extends the frustum and crops per eye),
// it just throws away GPU work, so nothing looks broken while a chunk of every
// frame is being rendered and discarded.
//
// Logged ONCE, when the backbuffer is first known -- not per heartbeat, because
// it cannot change while the process lives.
void ReportResolution()
{
	static bool s_reported = false;
	if ( s_reported || !g_vr || !g_vr->IsReady() )
		return;

	unsigned int bbW = 0, bbH = 0;
	Stereo().BackbufferSize( bbW, bbH );
	if ( bbW == 0 || bbH == 0 )
		return;      // eye surfaces not built yet; try again next frame

	s_reported = true;

	unsigned int recW = 0, recH = 0;
	g_vr->RecommendedRenderTargetSize( recW, recH );

	const float bbAspect = (float)bbW / (float)bbH;
	const float recAspect = ( recH > 0 ) ? ( (float)recW / (float)recH ) : 0.0f;

	Log( "resolution: rendering %ux%u per eye (aspect %.3f) | runtime recommends "
		 "%ux%u (aspect %.3f) | vr_auto_resolution=%d scale=%.3f oversize=%d "
		 "max_height=%.0f",
		 bbW, bbH, bbAspect, recW, recH, recAspect,
		 g_autoResolution ? 1 : 0, g_resolutionScale,
		 g_allowOversizeWindow ? 1 : 0, g_maxRenderHeight );

	if ( recAspect <= 0.01f )
		return;

	// The aspect is the part that costs something when it is wrong. A 2% band
	// covers rounding to even pixel counts without waving through a real
	// mismatch.
	const float ratio = bbAspect / recAspect;
	if ( ratio < 0.98f || ratio > 1.02f )
	{
		const float waste = ( ratio > 1.0f ) ? ( 1.0f - 1.0f / ratio )
											 : ( 1.0f - ratio );
		LogWarn( "resolution: the render aspect is %.1f%% off the headset's. The "
				 "image is still geometrically correct -- the frustum is extended "
				 "and cropped per eye -- but roughly %.0f%% of every rendered "
				 "frame is thrown away before submission. Remove -w/-h from "
				 "Steam's launch options and let vr_auto_resolution pick them.",
				 ( ratio > 1.0f ? ratio - 1.0f : 1.0f - ratio ) * 100.0f,
				 waste * 100.0f );
	}
	else if ( bbW != recW || bbH != recH )
	{
		const float effective = ( recW > 0 ) ? (float)bbW / (float)recW : 0.0f;

		// The effective scale can be LOWER than the requested one, and that is
		// not a fault: the launcher clamps the window to the physical desktop,
		// because the engine refuses a windowed mode larger than the display
		// ("Failed to set video mode - resetting to defaults"). Saying so here
		// stops the two numbers reading as a bug -- the mod cannot see the
		// clamp, it only sees the result.
		Log( "resolution: aspect matches the headset. Requested scale %.3f, "
			 "effective %.3f%s. Lower it for performance, raise it for sharpness "
			 "-- the scene is rendered TWICE per frame, so cost goes with the "
			 "square.",
			 g_resolutionScale, effective,
			 ( effective < g_resolutionScale - 0.01f )
				 ? ( g_allowOversizeWindow
					 ? " -- lower than requested even though "
					   "vr_allow_oversize_window = 1, so something OTHER than "
					   "the desktop clamp reduced it; read the launcher's output"
					 : " -- lower than requested, so the launcher clamped it to "
					   "fit the desktop. Set vr_allow_oversize_window = 1 to "
					   "render at the headset's size anyway" )
				 : "" );
	}
}

void PostEyePass( int eye )
{
	g_menuCursor.Draw( eye );
	g_lookArrow.Draw( eye );
}

void HandleHotKeys()
{
	if ( g_keyToggle.Pressed() )
	{
		g_vrCameraEnabled = !g_vrCameraEnabled;
		if ( !g_vrCameraEnabled )
			g_camera.Release();
		else
			g_camera.Recenter();
		Log( "camera: VR camera %s", g_vrCameraEnabled ? "ENABLED" : "disabled" );
	}

	if ( g_keyRecenter.Pressed() )
		g_camera.Recenter();

	if ( g_keyRoll.Pressed() )
	{
		g_camera.SetApplyRoll( !g_camera.ApplyRoll() );
		Log( "camera: roll %s", g_camera.ApplyRoll() ? "on" : "off" );
	}

	// Live body-zone tuning.
	//
	// ABOVE the eye-alignment guard for exactly the reason the viewmodel
	// block below documents: that guard returns early when
	// eye_adjust_enabled is off, and this requires it to be off. Putting it
	// underneath would make it unreachable in the only configuration that
	// enables it -- the bug that cost the viewmodel tuner a session.
	// Ahead of the zone block for the same reason that one sits where it does:
	// whichever adjuster is enabled must be reachable, and only one may be on.
	if ( g_menuPanelAdjust )
	{
		const float d = g_menuPanelStep;
		if ( g_keyMpFurther.Pressed() )   g_menuPanelSettings.distance += d;
		if ( g_keyMpNearer.Pressed() )    g_menuPanelSettings.distance -= d;
		if ( g_keyMpLeft.Pressed() )      g_menuPanelSettings.yawOffset -= d;
		if ( g_keyMpRight.Pressed() )     g_menuPanelSettings.yawOffset += d;
		if ( g_keyMpUp.Pressed() )        g_menuPanelSettings.heightOffset += d;
		if ( g_keyMpDown.Pressed() )      g_menuPanelSettings.heightOffset -= d;
		if ( g_keyMpPitchUp.Pressed() )   g_menuPanelSettings.pitch += d;
		if ( g_keyMpPitchDown.Pressed() ) g_menuPanelSettings.pitch -= d;
		if ( g_keyMpRoll.Pressed() )      g_menuPanelSettings.roll += d;
		if ( g_keyMpStepDown.Pressed() )  g_menuPanelStep *= 0.5f;
		if ( g_keyMpStepUp.Pressed() )    g_menuPanelStep *= 2.0f;

		if ( g_menuPanelSettings.distance < 20.0f )
			g_menuPanelSettings.distance = 20.0f;

		// Reset is confirm-on-second-press: one fat-fingered numpad 5 should not
		// throw away a tuning session, which is the lesson the viewmodel tuner
		// paid for.
		if ( g_keyMpReset.Pressed() )
		{
			const DWORD now = GetTickCount();
			if ( g_lastMpResetMs != 0 && ( now - g_lastMpResetMs ) < kResetDoubleClickMs )
			{
				g_menuPanelSettings.yawOffset = 0.0f;
				g_menuPanelSettings.heightOffset = 0.0f;
				g_menuPanelSettings.pitch = 0.0f;
				g_menuPanelSettings.roll = 0.0f;
				g_menuPanelSettings.distance = 150.0f;
				g_lastMpResetMs = 0;
				Log( "menu panel: RESET" );
			}
			else
			{
				g_lastMpResetMs = now;
				Log( "menu panel: press numpad 5 again within %.1fs to reset",
					 kResetDoubleClickMs / 1000.0f );
			}
		}

		// Printed as cfg lines so a tuning session ends with numbers to keep,
		// rather than with a feeling.
		if ( g_keyMpDump.Pressed() )
		{
			Log( "menu panel: paste these into sinvr.cfg --" );
			Log( "menu_anchor_distance = %.1f", g_menuPanelSettings.distance );
			Log( "menu_anchor_yaw = %.1f", g_menuPanelSettings.yawOffset );
			Log( "menu_anchor_height = %.1f", g_menuPanelSettings.heightOffset );
			Log( "menu_panel_pitch = %.1f", g_menuPanelSettings.pitch );
			Log( "menu_panel_roll = %.1f", g_menuPanelSettings.roll );
			Log( "menu_panel_width = %.1f", g_menuPanelSettings.width );
		}
	}

	if ( g_zoneAdjust )
	{
		if ( g_keyZoneNext.Pressed() )    g_zones.SelectNext();
		if ( g_keyZoneMode.Pressed() )    g_zones.ToggleMode();
		if ( g_keyZoneForward.Pressed() ) g_zones.Nudge( 0, +1.0f );
		if ( g_keyZoneBack.Pressed() )    g_zones.Nudge( 0, -1.0f );
		if ( g_keyZoneRight.Pressed() )   g_zones.Nudge( 1, +1.0f );
		if ( g_keyZoneLeft.Pressed() )    g_zones.Nudge( 1, -1.0f );
		if ( g_keyZoneUp.Pressed() )      g_zones.Nudge( 2, +1.0f );
		if ( g_keyZoneDown.Pressed() )    g_zones.Nudge( 2, -1.0f );
		if ( g_keyZoneStepDown.Pressed() ) g_zones.ScaleStep( 0.5f );
		if ( g_keyZoneStepUp.Pressed() )   g_zones.ScaleStep( 2.0f );
		if ( g_keyZoneSnap.Pressed() && g_vr )
			g_zones.SnapToHand( *g_vr );
		if ( g_keyZoneDump.Pressed() )
			g_zones.LogAsConfig();

		// Reset needs TWO presses, close together -- the same guard the
		// viewmodel tuner has, for the same reason: every other key here
		// nudges by a step and can be nudged back, this one throws the zone
		// away, and it sits in the middle of the four direction keys.
		if ( g_keyZoneReset.Pressed() )
		{
			const DWORD now = GetTickCount();
			if ( g_lastZoneResetMs != 0 &&
				 (DWORD)( now - g_lastZoneResetMs ) <= kResetDoubleClickMs )
			{
				g_lastZoneResetMs = 0;
				g_zones.ResetSelected();
			}
			else
			{
				g_lastZoneResetMs = now;
				Log( "zones: press numpad 5 again within %.1fs to reset '%s' to "
						  "the values this session started with",
						  kResetDoubleClickMs / 1000.0f,
						  ZoneStyleFor( g_zones.Selected() ).name );
			}
		}
	}

	// Live viewmodel offset tuning.
	//
	// MUST come before the eye-alignment guard below. That guard returns early
	// when eye_adjust_enabled is off -- and viewmodel adjust requires it to be
	// off, since the two share the numpad. Sitting under it meant this block
	// could never run under the only configuration that enables it, which is
	// exactly what happened: "viewmodel adjust: ON" in the log, and zero nudges.
	// Bore zeroing. Deliberately NOT gated on g_viewModelAdjust: that mode owns
	// the numpad and turning it on moves the gun about, which is the last thing
	// wanted while sighting down it.
	if ( g_boreAdjust )
	{
		if ( g_keyBoreAxis.Pressed() )  CycleBoreAxis();
		if ( g_keyBoreLeft.Pressed() )  NudgeBore( -1.0f );
		if ( g_keyBoreRight.Pressed() ) NudgeBore( +1.0f );
		if ( g_keyBoreSave.Pressed() )  SaveBore();
	}

	if ( g_viewModelAdjust )
	{
		if ( g_keyVmForward.Pressed() ) g_viewModel.Nudge( +1.0f, 0.0f, 0.0f );
		if ( g_keyVmBack.Pressed() )    g_viewModel.Nudge( -1.0f, 0.0f, 0.0f );
		if ( g_keyVmRight.Pressed() )   g_viewModel.Nudge( 0.0f, +1.0f, 0.0f );
		if ( g_keyVmLeft.Pressed() )    g_viewModel.Nudge( 0.0f, -1.0f, 0.0f );
		if ( g_keyVmUp.Pressed() )      g_viewModel.Nudge( 0.0f, 0.0f, +1.0f );
		if ( g_keyVmDown.Pressed() )    g_viewModel.Nudge( 0.0f, 0.0f, -1.0f );
		// Reset needs TWO presses, close together.
		//
		// It is the one key here that destroys work: every other key nudges by a
		// step and can be nudged back, but reset zeroes a weapon that may have
		// taken a while to dial in, and it sits directly between the four
		// direction keys on the numpad. One fat-fingered press was costing a
		// tuning session.
		if ( g_keyVmReset.Pressed() )
		{
			const DWORD now = GetTickCount();
			if ( g_lastResetPressMs != 0 &&
				 (DWORD)( now - g_lastResetPressMs ) <= kResetDoubleClickMs )
			{
				g_lastResetPressMs = 0;
				g_viewModel.ResetOffsets();
			}
			else
			{
				g_lastResetPressMs = now;
				Log( "viewmodel adjust: press numpad 5 again within %.1fs to RESET "
					 "this weapon's offsets to zero -- a single press does nothing",
					 kResetDoubleClickMs / 1000.0f );
			}
		}
		if ( g_keyVmMode.Pressed() )    g_viewModel.ToggleAdjustMode();
		if ( g_keyVmDump.Pressed() )
		{
			g_viewModel.LogOffsets( "current" );
			SaveViewModelOffsets();
		}
		if ( g_keyVmStepDown.Pressed() ) g_viewModel.ChangeStep( 0.5f );
		if ( g_keyVmStepUp.Pressed() )   g_viewModel.ChangeStep( 2.0f );
	}

	if ( !Stereo().Ready() || !Stereo().LiveAdjustEnabled() )
		return;

	if ( g_keySelectLeft.Pressed() )  Stereo().SelectEye( kEyeLeft );
	if ( g_keySelectRight.Pressed() ) Stereo().SelectEye( kEyeRight );

	if ( g_keyNudgeLeft.Pressed() )  Stereo().NudgeSelectedEye( -1.0f, 0.0f );
	if ( g_keyNudgeRight.Pressed() ) Stereo().NudgeSelectedEye( 1.0f, 0.0f );
	if ( g_keyNudgeUp.Pressed() )    Stereo().NudgeSelectedEye( 0.0f, -1.0f );
	if ( g_keyNudgeDown.Pressed() )  Stereo().NudgeSelectedEye( 0.0f, 1.0f );

	if ( g_keyResetEye.Pressed() ) Stereo().ResetSelectedEye();
	if ( g_keyStepDown.Pressed() ) Stereo().ChangeStep( 0.5f );
	if ( g_keyStepUp.Pressed() )   Stereo().ChangeStep( 2.0f );

}

//-----------------------------------------------------------------------------
// Push the engine's world-visibility settings back where we want them.
//
// Everything here is aimed at one thing: the engine deciding not to draw world
// geometry the headset can see. Four separate mechanisms can do that, and only
// the first is reachable through an interface we have hooked.
//
//   frustum      widened by raising the game's FOV -- the only lever, since the
//                planes themselves are built by angle inside engine.dll
//   area portals computed in the *engine's* screen space, so a doorway that sits
//                outside the engine's narrower frustum closes the whole area
//                behind it. r_portalsopenall skips the test entirely.
//   occluders    func_occluder volumes, sized for a 75-degree view
//   PVS          origin-based, so it cannot explain anything that changes as the
//                head turns -- off by default, since r_novis is expensive and
//                the evidence does not point at it
//
// None of these change the rendered image directly: the projection is replaced
// per eye regardless. They only change what the engine bothers to submit.
//-----------------------------------------------------------------------------
void ApplyEngineTuning( const char* reason )
{
	if ( !g_engine.Valid() )
		return;

	char cmd[96];
	++g_engineTuneCount;

	if ( g_allowCheats )
		g_engine.ClientCmd( "sv_cheats 1\n" );

	int fov = g_engineFov;

	// When stereo owns CViewSetup it writes the culling FOV straight into the
	// view every frame, which is exact, immediate and cannot be reverted by a
	// level load. Sending cvars as well would only fight it.
	if ( Stereo().OwnsViewSetup() )
		fov = -1;
	else if ( g_engineFovIsAuto )
	{
		// Only widen when stereo is actually replacing the projection. Without
		// that, a wider engine FOV would change the picture rather than just what
		// the engine keeps, which is not what this setting is for.
		fov = 0;
		if ( Stereo().Ready() )
		{
			// Fall back rather than skip. The render aspect is not known until
			// the first Present creates the eye surfaces, and spending the first
			// seconds of a map at the game's own 75 is precisely the failure this
			// exists to prevent.
			//
			// The fallback is derived from the headset's own frustum rather than
			// being a fixed number, because a fixed number is only ever right for
			// the headset it was picked on. It omits the aspect extension, which
			// is the part that needs the render target, so it can be slightly
			// narrow for the frame or two before the real value takes over.
			float required = Stereo().RequiredEngineFovX();
			if ( required <= 0.0f )
				required = Stereo().FallbackEngineFovX();

			fov = ( required > 0.0f ) ? (int)( required + 0.5f ) : 0;
		}
	}

	if ( fov > 0 )
	{
		// Three names because SiN's cvar set is not documented and they are not
		// equivalent: fov_desired is the archived user setting and is bounds-
		// clamped, default_fov is cheat-flagged and unclamped, fov is the direct
		// override. An unknown name is a harmless console warning. Which of them
		// actually took is answered by the "stereo frustum:" heartbeat line, not
		// by anything here.
		_snprintf_s( cmd, sizeof( cmd ), _TRUNCATE, "fov_desired %d\n", fov );
		g_engine.ClientCmd( cmd );
		_snprintf_s( cmd, sizeof( cmd ), _TRUNCATE, "default_fov %d\n", fov );
		g_engine.ClientCmd( cmd );
		_snprintf_s( cmd, sizeof( cmd ), _TRUNCATE, "fov %d\n", fov );
		g_engine.ClientCmd( cmd );
	}

	// Source's crosshair is a HUD element drawn at SCREEN CENTRE -- it is not a
	// world-space aim indicator. Once the weapon aims independently of the head
	// it therefore shows the middle of your view, which is precisely where you
	// are NOT shooting, and reading it as the aim point is actively misleading.
	//
	// CHudCrosshair CAN draw off-centre -- Paint() projects
	// (m_curViewAngles + m_vecCrossHairOffsetAngle) through ScreenTransform for
	// autoaim -- but it skips that path entirely when the offset is zero, and
	// under our setup the engine's view angles ARE the aim angles, so the honest
	// offset is exactly zero. Hiding it beats shipping an epsilon-nudge hack; a
	// real laser sight is the proper answer and is a separate job.
	if ( g_hideCrosshairWhenAiming )
		g_engine.ClientCmd( g_camera.GetAimSource() == kAimController
								? "crosshair 0\n" : "crosshair 1\n" );

	if ( g_openAllPortals )
		g_engine.ClientCmd( "r_portalsopenall 1\n" );
	if ( g_disableOcclusion )
		g_engine.ClientCmd( "r_occlusion 0\n" );
	if ( g_disableVis )
		g_engine.ClientCmd( "r_novis 1\n" );

	// Only the first few are worth a line each; after that it is just noise, and
	// the heartbeat reports the effect rather than the intent. Values shown are
	// the cvar values sent, with '-' for "not sent at all".
	if ( g_engineTuneCount <= 3 )
		Log( "engine tuning (%s, #%d): fov=%d%s | sv_cheats=%s r_portalsopenall=%s "
			 "r_occlusion=%s r_novis=%s",
			 reason, g_engineTuneCount, fov, g_engineFovIsAuto ? " (auto)" : "",
			 g_allowCheats ? "1" : "-", g_openAllPortals ? "1" : "-",
			 g_disableOcclusion ? "0" : "-", g_disableVis ? "1" : "-" );
}

void MaintainEngineTuning( bool inGame )
{
	if ( !inGame )
	{
		g_wasInGame = false;
		return;
	}

	const DWORD now = GetTickCount();

	if ( !g_wasInGame )
	{
		g_wasInGame = true;
		g_lastEngineTuneMs = now;
		ApplyEngineTuning( "entered map" );
		return;
	}

	if ( g_engineTuneIntervalMs > 0 &&
		 (DWORD)( now - g_lastEngineTuneMs ) >= (DWORD)g_engineTuneIntervalMs )
	{
		g_lastEngineTuneMs = now;
		ApplyEngineTuning( "periodic" );
	}
}

//-----------------------------------------------------------------------------
// Viewmodel transform, sampled around each eye pass.
//
// The flicker to investigate: when firing, the gun appears to flash sideways.
// The suspicion is structural rather than cosmetic. Our transform write happens
// ONCE per frame, before RenderBothEyes -- but RenderBothEyes runs the engine's
// whole View_Render twice, and SetUpView re-poses the viewmodel from THAT EYE's
// view origin inside each pass. If the engine's re-pose wins in one eye and
// ours wins in the other, the two eyes disagree about where the gun is, and
// binocular rivalry of that kind reads exactly as a model flashing off to one
// side. Firing would make it worse because the punch moves the engine's view
// origin, so the two candidate positions are furthest apart at that moment.
//
// This does not fix anything -- it establishes whether that is what is
// happening, by printing the transform before and after each pass. Guessing at
// a fix without it would be the third confident wrong diagnosis on this file.
//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
// Every weapon-shaped renderable in the world, dumped at the instant of firing.
//
// RESOLVED, kept because the tool is still the right way to enumerate weapon
// renderables at the instant of firing. The second gun model is the C_Weapon*
// entity's own v_ model at eye height -- now pinned and hidden by
// weapon_entity_pin / weapon_entity_hide. The muzzle flash was NOT attached to
// it: that was FormatViewModelAttachment, see viewmodel_fov_match.
//
// The earlier layout scan reported m_hViewModel[1..3] empty, but it sampled
// every three seconds while standing still. If the second model only exists
// during a shot, that scan could not have seen it. This one runs on the frame a
// firing animation starts.
//
// Read-only. m_nModelIndex and m_vecAbsOrigin are C_BaseEntity fields, so the
// offsets resolved for the viewmodel are valid for any client entity.
//-----------------------------------------------------------------------------
int g_fireDumpsLeft = 0;

void DumpFiringEntities()
{
	if ( g_fireDumpsLeft <= 0 || !g_entities.Valid() || !g_viewModelAnim.Bound() )
		return;
	--g_fireDumpsLeft;

	const int modelIdxOff = g_viewModelAnim.ModelIndexOffset();
	if ( modelIdxOff < 0 )
		return;

	const int highest = g_entities.GetHighestEntityIndex();
	Log( "=== firing entity dump (%d dumps left, %d entities) ===",
		 g_fireDumpsLeft, highest );

	int shown = 0;
	for ( int i = 1; i <= highest && i < 2048 && shown < 24; ++i )
	{
		void* ent = g_entities.GetClientEntity( i );
		if ( !ent )
			continue;

		int modelIndex = 0;
		Vector origin = { 0.0f, 0.0f, 0.0f };
		__try
		{
			auto* b = reinterpret_cast<unsigned char*>( ent );
			modelIndex = *reinterpret_cast<const int*>( b + modelIdxOff );
			origin = *reinterpret_cast<const Vector*>( b + viewmodel_offset::kAbsOrigin );
		}
		__except ( EXCEPTION_EXECUTE_HANDLER )
		{
			continue;
		}

		if ( modelIndex <= 0 )
			continue;

		const char* modelName = g_viewModelAnim.LookupModelName( modelIndex );
		if ( !modelName || !strstr( modelName, "weapons/" ) )
			continue;

		++shown;
		Log( "  ent[%4d] %-22s %-46s abs=(%.1f %.1f %.1f)",
			 i, GetRttiName( ent ), modelName, origin.x, origin.y, origin.z );
	}

	// And the handle array again, this time at the moment it matters.
	const int playerIndex = g_engine.GetLocalPlayer();
	void* player = ( playerIndex > 0 ) ? g_entities.GetClientEntity( playerIndex ) : nullptr;
	if ( player )
	{
		__try
		{
			for ( int i = 0; i < 4; ++i )
			{
				const unsigned int h = *reinterpret_cast<unsigned int*>(
					reinterpret_cast<unsigned char*>( player ) +
					player_offset::kViewModelHandle + i * 4 );
				if ( h == 0 || h == 0xFFFFFFFFu )
					continue;
				void* e = g_entities.GetClientEntityFromHandle( h );
				Log( "  m_hViewModel[%d] -> %p  %s", i, e, e ? GetRttiName( e ) : "<null>" );
			}
		}
		__except ( EXCEPTION_EXECUTE_HANDLER )
		{
			}
	}
	Log( "=== end firing entity dump ===" );
}

void TraceViewModel( const char* tag )
{
	if ( g_vmTraceFrames <= 0 )
		return;

	void* vm = g_viewModel.ResolveViewModel( g_engine );
	if ( !vm )
	{
		Log( "[vmtrace] %-5s eye=%d  <no viewmodel>", tag, Stereo().CurrentEye() );
		return;
	}

	auto* b = reinterpret_cast<unsigned char*>( vm );
	const Vector loc = *reinterpret_cast<const Vector*>( b + viewmodel_offset::kOrigin );
	const Vector o = *reinterpret_cast<const Vector*>( b + viewmodel_offset::kAbsOrigin );
	const float* m = reinterpret_cast<const float*>( b + viewmodel_offset::kCoordinateFrame );

	// LOCAL is the one that matters and the one every previous trace ignored.
	// CBaseViewModel::CalcViewModelView ends in SetLocalOrigin/SetLocalAngles,
	// which marks the abs transform dirty -- so abs origin and the coordinate
	// frame are both REBUILT FROM LOCAL at draw time, discarding anything we
	// wrote to them. If local changes across a pass, the engine is writing it
	// inside SetUpView and no amount of writing the derived fields can win.
	Log( "[vmtrace] %-5s eye=%d local=(%.2f %.2f %.2f) abs=(%.2f %.2f %.2f) frame=(%.2f %.2f %.2f)",
		 tag, Stereo().CurrentEye(), loc.x, loc.y, loc.z, o.x, o.y, o.z,
		 m[3], m[7], m[11] );
}

void __fastcall Detour_View_Render( void* thisptr, void* edx, vrect_t* rect )
{
	++g_frames;
	g_renderThreadId = GetCurrentThreadId();

	if ( g_frames == 1 )
		Log( "View_Render hook is live (first call, rect=%p, tid=%lu)",
			 rect, g_renderThreadId );

	HandleHotKeys();

	// One-shot, and self-deferring until the eye surfaces exist. A not-ready
	// value must never latch a conclusion -- that habit has cost this project
	// two sessions in two different subsystems.
	ReportResolution();

	// Needs a live game before the cvars exist, so this runs per frame rather
	// than at init -- and keeps running, because a level load undoes it.
	const bool inGame = g_engine.Valid() && g_engine.IsInGame();
	MaintainEngineTuning( inGame );

	// ---- IS A MENU UP? -----------------------------------------------------
	//
	// `IsInGame()` CANNOT answer this, and that is the whole reason the main
	// menu was unusable: **the main menu runs a background map, so IsInGame()
	// returns true there.** Every gameplay gate in this file keys off it, so on
	// the menu the aim decoupling, the gesture detectors, the holsters and the
	// world overlays were all running as if the player were in a firefight.
	//
	// The OS cursor is the signal that does work -- Source shows it for menus
	// and hides it during gameplay. Sampled exactly ONCE per frame, here at the
	// top, and pushed to everything that needs it: the camera reads it before
	// it composes angles, and the Ortho hook reads it during the eye passes. Two
	// samplers would straddle the frame a menu opens and disagree.
	//
	// ---- AND IT NEEDS A WAY OUT --------------------------------------------
	//
	// One OS-level boolean gates the entire gameplay input path. When it is
	// wrong there is no recovery: aiming, turning and every gesture are dead
	// and the player has no way to say so. `menu_gating = 0` was the only
	// escape, and turning it off re-breaks the MAIN MENU -- the camera then
	// accumulates the background map's scripted yaw and the view locks. So the
	// kill switch was never a real answer; both of its positions are broken in
	// a different place.
	//
	// Two cross-checks, both cheap:
	//
	//   DEBOUNCE   the cursor must be seen for several consecutive frames
	//              before menu mode LATCHES. Entering is debounced, leaving is
	//              immediate -- being wrongly in menu mode is the expensive
	//              failure, so the asymmetry is deliberate.
	//   LIVENESS   if the player is feeding real gameplay input while the mod
	//              believes a menu is up, the MOD IS WRONG. Menus do not
	//              produce stick movement and a trigger at the same time.
	//
	// The liveness test deliberately ignores the trigger ALONE: the menu
	// pointer clicks with it, so a trigger during a menu is expected. It wants
	// evidence no menu produces -- the movement stick pushed past its deadzone.
	const bool cursorShown = IsInteractiveUiVisible();

	static unsigned int s_cursorFrames = 0;
	s_cursorFrames = cursorShown ? ( s_cursorFrames + 1 ) : 0;
	const bool cursorLatched = s_cursorFrames >= kMenuLatchFrames;

	bool uiVisible = g_menuGating && cursorLatched;

	if ( uiVisible && g_vr && g_vr->IsReady() )
	{
		const VRInputState& in = g_vr->Input();
		const float move = fabsf( in.moveX ) + fabsf( in.moveY );
		if ( in.valid && move > kMenuEscapeMove )
		{
			uiVisible = false;
			if ( ++g_menuEscapes == 1 || ( g_menuEscapes % 100 ) == 0 )
				LogWarn( "menu: LIVENESS ESCAPE (%u) -- the cursor is reported "
						 "visible but the movement stick is at %.2f, which no "
						 "menu produces. Treating this as gameplay. If this "
						 "repeats, the cursor is being left shown after a menu "
						 "closes and IsInteractiveUiVisible is the thing to fix.",
						 g_menuEscapes, move );
		}
	}

	Stereo().SetUiVisible( uiVisible );
	g_camera.SetUiMode( uiVisible );

	// Head tracking, stereo rendering and frame submission stay live on the
	// menu -- the player still has a head and still needs to look around.
	// Everything that issues a COMMAND or draws a gameplay overlay does not.
	const bool gameplay = inGame && !uiVisible;

	// For crash reports: "it crashes at the title screen" is a menu state, so
	// the breadcrumbs need to say which one the game was in.
	{
		static bool s_crumbUi = false;
		static bool s_crumbInGame = false;
		static bool s_crumbFirst = true;
		if ( s_crumbFirst || uiVisible != s_crumbUi || inGame != s_crumbInGame )
		{
			s_crumbFirst = false;
			s_crumbUi = uiVisible;
			s_crumbInGame = inGame;
			Breadcrumb( "game: %s, %s", inGame ? "map loaded" : "no map",
						uiVisible ? "menu UP" : "menu down" );
		}
	}

	// ---- THE SERVER'S INTERFACES, WHICH ONLY EXIST IN A MAP -------------
	//
	// se1/bin/server.dll is loaded when a map loads, so the startup dump ran
	// too early to see it -- and the server side is exactly where the movement
	// question lives. One shot, the first time we are in a map.
	// server.dll exists only inside a map, so this binds lazily rather than
	// latching a failure at startup.
	if ( inGame && Movement().Enabled() && !Movement().Bound() )
	{
		if ( Movement().Bind() )
			Movement().Hook( g_config.GetInt( "server_movement_slot", 1 ) );
	}
	// The shot probe rides on the same lazy binding: the server's trace comes
	// from engine.dll, which is always loaded, but there is no point hooking the
	// engine's hottest function before there is a map to shoot in.
	if ( inGame && Shots().Enabled() && !Shots().Bound() )
	{
		if ( Shots().Bind() )
			Shots().Hook();
	}

	if ( Movement().Bound() )
	{
		const CViewSetup* mvs = Stereo().ViewSetup();
		if ( mvs )
			Movement().SetReference( mvs->origin );
		if ( mvs )
			Shots().SetEye( mvs->origin );

		// The muzzle, from the viewmodel's collided pose. Invalid until the gun
		// has been placed at least once -- and an invalid muzzle leaves the ray
		// alone rather than firing everything from the world origin.
		Shots().SetMuzzle( g_viewModel.ShootOriginWorld(),
						   g_viewModel.HaveShootOrigin() );

		// ---- TRUE 6DoF: THE BODY CHASES THE HEAD --------------------------
		//
		// The target is the head offset the camera has ALREADY collided. Using
		// the collided value rather than the raw one matters: the raw offset can
		// point through a wall, and handing that to the mover would ask the body
		// to walk somewhere the head was refused.
		//
		// Gated on gameplay. A cutscene or a menu leaves the player frozen while
		// the physical head keeps moving, and chasing it then would walk the
		// body across the map while the player is not in control of it.
		const bool sixdofLive = gameplay && g_camera.PositionalTracking();
		// The head's facing and tilt shape the deadzone -- see SixDofSettings.
		// Roll only reads as a lean while the head is roughly level: looking
		// steeply up or down trades yaw and roll into each other.
		const QAngle& headAngles = g_camera.ViewAngles();
		const float leanRoll =
			( fabsf( headAngles.x ) < 60.0f ) ? g_camera.HeadRoll() : 0.0f;
		Movement().SetBodyTarget( g_camera.PositionalOffset(), sixdofLive,
								  headAngles.y, leanRoll );

		// The engine's own trace, and the local player to skip -- without the
		// skip every sweep stops at zero range on the player's own hull, which
		// would read as "blocked everywhere" and quietly disable the feature.
		const int spi = g_engine.GetLocalPlayer();
		Movement().SetTrace( &g_collisionTrace,
							 ( spi > 0 && g_entities.Valid() )
								 ? g_entities.GetClientEntity( spi ) : nullptr );

		// ---- AND THE HEAD GIVES BACK WHAT THE BODY TOOK -------------------
		//
		// Drained every frame regardless of gameplay state. Ticks the mover
		// completed are already in the world whatever the client is doing now,
		// and leaving them undrained would apply them later against a stale
		// body yaw -- shifting the reference sideways.
		const Vector bodyMoved = Movement().TakeAchieved();
		g_camera.ShiftPositionReference( bodyMoved );
	}

	if ( inGame && g_logInterfacesInGame )
	{
		g_logInterfacesInGame = false;
		Log( "--- interface dump, in-game ---" );
		LogModuleInterfaces( "server.dll" );
		Log( "--- end in-game interface dump ---" );
	}


	// Released the moment there is no menu, wherever that leaves us.
	//
	// It used to be released only in the `!gameplay` branch's else, which a
	// normal menu-to-gameplay transition never enters: closing a pause menu
	// flips `gameplay` to true and skips that whole block. So the marker
	// stayed armed and every eye pass kept re-submitting the cross at the
	// last place the player had pointed -- a cursor hanging in the level,
	// not moving, exactly as reported.
	if ( !uiVisible )
	{
		g_menuPointer.Release();
		// Disarmed in the SAME place the pointer is released, deliberately.
		// These two are the whole of "a menu is up" as far as drawing is
		// concerned, and the one bug this area has already produced was a
		// cursor left armed after a menu closed, re-submitting itself into
		// the level every eye pass. One release site, both cursors.
		g_menuCursor.Disarm();
		g_lookArrow.Disarm();
		Stereo().ClearMenuPanel();
		g_menuPanelGeom.valid = false;
		if ( g_menuDownSinceMs == 0 )
			g_menuDownSinceMs = GetTickCount();
	}

	if ( g_vrCameraEnabled && g_vr && g_vr->IsReady() && inGame )
	{
		// Keep the aim on the same controller-to-weapon transform as the model,
		// including any live numpad tuning done this frame.
		g_camera.SetWeaponAngleOffset( g_viewModel.Settings().anglePitch,
									   g_viewModel.Settings().angleYaw );
		// Which gun is in hand, so the per-weapon offsets and the aim
		// convergence both follow a weapon switch. Resolved once, by the
		// animation module, and shared from there.
		g_viewModel.SetCurrentModel( g_viewModelAnim.CurrentModelKey() );

		// The EFFECTIVE offsets, not the global ones -- otherwise convergence
		// would aim from where the pistol's muzzle would have been while the
		// player is holding the shotgun.
		{
			const ModelOffsets off = g_viewModel.Eff();
			g_camera.SetWeaponPositionOffset( off.forward, off.right, off.up );

			// Per-weapon bore, for the same reason: the three guns carry their
			// sights differently, so a switch has to change the zero with it.
			float boreRight = g_boreGlobalRight, boreUp = g_boreGlobalUp;
			float boreFwd = g_boreGlobalFwd;
			float boreYaw = 0.0f, borePitch = 0.0f;
			BoreFor( g_viewModel.CurrentModelKey(), boreRight, boreUp, boreFwd,
					 boreYaw, borePitch );
			g_camera.SetBoreOffset( boreRight, boreUp );
			g_camera.SetBoreAngles( boreYaw, borePitch );

			// ---- THE SAME THREE NUMBERS, POINTED SOMEWHERE ELSE ---------
			//
			// With shot_from_gun the lateral/up/forward values stop being a
			// parallax fudge on the convergence target and become the MUZZLE's
			// position in the gun's own basis -- which is what the tuner was
			// always trying to express. SetBoreOffset above is inert in this
			// mode; this is the live path.
			g_viewModel.SetShootOffset( boreRight, boreUp, boreFwd );
		}

		// Recoil compensation. The engine will add m_vecPunchAngle back when it
		// computes the shot, so handing it the negated punch cancels the kick.
		// Read from the CLIENT's player entity -- the value is networked, so it
		// mirrors the server's without needing server-side entity access.
		QAngle correction = { 0.0f, 0.0f, 0.0f };

		// READ UNCONDITIONALLY. This used to be gated on
		// `g_recoilCompensation > 0`, which meant the punch value was only
		// available when the feature that consumes it was switched on -- so the
		// run performed specifically to test recoil, with compensation set to
		// 0, logged punch=(0.00 0.00) and could not answer the question it was
		// run to answer. Same shape as lesson 2 in section 10, and it cost a
		// test run here too.
		//
		// The CORRECTION is still gated below; only the measurement moved.
		if ( g_punchAngleOffset >= 0 && g_entities.Valid() )
		{
			const int playerIndex = g_engine.GetLocalPlayer();
			void* player = ( playerIndex > 0 ) ? g_entities.GetClientEntity( playerIndex )
											   : nullptr;
			if ( player )
			{
				const QAngle punch = *reinterpret_cast<const QAngle*>(
					reinterpret_cast<const unsigned char*>( player ) + g_punchAngleOffset );

				// A punch beyond a few degrees is not recoil, it is a wrong
				// offset -- ignore it rather than yank the player's aim across
				// the room.
				if ( fabsf( punch.x ) < 30.0f && fabsf( punch.y ) < 30.0f )
				{
					// Always published for the trace; scaled by the setting
					// only where it is actually applied to the aim.
					g_camera.SetPunchRaw( punch );
					correction.x = punch.x * g_recoilCompensation;
					correction.y = punch.y * g_recoilCompensation;
				}
			}
		}
		g_camera.SetAimCorrection( correction );

		// Firing state for the recoil view lock, with a TAIL. The kick does not
		// land on the frame the button goes down: the server applies it and the
		// rotated angles arrive over the following frames, so releasing the
		// lock with the button would let the last of every burst through. Same
		// reasoning as melee's aim tail.
		{
			const DWORD nowMs = GetTickCount();
			if ( g_input.AttackHeld() )
				g_firingUntilMs = nowMs + (DWORD)g_recoilLockTailMs;
			g_camera.SetFiring( g_firingUntilMs != 0 &&
								(int)( nowMs - g_firingUntilMs ) < 0 );
		}

		// Physical crouch, BEFORE the camera: it decides the duck
		// compensation, and the camera folds that into the view origin it
		// builds a few lines below. Computed after the poses are fresh and
		// before anything reads the offset, so there is no frame of lag
		// between the head moving and the view following it.
		{
			// The engine's current eye height above the player's feet. Read
			// from the CLIENT's own entity by name, like m_vecPunchAngle --
			// a negative value means "unreadable" and disables the
			// compensation rather than substituting a guess.
			float viewOffsetZ = -1.0f;
			int waterLevel = 0;
			// One entity fetch for both -- they want the same player on the same
			// frame, and GetClientEntity is a virtual call plus a bounds check.
			if ( ( g_viewOffsetOffset >= 0 || g_waterLevelOffset >= 0 ) &&
				 g_entities.Valid() )
			{
				const int playerIndex = g_engine.GetLocalPlayer();
				void* player = ( playerIndex > 0 )
					  ? g_entities.GetClientEntity( playerIndex ) : nullptr;
				if ( player && g_waterLevelOffset >= 0 )
				{
					const int wl = *reinterpret_cast<const int*>(
						reinterpret_cast<const unsigned char*>( player ) +
						g_waterLevelOffset );
					// 0..3 or the offset is wrong. Treating garbage as 'swimming'
					// would take the player's aim pitch away on dry land.
					if ( wl >= 0 && wl <= 3 )
						waterLevel = wl;
				}
				if ( player && g_playerFlagsOffset >= 0 )
				{
					const int flags = *reinterpret_cast<const int*>(
						reinterpret_cast<const unsigned char*>( player ) +
						g_playerFlagsOffset );
					g_camera.SetPlayerFrozen( ( flags & kFlFrozen ) != 0 );
				}
				if ( player && g_viewOffsetOffset >= 0 )
				{
					// One float -- the Z component -- because that is how the
					// engine networks it. Reading a whole Vector here would take
					// the two floats that happen to follow it in memory.
					const float vz = *reinterpret_cast<const float*>(
						reinterpret_cast<const unsigned char*>( player ) +
						g_viewOffsetOffset );
					// A sane eye height. Anything else is a wrong offset, not a
					// crouching player, and feeding it in would move the camera.
					if ( vz > 0.0f && vz < 128.0f )
						viewOffsetZ = vz;
				}
			}

			// Crouching to read the bottom of a menu should not duck the player
			// or shift the view compensation. Held to a clean zero rather than
			// simply skipped, so re-entering gameplay does not inherit whatever
			// the compensation happened to be when the menu opened.
			if ( gameplay )
			{
				g_physicalCrouch.Update( *g_vr, g_camera.StandingHeight(),
						 g_camera.HaveStandingHeight(), viewOffsetZ );
				g_camera.SetDuckCompensation( g_physicalCrouch.ViewCompensation() );
				g_input.SetPhysicalCrouch( g_physicalCrouch.Crouching() );
			}
			else
			{
				g_camera.SetDuckCompensation( 0.0f );
				g_input.SetPhysicalCrouch( false );
			}

			// WL_Waist. The same threshold CGameMovement uses to switch to
			// WaterMove, so the override starts exactly when the pitch starts
			// steering and not a moment before.
			g_camera.SetSwimming( waterLevel >= 2 );

			// Two-handed grip, before the camera composes anything: it decides
			// where the gun points, and the camera's aim and the viewmodel both
			// read that answer from EffectiveWeaponAngles.
			if ( gameplay )
			{
				g_twoHanded.SetCurrentModel( g_viewModel.CurrentModelKey() );
				// Empty hands have no barrel to line up, and the blend aims the
				// ENGINE -- which is also the movement basis. See the gate in
				// TwoHanded::Update.
				g_twoHanded.SetWeaponInHand( g_viewModelAnim.WeaponInHand() );
				g_twoHanded.Update( *g_vr );
				g_camera.SetTwoHanded( g_twoHanded.Active(),
						 g_twoHanded.BlendedAngles(),
						 g_twoHanded.BlendWeight() );
			}
			else
			{
				// Cleared, not left latched -- a grip that happened to be
				// engaged when the menu opened would otherwise keep steering
				// the aim while the player is clicking buttons.
				g_camera.SetTwoHanded( false, QAngle{ 0.0f, 0.0f, 0.0f }, 0.0f );
			}
			g_lastWaterLevel = waterLevel;
		}

		// ---- WHAT THE HEAD IS ALLOWED TO WALK THROUGH ---------------------
		//
		// Set immediately before Apply, because Apply is what recomputes the
		// 6DoF offset and the sweep happens inside it. Cleared when any piece
		// is missing rather than left stale: a collision origin from an earlier
		// map would clamp against geometry that is no longer there.
		{
			const CViewSetup* cvs = Stereo().ViewSetup();
			const int cpi = g_engine.GetLocalPlayer();
			void* cself = ( cpi > 0 && g_entities.Valid() )
							  ? g_entities.GetClientEntity( cpi ) : nullptr;
			if ( cvs && g_collisionTrace.Valid() )
				g_camera.SetCollisionContext( &g_collisionTrace, cself, cvs->origin );
			else
				g_camera.ClearCollisionContext();

			// The gun collides against the same trace and skips the same
			// entity. Set from HERE rather than beside the viewmodel's own
			// Apply so the two can never disagree about which entity is the
			// player, and so both go stale together on a level change.
			if ( g_collisionTrace.Valid() )
				g_viewModel.SetCollisionContext( &g_collisionTrace, cself );
			else
				g_viewModel.ClearCollisionContext();
		}

		// A melee swing aims along the gaze, not along the hand. Pushed in
		// before Apply, because Apply is what writes the angles the engine
		// resolves the attack with.
		//
		// +use joins it for the same reason and through the SAME call. The
		// engine's aim pitch is the controller's pose plus viewmodel_angle_pitch,
		// so a use trace runs 9-24 degrees below the player's gaze -- right for
		// a muzzle, wrong for reaching a door handle. See use_aim_head.
		//
		// ONE writer, deliberately. Two calls would each track their own idea of
		// whether the flag is wanted and the last one to clear it would win --
		// exactly the +duck two-writer bug that got a player stuck crouching.
		g_camera.SetForceHeadAim( g_melee.AimForward() || g_input.UseAimHead() );

		// ---- WHICH MAP ARE WE ON -------------------------------------------
		//
		// Hoisted ABOVE the teleport detector on purpose, and the order is
		// load-bearing. The detector compares this frame's origin against the
		// last one it saw; on the first frame of a new level that stale origin
		// belongs to the PREVIOUS map, and the difference between two maps'
		// coordinate systems is arbitrarily large. Left below, every level
		// change would read as a teleport and turn the player -- which is
		// precisely the behaviour map_turn_to_face was switched off to stop.
		//
		// Only the map bookkeeping happens here. Arming and running the turn
		// stay below, because the turn needs the look target that the block
		// after this one sets.
		g_mapChangedThisFrame = false;
		if ( g_engine.Valid() )
		{
			const char* lvl = g_engine.GetLevelName();
			if ( lvl && *lvl &&
				 strncmp( lvl, g_lastMapName, sizeof( g_lastMapName ) - 1 ) != 0 )
			{
				strncpy_s( g_lastMapName, lvl, _TRUNCATE );
				g_mapChangedThisFrame = true;
				// Only arm after the FIRST map -- coming from the menu's
				// background map into the opening level is a legitimate spawn
				// and the game already points the player correctly.
				//
				// Gated: a LEVEL CHANGE turns the player only when
				// map_turn_to_face is on, which it is not by default.
				if ( g_mapTurnEnabled )
				{
					g_mapTurnPending = g_haveSeenAMap;
					g_mapTurnFrames = 0;
					g_mapTurnCause = "level change";
					if ( g_mapTurnPending )
						Log( "map: changed to %s -- will point you at the scene "
							 "once, %d frames in", lvl, g_mapTurnDelayFrames );
				}
				g_haveSeenAMap = true;
				// A new map's first origin has nothing to do with the last
				// map's. Drop the reference rather than measure across it.
				g_haveLastOrigin = false;
				// The level-change flash goes through the same hook as the
				// teleport one, so it needs the same suspension -- and unlike
				// the turn, this applies on the very first map too.
				SuspendHudAnchorForTransition();
			}
		}

		// The scripted camera's own angles live in CViewSetup and nowhere else
		// -- GetViewAngles holds the PLAYER's, frozen at the pose the script
		// ends in. Read BEFORE the eye passes, so this is the engine's value
		// rather than the per-eye one the renderer writes.
		//
		// Pushed in even when no cutscene is running, so the one-shot alignment
		// has a target on the frame a cutscene is detected.
		{
			const CViewSetup* lvs = Stereo().ViewSetup();
			if ( lvs && !uiVisible )
				g_camera.SetLookTarget( lvs->angles.x, lvs->angles.y );
			else
				g_camera.ClearLookTarget();

			// ---- A TELEPORT INSIDE ONE MAP IS ALSO A SCENE CHANGE ---------
			//
			// SiN's flash-cuts during the intro02 car ride are not cutscenes
			// and not level changes: the game MOVES the player to a room built
			// elsewhere in the same map, plays the scene, and moves them back.
			// Measured -- the car drives Y 10988 -> -1062 at ~600 units/s, then
			// the origin jumps ~5500 units to (4098 3802 -263), sits STILL for
			// three heartbeats, and jumps back into the moving car.
			//
			// FL_FROZEN is clear throughout and no camera is driven, so every
			// scripted-camera detector is blind to it. The ORIGIN is not.
			//
			// Safe as a trigger for the same reason a map change is: turning
			// the player does not move them, so the correction cannot re-arm
			// its own trigger. That is the test the divergence latch failed.
			//
			// The threshold has enormous headroom. Normal motion is ~7 units
			// per frame even in the car (600 u/s at 90 fps); the smallest jump
			// seen here is three orders of magnitude clear of it.
			if ( lvs && !uiVisible && g_mapTurnOnTeleport && g_haveSeenAMap &&
				 !g_mapChangedThisFrame )
			{
				const Vector& o = lvs->origin;
				if ( g_haveLastOrigin )
				{
					const float dx = o.x - g_lastOrigin.x;
					const float dy = o.y - g_lastOrigin.y;
					const float dz = o.z - g_lastOrigin.z;
					const float d2 = dx * dx + dy * dy + dz * dz;
					const float lim = g_mapTurnTeleportUnits * g_mapTurnTeleportUnits;
					if ( d2 > lim && !g_mapTurnPending )
					{
						g_mapTurnPending = true;
						g_mapTurnFrames = 0;
						g_mapTurnCause = "in-map teleport";
						++g_teleportCount;
						SuspendHudAnchorForTransition();
						Log( "map: TELEPORTED %.0f units inside %s -- a flash-cut "
							 "scene, not a level change. Will point you at it "
							 "once, %d frames in.",
							 sqrtf( d2 ), g_lastMapName, g_mapTurnDelayFrames );
					}
				}
				g_lastOrigin = o;
				g_haveLastOrigin = true;
			}
			else if ( uiVisible || !lvs )
			{
				// Do not measure across a menu or a missing view -- the
				// difference either side of the gap is not a teleport.
				g_haveLastOrigin = false;
			}
		}

		// ---- TURN TO FACE THE SCENE ON A MAP CHANGE ------------------------
		//
		// SiN's "white flash into a different scene with a character" is a
		// LEVEL CHANGE, not an in-map cutscene: SE1_intro01 -> se1_intro02,
		// with the player spawned facing away from what the scene is about.
		// The player is NOT frozen there and the camera does not rotate
		// enough, so neither scripted-camera detector sees it -- and the
		// divergence detector that WOULD have seen it is a feedback loop.
		//
		// A map change is the signal those lack: it is a fact about the world,
		// it is edge-triggered, and turning the player cannot cause another
		// one. That makes it a safe trigger for the one-shot in the exact way
		// divergence was not.
		//
		// **Not a hard-coded 180.** Measured on se1_intro02 the error runs
		// 99..142 degrees and MOVES, because the scene camera pans -- a fixed
		// half-turn would overshoot by up to 80. Aiming at the camera's actual
		// heading needs no per-scene number and works on every map.
		//
		// Self-cancelling where it is not wanted: on an ordinary spawn the
		// camera IS the player's view, so the turn is ~0 degrees and nothing
		// visibly happens. That is why this can be on by default.
		// ---- RUN THE TURN ---------------------------------------------------
		//
		// The map bookkeeping this depends on now happens further up, above the
		// teleport detector -- see the comment there for why the order matters.
		// What is left here is arming's consequence and the turn itself, and it
		// is NOT gated on g_mapTurnEnabled: that setting decides whether a
		// LEVEL CHANGE arms the one-shot, while an in-map TELEPORT arms the
		// same one-shot through map_turn_on_teleport.
		//
		// Those two used to share a gate by accident. `g_haveSeenAMap` was set
		// only inside `if ( g_mapTurnEnabled )`, and the teleport detector
		// requires it -- so turning the level-change turn off silently switched
		// the teleport turn off with it. A whole session logged `0 in-map
		// teleport(s) seen`: not one fire, and not one rejection either,
		// because the detector never ran and never even sampled an origin,
		// while `map_turn_on_teleport = 1` sat in the config doing nothing.
		// Same trap as `recoil_compensation = 0` disabling FL_FROZEN.
		if ( g_engine.Valid() )
		{
			const char* lvl = g_engine.GetLevelName();
			if ( !lvl || !*lvl )
			{
				// Between levels. Drop a pending request rather than carrying
				// it across the gap, where the look target is meaningless.
				g_mapTurnPending = false;
				g_mapTurnFrames = 0;
			}

			if ( g_mapTurnPending && g_camera.HaveLookTarget() && !uiVisible )
			{
				// Deferred, because on the first frames of a level CViewSetup
				// has not settled and may still hold the previous view -- the
				// same reason the cutscene alignment defers a frame, only
				// longer because a level load is a bigger discontinuity.
				if ( ++g_mapTurnFrames >= g_mapTurnDelayFrames )
				{
					float mr = 0.0f, mu = 0.0f, mdeg = 0.0f;
					const bool haveErr = g_camera.LookError( mr, mu, mdeg );
					g_mapTurnPending = false;

					// A tiny error is an ordinary spawn. Turning by 3 degrees
					// is not worth a body-yaw discontinuity, and reporting it
					// as a correction would be misleading.
					if ( haveErr && mdeg >= g_mapTurnMinDegrees )
					{
						g_camera.RequestTurnToFace();
						++g_mapTurnCount;
						Log( "map: %.0f deg away from what the camera is "
							 "looking at -- turning you to face it (once). "
							 "Trigger was a %s, not a cutscene; FL_FROZEN is %s.",
							 mdeg, g_mapTurnCause,
							 g_camera.PlayerFrozen() ? "set" : "clear" );
					}
					else
					{
						Log( "map: already facing the camera after the %s "
							 "(%.0f deg, under the %.0f deg minimum) -- no turn "
							 "needed", g_mapTurnCause, mdeg, g_mapTurnMinDegrees );
					}
				}
			}
		}

		g_camera.Apply( g_engine, *g_vr );

		// AFTER Apply -- the drive state and the head's own angles are both
		// decided inside it.
		{
			float ar = 0.0f, au = 0.0f, adeg = 0.0f;
			const bool have = g_camera.LookError( ar, au, adeg );
			g_lookArrow.Update( have && g_camera.EngineDriving() && !uiVisible,
								ar, au, adeg );

			// ---- HOW FAR THE ENGINE'S CAMERA IS FROM THE PLAYER'S HEAD -----
			//
			// In normal play this is ~0 BY CONSTRUCTION: CViewSetup.angles is
			// the view we wrote from the head, so the camera and the head are
			// the same thing. It only grows when the engine is pointing its
			// camera somewhere of its own accord -- which is a scripted camera,
			// whether or not the player is FL_FROZEN and whether or not that
			// camera ROTATES.
			//
			// That makes it the signal both existing detectors lack. FL_FROZEN
			// misses a scene that leaves the player in control (a vehicle
			// ride); the duration heuristic misses a scene whose camera is
			// static. Divergence misses neither.
			//
			// Measuring before building the detector, because the THRESHOLD is
			// the whole question and there is no honest way to guess it. The
			// counters below are deliberately split by whether a stretch was
			// already detected: frames that are diverged AND undetected are the
			// ones the player experiences as "I could not see the scene".
			if ( have && !uiVisible )
			{
				if ( adeg > g_lookDivergenceMax )
					g_lookDivergenceMax = adeg;
				if ( g_camera.EngineDriving() )
					++g_lookDivergedDetected;
				else
				{
					++g_lookFramesUndetected;
					if ( adeg > 60.0f )
					{
						++g_lookDivergedUndetected;
						if ( adeg > g_lookDivergenceMaxUndetected )
							g_lookDivergenceMaxUndetected = adeg;
					}
				}
			}

		}

		// Controllers AFTER the camera, so a snap turn applied this frame is in
		// the body yaw the next frame composes from, and BEFORE the eye passes,
		// so the view the player sees already reflects the turn.
		// dt scales the smooth turn rate, so the clock's granularity is the
		// smoothness -- see NowSeconds. GetTickCount is still read beside it,
		// purely so the log can say whether it really was the problem.
		const double nowSec = NowSeconds();
		const DWORD nowTick = GetTickCount();
		float dt = ( g_lastInputTime != 0.0 )
					  ? (float)( nowSec - g_lastInputTime ) : 0.0f;
		float legacyDt = ( g_lastInputTick != 0 )
					  ? ( nowTick - g_lastInputTick ) / 1000.0f : 0.0f;
		g_lastInputTime = nowSec;
		g_lastInputTick = nowTick;
		// paused, alt-tabbed or first frame: no smooth turn burst. Discarded
		// from the measurement too -- both clocks, or the comparison is not
		// over the same frames.
		if ( dt < 0.0f || dt > 0.25f )
			dt = legacyDt = 0.0f;
		g_inputClock.Note( dt, legacyDt );
		// How far to rotate the movement stick, in room space.
		//
		// The engine walks along cmd->viewangles, which is whatever we wrote to
		// SetViewAngles -- so the frame movement is resolved in is the AIM
		// frame, not always the head. The offset is therefore the difference
		// between where the player wants to walk and what the engine is aiming:
		//
		//     offset = moveFrameYaw - aimFrameYaw
		//
		// which collapses to zero whenever the two sources agree, and needs no
		// world conversion because both devices live in the same room space.
		// Falls back to the head on either side if a pose is missing, rather
		// than freezing on a stale direction.
		//
		// ---- THE AIM FRAME COMES FROM THE CAMERA, NOT FROM THE POSE --------
		//
		// This used to read `weapon.angles.y` -- the RAW controller yaw -- and
		// that is not the yaw the engine walks along. Between the pose and
		// SetViewAngles the aim goes through the Euler composition of the
		// weapon tilt, the two-handed blend, aim convergence and the recoil
		// correction, and none of those are in the raw pose.
		//
		// Measured over a session: a steady 56 deg of error one-handed (the
		// 58 deg viewmodel tilt, composed about the controller's own axes), and
		// 80..125 deg once the two-handed grip latched. Movement had simply
		// stopped following the head, which is exactly how it was reported.
		//
		// VRCamera::AimYawOffset() is the difference between the head's view
		// yaw and the yaw it actually handed the engine, so the picture and the
		// hit-test are one piece of maths rather than two. `aimFrameYaw` is
		// reconstructed from it in room space so the controller-movement case
		// below still composes the way it always did.
		float moveYawOffset = 0.0f;
		{
			const HmdPose& head = g_vr->Hmd();
			if ( head.valid )
			{
				const ControllerPose& offHand = g_vr->OffHand();

				const bool moveByController =
					( g_input.Settings().movementDirection == kMoveDirController ) &&
					offHand.valid;

				const float moveFrameYaw = moveByController ? offHand.angles.y : head.angles.y;
				const float aimFrameYaw =
					NormalizeAngle( head.angles.y - g_camera.AimYawOffset() );
				moveYawOffset = NormalizeAngle( moveFrameYaw - aimFrameYaw );
			}
		}
		// None of this belongs on a menu. Reaching for a holster, swinging at
		// the air or dropping a hand to the waist should not draw a weapon,
		// melee or reload while the player is choosing "New Game" -- and
		// `+forward` on the movement stick should not walk the background map
		// around behind the panel.
		if ( gameplay )
		{
			// Holsters before input: GameInput reads the grip and needs to know
			// whether the hand is in a zone at that moment.
			g_holsters.Update( *g_vr );

			g_input.Apply( g_engine, g_vr->Input(), g_camera, dt, moveYawOffset );

			// Melee by gesture. After the camera so the poses are this frame's,
			// and independent of GameInput because it is not a button -- it reads
			// the weapon hand's motion rather than any action state.
			g_melee.Update( g_engine, *g_vr );

			// Arcade reload. AFTER melee, so Busy() describes this frame rather
			// than the last one -- the two share the bottom of a swing's path
			// and the interlock is only meaningful in that order.
			// Active(), not Busy(): Busy() includes melee's 0.8s cooldown, which
			// is there to stop a second SWING and has nothing to do with
			// reloading. See MeleeGesture::Active.
			g_arcadeReload.Update( g_engine, *g_vr, g_melee.Active( GetTickCount() ) );
		}
		else
		{
			// RELEASE rather than skip. A held command is engine key state, so
			// simply not calling Apply would leave whatever was down at the
			// moment the menu opened held forever -- which is how a player gets
			// stuck walking into a wall behind the pause menu. Same reasoning as
			// the no-camera branch at the bottom of this function.
			g_input.ReleaseAll( g_engine );
			g_melee.Release( g_engine );
			g_arcadeReload.Release( g_engine );

			// ...except the MENU button. Releasing everything else and then not
			// reading this one meant the button could open a menu and never
			// close it, because Apply() stops being called the moment one is up.
			g_input.ApplyMenuOnly( g_engine, g_vr->Input() );

			// The menu pointer replaces them. It is the only thing driving
			// input while a menu is up, so it cannot fight the gameplay
			// bindings -- they were all released on the line above.
			if ( uiVisible )
			{
				// ---- ONE geometry, built once, read by both -----------------
				//
				// The renderer draws the menu on this quad and the pointer
				// traces against this quad. That is the single most important
				// property here: four earlier rounds failed because the picture
				// and the hit-test were computed by different routes and could
				// not be made to agree.
				//
				// The direction is captured ONCE at startup and re-captured on
				// RECENTRE -- not per menu. Re-capturing per menu is what made
				// it appear to follow the head: moving between menu screens
				// blinks the cursor off, so every submenu re-pinned it wherever
				// the player happened to be looking.
				// ---- align to the MENU SCENE, not to the player -------------
				//
				// The background map has its own scripted camera, staged so the
				// NPC and the logo face it. That direction is knowable: the
				// engine re-asserts its own view yaw every frame, and the camera
				// reads it before overwriting. The heartbeat measured the gap at
				// a constant -25 degrees, which is precisely the "off to the
				// right and at a slight angle" that pinning to the player's own
				// heading produced.
				//
				// Still captured ONCE and held -- re-taking it per menu is what
				// made it appear to follow the head, since menu transitions
				// blink the cursor off. Recentre re-takes it.
				// ---- when to re-pin, and why it is not simply "on open" ----
				//
				// Pinning ONCE at startup put the pause menu wherever the player
				// had been facing when the game launched. The log said so
				// outright: `pinned yaw 180.0` against `engine yaw 49.8`, with
				// the pointer frozen and reporting `pointing OFF the panel` --
				// the panel was 130 degrees off to one side and could not be
				// pointed at, let alone seen.
				//
				// Re-pinning on every open is what the earlier version did, and
				// it read as the panel following the head: moving between menu
				// SCREENS blinks the cursor off for a frame or two, so each
				// submenu counted as a fresh open and re-pinned.
				//
				// So it is debounced. A blink is milliseconds; putting a menu
				// away and opening another one later is not. The gap tells the
				// two apart with nothing to configure per screen.
				const DWORD nowMs = GetTickCount();
				const bool realOpen =
					g_menuDownSinceMs != 0 &&
					( nowMs - g_menuDownSinceMs ) > g_menuRepinMs;
				g_menuDownSinceMs = 0;

				const unsigned int rc = g_camera.RecentreCount();
				if ( !g_menuAnchorHeld || rc != g_menuAnchorRecentre || realOpen )
				{
					// The FIRST pin can follow the menu scene, which is staged
					// for a camera the player is not controlling. A re-pin
					// cannot: the player is standing somewhere, looking
					// somewhere, and the menu wants to be in front of THEM.
					const bool first = !g_menuAnchorHeld;
					g_menuAnchorYaw = ( first && g_menuAlignToScene )
						? g_camera.LastEngineYaw()
						: g_camera.ViewAngles().y;
					g_menuAnchorRecentre = rc;
					g_menuAnchorHeld = true;
					if ( !first )
						++g_menuRepins;
				}

				const CViewSetup* mvs = Stereo().ViewSetup();
				if ( mvs )
				{
					const Vector& moff = g_camera.PositionalOffset();
					const Vector headWorld = { mvs->origin.x + moff.x,
											   mvs->origin.y + moff.y,
											   mvs->origin.z + moff.z };

					// The HUD's own shape, so the quad is not stretched. Taken
					// from the ortho window the menu is actually drawn with
					// rather than assumed from the window size -- those differ
					// here, 1800x2124 against a 1200x1416 client.
					const float hudAspect = Stereo().MenuHudAspect();

					g_menuPanelGeom = BuildMenuPanel( g_menuPanelSettings,
													  g_menuAnchorYaw,
													  g_camera.ViewAngles().y,
													  headWorld, hudAspect );

					// The head basis the renderer needs to express the quad in
					// each eye's view space.
					Vector hf, hr, hu;
					AnglesToBasis( g_camera.ViewAngles(), hf, hr, hu );
					Stereo().SetMenuPanel( g_menuPanelGeom, hf, hr, hu );

					// Room space and world space differ by a pure yaw, and the
					// camera owns it. Handed over rather than re-derived.
					g_menuPointer.SetRoomToWorldYaw(
						NormalizeAngle( g_camera.ViewAngles().y - g_vr->Hmd().angles.y ) );
					g_menuPointer.UpdateOnPanel( *g_vr, g_menuPanelGeom );

					// The D3D9 cursor reads the SAME world position the
					// overlay marker does, so the two can never point at
					// different places.
					//
					// CursorWorld, NOT MarkerWorld. MarkerWorld folds in
					// whether the OVERLAY should draw, and the overlay is
					// deliberately off whenever this cursor is on -- so
					// arming off it armed nothing, ever. The question here is
					// only "does the pointer have a position".
					{
						Vector cw;
						if ( g_menuPointer.CursorWorld( cw ) )
							g_menuCursor.Arm( cw );
						else
							g_menuCursor.Disarm();
					}
				}
			}
			// No `else` here any more: the release is done unconditionally at the
			// top of this function, because THIS branch is unreachable on the
			// transition that actually matters. Closing a pause menu makes
			// `gameplay` true, which skips the whole enclosing block.
			//
			// The pinned yaw is deliberately NOT reset by either path. Menu
			// transitions blink the cursor off for a frame, so resetting it
			// per menu re-pinned the panel wherever the head happened to be --
			// which is what "it still rotates with the HMD" was. Recentre
			// re-takes it.
		}

		// The engine's angles may now be the weapon's; the eyes keep the head's.
		Stereo().SetViewAngleOverride( g_camera.ViewAngles() );

		// Viewmodel animation. Runs BEFORE the transform write and outside the
		// followController gate: an animation the player did not perform is
		// wrong whether or not the gun is pinned to the hand, and the entity
		// lookup is the same one either way.
		if ( g_viewModelAnim.Bound() && g_viewModel.Ready() )
		{
			void* vm = g_viewModel.ResolveViewModel( g_engine );
			if ( vm )
				g_viewModelAnim.Apply( vm );
		}

		// Layout diagnosis, spaced out so it never costs a frame twice in a row.
		// Read-only: it reports which fields of the entity currently hold the
		// abs origin, which is how we find the one the RENDER uses.
		if ( g_viewModel.DiagnoseRunsLeft() > 0 )
		{
			const DWORD nowMs = GetTickCount();
			if ( (DWORD)( nowMs - g_lastDiagnoseMs ) > 3000u )
			{
				g_lastDiagnoseMs = nowMs;
				g_viewModel.Diagnose( g_engine );
			}
		}

		if ( g_viewModelAnim.JustStartedFiring() )
			DumpFiringEntities();

		// Pin the weapon to the controller. Runs here because SetUpView has
		// already positioned the viewmodel from the player's eye by now -- this
		// is the window between that and the scene being drawn, and whether the
		// write survives it is exactly what the heartbeat reports.
		// `gameplay`, not `inGame`: the menu's background map has a viewmodel and
		// the setter hook was cheerfully driving it to the player's hand behind
		// the panel -- 13119 substitutions on a menu nobody was playing.
		if ( gameplay && g_viewModel.Ready() && Stereo().OwnsViewSetup() )
		{
			const CViewSetup* vs = Stereo().ViewSetup();
			if ( vs )
			{
				// The SAME duck compensation the eyes get, or the gun is lowered
				// twice while the head is not.
				//
				// vs->origin is the engine's base, which the engine has ALREADY
				// ducked. The eyes escape that because their offset carries the
				// compensation; the viewmodel adds a hand offset measured from the
				// STANDING recentre reference, so it inherits the duck untouched.
				// Symptom on hardware: crouch, and the view drops correctly while
				// the gun sinks past your knees.
				Vector vmBase = vs->origin;
				vmBase.z += g_camera.DuckCompensation();
				g_viewModel.Apply( g_engine, *g_vr, g_camera, vmBase );

				// ---- ONE FRAME OLD, ON PURPOSE ------------------------
				//
				// The camera runs before the viewmodel, so this is last
				// frame's measurement. That is fine and not worth the
				// restructuring to avoid: what crosses is a DISTANCE, and a
				// distance to what the barrel points at changes slowly
				// compared with 11 ms. The aim DIRECTION is recomputed from
				// the live controller pose every frame regardless.
				g_camera.SetTracedAimDistance( g_viewModel.AimDistance() );

				// The hand markers share the viewmodel's base deliberately: a
				// marker on the raw eye origin would sink through the floor when
				// the player crouched, exactly as the gun used to.
				if ( g_vr )
					g_handMarker.Prepare( vmBase, *g_vr, g_camera,
							  g_twoHanded.Engaged(),
							  g_viewModelAnim.Unarmed(),
							  g_viewModelAnim.WeaponHandleRaw() );

					// The foregrip box is a tuning aid, so it follows the tuning flag
					// rather than being on whenever the markers are. Once the grip
					// points are dialled in it is only clutter.
					Vector gripRoom;
					if ( g_twoHanded.Settings().debug &&
						 g_twoHanded.GripPoint( gripRoom ) )
						g_handMarker.PrepareGrip( vmBase, g_camera, gripRoom,
								  g_twoHanded.Engaged() );
			}

		}

		// ---- the anchored HUD ------------------------------------------------
		//
		// Anchored to the BODY, not the head: it sits at a fixed place relative
		// to where the player faces, so turning the head moves it across the
		// view and looking down-and-across brings it into sight. Head-anchored
		// would just be the flat HUD again, and hand-anchored was tried -- see
		// HANDOVER.md, it needs a real world-space quad rather than an ortho
		// shift before it is worth having.
		//
		// Only the world vector and the head basis are handed over. The eye-
		// specific part is done per eye inside the Ortho hook, which is what
		// makes the panel converge at its real distance.
		if ( Stereo().HudAnchored() )
		{
			// Body forward from the YAW ONLY -- the anchor must not pitch or roll
			// with the head, or looking down would drag the panel down with you
			// and you could never look at it.
			// YAW ONLY, and which yaw is a real choice:
			//
			//   head yaw  the panel stays in front of you wherever you turn, so it
			//             is always findable by looking down. This is the default.
			//   body yaw  the panel is pinned to the world and you must turn your
			//             BODY back to it -- with stick turning that means it can
			//             sit behind you and never be found.
			//
			// PITCH is excluded either way, and that is the part that matters: if
			// the anchor pitched with the head it would sink as you looked down and
			// stay just as far out of view, so you could never bring it into sight.
			// Excluding pitch is what makes 'below your view until you look down'
			// work at all.
			const float yawDeg = g_hudAnchorFollowYaw ? g_camera.ViewAngles().y
				  : g_camera.RoomYawToWorld( 0.0f );
			const float bodyYawRad = yawDeg * 0.01745329252f;
			const float cy = cosf( bodyYawRad );
			const float sy = sinf( bodyYawRad );
			const Vector bodyF = { cy, sy, 0.0f };
			const Vector bodyR = { sy, -cy, 0.0f };
			const Vector worldUp = { 0.0f, 0.0f, 1.0f };

			const Vector headToAnchor = {
				bodyF.x * g_hudAnchorForward + bodyR.x * g_hudAnchorRight + worldUp.x * g_hudAnchorUp,
				bodyF.y * g_hudAnchorForward + bodyR.y * g_hudAnchorRight + worldUp.y * g_hudAnchorUp,
				bodyF.z * g_hudAnchorForward + bodyR.z * g_hudAnchorRight + worldUp.z * g_hudAnchorUp };

			// The head basis is what the projection resolves against.
			Vector hf, hr, hu;
			AnglesToBasis( g_camera.ViewAngles(), hf, hr, hu );
			Stereo().SetHudAnchor( headToAnchor, hf, hr, hu );
		}

		// Hand the frame's head displacement to the renderer before it snapshots
		// CViewSetup, so both eyes are built from the same position.
		Stereo().SetPositionalOffset( g_camera.PositionalOffset() );

		// Zone boxes, before the eye passes so they are part of the world the
		// passes render. The head's world origin is the base view origin plus
		// the 6DoF offset -- the same sum ApplyEyeToViewSetup makes -- and
		// ViewSetup() still holds the base here, since the passes have not run.
		//
		// The yaw is the CAMERA's, not the HMD pose's: the boxes are placed in
		// the world and the room->world mapping is a pure yaw rotation, so the
		// head's world yaw is what turns a body-frame offset into a world one.
		// Gameplay overlays only. On the menu the laser was tracing the
		// background map from the engine's eye and drawing a crosshair on the
		// scenery BEHIND the panel -- 13120 traces, all "ON GEOMETRY", none of
		// them pointing at anything the player could click. A menu pointer is a
		// different thing and is built separately.
		// `g_zones.Visible()` lets the zone boxes draw during MENUS too. That is
		// deliberate and it earned its place: they are an independent consumer of
		// the same overlay interface, so pointing them at a state and seeing
		// whether they appear answers "does the engine draw overlays here at
		// all" without touching the feature under suspicion.
		//
		// It settled the menu cursor: boxes show on the main menu and in
		// gameplay, and vanish in a pause menu -- same as the cursor. Behind
		// zone_boxes_visible, which is off by default.
		if ( ( gameplay || g_zones.Visible() ) && ( g_zones.Visible() || g_laserDot.Ready() ) )
		{
			const CViewSetup* vs = Stereo().ViewSetup();
			if ( vs )
			{
				const Vector& off = g_camera.PositionalOffset();
				const Vector headWorld = { vs->origin.x + off.x,
						   vs->origin.y + off.y,
						   vs->origin.z + off.z };
				// Position only -- the boxes are submitted per eye pass, because the
				// overlay list is cleared at the top of each one.
				g_zones.Prepare( headWorld, g_camera.ViewAngles().y );

				// ---- THE AIM LINE ------------------------------------
				//
				// ONE line now, because there is one ray. The blue model axis
				// and the red shot line existed to show the relationship
				// between three different rays that had to be reconciled; with
				// the shot leaving the muzzle down the barrel there is nothing
				// left to reconcile, and drawing two more lines on top of the
				// only one that matters was just clutter.
				//
				// Drawn from the SHOOT ORIGIN -- the exact point handed to the
				// trace rewrite -- along the bore-corrected forward. So this is
				// not an illustration of the shot, it IS the shot: if the line
				// leaves the wrong part of the gun, that is the thing the
				// lateral/up/forward knobs are there to fix.
				if ( g_aimDebug.Enabled() && g_viewModel.HaveShootOrigin() )
				{
					const Vector& gunPos = g_viewModel.ShootOriginWorld();

					Vector gunFwd, shotRight, shotUp;
					AnglesToBasis( g_camera.LastWritten(), gunFwd, shotRight, shotUp );

					const float d = g_viewModel.AimDistance();
					const float hitDist = ( d > 0.0f ) ? d : 600.0f;
					const Vector gunHit = { gunPos.x + gunFwd.x * hitDist,
											gunPos.y + gunFwd.y * hitDist,
											gunPos.z + gunFwd.z * hitDist };

					g_aimDebug.Prepare( gunPos, gunFwd, gunHit,
										g_camera.LastConvergenceUsed(),
										g_camera.LastConvergenceMeasured() );

					float br = 0.0f, bu = 0.0f, bf = 0.0f, by = 0.0f, bp = 0.0f;
					BoreFor( g_viewModel.CurrentModelKey(), br, bu, bf, by, bp );
					g_aimDebug.DrawText( g_viewModel.CurrentModelKey(),
										 BoreAxisName( g_boreAxis ),
										 by, bp, br, bu, bf );
								}

				// The dot goes on the ENGINE's view origin, NOT headWorld. The
				// engine fires from the player's eye; the 6DoF offset moves what
				// the player SEES from, and adding it here would draw the dot on
				// a ray the bullet does not travel. The angles are the ones we
				// actually wrote to the engine, which is what it will shoot
				// along.
				if ( g_laserDot.Ready() )
				{
					// The player entity is handed in so the trace can skip it. It
					// is the entity the ray starts INSIDE, and without it every
					// trace stops at zero range on the player's own hull.
					const int pi = g_engine.GetLocalPlayer();
					void* self = ( pi > 0 && g_entities.Valid() )
						   ? g_entities.GetClientEntity( pi ) : nullptr;

					// ---- THE DOT MUST SHARE THE SHOT'S RAY ----------------
					//
					// With shot_from_gun the bullet leaves the MUZZLE, so a dot
					// traced from the eye is on a different line and lands
					// somewhere the shot does not -- worst exactly where it
					// matters, close up and round cover, because that is where
					// the two rays diverge most.
					//
					// Same origin, same angles, one ray. The dot stops being an
					// approximation of the shot and becomes a readout of it.
					//
					// Falls back to the eye when the gun is not being fired
					// from: with the convergence hack the eye IS the origin, and
					// tracing from the muzzle there would put the dot on a line
					// the bullet does not travel.
					const bool fromGun = g_camera.ShotFromGun()
										 && g_viewModel.HaveShootOrigin();
					const Vector& dotOrigin = fromGun
						? g_viewModel.ShootOriginWorld() : vs->origin;

					// The reticle follows the weapon, the way the game's own
					// HUD crosshair does.
					g_laserDot.SetWeapon( g_viewModel.CurrentModelKey() );

					g_laserDot.Draw( dotOrigin, g_camera.LastWritten(),
						     g_camera.AimConvergence(), self );
				}
			}
		}
	}
	else
	{
		// No camera this frame -- do not leave the last offset applied, or the
		// view stays leaned while the game owns it again, and do not leave a
		// movement or fire command asserted.
		Stereo().SetPositionalOffset( Vector{ 0.0f, 0.0f, 0.0f } );
		Stereo().ClearViewAngleOverride();
		g_input.SetPhysicalCrouch( false );
		g_input.ReleaseAll( g_engine );
		g_melee.Release( g_engine );
		g_arcadeReload.Release( g_engine );
		g_lastInputTick = 0;
		g_lastInputTime = 0.0;
	}

	// The view bind may have been attempted before the first map, when the
	// view legitimately has no size. Cheap no-op once it has succeeded.
	Stereo().RetryViewBind();

	auto original = reinterpret_cast<ViewRenderFn>( g_viewRenderHook.Original() );
	if ( !original )
		return;

	// Guard against re-entry. Source can call back into its own render path
	// (water reflections being the usual culprit); an inner call must run once,
	// normally, not become a second pair of eye passes.
	static int s_depth = 0;
	if ( s_depth > 0 || !Stereo().Ready() )
	{
		++s_depth;
		original( thisptr, edx, rect );
		--s_depth;
		return;
	}

	struct EyeCtx
	{
		void* thisptr;
		void* edx;
		vrect_t* rect;
	};
	EyeCtx ctx = { thisptr, edx, rect };

	++s_depth;
	Stereo().RenderBothEyes(
		[]( void* c )
		{
			EyeCtx* e = static_cast<EyeCtx*>( c );
			auto fn = reinterpret_cast<ViewRenderFn>( g_viewRenderHook.Original() );
			TraceViewModel( "pre" );
			fn( e->thisptr, e->edx, e->rect );
			TraceViewModel( "post" );
		},
		&ctx );
	--s_depth;

	if ( g_vmTraceFrames > 0 )
		--g_vmTraceFrames;
}

//-----------------------------------------------------------------------------
// Verification pass. Expected values were read out of a live process; anything
// else means the vtable mapping has drifted and nothing downstream is safe.
//   GetEngineBuildNumber    -> 7
//   GetProductVersionString -> "1.0.0.0"
//   GetGameDirectory        -> "...\sin episodes emergence\SE1"
//-----------------------------------------------------------------------------
void RunSanityCheck()
{
	ModuleRange engineRange = GetModuleRange( "engine.dll" );
	ModuleRange clientRange = GetModuleRange( "client.dll" );

	Log( "engine.dll  %p - %p", (void*)engineRange.base, (void*)engineRange.end );
	Log( "client.dll  %p - %p", (void*)clientRange.base, (void*)clientRange.end );

	Log( "IVEngineClient  inst=%p  rtti=%s", g_engine.Raw(), GetRttiName( g_engine.Raw() ) );
	Log( "IBaseClientDLL  inst=%p  rtti=%s", g_client, GetRttiName( g_client ) );

	int engineSlots = ProbeVTableLength( g_engine.Raw(), engineRange );
	int clientSlots = ProbeVTableLength( g_client, clientRange );

	Log( "IVEngineClient vtable slots: %d (expect %d) %s",
		 engineSlots, engine_slot::kSlotCount,
		 engineSlots == engine_slot::kSlotCount ? "== MATCH" : "== MISMATCH, review slot indices" );
	Log( "IBaseClientDLL vtable slots: %d (expect %d) %s",
		 clientSlots, client_slot::kSlotCount,
		 clientSlots == client_slot::kSlotCount ? "== MATCH" : "== MISMATCH, review slot indices" );

	// IMaterialSystem is what stereo will hook (per-eye view and projection
	// matrices, plus the render target swap), so its slot count needs
	// establishing before any of that is written.
	void* matsys = GetInterface( "materialsystem.dll", "VMaterialSystem076" );
	if ( matsys )
	{
		ModuleRange matsysRange = GetModuleRange( "materialsystem.dll" );
		int matsysSlots = ProbeVTableLength( matsys, matsysRange );
		Log( "IMaterialSystem inst=%p  rtti=%s", matsys, GetRttiName( matsys ) );
		Log( "IMaterialSystem vtable slots: %d (SDK 2004 declares %d) %s",
			 matsysSlots, matsys_slot::kSlotCount,
			 matsysSlots == matsys_slot::kSlotCount
				 ? "== MATCH, slot indices usable as-is"
				 : "== MISMATCH, stereo slot indices need re-deriving" );

		// ---- one-shot vtable dump, for finding a slot we do not have yet ----
		//
		// `matsys_vtable_dump = 1`. Off by default; this is archaeology, not
		// telemetry, and it writes ~140 lines once at startup.
		//
		// WHY THIS EXISTS. The slot table in source_interfaces.h says, in as
		// many words, that its numbers must never be interpolated between known
		// anchors -- the shift from the SDK is +4 in the render-target block and
		// +3 in the matrix block, because Ritual inserted methods in more than
		// one place. So a new slot cannot be derived on paper; it has to be read
		// out of the running game and identified by its disassembly, which is
		// how every index already in that table was proved.
		//
		// Printed per slot: the index, the absolute address, the offset from
		// materialsystem.dll's base (so it can be matched against a disassembler
		// loaded at any base), and the first twelve bytes -- enough to recognise
		// a `mov eax,[ecx+X]; ret` accessor or read the `ret N` of a thunk, and
		// so tell a two-argument method from a three-argument one.
		//
		// Bounds are checked against the module before reading, and the read is
		// inside __try for the same reason ProbeVTableLength's is: a vtable that
		// is not what we think it is must not take the game down.
		if ( g_config.GetBool( "matsys_vtable_dump", false ) && matsysSlots > 0 )
		{
			void** vt = VTableOf( matsys );
			Log( "matsys vtable dump: vtable=%p  module base=%p  slots=%d",
				 vt, (void*)matsysRange.base, matsysSlots );
			for ( int i = 0; i < matsysSlots; ++i )
			{
				auto* fn = reinterpret_cast<const unsigned char*>( vt[i] );
				if ( !matsysRange.Contains( fn ) )
				{
					Log( "  vt[%3d] %p  (outside materialsystem.dll)", i, fn );
					continue;
				}
				char bytes[80];
				bytes[0] = '\0';
				__try
				{
					for ( int b = 0; b < 12; ++b )
					{
						char one[8];
						_snprintf_s( one, sizeof( one ), _TRUNCATE, "%02X ", fn[b] );
						strcat_s( bytes, sizeof( bytes ), one );
					}
				}
				__except ( EXCEPTION_EXECUTE_HANDLER )
				{
					strcpy_s( bytes, sizeof( bytes ), "<unreadable>" );
				}
				Log( "  vt[%3d] %p  +0x%06X  %s", i, fn,
					 (unsigned)( reinterpret_cast<uintptr_t>( fn ) - matsysRange.base ),
					 bytes );
			}
			Log( "matsys vtable dump: end. Set matsys_vtable_dump = 0 to stop." );
		}
	}
	else
	{
		LogWarn( "IMaterialSystem (VMaterialSystem076) not available" );
	}

	__try
	{
		Log( "GetEngineBuildNumber()    = %u   (expect 7)", g_engine.GetEngineBuildNumber() );
		Log( "GetProductVersionString() = \"%s\"   (expect 1.0.0.0)", g_engine.GetProductVersionString() );
		Log( "GetGameDirectory()        = \"%s\"", g_engine.GetGameDirectory() );
		Log( "GetDXSupportLevel()       = %d", g_engine.GetDXSupportLevel() );
		Log( "SupportsHDR()             = %s", g_engine.SupportsHDR() ? "true" : "false" );
		Log( "GetMaxClients()           = %d", g_engine.GetMaxClients() );

		int w = 0, h = 0;
		g_engine.GetScreenSize( w, h );
		Log( "GetScreenSize()           = %dx%d", w, h );
		Log( "GetScreenAspectRatio()    = %.4f", g_engine.GetScreenAspectRatio() );
	}
	__except ( EXCEPTION_EXECUTE_HANDLER )
	{
		Log( "!! EXCEPTION during interface calls -- slot indices are wrong" );
	}
}

//-----------------------------------------------------------------------------
// One-shot environment dump. The build machine is not the VR machine, so a log
// sent back has to answer "what was this actually running on?" without a
// follow-up round trip.
// The release this build is. Logged first thing, so a log from someone else's
// machine says which build it came from -- the 2026-09-11 crash could not be
// matched to one, because nothing in it said.
constexpr const char* kSinVRVersion = "1.0.1";

void LogEnvironment()
{
	Log( "--- environment ---" );
	Log( "sinvr version  : %s", kSinVRVersion );

	// Which Steam branch -- the same test the launcher makes. Here too because
	// sinvr.log is what gets sent with a bug report, and "the arms are showing"
	// is exactly the report this explains: on the default branch the game reads
	// its content from vpks\ and ignores the mod's loose hands.vmt.
	{
		wchar_t pattern[MAX_PATH] = { 0 };
		GetModuleFileNameW( NULL, pattern, MAX_PATH );
		wchar_t* slash = wcsrchr( pattern, L'\\' );
		if ( slash )
		{
			wcscpy_s( slash + 1, MAX_PATH - ( slash + 1 - pattern ), L"vpks\\*_dir.vpk" );
			WIN32_FIND_DATAW fd = {};
			HANDLE find = FindFirstFileW( pattern, &fd );
			if ( find != INVALID_HANDLE_VALUE )
			{
				FindClose( find );
				LogWarn( "game branch    : DEFAULT -- content packed in vpks\\. The game ignores "
						 "the mod's loose content files on this branch, so the arms show and "
						 "the optional folders do nothing. Switch to the 'loose' beta: Steam "
						 "-> Properties -> Betas." );
			}
			else
			{
				Log( "game branch    : loose content (no vpks\\ archives) -- the mod's "
					 "content files apply" );
			}
		}
	}

	wchar_t exe[MAX_PATH] = { 0 };
	GetModuleFileNameW( NULL, exe, MAX_PATH );
	Log( "exe            : %S", exe );
	Log( "pid            : %lu", GetCurrentProcessId() );

	SYSTEM_INFO si = {};
	GetNativeSystemInfo( &si );
	Log( "cpu cores      : %lu", si.dwNumberOfProcessors );

	MEMORYSTATUSEX mem = { sizeof( mem ) };
	if ( GlobalMemoryStatusEx( &mem ) )
		Log( "ram            : %llu MB total, %llu MB free",
			 mem.ullTotalPhys / ( 1024ull * 1024ull ),
			 mem.ullAvailPhys / ( 1024ull * 1024ull ) );

	// Address space. This is a 32-bit process sharing one flat range with DXVK,
	// and the ceiling is 2 GB unless the exe was mapped LARGE_ADDRESS_AWARE --
	// which only sinvr_launcher.exe (or an NTCore-patched exe) arranges, because
	// the loader decides it before this DLL exists. Reported from the live
	// process rather than from any header, so it says what we GOT, not what was
	// asked for.
	{
		const AddressSpaceStats as = QueryAddressSpace();
		Log( "address space  : %llu MB limit -- %s",
			 as.limitMB,
			 as.largeAddressAware
				 ? "LARGE_ADDRESS_AWARE, launched via sinvr_launcher.exe or a patched exe"
				 : "2 GB CAP. Launch with sinvr_launcher.exe for the full 4 GB." );
		Log( "               : %llu MB committed, %llu MB reserved, %llu MB free "
			 "(largest run %llu MB, %u regions)",
			 as.committedMB, as.reservedMB, as.freeMB, as.largestFreeMB, as.regions );
	}

	// Which graphics stack is actually loaded tells us whether DXVK is in play.
	const char* graphicsModules[] = { "d3d9.dll", "vulkan-1.dll", "shaderapidx9.dll",
									  "openvr_api.dll", "dinput8.dll" };
	for ( const char* name : graphicsModules )
	{
		HMODULE m = GetModuleHandleA( name );
		if ( !m )
		{
			Log( "module %-16s : not loaded", name );
			continue;
		}

		wchar_t path[MAX_PATH] = { 0 };
		GetModuleFileNameW( m, path, MAX_PATH );
		Log( "module %-16s : %p  %S", name, m, path );
	}

	// Dump every effective setting. A changed built-in default does NOT reach an
	// existing sinvr.cfg -- the file wins -- so "I changed the default" and
	// "the run used the new value" are different claims. Print what is actually
	// in force so a log can never be misread again.
	Log( "config         : %s, %zu entries",
		 g_config.Loaded() ? "loaded" : "defaults", g_config.Count() );
	Log( "--- effective settings ---" );
	{
		size_t keyCount = 0;
		const char* const* keys = Config::KnownKeys( keyCount );
		for ( size_t i = 0; i < keyCount; ++i )
		{
			const char* value = g_config.GetString( keys[i], nullptr );
			Log( "  %-26s = %s%s", keys[i],
				 value ? value : "<built-in default>",
				 value ? "" : " (not present in file)" );
		}
	}

	if ( g_config.GetBool( "freeze_time_across_eyes", false ) )
		LogWarn( "  !! freeze_time_across_eyes is ON. This is known to cripple the "
				 "frame rate (7 frames per 5s instead of 450) and hang. Set it to 0 "
				 "unless you are deliberately investigating it." );
	if ( g_config.UpgradedFrom() > 0 )
		LogWarn( "config         : added %d key(s) missing from an older build. "
				 "Your existing settings were KEPT -- see the 'config:' lines above "
				 "for which keys, and sinvr.cfg.old for the file as it was.",
				 g_config.UpgradedFrom() );
	Log( "-------------------" );
}

DWORD WINAPI InitThread( LPVOID )
{
	// Config first: it decides how much of the rest gets logged.
	g_config.LoadOrCreate( SiblingPath( L"sinvr.cfg" ) );
	SetLogLevel( g_config.GetInt( "log_level", kLogInfo ) );

	LogReset( "=== SiN VR phase 2 (head tracking) ===" );
	SetLogLevel( g_config.GetInt( "log_level", kLogInfo ) );
	Log( "log level %d, sinvr.cfg beside the exe controls it",
		 (int)LogThreshold() );

	g_heartbeatSeconds = g_config.GetInt( "log_heartbeat_seconds", 5 );
	Probe().SetEnabled( g_config.GetBool( "menu_draw_probe", true ) );
	Probe().SetFixMode( g_config.GetInt( "menu_depth_fix", kFixUiBias ) );
	Probe().SetBiasStep( g_config.GetFloat( "menu_depth_bias", 3.0f ) );
	g_vrCameraEnabled = g_config.GetBool( "vr_enabled", true );
	g_camera.SetApplyRoll( g_config.GetBool( "apply_roll", true ) );
	g_camera.SetPitchLimit( g_config.GetFloat( "pitch_limit", 89.0f ) );

	g_engineFov = g_config.GetInt( "engine_fov", 0 );
	g_engineFovIsAuto = ( g_engineFov == 0 );
	g_allowCheats = g_config.GetBool( "engine_allow_cheats", true );

	// Default off now. This was insurance against area portals being the cause of
	// the missing world geometry; the cause turned out to be the culling frustum,
	// which the mod now sets directly and verifies every heartbeat. Forcing every
	// portal open makes the engine draw the whole map, twice per frame, which in
	// a corridor-heavy game is most of the frame cost for no benefit.
	// Default ON. It is the per-eye doorway fix from 2026-08-24: the engine's
	// area-portal decision is made from the view ORIGIN, the two eyes are 2.5
	// units apart, and in a doorway they land in different areas so one eye
	// loses the room beyond. This fallback said `false` for two days after that
	// landed, so every fresh install silently had the bug back -- exactly the
	// two-places trap section 10 warns about, and audit_config.py cannot see it.
	g_openAllPortals = g_config.GetBool( "engine_portals_open_all", true );
	g_disableOcclusion = g_config.GetBool( "engine_disable_occlusion", true );

	// ---- EXPERIMENTAL PERFORMANCE LEVELS -------------------------------
	//
	// Everything this touches already existed as its own key. What was missing
	// was the observation that ALL of them ship in the safe-and-slow position,
	// each for a good reason found separately, and that nobody had ever
	// measured what they cost together.
	//
	// THE LEVEL WINS over the individual keys, deliberately. The first design
	// let an explicitly-set key override the level -- which would have been a
	// silent no-op, because this project writes EVERY key into the generated
	// cfg, so "explicitly set" is true of all of them on every install. To tune
	// by hand, set perf_level = 0.
	//
	// Ordered by ARTIFACT RISK rather than by saving, because the player is the
	// only one who can judge whether an artifact is acceptable and should meet
	// the invisible ones first:
	//
	//   1  Occlusion culling back ON, and the engine FOV margin trimmed to what
	//      the render actually reaches. Measured spare was 5.4% horizontally,
	//      and frustum cost goes as the SQUARE of the tangent. No artifact
	//      expected from either: occlusion is a real engine feature that was
	//      switched off defensively, and the margin is pure slack.
	//
	//   2  Also stops RESCUING boxes the engine wanted to cull. Measured 32238
	//      rescues out of 71190 tests -- 45% of the geometry the engine had
	//      already decided was invisible is drawn anyway. The rescue predates
	//      the engine-FOV fix, which addressed the same problem at its source,
	//      so this may be pure cost. Artifact if wrong: geometry popping at the
	//      edge of vision.
	//
	//   3  Also lets area portals close again. The big one indoors, and SiN is
	//      almost entirely indoors -- forcing them open draws every room beyond
	//      every doorway. The artifact is KNOWN and is why the default is 1:
	//      standing in a doorway, one eye can lose the room beyond. Offered
	//      because "a doorway sometimes flickers" is a trade some players will
	//      take, not because the artifact went away.
	//
	// Every level is measurable from lines that already exist: `stereo frame
	// cost` for the saving and `stereo culling` for what stopped being drawn.
	g_perfLevel = g_config.GetInt( "perf_level", 0 );
	if ( g_perfLevel < 0 ) g_perfLevel = 0;
	if ( g_perfLevel > 3 ) g_perfLevel = 3;

	if ( g_perfLevel >= 1 )
		g_disableOcclusion = false;      // re-enable the engine's occlusion culling
	if ( g_perfLevel >= 3 )
		g_openAllPortals = false;        // let area portals close

	if ( g_perfLevel > 0 )
		Log( "perf: EXPERIMENTAL level %d -- occlusion culling %s | cull rescues "
			 "%s | area portals %s | fov margin %s. These OVERRIDE the individual "
			 "keys; set perf_level = 0 to tune by hand. Compare `stereo frame "
			 "cost` against a level 0 run, and if nothing moved, put it back.",
			 g_perfLevel,
			 g_disableOcclusion ? "still OFF" : "ON",
			 ( g_perfLevel >= 2 ) ? "OFF" : "on",
			 g_openAllPortals ? "forced open" : "allowed to CLOSE",
			 ( g_perfLevel >= 1 ) ? "1.00 (trimmed)" : "1.05" );
	g_disableVis = g_config.GetBool( "engine_disable_vis", false );
	g_engineTuneIntervalMs = g_config.GetInt( "engine_tune_interval_seconds", 5 ) * 1000;

	InstallCrashHandler();
	LogEnvironment();

	// The engine and game DLLs load well after our proxy does.
	const int kTimeoutMs = 120000;
	const int kPollMs = 250;
	int waited = 0;
	void* engineIface = nullptr;

	while ( waited < kTimeoutMs )
	{
		engineIface = GetInterface( "engine.dll", "VEngineClient012" );
		g_client = GetInterface( "client.dll", "VClient011" );

		if ( engineIface && g_client )
			break;

		Sleep( kPollMs );
		waited += kPollMs;
	}

	if ( !engineIface || !g_client )
	{
		Log( "!! timed out after %dms (engine=%p client=%p)", waited, engineIface, g_client );
		return 0;
	}

	g_engine = EngineClient( engineIface );
	Log( "Interfaces bound after %dms", waited );

	// ---- WHAT EACH MODULE ACTUALLY OFFERS ------------------------------
	//
	// One-shot, opt-in, read-only. Every interface this project uses was
	// found by GUESSING a version string and seeing whether the factory
	// returned non-null; this is the first time the question is put to the
	// binary instead. Placed here because the modules are known loaded and
	// nothing has been hooked yet.
	g_logInterfacesInGame = g_config.GetBool( "log_interfaces", false );

	// OFF by default and gated for the whole session. This is the only part of
	// the mod that touches server.dll, and everything else works with it off --
	// see server_movement in the cfg.
	Movement().SetEnabled( g_config.GetBool( "server_movement", true ) );
	Shots().SetEnabled( g_config.GetBool( "shot_probe", false ) );
	{
		// The two halves of "shots from the gun" are one switch, because either
		// alone is worse than neither: the origin without the direction fires a
		// convergence-bent angle from the muzzle, and the direction without the
		// origin fires the raw gun axis from the eye. Both are a miss.
		//
		// It also implies the probe's hook, since the rewrite lives in it.
		const bool fromGun = g_config.GetBool( "shot_from_gun", true );
		if ( fromGun )
			Shots().SetEnabled( true );
		Shots().SetRewrite( fromGun );
		g_camera.SetShotFromGun( fromGun );
	}
	{
		SixDofSettings six;
		// Two separate switches on purpose: server_movement binds and hooks,
		// which is observational; sixdof_body is what actually writes to the
		// move data. A build that misbehaves can be halved in one launch.
		six.body = g_config.GetBool( "sixdof_body", true );
		six.deadzone = g_config.GetFloat( "sixdof_deadzone", 12.0f );
		six.rate = g_config.GetFloat( "sixdof_rate", 120.0f );
		six.chase = g_config.GetFloat( "sixdof_chase", 0.15f );
		six.maxStep = g_config.GetFloat( "sixdof_max_step", 8.0f );
		six.settle = g_config.GetFloat( "sixdof_settle", 1.0f );
		six.forwardRatio = g_config.GetFloat( "sixdof_forward_ratio", 0.65f );
		six.leanTilt = g_config.GetFloat( "sixdof_lean_tilt", 0.5f );
		six.ramp = g_config.GetFloat( "sixdof_ramp", 0.3f );
		Movement().SetSixDof( six );
	}
	if ( g_logInterfacesInGame )
	{
		Log( "--- interface dump ---" );
		LogModuleInterfaces( "client.dll" );
		LogModuleInterfaces( "engine.dll" );
		LogModuleInterfaces( "vgui2.dll" );
		LogModuleInterfaces( "materialsystem.dll" );
		Log( "--- end interface dump ---" );
	}

	RunSanityCheck();

	// VR is optional: if it fails we still hook, so the game stays playable and
	// the log explains why there is no head tracking.
	const bool submitFrames = g_config.GetBool( "submit_frames", true );

	VRBackendSettings vrSettings;
	vrSettings.worldScale = g_config.GetFloat( "world_scale", kSourceUnitsPerMeter );
	vrSettings.sceneApp = submitFrames;
	vrSettings.pauseSubmitOnStandby = g_config.GetBool( "vr_pause_submit_on_standby", true );
	vrSettings.guardSubmit = g_config.GetBool( "vr_submit_guard", true );
	vrSettings.checkOutputDevice = g_config.GetBool( "vr_submit_check_gpu", true );

	g_camera.SetPositionalTracking( g_config.GetBool( "positional_tracking", true ) );
	g_camera.SetPositionalScale( g_config.GetFloat( "positional_scale", 1.0f ) );
	g_camera.SetPositionalMaxOffset( g_config.GetFloat( "positional_max_offset", 80.0f ) );
	g_camera.SetAimConvergence( g_config.GetFloat( "aim_convergence_distance", 250.0f ) );
	// ---- READ THE KEY, NOT THE STRUCT ----------------------------------
	//
	// This said `vrSettings.leftHanded` and ran EIGHTY-EIGHT LINES BEFORE that
	// field is assigned, so it always read the default -- false. Every weapon
	// then loaded the RIGHT-handed bore zero while the player was left-handed:
	// the assault rifle 13 units out, the scattergun 18. Reported from the
	// headset as "the crosshair is wrong", with the log cheerfully stating
	// "right-handed profile" next to left_handed = 1 in the cfg.
	//
	// Reading the config key directly removes the ordering dependency instead
	// of merely satisfying it. A value that is only correct when an unrelated
	// line has already run is a trap that re-arms every time the file is
	// edited.
	g_boreLeftHanded = g_config.GetBool( "left_handed", false );
	g_camera.SetAimOriginHead( g_config.GetBool( "aim_origin_head", false ) );
	g_boreGlobalRight = g_config.GetFloat( "aim_bore_right", -3.0f );
	g_boreGlobalUp = g_config.GetFloat( "aim_bore_up", 0.0f );
	g_boreGlobalFwd = g_config.GetFloat( "aim_bore_fwd", 0.0f );
	g_boreGlobalYaw = g_config.GetFloat( "aim_bore_yaw", 0.0f );
	g_boreGlobalPitch = g_config.GetFloat( "aim_bore_pitch", 0.0f );
	g_boreAdjust = g_config.GetBool( "aim_bore_adjust", true );
	g_boreStep = g_config.GetFloat( "aim_bore_step", 0.5f );
	g_boreAngleStep = g_config.GetFloat( "aim_bore_angle_step", 0.25f );
	g_camera.SetBoreOffset( g_boreGlobalRight, g_boreGlobalUp );

	// ---- THE TUNED ZEROS, BUILT IN --------------------------------------
	//
	// Measured in the headset 2026-09-06 against the point of impact, one gun at
	// a time, with the green shot line visible. They live in CODE, not only in a
	// cfg, for the reason the per-weapon grips and the LAA byte patch both
	// taught this project: a fresh install reverts anything that exists only in
	// a file, and undocumented tuning then looks like a bug in the mod rather
	// than a missing setting.
	//
	// ---- THESE ARE NOT THE OLD NUMBERS, AND NOT THE SAME QUANTITY -------
	//
	// The previous seeds (magnum -17.5, rifle -25.5, scattergun +3.5 left-handed;
	// -34.5 / -38.5 / -14.5 right-handed) were PARALLAX corrections for a shot
	// leaving the eye. shot_from_gun deleted that problem, and these are now the
	// MUZZLE's position in the gun's own basis. Numerically similar in places,
	// physically unrelated, and the old values would throw the origin metres off
	// the gun. They are recorded here only so nobody restores them.
	//
	// ---- WHY BOTH HANDS GET THE SAME VALUES NOW ------------------------
	//
	// The old seeds were per-hand and the comment beside them warned that the
	// right-handed set was NOT the left mirrored but shifted by a roughly
	// constant amount. That was true OF PARALLAX: the gun sits the other side of
	// the head, so the eye-to-muzzle offset flips and shifts by about 20 units.
	//
	// It does not carry over. `left_handed` only swaps WHICH CONTROLLER drives
	// the weapon -- checked, it is a hand-index swap in openvr_backend and
	// nothing more. The model is not mirrored, so the muzzle sits in the same
	// place in the gun whichever hand holds it, and a position in the gun's own
	// basis is handedness-independent by construction.
	//
	// So the right-handed profile is seeded from the same measurements rather
	// than from a mirror or a guess. What may still differ is grip ergonomics --
	// how a controller naturally sits in one hand versus the other, which shows
	// up in YAW. That is a small personal correction, and the live tuner and the
	// per-hand cfg keys are still there for it.
	{
		struct BoreSeed { const char* model; float right, up, fwd, yaw, pitch; };

		static const BoreSeed kBoreSeeds[] = {
			//                 right    up     fwd    yaw    pitch
			{ "v_magnum",        -0.5f,  0.5f,  8.5f,  2.25f, 0.5f },
			{ "v_assault_rifle", -0.5f,  2.0f, 14.0f,  4.00f, 1.5f },
			{ "v_scattergun",     1.0f,  1.0f, 17.0f, -0.75f, 0.0f },
		};

		const int seedCount =
			(int)( sizeof( kBoreSeeds ) / sizeof( kBoreSeeds[0] ) );

		for ( int i = 0; i < seedCount; ++i )
		{
			const BoreSeed& sd = kBoreSeeds[i];
			BoreOffset* b = FindBore( sd.model, true );
			if ( b )
			{
				b->right = sd.right;
				b->up = sd.up;
				b->fwd = sd.fwd;
				b->yaw = sd.yaw;
				b->pitch = sd.pitch;
			}
		}
	}

	// Per-weapon zeros from the cfg. OPTIONAL keys -- absent by default and NOT
	// in KnownKeys, exactly like the per-weapon viewmodel offsets. Config still
	// WINS over the seeds above, so a player who tunes their own is not
	// overruled on the next launch.
	{
		static const char* const kBoreModels[] = {
			"v_magnum", "v_assault_rifle", "v_scattergun" };
		for ( const char* model : kBoreModels )
		{
			char kr[64], ku[64];
			_snprintf_s( kr, sizeof( kr ), _TRUNCATE, "aim_bore_right_%s_%s",
						 model, BoreHandSuffix() );
			_snprintf_s( ku, sizeof( ku ), _TRUNCATE, "aim_bore_up_%s_%s",
						 model, BoreHandSuffix() );
			char kyg[64];
			_snprintf_s( kyg, sizeof( kyg ), _TRUNCATE, "aim_bore_yaw_%s_%s",
						 model, BoreHandSuffix() );
			if ( !g_config.GetString( kr, nullptr ) && !g_config.GetString( ku, nullptr ) &&
				 !g_config.GetString( kyg, nullptr ) )
				continue;

			BoreOffset* b = FindBore( model, true );
			if ( !b )
				continue;
			b->right = g_config.GetFloat( kr, g_boreGlobalRight );
			b->up = g_config.GetFloat( ku, g_boreGlobalUp );

			char kf[64];
			_snprintf_s( kf, sizeof( kf ), _TRUNCATE, "aim_bore_fwd_%s_%s",
						 model, BoreHandSuffix() );
			b->fwd = g_config.GetFloat( kf, g_boreGlobalFwd );

			char ky[64], kp[64];
			_snprintf_s( ky, sizeof( ky ), _TRUNCATE, "aim_bore_yaw_%s_%s",
						 model, BoreHandSuffix() );
			_snprintf_s( kp, sizeof( kp ), _TRUNCATE, "aim_bore_pitch_%s_%s",
						 model, BoreHandSuffix() );
			b->yaw = g_config.GetFloat( ky, g_boreGlobalYaw );
			b->pitch = g_config.GetFloat( kp, g_boreGlobalPitch );

			Log( "aim bore: %s from cfg yaw=%.2f pitch=%.2f right=%.2f up=%.2f "
				 "fwd=%.2f",
				 model, b->yaw, b->pitch, b->right, b->up, b->fwd );
		}
	}

	Log( "aim bore: %s-handed profile | global right=%.2f up=%.2f | live zeroing "
		 "%s ( , left  . right  / save ), step %.2f",
		 BoreHandSuffix(), g_boreGlobalRight, g_boreGlobalUp,
		 g_boreAdjust ? "ON" : "off", g_boreStep );
	for ( int i = 0; i < g_boreCount; ++i )
		Log( "aim bore: %-16s yaw=%6.2f pitch=%6.2f right=%7.2f up=%6.2f fwd=%6.2f",
			 g_bore[i].model, g_bore[i].yaw, g_bore[i].pitch,
			 g_bore[i].right, g_bore[i].up, g_bore[i].fwd );

	vrSettings.leftHanded = g_config.GetBool( "left_handed", false );
	vrSettings.swapThumbsticks = g_config.GetBool( "swap_thumbsticks", false );
	g_swapThumbsticks = vrSettings.swapThumbsticks;
	g_hideCrosshairWhenAiming = g_config.GetBool( "hide_crosshair_when_aiming", true );

	// Entity list, for reaching the viewmodel. Bound the same way as the other
	// three interfaces; the singleton address found by disassembly is recorded
	// in client_entity_list.h as evidence only, never used.
	{
		void* raw = GetInterface( "client.dll", "VClientEntityList003" );
		ClientEntityList list( raw );
		if ( !raw )
		{
			LogWarn( "entity list: VClientEntityList003 not returned by client.dll "
					 "-- viewmodel tracking unavailable" );
		}
		else
		{
			g_entities = list;
			g_viewModel.Bind( g_entities );

			// Deliberately NOT gated on GetMaxEntities here. That field is only
			// populated once the client is connected, so at mod-init it reads 0
			// on a perfectly good interface -- which is precisely what happened
			// on the first attempt, and the hard refusal it triggered disabled
			// viewmodel tracking for the whole session while the map was in fact
			// correct. The check now lives in ViewModelDriver::Apply, where it
			// retries until the entity list is live.
			Log( "entity list: VClientEntityList003 at %p (maxEntities=%d now; "
				 "0 before a map is loaded is normal -- verified lazily)",
				 raw, list.GetMaxEntities() );
		}
	}

	// Recoil compensation needs m_vecPunchAngle's offset. Looked up BY NAME from
	// the client's own RecvTables rather than hardcoded, so a game update moving
	// the field does not silently turn this into a write-adjacent read.
	g_recoilCompensation = g_config.GetFloat( "recoil_compensation", 0.0f );
	if ( g_recoilCompensation < 0.0f ) g_recoilCompensation = 0.0f;
	if ( g_recoilCompensation > 1.0f ) g_recoilCompensation = 1.0f;

	// ---- THESE LOOKUPS ARE NOT ABOUT RECOIL AND MUST NOT BE GATED ON IT ----
	//
	// This whole block used to sit inside `if ( g_recoilCompensation > 0.0f )`,
	// which made an unrelated balance setting the switch for two other
	// subsystems:
	//
	//   m_fFlags        -> FL_FROZEN -> the cutscene detector fixed 2026-08-26
	//   m_vecPunchAngle -> the viewkick, which recoil_view_lock needs
	//
	// So setting `recoil_compensation = 0` -- the documented default, and the
	// exact thing to try when investigating recoil -- silently turned OFF
	// cutscene detection and made the punch angle unreadable. A run performed
	// to test recoil could therefore never see the recoil, and the log lost the
	// two lines that would have said so.
	//
	// The viewmodel animation code immediately below already had to work around
	// this, in as many words: "shares the NetProps instance with recoil
	// compensation -- which only initialises it when that feature is on, so
	// initialise it here too rather than depending on an unrelated setting".
	// That workaround was the warning, and it was left in place rather than the
	// cause being fixed.
	if ( g_netProps.Init( g_client ) )
	{
		g_playerFlagsOffset = g_netProps.Find( "CBasePlayer", "m_fFlags" );
		if ( g_playerFlagsOffset < 0 )
			g_playerFlagsOffset = g_netProps.FindAnywhere( "m_fFlags", nullptr );
		Log( "cutscene: m_fFlags %s -- FL_FROZEN is %s, so a "
			 "point_viewcontrol is detected the frame it starts rather than "
			 "six frames later",
			 g_playerFlagsOffset >= 0 ? "found" : "NOT FOUND",
			 g_playerFlagsOffset >= 0 ? "readable"
									  : "unreadable; falling back to the "
										"duration heuristic alone" );

		g_punchAngleOffset = g_netProps.Find( "CBasePlayer", "m_vecPunchAngle" );
		if ( g_punchAngleOffset < 0 )
			g_punchAngleOffset = g_netProps.Find( "CSinPlayer", "m_vecPunchAngle" );
		if ( g_punchAngleOffset < 0 )
			g_punchAngleOffset = g_netProps.Find( "CHL2_Player", "m_vecPunchAngle" );
	}

	// Reported whatever compensation is set to, because the punch angle is now
	// read for recoil_view_lock as well -- and because a missing line is the
	// hardest kind of evidence to notice.
	if ( g_punchAngleOffset >= 0 )
	{
		Log( "recoil: compensation %.2f, m_vecPunchAngle at +0x%X (found by name). "
			 "NOTE: on SiN this reads 0 through a whole burst -- the kick is "
			 "applied server-side and arrives as already-rotated view angles, "
			 "so recoil_view_lock keys off FIRING, not off punch.",
			 g_recoilCompensation, g_punchAngleOffset );
	}
	else
	{
		LogWarn( "recoil: m_vecPunchAngle not found in any client RecvTable -- "
				 "compensation AND recoil_view_lock are both disabled. The engine "
				 "fires along EyeAngles()+punch, so shots will keep drifting." );
		g_recoilCompensation = 0.0f;
	}

	// Viewmodel animation suppression. Needs the client's RecvTables, so it
	// shares the NetProps instance with recoil compensation -- which only
	// initialises it when that feature is on, so initialise it here too
	// rather than depending on an unrelated setting being enabled.
	{
		ViewModelAnimSettings animSettings;
		animSettings.log = g_config.GetBool( "viewmodel_anim_log", false );
		// Both default ON. The gun replaying a swing your arm already made, and
		// leaving your hand to reload itself, are the two animations that most
		// obviously fight a tracked controller.
		animSettings.suppressMelee = g_config.GetBool( "viewmodel_anim_suppress_melee", true );
		animSettings.suppressReload = g_config.GetBool( "viewmodel_anim_suppress_reload", true );
		animSettings.suppressDraw = g_config.GetBool( "viewmodel_anim_suppress_draw", false );
		animSettings.suppressLower = g_config.GetBool( "viewmodel_anim_suppress_lower", false );
		g_viewModelAnim.SetSettings( animSettings );

		// Bound unconditionally, even with every suppression off: the PER-WEAPON
		// viewmodel offsets need to know which model is in hand, and this is the
		// one place that resolves it. Behaviour is still gated by the settings
		// above -- binding only reads offsets and a model name.
		if ( !g_netProps.Valid() )
			g_netProps.Init( g_client );
		void* modelInfo = GetInterface( "engine.dll", "VModelInfoClient003" );
		g_viewModelAnim.Bind( g_netProps, modelInfo );
	}

	ViewModelSettings vmSettings;
	vmSettings.followController = g_config.GetBool( "viewmodel_follow_controller", true );
	vmSettings.offsetForward = g_config.GetFloat( "viewmodel_offset_forward", -26.0f );
	vmSettings.offsetRight = g_config.GetFloat( "viewmodel_offset_right", -10.0f );
	vmSettings.offsetUp = g_config.GetFloat( "viewmodel_offset_up", 6.0f );
	vmSettings.anglePitch = g_config.GetFloat( "viewmodel_angle_pitch", 58.0f );
	vmSettings.angleYaw = g_config.GetFloat( "viewmodel_angle_yaw", 0.0f );
	vmSettings.angleRoll = g_config.GetFloat( "viewmodel_angle_roll", 0.0f );
	vmSettings.writeCoordinateFrame = g_config.GetBool( "viewmodel_write_coordinate_frame", true );
	vmSettings.hookSetters = g_config.GetBool( "viewmodel_hook_setters", true );
	vmSettings.collide = g_config.GetBool( "viewmodel_collide", true );
	vmSettings.collideRadius = g_config.GetFloat( "viewmodel_collide_radius", 2.0f );
	vmSettings.collideBarrel = g_config.GetFloat( "viewmodel_collide_barrel", 12.0f );
	vmSettings.collideResponse = g_config.GetFloat( "viewmodel_collide_response", 0.35f );
	vmSettings.collideDeflect = g_config.GetFloat( "viewmodel_collide_deflect", 60.0f );
	vmSettings.collideDeflectCurve = g_config.GetFloat( "viewmodel_collide_deflect_curve", 1.5f );
	vmSettings.aimTrace = g_config.GetBool( "aim_convergence_trace", true );
	vmSettings.aimTraceMax = g_config.GetFloat( "aim_convergence_max", 4096.0f );
	vmSettings.pinWeaponEntity = g_config.GetBool( "weapon_entity_pin", true );
	vmSettings.hideWeaponEntity = g_config.GetBool( "weapon_entity_hide", true );
	g_viewModel.SetSettings( vmSettings );

	// The weapon entity's field offsets come from the animation module, which
	// already walked the RecvTables. Bind() ran earlier, so they are resolved.
	g_viewModel.SetWeaponFieldOffsets( g_viewModelAnim.WeaponHandleOffset(),
									   g_viewModelAnim.EffectsOffset() );

	// Per-weapon position offsets.
	//
	// SEEDED IN CODE FIRST, then overridden by config.
	//
	// ---- WHAT "ABSENT" MEANS, PRECISELY ---------------------------------
	//
	// The keys are absent-by-default because listing them in the generated cfg
	// would make every launch see them missing and regenerate the file,
	// discarding tuned values on every start.
	//
	// It is tempting to describe that as "absent means use the global", and
	// that description is WRONG once these seeds exist -- it was true only
	// before them. SetModelOffsets sets `configured`, and Eff() returns the
	// per-model values whenever that flag is set, so:
	//
	//     key absent   -> the SEED below, per weapon
	//     key present  -> the config value, per weapon
	//     no seed and no key -> the global offsets
	//
	// Only a weapon this table does not name ever reaches the global. That
	// matters: the three viewmodels are authored with different origins and
	// need genuinely different offsets -- the scattergun sits 16 units further
	// forward than the magnum -- so collapsing them onto one global value
	// would put two of the three guns in the wrong place for every player who
	// never tuned them.
	//
	// Measured on a Vive Cosmos, right-handed grip. A starting point, not a
	// calibration -- the numpad tuner still exists because hands differ.
	{
		struct Seed { const char* model; float fwd, right, up; };
		static const Seed kSeeds[] = {
			{ "v_magnum",        -26.0f, -10.0f, 5.0f },
			{ "v_assault_rifle", -24.0f,  -9.0f, 8.0f },
			{ "v_scattergun",    -10.0f,  -4.0f, 4.0f },
		};
		for ( const Seed& sd : kSeeds )
			g_viewModel.SetModelOffsets( sd.model, sd.fwd, sd.right, sd.up );

		// Config still wins. An absent key leaves the seed in place rather
		// than falling back to the global, which is the whole point.
		static const char* const kModels[] = { "v_magnum", "v_assault_rifle", "v_scattergun" };
		for ( const char* model : kModels )
		{
			char kf[64], kr[64], ku[64];
			_snprintf_s( kf, sizeof( kf ), _TRUNCATE, "viewmodel_offset_forward_%s", model );
			_snprintf_s( kr, sizeof( kr ), _TRUNCATE, "viewmodel_offset_right_%s", model );
			_snprintf_s( ku, sizeof( ku ), _TRUNCATE, "viewmodel_offset_up_%s", model );
			if ( !g_config.GetString( kf, nullptr ) && !g_config.GetString( kr, nullptr ) &&
				 !g_config.GetString( ku, nullptr ) )
				continue;
			const float f = g_config.GetFloat( kf, vmSettings.offsetForward );
			const float r = g_config.GetFloat( kr, vmSettings.offsetRight );
			const float u = g_config.GetFloat( ku, vmSettings.offsetUp );
			g_viewModel.SetModelOffsets( model, f, r, u );
			Log( "viewmodel: %s overridden from cfg fwd=%.1f right=%.1f up=%.1f",
				 model, f, r, u );
		}

		// ---- SAY WHAT IS ACTUALLY IN EFFECT, ALWAYS ---------------------
		//
		// The line above only printed when a cfg key was present, so a fresh
		// install -- the case where someone is most likely to be wondering
		// whether per-weapon offsets exist at all -- logged nothing, and the
		// only way to find out was to read the source. That is the same
		// "a conditional log is a bad instrument" trap this project has hit
		// before: the state it could not describe was exactly the default one.
		for ( const Seed& sd : kSeeds )
		{
			ModelOffsets eff;
			const bool have = g_viewModel.ModelOffsetsFor( sd.model, eff );
			Log( "viewmodel: %-16s fwd=%.1f right=%.1f up=%.1f  (%s)",
				 sd.model, eff.forward, eff.right, eff.up,
				 !have ? "GLOBAL -- no per-weapon value"
					   : "per-weapon" );
		}
	}

	g_vmTraceFrames = g_config.GetInt( "viewmodel_trace_eye_passes", 0 );
	g_viewModel.SetDiagnoseRuns( g_config.GetInt( "viewmodel_diagnose", 0 ) );
	g_fireDumpsLeft = g_config.GetInt( "viewmodel_diagnose_fire", 0 );
	if ( g_vmTraceFrames > 0 )
		Log( "viewmodel: tracing the transform around both eye passes for %d frames "
			"-- this is the flicker investigation, set viewmodel_trace_eye_passes=0 "
			"to stop it", g_vmTraceFrames );

	g_viewModelAdjust = g_config.GetBool( "viewmodel_adjust_enabled", false );
	if ( g_viewModelAdjust && g_config.GetBool( "eye_adjust_enabled", false ) )
	{
		// Both read the same numpad keys. Rather than have one silently win --
		// which would look like "the adjust keys do the wrong thing" and be
		// miserable to diagnose in a headset -- refuse and say which to turn off.
		g_viewModelAdjust = false;
		LogWarn( "viewmodel_adjust_enabled and eye_adjust_enabled are both on and "
				 "share the numpad. Disabling viewmodel adjust for this run -- "
				 "turn eye_adjust_enabled off to tune the weapon offsets." );
	}
	if ( g_viewModelAdjust )
	{
		Log( "viewmodel adjust: ON -- numpad 8/2 forward/back, 4/6 left/right, "
			 "9/3 up/down, 7 switch POSITION<->ANGLES, 5+5 reset (double press), -/+ step, "
			"0 PRINT AND SAVE to sinvr.cfg. "
			 "Values are logged as cfg "
			 "lines each time, so tune it once and paste the result." );
		g_viewModel.LogOffsets( "starting values" );
	}

	{
		const char* aim = g_config.GetString( "aim_source", "controller" );
		const bool byController = ( aim && _stricmp( aim, "controller" ) == 0 );
		g_camera.SetAimSource( byController ? kAimController : kAimHmd );
		if ( aim && !byController && _stricmp( aim, "hmd" ) != 0 )
			LogWarn( "aim_source '%s' not recognised -- using 'hmd'. "
					 "Valid values are 'hmd' and 'controller'.", aim );
	}

	// Body zones. Loaded BEFORE the features that point at them, because they
	// hold a pointer rather than a copy -- one box, drawn and tested, so the
	// picture can never disagree with the behaviour.
	{
		struct ZoneDefault { int id; float f, l, u, sf, sl, su; };

		// ---- TWO HAND-TUNED PROFILES, NOT ONE SET PLUS A SIGN FLIP --------
		//
		// The holster zones sit in BODY space, and `left_handed` swaps which
		// controller reaches for them. So one set of numbers cannot serve both:
		// the same-side and cross-body reaches trade places, and the shoulder
		// boxes are not even the same shape as each other -- tuning found the
		// cross-body one wants to be shallower.
		//
		// Mirroring the lateral axis automatically was the obvious alternative
		// and was rejected: the snap-to-hand tuner writes the value it MEASURED,
		// so a mirrored load with an unmirrored save drifts further out every
		// time a player tunes. Two explicit profiles, each tuned by hand for the
		// hand it belongs to, has no such failure mode.
		//
		// melee_gesture.h does mirror itself (`inwardSign`), and that is fine --
		// a swing is a direction, not a place.
		static const ZoneDefault kHolsterLeftHanded[] = {
			// Tuned in the headset 2026-09-05, weapon hand = LEFT.
			// The hip box is deliberately the biggest of the three: it was the
			// one that kept missing, because a reach to the hip is the least
			// repeatable -- the arm is extended and the hand arrives at a
			// different angle every time.
			{ kZoneHolsterHip,   -20.0f, -22.0f, -41.0f, 52.0f, 40.0f, 42.0f },
			{ kZoneHolsterLeft,  -12.0f, -18.0f,   6.0f, 48.0f, 24.0f, 28.0f },
			{ kZoneHolsterRight, -12.0f,  22.0f,  -6.0f, 46.0f, 26.0f, 20.0f },
		};

		static const ZoneDefault kHolsterRightHanded[] = {
			// MEASURED 2026-09-07 from 22 logged grip presses, weapon hand = RIGHT.
			// No longer the mirrored placeholder.
			//
			// The mirror PLAYED fine -- 20 of 22 reaches hit -- and was reported as
			// needing no tuning. The log disagreed: every reach sat 15 to 23 units
			// FORWARD of its box centre, the hip hits came within 0.2 units of
			// falling out, and both misses were near-misses by 0.8. It only worked
			// because the boxes are large, and it was riding their back edges.
			//
			// Recentred on the measured reaches with the sizes left alone, the worst
			// margin in each zone goes from -0.8 / 2.3 / -0.8 to 11.8 / 10.6 / 6.9.
			//
			// > A hit rate is not a margin. 20 of 22 felt fine and was one bad
			// > reach away from feeling broken, and only the logged positions
			// > could tell the difference.
			{ kZoneHolsterHip,      3.0f,    8.6f,  -24.8f, 52.0f, 40.0f, 42.0f },
			{ kZoneHolsterLeft,     4.1f,    9.7f,   -0.5f, 48.0f, 24.0f, 28.0f },
			{ kZoneHolsterRight,    3.4f,  -10.1f,   -1.8f, 46.0f, 26.0f, 20.0f },
		};

		// Handedness-independent: melee resolves its own direction, and the
		// reload zone is at the centre of the body.
		static const ZoneDefault kSharedZones[] = {
			{ kZoneMeleeStart, 0.0f, 0.0f, 12.0f, 80.0f, 80.0f, 56.0f },
			{ kZoneReload, 4.0f, 4.0f, -37.0f, 56.0f, 30.0f, 34.0f },
		};

		// Direct from the config for the same reason the bore zeros are -- see
		// the note there. This one happens to sit after the assignment and was
		// correct, which is exactly what makes the pattern worth removing
		// rather than relying on.
		const bool lh = g_config.GetBool( "left_handed", false );
		const ZoneDefault* holsters = lh ? kHolsterLeftHanded : kHolsterRightHanded;

		// The cfg carries ONE set of zone keys, because a person has one
		// handedness -- and which profile they want follows from left_handed,
		// so nothing needs to state it separately. The consequence, noted in
		// the cfg beside the values: flipping left_handed leaves the other
		// hand's numbers in the file, and they must be re-tuned or deleted for
		// the built-in profile to apply.
		// Counted so the log can distinguish "the built-in profile is in
		// effect" from "a cfg value is overriding it". That distinction IS the
		// bug this block used to have -- the generated cfg shipped the
		// right-handed numbers, they overrode the built-in profile, and a
		// left-handed player reached for right-handed zones with nothing
		// anywhere saying so.
		int zoneOverrides = 0;
		for ( int pass = 0; pass < 2; ++pass )
		{
			const ZoneDefault* table = ( pass == 0 ) ? holsters : kSharedZones;
			const int count = ( pass == 0 )
				? (int)( sizeof( kHolsterLeftHanded ) / sizeof( kHolsterLeftHanded[0] ) )
				: (int)( sizeof( kSharedZones ) / sizeof( kSharedZones[0] ) );


			for ( int i = 0; i < count; ++i )
			{
				const ZoneDefault& d = table[i];
				const char* pfx = ZoneStyleFor( d.id ).configPrefix;
				char key[64];
				ZoneBox& z = g_zones.Get( d.id );

				z.forward = d.f;
				z.lateral = d.l;
				z.up = d.u;
				z.sizeForward = d.sf;
				z.sizeLateral = d.sl;
				z.sizeUp = d.su;

				_snprintf_s( key, sizeof( key ), _TRUNCATE, "%s_forward", pfx );
				z.forward = g_config.GetFloat( key, d.f );
				_snprintf_s( key, sizeof( key ), _TRUNCATE, "%s_lateral", pfx );
				z.lateral = g_config.GetFloat( key, d.l );
				_snprintf_s( key, sizeof( key ), _TRUNCATE, "%s_up", pfx );
				z.up = g_config.GetFloat( key, d.u );
				_snprintf_s( key, sizeof( key ), _TRUNCATE, "%s_size_forward", pfx );
				z.sizeForward = g_config.GetFloat( key, d.sf );
				_snprintf_s( key, sizeof( key ), _TRUNCATE, "%s_size_lateral", pfx );
				z.sizeLateral = g_config.GetFloat( key, d.sl );
				_snprintf_s( key, sizeof( key ), _TRUNCATE, "%s_size_up", pfx );
				z.sizeUp = g_config.GetFloat( key, d.su );

				// Only the holster boxes are per-hand, so only they can be
				// overridden into the wrong hand's geometry. The melee and
				// reload zones are shared and ship in the cfg as normal.
				if ( pass == 0 )
				{
					_snprintf_s( key, sizeof( key ), _TRUNCATE, "%s_lateral", pfx );
					if ( g_config.GetString( key, nullptr ) )
						++zoneOverrides;
				}
			}
		}

		Log( "holster zones: %s-handed profile in effect%s",
			 lh ? "LEFT" : "right",
			 zoneOverrides
				 ? "  <-- but the cfg OVERRIDES it; those values are tuned for "
				   "ONE hand, so delete them if you have flipped left_handed"
				 : " (built in -- no zone_holster_* lines in the cfg)" );

		g_zones.CaptureBaseline();
		g_zones.SetVisible( g_config.GetBool( "zone_boxes_visible", false ) );

		// Tuning implies drawing: nudging a box you cannot see is not a
		// feature, it is a way to lose an afternoon. Turned on rather than
		// refused, because the intent is unambiguous.
		g_zoneAdjust = g_config.GetBool( "zone_adjust_enabled", false );
		if ( g_zoneAdjust )
		{
			if ( g_config.GetBool( "eye_adjust_enabled", false ) ||
				 g_viewModelAdjust )
			{
				g_zoneAdjust = false;
				LogWarn( "zone_adjust_enabled is on together with another numpad "
						  "tuner, and all three share the numpad. Zone tuning is "
						  "OFF; turn eye_adjust_enabled and "
						  "viewmodel_adjust_enabled off to position the zones." );
			}
			else if ( !g_zones.Visible() )
			{
				g_zones.SetVisible( true );
				Log( "zones: zone_adjust_enabled turned the boxes on -- "
						  "zone_boxes_visible was 0, and tuning an invisible box "
						  "is not something anyone wants" );
			}
		}
		g_zones.SetAdjustEnabled( g_zoneAdjust );
		if ( g_zones.Visible() )
		{
			g_zones.BindOverlay( GetInterface( "engine.dll", "VDebugOverlay003" ) );
			g_zones.LogAsConfig();
		}

		// The laser dot draws through the same interface. Its own binding
		// rather than a shared one, so turning the zone boxes off does not
		// silently take the crosshair with them.
		// Two-handed weapons.
		{
			TwoHandedSettings th;
			th.enabled = g_config.GetBool( "two_handed", true );

			// auto / toggle / hold. Named rather than numbered because a
			// number in a cfg file tells the reader nothing, and this is a
			// setting people will actually change.
			{
				const char* m = g_config.GetString( "two_handed_mode", "auto" );
				if ( m && _stricmp( m, "toggle" ) == 0 )
					th.mode = kTwoHandedToggle;
				else if ( m && _stricmp( m, "hold" ) == 0 )
					th.mode = kTwoHandedHold;
				else
				{
					th.mode = kTwoHandedAuto;
					if ( m && _stricmp( m, "auto" ) != 0 )
						LogWarn( "two_handed_mode '%s' not recognised -- using "
								 "'auto'. Valid values are 'auto', 'toggle' and "
								 "'hold'.", m );
				}
			}
			// ---- WHO OWNS THE OFF HAND'S GRIP -----------------------------
			//
			// Decided here because this is the only place that knows both
			// answers. The foregrip has first claim: in `toggle` or `hold` the
			// grip IS the two-handed control, and firing a grenade every time
			// somebody steadies their rifle would be a worse bug than having no
			// grenade button at all.
			//
			// In `auto` -- the default -- the foregrip needs no button and the
			// grip is genuinely idle, so grenades get it. That gives Touch and
			// WMR a real grenade button in the shipped configuration, where
			// before there was none and turn_stick_grenade was the only route.
			th.debug = g_config.GetBool( "two_handed_debug", false );
			th.radius = g_config.GetFloat( "two_handed_radius", 6.0f );
			th.releaseRadius =
				g_config.GetFloat( "two_handed_release_radius", 9.0f );
			th.gripForward = g_config.GetFloat( "two_handed_grip_forward", 0.0f );
			th.gripRight = g_config.GetFloat( "two_handed_grip_right", 0.0f );
			th.gripUp = g_config.GetFloat( "two_handed_grip_up", 7.5f );
			th.blend = g_config.GetFloat( "two_handed_blend", 1.0f );
			g_twoHanded.SetSettings( th );

			// Per-weapon foregrips, absent-by-default exactly like the
			// per-weapon viewmodel offsets -- so they must NOT be in KnownKeys,
			// or every launch would see them missing and regenerate the file.
			// THE TUNED VALUES LIVE HERE, IN CODE, NOT ONLY IN THE CFG.
			//
			// They used to be absent-by-default: no config entry meant the
			// global foregrip, and the global is wrong for every weapon. That
			// held for as long as the deployed cfg carried the tuning -- and
			// then a fresh install regenerated the file on 2026-08-30 and every
			// per-weapon value went with it, silently. The pistol was back to
			// blend 1.0, which points the gun along the hand-to-hand line when a
			// cupped pistol grip's hand-to-hand vector is not the barrel.
			//
			// Measured on hardware 2026-08-23; see *Tuned values* in HANDOVER.md.
			// The config keys still OVERRIDE these, so tuning is unaffected --
			// what changes is that losing the cfg no longer loses the tuning.
			//
			// blend 0 for the magnum is not a placeholder. It means the aim
			// stays on the weapon hand for that weapon, deliberately.
			struct WeaponGrip
			{
				const char* key;
				float forward, right, up, blend;
			};
			static const WeaponGrip kWeapons[] = {
				{ "v_magnum",        -3.0f, 0.0f,  -4.0f, 0.0f },
				{ "v_assault_rifle",  2.0f, 0.0f,  -9.0f, 1.0f },
				{ "v_scattergun",     3.0f, 0.0f, -15.0f, 1.0f },
			};
			for ( const WeaponGrip& w : kWeapons )
			{
				char k[64];
				_snprintf_s( k, sizeof( k ), _TRUNCATE, "two_handed_grip_forward_%s", w.key );
				const float f = g_config.GetFloat( k, w.forward );
				_snprintf_s( k, sizeof( k ), _TRUNCATE, "two_handed_grip_right_%s", w.key );
				const float r = g_config.GetFloat( k, w.right );
				_snprintf_s( k, sizeof( k ), _TRUNCATE, "two_handed_grip_up_%s", w.key );
				const float u = g_config.GetFloat( k, w.up );
				_snprintf_s( k, sizeof( k ), _TRUNCATE, "two_handed_blend_%s", w.key );
				const float bl = g_config.GetFloat( k, w.blend );
				g_twoHanded.SetModelGrip( w.key, f, r, u, bl );
				Log( "grip: %s foregrip fwd=%.1f right=%.1f up=%.1f blend %.2f%s%s",
					 w.key, f, r, u, bl,
					 ( bl <= 0.0f ) ? "  (aim stays on the weapon hand)" : "",
					 ( f == w.forward && r == w.right && u == w.up && bl == w.blend )
						 ? "  [built-in]" : "  [from cfg]" );
			}

			if ( th.enabled || th.debug )
				Log( "grip: two-handed %s -- bring the off hand within %.1fu of the "
						  "foregrip and the gun points along the line between your "
						  "hands. Roll still comes from the weapon wrist. The aim and "
						  "the model both read this, so they cannot diverge.",
						  th.enabled ? "ON" : "measuring only", th.radius );

			// The button modes need saying out loud, because WHICH hand's grip
			// they are on is derived from left_handed rather than written
			// anywhere the player can see, and because one controller family
			// cannot do them at all.
			if ( ( th.enabled || th.debug ) && th.mode != kTwoHandedAuto )
				Log( "grip: mode is %s -- the %s hand's grip %s. That grip is free "
					 "on Rift/Touch, Index and Cosmos because NextWeapon is bound "
					 "on both grips while only the weapon hand's is read. VIVE "
					 "WANDS HAVE NO FREE GRIP -- theirs carry Use and Reload, so "
					 "set two_handed_mode = auto there.",
					 TwoHandedModeName( th.mode ),
					 vrSettings.leftHanded ? "RIGHT" : "LEFT",
					 th.mode == kTwoHandedHold
						 ? "must be HELD to keep hold of the gun"
						 : "TOGGLES the grip on and off" );
		}

		// Hand markers. Bound separately from the laser dot for the same
		// reason the laser is bound separately from the zones: turning one
		// off must not silently take another with it.
		{
			HandMarkerSettings hm;
			// Defaults to `auto`, not `off`. An empty hand with nothing marking
			// it is invisible, and the intro is entirely unarmed -- the markers
			// were reported "lost" from a config that had simply been
			// regenerated with the old default.
			const char* mode = g_config.GetString( "hand_marker", "auto" );
			if ( _stricmp( mode, "both" ) == 0 )
				hm.mode = kHandMarkerBoth;
			else if ( _stricmp( mode, "off_hand" ) == 0 )
				hm.mode = kHandMarkerOffHand;
			else if ( _stricmp( mode, "auto" ) == 0 )
				hm.mode = kHandMarkerAuto;
			else if ( _stricmp( mode, "off" ) != 0 )
			{
				// An unrecognised value used to fall through to `off` in
				// silence, which is indistinguishable from the feature being
				// broken. Say so, and keep the default rather than the silence.
				LogWarn( "hands: hand_marker = \"%s\" is not one of "
						 "off/off_hand/both/auto -- using auto", mode );
				hm.mode = kHandMarkerAuto;
			}
			hm.size = g_config.GetFloat( "hand_marker_size", 0.75f );
			hm.forwardLength =
				g_config.GetFloat( "hand_marker_forward", 6.0f );
			hm.offsetForward =
				g_config.GetFloat( "hand_marker_offset_forward", -1.5f );
			hm.offsetRight =
				g_config.GetFloat( "hand_marker_offset_right", 0.0f );
			hm.offsetUp =
				g_config.GetFloat( "hand_marker_offset_up", 0.0f );
			g_handMarker.SetSettings( hm );

			// The menu pointer binds UNCONDITIONALLY. It sat inside the hand
			// marker's enable block, so turning hand markers off would have
			// silently killed the menu cursor -- two unrelated features sharing
			// one `if` because of an indentation slip. Not the current fault
			// (hand_marker is on) but it would have been someone's.
			g_menuPointer.Bind( GetInterface( "engine.dll", "VDebugOverlay003" ) );

			if ( hm.mode != kHandMarkerOff )
			{
				g_handMarker.Bind( GetInterface( "engine.dll", "VDebugOverlay003" ) );
				Log( "hands: markers ON (%s) -- a box at the hand rotated to the "
						  "controller, plus a whisker along its forward axis. This is "
						  "the instrument for two-handed grip work: the blend cannot "
						  "be tuned while the off hand is invisible.",
						  ( hm.mode == kHandMarkerBoth ) ? "both hands" : "off hand" );
			}
		}

		{
			LaserDotSettings laser;
			laser.enabled = g_config.GetBool( "laser_dot", false );
			laser.perWeapon = g_config.GetBool( "laser_dot_per_weapon", true );
		laser.perWeapon = g_config.GetBool( "laser_dot_per_weapon", true );
			laser.distance = g_config.GetFloat( "laser_dot_distance", 0.0f );
			laser.size = g_config.GetFloat( "laser_dot_size", 1.2f );
			laser.gap = g_config.GetFloat( "laser_dot_gap", 0.35f );
			laser.trace = g_config.GetBool( "laser_dot_trace", true );
			laser.maxDistance = g_config.GetFloat( "laser_dot_max_distance", 4096.0f );
			laser.throughWalls = g_config.GetBool( "laser_dot_through_walls", false );
			laser.submits = g_config.GetInt( "laser_dot_submits", 1 );
			laser.debugTint = g_config.GetBool( "laser_dot_debug_tint", false );

			AimDebugSettings aimDbg;
			aimDbg.enabled = g_config.GetBool( "aim_debug", false );
			aimDbg.length = g_config.GetFloat( "aim_debug_length", 600.0f );
			g_aimDebug.SetSettings( aimDbg );
			g_laserDot.SetSettings( laser );

			if ( laser.enabled )
			{
				g_laserDot.BindTrace( GetInterface( "engine.dll", "EngineTraceClient003" ) );
				if ( g_aimDebug.Enabled() )
					g_aimDebug.Bind( GetInterface( "engine.dll", "VDebugOverlay003" ) );
			}

			// Bound unconditionally, NOT inside the laser's block: head
			// collision must not depend on the crosshair being enabled.
			{
				g_collisionTrace.Bind( GetInterface( "engine.dll", "EngineTraceClient003" ) );
				Log( "6dof collision: %s -- the head %s be stopped by walls",
					 g_collisionTrace.Valid() ? "ON" : "no trace interface",
					 g_collisionTrace.Valid() ? "will" : "will NOT" );
	Stereo().SetPreEyePass( &PreEyePass );
	Stereo().SetPostEyePass( &PostEyePass );
				if ( g_laserDot.Bind( GetInterface( "engine.dll", "VDebugOverlay003" ) ) )
					Log( "laser: dot ON at %s units -- drawn on the ENGINE's eye ray, "
							  "which is where bullets actually come from, not from the "
							  "controller. Below skill 3 autoaim can still bend the shot "
							  "up to 5 degrees off it.",
							  ( laser.distance > 0.0f ) ? "a fixed" : "the convergence" );
				else
					LogWarn( "laser: VDebugOverlay003 unavailable -- no dot" );
			}
		}
		if ( g_zoneAdjust )
			Log( "zones: TUNING ON -- numpad 1 next zone, 7 position/size, "
					  "8/2 fwd/back, 4/6 left/right, 9/3 up/down, . SNAP to "
					  "the weapon hand, -/+ step, 0 dump cfg lines, 5 5 reset" );
	}

	GameInputSettings inputSettings;
	inputSettings.enabled = g_config.GetBool( "controller_input", true );
	{
		const char* dir = g_config.GetString( "movement_direction", "hmd" );
		const bool byController = ( dir && _stricmp( dir, "controller" ) == 0 );
		inputSettings.movementDirection = byController ? kMoveDirController : kMoveDirHmd;
		if ( dir && !byController && _stricmp( dir, "hmd" ) != 0 )
			LogWarn( "movement_direction '%s' not recognised -- using 'hmd'. "
					 "Valid values are 'hmd' and 'controller'.", dir );
	}
	inputSettings.moveDeadzone = g_config.GetFloat( "controller_move_deadzone", 0.35f );
	inputSettings.turnDeadzone = g_config.GetFloat( "controller_turn_deadzone", 0.35f );
	inputSettings.snapTurn = g_config.GetBool( "controller_snap_turn", true );
	inputSettings.snapDegrees = g_config.GetFloat( "controller_snap_degrees", 30.0f );
	inputSettings.turnStickCrouch = g_config.GetBool( "turn_stick_crouch", true );
	inputSettings.turnStickGrenade = g_config.GetBool( "turn_stick_grenade", false );
	inputSettings.turnStickThreshold = g_config.GetFloat( "turn_stick_threshold", 0.65f );

	// ---- WHO OWNS THE OFF HAND'S GRIP -----------------------------------
	//
	// The foregrip has first claim. In `toggle` or `hold` the grip IS the
	// two-handed control, and firing a grenade every time somebody steadies
	// their rifle would be a worse bug than having no grenade button at all.
	//
	// In `auto` -- the default -- the foregrip needs no button and the grip is
	// genuinely idle, so grenades get it. That gives Touch and WMR a real
	// grenade button in the SHIPPED configuration, where before there was none
	// and turn_stick_grenade was the only route to one.
	// Read from the CONFIG, not from the two-handed block that parsed the same
	// key several hundred lines above. That block runs first today, and a value
	// that is only correct because an unrelated line ran first is a trap which
	// re-arms every time the file is edited -- it is exactly how the handedness
	// bore profile silently loaded the wrong hand for two sessions.
	const char* thMode = g_config.GetString( "two_handed_mode", "auto" );
	const bool foregripWantsGrip =
		g_config.GetBool( "two_handed", true ) && thMode &&
		( _stricmp( thMode, "toggle" ) == 0 || _stricmp( thMode, "hold" ) == 0 );

	inputSettings.offHandGrenade =
		g_config.GetBool( "off_hand_grenade", true ) && !foregripWantsGrip;

	Log( "grenade: off-hand grip %s",
		 inputSettings.offHandGrenade
			 ? "throws grenades (two_handed_mode = auto leaves it free)"
			 : ( foregripWantsGrip
					 ? "is the TWO-HANDED grip -- two_handed_mode is not 'auto', "
					   "so grenades need turn_stick_grenade = 1"
					 : "unused (off_hand_grenade = 0)" ) );
	inputSettings.menuDoubleClick = g_config.GetBool( "menu_double_click", true );
	inputSettings.useTrace = g_config.GetBool( "use_trace", false );
	inputSettings.useAimHead = g_config.GetBool( "use_aim_head", true );
	inputSettings.useAimTailSeconds = g_config.GetFloat( "use_aim_tail", 0.20f );
	inputSettings.menuDoubleClickMs =
		(unsigned int)g_config.GetFloat( "menu_double_click_ms", 400.0f );
	inputSettings.smoothDegreesPerSecond =
		g_config.GetFloat( "controller_smooth_turn_speed", 120.0f );
	// Arcade reloading. Read BEFORE GameInput's settings are committed on
	// the next line, because enabling it takes the reload button away from
	// GameInput -- one read, rather than the same key consulted twice in
	// two places that could drift apart.
	//
	// Same calibration shape as melee: with arcade_reload = 0 and
	// arcade_reload_debug = 1 it measures and logs every attempt without
	// ever reloading, and the button keeps working while you tune.
	{
		ArcadeReloadSettings arcade;
		arcade.enabled = g_config.GetBool( "arcade_reload", false );
		arcade.debug = g_config.GetBool( "arcade_reload_debug", false );
		arcade.zone = &g_zones.Get( kZoneReload );
		arcade.dwellSeconds = g_config.GetFloat( "arcade_reload_dwell", 0.12f );
		arcade.maxSpeed = g_config.GetFloat( "arcade_reload_max_speed", 30.0f );
		arcade.rearmAbove =
			g_config.GetFloat( "arcade_reload_rearm_above", 8.0f );
		arcade.cooldown = g_config.GetFloat( "arcade_reload_cooldown", 1.5f );
		arcade.holdSeconds =
			g_config.GetFloat( "arcade_reload_hold_seconds", 0.12f );
		g_arcadeReload.SetSettings( arcade );
		inputSettings.suppressReloadButton = arcade.enabled;

		if ( arcade.enabled || arcade.debug )
			Log( "reload: arcade %s, debug %s -- drop the %s hand to %.0f u below "
					  "the reload box and hold it for %.2fs. Reload button %s.",
					  arcade.enabled ? "ON" : "off (measuring only)",
					  arcade.debug ? "ON" : "off",
					  vrSettings.leftHanded ? "LEFT" : "right",
					  g_zones.Get( kZoneReload ).up, arcade.dwellSeconds,
					  arcade.enabled ? "UNBOUND -- the gesture owns it"
						   : "still active" );
	}

	// Swimming. The engine takes its swim direction from the command
	// angles' pitch, so aim decoupling quietly broke it -- see vr_camera.h.
	{
		const char* mode = g_config.GetString( "swim_pitch", "movement" );
		SwimPitchSource src = kSwimPitchOff;
		if ( _stricmp( mode, "hmd" ) == 0 )
			src = kSwimPitchHmd;
		else if ( _stricmp( mode, "controller" ) == 0 )
			src = kSwimPitchController;
		else if ( _stricmp( mode, "movement" ) == 0 )
		{
			// Follow whatever already steers the player on land. Swimming
			// vertically is a movement question, and movement_direction is
			// already the answer to it -- a second setting could disagree with
			// the first and there would be no principled way to resolve it.
			src = ( inputSettings.movementDirection == kMoveDirController )
				  ? kSwimPitchController : kSwimPitchHmd;
		}
		g_camera.SetSwimPitchSource( src );

		if ( src != kSwimPitchOff )
		{
			// m_nWaterLevel IS a RecvProp, unlike m_vecViewOffset -- but the
			// airboat declares one too, at a different offset, which is why
			// FindAnywhere searches player classes first.
			const char* foundIn = nullptr;
			if ( g_netProps.Init( g_client ) )
			{
				g_waterLevelOffset = g_netProps.Find( "CBasePlayer", "m_nWaterLevel" );
				if ( g_waterLevelOffset >= 0 ) foundIn = "CBasePlayer";
				if ( g_waterLevelOffset < 0 )
					g_waterLevelOffset =
						g_netProps.FindAnywhere( "m_nWaterLevel", &foundIn );
			}

			if ( g_waterLevelOffset >= 0 )
				Log( "swim: vertical swimming ON, steered by the %s -- "
						  "m_nWaterLevel at +0x%X in %s (found by name). Note the "
						  "engine takes ONE pitch for movement AND shooting, so "
						  "while swimming your shots follow the same device.",
						  ( src == kSwimPitchController ) ? "off-hand controller"
							  : "headset",
						  g_waterLevelOffset, foundIn ? foundIn : "?" );
			else
			{
				g_camera.SetSwimPitchSource( kSwimPitchOff );
				LogWarn( "swim: m_nWaterLevel was not found in ANY client class, "
						  "so swimming cannot be detected and the player will sink "
						  "while holding the gun level. Set aim_source = hmd to "
						  "work around it." );
			}
		}
	}

	// Physical crouching.
	{
		PhysicalCrouchSettings crouch;
		crouch.enabled = g_config.GetBool( "physical_crouch", true );
		crouch.debug = g_config.GetBool( "physical_crouch_debug", false );
		crouch.triggerFraction =
			g_config.GetFloat( "physical_crouch_trigger", 0.70f );
		crouch.releaseFraction =
			g_config.GetFloat( "physical_crouch_release", 0.77f );
		crouch.compensate =
			g_config.GetBool( "physical_crouch_compensate", true );

		// Release must be ABOVE trigger or the player can never stand back
		// up -- and a config that cannot be escaped from inside a headset is
		// worth refusing rather than obeying.
		if ( crouch.releaseFraction <= crouch.triggerFraction )
		{
			crouch.releaseFraction = crouch.triggerFraction + 0.05f;
			LogWarn( "physical_crouch_release must be above "
					  "physical_crouch_trigger, or standing up is impossible. "
					  "Using %.2f.", crouch.releaseFraction );
		}
		g_physicalCrouch.SetSettings( crouch );

		if ( crouch.enabled )
		{
			// m_vecViewOffset is how the engine's duck depth is measured rather
			// than assumed. Same by-name lookup as m_vecPunchAngle, and the same
			// reason: a game update that moves the field turns a hardcoded
			// offset into a read of whatever now lives there.
			// The prop name is m_vecViewOffset[2], NOT m_vecViewOffset.
			//
			// Source networks the view offset as three separate FLOATS --
			// c_baseplayer.cpp sends m_vecViewOffset[0..2] individually into
			// DT_LocalPlayerExclusive -- so there is no vector prop of that name
			// to find. The first version looked for one, missed, and disabled
			// the compensation with a warning; on hardware that presented as
			// "the crouch still doubles", which is the symptom the whole
			// feature exists to prevent.
			//
			// Only [2] is read, because only the height matters here.
			const char* foundIn = nullptr;
			if ( g_netProps.Init( g_client ) )
			{
				// By class first, then anywhere. Guessing the network class name
				// is what failed before -- three plausible names, all wrong --
				// and the class is not the interesting part of the question.
				g_viewOffsetOffset = g_netProps.Find( "CBasePlayer", "m_vecViewOffset[2]" );
				if ( g_viewOffsetOffset >= 0 ) foundIn = "CBasePlayer";
				if ( g_viewOffsetOffset < 0 )
					g_viewOffsetOffset =
						g_netProps.FindAnywhere( "m_vecViewOffset[2]", &foundIn );
			}

			if ( g_viewOffsetOffset >= 0 )
				Log( "crouch: physical crouching ON -- below %.0f%% of standing "
						  "height ducks, above %.0f%% stands. m_vecViewOffset[2] at "
						  "+0x%X in %s (found by name), so the engine's duck is "
						  "cancelled in the view and you are not lowered twice.",
						  crouch.triggerFraction * 100.0f,
						  crouch.releaseFraction * 100.0f, g_viewOffsetOffset,
						  foundIn ? foundIn : "?" );
			else if ( crouch.compensate )
				LogWarn( "crouch: m_vecViewOffset[2] was not found in ANY of the "
						  "registered client classes, so the engine's duck cannot "
						  "be cancelled and crouching will lower the view TWICE. "
						  "That is a real change in the game's network tables, not "
						  "a wrong class name -- every class was searched." );

			Log( "crouch: recentre while STANDING -- standing height is taken "
					  "at the recentre, so recentring in a chair calibrates a "
					  "seated player as standing and nothing will ever trigger." );
		}
	}

	g_input.SetSettings( inputSettings );


	// Holster zones. Reach to a place on your body and grip to draw.
	{
		HolsterSettings h;
		h.enabled = g_config.GetBool( "holster_zones", true );
		h.debug = g_config.GetBool( "holster_debug", false );
		h.leftSlot = g_config.GetInt( "holster_left_slot", 3 );
		h.rightSlot = g_config.GetInt( "holster_right_slot", 2 );
		h.hipSlot = g_config.GetInt( "holster_hip_slot", 1 );
		h.gripCycles = g_config.GetBool( "holster_grip_cycles", false );
		h.drawSound = g_config.GetString( "holster_draw_sound",
										  "Player.WeaponSelected" );
		// Rejected LOUDLY rather than dropped silently. This value is pasted
		// into a console command, so it is whitelisted at the point of use --
		// but a value that is simply ignored, with the draw sound quietly gone,
		// is the kind of silent failure this project keeps paying for. Say
		// which value was refused and what is being used instead.
		if ( !IsSafeSoundName( h.drawSound ) )
		{
			LogWarn( "holster_draw_sound = \"%s\" REFUSED -- a sound name may "
					 "only contain letters, digits, '.', '_', '-' and '/'. This "
					 "string is pasted into an engine console command, and "
					 "Source splits commands on ';' and newline, so anything "
					 "else would be command injection. Falling back to the "
					 "default.", h.drawSound );
			h.drawSound = "Player.WeaponSelected";
		}
		h.hip = &g_zones.Get( kZoneHolsterHip );
		h.left = &g_zones.Get( kZoneHolsterLeft );
		h.right = &g_zones.Get( kZoneHolsterRight );
		g_holsters.SetSettings( h );
		g_input.SetHolsterZones( &g_holsters );
		if ( h.enabled )
			Log( "holster zones: ON -- grip the weapon hand at your left for slot%d, "
				"right for slot%d, hip for slot%d. Grip anywhere else %s.",
				 h.leftSlot, h.rightSlot, h.hipSlot,
				 h.gripCycles ? "still cycles weapons"
							  : "does NOTHING (holster_grip_cycles = 0)" );
	}
	vrSettings.controllerInput = inputSettings.enabled;

	// Melee by gesture. Deliberately readable with the feature OFF: with
	// melee_debug = 1 it measures and logs every candidate swing without
	// issuing anything, which is how the thresholds get set from real numbers
	// instead of guessed at. See melee_gesture.h.
	{
		MeleeGestureSettings melee;
		melee.enabled = g_config.GetBool( "melee_gesture", true );
		melee.aimForward = g_config.GetBool( "melee_aim_forward", true );
		melee.aimTailSeconds = g_config.GetFloat( "melee_aim_tail", 0.25f );
		melee.debug = g_config.GetBool( "melee_debug", false );
		melee.minSpeed = g_config.GetFloat( "melee_min_speed", 55.0f );
		melee.minTravel = g_config.GetFloat( "melee_min_travel", 12.0f );
		melee.startZone = &g_zones.Get( kZoneMeleeStart );
		melee.minDownness = g_config.GetFloat( "melee_min_downness", 0.55f );
		melee.minInward = g_config.GetFloat( "melee_min_inward", 0.0f );
		melee.maxDuration = g_config.GetFloat( "melee_max_duration", 0.7f );
		melee.cooldown = g_config.GetFloat( "melee_cooldown", 0.8f );
		melee.holdSeconds = g_config.GetFloat( "melee_hold_seconds", 0.12f );
		g_melee.SetSettings( melee );

		if ( melee.enabled || melee.debug )
			Log( "melee: gesture %s, debug %s -- swing the %s hand down from head "
				 "height (>= %.0f u/s, >= %.0f units, starting inside the melee "
				 "start box)",
				 melee.enabled ? "ON" : "off (measuring only)",
				 melee.debug ? "ON" : "off",
				 vrSettings.leftHanded ? "LEFT" : "right",
				 melee.minSpeed, melee.minTravel );
	}

	g_vr = CreateOpenVRBackend();
	if ( g_vr->Init( vrSettings ) )
	{
		Log( "VR backend '%s' ready", g_vr->Name() );
	}
	else
	{
		LogError( "VR backend unavailable: %s", g_vr->LastError() );
		Log( "continuing without head tracking" );
	}

	void* original = g_viewRenderHook.Install( g_client, client_slot::kView_Render,
											   &Detour_View_Render );
	if ( original )
		Log( "Hooked IBaseClientDLL::View_Render (slot %d), original=%p",
			 client_slot::kView_Render, original );
	else
		Log( "!! failed to hook View_Render" );

	// Stereo. Must be up before the Present hook so the first submitted frame
	// already has eye surfaces to use.
	if ( g_vr->IsReady() && g_config.GetBool( "stereo", true ) )
	{
		StereoSettings stereoSettings;
		stereoSettings.perEyeProjection = g_config.GetBool( "stereo_projection", true );
		stereoSettings.matchViewmodelFov = g_config.GetBool( "viewmodel_fov_match", true );

		// ONE key drives BOTH halves -- the near/far match here and the slab
		// widening in draw_probe.h. They are not independently useful and
		// enabling either alone is wrong; see viewmodelDepth in stereo.h.
		{
			const bool vmDepth = g_config.GetBool( "viewmodel_depth", true );
			stereoSettings.viewmodelDepth = vmDepth;
			Probe().SetViewmodelDepth( vmDepth );
		}
		g_menuGating = g_config.GetBool( "menu_gating", true );

	{
		// ---- resolution, decided by the LAUNCHER ---------------------
		//
		// Read here only so the mod can report whether it worked. Acting on
		// these is impossible from inside the process: the eye surfaces are
		// created to match the backbuffer, so the window size IS the render
		// resolution and it was fixed before this code existed.
		//
		// They are in KnownKeys because they are real, documented settings that
		// belong in this file -- the audit's "dead config" check would flag them
		// otherwise, and the honest answer to that is that they ARE read, just
		// for reporting rather than for effect.
		// ---- what a scripted camera is allowed to do ---------------------
		{
			const char* cs = g_config.GetString( "cutscene_camera", "head" );
			const bool follow = ( cs && _stricmp( cs, "engine" ) == 0 );
			g_camera.SetCutsceneFollow( follow );
			g_camera.SetCutsceneRecenter(
				g_config.GetBool( "cutscene_recenter", true ) && !follow );
			g_camera.SetFrozenLatch(
				g_config.GetBool( "cutscene_frozen_latch", true ) );
			if ( g_vr )
				g_vr->SetUseCompositorPoses(
					g_config.GetBool( "compositor_poses", true ) );
			g_camera.SetRecoilViewLock(
				g_config.GetBool( "recoil_view_lock", true ) );
			g_camera.SetRecoilLockMaxDegrees(
				g_config.GetFloat( "recoil_view_lock_max_degrees", 3.0f ) );
			g_recoilLockTailMs =
				g_config.GetFloat( "recoil_view_lock_tail_ms", 250.0f );
			g_camera.SetRotationTrace(
				g_config.GetBool( "rotation_trace", false ),
				(int)g_config.GetFloat( "rotation_trace_frames", 900.0f ) );
			g_mapTurnEnabled =
				g_config.GetBool( "map_turn_to_face", false ) && !follow;
			g_mapTurnDelayFrames =
				(int)g_config.GetFloat( "map_turn_delay_frames", 45.0f );
			g_mapTurnMinDegrees =
				g_config.GetFloat( "map_turn_min_degrees", 25.0f );
			g_mapTurnOnTeleport =
				g_config.GetBool( "map_turn_on_teleport", true ) && !follow;
			g_mapTurnTeleportUnits =
				g_config.GetFloat( "map_turn_teleport_units", 400.0f );
			g_hudAnchorTransitionMs =
				g_config.GetFloat( "hud_anchor_transition_ms", 2500.0f );
			if ( g_mapTurnDelayFrames < 1 )
				g_mapTurnDelayFrames = 1;

			LookArrowSettings la;
			la.enabled = g_config.GetBool( "cutscene_look_arrow", true ) && !follow;
			la.fovFraction = g_config.GetFloat( "cutscene_look_arrow_fov", 0.85f );
			la.hideDegrees = g_config.GetFloat( "cutscene_look_arrow_hide", 25.0f );
			la.fadeDegrees = g_config.GetFloat( "cutscene_look_arrow_fade", 60.0f );
			la.size = g_config.GetInt( "cutscene_look_arrow_size", 80 );
			la.pulseHz = g_config.GetFloat( "cutscene_look_arrow_pulse", 1.4f );
			g_lookArrow.SetSettings( la );
			if ( cs && !follow && _stricmp( cs, "head" ) != 0 )
				LogWarn( "cutscene_camera '%s' not recognised -- using 'head'. "
						 "Valid values are 'head' and 'engine'.", cs );


			Log( "cutscene: a scripted camera %s. Turn-to-face %s, look arrow %s.",
				 follow ? "MOVES your view, as the flat game does"
						: "is IGNORED -- your head keeps the view",
				 ( g_config.GetBool( "cutscene_recenter", true ) && !follow )
					 ? "on" : "off",
				 la.enabled ? "on" : "off" );
		}

		// ---- hotkey modifiers -------------------------------------------
		{
			const char* m = g_config.GetString( "hotkey_modifier", "ctrl_alt" );
			int mods = kModCtrl | kModAlt;
			if ( m && _stricmp( m, "none" ) == 0 )         mods = kModNone;
			else if ( m && _stricmp( m, "ctrl" ) == 0 )    mods = kModCtrl;
			else if ( m && _stricmp( m, "alt" ) == 0 )     mods = kModAlt;
			else if ( m && _stricmp( m, "ctrl_shift" ) == 0 ) mods = kModCtrl | kModShift;
			else if ( m && _stricmp( m, "ctrl_alt" ) != 0 )
				LogWarn( "hotkey_modifier '%s' not recognised -- using 'ctrl_alt'. "
						 "Valid values are ctrl_alt, ctrl, alt, ctrl_shift, none.", m );

			g_keyRecenter.SetMods( mods );
			g_keyToggle.SetMods( mods );
			g_keyRoll.SetMods( mods );

			Log( "hotkeys: %sF9 recentre | %sF10 VR camera on/off | %sF11 roll "
				 "on/off. Modifiers must match EXACTLY and the game must be "
				 "focused, so a recorder bound to F10 or Ctrl+F11 no longer "
				 "trips them.%s",
				 HotKeyModName( mods ), HotKeyModName( mods ), HotKeyModName( mods ),
				 mods == kModNone
					 ? "  <-- hotkey_modifier = none: bare F9/F10/F11, which WILL "
					   "collide with recording software" : "" );
		}

		g_camera.SetCollideOffset( g_config.GetBool( "positional_collide", true ) );
		g_camera.SetCollisionRadius( g_config.GetFloat( "positional_collide_radius", 6.0f ) );
		g_camera.SetCollisionBackoff( g_config.GetFloat( "positional_collide_backoff", 0.05f ) );

		g_autoResolution = g_config.GetBool( "vr_auto_resolution", true );
		g_resolutionScale = g_config.GetFloat( "vr_resolution_scale", 1.0f );
		g_allowOversizeWindow = g_config.GetBool( "vr_allow_oversize_window", true );
		g_maxRenderHeight = g_config.GetFloat( "vr_max_render_height", 0.0f );

		// The desktop mirror is sized independently of the render. Applied on
		// the first Present, once there is a backbuffer to size against.
		//
		// On by default since 2026-09-13: confirmed on hardware with the desktop
		// at 1080p, the game window hidden, and the menu pointer reaching every
		// button past the monitor's edge.
		ConfigureDesktopWindow( g_config.GetBool( "vr_desktop_window_fit", true ),
				g_config.GetInt( "vr_desktop_window_height", 0 ),
				g_config.GetBool( "vr_desktop_window_hide", true ),
				g_config.GetBool( "vr_game_window_centred", true ) );

		// ---- the D3D9 menu cursor ------------------------------------
		//
		// Read BEFORE the pointer settings, because it decides whether the
		// pointer's own overlay marker is wanted at all. Two cursors on the
		// main menu -- one of which vanishes when paused -- would be worse
		// than either alone.
		{
			MenuCursorSettings mc;
			mc.enabled = g_config.GetBool( "menu_cursor_d3d9", true );
			mc.size = g_config.GetInt( "menu_cursor_size", 16 );
			mc.thickness = g_config.GetInt( "menu_cursor_thickness", 4 );
			mc.outline = g_config.GetInt( "menu_cursor_outline", 2 );
			mc.r = g_config.GetInt( "menu_cursor_r", 255 );
			mc.g = g_config.GetInt( "menu_cursor_g", 240 );
			mc.b = g_config.GetInt( "menu_cursor_b", 64 );
			g_menuCursor.SetSettings( mc );
		}

		MenuPointerSettings mp;
		mp.enabled = g_config.GetBool( "menu_pointer", true );
		mp.useOffHand = g_config.GetBool( "menu_pointer_off_hand", false );
		mp.click = g_config.GetBool( "menu_pointer_click", true );
		mp.pitch = g_config.GetFloat( "menu_pointer_pitch", 58.0f );
		mp.yaw = g_config.GetFloat( "menu_pointer_yaw", 0.0f );
		mp.smoothing = g_config.GetFloat( "menu_pointer_smoothing", 0.35f );
		mp.clickDebounceMs = g_config.GetInt( "menu_click_debounce_ms", 100 );
		mp.direct = g_config.GetBool( "menu_pointer_direct", true );
		mp.marker = g_config.GetBool( "menu_pointer_marker", true );
		mp.markerDistance = g_config.GetFloat( "menu_pointer_marker_distance", 120.0f );
		mp.markerSize = g_config.GetFloat( "menu_pointer_marker_size", 3.0f );
		// ONE number, shared with the renderer -- see menu_scale.
		mp.menuScale = g_config.GetFloat( "menu_scale", 0.6f );
		mp.screenMarker = g_config.GetBool( "menu_pointer_screen_marker", false );
		mp.debug = g_config.GetBool( "menu_pointer_debug", false );
		// ---- ONE CURSOR, NOT TWO ---------------------------------------
		//
		// The overlay cross and the D3D9 cross draw the same point by different
		// routes. With both on, the main menu shows two cursors and a pause
		// menu shows one, which is a confusing way to discover that the overlay
		// one does not work when paused.
		//
		// So the D3D9 cursor, when enabled, owns the job outright. Set
		// menu_cursor_d3d9 = 0 to hand it back rather than turning the marker
		// on alongside.
		if ( g_menuCursor.Settings().enabled && mp.marker )
		{
			mp.marker = false;
			Log( "menu cursor: the D3D9 cursor is on, so the world-space overlay "
				 "cross is suppressed -- one cursor, not two. menu_cursor_d3d9 = 0 "
				 "returns to the overlay, which cannot be seen in a pause menu." );
		}

		g_menuPointer.SetSettings( mp );
	}
		stereoSettings.eyeSeparationScale = g_config.GetFloat( "eye_separation_scale", 1.0f );
		stereoSettings.liveAdjust = g_config.GetBool( "eye_adjust_enabled", false );
		stereoSettings.initialAdjust[kEyeLeft].x = g_config.GetFloat( "left_eye_offset_x", 0.0f );
		stereoSettings.initialAdjust[kEyeLeft].y = g_config.GetFloat( "left_eye_offset_y", 0.0f );
		stereoSettings.initialAdjust[kEyeRight].x = g_config.GetFloat( "right_eye_offset_x", 0.0f );
		stereoSettings.initialAdjust[kEyeRight].y = g_config.GetFloat( "right_eye_offset_y", 0.0f );
		stereoSettings.hudConvergence = g_config.GetFloat( "hud_convergence", 1.0f );
		stereoSettings.menuConvergence = g_config.GetFloat( "menu_convergence", 1.0f );
		stereoSettings.menuScale = g_config.GetFloat( "menu_scale", 0.6f );
		stereoSettings.menuDepthScale = g_config.GetFloat( "menu_depth_scale", 2.0f );
		stereoSettings.menuDepthStep = g_config.GetFloat( "menu_depth_step", 0.05f );
		stereoSettings.menuAnchor = g_config.GetBool( "menu_anchor", true );
		stereoSettings.menuAnchorDistance = g_config.GetFloat( "menu_anchor_distance", 150.0f );
		g_menuPanelSettings.enabled = g_config.GetBool( "menu_world_panel", true );
		g_menuPanelSettings.distance = g_config.GetFloat( "menu_anchor_distance", 150.0f );
		g_menuPanelSettings.yawOffset = g_config.GetFloat( "menu_anchor_yaw", -12.0f );
		g_menuPanelSettings.heightOffset = g_config.GetFloat( "menu_anchor_height", 0.0f );
		g_menuPanelSettings.width = g_config.GetFloat( "menu_panel_width", 190.0f );
		g_menuPanelSettings.billboard = g_config.GetBool( "menu_panel_billboard", false );
		g_menuAlignToScene = g_config.GetBool( "menu_panel_align_scene", true );
		g_menuRepinMs =
			(unsigned int)g_config.GetFloat( "menu_panel_repin_ms", 750.0f );
		g_menuPanelSettings.pitch = g_config.GetFloat( "menu_panel_pitch", 0.0f );
		g_menuPanelSettings.roll = g_config.GetFloat( "menu_panel_roll", 0.0f );
		g_menuPanelAdjust = g_config.GetBool( "menu_panel_adjust", false );
		stereoSettings.freezeTime = g_config.GetBool( "freeze_time_across_eyes", false );
		stereoSettings.skipHudShiftInMenus = g_config.GetBool( "hud_shift_skip_in_menus", true );
		stereoSettings.tracePasses = g_config.GetInt( "trace_eye_passes", 2 );
		stereoSettings.relaxCulling = g_config.GetBool( "relax_frustum_culling", true );
		stereoSettings.relaxFrustum = g_config.GetBool( "relax_cull_frustum", true );
		stereoSettings.relaxArea = g_config.GetBool( "relax_cull_area", true );
		// Default off: the PVS is computed from the view origin, so it cannot
		// explain anything that changes as the head turns, and forcing it true
		// draws entities standing in rooms the player cannot see.
		stereoSettings.relaxPvs = g_config.GetBool( "relax_cull_pvs", false );
		stereoSettings.relaxOcclusion = g_config.GetBool( "relax_cull_occlusion", true );

		// Level 2+: stop overriding the engine's cull decisions. See the block
		// beside engine_portals_open_all for why the level wins outright.
		if ( g_perfLevel >= 2 )
		{
			stereoSettings.relaxCulling = false;
			stereoSettings.relaxFrustum = false;
			stereoSettings.relaxArea = false;
			stereoSettings.relaxOcclusion = false;
		}
		// 1.05 rather than 1.15: the coverage report measured 15% spare
		// horizontally and 33% vertically at 1.15, and frustum cost goes as the
		// square of the tangent.
		stereoSettings.engineFovMargin = g_config.GetFloat( "engine_fov_margin", 1.05f );
		if ( g_perfLevel >= 1 )
			stereoSettings.engineFovMargin = 1.0f;
		stereoSettings.ownViewSetup = g_config.GetBool( "view_setup_ownership", true );
		stereoSettings.slowFrameMs = g_config.GetFloat( "log_slow_frame_ms", 500.0f );

		void* matsysForStereo = GetInterface( "materialsystem.dll", "VMaterialSystem076" );
		if ( !Stereo().Init( matsysForStereo, g_engine.Raw(), g_vr, stereoSettings ) )
		{
			LogError( "stereo unavailable -- falling back to mono submission" );
		}
		else
		{
			// Needs the View_Render hook to be in place: the `view` global is read
			// out of that function's code rather than hard-coded to an RVA.
			Stereo().SetViewBindForcedRetries(
			g_config.GetInt( "view_bind_force_retries", 0 ) );
		Stereo().BindViewSetup( g_viewRenderHook.Original() );

		{
			const char* anchor = g_config.GetString( "hud_anchor", "body" );
			const bool body = ( anchor && _stricmp( anchor, "body" ) == 0 );
			Stereo().SetHudAnchored( body );
			Stereo().SetSkipRtProjections(
				g_config.GetBool( "stereo_skip_rt_projections", true ) );
			Stereo().SetRtAspectTolerance(
				g_config.GetFloat( "stereo_rt_aspect_tolerance", 0.02f ) );
			Stereo().SetMaterialTrace(
				g_config.GetBool( "hud_material_trace", false ) );
			Stereo().SetFullscreenFadeCalls(
				(int)g_config.GetFloat( "hud_anchor_fade_calls", 0.0f ) );
			Stereo().SetHudClamp( g_config.GetBool( "hud_anchor_clamp", true ) );
			Stereo().SetHudScale( g_config.GetFloat( "hud_anchor_scale", 1.0f ) );
			Stereo().SetHudInvert(
				g_config.GetBool( "hud_anchor_invert_x", false ),
				g_config.GetBool( "hud_anchor_invert_y", false ) );
			// Placed in the headset 2026-09-02 and kept. Further forward than the
			// old 55, so it converges at a comfortable depth, and to the RIGHT
			// rather than the left -- the sign is the difference between the
			// panel sitting over the weapon hand and sitting clear of it.
			g_hudAnchorForward = g_config.GetFloat( "hud_anchor_forward", 75.0f );
			g_hudAnchorRight = g_config.GetFloat( "hud_anchor_right", 25.0f );
			g_hudAnchorUp = g_config.GetFloat( "hud_anchor_up", -10.0f );
			g_hudAnchorFollowYaw = g_config.GetBool( "hud_anchor_follow_yaw", true );
			if ( body )
				Log( "hud: anchored at forward=%.0f right=%.0f up=%.0f, following %s yaw and ignoring pitch -- so it sits below your view until you look down. Projected per eye, so it converges at that distance rather than at infinity.",
					 g_hudAnchorForward, g_hudAnchorRight, g_hudAnchorUp,
					 g_hudAnchorFollowYaw ? "HEAD" : "body" );
			else if ( anchor && _stricmp( anchor, "off" ) != 0 )
				LogWarn( "hud_anchor '%s' not recognised -- using 'off'. Valid values are 'off' and 'body'.", anchor );
		}
		}
	}
	else
	{
		Log( "stereo disabled by config" );
	}

	// Frame submission. Separate from the camera on purpose: if the headset
	// stays black we need to know whether the fault is here or in the pose path.
	if ( g_vr->IsReady() && submitFrames )
	{
		if ( !InstallD3D9PresentHook( g_vr, true ) )
			LogError( "frame submission unavailable -- the headset will stay black" );
	}
	else
	{
		Log( "frame submission disabled (submit_frames=%d, vr ready=%d)",
			 submitFrames ? 1 : 0, g_vr->IsReady() ? 1 : 0 );
	}

	Log( "Ready. F9 recentre | F10 toggle VR camera | F11 toggle roll" );

	// Heartbeat: distinguishes a stalled hook from a stalled game, and gives a
	// pose sample to sanity-check the conversion against how it felt.
	if ( g_heartbeatSeconds <= 0 )
	{
		Log( "heartbeat disabled via config" );
		return 0;
	}

	unsigned long long last = 0;
	int stalledTicks = 0;
	bool stallReported = false;
	unsigned long long worstLargestFreeMB = ~0ull;
	bool addressSpaceWarned = false;

	while ( true )
	{
		Sleep( g_heartbeatSeconds * 1000 );

		// Hang watchdog. A GPU timeout raises no exception -- the process stays
		// alive, this thread keeps ticking, and only the frame counter stops.
		// That is what the last failure looked like, so it gets its own report.
		if ( g_frames == last && g_engine.Valid() )
		{
			if ( ++stalledTicks >= 2 && !stallReported )
			{
				stallReported = true;
				CrashLog( "=== RENDER STALL ===" );
				CrashLog( "no frames for ~%d seconds, process still alive",
						  stalledTicks * g_heartbeatSeconds );
				CrashLog( "frames=%llu present=%llu vrIface=%d ingame=%d",
						  g_frames, D3D9PresentCount(), D3D9VRInterfaceBound() ? 1 : 0,
						  g_engine.IsInGame() ? 1 : 0 );
				CrashLog( "typical cause: GPU timeout (TDR). Check the DXVK log for "
						  "VK_ERROR_DEVICE_LOST and Windows for LiveKernelEvent 0x141." );

				// The whole point: find out *where* it is wedged. A hang raises
				// no exception, so the only way to know is to suspend the thread
				// and read its stack.
				CrashLog( "--- stuck thread stacks ---" );
				LogThreadStack( g_renderThreadId, "render thread (View_Render)" );

				DWORD presentTid = (DWORD)D3D9PresentThreadId();
				if ( presentTid != g_renderThreadId )
					LogThreadStack( presentTid, "present thread" );

				// What the mod was calling into, if anything, and what led up to it.
				if ( ExternalCall() )
					CrashLog( "OpenVR call in progress: %s", ExternalCall() );
				CrashLog( "--- last events before the stall ---" );
				DumpBreadcrumbs();

				CrashLog( "=== END RENDER STALL ===" );
			}
		}
		else
		{
			stalledTicks = 0;
			stallReported = false;
		}

		if ( g_vr && g_vr->IsReady() && g_vr->Hmd().valid )
		{
			const HmdPose& p = g_vr->Hmd();
			Log( "heartbeat: %llu frames (+%llu) present=%llu vrIface=%d ingame=%d vr=%d | "
				 "hmd ang=(%.1f %.1f %.1f) pos=(%.1f %.1f %.1f)",
				 g_frames, g_frames - last, D3D9PresentCount(),
				 D3D9VRInterfaceBound() ? 1 : 0,
				 g_engine.IsInGame() ? 1 : 0, g_vrCameraEnabled ? 1 : 0,
				 p.angles.x, p.angles.y, p.angles.z,
				 p.position.x, p.position.y, p.position.z );
		}
		else
		{
			Log( "heartbeat: %llu frames (+%llu) ingame=%d | no valid HMD pose",
				 g_frames, g_frames - last, g_engine.IsInGame() ? 1 : 0 );
		}

		// Submission safety: headset activity, scene focus, caught faults and
		// the compositor's frame counts. Cached by the render thread -- this
		// thread never calls into the runtime.
		if ( g_vr && g_vr->IsReady() )
			g_vr->LogSubmitSafety();

		// Draw-call measurement for the save/load z-fighting work. Reports the
		// LAST INTERVAL only and then resets, because the experiment is a
		// comparison between a heartbeat with a dialog open and one without,
		// and a cumulative counter cannot answer "is it happening now".
		Probe().Report();

		// Aim convergence readback. "measured" vs "fixed" is the whole
		// question: a convergence permanently stuck at the fallback means the
		// barrel trace is finding nothing and the shot is still only exact at
		// one range, which from inside the headset is indistinguishable from
		// the feature working.
		// Any line the logger could not write. Must be ZERO -- a non-zero count
		// means measurements in this run are incomplete, and an incomplete log
		// reads exactly like a complete one.
		if ( LogDropped() > 0 )
			LogWarn( "log: %u line(s) were DROPPED -- something outside this "
					 "process is holding sinvr.log. Any counts above may be short.",
					 LogDropped() );

		Log( "aim: convergence %.0f units (%s) | barrel trace %s",
			 g_camera.LastConvergenceUsed(),
			 g_camera.LastConvergenceMeasured() ? "MEASURED" : "fixed fallback",
			 g_camera.TracedAimDistance() > 0.0f ? "hit" : "no hit" );

		// 6DoF readback. "positional_tracking = 1" in the settings block is the
		// intent; this is the effect. A permanently zero offset with tracking on
		// means the pose is not moving or the reference was never taken, and a
		// permanently clamped one means the reference is stale or the scale is
		// wrong -- both feel like "6DoF is broken" from inside the headset and
		// are indistinguishable without the number.
		{
			const Vector& p = g_camera.PositionalOffset();
			const float len = sqrtf( p.x * p.x + p.y * p.y + p.z * p.z );
			Log( "6dof: %s offset=(%.1f %.1f %.1f) |%.1f| units%s",
				 g_camera.PositionalTracking() ? "on" : "OFF",
				 p.x, p.y, p.z, len,
				 g_camera.PositionalClamped() ? "  <- CLAMPED at positional_max_offset" : "" );
		}

		// Controller readback. `valid=0` separates "no controllers / no manifest"
		// from "controllers are fine, the bindings are wrong" -- which look
		// identical from inside the headset and have completely different fixes.
		if ( g_vr && g_input.Settings().enabled )
		{
			const VRInputState& in = g_vr->Input();
			Log( "input: valid=%d move=(%.2f %.2f) turn=(%.2f %.2f) "
				 "held[atk=%d alt=%d jmp=%d use=%d rld=%d duck=%d] cmds=%u turn=%s",
				 in.valid ? 1 : 0, in.moveX, in.moveY, in.turnX, in.turnY,
				 in.attack ? 1 : 0, in.attack2 ? 1 : 0, in.jump ? 1 : 0,
				 in.use ? 1 : 0, in.reload ? 1 : 0, in.crouch ? 1 : 0,
				 g_input.CommandsIssued(),
				 g_input.Settings().snapTurn ? "snap" : "smooth" );
			g_inputClock.LogState();
			if ( g_vr->IsLeftHanded() )
				Log( "input: LEFT-HANDED -- trigger/stick/button pairs mirrored, so "
					 "the reported held[] flags are already in game terms, not "
					 "physical-hand terms" );

			// Which physical thumb ends up doing what. The two settings XOR, so
			// neither alone tells you -- and the clicks can differ from the
			// sticks when the bindings could not be read.
			Log( "sticks: left_handed=%d swap_thumbsticks=%d -> move on %s stick, "
					  "turn on %s stick | clicks %s (L=%s R=%s)",
					  g_vr->IsLeftHanded() ? 1 : 0,
					  g_swapThumbsticks ? 1 : 0,
					  g_vr->ThumbsticksSwapped() ? "RIGHT" : "LEFT",
					  g_vr->ThumbsticksSwapped() ? "LEFT" : "RIGHT",
					  g_vr->StickClicksSwapped()
						  ? "swapped with them"
						  : ( g_vr->ThumbsticksSwapped()
							  ? "NOT swapped -- bindings put no click on both sticks"
							  : "unswapped" ),
					  g_vr->StickClickName( kHandLeft ),
					  g_vr->StickClickName( kHandRight ) );

			// First presses climbing with opens at zero means the button works
			// and the SECOND click is missing the window -- which is a very
			// different fix from the button never arriving at all.
			Log( "menu button: %u first press(es), %u opened | %s | a single click "
				 "is ignored on purpose: opening the menu mid-firefight is worse "
				 "than any other misfire",
				 g_input.MenuFirstPresses(), g_input.MenuOpens(),
				 g_input.Settings().menuDoubleClick
					 ? "DOUBLE click required" : "single click (menu_double_click = 0)" );

			// Hand poses. valid=0 on a hand means that controller is off or
			// asleep -- which is also why controller-relative movement silently
			// falls back to head-relative, so it needs to be visible.
			const ControllerPose& lh = g_vr->Controller( kHandLeft );
			const ControllerPose& rh = g_vr->Controller( kHandRight );
			// `offset` is what the movement stick is rotated by, and it now comes
			// from VRCamera::AimYawOffset(): the head's view yaw minus the yaw
			// handed to the engine. It should equal (view.y - engine.y) on the
			// `aim:` line below, to within rounding. If those two ever drift
			// apart, movement has stopped following the head and the
			// two-routes-to-one-quantity bug is back.
			Log( "hands: %s | L valid=%d ang=(%.0f %.0f %.0f) pos=(%.1f %.1f %.1f)"
				 " | R valid=%d ang=(%.0f %.0f %.0f) pos=(%.1f %.1f %.1f)"
				 " | movedir=%s offset=%.0f deg (from the camera's own aim --"
				 " cross-check against view.y-engine.y on the aim: line)",
				 g_vr->IsLeftHanded() ? "LEFT-handed (weapon=L)" : "right-handed (weapon=R)",
				 lh.valid ? 1 : 0, lh.angles.x, lh.angles.y, lh.angles.z,
				 lh.position.x, lh.position.y, lh.position.z,
				 rh.valid ? 1 : 0, rh.angles.x, rh.angles.y, rh.angles.z,
				 rh.position.x, rh.position.y, rh.position.z,
				 g_input.Settings().movementDirection == kMoveDirController
					 ? "controller" : "hmd",
				 g_input.LastMoveYawOffset() );

			// aim=controller in the settings block is the intent; this is the
			// effect. They differ whenever the weapon hand loses tracking, and
			// that difference is invisible from inside the headset.
		g_viewModel.LogState( g_vr );
		g_viewModelAnim.LogState();
		g_holsters.LogState();
		g_zones.LogState( g_vr );
		g_laserDot.LogState();
		g_handMarker.LogState( g_vr );
		g_twoHanded.LogState();
		if ( Stereo().HudAnchored() )
			Log( "hud: anchored placements=%u last screen fraction=(%.2f %.2f) -- 0.5,0.5 is dead centre of the view; the panel should sit low and to one side and hold still as you turn your head",
				 Stereo().HudPlacements(), Stereo().HudFx(), Stereo().HudFy() );

			// Menu state, reported as effect. `ingame=1 menu=1` is the case that
			// broke everything and reads as a contradiction until you know the
			// main menu runs a background map -- so the line says so outright.
			// ---- is the game PAUSED? -------------------------------------
			//
			// This is the question the cursor turns on, and it separates two
			// states the mod otherwise cannot tell apart: the MAIN menu runs a
			// background map with time advancing, while a PAUSE menu freezes it.
			//
			// The evidence that made it worth measuring is a controlled
			// comparison the player made without meaning to: the same cursor,
			// submitted continuously, was INVISIBLE while paused and VISIBLE the
			// moment the menu closed. Same submission, same position, different
			// pause state -- which points at the engine not drawing its overlay
			// list while time is stopped, not at anything in our code.
			//
			// A frozen figure here while `menu: UP` says exactly that.
			{
				static float lastMenuTime = -1.0f;
				const float t = g_engine.Valid() ? g_engine.Time() : 0.0f;
				const bool frozen = ( lastMenuTime >= 0.0f ) &&
									( t - lastMenuTime ) < 0.001f &&
									( t - lastMenuTime ) > -0.001f;
				Log( "menu time: engine clock %.2f (%s)%s", t,
					 frozen ? "FROZEN -- the game is paused" : "advancing",
					 ( frozen && g_camera.UiMode() )
						 ? " | debug overlays are not drawn while time is stopped,"
						   " which is why the world-space cursor vanishes here but"
						   " shows on the main menu" : "" );
				lastMenuTime = t;
			}

			Log( "menu: %s (cursor %s) | gameplay systems %s | the main menu runs a "
				 "BACKGROUND MAP, so IsInGame()=%d here and cannot gate them",
				 g_camera.UiMode() ? "UP" : "down",
				 g_camera.UiMode() ? "shown" : "hidden",
				 g_camera.UiMode() ? "HELD OFF (aim follows the HEAD)" : "live",
				 ( g_engine.Valid() && g_engine.IsInGame() ) ? 1 : 0 );

			// Menu mode can no longer strand gameplay silently, and this is how
			// that is seen. `menu_gating` remains the kill switch, but it is no
			// longer the ONLY way out -- and it must be ON for the main menu to
			// work at all, so a run with it off is now the abnormal one.
			if ( g_camera.EngineDriveEvents() > 0 )
	{
		g_lookArrow.LogState();
		Log( "cutscene: %u scripted-camera stretch(es) this session | currently %s",
			 g_camera.EngineDriveEvents(),
			 g_camera.EngineDriving() ? "ACTIVE -- your head owns the view" : "idle" );
	}

	// UNCONDITIONAL, and that is the point. The old line above only printed
	// once a stretch had been detected, so a cutscene the detector MISSED --
	// exactly the failure being hunted -- produced no output at all. A
	// conditional log is a bad instrument when the condition is the thing you
	// are diagnosing.
	//
	// Reads as: does the game say the player has lost control, and did we act
	// on it. Those two disagreeing is the whole diagnosis.
	// The pose source, and the spread that justifies it. Reported as EFFECT:
	// a wide min..max here is the jitter the self-predicted path was feeding
	// straight into the head's yaw.
	if ( g_vr )
	{
		float pmn = 0.0f, pmx = 0.0f, pavg = 0.0f;
		Log( "recoil lock: %s | suppressed %.1f deg over %u frame(s) this "
			 "session | firing tail %.0f ms, cap %.1f deg. Non-zero here while "
			 "shooting is the rifle's yaw kick (kickRampMin/Max +-1.0) NOT "
			 "being turned into a body-yaw turn.",
			 g_config.GetBool( "recoil_view_lock", true ) ? "ON" : "off",
			 g_camera.RecoilSuppressedDegrees(),
			 g_camera.RecoilSuppressedFrames(), g_recoilLockTailMs,
			 g_config.GetFloat( "recoil_view_lock_max_degrees", 3.0f ) );
		g_vr->PredictionStats( pmn, pmx, pavg );
		Log( "pose: head from %s | self-predicted interval would be "
			 "avg=%.4f min=%.4f max=%.4f s (spread %.1f ms). Spread near a "
			 "whole frame means that interval was unusable, and it scaled "
			 "straight into TURNING because prediction error follows velocity.",
			 g_config.GetBool( "compositor_poses", true )
				 ? "the COMPOSITOR's renderPoses" : "our own prediction",
			 pavg, pmn, pmx, ( pmx - pmn ) * 1000.0f );
	}

	// WHICH MAP. Every scene report so far has been "the bit in the car" or
	// "the second cutscene", which cannot be matched to a log without asking.
	// GetLevelName is slot 53 and was already bound and unused.
	{
		const char* lvl = g_engine.Valid() ? g_engine.GetLevelName() : nullptr;
		// Both triggers, reported separately. They are independently switchable
		// and they used to be reported as one, which is how "turn-to-face off"
		// read as an explanation for the teleport turn never firing when the
		// real cause was that the detector had been gated out of existence.
		Log( "map: %s | on level change %s, on in-map teleport %s | fired %u "
			 "time(s), %u teleport(s) seen%s | HUD anchor %s",
			 ( lvl && *lvl ) ? lvl : "<none -- not in a level>",
			 g_mapTurnEnabled ? "ON" : "off",
			 g_mapTurnOnTeleport ? "ON" : "off",
			 g_mapTurnCount, g_teleportCount,
			 g_mapTurnPending ? " | ARMED, waiting for the view to settle" : "",
			 Stereo().HudAnchorSuspended( GetTickCount() )
				 ? "SUSPENDED for a transition (the fade draws full-screen)"
				 : "normal" );
	}

	if ( g_playerFlagsOffset >= 0 )
	{
		Log( "cutscene: FL_FROZEN %s now | %u episode(s), %u frame(s) this "
			 "session | %u stretch(es) detected. Frozen episodes with NO "
			 "stretch means the game froze the player and we did not treat it "
			 "as a scene -- a camera that does not ROTATE looks identical to no "
			 "camera to the duration heuristic.",
			 g_camera.PlayerFrozen() ? "SET" : "clear",
			 g_camera.FrozenEpisodes(), g_camera.FrozenFrames(),
			 g_camera.EngineDriveEvents() );
	}
	else
	{
		Log( "cutscene: m_fFlags was never resolved -- FL_FROZEN is UNREADABLE "
			 "this run, so only the duration heuristic is detecting scenes and "
			 "a static cutscene camera cannot be seen at all" );
	}

	{
		const float pct = ( g_lookFramesUndetected > 0 )
			? ( 100.0f * (float)g_lookDivergedUndetected
					/ (float)g_lookFramesUndetected )
			: 0.0f;
		Log( "cutscene: camera-vs-head divergence -- max %.0f deg this session "
			 "| UNDETECTED frames over 60 deg: %u of %u (%.1f%%), worst %.0f deg "
			 "| detected-stretch frames %u",
			 g_lookDivergenceMax, g_lookDivergedUndetected,
			 g_lookFramesUndetected, pct, g_lookDivergenceMaxUndetected,
			 g_lookDivergedDetected );
		Log( "cutscene: near 0 is normal play -- CViewSetup.angles IS the view "
			 "we wrote from the head, so a LARGE value means the engine is "
			 "aiming a camera of its own and we did not notice. Those are the "
			 "frames the player spends unable to see the scene." );
		Log( "cutscene: divergence DRIVES NOTHING -- measurement only. Latching "
			 "it re-fired turn-to-face every time the player looked away, "
			 "because the turn is what collapses the divergence. A detector "
			 "cannot measure a quantity its own correction changes." );
	}

	{
		const Vector& po = g_camera.PositionalOffset();
		Log( "6dof collision: %s | %u stop(s) this session | last fraction %.2f "
			 "| %u startsolid (ignored) | offset (%.1f %.1f %.1f)",
			 !g_collisionTrace.Valid() ? "NO TRACE -- walls will not stop the head"
									   : ( g_camera.PositionalCollided()
											   ? "STOPPED at a wall right now" : "clear" ),
			 g_camera.CollisionCount(), g_camera.LastCollisionFraction(),
			 g_camera.CollisionStartSolid(),
			 po.x, po.y, po.z );
	}

	// ---- TRUE 6DoF ------------------------------------------------------
	//
	// The two counters are the whole diagnosis. Steps rising with the player
	// walking about means the body is following; steps at zero with
	// sixdof_body = 1 means the target is never leaving the deadzone, or
	// gameplay gating is holding it off. Blocked rising steadily rather than
	// occasionally means the sweep hull is catching on something it should not,
	// and the body will feel like it sticks.
	if ( Movement().Enabled() )
	{
		const SixDofSettings& six = Movement().SixDof();
		Log( "sixdof: %s | hook %s | %u chase(s), %u step(s), %u blocked | "
			 "starts past %.1f sideways (+%.2f per degree of lean tilt) / %.1f "
			 "forward, stops inside %.1f | up to speed in %.2fs | chase %.2f "
			 "rate %.0f max %.1f",
			 six.body ? "body chasing head"
					  : "OBSERVING only (sixdof_body = 0)",
			 Movement().Bound() ? "bound" : "NOT bound -- no map loaded yet",
			 Movement().ChaseCount(), Movement().StepCount(), Movement().BlockedCount(),
			 six.deadzone, six.leanTilt, six.deadzone * six.forwardRatio,
			 six.settle, six.ramp, six.chase, six.rate, six.maxStep );
	}

	if ( Shots().Enabled() || Shots().Bound() )
	{
		Log( "shot probe: %s | %d window(s) logged | %u ray(s) moved to the muzzle",
			 !Shots().Bound()
				 ? "enabled but NOT hooked -- no map yet, or the interface name "
				   "is wrong (see the dump above)"
				 : ( Shots().Rewriting() ? "hooked, REWRITING (shots leave the gun)"
										 : "hooked, observational only" ),
			 Shots().Shots(), Shots().Moved() );
	}

	Log( "menu gating: %s | %u liveness escape(s)%s",
				 g_menuGating ? "on" : "OFF -- the main menu will LOCK its yaw, "
								"because the camera then accumulates the "
								"background map's scripted heading",
				 g_menuEscapes,
				 g_menuEscapes ? "  <-- the cursor was reported visible during "
								 "real gameplay input; see IsInteractiveUiVisible"
							   : "" );

			if ( g_camera.UiMode() )
			{
				// EVERY heartbeat, never once. The previous version logged the
				// panel's placement a single time when it was captured, and that
				// line never appeared in three builds' worth of logs -- so the
				// one fact that would have shown the anchor was broken was the
				// one fact the instrument could not deliver. A one-shot log is a
				// bad instrument: it cannot be missed twice, but it can be missed
				// once, and then it is gone.
				Log( "menu panel: %s | pinned yaw %.1f (recentre #%u) | centre "
					 "(%.0f %.0f %.0f) | %.0f x %.0f units at %.0f away | %u draws | engine yaw %.1f",
					 g_menuPanelGeom.valid
						 ? ( g_menuPanelSettings.enabled ? "WORLD QUAD"
														 : "flat overlay (menu_world_panel=0)" )
						 : "NOT BUILT -- falling back to the flat overlay",
					 g_menuAnchorYaw, g_menuAnchorRecentre,
					 g_menuPanelGeom.centre.x, g_menuPanelGeom.centre.y,
					 g_menuPanelGeom.centre.z,
					 g_menuPanelGeom.halfWidth * 2.0f, g_menuPanelGeom.halfHeight * 2.0f,
					 g_menuPanelSettings.distance, Stereo().MenuPanelDraws(),
					 g_camera.LastEngineYaw() );
				Log( "menu panel: %u re-pin(s) this session -- the panel is placed in front of you each time a menu is genuinely opened, but NOT when moving between menu screens (a %u ms gap tells those apart)",
					 g_menuRepins, g_menuRepinMs );
				Log( "menu panel: yaw %+.1f pitch %+.1f roll %+.1f height %+.1f | step %.1f | %s",
					 g_menuPanelSettings.yawOffset, g_menuPanelSettings.pitch,
					 g_menuPanelSettings.roll, g_menuPanelSettings.heightOffset,
					 g_menuPanelStep,
					 g_menuPanelAdjust ? "numpad tuning ON -- 0 dumps cfg lines"
									   : "menu_panel_adjust = 1 to tune on the numpad" );
				g_menuPointer.LogState();
				g_menuCursor.LogState();

				// The numbers behind "is it even being drawn". If the position is
				// sane, the distance is sane and the submissions climb, then the
				// mod has done its part and anything still invisible is the
				// engine's compositing order, not ours.
				{
					Vector mw;
					const bool have = g_menuPointer.CursorWorld( mw );
					const Vector& hp = g_menuPanelGeom.headWorld;
					const float dx = mw.x - hp.x, dy = mw.y - hp.y, dz = mw.z - hp.z;
					Log( "menu pointer target: %s at (%.0f %.0f %.0f), %.0f units from the head "
						 "| overlay %s | %u line(s) issued this session",
						 have ? "have a position" : "NO POSITION -- the pointer is not on the panel",
						 mw.x, mw.y, mw.z,
						 sqrtf( dx * dx + dy * dy + dz * dz ),
						 g_menuPointer.OverlayBound() ? "BOUND" : "NOT BOUND",
						 g_menuPointer.OverlayLines() );
				}
			}

			// The feedback loop that made the menu view feel LOCKED, as a number.
			//
			// `engine rewrote` is how far the engine moved the view angles since
			// our last write. In gameplay that is the player's mouse and it
			// SHOULD be accumulated. On the menu it is the background map's own
			// camera talking to itself, and accumulating it made the body yaw
			// absorb exactly enough to cancel the head.
			//
			// A large suppressed total is the POSITIVE result -- it is the
			// cancellation that is no longer happening. Near zero on the menu
			// would mean the loop was NOT the cause and the lock is something
			// else, so read this before believing the fix.
			Log( "menu yaw: engine rewrote %.2f deg since our last write | body yaw %.1f"
				 " | suppressed %.0f deg over %u menu frames%s",
				 g_camera.LastEngineDelta(), g_camera.BodyYaw(),
				 g_camera.UiSuppressedYaw(), g_camera.UiSuppressedFrames(),
				 ( g_camera.UiSuppressedFrames() > 0 && g_camera.UiSuppressedYaw() < 1.0f )
					 ? "  <-- near zero: the menu is NOT rewriting angles, so the"
					   " lock is something else" : "" );

			// ---- THE ENGINE'S OWN YAW, EVERY HEARTBEAT, IN EVERY STATE -----
			//
			// This used to be printed only inside the UI-mode block, so on the
			// run that produced the main-menu yaw lock it was never printed at
			// all -- and the one number that identifies a scripted camera had to
			// be INFERRED from the written angles instead of read. That is the
			// "a one-shot log is a bad instrument" lesson wearing a new hat: a
			// conditional log is the same trap when the condition is exactly the
			// state you are trying to diagnose.
			//
			// Read it like this:
			//   engine yaw CHANGING       the engine is following our writes --
			//                             ordinary play, guard should be idle
			//   engine yaw PINNED         a scripted camera is re-asserting it;
			//                             the guard should be counting
			// ---- CUMULATIVE IS NOT CURRENT --------------------------------
			//
			// This printed "FIRING" whenever the guard had EVER fired, so a
			// run that spent time on the main menu -- where firing is correct
			// and expected -- then read "FIRING" for the rest of the session
			// no matter what gameplay was doing. Reading it while chasing an
			// unrelated tracking fault cost real time, because the one line
			// that should have said "this is not your problem" said the
			// opposite.
			//
			// The DELTA since the previous heartbeat is what answers "is it
			// happening now". The total stays, because a large total is still
			// the positive result for the menu case.
			{
				const unsigned int frames = g_camera.ScriptedYawFrames();
				const unsigned int recent = frames - g_lastHeartbeatScriptedFrames;
				g_lastHeartbeatScriptedFrames = frames;

				Log( "engine yaw: %.2f (was %.2f) | scripted-camera guard: %s "
					 "(%u frame(s) since last heartbeat, %u total, %.0f deg "
					 "suppressed)%s",
					 g_camera.LastEngineYaw(), g_lastHeartbeatEngineYaw,
					 recent ? "ACTIVE NOW" : "idle",
					 recent, frames, g_camera.ScriptedYawSuppressed(),
					 recent ? "  <-- the engine is driving its own camera; body "
							  "yaw is NOT absorbing it, which is what stops the "
							  "view locking"
							: "" );
			}
			g_lastHeartbeatEngineYaw = g_camera.LastEngineYaw();

			const QAngle& va = g_camera.ViewAngles();
			Log( "aim: source=%s effective=%s | view=(%.0f %.0f %.0f) engine=(%.0f %.0f %.0f)",
				 g_camera.GetAimSource() == kAimController ? "controller" : "hmd",
				 g_camera.AimFromController() ? "CONTROLLER" : "head",
				 va.x, va.y, va.z,
				 g_camera.LastWritten().x, g_camera.LastWritten().y,
				 g_camera.LastWritten().z );

			g_melee.LogState();
			g_arcadeReload.LogState();
			g_physicalCrouch.LogState();

			// Effect, not intent: `applied` is the only thing that separates
			// "not in water" from "in water and the override never fired".
			if ( g_waterLevelOffset >= 0 )
				Log( "swim: water level %d -> %s | pitch override %s",
						  g_lastWaterLevel,
						  ( g_lastWaterLevel >= 2 ) ? "SWIMMING" : "on land",
						  g_camera.SwimPitchApplied() ? "ACTIVE" : "idle" );
		}

		if ( Stereo().Ready() )
			Stereo().LogMatrixPathStats();

		// Address space, and specifically the LARGEST FREE RUN rather than the
		// total. A 32-bit process does not die when free memory reaches zero, it
		// dies when no single run is big enough for the next request -- so the
		// total can look comfortable while an eye surface has nowhere to go. The
		// worst value seen is kept for the same reason the frame cost keeps its
		// max: the failure is the far end of a ramp and an instantaneous reading
		// misses it.
		{
			const AddressSpaceStats as = QueryAddressSpace();
			if ( as.largestFreeMB < worstLargestFreeMB )
				worstLargestFreeMB = as.largestFreeMB;

			Log( "address space: %llu/%llu MB committed, largest free run %llu MB "
				 "(worst %llu MB)%s",
				 as.committedMB, as.limitMB, as.largestFreeMB, worstLargestFreeMB,
				 as.largeAddressAware ? "" : " | 2 GB CAP" );

			// 64 MB is roughly two full-size eye surfaces. Below that the next
			// render-target allocation is the one that fails.
			if ( as.largestFreeMB < 64 && !addressSpaceWarned )
			{
				addressSpaceWarned = true;
				CrashLog( "=== ADDRESS SPACE LOW ===" );
				CrashLog( "largest free run is %llu MB of %llu MB total free; limit %llu MB (%s)",
						  as.largestFreeMB, as.freeMB, as.limitMB,
						  as.largeAddressAware ? "4 GB" : "2 GB CAP" );
				CrashLog( "allocations from here fail against fragmentation, not exhaustion, so "
						  "the next crash may surface anywhere. If this is a 2 GB CAP run, "
						  "launch via sinvr_launcher.exe." );
				CrashLog( "=== END ADDRESS SPACE LOW ===" );
			}
		}

		last = g_frames;
	}
}

} // namespace

BOOL WINAPI DllMain( HINSTANCE inst, DWORD reason, LPVOID )
{
	if ( reason == DLL_PROCESS_ATTACH )
	{
		DisableThreadLibraryCalls( inst );
		CreateThread( NULL, 0, InitThread, NULL, 0, NULL );
	}
	return TRUE;
}
