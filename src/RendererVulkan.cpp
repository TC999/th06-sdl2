    // SPDX-License-Identifier: MIT
// Phase 3 — RendererVulkan implementation. See header for design notes.
#include "RendererVulkan.hpp"
#include "sdl2_renderer.hpp"  // VertexDiffuseXyzrwh + friends + RenderVertexInfo (forward-declared in IRenderer.hpp)
#include "AnmManager.hpp"     // RenderVertexInfo full definition

#include "vulkan/VkContext.hpp"
#include "vulkan/VkSwapchain.hpp"
#include "vulkan/VkFrameContext.hpp"
#include "vulkan/VkRenderTarget.hpp"
#include "vulkan/VkPipelineCache.hpp"
#include "vulkan/VkPipelineKey.hpp"
#include "vulkan/VkResources.hpp"
#include "vulkan/VkTextureManager.hpp"

// Phase 5b.2: imgui_impl_vulkan integration. Order matters — volk forces
// VK_NO_PROTOTYPES, so imgui_impl_vulkan.h sees no prototypes and switches to
// its own loader; we feed it volk's vkGetInstanceProcAddr via LoadFunctions.
// Gated by TH06_HAS_IMGUI so vk_smoketest (which doesn't link ImGui) stays minimal.
#ifdef TH06_HAS_IMGUI
#include <imgui.h>
#include <imgui_impl_vulkan.h>
#include <imgui_impl_sdl.h>

#include "thprac_gui_integration.h"
#include "thprac_th06.h"
#ifndef TH06_VK_NO_THPRAC
#include "thprac_games.h"  // g_adv_igi_options.th06_unlock_framerate (vsync override)
#define TH06_VK_UNLOCK_FRAMERATE() (THPrac::g_adv_igi_options.th06_unlock_framerate)
#else
#define TH06_VK_UNLOCK_FRAMERATE() (false)
#endif
#endif

#include <SDL_log.h>
#include <SDL_image.h>
#include <SDL_rwops.h>
#include <SDL_surface.h>
#include <SDL_vulkan.h>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>
#include <cmath>

#ifndef TH06_VK_SHADER_DIR
#define TH06_VK_SHADER_DIR "shaders_vk"
#endif

namespace th06 {

namespace {

inline void ColorToFloat4(D3DCOLOR c, float out[4]) {
    out[0] = ((c >> 16) & 0xFF) / 255.0f;  // R
    out[1] = ((c >> 8)  & 0xFF) / 255.0f;  // G
    out[2] = ((c)       & 0xFF) / 255.0f;  // B
    out[3] = ((c >> 24) & 0xFF) / 255.0f;  // A
}

// Push constant block — must match GLSL `layout(push_constant) uniform PC` in all vertex shaders.
// Total = 8 + 8 + 64 + 16 + 16 + 16 = 128 bytes (within Vulkan-guaranteed 128-byte minimum).
struct PushConstants {
    float invScreen[2];     // 1/screenWidth, 1/screenHeight (used by xyzrwh paths)
    float _pad[2];
    float mvp[16];          // column-major in shader; we feed row-major D3D bytes -> implicit transpose
    float fogColor[4];      // RGBA, all 0..1
    float fogParams[4];     // [0]=start, [1]=end, [2]=enabled (1.0 = on, 0.0 = off), [3]=pad
    float textureFactor[4]; // RGBA fallback vertex color for layouts that lack per-vertex color
                            // (Tex1Xyzrwh / RenderVertexInfoXyz). Mirrors GL ApplyVertexColor(textureFactor)
                            // path in DrawTriangleStripTex / DrawVertexBuffer3D — critical for transparency
                            // (textureFactor.a < 1.0 must propagate to fragment alpha).
};

inline void Mat4Mul_RowMajor(const float* a, const float* b, float* out) {
    // out = a * b, all row-major 4x4.
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            float s = 0.f;
            for (int k = 0; k < 4; ++k) {
                s += a[r * 4 + k] * b[k * 4 + c];
            }
            out[r * 4 + c] = s;
        }
    }
}

}  // namespace

RendererVulkan::RendererVulkan() {
    window               = nullptr;
    glContext            = nullptr;
    screenWidth          = 0;
    screenHeight         = 0;
    viewportX = viewportY = viewportW = viewportH = 0;
    currentTexture       = 0;
    currentBlendMode     = 0;
    currentColorOp       = 0;
    currentVertexShader  = 0;
    currentZWriteDisable = 0;
    textureFactor        = 0xFFFFFFFF;
    fogEnabled           = 0;
    fogColor             = 0;
    fogStart             = 0.f;
    fogEnd               = 0.f;
    realScreenWidth      = 0;
    realScreenHeight     = 0;
}

RendererVulkan::~RendererVulkan() { destroyAll(); }

void RendererVulkan::destroyAll() {
    if (!initialized_) return;
    VkDevice dev = ctx_ ? ctx_->device() : VK_NULL_HANDLE;
    if (dev) vkDeviceWaitIdle(dev);

    if (defaultTex_)    defaultTex_->Shutdown(*ctx_);
    if (textureMgr_)    textureMgr_->Shutdown(*ctx_);
    if (uploadHeap_)    uploadHeap_->Shutdown(*ctx_);
    if (pipelineCache_) pipelineCache_->Shutdown(*ctx_);
    if (renderTarget_)  renderTarget_->Destroy(*ctx_);

    if (dev) {
        for (auto& m : vertModules_) if (m) { vkDestroyShaderModule(dev, m, nullptr); m = VK_NULL_HANDLE; }
        if (fragColorMod_) { vkDestroyShaderModule(dev, fragColorMod_, nullptr); fragColorMod_ = VK_NULL_HANDLE; }
        if (fragTexMod_)   { vkDestroyShaderModule(dev, fragTexMod_,   nullptr); fragTexMod_   = VK_NULL_HANDLE; }
        if (pipelineLayoutNoTex_) { vkDestroyPipelineLayout(dev, pipelineLayoutNoTex_, nullptr); pipelineLayoutNoTex_ = VK_NULL_HANDLE; }
        if (pipelineLayoutTex_)   { vkDestroyPipelineLayout(dev, pipelineLayoutTex_,   nullptr); pipelineLayoutTex_   = VK_NULL_HANDLE; }
        if (descLayoutTex_)       { vkDestroyDescriptorSetLayout(dev, descLayoutTex_, nullptr); descLayoutTex_ = VK_NULL_HANDLE; }
        if (descriptorPool_)      { vkDestroyDescriptorPool(dev, descriptorPool_, nullptr); descriptorPool_ = VK_NULL_HANDLE; }
    }

    if (frames_) frames_->Destroy(*ctx_);
    if (swap_)   swap_->Destroy(*ctx_);

    defaultTex_.reset();
    textureMgr_.reset();
    uploadHeap_.reset();
    pipelineCache_.reset();
    renderTarget_.reset();
    frames_.reset();
    swap_.reset();
    if (ctx_) ctx_->Shutdown();
    ctx_.reset();
    initialized_ = false;
}

bool RendererVulkan::initShaderModules() {
    static const char* kVertNames[6] = {
        "v_diffuse_xyzrwh.vert.spv",
        "v_tex1_xyzrwh.vert.spv",
        "v_tex1_diffuse_xyzrwh.vert.spv",
        "v_tex1_diffuse_xyz.vert.spv",
        "v_render_vertex_info.vert.spv",
        "v_tex1_diffuse_xyz_clip.vert.spv",  // Phase 6.2 sprite-batch pretransformed
    };
    // Phase 5b.3: on Android, the shaders live inside the APK at
    // assets/shaders_vk/, which SDL_RWFromFile reads when given a relative
    // path. The TH06_VK_SHADER_DIR macro (an absolute build-tree path) is
    // only meaningful on desktop builds.
#ifdef __ANDROID__
    const std::string base = "shaders_vk/";
#else
    const std::string base = std::string(TH06_VK_SHADER_DIR) + "/";
#endif
    for (int i = 0; i < 6; ++i) {
        std::vector<uint32_t> words;
        if (!vk::LoadSpvFile(base + kVertNames[i], words)) return false;
        if (!vk::CreateShaderModule(*ctx_, words, &vertModules_[i])) return false;
    }
    {
        std::vector<uint32_t> words;
        if (!vk::LoadSpvFile(base + "f_color.frag.spv", words)) return false;
        if (!vk::CreateShaderModule(*ctx_, words, &fragColorMod_)) return false;
    }
    {
        std::vector<uint32_t> words;
        if (!vk::LoadSpvFile(base + "f_textured.frag.spv", words)) return false;
        if (!vk::CreateShaderModule(*ctx_, words, &fragTexMod_)) return false;
    }
    return true;
}

bool RendererVulkan::initLayoutsAndPool() {
    VkDevice dev = ctx_->device();

    // Descriptor set layout: set=0 binding=0 = combined image sampler (frag stage)
    VkDescriptorSetLayoutBinding bind = {};
    bind.binding         = 0;
    bind.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bind.descriptorCount = 1;
    bind.stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo dslci = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    dslci.bindingCount = 1;
    dslci.pBindings    = &bind;
    TH_VK_CHECK(vkCreateDescriptorSetLayout(dev, &dslci, nullptr, &descLayoutTex_));

    // Push constant range: vertex stage (mvp/invScreen) + fragment stage (fog)
    VkPushConstantRange pcr = {};
    pcr.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    pcr.offset     = 0;
    pcr.size       = sizeof(PushConstants);

    // Sanity-check: PushConstants (currently 112 B = invScreen+pad+mvp+fog) must fit
    // within the device's reported limit. Vulkan guarantees ≥128 B everywhere, but a
    // future PushConstants growth could break on a tightly-bounded driver — fail fast
    // here rather than mysteriously corrupt state at vkCmdPushConstants time.
    {
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(ctx_->physicalDevice(), &props);
        if (sizeof(PushConstants) > props.limits.maxPushConstantsSize) {
            std::fprintf(stderr, "[VK] FATAL: PushConstants=%zu B exceeds device limit %u B\n",
                         sizeof(PushConstants), props.limits.maxPushConstantsSize);
            return false;
        }
    }

    // Pipeline layout (no texture)
    VkPipelineLayoutCreateInfo plci_no = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    plci_no.pushConstantRangeCount = 1;
    plci_no.pPushConstantRanges    = &pcr;
    TH_VK_CHECK(vkCreatePipelineLayout(dev, &plci_no, nullptr, &pipelineLayoutNoTex_));

    // Pipeline layout (textured)
    VkPipelineLayoutCreateInfo plci_tex = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    plci_tex.setLayoutCount         = 1;
    plci_tex.pSetLayouts            = &descLayoutTex_;
    plci_tex.pushConstantRangeCount = 1;
    plci_tex.pPushConstantRanges    = &pcr;
    TH_VK_CHECK(vkCreatePipelineLayout(dev, &plci_tex, nullptr, &pipelineLayoutTex_));

    // Descriptor pool — Phase 2: just enough for the default texture set.
    VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 16 };
    VkDescriptorPoolCreateInfo dpci = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    dpci.maxSets       = 16;
    dpci.poolSizeCount = 1;
    dpci.pPoolSizes    = &ps;
    TH_VK_CHECK(vkCreateDescriptorPool(dev, &dpci, nullptr, &descriptorPool_));
    return true;
}

void RendererVulkan::Init(SDL_Window *win, SDL_GLContext /*ctx_unused*/, i32 w, i32 h) {
    if (initialized_) return;
    window           = win;
    glContext        = nullptr;
    screenWidth      = w;
    screenHeight     = h;
    // realScreenWidth/Height must reflect the actual drawable size, not the
    // logical 640x480, so screen-space overlays (e.g. touch virtual buttons
    // in thprac_games.cpp Vulkan branch) can compute the pillarbox layout.
    int drawW = w, drawH = h;
    SDL_Vulkan_GetDrawableSize(win, &drawW, &drawH);
    realScreenWidth  = drawW;
    realScreenHeight = drawH;
    viewportW        = w;
    viewportH        = h;

    ctx_           = std::make_unique<vk::VkContext>();
    swap_          = std::make_unique<vk::VkSwapchain>();
    frames_        = std::make_unique<vk::VkFrameContext>();
    renderTarget_  = std::make_unique<vk::VkRenderTarget>();
    pipelineCache_ = std::make_unique<vk::PipelineCache>();
    uploadHeap_    = std::make_unique<vk::VkUploadHeap>();
    defaultTex_    = std::make_unique<vk::VkDefaultTexture>();
    textureMgr_    = std::make_unique<vk::VkTextureManager>();

    vk::VkContextCreateInfo ci{};
    ci.window = win;
#ifndef NDEBUG
    ci.enableValidation = true;
#else
    ci.enableValidation = false;
#endif
    if (!ctx_->Init(ci)) {
        std::fprintf(stderr, "[VK] RendererVulkan: VkContext::Init failed\n");
        return;
    }

    vk::SwapchainCreateInfo sci{};
    sci.requestedWidth  = uint32_t(w);
    sci.requestedHeight = uint32_t(h);
    // Default vsync = on (FIFO). Honor F11 "Unlock Frame Rate" toggle so
    // benchmarks can break monitor refresh cap (mirrors original D3D8
    // D3DPRESENT_INTERVAL_IMMEDIATE behavior).
    sci.vsync           = !TH06_VK_UNLOCK_FRAMERATE();
    sci.srgb            = false;
    if (!swap_->Recreate(*ctx_, sci))                      { std::fprintf(stderr, "[VK] swapchain init failed\n"); return; }
    if (!frames_->Init(*ctx_))                             { std::fprintf(stderr, "[VK] frames init failed\n"); return; }
    if (!renderTarget_->Create(*ctx_, *swap_))             { std::fprintf(stderr, "[VK] rt init failed\n"); return; }
    if (!initShaderModules())                              { std::fprintf(stderr, "[VK] shader load failed\n"); return; }
    if (!initLayoutsAndPool())                             { std::fprintf(stderr, "[VK] layout/pool failed\n"); return; }

    vk::PipelineFactoryDeps deps{};
    deps.renderPass     = renderTarget_->renderPass();
    deps.layoutNoTex    = pipelineLayoutNoTex_;
    deps.layoutTextured = pipelineLayoutTex_;
    for (int i = 0; i < 6; ++i) deps.vertModules[i] = vertModules_[i];
    deps.fragColor    = fragColorMod_;
    deps.fragTextured = fragTexMod_;
    if (!pipelineCache_->Init(*ctx_, deps))                { std::fprintf(stderr, "[VK] pipeline cache init failed\n"); return; }
    if (!uploadHeap_->Init(*ctx_))                         { std::fprintf(stderr, "[VK] upload heap init failed\n"); return; }
    if (!defaultTex_->Init(*ctx_, descriptorPool_, descLayoutTex_)) { std::fprintf(stderr, "[VK] default tex init failed\n"); return; }
    if (!textureMgr_->Init(*ctx_, descLayoutTex_))         { std::fprintf(stderr, "[VK] texture mgr init failed\n"); return; }

    initialized_ = true;
    std::fprintf(stderr, "[VK] RendererVulkan Phase 3 initialized %dx%d (validation=%s)\n",
                 w, h, ctx_->validationActive() ? "on" : "off");
}

