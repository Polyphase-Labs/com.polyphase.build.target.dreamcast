#include "DcIso9660.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <system_error>

namespace fs = std::filesystem;

namespace dcdisc {
namespace {

constexpr uint32_t kSectorSize    = 2048;
constexpr uint32_t kSystemSectors = 16;    // sectors 0..15 hold IP.BIN
constexpr uint32_t kPvdSector     = 16;
constexpr uint32_t kSvdSector     = 17;    // Joliet
constexpr uint32_t kTerminator    = 18;
constexpr uint32_t kFirstFree     = 19;

inline uint32_t RoundUpSectors(uint64_t bytes) {
    return uint32_t((bytes + kSectorSize - 1) / kSectorSize);
}

// ---- little/big-endian field writers -----------------------------------
void Put16LE(uint8_t* p, uint16_t v) { p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF; }
void Put16BE(uint8_t* p, uint16_t v) { p[0] = (v >> 8) & 0xFF; p[1] = v & 0xFF; }
void Put32LE(uint8_t* p, uint32_t v) { p[0]=v&0xFF; p[1]=(v>>8)&0xFF; p[2]=(v>>16)&0xFF; p[3]=(v>>24)&0xFF; }
void Put32BE(uint8_t* p, uint32_t v) { p[0]=(v>>24)&0xFF; p[1]=(v>>16)&0xFF; p[2]=(v>>8)&0xFF; p[3]=v&0xFF; }
void Put16Both(uint8_t* p, uint16_t v) { Put16LE(p, v); Put16BE(p + 2, v); }
void Put32Both(uint8_t* p, uint32_t v) { Put32LE(p, v); Put32BE(p + 4, v); }

// A directory record's 7-byte recording timestamp. A fixed valid date is enough
// for booting; month/day of 0 would be invalid, so use 2025-01-01.
void PutDirDate(uint8_t* p) {
    p[0] = 125; p[1] = 1; p[2] = 1; p[3] = 0; p[4] = 0; p[5] = 0; p[6] = 0;
}

// ---- name mangling ------------------------------------------------------
bool IsDChar(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

std::string MangleComponent(const std::string& in, bool allowDot) {
    std::string out;
    for (char c : in) {
        char u = c;
        if (u >= 'a' && u <= 'z') u = char(u - 'a' + 'A');
        if (IsDChar(u) || (allowDot && u == '.')) out += u;
        else out += '_';
    }
    return out;
}

// ISO9660 level-2 identifier (<= 31 chars). Files get ";1".
std::string IsoFileId(const std::string& name) {
    std::string base = name, ext;
    size_t dot = name.find_last_of('.');
    if (dot != std::string::npos && dot != 0) { base = name.substr(0, dot); ext = name.substr(dot + 1); }
    base = MangleComponent(base, false);
    ext  = MangleComponent(ext, false);
    // Keep total (base + '.' + ext + ";1") within 31 chars.
    std::string id = base;
    if (!ext.empty()) id += "." + ext;
    if (id.size() > 29) id = id.substr(0, 29);
    id += ";1";
    return id;
}

std::string IsoDirId(const std::string& name) {
    std::string id = MangleComponent(name, false);
    if (id.empty()) id = "_";
    if (id.size() > 31) id = id.substr(0, 31);
    return id;
}

// Joliet identifier as UCS-2 big-endian code units. Files get ";1".
std::vector<uint16_t> JolietId(const std::string& name, bool isFile) {
    std::string n = name;
    const size_t maxChars = isFile ? 62 : 64; // leave room for ";1" on files
    if (n.size() > maxChars) n = n.substr(0, maxChars);
    std::vector<uint16_t> u;
    for (unsigned char c : n) u.push_back(c);
    if (isFile) { u.push_back(';'); u.push_back('1'); }
    return u;
}

std::vector<uint8_t> Ucs2Bytes(const std::vector<uint16_t>& u) {
    std::vector<uint8_t> b(u.size() * 2);
    for (size_t i = 0; i < u.size(); ++i) { b[2*i] = (u[i] >> 8) & 0xFF; b[2*i+1] = u[i] & 0xFF; }
    return b;
}

// ---- in-memory model ----------------------------------------------------
struct FileEnt {
    fs::path srcPath;
    uint64_t size = 0;
    std::string isoId;                 // "NAME.EXT;1"
    std::vector<uint16_t> jolietId;
    uint32_t lba = 0;                  // absolute; shared by both trees
    uint32_t sectors = 0;
};

struct DirEnt {
    std::string diskName;
    int parent = -1;
    std::vector<int> subdirs;          // indices into dirs
    std::vector<int> files;            // indices into files
    std::string isoId;                 // dir identifier (no ";1")
    std::vector<uint16_t> jolietId;
    uint32_t lbaP = 0, sizeP = 0;      // primary  extent + data length (sector-padded)
    uint32_t lbaJ = 0, sizeJ = 0;      // joliet   extent + data length
};

struct Rec {
    uint32_t extent;
    uint32_t size;
    bool isDir;
    std::vector<uint8_t> name;         // encoded identifier bytes (0x00/0x01 for . / ..)
};

struct Builder {
    std::vector<DirEnt> dirs;
    std::vector<FileEnt> files;
    const Iso9660Options* opts = nullptr;

    // Scan the tree breadth-first so parents precede children (path-table order).
    bool Scan(const fs::path& root, std::string* err) {
        DirEnt r; r.diskName = ""; r.parent = -1;
        dirs.push_back(r);
        std::vector<std::pair<int, fs::path>> queue = {{0, root}};
        for (size_t qi = 0; qi < queue.size(); ++qi) {
            int dirIdx = queue[qi].first;
            fs::path dirPath = queue[qi].second;

            std::vector<fs::directory_entry> entries;
            std::error_code ec;
            for (const auto& e : fs::directory_iterator(dirPath, ec)) entries.push_back(e);
            if (ec) { if (err) *err = "cannot read directory: " + dirPath.string(); return false; }

            // Deterministic order (uppercase disk name). DC/KOS scan linearly, so
            // strict ECMA collation is not required.
            std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) {
                std::string an = a.path().filename().string(), bn = b.path().filename().string();
                std::transform(an.begin(), an.end(), an.begin(), ::toupper);
                std::transform(bn.begin(), bn.end(), bn.begin(), ::toupper);
                return an < bn;
            });

            for (const auto& e : entries) {
                std::string name = e.path().filename().string();
                if (opts->excludeName && opts->excludeName(name)) continue;
                if (e.is_directory()) {
                    DirEnt d; d.diskName = name; d.parent = dirIdx;
                    d.isoId = IsoDirId(name); d.jolietId = JolietId(name, false);
                    int idx = int(dirs.size());
                    dirs.push_back(d);
                    dirs[dirIdx].subdirs.push_back(idx);
                    queue.push_back({idx, e.path()});
                } else if (e.is_regular_file()) {
                    FileEnt f; f.srcPath = e.path(); f.size = e.file_size();
                    f.isoId = IsoFileId(name); f.jolietId = JolietId(name, true);
                    f.sectors = RoundUpSectors(f.size);
                    int idx = int(files.size());
                    files.push_back(f);
                    dirs[dirIdx].files.push_back(idx);
                }
            }
        }
        return true;
    }

