// Composes the HMD orientation with the player's own turning, and pushes the
// result into the engine.
//
// The problem: SetViewAngles overwrites the engine's view angles wholesale, so
// naively writing the raw HMD orientation every frame would throw away all mouse
// and stick input -- you could only ever face where your neck faces.
//
// The fix: treat the engine's angles as an input channel. Whatever the engine
// changed since our last write *is* the player's turn input, so we accumulate
// that into a body yaw and add the head yaw on top:
//
//     final.yaw = bodyYaw + hmdYaw + recenterOffset
//
// Pitch and roll come from the head alone -- mouse pitch is deliberately
// discarded, because looking up in VR means looking up.
//
// ---- AIM DECOUPLING ---------------------------------------------------------
//
// With `aim_source = controller` the engine's view angles become the WEAPON
// HAND's direction and the rendered view keeps the head's, so you can aim
// somewhere you are not looking.
//
// This engine makes that far cheaper than the reference mods' technique.
// L4D2VR hooks CreateMove and rewrites `cmd->viewangles`, but that is a later
// Source signature -- `bool CreateMove(float, CUserCmd*)`. SiN is Source 2004,
// where it is `void CreateMove(int, float, bool)` with NO command pointer
// (verified on the live binary: slot 18 pushes three args and forwards to
// input->CreateMove at IInput slot 3). Instead, in_main.cpp builds the command
// like this:
//
//     engine->GetViewAngles( viewangles );        // in_main.cpp:45
//     VectorCopy( viewangles, cmd->viewangles );  // in_main.cpp:82
//
// so `cmd->viewangles` IS the engine's view angles -- the value this class
// already writes every frame. Aiming therefore needs no new hook and no
// CUserCmd layout reversing: write the controller's angles here, and override
// CViewSetup.angles with the head's for rendering, which is free because the
// mod already owns the view setup.
//
// ---- POSITIONAL (6DoF) ------------------------------------------------------
//
// The same class also maps the headset's tracked *position* into world space,
// because it already owns the only piece of state that mapping needs: the yaw
// that relates room-forward to game-forward.
//
//     game yaw = bodyYaw + hmdYaw + recenterOffset
//
// so a head at hmdYaw = 0 is facing game yaw (bodyYaw + recenterOffset), and
// that is exactly the angle to rotate a room-space displacement by to get a
// world-space one. Pitch and roll do not enter into it -- the play space floor
// stays the world floor however the head is tilted.
//
// The displacement is measured from a reference captured at recentre, not from
// the play space origin. Absolute room coordinates would shove the view by
// wherever the player happened to be standing when the map loaded, which is
// arbitrary; relative means "recentre puts your head where the game thinks your
// eyes are", and leaning, crouching and stepping all work from there.
#pragma once

#include <math.h>
#include "sdk/source_interfaces.h"
#include "sdk/engine_trace.h"
#include "vr/vr_backend.h"
#include "../common/log.h"

