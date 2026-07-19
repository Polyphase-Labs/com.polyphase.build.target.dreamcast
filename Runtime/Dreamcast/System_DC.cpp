/**
 * @file System_DC.cpp
 * @brief Dreamcast (KallistiOS) implementation of the engine's SYS_* surface.
 *
 * KallistiOS exposes a Unix-ish newlib layer (stdio, dirent, threads, mutexes)
 * so most of this mirrors the engine's Linux path rather than the PSP's Sce*
 * API. Dreamcast specifics:
 *
 *   - Assets ship on the GD-ROM/CD image (mkdcdisc folds the package dir into
 *     the disc), mounted at /cd/. Save data lives on the VMU (/vmu/a1/).
 *   - Threads are kthread_t* via thd_create; the routine signature matches the
 *     engine's ThreadFuncFP exactly, so no shim is needed.
 *   - "Mutexes" are KOS mutex_t.
 *   - Time is timer_us_gettime64().
 *   - No window concept (fixed 640x480); window/clipboard/dialog SYS_ are stubs.
 *
 * Built only when POLYPHASE_PLATFORM_ADDON is defined.
 */

#if defined(POLYPHASE_PLATFORM_ADDON)

#include "System/System.h"
#include "Engine.h"
#include "Stream.h"
#include "Log.h"
#include "Utilities.h"

#include <kos.h>
#include <arch/timer.h>

#include <dirent.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>

#include <string>
#include <vector>
#include <utility>

static bool sInitialized = false;

// =========================================================================
// Lifecycle
// =========================================================================

void SYS_Initialize()
{
    if (sInitialized) return;
    sInitialized = true;
    LogDebug("System_DC: initialised (KallistiOS)");
}

void SYS_Shutdown()
{
    sInitialized = false;
}

void SYS_Update()
{
    // KOS has no per-frame system pump equivalent; the maple/PVR drivers run on
    // their own threads. Nothing to do here.
}

// =========================================================================
// Paths
// =========================================================================

std::string SYS_GetPolyphasePath()
{
    // mkdcdisc folds the packaged asset directory into the disc root, mounted
    // at /cd/ at runtime. Cache it.
    static std::string sBase = "/cd/";
    return sBase;
}

std::string SYS_GetExecutablePath()
{
    return "/cd/1ST_READ.BIN";
}

std::string SYS_GetCurrentDirectoryPath()
{
    return "/cd/";
}

std::string SYS_GetAbsolutePath(const std::string& relativePath)
{
    if (!relativePath.empty() && relativePath[0] == '/') return relativePath;
    return SYS_GetPolyphasePath() + relativePath;
}

void SYS_ExplorerOpenDirectory(const std::string& /*dirPath*/) {}
void SYS_OpenFileWithDefaultApp(const std::string& /*filePath*/) {}
void SYS_SetWorkingDirectory(const std::string& /*dirPath*/) {}

// =========================================================================
// File I/O (newlib stdio — KOS maps /cd, /ram, /pc, /vmu transparently)
// =========================================================================

bool SYS_DoesFileExist(const char* path, bool /*isAsset*/)
{
    if (path == nullptr) return false;
    struct stat st;
    return stat(path, &st) == 0;
}

void SYS_AcquireFileData(const char* path, bool /*isAsset*/, int32_t maxSize,
                         char*& outData, uint32_t& outSize)
{
    outData = nullptr;
    outSize = 0;
    if (path == nullptr) return;

    FILE* f = fopen(path, "rb");
    if (f == nullptr)
    {
        LogWarning("SYS_AcquireFileData: fopen failed for '%s'", path);
        return;
    }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size < 0) { fclose(f); return; }

    uint32_t actualSize = (uint32_t)size;
    if (maxSize > 0 && actualSize > (uint32_t)maxSize) actualSize = (uint32_t)maxSize;

    outData = (char*)malloc(actualSize);
    if (outData == nullptr)
    {
        fclose(f);
        LogError("SYS_AcquireFileData: malloc(%u) failed for '%s'", actualSize, path);
        return;
    }

    const size_t read = fread(outData, 1, actualSize, f);
    fclose(f);
    outSize = (uint32_t)read;
}

