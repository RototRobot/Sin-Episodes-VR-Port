// Controller actions -> Source console commands.
//
// Source's movement and attack inputs are "+command"/"-command" pairs held by
// the engine's own key state, so the only thing this has to get right is
// EDGES: issue "+forward" once when the stick crosses the deadzone, "-forward"
// once when it comes back. Issuing "+forward" every frame is harmless but
// issuing "-forward" every frame means you never move.
//
// Going through ClientCmd rather than synthesising keyboard input is deliberate.
// Synthetic WM_KEYDOWN has to win a race with the game's own input polling and
// leaks into anything else focused; a console command lands in exactly one
// place, works while the engine has no window focus, and is what both reference
// mods do for this.
//
// Turning is NOT a console command. `bodyYaw` inside VRCamera is already the
// authority on where the player faces -- it is what the mouse feeds into -- so
// stick turn adds to it directly. Routing it through "+left"/"+right" would
// fight the camera for the same value and produce drift.
#pragma once

#include <windows.h>
#include <math.h>
#include <stdio.h>
#include "../sdk/source_interfaces.h"
#include "../vr/vr_backend.h"
#include "../vr_camera.h"
#include "holster_zones.h"
#include "../../common/log.h"

namespace sinvr {

// Where "forward" is when the stick is pushed forward.
enum MovementDirection
{
	// The view. Source's own behaviour: +forward moves along the view yaw, which
	// is HMD-driven, so you walk where you look.
	kMoveDirHmd = 0,

	// The off-hand controller. Point the controller where you want to go and
	// look somewhere else -- the standard VR arrangement once hands are tracked,
	// and the reason it matters here is that it decouples walking from aiming.
	kMoveDirController = 1,
};

struct GameInputSettings
{
	bool enabled = true;

	MovementDirection movementDirection = kMoveDirHmd;

	// Below this the stick is treated as centred. Sticks rest slightly off zero
	// and Source has no analog movement here anyway -- it is a binary +forward
	// -- so a generous deadzone costs nothing and stops drift walking you into
	// walls while you stand still.
	float moveDeadzone = 0.35f;
	float turnDeadzone = 0.35f;

	// Snap turning by default. Smooth stick turning is a well-known nausea
	// trigger for a lot of people and this is a fast shooter; smooth is
	// available for those who prefer it.
	// The arcade-reload gesture takes the reload command over when it is
	// enabled, so the button must stop driving it or the two fight for
	// the same +reload/-reload pair.
	bool suppressReloadButton = false;

	bool snapTurn = true;
	float snapDegrees = 30.0f;
	float smoothDegreesPerSecond = 120.0f;

	// ---- the TURN stick's vertical axis ----------------------------------
	//
	// It was read, logged and used for nothing. Turning only ever needed the
	// horizontal axis, so a whole analogue axis sat free on the one control
	// every headset has -- worth more than any remaining button, because it
	// exists on all four controllers including Vive wands, where every single
	// physical input was already spoken for.
	//
	// It follows handedness and the thumbstick swap for free: the backend picks
	// which physical stick feeds `Turn` before this ever sees it, so "down on
	// the stick you turn with" stays true in every configuration without this
	// code knowing which hand that is.
	bool turnStickCrouch = true;    // push DOWN to duck -- frees the grip
	bool turnStickGrenade = false;  // push UP to throw

	// ---- THE OFF HAND'S GRIP, WHEN NOTHING ELSE WANTS IT ------------------
	//
	// On Touch and WMR every single input is spoken for -- both triggers, A/B,
	// X/Y, both sticks and both grips -- so grenades shipped with NO BUTTON AT
	// ALL and turn_stick_grenade was the only route to one.
	//
	// Except the off hand's grip is not really taken. The manifest binds
	// NextWeapon on BOTH grips, but the runtime reads it per device: the weapon
	// hand's grip draws from a holster, and the other one is left inert unless
	// two_handed_mode is `toggle` or `hold`. In the default `auto` mode nothing
	// uses it.
	//
	// So it carries grenades, and stands down the moment the foregrip wants it.
	// That is decided in dllmain from two_handed_mode rather than here -- this
	// file is told the answer, it does not go looking for it.
	bool offHandGrenade = true;

