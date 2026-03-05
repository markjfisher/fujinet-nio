#include "fujinet/fs/storage_manager.h"
#include "fujinet/fs/filesystem.h"
#include "fujinet/fs/uri_parser.h"

namespace fujinet::fs {

bool StorageManager::registerFileSystem(std::unique_ptr<IFileSystem> fs)
{
    if (!fs) {
        return false;
    }

    std::string key = fs->name();

    auto [it, inserted] = _fileSystems.emplace(std::move(key), std::move(fs));
    return inserted;
}


bool StorageManager::unregisterFileSystem(const std::string& name)
{
    return _fileSystems.erase(name) > 0;
}

IFileSystem* StorageManager::get(const std::string& name)
{
    auto it = _fileSystems.find(name);
    return (it == _fileSystems.end()) ? nullptr : it->second.get();
}

const IFileSystem* StorageManager::get(const std::string& name) const
{
    auto it = _fileSystems.find(name);
    return (it == _fileSystems.end()) ? nullptr : it->second.get();
}

std::vector<std::string> StorageManager::listNames() const
{
    std::vector<std::string> out;
    out.reserve(_fileSystems.size());
    for (auto const& [name, _] : _fileSystems) {
        out.push_back(name);
    }
    return out;
}

IFileSystem* StorageManager::getByScheme(const std::string& scheme)
{
    // For now, we assume scheme is the same as filesystem name
    return get(scheme);
}

const IFileSystem* StorageManager::getByScheme(const std::string& scheme) const
{
    // For now, we assume scheme is the same as filesystem name
    return get(scheme);
}

std::pair<IFileSystem*, std::string> StorageManager::resolveUri(const std::string& uri)
{
    auto parts = parse_uri(uri);
    
    // If scheme is provided, try to find a filesystem with that scheme
    if (!parts.scheme.empty()) {
        auto fs = getByScheme(parts.scheme);
        if (fs) {
            // For schemes with authority (e.g., tnfs://host:port/path, http://host/path),
            // reconstruct the full URI with authority preserved.
            // This is important for TNFS which needs host:port to connect.
            if (!parts.authority.empty()) {
                // Reconstruct: scheme://authority/path
                std::string fullPath = parts.scheme + "://" + parts.authority + parts.path;
                return {fs, fullPath};
            }
            return {fs, parts.path};
        }
    } else {
        // No scheme, treat as host filesystem path
        auto fs = get("host");
        if (fs) {
            return {fs, parts.path};
        }
    }
    
    return {nullptr, ""};
}

} // namespace fujinet::fs
