#pragma once

#include "inttypes.hpp"
#include <cstring>

// Per-frame touch event data synchronized through lockstep packets.
// All coordinates are in game-space (640x480).
// NOTE: This header is included inside namespace th06::Netplay in NetplayInternal.hpp.
// Do NOT add a namespace wrapper here.
#pragma pack(push, 1)
struct TouchFrameData
{
    u8 flags = 0; // bit0=tapPending, bit1=swipeXPending, bit2=swipeYPending,
                  // bit3=deferredBomb, bit4=analogActive
    float tapGameX = 0;
    float tapGameY = 0;
    float swipeXDelta = 0;
    float swipeYDelta = 0;
    i8 analogX = 0;
    i8 analogY = 0;

    static constexpr u8 kFlagTap = 1 << 0;
    static constexpr u8 kFlagSwipeX = 1 << 1;
    static constexpr u8 kFlagSwipeY = 1 << 2;
    static constexpr u8 kFlagBomb = 1 << 3;
    static constexpr u8 kFlagAnalog = 1 << 4;

    void Clear() { std::memset(this, 0, sizeof(*this)); }
};
#pragma pack(pop)
