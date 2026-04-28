#include "GameWindow.hpp"
#include "AndroidTouchInput.hpp"
#include "AnmManager.hpp"
#include "AssetIO.hpp"
#include "Controller.hpp"
#include "GameErrorContext.hpp"
#include "GameManager.hpp"
#include "NetplaySession.hpp"
#include "OnlineMenu.hpp"
#include "PortableGameplayRestore.hpp"
#include "RendererGLES.hpp"
#include "ScreenEffect.hpp"
#include "Session.hpp"
#include "SoundPlayer.hpp"
#include "Stage.hpp"
#include "Supervisor.hpp"
#include "diffbuild.hpp"
#include "i18n.hpp"
#include "sdl2_renderer.hpp"
#include "thprac_games.h"
#include "thprac_gui_integration.h"
#include <imgui.h>
#include <cmath>
#ifdef TH06_USE_VULKAN
#include <SDL_vulkan.h>
#endif
#include <cstdio>

namespace th06
{
DIFFABLE_STATIC(GameWindow, g_GameWindow)
DIFFABLE_STATIC(i32, g_TickCountToEffectiveFramerate)
DIFFABLE_STATIC(f64, g_LastFrameTime)

int g_AndroidImeInsetPx = 0;

#ifdef __ANDROID__
// On Android, sdl2_renderer.cpp is excluded (fixed-function GL).
// Provide the global renderer pointer here.
IRenderer *g_Renderer = nullptr;
#endif

#define FRAME_TIME (1000. / 60.)
constexpr int kRollbackCatchupCalcLimit = 32;

static double GetFrameTime() {
    float speed = Session::IsRemoteNetplaySession() ? 1.0f : THPrac::THPracGetSpeedMultiplier();
    return (speed > 0.01f) ? (FRAME_TIME / speed) : FRAME_TIME;
}

static int GetPreferredSwapInterval(bool windowed)
{
#ifdef __ANDROID__
    (void)windowed;
    // On Android, pacing is usually more stable when we let EGL/SurfaceFlinger
    // block on the display cadence instead of combining swap interval 0 with a
    // coarse SDL_Delay-based software limiter.
    return 1;
#else
    // In windowed mode on desktop, the software busy-wait in Render() handles
    // frame pacing (matching the original D3D8 behaviour where windowed Present
    // is immediate).  Enabling VSync here would add an extra display-period
    // delay on top of the busy-wait, halving the effective frame rate.
    return 0;
#endif
}

static bool g_PendingWindowModeChange = false;
static bool g_PendingWindowModeWindowed = false;
static bool g_PendingRestart = false;

static bool ShouldFreezeWhenInactive()
{
    return !(THPrac::g_adv_igi_options.th06_run_in_background || OnlineMenu::ShouldForceRunInBackground());
}

static bool IsWindowPresentationUnavailable()
{
    if (g_GameWindow.sdlWindow == NULL)
    {
        return false;
    }

    const Uint32 windowFlags = SDL_GetWindowFlags(g_GameWindow.sdlWindow);
    return (windowFlags & (SDL_WINDOW_MINIMIZED | SDL_WINDOW_HIDDEN)) != 0;
}

static bool ShouldRunHeadlessWhenInactive()
{
    return !ShouldFreezeWhenInactive() && IsWindowPresentationUnavailable();
}

#ifdef __ANDROID__
static void LogAndroidInputEvent(const SDL_Event &event)
{
    switch (event.type)
    {
    case SDL_KEYDOWN:
    case SDL_KEYUP:
        SDL_Log("[input/sdl] key %s scancode=%s(%d) sym=%s(%d) repeat=%d state=%d",
                event.type == SDL_KEYDOWN ? "down" : "up", SDL_GetScancodeName(event.key.keysym.scancode),
                event.key.keysym.scancode, SDL_GetKeyName(event.key.keysym.sym), event.key.keysym.sym,
                event.key.repeat, event.key.state);
        break;
    case SDL_TEXTINPUT:
        SDL_Log("[input/sdl] text \"%s\"", event.text.text);
        break;
    case SDL_WINDOWEVENT:
        if (event.window.event == SDL_WINDOWEVENT_FOCUS_GAINED ||
            event.window.event == SDL_WINDOWEVENT_FOCUS_LOST)
        {
            SDL_Log("[input/sdl] window event=%d", event.window.event);
        }
        break;
    case SDL_APP_WILLENTERBACKGROUND:
    case SDL_APP_DIDENTERBACKGROUND:
    case SDL_APP_WILLENTERFOREGROUND:
    case SDL_APP_DIDENTERFOREGROUND:
        SDL_Log("[input/sdl] app event type=%u", event.type);
        break;
    case SDL_JOYDEVICEADDED:
    case SDL_JOYDEVICEREMOVED:
        SDL_Log("[input/sdl] joy device type=%u which=%d", event.type, event.jdevice.which);
        break;
    case SDL_CONTROLLERDEVICEADDED:
    case SDL_CONTROLLERDEVICEREMOVED:
        SDL_Log("[input/sdl] controller device type=%u which=%d", event.type, event.cdevice.which);
        break;
    case SDL_JOYBUTTONDOWN:
    case SDL_JOYBUTTONUP:
        SDL_Log("[input/sdl] joy button %s which=%d button=%d state=%d",
                event.type == SDL_JOYBUTTONDOWN ? "down" : "up", event.jbutton.which, event.jbutton.button,
                event.jbutton.state);
        break;
    case SDL_JOYHATMOTION:
        SDL_Log("[input/sdl] joy hat which=%d hat=%d value=%d", event.jhat.which, event.jhat.hat,
                event.jhat.value);
        break;
    case SDL_CONTROLLERBUTTONDOWN:
    case SDL_CONTROLLERBUTTONUP:
        SDL_Log("[input/sdl] controller button %s which=%d button=%d state=%d",
                event.type == SDL_CONTROLLERBUTTONDOWN ? "down" : "up", event.cbutton.which,
                event.cbutton.button, event.cbutton.state);
        break;
    case SDL_FINGERDOWN:
    case SDL_FINGERMOTION:
    case SDL_FINGERUP:
        SDL_Log("[input/sdl] finger %s id=%lld x=%.3f y=%.3f",
                event.type == SDL_FINGERDOWN ? "down" : (event.type == SDL_FINGERUP ? "up" : "motion"),
                (long long)event.tfinger.fingerId, event.tfinger.x, event.tfinger.y);
        break;
    default:
        break;
    }
}
#endif

static void ApplyPendingWindowMode()
{
    if (!g_PendingWindowModeChange)
        return;
    g_PendingWindowModeChange = false;
    bool windowed = g_PendingWindowModeWindowed;

    SDL_Window *win = g_GameWindow.sdlWindow;

    if (windowed)
    {
        SDL_SetWindowBordered(win, SDL_TRUE);
        SDL_SetWindowSize(win, GAME_WINDOW_WIDTH, GAME_WINDOW_HEIGHT);
        SDL_SetWindowPosition(win, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
        g_GameWindow.screenWidth = 0;
        g_GameWindow.screenHeight = 0;
        SDL_ShowCursor(SDL_ENABLE);
    }
    else
    {
        SDL_DisplayMode dm;
        SDL_GetDesktopDisplayMode(0, &dm);
        SDL_SetWindowBordered(win, SDL_FALSE);
        SDL_SetWindowPosition(win, 0, 0);
        SDL_SetWindowSize(win, dm.w + 1, dm.h);
        g_GameWindow.screenWidth = dm.w;
        g_GameWindow.screenHeight = dm.h;
        SDL_ShowCursor(SDL_DISABLE);
    }

    SDL_PumpEvents();
    const int preferredSwapInterval = GetPreferredSwapInterval(windowed);
    SDL_GL_SetSwapInterval(preferredSwapInterval);
    g_Supervisor.vsyncEnabled = preferredSwapInterval > 0 ? 1 : 0;

    // Set real screen dimensions directly — don't rely on SDL_GL_GetDrawableSize
    // which may not be up to date yet on Windows after an async resize.
    if (windowed)
    {
        g_Renderer->realScreenWidth = GAME_WINDOW_WIDTH;
        g_Renderer->realScreenHeight = GAME_WINDOW_HEIGHT;
    }
    else
    {
        // Use the known display resolution (match the borderless window we just created)
        SDL_DisplayMode dm;
        SDL_GetDesktopDisplayMode(0, &dm);
        g_Renderer->realScreenWidth = dm.w + 1;
        g_Renderer->realScreenHeight = dm.h;
    }

    // Lightweight resize: only recreate FBO and update screen dimensions.
    g_Renderer->ResizeTarget();

    // Reset AnmManager caches so next draw call re-applies all state
    if (g_AnmManager != NULL)
    {
        g_AnmManager->currentBlendMode = 0xff;
        g_AnmManager->currentColorOp = 0xff;
        g_AnmManager->currentVertexShader = 0xff;
        g_AnmManager->currentTexture = 0;
    }
    g_Stage.skyFogNeedsSetup = 1;

    UpdateWindowTitle();
}

#pragma var_order(res, viewport, slowdown, local_34, delta, curtime)
RenderResult GameWindow::Render()
{
    Uint32 _renderStartMs = SDL_GetTicks();
    Uint32 _drawCostMs = 0;
    Uint32 _calcCostMs = 0;
    i32 res;
    f64 slowdown;
    D3DVIEWPORT8 viewport;
    f64 delta;
    u32 curtime;
    f64 local_34;

    if (this->lastActiveAppValue == 0 && ShouldFreezeWhenInactive())
    {
        // Frozen: keep frame-pacing baseline current so we don't catch-up
        // a huge burst of logic frames the moment we resume.
        g_LastFrameTime = SDL_GetTicks();
        return RENDER_RESULT_KEEP_RUNNING;
    }

    // Android: when the Activity is paused the SDL surface is destroyed but
    // SDL may dispatch APP_DIDENTERBACKGROUND followed immediately by
    // APP_WILLENTERFOREGROUND (observed on vivo PD2323), so lastActiveAppValue
    // bounces back to 1 before the surface is actually re-created. Calling
    // BeginFrame()/Present() against a dead surface hangs the main thread
    // (Vulkan vkAcquireNextImageKHR blocks indefinitely on Adreno). Bail
    // out for any backend whenever the window is currently un-presentable.
    if (IsWindowPresentationUnavailable())
    {
        g_LastFrameTime = SDL_GetTicks();
        return RENDER_RESULT_KEEP_RUNNING;
    }

    if (g_PendingRestart)
    {
        g_PendingRestart = false;
        AssetIO::DiagLog("GW", "Render: g_PendingRestart was true -> RENDER_RESULT_RESTART");
        return RENDER_RESULT_RESTART;
    }

    ApplyPendingWindowMode();

    if (this->curFrame == 0)
    {
    LOOP_USING_GOTO_BECAUSE_WHY_NOT:
        if (g_Supervisor.cfg.frameskipConfig <= this->curFrame)
        {
            const bool holdPausePresentation = Netplay::IsPausePresentationHoldActive();
            // Capture the screenshot BEFORE clearing/drawing the new frame.
            // The FBO still holds the previous frame's clean game scene.
            // This must happen before Clear/DrawChain so that the pause menu
            // background texture is filled with valid data before it is drawn.
            // Previously this lived in Present(), but frame-skipping could
            // skip Present() while still running DrawChain, causing the menu
            // background to show stale/undefined texture data ("texture overflow").
            if (!holdPausePresentation)
            {
                g_AnmManager->TakeScreenshotIfRequested();
                if (g_Supervisor.IsUnknown() || PortableGameplayRestore::ConsumeForcedClearFrameRequested() ||
                    PortableGameplayRestore::ShouldForceClearGameplayFrame())
                {
                    viewport.X = 0;
                    viewport.Y = 0;
                    viewport.Width = 640;
                    viewport.Height = 480;
                    viewport.MinZ = 0.0;
                    viewport.MaxZ = 1.0;
                    g_Renderer->SetViewport(viewport.X, viewport.Y, viewport.Width, viewport.Height, viewport.MinZ, viewport.MaxZ);
                    g_Renderer->Clear(g_Stage.skyFog.color, 1, 1);
                    g_Renderer->SetViewport(g_Supervisor.viewport.X, g_Supervisor.viewport.Y,
                                           g_Supervisor.viewport.Width, g_Supervisor.viewport.Height,
                                           g_Supervisor.viewport.MinZ, g_Supervisor.viewport.MaxZ);
                }
                g_Renderer->BeginScene();
                g_Chain.RunDrawChain();
                g_Renderer->EndScene();
                _drawCostMs = SDL_GetTicks() - _renderStartMs;
                g_Renderer->SetTexture(0);
                // Invalidate AnmManager's texture cache so it stays in sync
                // with the renderer.  Without this, the next frame's first
                // draw call may skip SetTexture (cache hit in AnmManager)
                // while the renderer/GL has a completely different texture
                // bound (e.g. the FBO color texture from EndFrame's blit),
                // causing the previous frame's scene to "leak" onto sprites.
                g_AnmManager->SetCurrentTexture(0);
                g_AnmManager->SetCurrentSprite(nullptr);
            }
        }

        g_Supervisor.viewport.X = 0;
        g_Supervisor.viewport.Y = 0;
        g_Supervisor.viewport.Width = 640;
        g_Supervisor.viewport.Height = 480;
        g_Renderer->SetViewport(0, 0, 640, 480);
        Uint32 _calcStart = SDL_GetTicks();
        res = g_Chain.RunCalcChain();
        _calcCostMs = SDL_GetTicks() - _calcStart;
        THPrac::THPracGuiUpdate();
        if (res == 0)
        {
            return RENDER_RESULT_EXIT_SUCCESS;
        }
        if (res == -1)
        {
            return RENDER_RESULT_EXIT_ERROR;
        }
        for (int rollbackCatchupFrames = 0; rollbackCatchupFrames < kRollbackCatchupCalcLimit &&
                                           Netplay::NeedsRollbackCatchup();
             ++rollbackCatchupFrames)
        {
            res = g_Chain.RunCalcChain();
            THPrac::THPracGuiUpdate();
            if (res == 0)
            {
                return RENDER_RESULT_EXIT_SUCCESS;
            }
            if (res == -1)
            {
                return RENDER_RESULT_EXIT_ERROR;
            }
        }
        if (!Netplay::NeedsRollbackCatchup())
        {
            g_SoundPlayer.PlaySounds();
        }
        this->curFrame++;
    }

    if (g_Supervisor.cfg.windowed != false || g_Supervisor.ShouldRunAt60Fps())
    {
        if (this->curFrame != 0)
        {
            g_Supervisor.framerateMultiplier = 1.0;
            slowdown = SDL_GetTicks();
            if (slowdown < g_LastFrameTime)
            {
                g_LastFrameTime = slowdown;
            }
            local_34 = fabs(slowdown - g_LastFrameTime);
            if (local_34 >= GetFrameTime())
            {
                // Cap catch-up: if the process was suspended (e.g. switching
                // to/from background, GC stall), the elapsed time can be huge
                // and the original do/while would burst-run many logic frames
                // in a single Render() call (visible as a brief speed-up).
                // Limit to at most kMaxCatchupFrames worth of catch-up; beyond
                // that, snap the baseline to "now".
                constexpr double kMaxCatchupFrames = 3.0;
                const double maxCatchup = GetFrameTime() * kMaxCatchupFrames;
                if (local_34 > maxCatchup)
                {
                    g_LastFrameTime = slowdown - GetFrameTime();
                    local_34 = GetFrameTime();
                }
                do
                {
                    g_LastFrameTime += GetFrameTime();
                    local_34 -= GetFrameTime();
                } while (local_34 >= GetFrameTime());

                if (g_Supervisor.cfg.frameskipConfig < this->curFrame)
                    goto I_HAVE_NO_CLUE_WHY_BUT_I_MUST_JUMP_HERE;
                goto LOOP_USING_GOTO_BECAUSE_WHY_NOT;
            }
        }
    }

    if (g_Supervisor.cfg.windowed == false && !g_Supervisor.ShouldRunAt60Fps())
    {

        if (g_Supervisor.cfg.frameskipConfig >= this->curFrame)
        {
            Present();
            goto LOOP_USING_GOTO_BECAUSE_WHY_NOT;
        }

    I_HAVE_NO_CLUE_WHY_BUT_I_MUST_JUMP_HERE:
        Present();
        // F11 → Unlock Frame Rate: bypass the original (refresh-rate-aware)
        // sub-frame multiplier logic. With throttling off the loop rate is
        // not a meaningful proxy for monitor refresh, and any cached
        // multiplier (set during MainMenu sampling) becomes stale and makes
        // game speed drift wildly. Force a 1:1 mapping (one logic tick per
        // loop iteration, no sub-stepping) so logic and rendering accelerate
        // together cleanly.
        if (THPrac::g_adv_igi_options.th06_unlock_framerate)
        {
            g_Supervisor.framerateMultiplier          = 1.0f;
            g_Supervisor.effectiveFramerateMultiplier = 1.0f;
            g_TickCountToEffectiveFramerate = 0;
        }
        else if (g_Supervisor.framerateMultiplier == 0.f)
        {
            if (2 <= g_TickCountToEffectiveFramerate)
            {
                curtime = SDL_GetTicks();
                if (curtime < g_Supervisor.lastFrameTime)
                {
                    g_Supervisor.lastFrameTime = curtime;
                }
                delta = curtime - g_Supervisor.lastFrameTime;
                delta = (delta * 60.) / 2. / 1000.;
                delta /= (g_Supervisor.cfg.frameskipConfig + 1);
                if (delta >= .865)
                {
                    delta = 1.0;
                }
                else if (delta >= .6)
                {
                    delta = 0.8;
                }
                else
                {
                    delta = 0.5;
                }
                g_Supervisor.effectiveFramerateMultiplier = delta;
                g_Supervisor.lastFrameTime = curtime;
                g_TickCountToEffectiveFramerate = 0;
            }
        }
        else
        {
            g_Supervisor.effectiveFramerateMultiplier = g_Supervisor.framerateMultiplier;
        }
        this->curFrame = 0;
        g_TickCountToEffectiveFramerate = g_TickCountToEffectiveFramerate + 1;
    }
    {
        Uint32 _renderTotal = SDL_GetTicks() - _renderStartMs;
        if (_renderTotal >= 100)
        {
            std::fprintf(stderr,
                "[gw/slow] Render total=%u ms (calc=%u draw=%u)\n",
                (unsigned)_renderTotal, (unsigned)_calcCostMs, (unsigned)_drawCostMs);
            std::fflush(stderr);
        }
    }
    return RENDER_RESULT_KEEP_RUNNING;
}

void GameWindow::Present()
{
    i32 unused;

    const bool runHeadlessWhenInactive = ShouldRunHeadlessWhenInactive();

    if (!runHeadlessWhenInactive)
    {
        g_Renderer->EndFrame();
        g_Renderer->Present(); // Phase 5a (ADR-008): explicit present after submit.

        // Software frame-rate limiter. Even when the swapchain provides
        // VSync we cannot rely on it: e.g. on Android the SurfaceView VSync
        // signal is suspended/unblocked while the activity is being swiped
        // to background, which used to let the game burst to ~90fps for a
        // moment. Keep the SDL_Delay-based limiter active on every backend.
        // Honor the F11 advanced-tab "Unlock Frame Rate" toggle so users can
        // benchmark the renderer with throttling fully disabled.
        const bool unlockFps = THPrac::g_adv_igi_options.th06_unlock_framerate;
        if (g_GameWindow.screenWidth != 0 && !unlockFps)
        {
            static u32 s_lastPresentTime = 0;
            static double s_frameBudget = 0.0;
            u32 curTime = SDL_GetTicks();
            if (s_lastPresentTime != 0)
            {
                float speedMul = Session::IsRemoteNetplaySession() ? 1.0f : THPrac::THPracGetSpeedMultiplier();
                double targetMs = (speedMul > 0.01f) ? (FRAME_TIME / speedMul) : FRAME_TIME;
                u32 elapsed = curTime - s_lastPresentTime;
                s_frameBudget += targetMs - (double)elapsed;
                // Clamp to prevent budget from drifting too far during lag
                if (s_frameBudget < -targetMs)
                    s_frameBudget = -targetMs;
                else if (s_frameBudget > targetMs)
                    s_frameBudget = targetMs;
                if (s_frameBudget > 0.0)
                {
                    u32 delayMs = (u32)s_frameBudget;
                    if (delayMs > 0)
                        SDL_Delay(delayMs);
                }
            }
            // Record time BEFORE any delay — so the next frame's 'elapsed'
            // measures the full frame period (draw + calc + previous delay),
            // not just the draw+calc portion.
            s_lastPresentTime = curTime;
        }
    }
    if (g_Supervisor.unk198 != 0)
    {
        g_Supervisor.unk198--;
    }

    if (!runHeadlessWhenInactive)
    {
        // Begin next frame (binds FBO)
        g_Renderer->BeginFrame();
        // Invalidate AnmManager's render-state caches so the first draw of
        // the new frame always re-applies GL state.  EndFrame changes GL
        // state (FBO blit, state restore) behind AnmManager's back, so
        // stale caches here would cause the wrong texture / blend mode to
        // be used for sprites whose state happens to match the previous
        // frame's last draw.
        if (g_AnmManager != nullptr)
        {
            g_AnmManager->InvalidateDrawCaches();
        }
    }
    return;
}

i32 GameWindow::InitD3dInterface(void)
{
#ifdef __ANDROID__
    SDL_SetHint(SDL_HINT_ACCELEROMETER_AS_JOYSTICK, "0");
    // NOTE: Leave SDL_HINT_TOUCH_MOUSE_EVENTS at default (synthesized) so
    // ImGui (which only listens to mouse events) can still receive touch
    // input on thprac panels. The risk that ImGui's WantCaptureMouse gets
    // stuck at 1 is mitigated by:
    //   1) GameWindow's SDL_FINGERUP dispatch never being filtered by
    //      WantCap (see event loop below).
    //   2) GameWindow's DN/MOTION dispatch bypassing the WantCap filter
    //      whenever isInMenu != 0 (gameplay).
    //   3) AndroidTouchInput::Update() force-clears ImGui's MouseDown[]
    //      on stall recovery and when no fingers are physically down.
#endif
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER | SDL_INIT_JOYSTICK) < 0)
    {
        GameErrorContext::Fatal(&g_GameErrorContext, TH_ERR_D3D_ERR_COULD_NOT_CREATE_OBJ);
        return 1;
    }
    AndroidTouchInput::Init();
    return 0;
}

