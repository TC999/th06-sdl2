// SPDX-License-Identifier: MIT
#include "VkFrameContext.hpp"
#include "VkContext.hpp"

#include <cstdio>

namespace th06::vk {

VkFrameContext::~VkFrameContext() {
    for (auto& f : frames_) {
        if (f.pool != VK_NULL_HANDLE || f.imgAvail != VK_NULL_HANDLE
            || f.renderDone != VK_NULL_HANDLE || f.inFlight != VK_NULL_HANDLE) {
            std::fprintf(stderr,
                "[VK WARN] VkFrameContext destroyed without explicit Destroy(ctx); leaking.\n");
            break;
        }
    }
}

bool VkFrameContext::Init(VkContext& ctx) {
    VkDevice dev = ctx.device();
    VkCommandPoolCreateInfo pci{};
    pci.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pci.flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = ctx.graphicsQueueFamily();

    VkSemaphoreCreateInfo sci{};
    sci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    VkFenceCreateInfo fci{};
    fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;  // 第一帧不阻塞

    for (auto& f : frames_) {
        TH_VK_CHECK(vkCreateCommandPool(dev, &pci, nullptr, &f.pool));
        VkCommandBufferAllocateInfo cai{};
        cai.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cai.commandPool        = f.pool;
        cai.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cai.commandBufferCount = 1;
        TH_VK_CHECK(vkAllocateCommandBuffers(dev, &cai, &f.cmd));
        TH_VK_CHECK(vkCreateSemaphore(dev, &sci, nullptr, &f.imgAvail));
        TH_VK_CHECK(vkCreateSemaphore(dev, &sci, nullptr, &f.renderDone));
        TH_VK_CHECK(vkCreateFence(dev, &fci, nullptr, &f.inFlight));
    }
    currentIndex_ = 0;
    return true;
}

void VkFrameContext::Destroy(VkContext& ctx) {
    VkDevice dev = ctx.device();
    if (dev != VK_NULL_HANDLE) vkDeviceWaitIdle(dev);
    for (auto& f : frames_) {
        if (f.inFlight   != VK_NULL_HANDLE) vkDestroyFence(dev, f.inFlight, nullptr);
        if (f.renderDone != VK_NULL_HANDLE) vkDestroySemaphore(dev, f.renderDone, nullptr);
        if (f.imgAvail   != VK_NULL_HANDLE) vkDestroySemaphore(dev, f.imgAvail, nullptr);
        // command buffers 由 pool 释放
        if (f.pool       != VK_NULL_HANDLE) vkDestroyCommandPool(dev, f.pool, nullptr);
        f = FrameSync{};
    }
    currentIndex_ = 0;
}

}  // namespace th06::vk
