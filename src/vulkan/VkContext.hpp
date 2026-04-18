// SPDX-License-Identifier: MIT
// Phase 1 skeleton — Vulkan 实例 / 物理设备 / 逻辑设备 / 队列 / 内存分配器。
// 设计参考 PLAN.md Phase 1 + ADR-001 (单队列 graphics+present)。
#pragma once

#include <volk.h>
#include <SDL_video.h>
#include <cstdint>

namespace th06::vk {

// 中央错误检查宏。所有 VkResult 调用必须套用，否则 PLAN HARD RULE #14 会被违反。
#define TH_VK_CHECK(expr)                                                      \
    do {                                                                       \
        VkResult _r = (expr);                                                  \
        if (_r != VK_SUCCESS) {                                                \
            ::th06::vk::FatalVk(#expr, _r, __FILE__, __LINE__);                \
        }                                                                      \
    } while (0)

void FatalVk(const char* expr, VkResult r, const char* file, int line);
const char* VkResultToString(VkResult r);

struct VkContextCreateInfo {
    SDL_Window* window           = nullptr;
    bool        enableValidation = false;  // 仅 Debug 推荐 true；x86 下 SDK 1.4 无 layer 时会自动回退
    const char* appName          = "th06_sdl";
    uint32_t    appVersion       = 0;
};

// 持有 instance / surface / physicalDevice / device / queue / VMA allocator。
// 不持有 swapchain（swapchain 由 VkSwapchain 管），因为 swapchain 重建生命周期独立。
class VkContext {
public:
    VkContext()  = default;
    ~VkContext();

    // 不可拷贝
    VkContext(const VkContext&)            = delete;
    VkContext& operator=(const VkContext&) = delete;

    bool Init(const VkContextCreateInfo& info);
    void Shutdown();

    VkInstance       instance()       const { return instance_; }
    VkSurfaceKHR     surface()        const { return surface_; }
    VkPhysicalDevice physicalDevice() const { return physicalDevice_; }
    VkDevice         device()         const { return device_; }
    VkQueue          graphicsQueue()  const { return graphicsQueue_; }
    uint32_t         graphicsQueueFamily() const { return graphicsQueueFamily_; }
    bool             validationActive() const { return validationActive_; }

    // VMA 在 Phase 3 真正使用，Phase 1 仅占位。
    // VmaAllocator allocator() const { return allocator_; }

private:
    bool createInstance(const VkContextCreateInfo& info);
    bool createSurface(SDL_Window* window);
    bool pickPhysicalDevice();
    bool createDeviceAndQueue();
    void setupDebugMessenger();

    VkInstance               instance_              = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT debugMessenger_        = VK_NULL_HANDLE;
    VkSurfaceKHR             surface_               = VK_NULL_HANDLE;
    VkPhysicalDevice         physicalDevice_        = VK_NULL_HANDLE;
    VkDevice                 device_                = VK_NULL_HANDLE;
    VkQueue                  graphicsQueue_         = VK_NULL_HANDLE;
    uint32_t                 graphicsQueueFamily_   = UINT32_MAX;
    bool                     validationActive_      = false;
    bool                     debugUtilsLoaded_      = false;
};

}  // namespace th06::vk
