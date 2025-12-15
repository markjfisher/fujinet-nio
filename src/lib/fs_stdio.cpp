#include "fujinet/fs/fs_stdio.h"
#include "fujinet/core/logging.h"

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <memory>
#include <sys/stat.h>
#include <dirent.h>   // ESP-IDF provides this via newlib too
#include <errno.h>

namespace fujinet::fs {

using fujinet::fs::FileInfo;
using fujinet::fs::FileSystemKind;
using fujinet::fs::IFile;
using fujinet::fs::IFileSystem;

static constexpr const char* TAG = "fs";

// ----------------------
// StdioFile
// ----------------------
class StdioFile : public IFile {
public:
    explicit StdioFile(std::FILE* fp)
        : _fp(fp)
    {}

    ~StdioFile() override {
        if (_fp) {
            std::fclose(_fp);
        }
    }

    std::size_t read(void* dst, std::size_t maxBytes) override
    {
        if (!_fp || maxBytes == 0) return 0;
        return std::fread(dst, 1, maxBytes, _fp);
    }

    std::size_t write(const void* src, std::size_t bytes) override
    {
        if (!_fp || bytes == 0) return 0;
        return std::fwrite(src, 1, bytes, _fp);
    }

    bool seek(std::uint64_t offset) override
    {
        if (!_fp) return false;
        return std::fseek(_fp, static_cast<long>(offset), SEEK_SET) == 0;
    }

    std::uint64_t tell() const override
    {
        if (!_fp) return 0;
        long pos = std::ftell(_fp);
        return pos < 0 ? 0 : static_cast<std::uint64_t>(pos);
    }

    bool flush() override
    {
        if (!_fp) return false;
        return std::fflush(_fp) == 0;
    }

private:
    std::FILE* _fp{};
};

// ----------------------
// StdioFileSystem
// ----------------------
class StdioFileSystem : public IFileSystem {
public:
    StdioFileSystem(std::string rootDir, std::string name, FileSystemKind kind)
        : _root(std::move(rootDir))
        , _name(std::move(name))
        , _kind(kind)
    {
        // normalise root: no trailing slash (except "/" itself)
        if (!_root.empty() && _root.size() > 1 && _root.back() == '/') {
            _root.pop_back();
        }
        // Do we need this back in:
        // // Ensure root exists
        // struct stat st{};
        // if (stat(_root.c_str(), &st) != 0) {
        //     // mkdir -p equivalent (single level)
        //     ::mkdir(_root.c_str(), 0775);
        // }
    }

    FileSystemKind kind() const override { return _kind; }
    std::string    name() const override { return _name; }

    bool exists(const std::string& path) override
    {
        FileInfo info{};
        return stat(path, info);
    }

    bool isDirectory(const std::string& path) override
    {
        FileInfo info{};
        return stat(path, info) && info.isDirectory;
    }

    bool createDirectory(const std::string& path) override
    {
        // minimally: try mkdir on the full path.
        // You can add recursive creation helper if needed.
        const auto fp = fullPath(path);
        return ::mkdir(fp.c_str(), 0755) == 0 || (errno == EEXIST && isDirectory(path));
    }

    bool removeFile(const std::string& path) override
    {
        return ::remove(fullPath(path).c_str()) == 0;
    }

    bool removeDirectory(const std::string& path) override
    {
        return ::rmdir(fullPath(path).c_str()) == 0;
    }

    bool rename(const std::string& from, const std::string& to) override
    {
        return ::std::rename(fullPath(from).c_str(),
                             fullPath(to).c_str()) == 0;
    }

    std::unique_ptr<IFile> open(const std::string &path,
                                const char *mode) override
    {
        const std::string full = fullPath(path);
        auto fp = std::fopen(full.c_str(), mode);

        if (!fp)
        {
            const int e = errno;
            FN_LOGE(TAG,
                    "open failed: fs='%s' mode='%s' path='%s' full='%s' errno=%d (%s)",
                    name().c_str(),
                    mode ? mode : "(null)",
                    path.c_str(),
                    full.c_str(),
                    e,
                    std::strerror(e));

            return nullptr;
        }

        return std::make_unique<StdioFile>(fp);
    }

    bool stat(const std::string& path, FileInfo& out) override
    {
        struct stat st{};
        if (::stat(fullPath(path).c_str(), &st) != 0) {
            return false;
        }

        out.path = path;
        out.isDirectory = S_ISDIR(st.st_mode);
        out.sizeBytes   = S_ISREG(st.st_mode)
                            ? static_cast<std::uint64_t>(st.st_size)
                            : 0;

        if (st.st_mtime != 0) {
            out.modifiedTime =
                std::chrono::system_clock::from_time_t(st.st_mtime);
        } else {
            out.modifiedTime = {};
        }

        return true;
    }

    bool listDirectory(const std::string &path,
                       std::vector<FileInfo> &outEntries) override
    {
        const std::string dirPath = fullPath(path);
        DIR *dir = ::opendir(dirPath.c_str());
        if (!dir)
        {
            return false;
        }

        outEntries.clear();

        struct dirent *de;
        while ((de = ::readdir(dir)) != nullptr)
        {
            const char *name = de->d_name;
            if (!name)
                continue;

            // Skip '.' and '..'
            if (std::strcmp(name, ".") == 0 || std::strcmp(name, "..") == 0)
            {
                continue;
            }

            const std::string childPath = join(path, name);

            FileInfo info{};
            // stat() is now the authoritative metadata fill.
            // If stat fails (race / broken entry), we still include the path
            // but leave metadata at defaults.
            if (!stat(childPath, info))
            {
                info.path = childPath;
            }

            outEntries.push_back(std::move(info));
        }

        ::closedir(dir);
        return true;
    }

private:
    std::string fullPath(const std::string& rel) const
    {
        // rel is expected to be absolute within the FS ("/", "/foo", etc.)
        if (rel.empty() || rel == "/") {
            return _root;
        }

        if (rel.front() == '/') {
            return _root + rel;
        }
        return _root + "/" + rel;
    }

    static std::string join(const std::string& dir, const std::string& name)
    {
        if (dir.empty() || dir == "/") {
            return "/" + name;
        }
        if (dir.back() == '/') {
            return dir + name;
        }
        return dir + "/" + name;
    }

    std::string    _root;
    std::string    _name;
    FileSystemKind _kind;
};

// Factory
std::unique_ptr<fujinet::fs::IFileSystem>
create_stdio_filesystem(const std::string& rootDir,
                        const std::string& name,
                        FileSystemKind kind)
{
    FN_LOGI(TAG, "Creating stdio filesystem '%s' at root '%s'",
        name.c_str(), rootDir.c_str());
    return std::make_unique<StdioFileSystem>(rootDir, name, kind);
}

} // namespace fujinet::fs
