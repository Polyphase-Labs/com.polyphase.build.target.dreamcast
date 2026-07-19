/**
 * @file InputTypes_Platform.h
 * @brief Dreamcast platform extension for the engine's `InputTypes.h` fork.
 *
 * Pulls in KOS's maple-bus controller headers so engine code referencing DC
 * input types compiles. Input_DC.cpp keeps cont_state_t internal for now
 * (Phase-1 stub); this header is the seam for future inline DC input blocks.
 */

#pragma once

#include <stdint.h>
#include <dc/maple.h>
#include <dc/maple/controller.h>
