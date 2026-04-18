// SPDX-License-Identifier: MIT
// Phase 1 skeleton — 双缓冲帧上下文：command pool/buffer + 信号量 + fence。
// MAX_FRAMES_IN_FLIGHT 与 swapchain image count 解耦：前者控 CPU/GPU 同步，后者控 surface。
#pragma once

#include <volk.h>
#include <array>
#include <cstdint>

namespace th06::vk {

class VkContext;

constexpr uint32_t kMaxFramesInFlight = 2;

struct FrameSync {
    VkCommandPool   pool        = VK_NULL_HANDLE;
    VkCommandBuffer cmd         = VK_NULL_HANDLE;
    VkSemaphore     imgAvail    = VK_NULL_HANDLE;
    VkSemaphore     renderDone  = VK_NULL_HANDLE;
    VkFence         inFlight    = VK_NULL_HANDLE;
};

class VkFrameContext {
public:
    VkFrameContext()  = default;
    ~VkFrameContext();

    VkFrameContext(const VkFrameContext&)            = delete;
    VkFrameContext& operator=(const VkFrameContext&) = delete;

    bool Init(VkContext& ctx);
    void Destroy(VkContext& ctx);

    // 选下一个 in-flight 槽位（不和 swapchain image 绑死）
    FrameSync& current()    { return frames_[currentIndex_]; }
    void       advance()    { currentIndex_ = (currentIndex_ + 1) % kMaxFramesInFlight; }
    uint32_t   index() const { return currentIndex_; }

private:
    std::array<FrameSync, kMaxFramesInFlight> frames_{};
    uint32_t currentIndex_ = 0;
};

}  // namespace th06::vk
