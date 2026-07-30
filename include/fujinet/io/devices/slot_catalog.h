#pragma once

#include "fujinet/io/devices/app_store.h"

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace fujinet::io {

class SlotCatalog {
public:
    static constexpr std::uint8_t kRequestTailUri = 0x01U;
    static constexpr std::uint8_t kRequestFormatted = 0x02U;
    static constexpr std::uint8_t kResponseMore = 0x01U;
    static constexpr std::uint8_t kEntryValid = 0x01U;
    static constexpr std::uint8_t kEntryReadOnly = 0x02U;
    static constexpr std::uint8_t kEntryUriTruncated = 0x04U;

    struct Entry {
        std::uint8_t index{0};
        std::uint8_t flags{0};
        std::string uri;
    };

    struct RangeResult {
        bool more{false};
        std::uint8_t nextIndex{0};
        std::vector<std::uint8_t> presence;
        std::vector<Entry> entries;
    };

    explicit SlotCatalog(AppStore& store);

    bool range(std::uint8_t lower,
               std::uint8_t upper,
               std::uint8_t cursor,
               std::uint8_t requestFlags,
               std::uint8_t maxUriBytes,
               std::uint16_t maxPayloadBytes,
               RangeResult& out);

    void note_write(std::string_view ns, std::string_view key);
    void note_delete(std::string_view ns, std::string_view key);
    void invalidate();

private:
    bool ensure_index();
    static bool parse_slot_key(std::string_view ns, std::string_view key, std::uint8_t& index);
    bool occupied(std::uint8_t index) const;
    void set_occupied(std::uint8_t index, bool value);

    AppStore& _store;
    std::array<std::uint8_t, 32> _occupancy{};
    bool _indexValid{false};
};

} // namespace fujinet::io
