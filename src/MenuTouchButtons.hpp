#pragma once

#include "TouchVirtualButtons.hpp"
#include <SDL.h>

namespace th06
{

// Menu-context virtual buttons for touch input.
//
// Provides OK (confirm / Z) and Cancel (back / X) buttons that appear
// when the game is NOT in gameplay (i.e. main menu, options, stage select,
// pause menu overlays, etc.).  These are the touch equivalent of THSSS's
// top-corner "ok" and "cancel" arrows under each submenu's MobileInputObjects.
//
// Buttons use a "visual hold + single-frame pulse" model:
//   - On finger-down: the button becomes visually held and a one-shot
//     TouhouButton flag is queued (the "pulse").
//   - GetButtonFlags() returns the pulse flags exactly once, then clears them.
//     This guarantees the consuming code sees exactly one WAS_PRESSED edge.
//   - On finger-up: the visual held state is cleared.
//
// Hit-testing is anchor-aware: Cancel lives in the left pillarbox,
// OK lives in the right pillarbox.  A finger that misses all buttons is
// NOT consumed, so it can still be used for menu tap-select or swipe.
namespace MenuTouchButtons
{

// Initialize button layout. Call once at startup.
void Init();

// Process a finger-down event in game coordinates (640×480 space,
// with negative X for left pillarbox and X > 640 for right pillarbox).
// Returns true if the finger hit a menu button (caller should NOT pass
// this finger to the tap / swipe system).
bool HandleFingerDown(SDL_FingerID fingerId, float gameX, float gameY);

// Process a finger-up event. Returns true if the finger belonged to
// a menu button.
bool HandleFingerUp(SDL_FingerID fingerId);

// Return the one-shot pulse flags accumulated since the last call,
// then clear them.  Callers see each button press as a single frame
// of TH_BUTTON_SHOOT (OK) or TH_BUTTON_BOMB (Cancel).
u16 GetButtonFlags();

// Fill out[] with rendering info for visible menu buttons.
// Returns 0 when buttons should not be shown.
int GetButtonInfo(TouchButtonInfo *out, int maxCount);

// Returns true when menu buttons should be visible.
// Semantic: touch is enabled AND the game is in a menu scene
// (main menu, stage select, options, pause, etc.).
bool ShouldShow();

// Reset all button state (finger tracking, held, pulse).
void Reset();

// Returns true if the given finger is currently held by a menu button.
bool IsTrackedFinger(SDL_FingerID fingerId);

}; // namespace MenuTouchButtons
}; // namespace th06