void GameWindow::CreateGameWindow(void *unused)
{
    u32 windowFlags;
    i32 width;
    i32 height;

    g_GameWindow.lastActiveAppValue = 0;
    g_GameWindow.isAppActive = 0;

    // Phase 5b (ADR-008/010): use SDL_WINDOW_VULKAN for Vulkan backend; skip
    // GL attribute setup and SDL_GL_CreateContext below.
    const bool useVulkan = IsUsingVulkan();
    const u32 backendFlag = useVulkan ? SDL_WINDOW_VULKAN : SDL_WINDOW_OPENGL;

    if (!useVulkan)
    {
        SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
        SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
        SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
        SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 8);
        SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 16);
        SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    }

#if defined(TH06_USE_GLES)
#ifdef __ANDROID__
    // On Android, request a real GLES 2.0 context.
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
#else
    // On desktop (Windows/Linux), request a GL 2.0+ compatibility context
    // so we can use the same shader code (attribute/varying/texture2D).
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_COMPATIBILITY);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
#endif
#endif

#ifdef __ANDROID__
    // On Android, SDL manages the window via SDLActivity.
    // Use SDL_WINDOW_FULLSCREEN to let SDL handle the native surface.
    windowFlags = backendFlag | SDL_WINDOW_FULLSCREEN;
    g_GameWindow.sdlWindow = SDL_CreateWindow(
        "Touhou 06", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
        0, 0, windowFlags);