	// How far, and how straight. The second test is what stops a diagonal turn
	// from also crouching: the push has to be more vertical than horizontal, so
	// steering never triggers it however hard the stick is pushed sideways.
	float turnStickThreshold = 0.65f;

	// ---- the menu needs TWO presses ---------------------------------------
	//
	// A stick click is easy to catch by accident, and opening the menu
	// mid-firefight is worse than any other misfire in the game.
	//
	// Done HERE rather than in the binding, and that is the point. SteamVR
	// does support a `double` activation, but stacking a second button-mode
	// source onto a path a joystick source already claims did not fire -- and
	// from outside the runtime there is no way to see why. In the mod it is
	// four lines, it works identically on every controller, and it can be
	// logged. The project already uses exactly this shape for the numpad
	// reset, for exactly the same reason.
	bool menuDoubleClick = true;
	unsigned int menuDoubleClickMs = 400;

	// Log both edges of +use with the state that decides where it lands. Off by
	// default: it is an investigation aid, not telemetry. See the block in
	// Apply() for what it is chasing and why these particular fields.
	bool useTrace = false;

	// Resolve +use along the GAZE rather than along the gun.
	//
	// Measured 2026-09-01: with aim_source = controller the engine's aim pitch
	// is the controller's pose plus viewmodel_angle_pitch (58 degrees), so the
	// use trace runs 9-24 degrees BELOW where the player is looking. Confirmed
	// twice against the heartbeat: controller pitch -25 + 58 = 33 and the engine
	// had 33; -34 + 58 = 24 and the engine had 25. That is correct for shooting,
	// because the muzzle has to match the tilted gun model, and wrong for a use
	// trace, which the player aims with their eyes.
	//
	// Same mechanism melee already uses (melee_aim_forward), and it must be
	// OR'd into the SAME SetForceHeadAim call rather than written separately.
	bool useAimHead = true;

	// How long head aim outlives the button. The window is refreshed every frame
	// the button is down, so this is only the tail after release -- it exists
	// because ClientCmd is queued and the engine samples the button on a later
	// frame than the one we set it on.
	float useAimTailSeconds = 0.20f;
};

// Is this string safe to paste into a console command?
//
// Source's command buffer treats ';' and newline as separators and '"' as
// quoting, so any config value interpolated into a ClientCmd string must be
// restricted to characters that cannot end the command. Whitelist, not
// blacklist: the set of things a sound name legitimately contains is small and
// known, and the set of ways to escape a command interpreter is not.
//
// Length is capped well under the 96-byte command buffer so the name cannot be
// what truncates it.
inline bool IsSafeSoundName( const char* s )
{
	if ( !s || !*s )
		return false;

	size_t n = 0;
	for ( const char* p = s; *p; ++p, ++n )
	{
		if ( n >= 64 )
			return false;
		const unsigned char c = (unsigned char)*p;
		const bool ok = ( c >= 'A' && c <= 'Z' ) || ( c >= 'a' && c <= 'z' ) ||
						( c >= '0' && c <= '9' ) ||
						c == '.' || c == '_' || c == '-' || c == '/';
		if ( !ok )
			return false;
	}
	return true;
}

// Translates one frame of VRInputState into engine commands, remembering what it
// has already asserted so each +/- is issued exactly once.
class GameInput
{
public:
	void SetSettings( const GameInputSettings& s ) { m_settings = s; }
	const GameInputSettings& Settings() const { return m_settings; }
	// Reported so a menu that will not open can be told apart from a stick
	// click that never arrives: first presses climbing with opens at zero
	// means the button works and the second click is missing the window.
	unsigned int MenuFirstPresses() const { return m_menuFirstPresses; }
	unsigned int MenuOpens() const { return m_menuOpens; }

	// True while +use should resolve along the gaze rather than along the gun.
	// Read once per frame by the caller and OR'd with the melee window into the
	// SINGLE SetForceHeadAim call -- two writers would each track their own idea
	// of whether the flag is wanted and the last to clear it would win, which is
	// the same shape as the +duck two-writer bug this file already guards.
	bool UseAimHead() const
	{
		if ( !m_settings.useAimHead || m_useAimUntilMs == 0 )
			return false;
		return (DWORD)( GetTickCount() - m_useAimUntilMs ) > 0x80000000u;
	}

