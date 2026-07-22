/**
 * @file Audio_DC.cpp
 * @brief Dreamcast audio implementation — software mixer feeding one KOS
 *        snd_stream into the AICA.
 *
 * Architecture (mirrors the PSP port's single-mixer design):
 *   - Engine drives AUD_Play(voiceIndex 0..N, soundWave, vol, pitch, loop, ...)
 *     onto an internal voice table; AUD_Stop/SetVolume/SetPitch mutate entries.
 *   - One KOS snd_stream is allocated at 44100 Hz stereo. Its get-data callback
 *     mixes every active voice + stream into an interleaved S16 stereo buffer.
 *   - A dedicated polling thread calls snd_stream_poll() on a short interval so
 *     the AICA ring is kept fed; snd_stream_poll invokes our callback when it
 *     needs more PCM (the callback therefore runs on the poll thread).
 *   - Per-voice resampling is nearest-neighbour with a fractional source-frame
 *     cursor advanced by rate = sourceSampleRate * pitch / 44100 per output
 *     frame. Same quality tradeoff as the PSP path.
 *
 * Why snd_stream + a software mixer instead of snd_sfx per voice:
 *   snd_sfx gives fire-and-forget AICA channels but no per-frame control over
 *   pitch / looping / mixing that the engine's voice model needs, and no
 *   streaming path for the video player. One snd_stream + CPU mix mirrors what
 *   the PSP / Wii / 3DS backends already do and keeps all format handling
 *   (8/16-bit, mono/stereo, resample, per-side volume) in one place.
 *
 * Threading discipline:
 *   - Voice + stream state is guarded by a KOS binary semaphore (NOT a KOS
 *     mutex_t — the engine unlocks from non-owning threads, which mutex_t
 *     asserts on; same reason System_DC uses a semaphore for MutexObject).
 *   - The lock is held briefly by engine-side mutators and for the duration of
 *     one callback buffer-fill.
 *   - SoundWave pointers in voices are weak refs; the engine guarantees the
 *     SoundWave outlives any active play via its asset reference count.
 */

#if defined(POLYPHASE_PLATFORM_ADDON)

#include "Audio/Audio.h"
#include "Audio/AudioConstants.h"
#include "Engine/Assets/SoundWave.h"
#include "Log.h"

#include <kos.h>
#include <dc/sound/stream.h>
#include <kos/sem.h>
#include <kos/thread.h>

#include <stdlib.h>
#include <string.h>
#include <malloc.h>

namespace
{
    // ----- Output format ---------------------------------------------------
    constexpr int kOutputRate    = 44100;
    constexpr int kBytesPerFrame = 4;      // stereo 16-bit

    // SPU stream buffer size. Smaller than SND_STREAM_BUFFER_MAX (64K) to keep
    // the main-RAM mixer scratch small on the 16 MB console — 32K gives ~0.18 s
    // of latency, plenty when we poll from AUD_Update at frame rate. snd_stream
    // requests up to the whole buffer per callback, so the scratch is sized for
    // a full request.
    constexpr int kStreamBufBytes = 32 * 1024;
    constexpr int kMaxFrames      = kStreamBufBytes / kBytesPerFrame;

    constexpr float kMasterAtten = 0.5f;   // ~6 dB headroom before s16 clip

    // ----- Voice table -----------------------------------------------------
    struct DcVoice
    {
        bool           active        = false;
        const uint8_t* pcmData       = nullptr;
        uint32_t       numFrames      = 0;
        uint32_t       sampleRate     = 0;
        uint8_t        numChannels    = 1;
        uint8_t        bitsPerSample  = 16;
        bool           loop           = false;
        double         positionFrac   = 0.0;
        double         rate           = 1.0;
        int32_t        leftVolQ15     = 32768;
        int32_t        rightVolQ15    = 32768;
    };

    static DcVoice sVoices[AUDIO_MAX_VOICES];

    // ----- Streaming voice table (video player / PCM feeders) --------------
    constexpr uint32_t kMaxStreams     = 4;
    constexpr double   kStreamRingSecs = 0.5;

    struct DcStream
    {
        bool     inUse         = false;
        bool     paused        = false;
        uint32_t srcSampleRate = 0;
        uint8_t  numChannels   = 1;
        uint8_t  bitsPerSample = 16;
        int16_t* ring          = nullptr;
        uint32_t ringFrames    = 0;
        uint64_t writeFrameAbs = 0;
        double   readFrameAbs  = 0.0;
        double   rate          = 1.0;
        int32_t  leftVolQ15    = 32768;
        int32_t  rightVolQ15   = 32768;
    };

