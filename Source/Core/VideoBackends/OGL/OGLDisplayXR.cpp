// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Portions adapted from core/transport_weave.cpp of github.com/DisplayXR/displayxr-play-android,
// Boost Software License 1.0 (Externals/DisplayXR/LICENSE-BSL-1.0.txt).
//
// Protocol reference: XR_DXR_weave spec, sections 5b (Android) and 5b-sync (sync_file fences),
// and the runtime's test_apps/weave/weave_client_vk_android.
//
// This file deliberately includes neither Dolphin's GL headers nor its Vulkan loader: it talks to
// the system GLES/EGL libraries directly, and resolves Vulkan, the OpenXR loader and the
// AHardwareBuffer API (API 26+, above Dolphin's minimum) at run time.

#include "VideoBackends/OGL/OGLDisplayXR.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <fmt/format.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
// After gl3.h, which defines the GL types it uses.
#include <GLES2/gl2ext.h>

#include <android/hardware_buffer.h>
#include <android/looper.h>
#include <dlfcn.h>
#include <jni.h>
#include <sys/eventfd.h>
#include <sys/system_properties.h>
#include <unistd.h>

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#define XR_NO_PROTOTYPES
#define XR_USE_PLATFORM_ANDROID
#define XR_USE_GRAPHICS_API_VULKAN
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <openxr/XR_DXR_display_info.h>
#include <openxr/XR_DXR_weave.h>

#include "Common/Logging/Log.h"
#include "VideoCommon/DisplayXR.h"
#include "VideoCommon/OnScreenDisplay.h"
#include "jni/AndroidCommon/IDCache.h"

namespace OGL
{
namespace
{
// A submit that takes longer than this is left in flight; frames are shown in 2D until it returns.
constexpr std::chrono::milliseconds SUBMIT_WAIT{500};
constexpr int LOOPER_IDENT_COMMAND = 1;

// AHardwareBuffer entry points, API 26+.
struct AhbApi
{
  int (*allocate)(const AHardwareBuffer_Desc*, AHardwareBuffer**) = nullptr;
  void (*release)(AHardwareBuffer*) = nullptr;
  void (*describe)(const AHardwareBuffer*, AHardwareBuffer_Desc*) = nullptr;

  bool Load()
  {
    void* lib = dlopen("libandroid.so", RTLD_NOW | RTLD_LOCAL);
    if (!lib)
      return false;
    allocate = reinterpret_cast<decltype(allocate)>(dlsym(lib, "AHardwareBuffer_allocate"));
    release = reinterpret_cast<decltype(release)>(dlsym(lib, "AHardwareBuffer_release"));
    describe = reinterpret_cast<decltype(describe)>(dlsym(lib, "AHardwareBuffer_describe"));
    return allocate && release && describe;
  }
};
AhbApi s_ahb;

// EGL / GLES extension entry points.
struct EglApi
{
  PFNEGLGETNATIVECLIENTBUFFERANDROIDPROC get_native_client_buffer = nullptr;
  PFNEGLCREATEIMAGEKHRPROC create_image = nullptr;
  PFNEGLDESTROYIMAGEKHRPROC destroy_image = nullptr;
  PFNEGLCREATESYNCKHRPROC create_sync = nullptr;
  PFNEGLDESTROYSYNCKHRPROC destroy_sync = nullptr;
  PFNEGLWAITSYNCKHRPROC wait_sync = nullptr;
  PFNEGLDUPNATIVEFENCEFDANDROIDPROC dup_native_fence_fd = nullptr;
  PFNGLEGLIMAGETARGETTEXTURE2DOESPROC image_target_texture_2d = nullptr;
  bool have_fences = false;

  template <typename T>
  static void Get(const char* name, T* out)
  {
    *out = reinterpret_cast<T>(eglGetProcAddress(name));
  }

  bool Load()
  {
    Get("eglGetNativeClientBufferANDROID", &get_native_client_buffer);
    Get("eglCreateImageKHR", &create_image);
    Get("eglDestroyImageKHR", &destroy_image);
    Get("eglCreateSyncKHR", &create_sync);
    Get("eglDestroySyncKHR", &destroy_sync);
    Get("eglWaitSyncKHR", &wait_sync);
    Get("eglDupNativeFenceFDANDROID", &dup_native_fence_fd);
    Get("glEGLImageTargetTexture2DOES", &image_target_texture_2d);
    have_fences = create_sync && destroy_sync && wait_sync && dup_native_fence_fd;
    return get_native_client_buffer && create_image && destroy_image && image_target_texture_2d;
  }
};

// One AHardwareBuffer, seen by GL as a texture attached to a framebuffer.
struct AhbTarget
{
  AHardwareBuffer* ahb = nullptr;  // we hold exactly one reference
  EGLImageKHR image = EGL_NO_IMAGE_KHR;
  GLuint texture = 0;
  GLuint framebuffer = 0;
  u32 width = 0;
  u32 height = 0;

