#pragma once

#include <stddef.h>

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

// =============================================================================
//  On-disk replay layouts (architecture-independent, stable wire formats).
//
//  These structs are byte-identical on x86 (sizeof(void*)==4) and x64
//  (sizeof(void*)==8). They are NEVER allocated as in-memory game state —
//  they only appear briefly inside LoadReplayData/SaveReplay as the bridge
//  between the file bytes and the in-memory ReplayData.
//
//  Replay files written by 32-bit TH06 builds reuse the pointer slots in
//  ReplayData::stageReplayData / LegacyReplayData::stageReplayData as 32-bit
//  file offsets (relative to start of file). On 64-bit those slots would
//  occupy 8 bytes, breaking byte compatibility, so we model the wire format
//  with explicit u32 offset arrays.
//
//  Layouts are kept identical to what the 32-bit build produces:
//    OnDiskReplayData       : 84 bytes (= 0x54), current format with
//                              shottypeChara2 (post-vanilla addition).
//    OnDiskLegacyReplayData : 80 bytes (= 0x50), original ZUN format.
// =============================================================================
struct OnDiskReplayData
{
    char magic[4];          // 0..3
    u16 version;            // 4..5
    u8 shottypeChara;       // 6
    u8 shottypeChara2;      // 7
    u8 difficulty;          // 8
                            // 9..11 padding
    i32 checksum;           // 12..15
    u8 rngValue1;           // 16
    u8 rngValue2;           // 17
    i8 key;                 // 18
    i8 rngValue3;           // 19
    char date[9];           // 20..28
    char name[8];           // 29..36
                            // 37..39 padding
    i32 score;              // 40..43
    f32 slowdownRate2;      // 44..47
    f32 slowdownRate;       // 48..51
    f32 slowdownRate3;      // 52..55
    u32 stageOffset[7];     // 56..83 (file offsets, 0 == absent)
};
static_assert(sizeof(OnDiskReplayData) == 0x54, "OnDiskReplayData wire format must be 84 bytes");
static_assert(offsetof(OnDiskReplayData, key) == 18, "OnDiskReplayData key offset");
static_assert(offsetof(OnDiskReplayData, rngValue3) == 19, "OnDiskReplayData rngValue3 offset");
static_assert(offsetof(OnDiskReplayData, stageOffset) == 56, "OnDiskReplayData stageOffset position");

struct OnDiskLegacyReplayData
{
    char magic[4];          // 0..3
    u16 version;            // 4..5
    u8 shottypeChara;       // 6
    u8 difficulty;          // 7
    i32 checksum;           // 8..11
    u8 rngValue1;           // 12
    u8 rngValue2;           // 13
    i8 key;                 // 14
    i8 rngValue3;           // 15
    char date[9];           // 16..24
    char name[8];           // 25..32
                            // 33..35 padding
    i32 score;              // 36..39
    f32 slowdownRate2;      // 40..43
    f32 slowdownRate;       // 44..47
    f32 slowdownRate3;      // 48..51
    u32 stageOffset[7];     // 52..79 (file offsets, 0 == absent)
};
static_assert(sizeof(OnDiskLegacyReplayData) == 0x50, "OnDiskLegacyReplayData wire format must be 80 bytes");
static_assert(offsetof(OnDiskLegacyReplayData, key) == 14, "OnDiskLegacyReplayData key offset");
static_assert(offsetof(OnDiskLegacyReplayData, rngValue3) == 15, "OnDiskLegacyReplayData rngValue3 offset");
static_assert(offsetof(OnDiskLegacyReplayData, stageOffset) == 52, "OnDiskLegacyReplayData stageOffset position");

// Check if a replay version is one we can load (0x102 original or 0x103 analog).
inline bool IsValidReplayVersion(u16 version)
{
    return version == 0x102 || version == REPLAY_VERSION_ANALOG;
}

}; // namespace th06
