#pragma once

#include "fujinet/fs/filesystem.h"
#include "fujinet/fs/storage_manager.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace fujinet::io {

class AppStore {
public:
    struct Stat {
        bool exists{false};
        std::uint64_t sizeBytes{0};
        std::uint64_t modifiedUnixTime{0};
    };

    struct ReadResult {
        bool exists{false};
        bool eof{true};
        std::uint32_t offset{0};
        std::vector<std::uint8_t> data;
    };

    struct WriteResult {
        std::uint32_t offset{0};
        std::uint16_t written{0};
    };

    struct DeleteResult {
        bool deleted{false};
    };

    struct ListResult {
        bool more{false};
        std::uint16_t startIndex{0};
        std::vector<std::string> keys;
    };

    explicit AppStore(fs::StorageManager& storage);

    bool available() const;
    std::string backing_fs_name() const;

    bool stat(std::string_view ns, std::string_view key, Stat& out);
    bool read(std::string_view ns, std::string_view key, std::uint32_t offset, std::uint16_t maxBytes, ReadResult& out);
    bool write(std::string_view ns, std::string_view key, std::uint32_t offset, const std::uint8_t* data, std::uint16_t len, WriteResult& out);
    bool remove(std::string_view ns, std::string_view key, DeleteResult& out);
    bool list(std::string_view ns, std::uint16_t startIndex, std::uint16_t maxPayloadBytes, ListResult& out);
    bool rename(std::string_view ns, std::string_view oldKey, std::string_view newKey);

    bool get_current_host(std::string* uri, std::string* displayPath = nullptr);
    bool set_current_host(std::string_view spec);
    bool resolve_target(std::string_view spec, std::string& uri, std::string* displayPath = nullptr);

    static bool valid_namespace(std::string_view ns);
    static bool valid_key(std::string_view key);

private:
    fs::IFileSystem* backing_fs() const;
    std::string key_path(std::string_view ns, std::string_view key) const;
    std::string namespace_path(std::string_view ns) const;
    bool ensure_namespace_dir(std::string_view ns);
    bool raw_write(std::string_view ns, std::string_view key, std::uint32_t offset, const std::uint8_t* data, std::uint16_t len, WriteResult& out);
    bool update_host_history(std::string_view uri);

    fs::StorageManager& _storage;
};

} // namespace fujinet::io
