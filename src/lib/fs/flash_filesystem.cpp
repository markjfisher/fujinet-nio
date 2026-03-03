
#include "fujinet/fs/filesystem.h"

namespace fujinet::fs {

class FlashFileSystem final : public IFileSystem {
public:
    FileSystemKind kind() const override { return FileSystemKind::LocalFlash; }

    std::string name() const override { return "flash"; }

    bool exists(const std::string& path) override {
        // TODO: Implement Flash exists
        return false;
    }

    bool isDirectory(const std::string& path) override {
        // TODO: Implement Flash isDirectory
        return false;
    }

    bool createDirectory(const std::string& path) override {
        // TODO: Implement Flash createDirectory
        return false;
    }

    bool removeFile(const std::string& path) override {
        // TODO: Implement Flash removeFile
        return false;
    }

    bool removeDirectory(const std::string& path) override {
        // TODO: Implement Flash removeDirectory
        return false;
    }

    bool rename(const std::string& from, const std::string& to) override {
        // TODO: Implement Flash rename
        return false;
    }

    std::unique_ptr<IFile> open(const std::string& path, const char* mode) override {
        // TODO: Implement Flash open
        return nullptr;
    }

    bool stat(const std::string& path, FileInfo& outInfo) override {
        // TODO: Implement Flash stat
        return false;
    }

    bool listDirectory(const std::string& path, std::vector<FileInfo>& outEntries) override {
        // TODO: Implement Flash listDirectory
        return false;
    }
};

std::unique_ptr<IFileSystem> make_flash_filesystem() {
    return std::make_unique<FlashFileSystem>();
}

} // namespace fujinet::fs
