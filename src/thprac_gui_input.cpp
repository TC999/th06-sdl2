#include "thprac_gui_input.h"
#include <SDL.h>
#include <cstring>

// VK_ key code mappings (Win32 → SDL2)
#ifndef VK_LEFT
#define VK_LEFT   0x25
#define VK_UP     0x26
#define VK_RIGHT  0x27
#define VK_DOWN   0x28
#define VK_LSHIFT 0xA0
#define VK_RSHIFT 0xA1
#define VK_RETURN 0x0D
#define VK_ESCAPE 0x1B
#define VK_SPACE  0x20
#define VK_F1     0x70
#define VK_F2     0x71
#define VK_F3     0x72
#define VK_F4     0x73
#define VK_F5     0x74
#define VK_F6     0x75
#define VK_F7     0x76
#define VK_F8     0x77
#define VK_F9     0x78
#define VK_F10    0x79
#define VK_F11    0x7A
#define VK_F12    0x7B
#endif

static SDL_Scancode VKToScancode(int vk)
{
    switch (vk) {
    case VK_LEFT:   return SDL_SCANCODE_LEFT;
    case VK_UP:     return SDL_SCANCODE_UP;
    case VK_RIGHT:  return SDL_SCANCODE_RIGHT;
    case VK_DOWN:   return SDL_SCANCODE_DOWN;
    case VK_LSHIFT: return SDL_SCANCODE_LSHIFT;
    case VK_RSHIFT: return SDL_SCANCODE_RSHIFT;
    case VK_RETURN: return SDL_SCANCODE_RETURN;
    case VK_ESCAPE: return SDL_SCANCODE_ESCAPE;
    case VK_SPACE:  return SDL_SCANCODE_SPACE;
    case VK_F1:     return SDL_SCANCODE_F1;
    case VK_F2:     return SDL_SCANCODE_F2;
    case VK_F3:     return SDL_SCANCODE_F3;
    case VK_F4:     return SDL_SCANCODE_F4;
    case VK_F5:     return SDL_SCANCODE_F5;
    case VK_F6:     return SDL_SCANCODE_F6;
    case VK_F7:     return SDL_SCANCODE_F7;
    case VK_F8:     return SDL_SCANCODE_F8;
    case VK_F9:     return SDL_SCANCODE_F9;
    case VK_F10:    return SDL_SCANCODE_F10;
    case VK_F11:    return SDL_SCANCODE_F11;
    case VK_F12:    return SDL_SCANCODE_F12;
    case 0xC0: return SDL_SCANCODE_GRAVE; // VK_OEM_3 (~/`)
    default:
        // ASCII keys map directly for A-Z (0x41-0x5A) and 0-9 (0x30-0x39)
        if (vk >= 0x41 && vk <= 0x5A)
            return (SDL_Scancode)(SDL_SCANCODE_A + (vk - 0x41));
        if (vk >= 0x30 && vk <= 0x39)
            return (SDL_Scancode)(SDL_SCANCODE_0 + (vk - 0x30));
        return SDL_SCANCODE_UNKNOWN;
    }
}

