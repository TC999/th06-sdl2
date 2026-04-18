// SPDX-License-Identifier: MIT
// Phase 3 — see VkTextureManager.hpp.
#include "VkTextureManager.hpp"

#include "VkContext.hpp"

#include <cstdio>
#include <cstring>

namespace th06::vk {

VkTextureManager::~VkTextureManager() {}

bool VkTextureManager::Init(VkContext& ctx, VkDescriptorSetLayout texLayout) {
    layout_ = texLayout;

    // Two shared samplers — match desktop RendererGL (sdl2_renderer.cpp:103-115):
    //   ApplySamplerFor2D() = NEAREST/NEAREST/REPEAT (sprites, HUD, anm)
    //   ApplySamplerFor3D() = LINEAR /LINEAR /REPEAT (stage backgrounds, 3D meshes)
    // GL Init defaults wrap to REPEAT (line 167-168), so REPEAT is mandatory: any
    // texture sampled with UV outside [0,1] (scrolling skybox, fog tile) breaks
    // under CLAMP_TO_EDGE. drawCommon picks per-draw based on vertexLayout.
    VkSamplerCreateInfo sciN = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    sciN.magFilter    = VK_FILTER_NEAREST;
    sciN.minFilter    = VK_FILTER_NEAREST;
    sciN.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sciN.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sciN.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sciN.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sciN.maxLod       = 0.0f;
    TH_VK_CHECK(vkCreateSampler(ctx.device(), &sciN, nullptr, &samplerNearest_));

    VkSamplerCreateInfo sciL = sciN;
    sciL.magFilter    = VK_FILTER_LINEAR;
    sciL.minFilter    = VK_FILTER_LINEAR;
    sciL.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    TH_VK_CHECK(vkCreateSampler(ctx.device(), &sciL, nullptr, &samplerLinear_));

    // Transient command pool for staging uploads.
    VkCommandPoolCreateInfo cpci = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    cpci.queueFamilyIndex = ctx.graphicsQueueFamily();
    cpci.flags            = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT
                          | VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    TH_VK_CHECK(vkCreateCommandPool(ctx.device(), &cpci, nullptr, &cmdPool_));

    // First descriptor pool.
    return growPool(ctx);
}

void VkTextureManager::Shutdown(VkContext& ctx) {
    VkDevice     dev   = ctx.device();
    VmaAllocator alloc = ctx.allocator();
    if (dev == VK_NULL_HANDLE) return;

    for (auto& kv : entries_) {
        if (kv.second.view)  vkDestroyImageView(dev, kv.second.view, nullptr);
        if (kv.second.image && kv.second.alloc && alloc)
            vmaDestroyImage(alloc, kv.second.image, kv.second.alloc);
    }
    entries_.clear();

    for (auto p : pools_) if (p) vkDestroyDescriptorPool(dev, p, nullptr);
    pools_.clear();
    poolUsed_ = 0;

    if (cmdPool_) vkDestroyCommandPool(dev, cmdPool_, nullptr);
    if (samplerNearest_) vkDestroySampler(dev, samplerNearest_, nullptr);
    if (samplerLinear_)  vkDestroySampler(dev, samplerLinear_,  nullptr);
    cmdPool_        = VK_NULL_HANDLE;
    samplerNearest_ = VK_NULL_HANDLE;
    samplerLinear_  = VK_NULL_HANDLE;
    layout_         = VK_NULL_HANDLE;
}

bool VkTextureManager::growPool(VkContext& ctx) {
    VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kSetsPerPool };
    VkDescriptorPoolCreateInfo dpci = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    dpci.maxSets       = kSetsPerPool;
    dpci.poolSizeCount = 1;
    dpci.pPoolSizes    = &ps;

    VkDescriptorPool p = VK_NULL_HANDLE;
    VkResult r = vkCreateDescriptorPool(ctx.device(), &dpci, nullptr, &p);
    if (r != VK_SUCCESS) {
        std::fprintf(stderr, "[VkTextureManager] vkCreateDescriptorPool -> %d\n", int(r));
        return false;
    }
    pools_.push_back(p);
    poolUsed_ = 0;
    return true;
}

bool VkTextureManager::allocDescriptorSet(VkContext& ctx, VkDescriptorSet* outSet) {
    if (pools_.empty() || poolUsed_ >= kSetsPerPool) {
        if (!growPool(ctx)) return false;
    }
    VkDescriptorSetAllocateInfo dsai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    dsai.descriptorPool     = pools_.back();
    dsai.descriptorSetCount = 1;
    dsai.pSetLayouts        = &layout_;
    VkResult r = vkAllocateDescriptorSets(ctx.device(), &dsai, outSet);
    if (r == VK_ERROR_OUT_OF_POOL_MEMORY || r == VK_ERROR_FRAGMENTED_POOL) {
        if (!growPool(ctx)) return false;
        dsai.descriptorPool = pools_.back();
        r = vkAllocateDescriptorSets(ctx.device(), &dsai, outSet);
    }
    if (r != VK_SUCCESS) {
        std::fprintf(stderr, "[VkTextureManager] allocDescriptorSet -> %d\n", int(r));
        return false;
    }
    ++poolUsed_;
    return true;
}

