#include "fujinet/io/devices/slot_catalog.h"

#include <algorithm>
#include <cstddef>

namespace fujinet::io {

namespace {

constexpr std::string_view kNamespace = "config-nio";
constexpr std::string_view kKeyPrefix = "slot-";
constexpr std::uint8_t kRecordVersion = 1;
constexpr std::size_t kEntryHeaderBytes = 3;

std::size_t decimal_digits(std::uint8_t value)
{
    if (value >= 100) return 3;
    if (value >= 10) return 2;
    return 1;
}

} // namespace

SlotCatalog::SlotCatalog(AppStore& store)
    : _store(store)
{}

bool SlotCatalog::parse_slot_key(
    std::string_view ns, std::string_view key, std::uint8_t& index)
{
    if (ns != kNamespace || key.size() != 8 || key.substr(0, 5) != kKeyPrefix) {
        return false;
    }
    unsigned value = 0;
    for (std::size_t i = 5; i < key.size(); ++i) {
        if (key[i] < '0' || key[i] > '9') return false;
        value = value * 10U + static_cast<unsigned>(key[i] - '0');
    }
    if (value > 255U) return false;
    index = static_cast<std::uint8_t>(value);
    return true;
}

std::string SlotCatalog::slot_key(std::uint8_t index)
{
    std::string key{"slot-000"};
    key[5] = static_cast<char>('0' + index / 100U);
    key[6] = static_cast<char>('0' + (index / 10U) % 10U);
    key[7] = static_cast<char>('0' + index % 10U);
    return key;
}

bool SlotCatalog::get(std::uint8_t index, Entry& out)
{
    out = {};
    out.index = index;

    AppStore::ReadResult read{};
    if (!_store.read(kNamespace, slot_key(index), 0, 0xFFFFU, read)) {
        invalidate();
        return false;
    }
    if (!read.exists) {
        if (_indexValid) set_occupied(index, false);
        return true;
    }
    if (_indexValid) set_occupied(index, true);
    if (read.data.size() < 3 || read.data[0] != kRecordVersion) {
        return true;
    }

    out.flags = kEntryValid;
    if ((read.data[1] & 0x01U) != 0) out.flags |= kEntryReadOnly;
    out.uri.assign(read.data.begin() + 2, read.data.end());
    return true;
}

bool SlotCatalog::put(
    std::uint8_t index, std::uint8_t flags, std::string_view uri, Entry& out)
{
    // The private record adds version and flags before the URI and AppStore's
    // per-call write length is u16.
    if (uri.empty() || uri.size() > (0xFFFFU - 2U) ||
        (flags & ~kEntryReadOnly) != 0) {
        return false;
    }

    std::vector<std::uint8_t> record;
    record.reserve(2 + uri.size());
    record.push_back(kRecordVersion);
    record.push_back((flags & kEntryReadOnly) != 0 ? 0x01U : 0x00U);
    record.insert(record.end(), uri.begin(), uri.end());

    AppStore::WriteResult write{};
    if (!_store.write(kNamespace, slot_key(index), 0, record.data(),
                      static_cast<std::uint16_t>(record.size()), write) ||
        write.written != record.size()) {
        invalidate();
        return false;
    }
    if (_indexValid) set_occupied(index, true);

    out = {};
    out.index = index;
    out.flags = static_cast<std::uint8_t>(kEntryValid | (flags & kEntryReadOnly));
    out.uri.assign(uri);
    return true;
}

bool SlotCatalog::remove(std::uint8_t index, bool& deleted)
{
    AppStore::DeleteResult result{};
    if (!_store.remove(kNamespace, slot_key(index), result)) {
        invalidate();
        return false;
    }
    if (_indexValid) set_occupied(index, false);
    deleted = result.deleted;
    return true;
}

bool SlotCatalog::occupied(std::uint8_t index) const
{
    return (_occupancy[index >> 3U] & (1U << (index & 7U))) != 0;
}

void SlotCatalog::set_occupied(std::uint8_t index, bool value)
{
    const auto mask = static_cast<std::uint8_t>(1U << (index & 7U));
    if (value) {
        _occupancy[index >> 3U] |= mask;
    } else {
        _occupancy[index >> 3U] &= static_cast<std::uint8_t>(~mask);
    }
}

bool SlotCatalog::ensure_index()
{
    if (_indexValid) return true;

    _occupancy.fill(0);
    std::uint16_t start = 0;
    for (;;) {
        AppStore::ListResult page{};
        if (!_store.list(kNamespace, start, 0xFFFFU, page)) {
            return false;
        }
        for (const auto& key : page.keys) {
            std::uint8_t index = 0;
            if (parse_slot_key(kNamespace, key, index)) {
                set_occupied(index, true);
            }
        }
        if (!page.more) break;
        if (page.keys.empty()) return false;
        start = static_cast<std::uint16_t>(start + page.keys.size());
    }
    _indexValid = true;
    return true;
}

bool SlotCatalog::range(std::uint8_t lower,
                        std::uint8_t upper,
                        std::uint8_t cursor,
                        std::uint8_t requestFlags,
                        std::uint8_t maxUriBytes,
                        std::uint16_t maxPayloadBytes,
                        RangeResult& out)
{
    out = {};
    if (lower > upper || cursor < lower || cursor > upper || maxUriBytes == 0) {
        return false;
    }
    const auto presenceBytes =
        static_cast<std::size_t>((static_cast<unsigned>(upper) - lower + 8U) / 8U);
    if (maxPayloadBytes < presenceBytes + kEntryHeaderBytes || !ensure_index()) {
        return false;
    }

    out.presence.assign(presenceBytes, 0);
    for (unsigned raw = lower; raw <= upper; ++raw) {
        const auto index = static_cast<std::uint8_t>(raw);
        if (occupied(index)) {
            const auto relative = static_cast<unsigned>(index - lower);
            out.presence[relative >> 3U] |=
                static_cast<std::uint8_t>(1U << (relative & 7U));
        }
    }

    std::size_t used = presenceBytes;
    for (unsigned raw = cursor; raw <= upper; ++raw) {
        const auto index = static_cast<std::uint8_t>(raw);
        if (!occupied(index)) continue;

        Entry entry{};
        if (!get(index, entry)) {
            invalidate();
            return false;
        }
        if ((entry.flags & kEntryValid) == 0 && !occupied(index)) {
            continue;
        }

        if ((entry.flags & kEntryValid) != 0) {
            const auto uriSize = entry.uri.size();
            const auto copySize = std::min<std::size_t>(uriSize, maxUriBytes);
            auto copyBegin = entry.uri.begin();
            if (copySize < uriSize) {
                entry.flags |= kEntryUriTruncated;
                if ((requestFlags & kRequestTailUri) != 0) {
                    copyBegin = entry.uri.end() - static_cast<std::ptrdiff_t>(copySize);
                }
            }
            entry.uri = std::string(
                copyBegin, copyBegin + static_cast<std::ptrdiff_t>(copySize));
        }

        const bool formatted = (requestFlags & kRequestFormatted) != 0;
        const std::size_t entryBytes = formatted
            ? decimal_digits(index) + 2U +
                ((entry.flags & kEntryValid) != 0 ? entry.uri.size() : 9U) + 1U
            : kEntryHeaderBytes + entry.uri.size();
        if (used + entryBytes > maxPayloadBytes) {
            out.more = true;
            out.nextIndex = index;
            return true;
        }
        used += entryBytes;
        out.entries.push_back(std::move(entry));
    }
    return true;
}

void SlotCatalog::note_write(std::string_view ns, std::string_view key)
{
    std::uint8_t index = 0;
    if (_indexValid && parse_slot_key(ns, key, index)) {
        set_occupied(index, true);
    }
}

void SlotCatalog::note_delete(std::string_view ns, std::string_view key)
{
    std::uint8_t index = 0;
    if (_indexValid && parse_slot_key(ns, key, index)) {
        set_occupied(index, false);
    }
}

void SlotCatalog::invalidate()
{
    _indexValid = false;
}

} // namespace fujinet::io
