#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "fujinet/disk/cbm_volume.h"

namespace fujinet::tests {

// Builds a small valid 1541 disk image in memory: header sector, directory and
// file chains, laid out from the public format (not through CbmVolume, so a bug
// there cannot hide itself).
class D64Fixture {
public:
    static constexpr std::size_t sector_count = 683;
    static constexpr std::size_t image_bytes = sector_count * 256;

    D64Fixture() : bytes(image_bytes, 0)
    {
        auto* bam = sector(18, 0);
        bam[0] = 18;
        bam[1] = 1;
        bam[2] = 'A';
        // Every track but the directory's reports all its sectors free.
        for (std::uint8_t t = 1; t <= 35; ++t) {
            bam[4 + (t - 1) * 4] =
                t == 18 ? 0 : disk::cbm_sectors_on_track(t);
        }
        for (std::size_t i = 0x90; i <= 0xAA; ++i) bam[i] = 0xA0;
        name_disk("FIXTURE", "ID");
        bam[0xA5] = '2';
        bam[0xA6] = 'A';

        auto* dir = sector(18, 1);
        dir[0] = 0;
        dir[1] = 0xFF;
    }

    std::uint8_t* sector(std::uint8_t track, std::uint8_t number)
    {
        std::uint32_t block = 0;
        disk::cbm_block_number(track, number, sector_count, block);
        return bytes.data() + block * 256;
    }

    void name_disk(const std::string& name, const std::string& id)
    {
        auto* bam = sector(18, 0);
        for (std::size_t i = 0; i < 16; ++i) {
            bam[0x90 + i] = i < name.size() ? static_cast<std::uint8_t>(name[i]) : 0xA0;
        }
        bam[0xA2] = static_cast<std::uint8_t>(id[0]);
        bam[0xA3] = static_cast<std::uint8_t>(id[1]);
    }

    // Writes `data` as a sector chain starting at track 1 sector `firstSector`,
    // and returns the number of blocks used.
    std::uint16_t write_chain(std::uint8_t track, std::uint8_t first,
                              const std::vector<std::uint8_t>& data)
    {
        std::uint16_t blocks = 0;
        std::size_t pos = 0;
        std::uint8_t number = first;
        do {
            auto* s = sector(track, number);
            const std::size_t n = std::min<std::size_t>(254, data.size() - pos);
            std::copy_n(data.data() + pos, n, s + 2);
            pos += n;
            ++blocks;
            if (pos < data.size()) {
                s[0] = track;
                s[1] = static_cast<std::uint8_t>(number + 1);
                ++number;
            } else {
                s[0] = 0;
                s[1] = static_cast<std::uint8_t>(n + 1);
            }
        } while (pos < data.size());
        return blocks;
    }

    // Adds a directory entry in slot `slot` (0-7) of the first directory sector.
    void add_entry(std::size_t slot, std::uint8_t typeByte, const std::string& name,
                   std::uint8_t track, std::uint8_t number, std::uint16_t blocks)
    {
        auto* e = sector(18, 1) + slot * 32;
        e[2] = typeByte;
        e[3] = track;
        e[4] = number;
        for (std::size_t i = 0; i < 16; ++i) {
            e[5 + i] = i < name.size() ? static_cast<std::uint8_t>(name[i]) : 0xA0;
        }
        e[30] = static_cast<std::uint8_t>(blocks & 0xFF);
        e[31] = static_cast<std::uint8_t>(blocks >> 8);
    }

    // A PRG file: its chain starts at track 1, sector `first`.
    void add_prg(std::size_t slot, const std::string& name, std::uint8_t first,
                 const std::vector<std::uint8_t>& data)
    {
        const auto blocks = write_chain(1, first, data);
        add_entry(slot, 0x82, name, 1, first, blocks);
    }

    std::vector<std::uint8_t> bytes;
};

} // namespace fujinet::tests
