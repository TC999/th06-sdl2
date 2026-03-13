#include "SoundPlayer.hpp"

#include "FileSystem.hpp"
#include "Supervisor.hpp"
#include "i18n.hpp"
#include "utils.hpp"

#include <SDL.h>
#include <SDL_mixer.h>
#include <cstring>

namespace th06
{

#define BACKGROUND_MUSIC_BUFFER_SIZE 0x8000
#define BACKGROUND_MUSIC_WAV_NUM_CHANNELS 2
#define BACKGROUND_MUSIC_WAV_BITS_PER_SAMPLE 16
#define BACKGROUND_MUSIC_WAV_BLOCK_ALIGN BACKGROUND_MUSIC_WAV_BITS_PER_SAMPLE / 8 * BACKGROUND_MUSIC_WAV_NUM_CHANNELS

DIFFABLE_STATIC_ARRAY_ASSIGN(SoundBufferIdxVolume, 32, g_SoundBufferIdxVol) = {
    {0, -1500, 0},   {0, -2000, 0},   {1, -1200, 5},   {1, -1400, 5},  {2, -1000, 100}, {3, -500, 100},
    {4, -500, 100},  {5, -1700, 50},  {6, -1700, 50},  {7, -1700, 50}, {8, -1000, 100}, {9, -1000, 100},
    {10, -1900, 10}, {11, -1200, 10}, {12, -900, 100}, {5, -1500, 50}, {13, -900, 50},  {14, -900, 50},
    {15, -600, 100}, {16, -400, 100}, {17, -1100, 0},  {18, -900, 0},  {5, -1800, 20},  {6, -1800, 20},
    {7, -1800, 20},  {19, -300, 50},  {20, -600, 50},  {21, -800, 50}, {22, -100, 140}, {23, -500, 100},
    {24, -1000, 20}, {25, -1000, 90},
};
DIFFABLE_STATIC_ARRAY_ASSIGN(char *, 26, g_SFXList) = {
    "data/wav/plst00.wav", "data/wav/enep00.wav",   "data/wav/pldead00.wav", "data/wav/power0.wav",
    "data/wav/power1.wav", "data/wav/tan00.wav",    "data/wav/tan01.wav",    "data/wav/tan02.wav",
    "data/wav/ok00.wav",   "data/wav/cancel00.wav", "data/wav/select00.wav", "data/wav/gun00.wav",
    "data/wav/cat00.wav",  "data/wav/lazer00.wav",  "data/wav/lazer01.wav",  "data/wav/enep01.wav",
    "data/wav/nep00.wav",  "data/wav/damage00.wav", "data/wav/item00.wav",   "data/wav/kira00.wav",
    "data/wav/kira01.wav", "data/wav/kira02.wav",   "data/wav/extend.wav",   "data/wav/timeout.wav",
    "data/wav/graze.wav",  "data/wav/powerup.wav",
};
DIFFABLE_STATIC(SoundPlayer, g_SoundPlayer)

SoundPlayer::SoundPlayer()
{
    memset(this, 0, sizeof(SoundPlayer));
    for (i32 i = 0; i < ARRAY_SIZE_SIGNED(this->unk408); i++)
    {
        this->unk408[i] = -1;
    }
}

#pragma var_order(bufDesc, audioBuffer2Start, audioBuffer2Len, audioBuffer1Len, audioBuffer1Start, wavFormat)
ZunResult SoundPlayer::InitializeDSound(HWND gameWindow)
{
    this->manager = new CSoundManager();
    if (this->manager->Initialize(gameWindow, 2, 2, 44100, 16) < ZUN_SUCCESS)
    {
        GameErrorContext::Log(&g_GameErrorContext, TH_ERR_SOUNDPLAYER_FAILED_TO_INITIALIZE_OBJECT);
        if (this->manager != NULL)
        {
            delete this->manager;
            this->manager = NULL;
        }
        return ZUN_ERROR;
    }

    this->dsoundHdl = 1;
    this->backgroundMusicThreadHandle = NULL;
    this->initSoundBuffer = 1;
    this->gameWindow = gameWindow;
    GameErrorContext::Log(&g_GameErrorContext, TH_DBG_SOUNDPLAYER_INIT_SUCCESS);
    return ZUN_SUCCESS;
}

ZunResult SoundPlayer::Release(void)
{
    i32 i;

    if (this->manager == NULL)
    {
        return ZUN_SUCCESS;
    }
    for (i = 0; i < 0x80; i++)
    {
        this->soundChannels[i] = -1;
        if (this->soundBuffers[i] != NULL)
        {
            Mix_FreeChunk(this->soundBuffers[i]);
            this->soundBuffers[i] = NULL;
        }
    }
    StopBGM();
    this->dsoundHdl = 0;
    this->initSoundBuffer = 0;
    if (this->backgroundMusic != NULL)
    {
        delete this->backgroundMusic;
        this->backgroundMusic = NULL;
    }
    if (this->manager != NULL)
    {
        delete this->manager;
        this->manager = NULL;
    }
    return ZUN_SUCCESS;
}

void SoundPlayer::StopBGM()
{
    if (this->backgroundMusic != NULL)
    {
        this->backgroundMusic->Stop();
        utils::DebugPrint2("stop BGM\n");
        if (this->backgroundMusic != NULL)
        {
            delete this->backgroundMusic;
            this->backgroundMusic = NULL;
        }
    }
    return;
}

#pragma var_order(notifySize, waveFile, res, numSamplesPerSec, blockAlign, curTime, startTime, waitTime, curTime2,     \
                  startTime2, waitTime2)
