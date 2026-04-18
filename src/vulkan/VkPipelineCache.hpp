// SPDX-License-Identifier: MIT
// Phase 2 — L2 pipeline cache. Maps VkPipelineKey -> VkPipeline lazily.
//
// Per ADR-007:
//   - L1 = IRenderer state (current{BlendMode,ColorOp,ZWriteDisable,VertexShader,Texture})
//   - L2 = VkPipelineKey (this file)  — what gets hashed
//   - The cache stores the resulting VkPipeline, plus owns ONE VkPipelineCache (the native
//     Vulkan one) so the driver can deduplicate compile work across keys.
//
// Pipeline creation inputs are split:
//   - PER-KEY (varies):    blend / depth / shader modules / vertex input / topology / spec const
//   - GLOBAL (one-shot):   render pass, dynamic state (viewport, scissor), pipeline layouts,
//                          shader modules (loaded once from SPV), descriptor set layouts.
//
// To keep this file focused on the L2 mapping logic, GLOBAL inputs are handed in via
// PipelineFactoryDeps. Loading SPV / building layouts lives in RendererVulkan::Init.
#pragma once

#include <volk.h>
#include <unordered_map>
#include <cstdint>

#include "VkPipelineKey.hpp"

namespace th06::vk {

class VkContext;

// Pre-built resources that are constant across the lifetime of the renderer.
struct PipelineFactoryDeps {
    VkRenderPass     renderPass         = VK_NULL_HANDLE;
    VkPipelineLayout layoutNoTex        = VK_NULL_HANDLE;  // push-const only
    VkPipelineLayout layoutTextured     = VK_NULL_HANDLE;  // push-const + set 0 binding 0

    // Indexed by VertexLayout enum (Count entries).
    VkShaderModule   vertModules[uint32_t(VertexLayout::Count)] = {};

    // [0] = f_color, [1] = f_textured.
    VkShaderModule   fragColor          = VK_NULL_HANDLE;
    VkShaderModule   fragTextured       = VK_NULL_HANDLE;
};

// NOTE: class is named `PipelineCache` (not `VkPipelineCache`) because the Vulkan handle
// `VkPipelineCache` collides with the class name on 32-bit (where the handle is typedef'd to
// uint64_t directly, not VkPipelineCache_T*). Keeping the file name VkPipelineCache.* matches
// the rest of the th06::vk module convention.
class PipelineCache {
public:
    PipelineCache()  = default;
    ~PipelineCache();

    PipelineCache(const PipelineCache&)            = delete;
    PipelineCache& operator=(const PipelineCache&) = delete;

    bool Init(VkContext& ctx, const PipelineFactoryDeps& deps);
    void Shutdown(VkContext& ctx);

    // Returns a VkPipeline for the given key. Creates it lazily on first request.
    VkPipeline GetOrCreate(VkContext& ctx, const VkPipelineKey& key);

    // Diagnostic counters.
    uint32_t hits()    const { return hits_;    }
    uint32_t misses()  const { return misses_;  }
    size_t   size()    const { return map_.size(); }

private:
    VkPipeline createPipeline(VkContext& ctx, const VkPipelineKey& key);

    PipelineFactoryDeps deps_{};
    ::VkPipelineCache   nativeCache_  = VK_NULL_HANDLE;  // Vulkan handle (global ns) — driver-side dedup
    std::unordered_map<VkPipelineKey, VkPipeline, VkPipelineKeyHash> map_;

    uint32_t hits_   = 0;
    uint32_t misses_ = 0;
};

}  // namespace th06::vk
