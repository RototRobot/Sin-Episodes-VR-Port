// ABI mirror of IDirect3DVR9, the interface our DXVK fork exports.
//
// The canonical definition lives in dxvk-patch/d3d9_vr.h. This copy uses opaque
// pointers instead of Vulkan types so the mod never needs vulkan.h, keeping
// sinvr.dll free of any DXVK or Vulkan SDK build dependency. Same discipline as
// the Source interface mirrors: layout must match exactly, nothing else.
//
// !! If d3d9_vr.h changes, change this too.
#pragma once

#include <windows.h>
#include <d3d9.h>

namespace sinvr {

// Field-for-field identical to D3D9_TEXTURE_VR_DESC, and deliberately also to
// the leading members of vr::VRVulkanTextureData_t, so it can be handed to
// IVRCompositor::Submit without translation.
struct D3D9VRTextureDesc
{
	uint64_t Image;            // VkImage
	void*    Device;           // VkDevice
	void*    PhysicalDevice;   // VkPhysicalDevice
	void*    Instance;         // VkInstance
	void*    Queue;            // VkQueue
	uint32_t QueueFamilyIndex;

	uint32_t Width;
	uint32_t Height;
	uint32_t Format;           // VkFormat
	uint32_t SampleCount;
};

// Mirrors IDirect3DVR9. Method order is the vtable contract.
struct IDirect3DVR9Mirror
{
	// IUnknown
	virtual HRESULT STDMETHODCALLTYPE QueryInterface( REFIID riid, void** ppv ) = 0;
	virtual ULONG   STDMETHODCALLTYPE AddRef() = 0;
	virtual ULONG   STDMETHODCALLTYPE Release() = 0;

	// IDirect3DVR9
	virtual HRESULT STDMETHODCALLTYPE GetVRDesc( IDirect3DSurface9* pSurface,
												 D3D9VRTextureDesc* pDesc ) = 0;
	virtual HRESULT STDMETHODCALLTYPE TransferSurface( IDirect3DSurface9* pSurface,
													   BOOL waitResourceIdle ) = 0;
	virtual HRESULT STDMETHODCALLTYPE LockDevice() = 0;
	virtual HRESULT STDMETHODCALLTYPE UnlockDevice() = 0;
	virtual HRESULT STDMETHODCALLTYPE LockSubmissionQueue() = 0;
	virtual HRESULT STDMETHODCALLTYPE UnlockSubmissionQueue() = 0;
	virtual HRESULT STDMETHODCALLTYPE WaitDeviceIdle() = 0;
};

// Exported from our DXVK d3d9.dll as Direct3DCreateVR9 @39.
using Direct3DCreateVR9Fn = HRESULT( __stdcall* )( IDirect3DDevice9* pDevice,
												   IDirect3DVR9Mirror** ppInterface );

} // namespace sinvr
