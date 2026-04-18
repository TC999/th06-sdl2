// SPDX-License-Identifier: MIT
// Phase 2 — L2 pipeline cache impl. See header for design.
#include "VkPipelineCache.hpp"

#include "VkContext.hpp"

#include <array>
#include <cstdio>
#include <cstring>

namespace th06::vk {

namespace {

// ----- Vertex input descriptors per VertexLayout enum --------------------------------------
//
// Layout binding strides (must match RendererGL vertex structs in src/sdl2_renderer.hpp):
//   DiffuseXyzrwh        : 20 B  (vec4 pos + B8G8R8A8 color)        — color via U8N x4
//   Tex1Xyzrwh           : 24 B  (vec4 pos + vec2 uv)
//   Tex1DiffuseXyzrwh    : 28 B  (vec4 pos + B8G8R8A8 color + vec2 uv)
//   Tex1DiffuseXyz       : 24 B  (vec3 pos + B8G8R8A8 color + vec2 uv)
//   RenderVertexInfo     : 20 B  (vec3 pos + vec2 uv)
//
// D3D8/GL D3DCOLOR layout is BGRA in memory (B,G,R,A bytes). Vulkan vertex format
// VK_FORMAT_B8G8R8A8_UNORM matches this byte order and unpacks to (R=B,G=G,B=R,A=A) — but
// since we only re-pack as a single vec4 inside the shader and treat each channel
// symmetrically (color = vertColor.bgra, basically), we use B8G8R8A8_UNORM directly so the
// shader sees (R=R,G=G,B=B,A=A). The shaders themselves therefore use `in_color` as RGBA.

struct VertexFormatDesc {
    uint32_t                              stride;
    std::array<VkVertexInputAttributeDescription, 3> attrs;  // up to 3 (pos, color, uv)
    uint32_t                              attrCount;
};

VertexFormatDesc GetVertexFormat(VertexLayout vl) {
    VertexFormatDesc d{};
    switch (vl) {
    case VertexLayout::DiffuseXyzrwh:
        d.stride = 20;
        d.attrs[0] = { 0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 0  };  // pos
        d.attrs[1] = { 1, 0, VK_FORMAT_B8G8R8A8_UNORM,      16 };  // color
        d.attrCount = 2;
        break;
    case VertexLayout::Tex1Xyzrwh:
        d.stride = 24;
        d.attrs[0] = { 0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 0  };  // pos
        d.attrs[1] = { 1, 0, VK_FORMAT_R32G32_SFLOAT,       16 };  // uv (shader: location=1)
        d.attrCount = 2;
        break;
    case VertexLayout::Tex1DiffuseXyzrwh:
        d.stride = 28;
        d.attrs[0] = { 0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 0  };  // pos
        d.attrs[1] = { 1, 0, VK_FORMAT_B8G8R8A8_UNORM,      16 };  // color
        d.attrs[2] = { 2, 0, VK_FORMAT_R32G32_SFLOAT,       20 };  // uv
        d.attrCount = 3;
        break;
    case VertexLayout::Tex1DiffuseXyz:
        d.stride = 24;
        d.attrs[0] = { 0, 0, VK_FORMAT_R32G32B32_SFLOAT,    0  };  // pos
        d.attrs[1] = { 1, 0, VK_FORMAT_B8G8R8A8_UNORM,      12 };  // color
        d.attrs[2] = { 2, 0, VK_FORMAT_R32G32_SFLOAT,       16 };  // uv
        d.attrCount = 3;
        break;
    case VertexLayout::RenderVertexInfoXyz:
        d.stride = 20;
        d.attrs[0] = { 0, 0, VK_FORMAT_R32G32B32_SFLOAT,    0  };  // pos
        d.attrs[1] = { 1, 0, VK_FORMAT_R32G32_SFLOAT,       12 };  // uv (shader: location=1)
        d.attrCount = 2;
        break;
    default:
        break;
    }
    return d;
}

VkBlendFactor SrcFactor(BlendMode m) {
    return VK_BLEND_FACTOR_SRC_ALPHA;  // both ALPHA & ADD use SRC_ALPHA per RendererGL
}
VkBlendFactor DstFactor(BlendMode m) {
    return (m == BlendMode::Alpha) ? VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA
                                   : VK_BLEND_FACTOR_ONE;
}

VkCompareOp DepthOp(DepthFunc f) {
    return (f == DepthFunc::LessEqual) ? VK_COMPARE_OP_LESS_OR_EQUAL : VK_COMPARE_OP_ALWAYS;
}

VkPrimitiveTopology Topo(Topology t) {
    return (t == Topology::TriangleStrip) ? VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP
                                          : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;
}

}  // namespace

PipelineCache::~PipelineCache() {
    // Caller must Shutdown() with live ctx before drop.
}

bool PipelineCache::Init(VkContext& ctx, const PipelineFactoryDeps& deps) {
    deps_ = deps;
    VkPipelineCacheCreateInfo pci = { VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO };
    // Phase 2: empty cache. Phase 4 may load from disk.
    TH_VK_CHECK(vkCreatePipelineCache(ctx.device(), &pci, nullptr, &nativeCache_));
    return true;
}

void PipelineCache::Shutdown(VkContext& ctx) {
    VkDevice dev = ctx.device();
    if (dev == VK_NULL_HANDLE) return;
    for (auto& kv : map_) {
        if (kv.second) vkDestroyPipeline(dev, kv.second, nullptr);
    }
    map_.clear();
    if (nativeCache_) {
        vkDestroyPipelineCache(dev, nativeCache_, nullptr);
        nativeCache_ = VK_NULL_HANDLE;
    }
}

VkPipeline PipelineCache::GetOrCreate(VkContext& ctx, const VkPipelineKey& key) {
    auto it = map_.find(key);
    if (it != map_.end()) {
        ++hits_;
        return it->second;
    }
    ++misses_;
    VkPipeline p = createPipeline(ctx, key);
    map_.emplace(key, p);
    return p;
}

VkPipeline PipelineCache::createPipeline(VkContext& ctx, const VkPipelineKey& key) {
    // ---- Vertex input ----
    VertexFormatDesc vfd = GetVertexFormat(key.vertexLayout);

    VkVertexInputBindingDescription binding = {};
    binding.binding   = 0;
    binding.stride    = vfd.stride;
    binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    VkPipelineVertexInputStateCreateInfo vi = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    vi.vertexBindingDescriptionCount   = 1;
    vi.pVertexBindingDescriptions      = &binding;
    vi.vertexAttributeDescriptionCount = vfd.attrCount;
    vi.pVertexAttributeDescriptions    = vfd.attrs.data();

    // ---- Input assembly ----
    VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    ia.topology = Topo(key.topology);

    // ---- Viewport / scissor (dynamic) ----
    VkPipelineViewportStateCreateInfo vp = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    vp.viewportCount = 1;
    vp.scissorCount  = 1;

    // ---- Rasterization ----
    VkPipelineRasterizationStateCreateInfo rs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode    = VK_CULL_MODE_NONE;   // TH06 doesn't cull
    rs.frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth   = 1.0f;

    // ---- Multisample ----
    VkPipelineMultisampleStateCreateInfo ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    // ---- Depth/stencil ----
    VkPipelineDepthStencilStateCreateInfo ds = { VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
    ds.depthTestEnable  = key.depthTestEnable  ? VK_TRUE : VK_FALSE;
    ds.depthWriteEnable = key.depthWriteEnable ? VK_TRUE : VK_FALSE;
    ds.depthCompareOp   = DepthOp(key.depthFunc);

    // ---- Color blend ----
    VkPipelineColorBlendAttachmentState cba = {};
    cba.blendEnable         = VK_TRUE;
    cba.srcColorBlendFactor = SrcFactor(key.blendMode);
    cba.dstColorBlendFactor = DstFactor(key.blendMode);
    cba.colorBlendOp        = VK_BLEND_OP_ADD;
    cba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    cba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    cba.alphaBlendOp        = VK_BLEND_OP_ADD;
    cba.colorWriteMask      = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT
                            | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

    VkPipelineColorBlendStateCreateInfo cb = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    cb.attachmentCount = 1;
    cb.pAttachments    = &cba;

    // ---- Dynamic state ----
    VkDynamicState dynStates[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dyn = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    dyn.dynamicStateCount = 2;
    dyn.pDynamicStates    = dynStates;

    // ---- Shader stages ----
    VkShaderModule vertMod = deps_.vertModules[uint32_t(key.vertexLayout)];
    VkShaderModule fragMod = key.hasTexture ? deps_.fragTextured : deps_.fragColor;

    // Spec const: only fragment textured cares (slot 0 = COLOR_OP).
    int32_t specColorOp = (key.colorOp == ColorOp::Add) ? 1 : 0;
    VkSpecializationMapEntry specEntry = { 0, 0, sizeof(int32_t) };
    VkSpecializationInfo     specInfo  = {};
    specInfo.mapEntryCount = 1;
    specInfo.pMapEntries   = &specEntry;
    specInfo.dataSize      = sizeof(int32_t);
    specInfo.pData         = &specColorOp;

    VkPipelineShaderStageCreateInfo stages[2] = {};
    stages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vertMod;
    stages[0].pName  = "main";

    stages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fragMod;
    stages[1].pName  = "main";
    if (key.hasTexture) {
        stages[1].pSpecializationInfo = &specInfo;
    }

    // ---- Layout ----
    VkPipelineLayout layout = key.hasTexture ? deps_.layoutTextured : deps_.layoutNoTex;

    // ---- Assemble ----
    VkGraphicsPipelineCreateInfo gpci = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    gpci.stageCount          = 2;
    gpci.pStages             = stages;
    gpci.pVertexInputState   = &vi;
    gpci.pInputAssemblyState = &ia;
    gpci.pViewportState      = &vp;
    gpci.pRasterizationState = &rs;
    gpci.pMultisampleState   = &ms;
    gpci.pDepthStencilState  = &ds;
    gpci.pColorBlendState    = &cb;
    gpci.pDynamicState       = &dyn;
    gpci.layout              = layout;
    gpci.renderPass          = deps_.renderPass;
    gpci.subpass             = 0;

    VkPipeline pipe = VK_NULL_HANDLE;
    TH_VK_CHECK(vkCreateGraphicsPipelines(ctx.device(), nativeCache_, 1, &gpci, nullptr, &pipe));
    return pipe;
}

}  // namespace th06::vk