void SYS_ReleaseFileData(char* data)
{
    free(data);
}

bool SYS_CreateDirectory(const char* dirPath)
{
    if (dirPath == nullptr) return false;
    return mkdir(dirPath, 0777) == 0;
}

void SYS_RemoveDirectory(const char* dirPath)
{
    if (dirPath == nullptr) return;
    rmdir(dirPath);
}

// Directory iteration drains the whole listing into a heap vector up-front
// (see SystemTypes_Platform.h). Interleaving file I/O with readdir on KOS can
// silently skip entries; snapshotting avoids the hazard and matches the FAT/
// ISO filesystem behaviour the AssetManager relies on.
namespace
{
    using DirDrain = std::vector<std::pair<std::string, bool>>; // (name, isDir)
}

void SYS_OpenDirectory(const std::string& dirPath, DirEntry& outDirEntry)
{
    outDirEntry.mValid = false;
    outDirEntry.mDirDrain = nullptr;
    outDirEntry.mDirIndex = 0;

    DIR* dir = opendir(dirPath.c_str());
    if (dir == nullptr) return;

    DirDrain* drain = new DirDrain();
    struct dirent* ent = nullptr;
    while ((ent = readdir(dir)) != nullptr)
    {
        if (ent->d_name[0] == '\0') continue;
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;

        bool isDir = false;
        // KOS dirent may not fill d_type reliably; stat to be sure.
        std::string full = dirPath;
        if (!full.empty() && full.back() != '/') full += '/';
        full += ent->d_name;
        struct stat st;
        if (stat(full.c_str(), &st) == 0) isDir = S_ISDIR(st.st_mode);
        drain->push_back(std::make_pair(std::string(ent->d_name), isDir));
    }
    closedir(dir);

    strncpy(outDirEntry.mDirectoryPath, dirPath.c_str(), MAX_PATH_SIZE);
    outDirEntry.mDirectoryPath[MAX_PATH_SIZE] = '\0';
    outDirEntry.mDirDrain = drain;
    outDirEntry.mDirIndex = 0;
    outDirEntry.mValid = true;

    // Prime the first entry so callers get the same invariant as other
    // platforms: after Open, either mFilename holds the first name or !mValid.
    SYS_IterateDirectory(outDirEntry);
}

void SYS_IterateDirectory(DirEntry& dirEntry)
{
    DirDrain* drain = static_cast<DirDrain*>(dirEntry.mDirDrain);
    if (drain == nullptr || dirEntry.mDirIndex >= drain->size())
    {
        dirEntry.mValid = false;
        return;
    }

    const std::pair<std::string, bool>& e = (*drain)[dirEntry.mDirIndex++];
    strncpy(dirEntry.mFilename, e.first.c_str(), MAX_PATH_SIZE);
    dirEntry.mFilename[MAX_PATH_SIZE] = '\0';
    dirEntry.mDirectory = e.second;
    dirEntry.mValid = true;
}

void SYS_CloseDirectory(DirEntry& dirEntry)
{
    DirDrain* drain = static_cast<DirDrain*>(dirEntry.mDirDrain);
    delete drain;
    dirEntry.mDirDrain = nullptr;
    dirEntry.mDirIndex = 0;
    dirEntry.mValid = false;
}

bool SYS_CopyFile(const char* sourcePath, const char* destPath)
{
    if (sourcePath == nullptr || destPath == nullptr) return false;

    FILE* src = fopen(sourcePath, "rb");
    if (src == nullptr) return false;
    FILE* dst = fopen(destPath, "wb");
    if (dst == nullptr) { fclose(src); return false; }

    bool ok = true;
    char buf[4096];
    size_t read = 0;
    while ((read = fread(buf, 1, sizeof(buf), src)) > 0)
    {
        if (fwrite(buf, 1, read, dst) != read) { ok = false; break; }
    }
    fclose(src);
    fclose(dst);
    return ok;
}