    static DcStream sStreams[kMaxStreams];

    static semaphore_t   sLock = SEM_INITIALIZER(1);   // guards sVoices + sStreams
    static snd_stream_hnd_t sHnd = SND_STREAM_INVALID;
    static bool          sInited      = false;
    static kthread_t*    sPollThread  = nullptr;
    static volatile bool sRun         = false;

    // Interleaved stereo output buffer handed back to snd_stream. 32-byte
    // aligned per the KOS callback contract; s32 accumulator keeps mixing
    // headroom before saturation.
    static int16_t __attribute__((aligned(32))) sOutBuffer[kMaxFrames * 2];
    static int32_t sMixBuffer[kMaxFrames * 2];

    inline int32_t SaturateS16(int32_t v)
    {
        if (v >  32767) return  32767;
        if (v < -32768) return -32768;
        return v;
    }

    inline void FetchFrame(const DcVoice& v, uint32_t srcFrame, int32_t& outL, int32_t& outR)
    {
        if (v.bitsPerSample == 16)
        {
            const int16_t* s = reinterpret_cast<const int16_t*>(v.pcmData);
            if (v.numChannels == 2) { outL = s[srcFrame * 2 + 0]; outR = s[srcFrame * 2 + 1]; }
            else                    { outL = outR = s[srcFrame]; }
        }
        else   // 8-bit unsigned PCM
        {
            if (v.numChannels == 2)
            {
                outL = (int32_t(v.pcmData[srcFrame * 2 + 0]) - 128) << 8;
                outR = (int32_t(v.pcmData[srcFrame * 2 + 1]) - 128) << 8;
            }
            else { outL = outR = (int32_t(v.pcmData[srcFrame]) - 128) << 8; }
        }
    }

    inline void FetchStreamFrame(const DcStream& s, uint64_t srcFrameAbs, int32_t& outL, int32_t& outR)
    {
        const uint32_t ringIdx = (uint32_t)(srcFrameAbs % s.ringFrames);
        if (s.numChannels == 2) { outL = s.ring[ringIdx * 2 + 0]; outR = s.ring[ringIdx * 2 + 1]; }
        else                    { outL = outR = s.ring[ringIdx]; }
    }

    // Mix `frames` stereo frames into sOutBuffer. Runs on the poll thread.
    void MixFrames(int frames)
    {
        const int samples = frames * 2;
        memset(sMixBuffer, 0, sizeof(int32_t) * samples);

        sem_wait(&sLock);

        for (uint32_t vi = 0; vi < AUDIO_MAX_VOICES; ++vi)
        {
            DcVoice& v = sVoices[vi];
            if (!v.active || v.pcmData == nullptr || v.numFrames == 0) continue;

            for (int f = 0; f < frames; ++f)
            {
                uint32_t srcFrame = (uint32_t)v.positionFrac;
                if (srcFrame >= v.numFrames)
                {
                    if (v.loop)
                    {
                        v.positionFrac -= (double)v.numFrames;
                        srcFrame = (uint32_t)v.positionFrac;
                        if (srcFrame >= v.numFrames) srcFrame = 0;
                    }
                    else { v.active = false; break; }
                }

                int32_t srcL, srcR;
                FetchFrame(v, srcFrame, srcL, srcR);
                sMixBuffer[f * 2 + 0] += (srcL * v.leftVolQ15)  >> 15;
                sMixBuffer[f * 2 + 1] += (srcR * v.rightVolQ15) >> 15;
                v.positionFrac += v.rate;
            }
        }

        for (uint32_t si = 0; si < kMaxStreams; ++si)
        {
            DcStream& s = sStreams[si];
            if (!s.inUse || s.paused || s.ring == nullptr) continue;

            for (int f = 0; f < frames; ++f)
            {
                const uint64_t srcAbs = (uint64_t)s.readFrameAbs;
                if (srcAbs >= s.writeFrameAbs) break;   // under-run: silence tail
                int32_t srcL, srcR;
                FetchStreamFrame(s, srcAbs, srcL, srcR);
                sMixBuffer[f * 2 + 0] += (srcL * s.leftVolQ15)  >> 15;
                sMixBuffer[f * 2 + 1] += (srcR * s.rightVolQ15) >> 15;
                s.readFrameAbs += s.rate;
            }
        }

        sem_signal(&sLock);

        for (int i = 0; i < samples; ++i)
            sOutBuffer[i] = (int16_t)SaturateS16(sMixBuffer[i]);
    }