namespace sinvr {

// Which device's pitch steers the player while swimming.
//
// Source derives the swim direction from the command angles' PITCH --
// CGameMovement::WaterMove calls AngleVectors( mv->m_vecViewAngles ) and
// swims along the resulting forward vector. With aim_source = controller
// those angles are the WEAPON HAND's, so holding the gun level while
// swimming gives no vertical component and the player sinks. It never
// happens on a monitor because there the view pitch IS where you look.
enum SwimPitchSource
{
	kSwimPitchOff = 0,        // leave the aim pitch alone; sink as before
	kSwimPitchHmd = 1,        // swim where you look
	kSwimPitchController = 2, // swim where the off hand points
};

// What the engine's view angles -- and therefore aiming, shooting and the
// direction +forward walks -- are taken from.
enum AimSource
{
	kAimHmd = 0,          // look to aim. Unchanged behaviour.
	kAimController = 1,   // weapon hand aims; the head only decides what you see.
};

//-----------------------------------------------------------------------------
// Composing a controller pose with a fixed weapon offset.
//
// The naive version -- add the offset to each Euler component -- is wrong near
// vertical and was observed as the weapon flipping to point BACKWARDS instead of
// up. Euler angles are degenerate at pitch +-90: past it the valid
// representation wraps to (180 - pitch) with yaw turned 180, and addition does
// not do that, so the triple stops describing the rotation you meant.
//
// It bites far sooner than it looks. With a +58 degree pitch offset -- the real
// controller-to-weapon tilt measured on this rig -- only about 32 degrees of
// actual wrist pitch is needed to reach the singularity, so it is not an edge
// case, it is most of the useful range.
//
// So: turn the controller angles into a basis, rotate that basis about its OWN
// axes, and extract Euler once at the end. Same approach as Portal 2 VR, which
// builds its viewmodel angle with VectorAngles(forward, up) rather than by
// adding components.
//-----------------------------------------------------------------------------
constexpr float kDegPerRadian = 57.29577951308232f;

// How still the engine's own yaw has to be, frame to frame, before it counts as
// "the engine is holding a heading" rather than "the view is following us".
//
// The separation this relies on is large in both directions:
//
//   scripted camera  the engine stores the SAME float every frame, so the
//                    true frame-to-frame change is 0 and this only has to
//                    clear float noise.
//   ordinary play    the engine ACCEPTS our write, so its yaw follows the
//                    head -- it moves whenever the head moves. Measured
//                    per-frame head deltas on this rig run 0.07 to 0.34 deg,
//                    twenty to a hundred times this value.
//
// So the test is not "did the player turn a lot"; it is "did the engine's own
// heading move AT ALL". Deliberately far from both cases.
constexpr float kScriptedYawEpsilon = 0.003f;

// How many CONSECUTIVE frames of engine rewriting mean "a camera is being
// driven" rather than "something jumped once".
//
// A teleport or a spawn-angle set is one frame and is adopted; a cutscene runs
// for hundreds. Six frames is 67 ms at 90 fps -- long enough that no single
// event reaches it, short enough that almost none of a cutscene's rotation is
// absorbed before the detector latches.
constexpr unsigned int kDriveFrames = 6;

// Source's basis from a QAngle, matching mathlib. The conventions are not the
// obvious ones: pitch is positive DOWNWARDS, and `right` points along -Y at zero
// yaw.
inline void AnglesToBasis( const QAngle& angles, Vector& forward, Vector& right, Vector& up )
{
	const float sp = sinf( angles.x / kDegPerRadian ), cp = cosf( angles.x / kDegPerRadian );
	const float sy = sinf( angles.y / kDegPerRadian ), cy = cosf( angles.y / kDegPerRadian );
	const float sr = sinf( angles.z / kDegPerRadian ), cr = cosf( angles.z / kDegPerRadian );

	forward.x = cp * cy;
	forward.y = cp * sy;
	forward.z = -sp;

	right.x = -1.0f * sr * sp * cy + -1.0f * cr * -sy;
	right.y = -1.0f * sr * sp * sy + -1.0f * cr * cy;
	right.z = -1.0f * sr * cp;

	up.x = cr * sp * cy + -sr * -sy;
	up.y = cr * sp * sy + -sr * cy;
	up.z = cr * cp;
}

// Rodrigues rotation of v about a unit axis.
inline Vector RotateAboutAxis( const Vector& v, const Vector& axis, float degrees )
{
	const float r = degrees / kDegPerRadian;
	const float c = cosf( r ), s = sinf( r );
	const float d = axis.x * v.x + axis.y * v.y + axis.z * v.z;
	const Vector cross = { axis.y * v.z - axis.z * v.y,
						   axis.z * v.x - axis.x * v.z,
						   axis.x * v.y - axis.y * v.x };
	Vector out;
	out.x = v.x * c + cross.x * s + axis.x * d * ( 1.0f - c );
	out.y = v.y * c + cross.y * s + axis.y * d * ( 1.0f - c );
	out.z = v.z * c + cross.z * s + axis.z * d * ( 1.0f - c );
	return out;
}

// Basis -> QAngle, mathlib's VectorAngles. Always returns a valid triple: pitch
// lands in [-90, 90] with yaw carrying the rest, which is exactly the wrap that
// component-wise addition fails to do.
inline QAngle BasisToAngles( const Vector& forward, const Vector& up )
{
	QAngle out = { 0.0f, 0.0f, 0.0f };
	const float len2d = sqrtf( forward.x * forward.x + forward.y * forward.y );

	if ( len2d < 0.001f )
	{
		// Straight up or straight down: yaw is undefined from `forward` alone,
		// so take it from `up` instead of producing a garbage value.
		out.x = ( forward.z > 0.0f ) ? -90.0f : 90.0f;
		out.y = atan2f( -up.y, -up.x ) * kDegPerRadian;
		out.z = 0.0f;
		return out;
	}

	out.x = atan2f( -forward.z, len2d ) * kDegPerRadian;
	out.y = atan2f( forward.y, forward.x ) * kDegPerRadian;

	const Vector left = { up.y * forward.z - up.z * forward.y,
						  up.z * forward.x - up.x * forward.z,
						  up.x * forward.y - up.y * forward.x };
	out.z = atan2f( left.z, ( left.y * forward.x ) - ( left.x * forward.y ) ) * kDegPerRadian;
	return out;
}

// Controller angles + a fixed offset, composed properly.
//
// The offset is applied about the controller's OWN axes -- pitch about its
// right, yaw about its up, roll about its forward -- because that is what "the
// weapon sits at this angle in my hand" means. Applying it about world axes
// would make the correction change as you turn.
inline QAngle ComposeWeaponAngles( const QAngle& controller,
								   float pitchOffset, float yawOffset, float rollOffset )
{
	Vector forward, right, up;
	AnglesToBasis( controller, forward, right, up );

	if ( pitchOffset != 0.0f )
	{
		forward = RotateAboutAxis( forward, right, -pitchOffset );
		up = RotateAboutAxis( up, right, -pitchOffset );
	}
	if ( yawOffset != 0.0f )
	{
		forward = RotateAboutAxis( forward, up, yawOffset );
		right = RotateAboutAxis( right, up, yawOffset );
	}
	if ( rollOffset != 0.0f )
	{
		right = RotateAboutAxis( right, forward, rollOffset );
		up = RotateAboutAxis( up, forward, rollOffset );
	}

	return BasisToAngles( forward, up );
}

inline float NormalizeAngle( float deg )
{
	deg = fmodf( deg, 360.0f );
	if ( deg > 180.0f )
		deg -= 360.0f;
	else if ( deg < -180.0f )
		deg += 360.0f;
	return deg;
}

class VRCamera
{
public:
	// Call once per rendered frame, before the game builds its view.
	void Apply( const EngineClient& engine, IVRBackend& vr )
	{
		if ( !vr.Update() )
		{
			// Tracking dropped. Leave the engine's angles alone and re-sync on
			// the next good pose rather than snapping the view.
			m_haveLastWrite = false;
			return;
		}

		const HmdPose& hmd = vr.Hmd();

		QAngle current = { 0.0f, 0.0f, 0.0f };
		engine.GetViewAngles( current );

		// How far the engine moved the angles since our last write. In gameplay
		// that difference IS the player's turn input -- mouse look, a script,
		// a teleport -- and accumulating it is the whole design.
		float engineDelta = m_haveLastWrite
			? NormalizeAngle( current.y - m_lastWritten.y ) : 0.0f;
		m_rawEngineDelta = engineDelta;

		// ---- RECOIL IS NOT TURN INPUT ---------------------------------------
		//
		// Measured on hardware with the head deliberately still: hmd moved
		// +-0.03 deg while body yaw moved 0.23..0.95 deg per sample, with a
		// matching non-zero engineDelta. So the engine moves its OWN yaw while
		// firing, this treats that as the player turning, and the world swings
		// under a stationary head. Reported as "the assault rifle forces my
		// head side to side", and it is the same defect as the "horizontal
		// screen shake" logged against the rifle in section 12.
		//
		// It is independent of recoil_compensation, which only ever subtracted
		// punch from the AIM handed to the engine and never touched the view.
		// Setting that to 0 changed nothing, which is what proved the two are
		// separate problems.
		//
		// The viewkick is a known quantity -- m_vecPunchAngle, resolved by name
		// -- so the part of the engine's delta that IS the kick can be removed
		// rather than guessed at. What remains is genuine: a teleport, a script,
		// a push. Yaw only; pitch does not go through the body yaw at all.
		const float punchDeltaY = NormalizeAngle( m_punchRaw.y - m_lastPunchYaw );
		m_lastPunchYaw = m_punchRaw.y;
		m_lastPunchDeltaY = punchDeltaY;

		// ---- SUBTRACTING PUNCH DOES NOT WORK ON THIS GAME -------------------
		//
		// The first version subtracted the viewkick, on the reasoning that the
		// kick is a known quantity. It is not knowable HERE: measured on
		// hardware through a full burst, the client's m_vecPunchAngle stays at
		// 0.00 while the engine's yaw moves 0.3..0.6 deg a frame. SiN applies
		// the kick server-side and networks back the already-rotated view
		// angles, so there is nothing on the client to subtract.
		//
		// SiN's own weapon script says what is happening and why it is the
		// rifle (`weapon_assault_rifle.txt`):
		//
		//     kickMin      0.175  0.0   0.0      <- pitch only
		//     kickRampMin  0.0   -1.0  -0.275    <- YAW +-1.0 per shot, random
		//     kickRampMax  1.5    1.0   0.275       ramping up over 1.5s
		//
		// A random yaw per shot, growing the longer fire is held. The magnum
		// and scattergun have no yaw term, which is the whole of "only the
		// assault rifle". On a monitor this is the gun wandering; here the body
		// yaw absorbs every degree of it and the WORLD wanders instead.
		//
		// So the discriminator is not the punch, it is whether a shot is being
		// fired -- and the size, because a kick is small and bounded while a
		// teleport or a scripted camera is not. Both conditions, so a genuine
		// engine move during a firefight is still adopted.
		if ( m_recoilViewLock && m_firing &&
			 fabsf( engineDelta ) <= m_recoilLockMaxDegrees )
		{
			m_recoilSuppressed += fabsf( engineDelta );
			++m_recoilSuppressedFrames;
			engineDelta = 0.0f;
		}

		m_lastEngineDelta = engineDelta;

		// The engine's OWN heading, before we overwrite it. On the main menu
		// this is the background map's scripted camera -- the direction the
		// NPC and the logo are staged for. Nothing else knows it: once we
		// write, it is gone until the engine re-asserts it next frame.
		m_lastEngineYaw = current.y;
		m_lastEnginePitch = current.x;

		// ---- IS THE ENGINE DRIVING ITS OWN CAMERA? -------------------------
		//
		// This is the ROOT CAUSE of the main-menu yaw lock, and it is separate
		// from menu detection on purpose.
		//
		// `engineDelta` is treated as the player's turn input, which is right in
		// gameplay: mouse look moves the engine's angles and accumulating that
		// is the whole design. But the main menu's BACKGROUND MAP has a scripted
		// camera that re-asserts the SAME absolute yaw every frame, throwing our
		// write away. Accumulating that makes the body yaw absorb exactly enough
		// to cancel the head, and the view sits still however far the player
		// turns -- yaw only, since pitch and roll never go through the body yaw.
		//
		// Measured on the main menu: engine yaw pinned at -180.0 for a whole run
		// while the body yaw wandered 57.7 -> 75.5 -> 66.6 absorbing the head.
		//
		// The two cases are distinguishable without knowing anything about
		// menus, and BOTH conditions are needed:
		//
		//   the engine's ABSOLUTE yaw is unchanged since last frame   ...and...
		//   it does not match what we wrote (so our write was rejected)
		//
		//   gameplay, head still   engine yaw constant, delta 0    -> accumulate
		//                          (it equals our write; nothing to absorb)
		//   gameplay, mouse look   engine yaw MOVES                -> accumulate
		//   scripted camera        engine yaw constant, delta != 0 -> SUPPRESS
		//
		// Stick turning is unaffected either way: it goes through AddBodyYaw and
		// never touches the engine, so it produces no delta to suppress.
		//
		// Suppressing in a gameplay cutscene is also correct -- the body yaw
		// should not absorb a camera the player is not driving.
		const bool engineYawHeld =
			m_haveLastEngineYaw &&
			fabsf( NormalizeAngle( current.y - m_prevEngineYaw ) ) < kScriptedYawEpsilon;
		const bool engineRejectedOurWrite = fabsf( engineDelta ) >= kScriptedYawEpsilon;
		const bool scriptedCamera = engineYawHeld && engineRejectedOurWrite;

		m_prevEngineYaw = current.y;
		m_haveLastEngineYaw = true;

		if ( scriptedCamera )
		{
			m_scriptedYawSuppressed += fabsf( engineDelta );
			++m_scriptedYawFrames;
		}

		// ---- A CUTSCENE IS NOT A TELEPORT, AND BOTH LOOK LIKE ONE FRAME ----
		//
		// The guard above only catches a camera holding a FIXED heading -- the
		// main menu's background map. A cutscene camera MOVES, so its yaw is
		// different every frame, `engineYawHeld` is false, and every degree of
		// it was being accumulated into the body yaw as though the player had
		// turned. Head tracking then reads as the world rotating, which is
		// exactly what a new game did: SiN opens on SE1_intro01 and the intro
		// drives the camera. Loading a save skips it, which is why it only ever
		// showed on a new game.
		//
		// The thing that separates the two cases is DURATION, not size:
		//
		//     teleport / spawn angles   one frame, then the engine goes quiet
		//     cutscene camera           every frame, for hundreds of them
		//
		// So the delta is held rather than applied. If the engine goes quiet
		// again within kDriveFrames it was a jump and the held total is adopted
		// -- a teleport still works, one frame late. If it keeps coming, it is
		// a camera being driven, the held total is DISCARDED, and nothing more
		// is accumulated until it stops.
		//
		// At most kDriveFrames of a cutscene are absorbed before the detector
		// latches, which at 90 fps is a few tens of milliseconds.
		// ---- TWO WAYS TO KNOW, AND THE DIRECT ONE WINS ---------------------
		//
		// FL_FROZEN is the game saying the player has lost control, which is
		// what a point_viewcontrol does the instant it enables. Taking it as
		// immediate proof means NO rotation is absorbed at all -- the duration
		// heuristic necessarily lets kDriveFrames of it through before it
		// latches.
		//
		// The heuristic stays for scripted cameras that drive the view without
		// freezing movement, which FL_FROZEN would miss. Either is sufficient.
		const bool engineMoving = fabsf( engineDelta ) >= kScriptedYawEpsilon;

		// Instrument, unconditional: nothing reported FL_FROZEN's VALUE before,
		// only that the offset for it was found. That is lesson 22 -- the one
		// fact needed to diagnose a cutscene was the one fact no log could
		// deliver -- and it cost a session's worth of guessing about a scene
		// that turned out never to have been detected at all.
		if ( m_playerFrozen )
		{
			++m_frozenFrames;
			if ( !m_wasFrozen )
				++m_frozenEpisodes;
		}
		m_wasFrozen = m_playerFrozen;

		// ---- FL_FROZEN IS SUFFICIENT ON ITS OWN ----------------------------
		//
		// It used to be ANDed with engineMoving, which made it an accelerator
		// for the duration heuristic rather than a second detector -- directly
		// contradicting the comment above it, which says either is sufficient.
		//
		// The case that exposed it: a cutscene whose camera does not ROTATE.
		// SiN's car sequence cuts on a white flash to a fixed shot of a
		// character, and a fixed camera produces engineDelta ~ 0 every frame.
		// So engineMoving was false forever, the heuristic never counted a
		// frame, the FL_FROZEN path was gated off behind it, and the scene was
		// never detected: no turn-to-face, no look arrow, and the player was
		// left pointing wherever the teleport happened to drop them -- which
		// was backwards.
		//
		// A camera that does not move is not evidence that no camera is being
		// driven. FL_FROZEN is the game stating outright that the player is not
		// in control, and it does not need corroborating.
		// ---- A THIRD DETECTOR WAS TRIED HERE AND REMOVED --------------------
		//
		// Head-vs-camera DIVERGENCE looked ideal: ~0 in normal play (because
		// CViewSetup.angles IS the view we wrote from the head), 84..173 deg
		// through the scene that FL_FROZEN and the duration heuristic both
		// miss. It was built, played, and reverted the same session.
		//
		// **It is a feedback loop and no threshold can fix it.** Latching runs
		// turn-to-face, turn-to-face aligns the head with the camera,
		// divergence therefore collapses to ~0, the latch releases -- and the
		// moment the player looks away it fires again and drags them back.
		// Measured on hardware: 11 fires and 12 releases in one scene, reported
		// as "I kept getting forced back when I tried to look away".
		//
		// The hysteresis that was supposed to prevent this only sets HOW FAR
		// the player may look before being yanked, because the loop does not
		// close through the threshold -- it closes through the player's head.
		//
		// Same shape as the menu-view-locked loop in section 9 of the handover
		// and the animation suppressor's "compare against the engine, never
		// against what you last wrote". **A detector must not measure a
		// quantity its own corrective action changes.** Anything built here
		// needs a signal the turn does NOT move: FL_FROZEN is one, which is
		// why that detector is safe. The measurement is still taken and logged
		// -- see the heartbeat in dllmain -- it just drives nothing.
		const bool frozenLatch = m_playerFrozen && m_frozenLatch;
		if ( frozenLatch && !m_engineDriving )
		{
			m_driveFrames = kDriveFrames;   // latch immediately, absorb nothing
			m_pendingEngineYaw = 0.0f;
		}

		if ( engineMoving || frozenLatch )
		{
			++m_driveFrames;
			if ( m_driveFrames >= kDriveFrames )
			{
				if ( !m_engineDriving )
				{
					m_engineDriving = true;
					++m_engineDriveEvents;
					// ---- POINT THE PLAYER AT THE SCENE, ONCE -------------
					//
					// A cutscene that does not turn the player can start with
					// them facing away from it entirely. Aligning the body yaw
					// to the camera's ONCE, at the moment it begins, means they
					// start off looking the right way -- and because it is a
					// body-yaw change rather than a view rotation, it costs
					// nothing in comfort: the world turns while the head is
					// still, which is what a recentre already does.
					//
					// YAW ONLY. Pitch belongs to the player's neck and forcing
					// it would be both wrong and unpleasant; if the scene is
					// overhead they still have to look up, which is what the
					// arrow is for.
					m_pendingCutsceneAlign = m_cutsceneRecenter;
					Log( "camera: the ENGINE is driving the view (%s). Treating it "
						 "as a scripted camera, not as turn input -- your head "
						 "keeps the view. cutscene_camera = engine to follow it "
						 "instead.",
						 m_playerFrozen
							 ? "FL_FROZEN -- the game says the player is not in "
							   "control, so nothing was absorbed"
							 : "duration heuristic: continuous rewriting" );
				}
				m_pendingEngineYaw = 0.0f;
			}
			else
			{
				m_pendingEngineYaw = NormalizeAngle( m_pendingEngineYaw + engineDelta );
			}
		}
		else
		{
			// Quiet again. Anything held was a one-off after all.
			if ( m_engineDriving )
			{
				// ---- AND A FROZEN COUNTER MUST BE RE-ADOPTED ---------------
				//
				// Suppressing accumulation for the length of a cutscene leaves
				// the body yaw exactly as stale as the menu freeze does, for
				// exactly the same reason: the engine has been turning the
				// player all that time and we deliberately ignored every degree
				// of it. Resuming without re-adopting means the player's forward
				// is off by however far the scene turned them, which reads as
				// head tracking being broken the moment the cutscene ends.
				//
				// This is the same lesson as the menu exit below, and shipping
				// the suppression without it was the same mistake twice: the
				// freeze and the resync are one feature, not two.
				m_resyncAfterDrive = true;
				Log( "camera: the engine stopped driving the view after %u frame(s)"
					 " -- body yaw will be re-adopted, not accumulated",
					 m_driveFrames );
			}
			m_engineDriving = false;
			m_driveFrames = 0;
		}

		// ---- LEAVING A MENU: RESYNC, DO NOT ACCUMULATE ---------------------
		//
		// This is the regression that broke head rotation in a loaded level, and
		// it is a direct consequence of the freeze below.
		//
		// While a menu is up the body yaw is deliberately frozen, so it stops
		// tracking the engine -- by design, and measured at 775,995 degrees of
		// suppressed rewriting in one session. The moment gameplay resumes, that
		// frozen value is stale by an arbitrary amount AND the engine has since
		// set the player's spawn angles. Treating the difference as "turn input"
		// applies a single huge correction, and the player's forward ends up
		// pointing somewhere unrelated to their head.
		//
		// So the transition is a RESYNC, not an accumulation: adopt whatever the
		// engine now says, with the head's current orientation folded in, so the
		// view comes out of the menu pointing exactly where the game wants it.
		// Nothing is carried across -- which is the point, because nothing from
		// a frozen counter is worth carrying.
		// The one-shot cutscene alignment. Applied here, beside the other body
		// yaw decisions, so there is one place that owns this value.
		//
		// Deferred by a frame rather than done at the moment of detection: the
		// target comes from CViewSetup and is pushed in by the caller, so on the
		// very frame a cutscene is detected it may still hold the previous
		// view's angles.
		if ( m_pendingCutsceneAlign && m_haveLookTarget && m_haveLastWrite )
		{
			m_pendingCutsceneAlign = false;
			m_bodyYaw = NormalizeAngle( m_lookTargetYaw - hmd.angles.y - m_yawOffset );
			Log( "camera: cutscene started -- turned the body to face it "
				 "(camera yaw %.1f, head at %.1f). Yaw only; look up or down "
				 "yourself.",
				 m_lookTargetYaw, hmd.angles.y );
		}

		const bool leftUi = m_haveLastWrite && m_wasUiMode && !m_uiMode;
		const bool leftDrive = m_haveLastWrite && m_resyncAfterDrive && !m_uiMode;
		if ( leftUi || leftDrive )
		{
			// One resync, two triggers. A menu and a cutscene both freeze the
			// body yaw and both leave it stale; there is no reason for them to
			// have different recovery code, and every reason for them not to.
			m_bodyYaw = NormalizeAngle( current.y - hmd.angles.y - m_yawOffset );
			m_resyncAfterDrive = false;
			Log( "camera: left %s -- body yaw resynced to the engine's %.1f "
				 "(head at %.1f). A frozen body yaw is stale by an arbitrary "
				 "amount, so it must be re-adopted here rather than accumulated.",
				 leftUi ? "a menu" : "a scripted camera",
				 current.y, hmd.angles.y );
		}
		else if ( m_haveLastWrite && !m_uiMode )
		{
			// Anything the engine changed since our write is player turn input
			// -- UNLESS the engine is driving its own camera, in which case it
			// is the engine talking to itself and accumulating it is the
			// feedback loop described above.
			//
			// `cutsceneFollow` is the one case where a driven camera IS
			// accumulated: the player asked for the scripted camera to move
			// their view, which is the flat game's behaviour and the more
			// cinematic of the two.
			const bool driven = scriptedCamera ||
								( m_engineDriving && !m_cutsceneFollow );
			if ( !driven )
			{
				m_bodyYaw = NormalizeAngle( m_bodyYaw + engineDelta );
			}
			else if ( m_engineDriving )
			{
				m_scriptedYawSuppressed += fabsf( engineDelta );
				++m_scriptedYawFrames;
			}

			// A held total that survived to here was a jump, not a drive.
			if ( !m_engineDriving && m_pendingEngineYaw != 0.0f )
			{
				m_bodyYaw = NormalizeAngle( m_bodyYaw + m_pendingEngineYaw );
				m_pendingEngineYaw = 0.0f;
			}
		}
		else if ( m_haveLastWrite )
		{
			// ---- A MENU IS UP: DO NOT ACCUMULATE ---------------------------
			//
			// This feedback loop is what made the menu view feel LOCKED.
			//
			// The main menu's background map drives its own camera, so it
			// rewrites the view angles every frame. In gameplay that difference
			// is genuinely the player turning; on the menu it is the engine
			// talking to itself, and folding it into m_bodyYaw makes the body
			// yaw absorb exactly the amount needed to cancel the head:
			//
			//     write   H(n)   = bodyYaw + hmdYaw(n) + offset
			//     engine resets to M
			//     delta          = M - H(n)          -> bodyYaw += that
			//     write   H(n+1) = M + (hmdYaw(n+1) - hmdYaw(n))
			//
			// so every frame re-anchors to the MENU's angle and keeps only that
			// frame's head delta. The view sits still however far you turn.
			//
			// It also explains why the controller appeared to work before this:
			// its own rotation supplied a per-frame delta that survived the
			// cancellation, so the one device that should NOT have been steering
			// was the only one that could.
			//
			// Freezing the body yaw here means the menu view is
			// bodyYaw(frozen) + hmdYaw + offset -- absolute head tracking, which
			// is what a menu wants. There is no legitimate turn input to lose:
			// the movement stick is held off in menus anyway.
			m_uiSuppressedYaw += fabsf( engineDelta );
			++m_uiSuppressedFrames;
		}
		else
		{
			// First frame, or recovering from tracking loss: adopt the engine's
			// current heading so the view does not jump.
			m_bodyYaw = current.y;
			m_pendingRecenter = true;
		}

		if ( m_pendingRecenter )
		{
			m_yawOffset = -hmd.angles.y;
			// Same moment, same reason: from here on "no displacement" means
			// "where you were standing when you recentred".
			m_positionReference = hmd.position;
			m_havePositionReference = true;
			m_pendingRecenter = false;
			++m_recentreCount;
			Log( "camera: recentred (bodyYaw=%.1f hmdYaw=%.1f offset=%.1f) "
				 "position reference=(%.1f %.1f %.1f)",
				 m_bodyYaw, hmd.angles.y, m_yawOffset,
				 hmd.position.x, hmd.position.y, hmd.position.z );
		}

		UpdatePositionalOffset( hmd );

		// What the eyes see. Always the head, whatever aims.
		m_viewAngles.x = Clamp( hmd.angles.x, -m_pitchLimit, m_pitchLimit );
		m_viewAngles.y = NormalizeAngle( m_bodyYaw + hmd.angles.y + m_yawOffset );
		m_viewAngles.z = m_applyRoll ? hmd.angles.z : 0.0f;

		// ---- PER-FRAME ROTATION TRACE ---------------------------------------
		//
		// A 5-second heartbeat cannot see 90 Hz judder -- it samples one frame
		// in 450. Every rotation report so far has been argued from heartbeats,
		// which is why none of them located anything.
		//
		// The view yaw is the sum of exactly three terms:
		//
		//     view = bodyYaw + hmdYaw + yawOffset
		//
		// so logging all three per frame says WHICH ONE is not smooth, and that
		// is the entire diagnosis:
		//
		//   dHmd jumpy, dBody flat      the POSE is jittering -- runtime,
		//                               prediction, or the sample point
		//   dHmd smooth, dBody jumpy    we are corrupting it -- body yaw is
		//                               accumulating something it should not
		//   both smooth, dView jumpy    the composition or the write is wrong
		//   all smooth                  the jitter is downstream of here, in
		//                               the renderer or the submitted pose
		//
		// Logged only while the head is actually TURNING, because that is when
		// the fault is reported and a still head would fill the file with
		// nothing. Capped so a forgotten trace cannot fill a disk.
		if ( m_traceRotation && m_traceRotationLeft > 0 )
		{
			const float dHmd = NormalizeAngle( hmd.angles.y - m_lastTraceHmdYaw );

			// Also trace while there is RECOIL, not only while the head turns.
			// The reported fault is "firing forces my head to move", and the
			// question that settles it is whether the ENGINE's angles move on
			// their own during a burst -- because body yaw accumulates that
			// delta as if it were turn input, and the world then rotates under
			// a head that did not move. Punch is passed in as the aim
			// correction, so it is non-zero exactly while a kick is live.
			// Keyed on the RAW punch, not on the correction. The correction is
			// zero whenever recoil_compensation is 0, so keying on it made the
			// trace blind in precisely the configuration used to test recoil.
			const bool recoiling = ( m_punchRaw.x != 0.0f || m_punchRaw.y != 0.0f );
			if ( fabsf( dHmd ) > 0.02f || recoiling )
			{
				--m_traceRotationLeft;
				Log( "rot: hmd=%.3f d=%+.3f | body=%.3f d=%+.3f | off=%.3f | "
					 "view=%.3f d=%+.3f | eng=%.3f d=%+.3f | engRaw=%+.3f "
					 "punchDy=%+.3f engUsed=%+.3f | punch=(%.2f %.2f)%s",
					 hmd.angles.y, dHmd,
					 m_bodyYaw, NormalizeAngle( m_bodyYaw - m_lastTraceBodyYaw ),
					 m_yawOffset,
					 m_viewAngles.y,
					 NormalizeAngle( m_viewAngles.y - m_lastTraceViewYaw ),
					 m_lastEngineYaw,
					 NormalizeAngle( m_lastEngineYaw - m_lastTraceEngYaw ),
					 m_rawEngineDelta, m_lastPunchDeltaY, m_lastEngineDelta,
					 m_punchRaw.x, m_punchRaw.y,
					 recoiling ? "  <-- RECOIL" : "" );
			}
			m_lastTraceHmdYaw = hmd.angles.y;
			m_lastTraceBodyYaw = m_bodyYaw;
			m_lastTraceViewYaw = m_viewAngles.y;
			m_lastTraceEngYaw = m_lastEngineYaw;
		}

		// What the engine aims, shoots and walks along. Same body-yaw mapping,
		// because the controller is in the same room space as the head.
		QAngle out = m_viewAngles;
		m_aimFromController = false;

		// ---- A MENU IS UP: THE HEAD AIMS ---------------------------------
		//
		// Aim decoupling writes the WEAPON HAND's angles into the engine's view
		// angles. On the main menu that is wrong in the most visible way
		// possible: there is no weapon, and the engine's view angles are what
		// the background map -- and therefore the whole world -- is drawn from.
		// The result is that the player's view rotates with a controller they
		// are not pointing at anything, and does not follow their head.
		//
		// It went unnoticed because `IsInGame()` is TRUE on the main menu: it
		// runs a background map. Every gameplay gate in this project keys off
		// that, so none of them were closed on the menu.
		//
		// Forcing the head here rather than at the caller is deliberate --
		// m_viewAngles is already exactly "where the head looks", so this
		// cannot disagree with what the eyes are given, whichever render path
		// the menu's background map turns out to use.
		if ( m_uiMode )
		{
			engine.SetViewAngles( out );
			m_lastWritten = out;
			m_haveLastWrite = true;
			// The head is what aims here, so there is nothing for the movement
			// stick to correct for. Set explicitly rather than left stale, or
			// the frame a menu opens keeps the last gameplay offset.
			m_aimYawOffset = 0.0f;
			m_swimPitchApplied = false;
			// Recorded on BOTH exits from Apply, or the menu->gameplay edge
			// the resync above keys off would never be seen.
			m_wasUiMode = true;
			return;
		}

		// ---- A MELEE SWING AIMS WHERE YOU LOOK, NOT WHERE THE HAND ENDS ----
		//
		// Source resolves a melee attack along the player's VIEW ANGLES, and
		// under aim_source = controller those are the weapon hand's. A downward
		// swing therefore lands its hit pointing at the floor, which is where
		// the hand finished rather than where the player was aiming.
		//
		// Worse, the engine resolves the hit some frames AFTER "+melee" goes
		// down, so firing earlier in the swing does not help on its own -- by
		// resolve time the hand is lower still.
		//
		// So the melee window forces the head's angles for its whole duration,
		// button hold plus a tail that covers the engine's resolution. The gun
		// keeps pointing where the hand does; only what the ENGINE is told to
		// swing along changes.
		if ( m_aimSource == kAimController && !m_forceHeadAim )
		{
			const ControllerPose& weapon = vr.WeaponHand();
			if ( weapon.valid )
			{
				// The SAME angle offsets the viewmodel uses.
				//
				// They are not a cosmetic tweak to the model -- they are the
				// controller-to-weapon transform. A controller's pose axis is
				// tilted well away from the direction it feels like it points:
				// measured at +58 degrees of pitch on this rig. Applying that to
				// the model but not to the aim put the shots 58 degrees off the
				// gun, and since Source's pitch is positive DOWNWARDS, that came
				// out as firing almost straight up while the model looked right.
				// Composed as a rotation, not added component-wise -- see
				// ComposeWeaponAngles. Adding put the aim through the Euler
				// singularity at pitch 90 and flipped it backwards.
				// Through EffectiveWeaponAngles, not ComposeWeaponAngles directly, so
				// a two-handed grip moves the AIM and the MODEL together.
				const QAngle aimed = EffectiveWeaponAngles(
					weapon.angles, m_weaponAnglePitch, m_weaponAngleYaw, 0.0f );

				QAngle worldWeapon;
				worldWeapon.x = aimed.x;
				worldWeapon.y = NormalizeAngle( m_bodyYaw + aimed.y + m_yawOffset );
				worldWeapon.z = 0.0f;

				// ---- aim convergence -------------------------------------
				//
				// Source fires from the player's EYE, not from the weapon
				// model. Pointing the engine straight down the gun's own axis
				// therefore sends the shot along a line PARALLEL to the barrel
				// but displaced by however far the gun sits from the eye -- so
				// it lands consistently off to one side. The side flips with
				// handedness, which is why it only became obvious in
				// left-handed mode.
				//
				// Fix it the way iron sights do: aim the eye at the point the
				// barrel is pointing at, a chosen distance away. Exact at that
				// distance, and the error either side of it is far smaller than
				// the constant offset it replaces.
				//
				// Nothing here needs the head's 6DoF offset. The engine shoots
				// from the player's real eye, which our positional tracking
				// deliberately does not move, and the gun is measured from that
				// same origin -- so both sides of the subtraction share it and
				// it cancels.
				// ---- HOW FAR OUT TO CONVERGE ---------------------------
				//
				// A FIXED distance is exact at exactly one range. Aiming the
				// eye at a point 250 units down the barrel puts the shot where
				// the gun points only for targets at 250; nearer or further,
				// the eye-to-muzzle offset reappears as an error that grows
				// with the distance mismatch.
				//
				// The traced distance removes the guess. Converge on what the
				// barrel is REALLY pointing at and the engine's eye-to-target
				// line passes through that same point at EVERY range -- not a
				// better approximation, an exact one.
				//
				// Minus m_weaponOffForward because the trace starts at the hand
				// and the convergence below is measured from the MUZZLE, which
				// sits that far further along the same axis.
				//
				// It stays wrong in one case, and a narrower one than before:
				// when the eye-to-target line is blocked by something the
				// muzzle-to-target line is not -- the gun poking round a corner
				// with the player's eye still behind it.
				float convergence = m_aimConvergence;
				m_lastConvergenceMeasured = false;
				if ( m_tracedAim > 0.0f )
				{
					if ( m_tracedAim > 16.0f )
					{
						convergence = m_tracedAim;
						m_lastConvergenceMeasured = true;
					}
				}
				m_lastConvergence = convergence;

				// ---- SHOOTING FROM THE GUN: NO CONVERGENCE AT ALL -----
				//
				// When the shot leaves the MUZZLE, the convergence hack is not
				// merely unnecessary, it is actively wrong. Its whole job is to
				// bend the aim so a ray leaving the EYE passes through what the
				// barrel points at. Fire that bent angle from the muzzle as well
				// and the correction is applied twice.
				//
				// So the angles handed to the engine become the gun's own axis
				// and nothing else. One ray, from the muzzle, down the barrel --
				// which is what makes shooting round a corner work, because the
				// ray now starts on the far side of it.
				//
				// The ANGULAR bore survives, because it is still physically
				// real: it corrects the controller's grip axis to the model's
				// barrel axis. The LATERAL bore does not, and must not -- it was
				// a parallax correction for an eye-to-muzzle offset that no
				// longer exists.
				bool converged = false;
				if ( m_shotFromGun )
				{
					Vector f, r, u;
					AnglesToBasis( worldWeapon, f, r, u );

					if ( m_boreYaw != 0.0f )
					{
						f = RotateAboutAxis( f, u, m_boreYaw );
						r = RotateAboutAxis( r, u, m_boreYaw );
					}
					if ( m_borePitch != 0.0f )
					{
						f = RotateAboutAxis( f, r, m_borePitch );
						u = RotateAboutAxis( u, r, m_borePitch );
					}

					const float len2d = sqrtf( f.x * f.x + f.y * f.y );
					if ( len2d > 0.001f )
					{
						out.x = Clamp( atan2f( -f.z, len2d ) * kDegPerRadian,
									   -m_pitchLimit, m_pitchLimit );
						out.y = NormalizeAngle( atan2f( f.y, f.x ) * kDegPerRadian );
						converged = true;
					}

					// Published so the debug lines still describe what was used.
					// The muzzle is supplied by the viewmodel in this mode, so
					// the origin here is only a placeholder for the direction.
					m_aimGeom.muzzleRel = WorldOffsetForRoomPos( weapon.position );
					m_aimGeom.forward = f;
					m_aimGeom.targetRel = Vector{
						m_aimGeom.muzzleRel.x + f.x * 512.0f,
						m_aimGeom.muzzleRel.y + f.y * 512.0f,
						m_aimGeom.muzzleRel.z + f.z * 512.0f };
					m_aimGeom.valid = true;
				}
				else if ( convergence > 0.0f )
				{
					Vector f, r, u;
					AnglesToBasis( worldWeapon, f, r, u );

					// The angular correction, applied to the BASIS rather than
					// by adding to the Euler angles. Component-wise addition is
					// what put an earlier version of this code through the
					// pitch-90 singularity and flipped the aim backwards; a
					// rotation cannot do that.
					//
					// Yaw about the gun's own up, pitch about its own right, so
					// the correction means the same thing whatever attitude the
					// gun is held at.
					if ( m_boreYaw != 0.0f )
					{
						f = RotateAboutAxis( f, u, m_boreYaw );
						r = RotateAboutAxis( r, u, m_boreYaw );
					}
					if ( m_borePitch != 0.0f )
					{
						f = RotateAboutAxis( f, r, m_borePitch );
						u = RotateAboutAxis( u, r, m_borePitch );
					}
					// EYE -> hand, not reference -> hand.
					//
					// WorldOffsetForRoomPos measures from the recentre reference, so
					// subtracting the head's own displacement is what turns it into a
					// vector from the eye -- which is what everything below assumes it
					// already is. Standing still at the reference the two are equal and
					// this changes nothing, which is why it went unnoticed; leaning made
					// it wrong by the lean, and physical crouching makes it wrong by a
					// quarter of a metre, tilting the converged aim downward.
					//
					// The duck compensation cancels here, as it should: how far the
					// ENGINE ducked the player has nothing to do with where their hand
					// is relative to their eye.
					// ---- WHICH ORIGIN THE AIM IS MEASURED FROM ------------
					//
					// This block outputs ANGLES, so the origin is implicit:
					// whatever `hand` is relative to. Get that wrong and the
					// engine is handed a direction computed from one point and
					// fires it from another.
					//
					// WorldOffsetForRoomPos measures from the recentre
					// REFERENCE, and the reference in world IS the engine's eye
					// -- ApplyEyeToViewSetup adds the 6DoF offset on top of
					// vs->origin, so the un-offset origin is the reference.
					// The ENGINE fires from there, because positional tracking
					// deliberately does not move the player entity.
					//
					// Subtracting m_positionalOffset makes `hand` relative to
					// the player's PHYSICAL HEAD instead, which is a different
					// point whenever they lean or turn. The comment that
					// introduced it called that "a vector from the eye", which
					// conflated the head with the firing point.
					//
					// Kept switchable rather than simply corrected: the debug
					// lines can now show which is right, and this has been
					// reasoned about wrongly twice. `aim_origin_head = 1`
					// restores the old behaviour.
					// KNOWN, UNFIXED, DELIBERATELY: `hand` is measured from
					// the STANDING recentre reference while the engine fires
					// from vs->origin, which the engine has already DUCKED.
					// Standing they are the same point, so this costs nothing
					// and no tuning done so far is affected; physically
					// crouching makes them differ by ~20 units and tips the
					// converged aim downward.
					//
					// The one-line fix is `hand.z += m_duckCompensation` here.
					// It was written, and then reverted unverified when the aim
					// work was stopped in favour of true 6DoF -- because 6DoF
					// deletes this whole convergence path, and an unverified
					// change left in a live path we have stopped looking at is
					// exactly the kind of trap this project keeps finding.
					Vector hand = WorldOffsetForRoomPos( weapon.position );
					if ( m_aimOriginHead )
					{
						hand.x -= m_positionalOffset.x;
						hand.y -= m_positionalOffset.y;
						hand.z -= m_positionalOffset.z;
					}

					// ---- WHERE THE SHOT SHOULD CONVERGE ------------------
					//
					// MEASURED: the traced hit point, exactly. The trace began
					// at the HAND and ran along the barrel, so `hand + f * D`
					// IS the point the gun is pointing at -- nothing else needs
					// adding, and adding anything is a bug.
					//
					// The weapon POSITION offsets are deliberately absent here,
					// and their presence was a real aiming error. They are the
					// VIEWMODEL's offsets (`Eff()` -- forward -26, right -10,
					// up 6), which place the MODEL'S ORIGIN, a point 26 units
					// BEHIND the hand and 10 to its left. That is not the
					// muzzle and it is not on the barrel's axis. Using it as
					// the convergence origin displaced the target sideways by
					// right/up regardless of distance:
					//
					//     target = modelOrigin + f*D
					//            = hand + f*D + r*(-10) + u*(6)
					//
					// -- about 3.8 degrees left at iron-sight range, which is
					// exactly the "red dot too far left of the sights"
					// screenshot. It was invisible while the distance itself
					// was a guess, because a fixed convergence was wrong by
					// more than this anyway.
					//
					// FIXED FALLBACK keeps the old form untouched. It is what
					// the existing aim_convergence_distance was tuned against,
					// and it only runs when the barrel is pointing at nothing.
					float tx, ty, tz;
					if ( m_lastConvergenceMeasured )
					{
						// hand + bore + f*D. The bore offset is what lines the
						// model's iron sights up with the point of impact:
						// dropping the viewmodel's own offsets fixed a 10-unit
						// error to the LEFT and left a smaller one to the
						// RIGHT, because the true bore is neither at the
						// controller origin nor at the model origin but a short
						// way between them.
						tx = hand.x + r.x * m_boreRight + u.x * m_boreUp + f.x * convergence;
						ty = hand.y + r.y * m_boreRight + u.y * m_boreUp + f.y * convergence;
						tz = hand.z + r.z * m_boreRight + u.z * m_boreUp + f.z * convergence;
					}
					else
					{
						const float gx = hand.x + f.x * m_weaponOffForward
										 + r.x * m_weaponOffRight + u.x * m_weaponOffUp;
						const float gy = hand.y + f.y * m_weaponOffForward
										 + r.y * m_weaponOffRight + u.y * m_weaponOffUp;
						const float gz = hand.z + f.z * m_weaponOffForward
										 + r.z * m_weaponOffRight + u.z * m_weaponOffUp;

						tx = gx + f.x * convergence;
						ty = gy + f.y * convergence;
						tz = gz + f.z * convergence;
					}

					// Published for the visualiser. Written here, at the point
					// the values are final, so what is drawn is what was used.
					m_aimGeom.muzzleRel = Vector{
						hand.x + r.x * m_boreRight + u.x * m_boreUp,
						hand.y + r.y * m_boreRight + u.y * m_boreUp,
						hand.z + r.z * m_boreRight + u.z * m_boreUp };
					m_aimGeom.forward = f;
					m_aimGeom.targetRel = Vector{ tx, ty, tz };
					m_aimGeom.valid = true;

					const float len2d = sqrtf( tx * tx + ty * ty );
					if ( len2d > 0.001f )
					{
						out.x = Clamp( atan2f( -tz, len2d ) * kDegPerRadian,
									   -m_pitchLimit, m_pitchLimit );
						out.y = NormalizeAngle( atan2f( ty, tx ) * kDegPerRadian );
						converged = true;
					}
				}

				if ( !converged )
				{
					out.x = Clamp( worldWeapon.x, -m_pitchLimit, m_pitchLimit );
					out.y = worldWeapon.y;
				}

				// Cancel the engine's viewkick, which it will add back when it
				// computes EyeAngles() + punch for the shot. Applied AFTER
				// convergence and before the clamp, so the clamp still bounds
				// the final value the engine receives.
				if ( m_aimCorrection.x != 0.0f || m_aimCorrection.y != 0.0f )
				{
					out.x = Clamp( out.x - m_aimCorrection.x, -m_pitchLimit, m_pitchLimit );
					out.y = NormalizeAngle( out.y - m_aimCorrection.y );
				}
				// Roll is meaningless to the engine's aim and rolling the
				// command angles tilts the player's whole movement basis.
				out.z = 0.0f;
				m_aimFromController = true;
			}
			// Weapon hand not tracked: fall through to head aim rather than
			// freezing on the last controller direction.
		}

		LogTrace( "camera: hmd=(%.1f %.1f %.1f) body=%.1f -> view=(%.1f %.1f %.1f) "
				  "aim=(%.1f %.1f %.1f)%s",
				  hmd.angles.x, hmd.angles.y, hmd.angles.z, m_bodyYaw,
				  m_viewAngles.x, m_viewAngles.y, m_viewAngles.z,
				  out.x, out.y, out.z, m_aimFromController ? " [controller]" : "" );

		// ---- swimming ------------------------------------------------------
		//
		// The engine swims along the COMMAND angles' pitch, which with aim
		// decoupling is the weapon hand's -- so a player holding the gun level
		// has no vertical component and sinks. Overriding the pitch here is
		// the whole fix: yaw needs nothing, because GameInput already rotates
		// the movement stick by the difference between the move frame and the
		// aim frame.
		//
		// LAST, after convergence and recoil, because it replaces the pitch
		// outright rather than adjusting it -- and while swimming neither of
		// those has anything to say about which way the player is going.
		//
		// The cost is real and worth stating: the engine takes ONE pitch for
		// both movement and shooting, so while swimming the shot follows the
		// same device as the swim. There is no third value to give it.
		m_swimPitchApplied = false;
		if ( m_swimming && m_swimPitchSource != kSwimPitchOff )
		{
			// Already clamped to the pitch limit, and already the head's.
			float pitch = m_viewAngles.x;
			if ( m_swimPitchSource == kSwimPitchController )
			{
				const ControllerPose& offHand = vr.OffHand();
				if ( offHand.valid )
					pitch = offHand.angles.x;
			}
			out.x = Clamp( pitch, -m_pitchLimit, m_pitchLimit );
			m_swimPitchApplied = true;
		}

		engine.SetViewAngles( out );

		m_lastWritten = out;
		m_haveLastWrite = true;
		// The one number the movement stick needs, taken from the two values
		// that are definitionally correct: what the head sees, and what the
		// engine was told. Both are world-space, so the body yaw and the
		// recentre offset cancel and this is a pure room-space rotation.
		m_aimYawOffset = NormalizeAngle( m_viewAngles.y - out.y );
		m_wasUiMode = m_uiMode;
	}

