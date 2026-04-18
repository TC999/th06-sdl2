// SPDX-License-Identifier: MIT
// Phase 1 skeleton — Swapchain 创建 / 重建 / 镜像视图 / acquire。
#pragma once

#include <volk.h>
#include <vector>
#include <cstdint>

namespace th06::vk {

class VkContext;

struct SwapchainCreateInfo {
    uint32_t requestedWidth  = 0;   // 0 表示用 surface 当前 extent
    uint32_t requestedHeight = 0;
    bool     vsync           = true;
    bool     srgb            = false;  // Phase 1 默认线性，Phase 4 ImGui 切 sRGB
};

class VkSwapchain {
public:
    VkSwapchain()  = default;
    ~VkSwapchain();

    VkSwapchain(const VkSwapchain&)            = delete;
    VkSwapchain& operator=(const VkSwapchain&) = delete;

    // 首次创建 / 重建（窗口大小变化、out-of-date 时调）
    bool Recreate(VkContext& ctx, const SwapchainCreateInfo& info);
    void Destroy(VkContext& ctx);

    // 拿下一帧的 image index；若返回 VK_ERROR_OUT_OF_DATE_KHR，调用方应触发 Recreate。
    VkResult AcquireNext(VkContext& ctx, VkSemaphore signalSem, uint32_t* outIndex);

    VkSwapchainKHR handle()        const { return swapchain_; }
    VkFormat       imageFormat()   const { return imageFormat_; }
    VkColorSpaceKHR colorSpace()   const { return colorSpace_; }
    VkExtent2D     extent()        const { return extent_; }
    uint32_t       imageCount()    const { return uint32_t(images_.size()); }
    VkImage        image(uint32_t i)     const { return images_[i]; }
    VkImageView    imageView(uint32_t i) const { return views_[i]; }

private:
    void destroyViews(VkContext& ctx);

    VkSwapchainKHR           swapchain_   = VK_NULL_HANDLE;
    VkFormat                 imageFormat_ = VK_FORMAT_UNDEFINED;
    VkColorSpaceKHR          colorSpace_  = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    VkExtent2D               extent_      { 0, 0 };
    std::vector<VkImage>     images_;
    std::vector<VkImageView> views_;
};

}  // namespace th06::vk
