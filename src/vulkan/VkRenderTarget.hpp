// SPDX-License-Identifier: MIT
// Phase 3 — render pass + depth attachment + per-swap-image framebuffers.
//
// Single render pass design (one color + one depth, one subpass):
//   - color: format = swapchain (B8G8R8A8_UNORM), LOAD_OP_CLEAR,  STORE_OP_STORE,
//            initial = UNDEFINED, final = PRESENT_SRC_KHR
//   - depth: format = D32_SFLOAT (mandatory in Vulkan core), LOAD_OP_CLEAR, STORE_OP_DONT_CARE
//
// One depth image is allocated PER swapchain image. Recreated together with the swapchain.
// Phase 3 — VMA-managed (no raw vkAllocateMemory remains).
#pragma once

#include "VmaUsage.hpp"
#include <vector>
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

    // Build the render pass + per-image depth + per-image framebuffers.
    // Recreate by calling Destroy() then Create() on swapchain change.
    bool Create(VkContext& ctx, const VkSwapchain& swap);
    void Destroy(VkContext& ctx);

    VkRenderPass renderPass()                   const { return renderPass_; }
    VkFramebuffer framebuffer(uint32_t imgIdx)  const { return framebuffers_[imgIdx]; }
    VkFormat     depthFormat()                  const { return depthFormat_; }
    VkExtent2D   extent()                       const { return extent_; }

private:
    bool createRenderPass(VkContext& ctx, VkFormat colorFormat);
    bool createDepthResources(VkContext& ctx, VkExtent2D extent, uint32_t count);
    bool createFramebuffers(VkContext& ctx, const VkSwapchain& swap);

    VkRenderPass               renderPass_  = VK_NULL_HANDLE;
    VkFormat                   depthFormat_ = VK_FORMAT_D32_SFLOAT;
    VkExtent2D                 extent_      { 0, 0 };

    std::vector<VkImage>        depthImages_;
    std::vector<VmaAllocation>  depthAllocs_;
    std::vector<VkImageView>    depthViews_;
    std::vector<VkFramebuffer>  framebuffers_;
};

}  // namespace th06::vk
