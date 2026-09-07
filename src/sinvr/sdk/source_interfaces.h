// ABI mirrors for the Source 2004 interfaces exposed by SiN Episodes: Emergence.
//
// SiN's binaries are a Ritual fork of the HL2 codebase and report the exact same
// interface version strings as Source SDK 2004:
//
//   engine.dll  -> "VEngineClient012"  (IVEngineClient,  94 vtable slots)
//   client.dll  -> "VClient011"        (IBaseClientDLL,  47 vtable slots)
//
// We deliberately do NOT include the real SDK headers: they are written for
// VC6/VC7.1 and drag in tier0/tier1 link dependencies. All we need is the vtable
// layout, so we call slots by index instead. Slot numbers below were counted from
// source-sdk-2004-master/src_mod/public/cdll_int.h -- the line references are
// there so any slot can be re-verified against the header.
#pragma once

#include <windows.h>

namespace sinvr {

//-----------------------------------------------------------------------------
// Minimal value types. Layout must match Source exactly.
//-----------------------------------------------------------------------------
struct Vector
{
	float x, y, z;
};

// Source view angles: pitch, yaw, roll -- in degrees.
struct QAngle
{
	float x, y, z;
};

// Passed through View_Render untouched; we never dereference it.
struct vrect_t
{
	int x, y, width, height;
	vrect_t* pnext;
};

//-----------------------------------------------------------------------------
// Call virtual slot 'Index' on a Source interface pointer.
//-----------------------------------------------------------------------------
template <int Index, typename Ret, typename... Args>
inline Ret VCall( void* inst, Args... args )
{
	using Fn = Ret( __thiscall* )( void*, Args... );
	void** vtable = *reinterpret_cast<void***>( inst );
	return reinterpret_cast<Fn>( vtable[Index] )( inst, args... );
}

inline void** VTableOf( void* inst )
{
	return *reinterpret_cast<void***>( inst );
}

//-----------------------------------------------------------------------------
// IVEngineClient -- "VEngineClient012", from engine.dll
//
// !! SiN's engine.dll does NOT match SDK 2004 here. Verified against a live
// !! process (engine.dll @ 0x20000000): the vtable has 100 slots, not 94.
//
//   runtime 0..28   == SDK 0..28          (unchanged)
//   runtime 29      == inserted by Ritual -- `mov al,[eng+0x3E4319]; ret`,
//                      a bool getter with no SDK 2004 equivalent
//   runtime 30..94  == SDK 29..93         (shift +1)
//   runtime 95..99  == 5 methods appended past the end of the SDK interface
//
// So: SDK index i  ->  runtime index (i < 29 ? i : i + 1).
//
// Anchors used to prove the mapping, all read out of the live process:
//   19/20 matched pair -- GetViewAngles reads eng+0x3F51F4/F8/FC,
//                         SetViewAngles normalizes and writes the same three
//   26    cmp [eng+0x3F0CE8],6 / sete al   -- IsInGame (SIGNONSTATE_FULL)
//   33    bounds-check vs argc, index array -- Cmd_Argv (SDK 32)
//   38    returns "...\sin episodes emergence\SE1" -- GetGameDirectory (SDK 37)
//   39/40 jmp [eax+0x3C] / jmp [eax+0x38] -- WorldToScreen/ViewMatrix (SDK 38/39)
//   94    returns "1.0.0.0" -- GetProductVersionString (SDK 93)
//
// The SDK line references below still point at the method's declaration; the
// number in the enum is the *runtime* slot.
//-----------------------------------------------------------------------------
namespace engine_slot {
enum
{
	// --- below the insertion point: SDK indices are correct as-is ---
	kGetScreenSize          = 5,   // cdll_int.h:163
	kClientCmd              = 7,   // cdll_int.h:168
	kGetLocalPlayer         = 12,  // cdll_int.h:183
	kTime                   = 14,  // cdll_int.h:189
	kGetViewAngles          = 19,  // cdll_int.h:203  VERIFIED
	kSetViewAngles          = 20,  // cdll_int.h:205  VERIFIED
	kGetMaxClients          = 21,  // cdll_int.h:208
	kIsInGame               = 26,  // cdll_int.h:223  VERIFIED
	kIsConnected            = 27,  // cdll_int.h:225

