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
//
// ---- WHAT THE 2026-09-11 CRASH COULD NOT TELL US ---------------------------
//
// A player's report -- Quest 2 through SteamVR's Oculus driver, RTX 3070
// laptop -- read: ACCESS_VIOLATION reading 0x1E0 in nvoglv32.dll, four frames
// of vrclient.dll above it, then sinvr.dll. It could not answer:
//
//   which OpenVR call   only an offset into sinvr.dll, which names nothing
//                       without that exact build. ExternalCall() now does.
//   what came before    Breadcrumb() keeps the last 48 notable events -- the
//                       headset sleeping, the dashboard, a device reset, a
//                       menu opening -- and they go out with the report.
//   which driver        the VR-path modules and their file versions are now
//                       listed, NVIDIA's as a driver release number.
//   what state          sinvr_crash.dmp, a minidump for a debugger.
//
// And a report now survives the next launch as sinvr_crash.log.prev. It used
// to be deleted on startup -- which is exactly when an intermittent crash is
// being reproduced.
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
	using PFN_MiniDumpWriteDump = BOOL( WINAPI* )( HANDLE, DWORD, HANDLE, MINIDUMP_TYPE,
												   PMINIDUMP_EXCEPTION_INFORMATION,
												   PMINIDUMP_USER_STREAM_INFORMATION,
												   PMINIDUMP_CALLBACK_INFORMATION );

	HMODULE dll = nullptr;
	PFN_SymInitialize SymInitialize = nullptr;
	PFN_SymSetOptions SymSetOptions = nullptr;
	PFN_StackWalk64 StackWalk64 = nullptr;
	PFN_SymFunctionTableAccess64 SymFunctionTableAccess64 = nullptr;
	PFN_SymGetModuleBase64 SymGetModuleBase64 = nullptr;
	PFN_MiniDumpWriteDump MiniDumpWriteDump = nullptr;
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

		// Independent of the symbol engine: a dump needs none of it, so it is
		// fetched before the check that can give up on the stack walk.
		MiniDumpWriteDump = (PFN_MiniDumpWriteDump)GetProcAddress( dll, "MiniDumpWriteDump" );

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

// Cached, both of them. SiblingPath hands back ONE static buffer, so two paths
// from it cannot be held at once -- and these are used from inside a crash,
// where another thread may be halfway through asking it for something else.
inline const wchar_t* CrashLogPath()
{
	static wchar_t path[MAX_PATH] = { 0 };
	if ( !path[0] )
		wcscpy_s( path, MAX_PATH, SiblingPath( L"sinvr_crash.log" ) );
	return path;
}

inline const wchar_t* CrashDumpPath()
{
	static wchar_t path[MAX_PATH] = { 0 };
	if ( !path[0] )
		wcscpy_s( path, MAX_PATH, SiblingPath( L"sinvr_crash.dmp" ) );
	return path;
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
		FlushFileBuffers( f );
		CloseHandle( f );
	}

	// Mirror into the main log so one file still tells the whole story.
	LogError( "%s", body );
}

//-----------------------------------------------------------------------------
// ---- WHAT WERE WE CALLING WHEN IT DIED? -----------------------------------
//
// Every call into the OpenVR runtime on the submit path sets this first and
// clears it after. A crash or stall that lands inside one names it outright,
// instead of leaving an offset into one particular build of sinvr.dll.
//-----------------------------------------------------------------------------
inline const char* volatile& ExternalCall()
{
	static const char* volatile call = nullptr;
	return call;
}

// Non-zero while that call sits inside a __try that will recover from a fault.
// The vectored handler reads it to tell "about to be caught" from "fatal".
inline volatile LONG& GuardedDepth()
{
	static volatile LONG depth = 0;
	return depth;
}

//-----------------------------------------------------------------------------
// ---- WHAT HAPPENED JUST BEFORE --------------------------------------------
//
// A crash report says WHERE. It cannot say what changed: the headset going to
// sleep, the dashboard, a device reset, a menu opening -- the events worth
// suspecting. Those are kept here, the last 48, and go out with any crash or
// stall report. State changes only; nothing per frame, or the ring would hold
// the last half second and nothing else.
//-----------------------------------------------------------------------------
constexpr int kCrumbCount = 48;
constexpr int kCrumbLen = 160;

