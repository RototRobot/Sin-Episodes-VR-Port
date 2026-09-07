// Suppressing viewmodel animations that fight the player's own arm.
//
// ---- WHY THIS IS A SEPARATE PROBLEM FROM PINNING THE VIEWMODEL --------------
//
// viewmodel.h writes the viewmodel entity's TRANSFORM every frame, so the gun
// sits where the controller is. Animation is a different axis entirely: it moves
// BONES relative to that transform. The two never competed for the same value,
// which is why animation survived the transform work untouched -- and why the
// gun still performs motions the player's arm did not make, anchored to a hand
// that is somewhere else.
//
// The melee work made this obvious. Swing the controller down and the gesture
// fires "+melee"; the game then plays swing_miss / swing_hit, so the gun swings
// AGAIN on top of the swing the player just performed.
//
// ---- SEQUENCE INDICES ARE PER-MODEL, SO SUPPRESSION MUST BE BY NAME ---------
//
// m_nSequence is an index into the loaded model's own sequence list, and the
// three viewmodels SiN ships do not agree on the numbering:
//
//     reload        v_assault_rifle 7    v_magnum 7     v_scattergun (none --
//                                                       it has clip_reload1 = 8)
//     swing_miss    v_assault_rifle 18   v_magnum 14    v_scattergun 18
//     draw          v_assault_rifle 6    v_magnum 5     v_scattergun 5
//
// So "suppress sequence 7" is a per-weapon statement, and hard-coding one set of
// numbers would silence a reload on two weapons and something else on the third.
// The tables below are the real sequence lists, parsed out of the shipped .mdl
// files (studiohdr_t v44: numlocalseq at 0xBC, localseqindex at 0xC0 -- note
// those sit just BEFORE the two version ints at 0xC4/0xC8 that shift every later
// field, which is the same quirk the bodypart count hit; mstudioseqdesc_t stride
// 212, szlabelindex at +4 relative to the descriptor).
//
// Which model is loaded is resolved at runtime from m_nModelIndex through
// IVModelInfo, NOT inferred from the weapon class -- see model_info.h for why
// that mattered.
//
// ---- WHAT GETS SUPPRESSED IS A CHOICE, NOT AN OBVIOUS ANSWER ----------------
//
// Each category is its own switch because they are not equally clear-cut:
//
//   melee    the doubled swing. Suppress it -- your arm already did this.
//   reload   the gun leaves your hand and comes back. Suppress it, BUT: if the
//            reload sound is an animation EVENT rather than a WeaponSound call
//            in code, cancelling the sequence takes the sound with it. Listen
//            for that before deciding it is an improvement.
//   draw     draw / holster on weapon switch. Off by default -- it is short, and
//            it is the only feedback that a switch happened.
//   lower    the scripted "weapon lowered" state used around dialogue. Off by
//            default: it is the game deliberately taking the weapon away, and
//            suppressing it may look like the gun ignoring a cutscene.
//   fire     NEVER suppressed. Muzzle flash and shell ejection hang off these,
//            and the whole point is to keep what the player did not do himself.
//
// Cancelling works by writing the model's idle sequence over the one the game
// just set, in the same per-frame window the transform write uses.
//
// **This is a per-frame override, not a one-shot cancel.** An earlier version of
// this comment claimed the game sets m_nSequence only on the event, so a single
// overwrite would end the animation. The first session on hardware disproved it:
// one reload produced 285 consecutive frames of the game re-asserting seq 9 then
// seq 10, and one melee swing produced 36. m_nSequence is a networked field and
// the client restores it from the server's value every frame for as long as the
// animation is nominally playing, so we win only because our write happens last,
// in the render window. The cost is one int store per frame, which is nothing --
// but it does mean the SUPPRESSED counter below counts frames, not animations,
// and the two differ by two orders of magnitude.
//
// Reload still progressed start -> end normally across that fight, so the
// weapon's own state machine is not driven by the sequence we are overwriting.
#pragma once

