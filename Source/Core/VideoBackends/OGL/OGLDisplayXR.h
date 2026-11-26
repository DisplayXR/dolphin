// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <memory>

#include "Common/CommonTypes.h"

namespace OGL
{
// DisplayXR output for OpenGL ES on Android.
//
// The OpenXR runtime of a 3D display runs as a separate service. Through its XR_DXR_weave
// extension the app stays the owner of its window: each frame it hands the runtime the
// side-by-side pair in an AHardwareBuffer and gets back the image woven for the panel, plus the
// viewer's tracked eye positions. The OpenXR calls run on a thread of their own; the session's
// graphics binding is a private Vulkan device the runtime never renders with. The GL side only
// copies between the backbuffer and the shared buffers.
//
// Adapted from core/transport_weave.cpp of github.com/DisplayXR/displayxr-play-android
// (Boost Software License 1.0) and the runtime's weave_client_vk_android sample.
class DisplayXRWeave
{
public:
  DisplayXRWeave();
  ~DisplayXRWeave();
  DisplayXRWeave(const DisplayXRWeave&) = delete;
  DisplayXRWeave& operator=(const DisplayXRWeave&) = delete;

  // Render thread, with the GL context current. Starts the session on first use; returns false
  // until it is ready, and for good if it failed. On true the backbuffer (framebuffer 0, width x
  // height, holding the side-by-side pair) has been replaced by the woven image.
  bool WeaveBackbuffer(u32 width, u32 height);

  // Ends the session (the panel returns to 2D). The next WeaveBackbuffer starts a new one.
  // Render thread, GL context current if there is one.
  void Stop();

private:
  struct Impl;
  std::unique_ptr<Impl> m_impl;
};
}  // namespace OGL
