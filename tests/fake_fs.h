#pragma once

#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "fujinet/fs/filesystem.h"

namespace fujinet::tests {

class MemoryFile final : public fujinet::fs::IFile {
public:
    MemoryFile(std::vector<std::uint8_t>& bytes, bool readOnly)
        : _bytes(bytes), _readOnly(readOnly) {}

    std::size_t read(void* dst, std::size_t maxBytes) override
    {
        if (!dst) return 0;
        if (_pos >= _bytes.size()) return 0;
        const std::size_t n = std::min<std::size_t>(maxBytes, _bytes.size() - _pos);
        std::memcpy(dst, _bytes.data() + _pos, n);
        _pos += n;
        return n;
    }

    std::size_t write(const void* src, std::size_t bytes) override
    {
        if (_readOnly || !src) return 0;
        if (_pos > _bytes.size()) return 0;
        if (_pos + bytes > _bytes.size()) {
            _bytes.resize(_pos + bytes);
        }
        std::memcpy(_bytes.data() + _pos, src, bytes);
        _pos += bytes;
        return bytes;
    }

    bool seek(std::uint64_t offset) override
    {
        if (offset > _bytes.size()) {
            // Allow sparse seek when writable (mimics stdio behavior).
            if (_readOnly) return false;
            _bytes.resize(static_cast<std::size_t>(offset), 0);
        }
        _pos = static_cast<std::size_t>(offset);
        return true;
    }

    std::uint64_t tell() const override { return _pos; }
    bool flush() override { return true; }

private:
    std::vector<std::uint8_t>& _bytes;
    bool _readOnly{true};
    std::size_t _pos{0};
};

class MemoryFileSystem final : public fujinet::fs::IFileSystem {
public:
    explicit MemoryFileSystem(std::string name)
        : _name(std::move(name)) {}

    fujinet::fs::FileSystemKind kind() const override { return fujinet::fs::FileSystemKind::HostPosix; }
    std::string name() const override { return _name; }

    bool exists(const std::string& path) override
    {
        return _files.find(path) != _files.end();
    }

    bool isDirectory(const std::string&) override { return false; }

    bool createDirectory(const std::string&) override { return false; }
    bool removeFile(const std::string& path) override { return _files.erase(path) > 0; }
    bool removeDirectory(const std::string&) override { return false; }
    bool rename(const std::string& from, const std::string& to) override
    {
        auto it = _files.find(from);
        if (it == _files.end()) return false;
        _files[to] = std::move(it->second);
        _files.erase(it);
        return true;
    }

    std::unique_ptr<fujinet::fs::IFile> open(const std::string& path, const char* mode) override
    {
        const std::string m = mode ? std::string(mode) : std::string();
        const bool wantWrite = m.find('w') != std::string::npos || m.find('+') != std::string::npos || m.find('a') != std::string::npos;
        const bool readOnly = !wantWrite;

        auto it = _files.find(path);
        if (it == _files.end()) {
            if (m.find('w') != std::string::npos) {
                it = _files.emplace(path, std::vector<std::uint8_t>{}).first;
            } else {
                return nullptr;
            }
        }

        return std::make_unique<MemoryFile>(it->second, readOnly);
    }

    bool stat(const std::string& path, fujinet::fs::FileInfo& outInfo) override
    {
        auto it = _files.find(path);
        if (it == _files.end()) return false;
        outInfo.path = path;
        outInfo.isDirectory = false;
        outInfo.sizeBytes = it->second.size();
        return true;
    }

    bool listDirectory(const std::string&, std::vector<fujinet::fs::FileInfo>&) override { return false; }

    std::vector<std::uint8_t>& file_bytes(const std::string& path) { return _files[path]; }

private:
    std::string _name;
    std::unordered_map<std::string, std::vector<std::uint8_t>> _files;
};

} // namespace fujinet::tests


