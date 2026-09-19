// Networked property lookup by NAME.
//
// Every offset this mod uses so far was found by hand against a live process and
// written down as a constant -- which works, and is why client_entity_list.h
// carries its disassembly evidence, but each one is a hostage to the next game
// update. Source hands out a complete map of its networked fields at runtime,
// and using it costs almost nothing.
//
//   IBaseClientDLL::GetAllClasses()  -> ClientClass linked list   (SDK slot 5,
//                                       below the +1 shift boundary at 47)
//   ClientClass::m_pRecvTable        -> RecvTable
//   RecvTable::m_pProps[n]           -> RecvProp { name, ..., offset }
//
// Nested tables (DT_Local inside DT_BasePlayer, say) are recursed with their
// offsets accumulated, which is how a field like m_vecPunchAngle -- declared as
// m_Local.m_vecPunchAngle -- resolves to a single offset from the entity base.
//
// Layouts below are from the SDK headers and are NOT independently verified
// against SiN's binaries, so every lookup is bounds-checked and every failure is
// reported rather than assumed. A wrong layout here yields a nonsense offset,
// and a nonsense offset is a write into the middle of a live entity.
#pragma once

#include <windows.h>
#include "source_interfaces.h"
#include "../../common/log.h"

namespace sinvr {

// public/client_class.h
struct ClientClassRaw
{
	void* m_pCreateFn;          // 0x00
	void* m_pCreateEventFn;     // 0x04
	const char* m_pNetworkName; // 0x08
	void* m_pRecvTable;         // 0x0C
	ClientClassRaw* m_pNext;    // 0x10
	int m_ClassID;              // 0x14
};

// public/dt_recv.h -- RecvTable
struct RecvTableRaw
{
	void* m_pProps;             // 0x00  RecvProp*
	int m_nProps;               // 0x04
	void* m_pDecoder;           // 0x08
	const char* m_pNetTableName;// 0x0C
	bool m_bInitialized;        // 0x10
	bool m_bInMainList;         // 0x11
};

// public/dt_recv.h -- RecvProp. 60 bytes; the two fields that matter are the
// name at the front and the offset at 0x2C.
struct RecvPropRaw
{
	const char* m_pVarName;     // 0x00
	int m_RecvType;             // 0x04
	int m_Flags;                // 0x08
	int m_StringBufferSize;     // 0x0C
	int m_bInsideArray;         // 0x10  (bool, padded)
	const void* m_pExtraData;   // 0x14
	void* m_pArrayProp;         // 0x18
	void* m_ArrayLengthProxy;   // 0x1C
	void* m_ProxyFn;            // 0x20
	void* m_DataTableProxyFn;   // 0x24
	void* m_pDataTable;         // 0x28
	int m_Offset;               // 0x2C
	int m_ElementStride;        // 0x30
	int m_nElements;            // 0x34
	const char* m_pParentArrayPropName; // 0x38
};

class NetProps
{
public:
	// `client` is the IBaseClientDLL instance the mod already binds.
	bool Init( void* client )
	{
		m_head = nullptr;
		if ( !client )
			return false;

		__try
		{
			m_head = VCall<client_slot::kGetAllClasses, ClientClassRaw*>( client );
		}
		__except ( EXCEPTION_EXECUTE_HANDLER )
		{
			m_head = nullptr;
		}

		if ( !m_head )
		{
			LogWarn( "netprops: GetAllClasses returned nothing" );
			return false;
		}

		int classes = 0;
		for ( ClientClassRaw* c = m_head; c && classes < 4096; c = c->m_pNext )
			++classes;
		Log( "netprops: %d client classes registered", classes );
		return classes > 0;
	}

	bool Valid() const { return m_head != nullptr; }