ZunResult SoundPlayer::LoadWav(char *path)
{
    HRESULT res;
    CWaveFile waveFile;
    u32 startTime;
    u32 curTime;
    u32 waitTime;
    u32 blockAlign;
    u32 numSamplesPerSec;
    u32 notifySize;
    u32 startTime2;
    u32 curTime2;
    u32 waitTime2;

    if (this->manager == NULL)
    {
        return ZUN_ERROR;
    }
    if (g_Supervisor.cfg.playSounds == 0)
    {
        return ZUN_ERROR;
    }
    if (this->dsoundHdl == 0)
    {
        return ZUN_ERROR;
    }
    this->StopBGM();
    utils::DebugPrint2("load BGM\n");
    res = waveFile.Open(path, NULL, WAVEFILE_READ);
    if (res < 0)
    {
        utils::DebugPrint2("error : wav file load error %s\n", path);
        waveFile.Close();
        return ZUN_ERROR;
    }
    if (waveFile.GetSize() == 0)
    {
        waveFile.Close();
        return ZUN_ERROR;
    }
    startTime = SDL_GetTicks();
    curTime = startTime;
    waitTime = 100;
    while (curTime < startTime + waitTime && curTime >= startTime)
    {
        curTime = SDL_GetTicks();
    }
    waveFile.Close();
    res = this->manager->CreateStreaming(&this->backgroundMusic, path, 0, GUID(), 4, 0, NULL);
    if (res < 0)
    {
        utils::DebugPrint2(TH_ERR_SOUNDPLAYER_FAILED_TO_CREATE_BGM_SOUND_BUFFER);
        return ZUN_ERROR;
    }
    utils::DebugPrint2("comp\n");
    startTime2 = SDL_GetTicks();
    curTime2 = startTime2;
    waitTime2 = 100;
    while (curTime2 < startTime2 + waitTime2 && curTime2 >= startTime2)
    {
        curTime2 = SDL_GetTicks();
    }
    return ZUN_SUCCESS;
}

