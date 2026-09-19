// IEngineTrace -- a real ray against the world, so the laser dot lands on things.
//
// ---- WHY THE STRUCT RISK IS SMALLER THAN IT LOOKS -----------------------------
//
// Reversing two Valve structs to call one function is exactly the kind of thing
// this project makes a fuss about, so here is why it is bounded.
//
// `Ray_t` in SOURCE 2004 is four plain `Vector`s and two bools. Later branches
// changed those to `VectorAligned`, which would have demanded 16-byte stack
// alignment and made a wrong guess crash rather than misbehave -- but this is the
// 2004 tree and `cmodel.h` shows plain Vectors. It is an input, so if the layout
// were wrong the engine would trace a nonsense ray, not corrupt anything of ours.
//
// `trace_t` is only read, and only its FIRST 48 BYTES:
//
//     +0   startpos   Vector
//     +12  endpos     Vector      <- the only field the dot needs
//     +24  plane      cplane_t    (normal, dist, type, signbits, pad) = 20 bytes
//     +44  fraction   float       <- and this, to know whether it hit anything
//
// Both live in `CBaseTrace`, the base of `CGameTrace`, so nothing after them can
// move them. The buffer handed to the engine is deliberately far larger than the
// full `CGameTrace` and zeroed, so the engine writing more than we understand is
// harmless -- it writes into slack we own.
//
// ---- THE FILTER ---------------------------------------------------------------
//
// `ITraceFilter` is two pure virtuals, so a vtable of two function pointers is
// the whole implementation. Ours NEVER DEREFERENCES the entity pointer it is
// handed -- it only compares it -- so even if the pointer identity assumption
// below is wrong, the cost is a trace that hits the player rather than a fault.
//
// That assumption: `IClientEntity` derives from `IClientUnknown`, which derives
// from `IHandleEntity` as its first base, so the `IHandleEntity*` the engine
// passes should be the same address `GetClientEntity` returned. "Should" is doing
// work there, which is why the caller also range-checks the resulting fraction:
// a trace that stops at essentially zero distance means we hit ourselves, and
// that is reported rather than drawn.
#pragma once

#include <windows.h>
#include <math.h>
#include "source_interfaces.h"
#include "../../common/log.h"

namespace sinvr {

// Contents masks. MASK_SOLID is what a bullet stops on.
enum { kMaskSolid = 0x1 | 0x2 | 0x4000 | 0x2000000 | 0x8 | 0x10000 };

struct RayT
{
	Vector start;
	Vector delta;
	Vector startOffset;
	Vector extents;
	bool isRay;
	bool isSwept;
};

// Our ITraceFilter. Layout must be: vtable pointer first, then whatever we like.
class TraceFilterSkipOne
{
public:
	explicit TraceFilterSkipOne( void* skip )
		: m_vtable( s_vtable ), m_skip( skip )
	{
	}

private:
	// __thiscall, so `this` arrives in ECX and is the first parameter here.
	static bool __fastcall ShouldHitEntity( TraceFilterSkipOne* self, void* /*edx*/,
											void* entity, int /*contentsMask*/ )
	{
		// Compared, never dereferenced.
		return entity != self->m_skip;
	}

	static int __fastcall GetTraceType( TraceFilterSkipOne* /*self*/, void* /*edx*/ )
	{
		return 0;   // TRACE_EVERYTHING
	}

	static void* s_vtable[2];

	void* m_vtable;
	void* m_skip;
};

__declspec( selectany ) void* TraceFilterSkipOne::s_vtable[2] = {
	(void*)&TraceFilterSkipOne::ShouldHitEntity,
	(void*)&TraceFilterSkipOne::GetTraceType,
};

class EngineTrace
{
public:
	bool Bind( void* raw )
	{
		m_iface = nullptr;
		if ( !raw )
		{
			Log( "trace: EngineTraceClient003 not available -- the laser dot cannot "
				 "land on geometry and will stay at its fixed distance" );
			return false;
		}

		void** vtable = *reinterpret_cast<void***>( raw );
		if ( IsBadReadPtr( vtable, sizeof( void* ) * ( kTraceRay + 1 ) ) ||
			 !vtable[kTraceRay] ||
			 IsBadCodePtr( reinterpret_cast<FARPROC>( vtable[kTraceRay] ) ) )
		{
			LogWarn( "trace: EngineTraceClient003 slot %d does not look like code -- "
					 "tracing disabled", kTraceRay );
			return false;
		}

		m_iface = raw;
		Log( "trace: EngineTraceClient003 bound at %p (TraceRay = slot %d)",
			 raw, kTraceRay );
		return true;
	}

