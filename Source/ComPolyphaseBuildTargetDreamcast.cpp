// MSVC's SDL deprecations on getenv/fopen/strncpy don't apply to this addon —
// every call is bounds-checked and pointer-validated. Suppress before the CRT
// headers come in. (The build systems also define this on the command line;
// guard to avoid a redefinition warning when they do.)
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif

/**
 * @file ComPolyphaseBuildTargetDreamcast.cpp
 * @brief Dreamcast (KallistiOS) build-target addon for Polyphase Engine.
 *
 * Adds a "Dreamcast (KallistiOS)" entry to the editor's Build Profile
 * dropdown and drives compile -> package (.cdi/.gdi via mkdcdisc) -> run
 * (flycast/lxdream/redream, or dc-tool-ip to real hardware).
 *
 * Toolchain routing (per-profile, `dreamcast.toolchain`):
 *
 *   "dreamsdk"  (default on Windows when DREAMSDK_HOME is set)
 *       Runs the build inside DreamSDK's MSYS2 environment via
 *       <DreamSDK>\opt\dreamsdk\dreamsdk-runner.exe. The runner sets up
 *       KOS_BASE (/opt/toolchains/dc/kos), the sh-elf toolchain, make and
 *       mkdcdisc, and maps Windows drives as /<drive>/... (M: -> /m/...).
 *       This is the path for a machine that installed DreamSDK — it needs
 *       NO Windows-level KOS_BASE.
 *
 *   "kos-native" (default on Linux/macOS)
 *       Runs `bash <script>` natively; the generated script sources
 *       "$KOS_BASE/environ.sh" first. Requires KOS_BASE + a working
 *       KallistiOS install on the host.
 *
 *   "kos-wsl"   (Windows, KallistiOS installed inside WSL)
 *       Runs `wsl [-d <distro>] bash -lc "sh <script>"`; the script sources
 *       "$KOS_BASE/environ.sh". Windows paths are translated to /mnt/<drive>/.
 *
 * Because DreamSDK's runner and WSL both dislike long compound one-liners,
 * GetCompileCommand writes a small `polyphase_build.sh` into
 * <projectDir>/Intermediate/Dreamcast/ and the emitted command just runs it.
 *
 * Licensing isolation: the engine binary never links KallistiOS / PVR /
 * sh-elf. Every Dreamcast-specific reference lives inside this addon DLL and
 * its Runtime/Dreamcast tree.
 *
 * Maintainer: Polyphase Engine team.
 */

#include "Plugins/PolyphasePluginAPI.h"
#include "Plugins/PolyphaseEngineAPI.h"

#if EDITOR
#include "Plugins/EditorUIHooks.h"
#include "Plugins/PolyphaseBuildTargetAPI.h"
#include "imgui.h"
#include "DiscImage/DcDiscBuilder.h"
#endif

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

static PolyphaseEngineAPI* sEngineAPI = nullptr;

#if EDITOR
namespace
{
    // ----- Env / path helpers ----------------------------------------------

    std::string GetEnvOrEmpty(const char* name)
    {
        const char* v = std::getenv(name);
        return v ? std::string(v) : std::string();
    }

    bool FileExists(const std::string& path)
    {
        if (path.empty()) return false;
        std::error_code ec;
        return std::filesystem::exists(path, ec) && !std::filesystem::is_directory(path, ec);
    }

    bool DirExists(const std::string& path)
    {
        if (path.empty()) return false;
        std::error_code ec;
        return std::filesystem::is_directory(path, ec);
    }

    // Translate a Windows absolute path to DreamSDK's MSYS2 mount form:
    //   M:\Foo\Bar  ->  /m/Foo/Bar
    // Already-POSIX paths pass through; relative paths just get separators
    // normalised. DreamSDK mounts each drive at /<lowercase-letter>/.
    std::string WinToMsysPath(const std::string& winPath)
    {
        if (winPath.empty()) return winPath;
        if (winPath[0] == '/') return winPath;
        if (winPath.size() >= 2 && winPath[1] == ':')
        {
            std::string out = "/";
            out += static_cast<char>(std::tolower(static_cast<unsigned char>(winPath[0])));
            for (size_t i = 2; i < winPath.size(); ++i)
                out += (winPath[i] == '\\') ? '/' : winPath[i];
            return out;
        }
        std::string out = winPath;
        for (char& c : out) if (c == '\\') c = '/';
        return out;
    }

    // Translate a Windows absolute path to its default WSL2 mount:
    //   M:\Foo\Bar  ->  /mnt/m/Foo/Bar
    std::string WinToWslPath(const std::string& winPath)
    {
        if (winPath.empty()) return winPath;
        if (winPath[0] == '/') return winPath;
        if (winPath.size() >= 2 && winPath[1] == ':')
        {
            std::string out = "/mnt/";
            out += static_cast<char>(std::tolower(static_cast<unsigned char>(winPath[0])));
            for (size_t i = 2; i < winPath.size(); ++i)
                out += (winPath[i] == '\\') ? '/' : winPath[i];
            return out;
        }
        std::string out = winPath;
        for (char& c : out) if (c == '\\') c = '/';
        return out;
    }

    // ----- Toolchain selection ---------------------------------------------

    enum class Toolchain { DreamSDK, KosNative, KosWsl };

    // Per-profile option keys.
    constexpr const char* kToolchainKey    = "dreamcast.toolchain";    // "dreamsdk" | "kos-native" | "kos-wsl"
    constexpr const char* kDreamSdkHomeKey = "dreamcast.dreamsdkHome"; // override %DREAMSDK_HOME%
    constexpr const char* kKosBaseKey      = "dreamcast.kosBase";      // override $KOS_BASE (kos routes)
    constexpr const char* kWslDistroKey    = "dreamcast.wslDistro";    // kos-wsl: `wsl -d <distro>`
    constexpr const char* kJobsKey         = "dreamcast.jobs";         // make -j parallelism
    constexpr const char* kRegionKey       = "dreamcast.region";       // "NTSC-U" | "NTSC-J" | "PAL"
    constexpr const char* kDiscFormatKey   = "dreamcast.discFormat";   // "cdi" | "gdi"
    constexpr const char* kMakefileKey     = "dreamcast.makefile";     // bare filename in addon root, or absolute override
    constexpr const char* kPackagerKey     = "dreamcast.packager";     // "native" | "external"

