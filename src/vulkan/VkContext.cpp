// SPDX-License-Identifier: MIT
#include "VkContext.hpp"

#include <SDL_log.h>
#include <SDL_vulkan.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace th06::vk {

const char* VkResultToString(VkResult r) {
    switch (r) {
#define _C(x) case x: return #x;
        _C(VK_SUCCESS) _C(VK_NOT_READY) _C(VK_TIMEOUT) _C(VK_EVENT_SET) _C(VK_EVENT_RESET)
        _C(VK_INCOMPLETE) _C(VK_ERROR_OUT_OF_HOST_MEMORY) _C(VK_ERROR_OUT_OF_DEVICE_MEMORY)
        _C(VK_ERROR_INITIALIZATION_FAILED) _C(VK_ERROR_DEVICE_LOST) _C(VK_ERROR_MEMORY_MAP_FAILED)
        _C(VK_ERROR_LAYER_NOT_PRESENT) _C(VK_ERROR_EXTENSION_NOT_PRESENT)
        _C(VK_ERROR_FEATURE_NOT_PRESENT) _C(VK_ERROR_INCOMPATIBLE_DRIVER) _C(VK_ERROR_TOO_MANY_OBJECTS)
        _C(VK_ERROR_FORMAT_NOT_SUPPORTED) _C(VK_ERROR_FRAGMENTED_POOL) _C(VK_ERROR_UNKNOWN)
        _C(VK_ERROR_SURFACE_LOST_KHR) _C(VK_ERROR_NATIVE_WINDOW_IN_USE_KHR) _C(VK_SUBOPTIMAL_KHR)
        _C(VK_ERROR_OUT_OF_DATE_KHR) _C(VK_ERROR_INCOMPATIBLE_DISPLAY_KHR)
        _C(VK_ERROR_VALIDATION_FAILED_EXT)
#undef _C
        default: return "VK_ERROR_<unknown>";
    }
}

void FatalVk(const char* expr, VkResult r, const char* file, int line) {
    std::fprintf(stderr, "[VK FATAL] %s:%d: %s -> %s (%d)\n",
                 file, line, expr, VkResultToString(r), int(r));
    std::fflush(stderr);
    std::abort();
}

// ---- 内部辅助 -------------------------------------------------------------

namespace {

constexpr const char* kValidationLayerName = "VK_LAYER_KHRONOS_validation";

bool LayerAvailable(const char* name) {
    uint32_t count = 0;
    if (vkEnumerateInstanceLayerProperties(&count, nullptr) != VK_SUCCESS) return false;
    std::vector<VkLayerProperties> layers(count);
    if (vkEnumerateInstanceLayerProperties(&count, layers.data()) != VK_SUCCESS) return false;
    for (const auto& l : layers) {
        if (std::strcmp(l.layerName, name) == 0) return true;
    }
    return false;
}

VKAPI_ATTR VkBool32 VKAPI_CALL DebugMessengerCallback(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT,
    const VkDebugUtilsMessengerCallbackDataEXT* data,
    void*) {
    const char* sev = "INFO";
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)        sev = "ERROR";
    else if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) sev = "WARN";
    else if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT)    sev = "INFO";
    else if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT) sev = "VERB";
    std::fprintf(stderr, "[VK %s] %s\n", sev, data->pMessage ? data->pMessage : "(null)");
    return VK_FALSE;
}

}  // namespace

// ---- VkContext -----------------------------------------------------------

VkContext::~VkContext() { Shutdown(); }

bool VkContext::Init(const VkContextCreateInfo& info) {
    if (!info.window) {
        std::fprintf(stderr, "[VK] VkContext::Init: window is null\n");
        return false;
    }
    // volk: load vulkan-1.dll + bootstrap proc-addr resolver.
    // Idempotent: subsequent calls return VK_SUCCESS without reloading.
    {
        VkResult vr = volkInitialize();
        if (vr != VK_SUCCESS) {
            std::fprintf(stderr,
                "[VK] volkInitialize failed (%s); is vulkan-1.dll missing?\n",
                VkResultToString(vr));
            return false;
        }
    }
    if (!createInstance(info))   return false;
    if (info.enableValidation && validationActive_) {
        setupDebugMessenger();
    }
    if (!createSurface(info.window))  return false;
    if (!pickPhysicalDevice())        return false;
    if (!createDeviceAndQueue())      return false;
    return true;
}

