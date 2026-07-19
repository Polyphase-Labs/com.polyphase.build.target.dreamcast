/**
 * @file Audio_DC.cpp
 * @brief Dreamcast AICA audio backend for the engine's AUD_* surface.
 *
 * PHASE 1 SKELETON. Lifecycle is stubbed to no-ops so the engine boots
 * silently; a later phase wires KOS snd_* / the AICA sound driver. Audio
 * analysis is disabled by default on Dreamcast (16 MB RAM) via
 * Constants_Dreamcast.h.
 *
 * Built only when POLYPHASE_PLATFORM_ADDON is defined.
 */
#if defined(POLYPHASE_PLATFORM_ADDON)

#include "Audio/Audio.h"
#include "Engine/Assets/SoundWave.h"
#include "Log.h"

#include <kos.h>
#include <stdlib.h>

// ---- Lifecycle (Phase-1 stubs) --------------------------------------------

void AUD_Initialize() { LogDebug("Audio_DC: stub (AICA backend is a later phase)"); }
void AUD_Shutdown() {}
void AUD_Update() {}

// ---- Generated no-op stubs (engine ABI) -----------------------------------


void AUD_Play(uint32_t voiceIndex, SoundWave* soundWave, float volume,
              float pitch, bool loop, float /*startTime*/, bool spatial) {}
void AUD_Stop(uint32_t voiceIndex) {}
bool AUD_IsPlaying(uint32_t voiceIndex) { return false; }
void AUD_SetVolume(uint32_t voiceIndex, float leftVolume, float rightVolume) {}
void AUD_SetPitch(uint32_t voiceIndex, float pitch) {}
uint8_t* AUD_AllocWaveBuffer(uint32_t size) { return (uint8_t*)malloc(size); }
void AUD_FreeWaveBuffer(void* buffer) { free(buffer); }
void AUD_ProcessWaveBuffer(SoundWave* /*soundWave*/) {}
uint32_t AUD_OpenStream(uint32_t sampleRate, uint32_t numChannels, uint32_t bitsPerSample) { return 0; }
void AUD_CloseStream(uint32_t streamId) {}
int32_t AUD_SubmitStreamBuffer(uint32_t streamId, const uint8_t* data, uint32_t byteSize) { return 0; }
uint64_t AUD_GetStreamPlayedSamples(uint32_t streamId) { return 0; }
void AUD_SetStreamVolume(uint32_t streamId, float volume) {}
void AUD_SetStreamPaused(uint32_t streamId, bool paused) {}
void AUD_FlushStream(uint32_t streamId) {}

#endif // POLYPHASE_PLATFORM_ADDON