// SPDX-License-Identifier: MIT
// Phase 2 — see VkResources.hpp for design.
#include "VkResources.hpp"

#include "VkContext.hpp"
#include "VkRenderTarget.hpp"  // FindMemoryTypeIndex

#include <cstdio>
#include <cstring>
#include <fstream>

namespace th06::vk {

// =========================================================================================
//  VkUploadHeap
// =========================================================================================

VkUploadHeap::~VkUploadHeap() {}

bool VkUploadHeap::Init(VkContext& ctx) {
    for (uint32_t i = 0; i < kFramesInFlight; ++i) {
        VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        bci.size        = kBytesPerFrame;
        bci.usage       = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
        bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        TH_VK_CHECK(vkCreateBuffer(ctx.device(), &bci, nullptr, &frames_[i].buffer));

        VkMemoryRequirements req;
        vkGetBufferMemoryRequirements(ctx.device(), frames_[i].buffer, &req);
        uint32_t typeIdx = FindMemoryTypeIndex(ctx.physicalDevice(),
                                               req.memoryTypeBits,
                                               VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
                                             | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (typeIdx == UINT32_MAX) {
            std::fprintf(stderr, "[VkUploadHeap] no HOST_VISIBLE|COHERENT memory\n");
            return false;
        }
        VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        mai.allocationSize  = req.size;
        mai.memoryTypeIndex = typeIdx;
        // PHASE 2 CARVE-OUT: bare vkAllocateMemory — see VkRenderTarget.hpp note.
        TH_VK_CHECK(vkAllocateMemory(ctx.device(), &mai, nullptr, &frames_[i].memory));
        TH_VK_CHECK(vkBindBufferMemory(ctx.device(), frames_[i].buffer, frames_[i].memory, 0));
        TH_VK_CHECK(vkMapMemory(ctx.device(), frames_[i].memory, 0, VK_WHOLE_SIZE, 0,
                                &frames_[i].mapped));
        frames_[i].offset = 0;
    }
    return true;
}

void VkUploadHeap::Shutdown(VkContext& ctx) {
    VkDevice dev = ctx.device();
    if (dev == VK_NULL_HANDLE) return;
    for (uint32_t i = 0; i < kFramesInFlight; ++i) {
        if (frames_[i].mapped) {
            vkUnmapMemory(dev, frames_[i].memory);
            frames_[i].mapped = nullptr;
        }
        if (frames_[i].buffer) vkDestroyBuffer(dev, frames_[i].buffer, nullptr);
        if (frames_[i].memory) vkFreeMemory(dev, frames_[i].memory, nullptr);
        frames_[i] = {};
    }
}

void VkUploadHeap::BeginFrame(uint32_t frameIdx) {
    currentFrame_ = frameIdx % kFramesInFlight;
    frames_[currentFrame_].offset = 0;
}

bool VkUploadHeap::AllocVerts(VkDeviceSize size,
                              void**       outMapped,
                              VkBuffer*    outBuffer,
                              VkDeviceSize* outOffset) {
    PerFrame& f = frames_[currentFrame_];
    // Align to 16 to be safe across all vertex layouts.
    constexpr VkDeviceSize kAlign = 16;
    VkDeviceSize aligned = (f.offset + kAlign - 1) & ~(kAlign - 1);
    if (aligned + size > kBytesPerFrame) {
        std::fprintf(stderr, "[VkUploadHeap] frame %u exhausted (%llu + %llu > %llu)\n",
                     currentFrame_,
                     static_cast<unsigned long long>(aligned),
                     static_cast<unsigned long long>(size),
                     static_cast<unsigned long long>(kBytesPerFrame));
        return false;
    }
    *outMapped = static_cast<char*>(f.mapped) + aligned;
    *outBuffer = f.buffer;
    *outOffset = aligned;
    f.offset = aligned + size;
    return true;
}

// =========================================================================================
//  VkDefaultTexture (1x1 white)
// =========================================================================================

VkDefaultTexture::~VkDefaultTexture() {}

bool VkDefaultTexture::Init(VkContext& ctx,
                            VkDescriptorPool      pool,
                            VkDescriptorSetLayout texLayout) {
    VkDevice dev  = ctx.device();
    VkPhysicalDevice phys = ctx.physicalDevice();

    // Image: 1x1 R8G8B8A8 UNORM, sampled.
    VkImageCreateInfo ici = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ici.imageType   = VK_IMAGE_TYPE_2D;
    ici.format      = VK_FORMAT_R8G8B8A8_UNORM;
    ici.extent      = { 1, 1, 1 };
    ici.mipLevels   = 1;
    ici.arrayLayers = 1;
    ici.samples     = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling      = VK_IMAGE_TILING_LINEAR;     // tiny, can host-write directly
    ici.usage       = VK_IMAGE_USAGE_SAMPLED_BIT;
    ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ici.initialLayout = VK_IMAGE_LAYOUT_PREINITIALIZED;
    TH_VK_CHECK(vkCreateImage(dev, &ici, nullptr, &image_));

    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(dev, image_, &req);
    uint32_t typeIdx = FindMemoryTypeIndex(phys, req.memoryTypeBits,
                                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
                                         | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (typeIdx == UINT32_MAX) return false;

    VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    mai.allocationSize  = req.size;
    mai.memoryTypeIndex = typeIdx;
    // PHASE 2 CARVE-OUT.
    TH_VK_CHECK(vkAllocateMemory(dev, &mai, nullptr, &memory_));
    TH_VK_CHECK(vkBindImageMemory(dev, image_, memory_, 0));

    // Write 0xFFFFFFFF.
    void* mapped = nullptr;
    TH_VK_CHECK(vkMapMemory(dev, memory_, 0, VK_WHOLE_SIZE, 0, &mapped));
    VkSubresourceLayout subLayout;
    VkImageSubresource sub = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0 };
    vkGetImageSubresourceLayout(dev, image_, &sub, &subLayout);
    uint32_t* px = reinterpret_cast<uint32_t*>(static_cast<char*>(mapped) + subLayout.offset);
    *px = 0xFFFFFFFFu;
    vkUnmapMemory(dev, memory_);

    // Transition PREINITIALIZED -> SHADER_READ_ONLY_OPTIMAL via a one-shot cmd buffer.
    VkCommandPool tmpPool = VK_NULL_HANDLE;
    VkCommandPoolCreateInfo cpci = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    cpci.queueFamilyIndex = ctx.graphicsQueueFamily();
    cpci.flags            = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    TH_VK_CHECK(vkCreateCommandPool(dev, &cpci, nullptr, &tmpPool));

    VkCommandBuffer cb = VK_NULL_HANDLE;
    VkCommandBufferAllocateInfo cbai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    cbai.commandPool        = tmpPool;
    cbai.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = 1;
    TH_VK_CHECK(vkAllocateCommandBuffers(dev, &cbai, &cb));

    VkCommandBufferBeginInfo bbi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    bbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    TH_VK_CHECK(vkBeginCommandBuffer(cb, &bbi));

    VkImageMemoryBarrier bar = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    bar.oldLayout           = VK_IMAGE_LAYOUT_PREINITIALIZED;
    bar.newLayout           = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    bar.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bar.image               = image_;
    bar.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    bar.subresourceRange.levelCount = 1;
    bar.subresourceRange.layerCount = 1;
    bar.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
    bar.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

    vkCmdPipelineBarrier(cb,
                         VK_PIPELINE_STAGE_HOST_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &bar);
    TH_VK_CHECK(vkEndCommandBuffer(cb));

    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.commandBufferCount = 1;
    si.pCommandBuffers    = &cb;
    TH_VK_CHECK(vkQueueSubmit(ctx.graphicsQueue(), 1, &si, VK_NULL_HANDLE));
    TH_VK_CHECK(vkQueueWaitIdle(ctx.graphicsQueue()));
    vkDestroyCommandPool(dev, tmpPool, nullptr);

    // View
    VkImageViewCreateInfo ivci = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    ivci.image    = image_;
    ivci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    ivci.format   = VK_FORMAT_R8G8B8A8_UNORM;
    ivci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    ivci.subresourceRange.levelCount = 1;
    ivci.subresourceRange.layerCount = 1;
    TH_VK_CHECK(vkCreateImageView(dev, &ivci, nullptr, &view_));

    // Sampler — nearest, clamp-to-edge (TH06 textures are point-sampled at game level).
    VkSamplerCreateInfo sci = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    sci.magFilter = VK_FILTER_NEAREST;
    sci.minFilter = VK_FILTER_NEAREST;
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.maxLod = 0.0f;
    TH_VK_CHECK(vkCreateSampler(dev, &sci, nullptr, &sampler_));

    // Descriptor set
    VkDescriptorSetAllocateInfo dsai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    dsai.descriptorPool     = pool;
    dsai.descriptorSetCount = 1;
    dsai.pSetLayouts        = &texLayout;
    TH_VK_CHECK(vkAllocateDescriptorSets(dev, &dsai, &descSet_));

    VkDescriptorImageInfo dii = {};
    dii.sampler     = sampler_;
    dii.imageView   = view_;
    dii.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkWriteDescriptorSet w = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
    w.dstSet          = descSet_;
    w.dstBinding      = 0;
    w.descriptorCount = 1;
    w.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w.pImageInfo      = &dii;
    vkUpdateDescriptorSets(dev, 1, &w, 0, nullptr);
    return true;
}

void VkDefaultTexture::Shutdown(VkContext& ctx) {
    VkDevice dev = ctx.device();
    if (dev == VK_NULL_HANDLE) return;
    if (sampler_) vkDestroySampler(dev, sampler_, nullptr);
    if (view_)    vkDestroyImageView(dev, view_, nullptr);
    if (image_)   vkDestroyImage(dev, image_, nullptr);
    if (memory_)  vkFreeMemory(dev, memory_, nullptr);
    sampler_ = VK_NULL_HANDLE;
    view_    = VK_NULL_HANDLE;
    image_   = VK_NULL_HANDLE;
    memory_  = VK_NULL_HANDLE;
    descSet_ = VK_NULL_HANDLE;  // freed with pool
}

// =========================================================================================
//  SPIR-V loader
// =========================================================================================

bool LoadSpvFile(const std::string& path, std::vector<uint32_t>& outWords) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) {
        std::fprintf(stderr, "[LoadSpvFile] cannot open %s\n", path.c_str());
        return false;
    }
    std::streamsize sz = f.tellg();
    if (sz <= 0 || (sz % 4) != 0) {
        std::fprintf(stderr, "[LoadSpvFile] invalid size %lld for %s\n",
                     static_cast<long long>(sz), path.c_str());
        return false;
    }
    outWords.resize(static_cast<size_t>(sz / 4));
    f.seekg(0);
    f.read(reinterpret_cast<char*>(outWords.data()), sz);
    return f.good();
}

bool CreateShaderModule(VkContext& ctx,
                        const std::vector<uint32_t>& words,
                        VkShaderModule* outMod) {
    VkShaderModuleCreateInfo mci = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    mci.codeSize = words.size() * sizeof(uint32_t);
    mci.pCode    = words.data();
    TH_VK_CHECK(vkCreateShaderModule(ctx.device(), &mci, nullptr, outMod));
    return true;
}

}  // namespace th06::vk