#pragma var_order(fileData, bgmFile, loopEnd, loopStart)
ZunResult SoundPlayer::LoadPos(char *path)
{
    u8 *fileData;
    CWaveFile *bgmFile;
    i32 loopEnd;
    i32 loopStart;

    if (this->manager == NULL)
    {
        return ZUN_ERROR;
    }
    if (g_Supervisor.cfg.playSounds == 0)
    {
        return ZUN_ERROR;
    }
    if (this->backgroundMusic == NULL)
    {
        return ZUN_ERROR;
    }

    fileData = FileSystem::OpenPath(path, 0);
    if (fileData == NULL)
    {
        return ZUN_ERROR;
    }
    bgmFile = this->backgroundMusic->m_pWaveFile;
    loopEnd = *(i32 *)(fileData + 4) * 4;
    loopStart = *(i32 *)(fileData) * 4;
    bgmFile->m_loopStartPoint = loopStart;
    bgmFile->m_loopEndPoint = loopEnd;
    free(fileData);
    return ZUN_SUCCESS;
}

ZunResult SoundPlayer::InitSoundBuffers()
{
    i32 idx;
    if (this->manager == NULL)
    {
        return ZUN_ERROR;
    }
    else if (this->dsoundHdl == 0)
    {
        return ZUN_SUCCESS;
    }
    else
    {
        for (idx = 0; idx < 3; idx++)
        {
            this->soundBuffersToPlay[idx] = -1;
        }
        for (idx = 0; idx < ARRAY_SIZE_SIGNED(g_SFXList); idx++)
        {
            if (this->LoadSound(idx, g_SFXList[idx]) != ZUN_SUCCESS)
            {
                GameErrorContext::Log(&g_GameErrorContext, TH_ERR_SOUNDPLAYER_FAILED_TO_LOAD_SOUND_FILE,
                                      g_SFXList[idx]);
                return ZUN_ERROR;
            }
        }
        for (idx = 0; idx < ARRAY_SIZE(g_SoundBufferIdxVol); idx++)
        {
            i32 bufIdx = g_SoundBufferIdxVol[idx].bufferIdx;
            this->soundChannels[idx] = idx;
            if (this->soundBuffers[bufIdx] != NULL)
            {
                i32 dsVol = g_SoundBufferIdxVol[idx].volume;
                i32 sdlVol = (i32)((1.0f - ((f32)(-dsVol)) / 3000.0f) * MIX_MAX_VOLUME);
                if (sdlVol < 0)
                    sdlVol = 0;
                if (sdlVol > MIX_MAX_VOLUME)
                    sdlVol = MIX_MAX_VOLUME;
                Mix_VolumeChunk(this->soundBuffers[bufIdx], sdlVol);
            }
        }
    }
    return ZUN_SUCCESS;
}

WAVEFORMATEX *SoundPlayer::GetWavFormatData(u8 *soundData, char *formatString, i32 *formatSize,
                                            u32 fileSizeExcludingFormat)
{
    while (fileSizeExcludingFormat > 0)
    {
        *formatSize = *(i32 *)(soundData + 4);
        if (strncmp((char *)soundData, formatString, 4) == 0)
        {
            return (WAVEFORMATEX *)(soundData + 8);
        }
        fileSizeExcludingFormat -= (*formatSize + 8);
        soundData += *formatSize + 8;
    }
    return NULL;
}

#pragma var_order(sFDCursor, dsBuffer, wavDataPtr, formatSize, audioPtr2, audioSize2, audioSize1, audioPtr1,           \
                  soundFileData, wavData, fileSize)
