// Crash and hang reporting, written to sinvr_crash.log beside the exe.
//
// Two distinct failure modes need catching, and only one of them is a crash:
//
//   * A real fault (access violation and friends) raises an exception, which
//     the vectored handler below records.
//
//   * A GPU timeout does not. The last one showed up as Windows LiveKernelEvent
//     0x141 (VIDEO_ENGINE_TIMEOUT_DETECTED) with the process still alive and
//     DXVK reporting VK_ERROR_DEVICE_LOST -- frames simply stopped. No exception
//     is ever raised, so the watchdog below catches it by noticing that the
//     frame counter has stopped moving.
#pragma once

#include <windows.h>
#include <dbghelp.h>
#include "log.h"

namespace sinvr {

//-----------------------------------------------------------------------------
// DbgHelp, loaded dynamically so sinvr.dll keeps zero load-time dependencies.
//
// The frame-pointer walk only managed three frames into DXVK, because DXVK is
// built optimised with frame pointers omitted. StackWalk64 uses the unwind data
// instead and follows those frames properly.
//-----------------------------------------------------------------------------
struct DbgHelpApi
{
	using PFN_SymInitialize = BOOL( WINAPI* )( HANDLE, PCSTR, BOOL );
	using PFN_SymSetOptions = DWORD( WINAPI* )( DWORD );
	using PFN_StackWalk64 = BOOL( WINAPI* )( DWORD, HANDLE, HANDLE, LPSTACKFRAME64, PVOID,
											 PREAD_PROCESS_MEMORY_ROUTINE64,
											 PFUNCTION_TABLE_ACCESS_ROUTINE64,
											 PGET_MODULE_BASE_ROUTINE64,
											 PTRANSLATE_ADDRESS_ROUTINE64 );
	using PFN_SymFunctionTableAccess64 = PVOID( WINAPI* )( HANDLE, DWORD64 );
	using PFN_SymGetModuleBase64 = DWORD64( WINAPI* )( HANDLE, DWORD64 );

	HMODULE dll = nullptr;
	PFN_SymInitialize SymInitialize = nullptr;
	PFN_SymSetOptions SymSetOptions = nullptr;
	PFN_StackWalk64 StackWalk64 = nullptr;
	PFN_SymFunctionTableAccess64 SymFunctionTableAccess64 = nullptr;
	PFN_SymGetModuleBase64 SymGetModuleBase64 = nullptr;
	bool initialised = false;

