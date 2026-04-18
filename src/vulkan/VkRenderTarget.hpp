// SPDX-License-Identifier: MIT
// Fix 17: persistent OFFSCREEN color image (FBO equivalent of GL backend).
//
// Design (one color + one depth, one subpass, ONE framebuffer):
//   - color: format = swapchain (B8G8R8A8_UNORM),
//            LOAD_OP_LOAD, STORE_OP_STORE,
//            initial = COLOR_ATTACHMENT_OPTIMAL, final = COLOR_ATTACHMENT_OPTIMAL
//            (persistent across frames — matches GL FBO so HUD draws survive).
//   - depth: format = D32_SFLOAT, LOAD_OP_CLEAR, STORE_OP_DONT_CARE.
//
// EndFrame blits this offscreen color image into the acquired swapchain image
// and transitions the swap image to PRESENT_SRC. At creation we issue a
// one-shot clear-to-black + transition to COLOR_ATTACHMENT_OPTIMAL so the
// very first LOAD_OP_LOAD has well-defined source data.
#pragma once

#include "VmaUsage.hpp"
#include <cstdint>

namespace th06::vk {

class VkContext;
class VkSwapchain;

class VkRenderTarget {
public:
    VkRenderTarget()  = default;
    ~VkRenderTarget();

    VkRenderTarget(const VkRenderTarget&)            = delete;
    VkRenderTarget& operator=(const VkRenderTarget&) = delete;

    // Build the render pass + offscreen color + depth + framebuffer.
    // Recreate by calling Destroy() then Create() on swapchain change.
    bool Create(VkContext& ctx, const VkSwapchain& swap);
    void Destroy(VkContext& ctx);

    VkRenderPass  renderPass()                  const { return renderPass_; }
    VkFramebuffer framebuffer()                 const { return framebuffer_; }
    VkFramebuffer framebuffer(uint32_t /*idx*/) const { return framebuffer_; }
    VkFormat      depthFormat()                 const { return depthFormat_; }
    VkFormat      colorFormat()                 const { return colorFormat_; }
    VkExtent2D    extent()                      const { return extent_; }

    // Persistent offscreen color image (used for blit-to-swapchain in EndFrame
    // and for screenshot readback).
    VkImage       colorImage()                  const { return colorImage_; }

private:
    bool createRenderPass(VkContext& ctx, VkFormat colorFormat);
    bool createOffscreenColor(VkContext& ctx, VkExtent2D extent, VkFormat format);
    bool createDepthResources(VkContext& ctx, VkExtent2D extent);
    bool createFramebuffer(VkContext& ctx);
    bool initialClearAndTransition(VkContext& ctx);

    VkRenderPass   renderPass_  = VK_NULL_HANDLE;
    VkFormat       depthFormat_ = VK_FORMAT_D32_SFLOAT;
    VkFormat       colorFormat_ = VK_FORMAT_UNDEFINED;
    VkExtent2D     extent_      { 0, 0 };

    VkImage        colorImage_  = VK_NULL_HANDLE;
    VmaAllocation  colorAlloc_  = VK_NULL_HANDLE;
    VkImageView    colorView_   = VK_NULL_HANDLE;

    VkImage        depthImage_  = VK_NULL_HANDLE;
    VmaAllocation  depthAlloc_  = VK_NULL_HANDLE;
    VkImageView    depthView_   = VK_NULL_HANDLE;

    VkFramebuffer  framebuffer_ = VK_NULL_HANDLE;
};

}  // namespace th06::vk
