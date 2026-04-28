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
#include <vector>

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

    // --- Phase 5b.2: ImGui (imgui_impl_vulkan) integration ---
    // Lifecycle is symmetric with the GL backend:
    //   InitImGui     after RendererVulkan::Init() returns successfully.
    //   ShutdownImGui before destroyAll(); idempotent.
    //   NewFrameImGui at the start of every CPU frame (before ImGui::NewFrame).
    //   RenderImGui   inside the render pass scope of EndFrame; called from
    //                 thprac_games::GameGuiRender via the IsUsingVulkan() branch.
    bool InitImGui(SDL_Window* window);
    void ShutdownImGui();
    void NewFrameImGui();
    void RenderImGui();
    bool ImGuiReady() const { return imguiInitialized_; }

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

    // --- Sprite batcher (Phase 6) ---
    // Coalesces consecutive draws that share PSO key + texture + push-constant
    // state by converting their vertex streams to TRIANGLE_LIST and concatenating
    // them. Reduces vkCmdDraw count from O(sprites) to O(state-changes/frame).
    //
    // Vertex bandwidth amplification: 4-vert quads (the dominant case) expand
    // 1.5x (4 → 6 list verts); strips of length N expand by (3*(N-2))/N.
    //
    // CALL FlushBatch() before any non-draw GPU command (Clear, vkCmdSetViewport,
    // EndFrame end-of-renderpass) and before any state change that would alter
    // PSO/descriptor/push-constant identity. drawCommon does the second class
    // automatically by comparing against the pending batch's snapshot.
    struct PendingBatch {
        bool     active            = false;
        // PSO key fields
        int      vertexLayoutEnum  = 0;
        bool     hasTexture        = false;
        bool     depthTest         = false;
        uint8_t  blendMode         = 0;
        uint8_t  colorOp           = 0;
        uint8_t  zWriteDisable     = 0;
        uint8_t  depthFuncAlways   = 0;
        // Descriptor binding inputs
        uint32_t texture           = 0;
        bool     textureStageDiffuseOnly = false;
        // Push-constant inputs (must be identical across the batch)
        uint32_t textureFactor     = 0;
        int      fogEnabled        = 0;
        uint32_t fogColor          = 0;
        float    fogStart          = 0.0f;
        float    fogEnd            = 0.0f;
        float    mvp[16]           {};
        // 2D viewport (only meaningful for xyzrwh layouts)
        int      viewportX = 0, viewportY = 0, viewportW = 0, viewportH = 0;
        float    viewportMinZ = 0.0f, viewportMaxZ = 0.0f;
        int      screenWidth = 0, screenHeight = 0;
        // Vertex payload as TRIANGLE_LIST in the input layout's stride
        std::vector<uint8_t> verts;
        uint32_t vertexStride      = 0;
        int      vertCount         = 0;
        // Stats: how many input draws got merged into this batch.
        int      mergedDraws       = 0;
    };
    PendingBatch pendingBatch_{};
    // Per-frame stats (reset at BeginFrame); read by an optional DIAG print.
    uint32_t statsInputDraws_ = 0;
    uint32_t statsBatchFlush_ = 0;
    uint32_t statsVertsOut_   = 0;
    // ----- flush-reason histogram (Phase 6.1 diagnostic) -----
    // Counts WHY each batch ended. Indexed by the field whose mismatch
    // triggered the flush, plus catch-all buckets for non-draw-path flushes
    // (Clear / SetViewport / EndFrame). Values reset every BeginFrame and
    // dumped on the same cadence as the [Vk Batch] line.
    enum FlushReason : uint32_t {
        FR_None = 0,            // (placeholder, never counted)
        FR_FirstDraw,           // pending batch was empty (frame start / post-flush)
        FR_VertexLayout,
        FR_HasTexture,
        FR_DepthTest,
        FR_BlendMode,
        FR_ColorOp,
        FR_ZWriteDisable,
        FR_DepthFuncAlways,
        FR_Texture,             // texture binding changed (likely top culprit)
        FR_TextureStageDiffuseOnly,
        FR_TextureFactor,
        FR_FogEnabled,
        FR_FogParams,
        FR_Mvp,
        FR_Viewport,
        FR_ScreenSize,
        FR_NonDraw_Clear,
        FR_NonDraw_SetViewport,
        FR_NonDraw_EndFrame,
        FR_COUNT
    };
    uint32_t flushReasonHist_[FR_COUNT] = {};
    // Per-layout flush reason histogram, indexed [layout 0..7][FR_COUNT].
    // The layout dimension is the *outgoing* (just-flushed) batch's layout,
    // so this attributes flushes to the work that was actually completed,
    // matching how perf cost lands. Layout slot 7 reserved for future use.
    static constexpr int kLayoutSlots_ = 8;
    uint32_t flushReasonByLayout_[kLayoutSlots_][FR_COUNT] = {};
    // Bump both the global and per-layout reason histograms in lockstep,
    // attributing each flush to the OUTGOING (just-completing) batch's
    // vertex layout. Caller must invoke this BEFORE FlushBatch() resets
    // pendingBatch_.vertexLayoutEnum / .active.
    inline void bumpFlushReason(uint32_t reason) {
        if (reason >= FR_COUNT) return;
        ++flushReasonHist_[reason];
        const int li = pendingBatch_.vertexLayoutEnum;
        if (li >= 0 && li < kLayoutSlots_) ++flushReasonByLayout_[li][reason];
    }
    // Most-recent mismatch reason from batchStateMatches (read by drawCommon
    // immediately after the call to bump the histogram on actual flush).
    mutable uint32_t lastMismatchReason_ = FR_None;

    // Append `verts` (in input topology) to the pending batch as a triangle
    // list, expanding each input primitive in place. Caller has already
    // verified state matches the pending batch (or it is empty).
    void appendAsTriangleList(int topologyEnum,
                              const void* verts,
                              int count,
                              uint32_t vertexStride);
    // Phase 6.2: layout-3 (VertexTex1DiffuseXyz) → layout-5 (Tex1DiffuseXyzClip)
    // pretransform path. Multiplies each input vertex's pos through mvpMat_ on
    // the CPU and stores the resulting clip-space vec4 alongside untouched
    // diffuse/uv. Eliminates per-sprite mvp PushConstants delta — the dominant
    // flush trigger for 3D-layout sprites.
    void appendAsTriangleListPretransform3(int topologyEnum,
                                           const void* verts,
                                           int count);
    // Phase 6.3: layout 4 (RenderVertexInfoXyz, vec3 pos + vec2 uv, NO
    // per-vertex color) → layout 5 (Tex1DiffuseXyzClip) helper. In addition
    // to pre-multiplying mvp, this helper folds the current `textureFactor`
    // state into the per-vertex color slot. Layout 4's GL/old-Vulkan shader
    // only output `v_color = pc.textureFactor`, so writing that color per
    // vertex is functionally identical — and it lets adjacent draws with
    // differing textureFactor share a single batch (eliminating the second-
    // largest flush reason in Phase 6.2 telemetry: ~16% of all flushes).
    void appendAsTriangleListPretransform4(int topologyEnum,
                                           const void* verts,
                                           int count);
    // Returns true when the supplied draw description matches the in-flight
    // pending batch (so vertices may be appended without flushing).
    bool batchStateMatches(int vertexLayoutEnum,
                           bool hasTexture,
                           bool depthTest) const;
    // Snapshot current renderer state into `pendingBatch_` (resets the vertex
    // buffer to empty, mergedDraws to 0).
    void beginPendingBatch(int vertexLayoutEnum,
                           bool hasTexture,
                           bool depthTest,
                           uint32_t vertexStride);
