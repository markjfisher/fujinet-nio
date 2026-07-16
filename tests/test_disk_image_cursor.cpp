#include "doctest.h"

#include "fujinet/disk/atr_image.h"
#include "fujinet/disk/raw_image.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>
#include <utility>
#include <vector>

namespace {

struct FileStats {
    int seeks{0};
};

class TrackingFile final : public fujinet::fs::IFile {
public:
    TrackingFile(std::vector<std::uint8_t> bytes, FileStats& stats, bool readOnly)
        : _bytes(std::move(bytes)), _stats(stats), _readOnly(readOnly)
    {
    }

    std::size_t read(void* dst, std::size_t maxBytes) override
    {
        if (!dst || _pos >= _bytes.size()) return 0;
        const std::size_t n = std::min<std::size_t>(maxBytes, _bytes.size() - _pos);
        std::memcpy(dst, _bytes.data() + _pos, n);
        _pos += n;
        return n;
    }

    std::size_t write(const void* src, std::size_t bytes) override
    {
        if (_readOnly || !src) return 0;
        if (_pos + bytes > _bytes.size()) _bytes.resize(_pos + bytes);
        std::memcpy(_bytes.data() + _pos, src, bytes);
        _pos += bytes;
        return bytes;
    }

    bool seek(std::uint64_t offset) override
    {
        ++_stats.seeks;
        if (offset > _bytes.size()) return false;
        _pos = static_cast<std::size_t>(offset);
        return true;
    }

    std::uint64_t tell() const override { return _pos; }
    bool flush() override { return true; }

private:
    std::vector<std::uint8_t> _bytes;
    FileStats& _stats;
    bool _readOnly{true};
    std::size_t _pos{0};
};

std::vector<std::uint8_t> make_raw_bytes(std::uint16_t sectorSize, std::uint32_t sectors)
{
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(sectorSize) * sectors);
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        bytes[i] = static_cast<std::uint8_t>(i & 0xff);
    }
    return bytes;
}

std::vector<std::uint8_t> make_atr_128_bytes(std::uint32_t sectors)
{
    constexpr std::size_t headerBytes = 16;
    constexpr std::uint16_t sectorSize = 128;
    const std::uint32_t dataBytes = sectors * sectorSize;
    const std::uint32_t paragraphs = dataBytes / 16;

    std::vector<std::uint8_t> bytes(headerBytes + dataBytes);
    bytes[0] = 0x96;
    bytes[1] = 0x02;
    bytes[2] = static_cast<std::uint8_t>(paragraphs & 0xff);
    bytes[3] = static_cast<std::uint8_t>((paragraphs >> 8) & 0xff);
    bytes[4] = static_cast<std::uint8_t>(sectorSize & 0xff);
    bytes[5] = static_cast<std::uint8_t>((sectorSize >> 8) & 0xff);
    bytes[6] = static_cast<std::uint8_t>((paragraphs >> 16) & 0xff);

    for (std::size_t i = headerBytes; i < bytes.size(); ++i) {
        bytes[i] = static_cast<std::uint8_t>(i & 0xff);
    }
    return bytes;
}

} // namespace

TEST_CASE("Raw image skips seeks for sequential sector reads")
{
    FileStats stats;
    auto file = std::make_unique<TrackingFile>(make_raw_bytes(256, 4), stats, true);
    auto image = fujinet::disk::make_raw_disk_image();

    fujinet::disk::MountOptions opts{};
    opts.sectorSizeHint = 256;
    REQUIRE(image->mount(std::move(file), 4 * 256, opts).ok());
    CHECK(stats.seeks == 0);

    std::uint8_t sector[256]{};
    REQUIRE(image->read_sector(0, sector, sizeof(sector)).ok());
    CHECK(stats.seeks == 1);
    CHECK(sector[0] == 0x00);

    REQUIRE(image->read_sector(1, sector, sizeof(sector)).ok());
    REQUIRE(image->read_sector(2, sector, sizeof(sector)).ok());
    CHECK(stats.seeks == 1);
    {
        const auto imageStats = image->image_stats();
        CHECK(imageStats.readOps == 3);
        CHECK(imageStats.seekOps == 1);
        CHECK(imageStats.sequentialReadHits == 2);
        CHECK(imageStats.readBytes == 3 * 256);
    }

    REQUIRE(image->read_sector(0, sector, sizeof(sector)).ok());
    CHECK(stats.seeks == 2);
    CHECK(image->image_stats().seekOps == 2);
}

TEST_CASE("ATR image skips seeks for sequential sector reads")
{
    FileStats stats;
    auto bytes = make_atr_128_bytes(4);
    auto file = std::make_unique<TrackingFile>(bytes, stats, true);
    auto image = fujinet::disk::make_atr_disk_image();

    REQUIRE(image->mount(std::move(file), bytes.size(), fujinet::disk::MountOptions{}).ok());
    const int mountSeeks = stats.seeks;
    CHECK(mountSeeks == 1);

    std::uint8_t sector[128]{};
    REQUIRE(image->read_sector(0, sector, sizeof(sector)).ok());
    CHECK(stats.seeks == mountSeeks + 1);

    REQUIRE(image->read_sector(1, sector, sizeof(sector)).ok());
    REQUIRE(image->read_sector(2, sector, sizeof(sector)).ok());
    CHECK(stats.seeks == mountSeeks + 1);
    {
        const auto imageStats = image->image_stats();
        CHECK(imageStats.readOps == 3);
        CHECK(imageStats.seekOps == 1);
        CHECK(imageStats.sequentialReadHits == 2);
        CHECK(imageStats.readBytes == 3 * 128);
    }

    REQUIRE(image->read_sector(0, sector, sizeof(sector)).ok());
    CHECK(stats.seeks == mountSeeks + 2);
    CHECK(image->image_stats().seekOps == 2);
}
