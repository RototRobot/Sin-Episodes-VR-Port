#pragma once

// IDirect3DVR9 -- hands the Vulkan handles behind a D3D9 surface to a VR runtime.
//
// OpenVR and OpenXR cannot accept a D3D9 texture. Because DXVK *is* Vulkan
// underneath, the eye buffers already exist as VkImages; this interface simply
// exposes them, so submission costs nothing -- no readback, no copy, and no
// D3D9Ex shared-surface workaround.
//
// The fields of D3D9_TEXTURE_VR_DESC are laid out to match the leading members
// of vr::VRVulkanTextureData_t exactly, so a filled desc can be handed straight
// to IVRCompositor::Submit.
//
// Adapted for DXVK 3.0.2 from the equivalent file in L4D2VR's DXVK 2.6.1 fork,
// trimmed to the handle-accessor role: that version calls back into the mod's
// own globals, which we deliberately avoid. SiN VR lives in its own DLL and
// reaches this interface through the exported Direct3DCreateVR9 below.

#include <d3d9.h>

#define VK_USE_PLATFORM_WIN32_KHR 1
#include <vulkan/vulkan.h>
#undef VK_USE_PLATFORM_WIN32_KHR

struct D3D9_TEXTURE_VR_DESC {
  uint64_t         Image;            // VkImage, as uint64_t to match OpenVR
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

MIDL_INTERFACE("b1c3f2d4-5a6e-4f70-9c81-2d3e4f5a6b7c")
IDirect3DVR9 : public IUnknown {
  // Fills pDesc with the Vulkan handles backing pSurface.
  virtual HRESULT STDMETHODCALLTYPE GetVRDesc(
          IDirect3DSurface9*    pSurface,
          D3D9_TEXTURE_VR_DESC* pDesc) = 0;

  // Transitions the surface to TRANSFER_SRC_OPTIMAL, which is the layout the
  // compositor requires on submit. Pass TRUE to also wait for the resource.
  virtual HRESULT STDMETHODCALLTYPE TransferSurface(
          IDirect3DSurface9* pSurface,
          BOOL               waitResourceIdle) = 0;

  virtual HRESULT STDMETHODCALLTYPE LockDevice() = 0;
  virtual HRESULT STDMETHODCALLTYPE UnlockDevice() = 0;

  // VkQueue is externally synchronized and DXVK's submit thread also uses it,
  // so the queue must be held across an OpenVR submit or the two race.
  virtual HRESULT STDMETHODCALLTYPE LockSubmissionQueue() = 0;
  virtual HRESULT STDMETHODCALLTYPE UnlockSubmissionQueue() = 0;

  virtual HRESULT STDMETHODCALLTYPE WaitDeviceIdle() = 0;
};

#ifdef _MSC_VER
struct __declspec(uuid("b1c3f2d4-5a6e-4f70-9c81-2d3e4f5a6b7c")) IDirect3DVR9;
#else
__CRT_UUID_DECL(IDirect3DVR9, 0xb1c3f2d4, 0x5a6e, 0x4f70, 0x9c, 0x81, 0x2d, 0x3e, 0x4f, 0x5a, 0x6b, 0x7c);
#endif

// Exported from d3d9.dll. Resolve with GetProcAddress and pass the live device.
extern "C" HRESULT __stdcall Direct3DCreateVR9(
        IDirect3DDevice9* pDevice,
        IDirect3DVR9**    ppInterface);
