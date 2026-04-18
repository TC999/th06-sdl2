// SPDX-License-Identifier: MIT
// Phase 3 — IRenderer over Vulkan with full draw-path support + real texture manager.
//
// Phase-3 scope (per PLAN):
//   - 7 Draw* methods (color & textured, 2D & 3D, strip & fan)
//   - VMA-backed VkTextureManager: CreateFromMemory / CreateEmpty / Delete / UpdateSubImage
//   - 1x1 white default texture is now ONLY the fallback when SetTexture(0)/unknown id
//   - L1 (IRenderer state) -> L2 (VkPipelineKey) -> VkPipelineCache
//   - per-frame vertex upload heap (VMA host-coherent ring), one render pass + depth (VMA)
//   - SPV loaded from TH06_VK_SHADER_DIR (set by CMake)
//
// Phase-4 will add: ImGui pass, sRGB, surface ops (LoadSurfaceFromFile, CopySurface*, TakeScreenshot).
#pragma once

#include "IRenderer.hpp"

#include <volk.h>
#include <memory>
#include <cstdint>
#include <cstdio>

namespace th06::vk {
class VkContext;
class VkSwapchain;
class VkFrameContext;
class VkRenderTarget;
class PipelineCache;
class VkUploadHeap;
class VkDefaultTexture;
class VkTextureManager;
}

namespace th06 {

class RendererVulkan final : public IRenderer
{
public:
    RendererVulkan();
    ~RendererVulkan() override;

    // --- Lifecycle ---
    void Init(SDL_Window *win, SDL_GLContext ctx, i32 w, i32 h) override;
    void InitDevice(u32 opts) override;
    void Release() override;
    void ResizeTarget() override;
    void BeginScene() override;
    void EndScene() override;
    void BeginFrame() override;
    void EndFrame() override;
    void Present() override;

    // --- Clear / Viewport ---
    void Clear(D3DCOLOR color, i32 clearColor, i32 clearDepth) override;
    void SetViewport(i32 x, i32 y, i32 w, i32 h, f32 minZ = 0.0f, f32 maxZ = 1.0f) override;

    // --- Render State ---
    void SetBlendMode(u8 mode) override;
    void SetColorOp(u8 colorOp) override;
    void SetTexture(u32 tex) override;
    void SetTextureFactor(D3DCOLOR factor) override;
    void SetZWriteDisable(u8 disable) override;
    void SetDepthFunc(i32 alwaysPass) override;
    void SetDestBlendInvSrcAlpha() override;
    void SetDestBlendOne() override;
    void SetTextureStageSelectDiffuse() override;
    void SetTextureStageModulateTexture() override;
    void SetFog(i32 enable, D3DCOLOR color, f32 start, f32 end) override;

    // --- Transforms ---
    void SetViewTransform(const D3DXMATRIX *matrix) override;
    void SetProjectionTransform(const D3DXMATRIX *matrix) override;
    void SetWorldTransform(const D3DXMATRIX *matrix) override;
    void SetTextureTransform(const D3DXMATRIX *matrix) override;

    // --- Draw Primitives ---
    void DrawTriangleStrip(const VertexDiffuseXyzrwh *verts, i32 count) override;
    void DrawTriangleStripTex(const VertexTex1Xyzrwh *verts, i32 count) override;
    void DrawTriangleStripTextured(const VertexTex1DiffuseXyzrwh *verts, i32 count) override;
    void DrawTriangleStripTextured3D(const VertexTex1DiffuseXyz *verts, i32 count) override;
    void DrawTriangleFanTextured(const VertexTex1DiffuseXyzrwh *verts, i32 count) override;
    void DrawTriangleFanTextured3D(const VertexTex1DiffuseXyz *verts, i32 count) override;
    void DrawVertexBuffer3D(const RenderVertexInfo *verts, i32 count) override;

    // --- Texture Management (Phase 3) ---
    u32 CreateTextureFromMemory(const u8 *data, i32 dataLen, D3DCOLOR colorKey,
                                i32 *outWidth, i32 *outHeight) override;
    u32 CreateEmptyTexture(i32 width, i32 height) override;
    void DeleteTexture(u32 tex) override;
    void CopyAlphaChannel(u32 dstTex, const u8 *srcData, i32 dataLen, i32 width, i32 height) override;
    void UpdateTextureSubImage(u32 tex, i32 x, i32 y, i32 w, i32 h, const u8 *rgbaPixels) override;

