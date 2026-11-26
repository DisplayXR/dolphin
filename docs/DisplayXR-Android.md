# DisplayXR output on Android

The `DisplayXR` stereoscopic mode (Graphics > Stereoscopy > Stereoscopy Mode, "3D Display
(DisplayXR, OpenGL)") sends each frame to the OpenXR runtime of a 3D display, through the
runtime's `XR_DXR_weave` extension. The runtime interlaces ("weaves") the two eyes for the panel
and follows the viewer's eyes with the front camera. The runtime is installed separately; Dolphin
only carries the Khronos OpenXR loader.

Requirements: the OpenGL backend (`[Core] GFXBackend = OGL`), a GPU with geometry shaders (OpenGL
ES 3.2), and the DisplayXR runtime installed. On other backends the mode falls back to Off with a
message.

## Behavior

- Each eye is rendered with its own camera (Dolphin's usual stereoscopy); the pair is laid out
  half side-by-side over the whole window and woven by the runtime.
- On-screen messages and the FPS counter are drawn identically into both halves, so they sit flat
  at the screen plane.
- While emulation is paused, the app is in the background or the in-game menu is open, the session
  ends and the display shows the left eye in 2D. 3D returns on the next frame after that.
- "Follow Head (3D Display)", off by default, moves each eye's camera with the viewer's tracked eye.

## Depth settings

Dolphin's `StereoDepth` (separation) and `StereoConvergence` set each eye's camera offset and the
distance that lands on the screen plane. Content should sit near the screen plane: on a
2400x1080 panel, keep most of the scene within about 20 pixels of disparity, and nothing more than
about 40 pixels out of the screen.

Homebrew `.dol` files get a game ID made from their file name (`ID-<name>`), so per-program values
go in `User/GameSettings/ID-<name>.ini`. Values measured on a 2400x1080 panel for devkitPro's
`gamecube-examples` (disparity in screen pixels, negative is in front of the screen):

| Program | StereoDepth | StereoConvergence | Disparity (5th–95th percentile, extremes) |
|---|---|---|---|
| `graphics/gx/neheGX/lesson05` | 70 | 6 | −16 to +16 (−20, +22) |
| `graphics/gx/acube` | 100 | 185 | −24 to +18 (−32, +20) |
| `graphics/gx/neheGX/lesson09` | 80 | 14 | 0 to +14 |
| `graphics/gx/neheGX/lesson10` | 12 | 2 | 0 to +4 (−32, +6) |

```ini
# User/GameSettings/ID-lesson05.ini
[Video_Stereoscopy]
StereoDepth = 70
StereoConvergence = 6
```

## Debugging

- `adb shell setprop debug.dolphin.displayxr.eyes_manual 1` asks the runtime for manual eye
  tracking when the session starts, so the display stays in 3D with nobody in front of it.
- The runtime can capture what Dolphin submits and what it returns: create
  `displayxr_atlas_trigger.<package>` in the runtime's external files directory; it writes
  `displayxr_atlas.<package>.input.png` (the side-by-side pair) and `.woven.png` next to it.
