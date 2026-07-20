// DcDiscBuilder — build a self-booting Dreamcast DiscJuggler (.cdi) image
// entirely in-process, with no external executables and no GPL code.
//
// This is a port of mkdcdisc's build_cdi()/gather_files() (MIT) that swaps its
// GPL libisofs step for the bundled DcIso9660 writer. It produces the MIL-CD
// Audio/Data two-session layout (blank audio session, then an XA-Mode2-Form1
// data session at LBA ~11702) that a retail Dreamcast BIOS self-boots.
#pragma once

#include <functional>
#include <string>

namespace dcdisc {

struct DiscBuildParams
{
    std::string elfPath;         // compiled Dreamcast ELF (input)
    std::string discRootDir;     // staged disc contents (assets); 1ST_READ.BIN is written here
    std::string outputCdiPath;   // .cdi to produce
    std::string gameName;        // IP.BIN game name + volume label
    std::string author;          // IP.BIN company name
    std::string region;          // "NTSC-U" / "NTSC-J" / "PAL" / "" (all)
};

using LogFn = std::function<void(const std::string&)>;

// Returns true on success. On failure returns false and sets *err (if non-null).
bool BuildSelfBootCdi(const DiscBuildParams& params, std::string* err, const LogFn& log = {});

} // namespace dcdisc
