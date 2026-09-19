#pragma once
//-----------------------------------------------------------------------------
// IGameMovement -- the server's movement code, and the route to true 6DoF.
//
// ---- WHY THE SERVER, AND WHY THIS IS NOT THE SCARY VERSION ------------------
//
// Room-scale needs the PLAYER to move, not just the camera. The camera already
// follows the head and is already collided (see positional_collide), but the
// engine's own idea of where the player is does not move -- which is why shots
// come from the body rather than the head, and why leaning past a corner does
// not let you shoot round it.
//
// The client cannot fix that. `IInput` is not exposed -- the interface dump of
// 2026-09-05 lists all 55 of client.dll's interfaces and there is nothing
// input-shaped among them -- so the CUserCmd is out of reach without a
// signature scan. And client-side GameMovement is the PREDICTION copy, which
// single-player does not run.
//
// The same dump found `GameMovement001` in **server.dll**, which is
// authoritative. That makes this a NAMED INTERFACE FROM A FACTORY, exactly like
// every other interface this mod binds -- not vtable archaeology on CBasePlayer,
// and not a byte pattern that goes stale.
//
//     class IGameMovement {
//         virtual ~IGameMovement();
//         virtual void ProcessMovement( CBasePlayer*, CMoveData* );
//         virtual void Reset();
//         virtual void StartTrackPredictionErrors( CBasePlayer* );
//         virtual void FinishTrackPredictionErrors( CBasePlayer* );
//         virtual void DiffPrint( const char*, ... );
//     };
//
// Hooking ProcessMovement means the ENGINE'S OWN collision resolves whatever we
// add, and everything downstream -- EyePosition, use traces, enemy awareness --
// follows because the player genuinely moved.
//
// ---- THIS FILE MEASURES. IT DOES NOT MOVE ANYTHING. -------------------------
//
// Two things have to be verified before a single byte is written, and both were
// nearly assumed:
//
//   1. WHICH SLOT ProcessMovement is. It is probably 1 -- MSVC puts the scalar
//      deleting destructor at 0 -- but this game has shifted vtable slots in
//      four other interfaces, and IMaterialSystem's shift is not even uniform
//      across the class. So the vtable is dumped and read.
//
//   2. WHERE CMoveData's fields are. The layout differs between branches, and
//      the ones that matter (m_vecAbsOrigin, m_vecVelocity, m_flForwardMove)
//      cannot be guessed. So a window of the struct is dumped as floats and
//      correlated against values we already know from elsewhere.
//
// The detour calls through unconditionally on every path. Nothing downstream
// sees a different CMoveData.
//-----------------------------------------------------------------------------

#include <windows.h>
#include <string.h>

#include "source_interfaces.h"
#include "engine_trace.h"
#include "shot_probe.h"
#include "../hooks/vtable_hook.h"
#include "../../common/log.h"

namespace sinvr {

// How much of CMoveData to sample. Generous: the fields we want sit in the
// first couple of hundred bytes in every branch this could be, and reading a
// little past the end of a struct we do not own is safe inside __try while
// guessing too short is a silent dead end.
constexpr int kMoveDataWindow = 320;

// Measured 2026-09-05 by asking which bytes change when the player moves, and
// confirmed by the 64-unit standing gap between it and the view origin. See the
// table in HANDOVER.md; it is not a guess and it is not a signature.
constexpr int kMoveDataAbsOrigin = 100;

// The button bitfield, from the same delta probe. IN_ATTACK is bit 0 in every
// Source branch -- it is part of the network protocol, not a compile-time
// detail, so this is one of the few constants here that is not a measurement.
constexpr int kMoveDataButtons = 36;
constexpr int kInAttack = 1;

struct SixDofSettings
{
	// Master write enable. Separate from the probe's own enable on purpose:
	// binding and hooking ProcessMovement is observational and safe, WRITING to
	// the move data is the part that changes the game. Being able to run one
	// without the other is what makes a bad build diagnosable.
	bool body = false;

	// ---- LEAN versus WALK, which is the whole design ---------------------
	//
	// Below the deadzone the head moves and the body does not: that is LEANING,
	// and it is what lets you put your eye past a corner without walking your
	// hitbox into it. Beyond it the body chases, so the offset can never exceed
	// the deadzone for long: that is WALKING the play space.
	//
	// Without a deadzone every lean would drag the player's collision hull
	// forward and peeking round cover would be impossible -- the feature would
	// take away the thing it was asked for.
	float deadzone = 12.0f;      // units (~30 cm)

