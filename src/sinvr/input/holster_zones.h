// Holster zones -- reach to a place on your body and grip to draw that weapon.
//
// SiN registers slot1..slot9, the same direct weapon-select commands the number
// keys use, so this needs no new engine work. What it needs is a reliable answer
// to "where is the weapon hand relative to the player's body".
//
// ---- IT REUSES THE GRIP RATHER THAN ADDING AN ACTION ------------------------
//
// The weapon hand's grip is already bound to NextWeapon (right grip in the
// manifest; `left_handed` swaps the action handles, so it follows the weapon
// hand automatically). Rather than add a manifest action -- which every player
// would then have to rebind, and which would need a new default in four binding
// files -- the grip keeps its meaning and gains a context:
//
//     grip INSIDE a zone   -> select that zone's weapon   (slot1 / slot2 / ...)
//     grip OUTSIDE a zone  -> nothing, by default
//
// That second line was originally `invnext`, so the grip kept its old meaning
// wherever a zone did not claim it and nothing was lost. It was reported as a
// bug in play, and correctly: a grip is pressed incidentally all the time --
// steadying a two-handed gun, resting a hand -- and every one of those swapped
// the weapon.
//
// Turning it off is safe here for a countable reason rather than a hopeful one:
// SiN has THREE guns and there are THREE zones, so the cycle can reach nothing
// a zone does not already cover. `holster_grip_cycles = 1` restores it for a
// setup where that stops being true.
//
// ---- SAME FRAME AS THE MELEE GESTURE ----------------------------------------
//
// Room space, relative to the head's own yaw -- see melee_gesture.h for why that
// frame rather than world space. The short version: a snap turn does not move
// your hand, and "across the body" only means something in the player's own
// frame.
//
// Heights are measured DOWN FROM THE HEAD, so they follow the player's height
// instead of assuming one. A zone is therefore the same reach for a tall player
// and a short one.
#pragma once

#include <windows.h>
#include <math.h>
#include "../sdk/source_interfaces.h"
#include "../vr/vr_backend.h"
#include "zone_box.h"
#include "../../common/log.h"

namespace sinvr {

enum HolsterZone
{
	kHolsterNone = 0,
	kHolsterLeft,     // out to the player's left, upper body
	kHolsterRight,    // out to the player's right, upper body
	kHolsterHip,      // down at the waist
};

inline const char* HolsterZoneName( HolsterZone z )
{
	switch ( z )
	{
		case kHolsterLeft:  return "LEFT";
		case kHolsterRight: return "RIGHT";
		case kHolsterHip:   return "HIP";
		default:            return "none";
	}
}

struct HolsterSettings
{
	bool enabled = false;

	// Which weapon each zone draws. These are Source's slotN commands, i.e. the
	// number keys: 1 pistol, 2 shotgun, 3 assault rifle in SiN's default order.
	int leftSlot = 3;
	int rightSlot = 2;
	int hipSlot = 1;

	// Whether a grip OUTSIDE every zone still cycles weapons, as it did before
	// holsters existed. Default OFF: SiN has three guns and there are three
	// zones, so the cycle has nothing to reach that a zone does not already
	// cover, and leaving it on means every incidental grip swaps your weapon.
	bool gripCycles = false;

	// The three zones, as boxes. Owned by ZoneSet and pointed at here, so
	// the box being drawn and the box being tested are the same object --
	// a copy would let the picture and the behaviour drift apart, which is
	// the one failure this whole exercise cannot tolerate.
	const ZoneBox* hip = nullptr;
	const ZoneBox* left = nullptr;
	const ZoneBox* right = nullptr;

	// Log the measured position on every grip press, whether or not it hit a
	// zone. This is how the numbers above get set from a real player's reach
	// instead of guessed -- the same approach the melee thresholds used.
	bool debug = false;

