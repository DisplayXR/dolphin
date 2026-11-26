// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <optional>

#include "Common/CommonTypes.h"

// Shared state for the DisplayXR stereoscopic mode, in which the backend hands each frame's
// side-by-side pair to a 3D display's OpenXR runtime (the XR_DXR_weave extension). The runtime
// interlaces ("weaves") the pair for the panel and reports the viewer's tracked eye positions.

namespace DisplayXR
{
struct WindowOrigin
{
  int x = 0;
  int y = 0;
  int display_id = -1;  // -1: unknown, single display
};

// Called by the host when the render surface moves. The runtime anchors the panel's interlace
// phase to the surface's position on the screen, so a wrong origin swaps or mixes the eyes.
void SetWindowOrigin(int x, int y, int display_id);
WindowOrigin GetWindowOrigin();

// The display shows the game in 2D while emulation is paused or something covers the game (a
// menu, another screen). Both calls redraw the last frame, so the change shows even while paused.
void SetCovered(bool covered);  // host
void SetPaused(bool paused);    // emulation state
bool ShowFlat();

// The viewer's eyes, horizontally, in meters from the display's center (x to the right).
struct TrackedEyes
{
  float left_x = 0;
  float right_x = 0;
};

// Called by the backend once per presented frame, so the next frame's cameras all use the same
// positions. std::nullopt when the runtime has no live tracking.
void SetTrackedEyes(std::optional<TrackedEyes> eyes);
std::optional<TrackedEyes> GetTrackedEyes();
// Increments whenever SetTrackedEyes is called.
u32 GetTrackedEyesGeneration();
}  // namespace DisplayXR
