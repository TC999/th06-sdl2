#include "inttypes.hpp"
#include "sdl2_compat.hpp"

#include "FileSystem.hpp"
#include "MidiOutput.hpp"
#include "Supervisor.hpp"
#include "ZunMemory.hpp"
#include "i18n.hpp"
#include "utils.hpp"

#include <SDL.h>
#include <SDL_mixer.h>
#include <cstring>

namespace th06
{
MidiDevice::MidiDevice()
{
    this->handle = 0;
    this->deviceId = 0;
}

MidiDevice::~MidiDevice()
{
    this->Close();
}

ZunBool MidiDevice::OpenDevice(u32 uDeviceId)
{
    this->deviceId = uDeviceId;
    this->handle = 1;
    return false;
}

ZunResult MidiDevice::Close()
{
    this->handle = 0;
    return ZUN_SUCCESS;
}

ZunBool MidiDevice::SendLongMsg(void *pmh)
{
    return false;
}

ZunBool MidiDevice::SendShortMsg(u8 midiStatus, u8 firstByte, u8 secondByte)
{
    return false;
}

MidiTimer::MidiTimer()
{
    this->timerMin = 1;
    this->timerMax = 1000;
    this->timerId = 0;
}

MidiTimer::~MidiTimer()
{
    this->StopTimer();
}

u32 MidiTimer::StartTimer(u32 delay, void *cb, u32 data)
{
    this->StopTimer();

    this->timerId = SDL_AddTimer(delay, (SDL_TimerCallback)MidiTimer::DefaultTimerCallback, (void *)this);

    return this->timerId;
}

i32 MidiTimer::StopTimer()
{
    if (this->timerId != 0)
    {
        SDL_RemoveTimer(this->timerId);
    }

    this->timerId = 0;

    return 1;
}

u32 MidiTimer::DefaultTimerCallback(u32 interval, void *param)
{
    MidiTimer *timer = (MidiTimer *)param;
    timer->OnTimerElapsed();
    return interval;
}

u16 MidiOutput::Ntohs(u16 val)
{
    u8 tmp[2];

    tmp[0] = ((u8 *)&val)[1];
    tmp[1] = ((u8 *)&val)[0];

    return *(const u16 *)(&tmp);
}

u32 MidiOutput::SkipVariableLength(u8 **curTrackDataCursor)
{
    u32 length;
    u8 tmp;

    length = 0;
    do
    {
        tmp = **curTrackDataCursor;
        *curTrackDataCursor = *curTrackDataCursor + 1;
        length = length * 0x80 + (tmp & 0x7f);
    } while ((tmp & 0x80) != 0);

    return length;
}

MidiOutput::MidiOutput()
{
    this->tracks = NULL;
    this->divisions = 0;
    this->tempo = 0;
    this->numTracks = 0;
    this->unk2c4 = 0;
    this->fadeOutVolumeMultiplier = 0;
    this->fadeOutLastSetVolume = 0;
    this->unk2d0 = 0;
    this->unk2d4 = 0;
    this->unk2d8 = 0;
    this->unk2dc = 0;
    this->fadeOutFlag = 0;

    for (int i = 0; i < ARRAY_SIZE_SIGNED(this->midiFileData); i++)
    {
        this->midiFileData[i] = NULL;
    }

    for (int i = 0; i < ARRAY_SIZE_SIGNED(this->midiHeaders); i++)
    {
        this->midiHeaders[i] = NULL;
    }

    this->midiHeadersCursor = 0;
}

MidiOutput::~MidiOutput()
{
    this->StopPlayback();
    if (this->midiHeaders[0] != NULL)
    {
        Mix_FreeMusic((Mix_Music *)this->midiHeaders[0]);
        this->midiHeaders[0] = NULL;
    }
    this->ClearTracks();
    for (i32 i = 0; i < 32; i++)
    {
        this->ReleaseFileData(i);
    }
}

ZunResult MidiOutput::ReadFileData(u32 idx, char *path)
{
    if (g_Supervisor.cfg.musicMode != MIDI)
    {
        return ZUN_SUCCESS;
    }

    this->StopPlayback();
    this->ReleaseFileData(idx);

    this->midiFileData[idx] = FileSystem::OpenPath(path, false);

    if (this->midiFileData[idx] == (byte *)0x0)
    {
        g_GameErrorContext.Log(&g_GameErrorContext, TH_ERR_MIDI_FAILED_TO_READ_FILE, path);
        return ZUN_ERROR;
    }

    return ZUN_SUCCESS;
}

void MidiOutput::ReleaseFileData(u32 idx)
{
    u8 *data = this->midiFileData[idx];
    free(data);

    this->midiFileData[idx] = NULL;
}

#pragma var_order(trackIndex, data, tracks)
void MidiOutput::ClearTracks()
{
    i32 trackIndex;
    u8 *data;
    MidiTrack *tracks;

    for (trackIndex = 0; trackIndex < this->numTracks; trackIndex++)
    {
        data = this->tracks[trackIndex].trackData;
        free(data);
    }

    tracks = this->tracks;
    free(tracks);
    this->tracks = NULL;
    this->numTracks = 0;
}

#pragma var_order(trackIdx, currentCursor, currentCursorTrack, fileData, hdrLength, hdrRaw, trackLength,               \
                  endOfHeaderPointer)