	// How fast the body closes the gap, units/sec -- a CEILING on the chase
	// below, not the mechanism. Real walking is around 40 u/s.
	float rate = 120.0f;

	// ---- WHY THE CHASE IS PROPORTIONAL AND NOT A CONSTANT SPEED ---------
	//
	// Fraction of the remaining gap closed each tick. A constant-speed chase
	// runs at full pelt right up to the deadzone edge and then stops dead,
	// and any overshoot past the edge reverses -- so the body hunts about the
	// boundary and the compensation hunts with it.
	//
	// Closing a FRACTION makes the step shrink as the gap does, so it arrives
	// asymptotically and cannot overshoot at all. It is also why the residual
	// jitter is small where it matters: near the deadzone the steps are tiny.
	float chase = 0.15f;

	// Hard per-tick ceiling, independent of rate and dt. A hitch that produces
	// a huge dt must not produce a huge step.
	float maxStep = 8.0f;

	// ---- ONCE IT FOLLOWS, IT FOLLOWS ALL THE WAY ------------------------
	//
	// How close the body gets before a chase ends. The deadzone above only
	// STARTS a chase; see PreMove for the measurement that split the two.
	float settle = 1.0f;         // units

	// ---- A LEAN GOES SIDEWAYS, A WALK GOES FORWARD ----------------------
	//
	// `deadzone` is the SIDE-TO-SIDE radius, measured in the frame the head is
	// facing. Forward and back get this fraction of it, so a step ahead starts
	// the body sooner than the same distance sideways: people lean round a
	// corner and walk forward, and the shape follows that. 1.0 is the old
	// circle.
	float forwardRatio = 0.65f;

	// Extra side-to-side room per degree of head tilt TOWARD the side the head
	// has moved to. Leaning tips the head over; stepping sideways keeps it
	// upright -- so a tilted head is a lean, and gets more room before the body
	// follows. Capped at one extra deadzone. 0 turns it off.
	float leanTilt = 0.5f;       // units per degree

	// Seconds for the body to get from standing to full `rate`. See PreMove:
	// without it a chase started at full speed the instant the head crossed
	// the deadzone. 0 is that instant start.
	float ramp = 0.3f;
};

class GameMovementProbe
{
public:
	// Bound lazily -- server.dll only exists once a map is loaded, so binding at
	// startup would latch a permanent failure. That mistake has cost this
	// project two sessions already.
	bool Bind()
	{
		if ( m_iface )
			return true;
		if ( !m_enabled )
			return false;

		m_iface = GetInterface( "server.dll", "GameMovement001" );
		if ( !m_iface )
			return false;

		Log( "gamemovement: GameMovement001 bound at %p", m_iface );
		DumpVTable();
		return true;
	}

	void SetEnabled( bool on ) { m_enabled = on; }
	bool Enabled() const { return m_enabled; }
	bool Bound() const { return m_iface != nullptr; }

	// ---- THE 6DoF WRITE PATH --------------------------------------------
	void SetSixDof( const SixDofSettings& s ) { m_six = s; }
	const SixDofSettings& SixDof() const { return m_six; }
	void SetTrace( EngineTrace* t, void* skip ) { m_trace = t; m_traceSkip = skip; }

	// How far the body is BEHIND the head, world XY. Pushed in from the camera
	// each frame; cleared when tracking is not live so a stale target cannot
	// keep driving the player after tracking drops.
	// The head's world yaw and raw roll come with it: the deadzone is shaped
	// in the frame the head faces, and a tilt toward the lean widens it. See
	// SixDofSettings.
	void SetBodyTarget( const Vector& headOffsetWorld, bool valid,
						float headYawDeg, float headRollDeg )
	{
		m_target = headOffsetWorld;
		m_haveTarget = valid;
		m_headYaw = headYawDeg;
		m_headRoll = headRollDeg;
	}

