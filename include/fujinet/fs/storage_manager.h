#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "fujinet/fs/filesystem.h"

namespace fujinet::fs {

class StorageManager {
public:
    StorageManager() = default;

    // Registers a filesystem under its own name(). Returns false if that name already exists.
    bool registerFileSystem(std::unique_ptr<IFileSystem> fs)
    {
        if (!fs) {
            return false;
        }
        auto key = fs->name();
        auto [it, inserted] = _fileSystems.emplace(std::move(key), std::move(fs));
        return inserted;
    }

    // Lookup by name (e.g. "flash", "sd0").
    IFileSystem* get(const std::string& name)
    {
        auto it = _fileSystems.find(name);
        return (it == _fileSystems.end()) ? nullptr : it->second.get();
    }

    const IFileSystem* get(const std::string& name) const
    {
        auto it = _fileSystems.find(name);
        return (it == _fileSystems.end()) ? nullptr : it->second.get();
    }

    // Optional helpers for enumeration (might be nice for config UI later).
    std::vector<std::string> listNames() const
    {
        std::vector<std::string> out;
        out.reserve(_fileSystems.size());
        for (auto const& [name, _] : _fileSystems) {
            out.push_back(name);
        }
        return out;
    }

private:
    std::unordered_map<std::string, std::unique_ptr<IFileSystem>> _fileSystems;
};

} // namespace fujinet::fs
