#include "MenuTouchButtons.hpp"
#include "AndroidTouchInput.hpp"
#include "GameManager.hpp"

#include <SDL.h>
#include <cmath>

namespace th06
{

// ────────────────────────────────────────────────────────────────────────────
// Scene detection — semantic helpers
// ────────────────────────────────────────────────────────────────────────────
//
// g_GameManager.isInMenu has counter-intuitive semantics inherited from
// the original TH06 code:
//   isInMenu == 1  →  normal gameplay (player is "in the game", no menus)
//   isInMenu == 0  →  a menu is active (main menu, pause, retry, etc.)
//
// We wrap this in a semantic function so every call site reads clearly.

static bool IsMenuScene()
{
    return g_GameManager.isInMenu == 0;
}

// ────────────────────────────────────────────────────────────────────────────
// Button definitions
// ────────────────────────────────────────────────────────────────────────────
//
// Ported from THSSS (東方夏夜祭 v1.0.2)'s MobileInputObjects architecture:
//   cancel (top-left corner)  → replaces X key (back / cancel)
//   ok     (top-right corner) → replaces Z key (confirm / select)
//
// THSSS positions (Unity canvas 1920×1080, origin = center, Y-up):
//   cancel: (-639.8, 455.3)  →  screen (320, 85)  →  top-left
//   ok:     ( 639.8, 455.3)  →  screen (1600, 85)  →  top-right
//
// Coordinate mapping to th06_sdl (game viewport 640×480):
//   The game viewport is centered on screen with black pillarboxes on
//   wider displays.  NormalizedToGameCoords() maps touches such that:
//     - Left pillarbox  → gameX < 0
//     - Right pillarbox → gameX > 640
//   Specifically, the visual center of a button whose right edge touches
//   the game viewport left edge is at gameX = -radius (see comment in
//   TouchVirtualButtons.cpp).  By symmetry, the visual center of a button
//   whose left edge touches the game viewport right edge is at
//   gameX = 640 + radius.
//
//   Derivation (right pillarbox):
//     Screen-space center:  sx = rw - offsetX + sr
//     Game-space X:         (sx - offsetX) × 640 / scaledW
//       = (rw - 2·offsetX + sr) × 640 / scaledW
//       = (scaledW + sr) × 640 / scaledW
//       = 640 + sr × 640 / scaledW
//     Since sr = radius × (scaledH / 480) and scaledW = scaledH × 640/480:
//       = 640 + radius
//
//   So centerX = -radius  →  left pillarbox center.
//      centerX = 640+radius →  right pillarbox center.

struct MenuButtonDef
{
    ScreenAnchor anchor;    // which pillarbox
    float centerX;          // game-coordinate X for hit-test (see derivation above)
    float centerY;          // game-coordinate Y (maps to screen Y via scaledH/480)
    float radius;           // visual radius in game coordinates
    float hitRadius;        // touch hit detection radius (slightly larger than visual)
    u16 buttonFlag;         // TouhouButton flag to pulse on tap
    D3DCOLOR fillColor;     // normal fill
    D3DCOLOR fillPressed;   // pressed fill (higher alpha)
    D3DCOLOR borderColor;   // border ring color
    const char *label;      // text label rendered on the button
    float textScale;        // text rendering scale
};

static const MenuButtonDef kMenuButtons[] = {
    // Cancel (←) — left pillarbox, upper area
    // Maps to TH_BUTTON_BOMB so that WAS_PRESSED(TH_BUTTON_RETURNMENU) fires
    // (TH_BUTTON_RETURNMENU = TH_BUTTON_MENU | TH_BUTTON_BOMB, WAS_PRESSED
    //  triggers on ANY newly-pressed bit).
    {
        ScreenAnchor::LeftPillar,
        -22.0f,             // centerX = -radius → left pillarbox center
        30.0f,              // centerY: near top of screen
        22.0f,              // visual radius
        30.0f,              // hit radius (generous for fingertip)
        TH_BUTTON_BOMB,
        0x60FFFFFF,         // semi-transparent white fill
        0x98FFFFFF,         // brighter when pressed
        0xB0FFFFFF,         // border
        "<",                // left-arrow label
        1.6f,
    },
    // OK (→) — right pillarbox, upper area
    // Maps to TH_BUTTON_SHOOT so that WAS_PRESSED(TH_BUTTON_SELECTMENU) fires.
    {
        ScreenAnchor::RightPillar,
        640.0f + 22.0f,     // centerX = 640 + radius → right pillarbox center
        30.0f,              // centerY: near top of screen
        22.0f,              // visual radius
        30.0f,              // hit radius
        TH_BUTTON_SHOOT,
        0x60FFFFFF,
        0x98FFFFFF,
        0xB0FFFFFF,
        ">",                // right-arrow label
        1.6f,
    },
};

static constexpr int kMenuButtonCount = sizeof(kMenuButtons) / sizeof(kMenuButtons[0]);

// ────────────────────────────────────────────────────────────────────────────
// State
// ────────────────────────────────────────────────────────────────────────────

// Each button independently tracks its bound finger.
static SDL_FingerID g_HeldFinger[kMenuButtonCount];
static bool g_Held[kMenuButtonCount] = {};

// One-shot pulse flags: set on finger-down, consumed by GetButtonFlags().
// This ensures the game sees exactly one WAS_PRESSED edge per tap.
static u16 g_PulseFlags = 0;

// ────────────────────────────────────────────────────────────────────────────
// Lifecycle
// ────────────────────────────────────────────────────────────────────────────

void MenuTouchButtons::Init()
{
    Reset();
}

void MenuTouchButtons::Reset()
{
    for (int i = 0; i < kMenuButtonCount; i++)
    {
        g_Held[i] = false;
        g_HeldFinger[i] = -1;
    }
    g_PulseFlags = 0;
}

// ────────────────────────────────────────────────────────────────────────────
// Touch handling
// ────────────────────────────────────────────────────────────────────────────

bool MenuTouchButtons::HandleFingerDown(SDL_FingerID fingerId, float gameX, float gameY)
{
    if (!ShouldShow())
        return false;

    for (int i = 0; i < kMenuButtonCount; i++)
    {
        // Skip if this button is already held by another finger.
        if (g_Held[i])
            continue;

        float dx = gameX - kMenuButtons[i].centerX;
        float dy = gameY - kMenuButtons[i].centerY;
        float dist = std::sqrt(dx * dx + dy * dy);
        if (dist <= kMenuButtons[i].hitRadius)
        {
            g_Held[i] = true;
            g_HeldFinger[i] = fingerId;
            g_PulseFlags |= kMenuButtons[i].buttonFlag;
            SDL_Log("[menubtn] %s pressed (pulse 0x%x)",
                    kMenuButtons[i].label, kMenuButtons[i].buttonFlag);
            return true; // consume this finger — don't pass to tap/swipe
        }
    }
    return false; // miss — let caller pass to normal menu tap detection
}

bool MenuTouchButtons::HandleFingerUp(SDL_FingerID fingerId)
{
    for (int i = 0; i < kMenuButtonCount; i++)
    {
        if (g_HeldFinger[i] == fingerId)
        {
            g_Held[i] = false;
            g_HeldFinger[i] = -1;
            SDL_Log("[menubtn] %s released", kMenuButtons[i].label);
            return true;
        }
    }
    return false;
}

// ────────────────────────────────────────────────────────────────────────────
// Button flags — single-frame pulse
// ────────────────────────────────────────────────────────────────────────────

u16 MenuTouchButtons::GetButtonFlags()
{
    if (!ShouldShow())
    {
        // Not in a menu scene: clear everything so stale state doesn't leak.
        Reset();
        return 0;
    }

    // Return accumulated pulse flags and clear them.
    // The caller (AndroidTouchInput::GetTouchButtons) OR's this into the
    // frame's button word, and the macro WAS_PRESSED detects the edge
    // because the flag was absent on the previous frame.
    u16 flags = g_PulseFlags;
    g_PulseFlags = 0;
    return flags;
}

// ────────────────────────────────────────────────────────────────────────────
// Visibility
// ────────────────────────────────────────────────────────────────────────────

bool MenuTouchButtons::ShouldShow()
{
    if (!AndroidTouchInput::IsEnabled())
        return false;

    return IsMenuScene();
}

// ────────────────────────────────────────────────────────────────────────────
// Rendering info
// ────────────────────────────────────────────────────────────────────────────

int MenuTouchButtons::GetButtonInfo(TouchButtonInfo *out, int maxCount)
{
    if (!ShouldShow())
        return 0;

    int count = kMenuButtonCount < maxCount ? kMenuButtonCount : maxCount;
    for (int i = 0; i < count; i++)
    {
        out[i].gameY       = kMenuButtons[i].centerY;
        out[i].gameRadius  = kMenuButtons[i].radius;
        out[i].fillColor   = g_Held[i] ? kMenuButtons[i].fillPressed
                                       : kMenuButtons[i].fillColor;
        out[i].borderColor = kMenuButtons[i].borderColor;
        out[i].label       = kMenuButtons[i].label;
        out[i].textScale   = kMenuButtons[i].textScale;
        out[i].held        = g_Held[i];
        out[i].anchor      = kMenuButtons[i].anchor;
    }
    return count;
}

bool MenuTouchButtons::IsTrackedFinger(SDL_FingerID fingerId)
{
    for (int i = 0; i < kMenuButtonCount; i++)
    {
        if (g_Held[i] && g_HeldFinger[i] == fingerId)
            return true;
    }
    return false;
}

} // namespace th06
