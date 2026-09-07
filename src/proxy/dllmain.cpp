// dinput8.dll proxy -- the injection vector.
//
// engine.dll imports DINPUT8.dll, and the Windows loader searches the
// executable's directory before System32, so a dinput8.dll sitting next to
// SinEpisodes.exe is loaded early and reliably. It does nothing except forward
// every export to the real system DLL and pull in sinvr.dll alongside it.
//
// Keeping the mod logic in a separate DLL means iterating on it never requires
// touching this file.

#include <windows.h>
#include "../common/log.h"

namespace {

HMODULE g_realDInput8 = nullptr;
HMODULE g_mod = nullptr;

using PFN_DirectInput8Create = HRESULT( WINAPI* )( HINSTANCE, DWORD, const GUID&, void**, void* );
using PFN_DllCanUnloadNow    = HRESULT( WINAPI* )( void );
using PFN_DllGetClassObject  = HRESULT( WINAPI* )( const GUID&, const GUID&, void** );
using PFN_DllRegisterServer  = HRESULT( WINAPI* )( void );
using PFN_DllUnregisterServer = HRESULT( WINAPI* )( void );
using PFN_GetdfDIJoystick    = void*( WINAPI* )( void );

PFN_DirectInput8Create    g_DirectInput8Create = nullptr;
PFN_DllCanUnloadNow       g_DllCanUnloadNow = nullptr;
PFN_DllGetClassObject     g_DllGetClassObject = nullptr;
PFN_DllRegisterServer     g_DllRegisterServer = nullptr;
PFN_DllUnregisterServer   g_DllUnregisterServer = nullptr;
PFN_GetdfDIJoystick       g_GetdfDIJoystick = nullptr;

void LoadRealDInput8()
{
	wchar_t path[MAX_PATH] = { 0 };
	UINT len = GetSystemDirectoryW( path, MAX_PATH );
	if ( len == 0 || len >= MAX_PATH )
		return;

	wcscat_s( path, MAX_PATH, L"\\dinput8.dll" );
	g_realDInput8 = LoadLibraryW( path );
	if ( !g_realDInput8 )
	{
		sinvr::Log( "proxy: FAILED to load system dinput8.dll (err %lu)", GetLastError() );
		return;
	}

	g_DirectInput8Create =
		(PFN_DirectInput8Create)GetProcAddress( g_realDInput8, "DirectInput8Create" );
	g_DllCanUnloadNow =
		(PFN_DllCanUnloadNow)GetProcAddress( g_realDInput8, "DllCanUnloadNow" );
	g_DllGetClassObject =
		(PFN_DllGetClassObject)GetProcAddress( g_realDInput8, "DllGetClassObject" );
	g_DllRegisterServer =
		(PFN_DllRegisterServer)GetProcAddress( g_realDInput8, "DllRegisterServer" );
	g_DllUnregisterServer =
		(PFN_DllUnregisterServer)GetProcAddress( g_realDInput8, "DllUnregisterServer" );
	g_GetdfDIJoystick =
		(PFN_GetdfDIJoystick)GetProcAddress( g_realDInput8, "GetdfDIJoystick" );
}

// sinvr.dll lives beside this proxy, i.e. in the game root.
void LoadModDll( HINSTANCE self )
{
	wchar_t path[MAX_PATH] = { 0 };
	if ( GetModuleFileNameW( self, path, MAX_PATH ) == 0 )
		return;

	wchar_t* slash = wcsrchr( path, L'\\' );
	if ( !slash )
		return;
	wcscpy_s( slash + 1, MAX_PATH - ( slash + 1 - path ), L"sinvr.dll" );

	g_mod = LoadLibraryW( path );
	if ( !g_mod )
		sinvr::Log( "proxy: FAILED to load sinvr.dll (err %lu)", GetLastError() );
}

} // namespace

extern "C" {

HRESULT WINAPI DirectInput8Create( HINSTANCE hinst, DWORD version, const GUID& riid,
								   void** out, void* outer )
{
	if ( !g_DirectInput8Create )
		return E_FAIL;
	return g_DirectInput8Create( hinst, version, riid, out, outer );
}

HRESULT WINAPI DllCanUnloadNow( void )
{
	// Never let the loader drop us; sinvr.dll is holding hooks into client.dll.
	return S_FALSE;
}

HRESULT WINAPI DllGetClassObject( const GUID& rclsid, const GUID& riid, void** ppv )
{
	if ( !g_DllGetClassObject )
		return E_FAIL;
	return g_DllGetClassObject( rclsid, riid, ppv );
}

HRESULT WINAPI DllRegisterServer( void )
{
	return g_DllRegisterServer ? g_DllRegisterServer() : E_FAIL;
}

HRESULT WINAPI DllUnregisterServer( void )
{
	return g_DllUnregisterServer ? g_DllUnregisterServer() : E_FAIL;
}

void* WINAPI GetdfDIJoystick( void )
{
	return g_GetdfDIJoystick ? g_GetdfDIJoystick() : nullptr;
}

} // extern "C"

BOOL WINAPI DllMain( HINSTANCE self, DWORD reason, LPVOID )
{
	if ( reason == DLL_PROCESS_ATTACH )
	{
		DisableThreadLibraryCalls( self );
		LoadRealDInput8();
		LoadModDll( self );
	}
	return TRUE;
}