    constexpr const char* kRegionDefault     = "NTSC-U";
    constexpr const char* kDiscFormatDefault = "cdi";
    constexpr const char* kMakefileDefault   = "Makefile_Dreamcast";
    // "native" = the addon's built-in, GPL-free self-boot CDI writer (default;
    // no external tools). "external" = the classic mkdcdisc / DreamSDK pipeline.
    constexpr const char* kPackagerDefault   = "native";
    // KOS + sh-elf TUs are lighter than psp-gcc's, but the engine still has a
    // few 1 GB+ files; 4 is a safe laptop default. Workstations can bump it.
    constexpr const char* kJobsDefault       = "4";
    constexpr const char* kDreamSdkHomeFallback = "C:\\DreamSDK";

    std::string ReadOption(const PolyphaseBuildContext* ctx, const char* key, const char* fallback)
    {
        if (ctx == nullptr || ctx->GetProfileSetting == nullptr) return fallback ? fallback : "";
        char buf[512] = {0};
        if (ctx->GetProfileSetting(key, buf, sizeof(buf)) == 0 || buf[0] == '\0')
            return fallback ? fallback : "";
        return std::string(buf);
    }

    // Resolve the DreamSDK install root: profile override, then %DREAMSDK_HOME%,
    // then C:\DreamSDK.
    std::string ResolveDreamSdkHome(const PolyphaseBuildContext* ctx)
    {
        std::string home = ReadOption(ctx, kDreamSdkHomeKey, "");
        if (home.empty()) home = GetEnvOrEmpty("DREAMSDK_HOME");
        if (home.empty()) home = kDreamSdkHomeFallback;
        return home;
    }

    // Default toolchain when the profile hasn't set one: DreamSDK on Windows if
    // DREAMSDK_HOME is present, otherwise native KOS.
    const char* DefaultToolchainId(const PolyphaseBuildContext* ctx)
    {
#if defined(_WIN32)
        const std::string home = ResolveDreamSdkHome(ctx);
        if (DirExists(home)) return "dreamsdk";
        return "kos-native";
#else
        (void)ctx;
        return "kos-native";
#endif
    }

    Toolchain ResolveToolchain(const PolyphaseBuildContext* ctx)
    {
        std::string id = ReadOption(ctx, kToolchainKey, "");
        if (id.empty()) id = DefaultToolchainId(ctx);
        if (id == "dreamsdk")   return Toolchain::DreamSDK;
        if (id == "kos-wsl")    return Toolchain::KosWsl;
        return Toolchain::KosNative;
    }

    // Convert a host (Windows) path into the path namespace of the selected
    // build shell.
    std::string ToShellPath(Toolchain tc, const std::string& hostPath)
    {
#if defined(_WIN32)
        switch (tc)
        {
            case Toolchain::DreamSDK: return WinToMsysPath(hostPath);
            case Toolchain::KosWsl:   return WinToWslPath(hostPath);
            case Toolchain::KosNative:
            default:                  return hostPath; // native Windows path (rare on DC hosts)
        }
#else
        (void)tc;
        return hostPath;
#endif
    }

    std::string DreamSdkRunnerExe(const PolyphaseBuildContext* ctx)
    {
        return ResolveDreamSdkHome(ctx) + "\\opt\\dreamsdk\\dreamsdk-runner.exe";
    }

    int ReadJobs(const PolyphaseBuildContext* ctx)
    {
        const std::string jobsOpt = ReadOption(ctx, kJobsKey, kJobsDefault);
        int jobs = 0;
        for (char c : jobsOpt) { if (c < '0' || c > '9') { jobs = 0; break; } jobs = jobs * 10 + (c - '0'); }
        if (jobs < 1 || jobs > 64) jobs = 4;
        return jobs;
    }

    // Resolve the Makefile path (host form). Bare names live inside the addon;
    // absolute values are taken as-is.
    std::string ResolveMakefileHostPath(const PolyphaseBuildContext* ctx)
    {
        const std::string opt = ReadOption(ctx, kMakefileKey, kMakefileDefault);
        const bool isAbsolute =
            !opt.empty() && (opt[0] == '/' || (opt.size() >= 2 && opt[1] == ':'));
        if (isAbsolute) return opt;
        return std::string(ctx->projectDir) +
               "/Packages/com.polyphase.build.target.dreamcast/" + opt;
    }

    // Single-quote a path for a POSIX shell body, escaping embedded quotes.
    std::string Sq(const std::string& s)
    {
        std::string out = "'";
        for (char c : s) { if (c == '\'') out += "'\\''"; else out += c; }
        out += '\'';
        return out;
    }

    // ----- Validate (cached) -----------------------------------------------
    // Validate can be polled by the editor; a WSL/runner shell-out per frame
    // would be a serious hitch. Cache the last verdict keyed by the resolved
    // toolchain + the paths that feed it.

    int32_t sValidateCached = -1;             // -1 = unknown, 0/1 = last result
    std::string sValidateKey;
    char sValidateReason[512] = {0};

