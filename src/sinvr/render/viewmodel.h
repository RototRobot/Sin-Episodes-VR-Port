// Pinning the weapon to the controller.
//
// ---- WHY THIS SHAPE ---------------------------------------------------------
//
// Source positions the viewmodel in CViewRender::SetUpView:
//
//     pPlayer->CalcView( m_View.origin, m_View.angles, ... );   // view.cpp:534
//     pPlayer->CalcViewModelView( m_View.origin, m_View.angles ); // view.cpp:545
//
// so the gun is placed from the player's EYE, and there is no viewmodel origin
// field in CViewSetup to write -- only fovViewmodel/zNearViewmodel/zFarViewmodel.
// That is why the arms rotate with the controller once aim is decoupled (angles
// reach it through the engine's view angles) but never translate.
//
// Portal 2 VR solves this by hooking `C_BasePlayer::CalcViewModelView` and
// substituting its two arguments, letting the game's own code do the bob and
// sway from a different eye pose. That is the better long-term shape and it is
// where this should end up. It needs a SIGNATURE SCAN, because the function is
// NOT virtual -- `void CalcViewModelView(const Vector&, const QAngle&)` is a
// plain method in c_baseplayer.h -- and Portal 2 VR's own signature
// (`55 8B EC 83 EC 34 53 8B D9 80 BB`) is for a different game's client.dll.
//
// This does the cheaper thing first, built only on offsets already verified
// against the live process: find the viewmodel entity and write its transform
// directly, after SetUpView has positioned it and before the scene is drawn.
//
// **Whether that window exists is the open question**, and it is the reason
// every write is logged. If Source re-derives the abs transform from the local
// one after we write -- it keeps a dirty flag for exactly that -- the gun will
// not move and the log will say the values did not stick. That result sends us
// to the signature scan, and costs one build to learn instead of a day.
#pragma once

#include <math.h>
#include <string.h>
#include "../sdk/source_interfaces.h"
#include "../sdk/client_entity_list.h"
#include "../sdk/engine_trace.h"
#include "../vr/vr_backend.h"
#include "../vr_camera.h"
#include "../hooks/vtable_hook.h"
#include "../../common/log.h"

namespace sinvr {

// AnglesToBasis, RotateAboutAxis, BasisToAngles and ComposeWeaponAngles all come
// from vr_camera.h, so the weapon model and the aim share ONE definition of the
// controller-to-weapon transform. Two copies would drift, and the symptom would
// be the shot quietly diverging from the gun.

// Position offsets for ONE model.
//
// The three viewmodels are authored with different origins -- measured on
// hardware, the magnum sat correctly while the scattergun was too far back
// and the assault rifle slightly too far forward with the SAME offsets. That
// is a property of the models, not of the player's grip, so it cannot be
// tuned away globally: whatever value suits one gun is wrong for another.
//
// ANGLES are deliberately NOT per-model. They are the controller-to-weapon
// transform -- how the hand holds a gun -- which is the same physical fact
// whichever gun it is, and splitting them would mean re-tuning the aim three
// times for no reason.
// Source's AngleMatrix, laid out exactly as mathlib does it: a matrix3x4_t is
// float[3][4] in ROW-major order, whose COLUMNS are forward, left and up, with
// the origin in column 3.
//
// Reproduced here rather than derived from AnglesToBasis so it matches the
// engine's own memory layout literally -- this value is written straight into
// a live entity, and 'nearly the right basis' would be a subtly rotated gun.
inline void BuildCoordinateFrame( const QAngle& angles, const Vector& origin,
	  float m[12] )
{
	const float sp = sinf( angles.x / kDegPerRadian ), cp = cosf( angles.x / kDegPerRadian );
	const float sy = sinf( angles.y / kDegPerRadian ), cy = cosf( angles.y / kDegPerRadian );
	const float sr = sinf( angles.z / kDegPerRadian ), cr = cosf( angles.z / kDegPerRadian );

	m[0] = cp * cy;                      // [0][0]
	m[1] = sr * sp * cy + cr * -sy;      // [0][1]
	m[2] = cr * sp * cy + -sr * -sy;     // [0][2]
	m[3] = origin.x;                     // [0][3]

	m[4] = cp * sy;                      // [1][0]
	m[5] = sr * sp * sy + cr * cy;       // [1][1]
	m[6] = cr * sp * sy + -sr * cy;      // [1][2]
	m[7] = origin.y;                     // [1][3]

	m[8] = -sp;                          // [2][0]
	m[9] = sr * cp;                      // [2][1]
	m[10] = cr * cp;                     // [2][2]
	m[11] = origin.z;                    // [2][3]
}


//-----------------------------------------------------------------------------
// Substituting the viewmodel pose AT THE ENGINE'S OWN SETTER.
//
// ---- WHY EVERY EARLIER ATTEMPT FAILED ---------------------------------------
//
// Four rounds were spent writing the viewmodel's transform fields directly:
// m_vecOrigin, m_vecAbsOrigin, m_angRotation, m_angAbsRotation, and finally
// m_rgflCoordinateFrame. Each was verified to land, and none of them changed
// what was drawn. The measurements said why, once the right field was watched:
//
//   * m_vecAbsOrigin changed 0 times in 797 eye-pass pairs -- nothing overwrote
//     it, because nothing READS it.
//   * local and the coordinate frame changed together 301 times out of 797 --
//     the frame is rebuilt from local.
//   * at the viewmodel's own PerspectiveX, immediately before the gun is drawn,
//     our transform was intact 3114 times out of 3114.
//
// That last one is the killer: the transform is correct at draw time and the
// gun still draws in the wrong place. So the renderer is not reading it -- it is
// using a BONE CACHE built earlier from the engine's pose. SetLocalOrigin does
// not merely store a value, it invalidates that cache. Poking memory stores the
// value and skips the invalidation, so the bones never rebuild. L4D2VR reached
// the same conclusion and calls the real CBaseEntity::SetAbsOrigin rather than
// writing fields; their offsets.h notes it is "used for viewmodel stabilization"
// and their config calls the symptom "first-person viewmodel ghosting".
//
// ---- WHY THIS IS A VTABLE HOOK AND NOT A SIGNATURE SCAN ---------------------
//
// Both reference mods hook CalcViewModelView, which needs a byte signature
// because it is not virtual. Disassembling SiN's client.dll to find it -- via
// the ApplyShake(vmorigin, vmangles, 0.1) call, the only site in 39 candidates
// with two stack locals and that literal -- turned up something better at its
// tail:
//
//   0x2409CBE8  mov edx,[esi]; lea eax,[esp+4];    push eax; call [edx+0xE8]
//   0x2409CBF7  mov edx,[esi]; lea eax,[esp+0x10]; push eax; call [edx+0xF0]
//
// SetLocalOrigin and SetLocalAngles are called VIRTUALLY, through the
// viewmodel's own vtable. So the last thing CalcViewModelView does is reachable
// with the VTableHook machinery this project already uses everywhere else -- no
// trampoline, no byte pattern to go stale, and the substitution happens at the
// engine's own setter, so every invalidation the engine does still runs.
//
// The function itself is CBaseViewModel::CalcViewModelView at client.dll+0x9C970
// (`83 EC 78 8B 84 24 84 00 00 00 56 8B F1`), confirmed by `ret 0xC` -- three
// stack args, matching (owner, const Vector&, const QAngle&) -- and by its tail
// being exactly the SDK's final three statements.
//
// Substituting at the SETTER rather than at CalcViewModelView's arguments also
// discards the engine's bob, lag and shake, which are applied to the local
// variables before these calls. In VR that is what you want: the gun should sit
// where the player's hand is, not sway.
//-----------------------------------------------------------------------------
namespace vm_setter_hook {

// Verified from SiN's own code, above. Not from any SDK header.
constexpr int kSetLocalOrigin = 0xE8 / 4;   // 58
constexpr int kSetLocalAngles = 0xF0 / 4;   // 60

using SetOriginFn = void( __fastcall* )( void*, void*, const Vector* );
using SetAnglesFn = void( __fastcall* )( void*, void*, const QAngle* );

inline SetOriginFn g_originalSetOrigin = nullptr;
inline SetAnglesFn g_originalSetAngles = nullptr;
inline VTableHook g_originHook;
inline VTableHook g_anglesHook;

inline void* g_target = nullptr;
inline Vector g_origin = { 0.0f, 0.0f, 0.0f };
inline QAngle g_angles = { 0.0f, 0.0f, 0.0f };
inline bool g_active = false;
inline unsigned int g_originSubs = 0;
inline unsigned int g_angleSubs = 0;
inline unsigned int g_originCalls = 0;

inline void __fastcall Detour_SetLocalOrigin( void* thisptr, void* edx, const Vector* v )
{
	++g_originCalls;
	if ( g_active && thisptr == g_target && g_target )
	{
		v = &g_origin;
		++g_originSubs;
	}
	if ( g_originalSetOrigin )
		g_originalSetOrigin( thisptr, edx, v );
}

inline void __fastcall Detour_SetLocalAngles( void* thisptr, void* edx, const QAngle* a )
{
	if ( g_active && thisptr == g_target && g_target )
	{
		a = &g_angles;
		++g_angleSubs;
	}
	if ( g_originalSetAngles )
		g_originalSetAngles( thisptr, edx, a );
}

} // namespace vm_setter_hook

//-----------------------------------------------------------------------------
// Calling C_BaseEntity's own pose setters on an entity we do not own.
//
// The viewmodel is fixed by HOOKING these slots, because the engine calls them
// itself every frame. Nothing calls them on the WEAPON entity -- its transform
// arrives over the network -- so to move it we have to call them ourselves.
//
// ---- WHY THIS EXISTS AT ALL -------------------------------------------------
//
// The C_Weapon* entity carries a v_ model of its own and sits at a
// player-relative (-4.9, 0, 35.7) -- eye height. On a monitor that lands on top
// of the real viewmodel and nobody sees two guns; in VR the viewmodel goes to
// the player's hand and this one stays at the eye, so it is a second gun
// hanging in front of them. Pinning puts it on the real gun, hiding stops it
// drawing, and both are worth doing for their own sake.
//
// It was ALSO the second of three hypotheses for the misaligned muzzle flash,
// and it was wrong -- the flash was FormatViewModelAttachment, see
// matchViewmodelFov in stereo.h. That is recorded here rather than deleted
// because of HOW it was ruled out: this code ran perfectly (8321 pins, 0 raw
// fallback) and the flash was still wrong, which proved the flash comes off
// neither entity's attachment and left exactly one candidate. Build the counter
// that proves the code ran, or a negative result only tells you to guess again.
//
// ---- WHY THE SETTER AND NOT THE FIELDS --------------------------------------
//
// This is the ghosting lesson applied to a second entity. ApplyToWeaponEntity
// used to write m_vecOrigin / m_vecAbsOrigin / m_angRotation / m_angAbsRotation
// directly, which is exactly what failed four times on the viewmodel:
// SetLocalOrigin does not merely store a value, it INVALIDATES THE BONE CACHE.
// An attachment is derived from bones, so a raw write leaves the flash coming
// out of stale bones however correct the fields read back.
//
// Slots 58 / 60 (+0xE8 / +0xF0) were verified by disassembling
// CBaseViewModel::CalcViewModelView -- see vm_setter_hook above. They are
// reused here on the grounds that both classes derive from C_BaseEntity, so
// the C_BaseEntity portion of the vtable is common. That is an INFERENCE, not
// a disassembly of the weapon's own vtable, so every step is guarded and a
// failure degrades to the old raw-write path rather than faulting.
//-----------------------------------------------------------------------------
namespace entity_setter {

using SetOriginFn = void( __thiscall* )( void*, const Vector* );
using SetAnglesFn = void( __thiscall* )( void*, const QAngle* );

// A vtable slot that is not committed executable memory is not a function, and
// calling it would fault rather than fail. Cheap enough to do per frame, and it
// is the difference between "this build lays entities out differently" and a
// crash dump.
inline bool PlausibleCode( void* fn )
{
	if ( !fn )
		return false;
	MEMORY_BASIC_INFORMATION mbi = {};
	if ( VirtualQuery( fn, &mbi, sizeof( mbi ) ) != sizeof( mbi ) )
		return false;
	if ( mbi.State != MEM_COMMIT )
		return false;
	const DWORD exec = PAGE_EXECUTE | PAGE_EXECUTE_READ |
					   PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
	return ( mbi.Protect & exec ) != 0;
}

// Returns false if the slots could not be resolved or the call faulted, so the
// caller can fall back. Never throws.
inline bool SetPose( void* entity, const Vector& origin, const QAngle& angles )
{
	if ( !entity )
		return false;

	__try
	{
		void** vt = *reinterpret_cast<void***>( entity );
		if ( !vt )
			return false;

		void* so = vt[vm_setter_hook::kSetLocalOrigin];
		void* sa = vt[vm_setter_hook::kSetLocalAngles];
		if ( !PlausibleCode( so ) || !PlausibleCode( sa ) )
			return false;

		reinterpret_cast<SetOriginFn>( so )( entity, &origin );
		reinterpret_cast<SetAnglesFn>( sa )( entity, &angles );
	}
	__except ( EXCEPTION_EXECUTE_HANDLER )
	{
		return false;
	}
	return true;
}

} // namespace entity_setter

struct ModelOffsets
{
	char key[32] = { 0 };
	float forward = 0.0f;
	float right = 0.0f;
	float up = 0.0f;
	bool configured = false;   // false = fall back to the global values
};

// Time constant for easing the barrel retreat, in seconds. 60 ms is about
// four frames at 90 fps -- fast enough that the gun is clear before the player
// registers it, slow enough that a trace flickering blocked/clear across one
// frame does not read as a twitch.
constexpr float kBarrelTau = 0.06f;

// Shorter than the barrel's: this filters a NUMBER the aim is derived from, and
// aim wants to be current. 40 ms is under four frames at 90 fps.
constexpr float kAimTau = 0.04f;

struct ViewModelSettings
{
	// Off by default: this is the untested half of controller tracking, and a
	// wrong write goes into a live game entity.
	bool followController = false;

