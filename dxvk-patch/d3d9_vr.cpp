#include "../dxvk/dxvk_include.h"

#include "d3d9_vr.h"

#include "d3d9_include.h"
#include "d3d9_surface.h"
#include "d3d9_device.h"

namespace dxvk {

  class D3D9VR final : public ComObjectClamp<IDirect3DVR9> {

  public:

    D3D9VR(IDirect3DDevice9* pDevice)
      : m_device(static_cast<D3D9DeviceEx*>(pDevice)) {
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(
            REFIID  riid,
            void**  ppvObject) {
      if (ppvObject == nullptr)
        return E_POINTER;

      *ppvObject = nullptr;

      if (riid == __uuidof(IUnknown)
       || riid == __uuidof(IDirect3DVR9)) {
        *ppvObject = ref(this);
        return S_OK;
      }

      Logger::warn("D3D9VR::QueryInterface: Unknown interface query");
      return E_NOINTERFACE;
    }

    HRESULT STDMETHODCALLTYPE GetVRDesc(
            IDirect3DSurface9*    pSurface,
            D3D9_TEXTURE_VR_DESC* pDesc) {
      if (unlikely(pSurface == nullptr || pDesc == nullptr))
        return D3DERR_INVALIDCALL;

      D3D9Surface* surface = static_cast<D3D9Surface*>(pSurface);
      const auto* tex = surface->GetCommonTexture();

      if (unlikely(tex == nullptr))
        return D3DERR_INVALIDCALL;

      const auto& image = tex->GetImage();
      if (unlikely(image == nullptr))
        return D3DERR_INVALIDCALL;

      const auto* desc   = tex->Desc();
      const auto& device = tex->Device()->GetDXVKDevice();

      // OpenVR types VkImage as a uint64_t in its Vulkan texture data.
      pDesc->Image            = uint64_t(image->handle());
      pDesc->Device           = device->handle();
      pDesc->PhysicalDevice   = device->adapter()->handle();
      pDesc->Instance         = device->instance()->handle();
      pDesc->Queue            = device->queues().graphics.queueHandle;
      pDesc->QueueFamilyIndex = device->queues().graphics.queueIndex;

      pDesc->Width            = desc->Width;
      pDesc->Height           = desc->Height;
      pDesc->Format           = tex->GetFormatMapping().FormatColor;
      pDesc->SampleCount      = uint32_t(image->info().sampleCount);

      return D3D_OK;
    }

    HRESULT STDMETHODCALLTYPE TransferSurface(
            IDirect3DSurface9* pSurface,
            BOOL               waitResourceIdle) {
      if (unlikely(pSurface == nullptr))
        return D3DERR_INVALIDCALL;

      D3D9DeviceLock lock = m_device->LockDeviceExclusive();

      auto* tex = static_cast<D3D9Surface*>(pSurface)->GetCommonTexture();
      if (unlikely(tex == nullptr))
        return D3DERR_INVALIDCALL;

      const auto& image = tex->GetImage();
      if (unlikely(image == nullptr))
        return D3DERR_INVALIDCALL;

      VkImageSubresourceRange subresources = {
        VK_IMAGE_ASPECT_COLOR_BIT,
        0, image->info().mipLevels,
        0, image->info().numLayers
      };

      m_device->TransformImage(
        tex, &subresources,
        image->info().layout,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);

      if (waitResourceIdle)
        m_device->WaitForResource(*image, tex->GetMappingBufferSequenceNumber(0u), D3DLOCK_READONLY);

      return D3D_OK;
    }

    HRESULT STDMETHODCALLTYPE LockDevice() {
      m_lock = m_device->LockDeviceExclusive();
      return D3D_OK;
    }

    HRESULT STDMETHODCALLTYPE UnlockDevice() {
      m_lock = D3D9DeviceLock();
      return D3D_OK;
    }

    HRESULT STDMETHODCALLTYPE LockSubmissionQueue() {
      // Drain DXVK's CPU command stream while no other D3D9 call can append to
      // it, then take the queue's external-synchronization gate. After this
      // returns the device lock may be released -- the game can record the next
      // frame, but DXVK's submit thread cannot touch VkQueue until the eye
      // textures have gone to the compositor.
      D3D9DeviceLock lock = m_device->LockDeviceExclusive();
      m_device->Flush();
      m_device->SynchronizeCsThread(DxvkCsThread::SynchronizeAll);
      m_device->GetDXVKDevice()->lockSubmission();
      return D3D_OK;
    }

    HRESULT STDMETHODCALLTYPE UnlockSubmissionQueue() {
      m_device->GetDXVKDevice()->unlockSubmission();
      return D3D_OK;
    }

    HRESULT STDMETHODCALLTYPE WaitDeviceIdle() {
      // Callable from the present path while the game's material thread is
      // live. The D3D9 command chunk is device-owned mutable state, so flushing
      // without the same lock the draw calls use can race them.
      D3D9DeviceLock lock = m_device->LockDeviceExclusive();

      m_device->Flush();
      m_device->SynchronizeCsThread(DxvkCsThread::SynchronizeAll);
      m_device->GetDXVKDevice()->waitForIdle();
      return D3D_OK;
    }

  private:

    D3D9DeviceEx*  m_device;
    D3D9DeviceLock m_lock;

  };

}

extern "C" HRESULT __stdcall Direct3DCreateVR9(
        IDirect3DDevice9* pDevice,
        IDirect3DVR9**    ppInterface) {
  if (pDevice == nullptr || ppInterface == nullptr)
    return D3DERR_INVALIDCALL;

  // Whether D3DCREATE_MULTITHREADED was requested decides whether stock DXVK's
  // LockDevice() locks anything at all. The VR path no longer depends on it --
  // it uses LockDeviceExclusive -- but knowing which case we are in is the
  // difference between "that race was impossible" and "that race was wide open".
  const bool multithreaded =
    static_cast<dxvk::D3D9DeviceEx*>(pDevice)->IsDeviceMultithreaded();

  dxvk::Logger::info(multithreaded
    ? "D3D9VR: device is D3DCREATE_MULTITHREADED"
    : "D3D9VR: device is NOT D3DCREATE_MULTITHREADED -- "
      "stock LockDevice() locks nothing on this device");

  *ppInterface = new dxvk::D3D9VR(pDevice);
  return D3D_OK;
}