    void* MixCallback(snd_stream_hnd_t /*hnd*/, int smp_req, int* smp_recv)
    {
        // smp_req is interleaved bytes requested for a stereo 16-bit stream.
        int frames = smp_req / kBytesPerFrame;
        if (frames > kMaxFrames) frames = kMaxFrames;
        if (frames < 0) frames = 0;
        MixFrames(frames);
        *smp_recv = frames * kBytesPerFrame;
        return sOutBuffer;
    }

    // Dedicated audio thread: keeps the SPU fed independently of the render loop.
    // Polling + mixing + the G2 transfer to SPU RAM must NOT sit on the main
    // thread — there they periodically stall the render frame (the streamed-BGM
    // hitch). File I/O for the streaming pump stays on the main thread (engine's
    // UpdateStreamingSources); this thread only drains the already-submitted ring.
    void* PollThread(void* /*arg*/)
    {
        while (sRun)
        {
            snd_stream_poll(sHnd);
            thd_sleep(10);   // buffer depth (~0.18 s) comfortably covers the interval
        }
        return nullptr;
    }

    inline int32_t ClampVolQ15(float v)
    {
        const float scaled = v * kMasterAtten * 32768.0f;
        if (scaled < 0.0f)     return 0;
        if (scaled > 32768.0f) return 32768;
        return (int32_t)scaled;
    }
}  // namespace

// ----- API impl ------------------------------------------------------------

void AUD_Initialize()
{
    if (snd_stream_init() != 0)
    {
        LogError("Audio_DC: snd_stream_init failed");
        return;
    }

    sHnd = snd_stream_alloc(MixCallback, kStreamBufBytes);
    if (sHnd == SND_STREAM_INVALID)
    {
        LogError("Audio_DC: snd_stream_alloc failed");
        snd_stream_shutdown();
        return;
    }

    snd_stream_volume(sHnd, 255);
    snd_stream_start(sHnd, kOutputRate, 1 /*stereo*/);

    // Spawn the dedicated audio thread (offloads mixing off the render frame).
    sRun = true;
    kthread_attr_t attr = {};
    attr.create_detached = false;
    attr.stack_size      = 32 * 1024;
    attr.prio            = PRIO_DEFAULT;
    attr.label           = "polyphase_audio";
    sPollThread = thd_create_ex(&attr, PollThread, nullptr);
    if (sPollThread == nullptr)
    {
        LogError("Audio_DC: audio thread create failed — falling back to per-frame poll");
        sRun = false;   // AUD_Update will poll instead
    }

    sInited = true;
    LogDebug("Audio_DC: snd_stream up, 44100Hz stereo, %d voices%s", AUDIO_MAX_VOICES,
             sPollThread ? " (threaded)" : "");
}

void AUD_Shutdown()
{
    if (!sInited) return;

    sRun = false;
    if (sPollThread != nullptr) { thd_join(sPollThread, nullptr); sPollThread = nullptr; }

    if (sHnd != SND_STREAM_INVALID)
    {
        snd_stream_stop(sHnd);
        snd_stream_destroy(sHnd);
        sHnd = SND_STREAM_INVALID;
    }
    snd_stream_shutdown();

    for (uint32_t i = 0; i < kMaxStreams; ++i)
    {
        if (sStreams[i].ring != nullptr) free(sStreams[i].ring);
        sStreams[i] = DcStream{};
    }
    for (uint32_t i = 0; i < AUDIO_MAX_VOICES; ++i) sVoices[i] = DcVoice{};

    sInited = false;
}

// The dedicated audio thread does the polling/mixing. Only poll here as a
// fallback if that thread failed to spawn (keeps audio alive, just on the frame).
void AUD_Update()
{
    if (!sInited || sRun) return;   // sRun == thread is handling it
    snd_stream_poll(sHnd);
}

