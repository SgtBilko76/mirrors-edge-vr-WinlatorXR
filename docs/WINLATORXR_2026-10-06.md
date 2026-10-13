# WinlatorXR port for standalone Quest (2026-10-06)

Status: **implemented and tested under desktop Wine + DXVK with a simulated XrAPI host.
Not yet verified in a headset.** Nothing here changes the desktop OpenXR build; every change
is behind `MEVR_WINLATORXR` or is a behaviour-preserving refactor (see "Desktop build" below).

## How it works

[WinlatorXR](https://github.com/WinlatorXR/WinlatorXR) runs the Windows game on the headset
through Wine, Box64 and DXVK. There is no OpenXR runtime in the container. Instead it speaks
[XrAPI](https://winlatorxr.github.io/xrapi.html): tracking and buttons arrive as text on UDP
`127.0.0.1:7872`, the game replies on `7278` with the VR mode, stereo layout and FOV, and
WinlatorXR shows the game's own window to the eyes. The top-left pixel carries the `HMD_SYNC`
value of the pose the frame was drawn with.

The mod already draws both eyes side by side into the backbuffer (draw duplication), so the
picture needs no copy. Two pieces make the rest work:

- `src/winlatorxr.inl` is an in-process implementation of the OpenXR subset the mod calls,
  backed by XrAPI: instance/system/session, LOCAL and VIEW spaces, `xrLocateSpace`,
  `xrLocateViews`, and the action system, which resolves the mod's suggested Touch bindings
  to XrAPI's buttons and sticks. The game logic keeps calling OpenXR unchanged. One packet is
  latched per frame so the rendered pose, head tracking and the sync stamp agree.
- `src/winlatorxr_frame.inl` replaces `CaptureFrame` + `SubmitTestQuad` in Present. It uses
  the same per-frame duplication test to choose side-by-side (`MODE_3D 1`), alternate-eye
  (`MODE_3D 2`, blue sync channel = eye) or a flat screen (`MODE_VR 2`) for menus, loading and
  videos; draws the VR settings panel into each eye at the desktop quad's pose; stamps a 10x10
  sync block; and sends the forced render FOV back so the displayed and rendered frustum match.
  It also pins the game window borderless at 0,0 (posted to the window's own thread, as the
  CoD4 port found that restyling from the render thread deadlocks) and waits up to 20 ms per
  frame for a fresh pose (`MEVR_WINLATORXR_SYNC_WAIT_MS`).

Taken from the sibling ports: the XrAPI client and SBS/AER sync conventions from the Far Cry
WinlatorXR work, the packet parsing details, sync block, pacing and window pinning from the
CoD4 port (`docs/WINLATORXR.md` there).

### Controls (superseded 2026-10-06: tracked hands are now optional, see below)

Originally joypad only:

On the headset the controllers drive the mod's synthesised XInput pad with the **Hand tracking
off** layout from the README. `MenuApplyFeatures` forces `g_motionHands` off, so arm swing,
pistols, melee, motion parkour and physical crouching stay off; the VR menu shows those rows as
inactive. Pose actions report inactive. XrAPI's controller orientations are not OpenXR grip
poses (CoD4 measured a 180° roll and a non-grip "upward" quaternion), so nothing pretends
otherwise. Triggers and grips are digital in XrAPI and read as 0 or 1.

### Quest-specific behaviour

- `mevr.log` is written beside `d3d9.dll`, because the container's `LOCALAPPDATA` is inside
  WinlatorXR's private data and unreachable over adb.
- Frame dump: create `mevr_shot.request` beside the DLL and the next presented frame is
  written to `mevr_shot.bmp` (exactly what WinlatorXR receives, stamp included).
- Resolution `auto` uses half of WinlatorXR's virtual screen (from `Z:\tmp\xr\system`) as the
  eye width, so the game renders exactly the screen size. The screen must be 16:9: the engine
  letterboxes other aspects, and the log then says so.

## Headset startup failure: PhysX (2026-10-06)

The first headset launches crashed before a D3D9 device existed, with and without the mod and
with WinlatorXR's Goldberg plugin. Wine reported a null read at `MirrorsEdge.exe+FA85C7`, which
is a library shutdown on the engine's exit path, not the cause. A first-chance exception logger
in this flavour found the real fault: `+6BFC7A`, the result of `NxCreatePhysicsSDK(0x02080000,
...)` (PhysX 2.8.0) dereferenced while NULL.

The GOG installer runs `PhysX-9.13.0604-SystemSoftware-Legacy.msi`, which puts the PhysX core in
`C:\Program Files\NVIDIA Corporation\PhysX` with registry keys. A game folder copied to the
headset has neither. Reproduced on the desktop by hiding that folder (identical faults), and
fixed there without any installer by copying the game's own `Binaries\PhysXCore.bak` to
`Binaries\PhysXCore.dll`, which PhysXLoader picks up beside the exe. Ruled out on the way:
memory (4 GB address space, 3.3 GB physical free at load), the GPU (one adapter, SM 3.0), the
Qualcomm vendor id (desktop starts with it spoofed), and Steam.

Setup step for the headset: copy `PhysXCore.bak` to `PhysXCore.dll` in `Binaries`.

## Headset findings, continued (2026-10-06)

- **Box64 memory ordering is required.** With the PERFORMANCE preset (`BOX64_DYNAREC_STRONGMEM=0`)
  the engine's game and rendering threads see each other's writes out of order on the ARM CPU:
  "Pure virtual function being called" in the render-command loop (`+7307A1` -> `+7CB399`),
  a null vtable read at `+73079A`, and hangs while a chapter loads. Variables added to the
  container are overridden by the preset; the mod now logs the `BOX64_*` environment the game
  actually runs with. STABILITY (`STRONGMEM=2`) runs stable into the prologue. `-ONETHREAD`
  does not remove the rendering thread in this build.
- **WinlatorXR's screen mode is head-locked.** Flat frames (menus, loading, video) are now drawn
  by the mod on a screen 2 m ahead, fixed in the room and re-anchored on recentre, and sent as
  side-by-side. WinlatorXR's own **immersive** toggle must be on; the mod logs the immersive and
  side-by-side flags each packet reports.
- `Z:\tmp\xr\system` reported 840x880 for a 2560x1440 container; the eye size now comes from
  Wine's desktop mode. Tracking can arrive late, so the session no longer waits for it.
- **Frame rate.** Side-by-side stereo at 2560x1440 with STABILITY: 11-14 fps on the headset;
  steady, so not shader compilation (DXVK runs async with a state cache). `AlternateEye = on`
  (WinlatorXR flavour only) arms alternate-eye stereo instead: one full-width eye per frame,
  blue sync channel = eye. Desktop Wine: 38-52 fps against 20-29 side by side at the same spot.
  Headset measurement pending.
- **Render the headset's native FOV, as Far Cry does.** Alternate-eye at 16:9 sent 132 x 103.4
  degrees and the picture squeezed and swam with head movement: WinlatorXR shows each eye's region
  over the sent FOV but no wider than its native one. The forced frustum is now the headset's
  native FOV on both axes (Quest 3: 108.9 x 103.4) in this flavour; the engine's 16:9 frame is
  stretched to the screen by the window, so the screen's shape only sets pixel density. Far Cry's
  1792x1624 (the eye's ~1.10) is the recommended screen; with AlternateEye, Resolution = auto then
  renders 16:9 at 80% of the screen height (2310x1299).

- **Tracked hands, optional (later on 2026-10-06).** Pose actions are now live: aim = XrAPI's
  controller orientation with its 180 degree roll undone (CoD4 finding), grip = aim pitched up
  `WinlatorXrGripPitch` degrees (default 40, against which the mod's 40 degree wrist-down pistol
  calibration fits). The forced `MotionHands = off` was removed, so Hand tracking and its child
  features behave as on PC and can be switched in the VR menu. Desktop: all four poses tracked,
  arms follow the controllers. Grip angle on the headset not yet checked.
- **HUD in alternate-eye mode.** The Canvas UI placement (`GameUIScale`, `GameUIHeight`) used to
  run only side by side; in alternate-eye it now scales the full-width frame about its centre.

- **Left-eye flicker in alternate-eye (fixed in code, headset check pending).** A headset
  recording showed every fourth frame from a shifted viewpoint. The eye was stamped from a
  prediction made at the previous Present, which assumes a fixed one-frame render-thread lag that
  Box64 does not keep. The constant hook now counts which eye offset each frame's scene really
  got, and that eye is stamped; mismatches against the prediction are logged every 1200 frames.
- **2D screen whenever the player is not in control.** Not in control = the camera's view target
  is not the pawn, `bCinematicMode` is set, or the game is paused (not by the VR panel). Then
  stereo, the eye offset and the FOV force stand down (via the existing half-res standdown path),
  the game renders its own view, and the room-fixed screen is presented alternate-eye so each
  eye gets the full window. Stereo resumes after 0.5 s of stable control.
- **Headset controller layout (revised the same day).** A Far Cry-style remap of the face
  buttons was rejected in the headset: the game's prompts show Xbox buttons, so A/B/X/Y stay what
  they are labelled. Kept from Far Cry: the right stick click opens the VR panel (short click
  toggles), and right stick up / down are jump / quick turn whatever the hand setting.

  | Control | Action |
  |---|---|
  | A / B / X / Y | Use / look at / Reaction Time / weapon action |
  | Left / right trigger | Crouch & slide / attack |
  | Right stick | Turn (snap 45 by user choice); up = jump, down = quick turn |
  | Right stick click | VR settings panel |
  | Left stick click / menu button | In-game menu / pause |
  | Grips | Hands (tracked hands on), otherwise jump / quick turn |

- **WinlatorXR's virtual XInput pad.** WinlatorXR also presents the Touch controllers as an
  XInput pad with its own mapping; ControllerMode = auto switched to it on every input, giving
  WinlatorXR's buttons and the grips-as-jump layout. On the headset auto now keeps the Touch
  layout (ControllerMode = gamepad still selects a real pad).
- **Flicker at low frame rate.** The drawn-eye counter showed the old prediction wrong in 1199 of
  1200 frames (eyes were consistently swapped; now fixed), so the remaining flicker in both eyes
  is the alternate-eye refresh: at 13-16 fps each eye updates 7-8 times a second.

- **Explicit Resolution is enforced like Auto on the headset.** `Resolution = 1920x1080` was only
  offered as an additional mode and the engine kept its saved 1600x1200; the 16:9 scene was
  letterboxed into it and side-by-side stereo correctly refused. In this flavour an explicit size
  is also written to TdEngine.ini and offered as the only mode.
- **2D switching criteria narrowed.** `bCinematicMode` toggled during ordinary tutorial play and
  flashed the headset into 2D; it is no longer a reason. 2D now means: no pawn, the camera's view
  target is not the pawn, or the game has stayed paused for 30 frames. The log names the reason.
- Custom Box64 preset reached the game (`STRONGMEM=1, CALLRET=1, BIGBLOCK=2`); menus 51-54 fps.

- **Gamepad only (`WinlatorXrKeyboardMouse = off`, default).** WinlatorXR's emulated keyboard and
  mouse fired game actions on top of the synthesised pad. `src/winlatorxr_input.inl` silences the
  game's keyboard and mouse on its three routes: window messages are dropped in the subclassed
  window procedure, DirectInput devices created for GUID_SysKeyboard / GUID_SysMouse report no
  state or data, and GetAsyncKeyState / GetKeyState return 0 when called from the exe's own code
  (the mod's hotkeys still read the keyboard). Desktop: hooks installed, DirectInput mouse
  silenced, the title screen still advances on the pad's A.
- **Quest graphics profile (`WinlatorXrGraphics = quest`, default).** At startup, before the engine
  reads its config, the mod rewrites existing `[SystemSettings]` keys in TdEngine.ini:
  DynamicShadows, LightEnvironmentShadows, MotionBlur, DepthOfField, AmbientOcclusion,
  Distortion, LensFlares, UseVsync off; DetailMode 1; MaxShadowResolution 256; MaxAnisotropy 2;
  SkeletalMeshLODBias and ParticleLODBias 1. Bloom is left alone. `game` leaves the file be.
  Headset frame-rate effect not yet measured.

## Building

```sh
MEVR_OPENXR_INCLUDE=/path/to/openxr-sdk/include src/build_linux.py --winlatorxr
```

Output: `build/winlatorxr/d3d9.dll` and the generated `mevr.ini`. The flavour imports no
`openxr_loader.dll`, `d3d11.dll` or `dxgi.dll`; it adds `ws2_32.dll`. `--push` with
`MEVR_QUEST_GAME_DIR` copies the DLL to the headset.

## Desktop testing without a headset

`tools/fake-xrapi.py` plays WinlatorXR's side under desktop Wine (Wine maps `Z:\tmp\xr` to
`/tmp/xr`): it writes the `system` file, streams tracking at 72 Hz with a slowly moving head,
prints every mode the mod replies with, and takes button presses from `/tmp/xr/press`.

Run with DXVK's 32-bit `d3d9.dll` in the prefix and `WINEDLLOVERRIDES=d3d9=n,b`.

## Results so far (desktop Wine 10.0 + DXVK 3.0.2, GOG 1.01, simulated XrAPI)

| Check | Result |
|---|---|
| XrAPI handshake (`version`, `vr`, first packet) | Pass: FOV 108.9 x 103.4, IPD 63 mm |
| Runtime session to FOCUSED, actions attached, XInput hook | Pass |
| Resolution auto, second launch | Pass: engine renders 2048x1152 for a 2048x1152 screen |
| Title, menus, difficulty, intro video | Flat screen mode; menus driven by the joypad |
| Prologue | Stereo arms by itself; side-by-side, FOV 96.8 x 103.4 sent |
| VR settings panel (hold Y) | Drawn in both eyes with parallax, game paused behind it |
| Desktop frame rate in stereo | 19-33 fps, both game and render threads CPU-bound under Wine |

The desktop frame rate does not predict the headset. Own-process `ReadProcessMemory` was
suspected and measured: Wine 10 serves it locally at about 0.13 µs per call, so it is not the
cost, and that change was not kept. `GetAsyncKeyState` costs about 4.5 µs under Wine; the mod
polls about 56 keys per frame.

## Desktop build

Shared code was refactored without behaviour changes: `AdoptLocatedViews` (view location, IPD
and forced frustum, extracted from `SubmitTestQuad`) and `VrMenuTickVisible` /
`VrMenuRasterizeIfChanged` (from `VrMenuLayer`). `src/build_linux.py --compile-only` compiles
the desktop flavour on Linux; the Windows build and release workflow are unchanged and were
not run here.

## Open items

- Headset verification on WinlatorXR dawn-33: stereo, sync/reprojection, head tracking,
  controls, frame rate, and whether WinlatorXR's own controller-to-keyboard/mouse emulation
  reaches the game (CoD4 had to ignore it).
- Frame rate on Box64 with draw duplication; alternate-eye (F10 path) as a fallback.
- `README.md` and `packaging/README.txt` describe only the desktop release; update them once
  the Quest flavour is verified and has a release shape.
