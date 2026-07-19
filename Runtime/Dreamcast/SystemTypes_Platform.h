/**
 * @file SystemTypes_Platform.h
 * @brief Dreamcast platform extension for the engine's `SystemTypes.h` fork.
 *
 * Picked up automatically when `POLYPHASE_PLATFORM_ADDON=1` is defined:
 * ActionManager generates `<projectDir>/Generated/PolyphasePlatform_SystemTypes.h`
 * which includes this file by absolute path, and Makefile_Dreamcast puts
 * Generated/ on the include path.
 *
 * KallistiOS gives us a Unix-ish threading + filesystem layer; the typedefs
 * below map the engine's ThreadObject/MutexObject/DirEntry onto it.
 */

#pragma once

#include <stdint.h>
#include <stddef.h>

// KallistiOS brings in kthread_t / mutex_t and the newlib POSIX headers.
#include <kos.h>
#include <dirent.h>
#include <sys/types.h>
#include <sys/stat.h>

// ----- Threading typedefs --------------------------------------------------
// KOS threads are kthread_t* handles; thd_create's routine is
// `void* (*)(void*)`, which is EXACTLY the engine's ThreadFuncFP shape, so no
// trampoline shim is needed (System_DC hands the engine function straight to
// thd_create). ThreadFuncRet is void* — like the Linux/pthread arm — so
// THREAD_RETURN() resolves to `return 0;` (0 -> null pointer) and we do NOT
// define POLYPHASE_PLATFORM_ADDON_VOID_THREAD_RETURN.
// MutexObject is a KOS binary semaphore, NOT a KOS mutex_t. KOS mutexes assert
// that unlock is called by the locking thread (mutex.c: m->holder == thd), but
// the engine unlocks some locks from a different thread than locked them (the
// PSP port relies on the same any-thread-signal behaviour via a binary
// semaphore). A semaphore has no owner check, so it matches the engine's usage.
typedef kthread_t*  ThreadObject;
typedef semaphore_t MutexObject;
typedef void*       ThreadFuncRet;

// ----- DirEntry injection --------------------------------------------------
// KOS's readdir keeps internal state that can be corrupted if a file open/read
// is interleaved with directory iteration (same hazard the PSP port hit with
// sceIoDread). System_DC drains the whole directory into a heap vector at
// SYS_OpenDirectory time, then iterates the snapshot — so these members hold
// an opaque pointer to that vector plus the read cursor. Kept as void* to
// avoid pulling <vector> into the engine's SystemTypes.h.
#define POLYPHASE_PLATFORM_ADDON_DIRENTRY_MEMBERS \
    void*        mDirDrain = nullptr; \
    unsigned int mDirIndex = 0;

// ----- SystemState injection -----------------------------------------------
// Dreamcast has no windowing concept (fixed 640x480). Track only a quit flag
// for the main loop and a background flag for symmetry with the other consoles.
#define POLYPHASE_PLATFORM_ADDON_SYSTEMSTATE_MEMBERS \
    bool mQuitRequested = false; \
    bool mInBackground  = false;