#include <windows.h>
#include <string.h>
#include "../sdk/source_interfaces.h"
#include "../sdk/netprops.h"
#include "../sdk/model_info.h"
#include "../../common/log.h"

namespace sinvr {

//-----------------------------------------------------------------------------
// Sequence tables, parsed from the shipped models. Order IS the index.
//-----------------------------------------------------------------------------
namespace vm_sequences {

inline const char* const kAssaultRifle[] = {
	"idle01", "fire01", "fire02", "fire03", "fire04", "altfire", "draw",
	"reload", "Holster", "reload_start", "reload_end", "dryfire", "idletolow",
	"lowtoidle", "lowidle", "g_launcher_load", "grenade_pull", "grenade_toss",
	"swing_miss", "swing_hit", "swing_miss2", "swing_hit2",
};

inline const char* const kMagnum[] = {
	"idle01", "inoutidle", "altfire", "fire", "fire03", "draw", "draw2",
	"reload", "reload_start", "reload_end", "holster", "idletolow", "lowtoidle",
	"lowidle", "swing_miss", "swing_hit", "swing_miss2", "swing_hit2",
	"grenade_pull", "grenade_toss", "zoomidle", "zoomfire", "zoomfire_alt",
	"zoomfire2",
};

inline const char* const kScattergun[] = {
	"idle01", "fire01", "fire02", "fire03", "altfire1", "draw", "draw2",
	"holster", "clip_reload1", "reload_start", "reload_end", "pump", "dryfire",
	"lowered", "lowered_to_idle", "idle_to_lowered", "grenade_pull",
	"grenade_toss", "swing_miss", "swing_miss2", "swing_hit", "swing_hit2",
	"zoompump", "zoomidle", "zoomfire", "zoomfire2", "fire",
};

struct Table
{
	const char* modelKey;          // matched as a substring of the model path
	const char* const* names;
	int count;
};

inline const Table kTables[] = {
	{ "v_assault_rifle", kAssaultRifle, (int)( sizeof( kAssaultRifle ) / sizeof( kAssaultRifle[0] ) ) },
	{ "v_magnum",        kMagnum,       (int)( sizeof( kMagnum ) / sizeof( kMagnum[0] ) ) },
	{ "v_scattergun",    kScattergun,   (int)( sizeof( kScattergun ) / sizeof( kScattergun[0] ) ) },
};

inline const int kTableCount = (int)( sizeof( kTables ) / sizeof( kTables[0] ) );

} // namespace vm_sequences

enum AnimCategory
{
	kAnimUnknown = 0,
	kAnimIdle,
	kAnimFire,
	kAnimMelee,
	kAnimReload,
	kAnimDraw,
	kAnimLower,
	kAnimGrenade,
};

inline const char* AnimCategoryName( AnimCategory c )
{
	switch ( c )
	{
		case kAnimIdle:    return "idle";
		case kAnimFire:    return "fire";
		case kAnimMelee:   return "melee";
		case kAnimReload:  return "reload";
		case kAnimDraw:    return "draw";
		case kAnimLower:   return "lower";
		case kAnimGrenade: return "grenade";
		default:           return "other";
	}
}

// Case-insensitive substring test, used for both the model key and the name
// rules below. Kept local so this header needs nothing beyond string.h.
inline bool ContainsNoCase( const char* haystack, const char* needle )
{
	if ( !haystack || !needle )
		return false;
	const size_t nlen = strlen( needle );
	if ( nlen == 0 )
		return true;
	for ( const char* p = haystack; *p; ++p )
		if ( _strnicmp( p, needle, nlen ) == 0 )
			return true;
	return false;
}

// Classification is by NAME, so it survives a model whose numbering differs and
// needs no per-weapon rules.
//
// ORDER MATTERS. "lowered_to_idle" and "idle_to_lowered" contain both "low" and
// "idle", and "dryfire" contains "fire" -- so the more specific tests come
// first and the generic ones last.
inline AnimCategory ClassifySequence( const char* name )
{
	if ( !name || !*name )
		return kAnimUnknown;

	if ( ContainsNoCase( name, "swing" ) )
		return kAnimMelee;
	if ( ContainsNoCase( name, "reload" ) || _stricmp( name, "pump" ) == 0 ||
		 _stricmp( name, "zoompump" ) == 0 )
		return kAnimReload;
	if ( ContainsNoCase( name, "low" ) )
		return kAnimLower;
	if ( ContainsNoCase( name, "draw" ) || ContainsNoCase( name, "holster" ) )
		return kAnimDraw;
	if ( ContainsNoCase( name, "grenade" ) || ContainsNoCase( name, "launcher" ) )
		return kAnimGrenade;
	if ( ContainsNoCase( name, "fire" ) )
		return kAnimFire;
	if ( ContainsNoCase( name, "idle" ) )
		return kAnimIdle;
	return kAnimUnknown;
}

struct ViewModelAnimSettings
{
	// Log every sequence change. This is the instrumentation half and is worth
	// leaving on until the categories are confirmed against real play.
	bool log = false;

