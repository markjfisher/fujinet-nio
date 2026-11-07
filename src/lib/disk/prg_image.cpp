#include "fujinet/disk/prg_image.h"

#include "fujinet/disk/cbm_volume.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <utility>

namespace fujinet::disk {

namespace {

constexpr std::uint16_t sector_bytes = 256;
constexpr std::uint32_t sectors_on_disk = 683;
constexpr std::uint8_t tracks_on_disk = 35;

// Track 18 holds the header and the directory, and never file data. File sectors run
// in block order from track 1 sector 0, stepping over it. There is no disk to wait
// for, so nothing is gained by the interleave a real drive would use.
constexpr std::uint8_t directory_track = 18;
constexpr std::uint8_t header_sector = 0;
constexpr std::uint8_t directory_sector = 1;
constexpr std::uint32_t directory_track_first_block = 17 * 21;
constexpr std::uint32_t directory_track_sectors = 19;
constexpr std::uint32_t header_block = directory_track_first_block + header_sector;
constexpr std::uint32_t directory_block = directory_track_first_block + directory_sector;
constexpr std::uint32_t data_sectors = sectors_on_disk - directory_track_sectors;

constexpr std::size_t link_bytes = 2;
constexpr std::size_t data_bytes = sector_bytes - link_bytes;
constexpr std::uint64_t smallest_file = 2; // a load address
constexpr std::uint64_t largest_file = std::uint64_t{data_sectors} * data_bytes;

constexpr std::uint8_t shifted_space = 0xA0;

// Header sector.
constexpr std::uint8_t dos_version = 'A';
constexpr std::size_t bam_first_entry = 0x04;
constexpr std::size_t bam_entry_bytes = 4;
constexpr std::size_t disk_name = 0x90;
constexpr std::size_t disk_id = 0xA2;
constexpr std::size_t dos_type = 0xA5;
constexpr std::size_t padding_end = 0xAB; // shifted spaces from disk_name up to here
constexpr char id_text[2] = {'F', 'N'};
constexpr char dos_type_text[2] = {'2', 'A'};

// Directory sector. Its first entry shares its first two bytes with the link.
constexpr std::uint8_t no_next_track = 0;
constexpr std::uint8_t no_next_sector = 0xFF;
constexpr std::size_t entry_type = 0x02;
constexpr std::size_t entry_track = 0x03;
constexpr std::size_t entry_sector = 0x04;
constexpr std::size_t entry_name = 0x05;
constexpr std::size_t entry_blocks = 0x1E;
constexpr std::uint8_t type_closed_prg = 0x82;
constexpr std::uint8_t first_data_track = 1;
constexpr std::uint8_t first_data_sector = 0;

constexpr std::size_t name_bytes = CbmDirEntry::name_bytes;
constexpr const char* unnamed = "PROGRAM";
constexpr std::string_view program_extension = ".prg";
constexpr char replacement = '-';

std::uint32_t block_of_data_sector(std::uint32_t index) noexcept
{
    return index < directory_track_first_block ? index : index + directory_track_sectors;
}

bool data_sector_of_block(std::uint32_t block, std::uint32_t& index) noexcept
{
    if (block < directory_track_first_block) {
        index = block;
        return true;
    }
    if (block < directory_track_first_block + directory_track_sectors) {
        return false;
    }
    index = block - directory_track_sectors;
    return true;
}

void track_and_sector(std::uint32_t block, std::uint8_t& track, std::uint8_t& sector) noexcept
{
    track = 1;
    while (block >= cbm_sectors_on_track(track)) {
        block -= cbm_sectors_on_track(track);
        ++track;
    }
    sector = static_cast<std::uint8_t>(block);
}

char name_byte(char c) noexcept
{
    if (c >= 'a' && c <= 'z') {
        return static_cast<char>(c - 'a' + 'A');
    }
    if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) {
        return c;
    }
    switch (c) {
        case ' ': case '-': case '+': case '.': case '!':
        case '%': case '&': case '\'': case '(': case ')':
            return c;
        default:
            return replacement;
    }
}

bool ends_with_program_extension(std::string_view name) noexcept
{
    if (name.size() < program_extension.size()) {
        return false;
    }
    const std::string_view tail = name.substr(name.size() - program_extension.size());
    return std::equal(tail.begin(), tail.end(), program_extension.begin(), [](char a, char b) {
        return std::tolower(static_cast<unsigned char>(a)) == b;
    });
}

class PrgDiskImage final : public IDiskImage {
public:
    ImageType type() const noexcept override { return ImageType::Prg; }
    DiskGeometry geometry() const noexcept override { return _geometry; }
    bool read_only() const noexcept override { return true; }

    DiskResult mount(
        std::unique_ptr<fs::IFile> file,
        std::uint64_t sizeBytes,
        const MountOptions& opts
    ) override
    {
        if (!file) {
            return DiskResult{DiskError::OpenFailed};
        }
        if (!prg_disk_geometry(sizeBytes, _geometry)) {
            return DiskResult{DiskError::BadImage};
        }
        _file = std::move(file);
        _size = sizeBytes;
        _blocks = static_cast<std::uint32_t>((sizeBytes + data_bytes - 1) / data_bytes);
        _name = prg_disk_name(opts.sourcePath);
        _filePosition = unknown_position;
        return DiskResult{DiskError::None};
    }

    DiskResult unmount() override
    {
        _file.reset();
        _geometry = {};
        _size = 0;
        _blocks = 0;
        _name.clear();
        return DiskResult{DiskError::None};
    }

