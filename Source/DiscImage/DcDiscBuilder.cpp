#include "DcDiscBuilder.h"

#include "DcIpBin.h"
#include "DcIso9660.h"
#include "disc_image.h"
#include "elf_parser.hpp"
#include "scramble.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <system_error>

namespace fs = std::filesystem;

namespace dcdisc {
namespace {

void Log(const LogFn& log, const std::string& msg) { if (log) log(msg); }

bool HasExt(const std::string& name, const char* ext) {
    std::string n = name, e = ext;
    std::transform(n.begin(), n.end(), n.begin(), ::tolower);
    return n.size() >= e.size() && n.compare(n.size() - e.size(), e.size(), e) == 0;
}

// Delete a stale build artifact. Clears the read-only attribute first (Windows
// refuses to delete or truncate read-only files) and reports a precise,
// actionable error otherwise — the usual cause is an emulator still holding the
// previous .cdi open.
bool ForceRemove(const fs::path& p, std::string* err) {
    std::error_code ec;
    if (!fs::exists(p, ec)) return true;

    fs::permissions(p, fs::perms::owner_write, fs::perm_options::add, ec);
    ec.clear();
    fs::remove(p, ec);
    if (!fs::exists(p)) return true;

    if (err) {
        *err = "cannot delete stale file '" + p.string() + "'";
        if (ec) *err += " (" + ec.message() + ")";
        *err += " — it is probably still open in an emulator (flycast) or another "
                "program. Close it and build again.";
    }
    return false;
}

// ELF -> flat binary -> scrambled 1ST_READ.BIN bytes.
bool MakeBootBinary(const std::string& elfPath, std::vector<uint8_t>& out, std::string* err) {
    auto parser = elfparser::Parser::Load(fs::path(elfPath));
    if (!parser) { if (err) *err = "failed to parse Dreamcast ELF: " + elfPath; return false; }

    std::vector<char> bin;
    if (!(*parser)->fill_bin(bin) || bin.empty()) {
        if (err) *err = "no loadable segments in ELF: " + elfPath;
        return false;
    }
    std::vector<char> scrambled = scramble(bin);
    out.assign(scrambled.begin(), scrambled.end());
    return true;
}

} // namespace

bool BuildSelfBootCdi(const DiscBuildParams& params, std::string* err, const LogFn& log) {
    const fs::path discRoot(params.discRootDir);
    const std::string elfLeaf = fs::path(params.elfPath).filename().string();
    const fs::path cdiPath(params.outputCdiPath);
    const fs::path bootPath = discRoot / "1ST_READ.BIN";

    // 0) Delete artifacts left by a previous build BEFORE doing any work. Two
    //    reasons: a leftover that is read-only or locked would otherwise fail
    //    deep in the build with a confusing error, and clearing the .cdi up
    //    front means a later failure can't leave a stale image behind that
    //    looks like a fresh one.
    {
        fs::path isoPath = cdiPath; isoPath.replace_extension(".iso");
        fs::path gdiPath = cdiPath; gdiPath.replace_extension(".gdi");
        const fs::path stale[] = { bootPath, discRoot / "IP.BIN", cdiPath, isoPath, gdiPath };
        for (const fs::path& p : stale)
            if (!ForceRemove(p, err)) return false;
        Log(log, "Dreamcast: cleared previous build artifacts");
    }

    // 1) ELF -> scrambled 1ST_READ.BIN, written into the disc root.
    std::vector<uint8_t> bootBin;
    if (!MakeBootBinary(params.elfPath, bootBin, err)) return false;
    Log(log, "Dreamcast: converted ELF -> scrambled 1ST_READ.BIN (" +
             std::to_string(bootBin.size()) + " bytes)");

    {
        std::ofstream f(bootPath, std::ios::binary | std::ios::trunc);
        if (!f) { if (err) *err = "cannot write " + bootPath.string(); return false; }
        f.write(reinterpret_cast<const char*>(bootBin.data()), std::streamsize(bootBin.size()));
        f.close();
        if (!f) { if (err) *err = "failed writing " + bootPath.string() + " (disk full or read-only?)"; return false; }
    }

    // 2) IP.BIN bootstrap.
    IpBinParams ipp;
    ipp.gameName = params.gameName;
    ipp.companyName = params.author;
    ipp.areaSymbols = AreaSymbolsForRegion(params.region);
    std::vector<uint8_t> ipBin = BuildIpBin(ipp, bootBin);
    Log(log, "Dreamcast: generated IP.BIN (area=" + AreaSymbolsForRegion(params.region) + ")");

    // 3) Session 0: blank 4-second audio track (MIL-CD self-boot requires a
    //    leading audio session). start_lba is where the data session begins.
    cd_image_t* img = cd_new_image();
    cd_image_set_volume_name(img, params.gameName.c_str());
    cd_session_t* session0 = cd_new_session(img);
    cd_new_track_blank(session0, TRACK_TYPE_AUDIO, 2352 * 302);
    const uint32_t startLba = uint32_t(cd_session_length_in_sectors(session0));

    // 4) ISO9660(+Joliet) data image with IP.BIN as the 16-sector system area.
    Iso9660Options iso;
    iso.volumeId = params.gameName;
    iso.startLba = startLba;
    iso.systemArea = ipBin;
    // Keep 1ST_READ.BIN (the boot binary must ship on the disc); drop the ELF,
    // a stray IP.BIN (it lives in the system area, not as a file), and any disc
    // artifacts left in the package dir from a previous build.
    iso.excludeName = [elfLeaf](const std::string& name) {
        if (name == "1ST_READ.BIN") return false;
        if (name == "IP.BIN") return true;
        if (name == elfLeaf) return true;
        return HasExt(name, ".elf") || HasExt(name, ".bin") || HasExt(name, ".iso") ||
               HasExt(name, ".cdi") || HasExt(name, ".gdi");
    };

    std::string isoErr;
    std::vector<uint8_t> isoData = BuildIso9660(discRoot, iso, &isoErr);
    if (isoData.empty()) {
        if (err) *err = "ISO9660 build failed: " + isoErr;
        cd_free_image(&img);
        return false;
    }
    Log(log, "Dreamcast: built ISO data track (" + std::to_string(isoData.size()) +
             " bytes, data LBA " + std::to_string(startLba) + ")");

    // 5) Session 1: the data track (XA Mode-2 Form-1) + 2-sector postgap.
    cd_session_t* session1 = cd_new_session(img);
    cd_track_t* dataTrack = cd_new_track(session1, TRACK_TYPE_DATA, isoData.data(), uint32_t(isoData.size()));
    cd_track_set_mode(dataTrack, TRACK_MODE_XA_MODE2_FORM1);
    cd_track_set_postgap_sectors(dataTrack, 2);

    // 6) Emit the DiscJuggler .cdi.
    std::error_code ec;
    fs::create_directories(fs::path(params.outputCdiPath).parent_path(), ec);
    FILE* out = std::fopen(params.outputCdiPath.c_str(), "wb");
    if (!out) {
        if (err) *err = "cannot open output .cdi for writing: " + params.outputCdiPath +
                        " — it is probably still open in an emulator (flycast) or another "
                        "program. Close it and build again.";
        cd_free_image(&img);
        return false;
    }
    const std::string cdiName = fs::path(params.outputCdiPath).filename().string();
    cd_write_to_cdi(img, out, cdiName.c_str()); // returns false unconditionally; validate by size
    std::fclose(out);
    cd_free_image(&img);

    std::error_code sizeEc;
    const auto sz = fs::file_size(params.outputCdiPath, sizeEc);
    if (sizeEc || sz == 0) {
        if (err) *err = "CDI write produced no output: " + params.outputCdiPath;
        return false;
    }
    Log(log, "Dreamcast: wrote self-boot CDI " + params.outputCdiPath + " (" +
             std::to_string(sz) + " bytes)");
    return true;
}

} // namespace dcdisc
