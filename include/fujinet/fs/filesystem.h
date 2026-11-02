
#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <chrono>
#include <memory>

namespace fujinet::tnfs {
class ITnfsClient;
}

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

// True for filesystems reached over the network. They may be unreachable until
// the platform link is up, so a failed probe does not prove a path is missing.
constexpr bool is_network_kind(FileSystemKind kind)
{
    switch (kind) {
    case FileSystemKind::NetworkTnfs:
    case FileSystemKind::NetworkSmb:
    case FileSystemKind::NetworkFtp:
    case FileSystemKind::NetworkHttp:
        return true;
    case FileSystemKind::LocalFlash:
    case FileSystemKind::LocalSD:
    case FileSystemKind::HostPosix:
    case FileSystemKind::Unknown:
        return false;
    }
    return false;
}

// Directory ordering for range listings. Name order is the basename, ASCII
// case-insensitive; NameDirsFirst additionally puts directories before files.
enum class DirSort : std::uint8_t {
    None,           // filesystem order
    Name,
    NameDirsFirst,
};

enum class ListStatus : std::uint8_t {
    Ok,
    Error,
    Timeout, // a bounded network listing ran out of time
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
//
// Tasks: a local filesystem (create_stdio_filesystem: host, flash, sd0) may be
// used from more than one task at once, as the core and the HotSync service
// do. It keeps nothing between calls but its root, each IFile is a handle of
// its own, and the layer beneath serialises its own work: the kernel on POSIX;
// on ESP32, FatFs built reentrant (a lock per volume) and LittleFS's lock per
// mount. As on any OS, two writers to one file interleave, and a listing taken
// while another task adds files may or may not show them. Network filesystems
// (TNFS, HTTP) hold a session and are for the core task only.
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

    // List entries [start, start+count) of a directory in the requested order and report the
    // directory's total entry count. Pass count == npos for "everything from start".
    // The default is listDirectory() + sort + slice, so every filesystem works; filesystems
    // that can page cheaply (TNFS) override it together with supportsRangeListing().
    virtual ListStatus listDirectoryRange(
        const std::string& path,
        DirSort sort,
        std::size_t start,
        std::size_t count,
        std::vector<FileInfo>& out,
        std::size_t& total
    );

    // True when listDirectoryRange() fetches only the requested range (callers should then
    // request small ranges and cache them; otherwise request the whole directory once).
    virtual bool supportsRangeListing() const { return false; }
};

// Sort by basename (ASCII case-insensitive, ties by exact name), optionally directories first.
void sort_directory_entries(std::vector<FileInfo>& entries, DirSort sort);
// Case-insensitive ASCII "less" on names, ties broken by exact comparison.
bool name_less_ascii_ci(const std::string& a, const std::string& b);



} // namespace fujinet::fs