#else
    if (g_Supervisor.cfg.windowed == 0)
    {
        // Create a borderless window covering the screen.
        // Width is +1 pixel (overflows off right edge, invisible) so the OS
        // does not recognize it as an exact-screen-size fullscreen window,
        // preventing fullscreen optimizations that break screen capture
        // and block third-party overlay windows.
        SDL_DisplayMode dm;
        SDL_GetDesktopDisplayMode(0, &dm);
        windowFlags = backendFlag | SDL_WINDOW_SHOWN | SDL_WINDOW_BORDERLESS;
        g_GameWindow.sdlWindow = SDL_CreateWindow(
            "Touhou 06", 0, 0,
            dm.w + 1, dm.h, windowFlags);
    }
    else
    {
        windowFlags = backendFlag | SDL_WINDOW_SHOWN;
        g_GameWindow.sdlWindow = SDL_CreateWindow(
            "Touhou 06", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
            GAME_WINDOW_WIDTH, GAME_WINDOW_HEIGHT, windowFlags);
    }
#endif

    g_Supervisor.hwndGameWindow = (HWND)g_GameWindow.sdlWindow;

#ifdef __ANDROID__
    // SDL2 on Android starts text input by default after window creation,
    // which causes the soft keyboard (IME) to pop up — particularly visible
    // when returning from background. We never use SDL_TEXTINPUT here, so
    // disable it to keep the IME hidden.
    //
    // Desktop note: we deliberately do NOT call SDL_StopTextInput on
    // Windows/Linux/macOS. SDL's text-input state on desktop drives the IME
    // composition window (CJK/EastAsian users); forcibly disabling it would
    // break legitimate IME usage in any future text widget and has no
    // benefit on desktop where there's no on-screen keyboard popping up.
    SDL_StopTextInput();