	// `dt` is real seconds since the last call, used only for smooth turning.
	// `moveYawOffset` is degrees to rotate the movement stick by before it is
	// resolved into +forward/+moveleft/... -- see RotateStick.
	void Apply( const EngineClient& engine, const VRInputState& in,
				VRCamera& camera, float dt, float moveYawOffset )
	{
		if ( !m_settings.enabled || !engine.Valid() )
			return;

		if ( !in.valid )
		{
			// Controllers went away mid-press. Release everything rather than
			// leaving the engine holding +forward forever.
			ReleaseAll( engine );
			return;
		}

		// ---- movement ------------------------------------------------------
		float mx = in.moveX;
		float my = in.moveY;
		// Applied unconditionally: the caller works out the offset from BOTH the
		// movement source and the aim source, and it is exactly zero when they
		// agree. Gating it on movementDirection here would miss the case where
		// movement is head-relative but the engine is aiming with the
		// controller, which is the whole point of aim decoupling.
		if ( moveYawOffset != 0.0f )
			RotateStick( mx, my, moveYawOffset );
		m_lastMoveYawOffset = moveYawOffset;

		const float dz = m_settings.moveDeadzone;

		Hold( engine, m_forward, my > dz, "+forward", "-forward" );
		Hold( engine, m_back, my < -dz, "+back", "-back" );
		Hold( engine, m_moveRight, mx > dz, "+moveright", "-moveright" );
		Hold( engine, m_moveLeft, mx < -dz, "+moveleft", "-moveleft" );

		// ---- held actions --------------------------------------------------
		Hold( engine, m_attack, in.attack, "+attack", "-attack" );
		Hold( engine, m_attack2, in.attack2, "+attack2", "-attack2" );
		Hold( engine, m_jump, in.jump, "+jump", "-jump" );
		// ---- USE, and the trace behind it ----------------------------------
		//
		// Chasing a soft lock on the intro car: the door opened, the player did
		// not end up in the car, and the driving script that waits on that never
		// fired. It worked on the retry, which is the signature of something
		// marginal rather than broken -- so what is wanted is the STATE at the
		// moment of the press, on the attempt that fails.
		//
		// Source resolves +use with a trace from the eye along the ENGINE's view
		// angles. With aim_source = controller those are the controller's, not
		// the head's, so two quantities decide where a use lands and neither is
		// visible to the player:
		//
		//   aimYaw   how far the engine's aim sat from the head. Taken from
		//            AimYawOffset() and NOT recomputed from the controller pose
		//            -- that is composed with weapon tilt, the two-handed blend,
		//            convergence and recoil, and measuring it the other way was
		//            already found to be up to 125 degrees out. Two routes to
		//            one quantity will not agree.
		//   posOff   how far the camera sat from the origin the trace starts
		//            from. Lean in to reach a door and you are still tracing
		//            from the player origin, which is not where you are looking
		//            from.
		//
		// Logged on BOTH edges, because in Source +use ENTERS *and* EXITS a
		// vehicle. How long it was held is evidence, not decoration: a press
		// that spans the entry could plausibly be read as an exit.
		//
		// frozen is here because FL_FROZEN is how this game hands control to a
		// script -- if the sequence started and then let go, that is a different
		// bug from the sequence never starting.
		// Head aim for the duration of the press plus a tail. Refreshed every
		// frame the button is down, so releasing simply lets it expire.
		//
		// NOTE the one-frame lag: this is read by the caller BEFORE Apply runs,
		// so the very first frame of a press still writes controller-aimed
		// angles. It does not matter and is not worth restructuring for -- the
		// shortest press measured on hardware was 156 ms (14 frames) and
		// ClientCmd is queued anyway, so the engine samples the button well
		// inside the window. The tail covers the other end.
		if ( m_settings.useAimHead && in.use )
			m_useAimUntilMs =
				GetTickCount() + (DWORD)( m_settings.useAimTailSeconds * 1000.0f );

		if ( m_settings.useTrace && in.use != m_use )
		{
			const Vector& off = camera.PositionalOffset();
			const float   offLen = sqrtf( off.x * off.x + off.y * off.y + off.z * off.z );

			if ( in.use )
			{
				m_useDownMs = GetTickCount();
				const QAngle& va = camera.ViewAngles();
				const QAngle& ea = camera.LastWritten();
				// PITCH is printed as head-vs-engine because that is where the
				// error actually was, and the first version of this trace missed
				// it: it reported AimYawOffset() alone, which read under 5
				// degrees and made the aim look correct while the trace was
				// running up to 24 degrees low. There is no pitch equivalent in
				// VRCamera -- yaw is tracked because movement needs it -- so the
				// two angles are printed side by side and the delta with them.
				// WHERE the trace starts from, not just which way it points.
				//
				// This is the field the earlier versions of this trace lacked,
				// and it is the one that matters for an INTERMITTENT failure:
				// the same press from half a step further left is a different
				// trace. Source's FindUseEntity starts at the eye and takes the
				// best-aligned entity within PLAYER_USE_RADIUS, so with two use
				// targets close together -- a car door and a car seat -- which
				// one wins is decided by exactly this position and the angles
				// above. Log both, gather several presses that WORK and several
				// that do not, and diff them. A single failing press proves
				// nothing on a bug that only sometimes fires.
				const bool  haveOrg = camera.HaveEngineViewOrigin();
				const Vector& org = camera.EngineViewOrigin();

				Log( "use: PRESS | eye=(%.1f %.1f %.1f)%s | aim %s%s | pitch "
					 "head=%.1f engine=%.1f (delta %+.1f) | yaw head=%.1f "
					 "engine=%.1f (delta %+.1f) | posOffset=%.1f units%s%s | frozen=%d",
					 haveOrg ? org.x : 0.0f,
					 haveOrg ? org.y : 0.0f,
					 haveOrg ? org.z : 0.0f,
					 haveOrg ? "" : " (STALE -- no view setup this frame)",
					 camera.AimFromController() ? "from CONTROLLER" : "from head",
					 camera.ForceHeadAim() ? " (head-aim FORCED)" : "",
					 va.x, ea.x, ea.x - va.x,
					 va.y, ea.y, ea.y - va.y,
					 offLen,
					 camera.PositionalClamped() ? " CLAMPED" : "",
					 camera.PositionalCollided() ? " COLLIDED" : "",
					 camera.PlayerFrozen() ? 1 : 0 );
			}
			else
			{
				Log( "use: RELEASE after %lu ms | posOffset=%.1f units | frozen=%d",
					 (unsigned long)( GetTickCount() - m_useDownMs ),
					 offLen,
					 camera.PlayerFrozen() ? 1 : 0 );
			}
		}

		Hold( engine, m_use, in.use, "+use", "-use" );
		// Driven to false rather than skipped when the gesture owns it, so
		// a button held at the moment the setting changes is released
		// exactly once instead of sticking down forever.
		Hold( engine, m_reload,
			  m_settings.suppressReloadButton ? false : in.reload,
			  "+reload", "-reload" );
		// Down on the turn stick ducks. ORed with the button and the physical
		// crouch so there is still exactly ONE writer of +duck -- two would each
		// track their own idea of whether it is held and the last to release
		// would win, which is how a player gets stuck crouching.
		m_stickCrouch = m_settings.turnStickCrouch && StickPushed( in, -1.0f );
		m_stickGrenade = m_settings.turnStickGrenade && StickPushed( in, +1.0f );

		Hold( engine, m_crouch, in.crouch || m_physicalCrouch || m_stickCrouch,
			  "+duck", "-duck" );

		// SiN has a STANDALONE grenade throw -- `bind "g" "+grenade"` in its own
		// config_default.cfg, and the command is present in client.dll. This is
		// NOT the assault rifle's alt fire, which this project previously
		// concluded was the only grenade in the game. That conclusion came from
		// reading the weapon scripts and never checking the key binds.
		// HELD, not pressed: `+grenade` is a hold, and the off-hand grip gives
		// a hold for free. Using the press edge would throw on contact and
		// remove any control over the throw.
		const bool gripGrenade = m_settings.offHandGrenade && in.offHandGrip;
		Hold( engine, m_grenade, in.grenade || m_stickGrenade || gripGrenade,
			  "+grenade", "-grenade" );

		// ---- one-shots -----------------------------------------------------
		// Grip on the weapon hand. Inside a holster zone it draws that zone's
		// weapon directly; everywhere else it keeps its original meaning and
		// cycles. Reusing the button this way means no manifest change and no
		// rebinding for anyone, and a player who never reaches for a zone sees
		// no difference at all.
		if ( in.nextWeapon )
		{
			const int slot = m_holsters ? m_holsters->SlotForCurrentZone() : 0;
			if ( m_holsters )
				m_holsters->NotePress( slot > 0 );
			if ( slot > 0 )
			{
				char cmd[16];
				_snprintf_s( cmd, sizeof( cmd ), _TRUNCATE, "slot%d\n", slot );
				engine.ClientCmd( cmd );

				// `slotN` under hud_fastswitch is SILENT -- it returns from
				// FastWeaponSwitch before the engine's own EmitSound, so a
				// holster draw had no audible confirmation while the cycle it
				// replaced did. Issued separately rather than by turning
				// fastswitch off, because fastswitch is what makes the draw
				// instant, which is the whole point of reaching for it.
				// See the note on HolsterSettings::drawSound.
				const char* snd = m_holsters->Settings().drawSound;
				// Validated, because this is the ONE place a config STRING
					// reaches the engine's command interpreter. Source splits a
					// command buffer on ';' and newline, so an unchecked value
					// here turns `holster_draw_sound` into arbitrary console
					// execution every time the player draws from a holster --
					// and this mod sets `sv_cheats 1`, so the reachable surface
					// is the whole cheat-flagged command set.
					//
					// The realistic vector is not someone editing their own
					// file, which needs no exploit: it is a tuned sinvr.cfg
					// passed between players, which is a normal thing for a mod
					// to have happen. A sound name is [A-Za-z0-9._/-] and
					// nothing else, so the whitelist costs nothing.
					if ( snd && *snd && IsSafeSoundName( snd ) )
				{
					char play[96];
					_snprintf_s( play, sizeof( play ), _TRUNCATE,
								 "playgamesound %s\n", snd );
					engine.ClientCmd( play );
				}
			}
			else if ( !m_holsters || !m_holsters->Settings().enabled
					  || m_holsters->Settings().gripCycles )
			{
				// ---- GRIP OUTSIDE A ZONE ---------------------------------
				//
				// The grip is bound to NextWeapon in the manifest and the
				// holsters only add a CONTEXT to it, so outside a zone it does
				// what it always did. That is the design -- nothing is lost and
				// nobody has to rebind -- but it also means a grip pressed
				// anywhere else swaps your weapon, which is easy to do by
				// accident while steadying a two-handed gun or just resting a
				// hand.
				//
				// So this line is OFF by default and runs only when
				// holster_grip_cycles is set back to 1, or when the holsters
				// are disabled entirely and the grip has nothing else to mean.
				//
				// Worth knowing before changing it back: PrevWeapon is bound to
				// NOTHING on any of the four controller profiles, so this is the
				// only weapon cycle the mod has. It is safe to lose because SiN
				// has three guns and there are three zones -- a countable
				// argument, not a hopeful one. Add a fourth weapon with no zone
				// assigned and it becomes unreachable, with no keyboard to fall
				// back on inside a headset.
				engine.ClientCmd( "invnext\n" );
			}
		}
		if ( in.prevWeapon )
			engine.ClientCmd( "invprev\n" );
		if ( in.flashlight )
			engine.ClientCmd( "impulse 100\n" );
		if ( MenuPressed( in.menu ) )
			SendEscapeKey();
		if ( in.recenter )
			camera.Recenter();

		// ---- turning -------------------------------------------------------
		Turn( in.turnX, camera, dt );
	}