	// All off by default: cancelling an animation is a visible change to the
	// game, and which ones should go is a judgement call rather than a fix.
	bool suppressMelee = false;
	bool suppressReload = false;
	bool suppressDraw = false;
	bool suppressLower = false;
};

class ViewModelAnimation
{
public:
	void SetSettings( const ViewModelAnimSettings& s ) { m_settings = s; }
	const ViewModelAnimSettings& Settings() const { return m_settings; }

	// Resolves the networked field offsets by name. Nothing runs unless every
	// field this needs was found: a wrong offset here is a write into the middle
	// of a live entity, and the fallback of doing nothing is entirely acceptable.
	bool Bind( const NetProps& props, void* modelInfoIface )
	{
		m_modelInfo.Init( modelInfoIface );

		m_offSequence = props.Find( "CBaseViewModel", "m_nSequence" );
		m_offModelIndex = props.Find( "CBaseViewModel", "m_nModelIndex" );
		m_offCycle = props.Find( "CBaseViewModel", "m_flCycle" );
		m_offPlaybackRate = props.Find( "CBaseViewModel", "m_flPlaybackRate" );
		m_offNewSeqParity = props.Find( "CBaseViewModel", "m_nNewSequenceParity" );
		// The weapon entity behind the viewmodel, and the effect flags that can
		// hide it. SiN gives the WEAPON entity a v_ model of its own and parks it
		// at eye height, so in VR it becomes a second gun floating where the
		// flatscreen one used to be.
		m_offWeaponHandle = props.Find( "CBaseViewModel", "m_hWeapon" );
		m_offEffects = props.Find( "CBaseViewModel", "m_fEffects" );

		// m_flCycle is declared on C_BaseAnimating rather than on the viewmodel
		// table in some branches, so it is optional -- it is only used to restart
		// the idle cleanly and everything works without it.
		m_bound = ( m_offSequence >= 0 && m_offModelIndex >= 0 );

		if ( !m_bound )
		{
			LogWarn( "viewmodel anim: could not resolve m_nSequence (%d) / "
					 "m_nModelIndex (%d) on CBaseViewModel -- animation logging "
					 "and suppression are OFF for this run",
					 m_offSequence, m_offModelIndex );
			return false;
		}

		Log( "viewmodel anim: m_hWeapon at %s, m_fEffects at %s -- needed to deal with the weapon entity's own copy of the gun",
			 m_offWeaponHandle >= 0 ? "found" : "<absent>",
			 m_offEffects >= 0 ? "found" : "<absent>" );
		Log( "viewmodel anim: bound -- m_nSequence at +0x%X, m_nModelIndex at "
			 "+0x%X, m_flCycle at %s, m_flPlaybackRate at %s, parity at %s "
			 "(all found by name)",
			 m_offSequence, m_offModelIndex,
			 m_offCycle >= 0 ? "found" : "<absent>",
			 m_offPlaybackRate >= 0 ? "found" : "<absent>",
			 m_offNewSeqParity >= 0 ? "found" : "<absent>" );
		return true;
	}

