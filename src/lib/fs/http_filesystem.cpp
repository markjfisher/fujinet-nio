
#include "fujinet/fs/filesystem.h"

namespace fujinet::fs {

class HttpFileSystem final : public IFileSystem {
public:
    FileSystemKind kind() const override { return FileSystemKind::NetworkHttp; }

    std::string name() const override { return "http"; }

    bool exists(const std::string& path) override {
        // TODO: Implement HTTP exists
        return false;
    }

    bool isDirectory(const std::string& path) override {
        // TODO: Implement HTTP isDirectory
        return false;
    }

    bool createDirectory(const std::string& path) override {
        // TODO: Implement HTTP createDirectory
        return false;
    }

    bool removeFile(const std::string& path) override {
        // TODO: Implement HTTP removeFile
        return false;
    }

    bool removeDirectory(const std::string& path) override {
        // TODO: Implement HTTP removeDirectory
        return false;
    }

    bool rename(const std::string& from, const std::string& to) override {
        // TODO: Implement HTTP rename
        return false;
    }

    std::unique_ptr<IFile> open(const std::string& path, const char* mode) override {
        // TODO: Implement HTTP open
        return nullptr;
    }

    bool stat(const std::string& path, FileInfo& outInfo) override {
        // TODO: Implement HTTP stat
        return false;
    }

    bool listDirectory(const std::string& path, std::vector<FileInfo>& outEntries) override {
        // TODO: Implement HTTP listDirectory
        return false;
    }
};

std::unique_ptr<IFileSystem> make_http_filesystem() {
    return std::make_unique<HttpFileSystem>();
}

} // namespace fujinet::fs