	// Where the model sits relative to the controller, in Source units, along
	// the controller's own forward/right/up. SiN's viewmodels were authored for
	// a screen-centred weapon, so the origin is not at the grip and this needs
	// tuning per weapon; these are a starting point, not a calibration.
	float offsetForward = 0.0f;
	float offsetRight = 0.0f;
	float offsetUp = 0.0f;

	// Rotation offsets, degrees, applied on top of the controller orientation.
	// Needed because a controller is not held at the angle a gun model expects:
	// SiN authored these for a screen-centred weapon, so the model forward is
	// not the controller forward and no position offset can fix an angle.
	float anglePitch = 0.0f;
	float angleYaw = 0.0f;
	float angleRoll = 0.0f;

	// Write m_rgflCoordinateFrame as well as the origin fields. This is the
	// actual fix for the firing flicker; it is a setting only so it can be
	// turned off if it ever misbehaves, since it writes a matrix into a live
	// entity at an offset found by scanning.
	bool writeCoordinateFrame = true;

	// ---- the weapon entity's OWN copy of the gun ----------------------
	//
	// SiN's C_Weapon* entities each carry a v_ VIEWMODEL as their model and
	// sit at a player-relative (-4.9, 0, 35.7) -- eye height. On a monitor
	// that lands exactly on top of the real viewmodel and nobody ever sees
	// two guns. In VR we move the viewmodel to the player's hand and this one
	// stays at the eye, so it becomes a second gun hanging in front of them,
	// in precisely the spot the flatscreen weapon occupies.
	//
	// It has been suspected of TWO defects and caused NEITHER. It is not what
	// caused the ghosting -- that was the bone cache, see vm_setter_hook above
	// -- and it is not what misaligned the muzzle flash, which was
	// FormatViewModelAttachment rescaling the flash's offset from the eye (see
	// matchViewmodelFov in stereo.h).
	//
	// Pinning it is nonetheless correct on its own merits, and the run that
	// cleared it is worth keeping: `pinned 8321, 0 raw fallback` with the flash
	// still wrong is what proved the flash does not come off either entity's
	// attachment, which is what left FormatViewModelAttachment as the only
	// candidate. A mechanism that provably RAN and changed nothing eliminated
	// an entire layer in one run.
	//
	// pin  gives it the same transform as the viewmodel, so anything anchored
	//      to it -- flash, smoke, shell ejection -- happens at the real gun.
	//      Applied through the entity's own SetLocalOrigin/SetLocalAngles (see
	//      entity_setter above), never by writing the fields: the flash comes
	//      off a bone-derived attachment and only the setter invalidates the
	//      bone cache.
	// hide sets EF_NODRAW so the duplicate mesh stops rendering. Pinning
	//      without hiding leaves two identical models in the same place,
	//      which z-fights; hiding without pinning leaves the effects behind.
	//      Both together is the combination that behaves.
	bool pinWeaponEntity = true;
	bool hideWeaponEntity = true;

	// Substitute the pose at the engine's own SetLocalOrigin/SetLocalAngles.
	// THE fix for the viewmodel ghosting -- see the vm_setter_hook comment.
	bool hookSetters = true;

	// ---- COLLISION -- stop the gun phasing through walls -------------------
	//
	// Deliberately built like VRCamera's head collision (search m_collideOffset
	// there) rather than as a new idea: same EngineTrace, same hull sweep, same
	// startsolid rule. That code is proved on hardware and its failure modes are
	// already written down.
	//
	// TWO sweeps, because they fail differently:
	//
	//   BODY    eye -> hand. Stops you pushing the gun through a wall sideways.
	//   BARREL  hand -> muzzle, along the gun's OWN axis. Pulling the grip out
	//           of a wall still leaves the barrel buried in it, and the barrel
	//           is the part the player is looking down.
	//
	// The result is applied as a RIGID translation of the whole model, so the
	// gun never bends away from the hand that is holding it.
	bool collide = true;

	// Half-extent of the sweep hull. A ray would let the model's width clip
	// through; a fat hull makes the gun shy away from doorframes it would have
	// cleared.
	float collideRadius = 2.0f;

	// Hand-to-muzzle distance to keep clear, in Source units. 0 disables the
	// barrel sweep and collides the grip only.
	float collideBarrel = 12.0f;

	// How much of the barrel's penetration is actually taken off the gun.
	//
	// 1.0 pulls the barrel fully clear and reads as a SHOVE -- correct, and
	// unpleasant, which is what the first build was. Below 1 the barrel is
	// allowed to stay slightly buried in exchange for the gun not lunging at
	// the player. Purely a comfort trade: geometry accuracy for feel.
	//
	// Applies to the barrel retreat ONLY. The eye->hand clamp is a hard
	// constraint and stays whole -- softening THAT would let the gun enter the
	// wall and then ease back out, which is worse than either extreme.
	float collideResponse = 0.35f;

	// Maximum angle, in degrees, the barrel may be swung away from a surface
	// before the retreat has to take over. 0 disables deflection and leaves
	// only the clamp and the retreat -- the previous behaviour.
	//
	// Deflection is preferred over retreat because it keeps the gun WHERE YOUR
	// HAND IS. Retreat cannot: it slides the model away from the controller,
	// which is the thing that read as a shove. But it cannot cover everything
	// either -- pushed square into a wall there is no sideways to swing to --
	// so both exist and the re-trace decides how much of each is needed.
	float collideDeflect = 60.0f;

