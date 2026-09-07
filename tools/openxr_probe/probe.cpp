// openxr_probe -- step 4 of the OpenXR spike. See *CURRENT GOALS* in HANDOVER.md.
//
// ---- THE ONE QUESTION THIS ANSWERS ------------------------------------------
//
// Steps 1-3 of the spike are already done and passed: SteamVR Beta 2.17.2
// registers a 32-bit OpenXR runtime, and its `bin\vrclient.dll` carries the full
// entry points including XR_KHR_vulkan_enable2. All of that was read off the
// registry and the binary.
//
// What CANNOT be read off a binary is the thing that decides whether an OpenXR
// backend is viable at all:
//
//     will the runtime accept a VkDevice that DXVK created, and that we had no
//     opportunity to influence?
//
// That matters because the OpenXR Vulkan bindings assume the APPLICATION creates
// the Vulkan instance and device, after asking the runtime what it needs:
//
//   XR_KHR_vulkan_enable   the app queries xrGetVulkanInstanceExtensionsKHR /
//                          xrGetVulkanDeviceExtensionsKHR and must ENABLE those
//                          extensions when it creates its instance and device.
//   XR_KHR_vulkan_enable2  the app is expected to create them THROUGH the
//                          runtime, via xrCreateVulkanInstanceKHR /
//                          xrCreateVulkanDeviceKHR, so the runtime can inject
//                          whatever it needs.
//
// We can do NEITHER. DXVK creates its own VkInstance and VkDevice long before we
// are in the picture, with its own extension list, and there is no hook to add
// to it. So the real question is whether the runtime will take them anyway --
// and if not, exactly which requirement is unmet.
//
// This probe therefore does not port anything. It stands up a real DXVK device,
// asks the runtime what it wants, prints the difference, and then tries the one
// call that settles it: xrCreateSession with DXVK's handles in the graphics
// binding. An afternoon instead of a week.
//
// ---- WHAT IT DELIBERATELY DOES NOT DO ---------------------------------------
//
// No loader DLL. SteamVR ships no 32-bit openxr_loader.dll (only bin\win64\),
// and pulling in the Khronos one is a dependency this probe does not need: the
// runtime is reached directly through xrNegotiateLoaderRuntimeInterface, which
// is the same handshake the real loader performs. Fewer moving parts, and one
// less thing to blame if it fails.
//
// No rendering, no frame loop, no swapchain acquire. If the session is created
// the question is answered; everything after that is ordinary work.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>
#include <stdint.h>
#include <string>
#include <vector>

#define VK_USE_PLATFORM_WIN32_KHR 1
#include <vulkan/vulkan.h>

#define XR_USE_GRAPHICS_API_VULKAN 1
#define XR_USE_PLATFORM_WIN32 1
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

// The loader handshake structs. These are NOT in openxr.h -- they are the
// runtime-facing half of the loader interface, and the SDK ships them in their
// own header. Using it rather than hand-declaring them matters more than it
// looks: a wrong field here would fail the negotiate call, and that failure
// would look exactly like "the runtime rejected DXVK", which is the one wrong
// answer this probe must never give.
#include <openxr/openxr_loader_negotiation.h>

// The DXVK fork's VR interface. Kept as a local copy rather than including the
// DXVK tree, so this probe builds standalone.
struct D3D9_TEXTURE_VR_DESC
{
	uint64_t         Image;
	VkDevice         Device;
	VkPhysicalDevice PhysicalDevice;
	VkInstance       Instance;
	VkQueue          Queue;
	uint32_t         QueueFamilyIndex;
	uint32_t         Width;
	uint32_t         Height;
	VkFormat         Format;
	uint32_t         SampleCount;
};

MIDL_INTERFACE( "b1c3f2d4-5a6e-4f70-9c81-2d3e4f5a6b7c" )
IDirect3DVR9 : public IUnknown
{
	virtual HRESULT STDMETHODCALLTYPE GetVRDesc( IDirect3DSurface9*, D3D9_TEXTURE_VR_DESC* ) = 0;
	virtual HRESULT STDMETHODCALLTYPE TransferSurface( IDirect3DSurface9*, BOOL ) = 0;
	virtual HRESULT STDMETHODCALLTYPE LockDevice() = 0;
	virtual HRESULT STDMETHODCALLTYPE UnlockDevice() = 0;
	virtual HRESULT STDMETHODCALLTYPE LockSubmissionQueue() = 0;
	virtual HRESULT STDMETHODCALLTYPE UnlockSubmissionQueue() = 0;
	virtual HRESULT STDMETHODCALLTYPE WaitDeviceIdle() = 0;
};