    int32_t Dreamcast_Validate(char* outReason, size_t cap)
    {
        // Validate has no ctx, so read the profile-independent env fallbacks
        // (the common case). Users who override paths via the profile still get
        // a correct build; Validate is advisory.
        const std::string dreamHome = []{
            std::string h = GetEnvOrEmpty("DREAMSDK_HOME");
            return h.empty() ? std::string(kDreamSdkHomeFallback) : h;
        }();
        const std::string kosBase = GetEnvOrEmpty("KOS_BASE");

#if defined(_WIN32)
        const bool preferDreamSdk = DirExists(dreamHome);
#else
        const bool preferDreamSdk = false;
#endif

        // Cache key — recompute only when the inputs change.
        std::string key = std::string(preferDreamSdk ? "D:" : "K:") + dreamHome + "|" + kosBase;
        if (sValidateCached != -1 && key == sValidateKey)
        {
            std::snprintf(outReason, cap, "%s", sValidateReason);
            return sValidateCached;
        }
        sValidateKey = key;

        auto finish = [&](int32_t ok, const char* reason) -> int32_t {
            std::snprintf(sValidateReason, sizeof(sValidateReason), "%s", reason ? reason : "");
            std::snprintf(outReason, cap, "%s", sValidateReason);
            sValidateCached = ok;
            return ok;
        };

        if (preferDreamSdk)
        {
            const std::string runner = dreamHome + "\\opt\\dreamsdk\\dreamsdk-runner.exe";
            const std::string kos    = dreamHome + "\\opt\\toolchains\\dc\\kos";
            if (!FileExists(runner))
            {
                char msg[512];
                std::snprintf(msg, sizeof(msg),
                    "DreamSDK found at '%s' but dreamsdk-runner.exe is missing "
                    "(expected '%s'). Reinstall DreamSDK or set the Target Options "
                    "'DreamSDK Home' field.", dreamHome.c_str(), runner.c_str());
                return finish(0, msg);
            }
            if (!DirExists(kos))
            {
                char msg[512];
                std::snprintf(msg, sizeof(msg),
                    "DreamSDK at '%s' has no KallistiOS at '%s'. Open DreamSDK "
                    "Manager and install/build KallistiOS.", dreamHome.c_str(), kos.c_str());
                return finish(0, msg);
            }
            return finish(1, "");
        }

        // Native / WSL KallistiOS: require KOS_BASE + its canonical linker
        // script. (WSL's $KOS_BASE isn't visible to this Windows process, so on
        // Windows without DreamSDK we can only give guidance.)
        if (kosBase.empty())
        {
#if defined(_WIN32)
            return finish(0,
                "No DreamSDK (DREAMSDK_HOME) and no KOS_BASE. Install DreamSDK "
                "(recommended on Windows) or set the toolchain to 'kos-wsl' with "
                "KallistiOS installed inside WSL.");
#else
            return finish(0,
                "KOS_BASE is not set. Install KallistiOS and source "
                "kos/environ.sh (or set KOS_BASE) before launching the editor.");
#endif
        }
        const std::string anchor = kosBase + "/utils/ldscripts/shlelf.xc";
        if (!FileExists(anchor))
        {
            char msg[512];
            std::snprintf(msg, sizeof(msg),
                "KOS_BASE='%s' but '%s' is missing — the install may be "
                "incomplete. Re-run KallistiOS' setup.", kosBase.c_str(), anchor.c_str());
            return finish(0, msg);
        }
        return finish(1, "");
    }

    // ----- Compile ----------------------------------------------------------

    // Write the generated build script into Intermediate/Dreamcast/ and return
    // its HOST path (empty on failure). Paths inside the script are in the
    // selected shell's namespace.
    std::string WriteBuildScript(const PolyphaseBuildContext* ctx, Toolchain tc)
    {
        const std::string projectDir = ctx->projectDir;
        const std::string intHost    = projectDir + "/Intermediate/Dreamcast";

        std::error_code ec;
        std::filesystem::create_directories(intHost, ec);
        if (ec) return "";

        const std::string scriptHost = intHost + "/polyphase_build.sh";

        const std::string intSh  = ToShellPath(tc, intHost);
        const std::string mkSh   = ToShellPath(tc, ResolveMakefileHostPath(ctx));
        const std::string projSh = ToShellPath(tc, projectDir);
        const std::string engSh  = (ctx->engineDir && ctx->engineDir[0])
                                        ? ToShellPath(tc, ctx->engineDir) : std::string();
        const int jobs = ReadJobs(ctx);

        // KOS env: DreamSDK's runner sets it up already. For native/WSL routes
        // we source environ.sh (optionally after exporting a profile-supplied
        // KOS_BASE override).
        const bool sourceKosEnv = (tc != Toolchain::DreamSDK);
        const std::string kosOverride = ReadOption(ctx, kKosBaseKey, "");

        std::ofstream out(scriptHost, std::ios::binary | std::ios::trunc);
        if (!out.is_open()) return "";
        out << "#!/bin/sh\n";
        out << "set -e\n";
        if (sourceKosEnv)
        {
            if (!kosOverride.empty())
                out << "export KOS_BASE=" << Sq(kosOverride) << "\n";
            out << ". \"$KOS_BASE/environ.sh\"\n";
        }
        out << "mkdir -p " << Sq(intSh) << "\n";
        out << "cd " << Sq(intSh) << "\n";
        if (ctx->forceRebuild)
            out << "rm -f ./*.o ./*.d ./*.elf 2>/dev/null || true\n";
        out << "make -f " << Sq(mkSh)
            << " PROJECT_ROOT=" << Sq(projSh);
        if (!engSh.empty())
            out << " POLYPHASE_PATH=" << Sq(engSh);
        out << " -j" << jobs << "\n";
        out.close();
        return out.good() ? scriptHost : std::string();
    }