	bool Bound() const { return m_bound; }

	// True only on the frame a firing animation began. The second model the
	// player sees appears WHILE FIRING, so a diagnostic that samples every few
	// seconds -- as the first entity scan did -- can miss it entirely. That is
	// almost certainly why the earlier scan reported m_hViewModel[1..3] empty.
	bool JustStartedFiring() const { return m_justStartedFiring; }

	// Model name for any entity's m_nModelIndex, shared so callers do not need
	// their own IVModelInfo binding.
	const char* LookupModelName( int modelIndex )
	{
		return m_modelInfo.NameForIndex( modelIndex );
	}

	// m_nModelIndex lives on C_BaseEntity, so the offset resolved for the
	// viewmodel is valid for every client entity.
	int ModelIndexOffset() const { return m_offModelIndex; }
	int WeaponHandleOffset() const { return m_offWeaponHandle; }
	int EffectsOffset() const { return m_offEffects; }

	// Short key for the model currently held -- "v_magnum", "v_scattergun",
	// "v_assault_rifle" -- or nullptr when it could not be resolved.
	//
	// Exposed because the viewmodel POSITION offsets need it too: the models are
	// authored with different origins, so one set of offsets cannot put all
	// three in the hand. This class already does the resolving, and duplicating
	// it would give two answers to the same question.
	const char* CurrentModelKey() const
	{
		return m_table ? m_table->modelKey : nullptr;
	}

	// Is there actually a weapon in the player's hands?
	//
	// The viewmodel ENTITY exists continuously -- it is not created and
	// destroyed per weapon -- but with nothing drawn on it the engine leaves
	// m_nModelIndex at -1. That is the cheapest honest answer to "is the player
	// armed", and it is already read every frame by Apply().
	//
	// Deliberately NOT CurrentModelKey() != nullptr. That additionally requires
	// IVModelInfo to resolve the index to a NAME and the name to match one of
	// the three shipped viewmodels, so it reads "unarmed" for a weapon we
	// simply failed to identify -- and it was <unresolved> for an entire
	// session on the intro maps, where the player is genuinely unarmed.
	//
	// Unbound means UNKNOWN, and unknown returns true on purpose: a not-ready
	// interface must defer, never latch a refusal. A caller gating a feature on
	// this keeps the feature if we cannot tell, rather than losing it silently.
	bool WeaponInHand() const { return !m_bound || m_lastModelIndex >= 0; }

	// Is the player holding NOTHING?
	//
	// Deliberately a separate question from WeaponInHand() above, and measured
	// from a different field, because the two disagree and the difference is the
	// whole point: on the intro maps the viewmodel carries a model index (so
	// WeaponInHand() says armed) while m_hWeapon resolves to nothing (so there
	// is no gun). The hand marker needs the second answer; the two-handed grip
	// needs the first, and it is already tuned against it -- do not merge them.
	//
	// Read from m_hWeapon, whose offset was resolved BY NAME in Bind() rather
	// than hardcoded, so this does not inherit the +0x7C0 literal used
	// elsewhere for the same field.
	//
	// UNKNOWN READS AS ARMED, which is the opposite of WeaponInHand()'s
	// convention and is correct here: if we cannot tell, draw nothing rather
	// than put a box over a gun.
	bool Unarmed() const
	{
		return m_bound && m_offWeaponHandle >= 0 && !m_weaponHandleValid;
	}

	// The raw handle, for the diagnostic line. An "unarmed" answer that is
	// actually a bad offset looks identical from inside the headset.
	unsigned int WeaponHandleRaw() const { return m_weaponHandle; }

