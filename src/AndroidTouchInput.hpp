#pragma once

#include "Controller.hpp"
#include "AnmVm.hpp"
#include <SDL.h>

namespace th06
{
namespace AndroidTouchInput
{

void Init();
void HandleFingerDown(const SDL_TouchFingerEvent &event);
void HandleFingerMotion(const SDL_TouchFingerEvent &event);
void HandleFingerUp(const SDL_TouchFingerEvent &event);

// Mouse-to-touch simulation for desktop developer mode.
void HandleMouseButtonDown(const SDL_MouseButtonEvent &event, int windowW, int windowH);
void HandleMouseMotion(const SDL_MouseMotionEvent &event, int windowW, int windowH);
void HandleMouseButtonUp(const SDL_MouseButtonEvent &event, int windowW, int windowH);

// Call once per frame after processing events, before GetInput().
void Update();

// Returns true if touch input is currently active (Android always, desktop only in dev mode).
bool IsEnabled();

// Returns the current frame's touch-generated TouhouButton flags (back gesture etc.)
u16 GetTouchButtons();

// Get the current analog direction from touch input (for gameplay movement).
// Returns a vector where active=true when touch is providing continuous direction.
// Currently reserved for future gameplay touch (virtual joystick / touchpad mode).
const AnalogInput &GetAnalogInput();

// Returns true if given scancode is virtually pressed by touch.
bool IsTouchScancode(SDL_Scancode sc);
bool IsTouchScancodeSingle(SDL_Scancode sc);

// Consume the pending tap and return its game coordinates (640x480 space).
// Returns false if no tap is pending.
bool ConsumeTap(float &gameX, float &gameY);

// Consume the pending vertical swipe and return the Y delta in game coordinates.
// Positive = swipe down, negative = swipe up. Returns false if no swipe pending.
bool ConsumeSwipeY(float &deltaY);

// Consume the pending horizontal swipe and return the X delta in game coordinates.
// Positive = swipe right, negative = swipe left. Returns false if no swipe pending.
bool ConsumeSwipeX(float &deltaX);

// Check if a point (in game coordinates) falls within an AnmVm's rendered bounding box.
bool VmContainsPoint(const AnmVm &vm, float gameX, float gameY);

// Try to hit-test a pending tap against contiguous AnmVm items.
// items[0], items[stride], items[2*stride], ... are tested.
// If a tap hits item N: sets cursor = N, injects SELECTMENU into frame input, returns true.
bool TryTouchSelect(AnmVm *items, int count, int &cursor, int stride = 1);

// Try to hit-test a pending tap against a rectangular area in game coordinates.
// If hit, injects SELECTMENU. Returns true if tapped within the rect.
bool TryTouchSelectRect(float left, float top, float right, float bottom, int index, int &cursor);

// Reset all touch state.
void Reset();

// Inject deferred button presses (right-click/back gesture) into g_CurFrameInput.
// Call AFTER Session::AdvanceFrameInput() to bypass the controller pipeline.
void InjectDeferredButtons();

}; // namespace AndroidTouchInput
}; // namespace th06
