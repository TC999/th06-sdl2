// SPDX-License-Identifier: MIT
// Phase 3 — TextureManager: VMA-backed VkImage + VkImageView + per-texture VkDescriptorSet.
//
// Design (per PLAN Phase 3 Done When):
//   - All allocations via VMA (block allocator amortizes VkDeviceMemory across many textures)
//   - Single shared sampler (nearest, clamp) — TH06 sprites are point-sampled
//   - Shared descriptor pool sized for many sets (1024 default; transparent realloc on overflow)
//   - All textures are VK_FORMAT_R8G8B8A8_UNORM (TH06 only uses RGBA32 after SDL conversion)
//   - Upload via transient staging buffer (vmaCreateBuffer HOST_VISIBLE) → vkCmdCopyBufferToImage
//   - One-shot command buffer per upload; vkQueueWaitIdle for simplicity (Phase 3; can be
//     batched in Phase 4 if hot path matters)
//
// Texture id 0 is reserved for "no texture" (caller falls back to default 1x1 white).
#pragma once

#include "VmaUsage.hpp"
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace th06::vk {

class VkContext;

class VkTextureManager {
public:
    VkTextureManager()  = default;
    ~VkTextureManager();

    VkTextureManager(const VkTextureManager&)            = delete;
    VkTextureManager& operator=(const VkTextureManager&) = delete;

    bool Init(VkContext& ctx, VkDescriptorSetLayout texLayout);
    void Shutdown(VkContext& ctx);

    // Allocate a new texture filled with raw RGBA bytes (or zero if rgba==nullptr).
    // Returns 0 on failure, else a non-zero opaque id usable with SetTexture / Update / Delete.
    uint32_t CreateFromRgba(VkContext& ctx, int w, int h, const uint8_t* rgba);

    // Allocate empty (zero-initialized) texture; equivalent to CreateFromRgba(.., nullptr).
    uint32_t CreateEmpty(VkContext& ctx, int w, int h);

    // Free GPU resources for a previously-created id.
    void Delete(VkContext& ctx, uint32_t id);

    // Replace a sub-rect with new RGBA bytes.
    bool UpdateSubImage(VkContext& ctx, uint32_t id,
                        int x, int y, int w, int h, const uint8_t* rgba);

    // Look up the descriptor set bound for shader access (combined image+sampler).
    // Returns VK_NULL_HANDLE if id is unknown.
    VkDescriptorSet GetDescriptorSet(uint32_t id) const;

    // Query dimensions; returns false if id unknown.
    bool GetSize(uint32_t id, int* outW, int* outH) const;

    // Diagnostic: how many textures are alive.
    size_t aliveCount() const { return entries_.size(); }

private:
    struct Entry {
        VkImage         image    = VK_NULL_HANDLE;
        VmaAllocation   alloc    = VK_NULL_HANDLE;
        VkImageView     view     = VK_NULL_HANDLE;
        VkDescriptorSet descSet  = VK_NULL_HANDLE;
        int             width    = 0;
        int             height   = 0;
    };

    bool growPool(VkContext& ctx);
    bool allocDescriptorSet(VkContext& ctx, VkDescriptorSet* outSet);
    bool createImageAndView(VkContext& ctx, int w, int h, Entry& out);
    bool uploadRect(VkContext& ctx, Entry& e, int x, int y, int w, int h, const uint8_t* rgba);
    void writeDescriptor(VkContext& ctx, Entry& e);

    VkDescriptorSetLayout layout_   = VK_NULL_HANDLE;
    VkSampler             sampler_  = VK_NULL_HANDLE;
    VkCommandPool         cmdPool_  = VK_NULL_HANDLE;

    // Pools grow on overflow (chain). Each pool holds kSetsPerPool sets.
    static constexpr uint32_t kSetsPerPool = 1024;
    std::vector<VkDescriptorPool> pools_;
    uint32_t                      poolUsed_ = 0;  // sets allocated from pools_.back()

    std::unordered_map<uint32_t, Entry> entries_;
    uint32_t nextId_ = 1;  // 0 is reserved
};

}  // namespace th06::vk
