#pragma once

// A TNFS server for host tests: files in memory, a count of each kind of
// request, and the server's limit on open handles.

#include "fujinet/tnfs/tnfs_protocol.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace fujinet::tests {

// Counts requests and enforces an open-file limit.
class FakeTnfsServer : public fujinet::tnfs::ITnfsClient {
public:
    explicit FakeTnfsServer(bool parallel) : _parallel(parallel) {}

    std::map<std::string, std::vector<std::uint8_t>> files;
    std::size_t maxOpen = 16;
    int opens = 0, reads = 0, writes = 0, seeks = 0, batches = 0;

    bool mount(const std::string&, const std::string&, const std::string&) override { return true; }
    bool umount() override { return true; }
    bool stat(const std::string& path, fujinet::tnfs::TnfsStat& st) override
    {
        auto it = files.find(path);
        if (it == files.end()) return false;
        st = {};
        st.filesize = static_cast<uint32_t>(it->second.size());
        return true;
    }
    bool exists(const std::string& path) override { return files.count(path) != 0; }
    bool isDirectory(const std::string&) override { return false; }
    bool createDirectory(const std::string&) override { return true; }
    bool removeDirectory(const std::string&) override { return true; }
    bool removeFile(const std::string&) override { return true; }
    bool rename(const std::string&, const std::string&) override { return true; }
    std::vector<std::string> listDirectory(const std::string&) override { return {}; }

    int open(const std::string& path, uint16_t, uint16_t) override
    {
        if (!files.count(path) || _handles.size() >= maxOpen) return -1;
        const int h = _next++;
        _handles[h] = {path, 0};
        ++opens;
        return h;
    }
    bool close(int h) override { return _handles.erase(h) != 0; }

    std::size_t read(int h, void* buf, std::size_t bytes) override
    {
        ++reads;
        return do_read(h, buf, std::min<std::size_t>(bytes, 512));
    }
    std::size_t write(int h, const void* buf, std::size_t bytes) override
    {
        ++writes;
        auto& hd = _handles.at(h);
        auto& f = files[hd.path];
        bytes = std::min<std::size_t>(bytes, 512);
        if (hd.pos + bytes > f.size()) f.resize(hd.pos + bytes);
        std::memcpy(f.data() + hd.pos, buf, bytes);
        hd.pos += bytes;
        return bytes;
    }
    bool seek(int h, uint32_t offset) override
    {
        ++seeks;
        auto it = _handles.find(h);
        if (it == _handles.end()) return false;
        it->second.pos = offset;
        return true;
    }
    uint32_t tell(int h) override
    {
        auto it = _handles.find(h);
        return it == _handles.end() ? 0 : it->second.pos;
    }

    bool supports_parallel_reads() const override { return _parallel; }
    void read_parallel(std::vector<ReadAt>& batch) override
    {
        ++batches;
        for (auto& r : batch) {
            auto it = _handles.find(r.fileHandle);
            if (it == _handles.end()) {
                r.got = 0;
                continue;
            }
            it->second.pos = r.offset;
            r.got = do_read(r.fileHandle, r.dst, r.bytes);
        }
    }

    std::size_t open_handles() const { return _handles.size(); }

    // The server restarted: every handle it gave out is gone.
    void forget_handles() { _handles.clear(); }

private:
    struct Handle {
        std::string path;
        uint32_t pos;
    };

    std::size_t do_read(int h, void* buf, std::size_t bytes)
    {
        auto it = _handles.find(h);
        if (it == _handles.end()) return 0;
        auto& hd = it->second;
        const auto& f = files[hd.path];
        if (hd.pos >= f.size()) return 0;
        const std::size_t n = std::min<std::size_t>(bytes, f.size() - hd.pos);
        std::memcpy(buf, f.data() + hd.pos, n);
        hd.pos += static_cast<uint32_t>(n);
        return n;
    }

    bool _parallel;
    int _next = 1;
    std::map<int, Handle> _handles;
};

} // namespace fujinet::tests