	// ---- WHAT THE 6DoF OFFSET IS ALLOWED TO PASS THROUGH -------------------
	//
	// Pushed in once per frame from the View_Render hook, because all three
	// pieces live there and none of them belong to the camera: the trace
	// interface, the entity to skip, and the engine's own view origin.
	//
	// Handed in rather than reached for, so that a frame where any of them is
	// missing simply does not collide -- which is the old behaviour, not a
	// crash.
	void SetCollisionContext( EngineTrace* trace, void* skipEntity,
							  const Vector& engineOrigin )
	{
		m_trace = trace;
		m_collisionSkip = skipEntity;
		m_collisionOrigin = engineOrigin;
		m_haveCollisionOrigin = true;
	}
	void ClearCollisionContext() { m_haveCollisionOrigin = false; }

	// The engine's own view origin for this frame -- CViewSetup.origin, i.e.
	// the eye Source traces from. Named for what it IS rather than for the
	// collision sweep that happens to be its first consumer, because the use
	// trace needs exactly this and it is NOT the same as the VR camera's
	// position: the offset between them is the 6DoF offset.
	//
	// Valid only when Have... is true; it is cleared rather than left stale, so
	// an origin from an earlier map cannot be read as this one's.
	bool HaveEngineViewOrigin() const { return m_haveCollisionOrigin; }
	const Vector& EngineViewOrigin() const { return m_collisionOrigin; }