	bool Load()
	{
		if ( dll )
			return initialised;

		dll = LoadLibraryA( "dbghelp.dll" );
		if ( !dll )
			return false;

		SymInitialize = (PFN_SymInitialize)GetProcAddress( dll, "SymInitialize" );
		SymSetOptions = (PFN_SymSetOptions)GetProcAddress( dll, "SymSetOptions" );
		StackWalk64 = (PFN_StackWalk64)GetProcAddress( dll, "StackWalk64" );
		SymFunctionTableAccess64 =
			(PFN_SymFunctionTableAccess64)GetProcAddress( dll, "SymFunctionTableAccess64" );
		SymGetModuleBase64 = (PFN_SymGetModuleBase64)GetProcAddress( dll, "SymGetModuleBase64" );

		if ( !SymInitialize || !StackWalk64 || !SymFunctionTableAccess64 || !SymGetModuleBase64 )
			return false;

		if ( SymSetOptions )
			SymSetOptions( SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME );

		initialised = SymInitialize( GetCurrentProcess(), NULL, TRUE ) != FALSE;
		return initialised;
	}
};

inline DbgHelpApi& DbgHelp()
{
	static DbgHelpApi api;
	return api;
}

inline const wchar_t* CrashLogPath()
{
	return SiblingPath( L"sinvr_crash.log" );
}

inline void CrashLog( const char* fmt, ... )
{
	char body[1024];
	va_list args;
	va_start( args, fmt );
	_vsnprintf_s( body, sizeof( body ), _TRUNCATE, fmt, args );
	va_end( args );

	SYSTEMTIME st;
	GetLocalTime( &st );

	char line[1200];
	_snprintf_s( line, sizeof( line ), _TRUNCATE, "[%02d:%02d:%02d.%03d] %s\r\n",
				 st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, body );

	HANDLE f = CreateFileW( CrashLogPath(), FILE_APPEND_DATA, FILE_SHARE_READ, NULL,
							OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL );
	if ( f != INVALID_HANDLE_VALUE )
	{
		DWORD written = 0;
		WriteFile( f, line, (DWORD)strlen( line ), &written, NULL );
		CloseHandle( f );
	}

	// Mirror into the main log so one file still tells the whole story.
	LogError( "%s", body );
}

// Which module an address belongs to, and how far into it. Far more useful than
// a bare address when comparing runs, since module bases can move.
inline void DescribeAddress( void* addr, char* out, size_t outSize )
{
	HMODULE mod = NULL;
	if ( GetModuleHandleExA( GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
								 GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
							 (LPCSTR)addr, &mod ) &&
		 mod )
	{
		char path[MAX_PATH] = { 0 };
		GetModuleFileNameA( mod, path, MAX_PATH );

		const char* name = strrchr( path, '\\' );
		name = name ? name + 1 : path;

		uintptr_t offset = (uintptr_t)addr - (uintptr_t)mod;
		_snprintf_s( out, outSize, _TRUNCATE, "%p (%s+0x%IX)", addr, name, offset );
	}
	else
	{
		_snprintf_s( out, outSize, _TRUNCATE, "%p (unknown module)", addr );
	}
}

inline const char* ExceptionName( DWORD code )
{
	switch ( code )
	{
		case EXCEPTION_ACCESS_VIOLATION:      return "ACCESS_VIOLATION";
		case EXCEPTION_ILLEGAL_INSTRUCTION:   return "ILLEGAL_INSTRUCTION";
		case EXCEPTION_STACK_OVERFLOW:        return "STACK_OVERFLOW";
		case EXCEPTION_INT_DIVIDE_BY_ZERO:    return "INT_DIVIDE_BY_ZERO";
		case EXCEPTION_FLT_DIVIDE_BY_ZERO:    return "FLT_DIVIDE_BY_ZERO";
		case EXCEPTION_PRIV_INSTRUCTION:      return "PRIV_INSTRUCTION";
		case EXCEPTION_IN_PAGE_ERROR:         return "IN_PAGE_ERROR";
		case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: return "ARRAY_BOUNDS_EXCEEDED";
		case EXCEPTION_DATATYPE_MISALIGNMENT: return "DATATYPE_MISALIGNMENT";
		default:                              return "unknown";
	}
}

inline bool IsFatalException( DWORD code )
{
	switch ( code )
	{
		case EXCEPTION_ACCESS_VIOLATION:
		case EXCEPTION_ILLEGAL_INSTRUCTION:
		case EXCEPTION_STACK_OVERFLOW:
		case EXCEPTION_PRIV_INSTRUCTION:
		case EXCEPTION_IN_PAGE_ERROR:
		case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
			return true;
		default:
			return false;
	}
}

inline LONG CALLBACK VectoredCrashHandler( EXCEPTION_POINTERS* info )
{
	if ( !info || !info->ExceptionRecord )
		return EXCEPTION_CONTINUE_SEARCH;

	const EXCEPTION_RECORD* rec = info->ExceptionRecord;

	// First-chance C++ exceptions and debugger breakpoints are normal traffic.
	if ( !IsFatalException( rec->ExceptionCode ) )
		return EXCEPTION_CONTINUE_SEARCH;

	// Log once. A fault inside the fault path helps nobody.
	static LONG reported = 0;
	if ( InterlockedExchange( &reported, 1 ) != 0 )
		return EXCEPTION_CONTINUE_SEARCH;

	char where[512];
	DescribeAddress( rec->ExceptionAddress, where, sizeof( where ) );

	CrashLog( "=== CRASH ===" );
	CrashLog( "exception %s (0x%08lX) at %s",
			  ExceptionName( rec->ExceptionCode ), rec->ExceptionCode, where );

	if ( rec->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && rec->NumberParameters >= 2 )
	{
		const char* op = rec->ExceptionInformation[0] == 0   ? "reading"
						 : rec->ExceptionInformation[0] == 1 ? "writing"
															 : "executing";
		CrashLog( "  %s address 0x%IX", op, rec->ExceptionInformation[1] );
	}

#ifdef _M_IX86
	const CONTEXT* c = info->ContextRecord;
	if ( c )
	{
		CrashLog( "  eax=%08lX ebx=%08lX ecx=%08lX edx=%08lX",
				  c->Eax, c->Ebx, c->Ecx, c->Edx );
		CrashLog( "  esi=%08lX edi=%08lX ebp=%08lX esp=%08lX eip=%08lX",
				  c->Esi, c->Edi, c->Ebp, c->Esp, c->Eip );

		// Walk the frame chain. Source is built with frame pointers, so this is
		// usually good for a readable trace without needing DbgHelp.
		CrashLog( "  --- frames ---" );
		uintptr_t* frame = (uintptr_t*)c->Ebp;
		for ( int i = 0; i < 16 && frame; ++i )
		{
			if ( IsBadReadPtr( frame, sizeof( uintptr_t ) * 2 ) )
				break;

			void* ret = (void*)frame[1];
			if ( !ret )
				break;

			char frameWhere[512];
			DescribeAddress( ret, frameWhere, sizeof( frameWhere ) );
			CrashLog( "  [%02d] %s", i, frameWhere );

			uintptr_t* next = (uintptr_t*)frame[0];
			if ( next <= frame )
				break;
			frame = next;
		}
	}
#endif

	CrashLog( "=== END CRASH ===" );

	// Let the game's own handler run too -- we are only observing.
	return EXCEPTION_CONTINUE_SEARCH;
}

//-----------------------------------------------------------------------------
// Stack trace of another thread, captured by suspending it.
//
// This is the piece that was missing for hangs. An exception handler only fires
// on a fault; when the render thread simply stops making progress there is no
// event at all, and the only way to learn where it is stuck is to go and look.
// Suspend, read the frame chain, resume.
//-----------------------------------------------------------------------------
inline void LogThreadStack( DWORD threadId, const char* label )
{
	if ( threadId == 0 || threadId == GetCurrentThreadId() )
	{
		CrashLog( "  (%s: no usable thread id)", label );
		return;
	}

	HANDLE thread = OpenThread( THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT |
								   THREAD_QUERY_INFORMATION,
							   FALSE, threadId );
	if ( !thread )
	{
		CrashLog( "  (%s: OpenThread failed, err %lu)", label, GetLastError() );
		return;
	}

	if ( SuspendThread( thread ) == (DWORD)-1 )
	{
		CrashLog( "  (%s: SuspendThread failed, err %lu)", label, GetLastError() );
		CloseHandle( thread );
		return;
	}

	CONTEXT ctx = {};
	ctx.ContextFlags = CONTEXT_FULL;

	if ( GetThreadContext( thread, &ctx ) )
	{
#ifdef _M_IX86
		char where[512];
		DescribeAddress( (void*)ctx.Eip, where, sizeof( where ) );
		CrashLog( "  %s (tid %lu) is at %s", label, threadId, where );
		CrashLog( "    eax=%08lX ecx=%08lX edx=%08lX esi=%08lX edi=%08lX",
				  ctx.Eax, ctx.Ecx, ctx.Edx, ctx.Esi, ctx.Edi );
		CrashLog( "    ebp=%08lX esp=%08lX", ctx.Ebp, ctx.Esp );

		bool walked = false;
		if ( DbgHelp().Load() )
		{
			CONTEXT walkCtx = ctx;
			STACKFRAME64 frame64 = {};
			frame64.AddrPC.Offset = ctx.Eip;
			frame64.AddrPC.Mode = AddrModeFlat;
			frame64.AddrFrame.Offset = ctx.Ebp;
			frame64.AddrFrame.Mode = AddrModeFlat;
			frame64.AddrStack.Offset = ctx.Esp;
			frame64.AddrStack.Mode = AddrModeFlat;

			for ( int i = 0; i < 32; ++i )
			{
				if ( !DbgHelp().StackWalk64( IMAGE_FILE_MACHINE_I386, GetCurrentProcess(),
											 thread, &frame64, &walkCtx, NULL,
											 DbgHelp().SymFunctionTableAccess64,
											 DbgHelp().SymGetModuleBase64, NULL ) )
					break;

				if ( frame64.AddrPC.Offset == 0 )
					break;

				char frameWhere[512];
				DescribeAddress( (void*)(uintptr_t)frame64.AddrPC.Offset,
								 frameWhere, sizeof( frameWhere ) );
				CrashLog( "    [%02d] %s", i, frameWhere );
				walked = true;
			}
		}

		if ( !walked )
		{
			// Fallback: frame-pointer chain. Only good for code built with them.
			CrashLog( "    (StackWalk64 unavailable, using frame-pointer chain)" );
			uintptr_t* frame = (uintptr_t*)ctx.Ebp;
			for ( int i = 0; i < 24 && frame; ++i )
			{
				if ( IsBadReadPtr( frame, sizeof( uintptr_t ) * 2 ) )
					break;

				void* ret = (void*)frame[1];
				if ( !ret )
					break;

				char frameWhere[512];
				DescribeAddress( ret, frameWhere, sizeof( frameWhere ) );
				CrashLog( "    [%02d] %s", i, frameWhere );

				uintptr_t* next = (uintptr_t*)frame[0];
				if ( next <= frame )
					break;
				frame = next;
			}
		}
#else
		CrashLog( "  %s (tid %lu): stack walk only implemented for x86", label, threadId );
#endif
	}
	else
	{
		CrashLog( "  (%s: GetThreadContext failed, err %lu)", label, GetLastError() );
	}

	ResumeThread( thread );
	CloseHandle( thread );
}

inline void InstallCrashHandler()
{
	DeleteFileW( CrashLogPath() );
	AddVectoredExceptionHandler( 1 /* call first */, VectoredCrashHandler );
	Log( "crash handler installed -> sinvr_crash.log" );
}

} // namespace sinvr
