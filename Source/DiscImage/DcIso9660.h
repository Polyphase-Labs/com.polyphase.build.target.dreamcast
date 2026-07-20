// DcIso9660 — a minimal, GPL-free ISO9660 (+ Joliet) image writer.
//
// This replaces mkdcdisc's use of libisofs (GPL v2+). It produces exactly what
// the Dreamcast disc pipeline needs: a 2048-byte/sector image whose first 16
// sectors are a caller-supplied "system area" (the 32 KB IP.BIN), with every
// extent/path-table location stored as an ABSOLUTE LBA (startLba + relative) so
// the resulting data track mounts when written at the MIL-CD data offset
// (~LBA 11702). The returned bytes are the data-track payload that gets wrapped
// into XA Mode-2 Form-1 sectors by the CDI writer.
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace dcdisc {

struct Iso9660Options
{
    std::string volumeId;             // volume label (mangled to d-chars / UCS-2)
    uint32_t startLba = 0;            // ms_block: absolute LBA where sector 0 will live
    std::vector<uint8_t> systemArea; // 32768 bytes written to sectors 0..15 (IP.BIN)
    // Optional: return true to exclude a directory entry by its base name
    // (used to keep the .elf and disc artifacts out of the filesystem).
    std::function<bool(const std::string&)> excludeName;
};

// Build an ISO9660+Joliet image from the directory tree at rootDir.
// Returns the full image bytes (a multiple of 2048). On failure returns an empty
// vector and, if err != nullptr, sets *err.
std::vector<uint8_t> BuildIso9660(const std::filesystem::path& rootDir,
                                  const Iso9660Options& opts,
                                  std::string* err);

} // namespace dcdisc