	void SetCollideOffset( bool on ) { m_collideOffset = on; }
	void SetCollisionRadius( float r ) { m_collisionRadius = r; }
	void SetCollisionBackoff( float b ) { m_collisionBackoff = b; }

	bool PositionalCollided() const { return m_positionalCollided; }
	unsigned int CollisionCount() const { return m_collisionCount; }
	// A large count here means the sweep hull is too big for the spaces the
	// player is standing in -- lower positional_collide_radius.
	unsigned int CollisionStartSolid() const { return m_collisionStartSolid; }
	float LastCollisionFraction() const { return m_collisionFraction; }

	// Realign "forward in game" with "forward in the room", and re-zero the
	// positional reference at the same time.
	void Recenter() { m_pendingRecenter = true; }
	// Bumped every time a recentre completes. Anything that captured a
	// reference direction watches this and re-captures -- see the menu anchor.
	unsigned int RecentreCount() const { return m_recentreCount; }

	// Drop our influence so the engine owns the view again.
	void Release()
	{
		m_haveLastWrite = false;
		m_positionalOffset = { 0.0f, 0.0f, 0.0f };
	}

	// World-space displacement of the head from its recentre reference, in
	// Source units. Zero when positional tracking is off, when the pose is
	// invalid, or before the first recentre -- so callers can add it
	// unconditionally.
	const Vector& PositionalOffset() const { return m_positionalOffset; }

