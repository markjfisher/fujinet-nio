#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "fujinet/fs/filesystem.h"
#include "fujinet/fs/uri_parser.h"

namespace fujinet::fs {

// Simple name-based registry for filesystems.
class StorageManager {
public:
    StorageManager() = default;
    ~StorageManager() = default;

    StorageManager(const StorageManager&) = delete;
    StorageManager& operator=(const StorageManager&) = delete;

    // Registers a filesystem under the given name (e.g. "host", "flash", "sd0") taken from the fs.
    // Returns false if that name already exists or fs is null.
    bool registerFileSystem(std::unique_ptr<IFileSystem> fs);

    // Remove a filesystem by name. Returns false if not found.
    bool unregisterFileSystem(const std::string& name);

    // Lookup by name.
    IFileSystem*       get(const std::string& name);
    const IFileSystem* get(const std::string& name) const;

    // Optional helpers for enumeration.
    std::vector<std::string> listNames() const;

    // Lookup by URI scheme
    IFileSystem*       getByScheme(const std::string& scheme);
    const IFileSystem* getByScheme(const std::string& scheme) const;

    // Parse URI and get filesystem and path
    std::pair<IFileSystem*, std::string> resolveUri(const std::string& uri);

private:
    std::unordered_map<std::string, std::unique_ptr<IFileSystem>> _fileSystems;
};

} // namespace fujinet::fs
