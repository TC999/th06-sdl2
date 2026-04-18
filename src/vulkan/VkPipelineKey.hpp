// SPDX-License-Identifier: MIT
// L2 Vulkan Pipeline Cache Key — see DECISIONS.md ADR-007 for the strict three-layer model.
//
// Field ordering / hash composition / mutation rules:
//
// === Block A: fields derived from IRenderer state machine ===
// (One-to-one mapping with IRenderer; no "simplification merging" allowed.)
//   1. vertexLayout      (5 enum values; selects vertex shader + vertex input)
//   2. topology          (TRIANGLE_STRIP | TRIANGLE_FAN)
//   3. blendMode         (ALPHA | ADD; controls VkPipelineColorBlendStateCreateInfo)
//   4. colorOp           (MODULATE | ADD; controls fragment combiner — bake as spec const,
//                         so it lives BOTH in the key AND in the spec const value array)
//   5. depthTestEnable   (false for 2D xyzrwh paths; true for 3D xyz paths)
//   6. depthWriteEnable  (= !IRenderer::currentZWriteDisable)
//   7. depthFunc         (LEQUAL | ALWAYS — only meaningful when depthTestEnable=true)
//   8. hasTexture        (selects textured vs color fragment shader + descriptor set layout)
//
// === Block B: Vulkan-side derived fields ===
//   These are computed from Block A, so they're NOT stored in the key struct, but the cache
//   layer is still aware that they participate in pipeline identity (documented here for ADR-007
//   compliance):
//   - vertex shader module handle  = function of vertexLayout
//   - fragment shader module handle = function of hasTexture
//   - spec const values array       = function of colorOp
//   - render pass compat            = single global render pass (Phase 2; one color + one depth)
//   - descriptor set layout         = function of hasTexture
//
// MUTATION RULE: Adding/removing/reordering any Block A field requires editing ADR-007 first.
#pragma once

#include <cstdint>
#include <functional>

namespace th06::vk {

enum class VertexLayout : uint8_t {
    DiffuseXyzrwh        = 0,  // VertexDiffuseXyzrwh:        vec4 pos + B8G8R8A8 color
    Tex1Xyzrwh           = 1,  // VertexTex1Xyzrwh:           vec4 pos + vec2 uv
    Tex1DiffuseXyzrwh    = 2,  // VertexTex1DiffuseXyzrwh:    vec4 pos + color + vec2 uv
    Tex1DiffuseXyz       = 3,  // VertexTex1DiffuseXyz:       vec3 pos + color + vec2 uv
    RenderVertexInfoXyz  = 4,  // RenderVertexInfo:           vec3 pos + vec2 uv

    Count
};

enum class Topology : uint8_t {
    TriangleStrip = 0,
    TriangleFan   = 1,

    Count
};

enum class BlendMode : uint8_t {
    Alpha = 0,  // SRC_ALPHA · INV_SRC_ALPHA  (BLEND_MODE_ALPHA / INV_SRC_ALPHA)
    Add   = 1,  // SRC_ALPHA · ONE            (BLEND_MODE_ADD / ADDITIVE)

    Count
};

enum class ColorOp : uint8_t {
    Modulate = 0,  // tex * vertColor   (D3DTOP_MODULATE / GL_MODULATE)
    Add      = 1,  // tex + vertColor   (D3DTOP_ADD       / GL_ADD)

    Count
};

enum class DepthFunc : uint8_t {
    LessEqual = 0,  // GL_LEQUAL — D3D8 default
    Always    = 1,  // GL_ALWAYS — when SetDepthFunc(alwaysPass != 0)

    Count
};

struct VkPipelineKey {
    VertexLayout vertexLayout      = VertexLayout::DiffuseXyzrwh;
    Topology     topology          = Topology::TriangleStrip;
    BlendMode    blendMode         = BlendMode::Alpha;
    ColorOp      colorOp           = ColorOp::Modulate;
    DepthFunc    depthFunc         = DepthFunc::LessEqual;
    uint8_t      hasTexture        = 0;
    uint8_t      depthTestEnable   = 0;
    uint8_t      depthWriteEnable  = 1;
    // (no padding: 8 uint8 = 8 bytes, naturally packed)

    bool operator==(const VkPipelineKey& o) const noexcept {
        return vertexLayout     == o.vertexLayout
            && topology         == o.topology
            && blendMode        == o.blendMode
            && colorOp          == o.colorOp
            && depthFunc        == o.depthFunc
            && hasTexture       == o.hasTexture
            && depthTestEnable  == o.depthTestEnable
            && depthWriteEnable == o.depthWriteEnable;
    }
    bool operator!=(const VkPipelineKey& o) const noexcept { return !(*this == o); }
};
static_assert(sizeof(VkPipelineKey) == 8, "VkPipelineKey must pack into 8 bytes for fast hash");

struct VkPipelineKeyHash {
    std::size_t operator()(const VkPipelineKey& k) const noexcept {
        uint64_t v = 0;
        v |= uint64_t(uint8_t(k.vertexLayout))    << 0;
        v |= uint64_t(uint8_t(k.topology))        << 8;
        v |= uint64_t(uint8_t(k.blendMode))       << 16;
        v |= uint64_t(uint8_t(k.colorOp))         << 24;
        v |= uint64_t(uint8_t(k.depthFunc))       << 32;
        v |= uint64_t(uint8_t(k.hasTexture))      << 40;
        v |= uint64_t(uint8_t(k.depthTestEnable)) << 48;
        v |= uint64_t(uint8_t(k.depthWriteEnable))<< 56;
        return std::hash<uint64_t>{}(v);
    }
};

}  // namespace th06::vk