	// Head height above the play-space floor at the last recentre, which is
	// what physical crouching measures against. It is the SAME reference the
	// 6DoF offset uses, deliberately: a second calibration step would be one
	// more thing that can disagree, and recentring is already the moment the
	// player is standing where they mean to.
	// ---- THE BODY CAUGHT UP, SO THE HEAD HAS TO GIVE GROUND -------------
	//
	// True 6DoF moves the PLAYER ENTITY toward the head. If nothing else
	// happened the view would move twice: once because the entity moved (which
	// carries vs->origin with it) and once more because the head offset is
	// still measured from the old reference. The player would be shoved across
	// the room at double speed, which is the textbook way to make somebody sick.
	//
	// The offset is not stored -- it is DERIVED every frame from
	// `hmd.position - m_positionReference`. So the only correct way to shrink it
	// is to move the REFERENCE, not to subtract from the result: subtracting
	// would be overwritten by the next UpdatePositionalOffset before it was ever
	// used.
	//
	// Move the reference by the same distance the body moved and the two cancel
	// exactly: rendered eye = vs->origin + offset, both terms change by the same
	// delta with opposite sign, and the view does not move at all. That
	// stationary view is the entire safety argument for this feature.
	//
	// The delta arrives in WORLD space and the reference lives in ROOM space, so
	// it is rotated back through the body yaw and divided by the tracking scale
	// -- the exact inverse of what UpdatePositionalOffset does on the way out.
	void ShiftPositionReference( const Vector& worldDelta )
	{
		if ( !m_havePositionReference )
			return;
		if ( m_positionalScale <= 0.0001f )
			return;    // would divide by zero; scale 0 means tracking is off anyway
		if ( worldDelta.x == 0.0f && worldDelta.y == 0.0f )
			return;

		const float yawRad = ( m_bodyYaw + m_yawOffset ) * 0.01745329252f;
		const float c = cosf( yawRad );
		const float s = sinf( yawRad );

		// Inverse rotation. Forward was:
		//     out.x = dx*c - dy*s
		//     out.y = dx*s + dy*c
		const float dx =  worldDelta.x * c + worldDelta.y * s;
		const float dy = -worldDelta.x * s + worldDelta.y * c;

		m_positionReference.x += dx / m_positionalScale;
		m_positionReference.y += dy / m_positionalScale;
		// Z untouched: the body only ever chases HORIZONTALLY. Standing up is
		// not something the player's collision hull should follow, and the
		// standing height is what StandingHeight() reports to the duck logic.
	}

