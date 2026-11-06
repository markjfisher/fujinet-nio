#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "fujinet/disk/disk_types.h"

namespace fujinet::disk {

// Read-only view of the Commodore DOS 2.x structures on a 1541-layout disk:
// the header sector, the directory and the sector chains of files. Written
// from the public disk format. It knows nothing of the IEC bus or DiskService;
// sectors come through a callback, so it can be tested on a byte array.

enum class CbmFileType : std::uint8_t { Del = 0, Seq = 1, Prg = 2, Usr = 3, Rel = 4, Unknown = 5 };

enum class CbmError : std::uint8_t {
    None,
    NotReady,           // no media, or media that cannot be read at all
    ReadError,          // a sector could not be read
    IllegalTrackSector, // a chain points outside the disk, or loops
};

struct CbmHeader {
    std::string name;     // PETSCII; shifted spaces read as spaces, trailing ones dropped
    std::string idField;  // the 5 bytes after the name: ID, a separator, DOS type ("FN 2A")
    std::uint16_t freeBlocks{0};
};

struct CbmDirEntry {
    static constexpr std::size_t name_bytes = 16;

    CbmFileType type{CbmFileType::Del};
    bool closed{false}; // false: a "splat" file, written but never closed
    bool locked{false};
    std::array<std::uint8_t, name_bytes> rawName{}; // as stored, for the listing
    std::string name;   // what programs ask for: up to the first shifted space
    std::uint8_t track{0};
    std::uint8_t sector{0};
    std::uint16_t blocks{0};
};

// Sectors a track holds on the 1541 layout (tracks 1-42); 0 for any other track.
std::uint8_t cbm_sectors_on_track(std::uint8_t track) noexcept;

// Block number of a track and sector within a disk of `sectorCount` sectors.
bool cbm_block_number(std::uint8_t track, std::uint8_t sector, std::uint32_t sectorCount,
                      std::uint32_t& out) noexcept;

// CBM file name matching: '?' matches any one character, '*' matches everything
// from there on. Without a '*' the lengths must agree. Bytes compare as they are;
// a file name and a pattern are both PETSCII.
bool cbm_name_matches(const std::string& pattern, const std::string& name) noexcept;

// A type letter from a file name or a directory filter (P, S, U, L, D) against a
// file's type. 0 matches every type.
bool cbm_type_matches(char letter, CbmFileType type) noexcept;

class CbmVolume {
public:
    static constexpr std::size_t sector_bytes = 256;
    using Sector = std::array<std::uint8_t, sector_bytes>;
    using ReadSector = std::function<DiskResult(std::uint32_t lba, std::uint8_t* dst, std::size_t dstBytes)>;

    CbmVolume(ReadSector read, std::uint32_t sectorCount)
        : _read(std::move(read)), _sectorCount(sectorCount) {}

    CbmError read_header(CbmHeader& out) const;

    // Every used directory slot, in directory order, deleted slots excluded. On an
    // error `out` still holds the entries read before it, so a damaged directory can
    // be listed as far as it goes; a chain that loops ends the walk.
    CbmError read_directory(std::vector<CbmDirEntry>& out) const;

    // One sector by track and sector number.
    CbmError read_block(std::uint8_t track, std::uint8_t sector, Sector& out) const;

    std::uint32_t sector_count() const noexcept { return _sectorCount; }

private:
    ReadSector _read;
    std::uint32_t _sectorCount;
};

// Streams the data bytes of one file's sector chain. A chain longer than the
// disk has sectors is reported as an illegal track/sector, which also ends one
// that loops.
class CbmFileReader {
public:
    CbmFileReader() = default;
    CbmFileReader(const CbmVolume& volume, std::uint8_t track, std::uint8_t sector)
        : _volume(&volume), _track(track), _sector(sector) {}

    // Copies up to maxBytes of file data to dst and returns the count. `last` is
    // set when the final byte of the file is among them, so a caller can send the
    // end-of-file signal with that byte. error() says why a read stopped early.
    std::size_t read(std::uint8_t* dst, std::size_t maxBytes, bool& last);

    CbmError error() const noexcept { return _error; }

private:
    bool load_next();

    const CbmVolume* _volume{nullptr};
    std::uint8_t _track{0};
    std::uint8_t _sector{0};
    CbmVolume::Sector _data{};
    std::size_t _pos{0};
    std::size_t _end{0};
    std::uint32_t _sectorsRead{0};
    bool _finalSector{false};
    bool _done{false};
    CbmError _error{CbmError::None};
};

// The bytes LOAD"$" delivers: a tokenised BASIC program whose lines are the disk
// header, one line per matching file, and a BLOCKS FREE line, 32 bytes each as a
// 1541 lays them out. An empty `pattern` lists every name; `typeLetter` 0 every type.
std::vector<std::uint8_t> cbm_format_directory(const CbmHeader& header,
                                               const std::vector<CbmDirEntry>& entries,
                                               const std::string& pattern,
                                               char typeLetter = 0);

} // namespace fujinet::disk