  void Destroy(const EglApi& egl)
  {
    DropGL(egl);
    if (ahb)
      s_ahb.release(ahb);
    *this = AhbTarget{};
  }

  void DropGL(const EglApi& egl)
  {
    if (framebuffer)
      glDeleteFramebuffers(1, &framebuffer);
    if (texture)
      glDeleteTextures(1, &texture);
    if (image != EGL_NO_IMAGE_KHR)
      egl.destroy_image(eglGetCurrentDisplay(), image);
    framebuffer = texture = 0;
    image = EGL_NO_IMAGE_KHR;
  }

  bool Wrap(const EglApi& egl, AHardwareBuffer* buffer)
  {
    AHardwareBuffer_Desc desc{};
    s_ahb.describe(buffer, &desc);
    ahb = buffer;
    width = desc.width;
    height = desc.height;
    const EGLint attribs[] = {EGL_IMAGE_PRESERVED_KHR, EGL_TRUE, EGL_NONE};
    image = egl.create_image(eglGetCurrentDisplay(), EGL_NO_CONTEXT, EGL_NATIVE_BUFFER_ANDROID,
                             egl.get_native_client_buffer(buffer), attribs);
    if (image == EGL_NO_IMAGE_KHR)
    {
      ERROR_LOG_FMT(VIDEO, "DisplayXR: eglCreateImageKHR({}x{}) failed: {:#x}", width, height,
                    eglGetError());
      return false;
    }
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    egl.image_target_texture_2d(GL_TEXTURE_2D, static_cast<GLeglImageOES>(image));
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glGenFramebuffers(1, &framebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
    const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE)
    {
      ERROR_LOG_FMT(VIDEO, "DisplayXR: AHardwareBuffer framebuffer incomplete: {:#x}", status);
      return false;
    }
    return true;
  }

  bool Rewrap(const EglApi& egl)
  {
    AHardwareBuffer* const buffer = ahb;
    DropGL(egl);
    return buffer && Wrap(egl, buffer);
  }
};

// Saves and restores the GL state the copies disturb, so Dolphin's state tracking stays valid.
class GLStateGuard
{
public:
  GLStateGuard()
  {
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &m_read_fb);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &m_draw_fb);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &m_active_texture);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &m_texture);
    m_scissor = glIsEnabled(GL_SCISSOR_TEST);
    m_discard = glIsEnabled(GL_RASTERIZER_DISCARD);
    // Blits obey the scissor test and rasterizer discard.
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_RASTERIZER_DISCARD);
  }
  ~GLStateGuard()
  {
    glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(m_read_fb));
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(m_draw_fb));
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(m_texture));
    glActiveTexture(static_cast<GLenum>(m_active_texture));
    if (m_scissor)
      glEnable(GL_SCISSOR_TEST);
    if (m_discard)
      glEnable(GL_RASTERIZER_DISCARD);
  }

private:
  GLint m_read_fb = 0, m_draw_fb = 0, m_texture = 0, m_active_texture = 0;
  GLboolean m_scissor = GL_FALSE, m_discard = GL_FALSE;
};

// The application context, from the Java side. Not an Activity: given an Activity, the runtime's
// client adds a view of its own over the app's window.
jobject GetApplicationContextGlobalRef()
{
  JNIEnv* env = IDCache::GetEnvForThread();
  jclass native_library = IDCache::GetNativeLibraryClass();
  jmethodID method = env->GetStaticMethodID(native_library, "getApplicationContext",
                                            "()Landroid/content/Context;");
  if (!method)
  {
    env->ExceptionClear();
    return nullptr;
  }
  jobject local = env->CallStaticObjectMethod(native_library, method);
  if (env->ExceptionCheck())
  {
    env->ExceptionClear();
    return nullptr;
  }
  jobject global = local ? env->NewGlobalRef(local) : nullptr;
  if (local)
    env->DeleteLocalRef(local);
  return global;
}

enum class State
{
  Stopped,
  Starting,
  Ready,
  Failed,
};
}  // namespace

struct DisplayXRWeave::Impl
{
  // ---- set before the session thread starts ----
  JavaVM* vm = nullptr;
  jobject context = nullptr;  // global ref, application context

  // ---- session thread ----
  std::thread thread;
  int command_fd = -1;  // eventfd on the session thread's ALooper

