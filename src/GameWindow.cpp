#include "GameWindow.hpp"
#include "AnmManager.hpp"
#include "Controller.hpp"
#include "GameErrorContext.hpp"
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
#include <cmath>
#include <cstdio>

namespace th06
{
DIFFABLE_STATIC(GameWindow, g_GameWindow)
DIFFABLE_STATIC(i32, g_TickCountToEffectiveFramerate)
DIFFABLE_STATIC(f64, g_LastFrameTime)

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
    return windowed ? 1 : 0;
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
    i32 res;
    f64 slowdown;
    D3DVIEWPORT8 viewport;
    f64 delta;
    u32 curtime;
    f64 local_34;

    if (this->lastActiveAppValue == 0 && ShouldFreezeWhenInactive())
    {
        return RENDER_RESULT_KEEP_RUNNING;
    }

    if (g_PendingRestart)
    {
        g_PendingRestart = false;
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
                g_Renderer->SetTexture(0);
            }
        }

        g_Supervisor.viewport.X = 0;
        g_Supervisor.viewport.Y = 0;
        g_Supervisor.viewport.Width = 640;
        g_Supervisor.viewport.Height = 480;
        g_Renderer->SetViewport(0, 0, 640, 480);
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
            slowdown = timeGetTime();
            if (slowdown < g_LastFrameTime)
            {
                g_LastFrameTime = slowdown;
            }
            local_34 = fabs(slowdown - g_LastFrameTime);
            if (local_34 >= GetFrameTime())
            {
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
        if (g_Supervisor.framerateMultiplier == 0.f)
        {
            if (2 <= g_TickCountToEffectiveFramerate)
            {
                curtime = timeGetTime();
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
    return RENDER_RESULT_KEEP_RUNNING;
}

void GameWindow::Present()
{
    i32 unused;

    const bool runHeadlessWhenInactive = ShouldRunHeadlessWhenInactive();

    if (!runHeadlessWhenInactive)
    {
        g_Renderer->EndFrame();

 #ifndef __ANDROID__
        if (g_GameWindow.screenWidth != 0)
        {
            static u32 s_lastPresentTime = 0;
            u32 curTime = timeGetTime();
            if (s_lastPresentTime != 0)
            {
                float speedMul = Session::IsRemoteNetplaySession() ? 1.0f : THPrac::THPracGetSpeedMultiplier();
                u32 targetMs = (speedMul > 0.01f) ? (u32)(16.0f / speedMul) : 16;
                u32 elapsed = curTime - s_lastPresentTime;
                if (elapsed < targetMs)
                {
                    SDL_Delay(targetMs - elapsed);
                }
            }
            s_lastPresentTime = timeGetTime();
        }
 #endif
    }
    if (g_Supervisor.unk198 != 0)
    {
        g_Supervisor.unk198--;
    }

    if (!runHeadlessWhenInactive)
    {
        // Begin next frame (binds FBO)
        g_Renderer->BeginFrame();
    }
    return;
}

i32 GameWindow::InitD3dInterface(void)
{
#ifdef __ANDROID__
    SDL_SetHint(SDL_HINT_ACCELEROMETER_AS_JOYSTICK, "0");
#endif
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER | SDL_INIT_JOYSTICK) < 0)
    {
        GameErrorContext::Fatal(&g_GameErrorContext, TH_ERR_D3D_ERR_COULD_NOT_CREATE_OBJ);
        return 1;
    }
    return 0;
}

void GameWindow::CreateGameWindow(void *unused)
{
    u32 windowFlags;
    i32 width;
    i32 height;

    g_GameWindow.lastActiveAppValue = 0;
    g_GameWindow.isAppActive = 0;

    SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 16);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);

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
    windowFlags = SDL_WINDOW_OPENGL | SDL_WINDOW_FULLSCREEN;
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
        windowFlags = SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN | SDL_WINDOW_BORDERLESS;
        g_GameWindow.sdlWindow = SDL_CreateWindow(
            "Touhou 06", 0, 0,
            dm.w + 1, dm.h, windowFlags);
    }
    else
    {
        windowFlags = SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN;
        g_GameWindow.sdlWindow = SDL_CreateWindow(
            "Touhou 06", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
            GAME_WINDOW_WIDTH, GAME_WINDOW_HEIGHT, windowFlags);
    }
#endif

    g_Supervisor.hwndGameWindow = (HWND)g_GameWindow.sdlWindow;
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
            break;
        case SDL_JOYDEVICEADDED:
        case SDL_JOYDEVICEREMOVED:
        case SDL_CONTROLLERDEVICEADDED:
        case SDL_CONTROLLERDEVICEREMOVED:
            Controller::RefreshSDLController();
            Controller::ResetInputState();
            break;
        default:
            break;
        }
    }
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

    SDL_GLContext glCtx = SDL_GL_CreateContext(g_GameWindow.sdlWindow);
    if (glCtx == NULL)
    {
        GameErrorContext::Fatal(&g_GameErrorContext, TH_ERR_D3D_INIT_FAILED);
        return 1;
    }

    const int preferredSwapInterval = GetPreferredSwapInterval(g_Supervisor.cfg.windowed != 0);
    SDL_GL_SetSwapInterval(preferredSwapInterval);
    g_Supervisor.vsyncEnabled = preferredSwapInterval > 0 ? 1 : 0;

    // Select renderer based on persisted config (unk[0]: 0=GLES, 1=GL)
#ifdef __ANDROID__
    g_Renderer = GetRendererGLES();
#else
    g_Renderer = (g_Supervisor.cfg.unk[0] == 1) ? GetRendererGL() : GetRendererGLES();
#endif
    g_Renderer->Init(g_GameWindow.sdlWindow, glCtx, GAME_WINDOW_WIDTH, GAME_WINDOW_HEIGHT);
    UpdateWindowTitle();

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

void UpdateWindowTitle()
{
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
}

}; // namespace th06
