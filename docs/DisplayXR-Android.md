# Dolphin 3D (DisplayXR) for Android

Dolphin 3D is a fork of the Dolphin GameCube and Wii emulator for Android. It adds one stereo mode,
"3D Display (DisplayXR, OpenGL)": Dolphin renders each frame twice, once per eye, and hands the
pair to the DisplayXR runtime, which interlaces it for a glasses-free 3D display. Everything else
is Dolphin. The fork is not made or endorsed by the Dolphin team.

## What you need

- An Android device with a glasses-free 3D display.
- The DisplayXR runtime for Android, installed, together with the display plug-in for that device.
  The runtime is a separate app; this fork only carries the Khronos OpenXR loader that connects to
  it.
- A GPU with OpenGL ES 3.2. The 3D mode uses Dolphin's OpenGL backend.
- Your own software. The app comes with no games, no BIOS and no firmware, and this project does not
  link to any.

## Install

Download the signed APK from the Releases page of this repository and install it. It installs as a
separate app ("Dolphin 3D (DisplayXR)", package `org.displayxr.dolphin`) next to a regular Dolphin,
with its own settings and user folder.

## Turn on 3D

1. Settings > Graphics Settings > Video Backend: **OpenGL**.
2. Settings > Graphics Settings > Enhancements > Stereoscopy > Stereoscopy Mode: **3D Display
   (DisplayXR, OpenGL)**.
3. Start a game. The first time, Android asks for camera access; the display uses the front camera
   to follow your eyes.

The game shows in 3D while it runs. The display switches to 2D while the game is paused, while the
in-game menu is open and while the app is in the background, and back to 3D when you return.
On-screen messages and the FPS counter sit flat on the screen surface.

## Depth

Two settings under Stereoscopy set the depth:

- **Separation**: how far apart the two eye cameras are. Lower means flatter.
- **Convergence**: the distance in the game that appears on the screen surface. Objects nearer than
  it come out of the screen, objects farther go behind it.

Keep most of the picture near the screen surface: a 3D display shows small depth comfortably and
large depth as double edges.

Games differ in scale, so set depth per game:

- While a game runs, Settings in the in-game menu changes depth immediately. If the game has its own
  settings file (below), the change lasts until you close the game; otherwise it changes the global
  value.
- To keep a value for one game, write it into that game's settings file in the app's user folder,
  `Android/data/org.displayxr.dolphin/files/GameSettings/<GAME ID>.ini`:

  ```ini
  [Video_Stereoscopy]
  StereoDepth = 24
  StereoConvergence = 10
  ```

  `StereoDepth` is Separation. A game's own file takes priority over the global Stereoscopy
  settings, so changing the global values does not change a game that has a file. For a `.dol` or
  `.elf` program the game ID is `ID-` followed by its file name, for example `ID-neverball`.

## Known limits

- **OpenGL ES only.** The Vulkan backend has no 3D Display mode yet; with Vulkan selected the mode
  falls back to Off.
- **Depth is set by hand, per game.** There is no automatic depth; the defaults suit some games
  and are too strong or too flat for others.
- **No head-following parallax by default.** The display follows your eyes to keep both eyes'
  pictures in place. The scene itself does not change with head position unless you turn on
  "Follow Head (3D Display)", which is experimental.
- **Dialogs stay in 3D.** Only the in-game menu, pause and the background switch the display to 2D.
  Pop-up dialogs (alerts, controller choice) appear over the 3D picture.
- **Stereo rendering costs speed.** Each frame is drawn twice; games that run at full speed in 2D
  may not in 3D.
- **Touch controls are flat overlays** drawn over the 3D picture.

## Building

Build as upstream Dolphin for Android (see [AndroidSetup.md](../AndroidSetup.md)). `-PabiOnly=arm64-v8a` limits the
build to one ABI. The workflow in `.github/workflows/build-android.yml` builds the arm64 release APK, and signs and publishes it for
tags named `v*` when the signing secrets are set.

## How it works

- `Source/Core/VideoBackends/OGL/OGLDisplayXR.cpp` connects to the runtime through the OpenXR
  `XR_DXR_weave` extension: the side-by-side pair goes to the runtime in a shared hardware buffer,
  and the woven picture comes back and replaces the frame.
- `Source/Core/VideoCommon/Present.cpp` lays the two eyes out side by side, draws the on-screen UI
  into both halves, and shows the left eye alone while the game is paused or covered.
- Each eye is rendered with its own camera by Dolphin's existing stereoscopy. Nothing shifts the
  finished pictures.

### Debugging

- `adb shell setprop debug.dolphin.displayxr.eyes_manual 1` asks the runtime for manual eye
  tracking when a session starts, so the display stays in 3D with nobody in front of it.
- The runtime can save what the app submits and what it returns: create
  `displayxr_atlas_trigger.org.displayxr.dolphin` in the runtime's external files folder.

## License

GPL-2.0-or-later, like Dolphin. The OpenXR headers are Apache-2.0 OR MIT, the DisplayXR extension
headers Apache-2.0 (`Externals/DisplayXR/`), and parts of the weave transport are adapted from
DisplayXR code under the Boost Software License 1.0.
