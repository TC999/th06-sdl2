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
// Phase 5b.2: Vulkan ImGui owned by RendererVulkan.
#include "IRenderer.hpp"
#include "RendererVulkan.hpp"
#include <SDL.h>
#include <cstdio>

namespace THPrac {

extern void TH06Init();
extern void TH06Reset();

static bool s_initialized = false;
static bool s_headless    = false;  // true => no rendering backend; skip Update/Render

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

    // Phase 5b.2: Vulkan branch — RendererVulkan owns the imgui_impl_vulkan
    // lifecycle (LoadFunctions / Init / font upload / DescriptorPool / Shutdown).
    // We still need ImGui_ImplSDL2_InitForVulkan to wire up event/clipboard/etc.,
    // which RendererVulkan::InitImGui handles internally.
    const bool useVulkan = th06::IsUsingVulkan();
    if (useVulkan) {
        std::fprintf(stderr, "[thprac] useVulkan=1 g_Renderer=%p GetRendererVulkan()=%p\n",
                     (void*)th06::g_Renderer, (void*)th06::GetRendererVulkan());
        if (!th06::g_Renderer ||
            !static_cast<th06::RendererVulkan*>(th06::GetRendererVulkan())->InitImGui(window)) {
            std::fprintf(stderr, "[thprac] Vulkan ImGui init failed\n");
            ImGui::DestroyContext();
            return;
        }
    } else {
        ImGui_ImplSDL2_InitForOpenGL(window, glContext);
#if defined(TH06_USE_GLES)
        ImGui_ImplOpenGL3_Init();
#else
        ImGui_ImplOpenGL2_Init();
#endif
    }

    // Load a CJK-capable font for Chinese/Japanese locale strings.
    bool fontLoaded = false;
#ifdef __ANDROID__
    // On Android, load the bundled font from APK assets via SDL_RWFromFile.
    // System fonts may be in formats stb_truetype can't handle (variable fonts etc.)
    {
        static const char* assetFonts[] = {
            "font/Noto Sans SC.ttf",
            nullptr
        };
        for (int fi = 0; assetFonts[fi]; fi++) {
            SDL_RWops* rw = SDL_RWFromFile(assetFonts[fi], "rb");
            if (rw) {
                Sint64 size = SDL_RWsize(rw);
                if (size > 0) {
                    void* data = IM_ALLOC((size_t)size);
                    if (data && SDL_RWread(rw, data, 1, (size_t)size) == (size_t)size) {
                        // AddFontFromMemoryTTF takes ownership of data
                        ImFont* font = io.Fonts->AddFontFromMemoryTTF(
                            data, (int)size, 16.0f, nullptr, io.Fonts->GetGlyphRangesChineseFull());
                        if (font) {
                            fontLoaded = true;
                            SDL_RWclose(rw);
                            break;
                        }
                    }
                    if (!fontLoaded) IM_FREE(data);
                }
                SDL_RWclose(rw);
            }
        }
    }
#else
    // Desktop: try system fonts in order
    {
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
    }
#endif
    if (!fontLoaded) {
        io.Fonts->AddFontDefault();
    }
    // Pre-build the font atlas to catch failures early.
    // If Build() fails (e.g. font data incompatible), fall back to default.
    io.Fonts->Build();
    if (!io.Fonts->IsBuilt()) {
        io.Fonts->Clear();
        io.Fonts->AddFontDefault();
        io.Fonts->Build();
    }

    // Store window pointer for GameGuiBegin's NewFrame call
    SetGuiWindow(window);

    // Create thprac singletons and state
    TH06Init();

    s_initialized = true;
}

void THPracGuiProcessEvent(SDL_Event* event)
{
    if (s_initialized && !s_headless)
        ImGui_ImplSDL2_ProcessEvent(event);
}

void THPracGuiUpdate()
{
    if (!s_initialized || s_headless)
        return;
    TH06::THPracUpdate();
}

void THPracGuiRender()
{
    if (!s_initialized || s_headless)
        return;
    TH06::THPracRender();

#ifdef __ANDROID__
    // Sync Android soft-keyboard (IME) with ImGui's text-input intent.
    // ImGuiIO::WantTextInput becomes true while an InputText widget is active.
    // Toggle SDL text input accordingly so the IME pops up only when needed.
    static bool s_imeOn = false;
    bool want = ImGui::GetIO().WantTextInput;
    if (want && !s_imeOn) {
        SDL_StartTextInput();
        s_imeOn = true;
    } else if (!want && s_imeOn) {
        SDL_StopTextInput();
        s_imeOn = false;
    }
#endif
}

void THPracGuiInitHeadless(SDL_Window* window)
{
    if (s_initialized)
        return;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();

    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(640.0f, 480.0f);
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    // Default font is enough; thprac singletons only query glyph ranges, never render.
    io.Fonts->AddFontDefault();
    io.Fonts->Build();

    SetGuiWindow(window);

    // Construct all thprac singletons (THOverlay, GameGuiWnd subclasses) so the
    // game-thread inline accessors (THPracIsTimeLock etc.) don't crash.
    TH06Init();

    s_headless    = true;
    s_initialized = true;
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

    if (s_headless) {
        // Headless init only created the ImGui context (no SDL2 / GL backend).
    } else if (th06::IsUsingVulkan() && th06::g_Renderer) {
        // Phase 5b.2: RendererVulkan owns ImplVulkan + ImplSDL2 lifecycle.
        static_cast<th06::RendererVulkan*>(th06::GetRendererVulkan())->ShutdownImGui();
    } else {
#if defined(TH06_USE_GLES)
        ImGui_ImplOpenGL3_Shutdown();
#else
        ImGui_ImplOpenGL2_Shutdown();
#endif
        ImGui_ImplSDL2_Shutdown();
    }
    ImGui::DestroyContext();
    SetGuiWindow(nullptr);
    TH06Reset();
    s_initialized = false;
}

} // namespace THPrac