void SYS_CopyDirectory(const char* /*sourceDir*/, const char* /*destDir*/) {}
bool SYS_CopyDirectoryRecursive(const std::string& /*sourceDir*/, const std::string& /*destDir*/) { return false; }
void SYS_MoveDirectory(const char* sourceDir, const char* destDir)
{
    if (sourceDir && destDir) rename(sourceDir, destDir);
}
void SYS_MoveFile(const char* sourcePath, const char* destPath)
{
    if (sourcePath && destPath) rename(sourcePath, destPath);
}
void SYS_RemoveFile(const char* path)
{
    if (path) remove(path);
}
bool SYS_Rename(const char* oldPath, const char* newPath)
{
    if (oldPath == nullptr || newPath == nullptr) return false;
    return rename(oldPath, newPath) == 0;
}

std::vector<std::string> SYS_OpenFileDialog() { return {}; }
std::string SYS_SaveFileDialog() { return ""; }
std::string SYS_SelectFolderDialog() { return ""; }

std::string SYS_GetFileName(const std::string& path)
{
    const size_t slash = path.find_last_of("/\\");
    if (slash == std::string::npos) return path;
    return path.substr(slash + 1);
}

// =========================================================================
// Threading (KOS kthread_t / mutex_t)
// =========================================================================

ThreadObject* SYS_CreateThread(ThreadFuncFP func, void* arg)
{
    if (func == nullptr) return nullptr;

    // The engine's ThreadFuncFP is `void* (*)(void*)`, exactly KOS thd_create's
    // routine type — hand it straight through, no trampoline needed.
    //
    // KOS's default thread stack is only 32 KB (THD_STACK_SIZE) — too small for
    // the engine's worker threads (asset loading, etc.), which overrun it and
    // trip KOS's scheduler stack-underrun assert. Give them a generous 256 KB
    // via thd_create_ex.
    kthread_attr_t attr = {};
    attr.create_detached = false; // joinable (SYS_JoinThread reaps it)
    attr.stack_size      = 512 * 1024;
    attr.prio            = PRIO_DEFAULT;
    attr.label           = "polyphase_worker";

    kthread_t* th = thd_create_ex(&attr, func, arg);
    printf("[SYS_CreateThread] func=%p stack=%uKB -> th=%p\n",
           (void*)func, (unsigned)(attr.stack_size / 1024), (void*)th);
    if (th == nullptr)
    {
        LogError("SYS_CreateThread: thd_create_ex failed");
        return nullptr;
    }

    ThreadObject* out = new ThreadObject;
    *out = th;
    return out;
}

void SYS_JoinThread(ThreadObject* thread)
{
    if (thread == nullptr || *thread == nullptr) return;
    thd_join(*thread, nullptr);
    // thd_join reaps the thread; the handle is no longer valid afterwards.
    *thread = nullptr;
}

void SYS_DestroyThread(ThreadObject* thread)
{
    if (thread == nullptr) return;
    if (*thread != nullptr) thd_destroy(*thread);
    delete thread;
}

// MutexObject is a KOS binary semaphore (see SystemTypes_Platform.h) — the
// engine unlocks some locks from a non-owning thread, which a KOS mutex_t
// asserts on but a semaphore (no owner check) permits, matching the PSP port.
MutexObject* SYS_CreateMutex()
{
    MutexObject* m = new MutexObject;
    if (sem_init(m, 1) != 0)   // binary semaphore, initially unlocked (count 1)
    {
        LogError("SYS_CreateMutex: sem_init failed");
        delete m;
        return nullptr;
    }
    return m;
}