void AUD_Play(uint32_t voiceIndex, SoundWave* soundWave, float volume,
              float pitch, bool loop, float /*startTime*/, bool spatial)
{
    if (voiceIndex >= AUDIO_MAX_VOICES || soundWave == nullptr) return;
    const uint8_t* pcm = soundWave->GetWaveData();
    const uint32_t numFrames = soundWave->GetNumSamples();
    if (pcm == nullptr || numFrames == 0) return;

    sem_wait(&sLock);
    DcVoice& v      = sVoices[voiceIndex];
    v.pcmData       = pcm;
    v.numFrames     = numFrames;
    v.sampleRate    = soundWave->GetSampleRate();
    v.numChannels   = (uint8_t)soundWave->GetNumChannels();
    v.bitsPerSample = (uint8_t)soundWave->GetBitsPerSample();
    v.loop          = loop;
    v.positionFrac  = 0.0;
    v.rate          = (v.sampleRate > 0)
                      ? ((double)v.sampleRate * (double)pitch / (double)kOutputRate) : 1.0;
    const int32_t vq15 = ClampVolQ15(volume);
    v.leftVolQ15    = vq15;
    v.rightVolQ15   = vq15;
    v.active        = true;
    (void)spatial;   // engine sets L/R volumes separately for spatial panning
    sem_signal(&sLock);
}

void AUD_Stop(uint32_t voiceIndex)
{
    if (voiceIndex >= AUDIO_MAX_VOICES) return;
    sem_wait(&sLock);
    sVoices[voiceIndex].active = false;
    sem_signal(&sLock);
}

bool AUD_IsPlaying(uint32_t voiceIndex)
{
    if (voiceIndex >= AUDIO_MAX_VOICES) return false;
    return sVoices[voiceIndex].active;
}

void AUD_SetVolume(uint32_t voiceIndex, float leftVolume, float rightVolume)
{
    if (voiceIndex >= AUDIO_MAX_VOICES) return;
    sem_wait(&sLock);
    sVoices[voiceIndex].leftVolQ15  = ClampVolQ15(leftVolume);
    sVoices[voiceIndex].rightVolQ15 = ClampVolQ15(rightVolume);
    sem_signal(&sLock);
}

void AUD_SetPitch(uint32_t voiceIndex, float pitch)
{
    if (voiceIndex >= AUDIO_MAX_VOICES) return;
    sem_wait(&sLock);
    DcVoice& v = sVoices[voiceIndex];
    v.rate = (v.sampleRate > 0)
             ? ((double)v.sampleRate * (double)pitch / (double)kOutputRate) : 1.0;
    sem_signal(&sLock);
}

uint8_t* AUD_AllocWaveBuffer(uint32_t size) { return (uint8_t*)malloc(size); }
void AUD_FreeWaveBuffer(void* buffer) { free(buffer); }
void AUD_ProcessWaveBuffer(SoundWave* /*soundWave*/) {}

// ----- Streaming voices ---------------------------------------------------

uint32_t AUD_OpenStream(uint32_t sampleRate, uint32_t numChannels, uint32_t bitsPerSample)
{
    if (!sInited) return 0;
    if (numChannels != 1 && numChannels != 2)
    {
        LogWarning("AUD_OpenStream: only mono/stereo supported (got %u channels)", numChannels);
        return 0;
    }
    if (bitsPerSample != 16)
    {
        LogWarning("AUD_OpenStream: only 16-bit PCM supported (got %u bps)", bitsPerSample);
        return 0;
    }

    sem_wait(&sLock);
    uint32_t pickedIdx = kMaxStreams;
    for (uint32_t i = 0; i < kMaxStreams; ++i)
        if (!sStreams[i].inUse) { pickedIdx = i; break; }

    if (pickedIdx == kMaxStreams)
    {
        sem_signal(&sLock);
        LogWarning("AUD_OpenStream: no free stream slots (pool=%u)", kMaxStreams);
        return 0;
    }

    DcStream& s = sStreams[pickedIdx];
    const uint32_t ringFrames = (uint32_t)((double)sampleRate * kStreamRingSecs);
    const uint32_t int16Count = ringFrames * numChannels;
    s.ring = (int16_t*)memalign(32, int16Count * sizeof(int16_t));
    if (s.ring == nullptr)
    {
        sem_signal(&sLock);
        LogError("AUD_OpenStream: ring alloc failed");
        return 0;
    }

    s.ringFrames    = ringFrames;
    s.srcSampleRate = sampleRate;
    s.numChannels   = (uint8_t)numChannels;
    s.bitsPerSample = (uint8_t)bitsPerSample;
    s.writeFrameAbs = 0;
    s.readFrameAbs  = 0.0;
    s.rate          = (double)sampleRate / (double)kOutputRate;
    s.leftVolQ15    = ClampVolQ15(1.0f);
    s.rightVolQ15   = ClampVolQ15(1.0f);
    s.paused        = false;
    s.inUse         = true;
    sem_signal(&sLock);

    return pickedIdx + 1;   // 1-based; 0 is the failure sentinel
}

