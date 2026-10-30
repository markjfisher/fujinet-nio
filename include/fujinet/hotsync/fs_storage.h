#pragma once

#include "fujinet/hotsync/storage.h"

#include <string>

namespace fujinet::fs {
class IFileSystem;
}

namespace fujinet::hotsync {

// Storage in a folder of a NIO filesystem (the persistent one in practice).
class FsStorage : public Storage {
public:
    // root is a path within fs, e.g. "/hotsync".
    FsStorage(fs::IFileSystem& fs, std::string root);

    std::vector<std::string> pending_installs() override;
    bool read_install(const std::string& fileName, Bytes& out, std::size_t maxBytes) override;
    bool mark_installed(const std::string& fileName) override;
    bool write_backup(const std::string& user, const std::string& fileName, const Bytes& data) override;

    // Creates install/ so people can see where to put files.
    bool create_install_folder();
    // Puts a file in install/ for the next sync, replacing one of that name.
    // It is written in incoming/ first, so a sync never sees half of it.
    bool queue_install(const std::string& fileName, const Bytes& data);

private:
    std::string path(const std::string& folder, const std::string& fileName = {}) const;
    bool ensure_dir(const std::string& dir);
    bool read_file(const std::string& file, Bytes& out, std::size_t maxBytes);
    bool write_file(const std::string& dir, const std::string& fileName, const Bytes& data);

    fs::IFileSystem& _fs;
    std::string _root;
};

} // namespace fujinet::hotsync
