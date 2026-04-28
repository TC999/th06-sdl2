#include "AndroidTouchInput.hpp"
#include "TouchVirtualButtons.hpp"
#include "MenuTouchButtons.hpp"

#include "IRenderer.hpp"
#include "GameWindow.hpp"
#include "Player.hpp"
#include "Supervisor.hpp"

#include <SDL.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdarg>
#include "imgui.h"

#include "GamePaths.hpp"
#include <cerrno>
#ifdef __ANDROID__
#include <sys/stat.h>
#endif

// Developer mode check (defined in thprac_th06.cpp).
namespace THPrac { namespace TH06 {
    bool THPracIsNewTouchEnabled();
    bool THPracIsMouseFollowEnabled();
    bool THPracIsMouseTouchDragEnabled();
}}
namespace THPrac {
    bool THPracGuiIsReady();
}

extern th06::IRenderer *g_Renderer;

namespace th06
{

// ────────────────────────────────────────────────────────────────────────────
// Persistent diagnostic logger
// ────────────────────────────────────────────────────────────────────────────
//
// All touch-pipeline events are written to a per-session file under
// `${GetUserPath()}/touch_diag/touch_<startTimeMs>.log` so the trace can be
// retrieved without `adb logcat`. Open lazily on first use; flush after every
// line so a crash/freeze still yields the most recent state.
//
// Each line has the form:
//     <ms>ms <tag> <message>
// where `ms` is `SDL_GetTicks()` at write time. The same line is also echoed
// to stderr (Android logcat) for live tailing.

namespace TouchDiag
{
    static std::FILE *s_logFile = nullptr;
    static bool s_initAttempted = false;

    static void Init()
    {
        if (s_initAttempted)
            return;
        s_initAttempted = true;
        const char *userPath = GamePaths::GetUserPath();
        if (userPath == nullptr)
            userPath = "";

        // 优先尝试使用 Java 端 SessionLogCollector 写入的会话目录
        // ${userPath}/current_session.txt 的内容是会话目录的绝对路径。
        char dirPath[600];
        dirPath[0] = '\0';
        {
            char markerPath[700];
            std::snprintf(markerPath, sizeof(markerPath),
                          "%scurrent_session.txt", userPath);
            std::FILE *mf = std::fopen(markerPath, "r");
            if (mf != nullptr)
            {
                if (std::fgets(dirPath, sizeof(dirPath), mf) != nullptr)
                {
                    // 去掉行尾换行
                    size_t L = std::strlen(dirPath);
                    while (L > 0 && (dirPath[L - 1] == '\n' || dirPath[L - 1] == '\r'))
                    {
                        dirPath[--L] = '\0';
                    }
                }
                std::fclose(mf);
            }
        }
        if (dirPath[0] == '\0')
        {
            // 兜底：旧路径 ${userPath}/touch_diag/
            std::snprintf(dirPath, sizeof(dirPath), "%stouch_diag", userPath);
        }
#ifdef __ANDROID__
        if (mkdir(dirPath, 0755) != 0 && errno != EEXIST)
        {
            // Fall back: try cwd.
            std::snprintf(dirPath, sizeof(dirPath), "touch_diag");
            mkdir(dirPath, 0755);
        }
#endif
        char filePath[800];
        std::snprintf(filePath, sizeof(filePath), "%s/touch_native.log", dirPath);
        s_logFile = std::fopen(filePath, "a");
        if (s_logFile != nullptr)
        {
            std::fprintf(s_logFile,
                "# touch_diag session start; ticks_at_open=%u\n",
                (unsigned)SDL_GetTicks());
            std::fflush(s_logFile);
            std::fprintf(stderr, "[touch/diag] log file = %s\n", filePath);
            std::fflush(stderr);
        }
        else
        {
            std::fprintf(stderr,
                "[touch/diag] FAILED to open log file (path=%s, errno=%d)\n",
                filePath, errno);
            std::fflush(stderr);
        }
    }

    static void Logv(const char *tag, const char *fmt, std::va_list ap)
    {
        Init();
        char buf[512];
        std::vsnprintf(buf, sizeof(buf), fmt, ap);
        Uint32 ts = SDL_GetTicks();
        if (s_logFile != nullptr)
        {
            std::fprintf(s_logFile, "%ums %s %s\n", (unsigned)ts, tag, buf);
            std::fflush(s_logFile);
        }
        std::fprintf(stderr, "[%s] %s\n", tag, buf);
        std::fflush(stderr);
    }