	// Same window as the transform write: after SetUpView has posed the
	// viewmodel, before the scene is drawn.
	void Apply( void* viewModel )
	{
		if ( !m_bound || !viewModel )
			return;

		auto* base = reinterpret_cast<unsigned char*>( viewModel );
		int* seqField = reinterpret_cast<int*>( base + m_offSequence );
		const int modelIndex = *reinterpret_cast<const int*>( base + m_offModelIndex );
		const int seq = *seqField;
		const int parity = ( m_offNewSeqParity >= 0 )
			? *reinterpret_cast<const int*>( base + m_offNewSeqParity ) : 0;

		if ( modelIndex != m_lastModelIndex )
		{
			m_lastModelIndex = modelIndex;
			ResolveTable( modelIndex );
		}

		// An unset EHANDLE is 0xFFFFFFFF, and 0 on some paths; both mean no
		// weapon. Sampled here rather than in a getter so it costs nothing and
		// is taken in the same window as everything else this reads.
		if ( m_offWeaponHandle >= 0 )
		{
			m_weaponHandle = *reinterpret_cast<const unsigned int*>(
				base + m_offWeaponHandle );
			m_weaponHandleValid =
				( m_weaponHandle != 0 && m_weaponHandle != 0xFFFFFFFFu );
		}

		m_justStartedFiring = false;
		const char* name = NameFor( seq );
		const AnimCategory cat = ClassifySequence( name );

		// Compared against the last value the GAME had, never against what we
		// wrote. Storing our own idle write here instead made every frame of a
		// suppressed animation read as a fresh transition: one reload logged 285
		// identical lines and the category counters were off by 100x.
		//
		// A repeat of the same sequence -- firing twice -- keeps m_nSequence
		// constant and flips the parity instead, so both have to be watched or
		// every shot after the first goes unrecorded.
		const bool suppress = ShouldSuppress( cat );
		const bool changed = ( seq != m_lastGameSeq ) || ( parity != m_lastParity );
		m_lastGameSeq = seq;
		m_lastParity = parity;

		if ( changed )
		{
			++m_transitions;
			if ( cat == kAnimFire )
				m_justStartedFiring = true;
			m_countByCategory[cat]++;
			if ( suppress )
				++m_suppressedAnims;
			if ( m_settings.log )
				Log( "viewmodel anim: %-14s seq=%-3d [%s]%s%s",
					 name ? name : "<unnamed>", seq, AnimCategoryName( cat ),
					 m_table ? "" : "  (no sequence table for this model)",
					 suppress ? "  -> SUPPRESSING" : "" );
		}

		if ( !suppress || m_idleSeq < 0 || seq == m_idleSeq )
			return;

		// Cancel it by putting the idle sequence back. Restarting the cycle
		// matters: the suppressed animation may have been most of the way
		// through, and an idle entered at cycle 0.9 pops.
		//
		// m_lastGameSeq is deliberately NOT updated here -- it holds what the
		// game wanted, so the next frame's re-assertion of the same sequence is
		// recognised as the same animation rather than a new one.
		*seqField = m_idleSeq;
		if ( m_offCycle >= 0 )
			*reinterpret_cast<float*>( base + m_offCycle ) = 0.0f;
		++m_suppressedFrames;
	}

	void LogState() const
	{
		if ( !m_settings.log && !AnySuppression() )
			return;

		if ( !m_bound )
		{
			Log( "viewmodel anim: NOT BOUND -- offsets unresolved, nothing observed" );
			return;
		}

		// Two suppression numbers, because they answer different questions:
		// `anims` is how many animations were cancelled, `frames` is how many
		// times we had to re-assert it against the game. A large ratio between
		// them is normal and expected -- see the note at the top of this file.
		Log( "viewmodel anim: model=%s table=%s idleSeq=%d | transitions=%u "
			 "suppressed anims=%u (over %u frames of re-assertion)",
			 m_modelName[0] ? m_modelName : "<unresolved>",
			 m_table ? m_table->modelKey : "<none>", m_idleSeq,
			 m_transitions, m_suppressedAnims, m_suppressedFrames );
	}

private:
	bool AnySuppression() const
	{
		return m_settings.suppressMelee || m_settings.suppressReload ||
			   m_settings.suppressDraw || m_settings.suppressLower;
	}

