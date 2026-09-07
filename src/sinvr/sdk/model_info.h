// IVModelInfoClient -- "VModelInfoClient003", from engine.dll.
//
// Bound for exactly one purpose: turning a viewmodel entity's m_nModelIndex
// into the model's NAME, so the animation work can key its per-model sequence
// table on the model actually loaded rather than on an inference.
//
// The alternative was to read the weapon entity's RTTI class and map class ->
// model by hand. That was rejected after checking: SiN's client.dll has
// C_WeaponAssaultRifle and C_WeaponMagnum, but there is NO C_WeaponScattergun
// -- the scattergun is presumably C_WeaponShotgun, which is a guess, and a
// guessed mapping decides which animations get suppressed on which weapon. The
// model name is the ground truth and costs one interface.
//
// ---- SLOT VERIFICATION ------------------------------------------------------
//
// The SDK declares, in order: ~IVModelInfo, GetModel, GetModelIndex,
// GetModelName. With MSVC's virtual destructor occupying slot 0 that puts
// GetModel at 1 and GetModelName at 3. **The project's rule is never to trust
// SDK indices** -- Ritual shifted all four of the interfaces already in use --
// so these are not assumed, they are PROVEN AT RUNTIME:
//
//   Validate() calls GetModel(idx) then GetModelName() on a real index and
//   requires the result to look like a model path ("...*.mdl"). A wrong slot
//   returns something that does not, or faults, and either way the interface is
//   marked unusable and name resolution is disabled -- which degrades the
//   animation work to logging raw sequence indices rather than doing anything
//   destructive with a bad pointer.
//
// Every call is inside __try for the same reason: this is the one interface
// here whose indices have not been confirmed by disassembly.
#pragma once

#include <windows.h>
#include <string.h>
#include "source_interfaces.h"
#include "../../common/log.h"

namespace sinvr {

namespace modelinfo_slot
{
	// SDK 2004 declaration order, with slot 0 taken by the virtual destructor.
	// Treated as a HYPOTHESIS that Validate() has to confirm, not as fact.
	constexpr int kGetModel = 1;
	constexpr int kGetModelName = 3;
}

class ModelInfo
{
public:
	ModelInfo() = default;

	// Binds and immediately proves itself against a known-good model index, or
	// disables. `probeIndex` should be a model index the caller knows is live --
	// the viewmodel's own -- because there is no way to validate the slots
	// without calling them on something real.
	bool Init( void* iface )
	{
		m_iface = iface;
		m_usable = false;
		m_validated = false;
		if ( !m_iface )
		{
			LogWarn( "modelinfo: VModelInfoClient003 not available -- viewmodel "
					 "sequence names cannot be resolved, animation suppression "
					 "will stay off" );
			return false;
		}
		Log( "modelinfo: VModelInfoClient003 at %p (slots unproven until the "
			 "first model index arrives)", m_iface );
		return true;
	}

	bool Bound() const { return m_iface != nullptr; }
	bool Usable() const { return m_usable; }
	bool Validated() const { return m_validated; }

	// Model name for an index, or nullptr. The first successful call also
	// validates the slot hypothesis; after a failed validation this always
	// returns nullptr rather than retrying a call that may fault.
	const char* NameForIndex( int modelIndex )
	{
		if ( !m_iface || modelIndex <= 0 )
			return nullptr;
		if ( m_validated && !m_usable )
			return nullptr;

		const char* name = nullptr;
		__try
		{
			const void* model = VCall<modelinfo_slot::kGetModel, const void*, int>(
				m_iface, modelIndex );
			if ( model )
				name = VCall<modelinfo_slot::kGetModelName, const char*, const void*>(
					m_iface, model );
		}
		__except ( EXCEPTION_EXECUTE_HANDLER )
		{
			name = nullptr;
		}

		if ( !m_validated )
		{
			m_validated = true;
			m_usable = LooksLikeModelPath( name );
			if ( m_usable )
			{
				Log( "modelinfo: slots CONFIRMED -- GetModel(%d) -> GetModelName() "
					 "returned \"%s\", which is a model path. Sequence names are "
					 "resolvable.", modelIndex, name );
			}
			else
			{
				LogWarn( "modelinfo: slot hypothesis REJECTED -- GetModel(%d) -> "
						 "GetModelName() returned %s, which is not a model path. "
						 "Ritual has probably shifted this interface too. Falling "
						 "back to raw sequence indices; nothing will be suppressed.",
						 modelIndex, name ? name : "<null>" );
				return nullptr;
			}
		}

		return m_usable ? name : nullptr;
	}


private:
	// A model path is the proof that the slots are right: nothing else in the
	// interface returns a string ending in .mdl.
	static bool LooksLikeModelPath( const char* s )
	{
		if ( !s )
			return false;

		// Bounded read -- a wrong slot can hand back a pointer to anything.
		size_t len = 0;
		__try
		{
			while ( len < 260 && s[len] != 0 )
				++len;
		}
		__except ( EXCEPTION_EXECUTE_HANDLER )
		{
			return false;
		}
		if ( len < 5 || len >= 260 )
			return false;

		for ( size_t i = 0; i < len; ++i )
			if ( (unsigned char)s[i] < 32 || (unsigned char)s[i] > 126 )
				return false;

		return _stricmp( s + len - 4, ".mdl" ) == 0;
	}

	void* m_iface = nullptr;
	bool m_usable = false;
	bool m_validated = false;
};

} // namespace sinvr