    DiskResult read_sector(std::uint32_t lba, std::uint8_t* dst, std::size_t dstBytes) override
    {
        if (!_file) {
            return DiskResult{DiskError::NotMounted};
        }
        if (!dst || dstBytes < sector_bytes) {
            return DiskResult{DiskError::InvalidSlot};
        }
        if (lba >= sectors_on_disk) {
            return DiskResult{DiskError::OutOfRange};
        }

        std::memset(dst, 0, sector_bytes);
        std::uint32_t index = 0;
        if (lba == header_block) {
            fill_header(dst);
        } else if (lba == directory_block) {
            fill_directory(dst);
        } else if (data_sector_of_block(lba, index) && index < _blocks) {
            const DiskResult result = fill_data(index, dst);
            if (!result.ok()) {
                return result;
            }
        }
        return DiskResult{DiskError::None, sector_bytes};
    }

    DiskResult write_sector(std::uint32_t, const std::uint8_t*, std::size_t) override
    {
        if (!_file) {
            return DiskResult{DiskError::NotMounted};
        }
        return DiskResult{DiskError::ReadOnly};
    }

    DiskResult flush() override
    {
        if (!_file) {
            return DiskResult{DiskError::NotMounted};
        }
        return DiskResult{DiskError::None};
    }

private:
    static constexpr std::uint64_t unknown_position = ~std::uint64_t{0};

    bool block_in_use(std::uint32_t block) const noexcept
    {
        if (block == header_block || block == directory_block) {
            return true;
        }
        std::uint32_t index = 0;
        return data_sector_of_block(block, index) && index < _blocks;
    }

    void write_name(std::uint8_t* dst) const noexcept
    {
        std::memset(dst, shifted_space, name_bytes);
        std::memcpy(dst, _name.data(), std::min(_name.size(), name_bytes));
    }

    void fill_header(std::uint8_t* dst) const noexcept
    {
        dst[0] = directory_track;
        dst[1] = directory_sector;
        dst[2] = dos_version;

        std::uint32_t block = 0;
        for (std::uint8_t track = 1; track <= tracks_on_disk; ++track) {
            std::uint8_t* entry = dst + bam_first_entry + (track - 1) * bam_entry_bytes;
            const std::uint8_t onTrack = cbm_sectors_on_track(track);
            for (std::uint8_t sector = 0; sector < onTrack; ++sector, ++block) {
                if (block_in_use(block)) {
                    continue;
                }
                ++entry[0];
                entry[1 + sector / 8] |= static_cast<std::uint8_t>(1u << (sector % 8));
            }
        }

        std::memset(dst + disk_name, shifted_space, padding_end - disk_name);
        write_name(dst + disk_name);
        std::memcpy(dst + disk_id, id_text, sizeof(id_text));
        std::memcpy(dst + dos_type, dos_type_text, sizeof(dos_type_text));
    }

    void fill_directory(std::uint8_t* dst) const noexcept
    {
        dst[0] = no_next_track;
        dst[1] = no_next_sector;
        dst[entry_type] = type_closed_prg;
        dst[entry_track] = first_data_track;
        dst[entry_sector] = first_data_sector;
        write_name(dst + entry_name);
        dst[entry_blocks] = static_cast<std::uint8_t>(_blocks & 0xFF);
        dst[entry_blocks + 1] = static_cast<std::uint8_t>(_blocks >> 8);
    }

    DiskResult fill_data(std::uint32_t index, std::uint8_t* dst)
    {
        const std::uint64_t offset = std::uint64_t{index} * data_bytes;
        const auto want = static_cast<std::size_t>(std::min<std::uint64_t>(data_bytes, _size - offset));
        if (offset != _filePosition && !_file->seek(offset)) {
            _filePosition = unknown_position;
            return DiskResult{DiskError::IoError};
        }
        if (_file->read(dst + link_bytes, want) != want) {
            _filePosition = unknown_position;
            return DiskResult{DiskError::IoError};
        }
        _filePosition = offset + want;

        if (index + 1 < _blocks) {
            track_and_sector(block_of_data_sector(index + 1), dst[0], dst[1]);
        } else {
            dst[0] = 0;
            dst[1] = static_cast<std::uint8_t>(want + 1); // the index of the last byte in use
        }
        return DiskResult{DiskError::None};
    }

    std::unique_ptr<fs::IFile> _file;
    DiskGeometry _geometry{};
    std::uint64_t _size{0};
    std::uint64_t _filePosition{unknown_position};
    std::uint32_t _blocks{0};
    std::string _name;
};

} // namespace

bool prg_disk_geometry(std::uint64_t sizeBytes, DiskGeometry& out) noexcept
{
    if (sizeBytes < smallest_file || sizeBytes > largest_file) {
        return false;
    }
    out.sectorSize = sector_bytes;
    out.sectorCount = sectors_on_disk;
    out.supportsVariableSectorSize = false;
    return true;
}

std::string prg_disk_name(std::string_view path)
{
    const auto slash = path.find_last_of('/');
    std::string_view base = slash == std::string_view::npos ? path : path.substr(slash + 1);
    if (ends_with_program_extension(base)) {
        base.remove_suffix(program_extension.size());
    }

    std::string name;
    for (const char c : base.substr(0, name_bytes)) {
        name.push_back(name_byte(c));
    }
    return name.empty() ? std::string(unnamed) : name;
}

std::unique_ptr<IDiskImage> make_prg_disk_image()
{
    return std::make_unique<PrgDiskImage>();
}

} // namespace fujinet::disk