void VkContext::Shutdown() {
    if (device_ != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(device_);
        vkDestroyDevice(device_, nullptr);
        device_ = VK_NULL_HANDLE;
    }
    if (surface_ != VK_NULL_HANDLE && instance_ != VK_NULL_HANDLE) {
        vkDestroySurfaceKHR(instance_, surface_, nullptr);
        surface_ = VK_NULL_HANDLE;
    }
    if (debugMessenger_ != VK_NULL_HANDLE && instance_ != VK_NULL_HANDLE) {
        auto destroy = (PFN_vkDestroyDebugUtilsMessengerEXT)
            vkGetInstanceProcAddr(instance_, "vkDestroyDebugUtilsMessengerEXT");
        if (destroy) destroy(instance_, debugMessenger_, nullptr);
        debugMessenger_ = VK_NULL_HANDLE;
    }
    if (instance_ != VK_NULL_HANDLE) {
        vkDestroyInstance(instance_, nullptr);
        instance_ = VK_NULL_HANDLE;
    }
    physicalDevice_      = VK_NULL_HANDLE;
    graphicsQueue_       = VK_NULL_HANDLE;
    graphicsQueueFamily_ = UINT32_MAX;
    validationActive_    = false;
    debugUtilsLoaded_    = false;
}

bool VkContext::createInstance(const VkContextCreateInfo& info) {
    // SDL2 提供需要的实例扩展（VK_KHR_surface + 平台 surface 扩展）
    unsigned int extCount = 0;
    if (!SDL_Vulkan_GetInstanceExtensions(info.window, &extCount, nullptr)) {
        std::fprintf(stderr, "[VK] SDL_Vulkan_GetInstanceExtensions count failed: %s\n",
                     SDL_GetError());
        return false;
    }
    std::vector<const char*> exts(extCount);
    if (!SDL_Vulkan_GetInstanceExtensions(info.window, &extCount, exts.data())) {
        std::fprintf(stderr, "[VK] SDL_Vulkan_GetInstanceExtensions list failed: %s\n",
                     SDL_GetError());
        return false;
    }

    bool wantValidation = info.enableValidation;
    bool layerOk = false;
    // volk: vkEnumerateInstanceLayerProperties is loaded by volkInitialize via the global
    // proc-addr table, so it's safe to query before any instance exists.
    if (wantValidation) layerOk = LayerAvailable(kValidationLayerName);
    if (wantValidation && !layerOk) {
        std::fprintf(stderr,
            "[VK WARN] %s not available; continuing without validation. "
            "(SDK 1.4 dropped 32-bit Windows layers.)\n",
            kValidationLayerName);
    }
    if (layerOk) {
        exts.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        debugUtilsLoaded_ = true;
    }

    VkApplicationInfo appInfo{};
    appInfo.sType              = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.pApplicationName   = info.appName;
    appInfo.applicationVersion = info.appVersion;
    appInfo.pEngineName        = "th06_sdl-vk";
    appInfo.engineVersion      = 1;
    appInfo.apiVersion         = VK_API_VERSION_1_1;

    VkInstanceCreateInfo ci{};
    ci.sType                   = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ci.pApplicationInfo        = &appInfo;
    ci.enabledExtensionCount   = uint32_t(exts.size());
    ci.ppEnabledExtensionNames = exts.empty() ? nullptr : exts.data();

    const char* layers[1] = { kValidationLayerName };
    if (layerOk) {
        ci.enabledLayerCount   = 1;
        ci.ppEnabledLayerNames = layers;
    }

    VkResult r = vkCreateInstance(&ci, nullptr, &instance_);
    if (r != VK_SUCCESS) {
        std::fprintf(stderr, "[VK] vkCreateInstance -> %s\n", VkResultToString(r));
        return false;
    }
    // volk: now bind instance-level function pointers (vkGetPhysicalDevice*, vkCreateDevice, etc.)
    volkLoadInstance(instance_);
    validationActive_ = layerOk;
    return true;
}

