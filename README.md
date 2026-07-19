# Polyphase Dreamcast Build Target

Adds a **Dreamcast (KallistiOS)** entry to the Polyphase editor's Build Profile
dropdown (under *Retro Consoles*). It compiles a project — engine + this addon's
`Runtime/Dreamcast` port — into a KallistiOS `.elf`, wraps it into a `.cdi`/`.gdi`
with `mkdcdisc`, and launches it in an emulator (flycast/lxdream/redream) or on
real hardware via `dc-tool-ip`.

This is an **editor addon**: the editor never links KallistiOS. Every SDK
reference lives in this addon DLL and its `Runtime/Dreamcast` tree.

---

## Toolchain routing (the key input)

Pick the toolchain in **Build Profiles → Target Options → Toolchain**:

| Option | When | How it runs |
| --- | --- | --- |
| **DreamSDK (Windows)** *(default on Windows)* | You installed [DreamSDK](https://www.dreamsdk.org/) | Build runs inside DreamSDK's MSYS2 env via `…\opt\dreamsdk\dreamsdk-runner.exe`. KallistiOS is bundled (`/opt/toolchains/dc/kos`); no `KOS_BASE` needed on the Windows side. |
| **KallistiOS (native)** *(default on Linux/macOS)* | KallistiOS installed on the host | Runs `bash`; the generated script sources `$KOS_BASE/environ.sh`. |
| **KallistiOS (WSL)** | KallistiOS installed inside WSL on Windows | Runs `wsl bash`; sources `$KOS_BASE/environ.sh`. Optional distro override. |

Additional Target Options: **DreamSDK Home** / **KOS_BASE override**, **WSL
Distro**, **Region** (NTSC-U/NTSC-J/PAL), **Disc Format** (cdi/gdi), **Parallel
Jobs**, **Makefile**.

Environment escape hatches: `DC_EMULATOR` (override flycast), `DC_HOST` (console
IP for *Run on Device*).

### How the build is dispatched

`GetCompileCommand` writes `<projectDir>/Intermediate/Dreamcast/polyphase_build.sh`
(mkdir + optional clean + `make -f Makefile_Dreamcast …`) and emits a single
command to run it under the selected shell. DreamSDK's runner and WSL both
dislike long compound one-liners, so a script keeps the invocation simple and
robust. Paths are translated to the shell's namespace (`M:\…` → `/m/…` for
DreamSDK, `/mnt/m/…` for WSL).

---

## Pipeline

```
cook (Linux base cook)  →  GetCompileCommand → make -f Makefile_Dreamcast
   → Build/Dreamcast/<name>.elf  →  copied to Packaged/homebrew.dreamcast/
   → PostPackage: mkdcdisc → <name>.cdi   (+ Config.ini rewritten to 640x480)
   → Run: flycast <name>.cdi   |   dc-tool-ip -x <name>.elf
```

`Makefile_Dreamcast` builds out-of-tree from `Intermediate/Dreamcast/`, compiles
the engine tree + `Runtime/Dreamcast` with `kos-c++`, and stages the ELF to
`Build/Dreamcast/`. Only `Generated/EmbeddedScripts.cpp` is linked in (not
`EmbeddedAssets.cpp`) — the Dreamcast's 16 MB RAM can't hold embedded assets, so
they load from the disc (`/cd/`) at runtime.

---

## Hardware notes

| | |
| --- | --- |
| CPU | Hitachi SH-4 @ 200 MHz |
| RAM | 16 MB main / 8 MB VRAM / 2 MB audio |
| GPU | PowerVR2 (CLX2) tile-based deferred renderer |
| Display | 640×480 (VGA / RGB / composite) |
| Input | Maple bus (controllers, VMU, etc.) |
| Audio | AICA (ARM7 sound processor) |
| Storage | GD-ROM (~1 GB); homebrew boots from CD-R / disc images |

---

## Runtime port status (phased, like the PSP port)

The `Runtime/Dreamcast` tree is a **Variant-2** engine runtime. Phase 1 is a
skeleton that compiles and boots to a cleared screen; later phases fill in the
PowerVR2 renderer, UI, scripting, and input.

| Phase | Scope | Status |
| --- | --- | --- |
| **1** | Build target + toolchain routing; engine compiles + boots (`Graphics_PVR2` clears the screen; input/audio/scripts stubbed) | **in progress** |
| 2 | PowerVR2 3D rendering (mesh/texture upload, matrices, DMA/store-queue cache coherency) | todo |
| 3 | UI widgets (2D path; bake UV/position/rotation into vertices) | todo |
| 4 | Lua scripts ticking; asset registry with baked UUIDs | todo |
| 5 | Maple input (analog deadzone, +Y=up), VMU saves, filesystem polish, optional PVR2 twiddled-texture `CookAsset` | todo |

### Runtime files

| File | Role |
| --- | --- |
| `SystemTypes_Platform.h` / `InputTypes_Platform.h` / `AudioTypes_Platform.h` / `NetworkTypes_Platform.h` | Variant-2 type injection (threads/mutex/DirEntry/SystemState). |
| `System_DC.cpp` | `SYS_*` via KOS newlib (stdio, dirent, threads, mutexes, timer). Real. |
| `Main_DC.cpp` | KOS entry (`KOS_INIT_FLAGS`, `main`), Oct* hooks, 640×480 override, embedded-scripts wiring. |
| `Graphics_PVR2/Graphics_PVR2.cpp` | PowerVR2 `GFX_*` backend. Phase-1: init/clear + no-op resource/draw stubs. |
| `Input_DC.cpp` / `Audio_DC.cpp` / `Network_DC.cpp` | `INP_*` / `AUD_*` / `NET_*`. Phase-1 stubs. |

> **Compiling the runtime:** the runtime skeleton is written against KOS APIs but
> is validated by building it through the toolchain above against a real project
> (it needs the full engine source + the project's `Generated/` files). Expect to
> iterate on KOS API/signature mismatches the first time it compiles end-to-end —
> that's the Phase-1 shakeout.

---

## Requirements

- **Windows:** DreamSDK (recommended) — provides KallistiOS, `kos-c++`,
  `mkdcdisc`, `dc-tool-ip`, and the `dreamsdk-runner.exe` used to dispatch builds.
- **Linux/macOS:** a KallistiOS install with `$KOS_BASE` set + `mkdcdisc` on PATH.
- An emulator for *Run in Emulator*: flycast (default), lxdream, or redream.
- For *Run on Device*: a Dreamcast running dcload-ip and `DC_HOST=<ip>`.
