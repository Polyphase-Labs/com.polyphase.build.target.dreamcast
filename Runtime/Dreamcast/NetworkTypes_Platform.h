/**
 * @file NetworkTypes_Platform.h
 * @brief Dreamcast platform extension for the engine's `NetworkTypes.h` fork.
 *
 * Stub. Dreamcast networking (BBA / dcload) is out of scope for the initial
 * port; SocketHandle maps to int32 like the other Unix-like platforms. A real
 * port can opt into KOS's lwIP sockets and add proper headers here.
 */

#pragma once
#include <stdint.h>

typedef int32_t SocketHandle;