	// Shape of that ramp against penetration depth. 1.0 is linear; above 1 the
	// gun is left nearly alone when the barrel has only grazed a surface and
	// swings harder the further it is pushed through.
	float collideDeflectCurve = 1.5f;

	// ---- AIM CONVERGENCE, MEASURED ----------------------------------------
	//
	// Not really a viewmodel feature -- it belongs to the aim, and the config
	// keys say so. It lives here because THIS is where the gun's world position
	// and its true forward axis are known, and re-deriving them in VRCamera
	// would be two copies of the same maths that could drift apart. See
	// AimDistance() for what actually crosses the boundary.
	bool aimTrace = true;
	float aimTraceMax = 4096.0f;

};

class ViewModelDriver
{
public:
	void SetSettings( const ViewModelSettings& s ) { m_settings = s; }
	const ViewModelSettings& Settings() const { return m_settings; }

	// ---- live offset tuning -------------------------------------------------
	//
	// The model's origin is not at the grip -- observed as the controller sitting
	// at the BACK of the arm rather than in the hand -- and the distance from
	// origin to grip differs per weapon. That is a number nobody can derive from
	// the model, only dial in while wearing the headset, so it is adjustable at
	// runtime and dumped as cfg lines rather than guessed at in a rebuild.
	//
	// Same idea as the numpad eye alignment, and it prints the exact lines to
	// paste so a tuning session ends with something durable instead of a feeling.
	// Two things need dialling in and there are not enough spare numpad keys for
	// six axes, so the same three key pairs drive whichever mode is selected.
	enum AdjustMode { kAdjustPosition = 0, kAdjustAngles = 1 };

	void ToggleAdjustMode()
	{
		m_mode = ( m_mode == kAdjustPosition ) ? kAdjustAngles : kAdjustPosition;
		Log( "viewmodel adjust: now editing %s (step=%.2f)",
			 m_mode == kAdjustPosition ? "POSITION (forward/right/up)"
									   : "ANGLES (pitch/yaw/roll)",
			 m_step );
		LogOffsets( "mode changed" );
	}

	AdjustMode Mode() const { return m_mode; }

	void Nudge( float a, float b, float c )
	{
		if ( m_mode == kAdjustPosition )
		{
			// Edits THIS weapon's offsets, seeded from whatever is currently in
			// force, so tuning the shotgun cannot move the pistol that was
			// already dialled in.
			if ( m_current >= 0 )
			{
				if ( !m_models[m_current].configured )
				{
					const ModelOffsets seed = Eff();
					m_models[m_current].forward = seed.forward;
					m_models[m_current].right = seed.right;
					m_models[m_current].up = seed.up;
					m_models[m_current].configured = true;
				}
				m_models[m_current].forward += a * m_step;
				m_models[m_current].right += b * m_step;
				m_models[m_current].up += c * m_step;
			}
			else
			{
				// No model resolved -- fall back to the global set rather than
				// silently doing nothing while the player presses keys.
				m_settings.offsetForward += a * m_step;
				m_settings.offsetRight += b * m_step;
				m_settings.offsetUp += c * m_step;
			}
		}
		else
		{
			// Degrees, not units -- a 1.0 step is a sensible angular nudge as
			// well as a sensible positional one, so the same step serves both.
			m_settings.anglePitch += a * m_step;
			m_settings.angleYaw += b * m_step;
			m_settings.angleRoll += c * m_step;
		}
		LogOffsets( "nudged" );
	}

	void ResetOffsets()
	{
		if ( m_mode == kAdjustPosition )
		{
			if ( m_current >= 0 )
			{
				m_models[m_current].forward = 0.0f;
				m_models[m_current].right = 0.0f;
				m_models[m_current].up = 0.0f;
				m_models[m_current].configured = true;
			}
			m_settings.offsetForward = 0.0f;
			m_settings.offsetRight = 0.0f;
			m_settings.offsetUp = 0.0f;
		}
		else
		{
			m_settings.anglePitch = 0.0f;
			m_settings.angleYaw = 0.0f;
			m_settings.angleRoll = 0.0f;
		}
		LogOffsets( "reset" );
	}

	void ChangeStep( float factor )
	{
		m_step *= factor;
		if ( m_step < 0.05f )
			m_step = 0.05f;
		if ( m_step > 8.0f )
			m_step = 8.0f;
		LogOffsets( "step changed" );
	}

	void LogOffsets( const char* why ) const
	{
		Log( "viewmodel adjust (%s): editing %s, step=%.2f -- paste into sinvr.cfg:",
			 why, m_mode == kAdjustPosition ? "POSITION" : "ANGLES", m_step );
		// Position lines are emitted PER WEAPON when one is resolved, because
		// that is the form that can be pasted back and will still be right
		// after a weapon switch.
		if ( m_current >= 0 )
		{
			const ModelOffsets e = Eff();
			Log( "    viewmodel_offset_forward_%s = %.2f", m_models[m_current].key, e.forward );
			Log( "    viewmodel_offset_right_%s = %.2f", m_models[m_current].key, e.right );
			Log( "    viewmodel_offset_up_%s = %.2f", m_models[m_current].key, e.up );
		}
		else
		{
			Log( "    viewmodel_offset_forward = %.2f", m_settings.offsetForward );
			Log( "    viewmodel_offset_right = %.2f", m_settings.offsetRight );
			Log( "    viewmodel_offset_up = %.2f", m_settings.offsetUp );
		}
		Log( "    viewmodel_angle_pitch = %.2f", m_settings.anglePitch );
		Log( "    viewmodel_angle_yaw = %.2f", m_settings.angleYaw );
		Log( "    viewmodel_angle_roll = %.2f", m_settings.angleRoll );
	}

	// Which model is in the player's hands, from ViewModelAnimation, which
	// already resolves it through IVModelInfo. Pushed in every frame rather
	// than looked up here so there is exactly one resolver.
	void SetCurrentModel( const char* key )
	{
		if ( !key )
		{
			m_current = -1;
			return;
		}
		if ( m_current >= 0 && _stricmp( m_models[m_current].key, key ) == 0 )
			return;
		m_current = FindOrAdd( key );
		if ( m_current >= 0 )
			Log( "viewmodel: now holding %s -- offsets fwd=%.1f right=%.1f up=%.1f%s",
				 key, Eff().forward, Eff().right, Eff().up,
				 m_models[m_current].configured ? "" : " (global default -- not tuned for this weapon yet)" );
	}

	// Offsets in force right now: this model's if it has been tuned, the
	// global ones otherwise. Everything -- the model transform, the aim
	// convergence, the numpad tuner -- goes through here so they cannot
	// disagree about which gun is being positioned.
	ModelOffsets Eff() const
	{
		ModelOffsets out;
		if ( m_current >= 0 && m_models[m_current].configured )
		{
			out = m_models[m_current];
			return out;
		}
		out.forward = m_settings.offsetForward;
		out.right = m_settings.offsetRight;
		out.up = m_settings.offsetUp;
		return out;
	}

	const char* CurrentModelKey() const
	{
		return ( m_current >= 0 ) ? m_models[m_current].key : nullptr;
	}

	// What a NAMED weapon resolves to, without needing it to be in hand.
	//
	// Eff() answers for the weapon currently held, which is the wrong question
	// at startup when nothing is held yet. This is how the log can state what
	// each gun will use before the player has touched one.
	//
	// Returns false when that weapon has no per-weapon values, in which case
	// `out` is filled with the globals it would fall back to.
	bool ModelOffsetsFor( const char* key, ModelOffsets& out ) const
	{
		for ( int i = 0; i < m_modelCount; ++i )
		{
			if ( _stricmp( m_models[i].key, key ) == 0 && m_models[i].configured )
			{
				out = m_models[i];
				return true;
			}
		}
		out = ModelOffsets();
		out.forward = m_settings.offsetForward;
		out.right = m_settings.offsetRight;
		out.up = m_settings.offsetUp;
		return false;
	}

	// From config at startup.
	void SetModelOffsets( const char* key, float fwd, float right, float up )
	{
		const int i = FindOrAdd( key );
		if ( i < 0 )
			return;
		m_models[i].forward = fwd;
		m_models[i].right = right;
		m_models[i].up = up;
		m_models[i].configured = true;
	}