#endif
}

void GameWindow_ProcessEvents()
{
    SDL_Event event;
    while (SDL_PollEvent(&event))
    {
#ifdef __ANDROID__
        LogAndroidInputEvent(event);
#endif
        THPrac::THPracGuiProcessEvent(&event);
        switch (event.type)
        {
        case SDL_QUIT:
            g_GameWindow.isAppClosing = 1;
            break;
        case SDL_WINDOWEVENT:
            if (event.window.event == SDL_WINDOWEVENT_FOCUS_GAINED ||
                event.window.event == SDL_WINDOWEVENT_RESTORED ||
                event.window.event == SDL_WINDOWEVENT_SHOWN)
            {
                g_GameWindow.lastActiveAppValue = 1;
                g_GameWindow.isAppActive = 0;
                Controller::ResetInputState();
            }
            else if (event.window.event == SDL_WINDOWEVENT_FOCUS_LOST ||
                     event.window.event == SDL_WINDOWEVENT_MINIMIZED ||
                     event.window.event == SDL_WINDOWEVENT_HIDDEN)
            {
                g_GameWindow.lastActiveAppValue = ShouldFreezeWhenInactive() ? 0 : 1;
                g_GameWindow.isAppActive = ShouldFreezeWhenInactive() ? 1 : 0;
                Controller::ResetInputState();
            }
            else if (event.window.event == SDL_WINDOWEVENT_SIZE_CHANGED)
            {
                // Android pause/resume + IME show/hide repeatedly fires
                // SIZE_CHANGED with different SurfaceView dimensions (e.g.
                // 2696x1260 -> 2696x1127 when soft keyboard is up). We must
                //   1) refresh realScreen* (used for touch/pillarbox math)
                //   2) trigger a swapchain rebuild on Vulkan, otherwise the
                //      driver stretch-blits the old swapchain image into the
                //      new SurfaceView rect, producing the "wrong scaling on
                //      resume" symptom.
                if (g_Renderer != nullptr)
                {
                    int drawW = 0, drawH = 0;
#ifdef TH06_USE_VULKAN
                    if (IsUsingVulkan()) {
                        SDL_Vulkan_GetDrawableSize(g_GameWindow.sdlWindow, &drawW, &drawH);
                    } else
#endif
                    {
                        SDL_GL_GetDrawableSize(g_GameWindow.sdlWindow, &drawW, &drawH);
                    }
                    g_Renderer->realScreenWidth  = drawW;
                    g_Renderer->realScreenHeight = drawH;
                    g_Renderer->ResizeTarget();
#ifdef __ANDROID__
                    // Track maximum drawable height observed for this window
                    // (== full SurfaceView height, no IME). When the IME
                    // pops up, Android shrinks the SurfaceView (we set
                    // android:windowSoftInputMode=adjustResize) so the diff
                    // is the keyboard height in physical pixels.
                    static int s_maxDrawHPx = 0;
                    if (drawH > s_maxDrawHPx) s_maxDrawHPx = drawH;
                    int inset = s_maxDrawHPx - drawH;
                    if (inset < 0) inset = 0;
                    // Tiny diffs (status bar quirks) are not the IME.
                    if (inset < 64) inset = 0;
                    g_AndroidImeInsetPx = inset;
#endif
                    std::fprintf(stderr,
                        "[input/sdl] SIZE_CHANGED -> drawable=%dx%d (window event data=%d,%d) imeInset=%d\n",
                        drawW, drawH, event.window.data1, event.window.data2,
                        g_AndroidImeInsetPx);
                }
            }
            break;
        case SDL_APP_WILLENTERBACKGROUND:
        case SDL_APP_DIDENTERBACKGROUND:
            g_GameWindow.lastActiveAppValue = ShouldFreezeWhenInactive() ? 0 : 1;
            g_GameWindow.isAppActive = ShouldFreezeWhenInactive() ? 1 : 0;
            Controller::ResetInputState();
            break;
        case SDL_APP_WILLENTERFOREGROUND:
        case SDL_APP_DIDENTERFOREGROUND:
            g_GameWindow.lastActiveAppValue = 1;
            g_GameWindow.isAppActive = 0;
            Controller::ResetInputState();
#ifdef __ANDROID__
            // Suppress IME on resume — SDL re-enables text input internally.
            // Android-only: see the matching note in CreateGameWindow().
            SDL_StopTextInput();
#endif
            break;
        case SDL_JOYDEVICEADDED:
        case SDL_JOYDEVICEREMOVED:
        case SDL_CONTROLLERDEVICEADDED:
        case SDL_CONTROLLERDEVICEREMOVED:
            Controller::RefreshSDLController();
            Controller::ResetInputState();
            break;
        case SDL_FINGERDOWN:
        {
            // Bypass the ImGui-WantCaptureMouse filter during actual gameplay
            // (`isInMenu != 0` paradoxically means "playing", not in menu).
            // ImGui can falsely hold WantCaptureMouse=1 if a touch landed on
            // a thprac panel and the matching UP got swallowed; without this
            // bypass the player would lose all touch control. ImGui still
            // receives the event independently via THPracGuiProcessEvent.
            const bool wantCap = THPrac::THPracGuiIsReady() && ImGui::GetIO().WantCaptureMouse;
            const bool inGameplay = (g_GameManager.isInMenu != 0);
            const bool deliver = !wantCap || inGameplay;
            const Uint32 nowTicks = SDL_GetTicks();
            const int lagMs = (int)((Sint64)nowTicks - (Sint64)event.tfinger.timestamp);
            AndroidTouchInput::DiagLog("touch/sdl",
                "DN  fid=%lld ts=%u now=%u lag=%dms nx=%.3f ny=%.3f p=%.2f guiReady=%d wantCap=%d inGameplay=%d FILTERED=%d",
                (long long)event.tfinger.fingerId, event.tfinger.timestamp,
                nowTicks, lagMs,
                event.tfinger.x, event.tfinger.y, event.tfinger.pressure,
                THPrac::THPracGuiIsReady() ? 1 : 0,
                ImGui::GetIO().WantCaptureMouse ? 1 : 0,
                inGameplay ? 1 : 0,
                deliver ? 0 : 1);
            if (deliver)
                AndroidTouchInput::HandleFingerDown(event.tfinger);
            break;
        }
        case SDL_FINGERMOTION:
        {
            const bool wantCap = THPrac::THPracGuiIsReady() && ImGui::GetIO().WantCaptureMouse;
            const bool inGameplay = (g_GameManager.isInMenu != 0);
            const bool deliver = !wantCap || inGameplay;
            const Uint32 nowTicks = SDL_GetTicks();
            const int lagMs = (int)((Sint64)nowTicks - (Sint64)event.tfinger.timestamp);
            AndroidTouchInput::DiagLog("touch/sdl",
                "MV  fid=%lld ts=%u now=%u lag=%dms nx=%.3f ny=%.3f dx=%.3f dy=%.3f wantCap=%d inGameplay=%d FILTERED=%d",
                (long long)event.tfinger.fingerId, event.tfinger.timestamp,
                nowTicks, lagMs,
                event.tfinger.x, event.tfinger.y, event.tfinger.dx, event.tfinger.dy,
                ImGui::GetIO().WantCaptureMouse ? 1 : 0,
                inGameplay ? 1 : 0,
                deliver ? 0 : 1);
            if (deliver)
                AndroidTouchInput::HandleFingerMotion(event.tfinger);
            break;
        }
        case SDL_FINGERUP:
        {
            const Uint32 nowTicks = SDL_GetTicks();
            const int lagMs = (int)((Sint64)nowTicks - (Sint64)event.tfinger.timestamp);
            AndroidTouchInput::DiagLog("touch/sdl",
                "UP  fid=%lld ts=%u now=%u lag=%dms nx=%.3f ny=%.3f FILTERED=%d",
                (long long)event.tfinger.fingerId, event.tfinger.timestamp,
                nowTicks, lagMs,
                event.tfinger.x, event.tfinger.y,
                0);
            // FINGERUP is ALWAYS delivered to the game pipeline, even when
            // ImGui has WantCaptureMouse=1. Filtering UP creates a feedback
            // loop where ImGui receives DN (sets WantCap=1), then we filter
            // UP, ImGui never sees the release, WantCap stays stuck at 1
            // forever, and every subsequent finger is dropped → "自机
            // 完全不响应触控". HandleFingerUp must run so we can clean up
            // our own pointer state and movement finger.
            AndroidTouchInput::HandleFingerUp(event.tfinger);
        }
            break;
#ifndef __ANDROID__
        case SDL_MOUSEBUTTONDOWN:
            if (AndroidTouchInput::IsEnabled())
            {
                // Right-click (game back/cancel) always goes through, bypassing ImGui.
                // Left-click respects ImGui capture for thprac UI interaction.
                if (event.button.button == SDL_BUTTON_RIGHT || !THPrac::THPracGuiIsReady() || !ImGui::GetIO().WantCaptureMouse)
                {
                    int w, h;
                    SDL_GetWindowSize(g_GameWindow.sdlWindow, &w, &h);
                    AndroidTouchInput::HandleMouseButtonDown(event.button, w, h);
                }
            }
            break;
        case SDL_MOUSEMOTION:
            if (AndroidTouchInput::IsEnabled())
            {
                // Always process during left-button drag (prevents lost updates).
                // Otherwise respect ImGui capture.
                if ((event.motion.state & SDL_BUTTON_LMASK) || !THPrac::THPracGuiIsReady() || !ImGui::GetIO().WantCaptureMouse)
                {
                    int w, h;
                    SDL_GetWindowSize(g_GameWindow.sdlWindow, &w, &h);
                    AndroidTouchInput::HandleMouseMotion(event.motion, w, h);
                }
            }
            break;
        case SDL_MOUSEBUTTONUP:
            if (AndroidTouchInput::IsEnabled())
            {
                // Always process button-up to prevent stuck drag state.
                int w, h;
                SDL_GetWindowSize(g_GameWindow.sdlWindow, &w, &h);
                AndroidTouchInput::HandleMouseButtonUp(event.button, w, h);
            }
            break;
#endif
        default:
            break;
        }
    }

    AndroidTouchInput::Update();
}

