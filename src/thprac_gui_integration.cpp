// thprac_gui_integration.cpp - Bridges ImGui SDL2+OpenGL2 into the th06 game loop
// Initializes ImGui, forwards SDL events, and delegates per-frame updates/renders
// to thprac's TH06 GUI code.

#include "thprac_gui_integration.h"
#include "thprac_th06.h"
#include "thprac_games.h"
#include <imgui.h>
#include <imgui_impl_sdl.h>
#if defined(TH06_USE_GLES)
#include <imgui_impl_opengl3.h>
#else
#include <imgui_impl_opengl2.h>
#endif
#include <SDL.h>
#include <cstdio>

namespace THPrac {

extern void TH06Init();
extern void TH06Reset();

static bool s_initialized = false;

void THPracGuiInit(SDL_Window* window, void* glContext)
{
    if (s_initialized)
        return;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();

    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(640.0f, 480.0f);
    io.IniFilename = nullptr; // Don't write imgui.ini
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    ImGui_ImplSDL2_InitForOpenGL(window, glContext);
#if defined(TH06_USE_GLES)
    ImGui_ImplOpenGL3_Init();
#else
    ImGui_ImplOpenGL2_Init();
#endif

    // Load a CJK-capable font for Chinese/Japanese locale strings.
    // Try system fonts in order; fall back to ImGui's default ASCII font.
    bool fontLoaded = false;
    static const char* fontPaths[] = {
#ifdef _WIN32
        "C:\\Windows\\Fonts\\msyh.ttc",   // Microsoft YaHei
        "C:\\Windows\\Fonts\\msyhbd.ttc",  // Microsoft YaHei Bold
        "C:\\Windows\\Fonts\\simhei.ttf",  // SimHei
        "C:\\Windows\\Fonts\\simsun.ttc",  // SimSun
#elif defined(__APPLE__)
        "/System/Library/Fonts/Hiragino Sans GB.ttc",
        "/Library/Fonts/Arial Unicode.ttf",
#else
        "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/truetype/noto/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/google-noto-cjk/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/truetype/wqy/wqy-zenhei.ttc",
        "/usr/share/fonts/wenquanyi/wqy-zenhei/wqy-zenhei.ttc",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
#endif
        nullptr
    };
    for (int fi = 0; fontPaths[fi]; fi++) {
        const char* path = fontPaths[fi];
        FILE* f = fopen(path, "rb");
        if (f) {
            fclose(f);
            ImFont* font = io.Fonts->AddFontFromFileTTF(
                path, 16.0f, nullptr, io.Fonts->GetGlyphRangesChineseFull());
            if (font) {
                fontLoaded = true;
                break;
            }
        }
    }
    if (!fontLoaded) {
        io.Fonts->AddFontDefault();
    }

    // Store window pointer for GameGuiBegin's NewFrame call
    SetGuiWindow(window);

    // Create thprac singletons and state
    TH06Init();

    s_initialized = true;
}

void THPracGuiProcessEvent(SDL_Event* event)
{
    if (s_initialized)
        ImGui_ImplSDL2_ProcessEvent(event);
}

void THPracGuiUpdate()
{
    if (!s_initialized)
        return;
    TH06::THPracUpdate();
}

void THPracGuiRender()
{
    if (!s_initialized)
        return;
    TH06::THPracRender();
}

bool THPracGuiIsReady()
{
    return s_initialized;
}

void THPracGuiShutdown()
{
    if (!s_initialized)
        return;

    // Reset frame state machine so stale progress doesn't leak across restart
    GameGuiProgress = 0;
    // Bump generation so GameGuiWnd instances re-apply size/pos/locale
    GameGuiGeneration++;

#if defined(TH06_USE_GLES)
    ImGui_ImplOpenGL3_Shutdown();
#else
    ImGui_ImplOpenGL2_Shutdown();
#endif
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
    SetGuiWindow(nullptr);
    TH06Reset();
    s_initialized = false;
}

} // namespace THPrac