	//-------------------------------------------------------------------------
	// What actually draws the model.
	//
	// The eye-pass trace proved m_vecAbsOrigin is rock stable and identical in
	// both eyes while the gun was visibly flickering. Those two facts are only
	// compatible if THE FIELD WE MEASURED IS NOT THE FIELD THAT DRAWS. Source
	// renders a client entity from m_rgflCoordinateFrame -- a cached 3x4 matrix
	// built by CalcAbsolutePosition() -- not from m_vecAbsOrigin directly. We
	// write the origin fields and never touch that matrix, so the render uses
	// whatever the engine last computed, and our write only takes effect when
	// something else happens to rebuild the frame.
	//
	// That fits every symptom: mostly correct (the frame gets rebuilt most
	// frames), intermittently displaced (when it does not), and worst while
	// firing (the engine touches the entity most then).
	//
	// It was also thought to explain the misaligned muzzle flash. It did not --
	// that was FormatViewModelAttachment rescaling the flash's offset from the
	// eye, see viewmodel_fov_match. Attachments were never the problem.
	//
	// This is READ ONLY. It scans the entity for every field that currently
	// equals the abs origin -- as a Vector, and as the translation column of a
	// matrix3x4 -- and reports the offsets. Two known Vectors should show up
	// (local 0x01F0 and abs 0x0220); anything else is a cached copy we are
	// failing to update, and a matrix hit is the coordinate frame.
	//
	// Nothing is written on the strength of a scan until the same offset comes
	// back on repeated runs -- this file already carries one hard-won lesson
	// about a weakly-identified offset.
	//-------------------------------------------------------------------------
	void Diagnose( const EngineClient& engine )
	{
		if ( m_diagnoseRuns <= 0 )
			return;

		void* vm = ResolveViewModel( engine );
		if ( !vm )
		{
				Log( "viewmodel diag: no viewmodel entity to inspect (%s)",
				 m_lastFailure ? m_lastFailure : "unknown" );
			--m_diagnoseRuns;
			return;
		}

		--m_diagnoseRuns;
		auto* b = reinterpret_cast<unsigned char*>( vm );
		Vector abs = {};
		__try
		{
			abs = *reinterpret_cast<const Vector*>( b + viewmodel_offset::kAbsOrigin );
		}
		__except ( EXCEPTION_EXECUTE_HANDLER )
		{
			return;
		}

		Log( "viewmodel diag: entity %p abs=(%.2f %.2f %.2f) -- scanning for every copy", vm, abs.x, abs.y, abs.z );

		const int kLimit = 0x900;
		int vectorHits = 0, matrixHits = 0;

		__try
		{
			for ( int off = 0; off + 48 <= kLimit; off += 4 )
			{
				const float* f = reinterpret_cast<const float*>( b + off );

				// A plain Vector copy.
				if ( vectorHits < 12 && Near( f[0], abs.x ) && Near( f[1], abs.y ) &&
					 Near( f[2], abs.z ) )
				{
					++vectorHits;
					Log( "viewmodel diag:   +0x%04X  Vector  (%.2f %.2f %.2f)%s",
						 off, f[0], f[1], f[2],
						 off == viewmodel_offset::kOrigin ? "   <- m_vecOrigin (known)"
						 : off == viewmodel_offset::kAbsOrigin ? "   <- m_vecAbsOrigin (known)"
						 : "   <- UNKNOWN COPY" );
				}

				// matrix3x4_t is float[3][4], row major: the translation is the
				// fourth entry of each row, i.e. f[3], f[7], f[11].
				if ( matrixHits < 8 && Near( f[3], abs.x ) && Near( f[7], abs.y ) &&
					 Near( f[11], abs.z ) )
				{
					++matrixHits;
					Log( "viewmodel diag:   +0x%04X  matrix3x4 translation (%.2f %.2f %.2f) <- COORDINATE FRAME CANDIDATE",
						 off, f[3], f[7], f[11] );
					Log( "viewmodel diag:            rot [%.3f %.3f %.3f | %.3f %.3f %.3f | %.3f %.3f %.3f]",
						 f[0], f[1], f[2], f[4], f[5], f[6], f[8], f[9], f[10] );
				}
			}
		}
		__except ( EXCEPTION_EXECUTE_HANDLER )
		{
				Log( "viewmodel diag: scan faulted at the end of the entity -- partial result above" );
		}

		Log( "viewmodel diag: %d vector copies, %d matrix candidates. Two vectors are expected; a THIRD, or any matrix, is what the render is really using.",
			 vectorHits, matrixHits );

		// A second viewmodel would explain it just as well: SiN could keep more
		// than one, and writing only the first would leave the other drawing
		// wherever the engine put it.
		const int playerIndex = engine.GetLocalPlayer();
		void* player = ( playerIndex > 0 ) ? m_entities.GetClientEntity( playerIndex ) : nullptr;
		if ( !player )
			return;

		__try
		{
			for ( int i = 0; i < 4; ++i )
			{
				const unsigned int h = *reinterpret_cast<unsigned int*>(
					reinterpret_cast<unsigned char*>( player ) +
					player_offset::kViewModelHandle + i * 4 );
				if ( h == 0 || h == 0xFFFFFFFFu )
				{
					Log( "viewmodel diag: m_hViewModel[%d] empty", i );
					continue;
				}
				void* e = m_entities.GetClientEntityFromHandle( h );
				Log( "viewmodel diag: m_hViewModel[%d] handle=0x%08X -> %p%s",
					 i, h, e, ( e == vm ) ? "   <- the one we write" : "   <- ANOTHER ENTITY" );
			}
		}
		__except ( EXCEPTION_EXECUTE_HANDLER )
		{
				}
	}

	void SetDiagnoseRuns( int n ) { m_diagnoseRuns = n; }
	int DiagnoseRunsLeft() const { return m_diagnoseRuns; }

	// Netprop offsets for the weapon entity, from ViewModelAnimation, which
	// already owns the RecvTable walk.
	void SetWeaponFieldOffsets( int weaponHandle, int effects )
	{
		m_offWeaponHandle = weaponHandle;
		m_offEffects = effects;
	}

	void Bind( const ClientEntityList& list ) { m_entities = list; }
	bool Ready() const { return m_entities.Valid(); }

	// What the gun is allowed to pass through. Set immediately before Apply and
	// cleared when any piece is missing, for the same reason the head's version
	// is: a skip entity from an earlier map would be a stale pointer, and a
	// missing trace must mean "do not collide", never "collide against
	// nothing".
	void SetCollisionContext( EngineTrace* trace, void* skipEntity )
	{
		m_trace = trace;
		m_collisionSkip = skipEntity;
	}
	// Distance from the HAND, along the gun's true forward, to the first thing
	// the barrel is pointing at. Negative means nothing was hit and the caller
	// should fall back to its fixed distance.
	//
	// A SCALAR is deliberately the only thing that crosses into the aim path.
	// VRCamera already knows where the muzzle is relative to the eye and has
	// had that maths right for a long time; handing it a world POINT would mean
	// reconciling two frames of reference -- engine eye, rendered eye, duck
	// compensation, the 6DoF offset -- any one of which is a silent aiming bug.
	// It only ever lacked a good number for how far away to converge.
	float AimDistance() const { return m_aimDistance; }
	// The muzzle's position in the gun's own basis, pushed in per frame because
	// it is PER WEAPON and lives in the bore tuner rather than in settings.
	// In the gun's basis, not the world's, so the calibration means the same
	// thing whatever attitude the gun is held at.
	void SetShootOffset( float right, float up, float fwd )
	{
		m_shootRight = right;
		m_shootUp = up;
		m_shootFwd = fwd;
	}

	const Vector& ShootOriginWorld() const { return m_shootOrigin; }
	bool HaveShootOrigin() const { return m_haveShootOrigin; }

	// ---- WHERE THE GUN ACTUALLY IS, IN WORLD SPACE -------------------------
	//
	// The pose this driver placed the model at, published so a debug view can
	// draw the barrel from the gun rather than reconstructing it.
	//
	// Reconstruction is exactly what went wrong: the aim path carries its hand
	// vector relative to the HEAD, this one carries it relative to the recentre
	// REFERENCE, and the two differ by the 6DoF positional offset. A barrel
	// line built from the first drifts off the gun the moment the player leans
	// or turns their head -- which is not a rendering artefact, it is the
	// visualiser measuring from a different origin than the thing it is drawing.
	const Vector& AimOriginWorld() const { return m_aimOrigin; }
	const Vector& AimForwardWorld() const { return m_aimForward; }
	bool HaveAimGeometry() const { return m_haveAimGeom; }

	void ClearCollisionContext()
	{
		m_trace = nullptr;
		m_collisionSkip = nullptr;
		m_barrelRetreat = 0.0f;
		m_deflectAngle = 0.0f;
	}

	// The viewmodel entity, for anything else that needs it in the same frame
	// window -- the animation suppressor in viewmodel_anim.h.
	//
	// Public because that work must run whether or not `followController` is on:
	// animations fight the player's arm regardless of whether the gun is pinned
	// to it, and Apply() below returns early when the feature is off.
	void* ResolveViewModel( const EngineClient& engine )
	{
		if ( !m_entities.Valid() || !engine.Valid() )
			return nullptr;
		return FindViewModel( engine );
	}