typedef IDirect3D9* ( WINAPI* PFN_Direct3DCreate9 )( UINT );
typedef HRESULT( __stdcall* PFN_Direct3DCreateVR9 )( IDirect3DDevice9*, IDirect3DVR9** );

//------------------------------------------------------------------------------
// Reporting. Every step prints its own verdict, because a failure part-way needs
// to be attributable to the step that failed and not to the question.

static int g_step = 0;

static void Head( const char* what )
{
	printf( "\n[%d] %s\n", ++g_step, what );
}

static void Say( const char* fmt, ... )
{
	va_list a;
	va_start( a, fmt );
	printf( "      " );
	vprintf( fmt, a );
	printf( "\n" );
	va_end( a );
}

static const char* XrName( XrResult r )
{
	switch ( r )
	{
	case XR_SUCCESS:                             return "XR_SUCCESS";
	case XR_ERROR_VALIDATION_FAILURE:            return "XR_ERROR_VALIDATION_FAILURE";
	case XR_ERROR_RUNTIME_FAILURE:               return "XR_ERROR_RUNTIME_FAILURE";
	case XR_ERROR_OUT_OF_MEMORY:                 return "XR_ERROR_OUT_OF_MEMORY";
	case XR_ERROR_API_VERSION_UNSUPPORTED:       return "XR_ERROR_API_VERSION_UNSUPPORTED";
	case XR_ERROR_INITIALIZATION_FAILED:         return "XR_ERROR_INITIALIZATION_FAILED";
	case XR_ERROR_FUNCTION_UNSUPPORTED:          return "XR_ERROR_FUNCTION_UNSUPPORTED";
	case XR_ERROR_FEATURE_UNSUPPORTED:           return "XR_ERROR_FEATURE_UNSUPPORTED";
	case XR_ERROR_EXTENSION_NOT_PRESENT:         return "XR_ERROR_EXTENSION_NOT_PRESENT";
	case XR_ERROR_LIMIT_REACHED:                 return "XR_ERROR_LIMIT_REACHED";
	case XR_ERROR_SIZE_INSUFFICIENT:             return "XR_ERROR_SIZE_INSUFFICIENT";
	case XR_ERROR_HANDLE_INVALID:                return "XR_ERROR_HANDLE_INVALID";
	case XR_ERROR_INSTANCE_LOST:                 return "XR_ERROR_INSTANCE_LOST";
	case XR_ERROR_SYSTEM_INVALID:                return "XR_ERROR_SYSTEM_INVALID";
	case XR_ERROR_FORM_FACTOR_UNSUPPORTED:       return "XR_ERROR_FORM_FACTOR_UNSUPPORTED";
	case XR_ERROR_FORM_FACTOR_UNAVAILABLE:       return "XR_ERROR_FORM_FACTOR_UNAVAILABLE";
	case XR_ERROR_GRAPHICS_REQUIREMENTS_CALL_MISSING:
		return "XR_ERROR_GRAPHICS_REQUIREMENTS_CALL_MISSING";
	case XR_ERROR_GRAPHICS_DEVICE_INVALID:       return "XR_ERROR_GRAPHICS_DEVICE_INVALID";
	case XR_ERROR_RUNTIME_UNAVAILABLE:           return "XR_ERROR_RUNTIME_UNAVAILABLE";
	default:                                     return "(see XrResult in openxr.h)";
	}
}

static bool Ok( XrResult r, const char* call )
{
	if ( XR_SUCCEEDED( r ) )
		return true;
	Say( "FAILED  %s -> %d %s", call, (int)r, XrName( r ) );
	return false;
}

//------------------------------------------------------------------------------
// Finding the runtime, without a loader.

