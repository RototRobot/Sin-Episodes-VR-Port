# DXVK patch — `IDirect3DVR9`

Everything SiN VR needs from DXVK is in this folder. Two new files, two one-line
edits. Re-applying to a fresh DXVK checkout takes under a minute, which is why
the patch is kept here rather than only inside the fork.

## Why

OpenVR cannot accept a D3D9 texture — it takes D3D11, OpenGL or Vulkan. Since
DXVK already *is* Vulkan underneath, the eye buffers exist as `VkImage`s
already; they just aren't reachable through the D3D9 API. `IDirect3DVR9` exposes
them, so submission is zero-copy: no readback, no D3D9Ex shared surfaces.

`D3D9_TEXTURE_VR_DESC` is laid out to match the leading members of
`vr::VRVulkanTextureData_t`, so a filled desc goes straight to
`IVRCompositor::Submit`.

## Apply

1. Copy `d3d9_vr.h` and `d3d9_vr.cpp` into `src/d3d9/`.

2. `src/d3d9/meson.build` — add to the `d3d9_src` list:

   ```
   d3d9_src = [
     'd3d9_main.cpp',
     'd3d9_vr.cpp',        <-- add
     'd3d9_interface.cpp',
   ```

3. `src/d3d9/d3d9.def` — add after `Direct3DCreate9Ex @38`:

   ```
   Direct3DCreateVR9 @39
   ```

## Provenance

Adapted for **DXVK 3.0.2** from the equivalent file in L4D2VR's **DXVK 2.6.1**
fork (`dxvk_new/src/d3d9/d3d9_vr.cpp`). Differences:

| | L4D2VR | here |
|---|---|---|
| Coupling | `#include "L4D2VR/game.h"`, reads mod globals | none — pure handle accessor |
| Device lock | `LockDeviceExclusive()` (their addition) | `LockDevice()` (stock 3.0.2) |
| Methods | 12, incl. overlay drawing and frame ownership | 7 |
| Consumer | mod compiled *into* d3d9.dll | separate `sinvr.dll` via `GetProcAddress` |

Verified present in DXVK 3.0.2: `D3D9CommonTexture::{Device,Desc,GetImage,
GetFormatMapping,GetMappingBufferSequenceNumber}`, `DxvkDevice::{handle,adapter,
instance,queues,lockSubmission,unlockSubmission,waitForIdle}`,
`D3D9DeviceEx::{LockDevice,Flush,SynchronizeCsThread,TransformImage,
WaitForResource}`, `DxvkImageCreateInfo::{sampleCount,mipLevels,numLayers,layout}`.

## The submission-queue lock matters

`VkQueue` is externally synchronized and DXVK's submit thread uses it too, so it
must be held across an OpenVR submit or the two race. `LockSubmissionQueue()`
drains the CS thread under the device lock, then takes the queue gate — after
which the device lock can be released, so the game may record the next frame
while the compositor consumes this one.