	// Called once per frame from the View_Render hook, before the eye passes.
	// `viewOrigin` is the engine's own view origin for this frame -- the base
	// the head offset is measured from, so the hand lands relative to the head.
	void Apply( const EngineClient& engine, IVRBackend& vr, const VRCamera& camera,
				const Vector& viewOrigin )
	{
		m_applied = false;
		m_lastFailure = nullptr;

		if ( !m_settings.followController || !m_entities.Valid() || !engine.Valid() )
		{
			m_lastFailure = "disabled or not bound";
			return;
		}

		// Sanity-check the vtable map LAZILY, not at bind time.
		//
		// GetMaxEntities reads a field that is only populated once the client is
		// connected, so checking it during mod init reads 0 on a perfectly good
		// interface. Doing that check at bind time and refusing permanently is
		// exactly what happened first time round: "GetMaxEntities()=0 (expected
		// 2048). Refusing to use it" -- while the map was in fact correct, as
		// the live process later confirmed. A not-ready interface must defer,
		// never latch a refusal.
		if ( !m_verified )
		{
			const int maxEnts = m_entities.GetMaxEntities();
			if ( maxEnts != 2048 )
			{
				++m_verifyAttempts;
				m_lastMaxEntities = maxEnts;
				m_lastFailure = "entity list not ready yet (waiting for a map)";
				return;
			}
			m_verified = true;
			m_lastMaxEntities = maxEnts;
			Log( "viewmodel: entity list verified after %u attempt(s) "
				 "(GetMaxEntities=%d) -- tracking active",
				 m_verifyAttempts + 1, maxEnts );
		}

		const ControllerPose& hand = vr.WeaponHand();
		if ( !hand.valid )
		{
			m_lastFailure = "weapon hand not tracked";
			return;
		}

		void* viewModel = FindViewModel( engine );
		if ( !viewModel )
		{
			// The entity is gone -- a level unload, or the weapon put away.
			// The readback below describes an entity that no longer exists, so
			// retire it rather than letting the heartbeat quote it.
			m_haveSeenPose = false;
			return;   // FindViewModel sets m_lastFailure
		}

		// ---- READ BACK LAST FRAME'S WRITE, ON THIS THREAD ------------------
		//
		// The heartbeat used to do this itself, from a cached `m_lastViewModel`
		// guarded only by a null check. That is a raw pointer into the client's
		// entity list being dereferenced from the LOG thread while the render
		// thread owns the entity's lifetime -- and it crashed exactly as you
		// would expect: ACCESS_VIOLATION reading +0x220 (m_vecAbsOrigin) one
		// heartbeat after the level unloaded, with `ingame=0` on the very line
		// above it in the log.
		//
		// Nulling the cache on failure is not enough on its own, because the
		// unload can land between the log thread's null check and its read.
		// So the entity is only ever touched HERE, on the thread that already
		// owns it, and the heartbeat reads plain floats we own.
		//
		// Read BEFORE the write, deliberately: the question the heartbeat asks
		// is "did anything overwrite our pose before the gun was drawn", and
		// only the following frame can answer it. Reading here is that read.
		{
			auto* rb = reinterpret_cast<unsigned char*>( viewModel );
			m_seenOrigin = *reinterpret_cast<Vector*>( rb + viewmodel_offset::kAbsOrigin );
			m_seenAngles = *reinterpret_cast<QAngle*>( rb + viewmodel_offset::kAbsAngles );
			m_haveSeenPose = true;
		}

		// Where the controller is, in the world, using exactly the mapping the
		// head uses so the two cannot drift apart.
		const Vector handOffset = camera.WorldOffsetForRoomPos( hand.position );

		// Composed, not added -- same reason as the aim path.
		// The SAME source the aim uses -- see VRCamera::EffectiveWeaponAngles.
		// Composing here independently is how the model and the shots end up
		// pointing in different directions.
		const QAngle local = camera.EffectiveWeaponAngles(
			hand.angles, m_settings.anglePitch, m_settings.angleYaw,
			m_settings.angleRoll );
		QAngle angles;
		angles.x = local.x;
		angles.y = camera.RoomYawToWorld( local.y );
		angles.z = local.z;

		Vector forward, right, up;
		AnglesToBasis( angles, forward, right, up );

		const ModelOffsets off = Eff();

		// ---- COLLIDE BEFORE BUILDING THE ORIGIN ----------------------------
		//
		// After the two-handed grip, not before. The grip reaches this point
		// through EffectiveWeaponAngles above, so `angles` -- and therefore
		// `forward` and everything below -- already carry it. Colliding earlier
		// would correct a pose the player never sees.
		//
		// Collided about the HAND, not about the model origin. The origin sits
		// `viewmodel_offset_forward` behind the hand (-26 by default), so it is
		// not a point on the gun at all; sweeping to it would stop the player
		// short of walls their gun had not reached.
		//
		// And done BEFORE the origin is built, because deflection ROTATES the
		// basis -- the origin is derived from `forward`/`right`/`up`, so it has
		// to be computed from the corrected ones or the model would be placed
		// on the old axis and rendered on the new.
		// `handPos`, not `hand` -- `hand` is the ControllerPose above.
		Vector handPos = { viewOrigin.x + handOffset.x,
						   viewOrigin.y + handOffset.y,
						   viewOrigin.z + handOffset.z };

		// ---- MEASURED BEFORE COLLISION, AND THAT IS THE POINT --------------
		//
		// Deflection rotates the MODEL and deliberately never touches the aim.
		// Measuring after it would trace down the deflected barrel and converge
		// the shot on whatever the tilted model happens to face -- so the gun
		// would visibly shoot somewhere the player never pointed, which is the
		// exact failure the deflection was designed to avoid.
		MeasureAim( handPos, forward );

		handPos = CollideGun( viewOrigin, handPos, forward, right, up, angles );

		// ---- THE SHOOT POSITION, AFTER COLLISION --------------------------
		//
		// Published for the trace rewrite, and taken from the COLLIDED pose on
		// purpose: the uncollided hand can be inside a wall when the player
		// presses the gun against one, and a bullet starting inside geometry
		// hits nothing at all. The collided pose is the one the player can
		// actually see, so it is the one the shot should leave from.
		//
		// The forward push is what buys the corner shot -- the further along the
		// barrel the origin sits, the further past the corner it can be while
		// the player's eye is still behind it. It is bounded by the same
		// collision, because it is measured from an already-collided hand.
		m_shootOrigin.x = handPos.x + forward.x * m_shootFwd
						  + right.x * m_shootRight + up.x * m_shootUp;
		m_shootOrigin.y = handPos.y + forward.y * m_shootFwd
						  + right.y * m_shootRight + up.y * m_shootUp;
		m_shootOrigin.z = handPos.z + forward.z * m_shootFwd
						  + right.z * m_shootRight + up.z * m_shootUp;
		m_haveShootOrigin = true;

		Vector origin;
		origin.x = handPos.x + forward.x * off.forward
				   + right.x * off.right + up.x * off.up;
		origin.y = handPos.y + forward.y * off.forward
				   + right.y * off.right + up.y * off.up;
		origin.z = handPos.z + forward.z * off.forward
				   + right.z * off.right + up.z * off.up;

		auto* base = reinterpret_cast<unsigned char*>( viewModel );
		auto* localOrigin = reinterpret_cast<Vector*>( base + viewmodel_offset::kOrigin );
		auto* localAngles = reinterpret_cast<QAngle*>( base + viewmodel_offset::kAngles );
		auto* absOrigin = reinterpret_cast<Vector*>( base + viewmodel_offset::kAbsOrigin );
		auto* absAngles = reinterpret_cast<QAngle*>( base + viewmodel_offset::kAbsAngles );

		m_before = *absOrigin;

		// LOCAL AND ABSOLUTE, for both position and rotation.
		//
		// The first version wrote local+absolute origin but only local rotation,
		// and that asymmetry was the whole bug: the position moved with the
		// controller while the model kept rotating on the engine's angles. The
		// giveaway was roll -- the controller was at roll -62 while the entity
		// read back 0.000, which is what VRCamera forces on the aim angles.
		//
		// It matters more than it looks. The offset below is built from the
		// controller's basis, so if the model renders on a different basis the
		// offset points somewhere else entirely and the model appears to pivot
		// about the wrong point -- which is exactly how it looked.
		*localOrigin = origin;
		*absOrigin = origin;
		*localAngles = angles;
		*absAngles = angles;

		// ---- and the matrix the RENDER actually reads ----------------------
		//
		// Writing the origin fields alone is not enough and that took three
		// attempts to establish. Source draws a client entity from
		// m_rgflCoordinateFrame, which CalcAbsolutePosition() builds FROM THE
		// LOCAL origin -- and the engine's CalcViewModelView rewrites local
		// every frame. So the matrix routinely holds the engine's position
		// while m_vecAbsOrigin holds ours, and the gun draws where the engine
		// put it. Measured directly: local and the matrix agreed with abs on
		// the same 2 of 5 samples, and disagreed together on the other 3.
		VerifyCoordinateFrame( base, origin, angles );
		if ( m_frameVerified && m_settings.writeCoordinateFrame )
		{
			float m[12];
			BuildCoordinateFrame( angles, origin, m );
			memcpy( base + viewmodel_offset::kCoordinateFrame, m, sizeof( m ) );
			++m_frameWrites;
		}

		ApplyToWeaponEntity( viewModel, origin, angles );

		// Feed the setter hook. Done before the raw writes so that if the engine
		// calls SetLocalOrigin during this frame it already has our pose.
		vm_setter_hook::g_target = viewModel;
		vm_setter_hook::g_origin = origin;
		vm_setter_hook::g_angles = angles;
		vm_setter_hook::g_active = m_settings.hookSetters;
		InstallSetterHooks( viewModel );

		// Remembered for ReWriteInPass() -- see below.
		m_lastOrigin = origin;
		m_lastAngles = angles;
		m_haveLastPose = true;

		m_after = *absOrigin;
		m_wanted = origin;
		m_wantedAngles = angles;
		m_applied = true;
		++m_writes;
	}