	// --- at/after the insertion point: SDK index + 1 ---
	kCon_NPrintf            = 30,  // cdll_int.h:231  (SDK 29)

	// Frustum culling. Verified by disassembly: both take two Vector refs and
	// `ret 8`. The engine builds its frustum from the game's own symmetric FOV,
	// which is narrower than the headset's asymmetric per-eye frustum, so these
	// reject geometry that is genuinely visible in VR.
	kIsBoxVisible           = 34,  // cdll_int.h:241  (SDK 33)  frustum
	kIsBoxInViewCluster     = 35,  // cdll_int.h:244  (SDK 34)  PVS cluster
	kCullBox                = 36,  // cdll_int.h:247  (SDK 35)  frustum

	// Area portals and occluders. These are what make geometry beyond a doorway
	// simply not exist -- leaving a hole that shows whatever lies behind it,
	// rather than merely clipping at the frustum edge. Both are computed from
	// the engine's own view, which does not know about the VR frustum.
	kDoesBoxTouchAreaFrustum = 59, // cdll_int.h:318  (SDK 58)
	kIsOccluded              = 70, // cdll_int.h:340  (SDK 69)

	kGetGameDirectory       = 38,  // cdll_int.h:253  (SDK 37)  VERIFIED
	kGetDXSupportLevel      = 50,  // cdll_int.h:292  (SDK 49)
	kSupportsHDR            = 51,  // cdll_int.h:295  (SDK 50)
	kGetLevelName           = 53,  // cdll_int.h:301  (SDK 52)
	kIsPaused               = 80,  // cdll_int.h:362  (SDK 79)
	kGetScreenAspectRatio   = 90,  // cdll_int.h:388  (SDK 89)
	kGetEngineBuildNumber   = 93,  // cdll_int.h:395  (SDK 92)  returns 7
	kGetProductVersionString = 94, // cdll_int.h:396  (SDK 93)  VERIFIED "1.0.0.0"

	kSlotCount              = 100,
};
} // namespace engine_slot

//-----------------------------------------------------------------------------
// IMaterialSystem -- "VMaterialSystem076", from materialsystem.dll
//
// SDK 2004 imaterialsystem.h:341-782 declares 143 virtuals. SiN's has **158**,
// verified by reading a live process (materialsystem.dll @ 0x03D00000), so the
// SDK indices are NOT usable directly. Ritual modified this interface more
// heavily than the other two.
//
// The matrix block is identified and internally consistent, sitting at a
// uniform **+3** from the SDK's numbering. Evidence, all from disassembly:
//
//   66/67/68  three consecutive thin forwarders, tail-calling the shader API at
//             [eax+0x3C], [+0x40], [+0x44]        -> MatrixMode/Push/Pop
//   69/71/73  `mov ecx,12; rep movsd`, i.e. copy 48 bytes  -> matrix3x4_t
//   70/72/74  `sub esp,0x40`, i.e. a 64-byte local         -> VMatrix
//   75/76     both allocate 0x40                           -> GetMatrix pair
//   77        thin forwarder                               -> LoadIdentity
//   78        `fld [esp+0x2C]`, reads a 6th double         -> Ortho
//   79        `fld [esp+0x1C]`, reads a 4th double         -> PerspectiveX
//
// !! The overload pairs are REVERSED relative to the header. MSVC emits a group
// !! of same-named overloads in reverse declaration order, so the matrix3x4_t
// !! variant comes FIRST in the vtable even though the header declares VMatrix
// !! first. Reading the header alone would have got these backwards.
//
// The render-target block sits at +4, NOT the matrix block's +3. Both were read
// out of a live process; the shift genuinely is not uniform, so slot numbers
// here must never be interpolated between known anchors.
//
//   30  `cmp esi,[edi+0x11C]` early-out, validates the texture, tells the
//       shader API, stores at [this+0x11C], `ret 4`   -> SetRenderTarget
//   31  `mov eax,[ecx+0x11C]; ret`                    -> GetRenderTarget
//   32  writes two out-params via `ret 8`, falling back to the shader API's
//       backbuffer dimensions when [this+0x11C] is null
//                                                     -> GetRenderTargetDimensions
//
// Slot 29 is a *different* single-pointer setter storing [this+0x118]; it is not
// part of this triple and is one of Ritual's additions.
//-----------------------------------------------------------------------------
namespace matsys_slot {
enum
{
	// --- verified: render target block (+4 from SDK) ---
	kSetRenderTarget           = 30,  // SDK 26
	kGetRenderTarget           = 31,  // SDK 27
	kGetRenderTargetDimensions = 32,  // SDK 28