bool VkTextureManager::createImageAndView(VkContext& ctx, int w, int h, Entry& out) {
    VkImageCreateInfo ici = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ici.imageType   = VK_IMAGE_TYPE_2D;
    ici.format      = VK_FORMAT_R8G8B8A8_UNORM;
    ici.extent      = { uint32_t(w), uint32_t(h), 1 };
    ici.mipLevels   = 1;
    ici.arrayLayers = 1;
    ici.samples     = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling      = VK_IMAGE_TILING_OPTIMAL;
    ici.usage       = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VmaAllocationCreateInfo aci{};
    aci.usage         = VMA_MEMORY_USAGE_AUTO;
    aci.requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;

    VkResult r = vmaCreateImage(ctx.allocator(), &ici, &aci, &out.image, &out.alloc, nullptr);
    if (r != VK_SUCCESS) {
        std::fprintf(stderr, "[VkTextureManager] vmaCreateImage(%dx%d) -> %d\n", w, h, int(r));
        return false;
    }

    VkImageViewCreateInfo ivci = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    ivci.image    = out.image;
    ivci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    ivci.format   = VK_FORMAT_R8G8B8A8_UNORM;
    ivci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    ivci.subresourceRange.levelCount = 1;
    ivci.subresourceRange.layerCount = 1;
    TH_VK_CHECK(vkCreateImageView(ctx.device(), &ivci, nullptr, &out.view));

    out.width  = w;
    out.height = h;
    return true;
}

