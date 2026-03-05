#include "fujinet/fs/storage_manager.h"
#include "fujinet/fs/filesystem.h"
#include "fujinet/fs/path_resolvers/path_resolver.h"
#include "fujinet/fs/uri_parser.h"

namespace fujinet::fs {

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
    // First, parse the URI to understand its structure
    auto parts = parse_uri(uri);
    
    // Use the PathResolver to handle URI/path patterns
    // This delegates to registered handlers (TnfsUriResolver, FsPrefixResolver, etc.)
    PathContext ctx;  // Empty context - no cwd for now
    ResolvedTarget target;
    
    if (getPathResolver().resolve(uri, ctx, target)) {
        // Found a handler that can resolve this URI
        auto fs = get(target.fs_name);
        if (fs) {
            // If the original URI had authority (e.g., scheme://authority/path),
            // we need to preserve the full URI in the path for filesystems that need it (TNFS, HTTP, etc.)
            // The handler may have stripped the scheme, so reconstruct if needed
            if (!parts.scheme.empty() && !parts.authority.empty()) {
                // Reconstruct: scheme://authority/path
                std::string fullPath = parts.scheme + "://" + parts.authority + parts.path;
                return {fs, fullPath};
            }
            return {fs, target.fs_path};
        }
    }
    
    // Fallback: try host filesystem for plain paths
    auto fs = get("host");
    if (fs) {
        // Ensure path starts with /
        std::string path = uri;
        if (!path.empty() && path[0] != '/') {
            path = "/" + path;
        }
        return {fs, path};
    }
    
    return {nullptr, ""};
}

} // namespace fujinet::fs