	// The MENU button, and ONLY that, for when the rest of input is held off.
	//
	// Gating gameplay input on menus introduced a trap: the button could OPEN a
	// menu and then never close it, because Apply() -- which reads it -- stops
	// being called the moment a menu appears. Escape has to work from both sides
	// or it is not a menu button, it is a menu opener.
	// Opening the menu needs the KEY, not a command.
	//
	// `escape` really is a registered command -- its help string is in
	// engine.dll -- and the double-click detector really was firing: the log
	// read `6 first presses, 4 opened` while the menu stayed shut the whole
	// time. So the command was reaching the engine and being dropped.
	//
	// IVEngineClient::ClientCmd will not run an engine-level command; it is for
	// the client ones this file already uses successfully (invnext, impulse 100,
	// the +/- pairs). There is no console command that opens the menu in this
	// build either -- engine.dll has `gameui_hide` but no `gameui_activate`,
	// because the 2004 engine handles ESC inside its own key handler rather
	// than through a command.
	//
	// So the key is synthesised. The handover's warning about synthetic input is
	// about HELD movement racing the engine's per-frame poll; a one-shot toggle
	// has no held state to get wrong, and this is the same SendInput path the
	// menu pointer already uses for its clicks.
	static void SendEscapeKey()
	{
		INPUT in[2] = {};
		in[0].type = INPUT_KEYBOARD;
		in[0].ki.wVk = VK_ESCAPE;
		in[0].ki.wScan = (WORD)MapVirtualKey( VK_ESCAPE, MAPVK_VK_TO_VSC );
		in[1] = in[0];
		in[1].ki.dwFlags = KEYEVENTF_KEYUP;
		SendInput( 2, in, sizeof( in[0] ) );
	}