void VkTextureManager::writeDescriptor(VkContext& ctx, Entry& e) {
    // Write both descriptor sets — one for NEAREST sampler, one for LINEAR.
    VkDescriptorImageInfo diiN = {};
    diiN.sampler     = samplerNearest_;
    diiN.imageView   = e.view;
    diiN.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkDescriptorImageInfo diiL = {};
    diiL.sampler     = samplerLinear_;
    diiL.imageView   = e.view;
    diiL.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkWriteDescriptorSet w[2] = {};
    w[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    w[0].dstSet          = e.descSetNearest;
    w[0].dstBinding      = 0;
    w[0].descriptorCount = 1;
    w[0].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w[0].pImageInfo      = &diiN;
    w[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    w[1].dstSet          = e.descSetLinear;
    w[1].dstBinding      = 0;
    w[1].descriptorCount = 1;
    w[1].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w[1].pImageInfo      = &diiL;
    vkUpdateDescriptorSets(ctx.device(), 2, w, 0, nullptr);
}

bool VkTextureManager::uploadRect(VkContext& ctx, Entry& e,
                                  int x, int y, int w, int h, const uint8_t* rgba) {
    // For x==0, y==0 and full-image w==e.width, h==e.height: also handles transition from
    // UNDEFINED → SHADER_READ_ONLY_OPTIMAL on first upload.
    const VkDeviceSize byteCount = VkDeviceSize(w) * VkDeviceSize(h) * 4u;

    // Transient staging buffer (HOST_VISIBLE | mapped).
    VkBuffer        stagingBuf = VK_NULL_HANDLE;
    VmaAllocation   stagingAlloc = VK_NULL_HANDLE;
    {
        VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        bci.size        = byteCount;
        bci.usage       = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        VmaAllocationCreateInfo aci{};
        aci.usage = VMA_MEMORY_USAGE_AUTO;
        aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT
                  | VMA_ALLOCATION_CREATE_MAPPED_BIT;

        VmaAllocationInfo info{};
        VkResult r = vmaCreateBuffer(ctx.allocator(), &bci, &aci,
                                     &stagingBuf, &stagingAlloc, &info);
        if (r != VK_SUCCESS) {
            std::fprintf(stderr, "[VkTextureManager] staging vmaCreateBuffer -> %d\n", int(r));
            return false;
        }
        if (rgba) {
            std::memcpy(info.pMappedData, rgba, byteCount);
            {
                static int s = 0;
                if (s < 8 && w > 100 && h > 100) {
                    const uint8_t* p = (const uint8_t*)info.pMappedData;
                    std::fprintf(stderr, "[VkTex upload] w=%d h=%d bytes=%llu mapped[0..16]=%02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x\n",
                        w, h, (unsigned long long)byteCount,
                        p[0],p[1],p[2],p[3], p[4],p[5],p[6],p[7],
                        p[8],p[9],p[10],p[11], p[12],p[13],p[14],p[15]);
                    ++s;
                }
            }
        } else {
            std::memset(info.pMappedData, 0, byteCount);
        }
        vmaFlushAllocation(ctx.allocator(), stagingAlloc, 0, VK_WHOLE_SIZE);
    }

    VkCommandBuffer cb = VK_NULL_HANDLE;
    {
        VkCommandBufferAllocateInfo cbai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
        cbai.commandPool        = cmdPool_;
        cbai.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cbai.commandBufferCount = 1;
        TH_VK_CHECK(vkAllocateCommandBuffers(ctx.device(), &cbai, &cb));
    }
    VkCommandBufferBeginInfo bbi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    bbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    TH_VK_CHECK(vkBeginCommandBuffer(cb, &bbi));

    // Transition for transfer.
    VkImageMemoryBarrier toDst = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    toDst.oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED;  // safe; ignores existing content
    toDst.newLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toDst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toDst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toDst.image               = e.image;
    toDst.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    toDst.subresourceRange.levelCount = 1;
    toDst.subresourceRange.layerCount = 1;
    toDst.srcAccessMask = 0;
    toDst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &toDst);

    VkBufferImageCopy region = {};
    region.bufferOffset      = 0;
    region.bufferRowLength   = 0;  // tightly packed
    region.bufferImageHeight = 0;
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageOffset = { x, y, 0 };
    region.imageExtent = { uint32_t(w), uint32_t(h), 1 };
    vkCmdCopyBufferToImage(cb, stagingBuf, e.image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    VkImageMemoryBarrier toShader = toDst;
    toShader.oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toShader.newLayout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    toShader.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    toShader.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &toShader);

    TH_VK_CHECK(vkEndCommandBuffer(cb));

    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.commandBufferCount = 1;
    si.pCommandBuffers    = &cb;
    TH_VK_CHECK(vkQueueSubmit(ctx.graphicsQueue(), 1, &si, VK_NULL_HANDLE));
    TH_VK_CHECK(vkQueueWaitIdle(ctx.graphicsQueue()));

    vkFreeCommandBuffers(ctx.device(), cmdPool_, 1, &cb);
    vmaDestroyBuffer(ctx.allocator(), stagingBuf, stagingAlloc);
    return true;
}

uint32_t VkTextureManager::CreateFromRgba(VkContext& ctx, int w, int h, const uint8_t* rgba) {
    if (w <= 0 || h <= 0) return 0;
    Entry e;
    if (!createImageAndView(ctx, w, h, e)) return 0;
    if (!allocDescriptorSet(ctx, &e.descSetNearest)) {
        vkDestroyImageView(ctx.device(), e.view, nullptr);
        vmaDestroyImage(ctx.allocator(), e.image, e.alloc);
        return 0;
    }
    if (!allocDescriptorSet(ctx, &e.descSetLinear)) {
        vkDestroyImageView(ctx.device(), e.view, nullptr);
        vmaDestroyImage(ctx.allocator(), e.image, e.alloc);
        return 0;
    }
    if (!uploadRect(ctx, e, 0, 0, w, h, rgba)) {
        vkDestroyImageView(ctx.device(), e.view, nullptr);
        vmaDestroyImage(ctx.allocator(), e.image, e.alloc);
        return 0;
    }
    writeDescriptor(ctx, e);
    uint32_t id = nextId_++;
    entries_.emplace(id, e);
    return id;
}

uint32_t VkTextureManager::CreateEmpty(VkContext& ctx, int w, int h) {
    return CreateFromRgba(ctx, w, h, nullptr);
}

void VkTextureManager::Delete(VkContext& ctx, uint32_t id) {
    auto it = entries_.find(id);
    if (it == entries_.end()) return;
    vkDeviceWaitIdle(ctx.device());  // simple: ensure no in-flight cmd uses it
    if (it->second.view)  vkDestroyImageView(ctx.device(), it->second.view, nullptr);
    if (it->second.image && it->second.alloc)
        vmaDestroyImage(ctx.allocator(), it->second.image, it->second.alloc);
    // Descriptor set stays allocated in pool until pool destruction (Phase 3 trade-off:
    // we don't reset/free individual sets — texture turnover in TH06 is rare).
    entries_.erase(it);
}

bool VkTextureManager::UpdateSubImage(VkContext& ctx, uint32_t id,
                                      int x, int y, int w, int h, const uint8_t* rgba) {
    auto it = entries_.find(id);
    if (it == entries_.end()) return false;
    if (!rgba || w <= 0 || h <= 0) return false;
    if (x < 0 || y < 0 || x + w > it->second.width || y + h > it->second.height) return false;
    return uploadRect(ctx, it->second, x, y, w, h, rgba);
}

VkDescriptorSet VkTextureManager::GetDescriptorSet(uint32_t id, bool useLinear) const {
    auto it = entries_.find(id);
    if (it == entries_.end()) return VK_NULL_HANDLE;
    return useLinear ? it->second.descSetLinear : it->second.descSetNearest;
}

VkImage VkTextureManager::GetImage(uint32_t id) const {
    auto it = entries_.find(id);
    if (it == entries_.end()) return VK_NULL_HANDLE;
    return it->second.image;
}

bool VkTextureManager::GetSize(uint32_t id, int* outW, int* outH) const {
    auto it = entries_.find(id);
    if (it == entries_.end()) return false;
    if (outW) *outW = it->second.width;
    if (outH) *outH = it->second.height;
    return true;
}

}  // namespace th06::vk