    // Ordered directory records for one directory in one encoding. Values used
    // only for byte emission; record *lengths* depend solely on name sizes.
    std::vector<Rec> DirRecords(int dirIdx, bool joliet) const {
        const DirEnt& d = dirs[dirIdx];
        const DirEnt& parent = dirs[d.parent < 0 ? dirIdx : d.parent];
        std::vector<Rec> recs;
        recs.push_back({joliet ? d.lbaJ : d.lbaP, joliet ? d.sizeJ : d.sizeP, true, {0x00}});
        recs.push_back({joliet ? parent.lbaJ : parent.lbaP, joliet ? parent.sizeJ : parent.sizeP, true, {0x01}});
        for (int si : d.subdirs) {
            const DirEnt& s = dirs[si];
            Rec r; r.extent = joliet ? s.lbaJ : s.lbaP; r.size = joliet ? s.sizeJ : s.sizeP; r.isDir = true;
            r.name = joliet ? Ucs2Bytes(s.jolietId) : std::vector<uint8_t>(s.isoId.begin(), s.isoId.end());
            recs.push_back(std::move(r));
        }
        for (int fi : d.files) {
            const FileEnt& f = files[fi];
            Rec r; r.extent = f.lba; r.size = uint32_t(f.size); r.isDir = false;
            r.name = joliet ? Ucs2Bytes(f.jolietId) : std::vector<uint8_t>(f.isoId.begin(), f.isoId.end());
            recs.push_back(std::move(r));
        }
        return recs;
    }