#pragma var_order(using_d3d_hal, display_mode, present_params, camera_distance, half_height, half_width, aspect_ratio, \
                  field_of_view_y, up, at, eye, should_run_at_60_fps)
i32 GameWindow::InitD3dRendering(void)
{
    D3DXVECTOR3 eye;
    D3DXVECTOR3 at;
    D3DXVECTOR3 up;
    float half_width;
    float half_height;
    float aspect_ratio;
    float field_of_view_y;
    float camera_distance;

    if (!g_Supervisor.cfg.windowed)
    {
        SDL_DisplayMode dm;
        SDL_GetCurrentDisplayMode(0, &dm);
        g_GameWindow.screenWidth = dm.w;
        g_GameWindow.screenHeight = dm.h;
    }
    else
    {
        g_GameWindow.screenWidth = 0;
        g_GameWindow.screenHeight = 0;
    }

    SDL_GLContext glCtx = NULL;
    if (!IsUsingVulkan())
    {
        glCtx = SDL_GL_CreateContext(g_GameWindow.sdlWindow);
        if (glCtx == NULL)
        {
            GameErrorContext::Fatal(&g_GameErrorContext, TH_ERR_D3D_INIT_FAILED);
            return 1;
        }

        const int preferredSwapInterval = GetPreferredSwapInterval(g_Supervisor.cfg.windowed != 0);
        SDL_GL_SetSwapInterval(preferredSwapInterval);
        g_Supervisor.vsyncEnabled = preferredSwapInterval > 0 ? 1 : 0;
    }
    else
    {
        // Vulkan: vsync controlled by VK_PRESENT_MODE_FIFO_RELAXED in RendererVulkan.
        g_Supervisor.vsyncEnabled = 1;
    }

    // Select renderer based on Phase 5b CLI selection (g_SelectedBackend),
    // falling back to legacy persisted config (cfg.unk[0]) only when CLI absent.
    // Phase 5b.3: unified desktop+Android path. GL fixed-function backend is
    // desktop-only (uses glBegin etc., absent on GLES); Vulkan is gated on
    // TH06_USE_VULKAN and works on whichever platforms link it in.
    switch (g_SelectedBackend)
    {
#ifdef TH06_USE_VULKAN
        case BackendKind::Vulkan: g_Renderer = GetRendererVulkan(); break;
#endif
        case BackendKind::GLES:   g_Renderer = GetRendererGLES();   break;
#ifndef __ANDROID__
        case BackendKind::GL:     g_Renderer = GetRendererGL();     break;
#endif
        default:
            // cfg.unk[0]: 0/0xFF=GLES, 1=GL, 2=Vulkan (Phase 5b.2 in-game switcher)
            switch (g_Supervisor.cfg.unk[0])
            {
#ifndef __ANDROID__
                case 1:  g_Renderer = GetRendererGL();      g_SelectedBackend = BackendKind::GL;     break;
#endif
#ifdef TH06_USE_VULKAN
                case 2:  g_Renderer = GetRendererVulkan();  g_SelectedBackend = BackendKind::Vulkan; break;
#endif
                default: g_Renderer = GetRendererGLES();    g_SelectedBackend = BackendKind::GLES;   break;
            }
            break;
    }
    g_Renderer->Init(g_GameWindow.sdlWindow, glCtx, GAME_WINDOW_WIDTH, GAME_WINDOW_HEIGHT);
    UpdateWindowTitle();

    // Phase 5b.2: ImGui SDL2 backend now supports both GL and Vulkan.
    // THPracGuiInit branches on backend internally (Vulkan path uses
    // RendererVulkan::InitImGui; GL path uses ImGui_ImplOpenGL{2,3}_Init).
    // glCtx will be NULL on the Vulkan path and is unused there.
    THPrac::THPracGuiInit(g_GameWindow.sdlWindow, glCtx);

    g_Supervisor.lockableBackbuffer = 1;
    g_Supervisor.hasD3dHardwareVertexProcessing = 1;
    g_Supervisor.colorMode16Bits = 1;

    half_width = (float)GAME_WINDOW_WIDTH / 2.0;
    half_height = (float)GAME_WINDOW_HEIGHT / 2.0;
    aspect_ratio = (float)GAME_WINDOW_WIDTH / (float)GAME_WINDOW_HEIGHT;
    field_of_view_y = 0.52359879f;
    camera_distance = half_height / tanf(field_of_view_y / 2.0f);
    up.x = 0.0;
    up.y = 1.0;
    up.z = 0.0;
    at.x = half_width;
    at.y = -half_height;
    at.z = 0.0;
    eye.x = half_width;
    eye.y = -half_height;
    eye.z = -camera_distance;
    D3DXMatrixLookAtLH(&g_Supervisor.viewMatrix, &eye, &at, &up);
    D3DXMatrixPerspectiveFovLH(&g_Supervisor.projectionMatrix, field_of_view_y, aspect_ratio, 100.0, 10000.0);

    g_Renderer->viewMatrix = g_Supervisor.viewMatrix;
    g_Renderer->projectionMatrix = g_Supervisor.projectionMatrix;

    g_Supervisor.viewport.X = 0;
    g_Supervisor.viewport.Y = 0;
    g_Supervisor.viewport.Width = GAME_WINDOW_WIDTH;
    g_Supervisor.viewport.Height = GAME_WINDOW_HEIGHT;
    g_Supervisor.viewport.MinZ = 0.0f;
    g_Supervisor.viewport.MaxZ = 1.0f;

    InitD3dDevice();
    ScreenEffect::SetViewport(0);
    g_GameWindow.isAppClosing = 0;
    g_Supervisor.lastFrameTime = 0;
    g_Supervisor.framerateMultiplier = 0.0;
    return 0;
}

