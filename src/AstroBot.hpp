#pragma once

#include "Controller.hpp"

namespace th06::AstroBot
{
enum class Target : u8
{
    Off = 0,
    P1 = 1,
    P2 = 2,
};

enum class Action : u8
{
    Idle = 0,
    Up,
    Down,
    Left,
    Right,
    UpLeft,
    UpRight,
    DownLeft,
    DownRight,
};

enum class BypassReason : u8
{
    None = 0,
    Disabled,
    NoTarget,
    NotGameplay,
    NetplayInactive,
    ReplayOrDemo,
    SharedShell,
    PortableRestoreBusy,
    NoPlayer,
    PlayerInactive,
};

enum class ModeHint : u8
{
    None = 0,
    Force,
    Dest,
    Escape,
    Thread,
    MoveCheck,
};

struct StatusSnapshot
{
    bool active = false;
    bool eligible = false;
    bool remoteSession = false;
    Target target = Target::Off;
    Action action = Action::Idle;
    bool focus = false;
    bool bombCoolingDown = false;
    bool willBomb = false;
    float danger = 0.0f;
    int forecastThreatCount = 0;
    BypassReason bypassReason = BypassReason::None;
    ModeHint modeHint = ModeHint::None;
    u16 botInputBits = 0;
    u16 finalInputBits = 0;
};

u16 ProcessLocalGameplayInput(u16 nextInput);
u16 ProcessNetplayLocalInput(u16 nextInput);
StatusSnapshot GetStatusSnapshot();
} // namespace th06::AstroBot