	// True only on the SECOND press inside the window.
	//
	// Shared by both entry points, so opening and closing a menu take the same
	// gesture -- Apply() runs in gameplay and ApplyMenuOnly() runs while a menu
	// is up, and a menu button that opens with two clicks but closes with one
	// would be its own kind of wrong.
	bool MenuPressed( bool pressed )
	{
		if ( !pressed )
			return false;
		if ( !m_settings.menuDoubleClick )
			return true;

		const DWORD now = GetTickCount();
		if ( m_lastMenuPressMs != 0 &&
			 ( now - m_lastMenuPressMs ) < m_settings.menuDoubleClickMs )
		{
			m_lastMenuPressMs = 0;      // consumed, so three clicks are not two menus
			++m_menuOpens;
			return true;
		}
		m_lastMenuPressMs = now;
		++m_menuFirstPresses;
		return false;
	}

	// Is the TURN stick pushed hard enough, and straight enough, in `sign`?
	//
	// The second test is the one that matters. Without it, steering hard while
	// the thumb drifts low would duck the player mid-turn -- and a stick is
	// pushed diagonally far more often than anyone expects. Requiring the push
	// to be more vertical than horizontal makes turning and this gesture
	// mutually exclusive by construction rather than by a tuned threshold.
	bool StickPushed( const VRInputState& in, float sign ) const
	{
		const float y = in.turnY * sign;
		if ( y < m_settings.turnStickThreshold )
			return false;
		const float ax = in.turnX < 0.0f ? -in.turnX : in.turnX;
		return y > ax;
	}