	// Verification, read on the NEXT frame rather than immediately: the question
	// is not "did the store land" -- it obviously did -- but "did anything
	// overwrite it before the gun was drawn". Only the following frame can
	// answer that, so the heartbeat compares what we asked for against what the
	// entity holds now.
	void LogState( IVRBackend* vr ) const
	{
		if ( !m_settings.followController )
			return;

		if ( !m_applied )
		{
			Log( "viewmodel: not applied (%s) verified=%d maxEnts=%d writes=%u",
				 m_lastFailure ? m_lastFailure : "Apply() never ran -- driver not bound",
				 m_verified ? 1 : 0, m_lastMaxEntities, m_writes );
			return;
		}

		// The entity itself is NEVER touched here -- see the readback in
		// Apply(). This runs on the log thread and the entity belongs to the
		// render thread; dereferencing it from here is what crashed the
		// process on a level unload.
		Vector now = { 0.0f, 0.0f, 0.0f };
		QAngle nowAng = { 0.0f, 0.0f, 0.0f };
		bool held = false;
		bool angHeld = false;
		if ( m_haveSeenPose )
		{
			now = m_seenOrigin;
			nowAng = m_seenAngles;

			const float dx = now.x - m_wanted.x;
			const float dy = now.y - m_wanted.y;
			const float dz = now.z - m_wanted.z;
			held = ( dx * dx + dy * dy + dz * dz ) < 1.0f;

			// Roll is checked deliberately, and reported separately from
			// position. The engine forces roll to 0 on the aim angles while the
			// controller almost never sits at exactly 0, so roll is the one
			// component that says unambiguously whose angles are in there --
			// pitch and yaw match closely enough under aim_source=controller to
			// prove nothing.
			const float da = nowAng.x - m_wantedAngles.x;
			const float db = nowAng.y - m_wantedAngles.y;
			const float dc = nowAng.z - m_wantedAngles.z;
			angHeld = ( da * da + db * db + dc * dc ) < 1.0f;
		}

		Log( "viewmodel: pos wrote=(%.1f %.1f %.1f) holds=(%.1f %.1f %.1f) %s | "
			 "writes=%u",
			 m_wanted.x, m_wanted.y, m_wanted.z, now.x, now.y, now.z,
			 held ? "STUCK" : "OVERWRITTEN", m_writes );

		// Cumulative on purpose -- these answer "is collision working at all",
		// not "is it happening now". A body count that never moves while the
		// player pushes the gun into a wall means the trace or the skip entity
		// is wrong, and startsolid climbing without the others means the hull is
		// too fat for the spaces this game has.
		if ( m_settings.collide )
			Log( "viewmodel: collide %s radius=%.1f barrel=%.1f response=%.2f deflect=%.0fdeg | "
				 "body=%u deflect=%u retreat=%u startsolid=%u | now: deflect %.1fdeg retreat %.1f",
				 ( m_trace && m_trace->Valid() ) ? "ON" : "NO TRACE -- walls will not stop the gun",
				 m_settings.collideRadius, m_settings.collideBarrel,
				 m_settings.collideResponse, m_settings.collideDeflect,
				 m_collideBody, m_collideDeflect, m_collideBarrel, m_collideStartSolid,
				 m_deflectAngle, m_barrelRetreat );
		Log( "viewmodel: SETTER HOOK %s -- engine called SetLocalOrigin %u times, we substituted our pose %u times (angles %u). Substitutions climbing means the engine is placing the gun where WE say, through its own code path.",
			 m_settersHooked ? ( m_settings.hookSetters ? "active" : "installed but off" )
				 : "not installed",
			 vm_setter_hook::g_originCalls, vm_setter_hook::g_originSubs,
			 vm_setter_hook::g_angleSubs );
		// Reported as EFFECT, not intent -- the two diverge in four different
		// ways here and they are indistinguishable from inside the headset.
		// This is SiN's second copy of the gun: the C_Weapon* entity carries a
		// v_ model of its own at eye height, which in VR is a gun hanging in
		// front of the player. Nothing here is about the muzzle flash; that was
		// FormatViewModelAttachment -- see viewmodel_fov_match.
		if ( !m_settings.pinWeaponEntity && !m_settings.hideWeaponEntity )
		{
			Log( "viewmodel: weapon entity -- switches OFF, so it is never looked up. "
				 "SiN's second copy of the gun is left where it is, at eye height." );
		}
		else if ( !m_weaponEntity )
		{
			LogWarn( "viewmodel: weapon entity NOT FOUND -- the viewmodel's weapon handle "
					 "at +0x%X did not resolve to a client entity. Nothing was pinned or "
					 "hidden, so the duplicate gun is still drawing at eye height.",
					 m_offWeaponHandle );
		}
		else
		{
			Log( "viewmodel: weapon entity %p -- pinned %u via the engine's SetLocalOrigin "
				 "(%u raw fallback), hidden %u. This is SiN's second copy of the gun, at "
				 "eye height; pinning puts it on the real gun and hiding stops it drawing. "
				 "NOT the muzzle flash -- that was FormatViewModelAttachment, see "
				 "viewmodel_fov_match.",
				 m_weaponEntity, m_weaponPins, m_weaponPinsRaw, m_weaponHides );
		}
		Log( "viewmodel: coordinate frame %s, %u writes -- this is what the render reads, and the origin fields alone never reached it",
			 m_frameVerified ? "CONFIRMED and being written"
				 : ( m_frameVerifyFailed ? "NOT FOUND -- flicker expected" : "verifying..." ),
			 m_frameWrites );
		Log( "viewmodel: ang wrote=(%.1f %.1f %.1f) holds=(%.1f %.1f %.1f) %s "
			 "(roll is the discriminator -- engine forces it to 0)",
			 m_wantedAngles.x, m_wantedAngles.y, m_wantedAngles.z,
			 nowAng.x, nowAng.y, nowAng.z,
			 angHeld ? "STUCK" : "OVERWRITTEN -- rotation still the engine's" );
		(void)vr;
	}

private:
	static bool Near( float a, float b ) { return fabsf( a - b ) < 0.01f; }

private:
	// One-time vtable patchprivate:
	// One-time vtable patch on the viewmodel's class. Deferred until we hold a
	// real entity, because the vtable is read from the instance.
	static Vector Cross( const Vector& a, const Vector& b )
	{
		return Vector{ a.y * b.z - a.z * b.y,
					   a.z * b.x - a.x * b.z,
					   a.x * b.y - a.y * b.x };
	}

	static float Dot( const Vector& a, const Vector& b )
	{
		return a.x * b.x + a.y * b.y + a.z * b.z;
	}

	static bool Normalize( Vector& v )
	{
		const float len = sqrtf( Dot( v, v ) );
		if ( len < 0.0001f )
			return false;
		v.x /= len; v.y /= len; v.z /= len;
		return true;
	}

	// Time-based, not a fixed per-frame fraction: a per-frame lerp would ease
	// twice as fast at 90 fps as at 45, so a feel tuned at one frame rate would
	// not survive the other.
	float EaseAlpha()
	{
		const unsigned long long now = GetTickCount64();
		float dt = m_lastCollideMs ? (float)( now - m_lastCollideMs ) * 0.001f : 0.016f;
		m_lastCollideMs = now;

		// A load screen or a hitch must not teleport the gun on resume.
		if ( dt > 0.25f )
			dt = 0.25f;

		return 1.0f - expf( -dt / kBarrelTau );
	}

	// Traces down the barrel to find what it is actually pointing at.
	//
	// A RAY, not a hull: a bullet is a point, and this is standing in for one.
	// The collision sweeps use a box because a gun is not a point, but that
	// argument does not transfer to where a shot lands.
	void MeasureAim( const Vector& handPos, const Vector& forward )
	{
		// Recorded before anything else, and unconditionally: a debug view
		// wants the gun's pose whether or not the trace found something.
		m_aimOrigin = handPos;
		m_aimForward = forward;
		m_haveAimGeom = true;

		if ( !m_settings.aimTrace || !m_trace || !m_trace->Valid() )
		{
			m_aimDistance = -1.0f;
			m_aimSmoothed = -1.0f;
			return;
		}

		const float far_ = m_settings.aimTraceMax;
		const Vector end = { handPos.x + forward.x * far_,
							 handPos.y + forward.y * far_,
							 handPos.z + forward.z * far_ };

		Vector hit;
		float fraction = 1.0f;
		float measured = -1.0f;
		if ( m_trace->Ray( handPos, end, m_collisionSkip, hit, fraction ) )
		{
			const float d = far_ * fraction;

			// A hit at essentially zero range is the muzzle already inside
			// geometry -- pressed against a wall, or the ray striking the
			// player. Converging on that would swing the aim wildly for a shot
			// that is going to hit the wall in front of the gun anyway, so the
			// fixed fallback is the better answer.
			if ( d > 8.0f )
				measured = d;
		}

		m_aimDistance = measured;

		// ---- SMOOTHED, AND ONLY LIGHTLY ------------------------------------
		//
		// Sweeping the barrel across the edge of a crate makes the traced
		// distance jump between near and far, and the convergence correction
		// scales as atan(offset / distance) -- so an unfiltered jump snaps the
		// aim. Easing the DISTANCE removes the snap while leaving the direction,
		// which is dominated by the barrel itself, untouched.
		//
		// A short time constant on purpose: during the transition the shot
		// converges somewhere between the two ranges, which is exactly what the
		// fixed distance does ALL the time. Worst case here is still better
		// than the behaviour it replaces.
		if ( measured <= 0.0f )
		{
			m_aimSmoothed = -1.0f;   // fall back cleanly rather than drifting
			return;
		}

		if ( m_aimSmoothed <= 0.0f )
			m_aimSmoothed = measured;      // first hit: adopt it outright
		else
			m_aimSmoothed += ( measured - m_aimSmoothed ) * AimAlpha();

		m_aimDistance = m_aimSmoothed;
	}

	float AimAlpha()
	{
		const unsigned long long now = GetTickCount64();
		float dt = m_lastAimMs ? (float)( now - m_lastAimMs ) * 0.001f : 0.016f;
		m_lastAimMs = now;
		if ( dt > 0.25f )
			dt = 0.25f;
		return 1.0f - expf( -dt / kAimTau );
	}

