# SiN VR

A virtual reality mod for **SiN Episodes: Emergence**, adding stereoscopic
rendering, 6DoF head tracking and motion controller support to a game that
shipped in 2006 with no idea any of that would ever exist.

It runs as an injected DLL alongside a small fork of DXVK, and Launcher for LAA and Resolution settings

[![License: Apache 2.0](https://img.shields.io/badge/License-Apache_2.0-blue.svg)](LICENSE)

## Video

[![Trailer](https://img.youtube.com/vi/HUOkcr81XGo/0.jpg)](https://youtu.be/HUOkcr81XGo)

The first few minutes of the game's intro, and a cut of some late-game
gameplay.

> [!NOTE]
> **This mod was made with heavy AI (Claude) assistance.** I want to be upfront
> about that.

> [!IMPORTANT]
> **Windows may block this mod from running.**
>
> **Smart App Control** (Windows 11) refuses unsigned programs outright. This
> mod is unsigned, so on a machine where SAC is enforced the game will start and
> the VR mod simply will not load.
>
> **Antivirus exclusions do not help.** SAC is a separate system with no
> exclusion list, no "allow once", and no per-file unblock. The only options are
> to sign the binaries with a trusted certificate or to turn SAC off:
>
> `Windows Security → App & browser control → Smart App Control → Off`
>
> Ordinary antivirus may also object, independently. What this mod does —
> injecting a DLL and writing into another process — is mechanically
> indistinguishable from what malware does. That one can be fixed with an
> exclusion.

---

## About the game

**SiN Episodes: Emergence** (Ritual Entertainment, 2006) was the first — and, as
it turned out, only — instalment of a planned episodic FPS series built on
Valve's Source engine. Ritual was acquired shortly after release and the
remaining episodes were never made, which leaves Emergence a roughly
2 to 3 hour shooter that ends on a cliffhanger nobody ever resolved.

This was a good fit for a VR project as the Source 2004 SDK is available and the existing
Left 4 Dead 2 and Portal 2 VR mods to use for heavy reference.

---

## Requirements

- **SiN Episodes: Emergence** on Steam
- A SteamVR-compatible headset with two motion controllers
- **SteamVR** installed and running
- A GPU with Vulkan support (DXVK translates the game's Direct3D 9 to Vulkan)
- Windows

This mod was developed and tested on a Vive Cosmos and an HP Reverb G2. However it should work on any OpenVR compatible Headset
as nothing was developed to be specific to either. Every headset is queried from the runtime at
startup, including per-eye field of view, IPD and render resolution.

---

## Features

### Rendering

- **Stereoscopic rendering** — The Game is running Native Stereo Images once per eye at 90 fps Not AER or any other alternative methods.
- **Per-eye asymmetric projection** taken from the runtime, so the image is
  geometrically correct rather than an approximation
- **Automatic render resolution** — the game window is sized from the headset's
  own recommendation, so SteamVR's resolution slider is the control and no
  numbers are hard-coded to one headset
- **Correct world geometry** — the engine's culling frustum is widened to match
  the much wider VR field of view, which is what stops walls and ceilings
  vanishing
- **6DoF head tracking**, collided against the world so you cannot lean or step
  through walls

### Weapons and hands

- **Aim decoupling** — the weapon hand aims; the head only decides what you see
- **Weapon pinned to the controller**, with per-weapon position and angle offsets
- **Two-handed weapons** — bring the off hand to the foregrip and the gun points
  along the line between your hands
- **Melee by gesture** — a downward swing of the weapon hand will do a Bash.
- **Arcade reload** — drop the weapon hand to your waist to reload. this setting will disable button reloading but also can be turned off.
- **Holster zones** — reach to your left shoulder, right shoulder or hip and grip
  to draw that weapon directly. The three zones cover SiN's three guns, so grip
  does nothing elsewhere and cannot swap your weapon by accident. Tuned per hand
  and built in, so left-handed mode needs no extra setup
- **Grenades on the off hand's grip**, which nothing else was using — the one
  spare input on a controller that has none
- **Recoil compensation**, optional, off by default because it is a balance change
- **Shots leave the gun, not your face** — the bullet starts at the muzzle and
  travels down the barrel, so sighting works at every range and **you can shoot
  round a corner** with the gun past it and your head still behind. Per-weapon
  muzzle calibration ships tuned; `shot_from_gun`

### Movement and comfort

- **Smooth or snap turning**
- **Physical crouch** — duck by physically ducking
- **Swimming** is directed by the headset rather than the weapon hand
- **Left-handed mode** Swaps the gun hand and will auto mirrors every button pair
- **Thumbstick swap**, If you prefer to use the Right handed thumb stick on leftie mode.
- **True room-scale walking** — walk in your play space and the player character
  walks with you through the level, collided by the engine's own movement code.
  Lean freely inside a deadzone so you can still peek round cover without
  committing your body; step past it and your body follows. `sixdof_body`
- **Room-scale lean** within a configurable radius

### Interface

- **Menus on a world panel** — the 2D menu is projected onto a quad standing in
  front of you at a real distance, with real per-eye depth
- **Point-and-click menus** — a controller drives the cursor by tracing against
  that same panel
- **A cursor that works everywhere**, including in-game pause menus and over
  opaque dialogs, drawn directly into the eye image
- **Body-anchored HUD**, the game's HUD placed relative to your body
  rather than welded to your face.
- **World-space laser crosshair**, optional, traced from the muzzle along the
  same ray the bullet travels — a readout of the shot rather than an
  approximation of it. It draws **the weapon's own reticle**, taken from the
  game's crosshair font rather than redrawn by eye

---

## Installation

> [!IMPORTANT]
> **Switch the game to its `loose` Steam branch first** — right-click SiN
> Episodes: Emergence → Properties → Betas → `loose`. On the default branch the
> game reads its content from the packed archives in `vpks\` and ignores loose
> files with the same name, so the arm-hiding `hands.vmt` and both optional
> folders silently do nothing. The mod itself runs on either branch; those
> content overrides only work on `loose`. `sinvr_launcher.exe` and `sinvr.log`
> both warn when the game is on the default branch.

### 1. Build or obtain the files

You need six files plus one folder, all of which `build.bat` produces in
`build\` — plus `uninstall_sinvr.bat`, which ships in the release package:

| file | what it is |
|---|---|
| `sinvr.dll` | the mod |
| `dinput8.dll` | a proxy DLL, which is how the mod gets loaded |
| `d3d9.dll` | the DXVK fork (see *Building* below) |
| `openvr_api.dll` | OpenVR, 32-bit |
| `openvr_api_dxvk.dll` | OpenVR for the DXVK side |
| `sinvr_launcher.exe` | the launcher (see below) |
| `actions\` | the OpenVR action manifest and default controller bindings |
| `uninstall_sinvr.bat` | removes all of the above again — see [Uninstalling](#uninstalling) |

### 2. Copy them into the game folder

Put all of the above **next to `SinEpisodes.exe`**, typically:

```
C:\Program Files (x86)\Steam\steamapps\common\SiN Episodes Emergence\
```

The `actions` folder goes there as a folder, not as loose files.

### 3. Set the Steam launch options

Right-click the game in Steam → Properties → Launch Options, and set:

```
"C:\Program Files (x86)\Steam\steamapps\common\SiN Episodes Emergence\sinvr_launcher.exe" %command% -novid -windowed
```

Adjust the path if your Steam library is elsewhere. Then launch the game
normally from Steam.

> [!WARNING]
> **Do not add `-w` / `-h`.** The launcher asks the headset what resolution it
> wants and sets them itself. An explicit `-w`/`-h` always wins, which is useful
> for testing but defeats the automatic sizing — you get a low-resolution image
> in VR with nothing obviously wrong. The launcher says so on startup if it
> happens.

#### Why a launcher?

`SinEpisodes.exe` ships without the `LARGE_ADDRESS_AWARE` flag, which limits it
to 2 GB of address space — not enough once stereo rendering and VR buffers are
added, and the symptom is a slow collapse over several minutes ending in a
crash. The launcher makes a patched **copy** (`SinEpisodes_laa.exe`) with the
flag set and runs that. The shipped executable is opened read-only and never
modified.

It also queries SteamVR for the render resolution before the game starts, which
is why the game is born at the right size instead of having to change mode.

### 4. Optional content overrides

Two changes touch the game's own content. Neither is applied automatically.
Both are reversible, and both are undone by Steam's "verify integrity of game
files".

**Hide the viewmodel arms** — recommended once the weapon is pinned to a
controller, because otherwise the arms stretch from your face to wherever your
hand is. Copy from `content\`:

```
materials\models\weapons\v_hands\hands.vmt.hidden
  ->  SE1\materials\models\weapons\v_hands\hands.vmt
```

**Stop the gun auto-reloading when empty** — only wanted alongside
`arcade_reload = 1`. Run `tools\set_noautoreload.ps1 -Apply` (and `-Revert` to
undo). Do not apply it until the reload gesture is enabled, or you will have
neither.


---

## Controls

Defaults for Oculus Touch-style controllers (also WMR and Pimax for example)

`left_handed = 1` mirrors every pair. Both grips carry the same binding and the
mod reads them per hand, so they follow it too, with no rebinding.

| control | action |
|---|---|
| Left stick | Move |
| Right stick | Turn |
| Right stick, pushed down | Crouch (`turn_stick_crouch`) |
| Right stick, pushed up | Throw grenade — alternative, off by default (`turn_stick_grenade`) |
| Left stick click | Open / close menu (double-click by default) |
| Right stick click | Recentre |
| Right trigger | Fire |
| Left trigger | Alt fire |
| Right A | Jump |
| Right B | Use |
| Left X | Reload |
| Left Y | Flashlight |
| Weapon-hand grip | Draw the weapon you are reaching for (holster zones) |
| Off-hand grip | Throw grenade, or the two-handed foregrip — see below |

### Grenades on Touch-style controllers

Every face button, trigger and stick on a Touch-style controller is already
spoken for, and the inputs technically still free — the thumbrest, the
capacitive touch on the face buttons — do not survive the WMR remap.

**The off hand's grip carries it**, because nothing else was using it: the
weapon hand's grip draws from a holster zone, and the other one was idle. On by
default, no rebinding.

**It stands down when the foregrip wants that grip.** With
`two_handed_mode = toggle` or `hold` the grip *is* the two-handed control, and
throwing a grenade every time you steadied your rifle would be worse than having
no grenade button. Use the turn stick in those modes — its vertical axis is
otherwise unused, and crouch already takes the bottom half:

```
off_hand_grenade  = 1      # off-hand grip, unless two_handed_mode takes it
turn_stick_grenade = 1     # push the turn stick UP instead. Works everywhere
turn_stick_threshold = 0.65
```

`sinvr.log` says which is live on every launch, so you never have to guess.

Knuckles, Cosmos and Vive wands need neither — grenade already has the Index
trackpads, the Cosmos bumpers and the wand's left trackpad click.

There is also a number of numpad tuning mode for more fine tweaking if needed but these are mostly technical — 
if you want to adjust weapon positions or body zones by hand.

---

## Configuration

Settings live in **`sinvr.cfg`**, next to `SinEpisodes.exe`. It is generated on
first run, and every key is documented in the file itself with the reasoning
behind it.

**The Safest VR features are on by default.** A fresh install aims with the weapon
hand, pins the gun to the controller, and has melee, holsters, physical crouch,
the two-handed grip and the body-anchored HUD enabled. If any of it is not to
your taste, each is one line to turn off.

Two things are deliberately still off, because both are choices rather than
improvements: `arcade_reload` (it disables the reload button) and
`recoil_compensation` (it is a balance change).

### The settings most worth knowing

| setting | default | what it does |
|---|---|---|
| `left_handed` | `0` | Swaps the aiming hand **and** mirrors every button pair |
| `swap_thumbsticks` | `0` | Swaps move and turn sticks; composes with `left_handed` |
| `aim_source` | `controller` | The weapon hand aims. `hmd` reverts to the head aiming, as the flat game does |
| `movement_direction` | `hmd` | What "forward" means when you push the stick |
| `controller_snap_turn` | `0` | Snap turning instead of smooth |
| `melee_gesture` | `1` | Swing the weapon hand down to bash |
| `holster_zones` | `1` | Reach to your body and grip to draw a weapon |
| `off_hand_grenade` | `1` | Off-hand grip throws grenades. Turns itself off if `two_handed_mode` is `toggle` or `hold` |
| `holster_grip_cycles` | `0` | Whether a grip *outside* every zone also cycles weapons. Off, so grip is holster-only — SiN's three guns are covered by the three zones. `1` restores cycling |
| `two_handed` | `1` | Two-handed weapon grip |
| `physical_crouch` | `1` | Duck by physically ducking |
| `hud_anchor` | `body` | HUD placed relative to your body. `off` welds it to your view |
| `viewmodel_follow_controller` | `1` | Pins the gun to your hand |
| `arcade_reload` | `0` | Drop the weapon hand to your waist to reload. **Unbinds the reload button** — see the content override above |
| `recoil_compensation` | `0.0` | 0–1. Cancels weapon kick. A balance change, hence off |
| `laser_dot` | `0` | World-space laser crosshair, on the shot's own ray |
| `shot_from_gun` | `1` | Bullets leave the muzzle instead of your eye. Lets you shoot round corners |
| `sixdof_body` | `1` | Room-scale walking — your body follows you through the play space |
| `sixdof_deadzone` | `12.0` | How far you may lean before your body follows |
| `hand_marker` | `off` | A visible marker on the off hand |
| `positional_max_offset` | — | How far you may physically lean from centre |
| `positional_collide` | `1` | Stops leaning through walls |
| `vr_resolution_scale` | `1.0` | Multiplier on the headset's recommended resolution. **The first performance lever** — the scene renders twice, so cost goes with the square |
| `world_scale` | `39.37` | Source units per metre. Raise to feel smaller, lower to feel larger |

### Where the gun sits in your hand

A controller's tracked origin is not where a gun's grip is, and no two people
hold one the same way. The shipped values were measured on a Vive Cosmos with a
right-handed grip and are a **starting point**.

The global offsets, in Source units, applied to whichever weapon has no
per-weapon values of its own:

```
viewmodel_offset_forward = -26.0    - back toward you / + away
viewmodel_offset_right   = -10.0    - left / + right
viewmodel_offset_up      = 6.0      - down / + up
viewmodel_angle_pitch    = 58.0     the controller-to-weapon tilt, in degrees
viewmodel_angle_yaw      = 0.0
viewmodel_angle_roll     = 0.0
```

`viewmodel_angle_pitch` is the big one: a controller's
tracked axis points nowhere near the direction it feels like it points, and the
same 58 degrees is applied to the **aim** as well as the model, so the shot and
the barrel agree.

**Each weapon has its own position out of the box**, because the three
viewmodels are authored with different origins and genuinely need different
offsets — the shotgun sits 16 units further forward than the pistol. Those
per-weapon values are built into the mod and are what you get on a fresh
install; the global offsets above are only a fallback for a weapon with no
values of its own.

The per-weapon keys are **absent from the generated cfg on purpose**, and their
absence does *not* mean "use the global" — it means "use the built-in value".
Add them only to override:

```
viewmodel_offset_forward_v_magnum         viewmodel_offset_right_v_magnum         viewmodel_offset_up_v_magnum
viewmodel_offset_forward_v_assault_rifle  viewmodel_offset_right_v_assault_rifle  viewmodel_offset_up_v_assault_rifle
viewmodel_offset_forward_v_scattergun     viewmodel_offset_right_v_scattergun     viewmodel_offset_up_v_scattergun
```

#### Adjusting it in the headset

Editing numbers, restarting, and looking again is a bad way to do this. Set:

```
viewmodel_adjust_enabled = 1
```

and the numpad moves **whichever gun is currently in your hands**, live:

| key | effect |
|---|---|
| `8` / `2` | forward / back |
| `4` / `6` | left / right |
| `9` / `3` | up / down |
| `7` | switch between moving the **position** and the **angles** |
| `+` / `-` | bigger / smaller step |
| `5` | reset this weapon (press twice) |
| `0` | **write the current values into `sinvr.cfg`** |

The same six movement keys do position or angles depending on which mode
numpad `7` has you in.

Numpad `0` is the important one — it saves, so a tuning session ends with
something durable rather than a feeling. Switch weapons in game and repeat for
each one. Turn `viewmodel_adjust_enabled` back to `0` when you are happy, or the
numpad keeps moving the gun.

The body zones — melee, holsters and the reload box — tune the same way with
`zone_adjust_enabled = 1` and `zone_boxes_visible = 1`, which draws them so you
can see what you are moving.

### Where the shot leaves the gun

Separate from the above, and worth knowing about only if the shot does not line
up with the barrel. Because bullets now leave the **muzzle**, the mod needs to
know where each weapon's muzzle is relative to your controller — five numbers
per gun, all shipped tuned.

Turn on the aim line to see it:

```
aim_debug = 1
```

A green line is drawn from the exact point the shot leaves, along the exact
direction it travels. It is not an illustration of the shot; it **is** the shot.
It should leave the muzzle and run down the barrel.

| key | effect |
|---|---|
| `'` | cycle axis — yaw, lateral, pitch, up, forward |
| `,` / `.` | adjust the current axis |
| `/` | **save all five into `sinvr.cfg`** for the current weapon |

Yaw and pitch aim the line; lateral, up and forward move where it starts.
Forward is the one that decides how far past a corner your shot begins.

Values are stored per weapon **and per hand**, so a left- and right-handed
calibration can coexist in one file. Turn `aim_debug` back to `0` when done.

### Settings you should leave alone

A few keys exist for diagnosing the mod rather than configuring it.

**`menu_gating`** is the one worth understanding, because its name suggests it is
optional and it is not.

The problem it solves: **the main menu runs a background map**, so the engine
reports "in game" while you are sitting in the menu. Every gameplay gate in the
mod keys off that, which meant on the menu the weapon hand was steering the view,
gesture detectors could fire, and the laser traced scenery behind the panel.
`menu_gating` uses the OS cursor — which Source shows for menus and hides during
play — to detect a menu and hold all of that off.

Setting it to `0` disables the whole mechanism, and **breaks the main menu**: the
camera then treats the background map's scripted camera as your turn input, the
body yaw absorbs exactly enough to cancel your head, and the view locks in yaw
while pitch and roll keep tracking. You cannot then point at the menu to start a
level.

It was originally a kill switch in case menu detection went wrong and left
gameplay dead. That escape hatch is now automatic — pushing the movement stick
overrules a wrongly-detected menu — so there is no longer a reason to touch it.

The `*_debug` keys, `zone_boxes_visible`, `trace_eye_passes` and the
`relax_cull_*` family are similar: instruments, not settings.

---

## Building from source

### Prerequisites

- **Visual Studio 2017 or newer** with the C++ x86 build tools. The build script
  finds it via `vswhere`, so no paths need editing.
- **The OpenVR SDK** checked out next to this folder, so that
  `..\openvr-master\headers\openvr.h` exists. Headers only — `openvr_api.dll` is
  loaded at runtime, never linked.
- **Python 3** for the config audit tools (optional).
- For the DXVK fork: **meson** and **ninja** (`pip install meson ninja`), and the
  Vulkan SDK.

Everything is built **32-bit**. `SinEpisodes.exe` is a 32-bit process, so every
DLL injected into it must be x86 regardless of the host machine.

### Build the mod

```
build.bat
```

This produces `sinvr.dll`, `dinput8.dll`, `sinvr_launcher.exe` and stages
`actions\` into `build\`.

> [!CAUTION]
> Run this through PowerShell or a normal command prompt. Invoking it as
> `cmd /c build.bat` from a POSIX shell (Git Bash, WSL) **hangs and has to be
> killed**.

### Build the DXVK fork

```
build_dxvk.bat
```

The fork adds one interface (`Direct3DCreateVR9`) that hands out the Vulkan image
behind a Direct3D 9 surface, which is what lets frames be submitted to the
compositor.

> [!NOTE]
> **The DXVK tree is not in this repository.** It is around 900 MB and contains
> a single file past GitHub's limit, so it is git-ignored rather than committed.
>
> What this project adds is in **`dxvk-patch\`**, which is a few hundred lines.
> To build it yourself, clone [DXVK](https://github.com/doitsujin/dxvk) into
> `dxvk-sinvr\`, apply those files, then run `build_dxvk.bat`. Everything else
> in the mod builds without it — you only need this if you want to rebuild
> `d3d9.dll` rather than use the one in the release.

Run `build_dxvk.bat clean` once on a new machine to reconfigure from scratch.

The shipped `d3d9.dll` is a **modified** DXVK build. Problems with it are this
project's, not upstream's — see `licenses\THIRD-PARTY-NOTICES.txt` in the
release package.

### Verify

```
py -3 tools\audit_config.py
```

Cross-checks that every config key is declared, read, and present in the
generated defaults. It has caught real bugs more than once.

---

## Known limitations

None of these break the game. They are places where the mod stops short of what
you might reasonably expect, usually because the fix costs more than the problem
is worth. Each one says what you can do about it, where anything can be.

> [!TIP]
> Entries **struck through** were limitations in older versions and now work.
> They are kept so that anyone who read the old list, or an old thread, can see
> they are dealt with.

### Soft Lock entering a Car

this is the most Severe issue, sometimes when you enter a car it will just 
eject you, I was unable to find the cause, in any case there are only two
instances of getting into a car so the workaround for now is just to make 
a save before hand.

### ~~It is not true room-scale~~ — FIXED

Walking in your play space now walks the player character through the level. It
runs through the server's own movement code, so the engine does the collision,
enemies react to where you really are, and shots leave from there too.

Your view does not move while this happens: the body catches up to your head and
the head gives back exactly the distance the body covered, so the two cancel.
That is what keeps it from causing sickness.

Leaning still works and is deliberately separate: inside `sixdof_deadzone` your
head moves and your body stays put, so you can put an eye past a corner without
walking your hitbox into the open. Step further and the body follows — all the
way, so when you stop walking your body is back under your head rather than
parked 30 cm short of it. (Older builds stopped it at the deadzone edge, which is
why walls could feel too far away, or too close, until you recentred.)

The deadzone is wider **sideways** than forward, because a lean goes sideways and
a walk goes forward — and tilting your head into a lean buys more room still,
since leaning tips your head over and stepping sideways keeps it upright. When
the body does start to follow, it gets up to speed over a third of a second
rather than all at once.

| setting | default | what it does |
|---|---|---|
| `sixdof_deadzone` | `12.0` | How far you may lean **sideways** before the body follows (~30 cm). `0` makes every lean commit it |
| `sixdof_forward_ratio` | `0.65` | Forward and back get this fraction of the sideways room. `1.0` is a plain circle |
| `sixdof_lean_tilt` | `0.5` | Extra sideways room per degree your head tilts into the lean. `0` ignores tilt |
| `sixdof_settle` | `1.0` | Once the body follows, how close it comes before it stops |
| `sixdof_ramp` | `0.3` | Seconds for the body to get up to speed. `0` starts it at full speed |
| `sixdof_chase` | `0.15` | How quickly the body closes the gap. Higher tracks you tighter, lower is smoother |
| `server_movement` | `1` | Required by room-scale, and the only part of the mod that writes to `server.dll`. Turning it off disables room-scale — but **not** `shot_from_gun`, which hooks the engine's trace independently. Set both to `0` to disable everything that hooks or writes |

### ~~The render resolution is capped by your desktop~~ — FIXED

The engine used to refuse a window larger than your display, which clamped the
render to your monitor and made **SteamVR's resolution slider do nothing above
that point**. The refusal came through Direct3D, and this mod ships its own
Direct3D build, so it can report a display large enough for the window the
headset asks for:

```
vr_allow_oversize_window = 1
```

On the development machine that took the render from 2206×2160 to the headset's
full 2444×2392. `sinvr.log` reports the requested and actual sizes so you can
confirm they match:

```
resolution: rendering 2444x2392 per eye | runtime recommends 2444x2392
```

It matters most on a **small monitor** — on a 1080p desktop the old ceiling meant
you could not reach a modern headset's native resolution at all.

- **Start through `sinvr_launcher.exe`.** That was always the rule and now it
  matters more — the launcher is what tells Direct3D to accept the larger
  window, so starting any other way fails to set the video mode.
- **How much to render is still SteamVR's slider**, which is live again above the
  old ceiling. Watch `stereo frame cost` in `sinvr.log` — cost climbs quickly.

Raising the desktop resolution with NVIDIA DSR or AMD VSR still works, but is no
longer necessary.

### ~~The game window hangs off the edge of your screen~~ — FIXED

Rendering at the headset's resolution means a game *window* that size too, which
on most monitors ran off the bottom and right, had no reachable title bar, and
showed a magnified corner of the frame rather than a picture of it.

The desktop now gets its own small **mirror window** instead, sized to fit your
screen with the aspect kept. The mod's Direct3D build scales the finished frame
into it on the way out, so the render, the eye buffers and the image sent to
SteamVR all stay at full resolution. The big game window is left exactly as it
was — same size, same place — and made invisible. Source reads that window's
size and uses it to scope part of its render, which is why two earlier attempts
that *resized* it broke the headset image.

Menus still work with the controller pointer everywhere, including buttons that
lie past the edge of your monitor: the pointer talks to the game directly rather
than moving the Windows cursor, which Windows will not move off the desktop.

The big window is also slid — never resized — so its middle stays on your
monitor. Source re-centres the mouse on the middle of its window every frame; on
a 1080p screen that middle was below the bottom edge, Windows clamped the cursor
there, and the game read it as the mouse moving up. Shots landed about 10° above
the laser dot with nobody touching the mouse.

| setting | default | what it does |
|---|---|---|
| `vr_desktop_window_fit` | `1` | Show the game in a small mirror window that fits your desktop. `0` goes back to the full-size window |
| `vr_desktop_window_hide` | `1` | Hide the full-size game window so only the mirror shows. Needs the mirror on |
| `vr_desktop_window_height` | `0` | An exact mirror height in pixels, aspect preserved. Overrides the fit |
| `vr_game_window_centred` | `1` | Keep the full-size window's middle on your monitor, so the mouse re-centre cannot push your aim |
| `menu_pointer_direct` | `1` | Menu pointing that reaches past the edge of the screen. `0` is the old cursor-moving route |

### Menus and dialogs get small at high resolutions

Once you are rendering at your headset's full resolution, SiN's own Load, Save
and Options dialogs become hard to read. They are laid out in fixed pixel sizes
that were chosen for a 1024×768 monitor, so the higher you render the smaller a
share of the screen they take — the Load dialog goes from 43% of the screen
height at 1080 to 19% at 2392. The game has no setting for this and the dialogs
cannot scale themselves.

There is a tool for it:

```bash
py -3 tools/scale_menu_ui.py --for-height 2392 --apply
```

Use the **per-eye render height** the launcher prints at startup (`rendering
2444x2392 per eye` → `2392`). Run it with no arguments first to see what it
would do, and `--revert` puts everything back.

Like the hidden-arms override, **this edits game content**, so it is not applied
for you. It keeps every original as `.sinvr-original`, refuses to run twice
without a revert so scales cannot compound, and Steam's "verify integrity of
game files" undoes it.

Two known limits: the **save-game thumbnails do not scale**, so they sit with
black padding inside the larger boxes; and the in-game HUD's fonts are left
alone unless you add `--hud-fonts`, because the HUD is a separate thing from
the menus.

### ~~The Save and Load screens shimmer~~ — FIXED

Those two screens stack several panels at the same depth, which made them fight.
They are now separated by a depth bias applied only to the interface.

A scrollbar in the Load list can still flicker very slightly. Raise
`menu_depth_scale` (default `0.25`) if it bothers you.

### There are no hands

The arms are hidden, so the weapon floats.SiN has no standalone hand model at all.
Each weapon's viewmodel is a single mesh with the arms baked into it,
 which is why they can only be hidden by material and not posed to your controller.

### Vive wands cannot use the two-handed grip modes

`two_handed_mode = toggle` and `hold` need a free grip button, and on Vive wands
both grips already carry Use and Reload. Wands work fine on the default `auto`
mode, which needs no button at all.

### Enemies can occasionally be visible through a wall

The mod relaxes some of the engine's culling, because those tests were written
for a 75° monitor view and throw away things a VR eye can genuinely see. The
trade is that an entity in a room you cannot see may occasionally be drawn.

Rare, and much less annoying than geometry disappearing. `relax_cull_area = 0`
if you would rather have it the other way.

### Performance costs more than it looks like it should

`engine_portals_open_all = 1` is on by default because without it the two eyes
disagree about what is visible through a doorway — one eye loses the room beyond.
The cure is to stop the engine closing doorways at all, which means it draws far
more of the map than a flat game would, twice per frame.

If you are short of frames, that setting and `vr_resolution_scale` are the two
biggest levers, in that order.


---

## Uninstalling

`uninstall_sinvr.bat` ships beside the mod files, so it lands next to
`SinEpisodes.exe` when you copy them in. Run it from there. It:

- deletes the mod's files and the `actions` folder
- puts the game's own `hands.vmt` back — deleting ours *without* restoring it
  leaves the arms drawing as pink-and-black checkerboard
- turns the vanilla crosshair back on (see below)
- asks whether to keep `sinvr.cfg`, so your tuning survives a reinstall
- tells you if either optional folder is installed

It refuses to run unless `SinEpisodes.exe` is beside it, so it cannot delete
things in the wrong folder.

> [!IMPORTANT]
> Two things it deliberately does not do:
>
> **The Steam launch option** — clear it yourself, or the game tries to start
> through a launcher that is no longer there. A batch file cannot reach into
> Steam.
>
> **The optional folders** — neither *adds* a file; both **replace** files the
> game already had, so there is nothing to delete, and deleting would leave the
> game missing content. Steam's *verify integrity of game files* restores them.

### One setting outlives the files

> [!WARNING]
> The mod hides the game's 2D crosshair with the game's own `crosshair` setting,
> because a crosshair drawn at screen centre points exactly where you are *not*
> shooting once the gun aims independently of your head. Source **saves** that
> setting, so removing the mod by hand leaves the vanilla game with no
> crosshair — and nothing connects the two.
>
> `crosshair 1` in the console puts it back. The uninstaller does it for you.

Nothing else the mod changes is persisted — `sv_cheats`, `r_portalsopenall`,
`r_occlusion` and `r_novis` all live only for the session.

---

## Troubleshooting

**The game starts on the monitor but nothing appears in the headset.**
Check `sinvr.log` next to the executable. It records every interface it binds and
says plainly when one fails.

**"Failed to set video mode".**
The requested window is larger than your desktop resolution. The launcher clamps
to the physical desktop automatically, so this should not happen — but if it
does, lower `vr_resolution_scale`, or raise your desktop resolution (NVIDIA DSR
or AMD VSR will let you exceed the panel).

**The arms stretch across the screen.**
Check that the game is on its **`loose`** Steam branch (Properties → Betas). On
the default branch the game reads `hands.vmt` from its packed VPK archives and
ignores the loose override entirely — and the two optional folders with it. Then
make sure the `hands.vmt` content override above is in place.

**The arms come back after a Steam update.**
Steam restores the shipped file on a branch switch or "verify integrity". Re-copy
it.

**Gameplay feels dead — no aiming, no turning, no gestures.**
The mod thinks a menu is open. It should recover on its own: pushing the movement
stick overrules a wrongly-detected menu and logs `menu: LIVENESS ESCAPE`. If it
persists, check the `menu:` line in the log.

**Everything is fine but performance is poor.**
Lower `vr_resolution_scale` first. The scene is rendered twice per frame, so cost
scales with the square of this value.

**The game crashes, or the headset freezes mid-session.**
Send the files next to the executable: `sinvr_crash.log`, `sinvr_crash.dmp` and
`sinvr.log` — or, if you have started the game again since, the same names ending
in `.prev`, which keep the previous run. The crash report names the SteamVR call
that was in progress, your graphics driver and SteamVR versions, and the last
events before it: the headset going to sleep, the dashboard, a level change.

Three safety catches are on by default, each switchable in `sinvr.cfg` so a crash
can be narrowed down:

| setting | what it does |
|---|---|
| `vr_pause_submit_on_standby` | Stops sending frames while the headset is asleep, and resumes when it wakes |
| `vr_submit_guard` | Catches a fault inside SteamVR or the driver instead of crashing: pauses 3 s and retries, and after three faults keeps the game running on the monitor only |
| `vr_submit_check_gpu` | On laptops with two GPUs, refuses to hand frames across when SteamVR and the game are on different ones — set the game to "High performance" in Windows graphics settings |

---

## For developers


`sinvr.log` is written next to the executable on every run and carries a
heartbeat with the state of every subsystem.

---

## Licence

This project's own code — `src/`, `tools/`, `actions/` and `dxvk-patch/` — is
released under the **Apache License 2.0**. See [LICENSE](LICENSE).

Three things it does *not* cover, listed in [NOTICE](NOTICE):

- **DXVK** (zlib/libpng). The `d3d9.dll` in the release is a **modified** build;
  the changes are in `dxvk-patch/`. Report problems with it here, not upstream.
- **OpenVR SDK** (BSD 3-Clause, © Valve). Used as a header at build time, and
  `openvr_api.dll` is redistributed unmodified.
- **The game's own files.** The overrides in `content/` are derived from SiN's
  material and script files and exist only to reach settings the game offers no
  other way to change. SiN Episodes: Emergence is © Ritual Entertainment; this
  mod is not affiliated with or endorsed by Ritual or Valve, and needs a
  legitimate copy of the game.

---

> [!IMPORTANT]
> ## Thanks
>
> This mod exists because other people solved harder problems first and published
> their work.
> 
> - **Praydog**, for the FEAR 2 VR mod and **UEVR** — the reference for what a
>   third-party VR injection can be, and a constant source of ideas about how to
>   approach an engine that does not want to cooperate.
> - **Philip Rebohle** and the **DXVK** contributors, without whom none of this is
>   possible. DXVK's Direct3D 9 to Vulkan translation is what makes a 2006 D3D9
>   game addressable by a modern VR compositor at all.
> - **sd805**, for the **Left 4 Dead 2 VR** mod, and **Keyou** for their fork — the
>   clearest worked example of Source-engine VR, and the source of several
>   techniques used here.
> - **Gistix**, for the **Portal 2 VR** mod, and **Spencer0187** for the fork that
>   added roomscale movement — the reference implementation for moving a Source
>   player entity with the player's real body.
> - **Valve**, for **OpenVR**, the **Source engine**, and releasing the **Source
>   SDK**. The 2004 SDK made it possible to check what the engine actually does
>   rather than guess.
> - **Ritual Entertainment**, for making SiN Episodes: Emergence. It deserved the
>   episodes it never got.

Any mistakes in this mod are my own, not theirs.