  // ---- OpenXR and the session's private Vulkan device (session thread only) ----
  PFN_xrGetInstanceProcAddr xr_gipa = nullptr;
  PFN_xrDestroyInstance xr_destroy_instance = nullptr;
  PFN_xrDestroySession xr_destroy_session = nullptr;
  PFN_xrWeaveBindWindow2DXR xr_bind_window = nullptr;
  PFN_xrWeaveSubmitDXR xr_submit = nullptr;
  XrInstance instance = XR_NULL_HANDLE;
  XrSystemId system = XR_NULL_SYSTEM_ID;
  XrSession session = XR_NULL_HANDLE;
  u32 weave_version = 0;
  PFN_vkGetInstanceProcAddr vk_gipa = nullptr;
  VkInstance vk_instance = VK_NULL_HANDLE;
  VkDevice vk_device = VK_NULL_HANDLE;
  PFN_vkDestroyInstance vk_destroy_instance = nullptr;
  PFN_vkDestroyDevice vk_destroy_device = nullptr;

  // ---- shared, guarded by mutex ----
  mutable std::mutex mutex;
  std::condition_variable cv;
  std::atomic<State> state{State::Stopped};
  std::string error;
  bool quit = false;
  bool geometry_dirty = false;
  DisplayXR::WindowOrigin origin;
  u32 window_width = 0, window_height = 0;

  struct Job
  {
    bool pending = false;
    bool done = false;
    AHardwareBuffer* input = nullptr;
    u32 width = 0, height = 0;
    int acquire_fd = -1;
    XrResult result = XR_ERROR_RUNTIME_FAILURE;
    AHardwareBuffer* woven = nullptr;  // set on the first submit and after a resize
    int release_fd = -1;
    std::optional<DisplayXR::TrackedEyes> eyes;
  } job;
  std::atomic<bool> stalled{false};

  // ---- render thread ----
  EglApi egl;
  bool egl_loaded = false;
  AhbTarget input, output;
  bool failure_reported = false;

  void Fail(std::string message)
  {
    ERROR_LOG_FMT(VIDEO, "DisplayXR: {}", message);
    std::lock_guard lock(mutex);
    error = std::move(message);
    state = State::Failed;
    cv.notify_all();
  }

  void Wake() const
  {
    const u64 one = 1;
    if (command_fd >= 0)
      (void)!write(command_fd, &one, sizeof(one));
  }

  bool UseSyncFd() const { return weave_version >= 20; }

  template <typename T>
  bool GetProc(const char* name, T* out)
  {
    *out = nullptr;
    return xr_gipa(instance, name, reinterpret_cast<PFN_xrVoidFunction*>(out)) == XR_SUCCESS &&
           *out;
  }

  // ======================= session thread ===================================
  void ThreadMain()
  {
    JNIEnv* env = nullptr;
    JavaVMAttachArgs args{JNI_VERSION_1_6, "DisplayXR session", nullptr};
    const bool attached = vm && vm->AttachCurrentThread(&env, &args) == JNI_OK;

    // The runtime may post display-processor setup to the calling thread's Looper, so this thread
    // owns one for its whole life. The command eventfd sits on it.
    ALooper* looper = ALooper_prepare(ALOOPER_PREPARE_ALLOW_NON_CALLBACKS);
    ALooper_addFd(looper, command_fd, LOOPER_IDENT_COMMAND, ALOOPER_EVENT_INPUT, nullptr, nullptr);

    if (BringUp())
    {
      std::lock_guard lock(mutex);
      state = State::Ready;
      cv.notify_all();
      NOTICE_LOG_FMT(VIDEO, "DisplayXR: session ready (XR_DXR_weave v{}, sync fd {})",
                     weave_version, UseSyncFd() ? "on" : "off");
    }

    while (true)
    {
      if (ALooper_pollOnce(-1, nullptr, nullptr, nullptr) == LOOPER_IDENT_COMMAND)
      {
        u64 value;
        (void)!read(command_fd, &value, sizeof(value));
      }
      std::unique_lock lock(mutex);
      if (quit)
        break;
      if (state != State::Ready)
      {
        // Answer any job, so the render thread never waits on a failed session.
        if (job.pending)
        {
          job.pending = false;
          job.done = true;
          job.result = XR_ERROR_RUNTIME_FAILURE;
          cv.notify_all();
        }
        continue;
      }
      if (geometry_dirty)
      {
        geometry_dirty = false;
        const DisplayXR::WindowOrigin o = origin;
        const u32 w = window_width, h = window_height;
        lock.unlock();
        BindWindow(o, w, h);
        lock.lock();
      }
      if (job.pending)
      {
        Job j = job;  // the render thread is waiting for the result
        lock.unlock();
        Submit(j);
        lock.lock();
        job = j;
        job.pending = false;
        job.done = true;
        cv.notify_all();
      }
    }

    TearDown();
    ALooper_removeFd(looper, command_fd);
    if (attached)
      vm->DetachCurrentThread();
  }