	// Sweeps the gun out of geometry. Returns where the hand may actually be,
	// and may ROTATE the basis and angles to swing the barrel clear.
	//
	// ---- THREE LAYERS, IN THIS ORDER, AND THEY ARE NOT INTERCHANGEABLE -----
	//
	//   1. BODY CLAMP    eye -> hand. A hard constraint: the hand may not end up
	//                    on the far side of a wall. This is the "stop matching
	//                    the controller" case and nothing else can cover it --
	//                    rotating a gun whose GRIP is already through a wall
	//                    only spins it inside the wall.
	//   2. DEFLECTION    swing the muzzle out along the surface normal, pivoting
	//                    about the hand. The gun stays where your hand is and
	//                    declines to put its barrel through the wall, which
	//                    reads far better than being shoved backwards.
	//   3. RETREAT       whatever deflection could not solve, taken off along
	//                    the barrel. The fallback for a barrel buried deeper
	//                    than the deflection limit can rescue -- pushed head-on
	//                    into a wall, where there is no sideways to swing to.
	//
	// Layer 3 is measured by RE-TRACING on the deflected axis rather than by
	// predicting what the rotation achieved. That costs one extra trace on
	// frames where the gun is already blocked, and it is exact instead of
	// approximately right -- so the residual is small whenever rotation worked
	// and large only when it genuinely could not.
	//
	// AIM IS NOT TOUCHED. Only the model moves. The shot still goes where the
	// controller points; deflecting the aim would mean the gun fires somewhere
	// the player never aimed, which is a far worse bug than a barrel in a wall.
	Vector CollideGun( const Vector& eye, const Vector& hand,
					   Vector& forward, Vector& right, Vector& up, QAngle& angles )
	{
		m_collidedNow = false;
		if ( !m_settings.collide || !m_trace || !m_trace->Valid() )
		{
			// Dropped rather than left standing: re-enabling collision, or
			// drawing a weapon again, must not resume a correction measured
			// against geometry from before.
			m_barrelRetreat = 0.0f;
			m_deflectAngle = 0.0f;
			return hand;
		}

		const float e = m_settings.collideRadius;
		const Vector extents = { e, e, e };
		Vector out = hand;

		// ---- 1. BODY: the engine's eye out to the hand ---------------------
		//
		// Started from the eye for the reason the head collision gives: that
		// point is by definition inside the playable space. Starting from last
		// frame's gun position would begin the sweep wherever the gun had
		// already got to, including inside a wall, and startsolid would then let
		// it keep going.
		{
			Vector hit;
			float fraction = 1.0f;
			bool startedSolid = false;
			const bool blocked = m_trace->HullEx( eye, hand, extents, m_collisionSkip,
												  hit, fraction, startedSolid );
			if ( startedSolid )
			{
				// NOT a wall -- the hull began intersecting something, and the
				// engine then reports fraction 0 whichever way the player is
				// reaching. Acting on it would snap the gun back to the face, in
				// doorways, which is where a player is most likely to be holding
				// the gun out. Left alone.
				++m_collideStartSolid;
			}
			else if ( blocked )
			{
				out.x = eye.x + ( hand.x - eye.x ) * fraction;
				out.y = eye.y + ( hand.y - eye.y ) * fraction;
				out.z = eye.z + ( hand.z - eye.z ) * fraction;
				m_collidedNow = true;
				++m_collideBody;
			}
		}

		const float len = m_settings.collideBarrel;
		if ( len <= 0.0f )
		{
			m_barrelRetreat = 0.0f;
			m_deflectAngle = 0.0f;
			return out;
		}

		const float alpha = EaseAlpha();

		// ---- 2. DEFLECT: swing the muzzle along the surface normal ---------
		float wantDeflect = 0.0f;
		Vector axis = { 0.0f, 0.0f, 1.0f };
		bool haveAxis = false;
		{
			const Vector muzzle = { out.x + forward.x * len,
									out.y + forward.y * len,
									out.z + forward.z * len };
			Vector hit, normal;
			float fraction = 1.0f;
			bool startedSolid = false, haveNormal = false;
			const bool blocked = m_trace->HullNormal( out, muzzle, extents, m_collisionSkip,
													  hit, fraction, startedSolid,
													  normal, haveNormal );
			if ( startedSolid )
			{
				++m_collideStartSolid;
			}
			else if ( blocked && haveNormal && m_settings.collideDeflect > 0.0f )
			{
				// ---- DEPTH DRIVES THE ANGLE DIRECTLY -------------------
				//
				// The first version aimed the muzzle at a point just outside
				// the surface and took whatever angle that implied. It was
				// geometrically tidy and far too subtle: the implied angle
				// saturated around 19 degrees with the whole barrel buried,
				// and the comfort response scaled even that down.
				//
				// So the angle is now the thing being asked for, not a
				// by-product. `t` is how much of the barrel is inside -- 0 at
				// the surface, 1 fully buried -- and it maps straight onto the
				// deflection.
				float t = 1.0f - fraction;
				if ( t < 0.0f ) t = 0.0f;
				if ( t > 1.0f ) t = 1.0f;

				// Shaped rather than linear, so a barrel that has barely
				// grazed a wall is left almost alone while one genuinely
				// pushed through swings hard out of the way. Above 1 the
				// response accelerates with depth, which is what "gets more
				// significant the further you push" describes.
				wantDeflect = m_settings.collideDeflect *
							  powf( t, m_settings.collideDeflectCurve );

				// Rotate forward TOWARD the surface normal. axis x forward
				// resolves to the component of the normal perpendicular to
				// forward, so a positive angle about it swings the muzzle out
				// of the surface -- checked against RotateAboutAxis rather
				// than assumed, because the wrong sign would drive the barrel
				// deeper.
				Vector a = Cross( forward, normal );
				if ( Normalize( a ) )
				{
					axis = a;
					haveAxis = true;
				}
				else
				{
					// Forward is parallel to the normal: pushed square into
					// the wall, where every direction to tilt is equally
					// arbitrary and picking one would look random. Deflection
					// genuinely cannot help here, and the retreat below is the
					// honest answer.
					wantDeflect = 0.0f;
				}
			}
		}

		// Eased in BOTH directions. The trace flips between blocked and clear
		// across a single frame, so an instant correction is a twitch going in
		// and another coming out.
		// The axis is REMEMBERED, and that is not a detail.
		//
		// When the gun is pulled clear the trace stops reporting a hit, so
		// there is no axis this frame -- and the first version then skipped the
		// rotation entirely while the eased angle was still non-zero. The gun
		// SNAPPED back to true instead of easing out, which is exactly half of
		// the smooth reverse this is supposed to have.
		if ( haveAxis )
			m_deflectAxis = axis;

		m_deflectAngle += ( wantDeflect - m_deflectAngle ) * alpha;

		if ( m_deflectAngle > 0.05f )
		{
			axis = m_deflectAxis;
			// The WHOLE basis, so roll is carried and the model does not shear.
			forward = RotateAboutAxis( forward, axis, m_deflectAngle );
			right   = RotateAboutAxis( right, axis, m_deflectAngle );
			up      = RotateAboutAxis( up, axis, m_deflectAngle );
			angles  = BasisToAngles( forward, up );
			m_collidedNow = true;
			++m_collideDeflect;
		}

		// ---- 3. RETREAT: only what deflection could not solve --------------
		float wantRetreat = 0.0f;
		{
			const Vector muzzle = { out.x + forward.x * len,
									out.y + forward.y * len,
									out.z + forward.z * len };
			Vector hit;
			float fraction = 1.0f;
			bool startedSolid = false;
			const bool blocked = m_trace->HullEx( out, muzzle, extents, m_collisionSkip,
												  hit, fraction, startedSolid );
			if ( startedSolid )
				++m_collideStartSolid;
			else if ( blocked )
			{
				wantRetreat = len * ( 1.0f - fraction ) * m_settings.collideResponse;
				++m_collideBarrel;
			}
		}

		m_barrelRetreat += ( wantRetreat - m_barrelRetreat ) * alpha;

		if ( m_barrelRetreat > 0.01f )
		{
			// Bounded by `len`, so this can never pull the gun further back than
			// one barrel length however hard a wall is pressed.
			out.x -= forward.x * m_barrelRetreat;
			out.y -= forward.y * m_barrelRetreat;
			out.z -= forward.z * m_barrelRetreat;
			m_collidedNow = true;
		}

		return out;
	}

	void InstallSetterHooks( void* viewModel )
	{
		if ( m_settersHooked || !m_settings.hookSetters || !viewModel )
			return;
		m_settersHooked = true;

		vm_setter_hook::g_originalSetOrigin =
			reinterpret_cast<vm_setter_hook::SetOriginFn>(
				vm_setter_hook::g_originHook.Install( viewModel,
					 vm_setter_hook::kSetLocalOrigin,
					 &vm_setter_hook::Detour_SetLocalOrigin ) );
		vm_setter_hook::g_originalSetAngles =
			reinterpret_cast<vm_setter_hook::SetAnglesFn>(
				vm_setter_hook::g_anglesHook.Install( viewModel,
					 vm_setter_hook::kSetLocalAngles,
					 &vm_setter_hook::Detour_SetLocalAngles ) );

		Log( "viewmodel: hooked the engine's own setters -- SetLocalOrigin at "
			"vtable slot %d (+0xE8), SetLocalAngles at slot %d (+0xF0), "
			"originals %p / %p. Both were read out of SiN's own "
			"CalcViewModelView, which calls them virtually.",
			 vm_setter_hook::kSetLocalOrigin, vm_setter_hook::kSetLocalAngles,
			 vm_setter_hook::g_originalSetOrigin, vm_setter_hook::g_originalSetAngles );

		if ( !vm_setter_hook::g_originalSetOrigin || !vm_setter_hook::g_originalSetAngles )
			LogError( "viewmodel: setter hook FAILED to install -- the gun will keep "
				"reverting to the engine's position" );
	}