	// Offset of `propName` from the start of an entity of network class
	// `className`, or -1. Recurses nested tables and accumulates their offsets,
	// so "m_vecPunchAngle" resolves even though it lives in DT_Local.
	int Find( const char* className, const char* propName ) const
	{
		if ( !m_head || !className || !propName )
			return -1;

		for ( ClientClassRaw* c = m_head; c; c = c->m_pNext )
		{
			if ( !c->m_pNetworkName || _stricmp( c->m_pNetworkName, className ) != 0 )
				continue;
			return FindInTable( reinterpret_cast<RecvTableRaw*>( c->m_pRecvTable ),
								propName, 0 );
		}
		return -1;
	}

	// Search EVERY registered class for a prop, and say which one had it.
	//
	// Find() needs the network class name, and guessing that is how the
	// m_vecViewOffset lookup failed silently: three plausible names were
	// tried, all three missed, and the feature that depended on it degraded
	// with a warning nobody reads until something looks wrong in the
	// headset. The class name is not the interesting part of the question --
	// the prop is -- so this asks the question that was actually meant.
	//
	// Player classes are searched FIRST. A prop like m_vecViewOffset exists
	// on several entity types at different offsets (C_BaseFlex has its own),
	// so first-match-anywhere could return a real offset into the wrong
	// class -- which is worse than finding nothing, because it reads as
	// success.
	int FindAnywhere( const char* propName, const char** outClass ) const
	{
		if ( outClass )
			*outClass = nullptr;
		if ( !m_head || !propName )
			return -1;

		for ( int pass = 0; pass < 2; ++pass )
		{
			for ( ClientClassRaw* c = m_head; c; c = c->m_pNext )
			{
				if ( !c->m_pNetworkName )
					continue;

				const bool playerish = ContainsNoCase( c->m_pNetworkName, "player" );
				if ( ( pass == 0 ) != playerish )
					continue;

				const int off = FindInTable(
					reinterpret_cast<RecvTableRaw*>( c->m_pRecvTable ), propName, 0 );
				if ( off >= 0 )
				{
					if ( outClass )
						*outClass = c->m_pNetworkName;
					return off;
				}
			}
		}
		return -1;
	}

private:
	static bool ContainsNoCase( const char* haystack, const char* needle )
	{
		if ( !haystack || !needle || !*needle )
			return false;
		for ( const char* h = haystack; *h; ++h )
		{
			const char* a = h;
			const char* b = needle;
			while ( *a && *b && ( *a | 0x20 ) == ( *b | 0x20 ) )
			{
				++a;
				++b;
			}
			if ( !*b )
				return true;
		}
		return false;
	}

	static int FindInTable( RecvTableRaw* table, const char* propName, int depth )
	{
		// Depth cap rather than trust: a malformed pointer chain would otherwise
		// recurse until the stack gives out.
		if ( !table || depth > 12 )
			return -1;

		int count = 0;
		const void* props = nullptr;
		__try
		{
			count = table->m_nProps;
			props = table->m_pProps;
		}
		__except ( EXCEPTION_EXECUTE_HANDLER )
		{
			return -1;
		}

		if ( !props || count <= 0 || count > 8192 )
			return -1;

		for ( int i = 0; i < count; ++i )
		{
			const RecvPropRaw* p =
				reinterpret_cast<const RecvPropRaw*>(
					reinterpret_cast<const unsigned char*>( props ) + i * sizeof( RecvPropRaw ) );

			const char* name = nullptr;
			int offset = 0;
			void* child = nullptr;
			__try
			{
				name = p->m_pVarName;
				offset = p->m_Offset;
				child = p->m_pDataTable;
			}
			__except ( EXCEPTION_EXECUTE_HANDLER )
			{
				continue;
			}

			if ( !name )
				continue;

			if ( _stricmp( name, propName ) == 0 )
				return offset;

			// Nested table: its props are relative to it, so carry the offset.
			if ( child )
			{
				const int inner = FindInTable( reinterpret_cast<RecvTableRaw*>( child ),
											   propName, depth + 1 );
				if ( inner >= 0 )
					return offset + inner;
			}
		}
		return -1;
	}

	ClientClassRaw* m_head = nullptr;
};

} // namespace sinvr
