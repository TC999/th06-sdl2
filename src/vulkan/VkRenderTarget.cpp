// SPDX-License-Identifier: MIT
// Fix 17 — see VkRenderTarget.hpp for the design.
//
// Persistent OFFSCREEN color image so HUD/UI draws survive across frames
// (Vulkan equivalent of GL backend's persistent FBO). EndFrame in the renderer
// blits this image into the acquired swapchain image.
#include "VkRenderTarget.hpp"

#include "VkContext.hpp"
#include "VkSwapchain.hpp"

#include <cstdio>

namespace th06::vk {

VkRenderTarget::~VkRenderTarget() {
    // Caller must Destroy() with a live ctx before dropping us. Asserting here would crash
    // on normal shutdown order, so just no-op.
}

bool VkRenderTarget::Create(VkContext& ctx, const VkSwapchain& swap) {
    extent_      = swap.extent();
    colorFormat_ = swap.imageFormat();
    if (!createRenderPass(ctx, colorFormat_))              return false;
    if (!createOffscreenColor(ctx, extent_, colorFormat_)) return false;
    if (!createDepthResources(ctx, extent_))               return false;
    if (!createFramebuffer(ctx))                           return false;
    if (!initialClearAndTransition(ctx))                   return false;
    return true;
}

void VkRenderTarget::Destroy(VkContext& ctx) {
    VkDevice dev = ctx.device();
    if (dev == VK_NULL_HANDLE) return;
    VmaAllocator alloc = ctx.allocator();

    if (framebuffer_) { vkDestroyFramebuffer(dev, framebuffer_, nullptr); framebuffer_ = VK_NULL_HANDLE; }

    if (depthView_)   { vkDestroyImageView(dev, depthView_, nullptr); depthView_ = VK_NULL_HANDLE; }
    if (depthImage_ && alloc) { vmaDestroyImage(alloc, depthImage_, depthAlloc_); depthImage_ = VK_NULL_HANDLE; depthAlloc_ = VK_NULL_HANDLE; }

    if (colorView_)   { vkDestroyImageView(dev, colorView_, nullptr); colorView_ = VK_NULL_HANDLE; }
    if (colorImage_ && alloc) { vmaDestroyImage(alloc, colorImage_, colorAlloc_); colorImage_ = VK_NULL_HANDLE; colorAlloc_ = VK_NULL_HANDLE; }

    if (renderPass_) { vkDestroyRenderPass(dev, renderPass_, nullptr); renderPass_ = VK_NULL_HANDLE; }
}

bool VkRenderTarget::createRenderPass(VkContext& ctx, VkFormat colorFormat) {
    VkAttachmentDescription attachments[2] = {};

    // Color (persistent offscreen) — LOAD_OP_LOAD so prior frame content survives.
    // Initial/final = COLOR_ATTACHMENT_OPTIMAL: render pass leaves the image in
    // that layout; EndFrame transitions it to TRANSFER_SRC for blit then back.
    attachments[0].format         = colorFormat;
    attachments[0].samples        = VK_SAMPLE_COUNT_1_BIT;
    attachments[0].loadOp         = VK_ATTACHMENT_LOAD_OP_LOAD;
    attachments[0].storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
    attachments[0].stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[0].initialLayout  = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    attachments[0].finalLayout    = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    // Depth — transient, cleared every frame.
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

    // External -> subpass: ensure prior color writes and EndFrame blit transitions
    // are visible before we LOAD_OP_LOAD this frame.
    VkSubpassDependency deps[2] = {};
    deps[0].srcSubpass    = VK_SUBPASS_EXTERNAL;
    deps[0].dstSubpass    = 0;
    deps[0].srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT
                          | VK_PIPELINE_STAGE_TRANSFER_BIT;
    deps[0].dstStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    deps[0].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT
                          | VK_ACCESS_TRANSFER_READ_BIT;
    deps[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT
                          | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

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

bool VkRenderTarget::createOffscreenColor(VkContext& ctx, VkExtent2D extent, VkFormat format) {
    VkImageCreateInfo ici = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ici.imageType   = VK_IMAGE_TYPE_2D;
    ici.format      = format;
    ici.extent      = { extent.width, extent.height, 1 };
    ici.mipLevels   = 1;
    ici.arrayLayers = 1;
    ici.samples     = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling      = VK_IMAGE_TILING_OPTIMAL;
    ici.usage       = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT
                    | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    ici.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VmaAllocationCreateInfo aci{};
    aci.usage         = VMA_MEMORY_USAGE_AUTO;
    aci.requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;

    VkResult r = vmaCreateImage(ctx.allocator(), &ici, &aci,
                                &colorImage_, &colorAlloc_, nullptr);
    if (r != VK_SUCCESS) {
        std::fprintf(stderr, "[VkRenderTarget] vmaCreateImage(color) -> %d\n", int(r));
        return false;
    }

    VkImageViewCreateInfo ivci = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    ivci.image    = colorImage_;
    ivci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    ivci.format   = format;
    ivci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    ivci.subresourceRange.levelCount = 1;
    ivci.subresourceRange.layerCount = 1;
    TH_VK_CHECK(vkCreateImageView(ctx.device(), &ivci, nullptr, &colorView_));
    return true;
}

bool VkRenderTarget::createDepthResources(VkContext& ctx, VkExtent2D extent) {
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

    VmaAllocationCreateInfo aci{};
    aci.usage         = VMA_MEMORY_USAGE_AUTO;
    aci.requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;

    VkResult r = vmaCreateImage(ctx.allocator(), &ici, &aci,
                                &depthImage_, &depthAlloc_, nullptr);
    if (r != VK_SUCCESS) {
        std::fprintf(stderr, "[VkRenderTarget] vmaCreateImage(depth) -> %d\n", int(r));
        return false;
    }

    VkImageViewCreateInfo ivci = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    ivci.image    = depthImage_;
    ivci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    ivci.format   = depthFormat_;
    ivci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    ivci.subresourceRange.levelCount = 1;
    ivci.subresourceRange.layerCount = 1;
    TH_VK_CHECK(vkCreateImageView(ctx.device(), &ivci, nullptr, &depthView_));
    return true;
}

bool VkRenderTarget::createFramebuffer(VkContext& ctx) {
    VkImageView attachments[2] = { colorView_, depthView_ };
    VkFramebufferCreateInfo fci = { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
    fci.renderPass      = renderPass_;
    fci.attachmentCount = 2;
    fci.pAttachments    = attachments;
    fci.width           = extent_.width;
    fci.height          = extent_.height;
    fci.layers          = 1;
    TH_VK_CHECK(vkCreateFramebuffer(ctx.device(), &fci, nullptr, &framebuffer_));
    return true;
}

bool VkRenderTarget::initialClearAndTransition(VkContext& ctx) {
    // One-shot cmd buffer: clear color image to black + transition UNDEFINED →
    // COLOR_ATTACHMENT_OPTIMAL so first frame's LOAD_OP_LOAD has well-defined data.
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandPoolCreateInfo cpci = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    cpci.queueFamilyIndex = ctx.graphicsQueueFamily();
    cpci.flags            = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    TH_VK_CHECK(vkCreateCommandPool(ctx.device(), &cpci, nullptr, &pool));

    VkCommandBuffer cb = VK_NULL_HANDLE;
    VkCommandBufferAllocateInfo cbai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    cbai.commandPool        = pool;
    cbai.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = 1;
    TH_VK_CHECK(vkAllocateCommandBuffers(ctx.device(), &cbai, &cb));

    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    TH_VK_CHECK(vkBeginCommandBuffer(cb, &bi));

    VkImageSubresourceRange range = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

    // UNDEFINED -> TRANSFER_DST_OPTIMAL
    {
        VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
        b.srcAccessMask = 0;
        b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        b.oldLayout     = VK_IMAGE_LAYOUT_UNDEFINED;
        b.newLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image         = colorImage_;
        b.subresourceRange = range;
        vkCmdPipelineBarrier(cb,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &b);
    }

    VkClearColorValue clear = {};
    clear.float32[0] = 0.0f; clear.float32[1] = 0.0f;
    clear.float32[2] = 0.0f; clear.float32[3] = 1.0f;
    vkCmdClearColorImage(cb, colorImage_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         &clear, 1, &range);

    // TRANSFER_DST_OPTIMAL -> COLOR_ATTACHMENT_OPTIMAL
    {
        VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
        b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT
                        | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        b.oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.newLayout     = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image         = colorImage_;
        b.subresourceRange = range;
        vkCmdPipelineBarrier(cb,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
            0, 0, nullptr, 0, nullptr, 1, &b);
    }

    TH_VK_CHECK(vkEndCommandBuffer(cb));

    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.commandBufferCount = 1;
    si.pCommandBuffers    = &cb;
    TH_VK_CHECK(vkQueueSubmit(ctx.graphicsQueue(), 1, &si, VK_NULL_HANDLE));
    vkQueueWaitIdle(ctx.graphicsQueue());

    vkDestroyCommandPool(ctx.device(), pool, nullptr);
    return true;
}

}  // namespace th06::vk
