// thprac_games.cpp - SDL2+OpenGL2 ImGui integration for th06 source build

#include "thprac_games.h"
#include <cstring>
#include <imgui.h>
#include <imgui_impl_sdl.h>
#include <imgui_impl_opengl2.h>
#include <SDL.h>

namespace THPrac {

// Global state
AdvancedGameOptions g_adv_igi_options;
int GameGuiProgress = 0;
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

    ImGui_ImplOpenGL2_NewFrame();
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
    ImGui::NewFrame();
    GameGuiProgress = 1;
}

void GameGuiEnd(bool draw_cursor)
{
    if (GameGuiProgress != 1)
        return;
    ImGuiIO& io = ImGui::GetIO();
    io.MouseDrawCursor = draw_cursor;
    ImGui::EndFrame();
    GameGuiProgress = 2;
}

void GameGuiRender(game_gui_impl /*impl*/)
{
    if (GameGuiProgress != 2)
        return;
    ImGui::Render();
    ImGui_ImplOpenGL2_RenderDrawData(ImGui::GetDrawData());
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
