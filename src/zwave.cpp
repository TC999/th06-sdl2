//-----------------------------------------------------------------------------
// File: zwave.cpp - SDL2_mixer based replacement for DSUtil
//-----------------------------------------------------------------------------
#include "zwave.hpp"
#include "AssetIO.hpp"
#include "utils.hpp"
#include <SDL.h>
#include <SDL_mixer.h>
#include <cstddef>
#include <cstring>

namespace th06
{

namespace
{
void EnsureOggBackendReady()
{
    static bool initialized = false;
    if (initialized)
    {
        return;
    }
    initialized = true;
    int requested = MIX_INIT_OGG;
    int got = Mix_Init(requested);
    if ((got & MIX_INIT_OGG) != MIX_INIT_OGG)
    {
        utils::DebugPrint2("Mix_Init(MIX_INIT_OGG) failed: %s\n", Mix_GetError());
    }
}
} // namespace

CSoundManager::CSoundManager()
{
    m_initialized = 0;
}

CSoundManager::~CSoundManager()
{
    if (m_initialized)
    {
        Mix_HookMusic(NULL, NULL);
        Mix_CloseAudio();
        m_initialized = 0;
    }
}

HRESULT CSoundManager::Initialize(HWND hWnd, DWORD dwCoopLevel, DWORD dwPrimaryChannels, DWORD dwPrimaryFreq,
                                  DWORD dwPrimaryBitRate)
{
    if (m_initialized)
    {
        Mix_CloseAudio();
        m_initialized = 0;
    }

    if (Mix_OpenAudio(dwPrimaryFreq, AUDIO_S16LSB, dwPrimaryChannels, 2048) < 0)
    {
        utils::DebugPrint2("Mix_OpenAudio failed: %s\n", Mix_GetError());
        return -1;
    }

    Mix_AllocateChannels(64);
    m_initialized = 1;
    return 0;
}

HRESULT CSoundManager::CreateStreaming(CStreamingSound **ppStreamingSound, char *strWaveFileName,
                                       DWORD dwCreationFlags, GUID guid3DAlgorithm, DWORD dwNotifyCount,
                                       DWORD dwNotifySize, HANDLE hNotifyEvent)
{
    if (!m_initialized)
        return -1;

    CWaveFile *pWaveFile = new CWaveFile();
    if (pWaveFile->Open(strWaveFileName, NULL, WAVEFILE_READ) != 0)
    {
        delete pWaveFile;
        return -1;
    }

    CStreamingSound *pStream = new CStreamingSound();
    pStream->m_pWaveFile = pWaveFile;
    pStream->m_pcmData = pWaveFile->m_pcmData;
    pStream->m_pcmDataSize = pWaveFile->m_pcmDataSize;
    pStream->m_playPosition = 0;
    pStream->m_isPlaying = 0;

    *ppStreamingSound = pStream;
    return 0;
}

CStreamingSound::CStreamingSound()
{
    m_pWaveFile = NULL;
    m_pcmData = NULL;
    m_pcmDataSize = 0;
    m_playPosition = 0;
    m_isPlaying = 0;
    m_dwCurFadeoutProgress = 0;
    m_dwTotalFadeout = 0;
    m_dwIsFadingOut = 0;
}

CStreamingSound::~CStreamingSound()
{
    Stop();
    if (m_pWaveFile != NULL)
    {
        delete m_pWaveFile;
        m_pWaveFile = NULL;
    }
}

void CStreamingSound::MusicHookCallback(void *udata, u8 *stream, int len)
{
    CStreamingSound *self = (CStreamingSound *)udata;
    if (self == NULL || self->m_pcmData == NULL || !self->m_isPlaying)
    {
        memset(stream, 0, len);
        return;
    }

    i32 loopEnd = 0;
    i32 loopStart = 0;
    if (self->m_pWaveFile != NULL)
    {
        loopEnd = self->m_pWaveFile->m_loopEndPoint;
        loopStart = self->m_pWaveFile->m_loopStartPoint;
    }

    u32 effectiveEnd = (loopEnd > 0 && (u32)loopEnd <= self->m_pcmDataSize) ? (u32)loopEnd : self->m_pcmDataSize;

    int bytesWritten = 0;
    while (bytesWritten < len)
    {
        u32 available = effectiveEnd - self->m_playPosition;
        int toWrite = len - bytesWritten;
        if ((u32)toWrite > available)
            toWrite = available;

        memcpy(stream + bytesWritten, self->m_pcmData + self->m_playPosition, toWrite);
        self->m_playPosition += toWrite;
        bytesWritten += toWrite;

        if (self->m_playPosition >= effectiveEnd)
        {
            if (loopStart >= 0 && loopEnd > 0)
            {
                self->m_playPosition = loopStart;
            }
            else
            {
                memset(stream + bytesWritten, 0, len - bytesWritten);
                self->m_isPlaying = 0;
                break;
            }
        }
    }
}

HRESULT CStreamingSound::Play(DWORD dwPriority, DWORD dwFlags)
{
    m_dwIsFadingOut = 0;
    m_dwCurFadeoutProgress = 0;
    m_dwTotalFadeout = 0;
    m_isPlaying = 1;
    // Set BGM volume to 45% (75% base * 60% reduction)
    Mix_VolumeMusic(MIX_MAX_VOLUME * 45 / 100);
    Mix_HookMusic(MusicHookCallback, this);
    return 0;
}

HRESULT CStreamingSound::Stop()
{
    m_isPlaying = 0;
    Mix_HookMusic(NULL, NULL);
    m_dwIsFadingOut = 0;
    return 0;
}

HRESULT CStreamingSound::Reset()
{
    m_playPosition = 0;
    return 0;
}

HRESULT CStreamingSound::UpdateFadeOut()
{
    if (m_dwIsFadingOut != 0)
    {
        m_dwCurFadeoutProgress = m_dwCurFadeoutProgress - 1;
        if (m_dwCurFadeoutProgress <= 0)
        {
            m_dwIsFadingOut = 0;
            Stop();
            return 1;
        }
        int vol = (m_dwCurFadeoutProgress * MIX_MAX_VOLUME) / m_dwTotalFadeout;
        Mix_VolumeMusic(vol);
    }
    return 0;
}

HRESULT CStreamingSound::HandleWaveStreamNotification(BOOL bLoopedPlay)
{
    return 0;
}

HRESULT CStreamingSound::FillBufferWithSound(void *unused, BOOL bRepeatWavIfBufferLarger)
{
    return 0;
}

void *CStreamingSound::GetBuffer(DWORD dwIndex)
{
    return NULL;
}

static u8 *FindChunk(u8 *data, u32 dataSize, const char *id, u32 *outSize)
{
    u8 *end = data + dataSize;
    while (data + 8 <= end)
    {
        if (memcmp(data, id, 4) == 0)
        {
            *outSize = utils::ReadUnaligned<u32>(data + 4);
            return data + 8;
        }
        u32 chunkSize = utils::ReadUnaligned<u32>(data + 4);
        data += 8 + chunkSize;
    }
    return NULL;
}

CWaveFile::CWaveFile()
{
    m_pwfx = NULL;
    m_dwSize = 0;
    m_loopEndPoint = 0;
    m_loopStartPoint = 0;
    m_fileData = NULL;
    m_pcmData = NULL;
    m_pcmDataSize = 0;
    memset(&m_wfxStorage, 0, sizeof(m_wfxStorage));
}

CWaveFile::~CWaveFile()
{
    Close();
}

HRESULT CWaveFile::Open(char *strFileName, WAVEFORMATEX *pwfx, DWORD dwFlags)
{
    if (strFileName == NULL)
        return -1;

    Close();

    // Try a sibling .ogg companion first; OGG Vorbis is decoded by SDL_mixer
    // (Mix_LoadWAV_RW) into the device's native PCM format (16-bit stereo at
    // 44100 Hz here), which matches our WAV pipeline byte-for-byte. Loop
    // points stored in .pos files (sample-pair offsets * 4) therefore stay
    // valid against the decoded buffer. If no .ogg sibling exists or it
    // fails to decode, we fall back to the original WAV path below.
    {
        char oggPath[512];
        size_t nameLen = std::strlen(strFileName);
        if (nameLen > 0 && nameLen + 1 <= sizeof(oggPath))
        {
            std::memcpy(oggPath, strFileName, nameLen + 1);
            char *ext = std::strrchr(oggPath, '.');
            if (ext != NULL && (size_t)(ext - oggPath) + 4 < sizeof(oggPath))
            {
                ext[1] = 'o';
                ext[2] = 'g';
                ext[3] = 'g';
                ext[4] = '\0';

                SDL_RWops *probe = AssetIO::OpenRW(oggPath);
                if (probe != NULL)
                {
                    EnsureOggBackendReady();
                    Mix_Chunk *chunk = Mix_LoadWAV_RW(probe, 1);
                    if (chunk != NULL && chunk->abuf != NULL && chunk->alen > 0)
                    {
                        // Sanity-check the device format actually matches what
                        // the .pos loop points assume (44100/16/2 → 4 B/frame).
                        // If not, the loop math will desync — log loud and continue.
                        int qFreq = 0; Uint16 qFmt = 0; int qCh = 0;
                        int querySpec = Mix_QuerySpec(&qFreq, &qFmt, &qCh);
                        utils::DebugPrint2(
                            "[OGG] decoded %s: alen=%u, dev=%dHz/%dch/%dbit (querySpec=%d)\n",
                            oggPath, (unsigned)chunk->alen, qFreq, qCh,
                            (int)SDL_AUDIO_BITSIZE(qFmt), querySpec);

                        m_fileData = new u8[chunk->alen];
                        std::memcpy(m_fileData, chunk->abuf, chunk->alen);
                        m_pcmData = m_fileData;
                        m_pcmDataSize = chunk->alen;
                        m_dwSize = chunk->alen;

                        // Synthesize a WAVEFORMATEX matching the device's
                        // open audio format so callers that inspect m_pwfx
                        // see consistent metadata.
                        if (querySpec == 0)
                        {
                            qFreq = 44100;
                            qFmt = AUDIO_S16LSB;
                            qCh = 2;
                        }
                        std::memset(&m_wfxStorage, 0, sizeof(m_wfxStorage));
                        m_wfxStorage.wFormatTag = 1; // WAVE_FORMAT_PCM
                        m_wfxStorage.nChannels = (u16)qCh;
                        m_wfxStorage.nSamplesPerSec = (u32)qFreq;
                        m_wfxStorage.wBitsPerSample = (u16)SDL_AUDIO_BITSIZE(qFmt);
                        m_wfxStorage.nBlockAlign =
                            (u16)(m_wfxStorage.nChannels * (m_wfxStorage.wBitsPerSample / 8));
                        m_wfxStorage.nAvgBytesPerSec =
                            m_wfxStorage.nSamplesPerSec * m_wfxStorage.nBlockAlign;
                        m_wfxStorage.cbSize = 0;
                        m_pwfx = &m_wfxStorage;

                        Mix_FreeChunk(chunk);
                        return 0;
                    }
                    if (chunk != NULL)
                    {
                        utils::DebugPrint2(
                            "[OGG] empty chunk for %s (alen=%u abuf=%p) — falling back to WAV\n",
                            oggPath, chunk != NULL ? (unsigned)chunk->alen : 0u,
                            chunk != NULL ? (void *)chunk->abuf : NULL);
                        Mix_FreeChunk(chunk);
                    }
                    else
                    {
                        utils::DebugPrint2("[OGG] Mix_LoadWAV_RW failed for %s: %s\n", oggPath, Mix_GetError());
                    }
                    // Fall through to WAV fallback below.
                }
                else
                {
                    utils::DebugPrint2("[OGG] no sibling .ogg for %s (probe miss)\n", oggPath);
                }
            }
        }
    }

    SDL_RWops *rw = AssetIO::OpenRW(strFileName);
    if (rw == NULL)
    {
        utils::DebugPrint2("error: cannot open %s\n", strFileName);
        return -1;
    }

    i64 fileSize = SDL_RWsize(rw);
    if (fileSize <= 0)
    {
        SDL_RWclose(rw);
        return -1;
    }

    m_fileData = new u8[fileSize];
    if (SDL_RWread(rw, m_fileData, 1, fileSize) != (size_t)fileSize)
    {
        delete[] m_fileData;
        m_fileData = NULL;
        SDL_RWclose(rw);
        return -1;
    }
    SDL_RWclose(rw);

    if (fileSize < 12 || memcmp(m_fileData, "RIFF", 4) != 0 || memcmp(m_fileData + 8, "WAVE", 4) != 0)
    {
        utils::DebugPrint2("error: not a WAV file: %s\n", strFileName);
        delete[] m_fileData;
        m_fileData = NULL;
        return -1;
    }

    u32 riffSize = utils::ReadUnaligned<u32>(m_fileData + 4);
    u8 *chunks = m_fileData + 12;
    u32 chunksSize = riffSize - 4;

    u32 fmtSize = 0;
    u8 *fmtData = FindChunk(chunks, chunksSize, "fmt ", &fmtSize);
    if (fmtData == NULL || fmtSize < 16)
    {
        delete[] m_fileData;
        m_fileData = NULL;
        return -1;
    }

    memcpy(&m_wfxStorage, fmtData, fmtSize < sizeof(WAVEFORMATEX) ? fmtSize : sizeof(WAVEFORMATEX));
    if (fmtSize < sizeof(WAVEFORMATEX))
        m_wfxStorage.cbSize = 0;
    m_pwfx = &m_wfxStorage;

    u32 dataSize = 0;
    u8 *dataChunk = FindChunk(chunks, chunksSize, "data", &dataSize);
    if (dataChunk == NULL)
    {
        delete[] m_fileData;
        m_fileData = NULL;
        return -1;
    }

    m_pcmData = dataChunk;
    m_pcmDataSize = dataSize;
    m_dwSize = dataSize;

    return 0;
}

HRESULT CWaveFile::Close()
{
    if (m_fileData != NULL)
    {
        delete[] m_fileData;
        m_fileData = NULL;
    }
    m_pcmData = NULL;
    m_pcmDataSize = 0;
    m_dwSize = 0;
    m_pwfx = NULL;
    return 0;
}

DWORD CWaveFile::GetSize()
{
    return m_dwSize;
}

HRESULT CWaveFile::ResetFile(bool loop)
{
    return 0;
}

}; // namespace th06

