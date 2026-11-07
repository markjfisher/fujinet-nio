#pragma once

#include "fujinet/io/devices/virtual_device.h"
#include "fujinet/fs/storage_manager.h"
#include "fujinet/io/directory_cache.h"

#include <chrono>
#include <memory>
#include <string>

namespace fujinet::io {

class FileDevice : public VirtualDevice {
public:
    // How long a network file stays open after a ReadFile, waiting for the next one.
    static constexpr std::chrono::milliseconds kReadIdle{5000};

    explicit FileDevice(fs::StorageManager& storage,
                        std::chrono::milliseconds readIdle = kReadIdle);

    IOResponse handle(const IORequest& request) override;
    void poll() override;

private:
    // A host reads a file in many small ReadFile requests. On a network filesystem each open
    // costs round trips (TNFS opens the file and its read-ahead handles; HTTP fetches the
    // whole body), so the file from the last read stays open for the next one: see
    // handle_read_file.
    struct HeldRead {
        fs::IFileSystem* fs{nullptr};
        std::string path;
        std::unique_ptr<fs::IFile> file;
        std::chrono::steady_clock::time_point lastUse{};
    };

    fs::StorageManager& _storage;
    DirectoryCache _dirCache;
    HeldRead _heldRead;
    std::chrono::milliseconds _readIdle;

    std::unique_ptr<fs::IFile> open_for_read(fs::IFileSystem& fs, const std::string& path,
                                             bool& reused);

    IOResponse handle_stat(const IORequest& request);
    IOResponse handle_list_directory(const IORequest& request);
    IOResponse handle_list_pages(const IORequest& request);
    IOResponse handle_read_file(const IORequest& request);
    IOResponse handle_write_file(const IORequest& request);
    IOResponse handle_resolve_path(const IORequest& request);
    IOResponse handle_make_directory(const IORequest& request);
};

} // namespace fujinet::io
