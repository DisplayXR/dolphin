// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoCommon/DisplayXR.h"

#include <mutex>

namespace DisplayXR
{
static std::mutex s_mutex;
static WindowOrigin s_window_origin;
static std::optional<TrackedEyes> s_tracked_eyes;
static u32 s_tracked_eyes_generation = 0;

void SetWindowOrigin(int x, int y, int display_id)
{
  std::lock_guard lock(s_mutex);
  s_window_origin = {x, y, display_id};
}

WindowOrigin GetWindowOrigin()
{
  std::lock_guard lock(s_mutex);
  return s_window_origin;
}

void SetTrackedEyes(std::optional<TrackedEyes> eyes)
{
  std::lock_guard lock(s_mutex);
  s_tracked_eyes = eyes;
  ++s_tracked_eyes_generation;
}

std::optional<TrackedEyes> GetTrackedEyes()
{
  std::lock_guard lock(s_mutex);
  return s_tracked_eyes;
}

u32 GetTrackedEyesGeneration()
{
  std::lock_guard lock(s_mutex);
  return s_tracked_eyes_generation;
}
}  // namespace DisplayXR
