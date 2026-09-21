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

      // Before the handle leaves DXVK. See PinImage.
      if (image->canRelocate())
        PinImage(image);

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
      // 3.0.2 renamed this: D3D9_VK_FORMAT_MAPPING::FormatColor became ::Format
      // (with ::FormatSrgb alongside it). L4D2VR's 2.6.1 fork still uses the old
      // name. Submit the linear format -- OpenVR is told the colour space
      // separately, via Texture_t::eColorSpace.
      pDesc->Format           = tex->GetFormatMapping().Format;
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

    // ---- A VkImage HANDED OUTSIDE DXVK MUST NOT MOVE -------------------------
    //
    // DXVK 2.5+ relocates images to defragment video memory, and to evict them
    // when over budget: a new VkImage is created, the contents copied, and the
    // old VkImage destroyed once DXVK's OWN work on it has finished. SteamVR's
    // work is not DXVK's. It copies from the handle GetVRDesc gave it -- so a
    // relocation leaves it copying from a destroyed image.
    //
    // That is the crash reported 2026-09-11 and 2026-09-21: ACCESS_VIOLATION
    // reading 0x1E0 in nvoglv32.dll, from vrclient.dll's vkCmdCopyImage. The
    // 09-21 minidump settled it: the copy's SOURCE -- our eye image -- was a
    // freed driver object (no vtable at +0, no memory bound), while SteamVR's
    // destination was intact.
    //
    // DXVK's own interop does this for the same reason (D3D11Device::LockImage):
    // stableGpuAddress takes the image out of relocation for good. Only the
    // stable-address bit is requested, so the fast path in
    // ensureImageCompatibility applies -- no barrier, no render pass ended --
    // and running it ahead of queued work on the high-priority queue is safe.
    // Synchronous, so the handle read after it is final.
    void PinImage(const Rc<DxvkImage>& image) {
      D3D9DeviceLock lock = m_device->LockDeviceExclusive();

      if (!image->canRelocate())
        return;

      auto chunk = m_device->AllocCsChunk();
      chunk->push([cImage = image] (DxvkContext* ctx) {
        DxvkImageUsageInfo usage;
        usage.stableGpuAddress = VK_TRUE;
        ctx->ensureImageCompatibility(cImage, usage);
      });
      m_device->InjectCsChunk(std::move(chunk), true);

      Logger::info(str::format("D3D9VR: pinned ", image->info().extent.width, "x",
        image->info().extent.height, " image against relocation -- it is being handed "
        "to the VR runtime", image->canRelocate() ? ", but it is STILL RELOCATABLE" : ""));
    }

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
