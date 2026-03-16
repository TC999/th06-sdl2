// thprac_gui_integration.cpp - Bridges ImGui SDL2+OpenGL2 into the th06 game loop
// Initializes ImGui, forwards SDL events, and delegates per-frame updates/renders
// to thprac's TH06 GUI code.

#include "thprac_gui_integration.h"
#include "thprac_th06.h"
#include "thprac_games.h"
#include <imgui.h>
#include <imgui_impl_sdl.h>
#include <imgui_impl_opengl2.h>
#include <SDL.h>
#include <cstdio>

namespace THPrac {

extern void TH06Init();

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
    ImGui_ImplOpenGL2_Init();

    // Load a CJK-capable font for Chinese/Japanese locale strings.
    // Try system fonts in order; fall back to ImGui's default ASCII font.
    bool fontLoaded = false;
    static const char* fontPaths[] = {
        "C:\\Windows\\Fonts\\msyh.ttc",   // Microsoft YaHei
        "C:\\Windows\\Fonts\\msyhbd.ttc",  // Microsoft YaHei Bold
        "C:\\Windows\\Fonts\\simhei.ttf",  // SimHei
        "C:\\Windows\\Fonts\\simsun.ttc",  // SimSun
    };
    for (auto path : fontPaths) {
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

} // namespace THPrac