    // Emit the outer command that runs the generated script under the selected
    // shell.
    std::string BuildRunScriptCommand(const PolyphaseBuildContext* ctx, Toolchain tc,
                                      const std::string& scriptHost)
    {
        const std::string scriptSh = ToShellPath(tc, scriptHost);
        switch (tc)
        {
            case Toolchain::DreamSDK:
            {
                // dreamsdk-runner.exe joins its args into a command run inside
                // the DreamSDK MSYS2 login environment. One quoted arg keeps it
                // a single simple command.
                //
                // The engine runs this via `cmd.exe /c <cmd>`. When a command
                // starts with a quoted path AND has more than one quote pair,
                // cmd.exe strips the OUTER quote pair, corrupting the command
                // (`"runner" "sh '..'"` -> `runner" "sh`). Wrap the whole thing
                // in an extra outer pair so cmd strips those and the real
                // quoting survives.
                return "\"\"" + DreamSdkRunnerExe(ctx) + "\" \"sh " + Sq(scriptSh) + "\"\"";
            }
            case Toolchain::KosWsl:
            {
                std::string distro = ReadOption(ctx, kWslDistroKey, "");
                std::string wsl = distro.empty() ? "wsl " : ("wsl -d " + distro + " ");
                return wsl + "bash -lc \"sh " + Sq(scriptSh) + "\"";
            }
            case Toolchain::KosNative:
            default:
                return "bash " + Sq(scriptSh);
        }
    }

    int32_t Dreamcast_GetCompileCommand(const PolyphaseBuildContext* ctx, char* outCmd, size_t cap)
    {
        if (ctx == nullptr || ctx->projectDir == nullptr) return 0;

        const Toolchain tc = ResolveToolchain(ctx);
        const std::string scriptHost = WriteBuildScript(ctx, tc);
        if (scriptHost.empty())
        {
            if (ctx->Log) ctx->Log(POLYPHASE_BT_LOG_ERROR,
                "Dreamcast: could not write Intermediate/Dreamcast/polyphase_build.sh.");
            return 0;
        }

        const std::string cmd = BuildRunScriptCommand(ctx, tc, scriptHost);
        std::snprintf(outCmd, cap, "%s", cmd.c_str());
        return 1;
    }

    int32_t Dreamcast_GetCompiledBinaryPath(const PolyphaseBuildContext* ctx, char* outPath, size_t cap)
    {
        if (ctx == nullptr || ctx->projectDir == nullptr || ctx->projectName == nullptr) return 0;
        // Makefile_Dreamcast stages the linked KOS ELF here; mkdcdisc wraps it
        // into .cdi/.gdi in PostPackage.
        std::snprintf(outPath, cap, "%s/Build/Dreamcast/%s.elf",
                      ctx->projectDir, ctx->projectName);
        return 1;
    }

    // ----- Package ----------------------------------------------------------

    // Force WindowWidth=640 / WindowHeight=480 (and UseAssetRegistry=1) in a
    // packaged Config.ini. The Dreamcast frame buffer is 640x480; the project's
    // Config.ini carries the desktop size the editor ran at, which
    // ReadEngineConfig would otherwise reload at boot and clobber the runtime's
    // OctPreInitialize default.
    void ForceDreamcastConfig(const std::string& configPath)
    {
        std::ifstream in(configPath);
        if (!in.is_open()) return;

        std::ostringstream out;
        std::string line;
        bool sawW = false, sawH = false, sawReg = false;
        while (std::getline(in, line))
        {
            if (line.rfind("WindowWidth=", 0) == 0)       { out << "WindowWidth=640\n";  sawW = true; }
            else if (line.rfind("WindowHeight=", 0) == 0) { out << "WindowHeight=480\n"; sawH = true; }
            else if (line.rfind("UseAssetRegistry=", 0) == 0) { out << "UseAssetRegistry=1\n"; sawReg = true; }
            else out << line << "\n";
        }
        in.close();

        if (!sawW)   out << "WindowWidth=640\n";
        if (!sawH)   out << "WindowHeight=480\n";
        if (!sawReg) out << "UseAssetRegistry=1\n";

        std::ofstream o(configPath, std::ios::trunc);
        if (o.is_open()) o << out.str();
    }

    // Map the profile region to makeip's area-symbol string (J/U/E).
    std::string AreaSymbols(const std::string& region)
    {
        if (region == "NTSC-U") return "U";
        if (region == "NTSC-J") return "J";
        if (region == "PAL")    return "E";
        return "JUE"; // all regions
    }

