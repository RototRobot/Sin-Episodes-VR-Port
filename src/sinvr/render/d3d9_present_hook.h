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

// ---- THE DESKTOP MIRROR IS A WINDOW OF OUR OWN ---------------------------
//
// Present is redirected into a small window the mod creates, via
// hDestWindowOverride, and DXVK scales the frame into it. The GAME window is
// never resized -- doing that corrupts the render, twice measured. See the long
// comment in the .cpp. Call this before the first Present.
//
//   fit      make a mirror sized to fit the desktop work area, aspect
//            preserved, when the render is larger than the screen.
//   height   an exact mirror height in pixels, or 0 for none. Overrides fit.
//   hide     additionally make the game window invisible (layered, alpha 1
//            -- not 0, which would make it click-through), without resizing it.
//   centred  slide the game window, never resize it, so its centre stays on its
//            monitor. Source re-centres the mouse there every frame, and an
//            off-screen centre reads as constant mouse movement -- shots high of
//            the laser dot. Independent of the mirror.
void ConfigureDesktopWindow( bool fit, int height, bool hideGameWindow,
							 bool keepCentred );

// The size the ENGINE renders at -- the backbuffer, which is NOT the window's
// client size once the mirror has been shrunk. VGUI and every other engine
// screen coordinate live in this space, so anything mapping into engine
// coordinates must ask here rather than calling GetClientRect.
void D3D9RenderSize( unsigned int& w, unsigned int& h );

// The window the GAME renders into, as resolved inside the Present hook, and
// the mirror window the mod made -- null when there is no mirror.
//
// Anything looking for "the game window" must ask here rather than enumerating
// the process's windows: since the mirror exists, an enumeration can and does
// pick the WRONG one, and it is the more likely of the two because it sits on
// top in Z order.
HWND D3D9GameWindow();
HWND D3D9MirrorWindow();

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