static std::string RegRead32BitActiveRuntime()
{
	// KEY_WOW64_32KEY: a 32-bit process already sees WOW6432Node by redirection,
	// but being explicit means this reads the same key whatever it is built as.
	HKEY key = nullptr;
	LONG rc = RegOpenKeyExA( HKEY_LOCAL_MACHINE, "SOFTWARE\\Khronos\\OpenXR\\1",
							 0, KEY_READ | KEY_WOW64_32KEY, &key );
	if ( rc != ERROR_SUCCESS )
		return std::string();

	char  buf[1024] = { 0 };
	DWORD size = sizeof( buf );
	DWORD type = 0;
	rc = RegQueryValueExA( key, "ActiveRuntime", nullptr, &type, (LPBYTE)buf, &size );
	RegCloseKey( key );

	if ( rc != ERROR_SUCCESS || type != REG_SZ )
		return std::string();
	return std::string( buf );
}

// The manifest is small and its shape is fixed, so this pulls "library_path" out
// directly rather than dragging in a JSON parser for one string. It does have to
// honour JSON escaping -- the value reads "bin\\vrclient.dll" on disk.
static std::string ManifestLibraryPath( const std::string& manifest )
{
	FILE* f = fopen( manifest.c_str(), "rb" );
	if ( !f )
		return std::string();

	std::string text;
	char        chunk[4096];
	size_t      n;
	while ( ( n = fread( chunk, 1, sizeof( chunk ), f ) ) > 0 )
		text.append( chunk, n );
	fclose( f );

	const size_t key = text.find( "library_path" );
	if ( key == std::string::npos )
		return std::string();

	size_t colon = text.find( ':', key );
	if ( colon == std::string::npos )
		return std::string();
	size_t open = text.find( '"', colon );
	if ( open == std::string::npos )
		return std::string();

	std::string out;
	for ( size_t i = open + 1; i < text.size(); ++i )
	{
		const char c = text[i];
		if ( c == '"' )
			break;
		if ( c == '\\' && i + 1 < text.size() )
		{
			++i;
			out.push_back( text[i] );   // \\ -> \, and anything else literally
			continue;
		}
		out.push_back( c );
	}
	return out;
}

static std::string DirOf( const std::string& path )
{
	const size_t slash = path.find_last_of( "\\/" );
	return slash == std::string::npos ? std::string() : path.substr( 0, slash + 1 );
}

//------------------------------------------------------------------------------

struct Dxvk
{
	HMODULE            module = nullptr;
	IDirect3D9*        d3d9 = nullptr;
	IDirect3DDevice9*  device = nullptr;
	IDirect3DVR9*      vr = nullptr;
	D3D9_TEXTURE_VR_DESC desc = {};
	HWND               window = nullptr;
};

