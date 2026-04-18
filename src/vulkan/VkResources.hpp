// SPDX-License-Identifier: MIT
// Phase 3 — bundled Vulkan-side resources kept outside the renderpass / pipeline cache:
//
//   1. VkUploadHeap         per-frame host-visible vertex staging ring buffer (VMA-backed)
//   2. VkDefaultTexture     1x1 white image + sampler + descriptor set (used as fallback
//                           when SetTexture(0) is called or texture id is unknown)
//   3. LoadSpvFile          read SPIR-V + vkCreateShaderModule helper
//
// All allocations now go through VMA (Phase 3, ADR-002 Amendment 2026-04-19).
#pragma once

#include "VmaUsage.hpp"
#include <cstdint>
#include <vector>
#include <string>

namespace th06::vk {

class VkContext;

// ----- Per-frame vertex staging ring -----------------------------------------------------
class VkUploadHeap {
public:
    static constexpr uint32_t kFramesInFlight    = 2;
    static constexpr VkDeviceSize kBytesPerFrame = 1u * 1024u * 1024u;  // 1 MB / frame

    VkUploadHeap()  = default;
    ~VkUploadHeap();

    bool Init(VkContext& ctx);
    void Shutdown(VkContext& ctx);

    // Call at start of frame `frameIdx % kFramesInFlight`. Resets the offset.
    void BeginFrame(uint32_t frameIdx);

    // Allocate `size` bytes for vertex data. Writes pointer + buffer + offset.
    // Returns false if heap exhausted (caller should fail / drop draw).
    bool AllocVerts(VkDeviceSize size,
                    void**       outMapped,
                    VkBuffer*    outBuffer,
                    VkDeviceSize* outOffset);

private:
    struct PerFrame {
        VkBuffer        buffer    = VK_NULL_HANDLE;
        VmaAllocation   alloc     = VK_NULL_HANDLE;
        void*           mapped    = nullptr;
        VkDeviceSize    offset    = 0;
    };
    PerFrame frames_[kFramesInFlight];
    uint32_t currentFrame_ = 0;
};

// ----- 1x1 white texture + sampler + descriptor --------------------------------------------
class VkDefaultTexture {
public:
    VkDefaultTexture()  = default;
    ~VkDefaultTexture();

    bool Init(VkContext& ctx,
              VkDescriptorPool      pool,
              VkDescriptorSetLayout texLayout);
    void Shutdown(VkContext& ctx);

    VkDescriptorSet descriptorSet() const { return descSet_; }

private:
    VkImage         image_   = VK_NULL_HANDLE;
    VmaAllocation   alloc_   = VK_NULL_HANDLE;
    VkImageView     view_    = VK_NULL_HANDLE;
    VkSampler       sampler_ = VK_NULL_HANDLE;
    VkDescriptorSet descSet_ = VK_NULL_HANDLE;
};

// ----- SPIR-V loader -----------------------------------------------------------------------
bool LoadSpvFile(const std::string& path, std::vector<uint32_t>& outWords);
bool CreateShaderModule(VkContext& ctx,
                        const std::vector<uint32_t>& words,
                        VkShaderModule* outMod);

}  // namespace th06::vk
