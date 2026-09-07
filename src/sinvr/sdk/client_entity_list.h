// IClientEntityList, and the route from it to the viewmodel.
//
// Everything here was verified against a live SinEpisodes_laa.exe on
// 2026-08-19 (loose branch, client.dll at 0x24000000). Disassembly evidence is
// recorded per item, because the SDK header is a guide to the SHAPE only --
// Ritual shifted every other interface this mod uses.
//
// ---- HOW THE INTERFACE WAS FOUND --------------------------------------------
//
// client.dll exports CreateInterface, and the version string is registered by a
// static constructor. Finding the singleton without calling anything:
//
//   1. "VClientEntityList003" lives at file 0x2973EC. client.dll's PE ImageBase
//      is 0x24000000 and it loads there, so no relocation: VA = 0x242973EC.
//   2. The ONLY reference to that VA is inside .text, not a data table --
//      because it is an argument to the registration call:
//
//        24268390  68 EC 73 29 24    push 0x242973EC     ; the name
//        24268395  68 B0 56 10 24    push 0x241056B0     ; the CreateFn
//        2426839A  B9 48 7E 35 24    mov  ecx, 0x24357E48 ; the InterfaceReg
//        2426839F  E8 BC BD EF FF    call InterfaceReg::InterfaceReg
//
//   3. That CreateFn is the classic EXPOSE_SINGLE_INTERFACE accessor:
//
//        241056B0  B8 DC FD 34 24    mov eax, 0x2434FDDC
//        241056B5  C3                ret
//
//      ==> the singleton is at 0x2434FDDC.
//
// **Do not hardcode that address.** It is recorded as evidence, not as an API.
// Resolve the interface through client.dll's CreateInterface like the other
// three, which survives a rebase and a game update; the number above is only
// how the vtable below was proved.
#pragma once

#include "source_interfaces.h"

namespace sinvr {

// Verified by disassembling five slots on the live vtable at 0x24297390. The
// vtable has THIRTEEN entries where the SDK declares nine, so four are appended
// -- but 0..8 are the SDK's, UNSHIFTED, which is not something to assume on this
// game and is why each one below carries its evidence.
namespace entlist_slot {
enum
{
	// 241056F0  8B 44 24 04       mov eax, [esp+4]            ; entnum
	//           8B 44 C1 14       mov eax, [ecx+eax*8+0x14]   ; array, stride 8
	//           C2 04 00          ret 4
	kGetClientNetworkable = 0,

	// 24105AF0  8B 44 24 04       mov eax, [esp+4]            ; entnum
	//           85 C0 / 7C 17     test eax,eax / jl  -> return 0
	//           05 FF EF FF FF    add eax, -0x1001
	//           C1 E0 04          shl eax, 4                  ; stride 16
	//           8B 0C 08          mov ecx, [eax+ecx]
	//           85 C9 / 74 08     test ecx,ecx / jz -> return 0
	//           8B 01 / FF 50 18  mov eax,[ecx] / call [eax+0x18]  ; downcast
	//           C2 04 00          ret 4
	//
	// The bounds check, the int argument and the virtual downcast together are
	// what identify this as GetClientEntity rather than one of its neighbours.
	kGetClientEntity = 3,

	// Same shape as slot 3 but taking a packed handle rather than an index.
	kGetClientEntityFromHandle = 4,

	// 24105700  80 7C 24 04 01    cmp byte [esp+4], 1   ; bIncludeNonNetworkable
	//           75 09             jne +9
	//           8B 41 0C          mov eax, [ecx+0x0C]
	//           03 41 04          add eax, [ecx+4]
	//           C2 04 00          ret 4
	//           8B 41 04          mov eax, [ecx+4]
	//           C2 04 00          ret 4
	kNumberOfEntities = 5,

	// 24105740  8B 41 10          mov eax, [ecx+0x10]
	//           C3                ret            <- no args, plain field read
	kGetHighestEntityIndex = 6,

	// 24105730  8B 41 08          mov eax, [ecx+8]
	//           C3                ret
	kGetMaxEntities = 8,

	kSlotCount = 13,   // SDK declares 9; four appended, same as IBaseClientDLL's +1
};
} // namespace entlist_slot

// Live sanity values read from the singleton's own fields, via the offsets the
// accessors above dereference. Useful as a bind-time check: if GetMaxEntities
// does not come back as 2048 the mapping is wrong and nothing below is safe.
//
//   [this+0x04] NumberOfEntities(false) = 351
//   [this+0x08] GetMaxEntities()        = 2048   <- standard MAX_EDICTS
//   [this+0x0C] non-networkable count   = 0
//   [this+0x10] GetHighestEntityIndex() = 1081

// ---- STRUCT OFFSETS ---------------------------------------------------------
//
// Found by signature rather than by guessing: the viewmodel's angles track the
// engine's view angles exactly (that is WHY the arms rotate with the controller
// once aim is decoupled), so scanning the object for a QAngle matching the live
// engine angles locates m_angRotation, and the Vector immediately before it is
// m_vecOrigin.
//
// Observed: engine view angles (-9.61, -90.03, 0.49) and, on the viewmodel,
// (-9.61, -90.03, 0.00) at 0x1FC -- pitch and yaw identical, roll dropped.
namespace viewmodel_offset {
enum
{
	kOrigin      = 0x01F0,   // Vector m_vecOrigin
	kAngles      = 0x01FC,   // QAngle m_angRotation
	kAbsOrigin   = 0x0220,   // Vector m_vecAbsOrigin

