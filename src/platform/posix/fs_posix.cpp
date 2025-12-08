#include "fujinet/fs/filesystem.h"
#include "fujinet/platform/posix/filesystem_factory.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <memory>
#include <system_error>
#include <chrono>

#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>

namespace fujinet::platform::posix {

namespace {

using fujinet::fs::FileInfo;
using fujinet::fs::FileSystemKind;
using fujinet::fs::IFile;
using fujinet::fs::IFileSystem;

// ----------------------
// PosixFile
// ----------------------

class PosixFile : public IFile {
public:
    explicit PosixFile(std::FILE* fp)
        : _fp(fp)
    {}

    ~PosixFile() override {
        if (_fp) {
            std::fclose(_fp);
        }
    }

    std::size_t read(void* dst, std::size_t maxBytes) override
    {
        if (!_fp || maxBytes == 0) {
            return 0;
        }
        return std::fread(dst, 1, maxBytes, _fp);
    }

    std::size_t write(const void* src, std::size_t bytes) override
    {
        if (!_fp || bytes == 0) {
            return 0;
        }
        return std::fwrite(src, 1, bytes, _fp);
    }

    bool seek(std::uint64_t offset) override
    {
        if (!_fp) {
            return false;
        }
        // On POSIX, fseek expects long; use fseeko if available.
        if (::fseeko(_fp, static_cast<off_t>(offset), SEEK_SET) != 0) {
            return false;
        }
        return true;
    }

    std::uint64_t tell() const override
    {
        if (!_fp) {
            return 0;
        }
        auto pos = ::ftello(_fp);
        if (pos < 0) {
            return 0;
        }
        return static_cast<std::uint64_t>(pos);
    }

    bool flush() override
    {
        if (!_fp) {
            return false;
        }
        return std::fflush(_fp) == 0;
    }

private:
    std::FILE* _fp{nullptr};
};

// ----------------------
// PosixFileSystem
// ----------------------

class PosixFileSystem : public IFileSystem {
public:
    PosixFileSystem(std::string root, std::string name)
        : _root(std::move(root))
        , _name(std::move(name))
    {
        // Normalize root: remove trailing slash if present.
        if (!_root.empty() && _root.back() == '/') {
            _root.pop_back();
        }

        // Ensure root exists
        struct stat st{};
        if (stat(_root.c_str(), &st) != 0) {
            // mkdir -p equivalent (single level)
            ::mkdir(_root.c_str(), 0775);
        }
    }

    FileSystemKind kind() const override {
        return FileSystemKind::HostPosix;
    }

    std::string name() const override {
        return _name;
    }

    bool exists(const std::string& path) override
    {
        struct stat st{};
        return ::stat(toFullPath(path).c_str(), &st) == 0;
    }

    bool isDirectory(const std::string& path) override
    {
        struct stat st{};
        if (::stat(toFullPath(path).c_str(), &st) != 0) {
            return false;
        }
        return S_ISDIR(st.st_mode);
    }

    bool createDirectory(const std::string& path) override
    {
        auto full = toFullPath(path);
        // 0777 with umask.
        if (::mkdir(full.c_str(), 0777) == 0) {
            return true;
        }
        // Already exists and is dir is considered success.
        if (errno == EEXIST) {
            return isDirectory(path);
        }
        return false;
    }

    bool removeFile(const std::string& path) override
    {
        auto full = toFullPath(path);
        return ::unlink(full.c_str()) == 0;
    }

    bool removeDirectory(const std::string& path) override
    {
        auto full = toFullPath(path);
        return ::rmdir(full.c_str()) == 0;
    }

    bool rename(const std::string& from, const std::string& to) override
    {
        auto src = toFullPath(from);
        auto dst = toFullPath(to);
        return ::rename(src.c_str(), dst.c_str()) == 0;
    }

    std::unique_ptr<IFile> open(const std::string& path, const char* mode) override
    {
        if (!mode) {
            return nullptr;
        }
        auto full = toFullPath(path);
        std::FILE* fp = std::fopen(full.c_str(), mode);
        if (!fp) {
            return nullptr;
        }
        return std::make_unique<PosixFile>(fp);
    }

    bool listDirectory(const std::string& path, std::vector<FileInfo>& outEntries) override
    {
        auto full = toFullPath(path);
        DIR* dir = ::opendir(full.c_str());
        if (!dir) {
            return false;
        }

        outEntries.clear();

        while (auto* ent = ::readdir(dir)) {
            const char* name = ent->d_name;
            if (!name) {
                continue;
            }

            // Skip "." and ".."
            if (std::strcmp(name, ".") == 0 || std::strcmp(name, "..") == 0) {
                continue;
            }

            FileInfo info{};
            // Build relative path within FS: path + "/" + name (minus leading slash normalisation).
            std::string relPath = joinPaths(path, name);
            info.path = relPath;

            auto fullChild = toFullPath(relPath);

            struct stat st{};
            if (::stat(fullChild.c_str(), &st) == 0) {
                info.isDirectory = S_ISDIR(st.st_mode);
                if (S_ISREG(st.st_mode)) {
                    info.sizeBytes = static_cast<std::uint64_t>(st.st_size);
                }
                if (st.st_mtime != 0) {
                    info.modifiedTime = std::chrono::system_clock::from_time_t(st.st_mtime);
                }
            }

            outEntries.push_back(std::move(info));
        }

        ::closedir(dir);
        return true;
    }

private:
    std::string toFullPath(const std::string& path) const
    {
        // Normalize: empty or "." → root
        if (path.empty() || path == ".") {
            return _root;
        }

        if (path.front() == '/') {
            // Already absolute within FS; append to root
            if (_root.empty()) {
                return path;
            }
            return _root + path;
        }

        // Relative path: root + "/" + path
        if (_root.empty()) {
            return path;
        }
        return _root + "/" + path;
    }

    static std::string joinPaths(const std::string& base, const std::string& name)
    {
        if (base.empty() || base == "/") {
            return std::string("/") + name;
        }
        if (base.back() == '/') {
            return base + name;
        }
        return base + "/" + name;
    }

    std::string _root;  // host filesystem root for this volume, e.g. "./fujinet-root"
    std::string _name;  // logical name, e.g. "host" or "flash"
};

// Factory function you can call from your POSIX bootstrap.
std::unique_ptr<IFileSystem> create_posix_filesystem(
    const std::string& rootDir,
    const std::string& name)
{
    return std::make_unique<PosixFileSystem>(rootDir, name);
}

} // namespace

// Public factory API (put this in a header later)
std::unique_ptr<fujinet::fs::IFileSystem>
create_host_filesystem(const std::string& rootDir, const std::string& name)
{
    return create_posix_filesystem(rootDir, name);
}

} // namespace fujinet::platform::posix
