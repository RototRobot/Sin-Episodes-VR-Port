#pragma once
//-----------------------------------------------------------------------------
// CAN A BULLET TRACE BE RECOGNISED? -- an observational probe, nothing more.
//
// ---- THE QUESTION ----------------------------------------------------------
//
// Goal 1 is for shots to leave the GUN rather than the eye: one ray, down the
// barrel, no convergence and no per-weapon bore values. The obvious way there is
// CBasePlayer::Weapon_ShootPosition, and that is exactly the archaeology this
// project keeps refusing -- a vtable with a couple of hundred unnamed entries,
// where a wrong slot is a crash and a right-looking wrong slot is worse.
//
// There is a better-shaped route. Bullets resolve through the SERVER's
// IEngineTrace, which is a named interface from a factory -- the same shape as
// every other interface this mod binds. If a bullet trace can be told apart from
// the thousands of other traces the engine runs every tick, its start point can
// be moved to the muzzle and CBasePlayer never has to be touched at all.
//
// "If" is the whole question, so this measures it and changes NOTHING.
//
// ---- WHY THIS CANNOT JUST LOG EVERY TRACE ----------------------------------
//
// TraceRay is one of the hottest functions in the engine -- movement, AI vision,
// physics and rendering all go through it, thousands of times a second. Logging
// unconditionally would produce a gigabyte of noise and change the frame timing
// enough to alter what it is measuring.
//
// So it is ARMED by the trigger. m_nButtons lives at CMoveData +36 (identified
// by the delta probe, see HANDOVER), and IN_ATTACK is bit 0. On the tick the
// button goes down the probe arms; the traces that follow within that command
// are logged, then it disarms. Firing happens in PostThink, AFTER
// ProcessMovement returns, so the window lands on the right side of the shot.
//
// Movement traces land in the same window and are logged too. That is
// deliberate: the question is whether bullets are DISTINGUISHABLE, and a sample
// containing only bullets could not answer it.
//
// ---- WHAT THE ANSWER WILL LOOK LIKE ----------------------------------------
//
// The contents mask is the likely discriminator -- movement sweeps use
// MASK_PLAYERSOLID (0x1030B) and bullets MASK_SHOT (0x4600B). If the log shows a
// clean split, the rewrite is gated on the mask plus a start point at the eye,
// and it is cheap and safe. If bullets share a mask with the AI's vision traces
// the idea is dead, and we will have learned that from a log rather than from a
// broken build.
//-----------------------------------------------------------------------------

#include <windows.h>
#include <math.h>

#include "source_interfaces.h"
#include "engine_trace.h"
#include "interface_list.h"
#include "../hooks/vtable_hook.h"
#include "../../common/log.h"

namespace sinvr {

// Same class as the client's, so the same slot. The client interface logs
// "TraceRay = slot 4" on every launch, which is a free check on this constant.
constexpr int kTraceRaySlot = 4;

class ShotProbe
{
public:
	void SetEnabled( bool on ) { m_enabled = on; }
	void SetRewrite( bool on ) { m_rewrite = on; }
	bool Rewriting() const { return m_rewrite; }
	unsigned int Moved() const { return m_moved; }

	// The muzzle, in world space, pushed in from the client each frame. Invalid
	// whenever there is no tracked weapon hand -- and an invalid muzzle must
	// leave the ray alone rather than fall back to the origin, which is the
	// middle of the map.
	void SetMuzzle( const Vector& world, bool valid )
	{
		m_muzzle = world;
		m_haveMuzzle = valid;
	}

	// ---- IS THIS THE PLAYER SHOOTING? -----------------------------------
	//
	// Measured, not assumed. The probe run of 2026-09-06 showed exactly two
	// MASK_SHOT rays per trigger pull starting at eyeDist 0.00 -- one at the
	// press, one at the discharge ~45 ms later -- against MASK_PLAYERSOLID hull
	// sweeps for movement and MASK_OPAQUE_AND_NPCS for sight checks.
	//
	// The EYE-DISTANCE test is the one that matters most and is easiest to leave
	// out. Other MASK_SHOT rays appear in the same window at eyeDist ~1700:
	// those are NPCs firing. Without this gate every enemy in the level would
	// shoot out of the player's gun.
	bool IsPlayerBullet( const RayT* r, unsigned int mask ) const
	{
		if ( mask != kMaskShot || !r->isRay )
			return false;
		if ( fabsf( r->extents.x ) + fabsf( r->extents.y )
			 + fabsf( r->extents.z ) > 0.01f )
			return false;

		const float ox = r->start.x + r->startOffset.x;
		const float oy = r->start.y + r->startOffset.y;
		const float oz = r->start.z + r->startOffset.z;
		const float dx = ox - m_eye.x, dy = oy - m_eye.y, dz = oz - m_eye.z;
		if ( dx * dx + dy * dy + dz * dz > kEyeSlack * kEyeSlack )
			return false;

		// A short MASK_SHOT ray from the eye is not a bullet -- melee and
		// point-blank checks live there too, and moving those to the muzzle
		// would change behaviour we were not asked to change.
		const float len2 = r->delta.x * r->delta.x + r->delta.y * r->delta.y
						   + r->delta.z * r->delta.z;
		return len2 > ( 512.0f * 512.0f );
	}