  bool BringUp()
  {
    // The Khronos loader for Android, packaged with the app. It finds the installed runtime.
    void* loader = dlopen("libopenxr_loader.so", RTLD_NOW | RTLD_LOCAL);
    if (!loader)
    {
      Fail(std::string("libopenxr_loader.so not found: ") + dlerror());
      return false;
    }
    xr_gipa = reinterpret_cast<PFN_xrGetInstanceProcAddr>(dlsym(loader, "xrGetInstanceProcAddr"));
    if (!xr_gipa)
    {
      Fail("the OpenXR loader exports no xrGetInstanceProcAddr");
      return false;
    }

    PFN_xrInitializeLoaderKHR initialize_loader = nullptr;
    if (GetProc("xrInitializeLoaderKHR", &initialize_loader))
    {
      XrLoaderInitInfoAndroidKHR init{XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR};
      init.applicationVM = vm;
      init.applicationContext = context;
      initialize_loader(reinterpret_cast<const XrLoaderInitInfoBaseHeaderKHR*>(&init));
    }

    PFN_xrEnumerateInstanceExtensionProperties enumerate_extensions = nullptr;
    PFN_xrCreateInstance create_instance = nullptr;
    if (!GetProc("xrEnumerateInstanceExtensionProperties", &enumerate_extensions) ||
        !GetProc("xrCreateInstance", &create_instance))
    {
      Fail("OpenXR global entry points missing");
      return false;
    }

    u32 count = 0;
    if (enumerate_extensions(nullptr, 0, &count, nullptr) != XR_SUCCESS)
    {
      Fail("no OpenXR runtime installed");
      return false;
    }
    std::vector<XrExtensionProperties> extensions(count, {XR_TYPE_EXTENSION_PROPERTIES});
    enumerate_extensions(nullptr, count, &count, extensions.data());
    bool have_display_info = false;
    for (const XrExtensionProperties& e : extensions)
    {
      if (!std::strcmp(e.extensionName, XR_DXR_WEAVE_EXTENSION_NAME))
        weave_version = e.extensionVersion;
      if (!std::strcmp(e.extensionName, XR_DXR_DISPLAY_INFO_EXTENSION_NAME))
        have_display_info = true;
    }
    if (weave_version == 0)
    {
      Fail("the installed OpenXR runtime has no XR_DXR_weave");
      return false;
    }

    std::vector<const char*> enabled = {XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME,
                                        XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME,
                                        XR_DXR_WEAVE_EXTENSION_NAME};
    if (have_display_info)
      enabled.push_back(XR_DXR_DISPLAY_INFO_EXTENSION_NAME);

    XrInstanceCreateInfoAndroidKHR android_info{XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR};
    android_info.applicationVM = vm;
    android_info.applicationActivity = context;
    XrInstanceCreateInfo create_info{XR_TYPE_INSTANCE_CREATE_INFO};
    create_info.next = &android_info;
    create_info.enabledExtensionCount = static_cast<u32>(enabled.size());
    create_info.enabledExtensionNames = enabled.data();
    std::strncpy(create_info.applicationInfo.applicationName, "Dolphin",
                 XR_MAX_APPLICATION_NAME_SIZE - 1);
    std::strncpy(create_info.applicationInfo.engineName, "Dolphin", XR_MAX_ENGINE_NAME_SIZE - 1);
    create_info.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);

    XrResult result = XR_ERROR_RUNTIME_UNAVAILABLE;
    for (int attempt = 0; attempt < 5 && result == XR_ERROR_RUNTIME_UNAVAILABLE; ++attempt)
    {
      result = create_instance(&create_info, &instance);
      if (result == XR_ERROR_RUNTIME_UNAVAILABLE)
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    if (result != XR_SUCCESS)
    {
      Fail(fmt::format("xrCreateInstance failed: {}", static_cast<int>(result)));
      return false;
    }

    PFN_xrGetSystem get_system = nullptr;
    if (!GetProc("xrDestroyInstance", &xr_destroy_instance) ||
        !GetProc("xrDestroySession", &xr_destroy_session) || !GetProc("xrGetSystem", &get_system))
    {
      Fail("OpenXR core entry points missing");
      return false;
    }

    XrSystemGetInfo system_info{XR_TYPE_SYSTEM_GET_INFO};
    system_info.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    if (get_system(instance, &system_info, &system) != XR_SUCCESS)
    {
      system_info.formFactor = XR_FORM_FACTOR_HANDHELD_DISPLAY;
      result = get_system(instance, &system_info, &system);
      if (result != XR_SUCCESS)
      {
        Fail(fmt::format("xrGetSystem failed: {}", static_cast<int>(result)));
        return false;
      }
    }

    if (!CreateSession())
      return false;

    if (!GetProc("xrWeaveBindWindow2DXR", &xr_bind_window) ||
        !GetProc("xrWeaveSubmitDXR", &xr_submit))
    {
      Fail("XR_DXR_weave entry points missing (runtime older than v7)");
      return false;
    }

    DisplayXR::WindowOrigin o;
    u32 w, h;
    {
      std::lock_guard lock(mutex);
      o = origin;
      w = window_width;
      h = window_height;
      geometry_dirty = false;
    }
    if (w && h)
      BindWindow(o, w, h);
    return true;
  }

