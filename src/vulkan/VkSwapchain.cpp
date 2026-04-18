// SPDX-License-Identifier: MIT
#include "VkSwapchain.hpp"
#include "VkContext.hpp"

#include <algorithm>
#include <cstdio>

namespace th06::vk {

namespace {

VkSurfaceFormatKHR ChooseSurfaceFormat(const std::vector<VkSurfaceFormatKHR>& avail, bool srgb) {
    if (srgb) {
        for (auto& f : avail) {
            if ((f.format == VK_FORMAT_B8G8R8A8_SRGB || f.format == VK_FORMAT_R8G8B8A8_SRGB)
                && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
                return f;
        }
    }
    for (auto& f : avail) {
        if ((f.format == VK_FORMAT_B8G8R8A8_UNORM || f.format == VK_FORMAT_R8G8B8A8_UNORM)
            && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
            return f;
    }
    return avail.front();
}

VkPresentModeKHR ChoosePresentMode(const std::vector<VkPresentModeKHR>& avail, bool vsync) {
    if (!vsync) {
        for (auto m : avail) if (m == VK_PRESENT_MODE_MAILBOX_KHR)   return m;
        for (auto m : avail) if (m == VK_PRESENT_MODE_IMMEDIATE_KHR) return m;
    }
    return VK_PRESENT_MODE_FIFO_KHR;  // 保证支持
}

VkExtent2D ChooseExtent(const VkSurfaceCapabilitiesKHR& caps, uint32_t w, uint32_t h) {
    if (caps.currentExtent.width != UINT32_MAX) return caps.currentExtent;
    VkExtent2D e{ w ? w : caps.minImageExtent.width,
                  h ? h : caps.minImageExtent.height };
    e.width  = std::max(caps.minImageExtent.width,  std::min(caps.maxImageExtent.width,  e.width));
    e.height = std::max(caps.minImageExtent.height, std::min(caps.maxImageExtent.height, e.height));
    return e;
}

}  // namespace

VkSwapchain::~VkSwapchain() {
    // Destroy 必须在 ctx 仍然活着时显式调，析构里没 ctx 拿不到 device。
    // 这里只是保险——如果 swapchain_ 仍非空，说明用户漏调 Destroy。
    if (swapchain_ != VK_NULL_HANDLE) {
        std::fprintf(stderr,
            "[VK WARN] VkSwapchain destroyed without explicit Destroy(ctx); leaking.\n");
    }
}

bool VkSwapchain::Recreate(VkContext& ctx, const SwapchainCreateInfo& info) {
    VkPhysicalDevice phys = ctx.physicalDevice();
    VkSurfaceKHR     surf = ctx.surface();
    VkDevice         dev  = ctx.device();

    VkSurfaceCapabilitiesKHR caps{};
    TH_VK_CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(phys, surf, &caps));

    uint32_t fmtCount = 0;
    TH_VK_CHECK(vkGetPhysicalDeviceSurfaceFormatsKHR(phys, surf, &fmtCount, nullptr));
    std::vector<VkSurfaceFormatKHR> fmts(fmtCount);
    TH_VK_CHECK(vkGetPhysicalDeviceSurfaceFormatsKHR(phys, surf, &fmtCount, fmts.data()));

    uint32_t pmCount = 0;
    TH_VK_CHECK(vkGetPhysicalDeviceSurfacePresentModesKHR(phys, surf, &pmCount, nullptr));
    std::vector<VkPresentModeKHR> pms(pmCount);
    TH_VK_CHECK(vkGetPhysicalDeviceSurfacePresentModesKHR(phys, surf, &pmCount, pms.data()));

    VkSurfaceFormatKHR sf  = ChooseSurfaceFormat(fmts, info.srgb);
    VkPresentModeKHR   pm  = ChoosePresentMode(pms, info.vsync);
    VkExtent2D         ext = ChooseExtent(caps, info.requestedWidth, info.requestedHeight);

    if (ext.width == 0 || ext.height == 0) {
        // 窗口被最小化；保留旧 swapchain，调用方应等下一次 resize。
        return false;
    }

    uint32_t desired = caps.minImageCount + 1;
    if (caps.maxImageCount != 0 && desired > caps.maxImageCount) desired = caps.maxImageCount;

    VkSwapchainKHR oldSwap = swapchain_;
    VkSwapchainCreateInfoKHR ci{};
    ci.sType            = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    ci.surface          = surf;
    ci.minImageCount    = desired;
    ci.imageFormat      = sf.format;
    ci.imageColorSpace  = sf.colorSpace;
    ci.imageExtent      = ext;
    ci.imageArrayLayers = 1;
    ci.imageUsage       = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT
                        | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.preTransform     = caps.currentTransform;
    ci.compositeAlpha   = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    ci.presentMode      = pm;
    ci.clipped          = VK_TRUE;
    ci.oldSwapchain     = oldSwap;

    VkSwapchainKHR newSwap = VK_NULL_HANDLE;
    TH_VK_CHECK(vkCreateSwapchainKHR(dev, &ci, nullptr, &newSwap));

    // 旧资源析构（顺序：先 views，再 oldSwap）
    destroyViews(ctx);
    if (oldSwap != VK_NULL_HANDLE) {
        vkDestroySwapchainKHR(dev, oldSwap, nullptr);
    }

    swapchain_   = newSwap;
    imageFormat_ = sf.format;
    colorSpace_  = sf.colorSpace;
    extent_      = ext;

    uint32_t imgCount = 0;
    TH_VK_CHECK(vkGetSwapchainImagesKHR(dev, swapchain_, &imgCount, nullptr));
    images_.resize(imgCount);
    TH_VK_CHECK(vkGetSwapchainImagesKHR(dev, swapchain_, &imgCount, images_.data()));

    views_.resize(imgCount, VK_NULL_HANDLE);
    for (uint32_t i = 0; i < imgCount; ++i) {
        VkImageViewCreateInfo vci{};
        vci.sType                       = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vci.image                       = images_[i];
        vci.viewType                    = VK_IMAGE_VIEW_TYPE_2D;
        vci.format                      = imageFormat_;
        vci.components                  = { VK_COMPONENT_SWIZZLE_IDENTITY,
                                            VK_COMPONENT_SWIZZLE_IDENTITY,
                                            VK_COMPONENT_SWIZZLE_IDENTITY,
                                            VK_COMPONENT_SWIZZLE_IDENTITY };
        vci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        vci.subresourceRange.levelCount = 1;
        vci.subresourceRange.layerCount = 1;
        TH_VK_CHECK(vkCreateImageView(dev, &vci, nullptr, &views_[i]));
    }

    std::fprintf(stderr,
        "[VK] Swapchain ready: %ux%u format=%d presentMode=%d images=%u\n",
        extent_.width, extent_.height, int(imageFormat_), int(pm), imgCount);
    return true;
}

void VkSwapchain::destroyViews(VkContext& ctx) {
    VkDevice dev = ctx.device();
    for (auto v : views_) {
        if (v != VK_NULL_HANDLE) vkDestroyImageView(dev, v, nullptr);
    }
    views_.clear();
    images_.clear();
}

void VkSwapchain::Destroy(VkContext& ctx) {
    VkDevice dev = ctx.device();
    if (dev != VK_NULL_HANDLE) vkDeviceWaitIdle(dev);
    destroyViews(ctx);
    if (swapchain_ != VK_NULL_HANDLE && dev != VK_NULL_HANDLE) {
        vkDestroySwapchainKHR(dev, swapchain_, nullptr);
    }
    swapchain_   = VK_NULL_HANDLE;
    imageFormat_ = VK_FORMAT_UNDEFINED;
    extent_      = { 0, 0 };
}

VkResult VkSwapchain::AcquireNext(VkContext& ctx, VkSemaphore signalSem, uint32_t* outIndex) {
    return vkAcquireNextImageKHR(ctx.device(), swapchain_, UINT64_MAX,
                                 signalSem, VK_NULL_HANDLE, outIndex);
}

}  // namespace th06::vk