	bool WantsRewrite() const { return m_rewrite && m_haveMuzzle; }
	void CountMove() { ++m_moved; }
	const Vector& Muzzle() const { return m_muzzle; }
	bool Enabled() const { return m_enabled; }
	bool Bound() const { return m_iface != nullptr; }
	int Shots() const { return m_shots; }

	bool Bind();
	bool Hook();

	// Where the engine believes the player's eye is, for the distance column --
	// the number that says whether a trace starts AT the shooter.
	void SetEye( const Vector& eye ) { m_eye = eye; }

	// ---- ARMED BY THE TRIGGER -------------------------------------------
	//
	// Called from ProcessMovement. Firing happens later in the same command, so
	// arming here and disarming on the next tick brackets exactly one shot.
	void OnTick( bool attackDown )
	{
		if ( m_armed )
		{
			// The previous tick's window closes here, reported so the log shows
			// where one shot's traces end and the next begins.
			Log( "shot: --- end of window, %d trace(s) seen ---", m_seen );
			m_armed = false;
		}

		const bool pressed = attackDown && !m_attackWas;
		m_attackWas = attackDown;

		if ( !pressed || !m_enabled || m_shots >= kMaxShots )
			return;

		++m_shots;
		m_armed = true;
		m_seen = 0;
		Log( "shot: --- IN_ATTACK pressed (%d/%d), eye=(%.1f %.1f %.1f) ---",
			 m_shots, kMaxShots, m_eye.x, m_eye.y, m_eye.z );

		if ( m_shots == kMaxShots )
			Log( "shot: this is the last window. Set shot_probe = 0 when done." );
	}

	// Cheap when disarmed: two loads and a branch. This runs thousands of times
	// a second on the engine's hottest path and must not cost anything there.
	bool Wants() const { return m_armed && m_seen < kMaxPerShot; }

	void NoteTrace( const void* ray, unsigned int mask );

private:
	static const int kMaxShots = 6;
	static const int kMaxPerShot = 24;
	// MASK_SHOT, confirmed bit-for-bit against the probe log rather than copied
	// from a header we do not have: SOLID|WINDOW|MOVEABLE|MONSTER|DEBRIS|HITBOX.
	static const unsigned int kMaskShot = 0x46004003u;
	// How close to the eye a ray must start to be the player's own shot. The
	// measured value is 0.00; this is slack for the view offset moving between
	// the tick and the trace, not a tolerance anything depends on.
	static constexpr float kEyeSlack = 4.0f;