public:
    // Flush any pending batched sprites to the command buffer. Safe to call
    // when no batch is pending (becomes a no-op). Must be called before any
    // command that mutates pipeline/descriptor/scissor/viewport state outside
    // of drawCommon's scope (Clear, SetViewport, EndFrame, ImGui pass).
    void FlushBatch();
private:

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

    // Phase 5b.2: dedicated ImGui descriptor pool + init flag (separate from descriptorPool_
    // so ImGui's combined-image-sampler set lifecycle is independent of texture descriptors).
    VkDescriptorPool      imguiDescPool_         = VK_NULL_HANDLE;
    bool                  imguiInitialized_      = false;
    bool                  imguiFontsUploaded_    = false;

    // Shader modules (owned). Index by VertexLayout enum value (0..4).
    VkShaderModule vertModules_[6] = {};
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

    // D3D8 viewport depth-range partitioning (commit 682146c): 3D BG writes [0.5,1.0],
    // sprites write [0.0,0.5], LEQUAL guarantees sprites in front. Vulkan SetViewport
    // must propagate these to VkViewport.minDepth/maxDepth.
    float    viewportMinZ_      = 0.0f;
    float    viewportMaxZ_      = 1.0f;

    // D3D8 texture-stage colorOp = SELECTARG2(diffuse): renderer-side override that
    // forces sampling defaultTex_ (1x1 white) without clobbering currentTexture.
    // Critical: AnmManager caches its own currentTexture and only re-binds on
    // *different* sourceFileIndex; if we zeroed renderer's currentTexture here,
    // subsequent ANM draws of the same sprite would silently render as solid
    // colored blocks (sampling white * diffuse). See bug 2026-04-18.
    bool     textureStageDiffuseOnly_ = false;
};

}  // namespace th06