	// Drains the distance the body actually covered since the last call. The
	// camera shifts its reference by exactly this, so the view stays still.
	//
	// DRAINED, not read: ticks and frames do not correspond one-to-one -- the
	// server can run several ticks between two renders, or none -- and a value
	// that is read without being cleared would be applied twice, moving the view
	// by the difference. That is the failure this whole feature is trying to
	// avoid, so it must not be reintroduced by the plumbing.
	Vector TakeAchieved()
	{
		const Vector v = m_achieved;
		m_achieved = { 0.0f, 0.0f, 0.0f };
		return v;
	}

	bool Moving() const { return m_moved; }
	unsigned int StepCount() const { return m_steps; }
	unsigned int BlockedCount() const { return m_blocked; }
	unsigned int ChaseCount() const { return m_chases; }

	// Called from the detour, around the original.
	void PreMove( void* moveData );

	// Reads IN_ATTACK so the shot probe can bracket a single trigger pull.
	// Guarded, because it reads a struct we do not own on a path that runs every
	// tick -- and because a fault here would take the game down mid-firefight.
	bool AttackDown( void* moveData ) const
	{
		if ( !moveData )
			return false;
		__try
		{
			const int buttons =
				*(const int*)( (const unsigned char*)moveData + kMoveDataButtons );
			return ( buttons & kInAttack ) != 0;
		}
		__except ( EXCEPTION_EXECUTE_HANDLER )
		{
			return false;
		}
	}

	// The eye origin, so the CMoveData correlator has something to match
	// against. Pushed in rather than read here: this file has no business
	// reaching into the view.
	void SetReference( const Vector& eye ) { m_eye = eye; m_haveEye = true; }

	bool Hook( int slot );
	void Note( void* moveData );

private:
	void DumpVTable()
	{
		void** vt = nullptr;
		__try
		{
			vt = *reinterpret_cast<void***>( m_iface );
		}
		__except ( EXCEPTION_EXECUTE_HANDLER )
		{
			LogError( "gamemovement: could not read the vtable" );
			return;
		}

		const HMODULE mod = GetModuleHandleA( "server.dll" );
		const uintptr_t base = (uintptr_t)mod;

		// Six declared slots; eight dumped so an extra appended one is visible
		// rather than silently shifting everything below it.
		for ( int i = 0; i < 8; ++i )
		{
			__try
			{
				void* fn = vt[i];
				unsigned char* p = (unsigned char*)fn;
				Log( "gamemovement: vtable[%d] = %p  (server.dll+0x%06X)  "
					 "%02X %02X %02X %02X %02X %02X",
					 i, fn, (unsigned)( (uintptr_t)fn - base ),
					 p[0], p[1], p[2], p[3], p[4], p[5] );
			}
			__except ( EXCEPTION_EXECUTE_HANDLER )
			{
				Log( "gamemovement: vtable[%d] -- not readable, stopping", i );
				break;
			}
		}
	}

	void* m_iface = nullptr;
	bool m_enabled = false;
	Vector m_eye = { 0.0f, 0.0f, 0.0f };
	bool m_haveEye = false;
	// Delta tracking. See Note().
	unsigned char m_prev[kMoveDataWindow] = {};
	bool m_havePrev = false;
	unsigned int m_changes[kMoveDataWindow / 4] = {};
	int m_liveTicks = 0;
	bool m_reported = false;
	VTableHook m_hook;

	SixDofSettings m_six;
	EngineTrace* m_trace = nullptr;
	void* m_traceSkip = nullptr;
	Vector m_target = { 0.0f, 0.0f, 0.0f };
	bool m_haveTarget = false;
	Vector m_achieved = { 0.0f, 0.0f, 0.0f };
	bool m_moved = false;
	unsigned int m_steps = 0;
	unsigned int m_blocked = 0;
	// Between a start past the deadzone and a stop inside `settle`. See PreMove.
	bool m_chasing = false;
	unsigned int m_chases = 0;
	// Pushed in with the target: the deadzone's frame and the lean tilt.
	float m_headYaw = 0.0f;
	float m_headRoll = 0.0f;
	// Length of the last applied step, which the ramp grows from.
	float m_lastStep = 0.0f;
	bool m_warnedNoTrace = false;