  // A weave-only session never runs a frame loop; it only has to exist. Its graphics binding is a
  // private Vulkan device created through the runtime, which therefore enables whatever the
  // runtime needs on it. Dolphin's own GL context is not involved.
  bool CreateSession()
  {
    void* vulkan = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
    vk_gipa =
        vulkan ?
            reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(vulkan, "vkGetInstanceProcAddr")) :
            nullptr;
    if (!vk_gipa)
    {
      Fail("libvulkan.so not available");
      return false;
    }

    PFN_xrGetVulkanGraphicsRequirements2KHR get_requirements = nullptr;
    PFN_xrCreateVulkanInstanceKHR create_vk_instance = nullptr;
    PFN_xrGetVulkanGraphicsDevice2KHR get_vk_physical_device = nullptr;
    PFN_xrCreateVulkanDeviceKHR create_vk_device = nullptr;
    PFN_xrCreateSession create_session = nullptr;
    if (!GetProc("xrGetVulkanGraphicsRequirements2KHR", &get_requirements) ||
        !GetProc("xrCreateVulkanInstanceKHR", &create_vk_instance) ||
        !GetProc("xrGetVulkanGraphicsDevice2KHR", &get_vk_physical_device) ||
        !GetProc("xrCreateVulkanDeviceKHR", &create_vk_device) ||
        !GetProc("xrCreateSession", &create_session))
    {
      Fail("XR_KHR_vulkan_enable2 entry points missing");
      return false;
    }
    XrGraphicsRequirementsVulkanKHR requirements{XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN_KHR};
    get_requirements(instance, system, &requirements);

    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "Dolphin";
    app.apiVersion = VK_MAKE_API_VERSION(0, XR_VERSION_MAJOR(requirements.minApiVersionSupported),
                                         XR_VERSION_MINOR(requirements.minApiVersionSupported), 0);
    VkInstanceCreateInfo vk_instance_info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    vk_instance_info.pApplicationInfo = &app;
    XrVulkanInstanceCreateInfoKHR xr_instance_info{XR_TYPE_VULKAN_INSTANCE_CREATE_INFO_KHR};
    xr_instance_info.systemId = system;
    xr_instance_info.pfnGetInstanceProcAddr = vk_gipa;
    xr_instance_info.vulkanCreateInfo = &vk_instance_info;
    VkResult vk_result = VK_SUCCESS;
    if (create_vk_instance(instance, &xr_instance_info, &vk_instance, &vk_result) != XR_SUCCESS ||
        vk_result != VK_SUCCESS)
    {
      Fail(
          fmt::format("xrCreateVulkanInstanceKHR failed (Vulkan {})", static_cast<int>(vk_result)));
      return false;
    }
    vk_destroy_instance =
        reinterpret_cast<PFN_vkDestroyInstance>(vk_gipa(vk_instance, "vkDestroyInstance"));
    vk_destroy_device =
        reinterpret_cast<PFN_vkDestroyDevice>(vk_gipa(vk_instance, "vkDestroyDevice"));
    const auto get_queue_families = reinterpret_cast<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(
        vk_gipa(vk_instance, "vkGetPhysicalDeviceQueueFamilyProperties"));

