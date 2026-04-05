#pragma once

#include "NetplayInternal.hpp"

namespace th06::Netplay
{
struct InputCommand
{
    int clientFrame = -1;
    u16 inputBits = 0;
    InGameCtrlType ctrl = IGC_NONE;
    unsigned int seq = 0;
};
} // namespace th06::Netplay