	bool HaveStandingHeight() const { return m_havePositionReference; }
	float StandingHeight() const { return m_positionReference.z; }

	// Units added back to the view's Z to cancel the engine's own duck, so
	// the camera tracks the real head instead of being lowered twice. See
	// physical_crouch.h -- this class only applies it.
	void SetDuckCompensation( float units ) { m_duckCompensation = units; }

	// Set from the engine's own m_nWaterLevel each frame -- 2 (waist) is
	// where CGameMovement switches to WaterMove, so it is where the pitch
	// starts steering.
	void SetSwimming( bool v ) { m_swimming = v; }
	void SetSwimPitchSource( SwimPitchSource s ) { m_swimPitchSource = s; }
	bool Swimming() const { return m_swimming; }
	bool SwimPitchApplied() const { return m_swimPitchApplied; }
	float DuckCompensation() const { return m_duckCompensation; }

	// Same room->world mapping the head uses, exposed for anything else tracked
	// in the play space -- the controllers. Takes a raw room-space position and
	// returns its world-space displacement from the recentre reference, so
	// `viewOrigin + WorldOffsetForRoomPos(controller.position)` puts a hand
	// where the player's hand really is relative to their head.
	//
	// Deliberately shares the reference and the yaw with the head: if the two
	// used different ones, the gun would drift away from the player as they
	// turned, which is exactly the class of bug that is hard to see and awful to
	// play.
	Vector WorldOffsetForRoomPos( const Vector& roomPos ) const
	{
		Vector out = { 0.0f, 0.0f, 0.0f };
		if ( !m_havePositionReference )
			return out;

		const float dx = ( roomPos.x - m_positionReference.x ) * m_positionalScale;
		const float dy = ( roomPos.y - m_positionReference.y ) * m_positionalScale;
		const float dz = ( roomPos.z - m_positionReference.z ) * m_positionalScale;

		const float yawRad = ( m_bodyYaw + m_yawOffset ) * 0.01745329252f;
		const float c = cosf( yawRad );
		const float s = sinf( yawRad );

		out.x = dx * c - dy * s;
		out.y = dx * s + dy * c;
		out.z = dz;
		return out;
	}

	// Room yaw -> world yaw, for orienting a tracked device in the world.
	float RoomYawToWorld( float roomYaw ) const
	{
		return NormalizeAngle( m_bodyYaw + roomYaw + m_yawOffset );
	}

	void SetPositionalTracking( bool on )
	{
		m_positionalTracking = on;
		if ( !on )
			m_positionalOffset = { 0.0f, 0.0f, 0.0f };
	}
	bool PositionalTracking() const { return m_positionalTracking; }

	void SetPositionalScale( float s ) { m_positionalScale = s; }

	// Hard ceiling on how far the view may be displaced. A tracking glitch that
	// reports the headset on the far side of the room would otherwise put the
	// camera inside the level geometry, and the recovery from that is worse than
	// the clamp. Also bounds how far the (uncollided) view can travel into a
	// wall until TraceEye exists.
	void SetPositionalMaxOffset( float units ) { m_positionalMaxOffset = units; }

	// Whether the last update hit that ceiling -- reported in the heartbeat,
	// because a permanently clamped offset means the reference is stale or the
	// scale is wrong, and both look like "6DoF feels weird" from inside.
	bool PositionalClamped() const { return m_positionalClamped; }

	// Head angles for rendering. Differs from what was written to the engine
	// only when the weapon hand is aiming; identical otherwise, so the renderer
	// can apply it unconditionally.
	const QAngle& ViewAngles() const { return m_viewAngles; }

	// True when this frame's engine angles actually came from the controller --
	// i.e. aim_source is controller AND the weapon hand was tracked. The second
	// half is why this is reported rather than assumed.
	bool AimFromController() const { return m_aimFromController; }

	// How far the HEAD's view yaw sits from the yaw we actually handed the
	// engine, this frame. The engine walks along its view angles, so anything
	// resolved in the head's frame -- the movement stick above all -- has to be
	// rotated by exactly this much.
	//
	// Read from here rather than recomputed from the controller pose, and that
	// is the whole point. The aim yaw is the controller's pose AFTER the Euler
	// composition of the weapon tilt, the two-handed blend, aim convergence and
	// recoil correction; the raw pose is none of those. Recomputing it from
	// `weapon.angles.y` measured 56 degrees out with the gun one-handed and up
	// to 125 degrees out with the two-handed grip engaged -- movement simply
	// stopped following the head. Two routes to one quantity will not agree.
	//
	// Zero by construction whenever the head is what aims: a menu, the melee
	// window, aim_source = head, or a weapon hand that lost tracking.
	float AimYawOffset() const { return m_aimYawOffset; }

	void SetAimSource( AimSource s ) { m_aimSource = s; }
	AimSource GetAimSource() const { return m_aimSource; }

	// A menu is up, so the head aims and nothing else does. See Apply().
	void SetUiMode( bool on ) { m_uiMode = on; }
	bool UiMode() const { return m_uiMode; }
	float LastEngineDelta() const { return m_lastEngineDelta; }
	// The engine's own view yaw as read at the top of this frame. On a menu,
	// the background map's camera direction.
	float LastEngineYaw() const { return m_lastEngineYaw; }
	float UiSuppressedYaw() const { return m_uiSuppressedYaw; }
	unsigned int UiSuppressedFrames() const { return m_uiSuppressedFrames; }

	// The scripted-camera guard, as numbers. A non-zero count on the main menu
	// is the POSITIVE result: it is the feedback loop no longer happening.
	// A non-zero count during ordinary gameplay would mean the guard is firing
	// where it should not, so it is reported rather than assumed silent.
	float ScriptedYawSuppressed() const { return m_scriptedYawSuppressed; }
	unsigned int ScriptedYawFrames() const { return m_scriptedYawFrames; }

	// Whether a scripted camera is currently driving the view, and how many
	// such stretches this session. Reported rather than acted on by anything
	// outside the camera -- the suppression itself happens in Apply().
	bool EngineDriving() const { return m_engineDriving; }
	unsigned int EngineDriveEvents() const { return m_engineDriveEvents; }
	float LastEnginePitch() const { return m_lastEnginePitch; }

	// Follow a scripted camera with the view (the flat game's behaviour) rather
	// than leaving the head in charge. Off by default: having your view rotated
	// for you is a well-known nausea trigger, and every other decision in this
	// project resolves the same way -- the head owns the view.
	void SetCutsceneFollow( bool on ) { m_cutsceneFollow = on; }
	void SetCutsceneRecenter( bool on ) { m_cutsceneRecenter = on; }

	// Pushed in each frame by the melee gesture. While set, the engine is given
	// the HEAD's angles even under aim_source = controller. See the note at the
	// aim block in Apply().
	// FL_FROZEN on the local player -- the game stating outright that the player
	// is not in control. See the note beside g_playerFlagsOffset in dllmain.
	void SetPlayerFrozen( bool on ) { m_playerFrozen = on; }
	bool PlayerFrozen() const { return m_playerFrozen; }

	// Whether FL_FROZEN alone may latch a scripted camera. Off restores the
	// old behaviour, where it only accelerated the duration heuristic -- kept
	// as the kill switch, because this flag is also set by things that are not
	// point_viewcontrol and a mis-latch suppresses body-yaw accumulation.
	void SetFrozenLatch( bool on ) { m_frozenLatch = on; }

	// Arm the per-frame rotation trace for N turning frames.
	void SetRotationTrace( bool on, int frames )
	{
		m_traceRotation = on;
		m_traceRotationLeft = on ? frames : 0;
	}
	int RotationTraceLeft() const { return m_traceRotationLeft; }

	// Ask for ONE turn-to-face, from outside the scripted-camera detector.
	//
	// Reuses the cutscene alignment's own one-shot flag, so there is a single
	// implementation of "point the body at what the camera is looking at" and
	// it is consumed the same way -- deferred until there is a look target and
	// a previous write, then cleared.
	//
	// The CALLER owns the trigger, and the trigger must be something this turn
	// cannot cause. A map change qualifies; head-vs-camera divergence does not,
	// which is the loop documented above.
	void RequestTurnToFace() { m_pendingCutsceneAlign = true; }
	bool TurnToFacePending() const { return m_pendingCutsceneAlign; }


	// For the heartbeat. Episodes, not frames, is the honest unit -- one
	// cutscene at 90 fps is thousands of frames and reads as a fault.
	unsigned int FrozenFrames() const { return m_frozenFrames; }
	unsigned int FrozenEpisodes() const { return m_frozenEpisodes; }

	// ---- WHERE THE SCRIPTED CAMERA IS LOOKING -----------------------------
	//
	// Pushed in from CViewSetup, which is the only place the cutscene camera's
	// own angles appear -- IVEngineClient::GetViewAngles holds the PLAYER's
	// command angles and is frozen at whatever pose the script ends in.
	void SetLookTarget( float pitch, float yaw )
	{
		m_lookTargetPitch = pitch;
		m_lookTargetYaw = yaw;
		m_haveLookTarget = true;
	}
	void ClearLookTarget() { m_haveLookTarget = false; }
	bool HaveLookTarget() const { return m_haveLookTarget; }
	float LookTargetYaw() const { return m_lookTargetYaw; }

	// ---- AND WHICH WAY THAT IS ON SCREEN, DONE WITH VECTORS ---------------
	//
	// The first attempt compared Euler components independently -- horizontal if
	// |yawError| >= |pitchError| -- and pointed the wrong way on the game's very
	// first cutscene. Measured there: target (pitch -87, yaw -42), head
	// (pitch -5, yaw 180), giving a yaw error of 138 against a pitch error of
	// 82. So it chose HORIZONTAL for a target that was almost straight up.
	//
	// **At the zenith, yaw is degenerate.** A target at pitch -87 has an almost
	// meaningless yaw, and no comparison of Euler components can know that. It
	// is the same trap this file already records for COMPOSING angles, applied
	// to comparing them.
	//
	// So both directions become unit vectors and the target is expressed in the
	// HEAD's own basis. `outRight` and `outUp` are then a genuine screen
	// direction, and `outDegrees` the true angle between the two -- all three
	// correct whatever the pitch.
	bool LookError( float& outRight, float& outUp, float& outDegrees ) const
	{
		outRight = 0.0f;
		outUp = 0.0f;
		outDegrees = 0.0f;
		if ( !m_haveLookTarget )
			return false;

		Vector tf, tr, tu;
		AnglesToBasis( QAngle{ m_lookTargetPitch, m_lookTargetYaw, 0.0f }, tf, tr, tu );

		Vector hf, hr, hu;
		AnglesToBasis( m_viewAngles, hf, hr, hu );

		const float alongF = tf.x * hf.x + tf.y * hf.y + tf.z * hf.z;
		const float alongR = tf.x * hr.x + tf.y * hr.y + tf.z * hr.z;
		const float alongU = tf.x * hu.x + tf.y * hu.y + tf.z * hu.z;

		// Angle between the two directions. acos of the forward component,
		// clamped because a dot product of unit vectors can drift past 1.
		float c = alongF;
		if ( c > 1.0f ) c = 1.0f;
		if ( c < -1.0f ) c = -1.0f;
		outDegrees = acosf( c ) * kDegPerRadian;

		// Screen direction, normalised. Behind the player this still points the
		// short way round, because right/up keep their sign as the target passes
		// 90 degrees off-axis.
		const float len = sqrtf( alongR * alongR + alongU * alongU );
		if ( len > 0.0001f )
		{
			outRight = alongR / len;
			outUp = alongU / len;
		}
		return true;
	}

