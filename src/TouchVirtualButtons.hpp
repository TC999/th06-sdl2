#pragma once

#include "inttypes.hpp"
#include "Controller.hpp"
#include "sdl2_compat.hpp"
#include <SDL.h>

namespace th06
{

// Which pillarbox (or screen edge) a touch button is anchored to.
// The renderer uses this to compute the button's screen-space X position:
//   LeftPillar  → button right edge touches game viewport left edge
//   RightPillar → button left edge touches game viewport right edge
enum class ScreenAnchor
{
    LeftPillar,
    RightPillar,
};

// Info struct for screen-space overlay rendering.
struct TouchButtonInfo
{
    float gameY;         // Y position in game coordinates (640×480)
    float gameRadius;    // visual radius in game coordinates
    D3DCOLOR fillColor;
    D3DCOLOR borderColor;
    const char *label;
    float textScale;
    bool held;           // currently pressed
    ScreenAnchor anchor; // which pillarbox to render on
};

// Simple virtual button overlay for touch input.
// Draws circular buttons on the screen-space black border during gameplay.
// Touch detection maps finger events to TouhouButton flags.
namespace TouchVirtualButtons
{

// Initialize button layout. Call once at startup.
void Init();

// Process a finger-down event. Returns true if the finger hit a button
// (caller should NOT pass this finger to the movement/tap system).
bool HandleFingerDown(SDL_FingerID fingerId, float gameX, float gameY);

// Process a finger-up event. Returns true if the finger belonged to a button.
bool HandleFingerUp(SDL_FingerID fingerId);

// Returns the current frame's button flags from held virtual buttons.
u16 GetButtonFlags();

// Get button layout info for screen-space overlay rendering.
// Returns 0 if buttons should not be shown, otherwise the number of buttons.
int GetButtonInfo(TouchButtonInfo *out, int maxCount);

// Returns true if virtual buttons should be shown (gameplay scene, touch enabled).
bool ShouldShow();

// Reset all button state.
void Reset();

// Returns true if the given finger is currently held by a virtual button.
bool IsTrackedFinger(SDL_FingerID fingerId);

}; // namespace TouchVirtualButtons
}; // namespace th06
