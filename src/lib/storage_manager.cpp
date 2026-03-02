#include "fujinet/fs/storage_manager.h"
#include "fujinet/fs/filesystem.h"

namespace fujinet::fs {

UriParts parse_uri(const std::string& uri)
{
    UriParts parts;
    
    // Find scheme
    auto scheme_end = uri.find(':');
    if (scheme_end != std::string::npos) {
        parts.scheme = uri.substr(0, scheme_end);
        
        // Check if there's an authority (//)
        if (scheme_end + 2 < uri.size() && uri[scheme_end + 1] == '/' && uri[scheme_end + 2] == '/') {
            // Parse authority and path
            auto path_start = uri.find('/', scheme_end + 3);
            if (path_start != std::string::npos) {
                parts.authority = uri.substr(scheme_end + 3, path_start - (scheme_end + 3));
                parts.path = uri.substr(path_start);
            } else {
                parts.authority = uri.substr(scheme_end + 3);
                parts.path = "/";
            }
        } else {
            // No authority, just path
            if (scheme_end + 1 < uri.size() && uri[scheme_end + 1] == '/') {
                parts.path = uri.substr(scheme_end + 1);
            } else {
                parts.path = "/" + uri.substr(scheme_end + 1);
            }
        }
    } else {
        // No scheme, treat as relative path or just path
        if (uri.empty() || uri[0] != '/') {
            parts.path = "/" + uri;
        } else {
            parts.path = uri;
        }
    }
    
    return parts;
}

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
    auto fs = getByScheme(parts.scheme);
    if (fs) {
        return {fs, parts.path};
    }
    return {nullptr, ""};
}

} // namespace fujinet::fs
