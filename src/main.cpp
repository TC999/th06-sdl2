#include "sdl2_compat.hpp"
#include <SDL.h>
#include <float.h>
#include <stdio.h>

#include "AnmManager.hpp"
#include "Chain.hpp"
#include "Controller.hpp"
#include "FileSystem.hpp"
#include "GameErrorContext.hpp"
#include "GamePaths.hpp"
#include "GameWindow.hpp"
#include "IRenderer.hpp"
#include "MidiOutput.hpp"
#include "NetplaySession.hpp"
#include "Session.hpp"
#include "SoundPlayer.hpp"
#include "Stage.hpp"
#include "Supervisor.hpp"
#include "TextHelper.hpp"
#include "WatchdogWin.hpp"
#include "ZunResult.hpp"
#include "i18n.hpp"
#include "thprac_gui_integration.h"
#include "thprac_th06.h"
#include "utils.hpp"

using namespace th06;

namespace
{
#if defined(_MSC_VER) && defined(_M_IX86)
void ConfigureReplayCompatibleFloatingPoint()
{
    unsigned int controlWord = 0;

    // Stock Touhou 6 replays are sensitive to the legacy x87 environment.
    // Modern MSVC/CRT startup can otherwise leave us with a different
    // precision path than the original VC7 build.
    _controlfp_s(&controlWord, _PC_24, _MCW_PC);
    _controlfp_s(&controlWord, _RC_NEAR, _MCW_RC);
    _clearfp();
}
#endif

bool PollManualDumpHotkey()
{
    static bool previousPressed = false;

    if (!THPrac::TH06::THPracIsManualDumpHotkeyEnabled())
    {
        previousPressed = false;
        return false;
    }

    const Uint8 *keyboardState = SDL_GetKeyboardState(nullptr);
    const bool ctrlPressed = keyboardState[SDL_SCANCODE_LCTRL] != 0 || keyboardState[SDL_SCANCODE_RCTRL] != 0;
    const bool dPressed = keyboardState[SDL_SCANCODE_D] != 0;
    const bool pressed = ctrlPressed && dPressed;
    const bool triggered = pressed && !previousPressed;
    previousPressed = pressed;
    return triggered;
}
} // namespace

int main(int argc, char *argv[])
{
    i32 renderResult = 0;

#if defined(_MSC_VER) && defined(_M_IX86)
    ConfigureReplayCompatibleFloatingPoint();
#endif

#ifdef __ANDROID__
    // On Android, SDL must be initialized before GamePaths::Init()
    // because SDL_AndroidGetInternalStoragePath() requires SDL_Init.
    if (SDL_Init(0) < 0)
    {
        return 1;
    }
#endif

    GamePaths::Init();

    if (g_Supervisor.LoadConfig(TH_CONFIG_FILE) != ZUN_SUCCESS)
    {
#ifdef __ANDROID__
        // On Android, config file may not exist on first run.
        // LoadConfig sets defaults and tries to write — if write fails,
        // continue anyway with defaults.
        SDL_Log("LoadConfig failed (first run?), continuing with defaults");
#else
        g_GameErrorContext.Flush();
        return -1;
#endif
    }

    if (GameWindow::InitD3dInterface())
    {
        g_GameErrorContext.Flush();
        return 1;
    }

    Session::UseLocalSession();

restart:
    GameWindow::CreateGameWindow(NULL);

    if (GameWindow::InitD3dRendering())
    {
        g_GameErrorContext.Flush();
        return 1;
    }

    g_SoundPlayer.InitializeDSound((HWND)g_GameWindow.sdlWindow);
    Controller::GetJoystickCaps();
    Controller::ResetKeyboard();

    g_AnmManager = new AnmManager();

    if (Supervisor::RegisterChain() != ZUN_SUCCESS)
    {
        goto stop;
    }
    if (!g_Supervisor.cfg.windowed)
    {
        SDL_ShowCursor(SDL_DISABLE);
    }

    g_GameWindow.curFrame = 0;
    WatchdogWin::Init();

    while (!g_GameWindow.isAppClosing)
    {
        WatchdogWin::TickHeartbeat();
        GameWindow_ProcessEvents();
        if (PollManualDumpHotkey())
        {
            WatchdogWin::RequestManualDump();
        }
#if defined(_MSC_VER) && defined(_M_IX86)
        ConfigureReplayCompatibleFloatingPoint();
#endif
        renderResult = g_GameWindow.Render();
        if (renderResult != 0)
        {
            goto stop;
        }
    }

stop:
    WatchdogWin::Shutdown();
    g_Chain.Release();
    g_SoundPlayer.Release();
    Netplay::Shutdown();

    delete g_AnmManager;
    g_AnmManager = NULL;

    // Clean up GL resources while the context is still valid.
    THPrac::THPracGuiShutdown();
    {
        SDL_GLContext ctx = g_Renderer ? g_Renderer->glContext : nullptr;
        if (g_Renderer)
            g_Renderer->Release();
        if (ctx)
            SDL_GL_DeleteContext(ctx);
    }

    SDL_DestroyWindow(g_GameWindow.sdlWindow);
    g_GameWindow.sdlWindow = NULL;

    if (renderResult == 2)
    {
        // Clean up resources that leak across restart cycles.
        // We cannot call Supervisor::DeletedCallback() here because
        // ReleasePbg3() has a built-in double-free (calls Release() then
        // delete which calls Release() again) that crashes on modern heaps.
        // PBG3 archives are re-released internally by LoadPbg3() on reload,
        // so only these three resources actually leak:
        if (g_Supervisor.midiOutput != NULL)
        {
            g_Supervisor.midiOutput->StopPlayback();
            delete g_Supervisor.midiOutput;
            g_Supervisor.midiOutput = NULL;
        }
        TextHelper::ReleaseTextBuffer();
        Controller::CloseSDLController();
        Netplay::Shutdown();

        g_GameErrorContext.ResetContext();

        GameErrorContext::Log(&g_GameErrorContext, TH_ERR_OPTION_CHANGED_RESTART);

        if (!g_Supervisor.cfg.windowed)
        {
            SDL_ShowCursor(SDL_ENABLE);
        }
        goto restart;
    }

    FileSystem::WriteDataToFile(TH_CONFIG_FILE, &g_Supervisor.cfg, sizeof(g_Supervisor.cfg));

    SDL_ShowCursor(SDL_ENABLE);
    g_GameErrorContext.Flush();
    SDL_Quit();
    return 0;
}