    static uint32_t RecLen(size_t nameLen) {
        uint32_t len = 33 + uint32_t(nameLen);
        if (len & 1) len++;             // pad to even
        return len;
    }

    // Directory data length: records packed into sectors without crossing a
    // sector boundary, rounded up to a whole number of sectors.
    static uint32_t DirDataLen(const std::vector<Rec>& recs) {
        uint32_t off = 0;
        for (const auto& r : recs) {
            uint32_t len = RecLen(r.name.size());
            if ((off % kSectorSize) + len > kSectorSize) off = ((off / kSectorSize) + 1) * kSectorSize;
            off += len;
        }
        return RoundUpSectors(off) * kSectorSize;
    }

    // Path table size in bytes for the given encoding (dir identifiers only).
    uint32_t PathTableSize(bool joliet) const {
        uint32_t total = 0;
        for (size_t i = 0; i < dirs.size(); ++i) {
            size_t idLen = (i == 0) ? 1 : (joliet ? dirs[i].jolietId.size() * 2 : dirs[i].isoId.size());
            uint32_t len = 8 + uint32_t(idLen);
            if (len & 1) len++;
            total += len;
        }
        return total;
    }
};

// Write one directory record; returns its (padded) length.
uint32_t WriteDirRecord(uint8_t* p, uint32_t extentLba, uint32_t dataLen, bool isDir,
                        const uint8_t* name, size_t nameLen) {
    uint32_t recLen = 33 + uint32_t(nameLen);
    bool pad = (recLen & 1) != 0;
    if (pad) recLen++;
    std::memset(p, 0, recLen);
    p[0] = uint8_t(recLen);
    p[1] = 0;
    Put32Both(p + 2, extentLba);
    Put32Both(p + 10, dataLen);
    PutDirDate(p + 18);
    p[25] = isDir ? 0x02 : 0x00;
    p[26] = 0; p[27] = 0;
    Put16Both(p + 28, 1);              // volume sequence number
    p[32] = uint8_t(nameLen);
    std::memcpy(p + 33, name, nameLen);
    return recLen;
}

// Fill a fixed a/d-char text field with spaces then copy an ASCII string.
void PutStrField(uint8_t* p, size_t len, const std::string& s) {
    std::memset(p, ' ', len);
    std::memcpy(p, s.data(), std::min(len, s.size()));
}

// A 17-byte "unspecified" volume datetime ("0"*16 + tz 0).
void PutVolDate(uint8_t* p) { std::memset(p, '0', 16); p[16] = 0; }

// Write a Primary (type 1) or Joliet SVD (type 2) volume descriptor.
void WriteVolumeDescriptor(uint8_t* sec, bool joliet, const std::string& volId,
                           uint32_t volumeSpaceSize, uint32_t pathTableSize,
                           uint32_t lPathLba, uint32_t mPathLba,
                           uint32_t rootExtent, uint32_t rootSize) {
    std::memset(sec, 0, kSectorSize);
    sec[0] = joliet ? 2 : 1;
    std::memcpy(sec + 1, "CD001", 5);
    sec[6] = 1;                        // version

    // System identifier (8..39) and volume identifier (40..71).
    if (joliet) {
        std::memset(sec + 8, 0, 32);
        // Volume identifier as UCS-2BE, space-padded.
        for (int i = 0; i < 16; ++i) { sec[40 + 2*i] = 0x00; sec[40 + 2*i + 1] = 0x20; }
        for (size_t i = 0; i < volId.size() && i < 16; ++i) { sec[40 + 2*i] = 0x00; sec[40 + 2*i + 1] = uint8_t(volId[i]); }
    } else {
        std::memset(sec + 8, ' ', 32);
        PutStrField(sec + 40, 32, volId);
    }

    Put32Both(sec + 80, volumeSpaceSize);         // volume space size
    if (joliet) { sec[88] = 0x25; sec[89] = 0x2F; sec[90] = 0x45; } // escape "%/E"
    Put16Both(sec + 120, 1);                       // volume set size
    Put16Both(sec + 124, 1);                       // volume sequence number
    Put16Both(sec + 128, uint16_t(kSectorSize));   // logical block size
    Put32Both(sec + 132, pathTableSize);           // path table size
    Put32LE(sec + 140, lPathLba);                  // L path table
    Put32LE(sec + 144, 0);
    Put32BE(sec + 148, mPathLba);                  // M path table
    Put32BE(sec + 152, 0);

    // Root directory record (34 bytes) at offset 156.
    uint8_t root0 = 0x00;
    WriteDirRecord(sec + 156, rootExtent, rootSize, true, &root0, 1);

    // Text identifier fields — spaces (128 each) then file-id fields (37 each).
    std::memset(sec + 190, ' ', 128);  // volume set id
    std::memset(sec + 318, ' ', 128);  // publisher
    std::memset(sec + 446, ' ', 128);  // data preparer
    std::memset(sec + 574, ' ', 128);  // application id
    std::memset(sec + 702, ' ', 37);   // copyright file id
    std::memset(sec + 739, ' ', 37);   // abstract file id
    std::memset(sec + 776, ' ', 37);   // bibliographic file id
    PutVolDate(sec + 813);             // creation
    PutVolDate(sec + 830);             // modification
    PutVolDate(sec + 847);             // expiration
    PutVolDate(sec + 864);             // effective
    sec[881] = 1;                      // file structure version
}

} // namespace

