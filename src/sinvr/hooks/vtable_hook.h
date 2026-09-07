// In-place vtable slot patching.
//
// Source interfaces are plain pure-virtual classes with a single global instance
// per DLL, so swapping a slot in the live vtable is enough to intercept every
// engine -> client call. The vtable lives in .rdata, hence the VirtualProtect.
#pragma once

#include <windows.h>
#include "../sdk/source_interfaces.h"

namespace sinvr {

class VTableHook
{
public:
	VTableHook() : m_vtable( nullptr ), m_index( 0 ), m_original( nullptr ) {}

	// Returns the original function pointer, or nullptr on failure.
	void* Install( void* instance, int index, void* detour )
	{
		if ( !instance || !detour )
			return nullptr;

		m_vtable = VTableOf( instance );
		m_index = index;

		DWORD oldProtect = 0;
		if ( !VirtualProtect( &m_vtable[m_index], sizeof( void* ),
							  PAGE_READWRITE, &oldProtect ) )
		{
			m_vtable = nullptr;
			return nullptr;
		}

		m_original = m_vtable[m_index];
		m_vtable[m_index] = detour;

		VirtualProtect( &m_vtable[m_index], sizeof( void* ), oldProtect, &oldProtect );
		return m_original;
	}

	void Remove()
	{
		if ( !m_vtable || !m_original )
			return;

		DWORD oldProtect = 0;
		if ( VirtualProtect( &m_vtable[m_index], sizeof( void* ),
							 PAGE_READWRITE, &oldProtect ) )
		{
			m_vtable[m_index] = m_original;
			VirtualProtect( &m_vtable[m_index], sizeof( void* ), oldProtect, &oldProtect );
		}

		m_vtable = nullptr;
		m_original = nullptr;
	}

	void* Original() const { return m_original; }
	bool Installed() const { return m_vtable != nullptr && m_original != nullptr; }

private:
	void** m_vtable;
	int m_index;
	void* m_original;
};

} // namespace sinvr
