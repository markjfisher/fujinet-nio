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

        char key[] = "slot-000";
        key[5] = static_cast<char>('0' + index / 100U);
        key[6] = static_cast<char>('0' + (index / 10U) % 10U);
        key[7] = static_cast<char>('0' + index % 10U);

        AppStore::ReadResult read{};
        if (!_store.read(kNamespace, key, 0, 0xFFFFU, read)) {
            invalidate();
            return false;
        }
        if (!read.exists) {
            set_occupied(index, false);
            continue;
        }

        Entry entry{};
        entry.index = index;
        if (read.data.size() >= 3 && read.data[0] == kRecordVersion) {
            entry.flags |= kEntryValid;
            if ((read.data[1] & 0x01U) != 0) entry.flags |= kEntryReadOnly;

            const auto uriBegin = read.data.begin() + 2;
            const auto uriSize = read.data.size() - 2;
            const auto copySize = std::min<std::size_t>(uriSize, maxUriBytes);
            auto copyBegin = uriBegin;
            if (copySize < uriSize) {
                entry.flags |= kEntryUriTruncated;
                if ((requestFlags & kRequestTailUri) != 0) {
                    copyBegin = read.data.end() - static_cast<std::ptrdiff_t>(copySize);
                }
            }
            entry.uri.assign(copyBegin, copyBegin + static_cast<std::ptrdiff_t>(copySize));
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
