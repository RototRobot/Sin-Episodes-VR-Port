// Address-space accounting for a 32-bit process.
//
// SinEpisodes.exe ships without IMAGE_FILE_LARGE_ADDRESS_AWARE, so it is capped
// at 2 GB of user-mode virtual address space -- shared with DXVK's shader cache,
// staging buffers and our four full-size eye render targets. The failure mode is
// not a clean "out of memory": the address space FRAGMENTS, so an allocation of
// a few MB fails while hundreds of MB are still nominally free, and it surfaces
// as a crash in whatever subsystem happened to ask next. That is why this is
// measured rather than assumed -- see the launcher in src/launcher/main.cpp,
// which is what actually lifts the cap.
//
// Header-only so the mod and the launcher can share it without a link step.
#pragma once

#include <windows.h>

namespace sinvr {

struct AddressSpaceStats
{
	// Whether this process actually got the 4 GB space. Read from the live
	// process, not from the PE header on disk: the loader decides this at image
	// map time and nothing afterwards can change it, so the running value is the
	// only one that means anything.
	bool largeAddressAware = false;

	unsigned long long limitMB = 0;         // highest usable user address + 1
	unsigned long long committedMB = 0;     // backed by RAM or pagefile
	unsigned long long reservedMB = 0;      // committed + reserve-only
	unsigned long long freeMB = 0;          // total unallocated
	unsigned long long largestFreeMB = 0;   // biggest single unallocated run
	unsigned int regions = 0;
};

// Walks the process's own VA space. Cheap enough for a heartbeat -- a few
// thousand VirtualQuery calls -- but not for a per-frame path.
inline AddressSpaceStats QueryAddressSpace()
{
	AddressSpaceStats a;

	// GetSystemInfo, deliberately NOT GetNativeSystemInfo: we want the limit
	// that applies to THIS process. Under WOW64 the native call reports the
	// 64-bit machine's range and would say 4 GB even when we are capped at 2.
	SYSTEM_INFO si = {};
	GetSystemInfo( &si );

	const uintptr_t low = (uintptr_t)si.lpMinimumApplicationAddress;
	const uintptr_t high = (uintptr_t)si.lpMaximumApplicationAddress;

	a.limitMB = ( (unsigned long long)high + 1 ) / ( 1024ull * 1024ull );
	a.largeAddressAware = ( high > 0x80000000u );

	unsigned long long committed = 0, reserved = 0, freeTotal = 0, largestFree = 0;

	MEMORY_BASIC_INFORMATION mbi = {};
	uintptr_t addr = low;
	while ( addr < high && VirtualQuery( (LPCVOID)addr, &mbi, sizeof( mbi ) ) == sizeof( mbi ) )
	{
		const unsigned long long size = (unsigned long long)mbi.RegionSize;

		if ( mbi.State == MEM_FREE )
		{
			freeTotal += size;
			if ( size > largestFree )
				largestFree = size;
		}
		else
		{
			reserved += size;
			if ( mbi.State == MEM_COMMIT )
				committed += size;
		}

		++a.regions;

		const uintptr_t next = (uintptr_t)mbi.BaseAddress + (uintptr_t)mbi.RegionSize;
		if ( next <= addr )   // no forward progress: stop rather than spin
			break;
		addr = next;
	}

	const unsigned long long mb = 1024ull * 1024ull;
	a.committedMB = committed / mb;
	a.reservedMB = reserved / mb;
	a.freeMB = freeTotal / mb;
	a.largestFreeMB = largestFree / mb;
	return a;
}

} // namespace sinvr