	// QAngle m_angAbsRotation.
	//
	// Less strongly proven than the three above, and worth saying so. It was
	// found as the only other QAngle-shaped triple in the object that tracks the
	// engine's view angles, in a raw float dump:
	//
	//     0x250   0.000    0.120    0.000   72.158
	//     0x260 -91.719    0.000    0.000    0.000
	//                  -> (72.158, -91.719, 0.000) at 0x25C
	//
	// What confirmed it matters: with the weapon hand tracked at roll -62 and
	// the mod writing kAngles every frame, BOTH kAngles and this read back with
	// roll 0.000 -- the engine's value, since VRCamera forces roll to 0 on the
	// aim angles. So writing the local rotation alone does not reach the render;
	// the absolute one has to be written too, exactly as it is for the origin.
	kAbsAngles   = 0x025C,

	// matrix3x4_t m_rgflCoordinateFrame -- THE FIELD THE RENDER ACTUALLY READS.
	//
	// Found 2026-08-21 while chasing the firing flicker, by scanning the live
	// entity for a matrix whose translation column equalled m_vecAbsOrigin. The
	// evidence is a correlation across five samples, not a single sighting:
	//
	//     +0x0220  m_vecAbsOrigin (we write it)   matched 5/5
	//     +0x01F0  m_vecOrigin, LOCAL             matched 2/5
	//     +0x0298  this matrix                    matched 2/5
	//
	// The local origin and the matrix agree on exactly the same samples, and
	// disagree together on the others, because CalcAbsolutePosition() rebuilds
	// the matrix FROM LOCAL -- and the engine's CalcViewModelView rewrites local
	// every frame. So the abs origin we write stays perfect while the matrix
	// that draws keeps the engine's position, and the gun is rendered where the
	// engine put it. Firing widens that window because attachment queries for
	// the muzzle flash force a bone setup, which is why the flicker tracks the
	// rate of fire.
	//
	// Confirmed to be a real transform rather than a coincidence: its rotation
	// read [-0.766 0.643 0 | -0.643 -0.766 0 | 0 0 1] -- an orthonormal yaw-only
	// basis with m[0][1] == -sin(yaw) and m[1][1] == cos(yaw), which is exactly
	// how Source's AngleMatrix lays one out.
	//
	// **This is why writing m_vecAbsOrigin was never sufficient**, and why an
	// eye-pass trace of that field showed it perfectly stable while the gun was
	// visibly flickering: the field being measured was not the field that draws.
	//
	// Still re-verified at runtime before anything is written to it -- see
	// ViewModelDriver::VerifyCoordinateFrame.
	kCoordinateFrame = 0x0298,

};
} // namespace viewmodel_offset

namespace player_offset {
enum
{
	// CBaseHandle to the viewmodel. Found by scanning C_BaseHLPlayer for a DWORD
	// whose low 12 bits are the viewmodel's entity index:
	//   offset 0x1058  raw=0x003E840C  index=1036  serial=1000
	// (MAX_EDICTS is 2048, so the entry mask is 12 bits.)
	kViewModelHandle = 0x1058,
};
} // namespace player_offset

// ---- WHAT WAS IN THE ENTITY LIST -------------------------------------------
//
// Identified by walking the array and reading each object's RTTI name. Indices
// are per-session and per-map -- recorded to show the shape, never to be
// hardcoded:
//
//     [   0] C_World
//     [   1] C_BaseHLPlayer      <- the local player, as GetLocalPlayer() returns
//     [   3] C_PlayerResource
//     [1036] C_BaseViewModel     <- what has to move
//     [1037] C_WeaponMagnum      <- whatever is held at the time
//
// 351 live entities, mostly C_Sprite (142) and C_RopeKeyframe (64).
//
// The right lookup at runtime is GetLocalPlayer() -> GetClientEntity(index) ->
// read the handle at player_offset::kViewModelHandle -> resolve it through
// kGetClientEntityFromHandle. That hardcodes no index and no array base.

//-----------------------------------------------------------------------------
// Thin wrapper, in the same shape as EngineClient.
//-----------------------------------------------------------------------------
class ClientEntityList
{
public:
	ClientEntityList() = default;
	explicit ClientEntityList( void* iface ) : m_iface( iface ) {}

	bool Valid() const { return m_iface != nullptr; }
	void* Raw() const { return m_iface; }

	void* GetClientEntity( int index ) const
	{
		if ( !m_iface || index < 0 )
			return nullptr;
		return VCall<entlist_slot::kGetClientEntity, void*, int>( m_iface, index );
	}

	void* GetClientEntityFromHandle( unsigned int handle ) const
	{
		if ( !m_iface || handle == 0xFFFFFFFFu )
			return nullptr;
		return VCall<entlist_slot::kGetClientEntityFromHandle, void*, unsigned int>(
			m_iface, handle );
	}

	int GetMaxEntities() const
	{
		if ( !m_iface )
			return 0;
		return VCall<entlist_slot::kGetMaxEntities, int>( m_iface );
	}

	int GetHighestEntityIndex() const
	{
		if ( !m_iface )
			return 0;
		return VCall<entlist_slot::kGetHighestEntityIndex, int>( m_iface );
	}

	// Bind-time assertion. MAX_EDICTS is 2048 on this engine and the field the
	// accessor reads was checked live, so a wrong value here means the slot map
	// is wrong -- and every offset downstream would then be read off the wrong
	// object. Cheaper to refuse than to write garbage into an entity.
	bool LooksSane() const
	{
		const int maxEnts = GetMaxEntities();
		return maxEnts == 2048;
	}

private:
	void* m_iface = nullptr;
};

} // namespace sinvr