static bool StandUpDxvk( Dxvk& dx, const std::string& d3d9Path )
{
	dx.module = LoadLibraryA( d3d9Path.c_str() );
	if ( !dx.module )
	{
		Say( "FAILED  LoadLibrary(\"%s\") -> %lu", d3d9Path.c_str(), GetLastError() );
		Say( "        Pass the fork's d3d9.dll with --d3d9 <path>." );
		return false;
	}
	Say( "loaded  %s", d3d9Path.c_str() );

	// If this resolves, the DLL is a DXVK build carrying our fork's interface.
	// The system d3d9.dll does not export it, which is the check that stops this
	// probe silently measuring Microsoft's D3D9 instead.
	PFN_Direct3DCreate9   create9 = (PFN_Direct3DCreate9)GetProcAddress( dx.module, "Direct3DCreate9" );
	PFN_Direct3DCreateVR9 createVR = (PFN_Direct3DCreateVR9)GetProcAddress( dx.module, "Direct3DCreateVR9" );
	if ( !create9 || !createVR )
	{
		Say( "FAILED  Direct3DCreate9=%p Direct3DCreateVR9=%p", create9, createVR );
		Say( "        Direct3DCreateVR9 missing means this is not the SiN VR DXVK fork." );
		return false;
	}

	WNDCLASSA wc = {};
	wc.lpfnWndProc = DefWindowProcA;
	wc.hInstance = GetModuleHandleA( nullptr );
	wc.lpszClassName = "SinVrOpenXrProbe";
	RegisterClassA( &wc );
	dx.window = CreateWindowA( "SinVrOpenXrProbe", "probe", WS_OVERLAPPEDWINDOW,
							   0, 0, 640, 480, nullptr, nullptr, wc.hInstance, nullptr );
	if ( !dx.window )
	{
		Say( "FAILED  CreateWindow -> %lu", GetLastError() );
		return false;
	}

	dx.d3d9 = create9( D3D_SDK_VERSION );
	if ( !dx.d3d9 )
	{
		Say( "FAILED  Direct3DCreate9 returned null" );
		return false;
	}

	D3DPRESENT_PARAMETERS pp = {};
	pp.BackBufferWidth = 640;
	pp.BackBufferHeight = 480;
	pp.BackBufferFormat = D3DFMT_X8R8G8B8;
	pp.BackBufferCount = 1;
	pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
	pp.hDeviceWindow = dx.window;
	pp.Windowed = TRUE;

	HRESULT hr = dx.d3d9->CreateDevice( D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, dx.window,
										D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_MULTITHREADED,
										&pp, &dx.device );
	if ( FAILED( hr ) || !dx.device )
	{
		Say( "FAILED  CreateDevice -> 0x%08lX", (unsigned long)hr );
		return false;
	}

	hr = createVR( dx.device, &dx.vr );
	if ( FAILED( hr ) || !dx.vr )
	{
		Say( "FAILED  Direct3DCreateVR9 -> 0x%08lX", (unsigned long)hr );
		return false;
	}

	IDirect3DSurface9* back = nullptr;
	hr = dx.device->GetBackBuffer( 0, 0, D3DBACKBUFFER_TYPE_MONO, &back );
	if ( FAILED( hr ) || !back )
	{
		Say( "FAILED  GetBackBuffer -> 0x%08lX", (unsigned long)hr );
		return false;
	}

	hr = dx.vr->GetVRDesc( back, &dx.desc );
	back->Release();
	if ( FAILED( hr ) )
	{
		Say( "FAILED  GetVRDesc -> 0x%08lX", (unsigned long)hr );
		return false;
	}

	Say( "VkInstance       %p", (void*)dx.desc.Instance );
	Say( "VkPhysicalDevice %p", (void*)dx.desc.PhysicalDevice );
	Say( "VkDevice         %p", (void*)dx.desc.Device );
	Say( "VkQueue          %p  family %u", (void*)dx.desc.Queue, dx.desc.QueueFamilyIndex );
	Say( "backbuffer       %ux%u  VkFormat %d", dx.desc.Width, dx.desc.Height, (int)dx.desc.Format );
	return true;
}

// Names a VkPhysicalDevice, so the comparison below reads as hardware rather
// than as two pointers.
static void DescribePhysicalDevice( VkInstance instance, VkPhysicalDevice pd, const char* label )
{
	HMODULE vk = LoadLibraryA( "vulkan-1.dll" );
	if ( !vk )
		return;
	auto gipa = (PFN_vkGetInstanceProcAddr)GetProcAddress( vk, "vkGetInstanceProcAddr" );
	if ( !gipa )
		return;
	auto props = (PFN_vkGetPhysicalDeviceProperties)gipa( instance, "vkGetPhysicalDeviceProperties" );
	if ( !props )
		return;

	VkPhysicalDeviceProperties p = {};
	props( pd, &p );
	Say( "%s %p  \"%s\"", label, (void*)pd, p.deviceName );
}

//------------------------------------------------------------------------------