    XrVulkanGraphicsDeviceGetInfoKHR device_get_info{XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR};
    device_get_info.systemId = system;
    device_get_info.vulkanInstance = vk_instance;
    VkPhysicalDevice physical_device = VK_NULL_HANDLE;
    if (get_vk_physical_device(instance, &device_get_info, &physical_device) != XR_SUCCESS ||
        !get_queue_families)
    {
      Fail("xrGetVulkanGraphicsDevice2KHR failed");
      return false;
    }
    u32 family_count = 0;
    get_queue_families(physical_device, &family_count, nullptr);
    std::vector<VkQueueFamilyProperties> families(family_count);
    get_queue_families(physical_device, &family_count, families.data());
    u32 family = 0;
    for (u32 i = 0; i < family_count; ++i)
    {
      if (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)
      {
        family = i;
        break;
      }
    }
    const float priority = 1.0f;
    VkDeviceQueueCreateInfo queue_info{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queue_info.queueFamilyIndex = family;
    queue_info.queueCount = 1;
    queue_info.pQueuePriorities = &priority;
    VkDeviceCreateInfo vk_device_info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    vk_device_info.queueCreateInfoCount = 1;
    vk_device_info.pQueueCreateInfos = &queue_info;
    XrVulkanDeviceCreateInfoKHR xr_device_info{XR_TYPE_VULKAN_DEVICE_CREATE_INFO_KHR};
    xr_device_info.systemId = system;
    xr_device_info.pfnGetInstanceProcAddr = vk_gipa;
    xr_device_info.vulkanPhysicalDevice = physical_device;
    xr_device_info.vulkanCreateInfo = &vk_device_info;
    if (create_vk_device(instance, &xr_device_info, &vk_device, &vk_result) != XR_SUCCESS ||
        vk_result != VK_SUCCESS)
    {
      Fail(fmt::format("xrCreateVulkanDeviceKHR failed (Vulkan {})", static_cast<int>(vk_result)));
      return false;
    }

    XrGraphicsBindingVulkanKHR binding{XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR};
    binding.instance = vk_instance;
    binding.physicalDevice = physical_device;
    binding.device = vk_device;
    binding.queueFamilyIndex = family;
    binding.queueIndex = 0;
    XrSessionCreateInfo session_info{XR_TYPE_SESSION_CREATE_INFO};
    session_info.next = &binding;
    session_info.systemId = system;
    const XrResult result = create_session(instance, &session_info, &session);
    if (result != XR_SUCCESS)
    {
      Fail(fmt::format("xrCreateSession failed: {}", static_cast<int>(result)));
      return false;
    }

    // Debug: with debug.dolphin.displayxr.eyes_manual set to 1, ask for MANUAL eye tracking, so
    // the display stays in 3D with nobody in front of it (useful when testing without a viewer).
    char value[PROP_VALUE_MAX] = {};
    __system_property_get("debug.dolphin.displayxr.eyes_manual", value);
    if (value[0] == '1')
    {
      PFN_xrRequestEyeTrackingModeDXR request_mode = nullptr;
      const XrResult r = GetProc("xrRequestEyeTrackingModeDXR", &request_mode) ?
                             request_mode(session, XR_EYE_TRACKING_MODE_MANUAL_DXR) :
                             XR_ERROR_FUNCTION_UNSUPPORTED;
      WARN_LOG_FMT(VIDEO, "DisplayXR: MANUAL eye tracking requested: {}", static_cast<int>(r));
    }
    return true;
  }

  void BindWindow(const DisplayXR::WindowOrigin& o, u32 w, u32 h)
  {
    // Android has no window handle to bind: the surface's position on the screen is the phase
    // anchor for the interlace.
    XrWeaveWindowGeometryDXR geometry{XR_TYPE_WEAVE_WINDOW_GEOMETRY_DXR};
    geometry.windowOriginOnScreen = {o.x, o.y};
    geometry.clientSize = {static_cast<s32>(w), static_cast<s32>(h)};
    geometry.displayId = o.display_id;
    XrWeaveBindWindowInfoDXR bind{XR_TYPE_WEAVE_BIND_WINDOW_INFO_DXR};
    bind.next = &geometry;
    bind.windowHandle = nullptr;
    const XrResult result = xr_bind_window(session, &bind);
    INFO_LOG_FMT(VIDEO, "DisplayXR: bind window {},{} {}x{} display {}: {}", o.x, o.y, w, h,
                 o.display_id, static_cast<int>(result));
  }

  void Submit(Job& j)
  {
    // Legacy layout: the whole input is one 2x1 side-by-side atlas exactly the size of the rect,
    // and the rect is the whole window (the input, the rect and the bound window agree).
    XrWeaveSubmitHandlesDXR kinds{XR_TYPE_WEAVE_SUBMIT_HANDLES_DXR};
    kinds.inputKind = XR_WEAVE_HANDLE_KIND_AHARDWAREBUFFER_DXR;
    kinds.overlayKind = XR_WEAVE_HANDLE_KIND_PLATFORM_DEFAULT_DXR;

    XrWeaveSubmitInfoDXR info{XR_TYPE_WEAVE_SUBMIT_INFO_DXR};
    info.next = &kinds;
    info.inputTexture = j.input;
    info.rect = {{0, 0}, {static_cast<s32>(j.width), static_cast<s32>(j.height)}};
    info.firstChunk = XR_TRUE;

    XrWeaveOutputDXR out{XR_TYPE_WEAVE_OUTPUT_DXR};
    XrWeaveSubmitSyncDXR sync_in{XR_TYPE_WEAVE_SUBMIT_SYNC_DXR};
    XrWeaveOutputSyncDXR sync_out{XR_TYPE_WEAVE_OUTPUT_SYNC_DXR};
    if (UseSyncFd())
    {
      sync_in.next = info.next;
      sync_in.acquireFenceFd = j.acquire_fd;  // the runtime's on success
      info.next = &sync_in;
      sync_out.releaseFenceFd = -1;
      out.next = &sync_out;
    }

    j.result = xr_submit(session, &info, &out);
    j.woven = nullptr;
    j.release_fd = -1;
    if (j.result != XR_SUCCESS)
    {
      WARN_LOG_FMT(VIDEO, "DisplayXR: xrWeaveSubmitDXR failed: {}", static_cast<int>(j.result));
      return;  // the acquire fd stays ours; the render thread closes it
    }
    if (UseSyncFd())
      j.acquire_fd = -1;
    j.woven = static_cast<AHardwareBuffer*>(out.weavedTexture);
    j.release_fd = UseSyncFd() ? sync_out.releaseFenceFd : -1;
    // Display space, meters, x to the right from the panel's center; for the next frame's cameras.
    j.eyes.reset();
    if (out.eyesValid && out.eyesTracking && out.eyeCount >= 2)
      j.eyes = DisplayXR::TrackedEyes{out.eyes[0].x, out.eyes[1].x};
  }