	// --- verified: matrix block (+3 from SDK) ---
	kMatrixMode               = 66,  // SDK 63
	kPushMatrix               = 67,  // SDK 64
	kPopMatrix                = 68,  // SDK 65
	kLoadMatrixMatrix3x4      = 69,  // SDK 67  <- note the pair is reversed
	kLoadMatrixVMatrix        = 70,  // SDK 66  <- stereo view-matrix hook
	kMultMatrixMatrix3x4      = 71,  // SDK 69
	kMultMatrixVMatrix        = 72,  // SDK 68
	kMultMatrixLocalMatrix3x4 = 73,  // SDK 71
	kMultMatrixLocalVMatrix   = 74,  // SDK 70
	kGetMatrixMatrix3x4       = 75,  // SDK 73
	kGetMatrixVMatrix         = 76,  // SDK 72
	kLoadIdentity             = 77,  // SDK 74
	kOrtho                    = 78,  // SDK 75
	kPerspectiveX             = 79,  // SDK 76  <- stereo projection hook

	// --- verified 2026-09-02 by live vtable dump (matsys_vtable_dump = 1) ---
	//
	// The per-DRAW hook. Ortho tells us which coordinate space is being drawn
	// in; this tells us WHAT is being drawn, by material name -- which is the
	// only thing that separates a full-screen fade from the HUD, since the two
	// are byte-identical in every argument Ortho receives.
	//
	//   49  push esi / mov esi,[esp+8] / test esi,esi / push edi / mov edi,ecx
	//       -- `this` in ecx, first stack argument NULL-CHECKED, which is
	//       Bind( IMaterial*, void* ) guarding a null material.
	//
	// Derived against a fresh parse of the 2004 SDK header rather than by
	// interpolating this table, because the numbers in the comments above came
	// from a DIFFERENT header revision and disagree with the 2004 one by +1 to
	// +2 in places. Against the 2004 header the live shift is a uniform +5, and
	// that was confirmed at ELEVEN independent anchors spanning slots 30 to 77
	// before this one was read off it:
	//
	//   25->30 SetRenderTarget      `cmp esi,[edi+0x11C]`  (matches the note above)
	//   26->31 GetRenderTarget      `mov eax,[ecx+0x11C]; ret`
	//   27->32 GetRenderTargetDimensions
	//   61..63->66..68 MatrixMode/PushMatrix/PopMatrix -- three consecutive
	//                  shader-API forwarders, `jmp [eax+0x3C/0x40/0x44]`
	//   64..71->69..76 the LoadMatrix / MultMatrix / MultMatrixLocal / GetMatrix
	//                  overload PAIRS, alternating two shapes, each with the
	//                  `sub esp,0x40` sixteen-float buffer
	//   72->77 LoadIdentity        `jmp [eax+0x58]`
	//
	// A uniform shift over that span plus a matching disassembly at the target
	// is the same standard every other index here was held to. It is NOT an
	// interpolation of the neighbouring entries in this enum -- do not treat it
	// as licence to interpolate the others.
	kBind                     = 49,  // SDK 2004 44