    // --- Surface Operations (Phase 3) ---
    u32 LoadSurfaceFromFile(const u8 *data, i32 dataLen, i32 *outWidth, i32 *outHeight) override;
    u32 LoadSurfaceFromFile(const u8 *data, i32 dataLen, D3DXIMAGE_INFO *info) override;
    void CopySurfaceToScreen(u32 surfaceTex, i32 srcX, i32 srcY, i32 dstX, i32 dstY,
                             i32 w, i32 h, i32 texW, i32 texH) override;
    void CopySurfaceToScreen(u32 surfaceTex, i32 texW, i32 texH, i32 dstX, i32 dstY) override;
    void CopySurfaceRectToScreen(u32 surfaceTex, i32 srcX, i32 srcY, i32 srcW, i32 srcH,
                                 i32 dstX, i32 dstY, i32 texW, i32 texH) override;
    void TakeScreenshot(u32 dstTex, i32 left, i32 top, i32 width, i32 height) override;

    // --- Phase 3 diagnostics (not in IRenderer; called by vk_smoketest --stress=N) ---
    // Stress N = 100: create+update+roundtrip-readback+delete; assert VMA block coalescing
    // (block count grows much less than N) and zero leaks (allocCount returns to baseline).
    // Returns 0 on success; positive count of failures otherwise. log==nullptr → no output.
    int Phase3StressTest(int n, std::FILE* log);

private:
    void destroyAll();
    bool initShaderModules();
    bool initLayoutsAndPool();
    void recomputeMvp();
    bool drawCommon(int vertexLayoutEnum,
                    int topologyEnum,
                    bool hasTexture,
                    bool depthTest,
                    const void* verts,
                    int count,
                    uint32_t vertexStride);

    std::unique_ptr<vk::VkContext>         ctx_;
    std::unique_ptr<vk::VkSwapchain>       swap_;
    std::unique_ptr<vk::VkFrameContext>    frames_;
    std::unique_ptr<vk::VkRenderTarget>    renderTarget_;
    std::unique_ptr<vk::PipelineCache>     pipelineCache_;
    std::unique_ptr<vk::VkUploadHeap>      uploadHeap_;
    std::unique_ptr<vk::VkDefaultTexture>  defaultTex_;
    std::unique_ptr<vk::VkTextureManager>  textureMgr_;

    // Pipeline layouts (owned)
    VkDescriptorSetLayout descLayoutTex_         = VK_NULL_HANDLE;  // set 0 binding 0 = sampler2D
    VkPipelineLayout      pipelineLayoutNoTex_   = VK_NULL_HANDLE;
    VkPipelineLayout      pipelineLayoutTex_     = VK_NULL_HANDLE;
    VkDescriptorPool      descriptorPool_        = VK_NULL_HANDLE;

    // Shader modules (owned). Index by VertexLayout enum value (0..4).
    VkShaderModule vertModules_[5] = {};
    VkShaderModule fragColorMod_   = VK_NULL_HANDLE;
    VkShaderModule fragTexMod_     = VK_NULL_HANDLE;

    // State
    bool     initialized_       = false;
    bool     swapchainOutOfDate_= false;
    uint32_t currentSwapImage_  = 0;
    bool     frameStarted_      = false;
    uint64_t frameCounter_      = 0;
    bool     inRenderPass_      = false;

    // Pending clear color (applied as render-pass LOAD_OP_CLEAR value at next BeginFrame).
    float    clearColor_[4]     = { 0.f, 0.f, 0.f, 1.f };

    // 3D transforms (D3DXMATRIX is row-major in TH06; product cached when dirty).
    float    viewMat_[16]       { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
    float    projMat_[16]       { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
    float    worldMat_[16]      { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
    float    mvpMat_[16]        { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
    bool     mvpDirty_          = true;

    // Cached extra state
    uint8_t  depthFuncAlways_   = 0;   // 0 = LEQUAL, 1 = ALWAYS
};

}  // namespace th06
