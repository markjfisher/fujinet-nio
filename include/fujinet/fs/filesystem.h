#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <chrono>
#include <memory>

namespace fujinet::fs {

enum class FileSystemKind {
    LocalFlash,
    LocalSD,
    HostPosix,
    NetworkTnfs,
    NetworkSmb,
    NetworkFtp,
    NetworkHttp,
    Unknown,
};

struct FileInfo {
    std::string path;   // Path relative to the root of this filesystem ("/", "/foo/bar.atr")
    bool        isDirectory{false};
    std::uint64_t sizeBytes{0};

    // Optional; can be left as a default-constructed time_point if not available.
    std::chrono::system_clock::time_point modifiedTime{};
};

// Simple file abstraction; streaming open file handle.
class IFile {
public:
    virtual ~IFile() = default;

    // Read up to maxBytes into dst, returns number of bytes actually read (0 on EOF or error).
    virtual std::size_t read(void* dst, std::size_t maxBytes) = 0;

    // Write up to bytes from src, returns number of bytes actually written.
    virtual std::size_t write(const void* src, std::size_t bytes) = 0;

    // Seek to absolute offset (from beginning). Returns false on failure.
    virtual bool seek(std::uint64_t offset) = 0;

    // Current position from beginning (or 0 on failure).
    virtual std::uint64_t tell() const = 0;

    // Flush buffered data to underlying storage if applicable.
    virtual bool flush() = 0;
};

// Abstract filesystem mounted at some root.
// All paths are POSIX-style within this FS ("/", "/dir/file", etc.).
class IFileSystem {
public:
    virtual ~IFileSystem() = default;

    // High-level classification: SD vs flash vs network, etc.
    virtual FileSystemKind kind() const = 0;

    // Stable, unique-ish identifier within the system, e.g. "flash", "sd0", "host".
    virtual std::string name() const = 0;

    // Basic file / directory queries.
    virtual bool exists(const std::string& path) = 0;
    virtual bool isDirectory(const std::string& path) = 0;

    // Directory & file manipulation.
    virtual bool createDirectory(const std::string& path) = 0;
    virtual bool removeFile(const std::string& path) = 0;
    virtual bool removeDirectory(const std::string& path) = 0;
    virtual bool rename(const std::string& from, const std::string& to) = 0;

    // Open a file using C stdio-style mode strings ("rb", "wb", "ab", "r+b", etc.).
    // Returns nullptr on failure.
    virtual std::unique_ptr<IFile> open(
        const std::string& path,
        const char* mode
    ) = 0;

    // Query metadata for a single path.
    // Returns false if path does not exist or on error.
    virtual bool stat(
        const std::string& path,
        FileInfo& outInfo
    ) = 0;

    // List entries in a directory. `path` is relative to this FS root.
    // Returns false on error.
    virtual bool listDirectory(
        const std::string& path,
        std::vector<FileInfo>& outEntries
    ) = 0;
};

} // namespace fujinet::fs