    // DreamSDK ships no mkdcdisc — it provides the classic toolchain
    // (sh-elf-objcopy -> scramble -> makeip -> mkisofs -> cdi4dc). Write a
    // package script that runs that pipeline and produces <name>.cdi in the
    // package dir. Returns the script's HOST path (empty on failure).
    std::string WritePackageScriptClassic(const PolyphaseBuildContext* ctx, Toolchain tc)
    {
        const std::string projectDir = ctx->projectDir ? ctx->projectDir : "";
        const std::string intHost    = projectDir + "/Intermediate/Dreamcast";
        std::error_code ec;
        std::filesystem::create_directories(intHost, ec);
        if (ec) return "";

        const std::string scriptHost = intHost + "/polyphase_package.sh";

        const std::string name   = ctx->projectName;
        const std::string title  = name;
        const std::string area   = AreaSymbols(ReadOption(ctx, kRegionKey, kRegionDefault));
        const std::string outSh  = ToShellPath(tc, std::string(ctx->packageOutputDir));
        const std::string intSh  = ToShellPath(tc, intHost);
        const std::string elfSh  = outSh + "/" + name + ".elf";
        // Persistent selfboot folder (sibling of packageOutputDir): the disc-root
        // staging (1ST_READ.BIN + cooked assets) copied out so an editor Build —
        // not just the manual script — leaves a folder ready to master into a
        // real-BIOS self-boot image with BootDreams.
        const std::string selfbootSh =
            ToShellPath(tc, (std::filesystem::path(ctx->packageOutputDir).parent_path()
                             / "selfboot").string());

        std::ofstream out(scriptHost, std::ios::binary | std::ios::trunc);
        if (!out.is_open()) return "";
        out << "#!/bin/sh\n";
        out << "set -e\n";
        out << "STAGE=" << Sq(intSh + "/pp_cdroot") << "\n";
        out << "rm -rf \"$STAGE\"\n";
        out << "mkdir -p \"$STAGE\"\n";
        // Copy the packaged assets into the disc-root staging dir, then drop the
        // build/gen artifacts so only the runtime files (Config.ini, <name>/,
        // Engine/, content) plus the scrambled boot binary ship on the disc.
        out << "cp -r " << Sq(outSh) << "/. \"$STAGE\"/\n";
        out << "rm -f \"$STAGE\"/*.elf \"$STAGE\"/*.bin \"$STAGE\"/*.iso "
               "\"$STAGE\"/*.cdi \"$STAGE\"/*.gdi \"$STAGE\"/IP.BIN 2>/dev/null || true\n";
        // ELF -> raw binary -> scrambled 1ST_READ.BIN (the DC boot binary).
        out << "sh-elf-objcopy -R .stack -O binary " << Sq(elfSh)
            << " " << Sq(intSh + "/pp.bin") << "\n";
        out << "scramble " << Sq(intSh + "/pp.bin") << " \"$STAGE/1ST_READ.BIN\"\n";
        // IP.BIN bootstrap (area symbols + title; default template/logo).
        out << "makeip -a " << area << " -g " << Sq(title)
            << " -f " << Sq(intSh + "/IP.BIN") << "\n";
        // MIL-CD self-boot (audio/data) layout: a retail Dreamcast BIOS only
        // self-boots CD-Rs via the audio-track trick — `mkisofs -C 0,11702`
        // reserves the leading audio track and `cdi4dc` WITHOUT -d builds the
        // Audio/Data image. (A plain Data/Data image boots under flycast's REIOS
        // HLE but a real BIOS drops to the CD-player screen.)
        out << "mkisofs -C 0,11702 -V " << Sq(name) << " -G " << Sq(intSh + "/IP.BIN")
            << " -joliet -rock -l -o " << Sq(intSh + "/pp.iso") << " \"$STAGE\"\n";
        out << "cdi4dc " << Sq(intSh + "/pp.iso") << " "
            << Sq(outSh + "/" + name + ".cdi") << "\n";
        // Also publish the disc-root staging as the persistent selfboot folder
        // (for BootDreams real-BIOS mastering) before cleaning up.
        out << "rm -rf " << Sq(selfbootSh) << "\n";
        out << "mkdir -p " << Sq(selfbootSh) << "\n";
        out << "cp -r \"$STAGE\"/. " << Sq(selfbootSh) << "/\n";
        out << "rm -rf \"$STAGE\" " << Sq(intSh + "/pp.bin") << " "
            << Sq(intSh + "/IP.BIN") << " " << Sq(intSh + "/pp.iso") << "\n";
        out.close();
        return out.good() ? scriptHost : std::string();
    }

    int32_t Dreamcast_PostPackage(const PolyphaseBuildContext* ctx)
    {
        if (ctx == nullptr || ctx->packageOutputDir == nullptr || ctx->projectName == nullptr) return 0;

        const std::string fmt    = ReadOption(ctx, kDiscFormatKey, kDiscFormatDefault);
        const std::string region = ReadOption(ctx, kRegionKey, kRegionDefault);
        const std::string outDir = ctx->packageOutputDir;
        const std::string name   = ctx->projectName;

        // Fixed 640x480 in both packaged Config.ini copies (root + <name>/).
        ForceDreamcastConfig(outDir + "/Config.ini");
        ForceDreamcastConfig(outDir + "/" + name + "/Config.ini");
        if (ctx->Log) ctx->Log(POLYPHASE_BT_LOG_DEBUG,
            "Dreamcast: patched WindowWidth/Height to 640/480 in packaged Config.ini");

        // Native packager (default): build the self-boot CDI entirely in-process
        // with the addon's built-in writer — no external tools, no DreamSDK/WSL,
        // no GPL. Produces the MIL-CD Audio/Data layout that boots on real
        // hardware. The "external" option falls back to the legacy pipelines
        // below. Compilation still runs under the selected toolchain regardless.
        if (ReadOption(ctx, kPackagerKey, kPackagerDefault) != "external")
        {
            dcdisc::DiscBuildParams p;
            p.elfPath       = outDir + "/" + name + ".elf";
            p.discRootDir   = outDir;
            p.outputCdiPath = outDir + "/" + name + ".cdi";
            p.gameName      = name;
            p.region        = region;

            auto logfn = [ctx](const std::string& m) {
                if (ctx->WriteOutputLine) ctx->WriteOutputLine(m.c_str());
            };
            if (ctx->WriteOutputLine)
                ctx->WriteOutputLine("Dreamcast: building self-boot CDI natively (no external tools)...");

            std::string dcErr;
            if (!dcdisc::BuildSelfBootCdi(p, &dcErr, logfn))
            {
                if (ctx->Log)
                    ctx->Log(POLYPHASE_BT_LOG_ERROR, ("Dreamcast native CDI build failed: " + dcErr).c_str());
                return 0;
            }
            if (ctx->Log)
            {
                char ok[512];
                std::snprintf(ok, sizeof(ok),
                    "Dreamcast package complete: %s/%s.cdi (native, region=%s)",
                    outDir.c_str(), name.c_str(), region.c_str());
                ctx->Log(POLYPHASE_BT_LOG_DEBUG, ok);
            }
            return 1;
        }

        const Toolchain tc = ResolveToolchain(ctx);
        std::string cmd;
        std::string producedFmt = fmt;

        if (tc == Toolchain::DreamSDK)
        {
            // DreamSDK has no mkdcdisc — run the classic pipeline via a script.
            // It always produces .cdi (cdi4dc's format).
            producedFmt = "cdi";
            const std::string scriptHost = WritePackageScriptClassic(ctx, tc);
            if (scriptHost.empty())
            {
                if (ctx->Log) ctx->Log(POLYPHASE_BT_LOG_ERROR,
                    "Dreamcast: could not write Intermediate/Dreamcast/polyphase_package.sh.");
                return 0;
            }
            const std::string scriptSh = ToShellPath(tc, scriptHost);
            // Extra outer quotes so cmd.exe /c keeps the real quoting (see the
            // GetCompileCommand DreamSDK note).
            cmd = "\"\"" + DreamSdkRunnerExe(ctx) + "\" \"sh " + Sq(scriptSh) + "\"\"";
        }
        else
        {
            // Native / WSL KallistiOS installs use mkdcdisc, which wraps the ELF
            // + asset dir directly and supports both .cdi and .gdi.
            const std::string elfSh    = ToShellPath(tc, outDir + "/" + name + ".elf");
            const std::string assetsSh = ToShellPath(tc, outDir);
            const std::string discSh   = ToShellPath(tc, outDir + "/" + name + "." + fmt);
            const std::string body =
                "mkdcdisc -e " + Sq(elfSh) + " -d " + Sq(assetsSh) + " -n " + Sq(name) +
                " -t " + Sq(region) + " -o " + Sq(discSh) + " -f " + fmt;
            if (tc == Toolchain::KosWsl)
            {
                std::string distro = ReadOption(ctx, kWslDistroKey, "");
                std::string wsl = distro.empty() ? "wsl " : ("wsl -d " + distro + " ");
                cmd = wsl + "bash -lc \"" + body + "\"";
            }
            else
            {
                cmd = "bash -lc \"" + body + "\"";
            }
        }

        if (ctx->WriteOutputLine) ctx->WriteOutputLine(cmd.c_str());
        const int rc = std::system(cmd.c_str());
        if (rc != 0)
        {
            if (ctx->Log)
            {
                char msg[256];
                std::snprintf(msg, sizeof(msg),
                    "Dreamcast disc build failed (rc=%d). DreamSDK route needs "
                    "scramble/makeip/mkisofs/cdi4dc; native/WSL needs mkdcdisc.", rc);
                ctx->Log(POLYPHASE_BT_LOG_ERROR, msg);
            }
            return 0;
        }

        if (ctx->Log)
        {
            char ok[512];
            std::snprintf(ok, sizeof(ok),
                "Dreamcast package complete: %s/%s.%s (region=%s)",
                outDir.c_str(), name.c_str(), producedFmt.c_str(), region.c_str());
            ctx->Log(POLYPHASE_BT_LOG_DEBUG, ok);
        }
        return 1;
    }

