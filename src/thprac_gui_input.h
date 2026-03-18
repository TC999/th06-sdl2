#pragma once
// thprac_gui_input.h - Adapter for th06 source build
// Replaces Win32 VK_ input with SDL2 keyboard state

#include <string>

namespace THPrac {
namespace Gui {

    // In-Game Input (replaces Win32 VK-based input)
    enum ingame_input_gen_t {
        INGAGME_INPUT_NONE,
        INGAGME_INPUT_GEN1,
        INGAGME_INPUT_GEN2,
    };

    bool InGameInputInit(ingame_input_gen_t gen, int reg1, int reg2, int reg3 = 0);
    bool InGameInputGet(int key);

    // Keyboard Input
    int  KeyboardInputUpdate(int v_key);
    void KeyboardInputUpdate();
    int  KeyboardInputGet(int v_key);
    bool KeyboardInputGetSingle(int v_key);
    bool KeyboardInputGetRaw(int v_key);
    void ResetKeyboardState();

    bool InGameInputGetConfirm();

    // Menu Chords
    enum ChordKeys {
        ChordKey_Ctrl,
        ChordKey_Shift,
        ChordKey_Alt,
        ChordKey_Caps,
        ChordKey_Tab,
        ChordKey_Space,
        ChordKey_Backspace,
        ChordKey_F11,
        ChordKey_F12,
        ChordKey_Insert,
        ChordKey_Home,
        ChordKey_PgUp,
        ChordKey_Delete,
        ChordKey_End,
        ChordKey_PgDn,
        ChordKey_KEYBOARD_COUNT,
        ChordKey_DPad_Up = ChordKey_KEYBOARD_COUNT,
        ChordKey_DPad_Down,
        ChordKey_DPad_Left,
        ChordKey_DPad_Right,
        ChordKey_A,
        ChordKey_B,
        ChordKey_X,
        ChordKey_Y,
        ChordKey_L1,
        ChordKey_L2,
        ChordKey_R1,
        ChordKey_R2,
        ChordKey_Start,
        ChordKey_Select,
        ChordKey_HomeMenu,
        ChordKey_COUNT
    };

    int GetChordPressedDuration(int chord);
    bool GetChordPressed(int chord);

    int GetBackspaceMenuChord();
    int GetAdvancedMenuChord();
    int GetSpecialMenuChord();
    int GetScreenshotChord();
    int GetLanguageChord();

    std::string HotkeyChordToLabel(int chord);
    int HotkeyChordToVK(int chord);
}
}