	void ApplyMenuOnly( const EngineClient& engine, const VRInputState& in )
	{
		if ( !engine.Valid() || !in.valid )
			return;
		if ( MenuPressed( in.menu ) )
			SendEscapeKey();
	}

	// Public so the hook can call it when VR input stops owning the game --
	// leaving +attack asserted while the player takes the headset off is the
	// kind of bug that empties a magazine into a wall.
	void ReleaseAll( const EngineClient& engine )
	{
		if ( !engine.Valid() )
			return;
		Hold( engine, m_forward, false, "+forward", "-forward" );
		Hold( engine, m_back, false, "+back", "-back" );
		Hold( engine, m_moveLeft, false, "+moveleft", "-moveleft" );
		Hold( engine, m_moveRight, false, "+moveright", "-moveright" );
		Hold( engine, m_attack, false, "+attack", "-attack" );
		Hold( engine, m_attack2, false, "+attack2", "-attack2" );
		Hold( engine, m_jump, false, "+jump", "-jump" );
		Hold( engine, m_use, false, "+use", "-use" );
		Hold( engine, m_reload, false, "+reload", "-reload" );
		Hold( engine, m_crouch, false, "+duck", "-duck" );
		Hold( engine, m_grenade, false, "+grenade", "-grenade" );
	}

	// Physical crouching, ORed into the crouch button below.
	//
	// Fed in rather than driving "+duck" itself, so there stays exactly ONE
	// writer on that command pair. Two writers would each track their own
	// idea of whether it is held, and the one that released last would win --
	// which is how a player ends up stuck crouching after letting go of a
	// button they were not the last to press.
	void SetPhysicalCrouch( bool v ) { m_physicalCrouch = v; }

	// Optional. Null means the grip behaves exactly as it always did.
	void SetHolsterZones( HolsterZones* h ) { m_holsters = h; }