	bool Valid() const { return m_iface != nullptr; }

	// Returns true and fills `hit` when something was struck. `skipEntity` is the
	// entity the ray starts inside -- the local player -- and may be null.
	//
	// `fraction` comes back so the caller can tell "hit a wall right in front of
	// me" from "hit myself", which look identical in the endpoint alone.
	bool Ray( const Vector& start, const Vector& end, void* skipEntity,
			  Vector& hit, float& fraction )
	{
		return Sweep( start, end, Vector{ 0.0f, 0.0f, 0.0f }, skipEntity, hit, fraction );
	}

	// ---- THE SAME TRACE, WITH A BOX AROUND IT ------------------------------
	//
	// A bullet is a point and a HEAD is not. A zero-extent ray slips through the
	// corner of a doorframe it technically missed, and the camera's near plane
	// then clips into geometry the trace said was clear -- so anything standing
	// in for a body has to sweep a box.
	//
	// `halfExtents` is half the box in each axis, so { 8, 8, 8 } is a 16-unit
	// cube. `isRay` MUST be false for a hull: it is what tells the engine to use
	// the extents at all, and leaving it true silently gives back a ray trace
	// that looks like it worked.
	bool Hull( const Vector& start, const Vector& end, const Vector& halfExtents,
			   void* skipEntity, Vector& hit, float& fraction )
	{
		bool startedSolid = false;
		return Sweep( start, end, halfExtents, skipEntity, hit, fraction, &startedSolid );
	}

	// ---- AND WHETHER THE SWEEP BEGAN INSIDE SOMETHING ----------------------
	//
	// `startsolid` and a wall at zero range both come back as fraction 0, and
	// they mean opposite things. A wall at zero range is "you may not move".
	// Startsolid is "this trace cannot tell you anything" -- the box began
	// intersecting geometry, so the engine reports the only fraction it can and
	// it carries no information about the direction of travel.
	//
	// Acting on it as though it were a wall collapses the 6DoF offset to zero,
	// which reads in the headset as tracking cutting out for a moment. The
	// caller wants to know the difference.
	bool HullEx( const Vector& start, const Vector& end, const Vector& halfExtents,
				 void* skipEntity, Vector& hit, float& fraction, bool& startedSolid )
	{
		return Sweep( start, end, halfExtents, skipEntity, hit, fraction, &startedSolid );
	}