	bool ShouldSuppress( AnimCategory c ) const
	{
		switch ( c )
		{
			case kAnimMelee:  return m_settings.suppressMelee;
			case kAnimReload: return m_settings.suppressReload;
			case kAnimDraw:   return m_settings.suppressDraw;
			case kAnimLower:  return m_settings.suppressLower;
			default:          return false;
		}
	}

	const char* NameFor( int seq ) const
	{
		if ( !m_table || seq < 0 || seq >= m_table->count )
			return nullptr;
		return m_table->names[seq];
	}

	// The model changed -- a weapon switch. Re-key the table and re-find the
	// idle sequence, because both are per-model.
	void ResolveTable( int modelIndex )
	{
		m_table = nullptr;
		m_idleSeq = -1;
		m_fireSeq = -1;
		m_modelName[0] = 0;

		const char* path = m_modelInfo.NameForIndex( modelIndex );
		if ( !path )
		{
			Log( "viewmodel anim: model index %d -- no name available, so no "
				 "sequence table. Names will read <unnamed> and nothing is "
				 "suppressed.", modelIndex );
			return;
		}

		strncpy_s( m_modelName, sizeof( m_modelName ), path, _TRUNCATE );

		for ( int i = 0; i < vm_sequences::kTableCount; ++i )
		{
			if ( ContainsNoCase( path, vm_sequences::kTables[i].modelKey ) )
			{
				m_table = &vm_sequences::kTables[i];
				break;
			}
		}

		if ( !m_table )
		{
			LogWarn( "viewmodel anim: \"%s\" has no sequence table -- it was not "
					 "one of the three viewmodels the game ships. Nothing will be "
					 "suppressed on it; regenerate the tables if this is a real "
					 "weapon.", path );
			return;
		}

		for ( int i = 0; i < m_table->count; ++i )
		{
			if ( m_idleSeq < 0 && ClassifySequence( m_table->names[i] ) == kAnimIdle )
				m_idleSeq = i;
			// The canonical fire animation: the first one that is plainly a
			// primary fire, skipping altfire, dryfire and the zoom variants,
			// which are different actions rather than variants of one.
			if ( m_fireSeq < 0 && ClassifySequence( m_table->names[i] ) == kAnimFire &&
				 !ContainsNoCase( m_table->names[i], "alt" ) &&
				 !ContainsNoCase( m_table->names[i], "dry" ) &&
				 !ContainsNoCase( m_table->names[i], "zoom" ) )
				m_fireSeq = i;
		}

		Log( "viewmodel anim: model \"%s\" -> table %s (%d sequences), idle is "
			 "seq %d (%s)",
			 path, m_table->modelKey, m_table->count, m_idleSeq,
			 m_idleSeq >= 0 ? m_table->names[m_idleSeq] : "NOT FOUND -- cannot suppress" );
	}

	ViewModelAnimSettings m_settings;
	ModelInfo m_modelInfo;

	bool m_bound = false;
	int m_offSequence = -1;
	int m_offModelIndex = -1;
	int m_offCycle = -1;
	int m_offPlaybackRate = -1;
	int m_offNewSeqParity = -1;
	int m_offWeaponHandle = -1;
	int m_offEffects = -1;

	const vm_sequences::Table* m_table = nullptr;
	int m_lastModelIndex = -1;
	unsigned int m_weaponHandle = 0xFFFFFFFFu;
	bool m_weaponHandleValid = false;
	int m_idleSeq = -1;
	int m_fireSeq = -1;
	char m_modelName[128] = { 0 };

	int m_lastGameSeq = -1;
	int m_lastParity = -1;
	unsigned int m_transitions = 0;
	unsigned int m_suppressedAnims = 0;
	unsigned int m_suppressedFrames = 0;
	bool m_justStartedFiring = false;
	unsigned int m_countByCategory[kAnimGrenade + 1] = { 0 };
};

} // namespace sinvr