	// The weapon entity's duplicate gun -- see ViewModelSettings above.
	void ApplyToWeaponEntity( void* viewModel, const Vector& origin,
			  const QAngle& angles )
	{
		if ( m_offWeaponHandle < 0 || !m_entities.Valid() )
			return;
		if ( !m_settings.pinWeaponEntity && !m_settings.hideWeaponEntity )
			return;

		const unsigned int handle = *reinterpret_cast<unsigned int*>(
			reinterpret_cast<unsigned char*>( viewModel ) + m_offWeaponHandle );
		if ( handle == 0 || handle == 0xFFFFFFFFu )
		{
			m_weaponEntity = nullptr;
			return;
		}

		void* weapon = m_entities.GetClientEntityFromHandle( handle );
		if ( !weapon )
		{
			m_weaponEntity = nullptr;
			return;
		}
		m_weaponEntity = weapon;
		auto* wb = reinterpret_cast<unsigned char*>( weapon );

		// C_BaseEntity is the root of the hierarchy, so every offset resolved
		// against the viewmodel is at the same place on any client entity.
		if ( m_settings.pinWeaponEntity )
		{
			// PREFER THE ENGINE'S OWN SETTER. The muzzle flash comes off this
			// entity's ATTACHMENT, and an attachment is derived from bones --
			// so the bone cache has to be invalidated or the flash keeps coming
			// out of the engine's pose no matter what the fields hold. That is
			// the ghosting lesson, and this is the second entity it applies to.
			if ( entity_setter::SetPose( weapon, origin, angles ) )
			{
				++m_weaponPins;
				// Deliberately NOT writing the coordinate frame here: the
				// setter makes the engine rebuild it from local, and a second
				// writer would only race the engine's own answer.
			}
			else
			{
				// Fallback: the old raw writes. Kept so an unresolvable vtable
				// degrades to the previous behaviour rather than to nothing --
				// but this path cannot fix the flash, for the reason above, and
				// it says so once rather than looking like success.
				*reinterpret_cast<Vector*>( wb + viewmodel_offset::kOrigin ) = origin;
				*reinterpret_cast<Vector*>( wb + viewmodel_offset::kAbsOrigin ) = origin;
				*reinterpret_cast<QAngle*>( wb + viewmodel_offset::kAngles ) = angles;
				*reinterpret_cast<QAngle*>( wb + viewmodel_offset::kAbsAngles ) = angles;
				if ( m_frameVerified && m_settings.writeCoordinateFrame )
				{
					float m[12];
					BuildCoordinateFrame( angles, origin, m );
					memcpy( wb + viewmodel_offset::kCoordinateFrame, m, sizeof( m ) );
				}
				++m_weaponPinsRaw;

				if ( !m_weaponSetterWarned )
				{
					m_weaponSetterWarned = true;
					LogWarn( "viewmodel: the weapon entity's SetLocalOrigin/SetLocalAngles "
							 "(slots %d/%d) could not be called -- falling back to raw field "
							 "writes. Those do NOT invalidate the bone cache, so the muzzle "
							 "flash will stay misaligned. The slots are inferred from "
							 "C_BaseEntity via the viewmodel; this build may lay them out "
							 "differently.",
							 vm_setter_hook::kSetLocalOrigin, vm_setter_hook::kSetLocalAngles );
				}
			}
		}

		if ( m_settings.hideWeaponEntity && m_offEffects >= 0 )
		{
			// EF_NODRAW. Set every frame rather than once: it is a networked
			// field, and the server's value comes back the moment it updates --
			// the same lesson m_nSequence taught.
			int* effects = reinterpret_cast<int*>( wb + m_offEffects );
			if ( ( *effects & kEfNoDraw ) == 0 )
			{
				*effects |= kEfNoDraw;
				++m_weaponHides;
			}
		}
	}

	// Prove the offset before writing 48 bytes into a live entity.
	//
	// The test is self-proving and needs no disassembly: build the matrix that
	// SHOULD be there from the entity's own abs origin and angles, and compare.
	// The engine rebuilds the frame from local on most frames, so on the
	// frames where local still holds our value the two must agree exactly. One
	// clean agreement is enough to identify the field; a run of frames with no
	// agreement at all means the offset is wrong and nothing is ever written.
	void VerifyCoordinateFrame( unsigned char* base, const Vector& origin,
			   const QAngle& angles )
	{
		if ( m_frameVerified || m_frameVerifyFrames > 600 )
		{
			if ( !m_frameVerified && !m_frameVerifyFailed )
			{
				m_frameVerifyFailed = true;
				LogWarn( "viewmodel: coordinate frame at +0x%X never matched the entity's own transform over %u frames -- the offset is wrong. NOT writing to it; the firing flicker will remain.",
					 viewmodel_offset::kCoordinateFrame, m_frameVerifyFrames );
			}
			return;
		}

		++m_frameVerifyFrames;

		float expected[12];
		BuildCoordinateFrame( angles, origin, expected );

		const float* live = reinterpret_cast<const float*>(
			base + viewmodel_offset::kCoordinateFrame );

		for ( int i = 0; i < 12; ++i )
			if ( !Near( live[i], expected[i] ) )
				return;

		m_frameVerified = true;
		Log( "viewmodel: coordinate frame CONFIRMED at +0x%X -- it matched the matrix built from the entity's own abs origin and angles, to all 12 floats, after %u frames. This is the field the render reads; writing it now.",
			 viewmodel_offset::kCoordinateFrame, m_frameVerifyFrames );
	}

	void* FindViewModel( const EngineClient& engine )
	{
		const int playerIndex = engine.GetLocalPlayer();
		if ( playerIndex <= 0 )
		{
			m_lastFailure = "no local player";
			return nullptr;
		}

		void* player = m_entities.GetClientEntity( playerIndex );
		if ( !player )
		{
			m_lastFailure = "local player entity not found";
			return nullptr;
		}

		const unsigned int handle = *reinterpret_cast<unsigned int*>(
			reinterpret_cast<unsigned char*>( player ) + player_offset::kViewModelHandle );
		if ( handle == 0 || handle == 0xFFFFFFFFu )
		{
			m_lastFailure = "no viewmodel handle (weapon holstered?)";
			return nullptr;
		}

		void* vm = m_entities.GetClientEntityFromHandle( handle );
		if ( !vm )
		{
			m_lastFailure = "viewmodel handle did not resolve";
			return nullptr;
		}

		m_lastViewModel = vm;
		return vm;
	}

	// Small fixed table: SiN ships exactly three viewmodels, and a fourth
	// slot means an unexpected one still gets tuned rather than ignored.
	int FindOrAdd( const char* key )
	{
		if ( !key || !*key )
			return -1;
		for ( int i = 0; i < m_modelCount; ++i )
			if ( _stricmp( m_models[i].key, key ) == 0 )
				return i;
		if ( m_modelCount >= (int)( sizeof( m_models ) / sizeof( m_models[0] ) ) )
		{
			LogWarn( "viewmodel: no free per-weapon offset slot for \"%s\" -- it "
				"will use the global offsets", key );
			return -1;
		}
		const int i = m_modelCount++;
		strncpy_s( m_models[i].key, sizeof( m_models[i].key ), key, _TRUNCATE );
		return i;
	}

	ViewModelSettings m_settings;
	ClientEntityList m_entities;
	ModelOffsets m_models[4];
	int m_modelCount = 0;
	int m_current = -1;
	int m_diagnoseRuns = 0;
	bool m_frameVerified = false;
	bool m_frameVerifyFailed = false;
	unsigned int m_frameVerifyFrames = 0;
	unsigned int m_frameWrites = 0;
	int m_offWeaponHandle = -1;
	int m_offEffects = -1;
	void* m_weaponEntity = nullptr;
	unsigned int m_weaponPins = 0;      // through the engine's setter -- the real path
	unsigned int m_weaponPinsRaw = 0;   // raw field fallback -- cannot fix the flash
	unsigned int m_weaponHides = 0;
	bool m_weaponSetterWarned = false;
	Vector m_lastOrigin = { 0.0f, 0.0f, 0.0f };
	QAngle m_lastAngles = { 0.0f, 0.0f, 0.0f };
	bool m_haveLastPose = false;
	bool m_settersHooked = false;
	static const int kEfNoDraw = 0x020;

	void* m_lastViewModel = nullptr;
	// Last frame's readback of the entity's abs pose, taken on the RENDER
	// thread so the log thread never has to dereference the entity. See the
	// readback block in Apply().
	Vector m_seenOrigin = { 0.0f, 0.0f, 0.0f };
	QAngle m_seenAngles = { 0.0f, 0.0f, 0.0f };
	bool m_haveSeenPose = false;

	// Collision. Borrowed, never owned -- see SetCollisionContext.
	EngineTrace* m_trace = nullptr;
	void* m_collisionSkip = nullptr;
	bool m_collidedNow = false;
	// Smoothed barrel retreat, in units, and the clock that eases it. See the
	// easing block in CollideGun.
	float m_barrelRetreat = 0.0f;
	float m_deflectAngle = 0.0f;
	float m_aimDistance = -1.0f;
	Vector m_shootOrigin = { 0.0f, 0.0f, 0.0f };
	float m_shootRight = 0.0f;
	float m_shootUp = 0.0f;
	float m_shootFwd = 0.0f;
	bool m_haveShootOrigin = false;
	float m_aimSmoothed = -1.0f;
	Vector m_aimOrigin = { 0.0f, 0.0f, 0.0f };
	Vector m_aimForward = { 1.0f, 0.0f, 0.0f };
	bool m_haveAimGeom = false;
	unsigned long long m_lastAimMs = 0;
	Vector m_deflectAxis = { 0.0f, 0.0f, 1.0f };
	unsigned long long m_lastCollideMs = 0;
	unsigned int m_collideBody = 0;
	unsigned int m_collideBarrel = 0;
	unsigned int m_collideDeflect = 0;
	unsigned int m_collideStartSolid = 0;
	Vector m_wanted = { 0.0f, 0.0f, 0.0f };
	Vector m_before = { 0.0f, 0.0f, 0.0f };
	Vector m_after = { 0.0f, 0.0f, 0.0f };
	bool m_verified = false;
	unsigned int m_verifyAttempts = 0;
	int m_lastMaxEntities = -1;
	bool m_applied = false;
	const char* m_lastFailure = nullptr;
	unsigned int m_writes = 0;
	float m_step = 1.0f;
	AdjustMode m_mode = kAdjustPosition;
	QAngle m_wantedAngles = { 0.0f, 0.0f, 0.0f };
};

} // namespace sinvr
