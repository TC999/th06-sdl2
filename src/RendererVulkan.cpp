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

#include <SDL_log.h>
#include <SDL_image.h>
#include <SDL_rwops.h>
#include <SDL_surface.h>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>

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
// Total = 8 + 8 + 64 + 16 + 16 = 112 bytes (within Vulkan-guaranteed 128-byte minimum).
struct PushConstants {
    float invScreen[2];   // 1/screenWidth, 1/screenHeight (used by xyzrwh paths)
    float _pad[2];
    float mvp[16];        // column-major in shader; we feed row-major D3D bytes -> implicit transpose
    float fogColor[4];    // RGBA, all 0..1
    float fogParams[4];   // [0]=start, [1]=end, [2]=enabled (1.0 = on, 0.0 = off), [3]=pad
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
    static const char* kVertNames[5] = {
        "v_diffuse_xyzrwh.vert.spv",
        "v_tex1_xyzrwh.vert.spv",
        "v_tex1_diffuse_xyzrwh.vert.spv",
        "v_tex1_diffuse_xyz.vert.spv",
        "v_render_vertex_info.vert.spv",
    };
    const std::string base = std::string(TH06_VK_SHADER_DIR) + "/";
    for (int i = 0; i < 5; ++i) {
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
    realScreenWidth  = w;
    realScreenHeight = h;
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
    sci.vsync           = true;
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
    for (int i = 0; i < 5; ++i) deps.vertModules[i] = vertModules_[i];
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

    auto& frame = frames_->current();
    VkDevice dev = ctx_->device();

    TH_VK_CHECK(vkWaitForFences(dev, 1, &frame.inFlight, VK_TRUE, UINT64_MAX));

    if (swapchainOutOfDate_) {
        vkDeviceWaitIdle(dev);
        renderTarget_->Destroy(*ctx_);
        vk::SwapchainCreateInfo sci{};
        sci.requestedWidth  = uint32_t(screenWidth);
        sci.requestedHeight = uint32_t(screenHeight);
        sci.vsync           = true;
        sci.srgb            = false;
        if (!swap_->Recreate(*ctx_, sci)) return;
        if (!renderTarget_->Create(*ctx_, *swap_)) return;
        swapchainOutOfDate_ = false;
    }

    VkResult acq = swap_->AcquireNext(*ctx_, frame.imgAvail, &currentSwapImage_);
    if (acq == VK_ERROR_OUT_OF_DATE_KHR) { swapchainOutOfDate_ = true; return; }
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

    // Begin render pass. LOAD_OP_CLEAR uses the values we provide here; subsequent in-pass
    // Clear() calls override via vkCmdClearAttachments.
    VkClearValue clearVals[2] = {};
    clearVals[0].color.float32[0] = clearColor_[0];
    clearVals[0].color.float32[1] = clearColor_[1];
    clearVals[0].color.float32[2] = clearColor_[2];
    clearVals[0].color.float32[3] = clearColor_[3];
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

    // Set viewport + scissor (dynamic)
    VkViewport vp = {};
    vp.x        = float(viewportX);
    vp.y        = float(viewportY);
    vp.width    = float(viewportW > 0 ? viewportW : int(ext.width));
    vp.height   = float(viewportH > 0 ? viewportH : int(ext.height));
    vp.minDepth = 0.0f;
    vp.maxDepth = 1.0f;
    VkRect2D sc = {};
    sc.extent   = ext;
    vkCmdSetViewport(frame.cmd, 0, 1, &vp);
    vkCmdSetScissor (frame.cmd, 0, 1, &sc);

    frameStarted_ = true;
}

void RendererVulkan::EndFrame() {
    if (!initialized_ || !frameStarted_) { frameStarted_ = false; return; }

    auto& frame = frames_->current();

    if (inRenderPass_) {
        vkCmdEndRenderPass(frame.cmd);
        inRenderPass_ = false;
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

    VkSwapchainKHR swapHandles[1] = { swap_->handle() };
    VkPresentInfoKHR pi = { VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores    = &frame.renderDone;
    pi.swapchainCount     = 1;
    pi.pSwapchains        = swapHandles;
    pi.pImageIndices      = &currentSwapImage_;
    VkResult pr = vkQueuePresentKHR(ctx_->graphicsQueue(), &pi);
    if (pr == VK_ERROR_OUT_OF_DATE_KHR || pr == VK_SUBOPTIMAL_KHR) {
        swapchainOutOfDate_ = true;
    } else if (pr != VK_SUCCESS) {
        std::fprintf(stderr, "[VK] vkQueuePresentKHR -> %s\n", vk::VkResultToString(pr));
    }

    frames_->advance();
    ++frameCounter_;
    frameStarted_ = false;
}

void RendererVulkan::Clear(D3DCOLOR color, i32 clearColor, i32 clearDepth) {
    ColorToFloat4(color, clearColor_);   // remembered for next BeginFrame
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
    VkClearRect rect = {};
    rect.rect.extent = ext;
    rect.layerCount  = 1;
    vkCmdClearAttachments(frame.cmd, attCount, atts, 1, &rect);
}

void RendererVulkan::SetViewport(i32 x, i32 y, i32 w, i32 h, f32, f32) {
    viewportX = x; viewportY = y; viewportW = w; viewportH = h;
    if (frameStarted_ && inRenderPass_) {
        VkViewport vp = {};
        vp.x = float(x); vp.y = float(y);
        vp.width = float(w); vp.height = float(h);
        vp.minDepth = 0.0f; vp.maxDepth = 1.0f;
        vkCmdSetViewport(frames_->current().cmd, 0, 1, &vp);
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
void RendererVulkan::SetTextureStageSelectDiffuse()      { currentTexture   = 0; /* mirrors RendererGL: zeroing currentTexture forces drawCommon to fallback to defaultTex_ (1x1 white), so untextured draws sample white and pure diffuse passes through. */ }
void RendererVulkan::SetTextureStageModulateTexture()    { /* SetTexture(prev) restores the binding; pipeline colorOp picks Modulate via currentColorOp. */ }
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
bool RendererVulkan::drawCommon(int vertexLayoutEnum,
                                int topologyEnum,
                                bool hasTexture,
                                bool depthTest,
                                const void* verts,
                                int count,
                                uint32_t vertexStride) {
    if (!initialized_ || !frameStarted_ || !inRenderPass_ || count <= 0) return false;
    if (mvpDirty_) recomputeMvp();

    auto& frame = frames_->current();

    // 1. Build L2 key.
    vk::VkPipelineKey key{};
    key.vertexLayout      = static_cast<vk::VertexLayout>(vertexLayoutEnum);
    key.topology          = static_cast<vk::Topology>(topologyEnum);
    key.blendMode         = (currentBlendMode == 0) ? vk::BlendMode::Alpha : vk::BlendMode::Add;
    key.colorOp           = (currentColorOp   == 0) ? vk::ColorOp::Modulate : vk::ColorOp::Add;
    key.depthFunc         = depthFuncAlways_ ? vk::DepthFunc::Always : vk::DepthFunc::LessEqual;
    key.hasTexture        = hasTexture ? 1 : 0;
    key.depthTestEnable   = depthTest  ? 1 : 0;
    key.depthWriteEnable  = currentZWriteDisable ? 0 : 1;

    VkPipeline pipeline = pipelineCache_->GetOrCreate(*ctx_, key);
    if (pipeline == VK_NULL_HANDLE) return false;

    // 2. Upload verts.
    void*        mapped = nullptr;
    VkBuffer     vbBuf  = VK_NULL_HANDLE;
    VkDeviceSize vbOff  = 0;
    VkDeviceSize bytes  = VkDeviceSize(vertexStride) * VkDeviceSize(count);
    if (!uploadHeap_->AllocVerts(bytes, &mapped, &vbBuf, &vbOff)) return false;
    std::memcpy(mapped, verts, bytes);

    // 3. Bind pipeline + (optional) descriptor set.
    vkCmdBindPipeline(frame.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    if (hasTexture) {
        VkDescriptorSet ds = textureMgr_ ? textureMgr_->GetDescriptorSet(currentTexture)
                                         : VK_NULL_HANDLE;
        if (ds == VK_NULL_HANDLE) ds = defaultTex_->descriptorSet();
        vkCmdBindDescriptorSets(frame.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                pipelineLayoutTex_, 0, 1, &ds, 0, nullptr);
    }

    // 4. Push constants (invScreen + mvp + fog).
    PushConstants pc{};
    pc.invScreen[0] = (screenWidth  > 0) ? 1.0f / float(screenWidth)  : 0.0f;
    pc.invScreen[1] = (screenHeight > 0) ? 1.0f / float(screenHeight) : 0.0f;
    std::memcpy(pc.mvp, mvpMat_, sizeof(pc.mvp));
    // Fog only applies to 3D draws (depthTest=true); 2D screen-space draws disable it
    // unconditionally even if SetFog(1) was called earlier (matches GL discipline:
    // TH06 only invokes SetFog before stage 3D background, and fog has no meaningful
    // viewZ for pre-projected xyzrwh verts).
    const bool fogOn = depthTest && (fogEnabled != 0);
    pc.fogColor[0]  = fogOn ? D3DCOLOR_R(fogColor) / 255.0f : 0.0f;
    pc.fogColor[1]  = fogOn ? D3DCOLOR_G(fogColor) / 255.0f : 0.0f;
    pc.fogColor[2]  = fogOn ? D3DCOLOR_B(fogColor) / 255.0f : 0.0f;
    pc.fogColor[3]  = fogOn ? D3DCOLOR_A(fogColor) / 255.0f : 0.0f;
    pc.fogParams[0] = fogOn ? fogStart : 0.0f;
    pc.fogParams[1] = fogOn ? fogEnd   : 1.0f;
    pc.fogParams[2] = fogOn ? 1.0f     : 0.0f;
    pc.fogParams[3] = 0.0f;
    VkPipelineLayout layout = hasTexture ? pipelineLayoutTex_ : pipelineLayoutNoTex_;
    vkCmdPushConstants(frame.cmd, layout,
                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                       0, sizeof(PushConstants), &pc);

    // 5. Bind vertex buffer + draw.
    vkCmdBindVertexBuffers(frame.cmd, 0, 1, &vbBuf, &vbOff);
    vkCmdDraw(frame.cmd, uint32_t(count), 1, 0, 0);
    return true;
}

// --- Draw method dispatch ---
// Vertex layout enum / topology enum integer values match VkPipelineKey enums:
//   VertexLayout: 0=DiffuseXyzrwh, 1=Tex1Xyzrwh, 2=Tex1DiffuseXyzrwh, 3=Tex1DiffuseXyz, 4=RenderVertexInfoXyz
//   Topology:     0=TriangleStrip, 1=TriangleFan
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

    // Apply color key (D3D8 semantics: matching pixels become alpha=0; others alpha=255).
    if (colorKey != 0) {
        const u8 ckR = u8((colorKey >> 16) & 0xFF);
        const u8 ckG = u8((colorKey >>  8) & 0xFF);
        const u8 ckB = u8((colorKey      ) & 0xFF);
        SDL_LockSurface(rgba);
        u8 *pixels = static_cast<u8*>(rgba->pixels);
        // ABGR8888 layout: byte order is R,G,B,A in memory.
        const i32 pixelCount = rgba->w * rgba->h;
        for (i32 i = 0; i < pixelCount; ++i) {
            u8 *p = pixels + i * 4;
            if (p[0] == ckR && p[1] == ckG && p[2] == ckB) p[3] = 0;
            else                                            p[3] = 255;
        }
        SDL_UnlockSurface(rgba);
    }

    SDL_LockSurface(rgba);
    u32 id = textureMgr_->CreateFromRgba(*ctx_, rgba->w, rgba->h,
                                         static_cast<const u8*>(rgba->pixels));
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
    SDL_LockSurface(rgba);
    u32 id = textureMgr_->CreateFromRgba(*ctx_, rgba->w, rgba->h,
                                         (const uint8_t*)rgba->pixels);
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

}  // namespace th06
