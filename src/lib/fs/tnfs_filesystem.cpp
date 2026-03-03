
#include "fujinet/fs/filesystem.h"
#include "fujinet/core/logging.h"

#include <cstring>
#include <vector>

namespace fujinet::fs {

class TnfsFile final : public IFile {
public:
    TnfsFile() = default;

    ~TnfsFile() override = default;

    std::size_t read(void* dst, std::size_t maxBytes) override {
        FN_LOGE("tnfs", "read() not implemented");
        return 0;
    }

    std::size_t write(const void* src, std::size_t bytes) override {
        FN_LOGE("tnfs", "write() not implemented");
        return 0;
    }

    bool seek(std::uint64_t offset) override {
        FN_LOGE("tnfs", "seek() not implemented");
        return false;
    }

    std::uint64_t tell() const override {
        FN_LOGE("tnfs", "tell() not implemented");
        return 0;
    }

    bool flush() override {
        FN_LOGE("tnfs", "flush() not implemented");
        return false;
    }
};

class TnfsFileSystem final : public IFileSystem {
public:
    TnfsFileSystem(std::string host, std::uint16_t port = 16384, std::string mountPath = "/", std::string user = "", std::string password = "")
        : _host(std::move(host))
        , _port(port)
        , _mountPath(std::move(mountPath))
        , _user(std::move(user))
        , _password(std::move(password))
    {
        FN_LOGI("tnfs", "TNFS filesystem created for host: %s:%d, mount path: %s", _host.c_str(), _port, _mountPath.c_str());
    }

    ~TnfsFileSystem() override = default;

    FileSystemKind kind() const override { return FileSystemKind::NetworkTnfs; }

    std::string name() const override { return "tnfs"; }

    bool exists(const std::string& path) override {
        FN_LOGE("tnfs", "exists() not implemented for path: %s", path.c_str());
        return false;
    }

    bool isDirectory(const std::string& path) override {
        FN_LOGE("tnfs", "isDirectory() not implemented for path: %s", path.c_str());
        return false;
    }

    bool createDirectory(const std::string& path) override {
        FN_LOGE("tnfs", "createDirectory() not implemented for path: %s", path.c_str());
        return false;
    }

    bool removeFile(const std::string& path) override {
        FN_LOGE("tnfs", "removeFile() not implemented for path: %s", path.c_str());
        return false;
    }

    bool removeDirectory(const std::string& path) override {
        FN_LOGE("tnfs", "removeDirectory() not implemented for path: %s", path.c_str());
        return false;
    }

    bool rename(const std::string& from, const std::string& to) override {
        FN_LOGE("tnfs", "rename() not implemented from %s to %s", from.c_str(), to.c_str());
        return false;
    }

    std::unique_ptr<IFile> open(const std::string& path, const char* mode) override {
        FN_LOGE("tnfs", "open() not implemented for path: %s, mode: %s", path.c_str(), mode);
        return nullptr;
    }

    bool stat(const std::string& path, FileInfo& outInfo) override {
        FN_LOGE("tnfs", "stat() not implemented for path: %s", path.c_str());
        return false;
    }

    bool listDirectory(const std::string& path, std::vector<FileInfo>& outEntries) override {
        FN_LOGE("tnfs", "listDirectory() not implemented for path: %s", path.c_str());
        return false;
    }

private:
    std::string _host;
    std::uint16_t _port;
    std::string _mountPath;
    std::string _user;
    std::string _password;
};

std::unique_ptr<IFileSystem> make_tnfs_filesystem() {
    return std::make_unique<TnfsFileSystem>("localhost");
}

std::unique_ptr<IFileSystem> make_tnfs_filesystem(
    const std::string& host,
    std::uint16_t port,
    const std::string& mountPath,
    const std::string& user,
    const std::string& password) {
    return std::make_unique<TnfsFileSystem>(host, port, mountPath, user, password);
}

} // namespace fujinet::fs