    // ----- Run --------------------------------------------------------------

    int32_t Dreamcast_RunInEmulator(const PolyphaseBuildContext* ctx, char* outCmd, size_t cap)
    {
        if (ctx == nullptr || ctx->packageOutputDir == nullptr || ctx->projectName == nullptr) return 0;

        std::string fmt = ReadOption(ctx, kDiscFormatKey, kDiscFormatDefault);
        // The native packager and the DreamSDK classic pipeline both always emit
        // .cdi, regardless of the profile's disc-format pick — match that here.
        if (ReadOption(ctx, kPackagerKey, kPackagerDefault) != "external") fmt = "cdi";
        else if (ResolveToolchain(ctx) == Toolchain::DreamSDK) fmt = "cdi";
        // flycast is the most widely-available cross-platform DC emulator;
        // override with DC_EMULATOR for lxdream/redream/etc.
        const std::string emu = GetEnvOrEmpty("DC_EMULATOR");
        const std::string exe = emu.empty() ? std::string("flycast") : emu;

        // Extra outer quotes: cmd.exe /c strips the outer pair when a command
        // starts with a quoted token and has more than one quote pair.
        std::snprintf(outCmd, cap, "\"\"%s\" \"%s/%s.%s\"\"",
                      exe.c_str(), ctx->packageOutputDir, ctx->projectName, fmt.c_str());
        return 1;
    }

    int32_t Dreamcast_RunOnDevice(const PolyphaseBuildContext* ctx, char* outCmd, size_t cap)
    {
        if (ctx == nullptr || ctx->packageOutputDir == nullptr || ctx->projectName == nullptr) return 0;

        const std::string host = GetEnvOrEmpty("DC_HOST");
        if (host.empty())
        {
            std::snprintf(outCmd, cap,
                "echo \"DC_HOST not set. Set DC_HOST=<dreamcast IP> and boot the console "
                "into dcload-ip before Run on Device.\" && exit 1");
            return 1;
        }

        // dc-tool-ip uploads and runs the raw ELF over the coder/BBA cable. It
        // lives in the toolchain env, so route it through the build shell.
        const Toolchain tc = ResolveToolchain(ctx);
        const std::string elfSh = ToShellPath(tc, std::string(ctx->packageOutputDir) + "/" + ctx->projectName + ".elf");
        const std::string body  = "dc-tool-ip -t " + host + " -x " + Sq(elfSh);

        switch (tc)
        {
            case Toolchain::DreamSDK:
                // Extra outer quotes (see GetCompileCommand's DreamSDK note).
                std::snprintf(outCmd, cap, "\"\"%s\" \"%s\"\"", DreamSdkRunnerExe(ctx).c_str(), body.c_str());
                break;
            case Toolchain::KosWsl:
            {
                std::string distro = ReadOption(ctx, kWslDistroKey, "");
                std::string wsl = distro.empty() ? "wsl " : ("wsl -d " + distro + " ");
                std::snprintf(outCmd, cap, "%sbash -lc \"%s\"", wsl.c_str(), body.c_str());
                break;
            }
            case Toolchain::KosNative:
            default:
                std::snprintf(outCmd, cap, "bash -lc \"%s\"", body.c_str());
                break;
        }
        return 1;
    }