void AUD_CloseStream(uint32_t streamId)
{
    if (streamId == 0 || streamId > kMaxStreams) return;
    sem_wait(&sLock);
    DcStream& s = sStreams[streamId - 1];
    if (s.inUse)
    {
        if (s.ring != nullptr) { free(s.ring); s.ring = nullptr; }
        s.ringFrames = 0; s.writeFrameAbs = 0; s.readFrameAbs = 0.0;
        s.inUse = false; s.paused = false;
    }
    sem_signal(&sLock);
}

int32_t AUD_SubmitStreamBuffer(uint32_t streamId, const uint8_t* data, uint32_t byteSize)
{
    if (streamId == 0 || streamId > kMaxStreams || data == nullptr || byteSize == 0) return 0;

    sem_wait(&sLock);
    DcStream& s = sStreams[streamId - 1];
    if (!s.inUse || s.ring == nullptr) { sem_signal(&sLock); return 0; }

    const uint32_t bpf = s.numChannels * 2;   // 16-bit guaranteed
    uint32_t submitFrames = byteSize / bpf;
    if (submitFrames == 0) { sem_signal(&sLock); return 0; }
    if (submitFrames > s.ringFrames) submitFrames = s.ringFrames;

    const uint64_t readAbsU64 = (uint64_t)s.readFrameAbs;
    const uint64_t inFlight   = s.writeFrameAbs - readAbsU64;
    const uint32_t freeFrames = (inFlight < s.ringFrames) ? (uint32_t)(s.ringFrames - inFlight) : 0u;
    if (freeFrames == 0) { sem_signal(&sLock); return 0; }   // ring full — caller retries next frame
    // PARTIAL accept: top up whatever space is free rather than rejecting the whole
    // chunk when it doesn't fully fit. Rejecting-all only refilled the ring when it
    // hit empty (sawtooth) → periodic underruns → stretched/slow playback. The pump
    // stashes the remainder and resubmits it, so a partial accept keeps the ring full.
    if (submitFrames > freeFrames) submitFrames = freeFrames;

    const int16_t* srcInt16 = reinterpret_cast<const int16_t*>(data);
    const uint32_t headIdx  = (uint32_t)(s.writeFrameAbs % s.ringFrames);
    const uint32_t firstChunkFrames = (headIdx + submitFrames <= s.ringFrames)
                                        ? submitFrames : (s.ringFrames - headIdx);

    memcpy(s.ring + headIdx * s.numChannels, srcInt16,
           firstChunkFrames * s.numChannels * sizeof(int16_t));
    if (firstChunkFrames < submitFrames)
    {
        const uint32_t wrappedFrames = submitFrames - firstChunkFrames;
        memcpy(s.ring, srcInt16 + firstChunkFrames * s.numChannels,
               wrappedFrames * s.numChannels * sizeof(int16_t));
    }

    s.writeFrameAbs += submitFrames;
    sem_signal(&sLock);
    return (int32_t)(submitFrames * bpf);
}

uint64_t AUD_GetStreamPlayedSamples(uint32_t streamId)
{
    if (streamId == 0 || streamId > kMaxStreams) return 0;
    const DcStream& s = sStreams[streamId - 1];
    return s.inUse ? (uint64_t)s.readFrameAbs : 0;
}

void AUD_SetStreamVolume(uint32_t streamId, float volume)
{
    if (streamId == 0 || streamId > kMaxStreams) return;
    sem_wait(&sLock);
    DcStream& s = sStreams[streamId - 1];
    if (s.inUse) { s.leftVolQ15 = ClampVolQ15(volume); s.rightVolQ15 = ClampVolQ15(volume); }
    sem_signal(&sLock);
}

void AUD_SetStreamPaused(uint32_t streamId, bool paused)
{
    if (streamId == 0 || streamId > kMaxStreams) return;
    sem_wait(&sLock);
    if (sStreams[streamId - 1].inUse) sStreams[streamId - 1].paused = paused;
    sem_signal(&sLock);
}

void AUD_FlushStream(uint32_t streamId)
{
    if (streamId == 0 || streamId > kMaxStreams) return;
    sem_wait(&sLock);
    DcStream& s = sStreams[streamId - 1];
    if (s.inUse) s.writeFrameAbs = (uint64_t)s.readFrameAbs;
    sem_signal(&sLock);
}

#endif // POLYPHASE_PLATFORM_ADDON
