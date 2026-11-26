# DisplayXR headers

Headers needed by the DisplayXR stereoscopic output on Android
(`Source/Core/VideoBackends/OGL/OGLDisplayXR.cpp`). Headers only; no runtime code is
linked. The runtime is installed separately on the device and reached through the
Khronos OpenXR loader for Android.

- `openxr.h`, `openxr_platform.h`, `openxr_platform_defines.h`,
  `openxr_loader_negotiation.h`: Khronos OpenXR headers, Apache-2.0 OR MIT.
- `XR_DXR_weave.h`, `XR_DXR_display_info.h`: provisional DisplayXR extensions,
  Apache-2.0.

Copied from https://github.com/DisplayXR/displayxr-runtime
(`src/external/openxr_includes/openxr/`, commit adaa652).