	void SetForceHeadAim( bool on ) { m_forceHeadAim = on; }
	bool ForceHeadAim() const { return m_forceHeadAim; }

	// Controller-to-weapon transform, shared with the viewmodel. Pushed in each
	// frame rather than read from config once, because it is tuned live on the
	// numpad and the aim has to follow the model as it moves -- otherwise you
	// dial the gun in and the shots stay where they were.
	//
	// ROLL is deliberately absent. The engine's view angles drive the player's
	// movement basis as well as the shot, so rolling them tilts which way
	// "forward" walks. The model takes roll; the aim never does.
	void SetWeaponAngleOffset( float pitch, float yaw )
	{
		m_weaponAnglePitch = pitch;
		m_weaponAngleYaw = yaw;
	}

	// The weapon's POSITION offsets, needed only for convergence: without them
	// the aim would converge on where the controller is rather than where the
	// muzzle ended up, which is most of the error we are trying to remove.
	void SetWeaponPositionOffset( float forward, float right, float up )
	{
		m_weaponOffForward = forward;
		m_weaponOffRight = right;
		m_weaponOffUp = up;
	}

	// Distance, in Source units, at which the eye's line and the barrel's line
	// meet. 0 disables it and aims straight down the barrel, which is the old
	// behaviour and useful for comparison.
	void SetAimConvergence( float units ) { m_aimConvergence = units; }
	float AimConvergence() const { return m_aimConvergence; }

	// ---- THE MEASURED CONVERGENCE DISTANCE ---------------------------------
	//
	// Distance from the HAND to whatever the barrel is actually pointing at,
	// traced by ViewModelDriver because that is where the gun's world pose is
	// known. Negative means nothing was hit.
	//
	// A scalar is all that crosses this boundary, deliberately: the maths below
	// already places the muzzle correctly relative to the eye and has done for a
	// long time. It only ever lacked a good number for how far out to converge.
	// ---- WHERE THE BORE IS, RELATIVE TO THE HAND ---------------------------
	//
	// A DISTANCE, not an angle, and that distinction is the whole reason this
	// is a separate setting.
	//
	// The barrel's axis does not pass through the controller's origin -- it sits
	// slightly to one side of it and slightly above, exactly as a real bore sits
	// below its sights. Offsetting the convergence target by that much makes the
	// angular correction come out as atan(offset / distance), which is correct
	// at every range on its own.
	//
	// An angular trim would look identical at the range it was tuned at and be
	// wrong everywhere else: over-correcting up close and under-correcting far
	// away. Sight alignment is a parallax problem, so it takes a parallax fix.
	void SetBoreOffset( float right, float up )
	{
		m_boreRight = right;
		m_boreUp = up;
	}

	// ---- AND THE ANGULAR HALF, WHICH TURNED OUT TO BE THE REAL ONE --------
	//
	// The bore OFFSET above was argued for on the grounds that sight alignment
	// is a parallax problem, so it wants a distance rather than an angle. If
	// that were the whole story the alignment would hold at EVERY range -- that
	// is the property a lateral offset has, and the reason it was chosen.
	//
	// It does not hold. Reported from the headset: the crosshair drifts in and
	// out of alignment with target distance. Two other things point the same
	// way -- the value needed was -25.5 units, which is 65 cm of lateral bore
	// offset on a gun held in one hand, and every weapon wanted a wildly
	// different one.
	//
	// All three are explained by an ANGULAR error: each viewmodel is authored
	// with its barrel and sights at a slightly different angle to the model's
	// forward, and we were cancelling an angle with a parallax term. That
	// matches at exactly one range and drifts either side of it.
	//
	// Both corrections now exist because both are physically real -- a gun has
	// a bore offset AND its sights can be off-axis. Which one carries the work
	// is a question for the headset, not for this comment.
	// See the block in the convergence path. 1 restores the pre-2026-09-06
	// behaviour of measuring the aim from the physical head.
	// True when the shot is being fired from the muzzle by the trace rewrite.
	// See the block in the convergence path: the two cannot both be on, because
	// each is a correction for the other's absence.
	void SetShotFromGun( bool on ) { m_shotFromGun = on; }
	bool ShotFromGun() const { return m_shotFromGun; }

	void SetAimOriginHead( bool on ) { m_aimOriginHead = on; }
	bool AimOriginHead() const { return m_aimOriginHead; }

	void SetBoreAngles( float yaw, float pitch )
	{
		m_boreYaw = yaw;
		m_borePitch = pitch;
	}

	void SetTracedAimDistance( float d ) { m_tracedAim = d; }
	float TracedAimDistance() const { return m_tracedAim; }
	float LastConvergenceUsed() const { return m_lastConvergence; }

	// ---- WHAT THE AIM ACTUALLY BUILT, FOR THE VISUALISER -------------------
	//
	// Published rather than recomputed. A debug view that derives its own copy
	// of the geometry can disagree with the code it is meant to be showing, and
	// then it is displaying its own arithmetic -- which is worse than no view
	// at all, because it looks authoritative.
	//
	// EYE-RELATIVE, in the same frame `hand` is computed in: add the engine's
	// eye world position (the collision origin) to get world space.
	struct AimGeometry
	{
		Vector muzzleRel = { 0.0f, 0.0f, 0.0f };   // bore origin, eye-relative
		Vector forward   = { 1.0f, 0.0f, 0.0f };   // world direction, corrected
		Vector targetRel = { 0.0f, 0.0f, 0.0f };   // convergence point
		bool valid = false;
	};
	const AimGeometry& LastAimGeometry() const { return m_aimGeom; }
	bool LastConvergenceMeasured() const { return m_lastConvergenceMeasured; }

	// ---- the two-handed override ----------------------------------------
	//
	// Set each frame from TwoHanded. When a foregrip is held the gun points
	// along the line between the hands rather than along the weapon hand's
	// own orientation.
	void SetTwoHanded( bool active, const QAngle& roomAngles, float weight )
	{
		m_twoHandedActive = active;
		m_twoHandedAngles = roomAngles;
		m_twoHandedWeight = ( weight < 0.0f ) ? 0.0f
				  : ( weight > 1.0f ) ? 1.0f : weight;
	}
	bool TwoHandedActive() const { return m_twoHandedActive; }

	// The room-space weapon angles IN FORCE. Both the aim and the viewmodel
	// must call this rather than composing their own.
	//
	// The handover records what happens otherwise at a cost of one session:
	// the weapon tilt was applied to the model and not to the aim, and the
	// shots came out 58 degrees off the gun. A two-handed blend can move the
	// direction by however far apart the hands are, so the same divergence
	// would be larger and would change as the player moved.
	//
	// Roll always comes from the composed wrist, never from the blend: a line
	// between two points has no roll, and the caller's own tiltRoll is
	// preserved so the aim can keep asking for zero while the model does not.
	QAngle EffectiveWeaponAngles( const QAngle& controller, float tiltPitch,
						  float tiltYaw, float tiltRoll ) const
	{
		const QAngle composed =
			ComposeWeaponAngles( controller, tiltPitch, tiltYaw, tiltRoll );
		if ( !m_twoHandedActive || m_twoHandedWeight <= 0.0f )
			return composed;

		// A weighted average of the two FORWARD VECTORS, not of the Euler
		// angles. Averaging angles crosses the yaw wrap at 180 and goes the long
		// way round -- the same class of mistake as adding the weapon tilt
		// component-wise, which put the gun through the pitch singularity and
		// flipped it backwards. Vectors have neither problem.
		const float w = m_twoHandedWeight;
		Vector a, b;
		ForwardFromAngles( composed, a );
		ForwardFromAngles( m_twoHandedAngles, b );

		Vector v = { a.x * ( 1.0f - w ) + b.x * w,
				 a.y * ( 1.0f - w ) + b.y * w,
				 a.z * ( 1.0f - w ) + b.z * w };
		const float len = sqrtf( v.x * v.x + v.y * v.y + v.z * v.z );
		if ( len < 0.001f )
			return composed;   // opposed directions -- keep the hand we trust
		v.x /= len; v.y /= len; v.z /= len;

		QAngle out;
		out.x = -asinf( v.z ) * kDegPerRadian;
		out.y = atan2f( v.y, v.x ) * kDegPerRadian;
		out.z = composed.z;   // roll is always the wrist's
		return out;
	}

	// Recoil compensation.
	//
	// The engine fires along EyeAngles() + m_vecPunchAngle (player.cpp:5711), so
	// viewkick moves the BULLET, not just the picture. On a monitor you see the
	// gun climb and pull down without thinking; here the view comes from your
	// head, so the kick never appears and the shots simply drift upward with no
	// feedback -- and SiN's kick is systematically upward, pitch ramping 1.0 to
	// 1.5 the longer fire is held (weapon_magnum.txt).
	//
	// Subtracting it here makes the two cancel. Passed in already scaled, so 0
	// leaves recoil untouched and 1 removes it entirely.
	void SetAimCorrection( const QAngle& c ) { m_aimCorrection = c; }

	// The RAW punch, whatever recoil_compensation is set to. Diagnostic only --
	// nothing keys off it -- but it is the value that says whether the engine's
	// yaw delta during a burst IS the viewkick or something else.
	void SetPunchRaw( const QAngle& p ) { m_punchRaw = p; }

	// Remove the viewkick's share of the engine's yaw delta before it is
	// accumulated as turn input. 0 restores the old behaviour.
	void SetRecoilViewLock( bool on ) { m_recoilViewLock = on; }
	void SetRecoilLockMaxDegrees( float d ) { m_recoilLockMaxDegrees = d; }

	// "A shot is being fired." Includes a tail, set by the caller, because the
	// kick lands a frame or two after the button and keeps arriving as the
	// server's angles catch up.
	void SetFiring( bool on ) { m_firing = on; }
	float RecoilSuppressedDegrees() const { return m_recoilSuppressed; }
	unsigned int RecoilSuppressedFrames() const { return m_recoilSuppressedFrames; }

	// Stick turning. Goes straight into the body yaw rather than through
	// "+left"/"+right" because bodyYaw is already the authority on where the
	// player faces -- it is what mouse input is accumulated into. Two things
	// writing the same value would drift against each other.
	//
	// Safe to call between Apply()s: the next Apply reads the engine's angles as
	// a delta against what WE last wrote, and this does not touch that, so the
	// added yaw survives instead of being mistaken for player input.
	void AddBodyYaw( float degrees )
	{
		m_bodyYaw = NormalizeAngle( m_bodyYaw + degrees );
	}

	void SetApplyRoll( bool on ) { m_applyRoll = on; }
	bool ApplyRoll() const { return m_applyRoll; }

	// Source misbehaves as pitch approaches +-90, so this stays configurable.
	void SetPitchLimit( float degrees ) { m_pitchLimit = degrees; }

	const QAngle& LastWritten() const { return m_lastWritten; }
	float BodyYaw() const { return m_bodyYaw; }

private:
	static float Clamp( float v, float lo, float hi )
	{
		return v < lo ? lo : ( v > hi ? hi : v );
	}