	// ---- WHY A DRAW HAS TO PLAY ITS OWN SOUND ------------------------------
	//
	// `invnext` is audible and `slotN` is not, and that is the ENGINE's
	// behaviour, not something this mod broke. Both go through the weapon
	// selection HUD, and only one of the paths through it emits:
	//
	//   invnext -> CycleToNextWeapon()   always EmitSound(
	//                                    "Player.WeaponSelectionMoveSlot" )
	//   slotN   -> SelectWeaponSlot()
	//              hud_fastswitch 1 -> FastWeaponSwitch() and RETURN, before
	//                                  the EmitSound at the end of the
	//                                  function. Silent on success; it only
	//                                  emits "Player.DenyWeaponSelection" on
	//                                  failure.
	//              hud_fastswitch 0 -> opens the selection HUD and emits, but
	//                                  does NOT equip anything
	//
	// So the silence is the price of the instant switch, and the instant switch
	// is the thing a holster draw needs. Verified against SiN's own client.dll
	// rather than assumed from the SDK: `playgamesound`, `hud_fastswitch`,
	// `Player.WeaponSelected` and `Player.WeaponSelectionMoveSlot` are all
	// present in that binary, and the sounds are defined in
	// SE1/scripts/game_sounds_ui.txt.
	//
	// A soundscript NAME rather than a bool, so it can be changed to the cycle
	// sound -- or emptied to silence it -- without a rebuild. Empty = no sound.
	const char* drawSound = "Player.WeaponSelected";
};

class HolsterZones
{
public:
	void SetSettings( const HolsterSettings& s ) { m_settings = s; }
	const HolsterSettings& Settings() const { return m_settings; }

	// Where the weapon hand is right now. Called every frame so the zone is
	// known before the grip is read, and so the heartbeat can report it.
	void Update( IVRBackend& vr )
	{
		m_zone = kHolsterNone;
		m_valid = false;

		if ( !m_settings.enabled )
			return;

		const ControllerPose& hand = vr.WeaponHand();
		const HmdPose& head = vr.Hmd();
		if ( !hand.valid || !head.valid )
			return;

		// Shared with every other zone consumer, so a box means the same
		// place here as it does where it is drawn.
		ToBodyFrame( hand.position, head.position, head.angles.y,
				 m_forward, m_lateral, m_height );
		m_valid = true;

		// Hip first, unchanged in spirit from the threshold version: a hand at
		// waist height is a hip draw wherever it is sideways, and testing the
		// side zones first would claim it whenever the arm hung slightly wide.
		// With boxes the overlap is visible rather than implied, which is most
		// of why they are boxes.
		if ( m_settings.hip && m_settings.hip->Contains( m_forward, m_lateral, m_height ) )
		{
			m_zone = kHolsterHip;
			return;
		}
		if ( m_settings.left && m_settings.left->Contains( m_forward, m_lateral, m_height ) )
		{
			m_zone = kHolsterLeft;
			return;
		}
		if ( m_settings.right && m_settings.right->Contains( m_forward, m_lateral, m_height ) )
			m_zone = kHolsterRight;
	}

	HolsterZone Zone() const { return m_zone; }

	// The slot for the current zone, or 0 for "no zone -- do the normal thing".
	int SlotForCurrentZone() const
	{
		switch ( m_zone )
		{
			case kHolsterLeft:  return m_settings.leftSlot;
			case kHolsterRight: return m_settings.rightSlot;
			case kHolsterHip:   return m_settings.hipSlot;
			default:            return 0;
		}
	}

	// Called by GameInput when the grip is pressed, so the measurement lands in
	// the log at the moment the player expected something to happen. A press
	// that hit no zone is logged too -- "I reached and nothing happened" needs
	// the numbers as much as a successful draw does.
	void NotePress( bool hitZone )
	{
		if ( hitZone )
			++m_draws;
		else
			++m_missedReaches;

		if ( !m_settings.debug )
			return;

		Log( "holster: grip at fwd=%.1f lateral=%.1f height=%.1f -> %s%s",
			 m_forward, m_lateral, m_height, HolsterZoneName( m_zone ),
			 hitZone ? ""
					 : ( m_settings.gripCycles ? "  -- no zone, cycling instead"
											   : "  -- NO ZONE, nothing happened" ) );
	}

	void LogState() const
	{
		if ( !m_settings.enabled )
			return;
		Log( "holster: zones on (L=slot%d R=slot%d hip=slot%d) | now %s "
			 "hand fwd=%.1f lat=%.1f h=%.1f | draws=%u reaches that missed=%u",
			 m_settings.leftSlot, m_settings.rightSlot, m_settings.hipSlot,
			 m_valid ? HolsterZoneName( m_zone ) : "<no tracking>",
			 m_forward, m_lateral, m_height, m_draws, m_missedReaches );
	}

private:
	HolsterSettings m_settings;

	HolsterZone m_zone = kHolsterNone;
	bool m_valid = false;
	float m_forward = 0.0f;
	float m_lateral = 0.0f;
	float m_height = 0.0f;

	unsigned int m_draws = 0;
	unsigned int m_missedReaches = 0;
};

} // namespace sinvr
