//-----------------------------------------------------------------------------
// File: zwave.hpp - SDL2_mixer based replacement for DSUtil
//-----------------------------------------------------------------------------
#ifndef DSUTIL_H
#define DSUTIL_H

#include "inttypes.hpp"
#include "sdl2_compat.hpp"
#include <SDL_mixer.h>
#include <atomic>
#include <memory>

namespace th06
{

class CSoundManager;
class CStreamingSound;
class CWaveFile;

// Out-of-band buffer for asynchronous BGM decode. The decoder thread owns
// the only writable reference until it publishes `state == 1`; after that
// the audio callback can read `pcm`/`pcmSize` lock-free under acquire.
// State `2` means the requester has discarded interest in the result;
// when the worker observes (or sets) state `2` the shared_ptr eventually
// drops to refcount 0 and the destructor releases the PCM buffer.
struct AsyncDecodeData
{
    std::atomic<int> state; // 0=running, 1=ready, 2=cancelled/failed
    char path[512];
    unsigned char *pcm;
    unsigned int pcmSize;
    int freq;
    int bits;
    int channels;
    AsyncDecodeData()
        : state(0), pcm(nullptr), pcmSize(0), freq(44100), bits(16), channels(2)
    {
        path[0] = '\0';
    }
    ~AsyncDecodeData()
    {
        delete[] pcm;
    }
};

#define WAVEFILE_READ 1
#define WAVEFILE_WRITE 2

#define DSUtil_StopSound(s)                                                                                            \
    {                                                                                                                  \
        if (s)                                                                                                         \
            s->Stop();                                                                                                 \
    }
#define DSUtil_PlaySound(s)                                                                                            \
    {                                                                                                                  \
        if (s)                                                                                                         \
            s->Play(0, 0);                                                                                             \
    }
#define DSUtil_PlaySoundLooping(s)                                                                                     \
    {                                                                                                                  \
        if (s)                                                                                                         \
            s->Play(0, 1);                                                                                             \
    }

class CSoundManager
{
  public:
    CSoundManager();
    ~CSoundManager();

    HRESULT Initialize(HWND hWnd, DWORD dwCoopLevel, DWORD dwPrimaryChannels, DWORD dwPrimaryFreq,
                       DWORD dwPrimaryBitRate);

    HRESULT CreateStreaming(CStreamingSound **ppStreamingSound, char *strWaveFileName, DWORD dwCreationFlags,
                            GUID guid3DAlgorithm, DWORD dwNotifyCount, DWORD dwNotifySize, HANDLE hNotifyEvent);

    i32 m_initialized;
};

class CStreamingSound
{
  public:
    CStreamingSound();
    ~CStreamingSound();

    HRESULT Play(DWORD dwPriority, DWORD dwFlags);
    HRESULT Stop();
    HRESULT Reset();

    HRESULT UpdateFadeOut();
    HRESULT HandleWaveStreamNotification(BOOL bLoopedPlay);

    HRESULT FillBufferWithSound(void *unused, BOOL bRepeatWavIfBufferLarger);
    void *GetBuffer(DWORD dwIndex);

    CWaveFile *m_pWaveFile;

    i32 m_dwCurFadeoutProgress;
    i32 m_dwTotalFadeout;
    DWORD m_dwIsFadingOut;

    u8 *m_pcmData;
    u32 m_pcmDataSize;
    u32 m_playPosition;
    i32 m_isPlaying;

    static void MusicHookCallback(void *udata, u8 *stream, int len);
};

class CWaveFile
{
  public:
    WAVEFORMATEX *m_pwfx;
    DWORD m_dwSize;
    i32 m_loopStartPoint;
    i32 m_loopEndPoint;

    u8 *m_fileData;
    u8 *m_pcmData;
    u32 m_pcmDataSize;
    WAVEFORMATEX m_wfxStorage;

    // Set by OpenAsync, consumed by CStreamingSound::MusicHookCallback. When
    // non-null, the audio callback reads the PCM buffer from this object
    // (after observing state==1) instead of from m_pcmData/m_pcmDataSize.
    // Lifetime is shared with the detached decoder worker so that swapping
    // BGM mid-decode never blocks the main thread.
    std::shared_ptr<AsyncDecodeData> m_async;

    CWaveFile();
    ~CWaveFile();

    HRESULT Open(char *strFileName, WAVEFORMATEX *pwfx, DWORD dwFlags);
    // Asynchronous variant: kicks off a background OGG decode and returns
    // immediately. The audio callback emits silence until decode completes.
    // Used for BGM where a 1–3 s decode would otherwise stall the main
    // thread (e.g. dialogue SKIP firing many MUSIC opcodes per frame).
    HRESULT OpenAsync(char *strFileName);
    HRESULT Close();
    DWORD GetSize();
    HRESULT ResetFile(bool loop);
    WAVEFORMATEX *GetFormat()
    {
        return m_pwfx;
    };
};
}; // namespace th06

#endif // DSUTIL_H
