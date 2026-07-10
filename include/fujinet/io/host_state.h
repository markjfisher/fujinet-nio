#pragma once

#include "fujinet/fs/storage_manager.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace fujinet::io {

class HostState {
public:
    static constexpr const char* kNamespace = "fujinet-nio";
    static constexpr const char* kCurrentHostKey = "current-host";
    static constexpr const char* kCurrentDisplayPathKey = "current-display-path";
    static constexpr const char* kHostHistoryKey = "host-history";
    static constexpr std::size_t kHostHistoryMax = 32;

    explicit HostState(fs::StorageManager& storage);

    bool get_current_host(std::string* uri, std::string* displayPath = nullptr);
    bool set_current_host(std::string_view spec);
    bool set_current_host_index(std::string_view indexText);
    bool set_current_host_index(std::size_t index);
    bool delete_history_index(std::string_view indexText);
    bool delete_history_index(std::size_t index);
    bool format_history(std::string* text);
    bool resolve_target(std::string_view spec, std::string& uri, std::string* displayPath = nullptr);

private:
    bool parse_index(std::string_view text, std::size_t* index) const;
    bool read_history(std::vector<std::string>& entries);
    bool write_history(const std::vector<std::string>& entries);
    bool write_value(std::string_view key, std::string_view value);
    bool update_history(std::string_view uri);

    fs::StorageManager& _storage;
};

} // namespace fujinet::io
