#pragma once

#include "../vr/vr_backend.h"

// GLOBAL scope, deliberately -- same as stereo.h. Naming the type for the first
// time inside `namespace sinvr` would declare sinvr::IDirect3DDevice9, a
// different and incomplete type that shadows the real one everywhere this
// header is included.
struct IDirect3DDevice9;

namespace sinvr {

// Hooks IDirect3DDevice9::Present and, once bound, submits each frame to the
// compositor. Requires our DXVK d3d9.dll (the one exporting Direct3DCreateVR9).
//
// Safe to call with submitEnabled=false: the hook still installs and logs, which
// isolates "did we find the device" from "did submission work".
bool InstallD3D9PresentHook( IVRBackend* backend, bool submitEnabled );

unsigned long long D3D9PresentCount();

// Thread that last ran Present, so the stall watchdog knows where to look.
unsigned long D3D9PresentThreadId();
bool D3D9VRInterfaceBound();

// Copies the current backbuffer into the given eye's surface. Called once per
// eye pass by the stereo render loop.
void CaptureEye( int eye );

// The device the GAME is rendering with, or null before the first Present.
//
// Exposed so the menu cursor can draw into the backbuffer at the end of an eye
// pass. Nothing else should reach for it: the device is only safe to touch on
// the render thread, inside a pass, which is exactly where that one caller is.
IDirect3DDevice9* D3D9Device();

} // namespace sinvr
