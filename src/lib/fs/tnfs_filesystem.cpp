
#include "fujinet/fs/filesystem.h"

namespace fujinet::fs {

class TnfsFileSystem final : public IFileSystem {
public:
    FileSystemKind kind() const override { return FileSystemKind::NetworkTnfs; }

    std::string name() const override { return "tnfs"; }

    bool exists(const std::string& path) override {
        // TODO: Implement TNFS exists
        return false;
    }

    bool isDirectory(const std::string& path) override {
        // TODO: Implement TNFS isDirectory
        return false;
    }

    bool createDirectory(const std::string& path) override {
        // TODO: Implement TNFS createDirectory
        return false;
    }

    bool removeFile(const std::string& path) override {
        // TODO: Implement TNFS removeFile
        return false;
    }

    bool removeDirectory(const std::string& path) override {
        // TODO: Implement TNFS removeDirectory
        return false;
    }

    bool rename(const std::string& from, const std::string& to) override {
        // TODO: Implement TNFS rename
        return false;
    }

    std::unique_ptr<IFile> open(const std::string& path, const char* mode) override {
        // TODO: Implement TNFS open
        return nullptr;
    }

    bool stat(const std::string& path, FileInfo& outInfo) override {
        // TODO: Implement TNFS stat
        return false;
    }

    bool listDirectory(const std::string& path, std::vector<FileInfo>& outEntries) override {
        // TODO: Implement TNFS listDirectory
        return false;
    }
};

std::unique_ptr<IFileSystem> make_tnfs_filesystem() {
    return std::make_unique<TnfsFileSystem>();
}

} // namespace fujinet::fs