ZunResult MidiOutput::ParseFile(i32 fileIdx)
{
    u8 hdrRaw[8];
    u32 trackLength;
    u8 *currentCursor, *currentCursorTrack, *endOfHeaderPointer;
    i32 trackIdx;
    u8 *fileData;
    u32 hdrLength;

    this->ClearTracks();
    currentCursor = this->midiFileData[fileIdx];
    fileData = currentCursor;
    if (currentCursor == NULL)
    {
        utils::DebugPrint2(TH_JP_ERR_MIDI_NOT_LOADED);
        return ZUN_ERROR;
    }

    // Read midi header chunk
    // First, read the header len
    memcpy(&hdrRaw, currentCursor, 8);

    // Get a pointer to the end of the header chunk
    currentCursor += sizeof(hdrRaw);
    hdrLength = MidiOutput::Ntohl(*(u32 *)(hdrRaw + 4));

    endOfHeaderPointer = currentCursor;
    currentCursor += hdrLength;

    // Read the format. Only three values of format are specified:
    //  0: the file contains a single multi-channel track
    //  1: the file contains one or more simultaneous tracks (or MIDI outputs) of a
    //  sequence
    //  2: the file contains one or more sequentially independent single-track
    //  patterns
    this->format = MidiOutput::Ntohs(*(u16 *)endOfHeaderPointer);

    // Read the divisions in this track. Note that this doesn't appear to support
    // "negative SMPTE format", which happens when the MSB is set.
    this->divisions = MidiOutput::Ntohs(*(u16 *)(endOfHeaderPointer + 4));
    // Read the number of tracks in this midi file.
    this->numTracks = MidiOutput::Ntohs(*(u16 *)(endOfHeaderPointer + 2));

    // Allocate this->divisions * 32 bytes.
    this->tracks = (MidiTrack *)ZunMemory::Alloc(sizeof(MidiTrack) * this->numTracks);
    memset(this->tracks, 0, sizeof(MidiTrack) * this->numTracks);
    for (trackIdx = 0; trackIdx < this->numTracks; trackIdx += 1)
    {
        currentCursorTrack = currentCursor;
        currentCursor += 8;

        // Read a track (MTrk) chunk.
        //
        // First, read the length of the chunk
        trackLength = MidiOutput::Ntohl(*(u32 *)(currentCursorTrack + 4));
        this->tracks[trackIdx].trackLength = trackLength;
        this->tracks[trackIdx].trackData = (u8 *)ZunMemory::Alloc(trackLength);
        this->tracks[trackIdx].trackPlaying = 1;
        memcpy(this->tracks[trackIdx].trackData, currentCursor, trackLength);
        currentCursor += trackLength;
    }
    this->tempo = 1000000;
    return ZUN_SUCCESS;
}

ZunResult MidiOutput::LoadFile(char *midiPath)
{
    if (this->ReadFileData(0x1f, midiPath) != ZUN_SUCCESS)
    {
        return ZUN_ERROR;
    }

    u32 fileSize = g_LastFileSize;
    this->ParseFile(0x1f);

    if (this->midiFileData[0x1f] != NULL && fileSize > 0)
    {
        SDL_RWops *rw = SDL_RWFromMem(this->midiFileData[0x1f], fileSize);
        if (rw != NULL)
        {
            Mix_Music *mus = Mix_LoadMUS_RW(rw, 1);
            if (mus != NULL)
            {
                if (this->midiHeaders[0] != NULL)
                {
                    Mix_FreeMusic((Mix_Music *)this->midiHeaders[0]);
                }
                this->midiHeaders[0] = mus;
            }
        }
    }

    this->ReleaseFileData(0x1f);

    return ZUN_SUCCESS;
}

void MidiOutput::LoadTracks()
{
    i32 trackIndex;
    MidiTrack *track = this->tracks;

    this->fadeOutVolumeMultiplier = 1.0;
    this->unk2dc = 0;
    this->fadeOutFlag = 0;
    this->volume = 0;
    this->unk130 = 0;

    for (trackIndex = 0; trackIndex < this->numTracks; trackIndex++, track++)
    {
        track->curTrackDataCursor = track->trackData;
        track->startTrackDataMaybe = track->curTrackDataCursor;
        track->trackPlaying = 1;
        track->trackLengthOther = MidiOutput::SkipVariableLength(&track->curTrackDataCursor);
    }
}

ZunResult MidiOutput::Play()
{
    if (this->tracks == NULL)
    {
        return ZUN_ERROR;
    }

    this->LoadTracks();

    if (this->midiHeaders[0] != NULL)
    {
        Mix_VolumeMusic(MIX_MAX_VOLUME * 45 / 100);
        Mix_PlayMusic((Mix_Music *)this->midiHeaders[0], -1);
    }

    return ZUN_SUCCESS;
}