#pragma var_order(fogVal, fogDensity, anm1, anm2, anm3, anm4)
void GameWindow::InitD3dDevice(void)
{
    g_Renderer->InitDevice(g_Supervisor.cfg.opts);

    if (g_AnmManager != NULL)
    {
        g_AnmManager->currentBlendMode = 0xff;
        g_AnmManager->currentColorOp = 0xff;
        g_AnmManager->currentVertexShader = 0xff;
        g_AnmManager->currentTexture = NULL;
    }
    g_Stage.skyFogNeedsSetup = 1;

    return;
}

bool IsUsingGLES()
{
    return g_Renderer == GetRendererGLES();
}

bool IsUsingVulkan()
{
#ifdef TH06_USE_VULKAN
    return g_SelectedBackend == BackendKind::Vulkan;
#else
    return false;
#endif
}

void UpdateWindowTitle()
{
    if (IsUsingVulkan())
    {
        SDL_SetWindowTitle(g_GameWindow.sdlWindow, "Touhou 06 [Vulkan]");
        return;
    }
    const char *glVer = (const char *)glGetString(GL_VERSION);
    const char *glRen = (const char *)glGetString(GL_RENDERER);
    const char *backendName = IsUsingGLES() ? "GLES (Shader)" : "GL (Fixed-Function)";
    char title[256];
    snprintf(title, sizeof(title), "Touhou 06 [%s | %s | %s]",
             backendName,
             glVer ? glVer : "?",
             glRen ? glRen : "?");
    SDL_SetWindowTitle(g_GameWindow.sdlWindow, title);
}

