
#include "fujinet/fs/filesystem.h"
#include "fujinet/tnfs/tnfs_protocol.h"
#include "fujinet/core/logging.h"

#include <cstring>
#include <memory>

namespace fujinet::fs {

class TnfsFile final : public IFile {
public:
    TnfsFile(std::shared_ptr<tnfs::ITnfsClient> client, int fileHandle)
        : _client(std::move(client))
        , _fileHandle(fileHandle)
        , _position(0)
    {
        FN_LOGI("tnfs_file", "File handle %d created", fileHandle);
    }

    ~TnfsFile() override {
        if (_fileHandle != -1) {
            _client->close(_fileHandle);
            FN_LOGI("tnfs_file", "File handle %d closed", _fileHandle);
        }
    }

    std::size_t read(void* dst, std::size_t maxBytes) override {
        std::size_t bytesRead = _client->read(_fileHandle, dst, maxBytes);
        _position += bytesRead;
        FN_LOGD("tnfs_file", "Read %zu bytes from position %lu", bytesRead, _position);
        return bytesRead;
    }

    std::size_t write(const void* src, std::size_t bytes) override {
        std::size_t bytesWritten = _client->write(_fileHandle, src, bytes);
        _position += bytesWritten;
        FN_LOGD("tnfs_file", "Wrote %zu bytes to position %lu", bytesWritten, _position);
        return bytesWritten;
    }

    bool seek(std::uint64_t offset) override {
        bool success = _client->seek(_fileHandle, static_cast<uint32_t>(offset));
        if (success) {
            _position = offset;
            FN_LOGD("tnfs_file", "Seek to position %lu", offset);
        } else {
            FN_LOGE("tnfs_file", "Seek failed");
        }
        return success;
    }

    std::uint64_t tell() const override {
        return _position;
    }

    bool flush() override {
        // TNFS doesn't have a flush command, so this is a no-op
        FN_LOGD("tnfs_file", "Flush called (no-op)");
        return true;
    }

private:
    std::shared_ptr<tnfs::ITnfsClient> _client;
    int _fileHandle;
    std::uint64_t _position;
};

class TnfsFileSystem final : public IFileSystem {
public:
    explicit TnfsFileSystem(std::shared_ptr<tnfs::ITnfsClient> client)
        : _client(std::move(client))
    {
        FN_LOGI("tnfs_fs", "TNFS filesystem created");
    }

    ~TnfsFileSystem() override {
        FN_LOGI("tnfs_fs", "TNFS filesystem destroyed");
    }

    FileSystemKind kind() const override {
        return FileSystemKind::NetworkTnfs;
    }

    std::string name() const override {
        return "tnfs";
    }

    bool exists(const std::string& path) override {
        FN_LOGD("tnfs_fs", "Checking if path exists: %s", path.c_str());
        return _client->exists(path);
    }

    bool isDirectory(const std::string& path) override {
        FN_LOGD("tnfs_fs", "Checking if path is directory: %s", path.c_str());
        return _client->isDirectory(path);
    }

    bool createDirectory(const std::string& path) override {
        FN_LOGD("tnfs_fs", "Creating directory: %s", path.c_str());
        return _client->createDirectory(path);
    }

    bool removeFile(const std::string& path) override {
        FN_LOGD("tnfs_fs", "Removing file: %s", path.c_str());
        return _client->removeFile(path);
    }

    bool removeDirectory(const std::string& path) override {
        FN_LOGD("tnfs_fs", "Removing directory: %s", path.c_str());
        return _client->removeDirectory(path);
    }

    bool rename(const std::string& from, const std::string& to) override {
        FN_LOGD("tnfs_fs", "Renaming %s to %s", from.c_str(), to.c_str());
        return _client->rename(from, to);
    }

    std::unique_ptr<IFile> open(const std::string& path, const char* mode) override {
        FN_LOGD("tnfs_fs", "Opening file: %s, mode: %s", path.c_str(), mode);

        uint16_t openMode = 0;
        uint16_t createPerms = 0;

        if (strstr(mode, "r") != nullptr) {
            openMode |= tnfs::OPENMODE_READ;
        }
        if (strstr(mode, "w") != nullptr) {
            openMode |= tnfs::OPENMODE_WRITE | tnfs::OPENMODE_WRITE_CREATE | tnfs::OPENMODE_WRITE_TRUNCATE;
        }
        if (strstr(mode, "a") != nullptr) {
            openMode |= tnfs::OPENMODE_WRITE | tnfs::OPENMODE_WRITE_CREATE | tnfs::OPENMODE_WRITE_APPEND;
        }
        if (strstr(mode, "+") != nullptr) {
            openMode |= tnfs::OPENMODE_READWRITE;
        }

        int fileHandle = _client->open(path, openMode, createPerms);
        if (fileHandle == -1) {
            FN_LOGE("tnfs_fs", "Failed to open file: %s", path.c_str());
            return nullptr;
        }

        return std::make_unique<TnfsFile>(_client, fileHandle);
    }

    bool stat(const std::string& path, FileInfo& outInfo) override {
        FN_LOGD("tnfs_fs", "Statting file: %s", path.c_str());

        tnfs::TnfsStat stat;
        if (!_client->stat(path, stat)) {
            return false;
        }

        outInfo.path = path;
        outInfo.isDirectory = stat.isDir;
        outInfo.sizeBytes = stat.filesize;
        outInfo.modifiedTime = std::chrono::system_clock::from_time_t(stat.mTime);

        return true;
    }

    bool listDirectory(const std::string& path, std::vector<FileInfo>& outEntries) override {
        FN_LOGD("tnfs_fs", "Listing directory: %s", path.c_str());

        std::vector<std::string> entries = _client->listDirectory(path);
        if (entries.empty()) {
            return false;
        }

        for (const auto& entryName : entries) {
            std::string entryPath = path + "/" + entryName;
            FileInfo info;
            if (!stat(entryPath, info)) {
                continue;
            }
            outEntries.push_back(info);
        }

        return true;
    }

private:
    std::shared_ptr<tnfs::ITnfsClient> _client;
};

std::unique_ptr<IFileSystem> make_tnfs_filesystem(std::shared_ptr<tnfs::ITnfsClient> client) {
    return std::make_unique<TnfsFileSystem>(std::move(client));
}

std::unique_ptr<IFileSystem> make_tnfs_filesystem(std::unique_ptr<tnfs::ITnfsClient> client) {
    return make_tnfs_filesystem(std::shared_ptr<tnfs::ITnfsClient>(std::move(client)));
}

std::unique_ptr<IFileSystem> make_tnfs_filesystem() {
    // Default implementation that returns a null pointer
    FN_LOGE("tnfs_fs", "make_tnfs_filesystem() with no parameters not implemented");
    return nullptr;
}

} // namespace fujinet::fs