    // ----- Editor profile UI ------------------------------------------------

    void Dreamcast_DrawProfileOptions(const PolyphaseBuildContext* ctx)
    {
        if (ctx == nullptr || ctx->SetProfileSetting == nullptr) return;

        // ----- Toolchain ---------------------------------------------------
        static const char* kToolchains[]      = { "dreamsdk", "kos-native", "kos-wsl" };
        static const char* kToolchainLabels[] = { "DreamSDK (Windows)", "KallistiOS (native)", "KallistiOS (WSL)" };
        std::string tcId = ReadOption(ctx, kToolchainKey, DefaultToolchainId(ctx));
        int tcIdx = 0;
        for (int i = 0; i < 3; ++i) if (tcId == kToolchains[i]) { tcIdx = i; break; }
        if (ImGui::Combo("Toolchain", &tcIdx, kToolchainLabels, IM_ARRAYSIZE(kToolchainLabels)))
            ctx->SetProfileSetting(kToolchainKey, kToolchains[tcIdx]);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(
                "How the build runs:\n"
                "  DreamSDK  - Windows MSYS2 env (dreamsdk-runner.exe). KallistiOS bundled.\n"
                "  KallistiOS native - bash + $KOS_BASE/environ.sh (Linux/macOS).\n"
                "  KallistiOS WSL    - wsl bash, KallistiOS installed inside WSL.");

        const std::string curTc = kToolchains[tcIdx];

        // ----- Disc packager -----------------------------------------------
        static const char* kPackagers[]      = { "native", "external" };
        static const char* kPackagerLabels[] = { "Native (built-in, no external tools)",
                                                 "External (mkdcdisc / DreamSDK)" };
        std::string pkgId = ReadOption(ctx, kPackagerKey, kPackagerDefault);
        int pkgIdx = (pkgId == "external") ? 1 : 0;
        if (ImGui::Combo("Disc Packager", &pkgIdx, kPackagerLabels, IM_ARRAYSIZE(kPackagerLabels)))
            ctx->SetProfileSetting(kPackagerKey, kPackagers[pkgIdx]);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(
                "How the .cdi disc image is built after compilation:\n"
                "  Native   - the addon's built-in self-boot CDI writer. No external\n"
                "             tools, no GPL. Produces a MIL-CD Audio/Data image that\n"
                "             self-boots on real hardware (fixes the CD-player fallback).\n"
                "  External - the classic mkdcdisc / DreamSDK cdi4dc pipeline (needs\n"
                "             those tools on the build shell; also supports .gdi).");

        // ----- DreamSDK Home (dreamsdk route) ------------------------------
        if (curTc == "dreamsdk")
        {
            std::string cur = ReadOption(ctx, kDreamSdkHomeKey, "");
            char buf[256] = {0};
            std::strncpy(buf, cur.c_str(), sizeof(buf) - 1);
            if (ImGui::InputText("DreamSDK Home", buf, sizeof(buf)))
                ctx->SetProfileSetting(kDreamSdkHomeKey, buf);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Windows path to the DreamSDK install (contains opt\\dreamsdk\\dreamsdk-runner.exe).\n"
                                  "Leave empty to use %%DREAMSDK_HOME%% or C:\\DreamSDK.");
        }

        // ----- KOS_BASE + WSL distro (kos routes) --------------------------
        if (curTc == "kos-native" || curTc == "kos-wsl")
        {
            std::string cur = ReadOption(ctx, kKosBaseKey, "");
            char buf[256] = {0};
            std::strncpy(buf, cur.c_str(), sizeof(buf) - 1);
            if (ImGui::InputText("KOS_BASE override", buf, sizeof(buf)))
                ctx->SetProfileSetting(kKosBaseKey, buf);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Path to KallistiOS AS SEEN BY THE BUILD SHELL (e.g. /opt/toolchains/dc/kos).\n"
                                  "Leave empty to use $KOS_BASE from the shell environment.");
        }
        if (curTc == "kos-wsl")
        {
            std::string cur = ReadOption(ctx, kWslDistroKey, "");
            char buf[64] = {0};
            std::strncpy(buf, cur.c_str(), sizeof(buf) - 1);
            if (ImGui::InputText("WSL Distro", buf, sizeof(buf)))
                ctx->SetProfileSetting(kWslDistroKey, buf);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Pass to `wsl -d <name>`. Leave empty for the default distro.");
        }

        // ----- Region ------------------------------------------------------
        static const char* kRegions[] = { "NTSC-U", "NTSC-J", "PAL" };
        std::string region = ReadOption(ctx, kRegionKey, kRegionDefault);
        int regionIdx = 0;
        for (int i = 0; i < 3; ++i) if (region == kRegions[i]) { regionIdx = i; break; }
        if (ImGui::Combo("Region", &regionIdx, kRegions, IM_ARRAYSIZE(kRegions)))
            ctx->SetProfileSetting(kRegionKey, kRegions[regionIdx]);

        // ----- Disc format -------------------------------------------------
        static const char* kFormats[] = { "cdi", "gdi" };
        std::string format = ReadOption(ctx, kDiscFormatKey, kDiscFormatDefault);
        int fmtIdx = 0;
        for (int i = 0; i < 2; ++i) if (format == kFormats[i]) { fmtIdx = i; break; }
        if (ImGui::Combo("Disc Format", &fmtIdx, kFormats, IM_ARRAYSIZE(kFormats)))
            ctx->SetProfileSetting(kDiscFormatKey, kFormats[fmtIdx]);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(".cdi for emulators / burning; .gdi for GD-ROM-style dumps.");

        // ----- Parallel jobs ----------------------------------------------
        {
            std::string cur = ReadOption(ctx, kJobsKey, kJobsDefault);
            int jobs = std::atoi(cur.c_str());
            if (jobs < 1 || jobs > 64) jobs = 4;
            if (ImGui::SliderInt("Parallel Jobs", &jobs, 1, 32))
            {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "%d", jobs);
                ctx->SetProfileSetting(kJobsKey, buf);
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("`make -j<N>`. The engine's larger TUs peak around 1 GB each; 4 is a safe laptop default.");
        }

        // ----- Makefile ----------------------------------------------------
        {
            std::string cur = ReadOption(ctx, kMakefileKey, kMakefileDefault);
            char buf[256] = {0};
            std::strncpy(buf, cur.c_str(), sizeof(buf) - 1);
            if (ImGui::InputText("Makefile", buf, sizeof(buf)))
                ctx->SetProfileSetting(kMakefileKey, buf);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("KOS build makefile. Bare name resolves inside the addon (default: Makefile_Dreamcast); absolute paths point at a fork.");
        }

        ImGui::Spacing();
        ImGui::TextDisabled("Disc image built by the native writer by default (no external tools); emulator defaults to flycast (override DC_EMULATOR).");
        ImGui::TextDisabled("Set DC_HOST=<ip> to enable 'Run on Device' via dc-tool-ip.");
    }

    // Canonical descriptor. Strings are deep-copied by the registry; this
    // static instance just needs to outlive the RegisterBuildTarget call.
    static PolyphaseBuildTargetDesc gDreamcastTarget{};
}
#endif // EDITOR

