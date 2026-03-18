#pragma once

#include "ZunBool.hpp"
#include "ZunResult.hpp"
#include "inttypes.hpp"
#include "sdl2_compat.hpp"

struct _Mix_Music;
typedef struct _Mix_Music Mix_Music;

namespace th06
{
struct MidiTimer
{
    MidiTimer();
    ~MidiTimer();

    virtual void OnTimerElapsed() = 0;

    i32 StopTimer();
    u32 StartTimer(u32 delay, void *cb, u32 data);

    static u32 DefaultTimerCallback(u32 interval, void *param);

    u32 timerId;
    u32 timerMin;
    u32 timerMax;
};
ZUN_ASSERT_SIZE(MidiTimer, 0x10);

enum MidiOpcode
{
    MIDI_OPCODE_CHANNEL_1 = 0x01,
    MIDI_OPCODE_CHANNEL_2 = 0x02,
    MIDI_OPCODE_CHANNEL_3 = 0x03,
    MIDI_OPCODE_CHANNEL_4 = 0x04,
    MIDI_OPCODE_CHANNEL_5 = 0x05,
    MIDI_OPCODE_CHANNEL_6 = 0x06,
    MIDI_OPCODE_CHANNEL_7 = 0x07,
    MIDI_OPCODE_CHANNEL_8 = 0x08,
    MIDI_OPCODE_CHANNEL_9 = 0x09,
    MIDI_OPCODE_CHANNEL_A = 0x0A,
    MIDI_OPCODE_CHANNEL_B = 0x0B,
    MIDI_OPCODE_CHANNEL_C = 0x0C,
    MIDI_OPCODE_CHANNEL_D = 0x0D,
    MIDI_OPCODE_CHANNEL_E = 0x0E,
    MIDI_OPCODE_CHANNEL_F = 0x0F,
    MIDI_OPCODE_NOTE_OFF = 0x80,
    MIDI_OPCODE_NOTE_ON = 0x90,
    MIDI_OPCODE_POLYPHONIC_AFTERTOUCH = 0xA0,
    MIDI_OPCODE_MODE_CHANGE = 0xB0,
    MIDI_OPCODE_PROGRAM_CHANGE = 0xC0,
    MIDI_OPCODE_CHANNEL_AFTERTOUCH = 0xD0,
    MIDI_OPCODE_PITCH_BEND_CHANGE = 0xE0,
    MIDI_OPCODE_SYSTEM_EXCLUSIVE = 0xF0,
    MIDI_OPCODE_MIDI_TIME_CODE_QTR_FRAME = 0xF1,
    MIDI_OPCODE_SONG_POSITION_POINTER = 0xF2,
    MIDI_OPCODE_SONG_SELECT = 0xF3,
    MIDI_OPCODE_RESERVED_F4 = 0xF4,
    MIDI_OPCODE_RESERVED_F5 = 0xF5,
    MIDI_OPCODE_TUNE_REQUEST = 0xF6,
    MIDI_OPCODE_END_OF_SYSEX = 0xF7,
    MIDI_OPCODE_TIMING_CLOCK = 0xF8,
    MIDI_OPCODE_RESERVED_F9 = 0xF9,
    MIDI_OPCODE_START = 0xFA,
    MIDI_OPCODE_CONTINUE = 0xFB,
    MIDI_OPCODE_STOP = 0xFC,
    MIDI_OPCODE_RESERVED_FD = 0xFD,
    MIDI_OPCODE_ACTIVE_SENSING = 0xFE,
    MIDI_OPCODE_SYSTEM_RESET = 0xFF,
};

struct MidiTrack
{
    u32 trackPlaying;
    i32 trackLengthOther;
    u32 trackLength;
    u8 opcode;
    u8 *trackData;
    u8 *curTrackDataCursor;
    u8 *startTrackDataMaybe;
    u32 unk1c;
};
ZUN_ASSERT_SIZE(MidiTrack, 0x20);

struct MidiDevice
{
    MidiDevice();
    ~MidiDevice();

    ZunResult Close();
    ZunBool OpenDevice(u32 uDeviceId);
    ZunBool SendShortMsg(u8 midiStatus, u8 firstByte, u8 secondByte);
    ZunBool SendLongMsg(void *pmh);

    u32 handle;
    u32 deviceId;
};
ZUN_ASSERT_SIZE(MidiDevice, 0x8);

struct MidiChannel
{
    u8 keyPressedFlags[16];
    u8 instrument;
    u8 instrumentBank;
    u8 pan;
    u8 effectOneDepth;
    u8 effectThreeDepth;
    u8 channelVolume;
    u8 modifiedVolume;
};

struct MidiOutput : MidiTimer
{
    MidiOutput();
    ~MidiOutput();

    void OnTimerElapsed();

    ZunResult UnprepareHeader(void *pmh);

    ZunResult StopPlayback();
    void LoadTracks();
    void ClearTracks();
    ZunResult ReadFileData(u32 idx, char *path);
    void ReleaseFileData(u32 idx);
    void ParseFile(u32 idx);
    void ProcessMsg(MidiTrack *track);

    ZunResult ParseFile(i32 idx);
    ZunResult LoadFile(char *midiPath);
    ZunResult Play();

    u32 SetFadeOut(u32 ms);
    void FadeOutSetVolume(i32 volume);

    static u16 Ntohs(u16 val);
    static u32 SkipVariableLength(u8 **curTrackDataCursor);

    static u32 Ntohl(u32 val)
    {
        const u8 *bytes = (const u8 *)&val;
        return ((u32)bytes[3] << 24) | ((u32)bytes[2] << 16) | ((u32)bytes[1] << 8) | (u32)bytes[0];
    }

    void *midiHeaders[32];
    i32 midiHeadersCursor;
    u8 *midiFileData[32];
    i32 numTracks;
    u32 format;
    i32 divisions;
    i32 tempo;
    u32 unk124;
    u64 volume;
    i64 unk130;
    MidiTrack *tracks;
    MidiDevice midiOutDev;
    u8 unk144[16];
    MidiChannel channels[16];
    i8 unk2c4;
    f32 fadeOutVolumeMultiplier;
    u32 fadeOutLastSetVolume;
    u32 unk2d0;
    u32 unk2d4;
    u32 unk2d8;
    u32 unk2dc;
    u32 fadeOutFlag;
    i32 fadeOutInterval;
    i32 fadeOutElapsedMS;
    u32 unk2ec;
    u64 unk2f0;
    u64 unk2f8;
};
ZUN_ASSERT_SIZE(MidiOutput, 0x300);
}; // namespace th06
