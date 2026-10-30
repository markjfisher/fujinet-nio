// HotSync builds only (FN_WITH_HOTSYNC): the ESP32 build compiles every
// source, so elsewhere this file compiles to nothing.
#if FN_WITH_HOTSYNC == 1

#include "fujinet/hotsync/palm_database.h"

#include <algorithm>
#include <cstring>

namespace fujinet::hotsync {

namespace {

// DatabaseHdrType field offsets.
constexpr std::size_t kHeaderSize = 78;
constexpr std::size_t kNameSize = 32;
constexpr std::size_t kAttributes = 32;
constexpr std::size_t kVersion = 34;
constexpr std::size_t kCreated = 36;
constexpr std::size_t kModified = 40;
constexpr std::size_t kBackedUp = 44;
constexpr std::size_t kModNum = 48;
constexpr std::size_t kAppInfo = 52;
constexpr std::size_t kSortInfo = 56;
constexpr std::size_t kType = 60;
constexpr std::size_t kCreator = 64;
constexpr std::size_t kNumRecords = 76;

constexpr std::size_t kRecordEntrySize = 8;    // u32 offset, u8 attributes, u24 unique ID
constexpr std::size_t kResourceEntrySize = 10; // type[4], u16 id, u32 offset

// Record attribute bits shared by the file format and DLP.
constexpr std::uint8_t kAttrDelete = 0x80;
constexpr std::uint8_t kAttrBusy = 0x20;
constexpr std::uint8_t kAttrArchive = 0x08;
constexpr std::uint8_t kAttrFlagsMask = 0xF0;
constexpr std::uint8_t kAttrCategoryMask = 0x0F;
// Files conventionally carry two zero bytes between the entry list and data.
constexpr std::size_t kGap = 2;

// In a file, the low nibble is the category unless the record is deleted or
// busy, in which case bit 3 is the archive flag. DLP keeps them separate.
void split_attributes(std::uint8_t fileAttrs, std::uint8_t& dlpAttrs, std::uint8_t& category)
{
    dlpAttrs = fileAttrs & kAttrFlagsMask;
    category = 0;
    if (fileAttrs & (kAttrDelete | kAttrBusy)) {
        dlpAttrs |= fileAttrs & kAttrArchive;
    } else {
        category = fileAttrs & kAttrCategoryMask;
    }
}

std::uint8_t join_attributes(std::uint8_t dlpAttrs, std::uint8_t category)
{
    const std::uint8_t fileAttrs = dlpAttrs & kAttrFlagsMask;
    if (dlpAttrs & (kAttrDelete | kAttrBusy)) {
        return static_cast<std::uint8_t>(fileAttrs | (dlpAttrs & kAttrArchive));
    }
    // Setting busy is the only way a file can keep an archived live record.
    if (dlpAttrs & kAttrArchive) {
        return static_cast<std::uint8_t>(fileAttrs | kAttrBusy | kAttrArchive);
    }
    return static_cast<std::uint8_t>(fileAttrs | (category & kAttrCategoryMask));
}

void put_type(Bytes& out, const char id[4])
{
    out.insert(out.end(), id, id + 4);
}

} // namespace

bool PalmDatabase::parse(const Bytes& file)
{
    if (file.size() < kHeaderSize) {
        return false;
    }
    const std::uint8_t* h = file.data();

    info = DlpDbInfo();
    const char* name = reinterpret_cast<const char*>(h);
    info.name.assign(name, std::find(name, name + kNameSize, '\0'));
    info.flags = get_be16(h + kAttributes);
    info.version = get_be16(h + kVersion);
    info.modNum = get_be32(h + kModNum);
    std::memcpy(info.type, h + kType, 4);
    std::memcpy(info.creator, h + kCreator, 4);
    created = get_be32(h + kCreated);
    modified = get_be32(h + kModified);
    backedUp = get_be32(h + kBackedUp);
    const std::uint32_t appInfoOffset = get_be32(h + kAppInfo);
    const std::uint32_t sortInfoOffset = get_be32(h + kSortInfo);

    const std::size_t count = get_be16(h + kNumRecords);
    const std::size_t entrySize = is_resource_db() ? kResourceEntrySize : kRecordEntrySize;
    if (kHeaderSize + count * entrySize > file.size()) {
        return false;
    }

    records.clear();
    resources.clear();
    std::vector<std::uint32_t> offsets;
    offsets.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        const std::uint8_t* e = h + kHeaderSize + i * entrySize;
        if (is_resource_db()) {
            DlpResource resource;
            std::memcpy(resource.type, e, 4);
            resource.id = get_be16(e + 4);
            resources.push_back(resource);
            offsets.push_back(get_be32(e + 6));
        } else {
            DlpRecord record;
            record.id = (static_cast<std::uint32_t>(e[5]) << 16) | (e[6] << 8) | e[7];
            split_attributes(e[4], record.attributes, record.category);
            records.push_back(record);
            offsets.push_back(get_be32(e));
        }
        if (offsets.back() > file.size()) {
            return false;
        }
    }