// ----- Plugin lifecycle -----------------------------------------------------

static int OnLoad(PolyphaseEngineAPI* api)
{
    sEngineAPI = api;
    if (api) api->LogDebug("com.polyphase.build.target.dreamcast loaded.");
    return 0;
}

static void OnUnload()
{
    if (sEngineAPI) sEngineAPI->LogDebug("com.polyphase.build.target.dreamcast unloaded.");
    sEngineAPI = nullptr;
}

static void RegisterTypes(void* /*nodeFactory*/) {}
static void RegisterScriptFuncs(lua_State* L) { (void)L; }

#if EDITOR
static void RegisterEditorUI(EditorUIHooks* hooks, uint64_t hookId)
{
    if (hooks == nullptr) return;

    if (hooks->RegisterBuildTarget == nullptr)
    {
        if (sEngineAPI)
        {
            sEngineAPI->LogWarning("com.polyphase.build.target.dreamcast: this engine "
                                   "build predates the build-target API (need plugin "
                                   "apiVersion >= 4). Target not registered.");
        }
        return;
    }

    gDreamcastTarget = {};
    gDreamcastTarget.apiVersion            = POLYPHASE_BUILD_TARGET_API_VERSION;
    gDreamcastTarget.targetId              = "homebrew.dreamcast";
    gDreamcastTarget.displayName           = "Dreamcast (KallistiOS)";
    gDreamcastTarget.iconText              = "";
    gDreamcastTarget.category              = "Retro Consoles";
    gDreamcastTarget.basePlatform          = 1; /* Platform::Linux — Unix-like cook + ELF */
    gDreamcastTarget.binaryExtension       = ".cdi";
    gDreamcastTarget.requiresDocker        = 0;
    gDreamcastTarget.supportsRunOnDevice   = 1;
    gDreamcastTarget.supportsEmulator      = 1;
    gDreamcastTarget.Validate              = &Dreamcast_Validate;
    gDreamcastTarget.PreCook               = nullptr;
    gDreamcastTarget.CookAsset             = nullptr; // Linux cook is acceptable for V1 (PVR2 twiddle is a later phase)
    gDreamcastTarget.GetCompileCommand     = &Dreamcast_GetCompileCommand;
    gDreamcastTarget.GetCompiledBinaryPath = &Dreamcast_GetCompiledBinaryPath;
    gDreamcastTarget.PostPackage           = &Dreamcast_PostPackage;
    gDreamcastTarget.RunOnDevice           = &Dreamcast_RunOnDevice;
    gDreamcastTarget.RunInEmulator         = &Dreamcast_RunInEmulator;
    gDreamcastTarget.DrawProfileOptions    = &Dreamcast_DrawProfileOptions;
    gDreamcastTarget.SerializeProfileOptions   = nullptr;
    gDreamcastTarget.DeserializeProfileOptions = nullptr;

    // Variant 2: ship an engine runtime. ActionManager writes
    // Generated/PolyphasePlatform_*.h bridges that #include the addon's
    // *_Platform.h headers, and Makefile_Dreamcast sets -DPOLYPHASE_PLATFORM_ADDON=1
    // + -I<Generated/> so the engine's fork headers pick up the DC typedefs.
    gDreamcastTarget.platformExtensionDir = "Runtime/Dreamcast";

    hooks->RegisterBuildTarget(hookId, &gDreamcastTarget);
}
#endif

extern "C" OCTAVE_PLUGIN_API int PolyphasePlugin_GetDesc(PolyphasePluginDesc* desc)
{
    if (desc == nullptr) return 1;
    desc->apiVersion          = OCTAVE_PLUGIN_API_VERSION;
    desc->pluginName          = "com.polyphase.build.target.dreamcast";
    desc->pluginVersion       = "1.0.0";
    desc->OnLoad              = OnLoad;
    desc->OnUnload            = OnUnload;
    desc->Tick                = nullptr;
    desc->TickEditor          = nullptr;
    desc->RegisterTypes       = RegisterTypes;
    desc->RegisterScriptFuncs = RegisterScriptFuncs;
#if EDITOR
    desc->RegisterEditorUI    = RegisterEditorUI;
#else
    desc->RegisterEditorUI    = nullptr;
#endif
    desc->OnEditorPreInit     = nullptr;
    desc->OnEditorReady       = nullptr;
    return 0;
}
