#include "TouchVirtualButtons.hpp"
#include "AndroidTouchInput.hpp"
#include "GameManager.hpp"

#include <SDL.h>
#include <cmath>
#include <cstring>

namespace th06
{

// ────────────────────────────────────────────────────────────────────────────
// Button definitions — proportionally mapped from THSSS (1920×1080) canvas
// ────────────────────────────────────────────────────────────────────────────
//
// THSSS original positions (canvas 1920×1080):
//   btn_ESC: center=(155, 103), 140×140
//   btn_Z:   center=(145, 500), 160×160
//   btn_S:   center=(145, 680), 160×160
//   btn_X:   center=(145, 860), 160×160
//
// Mapping to th06 (640×480):
//   y: fraction * 480,  radius: fraction_of_height * 480 / 2
//   x: buttons are rendered on the screen-space black border (pillarbox),
//      so game-space X is only used for touch hit detection.

struct VirtualButtonDef
{
    float centerX;          // game coordinates for hit detection
    float centerY;          // game Y coordinate (also maps to screen Y)
    float radius;           // visual radius in game coordinates
    float hitRadius;        // touch hit radius
    u16 buttonFlag;         // TouhouButton flag (0 if scancode-only)
    SDL_Scancode scancode;  // scancode to inject when held (SDL_SCANCODE_UNKNOWN = none)
    ScreenAnchor anchor;    // which pillarbox this button lives in
    D3DCOLOR fillColor;     // normal fill (ARGB, semi-transparent white)
    D3DCOLOR fillPressed;   // pressed fill (higher alpha)
    D3DCOLOR borderColor;   // border ring color
    const char *label;      // text label
    float textScale;        // text scale
    bool isToggle;          // true = toggle mode (tap on/off), false = hold mode
    bool alwaysVisible;     // true = show in ALL scenes (menu + gameplay)
};

// centerX is set to -radius so the hit-test center matches the visual
// center in the pillarbox.  The renderer places each button so its right
// edge touches the game viewport left edge:
//     sx_screen = offsetX - sr        (where sr = radius * yScale)
// Converting sx_screen back to game coordinates gives gameX = -radius.
// Using a positive centerX (e.g. 4.0) placed the hit-test center inside
// the game viewport, roughly 72 px to the RIGHT of the visual circle on
// a typical 2400×1080 phone, causing taps on the left half of the visible
// circle to miss.
static const VirtualButtonDef kButtons[] = {
    // ── 左侧黑边: 游戏操作按钮 (LeftPillar) ──
    // btn_ESC (menu/pause) — hold mode
    { -24.0f,  46.0f, 24.0f, 32.0f, TH_BUTTON_MENU, SDL_SCANCODE_UNKNOWN, ScreenAnchor::LeftPillar,
       0x60FFFFFF, 0x98FFFFFF, 0xB0FFFFFF, "ESC", 1.0f, false, false },
    // btn_Z (shoot) — toggle mode: tap to start shooting, tap again to stop
    { -28.0f, 222.0f, 28.0f, 36.0f, TH_BUTTON_SHOOT, SDL_SCANCODE_UNKNOWN, ScreenAnchor::LeftPillar,
       0x60FFFFFF, 0x98FFFFFF, 0xB0FFFFFF, "Z",   1.6f, true,  false },
    // btn_S (focus/slow) — toggle mode: tap to enable slow, tap again to disable
    { -28.0f, 302.0f, 28.0f, 36.0f, TH_BUTTON_FOCUS, SDL_SCANCODE_UNKNOWN, ScreenAnchor::LeftPillar,
       0x60FFFFFF, 0x98FFFFFF, 0xB0FFFFFF, "S",   1.6f, true,  false },
    // btn_X (bomb) — hold mode
    { -28.0f, 382.0f, 28.0f, 36.0f, TH_BUTTON_BOMB, SDL_SCANCODE_UNKNOWN, ScreenAnchor::LeftPillar,
       0x60FFFFFF, 0x98FFFFFF, 0xB0FFFFFF, "X",   1.6f, false, false },

    // ── 右侧黑边: 常驻功能键 (RightPillar, alwaysVisible=true) ──
    // btn_~ (tilde/grave) — hold mode, 输出 scancode
    { 660.0f,  80.0f, 20.0f, 28.0f, 0, SDL_SCANCODE_GRAVE, ScreenAnchor::RightPillar,
       0x60FFFFFF, 0x98FFFFFF, 0xB0FFFFFF, "~",   1.4f, false, true },
    // btn_F11 — hold mode, 输出 scancode
    { 660.0f, 140.0f, 20.0f, 28.0f, 0, SDL_SCANCODE_F11, ScreenAnchor::RightPillar,
       0x60FFFFFF, 0x98FFFFFF, 0xB0FFFFFF, "F11", 0.85f, false, true },
    // btn_Backspace (←) — hold mode, 输出 scancode
    { 660.0f, 200.0f, 20.0f, 28.0f, 0, SDL_SCANCODE_BACKSPACE, ScreenAnchor::RightPillar,
       0x60FFFFFF, 0x98FFFFFF, 0xB0FFFFFF, "BS",  1.0f, false, true },
};

static constexpr int kButtonCount = sizeof(kButtons) / sizeof(kButtons[0]);

// ────────────────────────────────────────────────────────────────────────────
// State
// ────────────────────────────────────────────────────────────────────────────

static SDL_FingerID g_HeldFinger[kButtonCount];
static bool g_Held[kButtonCount] = {};

void TouchVirtualButtons::Init()
{
    Reset();
}

void TouchVirtualButtons::Reset()
{
    for (int i = 0; i < kButtonCount; i++)
    {
        g_Held[i] = false;
        g_HeldFinger[i] = -1;
    }
}

// ────────────────────────────────────────────────────────────────────────────
// Touch handling
// ────────────────────────────────────────────────────────────────────────────

bool TouchVirtualButtons::HandleFingerDown(SDL_FingerID fingerId, float gameX, float gameY)
{
    bool gameplayVisible = ShouldShow();

    for (int i = 0; i < kButtonCount; i++)
    {
        // 非 alwaysVisible 按钮只在 gameplay 时可用.
        if (!kButtons[i].alwaysVisible && !gameplayVisible)
            continue;
        // alwaysVisible 按钮需要 IsEnabled() 检查.
        if (kButtons[i].alwaysVisible && !AndroidTouchInput::IsEnabled())
            continue;
        float dx = gameX - kButtons[i].centerX;
        float dy = gameY - kButtons[i].centerY;
        float dist = std::sqrt(dx * dx + dy * dy);
        if (dist <= kButtons[i].hitRadius)
        {
            if (kButtons[i].isToggle)
            {
                // Toggle mode: tap to switch on/off
                g_Held[i] = !g_Held[i];
                g_HeldFinger[i] = fingerId;
                SDL_Log("[vbtn] %s toggled %s", kButtons[i].label,
                        g_Held[i] ? "ON" : "OFF");
            }
            else
            {
                // Hold mode: held while finger is down
                g_Held[i] = true;
                g_HeldFinger[i] = fingerId;
                SDL_Log("[vbtn] %s pressed", kButtons[i].label);
            }
            return true;
        }
    }
    return false;
}

bool TouchVirtualButtons::HandleFingerUp(SDL_FingerID fingerId)
{
    for (int i = 0; i < kButtonCount; i++)
    {
        if (g_HeldFinger[i] == fingerId)
        {
            g_HeldFinger[i] = -1;
            if (!kButtons[i].isToggle)
            {
                // Hold mode: release on finger up
                g_Held[i] = false;
                SDL_Log("[vbtn] %s released", kButtons[i].label);
            }
            // Toggle mode: state persists, just clear finger tracking
            return true;
        }
    }
    return false;
}

u16 TouchVirtualButtons::GetButtonFlags()
{
    // Pause/retry menu: block gameplay button output but PRESERVE toggle state.
    // alwaysVisible buttons still output their scancodes.
    bool blockGameplay = g_GameManager.isInRetryMenu || g_GameManager.isInGameMenu;

    if (!ShouldShow() && !blockGameplay)
    {
        // Not in gameplay and not in pause menu: reset gameplay buttons,
        // but keep alwaysVisible buttons active.
        for (int i = 0; i < kButtonCount; i++)
        {
            if (!kButtons[i].alwaysVisible)
            {
                g_Held[i] = false;
                g_HeldFinger[i] = -1;
            }
        }
    }

    u16 flags = 0;
    for (int i = 0; i < kButtonCount; i++)
    {
        if (!g_Held[i])
            continue;

        // Gameplay buttons are blocked during pause/retry and non-gameplay.
        if (!kButtons[i].alwaysVisible && (blockGameplay || !ShouldShow()))
            continue;

        flags |= kButtons[i].buttonFlag;
        // 功能键: 通过 scancode 而非 TouhouButton flag 输出.
        if (kButtons[i].scancode != SDL_SCANCODE_UNKNOWN)
            AndroidTouchInput::InjectHeldScancode(kButtons[i].scancode);
    }
    return flags;
}

bool TouchVirtualButtons::ShouldShow()
{
    if (!AndroidTouchInput::IsEnabled())
        return false;

    // isInMenu == 1 means normal gameplay (no overlay menus).
    // isInMenu == 0 means main menu, pause menu, or retry menu.
    if (g_GameManager.isInMenu == 0)
        return false;

    return true;
}

// ────────────────────────────────────────────────────────────────────────────
// Screen-space button info for renderer overlay
// ────────────────────────────────────────────────────────────────────────────

int TouchVirtualButtons::GetButtonInfo(TouchButtonInfo *out, int maxCount)
{
    bool gameplayVisible = ShouldShow();
    bool touchEnabled = AndroidTouchInput::IsEnabled();
    if (!gameplayVisible && !touchEnabled)
        return 0;

    int count = 0;
    for (int i = 0; i < kButtonCount && count < maxCount; i++)
    {
        // 非 alwaysVisible 按钮只在 gameplay 时显示.
        if (!kButtons[i].alwaysVisible && !gameplayVisible)
            continue;
        // alwaysVisible 按钮在触控启用时始终显示.
        if (kButtons[i].alwaysVisible && !touchEnabled)
            continue;

        out[count].gameY       = kButtons[i].centerY;
        out[count].gameRadius  = kButtons[i].radius;
        out[count].fillColor   = g_Held[i] ? kButtons[i].fillPressed : kButtons[i].fillColor;
        out[count].borderColor = kButtons[i].borderColor;
        out[count].label       = kButtons[i].label;
        out[count].textScale   = kButtons[i].textScale;
        out[count].held        = g_Held[i];
        out[count].anchor      = kButtons[i].anchor;
        count++;
    }
    return count;
}

bool TouchVirtualButtons::IsTrackedFinger(SDL_FingerID fingerId)
{
    for (int i = 0; i < kButtonCount; i++)
    {
        if (g_Held[i] && g_HeldFinger[i] == fingerId)
            return true;
    }
    return false;
}

} // namespace th06