void VkContext::setupDebugMessenger() {
    if (!debugUtilsLoaded_) return;
    auto create = (PFN_vkCreateDebugUtilsMessengerEXT)
        vkGetInstanceProcAddr(instance_, "vkCreateDebugUtilsMessengerEXT");
    if (!create) return;

    VkDebugUtilsMessengerCreateInfoEXT ci{};
    ci.sType           = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
    ci.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT
                       | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    ci.messageType     = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT
                       | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT
                       | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    ci.pfnUserCallback = &DebugMessengerCallback;

    VkResult r = create(instance_, &ci, nullptr, &debugMessenger_);
    if (r != VK_SUCCESS) {
        std::fprintf(stderr, "[VK WARN] vkCreateDebugUtilsMessengerEXT -> %s\n",
                     VkResultToString(r));
        debugMessenger_ = VK_NULL_HANDLE;
    }
}

bool VkContext::createSurface(SDL_Window* window) {
    if (!SDL_Vulkan_CreateSurface(window, instance_, &surface_)) {
        std::fprintf(stderr, "[VK] SDL_Vulkan_CreateSurface failed: %s\n", SDL_GetError());
        return false;
    }
    return true;
}

bool VkContext::pickPhysicalDevice() {
    uint32_t count = 0;
    TH_VK_CHECK(vkEnumeratePhysicalDevices(instance_, &count, nullptr));
    if (count == 0) {
        std::fprintf(stderr, "[VK] No Vulkan-capable physical devices.\n");
        return false;
    }
    std::vector<VkPhysicalDevice> devs(count);
    TH_VK_CHECK(vkEnumeratePhysicalDevices(instance_, &count, devs.data()));

    auto scoreDevice = [this](VkPhysicalDevice d, uint32_t* outQueueFamily) -> int {
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(d, &props);
        uint32_t qfCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(d, &qfCount, nullptr);
        std::vector<VkQueueFamilyProperties> qfs(qfCount);
        vkGetPhysicalDeviceQueueFamilyProperties(d, &qfCount, qfs.data());
        for (uint32_t i = 0; i < qfCount; ++i) {
            if (!(qfs[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) continue;
            VkBool32 present = VK_FALSE;
            if (vkGetPhysicalDeviceSurfaceSupportKHR(d, i, surface_, &present) != VK_SUCCESS)
                continue;
            if (!present) continue;
            *outQueueFamily = i;
            int score = 1;
            if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)   score += 1000;
            if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU) score += 500;
            return score;
        }
        return -1;
    };

    int bestScore = -1;
    VkPhysicalDevice bestDev = VK_NULL_HANDLE;
    uint32_t bestQF = UINT32_MAX;
    for (auto d : devs) {
        uint32_t qf = UINT32_MAX;
        int s = scoreDevice(d, &qf);
        if (s > bestScore) { bestScore = s; bestDev = d; bestQF = qf; }
    }
    if (bestDev == VK_NULL_HANDLE) {
        std::fprintf(stderr, "[VK] No suitable physical device.\n");
        return false;
    }
    physicalDevice_      = bestDev;
    graphicsQueueFamily_ = bestQF;

    VkPhysicalDeviceProperties p{};
    vkGetPhysicalDeviceProperties(physicalDevice_, &p);
    std::fprintf(stderr, "[VK] Picked GPU: %s (api %u.%u.%u, queueFamily=%u)\n",
                 p.deviceName,
                 VK_VERSION_MAJOR(p.apiVersion),
                 VK_VERSION_MINOR(p.apiVersion),
                 VK_VERSION_PATCH(p.apiVersion),
                 graphicsQueueFamily_);
    return true;
}

bool VkContext::createDeviceAndQueue() {
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{};
    qci.sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qci.queueFamilyIndex = graphicsQueueFamily_;
    qci.queueCount       = 1;
    qci.pQueuePriorities = &prio;

    const char* devExts[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };

    VkPhysicalDeviceFeatures features{};
    // Phase 1 不启用任何高级特性；Phase 2/3 再按需补。

    VkDeviceCreateInfo dci{};
    dci.sType                   = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.queueCreateInfoCount    = 1;
    dci.pQueueCreateInfos       = &qci;
    dci.enabledExtensionCount   = 1;
    dci.ppEnabledExtensionNames = devExts;
    dci.pEnabledFeatures        = &features;

    TH_VK_CHECK(vkCreateDevice(physicalDevice_, &dci, nullptr, &device_));
    // volk: switch to device-level dispatch table (faster than instance-level for device fns).
    volkLoadDevice(device_);
    vkGetDeviceQueue(device_, graphicsQueueFamily_, 0, &graphicsQueue_);
    return true;
}

}  // namespace th06::vk
