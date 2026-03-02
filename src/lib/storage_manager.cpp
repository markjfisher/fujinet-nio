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
    auto fs = getByScheme(parts.scheme);
    if (fs) {
        return {fs, parts.path};
    }
    return {nullptr, ""};
}

} // namespace fujinet::fs