struct CrumbRing
{
	char text[kCrumbCount][kCrumbLen];
	volatile LONG next;
};

inline CrumbRing& Crumbs()
{
	static CrumbRing ring = {};
	return ring;
}

inline void Breadcrumb( const char* fmt, ... )
{
	char body[kCrumbLen - 16];
	va_list args;
	va_start( args, fmt );
	_vsnprintf_s( body, sizeof( body ), _TRUNCATE, fmt, args );
	va_end( args );

	SYSTEMTIME st;
	GetLocalTime( &st );

	CrumbRing& ring = Crumbs();
	const LONG slot = ( InterlockedIncrement( &ring.next ) - 1 ) % kCrumbCount;
	_snprintf_s( ring.text[slot], kCrumbLen, _TRUNCATE, "[%02d:%02d:%02d.%03d] %s",
				 st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, body );
}

inline void DumpBreadcrumbs()
{
	CrumbRing& ring = Crumbs();
	const LONG total = ring.next;
	if ( total <= 0 )
	{
		CrashLog( "  (no events recorded)" );
		return;
	}
	const LONG count = ( total < kCrumbCount ) ? total : kCrumbCount;
	for ( LONG i = total - count; i < total; ++i )
		CrashLog( "  %s", ring.text[i % kCrumbCount] );
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

//-----------------------------------------------------------------------------
// ---- WHICH DRIVER, WHICH RUNTIME ------------------------------------------
//
// A fault inside nvoglv32.dll is a question about one NVIDIA driver release,
// and the report never said which. File versions of the modules on the VR
// path, read from the files the process actually loaded.
//-----------------------------------------------------------------------------
inline bool FileVersionOf( HMODULE mod, DWORD& ms, DWORD& ls )
{
	using PFN_Size = DWORD( WINAPI* )( LPCWSTR, LPDWORD );
	using PFN_Info = BOOL( WINAPI* )( LPCWSTR, DWORD, DWORD, LPVOID );
	using PFN_Query = BOOL( WINAPI* )( LPCVOID, LPCWSTR, LPVOID*, PUINT );

	// version.dll by hand, for the same zero-dependency reason as DbgHelp.
	static HMODULE ver = LoadLibraryA( "version.dll" );
	if ( !ver )
		return false;
	static PFN_Size size = (PFN_Size)GetProcAddress( ver, "GetFileVersionInfoSizeW" );
	static PFN_Info info = (PFN_Info)GetProcAddress( ver, "GetFileVersionInfoW" );
	static PFN_Query query = (PFN_Query)GetProcAddress( ver, "VerQueryValueW" );
	if ( !size || !info || !query )
		return false;

	wchar_t path[MAX_PATH] = { 0 };
	if ( !GetModuleFileNameW( mod, path, MAX_PATH ) )
		return false;

	DWORD handle = 0;
	const DWORD n = size( path, &handle );

	// A fixed buffer, not the heap: this also runs inside a crash, where the
	// heap may be the thing that broke.
	static unsigned char buf[64 * 1024];
	if ( n == 0 || n > sizeof( buf ) || !info( path, 0, n, buf ) )
		return false;

	VS_FIXEDFILEINFO* ffi = nullptr;
	UINT len = 0;
	if ( !query( buf, L"\\", (LPVOID*)&ffi, &len ) || !ffi || len < sizeof( *ffi ) )
		return false;

	ms = ffi->dwFileVersionMS;
	ls = ffi->dwFileVersionLS;
	return true;
}

// `sink` is Log for the startup listing and CrashLog inside a report.
inline void DescribeVRModules( void ( *sink )( const char*, ... ) )
{
	static const char* const names[] = {
		"nvoglv32.dll",          // NVIDIA's 32-bit Vulkan/GL driver
		"amdvlk32.dll",          // AMD's
		"igvk32.dll",            // Intel's
		"vulkan-1.dll",
		"vrclient.dll",          // SteamVR's in-process client
		"openvr_api.dll",
		"openvr_api_dxvk.dll",
		"d3d9.dll",              // our DXVK
		"sinvr.dll",
	};

	for ( const char* name : names )
	{
		HMODULE m = GetModuleHandleA( name );
		if ( !m )
			continue;

		char path[MAX_PATH] = { 0 };
		GetModuleFileNameA( m, path, MAX_PATH );

		// The link time, from the module's own PE header. sinvr.dll carries no
		// version resource, and this is what tells one build of it from another
		// in a report from someone else's machine -- the 2026-09-11 crash could
		// not be matched to a build at all.
		char linked[40] = "?";
		{
			const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)m;
			const IMAGE_NT_HEADERS* nt =
				(const IMAGE_NT_HEADERS*)( (const unsigned char*)m + dos->e_lfanew );
			const ULONGLONG t = (ULONGLONG)nt->FileHeader.TimeDateStamp * 10000000ull +
								116444736000000000ull;
			FILETIME ft = { (DWORD)t, (DWORD)( t >> 32 ) };
			SYSTEMTIME st = {};
			if ( FileTimeToSystemTime( &ft, &st ) )
				_snprintf_s( linked, sizeof( linked ), _TRUNCATE,
							 "%04u-%02u-%02u %02u:%02u UTC", st.wYear, st.wMonth, st.wDay,
							 st.wHour, st.wMinute );
		}

		DWORD ms = 0, ls = 0;
		if ( !FileVersionOf( m, ms, ls ) )
		{
			sink( "  %-20s linked %s  %s", name, linked, path );
			continue;
		}

		// NVIDIA's file version carries the driver release in its last five
		// digits: 32.0.15.6094 is driver 560.94.
		char release[40] = "";
		if ( _stricmp( name, "nvoglv32.dll" ) == 0 )
		{
			const unsigned int r = ( HIWORD( ls ) % 10 ) * 10000 + LOWORD( ls );
			_snprintf_s( release, sizeof( release ), _TRUNCATE, "  = NVIDIA driver %u.%02u",
						 r / 100, r % 100 );
		}

		sink( "  %-20s %u.%u.%u.%u%s  linked %s  %s", name, HIWORD( ms ), LOWORD( ms ),
			  HIWORD( ls ), LOWORD( ls ), release, linked, path );
	}
}