	// Diagnostics for the heartbeat.
	unsigned int CommandsIssued() const { return m_commandsIssued; }
	float LastTurnDelta() const { return m_lastTurnDelta; }
	float LastMoveYawOffset() const { return m_lastMoveYawOffset; }

private:
	// Controller-relative movement, done entirely on this side.
	//
	// Source's +forward moves along the VIEW yaw, and there is no cvar for
	// "move along something else". But the four movement commands are just the
	// view frame's axes, so rotating the stick vector by (controller yaw - view
	// yaw) before deciding which ones to press lands the player in the right
	// direction with no engine change at all.
	//
	// The offset simplifies pleasantly. Controller and head are both in room
	// space, and both map to world by the same (bodyYaw + recenterOffset):
	//
	//     controllerWorld - viewWorld
	//       = (ctrlRoom + body + offset) - (hmdRoom + body + offset)
	//       = ctrlRoom - hmdRoom
	//
	// so no world conversion is needed anywhere -- it is just the room-space yaw
	// between the two devices.
	//
	// Movement stays quantised to the engine's four directions, so this is a
	// nearest-of-four approximation, not free 360 strafing. With a 0.35 deadzone
	// that is what the stick gives you anyway.
	static void RotateStick( float& x, float& y, float degrees )
	{
		const float r = degrees * 0.01745329252f;
		const float c = cosf( r );
		const float s = sinf( r );
		const float nx = x * c - y * s;
		const float ny = x * s + y * c;
		x = nx;
		y = ny;
	}

	void Hold( const EngineClient& engine, bool& state, bool want,
			   const char* onCmd, const char* offCmd )
	{
		if ( state == want )
			return;
		state = want;
		engine.ClientCmd( want ? onCmd : offCmd );
		++m_commandsIssued;
	}

	void Turn( float x, VRCamera& camera, float dt )
	{
		m_lastTurnDelta = 0.0f;
		const float dz = m_settings.turnDeadzone;

		if ( m_settings.snapTurn )
		{
			// One step per push. The stick has to return inside the deadzone
			// before it will fire again, otherwise holding it spins.
			if ( fabsf( x ) <= dz )
			{
				m_snapArmed = true;
				return;
			}
			if ( !m_snapArmed )
				return;
			m_snapArmed = false;
			m_lastTurnDelta = ( x > 0.0f ) ? m_settings.snapDegrees : -m_settings.snapDegrees;
		}
		else
		{
			if ( fabsf( x ) <= dz )
				return;
			// Rescale so the deadzone edge is zero speed rather than full speed.
			const float span = 1.0f - dz;
			const float mag = ( fabsf( x ) - dz ) / ( span > 0.0f ? span : 1.0f );
			const float dir = ( x > 0.0f ) ? 1.0f : -1.0f;
			m_lastTurnDelta = dir * mag * m_settings.smoothDegreesPerSecond * dt;
		}

		if ( m_lastTurnDelta != 0.0f )
			camera.AddBodyYaw( -m_lastTurnDelta );
	}

	GameInputSettings m_settings;

	bool m_forward = false;
	bool m_back = false;
	bool m_moveLeft = false;
	bool m_moveRight = false;
public:
	// For the recoil view lock: SiN's kick arrives as a change to the player's
	// networked view angles, so the only way to tell it from a real turn is to
	// know a shot is being fired.
	bool AttackHeld() const { return m_attack || m_attack2; }

private:
	bool m_attack = false;
	bool m_attack2 = false;
	bool m_jump = false;
	bool m_use = false;
	// Tick at the last +use edge, for the held-duration line in the use trace.
	DWORD m_useDownMs = 0;
	// End of the gaze-aim window for +use. Wrap-safe, same test melee uses.
	DWORD m_useAimUntilMs = 0;
	bool m_reload = false;
	bool m_crouch = false;
	bool m_grenade = false;
	bool m_stickCrouch = false;
	DWORD m_lastMenuPressMs = 0;
	unsigned int m_menuFirstPresses = 0;
	unsigned int m_menuOpens = 0;
	bool m_stickGrenade = false;

	bool m_snapArmed = true;
	float m_lastTurnDelta = 0.0f;
	float m_lastMoveYawOffset = 0.0f;
	unsigned int m_commandsIssued = 0;
	HolsterZones* m_holsters = nullptr;
	bool m_physicalCrouch = false;
};

} // namespace sinvr