namespace THPrac {
namespace Gui {

static bool s_prev_keys[SDL_NUM_SCANCODES] = {};
static bool s_curr_keys[SDL_NUM_SCANCODES] = {};

bool InGameInputInit(ingame_input_gen_t, int, int, int)
{
    return true;
}

bool InGameInputGet(int key)
{
    int numkeys = 0;
    const Uint8* state = SDL_GetKeyboardState(&numkeys);
    SDL_Scancode sc = VKToScancode(key);
    if (sc == SDL_SCANCODE_UNKNOWN || sc >= numkeys)
        return false;
    return state[sc] != 0;
}

void KeyboardInputUpdate()
{
    int numkeys = 0;
    const Uint8* state = SDL_GetKeyboardState(&numkeys);
    memcpy(s_prev_keys, s_curr_keys, sizeof(s_prev_keys));
    for (int i = 0; i < numkeys && i < SDL_NUM_SCANCODES; i++)
        s_curr_keys[i] = state[i] != 0;
}

int KeyboardInputUpdate(int v_key)
{
    KeyboardInputUpdate();
    return KeyboardInputGet(v_key);
}

int KeyboardInputGet(int v_key)
{
    SDL_Scancode sc = VKToScancode(v_key);
    if (sc == SDL_SCANCODE_UNKNOWN)
        return 0;
    return s_curr_keys[sc] ? 1 : 0;
}

bool KeyboardInputGetSingle(int v_key)
{
    SDL_Scancode sc = VKToScancode(v_key);
    if (sc == SDL_SCANCODE_UNKNOWN)
        return false;
    return s_curr_keys[sc] && !s_prev_keys[sc];
}

bool KeyboardInputGetRaw(int v_key)
{
    int numkeys = 0;
    const Uint8* state = SDL_GetKeyboardState(&numkeys);
    SDL_Scancode sc = VKToScancode(v_key);
    if (sc == SDL_SCANCODE_UNKNOWN || sc >= numkeys)
        return false;
    return state[sc] != 0;
}

bool InGameInputGetConfirm()
{
    return InGameInputGet(VK_RETURN);
}

int GetBackspaceMenuChord()  { return ChordKey_Backspace; }
int GetAdvancedMenuChord()   { return ChordKey_F11; }
int GetSpecialMenuChord()    { return ChordKey_F12; }
int GetScreenshotChord()     { return ChordKey_Home; }
int GetLanguageChord()       { return ChordKey_PgUp; }

int GetChordPressedDuration(int chord)
{
    // Frame-counting per chord key: increments each frame held, resets on release.
    // Matches original thprac behavior where GetChordPressed checks == 1 (first frame only).
    static uint8_t s_chord_duration[ChordKey_KEYBOARD_COUNT] = {};
    static const SDL_Scancode chord_scancodes[] = {
        SDL_SCANCODE_LCTRL,    // ChordKey_Ctrl
        SDL_SCANCODE_LSHIFT,   // ChordKey_Shift
        SDL_SCANCODE_LALT,     // ChordKey_Alt
        SDL_SCANCODE_CAPSLOCK, // ChordKey_Caps
        SDL_SCANCODE_TAB,      // ChordKey_Tab
        SDL_SCANCODE_SPACE,    // ChordKey_Space
        SDL_SCANCODE_BACKSPACE,// ChordKey_Backspace
        SDL_SCANCODE_F11,      // ChordKey_F11
        SDL_SCANCODE_F12,      // ChordKey_F12
        SDL_SCANCODE_INSERT,   // ChordKey_Insert
        SDL_SCANCODE_HOME,     // ChordKey_Home
        SDL_SCANCODE_PAGEUP,   // ChordKey_PgUp
        SDL_SCANCODE_DELETE,   // ChordKey_Delete
        SDL_SCANCODE_END,      // ChordKey_End
        SDL_SCANCODE_PAGEDOWN, // ChordKey_PgDn
    };
    if (chord < 0 || chord >= ChordKey_KEYBOARD_COUNT)
        return 0;
    int numkeys = 0;
    const Uint8* state = SDL_GetKeyboardState(&numkeys);
    SDL_Scancode sc = chord_scancodes[chord];
    if (sc >= numkeys)
        return 0;
    if (state[sc]) {
        if (s_chord_duration[chord] < 255)
            s_chord_duration[chord]++;
    } else {
        s_chord_duration[chord] = 0;
    }
    return s_chord_duration[chord];
}

bool GetChordPressed(int chord)
{
    return GetChordPressedDuration(chord) == 1;
}

std::string HotkeyChordToLabel(int chord)
{
    switch (chord) {
    case ChordKey_Ctrl:      return "Ctrl";
    case ChordKey_Shift:     return "Shift";
    case ChordKey_Alt:       return "Alt";
    case ChordKey_Backspace: return "Backspace";
    case ChordKey_F11:       return "F11";
    case ChordKey_F12:       return "F12";
    case ChordKey_Home:      return "Home";
    case ChordKey_PgUp:      return "PgUp";
    default:                 return "?";
    }
}

int HotkeyChordToVK(int chord)
{
    switch (chord) {
    case ChordKey_Backspace: return 0x08;
    case ChordKey_F11:       return VK_F11;
    case ChordKey_F12:       return VK_F12;
    default:                 return 0;
    }
}

}
}