void SYS_LockMutex(MutexObject* mutex)
{
    if (mutex == nullptr) return;
    sem_wait(mutex);
}

void SYS_UnlockMutex(MutexObject* mutex)
{
    if (mutex == nullptr) return;
    sem_signal(mutex);
}

void SYS_DestroyMutex(MutexObject* mutex)
{
    if (mutex == nullptr) return;
    sem_destroy(mutex);
    delete mutex;
}

void SYS_Sleep(uint32_t milliseconds)
{
    thd_sleep(milliseconds);
}

// =========================================================================
// Time
// =========================================================================

uint64_t SYS_GetTimeMicroseconds()
{
    return timer_us_gettime64();
}

// =========================================================================
// Process exec — N/A on Dreamcast. SystemUtils.cpp supplies ExecCommon /
// SYS_ExecFull (stubbed under POLYPHASE_PLATFORM_ADDON); just provide SYS_Exec.
// =========================================================================

void SYS_Exec(const char* /*cmd*/, std::string* output)
{
    if (output) output->clear();
}

// =========================================================================
// Memory (16 MB main RAM; 8 MB VRAM; 2 MB audio RAM)
// =========================================================================

void* SYS_AlignedMalloc(uint32_t size, uint32_t alignment)
{
    return memalign(alignment, size);
}

void SYS_AlignedFree(void* pointer)
{
    free(pointer);
}

std::vector<MemoryStat> SYS_GetMemoryStats()
{
    std::vector<MemoryStat> stats;

    struct mallinfo mi = mallinfo();

    MemoryStat mainRam;
    mainRam.mName = "MainRAM";
    mainRam.mBytesAllocated = (uint32_t)mi.uordblks;
    mainRam.mBytesFree = (16u * 1024u * 1024u) - (uint32_t)mi.uordblks;
    stats.push_back(mainRam);

    MemoryStat vram;
    vram.mName = "VRAM";
    vram.mBytesAllocated = 0; // PVR allocator not tracked yet (Phase 2)
    vram.mBytesFree = 8u * 1024u * 1024u;
    stats.push_back(vram);

    return stats;
}

float SYS_GetRAMUsage()    { auto s = SYS_GetMemoryStats(); return s.empty() ? 0.0f : (float)s[0].mBytesAllocated; }
float SYS_GetVRAMUsage()   { auto s = SYS_GetMemoryStats(); return s.size() < 2 ? 0.0f : (float)s[1].mBytesAllocated; }
float SYS_GetRAM1Usage()   { return SYS_GetRAMUsage(); }
float SYS_GetRAM2Usage()   { return 0.0f; }
float SYS_GetCPUUsage()    { return 0.0f; }
float SYS_GetTotalRAM()    { return (float)(16u * 1024u * 1024u); }
float SYS_GetTotalVRAM()   { return (float)(8u * 1024u * 1024u); }
float SYS_GetTotalRAM1()   { return SYS_GetTotalRAM(); }
float SYS_GetTotalRAM2()   { return 0.0f; }

// =========================================================================
// Save data — VMU (/vmu/a1/). Phase-1 minimal flat-file layout.
//
// Real VMU integration needs vmufs headers (icon + per-file blocks); for the
// skeleton we use newlib fopen against /vmu/a1/, which KOS maps to the VMU
// filesystem. Names are truncated to the VMU's 12-char limit by the driver.
// =========================================================================

namespace
{
    inline std::string SavePath(const char* saveName)
    {
        std::string p = "/vmu/a1/";
        p += saveName;
        return p;
    }
}

bool SYS_ReadSave(const char* saveName, Stream& outStream)
{
    if (saveName == nullptr) return false;
    if (!SYS_DoesSaveExist(saveName))
    {
        LogWarning("SYS_ReadSave: '%s' does not exist", saveName);
        return false;
    }
    outStream.ReadFile(SavePath(saveName).c_str(), /*isAsset=*/false);
    return outStream.GetSize() > 0;
}