  void TearDown()
  {
    if (session != XR_NULL_HANDLE)
      xr_destroy_session(session);
    if (vk_device != VK_NULL_HANDLE)
      vk_destroy_device(vk_device, nullptr);
    if (vk_instance != VK_NULL_HANDLE)
      vk_destroy_instance(vk_instance, nullptr);
    if (instance != XR_NULL_HANDLE)
      xr_destroy_instance(instance);
    session = XR_NULL_HANDLE;
    vk_device = VK_NULL_HANDLE;
    vk_instance = VK_NULL_HANDLE;
    instance = XR_NULL_HANDLE;
  }

  // ======================= render thread ====================================
  bool Start()
  {
    if (!s_ahb.allocate && !s_ahb.Load())
    {
      Fail("AHardwareBuffer needs Android 8.0 or newer");
      return false;
    }
    JNIEnv* env = IDCache::GetEnvForThread();
    env->GetJavaVM(&vm);
    context = GetApplicationContextGlobalRef();
    if (!context)
    {
      Fail("no application context");
      return false;
    }
    command_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (command_fd < 0)
    {
      Fail("eventfd failed");
      return false;
    }
    quit = false;
    error.clear();
    state = State::Starting;
    thread = std::thread([this] { ThreadMain(); });
    return true;
  }

  void Stop()
  {
    if (thread.joinable())
    {
      {
        std::lock_guard lock(mutex);
        quit = true;
      }
      Wake();
      thread.join();
    }
    if (command_fd >= 0)
      close(command_fd);
    command_fd = -1;
    if (egl_loaded && eglGetCurrentContext() != EGL_NO_CONTEXT)
    {
      input.Destroy(egl);
      output.Destroy(egl);
    }
    if (context)
      IDCache::GetEnvForThread()->DeleteGlobalRef(context);
    context = nullptr;
    job = Job{};
    stalled = false;
    failure_reported = false;
    geometry_dirty = false;
    window_width = window_height = 0;
    DisplayXR::SetTrackedEyes(std::nullopt);
    state = State::Stopped;
  }

  bool EnsureInput(u32 w, u32 h)
  {
    if (input.ahb && input.width == w && input.height == h)
      return true;
    input.Destroy(egl);
    AHardwareBuffer_Desc desc{};
    desc.width = w;
    desc.height = h;
    desc.layers = 1;
    desc.format = AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM;
    desc.usage = AHARDWAREBUFFER_USAGE_GPU_COLOR_OUTPUT | AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE;
    AHardwareBuffer* buffer = nullptr;
    if (s_ahb.allocate(&desc, &buffer) != 0 || !buffer)
    {
      ERROR_LOG_FMT(VIDEO, "DisplayXR: AHardwareBuffer_allocate {}x{} failed", w, h);
      return false;
    }
    if (!input.Wrap(egl, buffer))
    {
      input.Destroy(egl);
      return false;
    }
    return true;
  }

  int MakeAcquireFence()
  {
    if (!egl.have_fences || !UseSyncFd())
    {
      glFinish();  // synchronous contract: writes complete before the submit
      return -1;
    }
    const EGLDisplay display = eglGetCurrentDisplay();
    const EGLSyncKHR sync = egl.create_sync(display, EGL_SYNC_NATIVE_FENCE_ANDROID, nullptr);
    glFlush();  // the fence fd exists only once the fence is flushed
    const int fd = sync != EGL_NO_SYNC_KHR ? egl.dup_native_fence_fd(display, sync) : -1;
    if (sync != EGL_NO_SYNC_KHR)
      egl.destroy_sync(display, sync);
    if (fd < 0)
      glFinish();
    return fd;
  }

  void GpuWaitRelease(int fd)
  {
    if (fd < 0)
      return;  // already complete
    const EGLDisplay display = eglGetCurrentDisplay();
    const EGLint attribs[] = {EGL_SYNC_NATIVE_FENCE_FD_ANDROID, fd, EGL_NONE};
    const EGLSyncKHR sync = egl.create_sync(display, EGL_SYNC_NATIVE_FENCE_ANDROID, attribs);
    if (sync == EGL_NO_SYNC_KHR)
    {
      close(fd);
      glFinish();
      return;
    }
    egl.wait_sync(display, sync, 0);  // EGL owns the fd now
    egl.destroy_sync(display, sync);
  }

  bool AdoptOutput(AHardwareBuffer* woven)
  {
    output.Destroy(egl);
    if (output.Wrap(egl, woven))
      return true;
    output.Destroy(egl);
    return false;
  }

