
#include "fujinet/fs/filesystem.h"

namespace fujinet::fs {

class SdFileSystem final : public IFileSystem {
public:
    FileSystemKind kind() const override { return FileSystemKind::LocalSD; }

    std::string name() const override { return "sd"; }

    bool exists(const std::string& path) override {
        // TODO: Implement SD card exists
        return false;
    }

    bool isDirectory(const std::string& path) override {
        // TODO: Implement SD card isDirectory
        return false;
    }

    bool createDirectory(const std::string& path) override {
        // TODO: Implement SD card createDirectory
        return false;
    }

    bool removeFile(const std::string& path) override {
        // TODO: Implement SD card removeFile
        return false;
    }

    bool removeDirectory(const std::string& path) override {
        // TODO: Implement SD card removeDirectory
        return false;
    }

    bool rename(const std::string& from, const std::string& to) override {
        // TODO: Implement SD card rename
        return false;
    }

    std::unique_ptr<IFile> open(const std::string& path, const char* mode) override {
        // TODO: Implement SD card open
        return nullptr;
    }

    bool stat(const std::string& path, FileInfo& outInfo) override {
        // TODO: Implement SD card stat
        return false;
    }

    bool listDirectory(const std::string& path, std::vector<FileInfo>& outEntries) override {
        // TODO: Implement SD card listDirectory
        return false;
    }
};

std::unique_ptr<IFileSystem> make_sd_filesystem() {
    return std::make_unique<SdFileSystem>();
}

} // namespace fujinet::fs
