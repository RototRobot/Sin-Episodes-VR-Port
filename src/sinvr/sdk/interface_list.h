#pragma once
//-----------------------------------------------------------------------------
// EVERY INTERFACE A SOURCE MODULE EXPOSES, BY NAME.
//
// Until now every interface in this project was found by GUESSING a version
// string and seeing whether the factory returned non-null -- "VClient011",
// "VGUI_Input005", "EngineTraceClient003". That works when the guess is right
// and tells you nothing when it is wrong: a null return cannot distinguish
// "this module has no such interface" from "it has one under a name you did not
// think of".
//
// The immediate question is 6DoF. Moving the player needs the CUserCmd, and
// SDK 2004 does not expose IInput through CreateInterface -- so the choice
// between a clean interface route and a signature scan for CInput::CreateMove
// hangs entirely on what client.dll actually offers. That is a question about
// the binary, and it deserves an answer from the binary.
//
// ---- HOW THE LIST IS FOUND --------------------------------------------------
//
// Source's factory is a linked list built by static constructors:
//
//     class InterfaceReg {
//         InstantiateInterfaceFn m_CreateFn;   // +0
//         const char*            m_pName;      // +4
//         InterfaceReg*          m_pNext;      // +8
//         static InterfaceReg*   s_pInterfaceRegs;
//     };
//
// `s_pInterfaceRegs` is a private static and is not exported -- but the
// EXPORTED CreateInterface walks it, so its address appears as an absolute
// operand in that function's first few instructions.
//
// ---- AND WHY THE VALIDATION IS THE IMPORTANT PART ---------------------------
//
// Scanning for `mov reg, [imm32]` yields several candidates, most of which are
// other globals. Picking the wrong one and walking it dereferences arbitrary
// addresses as a linked list of strings.
//
// So a candidate is only accepted if WALKING it produces a chain of nodes whose
// name pointers are readable, printable ASCII, plausibly short, and whose next
// pointers stay inside the module. A wrong global fails that within a node or
// two. The best-scoring candidate wins, and if none scores at all we report the
// failure and change nothing -- exactly the rule the vgui cursor route was
// written down for after its check passed on garbage.
//
// Read-only from start to finish. Nothing here calls a factory, instantiates an
// interface, or writes to the game.
//-----------------------------------------------------------------------------

#include <windows.h>
#include <string.h>

#include "source_interfaces.h"
#include "../../common/log.h"

