// thprac_games.cpp - SDL2+OpenGL2 ImGui integration for th06 source build

#include "thprac_games.h"
#include <cstring>
#include <cstdio>
#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_impl_sdl.h>
#if defined(TH06_USE_GLES)
#include <imgui_impl_opengl3.h>
#else
#include <imgui_impl_opengl2.h>
#endif
// Phase 5b.2: Vulkan ImGui forwarding via the renderer (RendererVulkan owns the
// imgui_impl_vulkan lifecycle so cmd buffer / render pass details stay private).
#include "IRenderer.hpp"          // th06::IsUsingVulkan
#ifdef TH06_USE_VULKAN
#include "RendererVulkan.hpp"     // th06::GetRendererVulkan / RendererVulkan::RenderImGui
#endif
#include "TouchVirtualButtons.hpp"
#include "MenuTouchButtons.hpp"
#include <SDL.h>
#include "GameWindow.hpp" // th06::g_AndroidImeInsetPx

namespace THPrac {

// Global state
AdvancedGameOptions g_adv_igi_options;
int GameGuiProgress = 0;
int GameGuiGeneration = 0;
bool g_forceRenderCursor = false;
FastRetryOpt g_fast_re_opt;

// SDL window pointer for ImGui (set during init)
static SDL_Window* s_guiWindow = nullptr;

void SetGuiWindow(SDL_Window* w) { s_guiWindow = w; }

// ============================================================
// Game GUI - Real ImGui SDL2+OpenGL2 implementation
// ============================================================
void GameUpdateInner(int /*gamever*/) {}
void GameUpdateOuter(ImDrawList* /*p*/, int /*ver*/) {}
void FastRetry(int /*thprac_mode*/) {}

ImTextureID ReadImage(DWORD /*dxVer*/, DWORD /*device*/, const char* /*name*/, const char* /*srcData*/, size_t /*srcSz*/)
{
    return nullptr;
}

void SetDpadHook(uintptr_t /*addr*/, size_t /*instr_len*/) {}

void GameGuiInit(game_gui_impl /*impl*/, int /*device*/, int /*hwnd_addr*/,
    Gui::ingame_input_gen_t /*input_gen*/, int /*reg1*/, int /*reg2*/, int /*reg3*/,
    int /*wnd_size_flag*/, float /*x*/, float /*y*/)
{
    // ImGui context already created externally (thprac_gui_integration.cpp)
    // This function is kept for compatibility with thprac_th06.cpp calling convention
}

void GameGuiBegin(game_gui_impl /*impl*/, bool game_nav)
{
    if (!ImGui::GetCurrentContext())
        return;

    // Acquire game input for ImGui navigation
    ImGuiIO& io = ImGui::GetIO();
    if (game_nav) {
        Gui::GuiNavFocus::GlobalDisable(false);
        io.NavInputs[ImGuiNavInput_DpadUp] = Gui::InGameInputGet(VK_UP) ? 1.0f : 0.0f;
        io.NavInputs[ImGuiNavInput_DpadDown] = Gui::InGameInputGet(VK_DOWN) ? 1.0f : 0.0f;
        io.NavInputs[ImGuiNavInput_DpadLeft] = Gui::InGameInputGet(VK_LEFT) ? 1.0f : 0.0f;
        io.NavInputs[ImGuiNavInput_DpadRight] = Gui::InGameInputGet(VK_RIGHT) ? 1.0f : 0.0f;
        io.NavInputs[ImGuiNavInput_Activate] = Gui::InGameInputGet(VK_RETURN) ? 1.0f : 0.0f;
        io.NavInputs[ImGuiNavInput_Cancel] = Gui::InGameInputGet(VK_ESCAPE) ? 1.0f : 0.0f;
    } else {
        Gui::GuiNavFocus::GlobalDisable(true);
    }

    // Phase 5b.2: pick the matching ImGui backend NewFrame. On Vulkan we never
    // initialised the GL backend, so calling its NewFrame would deref unset state.
#ifdef TH06_USE_VULKAN
    if (th06::IsUsingVulkan() && th06::g_Renderer) {
        static_cast<th06::RendererVulkan*>(th06::GetRendererVulkan())->NewFrameImGui();
    } else
#endif
    {
#if defined(TH06_USE_GLES)
        ImGui_ImplOpenGL3_NewFrame();
#else
        ImGui_ImplOpenGL2_NewFrame();
#endif
    }
    if (!io.Fonts->IsBuilt())
        return;
    ImGui_ImplSDL2_NewFrame(s_guiWindow);
    // Override display size to game's logical resolution (640x480).
    // ImGui_ImplSDL2_NewFrame sets it to the actual window size, which
    // in fullscreen is the monitor resolution. We render ImGui into the
    // game's FBO at 640x480 so the letterboxing blit scales it correctly.
    float winW = io.DisplaySize.x;
    float winH = io.DisplaySize.y;
    io.DisplaySize = ImVec2(640.0f, 480.0f);
    io.DisplayFramebufferScale = ImVec2(1.0f, 1.0f);
    // Remap mouse position from window coordinates to game coordinates.
    // Account for letterbox/pillarbox offset and scaling.
    if (winW > 0 && winH > 0) {
        float scaleX, scaleY, offsetX, offsetY;
        if (winW * 480.0f > winH * 640.0f) {
            // Pillarbox (black bars on sides)
            float scaledW = winH * 640.0f / 480.0f;
            scaleX = scaleY = 480.0f / winH;
            offsetX = (winW - scaledW) * 0.5f;
            offsetY = 0.0f;
        } else {
            // Letterbox (black bars on top/bottom)
            float scaledH = winW * 480.0f / 640.0f;
            scaleX = scaleY = 640.0f / winW;
            offsetX = 0.0f;
            offsetY = (winH - scaledH) * 0.5f;
        }
        io.MousePos.x = (io.MousePos.x - offsetX) * scaleX;
        io.MousePos.y = (io.MousePos.y - offsetY) * scaleY;
    }
#ifdef __ANDROID__
    // Android IME: shrink the ImGui logical canvas height by the keyboard's
    // size so windows auto-clamped to DisplaySize stay above the keyboard.
    // Only do so when the focused input is actually obstructed.
    if (th06::g_AndroidImeInsetPx > 0 && winW > 0 && winH > 0) {
        float yScale = (winW * 480.0f > winH * 640.0f) ? (480.0f / winH)
                                                       : (640.0f / winW);
        float imeLogical = th06::g_AndroidImeInsetPx * yScale;
        if (imeLogical > 0.0f && imeLogical < 480.0f) {
            float keyboardTopY = 480.0f - imeLogical;

            // Decide whether the focused input intersects the keyboard area.
            // Robust order:
            //   1) ActiveIdWindow (the window owning the focused widget) —
            //      stable across frames as long as the input has focus.
            //   2) PlatformImePos (last caret position from the previous
            //      InputText render) — fallback if no ActiveIdWindow.
            //   3) WantTextInput true with neither hint available — assume
            //      obstructed (safe default).
            bool obstructed = false;
            const char* reason = "no-ime-needed";
            ImGuiContext* gctx = ImGui::GetCurrentContext();
            if (io.WantTextInput) {
                // Conservative margin so we lift even if the bottom of the
                // window is only marginally inside the keyboard band.
                const float kMargin = 8.0f;
                if (gctx && gctx->ActiveIdWindow) {
                    float wndBottom = gctx->ActiveIdWindow->Pos.y +
                                      gctx->ActiveIdWindow->Size.y;
                    obstructed = (wndBottom > keyboardTopY - kMargin);
                    reason = obstructed ? "wnd-overlap" : "wnd-above";
                } else if (gctx && gctx->PlatformImePos.x < 1.0e30f &&
                           gctx->PlatformImePos.x > 2.0f) {
                    // PlatformImePos == (1,1) is the per-frame reset value,
                    // ignore it and fall through to "safe lift".
                    float caretBottomY = gctx->PlatformImePos.y +
                                         ImGui::GetFontSize();
                    obstructed = (caretBottomY > keyboardTopY - kMargin);
                    reason = obstructed ? "caret-overlap" : "caret-above";
                } else {
                    obstructed = true;
                    reason = "want-input-no-pos";
                }
            }
            if (obstructed) {
                io.DisplaySize.y = keyboardTopY;
            }

            // Throttled diagnostic so we can verify the heuristic on device.
            static int s_logTick = 0;
            if ((++s_logTick % 30) == 0) {
                float wndBottom = (gctx && gctx->ActiveIdWindow)
                    ? (gctx->ActiveIdWindow->Pos.y + gctx->ActiveIdWindow->Size.y)
                    : -1.0f;
                std::fprintf(stderr,
                    "[ime] inset=%dpx imeLogical=%.1f kbdTopY=%.1f want=%d "
                    "wndBottom=%.1f caretImePos=(%.1f,%.1f) -> %s\n",
                    th06::g_AndroidImeInsetPx, imeLogical, keyboardTopY,
                    io.WantTextInput ? 1 : 0, wndBottom,
                    gctx ? gctx->PlatformImePos.x : -1.0f,
                    gctx ? gctx->PlatformImePos.y : -1.0f, reason);
            }
        }
    }
#endif
    ImGui::NewFrame();
    GameGuiProgress = 1;
}

void GameGuiEnd(bool draw_cursor)
{
    if (GameGuiProgress != 1)
        return;
    ImGuiIO& io = ImGui::GetIO();
    io.MouseDrawCursor = draw_cursor;

    // Phase 5b.2: emit touch virtual button overlay via ImGui's background draw
    // list. On Vulkan, the offscreen FBO is sized to the swapchain (e.g. 1708x1067)
    // and ImGui draw data is remapped (in RendererVulkan::RenderImGui) so that
    // ImGui logical coordinates cover the *entire FBO*, not just the playfield.
    // That means screen-space pixel positions can be expressed directly in
    // (logical/s) units. GLES has its own DrawScreenSpaceButtons() in EndFrame;
    // for Vulkan we piggyback on ImGui to avoid building a second pipeline.
    if (th06::IsUsingVulkan() && th06::g_Renderer) {
        th06::TouchButtonInfo buttons[12];
        int count = th06::TouchVirtualButtons::GetButtonInfo(buttons, 7);
        count += th06::MenuTouchButtons::GetButtonInfo(buttons + count, 5);
        if (count > 0) {
            // Get actual FBO size = swapchain extent. RendererVulkan keeps
            // realScreenWidth/Height in sync with the swapchain (resize hook).
            int rw = th06::g_Renderer->realScreenWidth;
            int rh = th06::g_Renderer->realScreenHeight;
            if (rw > 0 && rh > 0) {
                const float gameW = 640.0f;
                const float gameH = 480.0f;
                int scaledW, scaledH;
                if (rw * (int)gameH > rh * (int)gameW) {
                    scaledH = rh;
                    scaledW = (int)(rh * gameW / gameH);
                } else {
                    scaledW = rw;
                    scaledH = (int)(rw * gameH / gameW);
                }
                int offsetX = (rw - scaledW) / 2;
                int offsetY = (rh - scaledH) / 2;
                if (offsetX >= 5) {  // need pillarbox to draw on
                    // Uniform scale that ImGui's framebuffer uses (matches
                    // RendererVulkan::RenderImGui's s = min(fbW/640, fbH/480)).
                    const float s = (rw / gameW < rh / gameH) ? rw / gameW : rh / gameH;
                    // Convert screen pixel coords -> ImGui logical coords:
                    //   logical = DisplayPos + screen_pixel / s
                    //           = (-offsetX/s, -offsetY/s) + (px, py)/s
                    //           = ((px-offsetX)/s, (py-offsetY)/s)
                    const float yScale = (float)scaledH / gameH;
                    ImDrawList* dl = ImGui::GetBackgroundDrawList();
                    // io.DisplaySize was set to 640x480 in GameGuiBegin, so the
                    // background draw list's default ClipRect is also (0,0)-(640,480).
                    // RendererVulkan::RenderImGui later expands DisplayPos/Size
                    // to the letterbox-aware FBO range, but per-cmd ClipRect is
                    // captured AT submit time — so any draws outside [0,640]x[0,480]
                    // (i.e. anything on the pillarbox bars) get scissor-clipped.
                    // Push an extended clip rect matching the FBO logical extent.
                    const ImVec2 extClipMin(-(float)offsetX / s, -(float)offsetY / s);
                    const ImVec2 extClipMax((float)(rw - offsetX) / s, (float)(rh - offsetY) / s);
                    dl->PushClipRect(extClipMin, extClipMax, false);
                    // The Vulkan offscreen FBO uses LOAD_OP_LOAD and the game's
                    // Clear() only scissor-clears the playfield viewport. The
                    // pillarbox bars therefore accumulate content frame after
                    // frame; without resetting them, repeated semi-transparent
                    // overlays converge to opaque white. Reset both pillars to
                    // opaque black before drawing the buttons.
                    const ImU32 kBlack = IM_COL32(0, 0, 0, 255);
                    // Left bar: x in [-offX/s, 0)
                    dl->AddRectFilled(extClipMin, ImVec2(0.0f, extClipMax.y), kBlack);
                    // Right bar: x in (640, (rw-offX)/s]
                    dl->AddRectFilled(ImVec2(640.0f, extClipMin.y), extClipMax, kBlack);
                    // Top bar (in case letterboxed instead of pillarboxed): y < 0
                    if (offsetY > 0) {
                        dl->AddRectFilled(extClipMin, ImVec2(extClipMax.x, 0.0f), kBlack);
                        dl->AddRectFilled(ImVec2(extClipMin.x, 480.0f), extClipMax, kBlack);
                    }
                    for (int i = 0; i < count; i++) {
                        float sy = offsetY + (buttons[i].gameY / gameH) * scaledH;
                        float sr = buttons[i].gameRadius * yScale;
                        float sx;
                        if (buttons[i].anchor == th06::ScreenAnchor::RightPillar) {
                            sx = (float)(rw - offsetX) + sr;
                            if (sx > rw - sr) sx = rw - sr;
                        } else {
                            sx = (float)offsetX - sr;
                            if (sx < sr) sx = sr;
                        }
                        // Map (sx, sy, sr) screen pixels -> ImGui logical units
                        ImVec2 c((sx - offsetX) / s, (sy - offsetY) / s);
                        float r = sr / s;
                        // D3DCOLOR is ARGB; ImU32 is ABGR (IMGUI_COL32). Repack.
                        auto toImCol = [](D3DCOLOR d) -> ImU32 {
                            return IM_COL32(D3DCOLOR_R(d), D3DCOLOR_G(d),
                                            D3DCOLOR_B(d), D3DCOLOR_A(d));
                        };
                        dl->AddCircleFilled(c, r, toImCol(buttons[i].fillColor), 32);
                        dl->AddCircle(c, r, toImCol(buttons[i].borderColor), 32, 2.0f / s);
                        if (buttons[i].label && buttons[i].label[0]) {
                            ImVec2 ts = ImGui::CalcTextSize(buttons[i].label);
                            ImVec2 tp(c.x - ts.x * 0.5f, c.y - ts.y * 0.5f);
                            dl->AddText(tp, IM_COL32(255, 255, 255, 255), buttons[i].label);
                        }
                    }
                    dl->PopClipRect();
                }
            }
        }
    }

    ImGui::EndFrame();
    GameGuiProgress = 2;
}

void GameGuiRender(game_gui_impl /*impl*/)
{
    if (GameGuiProgress != 2)
        return;
    // Safety: only call ImGui::Render() if EndFrame() was properly called
    // for this frame. On a fresh/recreated context (FrameCountEnded=-1,
    // FrameCount=0), Render() would call EndFrame() → End() and crash
    // dereferencing a NULL CurrentWindow.
    ImGuiContext* ctx = ImGui::GetCurrentContext();
    if (!ctx || ctx->FrameCountEnded != ctx->FrameCount) {
        GameGuiProgress = 0;
        return;
    }
    ImGui::Render();
#ifdef TH06_USE_VULKAN
    if (th06::IsUsingVulkan() && th06::g_Renderer) {
        // Phase 5b.2: Vulkan branch — RendererVulkan::EndFrame already opened the
        // offscreen render pass and is calling THPracGuiRender → here. Forward
        // into the renderer which records ImGui draws into the current cmd buffer.
        static_cast<th06::RendererVulkan*>(th06::GetRendererVulkan())->RenderImGui();
    } else
#endif
    {
#if defined(TH06_USE_GLES)
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
#else
        ImGui_ImplOpenGL2_RenderDrawData(ImGui::GetDrawData());
#endif
    }
    GameGuiProgress = 0;
}

// ============================================================
// Advanced Options stubs
// ============================================================
void OILPInit(adv_opt_ctx& /*ctx*/) {}
void CenteredText(const char* /*text*/, float /*wndX*/) {}
float GetRelWidth(float rel) { return rel; }
float GetRelHeight(float rel) { return rel; }
void CalcFileHash(const wchar_t* /*file_name*/, uint64_t hash[2]) { hash[0] = 0; hash[1] = 0; }
void HelpMarker(const char* /*desc*/) {}
void CustomMarker(const char* /*text*/, const char* /*desc*/) {}

int FPSHelper(adv_opt_ctx& /*ctx*/, bool /*repStatus*/, bool /*vpFast*/, bool /*vpSlow*/, FPSHelperCallback* /*callback*/)
{
    return 0;
}

bool GameFPSOpt(adv_opt_ctx& ctx, bool /*replay*/)
{
    // SDL2-native game speed control via framerateMultiplier
    static int speedIdx = 3; // default = x1.0
    static const char* speedLabels[] = {
        "x0.25 (15fps)", "x0.5 (30fps)", "x0.75 (45fps)", "x1.0 (60fps)",
        "x1.5 (90fps)", "x2.0 (120fps)", "x3.0 (180fps)"
    };
    static const float speedValues[] = { 0.25f, 0.5f, 0.75f, 1.0f, 1.5f, 2.0f, 3.0f };
    static const int numSpeeds = 7;

    bool changed = false;
    ImGui::PushItemWidth(180.0f);
    if (ImGui::SliderInt(Gui::LocaleGetStr(TH_FPS_ADJ), &speedIdx, 0, numSpeeds - 1, speedLabels[speedIdx])) {
        changed = true;
        ctx.fps = (int)(speedValues[speedIdx] * 60.0f);
    }
    ImGui::PopItemWidth();
    return changed;
}
bool GameplayOpt(adv_opt_ctx& /*ctx*/) { return false; }
void AboutOpt(const char* /*thanks_text*/) {}
void InGameReactionTestOpt() {}
void InfLifeOpt() {}
void DisableKeyOpt() {}
void KeyHUDOpt() {}

// ============================================================
// Replay System stubs
// ============================================================
bool ReplaySaveParam(const wchar_t* /*rep_path*/, const std::string& /*param*/)
{
    return false;
}

bool ReplayLoadParam(const wchar_t* /*rep_path*/, std::string& /*param*/)
{
    return false;
}

// ============================================================
// String Encoding
// ============================================================
std::wstring mb_to_utf16(const char* str, unsigned int /*encoding*/)
{
    // Simple conversion for ASCII range; full implementation needed for CJK
    std::wstring result;
    if (!str) return result;
    while (*str) {
        result += static_cast<wchar_t>(static_cast<unsigned char>(*str));
        ++str;
    }
    return result;
}

// ============================================================
// Key Render stubs
// ============================================================
static std::vector<int> s_key_aps;

void RecordKey(int /*ver*/, uint32_t /*cur_key*/) {}
void KeysHUD(int /*ver*/, ImVec2 /*render_pos_arrow*/, ImVec2 /*render_pos_key*/, const KeyRectStyle& /*style*/, bool /*align_right_arrow*/, bool /*align_right_key*/) {}
void SaveKeyRecorded() {}
void ClearKeyRecord() {}
std::vector<int>& GetKeyAPS() { return s_key_aps; }

// ============================================================
// SSS stubs
// ============================================================
namespace SSS {
    void SSS_UI(int /*version*/) {}
    void SSS_Update(int /*ver*/) {}
}

} // namespace THPrac