	// Room-space displacement -> world-space displacement.
	//
	// Only yaw is involved. The play space floor is the world floor no matter
	// how the head is pitched or rolled, so rotating by anything other than the
	// room->game yaw would make crouching move you sideways.
	void UpdatePositionalOffset( const HmdPose& hmd )
	{
		m_positionalClamped = false;

		if ( !m_positionalTracking || !hmd.valid || !m_havePositionReference )
		{
			m_positionalOffset = { 0.0f, 0.0f, 0.0f };
			return;
		}

		const float dx = ( hmd.position.x - m_positionReference.x ) * m_positionalScale;
		const float dy = ( hmd.position.y - m_positionReference.y ) * m_positionalScale;
		const float dz = ( hmd.position.z - m_positionReference.z ) * m_positionalScale;

		// The heading the player's body faces when the head is centred, which is
		// what room-forward means in world terms.
		const float yawRad = ( m_bodyYaw + m_yawOffset ) * 0.01745329252f;
		const float c = cosf( yawRad );
		const float s = sinf( yawRad );

		Vector out;
		out.x = dx * c - dy * s;
		out.y = dx * s + dy * c;
		out.z = dz;

		// Clamp on magnitude rather than per-axis, so a diagonal lean is not
		// allowed further than a straight one.
		const float lenSq = out.x * out.x + out.y * out.y + out.z * out.z;
		const float maxLen = m_positionalMaxOffset;
		if ( maxLen > 0.0f && lenSq > maxLen * maxLen )
		{
			const float len = sqrtf( lenSq );
			const float k = maxLen / len;
			out.x *= k;
			out.y *= k;
			out.z *= k;
			m_positionalClamped = true;
		}

		// ---- AND THEN STOP IT GOING THROUGH THE WALL -----------------------
		//
		// The magnitude clamp above bounds how FAR the head may travel; it knows
		// nothing about what is in the way. Leaning into a wall put the camera
		// inside it, and at that point the near plane is behind the surface and
		// the player sees the level from the void.
		//
		// So the offset is swept against the world. A HULL, not a ray: the
		// laser's zero-extent trace is right for a bullet and wrong for a head,
		// because a point slips through the corner of a doorframe it technically
		// missed and the near plane then clips into geometry the trace called
		// clear.
		//
		// Traced from the ENGINE's eye -- the position the game believes the
		// player occupies -- because that point is by definition already inside
		// the playable space. Tracing from last frame's offset head would start
		// the sweep wherever the player had got to, including inside a wall they
		// had already leaned into, and startsolid would then let them keep going.
		m_positionalCollided = false;
		if ( m_collideOffset && m_trace && m_trace->Valid() && m_haveCollisionOrigin )
		{
			// ---- HORIZONTAL ONLY, AND THE FLOOR IS WHY -------------------
			//
			// The first version swept the whole 3D offset, which broke physical
			// crouching: crouching drives the offset's Z sharply DOWN, the sweep
			// then ran from the eye into the floor, came back with a small
			// fraction, and scaled the entire offset -- so the view stopped
			// descending and the player felt pushed back up.
			//
			// Vertical head movement is not something a wall can stop. You
			// cannot lean through a floor, and standing into a ceiling is not
			// worth a bug. What this exists to prevent is walking or leaning
			// SIDEWAYS through geometry, which is purely horizontal.
			//
			// So the sweep is flat and the fraction is applied to X and Y only.
			// Z passes through untouched, which is also what makes the duck
			// compensation below safe -- it is a vertical correction and has
			// nothing to collide with.
			const Vector target = { m_collisionOrigin.x + out.x,
									m_collisionOrigin.y + out.y,
									m_collisionOrigin.z };

			Vector hit;
			float fraction = 1.0f;
			bool startedSolid = false;
			const float e = m_collisionRadius;
			const bool blocked = m_trace->HullEx( m_collisionOrigin, target,
												  Vector{ e, e, e }, m_collisionSkip,
												  hit, fraction, startedSolid );

			// STARTSOLID IS NOT A WALL. It means the box began intersecting
			// something -- standing in a doorway with a 6-unit hull will do it --
			// and the engine then reports fraction 0 whichever way the player is
			// leaning. Clamping on it zeroes the offset outright, which reads as
			// head tracking dropping out for a moment, and it does so exactly
			// where the player is most likely to be leaning to see round a
			// corner. Left uncollided: the magnitude clamp still bounds it.
			if ( startedSolid )
			{
				++m_collisionStartSolid;
			}
			else if ( blocked )
			{
				// Back off short of the surface rather than sitting on it: a
				// camera exactly on a plane still shows z-fighting through the
				// near plane, and the backoff is what the player reads as "I
				// stopped at the wall" rather than "I am in the wall".
				float f = fraction - ( m_collisionBackoff > 0.0f && m_collisionBackoff < 1.0f
										   ? m_collisionBackoff : 0.0f );
				if ( f < 0.0f )
					f = 0.0f;

				out.x *= f;
				out.y *= f;
				// out.z deliberately untouched -- see above.
				m_positionalCollided = true;
				m_collisionFraction = fraction;
				++m_collisionCount;
			}
		}

		// The engine's duck, cancelled. AFTER the clamp on purpose: this is not
		// a lean and must not be counted against positional_max_offset, which
		// exists to bound how far a player can put their head through a wall.
		// Clamping it would also make the cancellation partial exactly when the
		// player is leaning and crouching at once, which is when a mismatch is
		// most obvious.
		//
		// It is after the COLLISION sweep for the same reason and one more: the
		// duck compensation is a correction for something the engine already
		// did, not a movement the player made, so there is nothing there to
		// collide with.
		out.z += m_duckCompensation;

		m_positionalOffset = out;
	}

	QAngle m_lastWritten = { 0.0f, 0.0f, 0.0f };
	bool m_haveLastWrite = false;
	// See AimYawOffset(). Head view yaw minus the yaw handed to the engine.
	float m_aimYawOffset = 0.0f;
	bool m_pendingRecenter = true;
	unsigned int m_recentreCount = 0;
	float m_bodyYaw = 0.0f;
	float m_yawOffset = 0.0f;
	bool m_applyRoll = true;
	float m_pitchLimit = 89.0f;

	// 6DoF collision. See UpdatePositionalOffset.
	EngineTrace* m_trace = nullptr;
	void* m_collisionSkip = nullptr;
	Vector m_collisionOrigin = { 0.0f, 0.0f, 0.0f };
	bool m_haveCollisionOrigin = false;
	bool m_collideOffset = true;
	float m_collisionRadius = 6.0f;
	float m_collisionBackoff = 0.05f;
	bool m_positionalCollided = false;
	float m_collisionFraction = 1.0f;
	unsigned int m_collisionCount = 0;
	unsigned int m_collisionStartSolid = 0;

	QAngle m_viewAngles = { 0.0f, 0.0f, 0.0f };
	AimSource m_aimSource = kAimHmd;
	bool m_uiMode = false;
	bool m_wasUiMode = false;
	// Diagnostics for the menu feedback loop -- see Apply(). A large
	// suppressed total is the POSITIVE result: it is how much per-frame
	// rewriting the menu's own camera was doing, and therefore how much
	// cancellation was being fed back into the body yaw.
	float m_lastEngineDelta = 0.0f;
	float m_lastEngineYaw = 0.0f;
	float m_uiSuppressedYaw = 0.0f;
	unsigned int m_uiSuppressedFrames = 0;

	// The engine's absolute yaw as of LAST frame, and whether we have one. Used
	// only to tell "the engine is holding a fixed heading" from "the player is
	// turning" -- see the scripted-camera block in Apply().
	float m_prevEngineYaw = 0.0f;
	bool m_haveLastEngineYaw = false;
	float m_scriptedYawSuppressed = 0.0f;
	unsigned int m_scriptedYawFrames = 0;

	// Sustained engine-driven camera -- a cutscene, as opposed to a one-frame
	// teleport. See the detector in Apply().
	float m_lastEnginePitch = 0.0f;
	unsigned int m_driveFrames = 0;
	float m_pendingEngineYaw = 0.0f;
	bool m_engineDriving = false;
	bool m_cutsceneFollow = false;
	bool m_forceHeadAim = false;
	bool m_playerFrozen = false;
	bool m_wasFrozen = false;
	bool m_frozenLatch = true;

	QAngle m_punchRaw = { 0.0f, 0.0f, 0.0f };
	float m_lastPunchYaw = 0.0f;
	float m_lastPunchDeltaY = 0.0f;
	float m_rawEngineDelta = 0.0f;
	bool m_recoilViewLock = true;
	bool m_firing = false;
	float m_recoilLockMaxDegrees = 3.0f;
	float m_recoilSuppressed = 0.0f;
	unsigned int m_recoilSuppressedFrames = 0;

	// Per-frame rotation trace. See the block in Apply.
	bool m_traceRotation = false;
	int m_traceRotationLeft = 0;
	float m_lastTraceHmdYaw = 0.0f;
	float m_lastTraceBodyYaw = 0.0f;
	float m_lastTraceViewYaw = 0.0f;
	float m_lastTraceEngYaw = 0.0f;
	unsigned int m_frozenFrames = 0;
	unsigned int m_frozenEpisodes = 0;

	// The scripted camera's own angles, and the one-shot alignment to them.
	float m_lookTargetPitch = 0.0f;
	float m_lookTargetYaw = 0.0f;
	bool m_haveLookTarget = false;
	bool m_pendingCutsceneAlign = false;
	bool m_cutsceneRecenter = true;
	// Set when a scripted camera stops driving. See the resync above -- the
	// freeze and the re-adoption are one feature.
	bool m_resyncAfterDrive = false;
	unsigned int m_engineDriveEvents = 0;

	float m_weaponAnglePitch = 0.0f;
	float m_weaponAngleYaw = 0.0f;
	float m_weaponOffForward = 0.0f;
	float m_weaponOffRight = 0.0f;
	float m_weaponOffUp = 0.0f;
	float m_aimConvergence = 0.0f;
	float m_tracedAim = -1.0f;
	float m_boreRight = 0.0f;
	float m_boreUp = 0.0f;
	float m_boreYaw = 0.0f;
	float m_borePitch = 0.0f;
	bool m_aimOriginHead = false;
	bool m_shotFromGun = false;
	mutable float m_lastConvergence = 0.0f;
	mutable bool m_lastConvergenceMeasured = false;
	mutable AimGeometry m_aimGeom;
	QAngle m_aimCorrection = { 0.0f, 0.0f, 0.0f };
	bool m_aimFromController = false;

	Vector m_positionReference = { 0.0f, 0.0f, 0.0f };
	float m_duckCompensation = 0.0f;
	bool m_swimming = false;
	static void ForwardFromAngles( const QAngle& a, Vector& out )
	{
		const float p = a.x / kDegPerRadian;
		const float y = a.y / kDegPerRadian;
		out = Vector{ cosf( p ) * cosf( y ), cosf( p ) * sinf( y ), -sinf( p ) };
	}

	bool m_twoHandedActive = false;
	float m_twoHandedWeight = 1.0f;
	QAngle m_twoHandedAngles = { 0.0f, 0.0f, 0.0f };
	bool m_swimPitchApplied = false;
	SwimPitchSource m_swimPitchSource = kSwimPitchOff;
	Vector m_positionalOffset = { 0.0f, 0.0f, 0.0f };
	bool m_havePositionReference = false;
	bool m_positionalTracking = true;
	bool m_positionalClamped = false;
	float m_positionalScale = 1.0f;
	float m_positionalMaxOffset = 80.0f;   // ~2 m in Source units
};

} // namespace sinvr