	// --- verified 2026-09-02 by live vtable dump, same method as kBind ---
	//
	// OverrideDepthEnable( bool bEnable, bool bDepthEnable ). Forces the depth
	// TEST off for everything drawn after it, which is what lets the flattened
	// menu recover VGUI's own painter-order behaviour -- see the z-fighting
	// write-up in HANDOVER.md for why that is the fix and depth separation is
	// not.
	//
	//   126  `mov ecx,[global]; mov eax,[ecx]; jmp [eax+0x1A0]` -- a pure
	//        forwarder to the shader API, which is exactly what this method is.
	//
	// A tail-jump carries no `ret N`, so the argument count cannot be read off
	// it. The evidence is positional, and the neighbours pin it from BOTH sides
	// at the same +5 that holds elsewhere:
	//
	//   117->122 SetInStubMode        `mov al,[esp+4]; mov [ecx+0x130],al; ret 4`
	//                                 -- an unmistakable single-bool setter
	//   119->124 CreateRenderTargetTexture -- reads a 5th argument and switches
	//                                 on an enum
	//   122->127 DrawScreenSpaceQuad  `sub esp,0xE0`, a mesh-building frame
	//   115,116->120,121 ClearColor3ub/4ub -- shader-API forwarders
	//
	// Four independent identifications bracketing it, and the shape at 126 is
	// the one this method should have.
	kOverrideDepthEnable      = 126, // SDK 2004 121

	kSlotCount                = 158, // SDK 2004 declares 143
};
} // namespace matsys_slot

// Still unidentified, and needed only if eye buffers are allocated through the
// material system: CreateRenderTargetTexture (SDK 121). The stereo path avoids
// it by copying the backbuffer into D3D9 render targets we create ourselves,
// which needs no further slot archaeology.

// Engine globals confirmed by disassembly, as RVAs from engine.dll's base.
// Kept for later: writing these directly is a cheaper path than SetViewAngles
// once we are driving the camera every frame.
namespace engine_rva {
enum : unsigned int
{
	kViewAnglesPitch = 0x3F51F4,
	kViewAnglesYaw   = 0x3F51F8,
	kViewAnglesRoll  = 0x3F51FC,
	kSignonState     = 0x3F0CE8,  // == 6 when fully in game
};
} // namespace engine_rva

class EngineClient
{
public:
	explicit EngineClient( void* iface ) : m_iface( iface ) {}

	bool Valid() const { return m_iface != nullptr; }
	void* Raw() const { return m_iface; }

