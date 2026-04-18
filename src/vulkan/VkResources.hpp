// SPDX-License-Identifier: MIT
// Phase 2 — bundled Vulkan-side resources kept outside the renderpass / pipeline cache:
//
//   1. VkUploadHeap         per-frame host-visible vertex staging ring buffer
//   2. VkDefaultTexture     1x1 white image + sampler + descriptor set (textured-path placeholder
//                           until Phase 3 wires real texture upload)
//   3. LoadSpvFile          read SPIR-V + vkCreateShaderModule helper
//
// Splitting these into 3 separate files would bloat the diff for what is a transient piece of
// Phase-2 plumbing — Phase 3 replaces #2 with a real texture manager and #1 may move to VMA.
#pragma once

#include <volk.h>
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
        VkBuffer       buffer = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        void*          mapped = nullptr;
        VkDeviceSize   offset = 0;
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
    VkDeviceMemory  memory_  = VK_NULL_HANDLE;
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
