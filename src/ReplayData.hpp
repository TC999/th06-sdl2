#pragma once

#include "inttypes.hpp"

namespace th06
{

// Replay format version that includes analog input data in the padding field.
// Version 0x102 (GAME_VERSION): original format, padding is unused/garbage.
// Version 0x103: padding repurposed as (analogX, analogY) signed bytes.
static constexpr u16 REPLAY_VERSION_ANALOG = 0x103;

struct ReplayDataInput
{
    i32 frameNum;
    u16 inputKey;
    // In replay version >= REPLAY_VERSION_ANALOG, this field stores compact
    // analog stick direction: analogX in bits [7:0], analogY in bits [15:8].
    // Each is a signed byte mapping -127..+127 to -1.0..+1.0.
    // In older replays (version 0x102), this field is unused and may contain
    // garbage — analog data must NOT be read unless the replay header version
    // is >= REPLAY_VERSION_ANALOG.
    union
    {
        u16 padding;
        struct
        {
            i8 analogX;
            i8 analogY;
        };
    };
};

struct StageReplayData
{
    i32 score;
    i16 randomSeed;
    i16 pointItemsCollected;
    u8 power;
    i8 livesRemaining;
    i8 bombsRemaining;
    u8 rank;
    i8 powerItemCountForScore;
    u8 power2;
    i8 livesRemaining2;
    i8 bombsRemaining2;
    ReplayDataInput replayInputs[53998];
};
ZUN_ASSERT_SIZE(StageReplayData, 0x69780);

struct ReplayData
{
    char magic[4];
    u16 version;
    u8 shottypeChara;
    u8 shottypeChara2;
    u8 difficulty;
    i32 checksum;
    u8 rngValue1;
    u8 rngValue2;
    i8 key;
    i8 rngValue3;
    char date[9];
    char name[8];
    i32 score;
    f32 slowdownRate2;
    f32 slowdownRate;
    f32 slowdownRate3;
    StageReplayData *stageReplayData[7];
};
ZUN_ASSERT_SIZE(ReplayData, 0x50);

// Check if a replay version is one we can load (0x102 original or 0x103 analog).
inline bool IsValidReplayVersion(u16 version)
{
    return version == 0x102 || version == REPLAY_VERSION_ANALOG;
}

}; // namespace th06