int main( int argc, char** argv )
{
	std::string d3d9Path =
		"C:\\Program Files (x86)\\Steam\\steamapps\\common\\SiN Episodes Emergence\\d3d9.dll";

	for ( int i = 1; i < argc; ++i )
	{
		if ( strcmp( argv[i], "--d3d9" ) == 0 && i + 1 < argc )
			d3d9Path = argv[++i];
	}

	// UNBUFFERED, and this is not a nicety. Redirected to a file, stdout is
	// block-buffered, so a crash anywhere in this probe discards every line it
	// had printed and the failure arrives as a bare access violation with no
	// indication of which step reached it. A diagnostic that loses its output
	// on the one path that matters is worthless.
	setvbuf( stdout, nullptr, _IONBF, 0 );

	printf( "openxr_probe -- does SteamVR's 32-bit OpenXR runtime accept a DXVK device?\n" );
	printf( "built %s %s, %u-bit process\n", __DATE__, __TIME__, (unsigned)( sizeof( void* ) * 8 ) );

	if ( sizeof( void* ) != 4 )
	{
		printf( "\nSTOP: this must be built 32-bit. A 64-bit build reads the 64-bit\n"
				"      runtime and proves nothing about SiN.\n" );
		return 2;
	}

	// ---- 1. DXVK -----------------------------------------------------------
	Head( "Stand up a real DXVK device and take its Vulkan handles" );
	Dxvk dx;
	if ( !StandUpDxvk( dx, d3d9Path ) )
		return 1;

	// ---- 2. Reach the runtime ----------------------------------------------
	Head( "Find the 32-bit OpenXR runtime and negotiate with it directly" );
	const std::string manifest = RegRead32BitActiveRuntime();
	if ( manifest.empty() )
	{
		Say( "FAILED  HKLM\\SOFTWARE\\WOW6432Node\\Khronos\\OpenXR\\1\\ActiveRuntime is not set." );
		Say( "        Opt into the SteamVR Beta and accept the prompt to make it the" );
		Say( "        default OpenXR runtime." );
		return 1;
	}
	Say( "manifest %s", manifest.c_str() );

	std::string lib = ManifestLibraryPath( manifest );
	if ( lib.empty() )
	{
		Say( "FAILED  no library_path in the manifest" );
		return 1;
	}
	// library_path may be relative to the manifest's own directory.
	if ( lib.size() > 1 && lib[1] != ':' && lib[0] != '\\' )
		lib = DirOf( manifest ) + lib;
	Say( "runtime  %s", lib.c_str() );

	HMODULE rt = LoadLibraryA( lib.c_str() );
	if ( !rt )
	{
		Say( "FAILED  LoadLibrary -> %lu", GetLastError() );
		return 1;
	}

	auto negotiate = (PFN_xrNegotiateLoaderRuntimeInterface)
		GetProcAddress( rt, "xrNegotiateLoaderRuntimeInterface" );
	if ( !negotiate )
	{
		Say( "FAILED  the runtime does not export xrNegotiateLoaderRuntimeInterface" );
		return 1;
	}

	XrNegotiateLoaderInfo want = {};
	want.structType = XR_LOADER_INTERFACE_STRUCT_LOADER_INFO;
	want.structVersion = XR_LOADER_INFO_STRUCT_VERSION;
	want.structSize = sizeof( want );
	want.minInterfaceVersion = 1;
	want.maxInterfaceVersion = XR_CURRENT_LOADER_RUNTIME_VERSION;
	want.minApiVersion = XR_MAKE_VERSION( 1, 0, 0 );
	want.maxApiVersion = XR_MAKE_VERSION( 1, 1, 99 );

	XrNegotiateRuntimeRequest got = {};
	got.structType = XR_LOADER_INTERFACE_STRUCT_RUNTIME_REQUEST;
	got.structVersion = XR_RUNTIME_INFO_STRUCT_VERSION;
	got.structSize = sizeof( got );

	if ( !Ok( negotiate( &want, &got ), "xrNegotiateLoaderRuntimeInterface" ) )
		return 1;
	if ( !got.getInstanceProcAddr )
	{
		Say( "FAILED  negotiate succeeded but returned no getInstanceProcAddr" );
		return 1;
	}
	Say( "negotiated: runtime API %u.%u.%u, interface version %u",
		 (unsigned)XR_VERSION_MAJOR( got.runtimeApiVersion ),
		 (unsigned)XR_VERSION_MINOR( got.runtimeApiVersion ),
		 (unsigned)XR_VERSION_PATCH( got.runtimeApiVersion ),
		 got.runtimeInterfaceVersion );

	PFN_xrGetInstanceProcAddr xrGIPA = got.getInstanceProcAddr;

	// The instance argument is NOT optional, and getting it wrong is silent.
	// xrGetInstanceProcAddr with XR_NULL_HANDLE returns only the four GLOBAL
	// entry points -- enumerate-extensions, enumerate-layers, create-instance,
	// and itself. Ask it for anything instance-level with a null handle and it
	// returns XR_ERROR_HANDLE_INVALID and leaves the pointer alone, so the call
	// site crashes on a null function rather than reporting anything useful.
	#define XRPROC( inst, name ) \
		PFN_##name name = nullptr; \
		xrGIPA( inst, #name, (PFN_xrVoidFunction*)&name )

	// Refuse to call through a null pointer. A probe whose failure mode is an
	// access violation tells us nothing about the question it was written to
	// answer -- it just looks like the runtime broke.
	#define NEEDPROC( name ) \
		do { if ( !name ) { Say( "FAILED  runtime did not provide %s", #name ); \
		     return 1; } } while ( 0 )

	XRPROC( XR_NULL_HANDLE, xrEnumerateInstanceExtensionProperties );
	XRPROC( XR_NULL_HANDLE, xrCreateInstance );
	if ( !xrEnumerateInstanceExtensionProperties || !xrCreateInstance )
	{
		Say( "FAILED  could not resolve the base entry points" );
		return 1;
	}

	// ---- 3. Extensions ------------------------------------------------------
	Head( "Which Vulkan bindings does this runtime offer, 32-bit?" );
	uint32_t extCount = 0;
	xrEnumerateInstanceExtensionProperties( nullptr, 0, &extCount, nullptr );
	std::vector<XrExtensionProperties> exts( extCount, { XR_TYPE_EXTENSION_PROPERTIES } );
	xrEnumerateInstanceExtensionProperties( nullptr, extCount, &extCount, exts.data() );

	bool haveV1 = false, haveV2 = false;
	for ( const auto& e : exts )
	{
		if ( strcmp( e.extensionName, XR_KHR_VULKAN_ENABLE_EXTENSION_NAME ) == 0 )  haveV1 = true;
		if ( strcmp( e.extensionName, XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME ) == 0 ) haveV2 = true;
	}
	Say( "%u extensions offered", extCount );
	Say( "XR_KHR_vulkan_enable   %s", haveV1 ? "PRESENT" : "absent" );
	Say( "XR_KHR_vulkan_enable2  %s", haveV2 ? "PRESENT" : "absent" );
	if ( !haveV1 && !haveV2 )
	{
		Say( "FAILED  neither Vulkan binding is offered -- there is no path from DXVK." );
		return 1;
	}

	// ---- 4. Instance --------------------------------------------------------
	Head( "Create an XrInstance" );
	std::vector<const char*> enable;
	if ( haveV1 ) enable.push_back( XR_KHR_VULKAN_ENABLE_EXTENSION_NAME );
	if ( haveV2 ) enable.push_back( XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME );

	XrInstanceCreateInfo ici = { XR_TYPE_INSTANCE_CREATE_INFO };
	strcpy( ici.applicationInfo.applicationName, "SiN VR OpenXR probe" );
	ici.applicationInfo.applicationVersion = 1;
	strcpy( ici.applicationInfo.engineName, "sinvr" );
	ici.applicationInfo.engineVersion = 1;
	ici.applicationInfo.apiVersion = XR_MAKE_VERSION( 1, 0, 0 );
	ici.enabledExtensionCount = (uint32_t)enable.size();
	ici.enabledExtensionNames = enable.data();

	XrInstance instance = XR_NULL_HANDLE;
	if ( !Ok( xrCreateInstance( &ici, &instance ), "xrCreateInstance" ) )
		return 1;

	// From here on the instance exists, so these resolve against IT.
	XRPROC( instance, xrGetInstanceProperties );
	XRPROC( instance, xrGetSystem );
	XRPROC( instance, xrCreateSession );
	XRPROC( instance, xrDestroySession );
	XRPROC( instance, xrDestroyInstance );
	XRPROC( instance, xrEnumerateSwapchainFormats );
	NEEDPROC( xrGetSystem );
	NEEDPROC( xrCreateSession );

	XrInstanceProperties ip = { XR_TYPE_INSTANCE_PROPERTIES };
	if ( xrGetInstanceProperties && XR_SUCCEEDED( xrGetInstanceProperties( instance, &ip ) ) )
		Say( "runtime  \"%s\" %u.%u.%u", ip.runtimeName,
			 (unsigned)XR_VERSION_MAJOR( ip.runtimeVersion ),
			 (unsigned)XR_VERSION_MINOR( ip.runtimeVersion ),
			 (unsigned)XR_VERSION_PATCH( ip.runtimeVersion ) );

	// ---- 5. System ----------------------------------------------------------
	Head( "Get the HMD system (SteamVR must be running with a headset)" );
	XrSystemGetInfo sgi = { XR_TYPE_SYSTEM_GET_INFO };
	sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
	XrSystemId system = XR_NULL_SYSTEM_ID;
	if ( !Ok( xrGetSystem( instance, &sgi, &system ), "xrGetSystem" ) )
	{
		Say( "        If this is FORM_FACTOR_UNAVAILABLE, start SteamVR with the" );
		Say( "        headset awake and run again. It is not a DXVK problem." );
		return 1;
	}
	Say( "systemId %llu", (unsigned long long)system );

	// ---- 6. What does the runtime require of Vulkan? ------------------------
	Head( "Ask the runtime what it requires -- and compare with what DXVK built" );

	if ( haveV1 )
	{
		PFN_xrGetVulkanGraphicsRequirementsKHR reqFn = nullptr;
		PFN_xrGetVulkanGraphicsDeviceKHR       devFn = nullptr;
		PFN_xrGetVulkanInstanceExtensionsKHR   instExtFn = nullptr;
		PFN_xrGetVulkanDeviceExtensionsKHR     devExtFn = nullptr;
		xrGIPA( instance, "xrGetVulkanGraphicsRequirementsKHR", (PFN_xrVoidFunction*)&reqFn );
		xrGIPA( instance, "xrGetVulkanGraphicsDeviceKHR", (PFN_xrVoidFunction*)&devFn );
		xrGIPA( instance, "xrGetVulkanInstanceExtensionsKHR", (PFN_xrVoidFunction*)&instExtFn );
		xrGIPA( instance, "xrGetVulkanDeviceExtensionsKHR", (PFN_xrVoidFunction*)&devExtFn );

		if ( reqFn )
		{
			XrGraphicsRequirementsVulkanKHR gr = { XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN_KHR };
			if ( XR_SUCCEEDED( reqFn( instance, system, &gr ) ) )
			{
				Say( "Vulkan API wanted: min %u.%u.%u  max %u.%u.%u",
					 (unsigned)XR_VERSION_MAJOR( gr.minApiVersionSupported ),
					 (unsigned)XR_VERSION_MINOR( gr.minApiVersionSupported ),
					 (unsigned)XR_VERSION_PATCH( gr.minApiVersionSupported ),
					 (unsigned)XR_VERSION_MAJOR( gr.maxApiVersionSupported ),
					 (unsigned)XR_VERSION_MINOR( gr.maxApiVersionSupported ),
					 (unsigned)XR_VERSION_PATCH( gr.maxApiVersionSupported ) );

				// DXVK hardcodes this (dxvk_instance.h, DxvkVulkanApiVersion).
				// If it sits above the runtime's ceiling that is a SECOND
				// blocker, independent of extensions, and a harder one -- it
				// cannot be fixed by handing DXVK a longer extension list.
				const XrVersion dxvkAsks = XR_MAKE_VERSION( 1, 3, 0 );
				if ( dxvkAsks > gr.maxApiVersionSupported )
					Say( "WARNING: DXVK asks for Vulkan 1.3 (DxvkVulkanApiVersion), "
						 "which is ABOVE that ceiling." );
			}
		}

		// The extension lists are the whole reason this section exists. DXVK
		// picked its own; if the runtime needs one DXVK did not enable, that is
		// the wall, and it is better to read it here than to infer it from a
		// session failure.
		auto dump = [&]( const char* what, XrResult ( XRAPI_PTR* fn )( XrInstance, XrSystemId, uint32_t, uint32_t*, char* ) )
		{
			if ( !fn ) return;
			uint32_t n = 0;
			if ( XR_FAILED( fn( instance, system, 0, &n, nullptr ) ) || n == 0 )
				{ Say( "%s: (none)", what ); return; }
			std::vector<char> buf( n );
			if ( XR_FAILED( fn( instance, system, n, &n, buf.data() ) ) ) return;
			Say( "%s: %s", what, buf.data() );
		};
		dump( "required VkInstance extensions", instExtFn );
		dump( "required VkDevice   extensions", devExtFn );
		Say( "^ diff these against the \"Enabled extensions\" block in" );
		Say( "  SinEpisodes_laa_d3d9.log. Anything missing there is the blocker." );

		if ( devFn )
		{
			VkPhysicalDevice wanted = VK_NULL_HANDLE;
			const XrResult dr = devFn( instance, system, dx.desc.Instance, &wanted );
			if ( XR_SUCCEEDED( dr ) )
			{
				DescribePhysicalDevice( dx.desc.Instance, wanted, "runtime wants  " );
				DescribePhysicalDevice( dx.desc.Instance, dx.desc.PhysicalDevice, "DXVK is using  " );
				Say( wanted == dx.desc.PhysicalDevice
						 ? "MATCH -- same physical device."
						 : "MISMATCH -- different GPUs. Session creation will fail." );
			}
			else
			{
				Say( "xrGetVulkanGraphicsDeviceKHR -> %d %s", (int)dr, XrName( dr ) );
				Say( "The runtime would not inspect DXVK's VkInstance. Before" );
				Say( "reading that as \"OpenXR rejects DXVK\", check the required" );
				Say( "instance extensions above against DXVK's own log: on native" );
				Say( "Windows DXVK's OpenXR extension provider does NOTHING (it" );
				Say( "only loads wineopenxr.dll), so the instance is very likely" );
				Say( "just missing extensions nobody asked DXVK to enable." );
			}
		}
	}

	// ---- 7. THE ANSWER ------------------------------------------------------
	Head( "THE QUESTION: xrCreateSession with DXVK's device" );

	XrGraphicsBindingVulkanKHR bind = { XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR };
	bind.instance = dx.desc.Instance;
	bind.physicalDevice = dx.desc.PhysicalDevice;
	bind.device = dx.desc.Device;
	bind.queueFamilyIndex = dx.desc.QueueFamilyIndex;
	bind.queueIndex = 0;

	XrSessionCreateInfo sci = { XR_TYPE_SESSION_CREATE_INFO };
	sci.next = &bind;
	sci.systemId = system;

	XrSession session = XR_NULL_HANDLE;
	XrResult  sr = xrCreateSession( instance, &sci, &session );

	printf( "\n" );
	printf( "==============================================================\n" );
	if ( XR_SUCCEEDED( sr ) )
	{
		printf( "  RESULT: PASS -- the runtime ACCEPTED DXVK's VkDevice.\n" );
		printf( "  An OpenXR backend is viable. Remaining work is the\n" );
		printf( "  acquire/copy/release submit path, not a blocker.\n" );
	}
	else
	{
		printf( "  RESULT: FAIL -- xrCreateSession -> %d %s\n", (int)sr, XrName( sr ) );
		printf( "  The runtime would not take a device it did not help create.\n" );
		printf( "  Read step 6 above for WHICH requirement is unmet before\n" );
		printf( "  concluding anything: a missing device extension is fixable\n" );
		printf( "  in the DXVK fork; a rejected VkInstance is not.\n" );
	}
	printf( "==============================================================\n" );

	if ( XR_SUCCEEDED( sr ) )
	{
		Head( "Swapchain formats the runtime will accept" );
		uint32_t fmtCount = 0;
		if ( xrEnumerateSwapchainFormats &&
			 XR_SUCCEEDED( xrEnumerateSwapchainFormats( session, 0, &fmtCount, nullptr ) ) && fmtCount )
		{
			std::vector<int64_t> fmts( fmtCount );
			xrEnumerateSwapchainFormats( session, fmtCount, &fmtCount, fmts.data() );
			for ( uint32_t i = 0; i < fmtCount; ++i )
				Say( "VkFormat %lld%s", (long long)fmts[i],
					 (VkFormat)fmts[i] == dx.desc.Format ? "   <- matches our eye surface" : "" );
		}
		if ( xrDestroySession ) xrDestroySession( session );
	}

	if ( xrDestroyInstance ) xrDestroyInstance( instance );
	if ( dx.vr )     dx.vr->Release();
	if ( dx.device ) dx.device->Release();
	if ( dx.d3d9 )   dx.d3d9->Release();
	if ( dx.window ) DestroyWindow( dx.window );

	return XR_SUCCEEDED( sr ) ? 0 : 1;
}
