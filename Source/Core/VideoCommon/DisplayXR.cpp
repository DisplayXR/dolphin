// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoCommon/DisplayXR.h"

#include <atomic>
#include <mutex>

#include "Core/Core.h"
#include "Core/System.h"
#include "VideoCommon/AsyncRequests.h"
#include "VideoCommon/Present.h"
#include "VideoCommon/VideoConfig.h"

namespace DisplayXR
{
static std::mutex s_mutex;
static WindowOrigin s_window_origin;
static std::optional<TrackedEyes> s_tracked_eyes;
static u32 s_tracked_eyes_generation = 0;
static std::atomic<bool> s_covered{false};
static std::atomic<bool> s_paused{false};

// Presents the last frame again on the video thread, in the mode ShowFlat() now asks for.
static void RequestRedraw()
{
  if (g_ActiveConfig.stereo_mode != StereoMode::DisplayXR)
    return;
  // With a single CPU/GPU thread the request would run on this (host) thread; the next frame
  // picks the change up instead.
  auto& system = Core::System::GetInstance();
  const Core::State state = Core::GetState(system);
  if (!system.IsDualCoreMode() || (state != Core::State::Running && state != Core::State::Paused))
    return;
  AsyncRequests::GetInstance()->PushEvent([] {
    if (g_presenter)
      g_presenter->Present();
  });
}

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

void SetCovered(bool covered)
{
  if (s_covered.exchange(covered) != covered)
    RequestRedraw();
}

void SetPaused(bool paused)
{
  if (s_paused.exchange(paused) != paused)
    RequestRedraw();
}

bool ShowFlat()
{
  return s_covered || s_paused;
}

u32 GetTrackedEyesGeneration()
{
  std::lock_guard lock(s_mutex);
  return s_tracked_eyes_generation;
}
}  // namespace DisplayXR
