/**
 * @file AudioTypes_Platform.h
 * @brief Dreamcast platform extension for the engine's `AudioTypes.h` fork.
 *
 * Stub — the engine's `AudioTypes.h` only forks on Windows today (XAudio2).
 * Dreamcast audio runs via KOS snd_* / the AICA driver; no shared types need
 * to leak into the engine layer yet.
 */

#pragma once
#include <stdint.h>