ZunResult MidiOutput::StopPlayback()
{
    if (this->tracks == NULL)
    {
        return ZUN_ERROR;
    }

    Mix_HaltMusic();
    this->StopTimer();

    return ZUN_SUCCESS;
}

ZunResult MidiOutput::UnprepareHeader(void *pmh)
{
    return ZUN_SUCCESS;
}

u32 MidiOutput::SetFadeOut(u32 ms)
{
    this->fadeOutVolumeMultiplier = 0.0;
    this->fadeOutInterval = ms;
    this->fadeOutElapsedMS = 0;
    this->unk2dc = 0;
    this->fadeOutFlag = 1;

    return 0;
}

#pragma var_order(trackIndex, local_14, trackLoaded)
void MidiOutput::OnTimerElapsed()
{
    if (this->fadeOutFlag != 0)
    {
        if (this->fadeOutElapsedMS < this->fadeOutInterval)
        {
            this->fadeOutVolumeMultiplier = 1.0f - (f32)this->fadeOutElapsedMS / (f32)this->fadeOutInterval;
            i32 vol = (i32)(this->fadeOutVolumeMultiplier * MIX_MAX_VOLUME);
            if (vol < 0)
                vol = 0;
            if (vol > MIX_MAX_VOLUME)
                vol = MIX_MAX_VOLUME;
            Mix_VolumeMusic(vol);
            this->fadeOutLastSetVolume = vol;
            this->fadeOutElapsedMS = this->fadeOutElapsedMS + 1;
        }
        else
        {
            this->fadeOutVolumeMultiplier = 0.0;
            Mix_HaltMusic();
            return;
        }
    }
}

void MidiOutput::ProcessMsg(MidiTrack *track)
{
    u8 opcode;
    u8 opcodeHigh;
    i32 curTrackLength;
    i32 nextTrackLength;
    u8 cVar1;

    opcode = *track->curTrackDataCursor;
    if (opcode < MIDI_OPCODE_NOTE_OFF)
    {
        opcode = track->opcode;
    }
    else
    {
        track->curTrackDataCursor += 1;
    }
    opcodeHigh = opcode & 0xf0;
    switch (opcodeHigh)
    {
    case MIDI_OPCODE_SYSTEM_EXCLUSIVE:
        if (opcode == MIDI_OPCODE_SYSTEM_EXCLUSIVE)
        {
            curTrackLength = MidiOutput::SkipVariableLength(&track->curTrackDataCursor);
            track->curTrackDataCursor += curTrackLength;
        }
        else if (opcode == MIDI_OPCODE_SYSTEM_RESET)
        {
            cVar1 = *track->curTrackDataCursor;
            track->curTrackDataCursor += 1;
            curTrackLength = MidiOutput::SkipVariableLength(&track->curTrackDataCursor);
            if (cVar1 == 0x2f)
            {
                track->trackPlaying = 0;
                return;
            }
            if (cVar1 == 0x51)
            {
                this->unk130 += (this->volume * this->divisions * 1000 / this->tempo);
                this->volume = 0;
                this->tempo = 0;
                for (i32 idx = 0; idx < curTrackLength; idx += 1)
                {
                    this->tempo = this->tempo * 0x100 + *track->curTrackDataCursor;
                    track->curTrackDataCursor += 1;
                }
                break;
            }
            track->curTrackDataCursor = track->curTrackDataCursor + curTrackLength;
        }
        break;
    case MIDI_OPCODE_NOTE_OFF:
    case MIDI_OPCODE_NOTE_ON:
    case MIDI_OPCODE_POLYPHONIC_AFTERTOUCH:
    case MIDI_OPCODE_MODE_CHANGE:
    case MIDI_OPCODE_PITCH_BEND_CHANGE:
        track->curTrackDataCursor += 2;
        break;
    case MIDI_OPCODE_PROGRAM_CHANGE:
    case MIDI_OPCODE_CHANNEL_AFTERTOUCH:
        track->curTrackDataCursor += 1;
        break;
    }
    track->opcode = opcode;
    nextTrackLength = MidiOutput::SkipVariableLength(&track->curTrackDataCursor);
    track->trackLengthOther = track->trackLengthOther + nextTrackLength;
    return;
}

#pragma var_order(arg1, idx, volumeByte, midiStatus, volumeClamped)
void MidiOutput::FadeOutSetVolume(i32 volume)
{
    if (this->unk2d4 != 0)
    {
        return;
    }
    i32 vol = (i32)(this->fadeOutVolumeMultiplier * MIX_MAX_VOLUME) + volume;
    if (vol < 0)
        vol = 0;
    if (vol > MIX_MAX_VOLUME)
        vol = MIX_MAX_VOLUME;
    Mix_VolumeMusic(vol);
    return;
}

}; // namespace th06