	// ---- AND THE SURFACE IT HIT ------------------------------------------
	//
	// `normal` is only written when the sweep hit something AND the value reads
	// as a unit vector. A caller that wants to steer by it must be able to tell
	// "the surface faces this way" from "we did not find out", because the two
	// produce very different gun poses -- see the deflection in viewmodel.h.
	//
	// cplane_t sits at +24, inside CBaseTrace, so the same argument that makes
	// endpos and fraction safe covers it: nothing after it is being read.
	bool HullNormal( const Vector& start, const Vector& end, const Vector& halfExtents,
					 void* skipEntity, Vector& hit, float& fraction, bool& startedSolid,
					 Vector& normal, bool& haveNormal )
	{
		return Sweep( start, end, halfExtents, skipEntity, hit, fraction, &startedSolid,
					  &normal, &haveNormal );
	}

private:
	bool Sweep( const Vector& start, const Vector& end, const Vector& halfExtents,
				void* skipEntity, Vector& hit, float& fraction,
				bool* startedSolid = nullptr, Vector* normal = nullptr,
				bool* haveNormal = nullptr )
	{
		if ( startedSolid )
			*startedSolid = false;
		if ( haveNormal )
			*haveNormal = false;

		if ( !m_iface )
			return false;

		const bool isRay = ( halfExtents.x == 0.0f && halfExtents.y == 0.0f &&
							 halfExtents.z == 0.0f );

		RayT ray = {};
		ray.start = start;
		ray.delta.x = end.x - start.x;
		ray.delta.y = end.y - start.y;
		ray.delta.z = end.z - start.z;
		ray.startOffset = Vector{ 0.0f, 0.0f, 0.0f };
		ray.extents = halfExtents;
		ray.isRay = isRay;
		ray.isSwept = ( ray.delta.x != 0.0f || ray.delta.y != 0.0f ||
						ray.delta.z != 0.0f );

		TraceFilterSkipOne filter( skipEntity );

		// Far bigger than CGameTrace and zeroed: the engine may write fields we
		// have not mapped, and they land in slack we own rather than past the end.
		unsigned char traceBuf[256] = {};

		using Fn = void( __thiscall* )( void*, const RayT&, unsigned int,
										void*, void* );
		void** vtable = *reinterpret_cast<void***>( m_iface );
		auto fn = reinterpret_cast<Fn>( vtable[kTraceRay] );

		__try
		{
			fn( m_iface, ray, kMaskSolid, &filter, traceBuf );
		}
		__except ( EXCEPTION_EXECUTE_HANDLER )
		{
			if ( !m_faulted )
			{
				m_faulted = true;
				LogError( "trace: TraceRay faulted -- the Ray_t or trace_t layout is "
						  "wrong for this build. Tracing disabled; the dot falls "
						  "back to its fixed distance." );
			}
			m_iface = nullptr;
			return false;
		}

		fraction = *reinterpret_cast<const float*>( traceBuf + kFractionOffset );
		hit = *reinterpret_cast<const Vector*>( traceBuf + kEndPosOffset );
		if ( startedSolid )
			*startedSolid = ( traceBuf[kStartSolidOffset] != 0 ) ||
							( traceBuf[kAllSolidOffset] != 0 );

		// A fraction outside 0..1 means the layout is not what we think it is.
		// Refusing here keeps a garbage endpoint from being drawn as a dot
		// somewhere across the map.
		if ( !( fraction >= 0.0f && fraction <= 1.0f ) )
		{
			if ( !m_warnedLayout )
			{
				m_warnedLayout = true;
				LogWarn( "trace: fraction read back as %f, which is not 0..1 -- the "
						 "trace_t layout does not match. Tracing disabled.", fraction );
			}
			m_iface = nullptr;
			return false;
		}

		// Validated as a unit vector before it is handed out. A wrong offset
		// here would not fault -- it would return plausible-looking garbage and
		// silently steer the gun somewhere absurd, which is exactly the failure
		// this file's fraction check exists to prevent for endpos.
		if ( normal && haveNormal && fraction < 1.0f )
		{
			const Vector n = *reinterpret_cast<const Vector*>( traceBuf + kPlaneNormalOffset );
			const float len2 = n.x * n.x + n.y * n.y + n.z * n.z;
			if ( len2 > 0.9f && len2 < 1.1f )
			{
				*normal = n;
				*haveNormal = true;
			}
		}

		++m_traces;
		return fraction < 1.0f;
	}

public:
	unsigned int Traces() const { return m_traces; }

private:
	enum { kTraceRay = 4 };          // IEngineTrace slot 4
	enum { kEndPosOffset = 12 };     // CBaseTrace::endpos
	enum { kPlaneNormalOffset = 24 };// CBaseTrace::plane.normal
	enum { kFractionOffset = 44 };   // CBaseTrace::fraction
	// CBaseTrace, continuing past fraction:
	//   +48 contents   int
	//   +52 dispFlags  unsigned short
	//   +54 allsolid   bool
	//   +55 startsolid bool
	// Both still inside CBaseTrace, so the same argument that makes endpos and
	// fraction safe covers these: nothing after them is being read.
	enum { kAllSolidOffset = 54 };
	enum { kStartSolidOffset = 55 };

	void* m_iface = nullptr;
	unsigned int m_traces = 0;
	bool m_faulted = false;
	bool m_warnedLayout = false;
};

} // namespace sinvr
