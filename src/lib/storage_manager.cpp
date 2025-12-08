#include "fujinet/fs/storage_manager.h"

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

} // namespace fujinet::fs
