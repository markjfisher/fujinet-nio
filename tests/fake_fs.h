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
        : _name(std::move(name))
    {
        _dirs.push_back("/"); // root
    }

    fujinet::fs::FileSystemKind kind() const override { return fujinet::fs::FileSystemKind::HostPosix; }
    std::string name() const override { return _name; }

    bool exists(const std::string& path) override
    {
        const std::string p = norm(path);
        if (is_dir_path(p)) return true;
        return _files.find(p) != _files.end();
    }

    bool isDirectory(const std::string& path) override { return is_dir_path(norm(path)); }

    bool createDirectory(const std::string& path) override
    {
        const std::string p = norm(path);
        if (p == "/") return true;
        const std::string parent = parent_path(p);
        if (!is_dir_path(parent)) return false;
        if (is_dir_path(p)) return true;
        _dirs.push_back(p);
        return true;
    }

    bool removeFile(const std::string& path) override
    {
        const std::string p = norm(path);
        return _files.erase(p) > 0;
    }

    bool removeDirectory(const std::string& path) override
    {
        const std::string p = norm(path);
        if (p == "/") return false;
        if (!is_dir_path(p)) return false;
        // must be empty (no immediate children)
        for (const auto& d : _dirs) {
            if (d != p && parent_path(d) == p) return false;
        }
        for (const auto& kv : _files) {
            if (parent_path(kv.first) == p) return false;
        }
        // erase
        for (auto it = _dirs.begin(); it != _dirs.end(); ++it) {
            if (*it == p) {
                _dirs.erase(it);
                return true;
            }
        }
        return false;
    }

    bool rename(const std::string& from, const std::string& to) override
    {
        const std::string f = norm(from);
        const std::string t = norm(to);
        auto it = _files.find(f);
        if (it == _files.end()) return false;
        // parent of dest must exist
        const std::string parent = parent_path(t);
        if (!is_dir_path(parent)) return false;
        _files[t] = std::move(it->second);
        _files.erase(it);
        return true;
    }

    std::unique_ptr<fujinet::fs::IFile> open(const std::string& path, const char* mode) override
    {
        const std::string m = mode ? std::string(mode) : std::string();
        const bool wantWrite = m.find('w') != std::string::npos || m.find('+') != std::string::npos || m.find('a') != std::string::npos;
        const bool readOnly = !wantWrite;

        const std::string p = norm(path);
        if (is_dir_path(p)) {
            return nullptr;
        }
        auto it = _files.find(p);
        if (it == _files.end()) {
            if (m.find('w') != std::string::npos) {
                // Require parent directory to exist.
                const std::string parent = parent_path(p);
                if (!is_dir_path(parent)) return nullptr;
                it = _files.emplace(p, std::vector<std::uint8_t>{}).first;
            } else {
                return nullptr;
            }
        }

        return std::make_unique<MemoryFile>(it->second, readOnly);
    }

    bool stat(const std::string& path, fujinet::fs::FileInfo& outInfo) override
    {
        const std::string p = norm(path);
        if (is_dir_path(p)) {
            outInfo.path = p;
            outInfo.isDirectory = true;
            outInfo.sizeBytes = 0;
            return true;
        }
        auto it = _files.find(p);
        if (it == _files.end()) return false;
        outInfo.path = p;
        outInfo.isDirectory = false;
        outInfo.sizeBytes = it->second.size();
        return true;
    }

    bool listDirectory(const std::string& path, std::vector<fujinet::fs::FileInfo>& out) override
    {
        const std::string p = norm(path);
        if (!is_dir_path(p)) return false;
        out.clear();

        for (const auto& d : _dirs) {
            if (d == "/") continue;
            if (parent_path(d) != p) continue;
            fujinet::fs::FileInfo fi{};
            fi.path = d;
            fi.isDirectory = true;
            fi.sizeBytes = 0;
            out.push_back(std::move(fi));
        }
        for (const auto& kv : _files) {
            if (parent_path(kv.first) != p) continue;
            fujinet::fs::FileInfo fi{};
            fi.path = kv.first;
            fi.isDirectory = false;
            fi.sizeBytes = kv.second.size();
            out.push_back(std::move(fi));
        }
        return true;
    }

    bool create_file(const std::string& path, const std::vector<std::uint8_t>& bytes)
    {
        const std::string p = norm(path);
        if (is_dir_path(p)) return false;
        const std::string parent = parent_path(p);
        if (!is_dir_path(parent)) return false;
        _files[p] = bytes;
        return true;
    }

    std::vector<std::uint8_t>& file_bytes(const std::string& path) { return _files[norm(path)]; }

private:
    static std::string norm(const std::string& in)
    {
        if (in.empty()) return "/";
        if (in[0] != '/') return "/" + in;
        if (in.size() > 1 && in.back() == '/') {
            // normalize away trailing slash
            std::string out = in;
            while (out.size() > 1 && out.back() == '/') out.pop_back();
            return out;
        }
        return in;
    }

    static std::string parent_path(const std::string& abs)
    {
        if (abs.empty() || abs == "/") return "/";
        std::string s = abs;
        while (s.size() > 1 && s.back() == '/') s.pop_back();
        const std::size_t slash = s.find_last_of('/');
        if (slash == std::string::npos || slash == 0) return "/";
        return s.substr(0, slash);
    }

    bool is_dir_path(const std::string& abs) const
    {
        for (const auto& d : _dirs) {
            if (d == abs) return true;
        }
        return false;
    }

    std::string _name;
    std::unordered_map<std::string, std::vector<std::uint8_t>> _files;
    std::vector<std::string> _dirs;
};

} // namespace fujinet::tests


