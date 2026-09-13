// sinvr_launcher.exe -- starts SiN Episodes with a 4 GB address space.
//
// ---- WHY -------------------------------------------------------------------
//
// SinEpisodes.exe ships without IMAGE_FILE_LARGE_ADDRESS_AWARE (verified:
// characteristics 0x010E, the 0x0020 bit clear), so Windows caps it at 2 GB of
// user-mode virtual address space. That is normally plenty for a 2006 game and
// nowhere near enough once DXVK is underneath it: DXVK's shader cache, staging
// buffers and our four full-size eye render targets all live in the same 2 GB.
//
// The failure is not a clean allocation error. The space FRAGMENTS, so a request
// for a few contiguous MB fails while hundreds of MB are still free in total,
// and the crash lands in whichever subsystem happened to ask next -- which is
// why it reads as a rendering or audio bug with nothing memory-shaped about it.
// This mod has already seen the near end of that ramp once, as
// "D3D9: Reporting out of memory from tracking".
//
// The kernel reads that PE flag at process CREATION. There is no runtime
// equivalent -- by the time dinput8.dll pulls the mod in, the image is long
// since mapped and the limit is fixed. So it has to happen at launch, which is
// what this program is for.
//
// ---- HOW -------------------------------------------------------------------
//
// Copy SinEpisodes.exe -> SinEpisodes_laa.exe, set the bit on the COPY, and
// launch that. The shipped executable is opened read-only and never written, so
// no crash or power cut can damage the installation and Steam's "verify
// integrity of game files" has nothing to revert.
//
// The technique is Praydog's, from FEAR2VR. His version is much larger than this
// one because FEAR2.exe is SteamStub-wrapped: the stub validates the image it was
// started from, so the launcher must start the copy suspended, patch a stub over
// the entry point, and rewrite PEB->Ldr, ProcessParameters->ImagePathName and the
// mapped PE characteristics back to the original's before the stub ever runs.
//
// None of that is needed here, and it was checked rather than assumed:
// SinEpisodes.exe has ordinary sections (.text/.rdata/.data/.rsrc/.reloc) with no
// .bind, no SteamStub or CEG markers, imports only KERNEL32 and USER32, and is a
// stock Source launcher_main.cpp bootstrap that sets PATH and loads bin\launcher.dll.
// There is nothing in it that validates its own image, so the copy just runs.
//
// Nor does this need to inject anything. engine.dll statically imports
// DirectInput8Create, so our dinput8.dll proxy is loaded from the exe directory
// by the normal search order and pulls in sinvr.dll itself -- and the copy sits
// in that same directory. The mod derives sinvr.log and sinvr.cfg from the exe's
// DIRECTORY, never its filename, so the rename is invisible to it.

// PROC_THREAD_ATTRIBUTE_PARENT_PROCESS and the attribute-list calls are Vista+.
// Stated rather than inherited from whatever the installed SDK happens to
// default to, since the rest of this project is deliberately pinned to an old
// target and a silent default is the kind of thing that changes under you.
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif

#include <windows.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <ctype.h>
#include <wctype.h>
#include <string>
#include <vector>
#include <fstream>
#include <filesystem>

// Interface definitions only. openvr_api.dll is loaded with LoadLibrary and its
// entry points resolved with GetProcAddress, exactly as the mod does it -- the
// launcher must still work on a machine with no runtime installed, and a
// load-time dependency would stop it starting at all.
#include <openvr.h>

namespace fs = std::filesystem;