void RendererVulkan::InitDevice(u32) { /* no-op for Vk; everything in Init */ }
void RendererVulkan::Release()       { destroyAll(); }
void RendererVulkan::ResizeTarget()  { swapchainOutOfDate_ = true; }
void RendererVulkan::BeginScene()    { /* no-op */ }
void RendererVulkan::EndScene()      { /* no-op */ }

void RendererVulkan::BeginFrame() {
    if (!initialized_) return;

    // Phase 6 batcher: defensive reset. Anything left over from a previous
    // frame's command buffer is now invalid (cmd buffer was either submitted
    // or abandoned); discard the staging vector so we start fresh.
    pendingBatch_.active = false;
    pendingBatch_.verts.clear();
    pendingBatch_.vertCount = 0;
    statsInputDraws_ = 0;
    statsBatchFlush_ = 0;
    statsVertsOut_   = 0;
    for (uint32_t i = 0; i < FR_COUNT; ++i) flushReasonHist_[i] = 0;
    for (int li = 0; li < kLayoutSlots_; ++li)
        for (uint32_t i = 0; i < FR_COUNT; ++i)
            flushReasonByLayout_[li][i] = 0;

    auto& frame = frames_->current();
    VkDevice dev = ctx_->device();

    TH_VK_CHECK(vkWaitForFences(dev, 1, &frame.inFlight, VK_TRUE, UINT64_MAX));

    // Live-toggle hook for F11 → Unlock Frame Rate. We track the last vsync
    // intent we applied to the swapchain; whenever the user flips the toggle
    // we rebuild ONLY the swapchain (present-mode change). We deliberately
    // do NOT go through the full swapchainOutOfDate_ path because that
    // destroys/recreates the offscreen render pass, which would invalidate
    // every pipeline cached in PipelineCache (deps.renderPass becomes a
    // dangling handle → AV deep inside the ICD on the next createPipeline).
    // The offscreen render target is independent of the swap images (we
    // blit from offscreen to swap), so present-mode changes need no RT work.
    {
        const bool wantVsync = !TH06_VK_UNLOCK_FRAMERATE();
        static bool s_lastVsyncIntent = true;
        if (wantVsync != s_lastVsyncIntent) {
            s_lastVsyncIntent = wantVsync;
            vkDeviceWaitIdle(dev);
            vk::SwapchainCreateInfo sci{};
            sci.requestedWidth  = uint32_t(screenWidth);
            sci.requestedHeight = uint32_t(screenHeight);
            sci.vsync           = wantVsync;
            sci.srgb            = false;
            if (!swap_->Recreate(*ctx_, sci)) {
                std::fprintf(stderr, "[VK] swapchain present-mode rebuild failed\n");
            }
        }
    }

    if (swapchainOutOfDate_) {
        vkDeviceWaitIdle(dev);
        renderTarget_->Destroy(*ctx_);
        vk::SwapchainCreateInfo sci{};
        sci.requestedWidth  = uint32_t(screenWidth);
        sci.requestedHeight = uint32_t(screenHeight);
        // Honor F11 "Unlock Frame Rate": switch present mode dynamically on
        // every swapchain rebuild so toggling the option live takes effect.
        sci.vsync           = !TH06_VK_UNLOCK_FRAMERATE();
        sci.srgb            = false;
        if (!swap_->Recreate(*ctx_, sci)) {
            // Swapchain rebuild failed — most likely VK_ERROR_SURFACE_LOST_KHR
            // because the Android SurfaceView was destroyed/recreated across
            // pause/resume. The old swapchain is permanently bound to the
            // dead surface, so we must destroy it BEFORE recreating
            // VkSurfaceKHR. Then retry Recreate against the fresh surface.
            std::fprintf(stderr, "[VK] swapchain recreate failed, attempting surface recreate\n");
            swap_->Destroy(*ctx_);
            if (!ctx_->RecreateSurface(window)) {
                return;
            }
            if (!swap_->Recreate(*ctx_, sci)) {
                std::fprintf(stderr, "[VK] swapchain recreate failed even after surface recreate\n");
                return;
            }
        }
        if (!renderTarget_->Create(*ctx_, *swap_)) return;
        // Keep realScreenWidth/Height in sync with swapchain so screen-space
        // overlays (touch buttons, etc.) get the correct pillarbox layout.
        int drawW = screenWidth, drawH = screenHeight;
        SDL_Vulkan_GetDrawableSize(window, &drawW, &drawH);
        realScreenWidth  = drawW;
        realScreenHeight = drawH;
        swapchainOutOfDate_ = false;
    }

    VkResult acq = swap_->AcquireNext(*ctx_, frame.imgAvail, &currentSwapImage_);
    if (acq == VK_ERROR_OUT_OF_DATE_KHR) { swapchainOutOfDate_ = true; return; }
    // VK_TIMEOUT can occur when the Android surface was destroyed mid-frame
    // (see VkSwapchain::AcquireNext hang-guard). VK_ERROR_SURFACE_LOST_KHR
    // is the explicit form of the same condition. In both cases drop this
    // frame and force a swapchain rebuild on the next BeginFrame; the
    // GameWindow::Render() fast-path also gates entry on
    // IsWindowPresentationUnavailable() so we won't busy-loop.
    if (acq == VK_TIMEOUT || acq == VK_NOT_READY || acq == VK_ERROR_SURFACE_LOST_KHR) {
        swapchainOutOfDate_ = true;
        return;
    }
    if (acq != VK_SUCCESS && acq != VK_SUBOPTIMAL_KHR) {
        std::fprintf(stderr, "[VK] AcquireNext -> %s\n", vk::VkResultToString(acq));
        return;
    }

    TH_VK_CHECK(vkResetFences(dev, 1, &frame.inFlight));
    TH_VK_CHECK(vkResetCommandBuffer(frame.cmd, 0));

    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    TH_VK_CHECK(vkBeginCommandBuffer(frame.cmd, &bi));

    uploadHeap_->BeginFrame(uint32_t(frameCounter_));

    // Begin render pass. Fix 17: color attachment is now LOAD_OP_LOAD on a
    // persistent OFFSCREEN image (FBO equivalent of GL backend), so HUD/UI
    // draws survive across frames. clearVals[0] is unused for color (LOAD_OP_LOAD
    // ignores it) but kept for the depth attachment which still uses LOAD_OP_CLEAR.
    // Game-issued Clear() calls go through in-pass vkCmdClearAttachments which
    // respects scissor/viewport rect.
    VkClearValue clearVals[2] = {};
    clearVals[0].color.float32[0] = 0.0f;
    clearVals[0].color.float32[1] = 0.0f;
    clearVals[0].color.float32[2] = 0.0f;
    clearVals[0].color.float32[3] = 1.0f;
    clearVals[1].depthStencil.depth   = 1.0f;
    clearVals[1].depthStencil.stencil = 0;

    VkExtent2D ext = renderTarget_->extent();
    VkRenderPassBeginInfo rpbi = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
    rpbi.renderPass        = renderTarget_->renderPass();
    rpbi.framebuffer       = renderTarget_->framebuffer(currentSwapImage_);
    rpbi.renderArea.extent = ext;
    rpbi.clearValueCount   = 2;
    rpbi.pClearValues      = clearVals;
    vkCmdBeginRenderPass(frame.cmd, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
    inRenderPass_ = true;

    // Set viewport + scissor (dynamic).
    // Letterbox: game logical resolution is screenWidth x screenHeight (640x480),
    // swapchain is full window. Compute scaled centered region preserving aspect.
    // SetViewport() below uses the same mapping for game-driven viewport changes.
    {
        const float gameW = float(screenWidth  > 0 ? screenWidth  : int(ext.width));
        const float gameH = float(screenHeight > 0 ? screenHeight : int(ext.height));
        const float scale = std::min(float(ext.width) / gameW, float(ext.height) / gameH);
        const float scaledW = gameW * scale;
        const float scaledH = gameH * scale;
        const float offsetX = (float(ext.width)  - scaledW) * 0.5f;
        const float offsetY = (float(ext.height) - scaledH) * 0.5f;
        const int   gvX = (viewportW > 0) ? viewportX : 0;
        const int   gvY = (viewportW > 0) ? viewportY : 0;
        const int   gvW = (viewportW > 0) ? viewportW : int(gameW);
        const int   gvH = (viewportH > 0) ? viewportH : int(gameH);
        VkViewport vp = {};
        vp.x        = offsetX + float(gvX) * scale;
        vp.y        = offsetY + float(gvY) * scale;
        vp.width    = float(gvW) * scale;
        vp.height   = float(gvH) * scale;
        vp.minDepth = viewportMinZ_;
        vp.maxDepth = viewportMaxZ_;
        // Fix 18: pixel-aligned scissor (round nearest) to avoid 1-px leak/clip
        // on letterbox sub-pixel boundaries. Truncating vp.x or vp.width drops
        // up to one pixel; bottom row of playfield could leak/show stale pixels.
        const int32_t  sx0 = int32_t(std::lround(vp.x));
        const int32_t  sy0 = int32_t(std::lround(vp.y));
        const int32_t  sx1 = int32_t(std::lround(vp.x + vp.width));
        const int32_t  sy1 = int32_t(std::lround(vp.y + vp.height));
        VkRect2D sc = {};
        sc.offset.x      = sx0;
        sc.offset.y      = sy0;
        sc.extent.width  = uint32_t(sx1 - sx0);
        sc.extent.height = uint32_t(sy1 - sy0);
        vkCmdSetViewport(frame.cmd, 0, 1, &vp);
        vkCmdSetScissor (frame.cmd, 0, 1, &sc);
    }

    frameStarted_ = true;
}

void RendererVulkan::EndFrame() {
    if (!initialized_ || !frameStarted_) { frameStarted_ = false; return; }

    auto& frame = frames_->current();

    if (inRenderPass_) {
#ifdef TH06_HAS_IMGUI
        // Phase 6 batcher: drain any sprites left after the game's last
        // explicit draw so they appear UNDER the ImGui overlay.
        if (pendingBatch_.active) bumpFlushReason(FR_NonDraw_EndFrame);
        FlushBatch();
        // Phase 5b.2: composite ImGui (thprac overlay) ON TOP of the game frame
        // while the offscreen render pass is still open. THPracGuiRender forwards
        // into GameGuiRender which (Vulkan branch) calls RenderImGui() below.
        THPrac::THPracGuiRender();
#else
        if (pendingBatch_.active) bumpFlushReason(FR_NonDraw_EndFrame);
        FlushBatch();
#endif
        vkCmdEndRenderPass(frame.cmd);
        inRenderPass_ = false;
    }

    // Fix 17: Blit persistent offscreen color image to acquired swapchain image,
    // then transition swap image to PRESENT_SRC_KHR. Offscreen color image stays
    // in COLOR_ATTACHMENT_OPTIMAL after the renderpass; we transition it to
    // TRANSFER_SRC_OPTIMAL for the blit and back to COLOR_ATTACHMENT_OPTIMAL.
    {
        VkImage offscreen = renderTarget_->colorImage();
        VkImage swapImg   = swap_->image(currentSwapImage_);
        VkExtent2D ext    = renderTarget_->extent();
        VkImageSubresourceRange colorRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

        // Barriers: offscreen COLOR_ATTACHMENT_OPTIMAL -> TRANSFER_SRC_OPTIMAL
        //           swap     UNDEFINED                 -> TRANSFER_DST_OPTIMAL
        VkImageMemoryBarrier preBlit[2] = {};
        preBlit[0].sType         = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        preBlit[0].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        preBlit[0].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        preBlit[0].oldLayout     = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        preBlit[0].newLayout     = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        preBlit[0].srcQueueFamilyIndex = preBlit[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        preBlit[0].image            = offscreen;
        preBlit[0].subresourceRange = colorRange;

        preBlit[1].sType         = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        preBlit[1].srcAccessMask = 0;
        preBlit[1].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        preBlit[1].oldLayout     = VK_IMAGE_LAYOUT_UNDEFINED;
        preBlit[1].newLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        preBlit[1].srcQueueFamilyIndex = preBlit[1].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        preBlit[1].image            = swapImg;
        preBlit[1].subresourceRange = colorRange;

        vkCmdPipelineBarrier(frame.cmd,
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            0, 0, nullptr, 0, nullptr, 2, preBlit);

        // Same-size 1:1 copy (offscreen and swap have identical extent).
        VkImageCopy copy = {};
        copy.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        copy.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        copy.extent         = { ext.width, ext.height, 1 };
        vkCmdCopyImage(frame.cmd,
            offscreen, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            swapImg,   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            1, &copy);

        // Barriers: swap     TRANSFER_DST_OPTIMAL -> PRESENT_SRC_KHR
        //           offscreen TRANSFER_SRC_OPTIMAL -> COLOR_ATTACHMENT_OPTIMAL
        VkImageMemoryBarrier postBlit[2] = {};
        postBlit[0].sType         = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        postBlit[0].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        postBlit[0].dstAccessMask = 0;
        postBlit[0].oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        postBlit[0].newLayout     = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        postBlit[0].srcQueueFamilyIndex = postBlit[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        postBlit[0].image            = swapImg;
        postBlit[0].subresourceRange = colorRange;

        postBlit[1].sType         = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        postBlit[1].srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        postBlit[1].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT
                                  | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        postBlit[1].oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        postBlit[1].newLayout     = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        postBlit[1].srcQueueFamilyIndex = postBlit[1].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        postBlit[1].image            = offscreen;
        postBlit[1].subresourceRange = colorRange;

        vkCmdPipelineBarrier(frame.cmd,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT
            | VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
            0, 0, nullptr, 0, nullptr, 2, postBlit);
    }

    TH_VK_CHECK(vkEndCommandBuffer(frame.cmd));

    VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.waitSemaphoreCount   = 1;
    si.pWaitSemaphores      = &frame.imgAvail;
    si.pWaitDstStageMask    = &waitStage;
    si.commandBufferCount   = 1;
    si.pCommandBuffers      = &frame.cmd;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores    = &frame.renderDone;
    TH_VK_CHECK(vkQueueSubmit(ctx_->graphicsQueue(), 1, &si, frame.inFlight));

    // Note: vkQueuePresentKHR moved to Present() per Phase 5a (ADR-008).
    // EndFrame submits the command buffer; Present() pushes the swapchain image.
    frameStarted_ = false;

    // Phase 6 batcher diagnostics — gated on user-selected log level >= Info(3),
    // emitted once per second of game time (60 frames) so it doesn't dominate
    // the log even at INFO. Format:
    //   [Vk Batch] frame=N  inDraws=I  flushes=F  vertsOut=V  ratio=I/F
    if (THPrac::TH06::THPracGetLogLevel() >= 3 && (frameCounter_ % 60u == 0u) && statsBatchFlush_ > 0) {
        const double ratio = double(statsInputDraws_) / double(statsBatchFlush_);
        // Compose a short flush-reason histogram so the user can tell which
        // state field is dominating cache misses. Print only the top-5 reasons
        // with non-zero counts, sorted descending.
        const char* names[FR_COUNT] = {
            "_none","FirstDraw","VtxLayout","HasTex","DepthTest","BlendMode",
            "ColorOp","ZWrite","DepthFunc","Texture","TexStgDiff","TexFactor",
            "FogEn","FogParams","Mvp","Viewport","ScreenSize",
            "ND_Clear","ND_SetVP","ND_EndFrame"
        };
        // Sort indices by count desc.
        uint32_t idx[FR_COUNT];
        for (uint32_t i = 0; i < FR_COUNT; ++i) idx[i] = i;
        std::sort(idx, idx + FR_COUNT, [&](uint32_t a, uint32_t b) {
            return flushReasonHist_[a] > flushReasonHist_[b];
        });
        char hist[256]; int hp = 0;
        for (uint32_t i = 0; i < FR_COUNT && i < 5; ++i) {
            uint32_t r = idx[i];
            if (flushReasonHist_[r] == 0) break;
            int n = std::snprintf(hist + hp, sizeof(hist) - hp,
                                  " %s=%u", names[r], flushReasonHist_[r]);
            if (n <= 0 || hp + n >= int(sizeof(hist))) break;
            hp += n;
        }
        hist[hp] = '\0';

        // Per-layout flush attribution: rank layouts by total flushes this
        // tick, then for each top layout list its top-2 reasons. This is the
        // signal needed to pick the next pre-multiply target — Phase 6.2 fixed
        // layout 3 but telemetry showed layout 4 was the actual hot path.
        char layHist[320]; int lp = 0;
        {
            uint32_t totalsByLayout[kLayoutSlots_] = {};
            for (int li = 0; li < kLayoutSlots_; ++li) {
                for (uint32_t r = 0; r < FR_COUNT; ++r) totalsByLayout[li] += flushReasonByLayout_[li][r];
            }
            int order[kLayoutSlots_];
            for (int li = 0; li < kLayoutSlots_; ++li) order[li] = li;
            std::sort(order, order + kLayoutSlots_, [&](int a, int b) {
                return totalsByLayout[a] > totalsByLayout[b];
            });
            for (int rank = 0; rank < kLayoutSlots_ && rank < 4; ++rank) {
                int li = order[rank];
                if (totalsByLayout[li] == 0) break;
                int n = std::snprintf(layHist + lp, sizeof(layHist) - lp,
                                      " L%d=%u(", li, totalsByLayout[li]);
                if (n <= 0 || lp + n >= int(sizeof(layHist))) break;
                lp += n;
                // Top-2 reasons within this layout.
                uint32_t ridx[FR_COUNT];
                for (uint32_t i = 0; i < FR_COUNT; ++i) ridx[i] = i;
                std::sort(ridx, ridx + FR_COUNT, [&](uint32_t a, uint32_t b) {
                    return flushReasonByLayout_[li][a] > flushReasonByLayout_[li][b];
                });
                int shown = 0;
                for (uint32_t i = 0; i < FR_COUNT && shown < 2; ++i) {
                    uint32_t r = ridx[i];
                    if (flushReasonByLayout_[li][r] == 0) break;
                    n = std::snprintf(layHist + lp, sizeof(layHist) - lp,
                                      "%s%s=%u", shown ? "," : "",
                                      names[r], flushReasonByLayout_[li][r]);
                    if (n <= 0 || lp + n >= int(sizeof(layHist))) break;
                    lp += n; ++shown;
                }
                n = std::snprintf(layHist + lp, sizeof(layHist) - lp, ")");
                if (n <= 0 || lp + n >= int(sizeof(layHist))) break;
                lp += n;
            }
        }
        layHist[lp] = '\0';

        std::fprintf(stderr,
            "[Vk Batch] f%u inDraws=%u flushes=%u vertsOut=%u merge=%.2fx |%s | byL%s\n",
            unsigned(frameCounter_),
            statsInputDraws_, statsBatchFlush_, statsVertsOut_, ratio, hist, layHist);
        // GUI-subsystem builds don't show stderr, so also tee to a file
        // next to the executable. Lazy-open on first hit; line-buffered.
        static FILE* s_batchLog = nullptr;
        if (!s_batchLog) {
            s_batchLog = std::fopen("vk_batch.log", "w");
        }
        if (s_batchLog) {
            std::fprintf(s_batchLog,
                "[Vk Batch] f%u inDraws=%u flushes=%u vertsOut=%u merge=%.2fx |%s | byL%s\n",
                unsigned(frameCounter_),
                statsInputDraws_, statsBatchFlush_, statsVertsOut_, ratio, hist, layHist);
            std::fflush(s_batchLog);
        }
    }
}

void RendererVulkan::Present() {
    if (!initialized_) return;

    auto& frame = frames_->current();

    VkSwapchainKHR swapHandles[1] = { swap_->handle() };
    VkPresentInfoKHR pi = { VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores    = &frame.renderDone;
    pi.swapchainCount     = 1;
    pi.pSwapchains        = swapHandles;
    pi.pImageIndices      = &currentSwapImage_;
    VkResult pr = vkQueuePresentKHR(ctx_->graphicsQueue(), &pi);
    // Note: VK_SUBOPTIMAL_KHR means the swapchain still works correctly but
    // the driver could pick a more efficient configuration. On Android we
    // intentionally pass preTransform=IDENTITY while caps.currentTransform is
    // typically ROTATE_90, which makes Adreno return SUBOPTIMAL on EVERY
    // present. Treating SUBOPTIMAL as out-of-date there causes a swapchain
    // rebuild each frame (drops to ~30 FPS, sustained vmaCreateImage churn).
    // The functional behaviour with IDENTITY is correct (SurfaceFlinger
    // performs the rotation), so we ignore SUBOPTIMAL and only rebuild on
    // OUT_OF_DATE / SURFACE_LOST.
    if (pr == VK_ERROR_OUT_OF_DATE_KHR || pr == VK_ERROR_SURFACE_LOST_KHR) {
        swapchainOutOfDate_ = true;
    } else if (pr != VK_SUCCESS && pr != VK_SUBOPTIMAL_KHR) {
        std::fprintf(stderr, "[VK] vkQueuePresentKHR -> %s\n", vk::VkResultToString(pr));
    }

    frames_->advance();
    ++frameCounter_;
}

void RendererVulkan::Clear(D3DCOLOR color, i32 clearColor, i32 clearDepth) {
    // Phase 6 batcher: pending sprites must reach the cmd buffer BEFORE the
    // clear, otherwise vkCmdClearAttachments would obscure their work.
    if (pendingBatch_.active) bumpFlushReason(FR_NonDraw_Clear);
    FlushBatch();
    // Only remember color when caller actually wants color cleared. Storing it
    // unconditionally bleeds the value into next frame's render-pass LOAD_OP_CLEAR
    // (which ignores scissor and paints the whole attachment). GameManager.cpp:229
    // calls Clear(skyFog.color, 0, 1) every frame for depth-only — without this
    // gate, the HUD/letterbox area would get repainted with the stage sky color
    // at the start of every frame.
    if (clearColor) ColorToFloat4(color, clearColor_);
    if (!initialized_ || !frameStarted_ || !inRenderPass_) return;
    if (!clearColor && !clearDepth)      return;

    auto& frame = frames_->current();
    VkExtent2D ext = renderTarget_->extent();

    VkClearAttachment atts[2] = {};
    uint32_t attCount = 0;
    if (clearColor) {
        atts[attCount].aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        atts[attCount].colorAttachment = 0;
        atts[attCount].clearValue.color.float32[0] = clearColor_[0];
        atts[attCount].clearValue.color.float32[1] = clearColor_[1];
        atts[attCount].clearValue.color.float32[2] = clearColor_[2];
        atts[attCount].clearValue.color.float32[3] = clearColor_[3];
        ++attCount;
    }
    if (clearDepth) {
        atts[attCount].aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        atts[attCount].clearValue.depthStencil.depth   = 1.0f;
        atts[attCount].clearValue.depthStencil.stencil = 0;
        ++attCount;
    }
    // Match RendererGL::Clear (sdl2_renderer.cpp:344-356): glClear is bounded by
    // glScissor (which is permanently enabled). When game calls Clear() after
    // SetViewport(playfield), GL only clears the playfield. Vulkan's vkCmdClearAttachments
    // takes an explicit rect that must mirror the current viewport — using full extent
    // here would wipe the HUD/letterbox area with the stage sky color.
    VkClearRect rect = {};
    if (viewportW > 0 && viewportH > 0) {
        const float gameW = float(screenWidth  > 0 ? screenWidth  : int(ext.width));
        const float gameH = float(screenHeight > 0 ? screenHeight : int(ext.height));
        const float scale = std::min(float(ext.width) / gameW, float(ext.height) / gameH);
        const float scaledW = gameW * scale;
        const float scaledH = gameH * scale;
        const float offsetX = (float(ext.width)  - scaledW) * 0.5f;
        const float offsetY = (float(ext.height) - scaledH) * 0.5f;
        // Fix 18: pixel-aligned (round nearest) so clear rect matches scissor exactly.
        const float gx0 = offsetX + float(viewportX) * scale;
        const float gy0 = offsetY + float(viewportY) * scale;
        const float gx1 = gx0 + float(viewportW) * scale;
        const float gy1 = gy0 + float(viewportH) * scale;
        const int32_t ix0 = int32_t(std::lround(gx0));
        const int32_t iy0 = int32_t(std::lround(gy0));
        const int32_t ix1 = int32_t(std::lround(gx1));
        const int32_t iy1 = int32_t(std::lround(gy1));
        rect.rect.offset.x      = ix0;
        rect.rect.offset.y      = iy0;
        rect.rect.extent.width  = uint32_t(ix1 - ix0);
        rect.rect.extent.height = uint32_t(iy1 - iy0);
    } else {
        rect.rect.extent = ext;
    }
    rect.layerCount  = 1;
    vkCmdClearAttachments(frame.cmd, attCount, atts, 1, &rect);
}

void RendererVulkan::SetViewport(i32 x, i32 y, i32 w, i32 h, f32 minZ, f32 maxZ) {
    {
        static int s = 0;
        if (s < 32) {
            std::fprintf(stderr, "[Vk SetVP] x=%d y=%d w=%d h=%d minZ=%.3f maxZ=%.3f (frameStarted=%d inRP=%d)\n",
                x, y, w, h, minZ, maxZ, frameStarted_?1:0, inRenderPass_?1:0);
            ++s;
        }
    }
    viewportX = x; viewportY = y; viewportW = w; viewportH = h;
    viewportMinZ_ = minZ; viewportMaxZ_ = maxZ;
    if (frameStarted_ && inRenderPass_) {
        // Phase 6 batcher: changing viewport breaks the in-flight batch's
        // 2D scissor/viewport snapshot, so flush before issuing vkCmdSet*.
        if (pendingBatch_.active) bumpFlushReason(FR_NonDraw_SetViewport);
        FlushBatch();
        // Apply same letterbox mapping as BeginFrame.
        VkExtent2D ext = renderTarget_->extent();
        const float gameW = float(screenWidth  > 0 ? screenWidth  : int(ext.width));
        const float gameH = float(screenHeight > 0 ? screenHeight : int(ext.height));
        const float scale = std::min(float(ext.width) / gameW, float(ext.height) / gameH);
        const float scaledW = gameW * scale;
        const float scaledH = gameH * scale;
        const float offsetX = (float(ext.width)  - scaledW) * 0.5f;
        const float offsetY = (float(ext.height) - scaledH) * 0.5f;
        VkViewport vp = {};
        vp.x        = offsetX + float(x) * scale;
        vp.y        = offsetY + float(y) * scale;
        vp.width    = float(w) * scale;
        vp.height   = float(h) * scale;
        vp.minDepth = minZ; vp.maxDepth = maxZ;  // D3D8 depth-range partitioning (commit 682146c)
        vkCmdSetViewport(frames_->current().cmd, 0, 1, &vp);
        // Match RendererGL::SetViewport (sdl2_renderer.cpp:367-368): scissor follows
        // viewport rect 1:1. D3D8 implicitly clips to viewport, GL emulates with
        // glScissor; without this, 3D background draws after a 2D layer (Fix 11
        // restore) leak into the HUD panel area.
        // Fix 18: pixel-aligned scissor (round nearest) — truncating vp.x/width
        // drops up to one pixel on sub-pixel letterbox boundaries.
        const int32_t  sx0 = int32_t(std::lround(vp.x));
        const int32_t  sy0 = int32_t(std::lround(vp.y));
        const int32_t  sx1 = int32_t(std::lround(vp.x + vp.width));
        const int32_t  sy1 = int32_t(std::lround(vp.y + vp.height));
        VkRect2D sc = {};
        sc.offset.x      = sx0;
        sc.offset.y      = sy0;
        sc.extent.width  = uint32_t(sx1 - sx0);
        sc.extent.height = uint32_t(sy1 - sy0);
        vkCmdSetScissor(frames_->current().cmd, 0, 1, &sc);
    }
}

// --- State setters ---
void RendererVulkan::SetBlendMode(u8 m)                  { currentBlendMode = m; }
void RendererVulkan::SetColorOp(u8 op)                   { currentColorOp = op; }
void RendererVulkan::SetTexture(u32 tex)                 { currentTexture = tex; }
void RendererVulkan::SetTextureFactor(D3DCOLOR f)        { textureFactor = f; }
void RendererVulkan::SetZWriteDisable(u8 d)              { currentZWriteDisable = d; }
void RendererVulkan::SetDepthFunc(i32 alwaysPass)        { depthFuncAlways_ = alwaysPass ? 1 : 0; }
void RendererVulkan::SetDestBlendInvSrcAlpha()           { currentBlendMode = 0; /* alpha (matches GL: glBlendFunc SRC_ALPHA, ONE_MINUS_SRC_ALPHA) */ }
void RendererVulkan::SetDestBlendOne()                   { currentBlendMode = 1; /* additive (matches GL: glBlendFunc SRC_ALPHA, ONE) */ }
// D3DTSS_COLOROP = SELECTARG2(diffuse): force sampling defaultTex_ (1x1 white)
// at draw time, but DO NOT zero currentTexture. AnmManager caches its own
// currentTexture and only re-binds on a different sourceFileIndex; clobbering
// renderer's currentTexture would desync that cache and cause subsequent
// same-sprite draws to render as solid colored blocks (white * diffuse).
void RendererVulkan::SetTextureStageSelectDiffuse()      { textureStageDiffuseOnly_ = true; }
void RendererVulkan::SetTextureStageModulateTexture()    { textureStageDiffuseOnly_ = false; }
void RendererVulkan::SetFog(i32 e, D3DCOLOR c, f32 s, f32 z) {
    fogEnabled = e; fogColor = c; fogStart = s; fogEnd = z;
    // Linear D3DFOGMODE_LINEAR fog: drawCommon pushes these into PushConstants
    // and f_textured.frag applies `mix(fogColor, base, clamp((end - viewZ)/(end - start)))`.
    // Gated to depthTest=true draws so 2D paths (xyzrwh, w=1) never trigger it.
}

// --- Transforms ---
void RendererVulkan::SetViewTransform(const D3DXMATRIX *matrix) {
    if (!matrix) return;
    std::memcpy(viewMat_, &matrix->m[0][0], sizeof(viewMat_));
    mvpDirty_ = true;
}
void RendererVulkan::SetProjectionTransform(const D3DXMATRIX *matrix) {
    if (!matrix) return;
    std::memcpy(projMat_, &matrix->m[0][0], sizeof(projMat_));
    mvpDirty_ = true;
}
void RendererVulkan::SetWorldTransform(const D3DXMATRIX *matrix) {
    if (!matrix) return;
    std::memcpy(worldMat_, &matrix->m[0][0], sizeof(worldMat_));
    mvpDirty_ = true;
}
void RendererVulkan::SetTextureTransform(const D3DXMATRIX *) {
    // Phase 2: not used by TH06 game logic in any draw path we're stubbing.
}

void RendererVulkan::recomputeMvp() {
    // D3D row-major math: (world * view) * proj.
    float wv[16];
    Mat4Mul_RowMajor(worldMat_, viewMat_, wv);
    Mat4Mul_RowMajor(wv, projMat_, mvpMat_);
    mvpDirty_ = false;
}

// --- Draw helpers ---
//
// Phase 6 sprite batcher: drawCommon now enqueues into pendingBatch_; the
// expensive AllocVerts + Bind* + Push + vkCmdDraw work happens once per batch
// in FlushBatch(). State-equality check decides whether the new draw can join
// the in-flight batch or must start a new one. All non-draw GPU commands
// (Clear, SetViewport, render-pass close) call FlushBatch() first to preserve
// submission order semantics.

bool RendererVulkan::batchStateMatches(int vertexLayoutEnum,
                                       bool hasTexture,
                                       bool depthTest) const {
    const PendingBatch& b = pendingBatch_;
    if (!b.active)                                            { lastMismatchReason_ = FR_FirstDraw;                return false; }
    if (b.vertexLayoutEnum  != vertexLayoutEnum)              { lastMismatchReason_ = FR_VertexLayout;             return false; }
    if (b.hasTexture        != hasTexture)                    { lastMismatchReason_ = FR_HasTexture;               return false; }
    if (b.depthTest         != depthTest)                     { lastMismatchReason_ = FR_DepthTest;                return false; }
    if (b.blendMode         != currentBlendMode)              { lastMismatchReason_ = FR_BlendMode;                return false; }
    if (b.colorOp           != currentColorOp)                { lastMismatchReason_ = FR_ColorOp;                  return false; }
    if (b.zWriteDisable     != currentZWriteDisable)          { lastMismatchReason_ = FR_ZWriteDisable;            return false; }
    if (b.depthFuncAlways   != depthFuncAlways_)              { lastMismatchReason_ = FR_DepthFuncAlways;          return false; }
    if (b.texture           != currentTexture)                { lastMismatchReason_ = FR_Texture;                  return false; }
    if (b.textureStageDiffuseOnly != textureStageDiffuseOnly_){ lastMismatchReason_ = FR_TextureStageDiffuseOnly;  return false; }
    if (b.fogEnabled        != fogEnabled)                    { lastMismatchReason_ = FR_FogEnabled;               return false; }
    if (b.fogColor != fogColor || b.fogStart != fogStart || b.fogEnd != fogEnd) { lastMismatchReason_ = FR_FogParams; return false; }
    // textureFactor is only consumed by shaders for layouts 1 (Tex1Xyzrwh) and
    // 4 (RenderVertexInfoXyz) — the others reference the field only to keep
    // PushConstants layout parity. Skipping the comparison for unused layouts
    // recovers the second-largest flush reason (~17.6% in Phase 6.1 telemetry).
    const bool layoutUsesTexFactor = (b.vertexLayoutEnum == 1) || (b.vertexLayoutEnum == 4);
    if (layoutUsesTexFactor && b.textureFactor != textureFactor)
                                                              { lastMismatchReason_ = FR_TextureFactor;            return false; }
    // MVP only matters for 3D-layout draws. The xyzrwh shaders (layouts 0/1/2)
    // build gl_Position directly from the pre-transformed screen-space input
    // and never read pc.mvp, so skipping the comparison there is visually
    // identity — and recovers the dominant flush reason in the histogram
    // (Phase 6.1 telemetry: ~79% of flushes were Mvp differences for sprites
    // that didn't even consume the matrix).
    const bool layoutUsesMvp = (b.vertexLayoutEnum == 3) || (b.vertexLayoutEnum == 4);
    if (layoutUsesMvp && std::memcmp(b.mvp, mvpMat_, sizeof(b.mvp)) != 0)
                                                              { lastMismatchReason_ = FR_Mvp;                      return false; }
    if (b.viewportX != viewportX || b.viewportY != viewportY ||
        b.viewportW != viewportW || b.viewportH != viewportH ||
        b.viewportMinZ != viewportMinZ_ || b.viewportMaxZ != viewportMaxZ_)     { lastMismatchReason_ = FR_Viewport; return false; }
    if (b.screenWidth != screenWidth || b.screenHeight != screenHeight)         { lastMismatchReason_ = FR_ScreenSize; return false; }
    return true;
}

void RendererVulkan::beginPendingBatch(int vertexLayoutEnum,
                                       bool hasTexture,
                                       bool depthTest,
                                       uint32_t vertexStride) {
    PendingBatch& b = pendingBatch_;
    b.active           = true;
    b.vertexLayoutEnum = vertexLayoutEnum;
    b.hasTexture       = hasTexture;
    b.depthTest        = depthTest;
    b.blendMode        = currentBlendMode;
    b.colorOp          = currentColorOp;
    b.zWriteDisable    = currentZWriteDisable;
    b.depthFuncAlways  = depthFuncAlways_;
    b.texture          = currentTexture;
    b.textureStageDiffuseOnly = textureStageDiffuseOnly_;
    b.textureFactor    = textureFactor;
    b.fogEnabled       = fogEnabled;
    b.fogColor         = fogColor;
    b.fogStart         = fogStart;
    b.fogEnd           = fogEnd;
    std::memcpy(b.mvp, mvpMat_, sizeof(b.mvp));
    b.viewportX = viewportX; b.viewportY = viewportY;
    b.viewportW = viewportW; b.viewportH = viewportH;
    b.viewportMinZ = viewportMinZ_; b.viewportMaxZ = viewportMaxZ_;
    b.screenWidth = screenWidth; b.screenHeight = screenHeight;
    b.vertexStride     = vertexStride;
    b.vertCount        = 0;
    b.mergedDraws      = 0;
    b.verts.clear();
}

void RendererVulkan::appendAsTriangleList(int topologyEnum,
                                          const void* verts,
                                          int count,
                                          uint32_t vertexStride) {
    if (count < 3) return;
    PendingBatch& b = pendingBatch_;
    const uint8_t* src = static_cast<const uint8_t*>(verts);

    // Topology 0 = STRIP, 1 = FAN, 2 = LIST.
    auto pushVert = [&](int idx) {
        const size_t off = b.verts.size();
        b.verts.resize(off + vertexStride);
        std::memcpy(b.verts.data() + off, src + size_t(idx) * vertexStride, vertexStride);
        ++b.vertCount;
    };
    const int triCount = count - 2;
    if (topologyEnum == 0) {                 // TRIANGLE_STRIP
        for (int i = 0; i < triCount; ++i) {
            if ((i & 1) == 0) {
                pushVert(i); pushVert(i + 1); pushVert(i + 2);
            } else {
                pushVert(i + 1); pushVert(i); pushVert(i + 2);
            }
        }
    } else if (topologyEnum == 1) {          // TRIANGLE_FAN
        for (int i = 0; i < triCount; ++i) {
            pushVert(0); pushVert(i + 1); pushVert(i + 2);
        }
    } else {                                  // TRIANGLE_LIST passthrough
        const int triCountList = (count / 3) * 3;
        for (int i = 0; i < triCountList; ++i) pushVert(i);
    }
}

void RendererVulkan::appendAsTriangleListPretransform3(int topologyEnum,
                                                       const void* verts,
                                                       int count) {
    if (count < 3) return;
    PendingBatch& b = pendingBatch_;
    // Input  layout 3 (Tex1DiffuseXyz):       vec3 pos + u32 color + vec2 uv = 24 B
    // Output layout 5 (Tex1DiffuseXyzClip):   vec4 clip + u32 color + vec2 uv = 28 B
    constexpr size_t kInStride  = 24;
    constexpr size_t kOutStride = 28;
    const uint8_t* src = static_cast<const uint8_t*>(verts);
    const float* M = mvpMat_;

    // Pre-transform input verts into a small scratch buffer so the strip/fan
    // expansion that follows can index them by primitive vertex. Reuse a
    // thread-local heap to avoid per-call allocation churn.
    static thread_local std::vector<uint8_t> xformed;
    xformed.resize(size_t(count) * kOutStride);
    for (int i = 0; i < count; ++i) {
        const uint8_t* sv = src + size_t(i) * kInStride;
        float px, py, pz;
        std::memcpy(&px, sv + 0, 4);
        std::memcpy(&py, sv + 4, 4);
        std::memcpy(&pz, sv + 8, 4);
        // mvpMat_ is the C++ row-major product (world*view*proj). The Vulkan
        // shaders memcpy the same bytes into a GLSL `mat4 mvp` (column-major
        // by default) and compute `pc.mvp * vec4(in_pos, 1.0)`. With those
        // bytes reinterpreted as columns, the shader expression becomes
        // mathematically equivalent to (v_row * M_rowmajor), i.e.
        //   clip[i] = sum_j(M[j*4+i] * v[j])
        // We replicate exactly that here so visual output is bit-identical
        // to the per-draw layout-3 path. Y-flip is applied by the shader,
        // not here.
        const float clipX = M[0]*px + M[4]*py + M[ 8]*pz + M[12];
        const float clipY = M[1]*px + M[5]*py + M[ 9]*pz + M[13];
        const float clipZ = M[2]*px + M[6]*py + M[10]*pz + M[14];
        const float clipW = M[3]*px + M[7]*py + M[11]*pz + M[15];
        uint8_t* dv = xformed.data() + size_t(i) * kOutStride;
        std::memcpy(dv +  0, &clipX, 4);
        std::memcpy(dv +  4, &clipY, 4);
        std::memcpy(dv +  8, &clipZ, 4);
        std::memcpy(dv + 12, &clipW, 4);
        std::memcpy(dv + 16, sv + 12, 4);  // diffuse (B8G8R8A8 D3DCOLOR)
        std::memcpy(dv + 20, sv + 16, 8);  // uv (vec2)
    }

    auto pushVert = [&](int idx) {
        const size_t off = b.verts.size();
        b.verts.resize(off + kOutStride);
        std::memcpy(b.verts.data() + off, xformed.data() + size_t(idx) * kOutStride, kOutStride);
        ++b.vertCount;
    };
    const int triCount = count - 2;
    if (topologyEnum == 0) {                 // TRIANGLE_STRIP
        for (int i = 0; i < triCount; ++i) {
            if ((i & 1) == 0) { pushVert(i);     pushVert(i + 1); pushVert(i + 2); }
            else              { pushVert(i + 1); pushVert(i);     pushVert(i + 2); }
        }
    } else if (topologyEnum == 1) {          // TRIANGLE_FAN
        for (int i = 0; i < triCount; ++i) {
            pushVert(0); pushVert(i + 1); pushVert(i + 2);
        }
    } else {                                  // TRIANGLE_LIST passthrough
        const int triCountList = (count / 3) * 3;
        for (int i = 0; i < triCountList; ++i) pushVert(i);
    }
}

void RendererVulkan::appendAsTriangleListPretransform4(int topologyEnum,
                                                       const void* verts,
                                                       int count) {
    if (count < 3) return;
    PendingBatch& b = pendingBatch_;
    // Input  layout 4 (RenderVertexInfoXyz):  vec3 pos + vec2 uv          = 20 B
    // Output layout 5 (Tex1DiffuseXyzClip):   vec4 clip + u32 BGRA + uv   = 28 B
    // The original layout-4 shader writes v_color = pc.textureFactor for
    // every vertex; we fold that into per-vertex color so the layout-5 PSO
    // (which reads v_color from input) reproduces it bit-identically. The
    // CURRENT textureFactor state is captured per call so two adjacent
    // calls with different textureFactor coexist in one batch.
    constexpr size_t kInStride  = 20;
    constexpr size_t kOutStride = 28;
    const uint8_t* src = static_cast<const uint8_t*>(verts);
    const float*   M   = mvpMat_;
    const uint32_t color = textureFactor;  // D3DCOLOR is little-endian 0xAARRGGBB
                                           // whose memory bytes B,G,R,A line up
                                           // with the Vulkan B8G8R8A8_UNORM
                                           // input on layout 5.

    static thread_local std::vector<uint8_t> xformed;
    xformed.resize(size_t(count) * kOutStride);
    for (int i = 0; i < count; ++i) {
        const uint8_t* sv = src + size_t(i) * kInStride;
        float px, py, pz;
        std::memcpy(&px, sv + 0, 4);
        std::memcpy(&py, sv + 4, 4);
        std::memcpy(&pz, sv + 8, 4);
        // Same row-major math as Pretransform3 (see comment there for the
        // bit-identical-to-shader argument).
        const float clipX = M[0]*px + M[4]*py + M[ 8]*pz + M[12];
        const float clipY = M[1]*px + M[5]*py + M[ 9]*pz + M[13];
        const float clipZ = M[2]*px + M[6]*py + M[10]*pz + M[14];
        const float clipW = M[3]*px + M[7]*py + M[11]*pz + M[15];
        uint8_t* dv = xformed.data() + size_t(i) * kOutStride;
        std::memcpy(dv +  0, &clipX, 4);
        std::memcpy(dv +  4, &clipY, 4);
        std::memcpy(dv +  8, &clipZ, 4);
        std::memcpy(dv + 12, &clipW, 4);
        std::memcpy(dv + 16, &color, 4);   // textureFactor folded in here
        std::memcpy(dv + 20, sv + 12, 8);  // uv (vec2) — note uv starts at
                                            // byte 12 in RenderVertexInfo
                                            // (vec3 pos = 12B, no padding).
    }

    auto pushVert = [&](int idx) {
        const size_t off = b.verts.size();
        b.verts.resize(off + kOutStride);
        std::memcpy(b.verts.data() + off, xformed.data() + size_t(idx) * kOutStride, kOutStride);
        ++b.vertCount;
    };
    const int triCount = count - 2;
    if (topologyEnum == 0) {                 // TRIANGLE_STRIP
        for (int i = 0; i < triCount; ++i) {
            if ((i & 1) == 0) { pushVert(i);     pushVert(i + 1); pushVert(i + 2); }
            else              { pushVert(i + 1); pushVert(i);     pushVert(i + 2); }
        }
    } else if (topologyEnum == 1) {          // TRIANGLE_FAN
        for (int i = 0; i < triCount; ++i) {
            pushVert(0); pushVert(i + 1); pushVert(i + 2);
        }
    } else {                                  // TRIANGLE_LIST passthrough
        const int triCountList = (count / 3) * 3;
        for (int i = 0; i < triCountList; ++i) pushVert(i);
    }
}

void RendererVulkan::FlushBatch() {
    PendingBatch& b = pendingBatch_;
    if (!b.active || b.vertCount <= 0) {
        // Nothing pending; clear flag defensively (e.g. after a frame reset).
        b.active = false;
        b.verts.clear();
        b.vertCount = 0;
        return;
    }
    if (!initialized_ || !frameStarted_ || !inRenderPass_) {
        // Render pass closed under us; abandon the batch silently. Should
        // only happen if the caller misused the API (e.g. submitted draws
        // outside BeginFrame/EndFrame).
        b.active = false;
        b.verts.clear();
        b.vertCount = 0;
        return;
    }

    auto& frame = frames_->current();

    // 1. Build L2 key — always TRIANGLE_LIST for batched output.
    vk::VkPipelineKey key{};
    key.vertexLayout      = static_cast<vk::VertexLayout>(b.vertexLayoutEnum);
    key.topology          = vk::Topology::TriangleList;
    key.blendMode         = (b.blendMode == 0) ? vk::BlendMode::Alpha : vk::BlendMode::Add;
    key.colorOp           = (b.colorOp   == 0) ? vk::ColorOp::Modulate : vk::ColorOp::Add;
    key.depthFunc         = b.depthFuncAlways ? vk::DepthFunc::Always : vk::DepthFunc::LessEqual;
    key.hasTexture        = b.hasTexture ? 1 : 0;
    key.depthTestEnable   = b.depthTest  ? 1 : 0;
    key.depthWriteEnable  = b.zWriteDisable ? 0 : 1;

    VkPipeline pipeline = pipelineCache_->GetOrCreate(*ctx_, key);
    if (pipeline == VK_NULL_HANDLE) {
        b.active = false; b.verts.clear(); b.vertCount = 0;
        return;
    }

    // 2. Upload all batched verts in one shot.
    void*        mapped = nullptr;
    VkBuffer     vbBuf  = VK_NULL_HANDLE;
    VkDeviceSize vbOff  = 0;
    VkDeviceSize bytes  = VkDeviceSize(b.vertexStride) * VkDeviceSize(b.vertCount);
    if (!uploadHeap_->AllocVerts(bytes, &mapped, &vbBuf, &vbOff)) {
        b.active = false; b.verts.clear(); b.vertCount = 0;
        return;
    }
    std::memcpy(mapped, b.verts.data(), bytes);

    // 3. Bind pipeline + (optional) descriptor set.
    vkCmdBindPipeline(frame.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    if (b.hasTexture) {
        // Linear filtering for the 3D scene path (layouts 3, 4) and the
        // pretransformed mirror of layout 3 (layout 5).
        const bool useLinearSampler = (b.vertexLayoutEnum == 3 ||
                                       b.vertexLayoutEnum == 4 ||
                                       b.vertexLayoutEnum == 5);
        VkDescriptorSet ds = VK_NULL_HANDLE;
        if (!b.textureStageDiffuseOnly && textureMgr_) {
            ds = textureMgr_->GetDescriptorSet(b.texture, useLinearSampler);
        }
        if (ds == VK_NULL_HANDLE) ds = defaultTex_->descriptorSet();
        vkCmdBindDescriptorSets(frame.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                pipelineLayoutTex_, 0, 1, &ds, 0, nullptr);
    }

    // 4. Push constants.
    PushConstants pc{};
    pc.invScreen[0] = (b.screenWidth  > 0) ? 1.0f / float(b.screenWidth)  : 0.0f;
    pc.invScreen[1] = (b.screenHeight > 0) ? 1.0f / float(b.screenHeight) : 0.0f;
    std::memcpy(pc.mvp, b.mvp, sizeof(pc.mvp));
    const bool fogOn = b.depthTest && (b.fogEnabled != 0);
    pc.fogColor[0]  = fogOn ? D3DCOLOR_R(b.fogColor) / 255.0f : 0.0f;
    pc.fogColor[1]  = fogOn ? D3DCOLOR_G(b.fogColor) / 255.0f : 0.0f;
    pc.fogColor[2]  = fogOn ? D3DCOLOR_B(b.fogColor) / 255.0f : 0.0f;
    pc.fogColor[3]  = fogOn ? D3DCOLOR_A(b.fogColor) / 255.0f : 0.0f;
    pc.fogParams[0] = fogOn ? b.fogStart : 0.0f;
    pc.fogParams[1] = fogOn ? b.fogEnd   : 1.0f;
    pc.fogParams[2] = fogOn ? 1.0f       : 0.0f;
    pc.fogParams[3] = 0.0f;
    pc.textureFactor[0] = D3DCOLOR_R(b.textureFactor) / 255.0f;
    pc.textureFactor[1] = D3DCOLOR_G(b.textureFactor) / 255.0f;
    pc.textureFactor[2] = D3DCOLOR_B(b.textureFactor) / 255.0f;
    pc.textureFactor[3] = D3DCOLOR_A(b.textureFactor) / 255.0f;
    VkPipelineLayout layout = b.hasTexture ? pipelineLayoutTex_ : pipelineLayoutNoTex_;
    vkCmdPushConstants(frame.cmd, layout,
                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                       0, sizeof(PushConstants), &pc);

    // 5. 2D viewport / scissor handling — identical math to the original
    //    drawCommon path, just done once per batch.
    const bool is2DLayout = (b.vertexLayoutEnum == 0 || b.vertexLayoutEnum == 1 || b.vertexLayoutEnum == 2);
    if (is2DLayout && b.viewportW > 0 && b.viewportH > 0) {
        VkExtent2D extL = renderTarget_->extent();
        const float gW = float(b.screenWidth  > 0 ? b.screenWidth  : int(extL.width));
        const float gH = float(b.screenHeight > 0 ? b.screenHeight : int(extL.height));
        const float sc = std::min(float(extL.width) / gW, float(extL.height) / gH);
        const float sW = gW * sc;
        const float sH = gH * sc;
        const float oX = (float(extL.width)  - sW) * 0.5f;
        const float oY = (float(extL.height) - sH) * 0.5f;
        VkViewport vp2 = {};
        vp2.x = oX; vp2.y = oY; vp2.width = sW; vp2.height = sH;
        vp2.minDepth = b.viewportMinZ; vp2.maxDepth = b.viewportMaxZ;
        const float gx0 = oX + float(b.viewportX) * sc;
        const float gy0 = oY + float(b.viewportY) * sc;
        const float gx1 = gx0 + float(b.viewportW) * sc;
        const float gy1 = gy0 + float(b.viewportH) * sc;
        const int32_t ix0 = int32_t(std::lround(gx0));
        const int32_t iy0 = int32_t(std::lround(gy0));
        const int32_t ix1 = int32_t(std::lround(gx1));
        const int32_t iy1 = int32_t(std::lround(gy1));
        VkRect2D sc2 = {};
        sc2.offset.x      = ix0;
        sc2.offset.y      = iy0;
        sc2.extent.width  = uint32_t(ix1 - ix0);
        sc2.extent.height = uint32_t(iy1 - iy0);
        vkCmdSetViewport(frame.cmd, 0, 1, &vp2);
        vkCmdSetScissor (frame.cmd, 0, 1, &sc2);
    }

    // 6. Bind vertex buffer + draw the entire batch in one call.
    vkCmdBindVertexBuffers(frame.cmd, 0, 1, &vbBuf, &vbOff);
    vkCmdDraw(frame.cmd, uint32_t(b.vertCount), 1, 0, 0);

    // 7. Restore game-logical viewport for any subsequent 3D draws (matches
    //    the original drawCommon's 2D-restore step).
    if (is2DLayout && b.viewportW > 0 && b.viewportH > 0) {
        VkExtent2D extR = renderTarget_->extent();
        const float gW = float(b.screenWidth  > 0 ? b.screenWidth  : int(extR.width));
        const float gH = float(b.screenHeight > 0 ? b.screenHeight : int(extR.height));
        const float sc = std::min(float(extR.width) / gW, float(extR.height) / gH);
        const float sW = gW * sc;
        const float sH = gH * sc;
        const float oX = (float(extR.width)  - sW) * 0.5f;
        const float oY = (float(extR.height) - sH) * 0.5f;
        VkViewport vp3 = {};
        vp3.x = oX + float(b.viewportX) * sc;
        vp3.y = oY + float(b.viewportY) * sc;
        vp3.width  = float(b.viewportW) * sc;
        vp3.height = float(b.viewportH) * sc;
        vp3.minDepth = b.viewportMinZ; vp3.maxDepth = b.viewportMaxZ;
        vkCmdSetViewport(frame.cmd, 0, 1, &vp3);
        const int32_t rx0 = int32_t(std::lround(vp3.x));
        const int32_t ry0 = int32_t(std::lround(vp3.y));
        const int32_t rx1 = int32_t(std::lround(vp3.x + vp3.width));
        const int32_t ry1 = int32_t(std::lround(vp3.y + vp3.height));
        VkRect2D sc3 = {};
        sc3.offset.x      = rx0;
        sc3.offset.y      = ry0;
        sc3.extent.width  = uint32_t(rx1 - rx0);
        sc3.extent.height = uint32_t(ry1 - ry0);
        vkCmdSetScissor(frame.cmd, 0, 1, &sc3);
    }

    statsVertsOut_   += uint32_t(b.vertCount);
    ++statsBatchFlush_;

    // Reset for next batch.
    b.active   = false;
    b.verts.clear();
    b.vertCount = 0;
    b.mergedDraws = 0;
}

bool RendererVulkan::drawCommon(int vertexLayoutEnum,
                                int topologyEnum,
                                bool hasTexture,
                                bool depthTest,
                                const void* verts,
                                int count,
                                uint32_t vertexStride) {
    if (!initialized_ || !frameStarted_ || !inRenderPass_ || count <= 0) return false;
    if (mvpDirty_) recomputeMvp();

    ++statsInputDraws_;

    // Phase 6.2/6.3: layouts 3 (Tex1DiffuseXyz, sprite) and 4
    // (RenderVertexInfoXyz, 3D scene mesh) get remapped to layout 5
    // (Tex1DiffuseXyzClip) with mvp pre-multiplied into pos on the CPU.
    // Layout 4 additionally folds textureFactor into per-vertex color.
    // The layout-5 shader ignores pc.mvp AND reads its color from input,
    // so adjacent draws with different SetWorldTransform / SetTextureFactor
    // values share one batch. Phase 6.2 telemetry attributed L4 = 97.3% of
    // all flushes (Mvp 82% + TexFactor 16%); this redirect kills both.
    const bool useCpuMvp3 = (vertexLayoutEnum == 3);
    const bool useCpuMvp4 = (vertexLayoutEnum == 4);
    const bool useCpuMvp  = useCpuMvp3 || useCpuMvp4;
    const int  effLayout = useCpuMvp ? 5 : vertexLayoutEnum;
    const uint32_t effStride = useCpuMvp ? uint32_t(28) /* vec4+u32+vec2 */
                                          : vertexStride;

    if (batchStateMatches(effLayout, hasTexture, depthTest)) {
        if      (useCpuMvp3) appendAsTriangleListPretransform3(topologyEnum, verts, count);
        else if (useCpuMvp4) appendAsTriangleListPretransform4(topologyEnum, verts, count);
        else                  appendAsTriangleList(topologyEnum, verts, count, vertexStride);
        ++pendingBatch_.mergedDraws;
        return true;
    }
    // Mismatch: bump histogram with the captured reason then flush+restart.
    if (lastMismatchReason_ < FR_COUNT) {
        bumpFlushReason(lastMismatchReason_);
    }
    if (pendingBatch_.active) FlushBatch();
    beginPendingBatch(effLayout, hasTexture, depthTest, effStride);
    if      (useCpuMvp3) appendAsTriangleListPretransform3(topologyEnum, verts, count);
    else if (useCpuMvp4) appendAsTriangleListPretransform4(topologyEnum, verts, count);
    else                  appendAsTriangleList(topologyEnum, verts, count, vertexStride);
    return true;
}

// --- Draw method dispatch ---
// Vertex layout enum / topology enum integer values match VkPipelineKey enums:
//   VertexLayout: 0=DiffuseXyzrwh, 1=Tex1Xyzrwh, 2=Tex1DiffuseXyzrwh,
//                 3=Tex1DiffuseXyz, 4=RenderVertexInfoXyz,
//                 5=Tex1DiffuseXyzClip (batcher-internal CPU-pretransformed
//                 mirror of layout 3; never used as a drawCommon input layout)
//   Topology:     0=TriangleStrip, 1=TriangleFan, 2=TriangleList (batcher output only)
void RendererVulkan::DrawTriangleStrip(const VertexDiffuseXyzrwh *verts, i32 count) {
    drawCommon(0, 0, /*hasTex*/false, /*depth*/false, verts, count, sizeof(VertexDiffuseXyzrwh));
}
void RendererVulkan::DrawTriangleStripTex(const VertexTex1Xyzrwh *verts, i32 count) {
    drawCommon(1, 0, /*hasTex*/true, /*depth*/false, verts, count, sizeof(VertexTex1Xyzrwh));
}
void RendererVulkan::DrawTriangleStripTextured(const VertexTex1DiffuseXyzrwh *verts, i32 count) {
    drawCommon(2, 0, /*hasTex*/true, /*depth*/false, verts, count, sizeof(VertexTex1DiffuseXyzrwh));
}
void RendererVulkan::DrawTriangleStripTextured3D(const VertexTex1DiffuseXyz *verts, i32 count) {
    drawCommon(3, 0, /*hasTex*/true, /*depth*/true, verts, count, sizeof(VertexTex1DiffuseXyz));
}
void RendererVulkan::DrawTriangleFanTextured(const VertexTex1DiffuseXyzrwh *verts, i32 count) {
    drawCommon(2, 1, /*hasTex*/true, /*depth*/false, verts, count, sizeof(VertexTex1DiffuseXyzrwh));
}
void RendererVulkan::DrawTriangleFanTextured3D(const VertexTex1DiffuseXyz *verts, i32 count) {
    drawCommon(3, 1, /*hasTex*/true, /*depth*/true, verts, count, sizeof(VertexTex1DiffuseXyz));
}
void RendererVulkan::DrawVertexBuffer3D(const RenderVertexInfo *verts, i32 count) {
    drawCommon(4, 0, /*hasTex*/true, /*depth*/true, verts, count, sizeof(RenderVertexInfo));
}

// --- Texture / surface (Phase 3 — real tex API; Phase 4 — surface ops + screenshot) ---

// File-internal helper used by Phase 3 stress + Phase 4 CopyAlphaChannel/TakeScreenshot.
// Reads the full RGBA contents of `img` (w x h, format must be R8G8B8A8_UNORM-compatible)
// into `dst`. `srcLayout` is the image's current layout (so the first barrier transitions
// from it to TRANSFER_SRC_OPTIMAL); `restoreLayout` is what to leave it in afterward.
// Synchronous (vkQueueWaitIdle).
static bool RV_ReadbackImageRgba(vk::VkContext& ctx, VkImage img, int w, int h,
                                 uint8_t* dst, size_t dstBytes,
                                 VkImageLayout srcLayout,
                                 VkImageLayout restoreLayout);

u32  RendererVulkan::CreateTextureFromMemory(const u8 *data, i32 dataLen, D3DCOLOR colorKey,
                                             i32 *outWidth, i32 *outHeight) {
    if (!initialized_ || !textureMgr_ || !data || dataLen <= 0) {
        if (outWidth)  *outWidth  = 0;
        if (outHeight) *outHeight = 0;
        return 0;
    }
    SDL_RWops *rw = SDL_RWFromConstMem(data, dataLen);
    if (!rw) return 0;
    SDL_Surface *surface = IMG_Load_RW(rw, 1);
    if (!surface) {
        std::fprintf(stderr, "[VK] CreateTextureFromMemory: IMG_Load_RW -> %s\n", IMG_GetError());
        return 0;
    }
    SDL_Surface *rgba = SDL_ConvertSurfaceFormat(surface, SDL_PIXELFORMAT_ABGR8888, 0);
    SDL_FreeSurface(surface);
    if (!rgba) return 0;
    {
        static int seen2 = 0;
        if (seen2 < 8) {
            std::fprintf(stderr, "[Vk LoadTex] w=%d h=%d pitch=%d ck=0x%x fmt=0x%x bpp=%d\n",
                rgba->w, rgba->h, rgba->pitch, (unsigned)colorKey,
                rgba->format ? rgba->format->format : 0u,
                rgba->format ? rgba->format->BitsPerPixel : 0);
            ++seen2;
        }
    }

    // Apply color key (D3D8 semantics: matching pixels become alpha=0; others alpha=255).
    if (colorKey != 0) {
        const u8 ckR = u8((colorKey >> 16) & 0xFF);
        const u8 ckG = u8((colorKey >>  8) & 0xFF);
        const u8 ckB = u8((colorKey      ) & 0xFF);
        SDL_LockSurface(rgba);
        u8 *pixels = static_cast<u8*>(rgba->pixels);
        // ABGR8888 layout: byte order is R,G,B,A in memory. Honor row pitch since
        // SDL surfaces may be padded for SIMD alignment (pitch >= w*4).
        for (i32 y = 0; y < rgba->h; ++y) {
            u8* row = pixels + size_t(y) * size_t(rgba->pitch);
            for (i32 x = 0; x < rgba->w; ++x) {
                u8 *p = row + x * 4;
                if (p[0] == ckR && p[1] == ckG && p[2] == ckB) p[3] = 0;
                else                                            p[3] = 255;
            }
        }
        SDL_UnlockSurface(rgba);
    }

    SDL_LockSurface(rgba);
    // VkTextureManager expects tightly packed rows (bufferRowLength=0). Repack if SDL
    // gave us padded rows.
    u32 id = 0;
    const int rowBytes = rgba->w * 4;
    if (rgba->pitch == rowBytes) {
        id = textureMgr_->CreateFromRgba(*ctx_, rgba->w, rgba->h,
                                         static_cast<const u8*>(rgba->pixels));
    } else {
        std::vector<u8> packed(size_t(rowBytes) * size_t(rgba->h));
        const u8* src = static_cast<const u8*>(rgba->pixels);
        for (i32 y = 0; y < rgba->h; ++y) {
            std::memcpy(packed.data() + size_t(y) * rowBytes,
                        src + size_t(y) * size_t(rgba->pitch),
                        rowBytes);
        }
        id = textureMgr_->CreateFromRgba(*ctx_, rgba->w, rgba->h, packed.data());
    }
    SDL_UnlockSurface(rgba);

    if (outWidth)  *outWidth  = rgba->w;
    if (outHeight) *outHeight = rgba->h;
    SDL_FreeSurface(rgba);
    return id;
}

u32  RendererVulkan::CreateEmptyTexture(i32 width, i32 height) {
    if (!initialized_ || !textureMgr_) return 0;
    return textureMgr_->CreateEmpty(*ctx_, width, height);
}

void RendererVulkan::DeleteTexture(u32 tex) {
    if (!initialized_ || !textureMgr_ || tex == 0) return;
    textureMgr_->Delete(*ctx_, tex);
}

void RendererVulkan::CopyAlphaChannel(u32 dstTex, const u8 *srcData, i32 dataLen,
                                      i32 width, i32 height) {
    if (!initialized_ || !textureMgr_ || dstTex == 0 || !srcData || dataLen <= 0) return;

    int dstW = 0, dstH = 0;
    if (!textureMgr_->GetSize(dstTex, &dstW, &dstH)) return;
    if (width  <= 0) width  = dstW;
    if (height <= 0) height = dstH;
    if (dstW <= 0 || dstH <= 0) return;

    // 1. Decode source via SDL_image to ABGR8888 (memory order R,G,B,A — matches Vulkan
    //    R8G8B8A8_UNORM exactly, same convention as CreateTextureFromMemory).
    SDL_RWops* rw = SDL_RWFromConstMem(srcData, dataLen);
    if (!rw) return;
    SDL_Surface* src = IMG_Load_RW(rw, 1);
    if (!src) return;
    SDL_Surface* srcRgba = SDL_ConvertSurfaceFormat(src, SDL_PIXELFORMAT_ABGR8888, 0);
    SDL_FreeSurface(src);
    if (!srcRgba) return;

    // 2. Read back current dst contents (full image).
    VkImage dstImg = textureMgr_->GetImage(dstTex);
    const size_t dstBytes = size_t(dstW) * size_t(dstH) * 4u;
    std::vector<uint8_t> dstPixels(dstBytes);
    if (!RV_ReadbackImageRgba(*ctx_, dstImg, dstW, dstH,
                              dstPixels.data(), dstBytes,
                              VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                              VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)) {
        SDL_FreeSurface(srcRgba);
        return;
    }

    // 3. Copy src.B -> dst.A (matches RendererGL semantics; SDL ABGR8888 byte order = RGBA so
    //    src[x*4+0] = R; we want B = src[x*4+2]).
    const int copyW = std::min(srcRgba->w, width);
    const int copyH = std::min(srcRgba->h, height);
    SDL_LockSurface(srcRgba);
    for (int y = 0; y < copyH; ++y) {
        const uint8_t* sp = (const uint8_t*)srcRgba->pixels + size_t(y) * srcRgba->pitch;
        uint8_t*       dp = dstPixels.data() + size_t(y) * size_t(width) * 4u;
        for (int x = 0; x < copyW; ++x) {
            dp[x * 4 + 3] = sp[x * 4 + 2];  // B -> A
        }
    }
    SDL_UnlockSurface(srcRgba);
    SDL_FreeSurface(srcRgba);

    // 4. Write back full image.
    textureMgr_->UpdateSubImage(*ctx_, dstTex, 0, 0, dstW, dstH, dstPixels.data());
}

void RendererVulkan::UpdateTextureSubImage(u32 tex, i32 x, i32 y, i32 w, i32 h,
                                           const u8 *rgbaPixels) {
    if (!initialized_ || !textureMgr_ || tex == 0) return;
    textureMgr_->UpdateSubImage(*ctx_, tex, x, y, w, h, rgbaPixels);
}

// --- Phase 4: Surface load (alias to texture create — surfaces and textures share backing) ---

u32 RendererVulkan::LoadSurfaceFromFile(const u8 *data, i32 dataLen, i32 *outW, i32 *outH) {
    // Same as CreateTextureFromMemory minus the colorKey alpha-mask pass.
    if (outW) *outW = 0;
    if (outH) *outH = 0;
    if (!initialized_ || !textureMgr_ || !data || dataLen <= 0) return 0;
    SDL_RWops* rw = SDL_RWFromConstMem(data, dataLen);
    if (!rw) return 0;
    SDL_Surface* surf = IMG_Load_RW(rw, 1);
    if (!surf) return 0;
    SDL_Surface* rgba = SDL_ConvertSurfaceFormat(surf, SDL_PIXELFORMAT_ABGR8888, 0);
    SDL_FreeSurface(surf);
    if (!rgba) return 0;
    {
        static int seen = 0;
        if (seen < 8) {
            const u8* px = (const u8*)rgba->pixels;
            std::fprintf(stderr, "[Vk LoadSurface] w=%d h=%d pitch=%d fmt=0x%x bpp=%d px[0..16]=%02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x\n",
                rgba->w, rgba->h, rgba->pitch,
                rgba->format ? rgba->format->format : 0u,
                rgba->format ? rgba->format->BitsPerPixel : 0,
                px[0],px[1],px[2],px[3], px[4],px[5],px[6],px[7],
                px[8],px[9],px[10],px[11], px[12],px[13],px[14],px[15]);
            ++seen;
        }
    }
    SDL_LockSurface(rgba);
    u32 id = 0;
    const int rowBytes = rgba->w * 4;
    if (rgba->pitch == rowBytes) {
        id = textureMgr_->CreateFromRgba(*ctx_, rgba->w, rgba->h,
                                         (const uint8_t*)rgba->pixels);
    } else {
        // SDL surface row pitch may exceed w*4 (alignment padding). VkTextureManager
        // assumes tightly packed source — repack to avoid horizontal-shear artifacts
        // (each row progressively offset, producing visible "barcode" stripes).
        std::vector<uint8_t> packed(size_t(rowBytes) * size_t(rgba->h));
        const uint8_t* src = static_cast<const uint8_t*>(rgba->pixels);
        for (i32 y = 0; y < rgba->h; ++y) {
            std::memcpy(packed.data() + size_t(y) * rowBytes,
                        src + size_t(y) * size_t(rgba->pitch),
                        rowBytes);
        }
        id = textureMgr_->CreateFromRgba(*ctx_, rgba->w, rgba->h, packed.data());
    }
    SDL_UnlockSurface(rgba);
    if (id != 0) {
        if (outW) *outW = rgba->w;
        if (outH) *outH = rgba->h;
    }
    SDL_FreeSurface(rgba);
    return id;
}

u32 RendererVulkan::LoadSurfaceFromFile(const u8 *data, i32 dataLen, D3DXIMAGE_INFO *info) {
    i32 w = 0, h = 0;
    u32 id = LoadSurfaceFromFile(data, dataLen, &w, &h);
    if (info) {
        info->Width  = u32(w);
        info->Height = u32(h);
    }
    return id;
}

// --- Phase 4: Surface blit to backbuffer (textured triangle strip with BLEND_ALPHA) ---

void RendererVulkan::CopySurfaceToScreen(u32 surfaceTex, i32 srcX, i32 srcY,
                                         i32 dstX, i32 dstY, i32 w, i32 h,
                                         i32 texW, i32 texH) {
    if (!initialized_ || !textureMgr_ || surfaceTex == 0) return;
    if (texW <= 0 || texH <= 0) return;
    const i32 drawW = (w > 0) ? w : texW;
    const i32 drawH = (h > 0) ? h : texH;
    const f32 u0 = f32(srcX) / f32(texW);
    const f32 v0 = f32(srcY) / f32(texH);
    const f32 u1 = f32(srcX + drawW) / f32(texW);
    const f32 v1 = f32(srcY + drawH) / f32(texH);

    // Save / set / restore state. Equivalent to GL's BLEND_MODE_ALPHA + colorOp 0 + tex bind.
    const u8  prevBlend = currentBlendMode;
    const u8  prevOp    = currentColorOp;
    const u32 prevTex   = currentTexture;
    SetBlendMode(0);   // 0 = Alpha (ADR-007)
    SetColorOp(0);     // 0 = Modulate
    SetTexture(surfaceTex);

    VertexTex1DiffuseXyzrwh quad[4]{};
    auto setV = [&](int i, f32 x, f32 y, f32 u, f32 v) {
        quad[i].position.x   = x;       quad[i].position.y   = y;
        quad[i].position.z   = 0.0f;    quad[i].position.w   = 1.0f;
        quad[i].diffuse      = 0xFFFFFFFFu;
        quad[i].textureUV.x  = u;       quad[i].textureUV.y  = v;
    };
    setV(0, f32(dstX),         f32(dstY),         u0, v0);
    setV(1, f32(dstX + drawW), f32(dstY),         u1, v0);
    setV(2, f32(dstX),         f32(dstY + drawH), u0, v1);
    setV(3, f32(dstX + drawW), f32(dstY + drawH), u1, v1);
    DrawTriangleStripTextured(quad, 4);

    SetTexture(prevTex);
    SetColorOp(prevOp);
    SetBlendMode(prevBlend);
}

void RendererVulkan::CopySurfaceToScreen(u32 surfaceTex, i32 texW, i32 texH,
                                         i32 dstX, i32 dstY) {
    CopySurfaceToScreen(surfaceTex, 0, 0, dstX, dstY, texW, texH, texW, texH);
}

void RendererVulkan::CopySurfaceRectToScreen(u32 surfaceTex, i32 srcX, i32 srcY,
                                             i32 srcW, i32 srcH,
                                             i32 dstX, i32 dstY,
                                             i32 texW, i32 texH) {
    // GL semantics: drawW == srcW, drawH == srcH.
    CopySurfaceToScreen(surfaceTex, srcX, srcY, dstX, dstY, srcW, srcH, texW, texH);
}

// --- Phase 4: Screenshot (swapchain image -> RGBA, BGRA->RGBA swap, into dst tex) ---
//
// Timing assumption (Phase 4): callable BETWEEN frames (after EndFrame, before next
// BeginFrame). When integrated into the live game loop in Phase 5, the GameWindow caller
// site sits inside the active render pass — at that point this function will need to
// suspend/resume the render pass. For the standalone smoketest the between-frames path
// is sufficient and exercises the readback machinery.
void RendererVulkan::TakeScreenshot(u32 dstTex, i32 left, i32 top, i32 width, i32 height) {
    if (!initialized_ || !textureMgr_ || dstTex == 0 || !ctx_ || !swap_) return;
    if (width <= 0 || height <= 0) return;

    int dstW = 0, dstH = 0;
    if (!textureMgr_->GetSize(dstTex, &dstW, &dstH)) return;
    if (width > dstW)  width  = dstW;
    if (height > dstH) height = dstH;

    if (frameStarted_ || inRenderPass_) {
        std::fprintf(stderr, "[VK] TakeScreenshot called mid-frame — Phase 5 will handle suspend/resume; skipping.\n");
        return;
    }

    // Make sure the previous present is fully done (sledgehammer: vkQueueWaitIdle).
    vkQueueWaitIdle(ctx_->graphicsQueue());

    const VkExtent2D extent = swap_->extent();
    if (uint32_t(left + width)  > extent.width || left < 0) return;
    if (uint32_t(top  + height) > extent.height || top  < 0) return;

    // Read full screenshot region from the most-recently-presented swap image.
    // After present, layout is PRESENT_SRC_KHR. Restore to PRESENT_SRC_KHR after read.
    const size_t bytes = size_t(width) * size_t(height) * 4u;
    std::vector<uint8_t> bgra(bytes);
    VkImage swapImg = swap_->image(currentSwapImage_);
    if (!swapImg) return;

    // ReadbackImageRgba reads the FULL image. We want only a subregion. Workaround: read
    // a temporary image-sized buffer, then crop. Acceptable for screenshot path (rare,
    // non-hot). Alternative: parameterize ReadbackImageRgba with a region — Phase 5+.
    std::vector<uint8_t> full(size_t(extent.width) * size_t(extent.height) * 4u);
    if (!RV_ReadbackImageRgba(*ctx_, swapImg,
                              int(extent.width), int(extent.height),
                              full.data(), full.size(),
                              VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                              VK_IMAGE_LAYOUT_PRESENT_SRC_KHR)) {
        return;
    }

    // Crop + BGRA → RGBA swap (swapchain format=44 = VK_FORMAT_B8G8R8A8_UNORM).
    std::vector<uint8_t> rgba(bytes);
    for (int y = 0; y < height; ++y) {
        const uint8_t* srcRow = full.data() + (size_t(top + y) * extent.width + left) * 4u;
        uint8_t*       dstRow = rgba.data() + size_t(y) * size_t(width) * 4u;
        for (int x = 0; x < width; ++x) {
            dstRow[x * 4 + 0] = srcRow[x * 4 + 2];  // R <- B
            dstRow[x * 4 + 1] = srcRow[x * 4 + 1];  // G
            dstRow[x * 4 + 2] = srcRow[x * 4 + 0];  // B <- R
            dstRow[x * 4 + 3] = srcRow[x * 4 + 3];  // A
        }
    }

    // Write into dst tex via UpdateSubImage (covers full dst if width/height match).
    textureMgr_->UpdateSubImage(*ctx_, dstTex, 0, 0, width, height, rgba.data());
}

// ============================================================================
// File-internal helpers (used by Phase 3 stress + Phase 4 surface ops).
// ============================================================================

// Read back a texture's full RGBA contents into `dst` (size = w*h*4 bytes).
// Issues a one-shot cmd buffer: srcLayout → TRANSFER_SRC_OPTIMAL,
// vkCmdCopyImageToBuffer into a transient HOST_VISIBLE staging buffer, then
// transitions back to restoreLayout. Synchronous (vkQueueWaitIdle).
static bool RV_ReadbackImageRgba(vk::VkContext& ctx, VkImage image, int w, int h,
                                 uint8_t* dst, size_t dstBytes,
                                 VkImageLayout srcLayout,
                                 VkImageLayout restoreLayout)
{
    if (!image || w <= 0 || h <= 0) return false;
    const VkDeviceSize bytes = VkDeviceSize(w) * VkDeviceSize(h) * 4u;
    if (dstBytes < bytes) return false;

    VkCommandPool pool = VK_NULL_HANDLE;
    {
        VkCommandPoolCreateInfo cpci = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
        cpci.queueFamilyIndex = ctx.graphicsQueueFamily();
        cpci.flags            = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        if (vkCreateCommandPool(ctx.device(), &cpci, nullptr, &pool) != VK_SUCCESS) return false;
    }
    VkCommandBuffer cb = VK_NULL_HANDLE;
    {
        VkCommandBufferAllocateInfo cbai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
        cbai.commandPool        = pool;
        cbai.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cbai.commandBufferCount = 1;
        if (vkAllocateCommandBuffers(ctx.device(), &cbai, &cb) != VK_SUCCESS) {
            vkDestroyCommandPool(ctx.device(), pool, nullptr); return false;
        }
    }

    VkBuffer      staging      = VK_NULL_HANDLE;
    VmaAllocation stagingAlloc = VK_NULL_HANDLE;
    VmaAllocationInfo stagingInfo{};
    {
        VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        bci.size  = bytes;
        bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VmaAllocationCreateInfo aci{};
        aci.usage = VMA_MEMORY_USAGE_AUTO;
        aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT
                  | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        if (vmaCreateBuffer(ctx.allocator(), &bci, &aci,
                            &staging, &stagingAlloc, &stagingInfo) != VK_SUCCESS) {
            vkDestroyCommandPool(ctx.device(), pool, nullptr); return false;
        }
    }

    VkCommandBufferBeginInfo bbi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    bbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cb, &bbi);

    VkImageMemoryBarrier toSrc = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    toSrc.oldLayout           = srcLayout;
    toSrc.newLayout           = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    toSrc.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toSrc.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toSrc.image               = image;
    toSrc.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    toSrc.subresourceRange.levelCount = 1;
    toSrc.subresourceRange.layerCount = 1;
    toSrc.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_SHADER_READ_BIT
                        | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    toSrc.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(cb,
                         VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &toSrc);

    VkBufferImageCopy region = {};
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageExtent = { uint32_t(w), uint32_t(h), 1 };
    vkCmdCopyImageToBuffer(cb, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           staging, 1, &region);

    VkImageMemoryBarrier toRestore = toSrc;
    toRestore.oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    toRestore.newLayout     = restoreLayout;
    toRestore.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    toRestore.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cb,
                         VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &toRestore);

    vkEndCommandBuffer(cb);
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.commandBufferCount = 1;
    si.pCommandBuffers    = &cb;
    vkQueueSubmit(ctx.graphicsQueue(), 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle(ctx.graphicsQueue());

    vmaInvalidateAllocation(ctx.allocator(), stagingAlloc, 0, VK_WHOLE_SIZE);
    std::memcpy(dst, stagingInfo.pMappedData, size_t(bytes));

    vmaDestroyBuffer(ctx.allocator(), staging, stagingAlloc);
    vkDestroyCommandPool(ctx.device(), pool, nullptr);  // frees cb
    return true;
}

int RendererVulkan::Phase3StressTest(int n, std::FILE* log)
{
    auto LOG = [&](const char* fmt, auto... args) {
        if (log) { std::fprintf(log, fmt, args...); std::fflush(log); }
    };
    if (!initialized_ || !textureMgr_) {
        LOG("[stress] FAIL: renderer not initialized\n");
        return 1;
    }
    if (n < 1) n = 100;
    VmaAllocator alloc = ctx_->allocator();

    VmaTotalStatistics stat0{};
    vmaCalculateStatistics(alloc, &stat0);
    const uint32_t base_alloc = stat0.total.statistics.allocationCount;
    const uint32_t base_block = stat0.total.statistics.blockCount;
    LOG("[stress] baseline VMA: blocks=%u allocs=%u bytes=%llu\n",
        base_block, base_alloc, (unsigned long long)stat0.total.statistics.allocationBytes);

    int errors = 0;
    std::vector<uint32_t> ids;        ids.reserve(size_t(n));
    std::vector<std::vector<uint8_t>> patterns; patterns.reserve(size_t(n));

    // Phase A: create N textures (varying sizes) + UpdateSubImage(full) with deterministic pattern.
    static const int sizes[] = {16, 24, 32, 48, 64};
    for (int i = 0; i < n; ++i) {
        const int s = sizes[i % 5];
        std::vector<uint8_t> px(size_t(s) * size_t(s) * 4u);
        for (int y = 0; y < s; ++y) {
            for (int x = 0; x < s; ++x) {
                uint8_t* p = &px[(size_t(y) * s + x) * 4];
                p[0] = uint8_t(i & 0xFF);
                p[1] = uint8_t(x & 0xFF);
                p[2] = uint8_t(y & 0xFF);
                p[3] = 0xFF;
            }
        }
        uint32_t id = textureMgr_->CreateEmpty(*ctx_, s, s);
        if (id == 0) { LOG("[stress] FAIL: CreateEmpty(%d) at i=%d\n", s, i); ++errors; break; }
        if (!textureMgr_->UpdateSubImage(*ctx_, id, 0, 0, s, s, px.data())) {
            LOG("[stress] FAIL: UpdateSubImage at i=%d\n", i); ++errors; break;
        }
        ids.push_back(id);
        patterns.push_back(std::move(px));
    }
    LOG("[stress] created %zu/%d textures\n", ids.size(), n);

    VmaTotalStatistics stat1{};
    vmaCalculateStatistics(alloc, &stat1);
    const int dAlloc = int(stat1.total.statistics.allocationCount) - int(base_alloc);
    const int dBlock = int(stat1.total.statistics.blockCount)      - int(base_block);
    LOG("[stress] after create: blocks=%u (+%d) allocs=%u (+%d) bytes=%llu\n",
        stat1.total.statistics.blockCount, dBlock,
        stat1.total.statistics.allocationCount, dAlloc,
        (unsigned long long)stat1.total.statistics.allocationBytes);

    // Done When: VkDeviceMemory allocations << textures (VMA block coalescing).
    if (dAlloc < n) {
        LOG("[stress] FAIL: expected dAlloc>=%d, got %d\n", n, dAlloc); ++errors;
    }
    if (dBlock >= n) {
        LOG("[stress] FAIL: expected dBlock<%d (block coalescing), got %d\n", n, dBlock); ++errors;
    } else {
        LOG("[stress] OK: VMA block coalescing — %d allocations across +%d block(s)\n", dAlloc, dBlock);
    }

    // Phase B: roundtrip readback verify on first 5 textures (memcmp).
    const int verifyN = (int)std::min(size_t(5), ids.size());
    int verified = 0;
    for (int i = 0; i < verifyN; ++i) {
        VkImage img = textureMgr_->GetImage(ids[i]);
        int w = 0, h = 0; textureMgr_->GetSize(ids[i], &w, &h);
        const size_t bytes = size_t(w) * size_t(h) * 4u;
        std::vector<uint8_t> back(bytes, 0xCD);
        if (!RV_ReadbackImageRgba(*ctx_, img, w, h, back.data(), bytes,
                                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)) {
            LOG("[stress] FAIL: readback i=%d\n", i); ++errors; continue;
        }
        if (std::memcmp(back.data(), patterns[size_t(i)].data(), bytes) != 0) {
            LOG("[stress] FAIL: roundtrip mismatch i=%d (%dx%d)\n", i, w, h); ++errors;
        } else {
            ++verified;
        }
    }
    LOG("[stress] roundtrip verified %d/%d (memcmp == 0)\n", verified, verifyN);

    // Phase C: delete all + leak check.
    for (uint32_t id : ids) textureMgr_->Delete(*ctx_, id);
    ids.clear();
    VmaTotalStatistics stat2{};
    vmaCalculateStatistics(alloc, &stat2);
    LOG("[stress] after delete: blocks=%u allocs=%u bytes=%llu\n",
        stat2.total.statistics.blockCount,
        stat2.total.statistics.allocationCount,
        (unsigned long long)stat2.total.statistics.allocationBytes);
    if (stat2.total.statistics.allocationCount != base_alloc) {
        LOG("[stress] FAIL: leak — allocCount delta=+%d\n",
            int(stat2.total.statistics.allocationCount) - int(base_alloc));
        ++errors;
    } else {
        LOG("[stress] OK: zero VMA leaks (allocCount returned to %u)\n", base_alloc);
    }

    LOG("[stress] DONE errors=%d\n", errors);
    return errors;
}

// ============================================================================
// Phase 5b.2: ImGui (imgui_impl_vulkan) integration.
//
// Lifecycle:
//   InitImGui     — after RendererVulkan::Init() returned successfully.
//   ShutdownImGui — before destroyAll(); called from main / GameWindow teardown.
//   NewFrameImGui — once per frame, before ImGui::NewFrame().
//   RenderImGui   — inside the persistent offscreen render pass scope, called
//                   from EndFrame() between game draws and vkCmdEndRenderPass.
//
// We share the offscreen render pass (LOAD_OP_LOAD, COLOR_ATTACHMENT_OPTIMAL)
// with the game so ImGui composes ON TOP of the game's frame, then the EndFrame
// blit copies the combined image to the swapchain.
// vk_smoketest never links ImGui — gate everything on TH06_HAS_IMGUI; provide
// no-op stubs in the absence so the class remains ABI-compatible.
// ============================================================================

#ifdef TH06_HAS_IMGUI

namespace {
// Volk thunk for ImGui's loader: the renderer uses VK_NO_PROTOTYPES (volk),
// so imgui_impl_vulkan.cpp must resolve every entrypoint via this callback.
// Use vkGetInstanceProcAddr loaded by volk; ImGui internally calls it for both
// instance- and device-level functions (it has its own GetDeviceProcAddr step).
PFN_vkVoidFunction ImGuiVkLoader(const char* name, void* user) {
    auto inst = static_cast<VkInstance>(user);
    return vkGetInstanceProcAddr(inst, name);
}

void ImGuiVkCheck(VkResult r) {
    if (r != VK_SUCCESS) {
        std::fprintf(stderr, "[VK ImGui] VkResult=%d\n", int(r));
    }
}
} // namespace

bool RendererVulkan::InitImGui(SDL_Window* window) {
    std::fprintf(stderr, "[VK ImGui] InitImGui ENTER (initialized_=%d ctx_=%p rt_=%p swap_=%p window=%p imguiInit=%d)\n",
                 (int)initialized_, (void*)ctx_.get(), (void*)renderTarget_.get(),
                 (void*)swap_.get(), (void*)window, (int)imguiInitialized_);
    if (imguiInitialized_) return true;
    if (!initialized_ || !ctx_ || !renderTarget_ || !swap_) {
        std::fprintf(stderr, "[VK ImGui] InitImGui called before renderer init\n");
        return false;
    }

    // 1. SDL2 backend: ImGui_ImplSDL2_InitForVulkan only needs the window.
    std::fprintf(stderr, "[VK ImGui] -> ImGui_ImplSDL2_InitForVulkan\n");
    if (!ImGui_ImplSDL2_InitForVulkan(window)) {
        std::fprintf(stderr, "[VK ImGui] ImGui_ImplSDL2_InitForVulkan failed\n");
        return false;
    }
    std::fprintf(stderr, "[VK ImGui] ImGui_ImplSDL2_InitForVulkan OK\n");

    // 2. Feed Vulkan loader (VK_NO_PROTOTYPES). Pass instance via user param.
    if (!ImGui_ImplVulkan_LoadFunctions(&ImGuiVkLoader, ctx_->instance())) {
        std::fprintf(stderr, "[VK ImGui] ImGui_ImplVulkan_LoadFunctions failed\n");
        ImGui_ImplSDL2_Shutdown();
        return false;
    }

    // 3. Dedicated descriptor pool. ImGui v1.82 only allocates one combined-image
    //    sampler set (the font atlas) by default; budget extras for user images.
    {
        VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 16 };
        VkDescriptorPoolCreateInfo dpci = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
        dpci.flags         = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
        dpci.maxSets       = 16;
        dpci.poolSizeCount = 1;
        dpci.pPoolSizes    = &ps;
        if (vkCreateDescriptorPool(ctx_->device(), &dpci, nullptr, &imguiDescPool_) != VK_SUCCESS) {
            std::fprintf(stderr, "[VK ImGui] vkCreateDescriptorPool failed\n");
            ImGui_ImplSDL2_Shutdown();
            return false;
        }
    }

    // 4. Init the Vulkan backend, sharing our offscreen render pass.
    {
        ImGui_ImplVulkan_InitInfo info = {};
        info.Instance        = ctx_->instance();
        info.PhysicalDevice  = ctx_->physicalDevice();
        info.Device          = ctx_->device();
        info.QueueFamily     = ctx_->graphicsQueueFamily();
        info.Queue           = ctx_->graphicsQueue();
        info.PipelineCache   = VK_NULL_HANDLE;
        info.DescriptorPool  = imguiDescPool_;
        info.Subpass         = 0;
        info.MinImageCount   = 2;
        info.ImageCount      = swap_->imageCount();
        info.MSAASamples     = VK_SAMPLE_COUNT_1_BIT;
        info.Allocator       = nullptr;
        info.CheckVkResultFn = &ImGuiVkCheck;
        if (!ImGui_ImplVulkan_Init(&info, renderTarget_->renderPass())) {
            std::fprintf(stderr, "[VK ImGui] ImGui_ImplVulkan_Init failed\n");
            vkDestroyDescriptorPool(ctx_->device(), imguiDescPool_, nullptr);
            imguiDescPool_ = VK_NULL_HANDLE;
            ImGui_ImplSDL2_Shutdown();
            return false;
        }
    }

    // 5. Font atlas upload is DEFERRED to first NewFrameImGui — thprac calls
    //    AddFontFromFileTTF() AFTER InitImGui returns, so uploading here would
    //    capture only the empty default atlas, leaving glyphs as solid color
    //    blocks. The lazy path mirrors imgui_impl_opengl2's behaviour.

    imguiInitialized_ = true;
    std::fprintf(stderr, "[VK ImGui] initialised (renderpass=0x%llx, imageCount=%u)\n",
                 static_cast<unsigned long long>((uint64_t)renderTarget_->renderPass()),
                 unsigned(swap_->imageCount()));
    return true;
}

namespace {
// One-shot command buffer helper for font upload. Returns true on success.
bool UploadImGuiFontsNow(th06::vk::VkContext* ctx) {
    VkCommandPool tmpPool = VK_NULL_HANDLE;
    VkCommandPoolCreateInfo cpci = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    cpci.flags            = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    cpci.queueFamilyIndex = ctx->graphicsQueueFamily();
    if (vkCreateCommandPool(ctx->device(), &cpci, nullptr, &tmpPool) != VK_SUCCESS)
        return false;
    VkCommandBuffer cb = VK_NULL_HANDLE;
    VkCommandBufferAllocateInfo cbai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    cbai.commandPool        = tmpPool;
    cbai.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = 1;
    vkAllocateCommandBuffers(ctx->device(), &cbai, &cb);
    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cb, &bi);
    ImGui_ImplVulkan_CreateFontsTexture(cb);
    vkEndCommandBuffer(cb);
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.commandBufferCount = 1;
    si.pCommandBuffers    = &cb;
    vkQueueSubmit(ctx->graphicsQueue(), 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle(ctx->graphicsQueue());
    ImGui_ImplVulkan_DestroyFontUploadObjects();
    vkDestroyCommandPool(ctx->device(), tmpPool, nullptr);
    return true;
}
} // namespace

void RendererVulkan::ShutdownImGui() {
    if (!imguiInitialized_) return;
    if (ctx_ && ctx_->device() != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(ctx_->device());
    }
    ImGui_ImplVulkan_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    if (imguiDescPool_ != VK_NULL_HANDLE && ctx_ && ctx_->device() != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(ctx_->device(), imguiDescPool_, nullptr);
    }
    imguiDescPool_    = VK_NULL_HANDLE;
    imguiInitialized_ = false;
}

void RendererVulkan::NewFrameImGui() {
    if (!imguiInitialized_) return;
    // Lazy font atlas upload: thprac registers fonts AFTER InitImGui returns,
    // so the first frame is the earliest moment we know the atlas is final.
    if (!imguiFontsUploaded_) {
        if (UploadImGuiFontsNow(ctx_.get())) {
            imguiFontsUploaded_ = true;
            std::fprintf(stderr, "[VK ImGui] font atlas uploaded (%dx%d)\n",
                         ImGui::GetIO().Fonts->TexWidth,
                         ImGui::GetIO().Fonts->TexHeight);
        } else {
            std::fprintf(stderr, "[VK ImGui] font atlas upload FAILED\n");
        }
    }
    ImGui_ImplVulkan_NewFrame();
}

void RendererVulkan::RenderImGui() {
    if (!imguiInitialized_ || !frameStarted_ || !inRenderPass_) return;
    ImDrawData* dd = ImGui::GetDrawData();
    if (!dd || dd->CmdListsCount == 0) return;
    auto& frame = frames_->current();

    // ImGui logical UI is 640x480 (set by GameGuiBegin). The offscreen FBO is
    // sized to the swapchain (e.g. 1708x1067 in fullscreen). To make ImGui
    // share the GAME's letterbox/pillarbox geometry — i.e. render only inside
    // the 640x480-aspect rectangle, NOT stretched anisotropically across the
    // whole FBO — we inject a uniform letterbox transform into ImDrawData
    // before handing it to imgui_impl_vulkan.
    //
    // imgui_impl_vulkan's vertex shader does:
    //     pos_clip = (pos_logical - DisplayPos) * 2/DisplaySize - 1
    //     viewport = (0, 0, DisplaySize * FramebufferScale)
    //
    // Setting DisplaySize=(fbW/s, fbH/s), FramebufferScale=(s,s),
    // DisplayPos=(-offX/s, -offY/s) where s = min(fbW/640, fbH/480) makes
    // logical (0,0)..(640,480) land inside the centered letterbox rect of the
    // full FBO viewport — same as the game.
    {
        const VkExtent2D ext = renderTarget_->extent();
        const float fbW = float(ext.width);
        const float fbH = float(ext.height);
        const float s   = std::min(fbW / 640.0f, fbH / 480.0f);
        if (s > 0.0f) {
            const float offX = (fbW - 640.0f * s) * 0.5f;
            const float offY = (fbH - 480.0f * s) * 0.5f;
            dd->DisplayPos        = ImVec2(-offX / s, -offY / s);
            dd->DisplaySize       = ImVec2(fbW / s, fbH / s);
            dd->FramebufferScale  = ImVec2(s, s);
        }
    }

    // Set sane viewport+scissor before ImGui's own vkCmdSetViewport call (the
    // impl will overwrite, but Vulkan requires both to be valid even if no
    // pipeline using dynamic state has been bound yet).
    {
        VkExtent2D ext = renderTarget_->extent();
        VkViewport vp = {};
        vp.x = 0.0f; vp.y = 0.0f;
        vp.width  = float(ext.width);
        vp.height = float(ext.height);
        vp.minDepth = 0.0f; vp.maxDepth = 1.0f;
        VkRect2D sc = { {0, 0}, ext };
        vkCmdSetViewport(frame.cmd, 0, 1, &vp);
        vkCmdSetScissor (frame.cmd, 0, 1, &sc);
    }
    ImGui_ImplVulkan_RenderDrawData(dd, frame.cmd);
}

#else  // !TH06_HAS_IMGUI — vk_smoketest stubs (no ImGui linkage).

bool RendererVulkan::InitImGui(SDL_Window*) { return false; }
void RendererVulkan::ShutdownImGui()        {}
void RendererVulkan::NewFrameImGui()        {}
void RendererVulkan::RenderImGui()          {}

#endif // TH06_HAS_IMGUI

// ----------------------------------------------------------------------------
// Phase 5a (ADR-008): static factory to mirror GetRendererGL/GLES.
// ----------------------------------------------------------------------------
static RendererVulkan s_RendererVulkan;
IRenderer *GetRendererVulkan() { return &s_RendererVulkan; }

}  // namespace th06
