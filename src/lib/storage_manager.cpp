#include "fujinet/fs/storage_manager.h"
#include "fujinet/fs/filesystem.h"
#include "fujinet/fs/path_resolvers/path_resolver.h"
#include "fujinet/fs/uri_parser.h"

#include <cctype>

namespace fujinet::fs {

static bool iequals(const std::string& a, const std::string& b)
{
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

// Singleton path resolver instance - handlers are registered in constructor
static PathResolver& getPathResolver()
{
    static PathResolver resolver;
    return resolver;
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
    auto it = _fileSystems.find(name);
    if (it != _fileSystems.end()) {
        _fileSystems.erase(it);
        return true;
    }
    for (it = _fileSystems.begin(); it != _fileSystems.end(); ++it) {
        if (iequals(it->first, name)) {
            _fileSystems.erase(it);
            return true;
        }
    }
    return false;
}

IFileSystem* StorageManager::get(const std::string& name)
{
    auto it = _fileSystems.find(name);
    if (it != _fileSystems.end()) return it->second.get();
    for (auto& [key, fs] : _fileSystems) {
        if (iequals(key, name)) return fs.get();
    }
    return nullptr;
}

const IFileSystem* StorageManager::get(const std::string& name) const
{
    auto it = _fileSystems.find(name);
    if (it != _fileSystems.end()) return it->second.get();
    for (const auto& [key, fs] : _fileSystems) {
        if (iequals(key, name)) return fs.get();
    }
    return nullptr;
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
    // Default: scheme is the same as filesystem name
    return get(scheme);
}

const IFileSystem* StorageManager::getByScheme(const std::string& scheme) const
{
    // Default: scheme is the same as filesystem name
    return get(scheme);
}

std::pair<IFileSystem*, std::string> StorageManager::resolveUri(const std::string& uri)
{
    // Parse the URI first to understand its structure
    auto parts = parse_uri(uri);
    
    // Use the PathResolver to handle all URI/path patterns
    PathContext ctx;
    ResolvedTarget target;
    
    if (getPathResolver().resolve(uri, ctx, target)) {
        auto fs = get(target.fs_name);
        if (fs) {
            // If handler already returned something with "://" in it, use it as-is
            // (this means handler preserved the full URI)
            if (target.fs_path.find("://") != std::string::npos) {
                return {fs, target.fs_path};
            }
            
            // Handler returned a simple path - check if we need to reconstruct
            // This is needed when the original URI had scheme+authority but handler stripped it
            // We detect this by checking if the returned path starts with "/" while original had "://"
            if (!parts.scheme.empty() && !parts.authority.empty() && 
                !target.fs_path.empty() && target.fs_path[0] == '/') {
                // Reconstruct: scheme://authority/path
                std::string fullPath = parts.scheme + "://" + parts.authority + parts.path;
                return {fs, fullPath};
            }
            
            return {fs, target.fs_path};
        }
    }

    return {nullptr, ""};
}

} // namespace fujinet::fs