void SwitchRenderer(bool useGLES)
{
    // Phase 5b (ADR-008): runtime hot-switch is GL/GLES only. Vulkan is
    // start-time-selected; switching to/from Vulkan requires full resource
    // teardown + ImGui backend swap (not supported in 5b.1).
    if (IsUsingVulkan())
    {
        static bool warned = false;
        if (!warned) {
            std::fprintf(stderr,
                "[GameWindow] SwitchRenderer ignored: Vulkan backend cannot hot-switch.\n");
            warned = true;
        }
        return;
    }
#ifdef __ANDROID__
    IRenderer *newRenderer = GetRendererGLES();
#else
    IRenderer *newRenderer = useGLES ? GetRendererGLES() : GetRendererGL();
#endif
    if (newRenderer == g_Renderer)
        return;

    SDL_Window *win = g_GameWindow.sdlWindow;
    SDL_GLContext ctx = g_Renderer->glContext;

    g_Renderer = newRenderer;
    g_Renderer->Init(win, ctx, GAME_WINDOW_WIDTH, GAME_WINDOW_HEIGHT);
    g_Renderer->InitDevice(g_Supervisor.cfg.opts);

    // Re-apply transforms
    g_Renderer->viewMatrix = g_Supervisor.viewMatrix;
    g_Renderer->projectionMatrix = g_Supervisor.projectionMatrix;
    g_Renderer->SetViewTransform(&g_Supervisor.viewMatrix);
    g_Renderer->SetProjectionTransform(&g_Supervisor.projectionMatrix);

    // Reset AnmManager caches so next draw call re-applies all state
    if (g_AnmManager != NULL)
    {
        g_AnmManager->currentBlendMode = 0xff;
        g_AnmManager->currentColorOp = 0xff;
        g_AnmManager->currentVertexShader = 0xff;
        g_AnmManager->currentTexture = 0;
    }
    g_Stage.skyFogNeedsSetup = 1;

    UpdateWindowTitle();
}

void SetWindowMode(bool windowed)
{
    g_PendingWindowModeChange = true;
    g_PendingWindowModeWindowed = windowed;
}

void RequestRestart()
{
    g_PendingRestart = true;
    AssetIO::DiagLog("GW", "RequestRestart() called");
}

}; // namespace th06
