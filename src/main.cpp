#include "sdl2_compat.hpp"
#include <SDL.h>
#include <float.h>
#include <stdio.h>
#include <string.h>

#ifdef __ANDROID__
#include <android/log.h>
#include <pthread.h>
#include <unistd.h>
#include <string>
#endif

#include "AnmManager.hpp"
#include "Chain.hpp"
#include "Controller.hpp"
#include "CrashHandler.hpp"
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

namespace th06 {

#ifdef __ANDROID__
BackendKind g_SelectedBackend = BackendKind::GLES;
#else
BackendKind g_SelectedBackend = BackendKind::GL;
#endif

// Parse `--backend={gl|gles|vulkan}` (or `--backend gl` form). Returns the
// requested backend, or platform default if argument absent / malformed.
// Phase 5a (ADR-008): single CLI knob, no config-file fallback yet.
BackendKind SelectBackendFromCommandLine(int argc, char *argv[])
{
    BackendKind result = PlatformDefaultBackend();
    for (int i = 1; i < argc; ++i)
    {
        const char *a = argv[i];
        if (a == nullptr) continue;
        const char *value = nullptr;
        if (std::strncmp(a, "--backend=", 10) == 0)
        {
            value = a + 10;
        }
        else if (std::strcmp(a, "--backend") == 0 && (i + 1) < argc)
        {
            value = argv[++i];
        }
        if (value == nullptr) continue;

        if (SDL_strcasecmp(value, "gl") == 0)            result = BackendKind::GL;
        else if (SDL_strcasecmp(value, "gles") == 0)     result = BackendKind::GLES;
        else if (SDL_strcasecmp(value, "vulkan") == 0)   result = BackendKind::Vulkan;
        else if (SDL_strcasecmp(value, "vk") == 0)       result = BackendKind::Vulkan;
        else
        {
            std::fprintf(stderr, "[main] Unknown --backend=%s; using default.\n", value);
        }
    }
    return result;
}

} // namespace th06

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

#ifdef __ANDROID__
    // Pipe stderr to logcat. Without this, every fprintf(stderr, ...) in the
    // engine (backend probe results, Vulkan init steps, swapchain capabilities,
    // texture upload errors, etc.) is silently dropped on Android, leaving us
    // blind when something goes wrong post-install. Using a background thread
    // that reads from a pipe attached to STDERR_FILENO and forwards each line
    // to __android_log_write under the "th06-stderr" tag.
    {
        static int s_stderrPipe[2] = { -1, -1 };
        if (pipe(s_stderrPipe) == 0)
        {
            ::dup2(s_stderrPipe[1], STDERR_FILENO);
            ::setvbuf(stderr, nullptr, _IOLBF, 0);  // line-buffer
            pthread_t tid;
            pthread_create(&tid, nullptr, [](void*) -> void* {
                char buf[1024];
                ssize_t n;
                std::string acc;
                while ((n = ::read(s_stderrPipe[0], buf, sizeof(buf) - 1)) > 0)
                {
                    acc.append(buf, n);
                    size_t pos;
                    while ((pos = acc.find('\n')) != std::string::npos)
                    {
                        std::string line = acc.substr(0, pos);
                        acc.erase(0, pos + 1);
                        __android_log_write(ANDROID_LOG_INFO, "th06-stderr",
                                            line.c_str());
                    }
                }
                if (!acc.empty())
                    __android_log_write(ANDROID_LOG_INFO, "th06-stderr", acc.c_str());
                return nullptr;
            }, nullptr);
            pthread_detach(tid);
        }
        std::fprintf(stderr, "[android] stderr -> logcat bridge installed\n");
    }
#endif

#ifdef _WIN32
    // Make the game portable: when launched via Explorer / shortcut / file
    // association the CWD is not guaranteed to be the exe directory, which
    // breaks all relative paths (PBG3 archives, th06.cfg, log.txt, etc.).
    // Force CWD to the exe's directory so double-clicking works the same as
    // `cd build_vk && th06.exe`.
    {
        wchar_t exePath[MAX_PATH];
        DWORD n = GetModuleFileNameW(NULL, exePath, MAX_PATH);
        if (n > 0 && n < MAX_PATH)
        {
            for (DWORD i = n; i > 0; --i)
            {
                if (exePath[i - 1] == L'\\' || exePath[i - 1] == L'/')
                {
                    exePath[i - 1] = L'\0';
                    break;
                }
            }
            SetCurrentDirectoryW(exePath);
        }
    }