    static void Log(const char *tag, const char *fmt, ...)
    {
        std::va_list ap;
        va_start(ap, fmt);
        Logv(tag, fmt, ap);
        va_end(ap);
    }
} // namespace TouchDiag

// Convenience: fall through to stderr for non-Android too.
#define TDIAG(tag, ...) TouchDiag::Log(tag, __VA_ARGS__)

// Public re-export so other TUs (GameWindow, Player) can write into the same
// per-session touch_diag log file (single source of truth for diagnostics).
void AndroidTouchInput::DiagLog(const char *tag, const char *fmt, ...)
{
    std::va_list ap;
    va_start(ap, fmt);
    TouchDiag::Logv(tag, fmt, ap);
    va_end(ap);
}

// ────────────────────────────────────────────────────────────────────────────
// Configuration
// ────────────────────────────────────────────────────────────────────────────

// Tap: max duration in milliseconds and max movement (normalized 0-1 coords).
static constexpr Uint32 kTapMaxDurationMs = 500;
static constexpr float kTapMaxMovement = 0.04f;

// Maximum simultaneous touch pointers tracked.
static constexpr int kMaxPointers = 10;

// ────────────────────────────────────────────────────────────────────────────
// Internal state
// ────────────────────────────────────────────────────────────────────────────

struct TouchPointer
{
    SDL_FingerID fingerId;
    float startX, startY;   // normalized 0-1
    float curX, curY;        // normalized 0-1
    Uint32 startTimeMs;
    bool active;
};

static TouchPointer g_Pointers[kMaxPointers] = {};
static int g_ActivePointerCount = 0;

// Button flags produced by gesture recognition this frame.
static u16 g_TouchButtonsCur = 0;
static u16 g_TouchButtonsPrev = 0;

// Pending button flags accumulated between Update() calls.
static u16 g_PendingButtons = 0;

// Two-finger tap detection.
static int g_TwoFingerTapCount = 0;
static Uint32 g_TwoFingerStartTimeMs = 0;

// Virtual scancode state for thprac GUI integration.
static bool g_TouchScancodes[SDL_NUM_SCANCODES] = {};
static bool g_TouchScancodesPrev[SDL_NUM_SCANCODES] = {};
static bool g_PendingScancodes[SDL_NUM_SCANCODES] = {};

// Pending tap position in game coordinates (640x480).
static bool g_TapPending = false;
static float g_TapGameX = 0.0f;
static float g_TapGameY = 0.0f;

// Pending swipe (vertical) in game-coordinate pixels.
static bool g_SwipeYPending = false;
static float g_SwipeYDelta = 0.0f;

// Pending swipe (horizontal) in game-coordinate pixels.
static bool g_SwipeXPending = false;
static float g_SwipeXDelta = 0.0f;

// Dialogue overlay flag: when true, taps are preserved during gameplay (not cleared in Update).
static bool g_DialogueOverlayActive = false;

// Continuous drag swipe accumulator.
static float g_DragSwipeAccumY = 0.0f;
static float g_DragSwipeAccumX = 0.0f;
static bool g_DragSwipeActive = false;

// Deferred bomb injection (right-click / two-finger tap).
// Set during event handling, consumed post-AdvanceFrameInput via InjectDeferredButtons().
static bool g_DeferredBombPending = false;

// Touch analog direction for gameplay movement (virtual joystick / touchpad).
// Reserved for future gameplay touch implementation.
static AnalogInput g_TouchAnalogInput = {};

// ── Touch-drag movement finger state ─────────────────────────────────────
// In gameplay, the first non-button finger becomes the "movement finger".
// Its per-frame delta in game coordinates drives AnalogMode::Displacement.
static SDL_FingerID g_MoveFingerId = -1;
static bool g_MoveFingerActive = false;
static float g_MovePrevGameX = 0.0f;
static float g_MovePrevGameY = 0.0f;
static float g_MoveDeltaX = 0.0f;   // accumulated delta this frame
static float g_MoveDeltaY = 0.0f;

// Movement-finger UP/DN debounce. After a stall, Android's InputDispatcher
// often synthesizes UP for the held finger (gesture timeout), then resumes
// DN+UP cycles instead of MOTION. Without debounce, each UP destroys the
// movement finger and the subsequent DN re-creates it with delta=0, so no
// drag motion ever reaches the player. We remember the last UP and, if a
// DN for fid=0 arrives within kMoveDebounceMs at a nearby position, we treat
// it as the SAME logical drag and accumulate the position delta directly.
static constexpr Uint32 kMoveDebounceMs = 200;
static Uint32 g_LastMoveUpMs = 0;
static float g_LastMoveUpGameX = 0.0f;
static float g_LastMoveUpGameY = 0.0f;

// After a main-thread stall we discard touch events whose timestamp is
// older than this watermark (ms in SDL_GetTicks units). This filters out
// the burst of stale UP/DN events that Android's input dispatcher
// re-injects once the app becomes responsive again, which would otherwise
// teleport the player ship around for several seconds.
static Uint32 g_StaleTouchCutoffMs = 0;

// ── Joystick-style stall extrapolation ──────────────────────────────────
// vivo OriginOS 6 等系统级"智能优化"会对第三方应用做 ~1Hz 的输入分发节流，
// 应用侧无法绕过。表现为拖动中 Android 凭空停 800ms~1100ms 不投递任何 MOVE
// 事件，玩家在那一秒"卡住"。cardanawandra 的 crossplatform 分支用虚拟摇杆
// 规避了这个问题——摇杆按住即持续输出方向，不依赖 MOVE 事件流。
// 我们在 native 层仿照同样思路：记录最近一次 MOVE 事件的瞬时速度，当 MOVE
// 缺席但 movFinger 仍 active 时，按上次速度继续推进玩家（带线性衰减），
// 真正的 MOVE 到达时立刻接管校准。
static Uint32 g_LastMoveEventMs = 0;     // 最近一次真实 MOVE 事件的 wall-clock
static float  g_LastMoveVx = 0.0f;       // 最近一次 MOVE 的瞬时速度 (game units / ms)
static float  g_LastMoveVy = 0.0f;
static Uint32 g_LastExtrapolateMs = 0;   // 上次外推 Update 的时间戳
static float  g_StallExtrapolateBudgetX = 0.0f;  // 本次 stall 已外推累积位移
static float  g_StallExtrapolateBudgetY = 0.0f;  // (真实 MOVE 到达时归零)
// ────────────────────────────────────────────────────────────────────────────
// Coordinate conversion
// ────────────────────────────────────────────────────────────────────────────

static void NormalizedToGameCoords(float normX, float normY, float &gameX, float &gameY)
{
    if (g_Renderer == nullptr)
    {
        gameX = normX * GAME_WINDOW_WIDTH;
        gameY = normY * GAME_WINDOW_HEIGHT;
        return;
    }

    int realW = g_Renderer->realScreenWidth;
    int realH = g_Renderer->realScreenHeight;
    int gameW = g_Renderer->screenWidth;
    int gameH = g_Renderer->screenHeight;

    if (realW <= 0 || realH <= 0 || gameW <= 0 || gameH <= 0)
    {
        gameX = normX * GAME_WINDOW_WIDTH;
        gameY = normY * GAME_WINDOW_HEIGHT;
        return;
    }

    // Compute letterbox/pillarbox offsets (mirrors RendererGLES::BlitFBO).
    int scaledW, scaledH;
    if (realW * gameH > realH * gameW)
    {
        scaledH = realH;
        scaledW = realH * gameW / gameH;
    }
    else
    {
        scaledW = realW;
        scaledH = realW * gameH / gameW;
    }
    int offsetX = (realW - scaledW) / 2;
    int offsetY = (realH - scaledH) / 2;

    float pixelX = normX * realW;
    float pixelY = normY * realH;

    gameX = (pixelX - offsetX) * (float)gameW / (float)scaledW;
    gameY = (pixelY - offsetY) * (float)gameH / (float)scaledH;
}

// ────────────────────────────────────────────────────────────────────────────
// Helpers
// ────────────────────────────────────────────────────────────────────────────

static TouchPointer *FindPointer(SDL_FingerID id)
{
    for (int i = 0; i < kMaxPointers; i++)
    {
        if (g_Pointers[i].active && g_Pointers[i].fingerId == id)
            return &g_Pointers[i];
    }
    return nullptr;
}

static TouchPointer *AllocPointer()
{
    for (int i = 0; i < kMaxPointers; i++)
    {
        if (!g_Pointers[i].active)
            return &g_Pointers[i];
    }
    return nullptr;
}

static void SetTouchButton(u16 button)
{
    g_PendingButtons |= button;
}

static void SetTouchScancode(SDL_Scancode sc)
{
    if (sc >= 0 && sc < SDL_NUM_SCANCODES)
        g_PendingScancodes[sc] = true;
}

static void MapButtonToScancode(u16 button)
{
    if (button & TH_BUTTON_UP)    SetTouchScancode(SDL_SCANCODE_UP);
    if (button & TH_BUTTON_DOWN)  SetTouchScancode(SDL_SCANCODE_DOWN);
    if (button & TH_BUTTON_LEFT)  SetTouchScancode(SDL_SCANCODE_LEFT);
    if (button & TH_BUTTON_RIGHT) SetTouchScancode(SDL_SCANCODE_RIGHT);
    if (button & TH_BUTTON_SHOOT) SetTouchScancode(SDL_SCANCODE_RETURN);
    if (button & TH_BUTTON_BOMB)  SetTouchScancode(SDL_SCANCODE_X);
    if (button & TH_BUTTON_MENU)  SetTouchScancode(SDL_SCANCODE_ESCAPE);
    if (button & TH_BUTTON_FOCUS) SetTouchScancode(SDL_SCANCODE_LSHIFT);
}

// ────────────────────────────────────────────────────────────────────────────
// Tap / gesture recognition
// ────────────────────────────────────────────────────────────────────────────

static void RecognizeTap(const TouchPointer &ptr, Uint32 releaseTimeMs)
{
    float dx = ptr.curX - ptr.startX;
    float dy = ptr.curY - ptr.startY;
    float dist = std::sqrt(dx * dx + dy * dy);
    Uint32 duration = releaseTimeMs - ptr.startTimeMs;

    // Tap: short duration, small movement → record position for menu hit-test.
    if (duration <= kTapMaxDurationMs && dist < kTapMaxMovement)
    {
        float gx, gy;
        NormalizedToGameCoords(ptr.curX, ptr.curY, gx, gy);
        g_TapPending = true;
        g_TapGameX = gx;
        g_TapGameY = gy;
        SDL_Log("[touch] tap at game(%.1f, %.1f) norm(%.3f, %.3f) dur=%ums",
                gx, gy, ptr.curX, ptr.curY, duration);
    }
    else if (dist >= kTapMaxMovement && !g_DragSwipeActive)
    {
        // Swipe: significant movement → record as vertical or horizontal swipe.
        // Skip if continuous drag swipe was already generated during this gesture.
        float startGX, startGY, endGX, endGY;
        NormalizedToGameCoords(ptr.startX, ptr.startY, startGX, startGY);
        NormalizedToGameCoords(ptr.curX, ptr.curY, endGX, endGY);
        float gameDY = endGY - startGY;
        float gameDX = endGX - startGX;
        if (std::abs(gameDY) > std::abs(gameDX) && std::abs(gameDY) > 10.0f)
        {
            g_SwipeYPending = true;
            g_SwipeYDelta = gameDY;
            SDL_Log("[touch] swipe dy=%.1f dx=%.1f", gameDY, gameDX);
        }
        else if (std::abs(gameDX) > std::abs(gameDY) && std::abs(gameDX) > 20.0f)
        {
            g_SwipeXPending = true;
            g_SwipeXDelta = gameDX;
            SDL_Log("[touch] swipe dx=%.1f dy=%.1f", gameDX, gameDY);
        }
    }
    // Reset drag swipe state on finger up.
    g_DragSwipeAccumY = 0.0f;
    g_DragSwipeAccumX = 0.0f;
    g_DragSwipeActive = false;
}

// ────────────────────────────────────────────────────────────────────────────
// Public API
// ────────────────────────────────────────────────────────────────────────────

void AndroidTouchInput::Init()
{
    Reset();
}

void AndroidTouchInput::HandleFingerDown(const SDL_TouchFingerEvent &event)
{
    if (g_StaleTouchCutoffMs != 0 && event.timestamp != 0 &&
        event.timestamp < g_StaleTouchCutoffMs)
    {
        TDIAG("touch/dn", "DROP-STALE fid=%lld ts=%u cutoff=%u",
              (long long)event.fingerId, (unsigned)event.timestamp,
              (unsigned)g_StaleTouchCutoffMs);
        return;
    }
    TDIAG("touch/dn",
        "fid=%lld nx=%.3f ny=%.3f ts=%u isInMenu=%d dlgOverlay=%d movFinger=%d activePtrs=%d plr=(%.1f,%.1f)",
        (long long)event.fingerId, event.x, event.y, (unsigned)event.timestamp,
        (int)g_GameManager.isInMenu, g_DialogueOverlayActive ? 1 : 0,
        g_MoveFingerActive ? 1 : 0, g_ActivePointerCount,
        g_Player.positionCenter.x, g_Player.positionCenter.y);

    // Check virtual buttons first — if a button is hit, consume the finger.
    {
        float gx, gy;
        NormalizedToGameCoords(event.x, event.y, gx, gy);
        if (TouchVirtualButtons::HandleFingerDown(event.fingerId, gx, gy))
        { TDIAG("touch/dn", "  -> consumed by VIRTUAL BUTTON (gx=%.1f gy=%.1f)", gx, gy); return; }
        // Check menu-context buttons (OK / Cancel).
        if (MenuTouchButtons::HandleFingerDown(event.fingerId, gx, gy))
        { TDIAG("touch/dn", "  -> consumed by MENU BUTTON (gx=%.1f gy=%.1f)", gx, gy); return; }
    }

    // In gameplay, claim the first non-button finger as the movement finger.
    // isInMenu == 1 means normal gameplay (no menus).
    // On desktop, requires the "mouse simulates finger drag" checkbox.
    {
#ifdef __ANDROID__
        bool touchDragAllowed = true;
#else
        bool touchDragAllowed = THPrac::TH06::THPracIsMouseTouchDragEnabled();
#endif
        if (touchDragAllowed && AndroidTouchInput::IsEnabled()
            && g_GameManager.isInMenu != 0 && !g_MoveFingerActive
            && !g_DialogueOverlayActive)
        {
            float gx, gy;
            NormalizedToGameCoords(event.x, event.y, gx, gy);

            // Debounce: if a movement finger UP just happened nearby, treat
            // this DN as the same logical drag (Android synthesizes UP/DN
            // cycles after stalls instead of MOTION, see kMoveDebounceMs).
            Uint32 now = SDL_GetTicks();
            bool isReconnect = (g_LastMoveUpMs != 0)
                && (now - g_LastMoveUpMs <= kMoveDebounceMs)
                && (std::abs(gx - g_LastMoveUpGameX) < 80.0f)
                && (std::abs(gy - g_LastMoveUpGameY) < 80.0f);

            g_MoveFingerActive = true;
            g_MoveFingerId = event.fingerId;

            // Reset stall extrapolation state on fresh DOWN — no velocity
            // history yet, don't extrapolate from a previous drag.
            g_LastMoveEventMs = 0;
            g_LastMoveVx = 0.0f;
            g_LastMoveVy = 0.0f;
            g_LastExtrapolateMs = 0;
            g_StallExtrapolateBudgetX = 0.0f;
            g_StallExtrapolateBudgetY = 0.0f;

            if (isReconnect)
            {
                // Same drag continuing across a spurious UP/DN cycle.
                // We deliberately DO NOT inject (newPos - oldPos) into
                // MoveDelta: that gap is unrecoverable phantom motion —
                // injecting it teleports the player ("闪现"). Just
                // resume the drag from the new finger position.
                float jumpDx = gx - g_LastMoveUpGameX;
                float jumpDy = gy - g_LastMoveUpGameY;
                g_MovePrevGameX = gx;
                g_MovePrevGameY = gy;
                // 清掉上一次 UP 后可能残留的 burst-smooth carry，
                // 否则 reconnect 后下一帧会消费一个错向的几像素 → 自机微跳。
                g_MoveDeltaX = 0.0f;
                g_MoveDeltaY = 0.0f;
                TDIAG("touch/dn",
                    "  -> movement RECONNECT (DROPPED jump dx=%.1f dy=%.1f gap=%ums)",
                    jumpDx, jumpDy,
                    (unsigned)(now - g_LastMoveUpMs));
            }
            else
            {
                g_MovePrevGameX = gx;
                g_MovePrevGameY = gy;
                g_MoveDeltaX = 0.0f;
                g_MoveDeltaY = 0.0f;
                SDL_Log("[touch] movement finger DOWN at game(%.1f, %.1f)", gx, gy);
            }
            return; // Don't create a gesture pointer during gameplay
        }
    }

    TouchPointer *ptr = AllocPointer();
    if (ptr == nullptr)
        return;

    ptr->fingerId = event.fingerId;
    ptr->startX = event.x;
    ptr->startY = event.y;
    ptr->curX = event.x;
    ptr->curY = event.y;
    ptr->startTimeMs = SDL_GetTicks();
    ptr->active = true;
    g_ActivePointerCount++;

    if (g_ActivePointerCount == 2)
    {
        g_TwoFingerTapCount++;
        g_TwoFingerStartTimeMs = SDL_GetTicks();
    }
}

void AndroidTouchInput::HandleFingerMotion(const SDL_TouchFingerEvent &event)
{
    if (g_StaleTouchCutoffMs != 0 && event.timestamp != 0 &&
        event.timestamp < g_StaleTouchCutoffMs)
    {
        return;
    }
    // Movement finger: accumulate game-coordinate delta for gameplay.
    if (g_MoveFingerActive && event.fingerId == g_MoveFingerId)
    {
        float curGX, curGY;
        NormalizedToGameCoords(event.x, event.y, curGX, curGY);
        float ddx = curGX - g_MovePrevGameX;
        float ddy = curGY - g_MovePrevGameY;
        g_MoveDeltaX += ddx;
        g_MoveDeltaY += ddy;
        g_MovePrevGameX = curGX;
        g_MovePrevGameY = curGY;

        // Track instantaneous velocity for stall extrapolation: if Android
        // pauses MOVE delivery (vivo 1Hz throttle), we keep injecting this
        // velocity until the next real MOVE arrives.
        Uint32 nowMs = SDL_GetTicks();
        Uint32 dtMs = (g_LastMoveEventMs != 0) ? (nowMs - g_LastMoveEventMs) : 0;
        if (dtMs > 0 && dtMs < 200)
        {
            g_LastMoveVx = ddx / (float)dtMs;
            g_LastMoveVy = ddy / (float)dtMs;
        }
        g_LastMoveEventMs = nowMs;
        g_LastExtrapolateMs = nowMs;  // reset extrapolation window
        // 真实 MOVE 到达，本次 stall 结束，归零外推累积预算
        g_StallExtrapolateBudgetX = 0.0f;
        g_StallExtrapolateBudgetY = 0.0f;

        TDIAG("touch/mv",
            "MOTION fid=%lld ts=%u dGX=%.2f dGY=%.2f accum=(%.2f,%.2f) pos=(%.1f,%.1f) plr=(%.1f,%.1f)",
            (long long)event.fingerId, (unsigned)event.timestamp,
            ddx, ddy, g_MoveDeltaX, g_MoveDeltaY, curGX, curGY,
            g_Player.positionCenter.x, g_Player.positionCenter.y);
        return; // Don't process as gesture during gameplay
    }

    TouchPointer *ptr = FindPointer(event.fingerId);
    if (ptr == nullptr)
        return;

    float prevX = ptr->curX;
    float prevY = ptr->curY;
    ptr->curX = event.x;
    ptr->curY = event.y;

    // Continuous drag swipe: accumulate movement in game coordinates.
    float prevGX, prevGY, curGX, curGY;
    NormalizedToGameCoords(prevX, prevY, prevGX, prevGY);
    NormalizedToGameCoords(ptr->curX, ptr->curY, curGX, curGY);
    g_DragSwipeAccumY += (curGY - prevGY);
    g_DragSwipeAccumX += (curGX - prevGX);

    // Generate vertical swipe event when accumulated Y exceeds one menu row (18px).
    if (std::abs(g_DragSwipeAccumY) >= 18.0f)
    {
        g_SwipeYPending = true;
        g_SwipeYDelta = g_DragSwipeAccumY;
        g_DragSwipeAccumY = 0.0f;
        g_DragSwipeActive = true;
        SDL_Log("[touch] continuous drag swipe dy=%.1f", g_SwipeYDelta);
    }

    // Generate horizontal swipe event when accumulated X exceeds threshold (40px).
    if (std::abs(g_DragSwipeAccumX) >= 40.0f)
    {
        g_SwipeXPending = true;
        g_SwipeXDelta = g_DragSwipeAccumX;
        g_DragSwipeAccumX = 0.0f;
        g_DragSwipeActive = true;
        SDL_Log("[touch] continuous drag swipe dx=%.1f", g_SwipeXDelta);
    }
}

void AndroidTouchInput::HandleFingerUp(const SDL_TouchFingerEvent &event)
{
    if (g_StaleTouchCutoffMs != 0 && event.timestamp != 0 &&
        event.timestamp < g_StaleTouchCutoffMs)
    {
        TDIAG("touch/up", "DROP-STALE fid=%lld ts=%u cutoff=%u",
              (long long)event.fingerId, (unsigned)event.timestamp,
              (unsigned)g_StaleTouchCutoffMs);
        return;
    }
    TDIAG("touch/up",
        "fid=%lld ts=%u movFinger=%d (id=%lld) activePtrs=%d plr=(%.1f,%.1f)",
        (long long)event.fingerId, (unsigned)event.timestamp,
        g_MoveFingerActive ? 1 : 0,
        (long long)g_MoveFingerId, g_ActivePointerCount,
        g_Player.positionCenter.x, g_Player.positionCenter.y);

    // Check if this finger belonged to a virtual button.
    if (TouchVirtualButtons::HandleFingerUp(event.fingerId))
        return;

    // Check if this finger belonged to a menu button.
    if (MenuTouchButtons::HandleFingerUp(event.fingerId))
        return;

    // Check if this was the movement finger.
    if (g_MoveFingerActive && event.fingerId == g_MoveFingerId)
    {
        // Remember position+time for the DN/UP debounce reconnect.
        g_LastMoveUpMs = SDL_GetTicks();
        g_LastMoveUpGameX = g_MovePrevGameX;
        g_LastMoveUpGameY = g_MovePrevGameY;
        SDL_Log("[touch] movement finger UP");
        g_MoveFingerActive = false;
        g_MoveFingerId = -1;
        // Note: keep accumulated MoveDelta so the current frame still applies it.
        return;
    }

    TouchPointer *ptr = FindPointer(event.fingerId);
    if (ptr == nullptr)
        return;

    ptr->curX = event.x;
    ptr->curY = event.y;

    // Two-finger tap → back/cancel.
    if (g_ActivePointerCount == 2 && g_TwoFingerTapCount > 0)
    {
        Uint32 now = SDL_GetTicks();
        if (now - g_TwoFingerStartTimeMs <= kTapMaxDurationMs)
        {
            bool allSmall = true;
            for (int i = 0; i < kMaxPointers; i++)
            {
                if (!g_Pointers[i].active)
                    continue;
                float dx = g_Pointers[i].curX - g_Pointers[i].startX;
                float dy = g_Pointers[i].curY - g_Pointers[i].startY;
                if (std::sqrt(dx * dx + dy * dy) >= kTapMaxMovement)
                {
                    allSmall = false;
                    break;
                }
            }

            if (allSmall)
            {
                g_DeferredBombPending = true;
                u16 btn = TH_BUTTON_BOMB;
                SetTouchButton(btn);
                MapButtonToScancode(btn);
                SDL_Log("[touch] two-finger tap → X (back)");
                for (int i = 0; i < kMaxPointers; i++)
                {
                    if (g_Pointers[i].active)
                    {
                        g_Pointers[i].active = false;
                        g_ActivePointerCount--;
                    }
                }
                g_TwoFingerTapCount = 0;
                return;
            }
        }
    }

    // Single-finger tap recognition.
    Uint32 now = SDL_GetTicks();
    RecognizeTap(*ptr, now);

    ptr->active = false;
    g_ActivePointerCount--;
    if (g_ActivePointerCount <= 0)
    {
        g_ActivePointerCount = 0;
        g_TwoFingerTapCount = 0;
    }
}

void AndroidTouchInput::Update()
{
    // ── Per-frame digest (throttled to ~once per 250ms) ─────────────────
    {
        static Uint32 s_LastDigestMs = 0;
        Uint32 nowDigest = SDL_GetTicks();
        if (nowDigest - s_LastDigestMs >= 250)
        {
            s_LastDigestMs = nowDigest;
            float fmul = g_Supervisor.effectiveFramerateMultiplier;
            TDIAG("touch/frame",
                "ptrs=%d movFinger=%d (id=%d) prev=(%.1f,%.1f) accum=(%.1f,%.1f) "
                "isInMenu=%d dlgOverlay=%d fmul=%.3f pendBtn=0x%x analog=(%.1f,%.1f,act=%d) plr=(%.1f,%.1f)",
                g_ActivePointerCount,
                g_MoveFingerActive ? 1 : 0, g_MoveFingerId,
                g_MovePrevGameX, g_MovePrevGameY,
                g_MoveDeltaX, g_MoveDeltaY,
                (int)g_GameManager.isInMenu,
                g_DialogueOverlayActive ? 1 : 0,
                fmul,
                (unsigned)g_PendingButtons,
                g_TouchAnalogInput.x, g_TouchAnalogInput.y,
                g_TouchAnalogInput.active ? 1 : 0,
                g_Player.positionCenter.x, g_Player.positionCenter.y);
        }
    }

    // ── Main-thread stall recovery ──────────────────────────────────────
    // If the previous Update() was >500ms ago, the main thread was blocked
    // (e.g. BGM decode, asset load, GC pause). During the stall, Android
    // kept pushing MotionEvents into SDL's queue, and they were all replayed
    // in the catch-up frame — so g_Pointers, the movement finger, drag-
    // accumulators and any pending tap/swipe are all in a corrupt state
    // built from out-of-order events with timestamps that don't reflect
    // reality. Treat this frame as "fresh start": discard all transient
    // touch state so the player can re-touch from a clean slate. The
    // alternative (trusting the replayed events) produces phantom long-
    // press SKIPs, swipes from accumulated delta, and "stuck" fingers.
    {
        static Uint32 s_LastUpdateMs = 0;
        Uint32 now = SDL_GetTicks();
        if (s_LastUpdateMs != 0 && (now - s_LastUpdateMs) >= 500)
        {
            TDIAG("touch/stall",
                "RECOVERY gap=%ums movFinger=%d activePtrs=%d "
                "dlgOverlay=%d isInMenu=%d guiReady=%d wantCapMouse=%d "
                "tap=%d swipeY=%d swipeX=%d dragAccum=(%.1f,%.1f)",
                (unsigned)(now - s_LastUpdateMs),
                g_MoveFingerActive ? 1 : 0,
                g_ActivePointerCount,
                g_DialogueOverlayActive ? 1 : 0,
                (int)g_GameManager.isInMenu,
                THPrac::THPracGuiIsReady() ? 1 : 0,
                (ImGui::GetCurrentContext() && ImGui::GetIO().WantCaptureMouse) ? 1 : 0,
                g_TapPending ? 1 : 0, g_SwipeYPending ? 1 : 0, g_SwipeXPending ? 1 : 0,
                g_DragSwipeAccumX, g_DragSwipeAccumY);
            SDL_Log("[touch] stall recovery: %u ms since last Update; resetting touch state",
                    (unsigned)(now - s_LastUpdateMs));
            for (int i = 0; i < kMaxPointers; i++)
                g_Pointers[i].active = false;
            g_ActivePointerCount = 0;
            g_TwoFingerTapCount = 0;
            // Save movement finger position for DN/UP debounce reconnect
            // (post-stall Android event stream is UP/DN cycles, not MOTION).
            if (g_MoveFingerActive)
            {
                g_LastMoveUpMs = now;
                g_LastMoveUpGameX = g_MovePrevGameX;
                g_LastMoveUpGameY = g_MovePrevGameY;
            }
            g_MoveFingerActive = false;
            g_MoveDeltaX = 0.0f;
            g_MoveDeltaY = 0.0f;
            g_TapPending = false;
            g_SwipeYPending = false;
            g_SwipeXPending = false;
            g_DragSwipeAccumY = 0.0f;
            g_DragSwipeAccumX = 0.0f;
            g_PendingButtons = 0;
            std::memset(g_PendingScancodes, 0, sizeof(g_PendingScancodes));

            // Force-release any phantom ImGui mouse capture. During the stall,
            // ImGui sees touch events as left-click; if a DOWN was delivered
            // without a matching UP (or out of order), WantCaptureMouse stays
            // sticky-true, and the SDL_FINGER* dispatch in GameWindow.cpp
            // silently drops every subsequent finger event ("re-touch doesn't
            // help, only a few pixels of motion sneak through" maps exactly
            // to this). Clear ImGui's mouse-down state so capture releases.
            if (ImGui::GetCurrentContext() != nullptr)
            {
                ImGuiIO &io = ImGui::GetIO();
                for (int b = 0; b < IM_ARRAYSIZE(io.MouseDown); b++)
                    io.MouseDown[b] = false;
                io.MouseDownDuration[0] = io.MouseDownDuration[1] = io.MouseDownDuration[2] = -1.0f;
            }

            // Drop any touch event with a timestamp older than NOW. The
            // dispatcher's backlog (collected during the stall) has stale
            // timestamps and would otherwise drive the player around.
            g_StaleTouchCutoffMs = now;
        }
        s_LastUpdateMs = now;
    }

    g_TouchButtonsPrev = g_TouchButtonsCur;
    std::memcpy(g_TouchScancodesPrev, g_TouchScancodes, sizeof(g_TouchScancodes));

    g_TouchButtonsCur = g_PendingButtons;
    std::memcpy(g_TouchScancodes, g_PendingScancodes, sizeof(g_TouchScancodes));

    g_PendingButtons = 0;
    std::memset(g_PendingScancodes, 0, sizeof(g_PendingScancodes));

    // NOTE: Do NOT clear g_TouchAnalogInput here. On Android, Update() runs
    // at render rate (often 200+ Hz) while the game logic ticks at 60 Hz, so
    // clearing per-Update would discard accumulated motion. Instead, the
    // consume block below `+=` accumulates each MoveDelta into
    // g_TouchAnalogInput, and Controller::GetInput drains it (via
    // ConsumeAnalogReset) once per game tick after copying to g_AnalogInput.
    // The mouse-follow code (`#ifndef __ANDROID__`) below still overwrites
    // unconditionally, which is fine on desktop where Update() runs at game
    // tick rate.

    // ── Discard stale taps during gameplay ──────────────────────────────
    // During gameplay (isInMenu != 0), no menus consume taps via
    // TryTouchSelect.  If a stale tap persists into a menu transition
    // (e.g. retry menu), it can silently move the cursor and cause
    // an unintended confirm on the player's first deliberate tap.
    // Clearing here is safe: Update() runs before the calc chain, so
    // taps generated while a menu is open (isInMenu == 0) are preserved.
    // Exception: dialogue overlay active → preserve taps for dialogue advance.
    if (g_GameManager.isInMenu != 0 && !g_DialogueOverlayActive)
    {
        g_TapPending = false;
    }

    // ── Dialogue long-press → SKIP (pre-lockstep) ──────────────────────
    // During dialogue, a long finger hold (500ms+) injects TH_BUTTON_SKIP
    // into the touch button state so it flows through Controller::GetInput()
    // → lockstep, ensuring both netplay machines see SKIP at the same frame.
    if (g_DialogueOverlayActive && IsAnyFingerHeld(500))
    {
        g_TouchButtonsCur |= TH_BUTTON_SKIP;
    }

    // ── Promote leftover dialogue-era finger to movement finger ────────
    // While the dialogue overlay is up, HandleFingerDown routes new
    // fingers into the gesture-pointer pool (so long-press → SKIP works)
    // instead of claiming them as the gameplay movement finger. When the
    // dialogue ends with a finger still pressed (typical: player held to
    // skip and gameplay resumes immediately), we previously required the
    // player to lift and re-touch before movement worked again, which felt
    // like input "stuttering" right after every dialogue. Promote the
    // first still-active gesture pointer to be the movement finger so the
    // held finger seamlessly takes over without a release.
    if (AndroidTouchInput::IsEnabled() && g_GameManager.isInMenu != 0
        && !g_DialogueOverlayActive && !g_MoveFingerActive)
    {
        for (int i = 0; i < kMaxPointers; i++)
        {
            if (!g_Pointers[i].active)
                continue;
            TouchPointer &ptr = g_Pointers[i];
            float gx, gy;
            NormalizedToGameCoords(ptr.curX, ptr.curY, gx, gy);
            g_MoveFingerActive = true;
            g_MoveFingerId = ptr.fingerId;
            g_MovePrevGameX = gx;
            g_MovePrevGameY = gy;
            g_MoveDeltaX = 0.0f;
            g_MoveDeltaY = 0.0f;
            // Free the gesture slot — this finger now drives movement,
            // not gestures. Don't decrement g_TwoFingerTapCount: that
            // counter tracks distinct finger-down events, not currently
            // active pointers, and is reset when all pointers lift.
            ptr.active = false;
            if (g_ActivePointerCount > 0) g_ActivePointerCount--;
            SDL_Log("[touch] post-dialogue promote to movement finger at game(%.1f, %.1f)",
                    gx, gy);
            break;
        }
    }

    // ── Touch-drag gameplay movement ────────────────────────────────────
    // When a movement finger is active during gameplay, its per-frame delta
    // drives AnalogMode::Displacement for uncapped 1:1 finger-to-player mapping.
    // The delta is pre-divided by effectiveFramerateMultiplier so that
    // Player.cpp's `pos += analog * multiplier` yields the original pixel delta,
    // preventing the "ice puck" overshoot at variable frame rates.
    if (AndroidTouchInput::IsEnabled() && g_GameManager.isInMenu != 0 && g_MoveFingerActive)
    {
        // ── Joystick-style stall extrapolation ─────────────────────────────
        // 当 vivo OriginOS 6 等系统级智能优化在拖动中凭空停掉 ~1000ms 的
        // MOVE 投递时，按"最近一次 MOVE 的瞬时速度"持续注入位移，让玩家
        // 沿上次方向继续滑行。新的真实 MOVE 一到达就立即重置
        // g_LastExtrapolateMs，外推自动停止。
        //
        // 实测 vivo 节流的 sinceLast 均匀分布到 ~800ms，所以 window 必须
        // 覆盖完整 1Hz 周期 (1500ms)。fade 不归零，保持 40% 底速度，避免
        // 断流后期玩家完全停下，恢复时又被真实事件冲倒造成顿挫。
        //
        // 精度保护：
        //   1) guard 30ms：超过正常事件间隔 (~16ms)，避开正常事件流。
        //   2) 速度阈值 0.05 units/ms：精调时速度几乎为 0，自动跳过。
        //   3) 单次 stall 位移上限 g_StallExtrapolateBudget：从最后一次
        //      真实 MOVE 起算，外推累积总位移不超过 200 units，防止失控。
        //   4) 速度上限 0.5 units/ms：极端 flick 后不会失控冲出去。
        //
        // 2026-04-26 更新：发现 1Hz 节流真因是 vivo PEM (Process Energy
        // Management) cgroup-freeze（详见 GamePerformanceService.java）。
        // 启动 foreground service 后真实 MOVE 流恢复正常，外推反而和真实
        // 流冲突导致自机乱飞。默认禁用此外推，仅作历史保留。
        // 如需在异常机型上重新启用，把下面的 0 改成 1。
#if 0
        {
            constexpr Uint32 kExtrapolateGuardMs    = 30;     // 至少这么久没新事件才外推
            constexpr Uint32 kExtrapolateWindowMs   = 1500;   // 最多外推这么久
            constexpr float  kExtrapolateMinSpeed   = 0.05f;  // 速度下限（精调保护）
            constexpr float  kExtrapolateMaxPerMs   = 0.5f;   // 速度安全上限
            constexpr float  kExtrapolateFadeFloor  = 0.40f;  // fade 不归零，保留 40% 底速
            constexpr float  kStallBudgetMaxUnits   = 200.0f; // 单次 stall 位移上限
            Uint32 nowMs = SDL_GetTicks();
            Uint32 sinceLast = (g_LastMoveEventMs != 0) ? (nowMs - g_LastMoveEventMs) : 0;
            float  speedMag = std::sqrt(g_LastMoveVx * g_LastMoveVx + g_LastMoveVy * g_LastMoveVy);
            if (sinceLast > kExtrapolateGuardMs && sinceLast < kExtrapolateWindowMs &&
                speedMag >= kExtrapolateMinSpeed && g_LastExtrapolateMs != 0 &&
                std::abs(g_StallExtrapolateBudgetX) < kStallBudgetMaxUnits &&
                std::abs(g_StallExtrapolateBudgetY) < kStallBudgetMaxUnits)
            {
                Uint32 stepMs = nowMs - g_LastExtrapolateMs;
                if (stepMs > 0 && stepMs < 100)
                {
                    // 渐变 fade：从 guard 时刻的 1.0 线性插值到 window 时刻
                    // 的 kExtrapolateFadeFloor (0.4)，不归零，避免断流末期
                    // 玩家完全停下、恢复时被真实事件冲倒的顿挫感。
                    float t = (float)(sinceLast - kExtrapolateGuardMs)
                            / (float)(kExtrapolateWindowMs - kExtrapolateGuardMs);
                    if (t < 0.0f) t = 0.0f; if (t > 1.0f) t = 1.0f;
                    float fade = 1.0f + (kExtrapolateFadeFloor - 1.0f) * t;
                    float vx = g_LastMoveVx * fade;
                    float vy = g_LastMoveVy * fade;
                    if (vx >  kExtrapolateMaxPerMs) vx =  kExtrapolateMaxPerMs;
                    if (vx < -kExtrapolateMaxPerMs) vx = -kExtrapolateMaxPerMs;
                    if (vy >  kExtrapolateMaxPerMs) vy =  kExtrapolateMaxPerMs;
                    if (vy < -kExtrapolateMaxPerMs) vy = -kExtrapolateMaxPerMs;
                    float exDx = vx * (float)stepMs;
                    float exDy = vy * (float)stepMs;
                    if (std::abs(exDx) > 0.01f || std::abs(exDy) > 0.01f)
                    {
                        g_MoveDeltaX += exDx;
                        g_MoveDeltaY += exDy;
                        // 同步 prev 位置，避免下一个真实 MOVE 用 prev 算出的
                        // delta 把外推过的部分再加一遍（双重计算 = 跳）。
                        g_MovePrevGameX += exDx;
                        g_MovePrevGameY += exDy;
                        // 累积本次 stall 已外推的总量（HandleFingerMotion 收
                        // 到真实 MOVE 时归零），超阈值后停止外推防失控。
                        g_StallExtrapolateBudgetX += exDx;
                        g_StallExtrapolateBudgetY += exDy;
                        TDIAG("touch/extra",
                            "STALL EXTRAPOLATE sinceLast=%ums step=%ums fade=%.2f "
                            "v=(%.3f,%.3f) inj=(%.2f,%.2f) budget=(%.1f,%.1f)",
                            (unsigned)sinceLast, (unsigned)stepMs, fade,
                            vx, vy, exDx, exDy,
                            g_StallExtrapolateBudgetX, g_StallExtrapolateBudgetY);
                    }
                }
            }
            g_LastExtrapolateMs = nowMs;
        }
#endif

        // ── Burst smoothing: cap per-Update consumption with carry-over ──
        // Android InputDispatcher periodically stalls touch delivery for
        // 300ms~1000ms, then dumps the queued events as a single batched
        // MotionEvent (with historicalEvents). HandleFingerMotion sums all
        // those deltas into g_MoveDeltaX/Y. Without a cap, the next
        // Update() would consume the full burst (e.g. 115 game-units in
        // one frame) — Player.cpp applies that as one teleport, which the
        // user perceives as "卡住 → 闪现". By limiting per-Update consume
        // to kMaxConsumePerUpdate and saving the remainder back into
        // g_MoveDelta, the burst is smoothed across several Updates so
        // the player slides smoothly to the catch-up position instead of
        // teleporting. Choose 16 game-units: at 250Hz Update this is
        // 4000 units/s — well above any plausible finger speed, so normal
        // motion has zero added latency, but worst-case bursts get split
        // over ~7 Updates (≈30ms) instead of one.
        constexpr float kMaxConsumePerUpdate = 16.0f;
        float dx = g_MoveDeltaX;
        float dy = g_MoveDeltaY;
        if (dx >  kMaxConsumePerUpdate) dx =  kMaxConsumePerUpdate;
        if (dx < -kMaxConsumePerUpdate) dx = -kMaxConsumePerUpdate;
        if (dy >  kMaxConsumePerUpdate) dy =  kMaxConsumePerUpdate;
        if (dy < -kMaxConsumePerUpdate) dy = -kMaxConsumePerUpdate;
        // Save unconsumed remainder for the next Update.
        float carryX = g_MoveDeltaX - dx;
        float carryY = g_MoveDeltaY - dy;

        // Compensate for the framerate multiplier that Player.cpp applies.
        float fmul = g_Supervisor.effectiveFramerateMultiplier;
        if (fmul < 0.01f) fmul = 1.0f;
        dx /= fmul;
        dy /= fmul;

        // Clamp and round for replay i8 encoding determinism.
        dx = std::round(dx);
        dy = std::round(dy);
        if (dx < -127.0f) dx = -127.0f;
        if (dx >  127.0f) dx =  127.0f;
        if (dy < -127.0f) dy = -127.0f;
        if (dy >  127.0f) dy =  127.0f;

        if (std::abs(dx) > 0.5f || std::abs(dy) > 0.5f)
        {
            // Synthesize direction buttons for PlayerDirection sprite animation.
            u16 dirButtons = 0;
            if (dy < -0.5f) dirButtons |= TH_BUTTON_UP;
            if (dy >  0.5f) dirButtons |= TH_BUTTON_DOWN;
            if (dx < -0.5f) dirButtons |= TH_BUTTON_LEFT;
            if (dx >  0.5f) dirButtons |= TH_BUTTON_RIGHT;
            g_TouchButtonsCur |= dirButtons;

            // ACCUMULATE across multiple Update() calls within the same
            // game tick (Update runs faster than 60 Hz on Android). The
            // accumulated value is drained by Controller::GetInput once
            // per game tick via ConsumeAnalogReset().
            g_TouchAnalogInput.x += dx;
            g_TouchAnalogInput.y += dy;
            // Clamp to i8 range for replay encoding.
            if (g_TouchAnalogInput.x < -127.0f) g_TouchAnalogInput.x = -127.0f;
            if (g_TouchAnalogInput.x >  127.0f) g_TouchAnalogInput.x =  127.0f;
            if (g_TouchAnalogInput.y < -127.0f) g_TouchAnalogInput.y = -127.0f;
            if (g_TouchAnalogInput.y >  127.0f) g_TouchAnalogInput.y =  127.0f;
            g_TouchAnalogInput.active = true;
            g_TouchAnalogInput.mode = AnalogMode::Displacement;
            TDIAG("touch/consume",
                "MOVE dx=%.1f dy=%.1f acc=(%.1f,%.1f) buttons=0x%x fmul=%.3f",
                dx, dy, g_TouchAnalogInput.x, g_TouchAnalogInput.y,
                (unsigned)dirButtons, fmul);
        }
        // else: no motion this Update — DON'T touch g_TouchAnalogInput.
        // It either still holds accumulated motion from earlier in this
        // tick (waiting for Player to read), or was already drained
        // (active=false) by ConsumeAnalogReset.

        // Consume delta for this frame, keeping any over-cap remainder
        // so the next Update can drain the burst smoothly.
        g_MoveDeltaX = carryX;
        g_MoveDeltaY = carryY;
    }
    else if (g_MoveFingerActive && g_GameManager.isInMenu == 0)
    {
        // Left gameplay (entered menu) — release movement finger.
        TDIAG("touch/mv", "RELEASE movement finger (entered menu)");
        g_MoveFingerActive = false;
        g_MoveFingerId = -1;
        g_MoveDeltaX = 0.0f;
        g_MoveDeltaY = 0.0f;
    }

    // Mouse-follow: poll current mouse position each frame.
    // Synthesizes direction buttons (for PlayerDirection animation) and sets
    // AnalogMode::Displacement with the pixel delta from player → mouse.
    // The delta is clamped to ±127 and rounded to integers BEFORE use, so
    // live play and replay see exactly the same displacement values.
    // This guarantees perfect replay determinism while allowing free movement.
#ifndef __ANDROID__
    if (THPrac::TH06::THPracIsMouseFollowEnabled() && g_GameManager.isInMenu)
    {
        int mx, my;
        SDL_GetMouseState(&mx, &my);
        int windowW = 0, windowH = 0;
        if (g_GameWindow.sdlWindow)
            SDL_GetWindowSize(g_GameWindow.sdlWindow, &windowW, &windowH);
        if (windowW > 0 && windowH > 0)
        {
            float normX = static_cast<float>(mx) / windowW;
            float normY = static_cast<float>(my) / windowH;
            float gameX, gameY;
            NormalizedToGameCoords(normX, normY, gameX, gameY);

            // Compute displacement from player to mouse.
            const float playerX = g_Player.positionCenter.x;
            const float playerY = g_Player.positionCenter.y;
            float dx = gameX - playerX;
            float dy = gameY - playerY;

            // Clamp to i8 range and round so live play matches replay encoding.
            dx = std::round(dx);
            dy = std::round(dy);
            if (dx < -127.0f) dx = -127.0f;
            if (dx >  127.0f) dx =  127.0f;
            if (dy < -127.0f) dy = -127.0f;
            if (dy >  127.0f) dy =  127.0f;

            const float dist = std::sqrt(dx * dx + dy * dy);

            // Dead-zone: if within 1 pixel, no movement needed.
            if (dist > 0.5f)
            {
                // Synthesize direction buttons for PlayerDirection (sprite animation).
                u16 dirButtons = 0;
                if (dy < -0.5f) dirButtons |= TH_BUTTON_UP;
                if (dy >  0.5f) dirButtons |= TH_BUTTON_DOWN;
                if (dx < -0.5f) dirButtons |= TH_BUTTON_LEFT;
                if (dx >  0.5f) dirButtons |= TH_BUTTON_RIGHT;
                g_TouchButtonsCur |= dirButtons;

                // Displacement mode: x, y are pixel delta (integer).
                g_TouchAnalogInput.x = dx;
                g_TouchAnalogInput.y = dy;
                g_TouchAnalogInput.active = true;
                g_TouchAnalogInput.mode = AnalogMode::Displacement;
            }
            else
            {
                g_TouchAnalogInput.x = 0.0f;
                g_TouchAnalogInput.y = 0.0f;
                g_TouchAnalogInput.active = false;
                g_TouchAnalogInput.mode = AnalogMode::Direction;
            }
        }
    }
#endif
}

u16 AndroidTouchInput::GetTouchButtons()
{
    return g_TouchButtonsCur | TouchVirtualButtons::GetButtonFlags()
                             | MenuTouchButtons::GetButtonFlags();
}

const AnalogInput &AndroidTouchInput::GetAnalogInput()
{
    return g_TouchAnalogInput;
}

void AndroidTouchInput::ConsumeAnalogReset()
{
    // Drain accumulated displacement after Controller::GetInput has copied
    // it into the per-tick g_AnalogInput. See AndroidTouchInput::Update()
    // (consume block) for the accumulation half of this contract.
    g_TouchAnalogInput.x = 0.0f;
    g_TouchAnalogInput.y = 0.0f;
    g_TouchAnalogInput.active = false;
}

bool AndroidTouchInput::IsTouchScancode(SDL_Scancode sc)
{
    if (sc < 0 || sc >= SDL_NUM_SCANCODES)
        return false;
    return g_TouchScancodes[sc];
}

bool AndroidTouchInput::IsTouchScancodeSingle(SDL_Scancode sc)
{
    if (sc < 0 || sc >= SDL_NUM_SCANCODES)
        return false;
    return g_TouchScancodes[sc] && !g_TouchScancodesPrev[sc];
}

bool AndroidTouchInput::ConsumeTap(float &gameX, float &gameY)
{
    if (!g_TapPending)
        return false;
    gameX = g_TapGameX;
    gameY = g_TapGameY;
    g_TapPending = false;
    return true;
}

bool AndroidTouchInput::VmContainsPoint(const AnmVm &vm, float gameX, float gameY)
{
    if (!vm.flags.isVisible || vm.sprite == nullptr)
        return false;

    float halfW = (vm.sprite->widthPx * vm.scaleX) / 2.0f;
    float halfH = (vm.sprite->heightPx * vm.scaleY) / 2.0f;

    // Account for posOffset (used for highlight/selection visual shift).
    float posX = vm.pos.x + vm.posOffset.x;
    float posY = vm.pos.y + vm.posOffset.y;

    float left, right, top, bottom;
    if (vm.flags.anchor & AnmVmAnchor_Left)
    {
        left = posX;
        right = posX + halfW * 2.0f;
    }
    else
    {
        left = posX - halfW;
        right = posX + halfW;
    }

    if (vm.flags.anchor & AnmVmAnchor_Top)
    {
        top = posY;
        bottom = posY + halfH * 2.0f;
    }
    else
    {
        top = posY - halfH;
        bottom = posY + halfH;
    }

    return gameX >= left && gameX <= right && gameY >= top && gameY <= bottom;
}

bool AndroidTouchInput::TryTouchSelect(AnmVm *items, int count, int &cursor, int stride)
{
    if (!g_TapPending)
        return false;

    float gx = g_TapGameX;
    float gy = g_TapGameY;

    for (int i = 0; i < count; i++)
    {
        AnmVm *vm = &items[i * stride];
        if (VmContainsPoint(*vm, gx, gy))
        {
            g_TapPending = false;

            if (cursor == i)
            {
                // Already on this item → inject SELECTMENU so WAS_PRESSED fires.
                g_CurFrameInput |= TH_BUTTON_SHOOT;
                g_LastFrameInput &= ~TH_BUTTON_SHOOT;
                SDL_Log("[touch] menu confirm item %d at game(%.1f, %.1f)", i, gx, gy);
            }
            else
            {
                // Move cursor to this item (visual update only, no confirm).
                cursor = i;
                SDL_Log("[touch] menu move cursor to %d at game(%.1f, %.1f)", i, gx, gy);
            }
            return true;
        }
    }
    return false;
}

bool AndroidTouchInput::TryTouchSelectRect(float left, float top, float right, float bottom,
                                            int index, int &cursor)
{
    if (!g_TapPending)
        return false;

    float gx = g_TapGameX;
    float gy = g_TapGameY;

    if (gx >= left && gx <= right && gy >= top && gy <= bottom)
    {
        g_TapPending = false;

        if (cursor == index)
        {
            g_CurFrameInput |= TH_BUTTON_SHOOT;
            g_LastFrameInput &= ~TH_BUTTON_SHOOT;
            SDL_Log("[touch] menu confirm rect item %d at game(%.1f, %.1f)", index, gx, gy);
        }
        else
        {
            cursor = index;
            SDL_Log("[touch] menu move cursor to rect item %d at game(%.1f, %.1f)", index, gx, gy);
        }
        return true;
    }
    return false;
}

bool AndroidTouchInput::ConsumeSwipeY(float &deltaY)
{
    if (!g_SwipeYPending)
        return false;
    deltaY = g_SwipeYDelta;
    g_SwipeYPending = false;
    return true;
}

bool AndroidTouchInput::ConsumeSwipeX(float &deltaX)
{
    if (!g_SwipeXPending)
        return false;
    deltaX = g_SwipeXDelta;
    g_SwipeXPending = false;
    return true;
}

void AndroidTouchInput::Reset()
{
    std::memset(g_Pointers, 0, sizeof(g_Pointers));
    g_ActivePointerCount = 0;
    g_TouchButtonsCur = 0;
    g_TouchButtonsPrev = 0;
    g_PendingButtons = 0;
    g_TwoFingerTapCount = 0;
    g_TwoFingerStartTimeMs = 0;
    g_TapPending = false;
    g_TapGameX = 0.0f;
    g_TapGameY = 0.0f;
    g_SwipeYPending = false;
    g_SwipeYDelta = 0.0f;
    g_SwipeXPending = false;
    g_SwipeXDelta = 0.0f;
    g_DragSwipeAccumY = 0.0f;
    g_DragSwipeAccumX = 0.0f;
    g_DragSwipeActive = false;
    g_DeferredBombPending = false;
    g_TouchAnalogInput = {};
    g_MoveFingerActive = false;
    g_MoveFingerId = -1;
    g_MovePrevGameX = 0.0f;
    g_MovePrevGameY = 0.0f;
    g_MoveDeltaX = 0.0f;
    g_MoveDeltaY = 0.0f;
    std::memset(g_TouchScancodes, 0, sizeof(g_TouchScancodes));
    std::memset(g_TouchScancodesPrev, 0, sizeof(g_TouchScancodesPrev));
    std::memset(g_PendingScancodes, 0, sizeof(g_PendingScancodes));
    TouchVirtualButtons::Reset();
    MenuTouchButtons::Reset();
}

void AndroidTouchInput::InjectDeferredButtons()
{
    if (g_DeferredBombPending)
    {
        g_DeferredBombPending = false;
        // Direct-inject BOMB into frame input AFTER AdvanceFrameInput has set g_CurFrameInput.
        // This matches the injection pattern used by TryTouchSelect for SHOOT.
        g_CurFrameInput |= TH_BUTTON_BOMB;
        g_LastFrameInput &= ~TH_BUTTON_BOMB;
        SDL_Log("[touch] deferred BOMB injected into g_CurFrameInput");
    }
}

TouchFrameData AndroidTouchInput::CaptureTouchFrameData()
{
    TouchFrameData d;
    d.Clear();

    if (g_TapPending)
    {
        d.flags |= TouchFrameData::kFlagTap;
        d.tapGameX = g_TapGameX;
        d.tapGameY = g_TapGameY;
    }
    if (g_SwipeXPending)
    {
        d.flags |= TouchFrameData::kFlagSwipeX;
        d.swipeXDelta = g_SwipeXDelta;
    }
    if (g_SwipeYPending)
    {
        d.flags |= TouchFrameData::kFlagSwipeY;
        d.swipeYDelta = g_SwipeYDelta;
    }
    if (g_DeferredBombPending)
    {
        d.flags |= TouchFrameData::kFlagBomb;
    }
    if (g_TouchAnalogInput.active)
    {
        d.flags |= TouchFrameData::kFlagAnalog;
        // Clamp to i8 range (already clamped in Update, but be safe).
        float ax = g_TouchAnalogInput.x;
        float ay = g_TouchAnalogInput.y;
        if (ax < -127.0f) ax = -127.0f;
        if (ax >  127.0f) ax =  127.0f;
        if (ay < -127.0f) ay = -127.0f;
        if (ay >  127.0f) ay =  127.0f;
        d.analogX = static_cast<i8>(ax);
        d.analogY = static_cast<i8>(ay);
    }

    return d;
}

void AndroidTouchInput::ApplyRemoteTouchFrameData(const TouchFrameData &data)
{

    // Tap event.
    if (data.flags & TouchFrameData::kFlagTap)
    {
        g_TapPending = true;
        g_TapGameX = data.tapGameX;
        g_TapGameY = data.tapGameY;
    }

    // Swipe X.
    if (data.flags & TouchFrameData::kFlagSwipeX)
    {
        g_SwipeXPending = true;
        g_SwipeXDelta = data.swipeXDelta;
    }

    // Swipe Y.
    if (data.flags & TouchFrameData::kFlagSwipeY)
    {
        g_SwipeYPending = true;
        g_SwipeYDelta = data.swipeYDelta;
    }

    // Deferred bomb.
    if (data.flags & TouchFrameData::kFlagBomb)
    {
        g_DeferredBombPending = true;
    }

    // Analog displacement.
    if (data.flags & TouchFrameData::kFlagAnalog)
    {
        g_TouchAnalogInput.x = static_cast<float>(data.analogX);
        g_TouchAnalogInput.y = static_cast<float>(data.analogY);
        g_TouchAnalogInput.active = true;
        g_TouchAnalogInput.mode = AnalogMode::Displacement;
    }
}

bool AndroidTouchInput::IsEnabled()
{
#ifdef __ANDROID__
    return true;
#else
    return THPrac::TH06::THPracIsNewTouchEnabled();
#endif
}

void AndroidTouchInput::ClearPendingTouchAfterCapture()
{
    g_TapPending = false;
    g_SwipeXPending = false;
    g_SwipeYPending = false;
    g_DeferredBombPending = false;
    g_TouchAnalogInput = {};
}

bool AndroidTouchInput::HasPendingTouchData()
{
    return g_TapPending || g_SwipeXPending || g_SwipeYPending;
}

// ────────────────────────────────────────────────────────────────────────────
// Mouse-to-touch simulation for desktop developer mode
// ────────────────────────────────────────────────────────────────────────────

static constexpr SDL_FingerID kMouseLeftFingerId = 1000;
static constexpr SDL_FingerID kMouseRightFingerId = 1001;

static void MouseToFinger(SDL_TouchFingerEvent &out, SDL_FingerID fakeId, int mx, int my, int windowW, int windowH)
{
    std::memset(&out, 0, sizeof(out));
    out.fingerId = fakeId;
    out.x = (windowW > 0) ? static_cast<float>(mx) / windowW : 0.0f;
    out.y = (windowH > 0) ? static_cast<float>(my) / windowH : 0.0f;
}

void AndroidTouchInput::HandleMouseButtonDown(const SDL_MouseButtonEvent &event, int windowW, int windowH)
{
    if (!IsEnabled())
        return;

    if (event.button == SDL_BUTTON_RIGHT)
    {
        // Right-click → X key (cancel/back in menus).
        // Use deferred injection for reliable delivery after AdvanceFrameInput.
        g_DeferredBombPending = true;
        u16 btn = TH_BUTTON_BOMB;
        SetTouchButton(btn);
        MapButtonToScancode(btn);
        SDL_Log("[touch] right-click → X (back, deferred)");
        return;
    }

    if (event.button != SDL_BUTTON_LEFT)
        return;

    SDL_TouchFingerEvent tfinger;
    MouseToFinger(tfinger, kMouseLeftFingerId, event.x, event.y, windowW, windowH);
    HandleFingerDown(tfinger);
}

void AndroidTouchInput::HandleMouseMotion(const SDL_MouseMotionEvent &event, int windowW, int windowH)
{
    if (!IsEnabled())
        return;
    // Only track left-button drag (right-click is instant back gesture).
    if (!(event.state & SDL_BUTTON_LMASK))
        return;

    SDL_TouchFingerEvent tfinger;
    MouseToFinger(tfinger, kMouseLeftFingerId, event.x, event.y, windowW, windowH);
    HandleFingerMotion(tfinger);
}

void AndroidTouchInput::HandleMouseButtonUp(const SDL_MouseButtonEvent &event, int windowW, int windowH)
{
    if (!IsEnabled())
        return;
    // Right-click back gesture was handled instantly on button-down, ignore release.
    if (event.button != SDL_BUTTON_LEFT)
        return;

    SDL_TouchFingerEvent tfinger;
    MouseToFinger(tfinger, kMouseLeftFingerId, event.x, event.y, windowW, windowH);
    HandleFingerUp(tfinger);
}

void AndroidTouchInput::SetDialogueOverlay(bool active)
{
    if (g_DialogueOverlayActive != active)
    {
        TDIAG("touch/dlg", "overlay %d -> %d movFinger=%d activePtrs=%d",
            g_DialogueOverlayActive ? 1 : 0, active ? 1 : 0,
            g_MoveFingerActive ? 1 : 0, g_ActivePointerCount);
    }
    g_DialogueOverlayActive = active;
    if (!active)
    {
        // Clear any stale tap when dialogue ends to prevent it from leaking
        // into gameplay or the next menu transition.
        g_TapPending = false;
    }
}

bool AndroidTouchInput::IsAnyFingerHeld(Uint32 minDurationMs)
{
    if (!IsEnabled())
        return false;

    Uint32 now = SDL_GetTicks();
    for (int i = 0; i < kMaxPointers; i++)
    {
        if (!g_Pointers[i].active)
            continue;

        // 排除被虚拟按钮或菜单按钮占用的手指.
        if (TouchVirtualButtons::IsTrackedFinger(g_Pointers[i].fingerId))
            continue;
        if (MenuTouchButtons::IsTrackedFinger(g_Pointers[i].fingerId))
            continue;

        // 排除移动手指（gameplay拖拽）.
        if (g_MoveFingerActive && g_Pointers[i].fingerId == g_MoveFingerId)
            continue;

        Uint32 duration = now - g_Pointers[i].startTimeMs;
        if (duration >= minDurationMs)
            return true;
    }
    return false;
}

void AndroidTouchInput::InjectHeldScancode(SDL_Scancode sc)
{
    if (sc > SDL_SCANCODE_UNKNOWN && sc < SDL_NUM_SCANCODES)
        g_TouchScancodes[sc] = true;
}

}; // namespace th06