namespace {

constexpr const char* kAppId = "1300";
constexpr const wchar_t* kExeName = L"SinEpisodes.exe";
constexpr const wchar_t* kCopyName = L"SinEpisodes_laa.exe";

// The five files the mod needs beside the exe. openvr_api_dxvk.dll is the
// unobvious one and the reason this list is checked at all: DXVK only uses
// openvr_api.dll if it is ALREADY loaded when the Vulkan instance is created,
// which it never is that early, so openvr_api_dxvk.dll is the fallback name it
// loads itself. Without it DXVK never asks OpenVR which instance extensions the
// compositor needs and submission fails -- with no error that points here.
const wchar_t* const kRequired[] = {
	L"d3d9.dll", L"sinvr.dll", L"dinput8.dll",
	L"openvr_api.dll", L"openvr_api_dxvk.dll",
};

void Say( const char* fmt, ... )
{
	va_list args;
	va_start( args, fmt );
	printf( "[laa] " );
	vprintf( fmt, args );
	printf( "\n" );
	va_end( args );
	fflush( stdout );
}

std::wstring RegString( HKEY root, const wchar_t* path, const wchar_t* name )
{
	wchar_t buf[512] = { 0 };
	DWORD cb = sizeof( buf );
	if ( RegGetValueW( root, path, name, RRF_RT_REG_SZ, nullptr, buf, &cb ) != ERROR_SUCCESS )
		return std::wstring();
	return buf;
}

DWORD RegDword( HKEY root, const wchar_t* path, const wchar_t* name )
{
	DWORD v = 0, cb = sizeof( v );
	if ( RegGetValueW( root, path, name, RRF_RT_REG_DWORD, nullptr, &v, &cb ) != ERROR_SUCCESS )
		return 0;
	return v;
}

std::string ReadWholeFile( const fs::path& p )
{
	std::ifstream in( p, std::ios::binary );
	if ( !in )
		return std::string();
	return std::string( ( std::istreambuf_iterator<char>( in ) ),
						std::istreambuf_iterator<char>() );
}

// The throwing fs::exists is wrong for anything derived from argv or from a VDF
// we did not write: "file not found" returns false, but a malformed path or a
// permission failure throws, and a launcher that dies with an unhandled
// filesystem_error before printing anything is the worst possible diagnostic.
bool Exists( const fs::path& p )
{
	std::error_code ec;
	return fs::exists( p, ec ) && !ec;
}

//-----------------------------------------------------------------------------
// PE inspection. Enough of the header to answer three questions: is it 32-bit,
// does it already have the flag, and where is the field.
struct PeInfo
{
	bool valid = false;
	uint16_t machine = 0;
	uint16_t characteristics = 0;
	uint32_t charsOffset = 0;   // file offset of the Characteristics field
	bool largeAddressAware = false;
};

PeInfo InspectPe( const fs::path& p )
{
	PeInfo info;

	std::ifstream f( p, std::ios::binary );
	if ( !f )
		return info;

	uint16_t mz = 0;
	f.read( reinterpret_cast<char*>( &mz ), 2 );
	if ( mz != 0x5A4D )   // "MZ"
		return info;

	uint32_t peOff = 0;
	f.seekg( 0x3C );
	f.read( reinterpret_cast<char*>( &peOff ), 4 );

	char sig[4] = { 0 };
	f.seekg( peOff );
	f.read( sig, 4 );
	if ( memcmp( sig, "PE\0\0", 4 ) != 0 )
		return info;

	// COFF header follows the 4-byte signature: Machine at +0, then
	// NumberOfSections, TimeDateStamp, PointerToSymbolTable, NumberOfSymbols,
	// SizeOfOptionalHeader, and Characteristics at +18 -- so +22 from the sig.
	f.seekg( peOff + 4 );
	f.read( reinterpret_cast<char*>( &info.machine ), 2 );

	info.charsOffset = peOff + 22;
	f.seekg( info.charsOffset );
	f.read( reinterpret_cast<char*>( &info.characteristics ), 2 );

	if ( !f )
		return info;

	info.largeAddressAware = ( info.characteristics & 0x0020 ) != 0;
	info.valid = true;
	return info;
}

// Writes the copy, sets the bit on it, and reads it back. The source is opened
// read-only and never written.
bool MakeLaaCopy( const fs::path& src, const fs::path& dst, const PeInfo& info )
{
	std::error_code ec;

	// A copy left over from a previous run is stale by definition -- the game
	// may have been updated since. If it cannot be removed the most likely
	// reason is that it is still running.
	if ( Exists( dst ) )
	{
		fs::remove( dst, ec );
		if ( ec )
		{
			Say( "cannot replace %S -- is a previous session still running?",
				 dst.filename().c_str() );
			return false;
		}
	}

	fs::copy_file( src, dst, fs::copy_options::overwrite_existing, ec );
	if ( ec )
	{
		Say( "could not copy the exe: %s", ec.message().c_str() );
		return false;
	}

	{
		std::fstream f( dst, std::ios::in | std::ios::out | std::ios::binary );
		if ( !f )
		{
			Say( "could not open the copy for writing" );
			return false;
		}
		uint16_t chars = (uint16_t)( info.characteristics | 0x0020 );
		f.seekp( info.charsOffset );
		f.write( reinterpret_cast<const char*>( &chars ), 2 );
		f.flush();
		if ( !f )
		{
			Say( "could not write the LARGE_ADDRESS_AWARE bit" );
			return false;
		}
	}

	// Read it back rather than trusting the write. This is the whole point of
	// the exercise; if the bit is not there the launch is pointless and we would
	// rather say so than start a 2 GB session that looks like a 4 GB one.
	const PeInfo after = InspectPe( dst );
	if ( !after.valid || !after.largeAddressAware )
	{
		Say( "the copy did not come back LARGE_ADDRESS_AWARE (characteristics 0x%04X)",
			 after.characteristics );
		return false;
	}

	Say( "copy ready: %S  characteristics 0x%04X -> 0x%04X",
		 dst.filename().c_str(), info.characteristics, after.characteristics );
	return true;
}

//-----------------------------------------------------------------------------
// Where the game is, without asking Steam to start it.
fs::path FindGameExe( const fs::path& selfDir )
{
	// 1. Beside this launcher. The normal case: it is deployed into the game
	//    root along with the mod DLLs.
	if ( Exists( selfDir / kExeName ) )
		return selfDir / kExeName;

	// 2. Ask Steam. It records which library holds each app, so this is a
	//    lookup rather than a search of every drive.
	std::wstring steam = RegString( HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath" );
	for ( size_t i = 0; i < steam.size(); ++i )
		if ( steam[i] == L'/' )
			steam[i] = L'\\';
	if ( steam.empty() )
		return fs::path();

	std::vector<fs::path> libraries;
	libraries.push_back( fs::path( steam ) );

	const std::string vdf = ReadWholeFile( fs::path( steam ) / "steamapps" / "libraryfolders.vdf" );
	for ( size_t at = vdf.find( "\"path\"" ); at != std::string::npos;
		  at = vdf.find( "\"path\"", at + 1 ) )
	{
		const size_t q1 = vdf.find( '"', at + 6 );
		const size_t q2 = ( q1 == std::string::npos ) ? q1 : vdf.find( '"', q1 + 1 );
		if ( q2 == std::string::npos )
			break;

		const std::string one = vdf.substr( q1 + 1, q2 - q1 - 1 );
		// The VDF escapes backslashes; unescape before using it as a path.
		std::string clean;
		for ( size_t i = 0; i < one.size(); ++i )
		{
			if ( one[i] == '\\' && i + 1 < one.size() && one[i + 1] == '\\' )
				++i;
			clean.push_back( one[i] );
		}
		libraries.push_back( fs::path( clean ) );
	}

	for ( size_t i = 0; i < libraries.size(); ++i )
	{
		const std::string manifest = ReadWholeFile(
			libraries[i] / "steamapps" / ( std::string( "appmanifest_" ) + kAppId + ".acf" ) );
		if ( manifest.empty() )
			continue;

		const size_t at = manifest.find( "\"installdir\"" );
		if ( at == std::string::npos )
			continue;
		const size_t q1 = manifest.find( '"', at + 12 );
		const size_t q2 = ( q1 == std::string::npos ) ? q1 : manifest.find( '"', q1 + 1 );
		if ( q2 == std::string::npos )
			continue;

		const std::string dir = manifest.substr( q1 + 1, q2 - q1 - 1 );
		const fs::path candidate = libraries[i] / "steamapps" / "common" / dir / kExeName;
		if ( Exists( candidate ) )
			return candidate;
	}

	return fs::path();
}

//-----------------------------------------------------------------------------
// Steam's launch context, derived from the registry rather than observed. The
// game is a Steamworks title and expects to be told which app it is.
struct SteamContext
{
	std::wstring environment;   // packed, double-null terminated
	std::wstring launchOptions;
	bool ok = false;
};

SteamContext DeriveSteamContext()
{
	SteamContext c;

	std::wstring steamPath = RegString( HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath" );
	for ( size_t i = 0; i < steamPath.size(); ++i )
		if ( steamPath[i] == L'/' )
			steamPath[i] = L'\\';

	const std::wstring user =
		RegString( HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"AutoLoginUser" );
	const DWORD active =
		RegDword( HKEY_CURRENT_USER, L"Software\\Valve\\Steam\\ActiveProcess", L"ActiveUser" );

	if ( steamPath.empty() || active == 0 )
		return c;

	// 32-bit account id -> SteamID64.
	const unsigned long long steamId = (unsigned long long)active + 76561197960265728ull;

	// The player's own Steam launch options, so -novid -windowed and anything
	// else set in the Steam UI still applies when launched this way.
	{
		const fs::path lc = fs::path( steamPath ) / "userdata" / std::to_wstring( active ) /
							"config" / "localconfig.vdf";
		const std::string all = ReadWholeFile( lc );
		const std::string key = std::string( "\"" ) + kAppId + "\"";
		for ( size_t at = all.find( key ); at != std::string::npos; at = all.find( key, at + 1 ) )
		{
			const std::string seg = all.substr( at, 1500 );
			const size_t lo = seg.find( "\"LaunchOptions\"" );
			if ( lo == std::string::npos )
				continue;
			const size_t q1 = seg.find( '"', lo + 15 );
			if ( q1 == std::string::npos )
				break;

			// ---- VDF ESCAPES A QUOTE AS \" AND THIS USED TO STOP AT IT -----
			//
			// Launch options that route through this launcher contain a quoted
			// path, so the stored value begins:
			//
			//     "\"C:\\...\\sinvr_launcher.exe\" %command% -novid -w 1800 ..."
			//
			// Scanning for a bare '"' therefore ended on the FIRST escaped one
			// and yielded a single backslash. Two things went wrong quietly:
			// that '\' was appended to the game's command line as a stray
			// argument, and the `%command%` self-reference check below never
			// matched, so the guard it exists to provide was not there.
			//
			// Unescaped as it is scanned, so the value is what Steam shows in
			// the launch-options box.
			std::string s;
			size_t q2 = std::string::npos;
			for ( size_t i = q1 + 1; i < seg.size(); ++i )
			{
				if ( seg[i] == '\\' && i + 1 < seg.size() )
				{
					const char n = seg[i + 1];
					// \" and \\ are the two VDF actually emits; anything else
					// keeps its backslash so a path is not silently altered.
					if ( n == '"' || n == '\\' )
					{
						s.push_back( n );
						++i;
						continue;
					}
					s.push_back( seg[i] );
					continue;
				}
				if ( seg[i] == '"' )
				{
					q2 = i;
					break;
				}
				s.push_back( seg[i] );
			}
			if ( q2 == std::string::npos )
				break;

			// Steam's %command% substitution lets the Play button route through
			// this launcher, by setting the launch options to something like
			//     "...\sinvr_launcher.exe" %command% -novid
			// In that case Steam has ALREADY expanded them into our argv, and
			// re-reading them here would append the launcher's own path to the
			// game's command line and hand it back its own launch string. Detect
			// our own name and take the argv instead.
			std::string lower;
			for ( size_t i = 0; i < s.size(); ++i )
				lower.push_back( (char)tolower( (unsigned char)s[i] ) );
			if ( lower.find( "sinvr_launcher" ) != std::string::npos ||
				 lower.find( "%command%" ) != std::string::npos )
				break;

			c.launchOptions.assign( s.begin(), s.end() );
			break;
		}
	}

	std::wstring env;
	wchar_t* block = GetEnvironmentStringsW();
	for ( wchar_t* p = block; p != nullptr && *p != L'\0'; )
	{
		const std::wstring one( p );
		// Ours win; drop inherited Steam vars so they cannot conflict.
		if ( one.rfind( L"Steam", 0 ) != 0 && one.rfind( L"STEAMID=", 0 ) != 0 )
		{
			env += one;
			env.push_back( L'\0' );
		}
		p += one.size() + 1;
	}
	if ( block != nullptr )
		FreeEnvironmentStringsW( block );

	const std::wstring appid( kAppId, kAppId + strlen( kAppId ) );
	const std::wstring vars[] = {
		L"SteamAppId=" + appid,
		L"SteamGameId=" + appid,
		L"SteamOverlayGameId=" + appid,
		std::wstring( L"SteamClientLaunch=1" ),
		std::wstring( L"SteamEnv=1" ),
		L"SteamPath=" + steamPath,
		L"SteamAppUser=" + user,
		L"SteamUser=" + user,
		L"STEAMID=" + std::to_wstring( steamId ),
	};
	for ( size_t i = 0; i < sizeof( vars ) / sizeof( vars[0] ); ++i )
	{
		env += vars[i];
		env.push_back( L'\0' );
	}
	env.push_back( L'\0' );

	c.environment = env;
	c.ok = !user.empty();
	return c;
}

DWORD FindPid( const wchar_t* imageName )
{
	DWORD pid = 0;
	HANDLE snap = CreateToolhelp32Snapshot( TH32CS_SNAPPROCESS, 0 );
	if ( snap == INVALID_HANDLE_VALUE )
		return 0;

	PROCESSENTRY32W pe = { sizeof( pe ) };
	if ( Process32FirstW( snap, &pe ) )
	{
		do
		{
			if ( _wcsicmp( pe.szExeFile, imageName ) == 0 )
			{
				pid = pe.th32ProcessID;
				break;
			}
		} while ( Process32NextW( snap, &pe ) );
	}
	CloseHandle( snap );
	return pid;
}

//-----------------------------------------------------------------------------
// Did it actually take? Reserve a page above the 2 GB line in the child. That
// address only exists if the loader granted the larger space, so this is a
// functional proof rather than a restatement of what we wrote into the header.
bool VerifyLargeAddressSpace( HANDLE process )
{
	void* high = VirtualAllocEx( process, (LPVOID)(uintptr_t)0xA0000000u, 0x1000,
								 MEM_RESERVE, PAGE_NOACCESS );
	if ( high == nullptr )
	{
		// Something may already own that exact address; retry letting the kernel
		// choose the highest it can, and check where it landed.
		high = VirtualAllocEx( process, nullptr, 0x1000, MEM_RESERVE | MEM_TOP_DOWN,
							   PAGE_NOACCESS );
		if ( high == nullptr )
			return false;

		const bool aboveLine = ( (uintptr_t)high > 0x80000000u );
		VirtualFreeEx( process, high, 0, MEM_RELEASE );
		return aboveLine;
	}

	VirtualFreeEx( process, high, 0, MEM_RELEASE );
	return true;
}

} // namespace

//-----------------------------------------------------------------------------
// MATCHING THE HEADSET'S RESOLUTION, WITHOUT ANYONE TYPING IT IN
//
// The game renders the scene once per eye into its own backbuffer, and the eye
// surfaces are created to match that backbuffer exactly -- so the GAME WINDOW
// SIZE *IS* the per-eye render resolution. There is no separate knob for it
// inside the mod, and there deliberately is not one: a mismatch would mean
// rescaling every frame.
//
// That left the resolution as two hand-typed numbers in Steam's launch options,
// correct for exactly one headset. This asks the runtime instead.
//
// ---- WHY THE ASPECT MATTERS MORE THAN THE SIZE ------------------------------
//
// ComputeEyeFrustum EXTENDS the rendered frustum to cover the render target's
// aspect and then crops back with per-eye bounds, so a mismatched aspect is
// still geometrically exact -- it just throws away rendered pixels. Getting the
// aspect from the runtime therefore does not merely tidy the setup, it stops the
// GPU rendering pixels that are cropped away before submission.
//
// ---- WHY THE LAUNCHER, AND NOT THE MOD --------------------------------------
//
// The mod could call mat_setvideomode once it is running, but that resets the
// D3D9 device mid-session, throws away and rebuilds the eye surfaces, and the
// player watches the game start at the wrong size and change. The launcher runs
// before any of that exists and the game is simply born correct.
//
// Initialised as VRApplication_Other: this is a query, not a scene, and it must
// not take the compositor's focus from the game that is about to start. Shut
// down again before the game launches so there is no second client holding the
// runtime.
struct HeadsetRes
{
	unsigned int width = 0;
	unsigned int height = 0;
	bool ok = false;
};

HeadsetRes QueryHeadsetResolution( const fs::path& gameDir )
{
	using PFN_InitInternal2 = uint32_t( __stdcall* )( vr::EVRInitError*,
													  vr::EVRApplicationType,
													  const char* );
	using PFN_ShutdownInternal = void( __stdcall* )();
	using PFN_GetGenericInterface = void*( __stdcall* )( const char*, vr::EVRInitError* );
	using PFN_IsHmdPresent = bool( __stdcall* )();

	HeadsetRes out;

	// Prefer the copy shipped beside the game, for the same reason the mod does:
	// whichever openvr_api.dll the mod will bind is the one whose opinion counts.
	HMODULE dll = nullptr;
	{
		const fs::path local = gameDir / L"openvr_api.dll";
		if ( Exists( local ) )
			dll = LoadLibraryW( local.c_str() );
	}
	if ( dll == nullptr )
		dll = LoadLibraryW( L"openvr_api.dll" );
	if ( dll == nullptr )
	{
		Say( "openvr_api.dll not found -- leaving the resolution alone" );
		return out;
	}

	auto initInternal2 = (PFN_InitInternal2)GetProcAddress( dll, "VR_InitInternal2" );
	auto shutdownInternal = (PFN_ShutdownInternal)GetProcAddress( dll, "VR_ShutdownInternal" );
	auto getInterface = (PFN_GetGenericInterface)GetProcAddress( dll, "VR_GetGenericInterface" );
	auto isHmdPresent = (PFN_IsHmdPresent)GetProcAddress( dll, "VR_IsHmdPresent" );

	if ( !initInternal2 || !shutdownInternal || !getInterface )
	{
		Say( "openvr_api.dll is missing expected exports -- leaving the resolution alone" );
		return out;
	}
	if ( isHmdPresent && !isHmdPresent() )
	{
		Say( "no HMD detected -- leaving the resolution alone" );
		return out;
	}

	vr::EVRInitError err = vr::VRInitError_None;
	initInternal2( &err, vr::VRApplication_Other, nullptr );
	if ( err != vr::VRInitError_None )
	{
		Say( "could not reach the OpenVR runtime (EVRInitError %d) -- leaving the "
			 "resolution alone", (int)err );
		return out;
	}

	auto* system = (vr::IVRSystem*)getInterface( vr::IVRSystem_Version, &err );
	if ( system != nullptr && err == vr::VRInitError_None )
	{
		uint32_t w = 0, h = 0;
		system->GetRecommendedRenderTargetSize( &w, &h );
		if ( w > 0 && h > 0 )
		{
			out.width = w;
			out.height = h;
			out.ok = true;
		}
	}
	else
	{
		Say( "%s unavailable (EVRInitError %d) -- leaving the resolution alone",
			 vr::IVRSystem_Version, (int)err );
	}

	shutdownInternal();
	return out;
}

// Append one "NAME=VALUE" to a packed, double-null-terminated environment block.
//
// The block ends with the last entry's terminator followed by the block's own,
// so the trailing one is removed, the entry appended with its terminator, and
// the block terminator restored.
void AppendEnvVar( std::wstring& block, const std::wstring& entry )
{
	if ( !block.empty() && block.back() == L'\0' )
		block.pop_back();
	block += entry;
	block.push_back( L'\0' );
	block.push_back( L'\0' );
}

// One key out of sinvr.cfg, without dragging the mod's whole config module in.
//
// The mod OWNS that file -- it regenerates it when keys are missing -- so the
// launcher only ever reads. A `key = value` line, '#' comments, whitespace
// anywhere.
bool ReadCfgFloat( const fs::path& cfg, const char* key, float& out )
{
	std::ifstream f( cfg );
	if ( !f )
		return false;

	std::string line;
	const std::string want( key );
	while ( std::getline( f, line ) )
	{
		const size_t hash = line.find( '#' );
		if ( hash != std::string::npos )
			line.erase( hash );

		const size_t eq = line.find( '=' );
		if ( eq == std::string::npos )
			continue;

		std::string k = line.substr( 0, eq );
		std::string v = line.substr( eq + 1 );
		auto trim = []( std::string& t ) {
			while ( !t.empty() && isspace( (unsigned char)t.front() ) ) t.erase( t.begin() );
			while ( !t.empty() && isspace( (unsigned char)t.back() ) ) t.pop_back();
		};
		trim( k );
		trim( v );
		if ( k != want || v.empty() )
			continue;

		out = (float)atof( v.c_str() );
		return true;
	}
	return false;
}

// The desktop the game's window has to fit inside, in PHYSICAL pixels.
//
// ---- WHY NOT GetSystemMetrics ----------------------------------------------
//
// On a scaled display those two disagree, and only one of them is the limit.
// Measured on the development rig -- a 4K monitor at 150%:
//
//     GetDeviceCaps( DESKTOPHORZRES/DESKTOPVERTRES )   3840x2160   <- the truth
//     GetSystemMetrics( SM_CXSCREEN/SM_CYSCREEN )      2560x1440   <- scaled
//
// The engine's mode check is against the physical desktop, and the evidence is
// that a 2124-tall window worked while a 2388-tall one produced
// "Failed to set video mode - resetting to defaults":
//
//     2124 <= 2160   accepted
//     2388 >  2160   refused
//
// Clamping to the scaled 2560x1440 instead would have thrown away a third of the
// available resolution for no reason. DESKTOPVERTRES reports the real mode
// whatever the process's DPI awareness, which is exactly what is wanted here.
void PhysicalDesktopSize( long& w, long& h )
{
	w = 0;
	h = 0;

	HDC dc = GetDC( nullptr );
	if ( dc != nullptr )
	{
		// DESKTOPHORZRES / DESKTOPVERTRES. Not in every SDK header under those
		// names, so the indices are used directly and named here.
		w = GetDeviceCaps( dc, 118 );
		h = GetDeviceCaps( dc, 117 );
		ReleaseDC( nullptr, dc );
	}

	// Fall back to the scaled metrics rather than to nothing: too small is a
	// working game, and zero would disable the clamp entirely.
	if ( w <= 0 || h <= 0 )
	{
		w = GetSystemMetrics( SM_CXSCREEN );
		h = GetSystemMetrics( SM_CYSCREEN );
	}
}

// Does a command line already carry one of these switches?
bool HasSwitch( const std::wstring& cmd, const wchar_t* sw )
{
	std::wstring hay;
	for ( wchar_t c : cmd )
		hay.push_back( (wchar_t)towlower( c ) );
	std::wstring needle( sw );

	size_t at = hay.find( needle );
	while ( at != std::wstring::npos )
	{
		// Must start a token, and end at one -- so "-w" does not match "-window"
		// and "-h" does not match "-height_something".
		const bool startOk = ( at == 0 ) || iswspace( hay[at - 1] );
		const size_t end = at + needle.size();
		const bool endOk = ( end >= hay.size() ) || iswspace( hay[end] );
		if ( startOk && endOk )
			return true;
		at = hay.find( needle, at + 1 );
	}
	return false;
}

int wmain( int argc, wchar_t** argv )
{
	// Double-clicked rather than run from a terminal: keep the window up at the
	// end so whatever went wrong is readable. GetConsoleProcessList reporting
	// exactly one attached process means the console was created for us.
	bool ownConsole = false;
	{
		DWORD pids[4] = { 0 };
		ownConsole = ( GetConsoleProcessList( pids, 4 ) == 1 );
	}

	printf( "SiN VR launcher -- starts the game with a 4 GB address space\n\n" );

	wchar_t selfPath[MAX_PATH] = { 0 };
	GetModuleFileNameW( nullptr, selfPath, MAX_PATH );
	const fs::path selfDir = fs::path( selfPath ).parent_path();

	// An explicit path wins, for an install this cannot find on its own.
	fs::path exe;
	std::wstring extraArgs;
	for ( int i = 1; i < argc; ++i )
	{
		const std::wstring a = argv[i];
		if ( a == L"--exe" && i + 1 < argc )
		{
			exe = argv[++i];
			continue;
		}

		// Steam %command% support. Setting the game's launch options to
		//     "...\sinvr_launcher.exe" %command%
		// makes the Play button route through here, which keeps the overlay and
		// playtime working. Steam expands %command% to the game's own command
		// line, so the first thing we are handed is the path to the exe -- that
		// is the target, not an argument to forward to it.
		if ( exe.empty() && a.size() >= 4 )
		{
			const fs::path candidate( a );
			std::wstring leaf = candidate.filename().wstring();
			for ( size_t k = 0; k < leaf.size(); ++k )
				leaf[k] = (wchar_t)towlower( leaf[k] );
			if ( leaf == L"sinepisodes.exe" && Exists( candidate ) )
			{
				exe = candidate;
				Say( "invoked via Steam %%command%% -- target taken from the command line" );
				continue;
			}
		}

		if ( !extraArgs.empty() )
			extraArgs += L" ";
		extraArgs += a;
	}

	int rc = 0;
	do
	{
		if ( exe.empty() )
			exe = FindGameExe( selfDir );

		if ( exe.empty() || !Exists( exe ) )
		{
			Say( "could not find %S. Put this launcher in the game folder, or pass:", kExeName );
			Say( "  sinvr_launcher.exe --exe \"C:\\path\\to\\%S\"", kExeName );
			rc = 1;
			break;
		}

		const fs::path gameDir = exe.parent_path();
		Say( "game: %S", exe.c_str() );

		// ---- preflight -----------------------------------------------------
		//
		// Every one of these has to be beside the exe or the mod degrades
		// silently rather than failing loudly. Cheaper to say so here than to
		// read it out of a log afterwards.
		{
			const size_t total = sizeof( kRequired ) / sizeof( kRequired[0] );
			int missing = 0;
			for ( size_t i = 0; i < total; ++i )
			{
				if ( !Exists( gameDir / kRequired[i] ) )
				{
					Say( "MISSING: %S", kRequired[i] );
					++missing;
				}
			}

			// Not in kRequired because its absence is survivable in a way the
			// DLLs' is not: no manifest means no controllers, but the game still
			// runs in VR on mouse and keyboard.
			if ( !Exists( gameDir / L"actions" / L"sinvr_actions.json" ) )
				Say( "note: no actions\\sinvr_actions.json -- controller input "
					 "will be off. Copy the mod's actions\\ folder here to enable it." );
			if ( missing > 0 )
			{
				Say( "%d of %zu mod files are not in the game folder.", missing, total );
				Say( "The game will still start, but the mod will not be complete." );
			}
			else
			{
				Say( "all %zu mod files present", total );
			}
		}

		// ---- which Steam BRANCH is this? -----------------------------------
		//
		// Measured 2026-09-13: on the default (public) branch the game's content
		// is packed into vpks\depot_*.vpk, and the engine reads materials and
		// scripts from there -- ignoring any loose file of the same name. So the
		// mod's hands.vmt (the arms) and both optional folders silently do
		// nothing on that branch, and the only symptom is the arms still
		// drawing. The "loose" beta branch keeps the same files unpacked, where
		// they work.
		//
		// The packed archives are the cause, so their presence is what is
		// tested -- not the branch name, which Steam keeps somewhere else.
		{
			bool packed = false;
			std::error_code ec;
			for ( fs::directory_iterator it( gameDir / L"vpks", ec ), end;
				  !ec && it != end; it.increment( ec ) )
			{
				const std::wstring name = it->path().filename().wstring();
				if ( name.size() > 8 &&
					 lstrcmpiW( name.c_str() + name.size() - 8, L"_dir.vpk" ) == 0 )
				{
					packed = true;
					break;
				}
			}

			if ( packed )
			{
				Say( "WARNING: this copy of SiN is on Steam's DEFAULT branch. Its game files" );
				Say( "  are packed in vpks\\, and the game ignores the mod's loose content files" );
				Say( "  there: the character's ARMS WILL SHOW, and the two optional folders do" );
				Say( "  nothing. VR itself still works." );
				Say( "  Fix: Steam -> right-click SiN Episodes: Emergence -> Properties -> Betas" );
				Say( "  -> choose \"loose\", let it download, then copy the mod's SE1 folder in" );
				Say( "  again." );
			}
			else
			{
				Say( "game branch: loose content (no vpks\\ archives) -- the mod's content files apply" );
			}
		}

		// ---- the copy ------------------------------------------------------
		const PeInfo info = InspectPe( exe );
		if ( !info.valid )
		{
			Say( "could not parse %S as a PE image", exe.filename().c_str() );
			rc = 1;
			break;
		}
		if ( info.machine != 0x014C )
		{
			Say( "unexpected machine 0x%04X -- this launcher is for the 32-bit build",
				 info.machine );
			rc = 1;
			break;
		}

		fs::path toRun = exe;
		if ( info.largeAddressAware )
		{
			// Already patched on disk, most likely by NTCore's 4GB Patch.
			// Nothing to do, and no reason to make a copy.
			Say( "the shipped exe is ALREADY large-address-aware (0x%04X) -- launching it directly",
				 info.characteristics );
		}
		else
		{
			const fs::path copy = gameDir / kCopyName;
			if ( !MakeLaaCopy( exe, copy, info ) )
			{
				rc = 1;
				break;
			}
			toRun = copy;
		}

		// ---- DXVK's application profile, which our own rename defeats -------
		//
		// DXVK ships a per-application profile keyed to the EXE FILE NAME. Its
		// entry for this game, in src/util/config/config.cpp, is:
		//
		//     { R"(\\SinEpisodes\.exe$)", {{
		//       { "d3d9.memoryTrackTest",             "True" },
		//     }} },
		//
		// Without memoryTrackTest, D3D9 texture creation never reports out of
		// memory, GetAvailableTextureMem() never falls, and the engine's texture
		// probe never terminates -- it allocates until the card AND system RAM
		// are gone, about 17 GB in two seconds, and dies dereferencing the null
		// allocation inside DXVK. That is the startup crash of 2026-08-30.
		//
		// We defeat that profile ourselves: the regex cannot match
		// SinEpisodes_laa.exe. It only ever worked because the shipped exe
		// happened to have been byte-patched large-address-aware, so this
		// launcher ran it under its own name -- an accident, undocumented, and
		// removed by any reinstall or file verification.
		//
		// ORDER MATTERS HERE, and it is why this sits above DeriveSteamContext.
		// That function snapshots OUR environment into the packed block it hands
		// CreateProcess. Setting the variable first means:
		//   * ctx.ok  -> the block was built after this, so it carries it
		//   * !ctx.ok -> CreateProcess gets nullptr and the child inherits ours
		// Both paths, by construction, with one write. The display-mode override
		// further down is computed after derivation and therefore has to write
		// twice; do not copy that pattern up here.
		//
		// Set unconditionally rather than only for the copy. The value is
		// exactly what DXVK's own profile would have applied, so the direct-exe
		// path is unchanged -- and making both launch paths behave identically
		// is the real fix for the class of bug that cost a day. It also keeps
		// working if a future DXVK renames or drops the profile.
		//
		// A DXVK_CONFIG the user already set is preserved and placed LAST, since
		// DXVK splits the variable on ';' and parses in order.
		{
			std::wstring cfg = L"d3d9.memoryTrackTest = True";

			// ---- IMAGE QUALITY OPTIONS, FROM sinvr.cfg ---------------------
			//
			// Measured 2026-09-05: the scene IS rendered into a 2444x2392
			// MSAA=4 target at native resolution -- so MSAA is genuinely
			// working, and the edge shimmer that survives it is NOT geometric
			// aliasing. MSAA anti-aliases polygon SILHOUETTES only. It cannot
			// touch an alpha-tested cutout, whose edge lives INSIDE the
			// polygon and therefore gets one sample however many the polygon
			// gets -- and SiN is full of grates, railings and ladders.
			//
			// Sample-rate shading runs the pixel shader per SAMPLE instead of
			// per pixel, which is the thing that does anti-alias those edges,
			// plus specular sparkle. DXVK's own note: "May improve visual
			// clarity at a significant performance cost."
			//
			// Driven from the cfg rather than hardcoded because the cost is
			// real and the benefit is a matter of taste, and because A/B-ing a
			// DXVK option should not need a rebuilt launcher.
			{
				const fs::path scfg = gameDir / L"sinvr.cfg";

				// Defaults ON. Measured on a 1080 Ti -- not new hardware -- at
				// native 2444x2392 with MSAA x4: a steady 450 frames per 5.13 s
				// heartbeat, about 88 fps. DXVK's "significant performance
				// cost" note is about the technique in general, not a
				// measurement of any particular machine.
				//
				// A weaker GPU has two dials that do not need this turned off:
				// SteamVR's own per-application resolution, and
				// vr_resolution_scale below it.
				float srs = 1.0f;
				if ( ReadCfgFloat( scfg, "dxvk_sample_rate_shading", srs ) && srs > 0.5f )
					cfg += L";d3d9.forceSampleRateShading = True";

				// Forced anisotropy. A different axis of the same complaint:
				// texture shimmer at grazing angles, which no amount of MSAA
				// or sample-rate shading addresses. Cheap, unlike the above.
				float aniso = 16.0f;
				if ( ReadCfgFloat( scfg, "dxvk_anisotropy", aniso ) && aniso >= 1.0f )
				{
					wchar_t buf[64];
					_snwprintf_s( buf, _TRUNCATE, L";d3d9.samplerAnisotropy = %d",
								  (int)aniso );
					cfg += buf;
				}
			}

			// ---- THE VGUI CURSOR ROUTE, OFFERED FROM THE CFG ---------------
			//
			// With an oversize window -- now the DEFAULT -- the OS cursor cannot
			// reach the part of the game window that hangs below the desktop, so
			// menu items down there are unclickable. Measured: "OS clamped on
			// 149 move(s), worst 867 px".
			//
			// Clipping the cursor to the window does NOT fix this. ClipCursor
			// constrains a cursor; it cannot extend the desktop. The only fix is
			// to stop using the OS cursor, which is what vgui_input.h does.
			//
			// It stays OFF by default and that is deliberate: it once crashed
			// the game at launch, because its slot check passed on garbage and
			// slot 5 was then called on a vtable where it is not SetCursorPos.
			// The premise was never disproved -- the CHECK was wrong -- but it
			// must not be default-on while unproven.
			//
			// This only makes it reachable from sinvr.cfg instead of from a
			// Windows environment variable. The mod still reads the variable, so
			// the risky code is untouched by this.
			{
				const fs::path vcfg = gameDir / L"sinvr.cfg";
				float vguiCursor = 0.0f;
				if ( ReadCfgFloat( vcfg, "vgui_cursor", vguiCursor ) &&
					 vguiCursor > 0.5f )
				{
					SetEnvironmentVariableW( L"SINVR_VGUI_CURSOR", L"1" );
					Say( "vgui_cursor = 1 -- SINVR_VGUI_CURSOR set. If the game "
						 "crashes at launch, set it back to 0." );
				}
			}

			wchar_t     existing[1024] = { 0 };
			const DWORD cap = (DWORD)( sizeof( existing ) / sizeof( existing[0] ) );
			const DWORD got = GetEnvironmentVariableW( L"DXVK_CONFIG", existing, cap );
			if ( got > 0 && got < cap )
				cfg += L";" + std::wstring( existing );

			if ( SetEnvironmentVariableW( L"DXVK_CONFIG", cfg.c_str() ) )
				Say( "DXVK_CONFIG=%S", cfg.c_str() );
			else
				Say( "WARNING: could not set DXVK_CONFIG (%lu). If the game dies during "
					 "startup with \"DxvkMemoryAllocator: Memory allocation failed\", put "
					 "\"d3d9.memoryTrackTest = True\" in a dxvk.conf beside the exe.",
					 GetLastError() );
		}

		// ---- launch --------------------------------------------------------
		// NOT const: when the desktop clamp is lifted the render size has to be
		// handed to our DXVK fork, and the size is not known until further down.
		SteamContext ctx = DeriveSteamContext();
		if ( !ctx.ok )
			Say( "could not derive Steam's context from the registry -- launching without it" );

		std::wstring cmdline = L"\"" + toRun.wstring() + L"\"";
		if ( !ctx.launchOptions.empty() )
		{
			cmdline += L" " + ctx.launchOptions;
			Say( "steam launch options: %S", ctx.launchOptions.c_str() );
		}
		if ( !extraArgs.empty() )
			cmdline += L" " + extraArgs;

		// ---- RESOLUTION, FROM THE RUNTIME RATHER THAN FROM MEMORY ----------
		//
		// Appended LAST so everything the player set is already on the line and
		// can be inspected before deciding whether to add anything.
		{
			const fs::path cfg = gameDir / L"sinvr.cfg";

			float autoRes = 1.0f;
			ReadCfgFloat( cfg, "vr_auto_resolution", autoRes );

			float scale = 1.0f;
			ReadCfgFloat( cfg, "vr_resolution_scale", scale );

			// AN EXPLICIT -w/-h ALWAYS WINS.
			//
			// Someone who typed a resolution meant it, and silently overriding
			// it would be the launcher arguing with the player about their own
			// launch options. Reported either way so the reason is visible.
			const bool userSized = HasSwitch( cmdline, L"-w" ) || HasSwitch( cmdline, L"-h" ) ||
								   HasSwitch( cmdline, L"-width" ) || HasSwitch( cmdline, L"-height" );

			if ( autoRes == 0.0f )
			{
				Say( "auto resolution is off (vr_auto_resolution = 0) -- using the "
					 "launch options as they are" );
			}
			else if ( userSized )
			{
				Say( "-w/-h are already set, so those win -- remove them from Steam's "
					 "launch options to let the headset decide" );
			}
			else
			{
				const HeadsetRes hr = QueryHeadsetResolution( gameDir );
				if ( hr.ok )
				{
					if ( scale <= 0.05f || scale > 4.0f )
					{
						Say( "vr_resolution_scale %.3f is out of range -- using 1.0", scale );
						scale = 1.0f;
					}

					double wantW = hr.width * (double)scale;
					double wantH = hr.height * (double)scale;

					// ---- IT MUST FIT ON THE DESKTOP ------------------------
					//
					// The engine refuses a windowed mode larger than the
					// physical desktop -- "Failed to set video mode - resetting
					// to defaults" -- and then rewrites its own video settings,
					// so overshooting is not merely a failed launch, it
					// disturbs the config on the way out.
					//
					// Scaled by ONE factor on both axes, deliberately. The
					// ASPECT is the part that has to survive: the mod extends
					// the engine frustum to cover the render target's aspect
					// and crops back per eye, so a preserved aspect is exact at
					// any size while a squashed one renders pixels that are
					// thrown away. Clamping the axes independently would fix
					// the size and break the thing that matters.
					long deskW = 0, deskH = 0;
					PhysicalDesktopSize( deskW, deskH );

					// ---- A CEILING ON THE RENDER SIZE -------------------
					//
					// SteamVR's slider is the supersampling control, and it is
					// easy to leave somewhere ambitious: a slider set high asked
					// for 3988x3904 per eye -- 15.6 MPix an eye, 31 MPix a frame
					// at 90 Hz -- and that did not merely run slowly, it took the
					// HEADSET down before the game was even playable.
					//
					// This caps the requested HEIGHT, aspect preserved, so the
					// slider cannot push past what the rig can survive. It is
					// applied BEFORE the desktop clamp, and independently of it:
					// this is about what the GPU and headset can take, which has
					// nothing to do with how big the monitor is.
					//
					// 0 disables it. Set it to your headset's native per-eye
					// height to say "render native, never supersample".
					float maxHeight = 0.0f;
					ReadCfgFloat( cfg, "vr_max_render_height", maxHeight );
					if ( maxHeight > 0.0f && wantH > (double)maxHeight )
					{
						const double shrink = (double)maxHeight / wantH;
						wantW *= shrink;
						wantH *= shrink;
						Say( "capped to %.0f px tall by vr_max_render_height "
							 "(x%.3f) -- SteamVR asked for more than this rig is "
							 "set to attempt",
							 (double)maxHeight, shrink );
					}

					// ---- LIFTING THE CLAMP ------------------------------
					//
					// The refusal is D3D9's, not Windows'. Proved by
					// experiment: with our DXVK fork reporting a larger
					// display mode, the engine accepted a 2560x2500 window on
					// a 3840x2160 desktop and rendered it -- eye surfaces came
					// back at the full 2500. Without the override, on the same
					// build and the same arguments, no device was created at
					// all. engine.dll asks D3D9 how big the display is; only
					// shaderapidx9.dll imports d3d9.dll, and that is us.
					//
					// This matters most on a SMALL monitor, where it is not
					// supersampling at all: a 1080p desktop cannot reach a
					// modern headset's native per-eye resolution in either
					// axis, so the clamp costs real sharpness rather than
					// optional extra.
					//
					// Off by default. The size still comes from SteamVR's own
					// resolution slider via GetRecommendedRenderTargetSize, so
					// the performance trade stays where it belongs -- with the
					// player, who can move the slider either way.
					float oversize = 1.0f;
					ReadCfgFloat( cfg, "vr_allow_oversize_window", oversize );
					const bool allowOversize = ( oversize != 0.0f );

					double fit = 1.0;
					if ( !allowOversize )
					{
						if ( deskW > 0 && wantW > deskW )
							fit = deskW / wantW;
						if ( deskH > 0 && wantH / deskH > 1.0 / fit )
							fit = deskH / wantH;
					}

					const bool clamped = ( fit < 0.999 );
					if ( clamped )
					{
						wantW *= fit;
						wantH *= fit;
					}

					// Even numbers. Odd render targets are legal but a needless
					// thing to be the first to discover on someone else's rig.
					long w = (long)( wantW + 0.5 ) & ~1L;
					long h = (long)( wantH + 0.5 ) & ~1L;

					if ( w < 640 ) w = 640;
					if ( h < 480 ) h = 480;
					if ( w > 8192 ) w = 8192;
					if ( h > 8192 ) h = 8192;

					wchar_t sized[64];
					swprintf_s( sized, L" -w %ld -h %ld", w, h );
					cmdline += sized;

					// Tell our DXVK fork to report a display at least this big,
					// or engine.dll refuses the mode and rewrites its own video
					// settings on the way out. Only ever inflated to what this
					// run actually asks for -- the lie is as small as it can be,
					// and it is not told at all when nothing oversized was
					// requested.
					if ( allowOversize && deskW > 0 && deskH > 0 &&
						 ( w > deskW || h > deskH ) )
					{
						wchar_t mode[80];
						swprintf_s( mode, L"SINVR_DISPLAY_MODE_OVERRIDE=%ldx%ld", w, h );
						AppendEnvVar( ctx.environment, mode );
						// BOTH paths, deliberately. CreateProcess is handed
						// ctx.environment only when ctx.ok; if deriving Steam's
						// context failed it is passed nullptr and the child
						// inherits ours instead. Setting only one of the two
						// would make the override vanish silently in exactly
						// the case that is already going wrong.
						wchar_t value[32];
						swprintf_s( value, L"%ldx%ld", w, h );
						SetEnvironmentVariableW( L"SINVR_DISPLAY_MODE_OVERRIDE", value );
						Say( "window is larger than the %ldx%ld desktop -- DXVK will "
							 "report a %ldx%ld display so the engine accepts it "
							 "(vr_allow_oversize_window = 1). The game window will "
							 "extend off-screen; that is expected and costs nothing.",
							 deskW, deskH, w, h );
					}

					Say( "headset wants %ux%u per eye; at scale %.3f that is %.0fx%.0f",
						 hr.width, hr.height, scale,
						 hr.width * (double)scale, hr.height * (double)scale );
					if ( clamped )
						Say( "clamped to the %ldx%ld desktop (x%.3f) -- the engine "
							 "refuses a window larger than the display",
							 deskW, deskH, fit );
					Say( "rendering %ldx%ld per eye (aspect %.3f, headset %.3f)",
						 w, h,
						 h > 0 ? (double)w / (double)h : 0.0,
						 hr.height > 0 ? (double)hr.width / (double)hr.height : 0.0 );

					// A resolution taken from a headset is almost never a monitor
					// MODE, so fullscreen has nothing to switch to. Added only
					// when the player has expressed no preference -- an explicit
					// -full is theirs to own, and is reported rather than
					// quietly overridden.
					const bool windowed = HasSwitch( cmdline, L"-window" ) ||
										  HasSwitch( cmdline, L"-windowed" ) ||
										  HasSwitch( cmdline, L"-sw" );
					const bool fullscreen = HasSwitch( cmdline, L"-full" ) ||
											HasSwitch( cmdline, L"-fullscreen" );
					if ( fullscreen )
					{
						Say( "NOTE: -full is set and %ldx%ld is unlikely to be a "
							 "monitor mode. If the game refuses to start, drop "
							 "-full or set vr_auto_resolution = 0.", w, h );
					}
					else if ( !windowed )
					{
						cmdline += L" -window";
						Say( "added -window, because a headset resolution is not a "
							 "monitor mode" );
					}
				}
			}
		}

		// Parent the game to steam.exe where possible. Not required here --
		// there is no SteamStub checking who started it -- but it is what Steam
		// expects, so the overlay attaches and playtime is recorded as usual.
		HANDLE steam = nullptr;
		const DWORD steamPid = FindPid( L"steam.exe" );
		if ( steamPid != 0 )
			steam = OpenProcess( PROCESS_CREATE_PROCESS, FALSE, steamPid );
		if ( steam == nullptr )
			Say( "Steam not available as a parent process -- continuing without it" );

		PROCESS_INFORMATION pi = {};
		bool started = false;
		{
			SIZE_T attrSize = 0;
			InitializeProcThreadAttributeList( nullptr, 1, 0, &attrSize );
			std::vector<uint8_t> attrBuf( attrSize );
			LPPROC_THREAD_ATTRIBUTE_LIST attrs =
				reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>( attrBuf.data() );

			STARTUPINFOEXW si = {};
			si.StartupInfo.cb = sizeof( si );

			DWORD flags = CREATE_UNICODE_ENVIRONMENT | CREATE_SUSPENDED;
			bool haveAttrs = false;
			if ( steam != nullptr &&
				 InitializeProcThreadAttributeList( attrs, 1, 0, &attrSize ) &&
				 UpdateProcThreadAttribute( attrs, 0, PROC_THREAD_ATTRIBUTE_PARENT_PROCESS,
											&steam, sizeof( steam ), nullptr, nullptr ) )
			{
				si.lpAttributeList = attrs;
				flags |= EXTENDED_STARTUPINFO_PRESENT;
				haveAttrs = true;
			}

			started = CreateProcessW(
						  toRun.c_str(), &cmdline[0], nullptr, nullptr, FALSE, flags,
						  ctx.ok ? (LPVOID)ctx.environment.c_str() : nullptr,
						  gameDir.c_str(), &si.StartupInfo, &pi ) != FALSE;

			if ( haveAttrs )
				DeleteProcThreadAttributeList( attrs );
		}
		if ( steam != nullptr )
			CloseHandle( steam );

		if ( !started )
		{
			Say( "CreateProcess failed (%lu)", GetLastError() );
			rc = 1;
			break;
		}

		// Started suspended purely so the address space can be proven before the
		// game does anything with it.
		if ( !VerifyLargeAddressSpace( pi.hProcess ) )
		{
			Say( "the process did NOT get a 4 GB address space -- terminating rather than" );
			Say( "running a session that would look patched and behave capped" );
			TerminateProcess( pi.hProcess, 0 );
			WaitForSingleObject( pi.hProcess, 5000 );
			CloseHandle( pi.hThread );
			CloseHandle( pi.hProcess );
			rc = 1;
			break;
		}

		ResumeThread( pi.hThread );
		Say( "running with a 4 GB address space (pid %lu)", pi.dwProcessId );
		Say( "the shipped %S was never written to", kExeName );
		Say( "log: %S\\sinvr.log", gameDir.c_str() );

		CloseHandle( pi.hThread );
		CloseHandle( pi.hProcess );
	} while ( false );

	if ( ownConsole )
	{
		printf( "\nPress Enter to close." );
		fflush( stdout );
		(void)getchar();
	}
	return rc;
}