    // Each block runs to the start of the next one that follows it.
    std::vector<std::uint32_t> starts(offsets);
    starts.push_back(appInfoOffset);
    starts.push_back(sortInfoOffset);
    std::sort(starts.begin(), starts.end());
    auto slice = [&](std::uint32_t start, Bytes& out) {
        if (start == 0 || start > file.size()) {
            return;
        }
        auto next = std::upper_bound(starts.begin(), starts.end(), start);
        const std::size_t end = next == starts.end() ? file.size() : std::min<std::size_t>(*next, file.size());
        out.assign(file.begin() + start, file.begin() + static_cast<std::ptrdiff_t>(end));
    };

    slice(appInfoOffset, appInfo);
    slice(sortInfoOffset, sortInfo);
    for (std::size_t i = 0; i < count; ++i) {
        slice(offsets[i], is_resource_db() ? resources[i].data : records[i].data);
    }
    return true;
}

Bytes PalmDatabase::serialize() const
{
    const std::size_t count = is_resource_db() ? resources.size() : records.size();
    const std::size_t entrySize = is_resource_db() ? kResourceEntrySize : kRecordEntrySize;
    std::uint32_t offset = static_cast<std::uint32_t>(kHeaderSize + count * entrySize + kGap);

    Bytes out(kNameSize, 0);
    std::memcpy(out.data(), info.name.data(), std::min(info.name.size(), kNameSize - 1));
    put_be16(out, info.flags);
    put_be16(out, info.version);
    put_be32(out, created);
    put_be32(out, modified);
    put_be32(out, backedUp);
    put_be32(out, info.modNum);
    put_be32(out, appInfo.empty() ? 0 : offset);
    offset += static_cast<std::uint32_t>(appInfo.size());
    put_be32(out, sortInfo.empty() ? 0 : offset);
    offset += static_cast<std::uint32_t>(sortInfo.size());
    put_type(out, info.type);
    put_type(out, info.creator);
    put_be32(out, 0); // unique ID seed
    put_be32(out, 0); // next record list
    put_be16(out, static_cast<std::uint16_t>(count));

    for (std::size_t i = 0; i < count; ++i) {
        if (is_resource_db()) {
            put_type(out, resources[i].type);
            put_be16(out, resources[i].id);
            put_be32(out, offset);
            offset += static_cast<std::uint32_t>(resources[i].data.size());
        } else {
            put_be32(out, offset);
            out.push_back(join_attributes(records[i].attributes, records[i].category));
            out.push_back(static_cast<std::uint8_t>(records[i].id >> 16));
            put_be16(out, static_cast<std::uint16_t>(records[i].id));
            offset += static_cast<std::uint32_t>(records[i].data.size());
        }
    }
    out.insert(out.end(), kGap, 0);
    out.insert(out.end(), appInfo.begin(), appInfo.end());
    out.insert(out.end(), sortInfo.begin(), sortInfo.end());
    for (std::size_t i = 0; i < count; ++i) {
        const Bytes& data = is_resource_db() ? resources[i].data : records[i].data;
        out.insert(out.end(), data.begin(), data.end());
    }
    return out;
}

namespace {

// Days from 0000-03-01 to 1904-01-01 in the proleptic Gregorian calendar.
constexpr std::int32_t kDays0000To1904 = 695361;

// Days between 1904-01-01 and the given civil date (Howard Hinnant's algorithm).
std::int32_t days_since_1904(std::int32_t y, std::uint32_t m, std::uint32_t d)
{
    y -= m <= 2;
    const std::int32_t era = (y >= 0 ? y : y - 399) / 400;
    const auto yoe = static_cast<std::uint32_t>(y - era * 400);
    const std::uint32_t doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
    const std::uint32_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<std::int32_t>(doe) - kDays0000To1904;
}

} // namespace

std::uint32_t palm_seconds_from_date(const DlpDateTime& t)
{
    if (t.year < 1904) {
        return 0;
    }
    const auto days = static_cast<std::uint32_t>(days_since_1904(t.year, t.month, t.day));
    return days * 86400u + t.hour * 3600u + t.minute * 60u + t.second;
}

DlpDateTime palm_date_from_seconds(std::uint32_t seconds)
{
    const std::int32_t z = static_cast<std::int32_t>(seconds / 86400) + kDays0000To1904;
    const std::int32_t era = z / 146097;
    const auto doe = static_cast<std::uint32_t>(z - era * 146097);
    const std::uint32_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const std::uint32_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const std::uint32_t mp = (5 * doy + 2) / 153;

    DlpDateTime t;
    t.day = static_cast<std::uint8_t>(doy - (153 * mp + 2) / 5 + 1);
    t.month = static_cast<std::uint8_t>(mp < 10 ? mp + 3 : mp - 9);
    t.year = static_cast<std::uint16_t>(static_cast<std::int32_t>(yoe) + era * 400 + (t.month <= 2));
    const std::uint32_t rem = seconds % 86400;
    t.hour = static_cast<std::uint8_t>(rem / 3600);
    t.minute = static_cast<std::uint8_t>((rem % 3600) / 60);
    t.second = static_cast<std::uint8_t>(rem % 60);
    return t;
}

} // namespace fujinet::hotsync

#endif // FN_WITH_HOTSYNC