ZunResult SoundPlayer::LoadSound(i32 idx, char *path)
{
    u8 *soundFileData;
    i32 fileSize;

    if (this->manager == NULL)
    {
        return ZUN_SUCCESS;
    }
    if (this->soundBuffers[idx] != NULL)
    {
        Mix_FreeChunk(this->soundBuffers[idx]);
        this->soundBuffers[idx] = NULL;
    }
    soundFileData = (u8 *)FileSystem::OpenPath(path, 0);
    if (soundFileData == NULL)
    {
        return ZUN_ERROR;
    }
    if (strncmp((char *)soundFileData, "RIFF", 4))
    {
        GameErrorContext::Log(&g_GameErrorContext, TH_ERR_NOT_A_WAV_FILE, path);
        free(soundFileData);
        return ZUN_ERROR;
    }
    fileSize = *(i32 *)(soundFileData + 4) + 8;
    SDL_RWops *rw = SDL_RWFromMem(soundFileData, fileSize);
    if (rw == NULL)
    {
        free(soundFileData);
        return ZUN_ERROR;
    }
    this->soundBuffers[idx] = Mix_LoadWAV_RW(rw, 1);
    free(soundFileData);
    if (this->soundBuffers[idx] == NULL)
    {
        utils::DebugPrint2("Mix_LoadWAV_RW failed: %s for %s\n", Mix_GetError(), path);
        return ZUN_ERROR;
    }
    return ZUN_SUCCESS;
}

#pragma var_order(buffer, res)
ZunResult SoundPlayer::PlayBGM(i32 isLooping)
{
    HRESULT res;

    utils::DebugPrint2("play BGM\n");
    if (this->backgroundMusic == NULL)
    {
        return ZUN_ERROR;
    }
    res = this->backgroundMusic->Reset();
    if (res < 0)
    {
        return ZUN_ERROR;
    }
    res = this->backgroundMusic->Play(0, 0);
    if (res < 0)
    {
        return ZUN_ERROR;
    }
    utils::DebugPrint2("comp\n");
    this->isLooping = isLooping;
    return ZUN_SUCCESS;
}

#pragma var_order(idx, sndBufIdx)
void SoundPlayer::PlaySounds()
{
    i32 idx;
    i32 sndBufIdx;

    if (this->manager == NULL)
    {
        return;
    }
    if (!g_Supervisor.cfg.playSounds)
    {
        return;
    }
    for (idx = 0; idx < ARRAY_SIZE_SIGNED(this->soundBuffersToPlay); idx++)
    {
        if (this->soundBuffersToPlay[idx] < 0)
        {
            break;
        }
        sndBufIdx = this->soundBuffersToPlay[idx];
        this->soundBuffersToPlay[idx] = -1;
        i32 bufIdx = g_SoundBufferIdxVol[sndBufIdx].bufferIdx;
        if (this->soundBuffers[bufIdx] == NULL)
        {
            continue;
        }
        i32 dsVol = g_SoundBufferIdxVol[sndBufIdx].volume;
        i32 sdlVol = (i32)((1.0f - ((f32)(-dsVol)) / 3000.0f) * MIX_MAX_VOLUME);
        if (sdlVol < 0)
            sdlVol = 0;
        if (sdlVol > MIX_MAX_VOLUME)
            sdlVol = MIX_MAX_VOLUME;
        Mix_VolumeChunk(this->soundBuffers[bufIdx], sdlVol);
        Mix_PlayChannel(-1, this->soundBuffers[bufIdx], 0);
    }
    return;
}

#pragma var_order(i, SFXToPlay)
void SoundPlayer::PlaySoundByIdx(SoundIdx idx, i32 unused)
{
    i32 SFXToPlay;
    i32 i;

    SFXToPlay = g_SoundBufferIdxVol[idx].unk;
    for (i = 0; i < 3; i++)
    {
        if (this->soundBuffersToPlay[i] < 0)
        {
            break;
        }
        if (this->soundBuffersToPlay[i] == idx)
        {
            return;
        }
    }
    if (i >= 3)
    {
        return;
    }
    this->soundBuffersToPlay[i] = idx;
    this->unk408[idx] = SFXToPlay;
    return;
}

void SoundPlayer::BackgroundMusicPlayerThread(void *lpThreadParameter)
{
    (void)lpThreadParameter;
}
}; // namespace th06
