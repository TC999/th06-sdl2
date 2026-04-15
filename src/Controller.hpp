#pragma once

#include "inttypes.hpp"
#include <SDL.h>

namespace th06
{

// ────────────────────────────────────────────────────────────────────────────
// Input semantics: context and analog direction
// ────────────────────────────────────────────────────────────────────────────

// Semantic context for interpreting direction input.
// Gameplay: player movement — supports proportional analog speed.
// Menu: UI navigation — always discrete 4/8-way, no analog scaling.
enum class InputContext
{
    Gameplay,
    Menu,
};

// How to interpret the analog vector values.
enum class AnalogMode
{
    // x, y are a direction × magnitude in [-1, 1].  Player speed is scaled
    // by magnitude (from character speed tables).
    Direction,
    // x, y are pixel displacement per frame (integer, clamped to ±127).
    // Player position is directly offset by (x, y).
    // Used by mouse-follow mode for replay-deterministic free movement.
    Displacement,
};

// Continuous analog direction vector from gamepad stick or touch input.
// When `active` is true, an analog source (gamepad axes / touch drag) is
// providing continuous direction + magnitude.  Player movement can scale
// speed proportionally.
// When `active` is false, movement uses the original discrete-speed tables.
struct AnalogInput
{
    float x;     // -1.0 (full left)  …  1.0 (full right), 0 = neutral
    float y;     // -1.0 (full up)    …  1.0 (full down),  0 = neutral
    bool active; // true ⟹ source is analog (joystick / touch)
    AnalogMode mode = AnalogMode::Direction;
};

// ────────────────────────────────────────────────────────────────────────────

enum TouhouButton
{
    TH_BUTTON_SHOOT = 1 << 0,
    TH_BUTTON_BOMB = 1 << 1,
    TH_BUTTON_FOCUS = 1 << 2,
    TH_BUTTON_MENU = 1 << 3,
    TH_BUTTON_UP = 1 << 4,
    TH_BUTTON_DOWN = 1 << 5,
    TH_BUTTON_LEFT = 1 << 6,
    TH_BUTTON_RIGHT = 1 << 7,
    TH_BUTTON_SKIP = 1 << 8,

    TH_BUTTON_SHOOT2 = 1 << 9,
    TH_BUTTON_BOMB2 = 1 << 10,
    TH_BUTTON_FOCUS2 = 1 << 11,
    TH_BUTTON_UP2 = 1 << 12,
    TH_BUTTON_DOWN2 = 1 << 13,
    TH_BUTTON_LEFT2 = 1 << 14,
    TH_BUTTON_RIGHT2 = 1 << 15,

    TH_BUTTON_UP_LEFT = TH_BUTTON_UP | TH_BUTTON_LEFT,
    TH_BUTTON_UP_RIGHT = TH_BUTTON_UP | TH_BUTTON_RIGHT,
    TH_BUTTON_DOWN_LEFT = TH_BUTTON_DOWN | TH_BUTTON_LEFT,
    TH_BUTTON_DOWN_RIGHT = TH_BUTTON_DOWN | TH_BUTTON_RIGHT,
    TH_BUTTON_DIRECTION = TH_BUTTON_DOWN | TH_BUTTON_RIGHT | TH_BUTTON_UP | TH_BUTTON_LEFT,

    TH_BUTTON_UP_LEFT2 = TH_BUTTON_UP2 | TH_BUTTON_LEFT2,
    TH_BUTTON_UP_RIGHT2 = TH_BUTTON_UP2 | TH_BUTTON_RIGHT2,
    TH_BUTTON_DOWN_LEFT2 = TH_BUTTON_DOWN2 | TH_BUTTON_LEFT2,
    TH_BUTTON_DOWN_RIGHT2 = TH_BUTTON_DOWN2 | TH_BUTTON_RIGHT2,
    TH_BUTTON_DIRECTION2 = TH_BUTTON_DOWN2 | TH_BUTTON_RIGHT2 | TH_BUTTON_UP2 | TH_BUTTON_LEFT2,

    TH_BUTTON_SELECTMENU = TH_BUTTON_SHOOT,
    TH_BUTTON_RETURNMENU = TH_BUTTON_MENU | TH_BUTTON_BOMB,
    TH_BUTTON_WRONG_CHEATCODE = TH_BUTTON_SHOOT | TH_BUTTON_BOMB | TH_BUTTON_MENU,
    TH_BUTTON_ANY = 0xFFFF,
};

namespace Controller
{
struct RuntimeState
{
    u16 focusButtonConflictState;
};

RuntimeState CaptureRuntimeState();
void RestoreRuntimeState(const RuntimeState &state);

u16 GetJoystickCaps(void);
u32 SetButtonFromControllerInputs(u16 *outButtons, i16 controllerButtonToTest, enum TouhouButton touhouButton,
                                  u32 inputButtons);
u16 GetControllerInput(u16 buttons);
u8 *GetControllerState();
u16 GetInput(void);
const AnalogInput &GetAnalogInput(void);
void SetAnalogInput(const AnalogInput &input);
const AnalogInput &GetAnalogInputP2(void);
void SetAnalogInputP2(const AnalogInput &input);
void ResetKeyboard(void);
void ResetDeviceInputState(void);
void ResetInputState(void);
void InitSDLController(void);
void RefreshSDLController(void);
void CloseSDLController(void);
}; // namespace Controller
}; // namespace th06