//-----------------------------------------------------------------------------
// StackWalk64 over a context, one CrashLog line per frame. Shared by the crash
// report (the faulting thread) and the stall report (a suspended one).
//-----------------------------------------------------------------------------
inline bool WalkStack( HANDLE thread, const CONTEXT& start, const char* indent, int maxFrames )
{
#ifdef _M_IX86
	if ( !DbgHelp().Load() )
		return false;

	CONTEXT walkCtx = start;
	STACKFRAME64 frame64 = {};
	frame64.AddrPC.Offset = start.Eip;
	frame64.AddrPC.Mode = AddrModeFlat;
	frame64.AddrFrame.Offset = start.Ebp;
	frame64.AddrFrame.Mode = AddrModeFlat;
	frame64.AddrStack.Offset = start.Esp;
	frame64.AddrStack.Mode = AddrModeFlat;

	bool walked = false;
	for ( int i = 0; i < maxFrames; ++i )
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
		CrashLog( "%s[%02d] %s", indent, i, frameWhere );
		walked = true;
	}
	return walked;
#else
	(void)thread;
	(void)start;
	(void)indent;
	(void)maxFrames;
	return false;
#endif
}

//-----------------------------------------------------------------------------
// A minidump, once per session, beside the exe. Opened in a debugger it shows
// every thread and the memory they point at -- which for a fault inside a
// graphics driver is the only view there is of what the driver was holding.
//-----------------------------------------------------------------------------
inline void WriteCrashDump( EXCEPTION_POINTERS* info )
{
	static LONG written = 0;
	if ( InterlockedExchange( &written, 1 ) != 0 )
		return;

	DbgHelp().Load();
	if ( !DbgHelp().MiniDumpWriteDump )
	{
		CrashLog( "  (dbghelp.dll unavailable -- no minidump)" );
		return;
	}

	HANDLE f = CreateFileW( CrashDumpPath(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
							FILE_ATTRIBUTE_NORMAL, NULL );
	if ( f == INVALID_HANDLE_VALUE )
	{
		CrashLog( "  (could not create sinvr_crash.dmp, err %lu)", GetLastError() );
		return;
	}

	MINIDUMP_EXCEPTION_INFORMATION mei = {};
	mei.ThreadId = GetCurrentThreadId();
	mei.ExceptionPointers = info;
	mei.ClientPointers = FALSE;

	// Referenced memory, not full memory: enough to follow the pointers the
	// faulting code held, at a few megabytes instead of the whole 4 GB space.
	const MINIDUMP_TYPE type = (MINIDUMP_TYPE)( MiniDumpWithIndirectlyReferencedMemory |
												MiniDumpWithThreadInfo |
												MiniDumpWithUnloadedModules );

	const BOOL ok = DbgHelp().MiniDumpWriteDump( GetCurrentProcess(), GetCurrentProcessId(),
												 f, type, info ? &mei : NULL, NULL, NULL );
	const DWORD err = ok ? 0 : GetLastError();
	CloseHandle( f );

	if ( ok )
		CrashLog( "  minidump written: sinvr_crash.dmp" );
	else
		CrashLog( "  minidump FAILED (err %lu)", err );
}

//-----------------------------------------------------------------------------
// The body of a report: what, where, registers, and the stack two ways.
//-----------------------------------------------------------------------------
inline void ReportException( EXCEPTION_POINTERS* info )
{
	const EXCEPTION_RECORD* rec = info->ExceptionRecord;

	char where[512];
	DescribeAddress( rec->ExceptionAddress, where, sizeof( where ) );
	CrashLog( "exception %s (0x%08lX) at %s, thread %lu",
			  ExceptionName( rec->ExceptionCode ), rec->ExceptionCode, where,
			  GetCurrentThreadId() );

	if ( rec->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && rec->NumberParameters >= 2 )
	{
		const char* op = rec->ExceptionInformation[0] == 0   ? "reading"
						 : rec->ExceptionInformation[0] == 1 ? "writing"
															 : "executing";
		CrashLog( "  %s address 0x%IX", op, rec->ExceptionInformation[1] );
	}

#ifdef _M_IX86
	const CONTEXT* c = info->ContextRecord;
	if ( !c )
		return;

	CrashLog( "  eax=%08lX ebx=%08lX ecx=%08lX edx=%08lX",
			  c->Eax, c->Ebx, c->Ecx, c->Edx );
	CrashLog( "  esi=%08lX edi=%08lX ebp=%08lX esp=%08lX eip=%08lX",
			  c->Esi, c->Edi, c->Ebp, c->Esp, c->Eip );

	// Walk the frame chain. Source is built with frame pointers, so this is
	// usually good for a readable trace without needing DbgHelp.
	CrashLog( "  --- frames (frame pointers) ---" );
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

	// And the unwind-data walk, which follows the frames the chain loses. The
	// 2026-09-11 report stopped dead after nine, at our own code.
	CrashLog( "  --- frames (StackWalk64) ---" );
	if ( !WalkStack( GetCurrentThread(), *c, "  ", 40 ) )
		CrashLog( "  (StackWalk64 unavailable)" );
#endif
}

inline LONG CALLBACK VectoredCrashHandler( EXCEPTION_POINTERS* info )
{
	if ( !info || !info->ExceptionRecord )
		return EXCEPTION_CONTINUE_SEARCH;

	const EXCEPTION_RECORD* rec = info->ExceptionRecord;

	// First-chance C++ exceptions and debugger breakpoints are normal traffic.
	if ( !IsFatalException( rec->ExceptionCode ) )
		return EXCEPTION_CONTINUE_SEARCH;

	const char* call = ExternalCall();

	// ---- A FAULT THE MOD IS ABOUT TO CATCH ----------------------------------
	//
	// Inside a guarded call into the runtime this is not the end of the
	// process: the __except around the call takes it next, and the backend
	// pauses submission. It still gets the full report -- it is the very thing
	// being hunted -- but it must not use up the one-shot latch below, or a
	// real crash later in the same session would go unreported.
	if ( GuardedDepth() > 0 )
	{
		static LONG guardedReports = 0;
		if ( InterlockedIncrement( &guardedReports ) > 3 )
			return EXCEPTION_CONTINUE_SEARCH;

		CrashLog( "=== FAULT inside %s -- GUARDED: caught, VR submission pauses ===",
				  call ? call : "a guarded OpenVR call" );
		ReportException( info );
		CrashLog( "  --- modules on the VR path ---" );
		DescribeVRModules( CrashLog );
		CrashLog( "  --- last events before it ---" );
		DumpBreadcrumbs();
		WriteCrashDump( info );
		CrashLog( "=== END FAULT ===" );
		return EXCEPTION_CONTINUE_SEARCH;
	}

	// Log once. A fault inside the fault path helps nobody.
	static LONG reported = 0;
	if ( InterlockedExchange( &reported, 1 ) != 0 )
		return EXCEPTION_CONTINUE_SEARCH;

	CrashLog( "=== CRASH ===" );
	CrashLog( "  (first-chance: if the game kept running after this, something "
			  "caught it -- the rest of sinvr.log will show it carrying on)" );
	if ( call )
		CrashLog( "  OpenVR call in progress: %s -- NOT guarded (vr_submit_guard = 0)", call );
	ReportException( info );
	CrashLog( "  --- modules on the VR path ---" );
	DescribeVRModules( CrashLog );
	CrashLog( "  --- last events before it ---" );
	DumpBreadcrumbs();
	WriteCrashDump( info );
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

		if ( !WalkStack( thread, ctx, "    ", 32 ) )
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

// One previous generation of a report, the way sinvr.log keeps sinvr.log.prev.
// True if there was a non-empty one to keep.
inline bool KeepPreviousFile( const wchar_t* current, const wchar_t* prevName )
{
	// Both copied out first: SiblingPath's buffer is shared.
	wchar_t cur[MAX_PATH];
	wcscpy_s( cur, MAX_PATH, current );
	wchar_t prev[MAX_PATH];
	wcscpy_s( prev, MAX_PATH, SiblingPath( prevName ) );

	WIN32_FILE_ATTRIBUTE_DATA fa = {};
	const bool had = GetFileAttributesExW( cur, GetFileExInfoStandard, &fa ) &&
					 ( fa.nFileSizeLow != 0 || fa.nFileSizeHigh != 0 );

	if ( !MoveFileExW( cur, prev, MOVEFILE_REPLACE_EXISTING ) )
		DeleteFileW( cur );
	return had;
}

inline void InstallCrashHandler()
{
	// KEPT, no longer deleted. The crash that prompted all of the above was
	// reported with a sinvr.log from a DIFFERENT run: the one that crashed had
	// already been replaced by the attempt to reproduce it.
	const bool hadReport = KeepPreviousFile( CrashLogPath(), L"sinvr_crash.log.prev" );
	const bool hadDump = KeepPreviousFile( CrashDumpPath(), L"sinvr_crash.dmp.prev" );

	AddVectoredExceptionHandler( 1 /* call first */, VectoredCrashHandler );
	Log( "crash handler installed -> sinvr_crash.log, plus sinvr_crash.dmp on a fault" );

	if ( hadReport )
		LogWarn( "crash handler: the PREVIOUS run left a crash or stall report -- kept as "
				 "sinvr_crash.log.prev%s. Send it along with sinvr.log.prev.",
				 hadDump ? " and sinvr_crash.dmp.prev" : "" );
}

} // namespace sinvr
