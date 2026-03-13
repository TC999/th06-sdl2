#include "Controller.hpp"

#include "GameErrorContext.hpp"
#include "Supervisor.hpp"
#include "diffbuild.hpp"
#include "i18n.hpp"
#include "utils.hpp"
#include <cstring>

namespace th06
{
DIFFABLE_STATIC(u16, g_FocusButtonConflictState)

static SDL_GameController *g_SDLController = NULL;
static SDL_Joystick *g_SDLJoystick = NULL;
static i32 g_SDLJoystickNumButtons = 0;
static i32 g_SDLJoystickNumAxes = 0;

u16 Controller::GetJoystickCaps(void)
{
    if (SDL_NumJoysticks() < 1)
    {
        GameErrorContext::Log(&g_GameErrorContext, TH_ERR_NO_PAD_FOUND);
        return 1;
    }

    g_SDLJoystick = SDL_JoystickOpen(0);
    if (g_SDLJoystick == NULL)
    {
        GameErrorContext::Log(&g_GameErrorContext, TH_ERR_NO_PAD_FOUND);
        return 1;
    }

    g_SDLJoystickNumButtons = SDL_JoystickNumButtons(g_SDLJoystick);
    g_SDLJoystickNumAxes = SDL_JoystickNumAxes(g_SDLJoystick);
    return 0;
}

#define KEYBOARD_KEY_PRESSED(button, x) keyboardState[x] ? button : 0

u16 Controller::GetControllerInput(u16 buttons)
{
    u32 ac;
    u32 joyButtons;
    i16 axisX, axisY;
    i32 axisThreshold;

    if (g_SDLJoystick == NULL)
    {
        if (g_SDLJoystickNumButtons == 0)
        {
            return buttons;
        }
        return buttons;
    }

    SDL_JoystickUpdate();

    joyButtons = 0;
    for (i32 i = 0; i < g_SDLJoystickNumButtons && i < 32; i++)
    {
        if (SDL_JoystickGetButton(g_SDLJoystick, i))
        {
            joyButtons |= (1 << i);
        }
    }

    ac = SetButtonFromControllerInputs(&buttons, g_Supervisor.cfg.controllerMapping.shootButton, TH_BUTTON_SHOOT,
                                       joyButtons);

    if (g_ControllerMapping.shootButton != g_ControllerMapping.focusButton)
    {
        SetButtonFromControllerInputs(&buttons, g_Supervisor.cfg.controllerMapping.focusButton, TH_BUTTON_FOCUS,
                                      joyButtons);
    }
    else
    {
        if (ac != 0)
        {
            if (g_FocusButtonConflictState < 16)
            {
                g_FocusButtonConflictState++;
            }

            if (g_FocusButtonConflictState >= 8)
            {
                buttons |= TH_BUTTON_FOCUS;
            }
        }
        else
        {
            if (g_FocusButtonConflictState > 8)
            {
                g_FocusButtonConflictState -= 8;
            }
            else
            {
                g_FocusButtonConflictState = 0;
            }
        }
    }

    SetButtonFromControllerInputs(&buttons, g_Supervisor.cfg.controllerMapping.bombButton, TH_BUTTON_BOMB,
                                  joyButtons);
    SetButtonFromControllerInputs(&buttons, g_Supervisor.cfg.controllerMapping.menuButton, TH_BUTTON_MENU,
                                  joyButtons);
    SetButtonFromControllerInputs(&buttons, g_Supervisor.cfg.controllerMapping.upButton, TH_BUTTON_UP,
                                  joyButtons);
    SetButtonFromControllerInputs(&buttons, g_Supervisor.cfg.controllerMapping.downButton, TH_BUTTON_DOWN,
                                  joyButtons);
    SetButtonFromControllerInputs(&buttons, g_Supervisor.cfg.controllerMapping.leftButton, TH_BUTTON_LEFT,
                                  joyButtons);
    SetButtonFromControllerInputs(&buttons, g_Supervisor.cfg.controllerMapping.rightButton, TH_BUTTON_RIGHT,
                                  joyButtons);
    SetButtonFromControllerInputs(&buttons, g_Supervisor.cfg.controllerMapping.skipButton, TH_BUTTON_SKIP,
                                  joyButtons);

    axisThreshold = 16384;
    if (g_SDLJoystickNumAxes >= 1)
    {
        axisX = SDL_JoystickGetAxis(g_SDLJoystick, 0);
        if (axisX > axisThreshold)
            buttons |= TH_BUTTON_RIGHT;
        if (axisX < -axisThreshold)
            buttons |= TH_BUTTON_LEFT;
    }
    if (g_SDLJoystickNumAxes >= 2)
    {
        axisY = SDL_JoystickGetAxis(g_SDLJoystick, 1);
        if (axisY > axisThreshold)
            buttons |= TH_BUTTON_DOWN;
        if (axisY < -axisThreshold)
            buttons |= TH_BUTTON_UP;
    }

    return buttons;
}

u32 Controller::SetButtonFromControllerInputs(u16 *outButtons, i16 controllerButtonToTest,
                                              enum TouhouButton touhouButton, u32 inputButtons)
{
    u32 mask;

    if (controllerButtonToTest < 0)
    {
        return 0;
    }

    mask = 1 << controllerButtonToTest;

    *outButtons |= (inputButtons & mask ? touhouButton & 0xFFFF : 0);