namespace sinvr {

struct InterfaceRegView
{
	void* createFn;
	const char* name;
	InterfaceRegView* next;
};

// A name has to look like an interface name, not merely be readable. Source's
// are short, printable, and have no spaces -- "VClient011", "VEngineClient012".
inline bool LooksLikeInterfaceName( const char* p )
{
	if ( !p || IsBadReadPtr( p, 4 ) )
		return false;

	int i = 0;
	for ( ; i < 64; ++i )
	{
		const unsigned char c = (unsigned char)p[i];
		if ( c == 0 )
			break;
		if ( c < 0x21 || c > 0x7E )
			return false;
	}
	return i >= 3 && i < 64;
}

// Walks a candidate head pointer and returns how many plausible nodes it holds.
// Zero means the candidate is not the list.
// ---- WHY THE WALK STOPPED IS PART OF THE ANSWER ---------------------------
//
// server.dll came back with five names and no way to tell "the list IS five
// long" from "our validation rejected node six". Those mean opposite things:
// the first says GameMovement001 is genuinely absent from the server, the
// second says our reader is broken and the whole result is worthless.
//
// A count with no terminator reason is the same mistake as "missing 1 key(s)"
// -- a number that cannot be checked against anything.
inline int ScoreInterfaceChain( InterfaceRegView* head, uintptr_t lo, uintptr_t hi,
								void ( *emit )( const char* ) = nullptr,
								const char** stopReason = nullptr,
								const void** stopAt = nullptr )
{
	int count = 0;
	InterfaceRegView* cur = head;
	if ( stopReason )
		*stopReason = "reached the end of the list (m_pNext was null)";
	if ( stopAt )
		*stopAt = nullptr;

	__try
	{
		// Bounded: a wrong candidate can produce a cycle, and a cycle in a
		// diagnostic is a hang at startup.
		for ( int guard = 0; cur && guard < 512; ++guard )
		{
			if ( stopAt )
				*stopAt = cur;

			if ( IsBadReadPtr( cur, sizeof( InterfaceRegView ) ) )
			{
				if ( stopReason ) *stopReason = "node was not readable";
				break;
			}

			const uintptr_t fn = (uintptr_t)cur->createFn;
			if ( fn < lo || fn >= hi )
			{
				if ( stopReason ) *stopReason = "m_CreateFn pointed OUTSIDE the module";
				break;
			}

			if ( !LooksLikeInterfaceName( cur->name ) )
			{
				if ( stopReason ) *stopReason = "m_pName did not look like an interface name";
				break;
			}

			if ( guard == 511 && stopReason )
				*stopReason = "hit the 512-node guard -- the list is longer than this reports";

			if ( emit )
				emit( cur->name );

			++count;
			cur = cur->next;
		}
	}
	__except ( EXCEPTION_EXECUTE_HANDLER )
	{
		// A fault means the candidate was not the list. That is a RESULT, not
		// an error -- most candidates are expected to fail here.
		if ( stopReason )
			*stopReason = "FAULTED while walking";
	}

	return count;
}

inline void EmitInterfaceName( const char* name )
{
	Log( "    %s", name );
}

// Returns the number of interfaces logged, or 0 if the list could not be found.
inline int LogModuleInterfaces( const char* moduleName )
{
	HMODULE mod = GetModuleHandleA( moduleName );
	if ( !mod )
	{
		LogWarn( "interfaces: %s is not loaded", moduleName );
		return 0;
	}

	auto factory = (unsigned char*)GetProcAddress( mod, "CreateInterface" );
	if ( !factory )
	{
		LogWarn( "interfaces: %s exports no CreateInterface", moduleName );
		return 0;
	}

	// Module bounds, so a candidate operand and the function pointers it leads
	// to can be range-checked rather than merely dereferenced.
	//
	// Read from the PE headers rather than through psapi: dllmain.cpp's
	// GetModuleRange already does exactly this, and adding a psapi dependency
	// to a diagnostic that can get its answer from the image itself is not a
	// trade worth making.
	uintptr_t lo = (uintptr_t)mod;
	uintptr_t hi = 0;
	{
		auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>( mod );
		if ( dos->e_magic != IMAGE_DOS_SIGNATURE )
			return 0;
		auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(
			reinterpret_cast<BYTE*>( mod ) + dos->e_lfanew );
		if ( nt->Signature != IMAGE_NT_SIGNATURE )
			return 0;
		hi = lo + nt->OptionalHeader.SizeOfImage;
	}

	// ---- CANDIDATES, FROM THE FIRST FEW INSTRUCTIONS ---------------------
	//
	// x86 forms that load a global into a register, which is what the walk's
	// first instruction has to be:
	//
	//     A1 imm32           mov eax, [imm32]
	//     8B 0D/15/1D/35/3D  mov ecx/edx/ebx/esi/edi, [imm32]
	//     83 3D imm32 00     cmp dword [imm32], 0
	uintptr_t cands[16];
	int candCount = 0;

	__try
	{
		for ( int i = 0; i < 96 && candCount < 16; ++i )
		{
			const unsigned char* p = factory + i;
			uintptr_t addr = 0;

			if ( p[0] == 0xA1 )
				addr = *(uintptr_t*)( p + 1 );
			else if ( p[0] == 0x8B && ( p[1] == 0x0D || p[1] == 0x15 ||
										p[1] == 0x1D || p[1] == 0x35 || p[1] == 0x3D ) )
				addr = *(uintptr_t*)( p + 2 );
			else if ( p[0] == 0x83 && p[1] == 0x3D )
				addr = *(uintptr_t*)( p + 2 );

			if ( addr >= lo && addr < hi )
			{
				bool dup = false;
				for ( int j = 0; j < candCount; ++j )
					if ( cands[j] == addr ) { dup = true; break; }
				if ( !dup )
					cands[candCount++] = addr;
			}
		}
	}
	__except ( EXCEPTION_EXECUTE_HANDLER )
	{
		LogWarn( "interfaces: faulted scanning %s CreateInterface", moduleName );
		return 0;
	}

	// ---- PICK THE ONE THAT ACTUALLY WALKS --------------------------------
	uintptr_t best = 0;
	int bestScore = 0;
	for ( int i = 0; i < candCount; ++i )
	{
		InterfaceRegView* head = nullptr;
		__try
		{
			head = *(InterfaceRegView**)cands[i];
		}
		__except ( EXCEPTION_EXECUTE_HANDLER )
		{
			continue;
		}

		const int score = ScoreInterfaceChain( head, lo, hi );
		if ( score > bestScore )
		{
			bestScore = score;
			best = cands[i];
		}
	}

	if ( bestScore < 3 )
	{
		LogWarn( "interfaces: %s -- could not locate s_pInterfaceRegs "
				 "(%d candidate operand(s), best chain %d node(s)). Nothing was "
				 "walked; the by-name lookups are unaffected.",
				 moduleName, candCount, bestScore );
		return 0;
	}

	Log( "interfaces: %s exposes %d interface(s)  [s_pInterfaceRegs at %p, "
		 "%d candidate(s) considered]",
		 moduleName, bestScore, (void*)best, candCount );

	InterfaceRegView* head = *(InterfaceRegView**)best;
	const char* why = "unknown";
	const void* where = nullptr;
	ScoreInterfaceChain( head, lo, hi, &EmitInterfaceName, &why, &where );

	// Repeated AFTER the names on purpose. The header above went missing from
	// one run's log entirely -- the dump runs on the render thread while init
	// logs from another, and a line was lost. A summary at both ends survives
	// losing either.
	Log( "interfaces: %s -- %d interface(s), walk stopped because %s (at %p)",
		 moduleName, bestScore, why, where );
	return bestScore;
}

} // namespace sinvr