	friend struct GameMovementDetour;
};

inline GameMovementProbe& Movement()
{
	static GameMovementProbe p;
	return p;
}

using ProcessMovementFn = void( __fastcall* )( void*, void*, void*, void* );
inline ProcessMovementFn g_originalProcessMovement = nullptr;

inline void __fastcall Detour_ProcessMovement( void* thisptr, void* edx,
											   void* player, void* moveData )
{
	Movement().Note( moveData );

	// BEFORE the original, so the engine's own movement code runs from the
	// position we placed and resolves anything our step got wrong. Writing
	// afterwards would put the player somewhere the engine had no chance to
	// validate, which is the difference between a step and a teleport.
	Movement().PreMove( moveData );

	// Bracket the shot for the trace probe. Here rather than in PreMove because
	// it is a different concern with a different switch, and PreMove returns
	// early on several paths that have nothing to do with the trigger.
	Shots().OnTick( Movement().AttackDown( moveData ) );

	if ( g_originalProcessMovement )
		g_originalProcessMovement( thisptr, edx, player, moveData );
}

// ---- WALKING THE PLAY SPACE -------------------------------------------------
//
// Each tick, close part of the gap between where the engine thinks the player
// is and where their head physically is. The camera then shifts its reference
// by the same amount, so the two cancel and the rendered view does not move.
//
// ---- WHY THIS IS A STEP AND NOT AN ASSIGNMENT ------------------------------
//
// Setting m_vecAbsOrigin straight to the head position would TELEPORT. The
// engine resolves the movement IT performs; it does not sweep to a position
// that has already been set, so the player could arrive inside a wall and the
// only thing that would notice is the player.
//
// So the move is swept first, with a hull, and the traced fraction is what
// actually gets applied. The destination is already known to be reachable --
// positional_collide swept the HEAD there and the head is what we are chasing
// -- so this sweep is about the PATH, and a path a few units long is one the
// trace can be trusted on.
//
// ---- AND WHY IT REPORTS WHAT IT DID, NOT WHAT IT INTENDED ------------------
//
// m_achieved accumulates the applied step, never the requested one. If the body
// is blocked and the camera decays its reference anyway, the view lurches by
// exactly the amount the body failed to move -- a jolt that happens precisely
// when the player is pressed against geometry. Blocked has to mean "the head
// stays leaned out", which is both correct and what the player expects.
inline void GameMovementProbe::PreMove( void* moveData )
{
	m_moved = false;

	if ( !moveData || !m_six.body || !m_haveTarget )
	{
		// A menu, a cutscene or lost tracking ends any chase in progress, so the
		// next one has to earn its start at the deadzone like any other.
		m_chasing = false;
		m_lastStep = 0.0f;
		return;
	}

	const float len = sqrtf( m_target.x * m_target.x + m_target.y * m_target.y );

	// ---- THE DEADZONE STARTS A CHASE; IT DOES NOT END ONE ---------------
	//
	// Until 2026-09-13 the chase closed the gap only DOWN TO the deadzone, so it
	// parked the body 12 units short of the head after every walk. Measured on
	// se1_docks01: each time the player stopped, the head sat 11.2 to 13.1
	// units out. That is 30 cm of body missing in whichever direction you last
	// walked -- a wall you then approached with the stick stopped you that much
	// early (or let you that much closer), and only a recentre put the two back
	// together.
	//
	// So the two jobs are split. The deadzone decides when a chase STARTS:
	// from a standstill, inside it, the head moves alone -- that is leaning, and
	// it is unchanged. Once started, the chase runs until the body is within
	// `settle` of the head. The band between the two is what stops it hunting:
	// a chase that has just ended cannot restart until the head has travelled
	// the whole deadzone again.
	//
	// ---- AND THE DEADZONE IS AN ELLIPSE, NOT A CIRCLE -------------------
	//
	// Asked for once the chase above was in: a lean goes mostly SIDEWAYS and a
	// walk mostly FORWARD, relative to where the head faces, and a lean tips
	// the head over where a side-step keeps it upright. So the start test is an
	// ellipse in the head's frame -- `deadzone` across, `deadzone *
	// forwardRatio` along -- and head roll toward the displaced side widens it
	// by `leanTilt` units per degree, up to one more deadzone.
	//
	// Positive roll is the head tipped RIGHT. That is Source's convention and
	// the one the view already uses: were it the other way round, the world
	// would roll backwards every time the player tilted their head.
	const float yawRad = m_headYaw * 0.01745329252f;
	const float fx = cosf( yawRad );
	const float fy = sinf( yawRad );
	const float along = m_target.x * fx + m_target.y * fy;
	const float across = m_target.x * fy - m_target.y * fx;    // + is the head's right

	const float fwdLimit = m_six.deadzone * m_six.forwardRatio;
	const float towardLean = ( across >= 0.0f ) ? m_headRoll : -m_headRoll;
	float tiltBonus = ( towardLean > 0.0f ) ? towardLean * m_six.leanTilt : 0.0f;
	if ( tiltBonus > m_six.deadzone )
		tiltBonus = m_six.deadzone;
	const float sideLimit = m_six.deadzone + tiltBonus;

	float settle = m_six.settle;
	if ( settle > m_six.deadzone )
		settle = m_six.deadzone;
	if ( settle > fwdLimit )
		settle = fwdLimit;
	if ( settle < 0.25f )
		settle = 0.25f;    // at or below zero the chase could never end

	if ( !m_chasing )
	{
		bool outside;
		if ( fwdLimit <= 0.01f || sideLimit <= 0.01f )
			outside = len > settle;    // deadzone 0: every movement commits, as documented
		else
		{
			const float a = along / fwdLimit;
			const float b = across / sideLimit;
			outside = ( a * a + b * b ) > 1.0f;
		}
		if ( !outside )
			return;

		m_chasing = true;
		m_lastStep = 0.0f;
		++m_chases;

		// The first few starts in full, so the shape can be tuned from what a
		// real lean and a real step produced rather than from a feeling.
		constexpr unsigned int kChaseStartsLogged = 12;
		if ( m_chases <= kChaseStartsLogged )
			Log( "sixdof: chase #%u started | head %.1f %s and %.1f %s of the body, "
				 "tilted %+.0f deg into it | limits %.1f forward/back, %.1f sideways "
				 "(%.1f of that from the tilt)",
				 m_chases, fabsf( along ), ( along >= 0.0f ) ? "ahead" : "behind",
				 fabsf( across ), ( across >= 0.0f ) ? "right" : "left",
				 towardLean, fwdLimit, sideLimit, tiltBonus );
	}
	if ( len <= settle )
	{
		m_chasing = false;
		m_lastStep = 0.0f;
		return;
	}

	// Proportional, and aimed at ZERO rather than at the settle radius. Aimed at
	// the radius it would approach it asymptotically and never cross it, so the
	// chase would never end and leaning would stop working. Aimed at zero it
	// crosses in a handful of ticks, and still cannot overshoot: a fraction of
	// the gap is always less than the gap.
	float want = len * m_six.chase;

	// ---- A FIXED TICK, NOT THE WALL CLOCK -------------------------------
	//
	// The first version measured dt with GetTickCount64, and that was the main
	// source of the walking jitter: its granularity is ~15.6 ms while the ticks
	// themselves are ~15 ms apart, so consecutive dt readings came back 0, 15.6,
	// 31.2 -- and the step size swung between nothing and double.
	//
	// Easing code elsewhere in this project uses the same clock and is fine,
	// because an exponential filter absorbs a lumpy dt. This does not absorb
	// anything: it drives a POSITION directly, so every wobble in dt is a wobble
	// in the world.
	//
	// ProcessMovement is a fixed-interval callback, so the honest value is a
	// constant. maxStep is what bounds the damage if it is ever wrong.
	constexpr float kNominalTick = 0.015f;
	const float byRate = m_six.rate * kNominalTick;
	if ( want > byRate )
		want = byRate;
	if ( want > m_six.maxStep )
		want = m_six.maxStep;

	// ---- UP TO SPEED GRADUALLY ------------------------------------------
	//
	// Reported 2026-09-13, straight after the chase started aiming at zero:
	// jitter moving from a lean into a walk. Aimed at zero, the very first step
	// is 15% of a gap that is already the whole deadzone -- full `rate` from a
	// standstill, in one tick. The camera gives back what the body takes, but
	// any mismatch between the tick the body moves on and the frame the view is
	// drawn on is multiplied by that speed, and the old chase (aimed at the
	// deadzone edge) never started faster than a crawl.
	//
	// So a step may grow only a little per tick, reaching `rate` after `ramp`
	// seconds. Slowing down needs no limit: the proportional chase already
	// eases off as the gap closes.
	if ( m_six.ramp > 0.0001f )
	{
		const float grow = ( m_six.rate / m_six.ramp ) * kNominalTick * kNominalTick;
		if ( want > m_lastStep + grow )
			want = m_lastStep + grow;
	}
	if ( want < 0.0001f )
		return;

	const float ux = m_target.x / len;
	const float uy = m_target.y / len;

	__try
	{
		Vector* origin = (Vector*)( (unsigned char*)moveData + kMoveDataAbsOrigin );
		const Vector from = *origin;
		Vector step = { ux * want, uy * want, 0.0f };

		// ---- SWEPT WITH A SHRUNKEN PLAYER HULL ---------------------------
		//
		// m_vecAbsOrigin is at the player's FEET; the standing hull is 32x32x72
		// above it. HullEx sweeps a box centred on the traced points, so the
		// trace runs at the hull's centre with matching half-extents.
		//
		// Deliberately a little SMALLER than the real hull (15 not 16, 30 not
		// 36). This sweep is insurance against a gross teleport, not the
		// authority on collision -- the engine's own movement runs immediately
		// afterwards and is. A hull larger than the real one would refuse moves
		// the engine would happily allow, and the player would feel their body
		// stick on doorframes it fits through.
		if ( m_trace && m_trace->Valid() )
		{
			const Vector c0 = { from.x, from.y, from.z + 32.0f };
			const Vector c1 = { c0.x + step.x, c0.y + step.y, c0.z };

			Vector hit;
			float fraction = 1.0f;
			bool startedSolid = false;
			const bool blocked = m_trace->HullEx( c0, c1, Vector{ 15.0f, 15.0f, 30.0f },
												  m_traceSkip, hit, fraction, startedSolid );

			// STARTSOLID is not a wall -- same argument as the head sweep. The
			// player hull overlaps triggers, ladders and their own ground all
			// the time; treating it as blocked would stop the body dead in the
			// exact places a player stands.
			if ( blocked && !startedSolid )
			{
				if ( fraction < 0.0f ) fraction = 0.0f;
				if ( fraction > 1.0f ) fraction = 1.0f;
				step.x *= fraction;
				step.y *= fraction;
				++m_blocked;
			}
		}
		else if ( !m_warnedNoTrace )
		{
			m_warnedNoTrace = true;
			Log( "sixdof: no trace interface -- stepping unswept, bounded by "
				 "sixdof_max_step (%.1f). The engine's own movement is the only "
				 "collision on this path.", m_six.maxStep );
		}

		if ( fabsf( step.x ) < 0.0001f && fabsf( step.y ) < 0.0001f )
		{
			m_lastStep = 0.0f;    // stopped dead: the ramp starts again from rest
			return;
		}

		origin->x = from.x + step.x;
		origin->y = from.y + step.y;

		// Reported to the camera as the thing that actually happened.
		m_achieved.x += step.x;
		m_achieved.y += step.y;

		// What was APPLIED, not what was wanted, so a step cut short by a wall
		// ramps back up from the speed the body really had.
		m_lastStep = sqrtf( step.x * step.x + step.y * step.y );

		// ---- SPEND THE TARGET AS IT IS USED ------------------------------
		//
		// The target is refreshed once per RENDER frame, but this runs once per
		// server TICK, and the two do not correspond. When two ticks fall inside
		// one frame the second used to see the same untouched gap as the first
		// and take another full step for a distance already covered -- so the
		// body overshot, the offset crossed zero, and the next frame chased it
		// back. That oscillation is jitter that only appears while walking,
		// which is exactly when it was reported.
		//
		// Subtracting what was just applied makes the ticks share one gap
		// instead of each acting on the whole of it.
		m_target.x -= step.x;
		m_target.y -= step.y;

		m_moved = true;
		++m_steps;
	}
	__except ( EXCEPTION_EXECUTE_HANDLER )
	{
		LogError( "sixdof: faulted writing m_vecAbsOrigin at +%d -- the offset is "
				  "wrong, or this is not a CMoveData. DISABLING the write path.",
				  kMoveDataAbsOrigin );
		m_six.body = false;
	}
}

inline bool GameMovementProbe::Hook( int slot )
{
	if ( !m_iface || g_originalProcessMovement )
		return g_originalProcessMovement != nullptr;

	g_originalProcessMovement = reinterpret_cast<ProcessMovementFn>(
		m_hook.Install( m_iface, slot, &Detour_ProcessMovement ) );

	if ( !g_originalProcessMovement )
	{
		LogError( "gamemovement: failed to hook slot %d", slot );
		return false;
	}

	Log( "gamemovement: hooked ProcessMovement (slot %d, orig=%p) -- "
		 "OBSERVATIONAL, the CMoveData is not modified",
		 slot, g_originalProcessMovement );
	return true;
}

// ---- WHAT CHANGES IS WHAT MATTERS -----------------------------------------
//
// The first version dumped a raw window and correlated it against the view
// origin. Two problems, both of which showed up on the first run:
//
//   * It dumped during the level's opening CUTSCENE, where the player is
//     FL_FROZEN. Consecutive dumps were byte-identical, which says nothing.
//   * It correlated against the VIEW origin, which during a cutscene is the
//     camera, not the player. So the origin field could not match and its
//     absence meant nothing either.
//
// Both are fixed by asking a better question. Instead of "what does this struct
// contain", ask "WHICH BYTES OF IT MOVE WHEN THE PLAYER DOES". A cutscene, a
// paused game and standing still all produce no deltas and are skipped for
// free, so the probe gates itself instead of depending on catching the right
// moment. Static garbage stays silent; origin, velocity and the move inputs all
// announce themselves.
//
// It also settles the slot: if these bytes track walking, the pointer really is
// a CMoveData and slot 1 really is ProcessMovement. If nothing ever changes
// while the player runs about, it is neither, and no amount of staring at the
// raw window would have told us.
inline void GameMovementProbe::Note( void* moveData )
{
	if ( !moveData || m_reported )
		return;

	__try
	{
		const unsigned char* base = (const unsigned char*)moveData;

		if ( !m_havePrev )
		{
			memcpy( m_prev, base, kMoveDataWindow );
			m_havePrev = true;
			return;
		}

		int changedThisTick = 0;
		for ( int off = 0; off + 4 <= kMoveDataWindow; off += 4 )
		{
			if ( memcmp( m_prev + off, base + off, 4 ) != 0 )
			{
				++m_changes[off / 4];
				++changedThisTick;
			}
		}
		memcpy( m_prev, base, kMoveDataWindow );

		// Only count ticks where SOMETHING moved. Standing still or frozen
		// contributes nothing, so the sample is always of real movement however
		// long the player takes to get going.
		if ( changedThisTick > 0 )
			++m_liveTicks;

		if ( m_liveTicks < 300 )
			return;

		m_reported = true;

		Log( "movedata: %d ticks of real movement sampled. Offsets that CHANGED, "
			 "with their latest values -- these are the live fields.", m_liveTicks );
		Log( "movedata: reference: eye=(%.1f %.1f %.1f)  (player origin sits "
			 "roughly 64 below this)", m_eye.x, m_eye.y, m_eye.z );

		for ( int i = 0; i < kMoveDataWindow / 4; ++i )
		{
			if ( m_changes[i] == 0 )
				continue;

			const int off = i * 4;
			const float f = *(const float*)( base + off );
			const int   n = *(const int*)( base + off );

			// A field that changes on nearly every live tick is continuous
			// (origin, velocity, angles). One that changes rarely is a button
			// mask or a one-shot. Saying which removes a whole round of
			// guessing from reading this.
			const int pct = ( m_changes[i] * 100 ) / m_liveTicks;

			Log( "movedata:   +%03d  float=%12.3f  int=%11d  changed %3d%% of ticks%s",
				 off, f, n, pct,
				 pct > 80 ? "  <- CONTINUOUS" : ( pct < 10 ? "  <- occasional" : "" ) );
		}

		Log( "movedata: end. Set server_movement = 0 to stop touching server.dll." );
	}
	__except ( EXCEPTION_EXECUTE_HANDLER )
	{
		LogError( "movedata: faulted reading CMoveData at %p -- the window is too "
				  "large, or slot %d is not ProcessMovement and this pointer is "
				  "something else entirely",
				  moveData, 1 );
		m_reported = true;
	}
}

} // namespace sinvr