std::vector<uint8_t> BuildIso9660(const fs::path& rootDir, const Iso9660Options& opts, std::string* err) {
    if (opts.systemArea.size() != kSystemSectors * kSectorSize) {
        if (err) *err = "system area must be exactly 32768 bytes";
        return {};
    }

    Builder b;
    b.opts = &opts;
    if (!b.Scan(rootDir, err)) return {};

    // --- Pass B: directory data lengths (independent of extent values) ------
    for (size_t i = 0; i < b.dirs.size(); ++i) {
        b.dirs[i].sizeP = Builder::DirDataLen(b.DirRecords(int(i), false));
        b.dirs[i].sizeJ = Builder::DirDataLen(b.DirRecords(int(i), true));
    }

    const uint32_t start = opts.startLba;
    const uint32_t pathSizeP = b.PathTableSize(false);
    const uint32_t pathSizeJ = b.PathTableSize(true);

    // --- Pass C: assign relative sector positions, then make LBAs absolute ---
    uint32_t rel = kFirstFree;
    auto take = [&](uint32_t sectors) { uint32_t at = rel; rel += sectors; return at; };

    const uint32_t lPathP = take(RoundUpSectors(pathSizeP));
    const uint32_t mPathP = take(RoundUpSectors(pathSizeP));
    const uint32_t lPathJ = take(RoundUpSectors(pathSizeJ));
    const uint32_t mPathJ = take(RoundUpSectors(pathSizeJ));

    for (auto& d : b.dirs) d.lbaP = start + take(d.sizeP / kSectorSize);
    for (auto& d : b.dirs) d.lbaJ = start + take(d.sizeJ / kSectorSize);
    for (auto& f : b.files) {
        f.lba = start + take(f.sectors);
    }

    const uint32_t totalRelSectors = rel;
    const uint32_t volumeSpaceSize = start + totalRelSectors; // absolute end

    std::vector<uint8_t> image(uint64_t(totalRelSectors) * kSectorSize, 0);

    // System area (IP.BIN) in sectors 0..15.
    std::memcpy(image.data(), opts.systemArea.data(), opts.systemArea.size());

    // Volume descriptors.
    std::string volId = MangleComponent(opts.volumeId.empty() ? "DREAMCAST" : opts.volumeId, false);
    if (volId.size() > 32) volId = volId.substr(0, 32);
    WriteVolumeDescriptor(image.data() + uint64_t(kPvdSector) * kSectorSize, false, volId,
                          volumeSpaceSize, pathSizeP, start + lPathP, start + mPathP,
                          b.dirs[0].lbaP, b.dirs[0].sizeP);
    WriteVolumeDescriptor(image.data() + uint64_t(kSvdSector) * kSectorSize, true, volId,
                          volumeSpaceSize, pathSizeJ, start + lPathJ, start + mPathJ,
                          b.dirs[0].lbaJ, b.dirs[0].sizeJ);
    // Volume descriptor set terminator.
    {
        uint8_t* t = image.data() + uint64_t(kTerminator) * kSectorSize;
        t[0] = 255; std::memcpy(t + 1, "CD001", 5); t[6] = 1;
    }

    // --- Path tables --------------------------------------------------------
    auto writePathTable = [&](uint32_t relLba, bool joliet, bool bigEndian) {
        uint8_t* base = image.data() + uint64_t(relLba) * kSectorSize;
        uint32_t off = 0;
        for (size_t i = 0; i < b.dirs.size(); ++i) {
            const DirEnt& d = b.dirs[i];
            std::vector<uint8_t> id;
            if (i == 0) id = {0x00};
            else if (joliet) id = Ucs2Bytes(d.jolietId);
            else id = std::vector<uint8_t>(d.isoId.begin(), d.isoId.end());

            uint32_t extent = joliet ? d.lbaJ : d.lbaP;
            uint16_t parentNo = uint16_t((d.parent < 0 ? 0 : d.parent) + 1); // 1-based

            uint8_t* p = base + off;
            p[0] = uint8_t(id.size());
            p[1] = 0;
            if (bigEndian) { Put32BE(p + 2, extent); Put16BE(p + 6, parentNo); }
            else           { Put32LE(p + 2, extent); Put16LE(p + 6, parentNo); }
            std::memcpy(p + 8, id.data(), id.size());
            uint32_t len = 8 + uint32_t(id.size());
            if (len & 1) { p[len] = 0; len++; }
            off += len;
        }
    };
    writePathTable(lPathP, false, false);
    writePathTable(mPathP, false, true);
    writePathTable(lPathJ, true, false);
    writePathTable(mPathJ, true, true);

    // --- Directory extents --------------------------------------------------
    auto writeDir = [&](int dirIdx, bool joliet) {
        const DirEnt& d = b.dirs[dirIdx];
        uint32_t baseRel = (joliet ? d.lbaJ : d.lbaP) - start;
        uint8_t* base = image.data() + uint64_t(baseRel) * kSectorSize;
        std::vector<Rec> recs = b.DirRecords(dirIdx, joliet);
        uint32_t off = 0;
        for (const auto& r : recs) {
            uint32_t len = Builder::RecLen(r.name.size());
            if ((off % kSectorSize) + len > kSectorSize) off = ((off / kSectorSize) + 1) * kSectorSize;
            WriteDirRecord(base + off, r.extent, r.size, r.isDir, r.name.data(), r.name.size());
            off += len;
        }
    };
    for (size_t i = 0; i < b.dirs.size(); ++i) { writeDir(int(i), false); writeDir(int(i), true); }

    // --- File data ----------------------------------------------------------
    for (const auto& f : b.files) {
        std::ifstream in(f.srcPath, std::ios::binary);
        if (!in) { if (err) *err = "cannot open file: " + f.srcPath.string(); return {}; }
        uint8_t* dst = image.data() + uint64_t(f.lba - start) * kSectorSize;
        in.read(reinterpret_cast<char*>(dst), std::streamsize(f.size));
    }

    return image;
}

} // namespace dcdisc