    return inputButtons & mask ? touhouButton & 0xFFFF : 0;
}

DIFFABLE_STATIC_ARRAY(u8, (32 * 4), g_ControllerData)

u8 *th06::Controller::GetControllerState()
{
    memset(&g_ControllerData, 0, sizeof(g_ControllerData));
    if (g_SDLJoystick == NULL)
    {
        return g_ControllerData;
    }

    SDL_JoystickUpdate();

    for (i32 i = 0; i < g_SDLJoystickNumButtons && i < 32; i++)
    {
        if (SDL_JoystickGetButton(g_SDLJoystick, i))
        {
            g_ControllerData[i] = 0x80;
        }
    }
    return g_ControllerData;
}

u16 Controller::GetInput(void)
{
    const u8 *keyboardState;
    u16 buttons;

    buttons = 0;
    keyboardState = SDL_GetKeyboardState(NULL);

    buttons |= KEYBOARD_KEY_PRESSED(TH_BUTTON_UP, SDL_SCANCODE_UP);
    buttons |= KEYBOARD_KEY_PRESSED(TH_BUTTON_DOWN, SDL_SCANCODE_DOWN);
    buttons |= KEYBOARD_KEY_PRESSED(TH_BUTTON_LEFT, SDL_SCANCODE_LEFT);
    buttons |= KEYBOARD_KEY_PRESSED(TH_BUTTON_RIGHT, SDL_SCANCODE_RIGHT);
    buttons |= KEYBOARD_KEY_PRESSED(TH_BUTTON_UP, SDL_SCANCODE_KP_8);
    buttons |= KEYBOARD_KEY_PRESSED(TH_BUTTON_DOWN, SDL_SCANCODE_KP_2);
    buttons |= KEYBOARD_KEY_PRESSED(TH_BUTTON_LEFT, SDL_SCANCODE_KP_4);
    buttons |= KEYBOARD_KEY_PRESSED(TH_BUTTON_RIGHT, SDL_SCANCODE_KP_6);
    buttons |= KEYBOARD_KEY_PRESSED(TH_BUTTON_UP_LEFT, SDL_SCANCODE_KP_7);
    buttons |= KEYBOARD_KEY_PRESSED(TH_BUTTON_UP_RIGHT, SDL_SCANCODE_KP_9);
    buttons |= KEYBOARD_KEY_PRESSED(TH_BUTTON_DOWN_LEFT, SDL_SCANCODE_KP_1);
    buttons |= KEYBOARD_KEY_PRESSED(TH_BUTTON_DOWN_RIGHT, SDL_SCANCODE_KP_3);
    buttons |= KEYBOARD_KEY_PRESSED(TH_BUTTON_HOME, SDL_SCANCODE_HOME);
    buttons |= KEYBOARD_KEY_PRESSED(TH_BUTTON_SHOOT, SDL_SCANCODE_Z);
    buttons |= KEYBOARD_KEY_PRESSED(TH_BUTTON_BOMB, SDL_SCANCODE_X);
    buttons |= KEYBOARD_KEY_PRESSED(TH_BUTTON_FOCUS, SDL_SCANCODE_LSHIFT);
    buttons |= KEYBOARD_KEY_PRESSED(TH_BUTTON_FOCUS, SDL_SCANCODE_RSHIFT);
    buttons |= KEYBOARD_KEY_PRESSED(TH_BUTTON_MENU, SDL_SCANCODE_ESCAPE);
    buttons |= KEYBOARD_KEY_PRESSED(TH_BUTTON_SKIP, SDL_SCANCODE_LCTRL);
    buttons |= KEYBOARD_KEY_PRESSED(TH_BUTTON_SKIP, SDL_SCANCODE_RCTRL);
    buttons |= KEYBOARD_KEY_PRESSED(TH_BUTTON_Q, SDL_SCANCODE_Q);
    buttons |= KEYBOARD_KEY_PRESSED(TH_BUTTON_S, SDL_SCANCODE_S);
    buttons |= KEYBOARD_KEY_PRESSED(TH_BUTTON_ENTER, SDL_SCANCODE_RETURN);

    return Controller::GetControllerInput(buttons);
}

void Controller::ResetKeyboard(void)
{
    SDL_PumpEvents();
}

void Controller::InitSDLController(void)
{
    if (SDL_NumJoysticks() > 0)
    {
        if (SDL_IsGameController(0))
        {
            g_SDLController = SDL_GameControllerOpen(0);
            if (g_SDLController != NULL)
            {
                g_SDLJoystick = SDL_GameControllerGetJoystick(g_SDLController);
            }
        }
        else
        {
            g_SDLJoystick = SDL_JoystickOpen(0);
        }

        if (g_SDLJoystick != NULL)
        {
            g_SDLJoystickNumButtons = SDL_JoystickNumButtons(g_SDLJoystick);
            g_SDLJoystickNumAxes = SDL_JoystickNumAxes(g_SDLJoystick);
        }
    }
}

void Controller::CloseSDLController(void)
{
    if (g_SDLController != NULL)
    {
        SDL_GameControllerClose(g_SDLController);
        g_SDLController = NULL;
        g_SDLJoystick = NULL;
    }
    else if (g_SDLJoystick != NULL)
    {
        SDL_JoystickClose(g_SDLJoystick);
        g_SDLJoystick = NULL;
    }
    g_SDLJoystickNumButtons = 0;
    g_SDLJoystickNumAxes = 0;
}
}; // namespace th06
