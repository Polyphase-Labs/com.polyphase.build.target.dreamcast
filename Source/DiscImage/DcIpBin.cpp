#include "DcIpBin.h"

#include <cstdio>
#include <cstring>
#include <ctime>
#include <functional>

namespace {
// Embedded templates (mkdcdisc, MIT). Kept TU-local via the anonymous namespace.
#include "IpBinTemplate.inc"  // unsigned char IP_BIN[]; unsigned int IP_BIN_len (=32768)
#include "DefaultMr.inc"      // unsigned char default_mr[]; unsigned int default_mr_len

// IP.BIN metadata field offsets (the meta block starts at 0).
constexpr size_t kIpBinSize        = 32768;
constexpr size_t kOffDeviceInfo    = 0x20; // "XXXX GD-ROM1/1  " — first 4 chars are the CRC
constexpr size_t kOffAreaSymbols   = 0x30; // 8 bytes
constexpr size_t kOffProductNumber = 0x40; // 10 bytes
constexpr size_t kOffProductVer    = 0x4A; // 6 bytes
constexpr size_t kOffReleaseDate   = 0x50; // 16 bytes
constexpr size_t kOffCompanyName   = 0x70; // 16 bytes
constexpr size_t kOffGameName      = 0x80; // 128 bytes
constexpr size_t kOffMrLogo        = 0x3820;

// Write a fixed-width field: fill with spaces, then copy up to len bytes of src.
// These IP.BIN fields are space-padded and NOT null-terminated.
void WriteField(uint8_t* base, size_t off, size_t len, const std::string& src)
{
    std::memset(base + off, ' ', len);
    const size_t n = src.size() < len ? src.size() : len;
    std::memcpy(base + off, src.data(), n);
}

// Dreamcast IP.BIN header CRC (Marcus Comstedt's makeip algorithm), computed over
// the 16 bytes of product number + version at 0x40, written as 4 hex digits at 0x20.
uint16_t IpBinCrc(const uint8_t* data, size_t size)
{
    uint32_t n = 0xFFFF;
    for (size_t i = 0; i < size; ++i)
    {
        n ^= (uint32_t(data[i]) << 8);
        for (int c = 0; c < 8; ++c)
            n = (n & 0x8000) ? ((n << 1) ^ 4129) : (n << 1);
    }
    return uint16_t(n & 0xFFFF);
}
} // namespace

namespace dcdisc {

std::string AreaSymbolsForRegion(const std::string& region)
{
    if (region == "NTSC-J") return "J       ";
    if (region == "NTSC-U") return " U      ";
    if (region == "PAL")    return "  E     ";
    return "JUE     "; // all regions
}

static std::string GenerateProductNumber(const std::vector<uint8_t>& bootBin)
{
    // Mirrors mkdcdisc: "IND-" + first 6 digits of a hash of the boot binary.
    const std::string raw(bootBin.begin(), bootBin.end());
    const size_t h = std::hash<std::string>()(raw);
    std::string s = "IND-" + std::to_string(h);
    return s.substr(0, 10);
}

std::vector<uint8_t> BuildIpBin(const IpBinParams& params, const std::vector<uint8_t>& bootBin)
{
    std::vector<uint8_t> ip(IP_BIN, IP_BIN + kIpBinSize);
    uint8_t* base = ip.data();

    if (params.applyDefaultMrLogo)
    {
        size_t n = default_mr_len;
        if (kOffMrLogo + n > kIpBinSize) n = kIpBinSize - kOffMrLogo;
        std::memcpy(base + kOffMrLogo, default_mr, n);
    }

    WriteField(base, kOffGameName, 128, params.gameName.empty() ? "Untitled Game" : params.gameName);
    WriteField(base, kOffCompanyName, 16, params.companyName.empty() ? "Unknown Author" : params.companyName);

    std::string release = params.releaseDate;
    if (release.empty())
    {
        char buf[9];
        std::time_t t = std::time(nullptr);
        std::tm tmv{};
#if defined(_WIN32)
        localtime_s(&tmv, &t);
#else
        tmv = *std::localtime(&t);
#endif
        std::strftime(buf, sizeof(buf), "%Y%m%d", &tmv);
        release = buf;
    }
    WriteField(base, kOffReleaseDate, 16, release);

    const std::string product = params.productNumber.empty()
        ? GenerateProductNumber(bootBin) : params.productNumber;
    WriteField(base, kOffProductNumber, 10, product);

    if (!params.areaSymbols.empty())
        WriteField(base, kOffAreaSymbols, 8, params.areaSymbols);

    // Recompute the device-info CRC over product number + version (0x40..0x4F)
    // and overwrite the leading 4 hex chars of the device-info field at 0x20.
    const uint16_t crc = IpBinCrc(base + kOffProductNumber, 16);
    char hex[5];
    std::snprintf(hex, sizeof(hex), "%04X", crc);
    std::memcpy(base + kOffDeviceInfo, hex, 4);

    return ip;
}

} // namespace dcdisc