	void GetScreenSize( int& w, int& h ) const
	{
		VCall<engine_slot::kGetScreenSize, void, int&, int&>( m_iface, w, h );
	}
	void ClientCmd( const char* cmd ) const
	{
		VCall<engine_slot::kClientCmd, void, const char*>( m_iface, cmd );
	}
	int GetLocalPlayer() const
	{
		return VCall<engine_slot::kGetLocalPlayer, int>( m_iface );
	}
	float Time() const
	{
		return VCall<engine_slot::kTime, float>( m_iface );
	}
	// NOTE: Source takes QAngle by non-const reference on both of these.
	void GetViewAngles( QAngle& va ) const
	{
		VCall<engine_slot::kGetViewAngles, void, QAngle&>( m_iface, va );
	}
	void SetViewAngles( QAngle& va ) const
	{
		VCall<engine_slot::kSetViewAngles, void, QAngle&>( m_iface, va );
	}
	int GetMaxClients() const
	{
		return VCall<engine_slot::kGetMaxClients, int>( m_iface );
	}
	bool IsInGame() const
	{
		return VCall<engine_slot::kIsInGame, bool>( m_iface );
	}
	bool IsConnected() const
	{
		return VCall<engine_slot::kIsConnected, bool>( m_iface );
	}
	const char* GetGameDirectory() const
	{
		return VCall<engine_slot::kGetGameDirectory, const char*>( m_iface );
	}
	int GetDXSupportLevel() const
	{
		return VCall<engine_slot::kGetDXSupportLevel, int>( m_iface );
	}
	bool SupportsHDR() const
	{
		return VCall<engine_slot::kSupportsHDR, bool>( m_iface );
	}
	const char* GetLevelName() const
	{
		return VCall<engine_slot::kGetLevelName, const char*>( m_iface );
	}
	bool IsPaused() const
	{
		return VCall<engine_slot::kIsPaused, bool>( m_iface );
	}
	float GetScreenAspectRatio() const
	{
		return VCall<engine_slot::kGetScreenAspectRatio, float>( m_iface );
	}
	unsigned int GetEngineBuildNumber() const
	{
		return VCall<engine_slot::kGetEngineBuildNumber, unsigned int>( m_iface );
	}
	const char* GetProductVersionString() const
	{
		return VCall<engine_slot::kGetProductVersionString, const char*>( m_iface );
	}

private:
	void* m_iface;
};

//-----------------------------------------------------------------------------
// IBaseClientDLL -- "VClient011", from SE1/bin/client.dll
// cdll_int.h:402-531.
//
// Verified against a live process (client.dll @ 0x24000000): 48 slots. SDK 2004
// declares 47 and slots 0..46 match it exactly -- the one extra is appended at
// index 47, so every index below is unshifted and correct.
//
// Anchors:
//   5   returns a global pointer          -- GetAllClasses
//   29  `mov eax,0x30; ret`               -- GetSpriteSize, sizeof(CEngineSprite)
//   43  returns a global pointer          -- GetStandardRecvProxies
//-----------------------------------------------------------------------------
namespace client_slot {
enum
{
	// SDK index 5, and IBaseClientDLL only appends at 47 -- so 0..46 are
	// unshifted and this needs no separate verification beyond that fact, which
	// View_Render at 23 already proves.
	kGetAllClasses     = 5,   // cdll_int.h:20
	kHudUpdate         = 8,   // cdll_int.h:428
	kCreateMove        = 18,  // cdll_int.h:456
	kView_Render       = 23,  // cdll_int.h:473  <- primary camera hook, VERIFIED
	kRenderView        = 24,  // cdll_int.h:476
	kFrameStageNotify  = 32,  // cdll_int.h:503
	kSlotCount         = 48,
};
} // namespace client_slot

// Matches ClientFrameStage_t in cdll_int.h:93-103.
enum ClientFrameStage_t
{
	FRAME_UNDEFINED = -1,
	FRAME_START,
	FRAME_NET_UPDATE_START,
	FRAME_NET_UPDATE_POSTDATAUPDATE_START,
	FRAME_NET_UPDATE_POSTDATAUPDATE_END,
	FRAME_NET_UPDATE_END,
	FRAME_RENDER_START,
	FRAME_RENDER_END
};

//-----------------------------------------------------------------------------
// Source module factory: every Source DLL exports CreateInterface.
//-----------------------------------------------------------------------------
using CreateInterfaceFn = void* ( * )( const char* name, int* returnCode );

inline void* GetInterface( const char* moduleName, const char* interfaceName )
{
	HMODULE mod = GetModuleHandleA( moduleName );
	if ( !mod )
		return nullptr;

	CreateInterfaceFn factory =
		reinterpret_cast<CreateInterfaceFn>( GetProcAddress( mod, "CreateInterface" ) );
	if ( !factory )
		return nullptr;

	int returnCode = 0;
	return factory( interfaceName, &returnCode );
}

} // namespace sinvr
