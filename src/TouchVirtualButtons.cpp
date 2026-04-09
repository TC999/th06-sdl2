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
    u16 buttonFlag;         // TouhouButton flag
    D3DCOLOR fillColor;     // normal fill (ARGB, semi-transparent white)
    D3DCOLOR fillPressed;   // pressed fill (higher alpha)
    D3DCOLOR borderColor;   // border ring color
    const char *label;      // text label
    float textScale;        // text scale
    bool isToggle;          // true = toggle mode (tap on/off), false = hold mode
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
    // btn_ESC (menu/pause) — hold mode
    { -24.0f,  46.0f, 24.0f, 32.0f, TH_BUTTON_MENU,
       0x60FFFFFF, 0x98FFFFFF, 0xB0FFFFFF, "ESC", 1.0f, false },
    // btn_Z (shoot) — toggle mode: tap to start shooting, tap again to stop
    { -28.0f, 222.0f, 28.0f, 36.0f, TH_BUTTON_SHOOT,
       0x60FFFFFF, 0x98FFFFFF, 0xB0FFFFFF, "Z",   1.6f, true  },
    // btn_S (focus/slow) — toggle mode: tap to enable slow, tap again to disable
    { -28.0f, 302.0f, 28.0f, 36.0f, TH_BUTTON_FOCUS,
       0x60FFFFFF, 0x98FFFFFF, 0xB0FFFFFF, "S",   1.6f, true  },
    // btn_X (bomb) — hold mode
    { -28.0f, 382.0f, 28.0f, 36.0f, TH_BUTTON_BOMB,
       0x60FFFFFF, 0x98FFFFFF, 0xB0FFFFFF, "X",   1.6f, false },
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
    if (!ShouldShow())
        return false;

    for (int i = 0; i < kButtonCount; i++)
    {
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
    // Pause/retry menu: block output but PRESERVE toggle state.
    // This check must come BEFORE ShouldShow() because there is a 1-frame
    // lag where isInMenu is still 1 (gameplay) after isInRetryMenu becomes 1
    // (due to chain priority: Supervisor(0) reads input before GameManager(4)
    // updates isInMenu). Without this early check, the SHOOT toggle leaks
    // into g_CurFrameInput on that frame.
    if (g_GameManager.isInRetryMenu || g_GameManager.isInGameMenu)
        return 0;

    if (!ShouldShow())
    {
        // Other non-gameplay states (title, main menu): fully reset
        for (int i = 0; i < kButtonCount; i++)
        {
            g_Held[i] = false;
            g_HeldFinger[i] = -1;
        }
        return 0;
    }

    u16 flags = 0;
    for (int i = 0; i < kButtonCount; i++)
    {
        if (g_Held[i])
            flags |= kButtons[i].buttonFlag;
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
    if (!ShouldShow())
        return 0;

    int count = kButtonCount < maxCount ? kButtonCount : maxCount;
    for (int i = 0; i < count; i++)
    {
        out[i].gameY      = kButtons[i].centerY;
        out[i].gameRadius = kButtons[i].radius;
        out[i].fillColor  = g_Held[i] ? kButtons[i].fillPressed : kButtons[i].fillColor;
        out[i].borderColor = kButtons[i].borderColor;
        out[i].label      = kButtons[i].label;
        out[i].textScale  = kButtons[i].textScale;
        out[i].held       = g_Held[i];
    }
    return count;
}

} // namespace th06
