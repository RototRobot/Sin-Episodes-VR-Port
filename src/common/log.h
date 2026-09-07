// Minimal levelled file logger. Header-only so both the proxy and the mod DLL
// can use it without a shared static lib.
//
// The build machine and the headset machine are different machines, so this log
// is the primary diagnostic channel. Verbosity is set in sinvr.cfg rather than
// at compile time, so a more detailed run never requires a rebuild.
#pragma once

#include <windows.h>
#include <stdio.h>
#include <stdarg.h>

namespace sinvr {

enum LogLevel
{
	kLogError = 0,
	kLogWarn  = 1,
	kLogInfo  = 2,
	kLogDebug = 3,
	kLogTrace = 4,
};

inline LogLevel& LogThreshold()
{
	static LogLevel level = kLogInfo;
	return level;
}

inline void SetLogLevel( int level )
{
	if ( level < kLogError ) level = kLogError;
	if ( level > kLogTrace ) level = kLogTrace;
	LogThreshold() = (LogLevel)level;
}

// Log lands next to SinEpisodes.exe so it is easy to find on the test rig.
inline const wchar_t* LogPath()
{
	static wchar_t path[MAX_PATH] = { 0 };
	if ( path[0] == 0 )
	{
		GetModuleFileNameW( NULL, path, MAX_PATH );
		wchar_t* slash = wcsrchr( path, L'\\' );
		if ( slash )
			wcscpy_s( slash + 1, MAX_PATH - ( slash + 1 - path ), L"sinvr.log" );
	}
	return path;
}

// Path to a sibling file of the log (config, dumps).
inline const wchar_t* SiblingPath( const wchar_t* filename )
{
	static wchar_t path[MAX_PATH];
	wcscpy_s( path, MAX_PATH, LogPath() );
	wchar_t* slash = wcsrchr( path, L'\\' );
	if ( slash )
		wcscpy_s( slash + 1, MAX_PATH - ( slash + 1 - path ), filename );
	return path;
}

// `flush` forces the line all the way to disk rather than leaving it in the OS
// file cache.
//
// This is not optional paranoia. A machine that bugchecks loses the cache
// entirely, and NTFS then recovers the file with the unwritten tail padded out
// with spaces -- which reads exactly like "the mod stopped here". A whole
// crash run was lost that way, including whether the settings under test were
// even in force. The cost is one flush per line; the heartbeat is every five
// seconds, so it is irrelevant except on the per-frame trace, which is excluded.
// ---- LINES WERE BEING LOST, SILENTLY --------------------------------------
//
// This opened the file with FILE_SHARE_READ and no lock. Two threads logging at
// once meant the second CreateFileW failed with a sharing violation and the
// function RETURNED, discarding the line with nothing to say it had.
//
// It is not rare and it is not harmless. The render thread logs while init
// still is, and an interface dump of ten entries came back as five on one run
// and three on the next -- each time looking exactly like a short list rather
// than a lost one. Two test runs were spent deciding whether server.dll really
// exposed five interfaces. Every measurement this log has ever carried was
// exposed to the same loss.
//
// A diagnostic that drops data under load, without saying so, is worse than no
// diagnostic: it does not fail, it MISLEADS.
inline CRITICAL_SECTION& LogLock()
{
	// Function-local static: initialised exactly once, thread-safely, on first
	// use -- which may be from any thread.
	static CRITICAL_SECTION cs = [] {
		CRITICAL_SECTION c;
		InitializeCriticalSection( &c );
		return c;
	}();
	return cs;
}

// Lines lost anyway -- to something outside this process holding the file.
// Counted rather than ignored, so a future loss is VISIBLE instead of being
// mistaken for a short list again.
inline unsigned int& LogDropped()
{
	static unsigned int dropped = 0;
	return dropped;
}

inline void LogRaw( const char* text, bool flush = true )
{
	EnterCriticalSection( &LogLock() );

	// The lock removes contention from THIS process. The retry covers anything
	// else holding the file -- a tail viewer, an editor, an antivirus scan --
	// which the lock cannot help with.
	HANDLE f = INVALID_HANDLE_VALUE;
	for ( int attempt = 0; attempt < 5; ++attempt )
	{
		f = CreateFileW( LogPath(), FILE_APPEND_DATA, FILE_SHARE_READ, NULL,
						 OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL );
		if ( f != INVALID_HANDLE_VALUE )
			break;
		Sleep( 1 );
	}

	if ( f == INVALID_HANDLE_VALUE )
	{
		++LogDropped();
		LeaveCriticalSection( &LogLock() );
		return;
	}

	DWORD written = 0;
	WriteFile( f, text, (DWORD)strlen( text ), &written, NULL );

	if ( flush )
		FlushFileBuffers( f );

	CloseHandle( f );
	LeaveCriticalSection( &LogLock() );
}

inline const char* LevelTag( LogLevel level )
{
	switch ( level )
	{
		case kLogError: return "ERR ";
		case kLogWarn:  return "WARN";
		case kLogDebug: return "DBG ";
		case kLogTrace: return "TRC ";
		default:        return "INFO";
	}
}

inline void LogAtV( LogLevel level, const char* fmt, va_list args )
{
	if ( level > LogThreshold() )
		return;

	char body[2048];
	_vsnprintf_s( body, sizeof( body ), _TRUNCATE, fmt, args );

	SYSTEMTIME st;
	GetLocalTime( &st );

	char line[2176];
	_snprintf_s( line, sizeof( line ), _TRUNCATE, "[%02d:%02d:%02d.%03d] %s %s\r\n",
				 st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
				 LevelTag( level ), body );

	// Everything except the per-frame trace goes straight to disk, so a hard
	// machine crash still leaves us the run that caused it.
	LogRaw( line, level != kLogTrace );
}

inline void LogAt( LogLevel level, const char* fmt, ... )
{
	if ( level > LogThreshold() )
		return;
	va_list args;
	va_start( args, fmt );
	LogAtV( level, fmt, args );
	va_end( args );
}

// Log() stays INFO so existing call sites keep working.
inline void Log( const char* fmt, ... )
{
	va_list args;
	va_start( args, fmt );
	LogAtV( kLogInfo, fmt, args );
	va_end( args );
}

inline void LogError( const char* fmt, ... )
{
	va_list args;
	va_start( args, fmt );
	LogAtV( kLogError, fmt, args );
	va_end( args );
}

inline void LogWarn( const char* fmt, ... )
{
	va_list args;
	va_start( args, fmt );
	LogAtV( kLogWarn, fmt, args );
	va_end( args );
}

inline void LogDebug( const char* fmt, ... )
{
	va_list args;
	va_start( args, fmt );
	LogAtV( kLogDebug, fmt, args );
	va_end( args );
}

inline void LogTrace( const char* fmt, ... )
{
	va_list args;
	va_start( args, fmt );
	LogAtV( kLogTrace, fmt, args );
	va_end( args );
}

// Starts a fresh log, KEEPING the previous run as `sinvr.log.prev`.
//
// This used to DeleteFileW the log outright, and that quietly cost a real
// investigation: a soft lock on the intro car sequence was hit once, worked on
// the retry, and by the time it was reported the only run that contained it had
// already been destroyed by the next launch. The bugs worth chasing here are
// exactly the intermittent ones, and those are always reported one run late.
//
// One generation is enough and is the point: two files, no growth, and the
// reproduction attempt never overwrites the evidence of the failure. Same
// idiom the config already uses for `sinvr.cfg.old`.
//
// MOVEFILE_REPLACE_EXISTING because the previous .prev must give way; if the
// rename fails for any reason the delete still runs, so a locked or missing
// file cannot stop the mod from starting.
inline void LogReset( const char* banner )
{
	const wchar_t* prev = SiblingPath( L"sinvr.log.prev" );
	if ( !MoveFileExW( LogPath(), prev, MOVEFILE_REPLACE_EXISTING ) )
		DeleteFileW( LogPath() );
	Log( "%s", banner );
}

} // namespace sinvr