  bool Weave(u32 w, u32 h)
  {
    if (state == State::Stopped && !Start())
      return false;
    if (state == State::Failed)
    {
      if (!failure_reported)
      {
        failure_reported = true;
        std::lock_guard lock(mutex);
        OSD::AddMessage("DisplayXR: " + error + ". Showing 2D.", 10000);
      }
      return false;
    }
    if (state != State::Ready || w < 2 || h < 1)
      return false;

    // A submit that outlived its wait still owns the input buffer: skip frames until it returns.
    if (stalled)
    {
      Job late;
      {
        std::lock_guard lock(mutex);
        if (job.pending && !job.done)
          return false;
        late = job;
        job = Job{};
      }
      stalled = false;
      GLStateGuard guard;
      if (late.acquire_fd >= 0)
        close(late.acquire_fd);
      if (late.result == XR_SUCCESS && late.woven)
        AdoptOutput(late.woven);
      GpuWaitRelease(late.release_fd);  // our next writes must follow the runtime's reads
    }

    if (!egl_loaded)
    {
      egl_loaded = egl.Load();
      if (!egl_loaded)
      {
        Fail("EGL_ANDROID_get_native_client_buffer, EGL_KHR_image or GL_OES_EGL_image missing");
        return false;
      }
    }

    // Publish the window geometry: the surface origin from the host, the size of the backbuffer.
    {
      const DisplayXR::WindowOrigin o = DisplayXR::GetWindowOrigin();
      std::lock_guard lock(mutex);
      if (o.x != origin.x || o.y != origin.y || o.display_id != origin.display_id ||
          w != window_width || h != window_height)
      {
        origin = o;
        window_width = w;
        window_height = h;
        geometry_dirty = true;
      }
    }

    GLStateGuard guard;
    if (!EnsureInput(w, h))
      return false;

    // 1. The side-by-side backbuffer into the input buffer, 1:1. GL rows run bottom-up and
    //    AHardwareBuffer rows top-down, hence the flip. The previous frame's release fence was
    //    waited on before its woven copy, so this write follows the runtime's last read.
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, input.framebuffer);
    glBlitFramebuffer(0, 0, w, h, 0, h, w, 0, GL_COLOR_BUFFER_BIT, GL_NEAREST);

    // 2. One acquire fence covers the input write and the previous woven read (GL runs in order;
    //    the woven buffer is single-buffered).
    const int acquire_fd = MakeAcquireFence();

    // 3. Hand the job to the session thread and wait for the submit to return.
    Job result;
    {
      std::unique_lock lock(mutex);
      job = Job{};
      job.pending = true;
      job.input = input.ahb;
      job.width = w;
      job.height = h;
      job.acquire_fd = acquire_fd;
      Wake();
      // Never hold the emulator's video thread hostage to a blocked submit.
      if (!cv.wait_for(lock, SUBMIT_WAIT, [&] { return job.done || state == State::Failed; }))
      {
        stalled = true;
        WARN_LOG_FMT(VIDEO, "DisplayXR: submit blocked for over {} ms", SUBMIT_WAIT.count());
        return false;
      }
      result = job;
      job = Job{};
    }
    if (result.acquire_fd >= 0)
      close(result.acquire_fd);  // not consumed
    if (result.result != XR_SUCCESS)
      return false;

    // 4. A new woven buffer comes back on the first submit and after a resize.
    if (result.woven && !AdoptOutput(result.woven))
    {
      if (result.release_fd >= 0)
        close(result.release_fd);
      return false;
    }
    if (!output.framebuffer && output.ahb && !output.Rewrap(egl))
      output.Destroy(egl);
    if (!output.framebuffer)
    {
      if (result.release_fd >= 0)
        close(result.release_fd);
      return false;
    }

    // 5. Wait (on the GPU) for the weave, then copy the woven image into the backbuffer: 1:1 and
    //    unfiltered, since the interlace pattern is per pixel.
    GpuWaitRelease(result.release_fd);
    DisplayXR::SetTrackedEyes(result.eyes);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, output.framebuffer);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    const GLint ow = static_cast<GLint>(output.width), oh = static_cast<GLint>(output.height);
    glBlitFramebuffer(0, 0, ow, oh, 0, h, ow, static_cast<GLint>(h) - oh, GL_COLOR_BUFFER_BIT,
                      GL_NEAREST);
    return true;
  }
};

DisplayXRWeave::DisplayXRWeave() : m_impl(std::make_unique<Impl>())
{
}

DisplayXRWeave::~DisplayXRWeave()
{
  m_impl->Stop();
}

bool DisplayXRWeave::WeaveBackbuffer(u32 width, u32 height)
{
  return m_impl->Weave(width, height);
}

void DisplayXRWeave::Stop()
{
  m_impl->Stop();
}
}  // namespace OGL
