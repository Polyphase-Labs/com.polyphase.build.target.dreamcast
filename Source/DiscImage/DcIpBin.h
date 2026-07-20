// DcIpBin — build a Dreamcast IP.BIN bootstrap header from the embedded
// template, patching the metadata fields for this game. This is a C++ port of
// mkdcdisc's generate_ip_bin (MIT), extended to recompute the device-info CRC
// when the product number changes (mkdcdisc left the template value in place).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace dcdisc {

struct IpBinParams
{
    std::string gameName;       // -> game_name   (128 bytes, space padded)
    std::string companyName;    // -> company_name (16 bytes)
    std::string releaseDate;    // "YYYYMMDD"; empty = today
    std::string productNumber;  // empty = derived from the boot binary (like mkdcdisc)
    std::string areaSymbols;    // 8-char field; empty = keep template default ("JUE     ")
    bool applyDefaultMrLogo = true;
};

// Map an addon region string to an 8-char IP.BIN area-symbol field.
//   "NTSC-J" -> "J       ", "NTSC-U" -> " U      ", "PAL" -> "  E     ",
//   anything else (incl. "all") -> "JUE     ".
std::string AreaSymbolsForRegion(const std::string& region);

// Build a 32768-byte IP.BIN. bootBin (the scrambled 1ST_READ.BIN) is used only
// to derive a product number when params.productNumber is empty.
std::vector<uint8_t> BuildIpBin(const IpBinParams& params, const std::vector<uint8_t>& bootBin);

} // namespace dcdisc