	void* m_iface;
	bool m_enabled;
	bool m_armed;
	bool m_attackWas;
	int m_shots;
	int m_seen;
	Vector m_eye;
	Vector m_muzzle;
	bool m_haveMuzzle;
	bool m_rewrite;
	unsigned int m_moved;
	VTableHook m_hook;

public:
	ShotProbe()
		: m_iface( 0 ), m_enabled( false ), m_armed( false ), m_attackWas( false ),
		  m_shots( 0 ), m_seen( 0 ), m_haveMuzzle( false ), m_rewrite( false ),
		  m_moved( 0 )
	{
		m_eye.x = m_eye.y = m_eye.z = 0.0f;
		m_muzzle.x = m_muzzle.y = m_muzzle.z = 0.0f;
	}
};

inline ShotProbe& Shots()
{
	static ShotProbe p;
	return p;
}

typedef void( __fastcall* TraceRayFn )( void*, void*, const void*, unsigned int,
										void*, void* );
inline TraceRayFn g_originalTraceRay = nullptr;

inline void __fastcall Detour_TraceRay( void* thisptr, void* edx, const void* ray,
										unsigned int mask, void* filter, void* trace )
{
	if ( Shots().Wants() )
		Shots().NoteTrace( ray, mask );

	// ---- THE SHOT, MOVED TO THE MUZZLE ---------------------------------
	//
	// The cheap tests are first and deliberately so: this function resolves
	// every bullet, footstep, line of sight and physics query in the game, so
	// the common path through here must be a couple of loads and a branch.
	if ( ray && Shots().WantsRewrite() )
	{
		__try
		{
			const RayT* r = (const RayT*)ray;
			if ( Shots().IsPlayerBullet( r, mask ) )
			{
				// ---- A COPY, NOT THE ENGINE'S OWN Ray_t ------------------
				//
				// The caller built this on its stack and may well use it again
				// after the trace returns -- for the tracer effect, a second
				// penetration trace, decal placement. Editing it in place would
				// change all of those silently and at a distance, which is the
				// hardest class of bug this project could give itself.
				//
				// A copy costs 50 bytes of stack and makes the change local to
				// exactly the one call we meant it for.
				RayT moved = *r;

				// The ray's true origin is start + startOffset, so the muzzle
				// goes into `start` MINUS the offset to leave the sum correct.
				// Setting start to the muzzle outright would be wrong by
				// startOffset on any ray that has one.
				const Vector& m = Shots().Muzzle();
				moved.start.x = m.x - r->startOffset.x;
				moved.start.y = m.y - r->startOffset.y;
				moved.start.z = m.z - r->startOffset.z;

				// delta is left ALONE. It carries both the direction and the
				// range, and the direction is already the gun's axis because
				// shot_from_gun stops the convergence from bending it. Aiming
				// the delta at the old endpoint instead would re-introduce the
				// eye-to-muzzle skew this whole change exists to remove.

				Shots().CountMove();

				if ( g_originalTraceRay )
					g_originalTraceRay( thisptr, edx, &moved, mask, filter, trace );
				return;
			}
		}
		__except ( EXCEPTION_EXECUTE_HANDLER )
		{
			// Fall through to the unmodified call below. A bad read here must
			// cost the player one ordinary bullet, not the game.
			LogError( "shot: faulted testing a ray -- rewrite DISABLED" );
			Shots().SetRewrite( false );
		}
	}

	// Unconditional, and this one matters more than most: a path through this
	// function that does not call the original is not a bug, it is the game
	// ceasing to work.
	if ( g_originalTraceRay )
		g_originalTraceRay( thisptr, edx, ray, mask, filter, trace );
}

inline bool ShotProbe::Bind()
{
	if ( m_iface )
		return true;
	if ( !m_enabled )
		return false;

	// Tried in order. The server's trace is conventionally EngineTraceServer003,
	// but a version string is exactly the kind of thing that gets guessed wrong,
	// and this project has a tool for not guessing.
	static const char* kNames[] = { "EngineTraceServer003", "EngineTraceServer004",
									"EngineTraceServer002" };
	for ( int i = 0; i < 3; ++i )
	{
		m_iface = GetInterface( "engine.dll", kNames[i] );
		if ( m_iface )
		{
			Log( "shot: %s bound at %p", kNames[i], m_iface );
			return true;
		}
	}

	// Not a silent failure. The interface walker exists precisely so that "the
	// name is wrong" and "there is no such interface" are different answers.
	LogWarn( "shot: no EngineTraceServer under any expected name -- dumping "
			 "engine.dll's interfaces so the real one is visible" );
	LogModuleInterfaces( "engine.dll" );
	m_enabled = false;      // do not repeat the dump every frame
	return false;
}

inline bool ShotProbe::Hook()
{
	if ( !m_iface || g_originalTraceRay )
		return g_originalTraceRay != nullptr;

	g_originalTraceRay = reinterpret_cast<TraceRayFn>(
		m_hook.Install( m_iface, kTraceRaySlot, &Detour_TraceRay ) );

	if ( !g_originalTraceRay )
	{
		LogError( "shot: failed to hook TraceRay (slot %d)", kTraceRaySlot );
		return false;
	}

	Log( "shot: hooked server TraceRay (slot %d, orig=%p) -- OBSERVATIONAL, "
		 "no ray is modified", kTraceRaySlot, g_originalTraceRay );
	return true;
}

// ---- WHAT A TRACE LOOKS LIKE FROM HERE -------------------------------------
//
// Ray_t's true origin is `start + startOffset`, not `start`. For a ray (as
// opposed to a swept hull) startOffset is zero and the two agree, but printing
// the sum costs nothing and printing only `start` would quietly mislead on the
// hull traces that share this window.
//
// `delta` carries both direction and LENGTH, so its magnitude separates a
// bullet's several-thousand-unit reach from a movement sweep's few units -- a
// second discriminator if the mask alone is not enough.
inline void ShotProbe::NoteTrace( const void* rayPtr, unsigned int mask )
{
	__try
	{
		const RayT* r = (const RayT*)rayPtr;

		const float ox = r->start.x + r->startOffset.x;
		const float oy = r->start.y + r->startOffset.y;
		const float oz = r->start.z + r->startOffset.z;

		const float len = sqrtf( r->delta.x * r->delta.x + r->delta.y * r->delta.y
								 + r->delta.z * r->delta.z );

		const float dx = ox - m_eye.x, dy = oy - m_eye.y, dz = oz - m_eye.z;
		const float fromEye = sqrtf( dx * dx + dy * dy + dz * dz );

		const float ext = fabsf( r->extents.x ) + fabsf( r->extents.y )
						  + fabsf( r->extents.z );

		Log( "shot:  [%2d] mask=0x%06X  %s  len=%8.1f  from=(%.1f %.1f %.1f)  "
			 "eyeDist=%7.2f  ext=%.1f%s",
			 m_seen, mask,
			 r->isRay ? "ray " : "HULL", len, ox, oy, oz, fromEye, ext,
			 // The signature we are hoping to find: a long thin ray starting
			 // exactly where the player's eye is. That is a bullet.
			 ( r->isRay && ext < 0.01f && len > 512.0f && fromEye < 4.0f )
				 ? "   <== LOOKS LIKE A BULLET" : "" );
		++m_seen;
	}
	__except ( EXCEPTION_EXECUTE_HANDLER )
	{
		LogError( "shot: faulted reading Ray_t -- the layout is wrong. DISABLING." );
		m_seen = kMaxPerShot;
		m_armed = false;
		m_enabled = false;
	}
}

} // namespace sinvr
