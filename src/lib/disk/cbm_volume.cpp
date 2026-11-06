#include "fujinet/disk/cbm_volume.h"

#include <algorithm>
#include <cstring>

namespace fujinet::disk {

namespace {

constexpr std::uint8_t header_track = 18;
constexpr std::uint8_t header_sector = 0;
constexpr std::uint8_t first_directory_sector = 1;
constexpr std::size_t bam_first_track_entry = 0x04;
constexpr std::size_t bam_entry_bytes = 4;
constexpr std::size_t bam_tracks = 35;
constexpr std::size_t name_offset = 0x90;
constexpr std::size_t name_bytes = CbmDirEntry::name_bytes;
constexpr std::size_t id_field_offset = 0xA2;
constexpr std::size_t id_field_bytes = 5;

constexpr std::size_t entry_bytes = 32;
constexpr std::size_t entries_per_sector = 8;
constexpr std::size_t entry_type = 0x02;
constexpr std::size_t entry_track = 0x03;
constexpr std::size_t entry_sector = 0x04;
constexpr std::size_t entry_name = 0x05;
constexpr std::size_t entry_blocks = 0x1E;

constexpr std::uint8_t type_mask = 0x0F;
constexpr std::uint8_t type_locked = 0x40;
constexpr std::uint8_t type_closed = 0x80;

constexpr std::uint8_t shifted_space = 0xA0;
constexpr std::uint8_t reverse_on = 0x12;
constexpr std::uint8_t quote = 0x22;
constexpr std::uint8_t space = 0x20;

// The load address of a listing is ignored by LOAD"$",8. LOAD"$",8,1 uses it, and
// the start of BASIC is a kinder place than the 1541's $0401, which is the screen
// on a C64.
constexpr std::uint16_t directory_load_address = 0x0801;
constexpr std::uint8_t line_link = 0x01; // any nonzero link: BASIC relinks after LOAD
constexpr std::size_t entry_text_bytes = 27;
constexpr std::size_t free_text_bytes = 25;

// Shifted spaces read as spaces in the header's text fields.
std::string text_field(const std::uint8_t* bytes, std::size_t length)
{
    std::string out;
    out.reserve(length);
    for (std::size_t i = 0; i < length; ++i) {
        out.push_back(static_cast<char>(bytes[i] == shifted_space ? space : bytes[i]));
    }
    return out;
}

CbmError from_disk_error(DiskError error) noexcept
{
    switch (error) {
        case DiskError::None:       return CbmError::None;
        case DiskError::NotMounted: return CbmError::NotReady;
        case DiskError::OutOfRange: return CbmError::IllegalTrackSector;
        default:                    return CbmError::ReadError;
    }
}

const char* type_name(CbmFileType type) noexcept
{
    switch (type) {
        case CbmFileType::Del:     return "DEL";
        case CbmFileType::Seq:     return "SEQ";
        case CbmFileType::Prg:     return "PRG";
        case CbmFileType::Usr:     return "USR";
        case CbmFileType::Rel:     return "REL";
        case CbmFileType::Unknown: break;
    }
    return "???";
}

void begin_line(std::vector<std::uint8_t>& out, std::uint16_t number)
{
    out.push_back(line_link);
    out.push_back(line_link);
    out.push_back(static_cast<std::uint8_t>(number & 0xFF));
    out.push_back(static_cast<std::uint8_t>(number >> 8));
}

void append_text(std::vector<std::uint8_t>& out, const std::string& text)
{
    out.insert(out.end(), text.begin(), text.end());
}

// One entry's 27 text bytes. The closing quote goes at the first shifted space or
// quote in the name; what follows it in the 16 name bytes shows with bit 7 cleared,
// as padding reads as spaces and as text hidden after a shifted space does on a 1541.
std::vector<std::uint8_t> entry_text(const CbmDirEntry& entry)
{
    const std::size_t lead = entry.blocks < 10 ? 3 : (entry.blocks < 100 ? 2 : 1);
    std::vector<std::uint8_t> text(lead, space);
    text.push_back(quote);

    bool closedQuote = false;
    for (const std::uint8_t byte : entry.rawName) {
        if (!closedQuote && (byte == shifted_space || byte == quote)) {
            text.push_back(quote);
            closedQuote = true;
            continue;
        }
        text.push_back(closedQuote ? static_cast<std::uint8_t>(byte & 0x7F) : byte);
    }
    text.push_back(closedQuote ? space : quote);

    text.push_back(entry.closed ? space : '*');
    const char* typeText = type_name(entry.type);
    text.insert(text.end(), typeText, typeText + 3);
    text.push_back(entry.locked ? '<' : space);
    text.resize(entry_text_bytes, space);
    return text;
}

} // namespace

std::uint8_t cbm_sectors_on_track(std::uint8_t track) noexcept
{
    if (track >= 1 && track <= 17) return 21;
    if (track >= 18 && track <= 24) return 19;
    if (track >= 25 && track <= 30) return 18;
    if (track >= 31 && track <= 42) return 17;
    return 0;
}

bool cbm_block_number(std::uint8_t track, std::uint8_t sector, std::uint32_t sectorCount,
                      std::uint32_t& out) noexcept
{
    const std::uint8_t onTrack = cbm_sectors_on_track(track);
    if (onTrack == 0 || sector >= onTrack) {
        return false;
    }
    std::uint32_t block = sector;
    for (std::uint8_t t = 1; t < track; ++t) {
        block += cbm_sectors_on_track(t);
    }
    if (block >= sectorCount) {
        return false;
    }
    out = block;
    return true;
}

bool cbm_name_matches(const std::string& pattern, const std::string& name) noexcept
{
    for (std::size_t i = 0; i < pattern.size(); ++i) {
        const char p = pattern[i];
        if (p == '*') {
            return true;
        }
        if (i >= name.size()) {
            return false;
        }
        if (p != '?' && p != name[i]) {
            return false;
        }
    }
    return pattern.size() == name.size();
}

bool cbm_type_matches(char letter, CbmFileType type) noexcept
{
    switch (letter) {
        case 0:   return true;
        case 'D': return type == CbmFileType::Del;
        case 'S': return type == CbmFileType::Seq;
        case 'P': return type == CbmFileType::Prg;
        case 'U': return type == CbmFileType::Usr;
        case 'L': return type == CbmFileType::Rel;
        default:  return false;
    }
}

CbmError CbmVolume::read_block(std::uint8_t track, std::uint8_t sector, Sector& out) const
{
    std::uint32_t block = 0;
    if (!cbm_block_number(track, sector, _sectorCount, block)) {
        return CbmError::IllegalTrackSector;
    }
    return from_disk_error(_read(block, out.data(), out.size()).error);
}

CbmError CbmVolume::read_header(CbmHeader& out) const
{
    Sector bam{};
    const CbmError error = read_block(header_track, header_sector, bam);
    if (error != CbmError::None) {
        return error;
    }

    out.name = text_field(&bam[name_offset], name_bytes);
    while (!out.name.empty() && out.name.back() == static_cast<char>(space)) {
        out.name.pop_back();
    }
    out.idField = text_field(&bam[id_field_offset], id_field_bytes);

    // Blocks free counts every track but the directory's own. A 40 or 42 track disk
    // keeps its extra tracks' counts in different places depending on the DOS, so
    // only the standard 35 are read.
    std::uint32_t free = 0;
    for (std::size_t t = 1; t <= bam_tracks; ++t) {
        if (t == header_track) {
            continue;
        }
        free += bam[bam_first_track_entry + (t - 1) * bam_entry_bytes];
    }
    out.freeBlocks = static_cast<std::uint16_t>(std::min<std::uint32_t>(free, 0xFFFF));
    return CbmError::None;
}

CbmError CbmVolume::read_directory(std::vector<CbmDirEntry>& out) const
{
    out.clear();

    // The directory always starts at 18/1: the link in the header sector is not
    // reliable. Remembering where it has been ends a chain that loops.
    std::vector<bool> visited(_sectorCount, false);
    std::uint8_t track = header_track;
    std::uint8_t number = first_directory_sector;
    Sector sector{};

    while (track != 0) {
        std::uint32_t block = 0;
        if (!cbm_block_number(track, number, _sectorCount, block) || visited[block]) {
            return CbmError::IllegalTrackSector;
        }
        visited[block] = true;

        const CbmError error = read_block(track, number, sector);
        if (error != CbmError::None) {
            return error;
        }
        for (std::size_t i = 0; i < entries_per_sector; ++i) {
            const std::uint8_t* raw = &sector[i * entry_bytes];
            if (raw[entry_type] == 0) {
                continue; // scratched or never used
            }
            CbmDirEntry entry;
            const std::uint8_t typeCode = raw[entry_type] & type_mask;
            entry.type = typeCode <= static_cast<std::uint8_t>(CbmFileType::Rel)
                             ? static_cast<CbmFileType>(typeCode)
                             : CbmFileType::Unknown;
            entry.closed = (raw[entry_type] & type_closed) != 0;
            entry.locked = (raw[entry_type] & type_locked) != 0;
            entry.track = raw[entry_track];
            entry.sector = raw[entry_sector];
            std::copy_n(&raw[entry_name], name_bytes, entry.rawName.begin());
            const auto* nameStart = &raw[entry_name];
            const auto* nameEnd = std::find(nameStart, nameStart + name_bytes, shifted_space);
            entry.name.assign(reinterpret_cast<const char*>(nameStart),
                              static_cast<std::size_t>(nameEnd - nameStart));
            entry.blocks = static_cast<std::uint16_t>(raw[entry_blocks] | (raw[entry_blocks + 1] << 8));
            out.push_back(std::move(entry));
        }
        track = sector[0];
        number = sector[1];
    }
    return CbmError::None;
}

bool CbmFileReader::load_next()
{
    if (_volume == nullptr || _track == 0) {
        _done = true;
        return false;
    }
    if (++_sectorsRead > _volume->sector_count()) {
        _error = CbmError::IllegalTrackSector;
        _done = true;
        return false;
    }
    _error = _volume->read_block(_track, _sector, _data);
    if (_error != CbmError::None) {
        _done = true;
        return false;
    }

    _pos = 2;
    if (_data[0] == 0) {
        // Final sector: byte 1 is the index of the last data byte. Below 2 it holds no data.
        _finalSector = true;
        _end = std::max<std::size_t>(static_cast<std::size_t>(_data[1]) + 1, 2);
    } else {
        _finalSector = false;
        _end = CbmVolume::sector_bytes;
    }
    _track = _data[0];
    _sector = _data[1];
    return true;
}

std::size_t CbmFileReader::read(std::uint8_t* dst, std::size_t maxBytes, bool& last)
{
    last = false;
    std::size_t copied = 0;
    while (copied < maxBytes && !_done) {
        if (_pos >= _end) {
            if (_finalSector) {
                _done = true;
                break;
            }
            if (!load_next()) {
                break;
            }
            continue;
        }
        const std::size_t n = std::min(maxBytes - copied, _end - _pos);
        std::memcpy(dst + copied, _data.data() + _pos, n);
        copied += n;
        _pos += n;
    }

    if (!_done && _pos >= _end && _finalSector) {
        _done = true;
    }
    // A chain cut short still ends the transfer on what was delivered.
    last = _done;
    return copied;
}

std::vector<std::uint8_t> cbm_format_directory(const CbmHeader& header,
                                               const std::vector<CbmDirEntry>& entries,
                                               const std::string& pattern,
                                               char typeLetter)
{
    std::vector<std::uint8_t> out;
    out.reserve(32 * (entries.size() + 2));
    out.push_back(static_cast<std::uint8_t>(directory_load_address & 0xFF));
    out.push_back(static_cast<std::uint8_t>(directory_load_address >> 8));

    // Header: line number 0 (the drive), in reverse video.
    begin_line(out, 0);
    out.push_back(reverse_on);
    out.push_back(quote);
    std::string title = header.name;
    title.resize(name_bytes, static_cast<char>(space));
    append_text(out, title);
    out.push_back(quote);
    out.push_back(space);
    append_text(out, header.idField);
    out.push_back(0);

    for (const auto& entry : entries) {
        if (!pattern.empty() && !cbm_name_matches(pattern, entry.name)) {
            continue;
        }
        if (!cbm_type_matches(typeLetter, entry.type)) {
            continue;
        }
        begin_line(out, entry.blocks);
        const std::vector<std::uint8_t> text = entry_text(entry);
        out.insert(out.end(), text.begin(), text.end());
        out.push_back(0);
    }

    begin_line(out, header.freeBlocks);
    std::string freeLine = "BLOCKS FREE.";
    freeLine.resize(free_text_bytes, static_cast<char>(space));
    append_text(out, freeLine);
    out.push_back(0);

    // Two zero bytes end the BASIC program.
    out.push_back(0);
    out.push_back(0);
    return out;
}

} // namespace fujinet::disk
