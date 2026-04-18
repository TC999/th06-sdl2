// SPDX-License-Identifier: MIT
// Phase 2 — see VkRenderTarget.hpp for the design + memory allocation carve-out note.
#include "VkRenderTarget.hpp"

#include "VkContext.hpp"
#include "VkSwapchain.hpp"

#include <cstdio>

namespace th06::vk {

VkRenderTarget::~VkRenderTarget() {
    // Caller must Destroy() with a live ctx before dropping us. Asserting here would crash
    // on normal shutdown order, so just no-op.
}

uint32_t FindMemoryTypeIndex(VkPhysicalDevice phys,
                             uint32_t typeBits,
                             VkMemoryPropertyFlags required) {
    VkPhysicalDeviceMemoryProperties props;
    vkGetPhysicalDeviceMemoryProperties(phys, &props);
    for (uint32_t i = 0; i < props.memoryTypeCount; ++i) {
        const bool typeOk = (typeBits & (1u << i)) != 0;
        const bool propOk = (props.memoryTypes[i].propertyFlags & required) == required;
        if (typeOk && propOk) {
            return i;
        }
    }
    return UINT32_MAX;
}

bool VkRenderTarget::Create(VkContext& ctx, const VkSwapchain& swap) {
    extent_ = swap.extent();
    if (!createRenderPass(ctx, swap.imageFormat())) return false;
    if (!createDepthResources(ctx, extent_, swap.imageCount())) return false;
    if (!createFramebuffers(ctx, swap)) return false;
    return true;
}

void VkRenderTarget::Destroy(VkContext& ctx) {
    VkDevice dev = ctx.device();
    if (dev == VK_NULL_HANDLE) return;

    for (auto fb : framebuffers_) if (fb)  vkDestroyFramebuffer(dev, fb, nullptr);
    for (auto v  : depthViews_)   if (v)   vkDestroyImageView(dev, v, nullptr);
    for (auto i  : depthImages_)  if (i)   vkDestroyImage(dev, i, nullptr);
    for (auto m  : depthMemories_)if (m)   vkFreeMemory(dev, m, nullptr);
    framebuffers_.clear();
    depthViews_.clear();
    depthImages_.clear();
    depthMemories_.clear();

    if (renderPass_) {
        vkDestroyRenderPass(dev, renderPass_, nullptr);
        renderPass_ = VK_NULL_HANDLE;
    }
}

bool VkRenderTarget::createRenderPass(VkContext& ctx, VkFormat colorFormat) {
    VkAttachmentDescription attachments[2] = {};

    // Color
    attachments[0].format         = colorFormat;
    attachments[0].samples        = VK_SAMPLE_COUNT_1_BIT;
    attachments[0].loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachments[0].storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
    attachments[0].stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[0].initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
    attachments[0].finalLayout    = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    // Depth
    attachments[1].format         = depthFormat_;
    attachments[1].samples        = VK_SAMPLE_COUNT_1_BIT;
    attachments[1].loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachments[1].storeOp        = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[1].stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[1].initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
    attachments[1].finalLayout    = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkAttachmentReference colorRef = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkAttachmentReference depthRef = { 1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };

    VkSubpassDescription subpass = {};
    subpass.pipelineBindPoint    = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments    = &colorRef;
    subpass.pDepthStencilAttachment = &depthRef;

    // External -> subpass dependency: ensures swapchain image acquire semaphore wait completes
    // before color writes; and depth read/write before depth tests.
    VkSubpassDependency deps[2] = {};
    deps[0].srcSubpass    = VK_SUBPASS_EXTERNAL;
    deps[0].dstSubpass    = 0;
    deps[0].srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    deps[0].dstStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    deps[0].srcAccessMask = 0;
    deps[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

    deps[1].srcSubpass    = VK_SUBPASS_EXTERNAL;
    deps[1].dstSubpass    = 0;
    deps[1].srcStageMask  = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT
                          | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    deps[1].dstStageMask  = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT
                          | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    deps[1].srcAccessMask = 0;
    deps[1].dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

    VkRenderPassCreateInfo rpci = { VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
    rpci.attachmentCount = 2;
    rpci.pAttachments    = attachments;
    rpci.subpassCount    = 1;
    rpci.pSubpasses      = &subpass;
    rpci.dependencyCount = 2;
    rpci.pDependencies   = deps;

    TH_VK_CHECK(vkCreateRenderPass(ctx.device(), &rpci, nullptr, &renderPass_));
    return true;
}

bool VkRenderTarget::createDepthResources(VkContext& ctx, VkExtent2D extent, uint32_t count) {
    depthImages_.resize(count, VK_NULL_HANDLE);
    depthMemories_.resize(count, VK_NULL_HANDLE);
    depthViews_.resize(count, VK_NULL_HANDLE);

    for (uint32_t i = 0; i < count; ++i) {
        VkImageCreateInfo ici = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
        ici.imageType   = VK_IMAGE_TYPE_2D;
        ici.format      = depthFormat_;
        ici.extent      = { extent.width, extent.height, 1 };
        ici.mipLevels   = 1;
        ici.arrayLayers = 1;
        ici.samples     = VK_SAMPLE_COUNT_1_BIT;
        ici.tiling      = VK_IMAGE_TILING_OPTIMAL;
        ici.usage       = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
        ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        TH_VK_CHECK(vkCreateImage(ctx.device(), &ici, nullptr, &depthImages_[i]));

        VkMemoryRequirements req;
        vkGetImageMemoryRequirements(ctx.device(), depthImages_[i], &req);
        uint32_t typeIdx = FindMemoryTypeIndex(ctx.physicalDevice(),
                                               req.memoryTypeBits,
                                               VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (typeIdx == UINT32_MAX) {
            std::fprintf(stderr, "[VkRenderTarget] no DEVICE_LOCAL memory type for depth\n");
            return false;
        }

        VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        mai.allocationSize  = req.size;
        mai.memoryTypeIndex = typeIdx;
        // PHASE 2 CARVE-OUT: bare vkAllocateMemory until VMA lands in Phase 3.
        TH_VK_CHECK(vkAllocateMemory(ctx.device(), &mai, nullptr, &depthMemories_[i]));
        TH_VK_CHECK(vkBindImageMemory(ctx.device(), depthImages_[i], depthMemories_[i], 0));

        VkImageViewCreateInfo ivci = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        ivci.image    = depthImages_[i];
        ivci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        ivci.format   = depthFormat_;
        ivci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        ivci.subresourceRange.levelCount = 1;
        ivci.subresourceRange.layerCount = 1;
        TH_VK_CHECK(vkCreateImageView(ctx.device(), &ivci, nullptr, &depthViews_[i]));
    }
    return true;
}

bool VkRenderTarget::createFramebuffers(VkContext& ctx, const VkSwapchain& swap) {
    const uint32_t n = swap.imageCount();
    framebuffers_.resize(n, VK_NULL_HANDLE);
    for (uint32_t i = 0; i < n; ++i) {
        VkImageView attachments[2] = { swap.imageView(i), depthViews_[i] };
        VkFramebufferCreateInfo fci = { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
        fci.renderPass      = renderPass_;
        fci.attachmentCount = 2;
        fci.pAttachments    = attachments;
        fci.width           = extent_.width;
        fci.height          = extent_.height;
        fci.layers          = 1;
        TH_VK_CHECK(vkCreateFramebuffer(ctx.device(), &fci, nullptr, &framebuffers_[i]));
    }
    return true;
}

}  // namespace th06::vk