bool SYS_WriteSave(const char* saveName, Stream& stream)
{
    if (saveName == nullptr) return false;
    const std::string path = SavePath(saveName);
    const bool ok = stream.WriteFile(path.c_str());
    if (ok) LogDebug("Save written: %s (%u bytes)", saveName, (unsigned)stream.GetSize());
    else    LogError("SYS_WriteSave: failed to write '%s' (VMU full or absent?)", path.c_str());
    return ok;
}

bool SYS_DoesSaveExist(const char* saveName)
{
    if (saveName == nullptr) return false;
    FILE* f = fopen(SavePath(saveName).c_str(), "rb");
    if (f == nullptr) return false;
    fclose(f);
    return true;
}

bool SYS_DeleteSave(const char* saveName)
{
    if (saveName == nullptr) return false;
    return remove(SavePath(saveName).c_str()) == 0;
}

void SYS_UnmountMemoryCard() {}

// =========================================================================
// Clipboard — N/A
// =========================================================================

void SYS_SetClipboardText(const std::string& /*str*/) {}
std::string SYS_GetClipboardText() { return ""; }

// =========================================================================
// Logging / assertions / console
// =========================================================================

// PHASE-1 DIAGNOSTIC LOG SINK. Main_DC points this at an on-screen console
// (conio over PVR — the display path proven to work under flycast/REIOS) once
// it's initialised, so the engine's own boot log renders on the Dreamcast
// screen. Until then (and if never set) we fall back to printf/stdout, which
// reaches dc-tool/serial. Defined here so System_DC needs no conio include.
void (*gDcLogSink)(const char* line) = nullptr;

void SYS_Log(LogSeverity severity, const char* format, va_list arg)
{
    char buf[1024];
    vsnprintf(buf, sizeof(buf), format, arg);

    const char* sevTag = (severity == LogSeverity::Error)   ? "[E] "
                       : (severity == LogSeverity::Warning) ? "[W] "
                       :                                       "[D] ";
    char line[1100];
    snprintf(line, sizeof(line), "%s%s", sevTag, buf);

    if (gDcLogSink != nullptr) gDcLogSink(line);
    else                       printf("%s\n", line);
}

void SYS_Assert(const char* exprString, const char* fileString, uint32_t lineNumber)
{
    printf("ASSERT: %s at %s:%u\n", exprString, fileString, (unsigned)lineNumber);
    fflush(stdout);
    // Return to the dcload host / reboot rather than corrupting execution.
    arch_exit();
}

void SYS_Alert(const char* message)
{
    printf("ALERT: %s\n", message);
    fflush(stdout);
}

void SYS_UpdateConsole() {}

int32_t SYS_GetPlatformTier()
{
    return 0; // Single Dreamcast tier.
}

// =========================================================================
// Window — all no-ops (fixed 640x480 screen).
// =========================================================================

void SYS_SetWindowTitle(const char* /*title*/) {}
void SYS_SetWindowIcon(const char* /*iconPath*/) {}
bool SYS_DoesWindowHaveFocus() { return true; }
void SYS_SetScreenOrientation(ScreenOrientation /*orientation*/) {}
ScreenOrientation SYS_GetScreenOrientation() { return ScreenOrientation::Landscape; }
void SYS_SetFullscreen(bool /*fullscreen*/) {}
bool SYS_IsFullscreen() { return true; }
void SYS_SetWindowRect(int32_t /*x*/, int32_t /*y*/, int32_t /*w*/, int32_t /*h*/) {}
void SYS_GetWindowRect(int32_t& outX, int32_t& outY, int32_t& outWidth, int32_t& outHeight)
{
    outX = 0;
    outY = 0;
    outWidth  = 640;
    outHeight = 480;
}
bool SYS_IsWindowMaximized() { return true; }
void SYS_MaximizeWindow() {}

#endif // POLYPHASE_PLATFORM_ADDON