#endif

#ifdef _WIN32
    timeBeginPeriod(1);
#endif

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

    // Phase 5b.3: probe backend availability once (after SDL_Init on Android,
    // first thing on desktop). Establishes whether Vulkan / GL / GLES are
    // usable on this device so subsequent selectors can clamp.
    th06::ProbeBackendAvailability();

    // Phase 5b (ADR-008/010): parse --backend and clamp to an available backend.
    // CLI request → ResolveBackend → never returns an unavailable kind.
    th06::g_SelectedBackend = th06::ResolveBackend(
        th06::SelectBackendFromCommandLine(argc, argv));
    if (th06::g_SelectedBackend == th06::BackendKind::Vulkan)
    {
        std::fprintf(stderr, "[main] backend=vulkan (Phase 5b.2 — ImGui Vulkan enabled).\n");
    }

    GamePaths::Init();

    CrashHandler::Init();

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
    // Phase 5b.2: Vulkan needs SDL_WINDOW_VULKAN at window-creation time,
    // which is decided by IsUsingVulkan() == (g_SelectedBackend==Vulkan).
    // GL ↔ GLES share SDL_WINDOW_OPENGL, so the legacy game-native restart
    // route in GameWindow.cpp's renderer switch (which reads cfg.unk[0])
    // already handles those without main.cpp doing anything.
    //
    // Only intervene when there is a Vulkan / non-Vulkan mismatch between
    // the current g_SelectedBackend and the persisted cfg.unk[0]; otherwise
    // leave g_SelectedBackend alone so the original flow stays intact.
    //
    // Phase 5b.3: also clamp through ResolveBackend so an unavailable backend
    // (e.g. Vulkan persisted on a device without a Vulkan loader) silently
    // falls back to GLES instead of crashing window creation.
    {
        const th06::BackendKind kPlatformDefault = th06::PlatformDefaultBackend();
        const bool cfgWantsVulkan = (g_Supervisor.cfg.unk[0] == 2);
        const bool selWantsVulkan = (th06::g_SelectedBackend == th06::BackendKind::Vulkan);
        if (cfgWantsVulkan && !selWantsVulkan)
        {
            th06::g_SelectedBackend = th06::BackendKind::Vulkan;
        }
        else if (!cfgWantsVulkan && selWantsVulkan)
        {
            th06::g_SelectedBackend = kPlatformDefault;
        }
        // Final clamp: if the resolved backend isn't actually available on
        // this device, fall back (Vulkan → platform default → GLES). Also
        // rewrite cfg.unk[0] so the next restart doesn't re-trip this.
        th06::BackendKind resolved = th06::ResolveBackend(th06::g_SelectedBackend);
        if (resolved != th06::g_SelectedBackend)
        {
            std::fprintf(stderr,
                "[main] selected backend unavailable; clamped to platform fallback.\n");
            th06::g_SelectedBackend = resolved;
            // Map back to cfg.unk[0]: 1=GL, 2=Vulkan, 0/0xFF=GLES.
            switch (resolved)
            {
                case th06::BackendKind::GL:     g_Supervisor.cfg.unk[0] = 1; break;
                case th06::BackendKind::Vulkan: g_Supervisor.cfg.unk[0] = 2; break;
                default:                        g_Supervisor.cfg.unk[0] = 0; break;
            }
        }
    }

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
    // Phase 5b: skip ImGui shutdown + SDL_GL_DeleteContext on Vulkan path.
    if (!th06::IsUsingVulkan())
    {
        THPrac::THPracGuiShutdown();
    }
    {
        SDL_GLContext ctx = g_Renderer ? g_Renderer->glContext : nullptr;
        if (g_Renderer)
            g_Renderer->Release();
        if (ctx && !th06::IsUsingVulkan())
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
    CrashHandler::Shutdown();
    SDL_Quit();
#ifdef _WIN32
    timeEndPeriod(1);
#endif
    return 0;
}
