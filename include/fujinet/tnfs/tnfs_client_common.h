#pragma once

#include "fujinet/tnfs/tnfs_protocol.h"
#include "fujinet/io/core/channel.h"
#include "fujinet/core/logging.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <memory>
#include <thread>
#include <vector>

namespace fujinet::tnfs {

static constexpr const char* TAG = "tnfs";

class CommonTnfsClient : public ITnfsClient {
public:
    CommonTnfsClient(std::unique_ptr<fujinet::io::Channel> channel, const char* transportName,
                     bool parallelReads = false)
        : _channel(std::move(channel))
        , _transportName(transportName)
        , _parallelReads(parallelReads)
    {
        FN_LOGI(TAG, "%s TNFS client created", _transportName);
    }

    ~CommonTnfsClient() override
    {
        umount();
    }

    bool mount(const std::string& mountPath, const std::string& user, const std::string& password) override
    {
        TnfsPacket pkt{};
        pkt.command = CMD_MOUNT;
        pkt.sequenceNum = _sequenceNum++;

        // Version 1.2 (minor, major)
        pkt.payload[0] = 0x02;
        pkt.payload[1] = 0x01;

        std::size_t offset = 2;
        if (!append_cstring(pkt.payload, sizeof(pkt.payload), offset, mountPath) ||
            !append_cstring(pkt.payload, sizeof(pkt.payload), offset, user) ||
            !append_cstring(pkt.payload, sizeof(pkt.payload), offset, password)) {
            FN_LOGE(TAG, "Mount payload too large");
            return false;
        }

        if (!send_and_receive(pkt, offset)) {
            FN_LOGE(TAG, "Mount failed: no response");
            return false;
        }

        if (pkt.payload[0] != RESULT_SUCCESS) {
            FN_LOGE(TAG, "Mount failed: %u", static_cast<unsigned>(pkt.payload[0]));
            return false;
        }

        // Any session number is valid: tnfsd gives its first session 0.
        _sessionId = read_u16le(pkt.sessionIdL, pkt.sessionIdH);
        _mounted = true;
        // Reply: status, server version (minor, major), min retry. OPENDIRX/READDIRX are 1.1+.
        const unsigned serverMinor = pkt.payload[1];
        const unsigned serverMajor = pkt.payload[2];
        if (serverMajor == 0 && serverMinor == 0) {
            // Unknown version: try the extended commands and learn from the reply.
        } else if (serverMajor < 1 || (serverMajor == 1 && serverMinor < 1)) {
            _noDirX = true;
        }
        FN_LOGI(TAG, "Mounted %s session 0x%04X", _transportName, static_cast<unsigned>(_sessionId));
        return true;
    }

    bool umount() override
    {
        if (!_mounted) {
            return true;
        }

        TnfsPacket pkt{};
        fill_session_header(pkt, CMD_UNMOUNT);
        if (!send_and_receive(pkt, 0)) {
            return false;
        }
        if (pkt.payload[0] != RESULT_SUCCESS) {
            return false;
        }

        _sessionId = 0;
        _mounted = false;
        return true;
    }

    bool stat(const std::string& path, TnfsStat& outStat) override
    {
        TnfsPacket pkt{};
        if (!build_path_command(CMD_STAT, path, pkt)) {
            return false;
        }
        if (pkt.payload[0] != RESULT_SUCCESS) {
            return false;
        }

        // TNFS STAT reply payload layout (after status byte at payload[0]):
        // mode:u16, uid:u16, gid:u16, size:u32, atime:u32, mtime:u32, ctime:u32
        // We currently consume mode/size/timestamps and derive directory status from mode.
        static constexpr std::uint16_t kModeTypeMask = 0xF000U;
        static constexpr std::uint16_t kModeDir = 0x4000U;

        outStat.mode = read_u16le(pkt.payload[1], pkt.payload[2]);
        outStat.isDir = (outStat.mode & kModeTypeMask) == kModeDir;
        outStat.filesize = read_u32le(pkt.payload + 7);
        outStat.aTime = read_u32le(pkt.payload + 11);
        outStat.mTime = read_u32le(pkt.payload + 15);
        outStat.cTime = read_u32le(pkt.payload + 19);
        return true;
    }

    bool exists(const std::string& path) override
    {
        TnfsStat st{};
        return stat(path, st);
    }

    bool isDirectory(const std::string& path) override
    {
        TnfsStat st{};
        return stat(path, st) && st.isDir;
    }

    bool createDirectory(const std::string& path) override
    {
        return simple_path_ok(CMD_MKDIR, path);
    }

    bool removeDirectory(const std::string& path) override
    {
        return simple_path_ok(CMD_RMDIR, path);
    }

    bool removeFile(const std::string& path) override
    {
        return simple_path_ok(CMD_UNLINK, path);
    }

    bool rename(const std::string& from, const std::string& to) override
    {
        if (!_mounted) {
            return false;
        }

        TnfsPacket pkt{};
        fill_session_header(pkt, CMD_RENAME);

        std::size_t offset = 0;
        if (!append_cstring(pkt.payload, sizeof(pkt.payload), offset, from) ||
            !append_cstring(pkt.payload, sizeof(pkt.payload), offset, to)) {
            return false;
        }

        if (!send_and_receive(pkt, offset)) {
            return false;
        }
        return pkt.payload[0] == RESULT_SUCCESS;
    }

    std::vector<std::string> listDirectory(const std::string& path) override
    {
        std::vector<std::string> entries;
        if (!_mounted) {
            return entries;
        }

        TnfsPacket pkt{};
        if (!build_path_command(CMD_OPENDIR, path, pkt)) {
            return entries;
        }
        if (pkt.payload[0] != RESULT_SUCCESS) {
            return entries;
        }

        const int dirHandle = static_cast<int>(pkt.payload[1]);
        while (true) {
            TnfsPacket readPkt{};
            fill_session_header(readPkt, CMD_READDIR);
            readPkt.payload[0] = static_cast<std::uint8_t>(dirHandle);

            if (!send_and_receive(readPkt, 1)) {
                break;
            }
            if (readPkt.payload[0] == RESULT_END_OF_FILE) {
                break;
            }
            if (readPkt.payload[0] != RESULT_SUCCESS) {
                break;
            }

            std::string entry(reinterpret_cast<char*>(readPkt.payload + 1));
            if (entry != "." && entry != "..") {
                entries.push_back(std::move(entry));
            }
        }

        TnfsPacket closePkt{};
        fill_session_header(closePkt, CMD_CLOSEDIR);
        closePkt.payload[0] = static_cast<std::uint8_t>(dirHandle);
        send_and_receive(closePkt, 1);

        return entries;
    }

    TnfsListStatus listDirectoryRange(const std::string& path, fs::DirSort sort, std::size_t start,
                                      std::size_t count, std::vector<TnfsDirEntry>& out, std::size_t& total,
                                      std::chrono::milliseconds deadline = kListingDeadline) override
    {
        out.clear();
        total = 0;
        if (!_mounted) {
            return TnfsListStatus::Error;
        }

        DeadlineScope scope(*this, deadline);

        if (!_noDirX) {
            const XResult r = list_range_x(path, sort, start, count, out, total);
            switch (r) {
            case XResult::Ok:          return TnfsListStatus::Ok;
            case XResult::Error:       return TnfsListStatus::Error;
            case XResult::Timeout:     return TnfsListStatus::Timeout;
            case XResult::Unsupported: break;
            }
            out.clear();
            total = 0;
        }

        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
            _deadline - std::chrono::steady_clock::now());
        if (left.count() <= 0) {
            return TnfsListStatus::Timeout;
        }
        TnfsListStatus r = ITnfsClient::listDirectoryRange(path, sort, start, count, out, total, left);
        if (r == TnfsListStatus::Error && _deadlineHit) {
            r = TnfsListStatus::Timeout;
        }
        if (r != TnfsListStatus::Ok) {
            out.clear();
        }
        return r;
    }

    int open(const std::string& path, uint16_t openMode, uint16_t createPerms) override
    {
        if (!_mounted) {
            return -1;
        }

        TnfsPacket pkt{};
        fill_session_header(pkt, CMD_OPEN);
        pkt.payload[0] = static_cast<std::uint8_t>(openMode & 0xFFU);
        pkt.payload[1] = static_cast<std::uint8_t>((openMode >> 8) & 0xFFU);
        pkt.payload[2] = static_cast<std::uint8_t>(createPerms & 0xFFU);
        pkt.payload[3] = static_cast<std::uint8_t>((createPerms >> 8) & 0xFFU);

        std::size_t offset = 4;
        if (!append_cstring(pkt.payload, sizeof(pkt.payload), offset, path)) {
            return -1;
        }

        if (!send_and_receive(pkt, offset)) {
            return -1;
        }
        if (pkt.payload[0] != RESULT_SUCCESS) {
            return -1;
        }
        return static_cast<int>(pkt.payload[1]);
    }

    bool close(int fileHandle) override
    {
        if (!_mounted) {
            return false;
        }

        TnfsPacket pkt{};
        fill_session_header(pkt, CMD_CLOSE);
        pkt.payload[0] = static_cast<std::uint8_t>(fileHandle);
        if (!send_and_receive(pkt, 1)) {
            return false;
        }
        return pkt.payload[0] == RESULT_SUCCESS;
    }

    std::size_t read(int fileHandle, void* buffer, std::size_t bytes) override
    {
        if (!_mounted) {
            return 0;
        }

        const std::size_t req = (bytes > 512) ? 512 : bytes;
        TnfsPacket pkt{};
        fill_session_header(pkt, CMD_READ);
        pkt.payload[0] = static_cast<std::uint8_t>(fileHandle);
        pkt.payload[1] = static_cast<std::uint8_t>(req & 0xFFU);
        pkt.payload[2] = static_cast<std::uint8_t>((req >> 8) & 0xFFU);
        if (!send_and_receive(pkt, 3)) {
            return 0;
        }

        if (pkt.payload[0] == RESULT_END_OF_FILE) {
            return 0;
        }
        if (pkt.payload[0] != RESULT_SUCCESS) {
            return 0;
        }

        std::size_t got = static_cast<std::size_t>(read_u16le(pkt.payload[1], pkt.payload[2]));
        if (got > req) {
            got = req;
        }
        std::memcpy(buffer, pkt.payload + 3, got);
        return got;
    }

    std::size_t write(int fileHandle, const void* buffer, std::size_t bytes) override
    {
        if (!_mounted) {
            return 0;
        }

        const std::size_t chunk = (bytes > 512) ? 512 : bytes;
        TnfsPacket pkt{};
        fill_session_header(pkt, CMD_WRITE);
        pkt.payload[0] = static_cast<std::uint8_t>(fileHandle);
        pkt.payload[1] = static_cast<std::uint8_t>(chunk & 0xFFU);
        pkt.payload[2] = static_cast<std::uint8_t>((chunk >> 8) & 0xFFU);
        std::memcpy(pkt.payload + 3, buffer, chunk);

        if (!send_and_receive(pkt, 3 + chunk)) {
            return 0;
        }
        if (pkt.payload[0] != RESULT_SUCCESS) {
            return 0;
        }
        return static_cast<std::size_t>(read_u16le(pkt.payload[1], pkt.payload[2]));
    }

    bool seek(int fileHandle, uint32_t offset) override
    {
        std::uint32_t ignored = 0;
        return lseek_internal(fileHandle, offset, 0, ignored); // SEEK_SET
    }

    uint32_t tell(int fileHandle) override
    {
        std::uint32_t pos = 0;
        if (!lseek_internal(fileHandle, 0, 1, pos)) { // SEEK_CUR
            return 0;
        }
        return pos;
    }

    bool supports_parallel_reads() const override { return _parallelReads; }

    void read_parallel(std::vector<ReadAt>& reads) override
    {
        for (auto& r : reads) {
            r.got = 0;
        }
        if (!_mounted || reads.empty() || !_parallelReads) {
            return;
        }

        // Seeks are idempotent, so they are retried.
        std::vector<TnfsPacket> pkts(reads.size());
        for (std::size_t i = 0; i < reads.size(); ++i) {
            fill_session_header(pkts[i], CMD_LSEEK);
            pkts[i].payload[0] = static_cast<std::uint8_t>(reads[i].fileHandle);
            pkts[i].payload[1] = 0; // SEEK_SET
            pkts[i].payload[2] = static_cast<std::uint8_t>(reads[i].offset & 0xFFU);
            pkts[i].payload[3] = static_cast<std::uint8_t>((reads[i].offset >> 8) & 0xFFU);
            pkts[i].payload[4] = static_cast<std::uint8_t>((reads[i].offset >> 16) & 0xFFU);
            pkts[i].payload[5] = static_cast<std::uint8_t>((reads[i].offset >> 24) & 0xFFU);
        }
        std::vector<bool> ok = exchange_all(pkts, std::vector<std::size_t>(reads.size(), 6), kMaxAttempts,
                                            kParallelTimeout);

        // Reads are sent once: a lost reply may have moved the handle.
        std::vector<std::size_t> which;
        std::vector<TnfsPacket> readPkts;
        for (std::size_t i = 0; i < reads.size(); ++i) {
            if (!ok[i] || pkts[i].payload[0] != RESULT_SUCCESS) {
                continue;
            }
            const std::size_t req = std::min<std::size_t>(reads[i].bytes, 512);
            TnfsPacket pkt{};
            fill_session_header(pkt, CMD_READ);
            pkt.payload[0] = static_cast<std::uint8_t>(reads[i].fileHandle);
            pkt.payload[1] = static_cast<std::uint8_t>(req & 0xFFU);
            pkt.payload[2] = static_cast<std::uint8_t>((req >> 8) & 0xFFU);
            which.push_back(i);
            readPkts.push_back(pkt);
        }
        if (readPkts.empty()) {
            return;
        }
        ok = exchange_all(readPkts, std::vector<std::size_t>(readPkts.size(), 3), 1, kParallelTimeout);
        for (std::size_t k = 0; k < which.size(); ++k) {
            ReadAt& r = reads[which[k]];
            const TnfsPacket& reply = readPkts[k];
            if (!ok[k] || reply.payload[0] != RESULT_SUCCESS) {
                continue;
            }
            const std::size_t req = std::min<std::size_t>(r.bytes, 512);
            const std::size_t got = std::min<std::size_t>(read_u16le(reply.payload[1], reply.payload[2]), req);
            std::memcpy(r.dst, reply.payload + 3, got);
            r.got = got;
        }
    }

private:
    static std::uint16_t read_u16le(std::uint8_t lo, std::uint8_t hi)
    {
        return static_cast<std::uint16_t>(lo) |
               (static_cast<std::uint16_t>(hi) << 8);
    }

    static std::uint32_t read_u32le(const std::uint8_t* p)
    {
        return static_cast<std::uint32_t>(p[0]) |
               (static_cast<std::uint32_t>(p[1]) << 8) |
               (static_cast<std::uint32_t>(p[2]) << 16) |
               (static_cast<std::uint32_t>(p[3]) << 24);
    }

    static bool append_cstring(std::uint8_t* dst, std::size_t dstSize, std::size_t& offset, const std::string& s)
    {
        if (offset >= dstSize) {
            return false;
        }
        const std::size_t bytesNeeded = s.size() + 1;
        if (bytesNeeded > (dstSize - offset)) {
            return false;
        }
        std::memcpy(dst + offset, s.c_str(), bytesNeeded);
        offset += bytesNeeded;
        return true;
    }

    void fill_session_header(TnfsPacket& pkt, std::uint8_t command)
    {
        pkt.sessionIdL = static_cast<std::uint8_t>(_sessionId & 0xFFU);
        pkt.sessionIdH = static_cast<std::uint8_t>((_sessionId >> 8) & 0xFFU);
        pkt.sequenceNum = _sequenceNum++;
        pkt.command = command;
    }

    bool simple_path_ok(std::uint8_t command, const std::string& path)
    {
        TnfsPacket pkt{};
        if (!build_path_command(command, path, pkt)) {
            return false;
        }
        return pkt.payload[0] == RESULT_SUCCESS;
    }

    bool build_path_command(std::uint8_t command, const std::string& path, TnfsPacket& pkt)
    {
        if (!_mounted) {
            return false;
        }

        fill_session_header(pkt, command);
        std::size_t offset = 0;
        if (!append_cstring(pkt.payload, sizeof(pkt.payload), offset, path)) {
            return false;
        }
        return send_and_receive(pkt, offset);
    }

    enum class XResult { Ok, Error, Timeout, Unsupported };

    // Bounds an operation: every send_and_receive inside gives up at the deadline.
    struct DeadlineScope {
        CommonTnfsClient& c;
        DeadlineScope(CommonTnfsClient& client, std::chrono::milliseconds budget) : c(client)
        {
            c._deadline = std::chrono::steady_clock::now() + budget;
            c._hasDeadline = true;
            c._deadlineHit = false;
        }
        ~DeadlineScope() { c._hasDeadline = false; }
    };

    XResult list_range_x(const std::string& path, fs::DirSort sort, std::size_t start, std::size_t count,
                         std::vector<TnfsDirEntry>& out, std::size_t& total)
    {
        TnfsPacket pkt{};
        fill_session_header(pkt, CMD_OPENDIRX);
        pkt.payload[0] = (sort == fs::DirSort::Name) ? DIROPT_NO_FOLDERSFIRST : 0;
        pkt.payload[1] = (sort == fs::DirSort::None) ? DIRSORT_NONE : 0;
        pkt.payload[2] = 0; // max results: unlimited
        pkt.payload[3] = 0;
        std::size_t offset = 4;
        const std::string noPattern;
        if (!append_cstring(pkt.payload, sizeof(pkt.payload), offset, noPattern) ||
            !append_cstring(pkt.payload, sizeof(pkt.payload), offset, path)) {
            return XResult::Error;
        }
        if (!send_and_receive(pkt, offset)) {
            return _deadlineHit ? XResult::Timeout : XResult::Error;
        }
        const std::uint8_t status = pkt.payload[0];
        if (status != RESULT_SUCCESS) {
            if (status == RESULT_FUNCTION_UNIMPLEMENTED) {
                _noDirX = true;
                return XResult::Unsupported;
            }
            if (status == RESULT_PROTOCOL_ERROR || status == RESULT_INVALID_ARGUMENT) {
                return XResult::Unsupported; // this call only; the path may be at fault
            }
            return XResult::Error;
        }

        const std::uint8_t handle = pkt.payload[1];
        total = read_u16le(pkt.payload[2], pkt.payload[3]);

        XResult result = XResult::Ok;
        std::size_t pos = 0;
        if (start > 0 && start < total) {
            TnfsPacket seekPkt{};
            fill_session_header(seekPkt, CMD_SEEKDIR);
            seekPkt.payload[0] = handle;
            seekPkt.payload[1] = static_cast<std::uint8_t>(start & 0xFFU);
            seekPkt.payload[2] = static_cast<std::uint8_t>((start >> 8) & 0xFFU);
            seekPkt.payload[3] = static_cast<std::uint8_t>((start >> 16) & 0xFFU);
            seekPkt.payload[4] = static_cast<std::uint8_t>((start >> 24) & 0xFFU);
            if (send_and_receive(seekPkt, 5) && seekPkt.payload[0] == RESULT_SUCCESS) {
                pos = start;
            } else if (_deadlineHit) {
                result = XResult::Timeout;
            }
            // Otherwise read forward from the beginning and drop the leading entries.
        }

        const std::size_t want = (count > total - std::min(total, start)) ? total - std::min(total, start) : count;
        const std::size_t stop = start + want;
        bool eof = (start >= total);
        while (result == XResult::Ok && !eof && pos < stop) {
            TnfsPacket rd{};
            fill_session_header(rd, CMD_READDIRX);
            rd.payload[0] = handle;
            rd.payload[1] = static_cast<std::uint8_t>(std::min<std::size_t>(stop - pos, 255));
            if (!send_and_receive(rd, 2)) {
                result = _deadlineHit ? XResult::Timeout : XResult::Error;
                break;
            }
            if (rd.payload[0] == RESULT_END_OF_FILE) {
                break;
            }
            if (rd.payload[0] != RESULT_SUCCESS) {
                result = XResult::Error;
                break;
            }
            const std::size_t entryCount = rd.payload[1];
            if ((rd.payload[2] & DIRSTATUS_EOF) != 0) {
                eof = true;
            }
            std::size_t idx = read_u16le(rd.payload[3], rd.payload[4]);
            std::size_t p = 5;
            std::size_t parsed = 0;
            for (; parsed < entryCount; ++parsed) {
                if (p + 13 + 1 > sizeof(rd.payload)) {
                    break;
                }
                const std::uint8_t flags = rd.payload[p];
                const std::uint32_t size = read_u32le(rd.payload + p + 1);
                const std::uint32_t mtime = read_u32le(rd.payload + p + 5);
                p += 13;
                const void* nul = std::memchr(rd.payload + p, 0, sizeof(rd.payload) - p);
                if (nul == nullptr) {
                    break;
                }
                std::string name(reinterpret_cast<const char*>(rd.payload + p));
                p += name.size() + 1;
                if (idx >= start && idx < stop && name != "." && name != "..") {
                    TnfsDirEntry e;
                    e.name = std::move(name);
                    e.isDir = (flags & DIRENTRY_DIR) != 0;
                    e.size = size;
                    e.mTime = mtime;
                    out.push_back(std::move(e));
                }
                ++idx;
            }
            if (parsed == 0) {
                break; // no progress
            }
            pos = idx;
        }

        close_dir(handle);
        if (result != XResult::Ok) {
            out.clear();
        }
        return result;
    }

    void close_dir(std::uint8_t handle)
    {
        TnfsPacket closePkt{};
        fill_session_header(closePkt, CMD_CLOSEDIR);
        closePkt.payload[0] = handle;
        if (_deadlineHit) {
            // Out of time: free the server's handle without waiting for the reply.
            _channel->write(reinterpret_cast<const std::uint8_t*>(&closePkt), 5);
            return;
        }
        send_and_receive(closePkt, 1);
    }

    bool lseek_internal(int fileHandle, std::uint32_t offset, std::uint8_t whence, std::uint32_t& outPos)
    {
        if (!_mounted) {
            return false;
        }

        TnfsPacket pkt{};
        fill_session_header(pkt, CMD_LSEEK);
        pkt.payload[0] = static_cast<std::uint8_t>(fileHandle);
        pkt.payload[1] = whence;  // SEEK_SET, SEEK_CUR, or SEEK_END
        pkt.payload[2] = static_cast<std::uint8_t>(offset & 0xFFU);
        pkt.payload[3] = static_cast<std::uint8_t>((offset >> 8) & 0xFFU);
        pkt.payload[4] = static_cast<std::uint8_t>((offset >> 16) & 0xFFU);
        pkt.payload[5] = static_cast<std::uint8_t>((offset >> 24) & 0xFFU);

        if (!send_and_receive(pkt, 6)) {
            return false;
        }
        if (pkt.payload[0] != RESULT_SUCCESS) {
            return false;
        }

        // TNFS lseek returns new position in payload[1..4].
        outPos = read_u32le(pkt.payload + 1);
        return true;
    }

    static constexpr std::chrono::milliseconds kTimeoutPerAttempt{1500};
    // Short: a block missed by read-ahead is read again with full retries.
    static constexpr std::chrono::milliseconds kParallelTimeout{400};
    static constexpr int kMaxAttempts = 3;
    static constexpr std::size_t kMinResponseSize = 5;

    // One reply, or false at the deadline.
    bool receive(TnfsPacket& response, std::chrono::steady_clock::time_point deadline)
    {
        while (true) {
            const std::size_t bytesRead =
                _channel->read(reinterpret_cast<std::uint8_t*>(&response), sizeof(response));
            if (bytesRead >= kMinResponseSize) {
                return true;
            }
            const auto now = std::chrono::steady_clock::now();
            if (now >= deadline) {
                return false;
            }
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
            if (_channel->supports_readable_wait()) {
                _channel->wait_for_readable(std::min(left, std::chrono::milliseconds(50)));
            } else {
                std::this_thread::sleep_for(std::min(left, std::chrono::milliseconds(10)));
            }
        }
    }

    bool is_reply_to(const TnfsPacket& response, std::uint8_t seq) const
    {
        if (response.sequenceNum != seq) {
            return false;
        }
        return !_mounted ||
               read_u16le(response.sessionIdL, response.sessionIdH) == _sessionId;
    }

    bool send_and_receive(TnfsPacket& pkt, std::size_t payloadSize)
    {
        const std::uint8_t expectedSeq = pkt.sequenceNum;
        std::vector<std::uint8_t> tx(4 + payloadSize);
        std::memcpy(tx.data(), &pkt, tx.size());

        for (int attempt = 0; attempt < kMaxAttempts; ++attempt) {
            if (_hasDeadline && std::chrono::steady_clock::now() >= _deadline) {
                _deadlineHit = true;
                break;
            }
            _channel->write(tx.data(), tx.size());
            auto deadline = std::chrono::steady_clock::now() + kTimeoutPerAttempt;
            if (_hasDeadline && _deadline < deadline) {
                deadline = _deadline;
            }

            TnfsPacket response{};
            while (receive(response, deadline)) {
                if (is_reply_to(response, expectedSeq)) {
                    pkt = response;
                    return true;
                }
            }
        }

        if (_hasDeadline && std::chrono::steady_clock::now() >= _deadline) {
            _deadlineHit = true;
        }
        FN_LOGE(TAG, "%s TNFS timeout for command 0x%02X", _transportName, static_cast<unsigned>(pkt.command));
        return false;
    }

    // Replies overwrite `pkts`; returns which packets were answered.
    std::vector<bool> exchange_all(std::vector<TnfsPacket>& pkts, const std::vector<std::size_t>& payloadSizes,
                                   int attempts, std::chrono::milliseconds timeout)
    {
        std::vector<bool> done(pkts.size(), false);
        std::size_t left = pkts.size();
        const std::vector<TnfsPacket> sent = pkts;

        for (int attempt = 0; attempt < attempts && left > 0; ++attempt) {
            for (std::size_t i = 0; i < sent.size(); ++i) {
                if (!done[i]) {
                    _channel->write(reinterpret_cast<const std::uint8_t*>(&sent[i]), 4 + payloadSizes[i]);
                }
            }
            const auto deadline = std::chrono::steady_clock::now() + timeout;
            TnfsPacket response{};
            while (left > 0 && receive(response, deadline)) {
                for (std::size_t i = 0; i < sent.size(); ++i) {
                    if (!done[i] && is_reply_to(response, sent[i].sequenceNum)) {
                        pkts[i] = response;
                        done[i] = true;
                        --left;
                        break;
                    }
                }
            }
        }
        if (left > 0) {
            FN_LOGW(TAG, "%s TNFS: %u of %u parallel replies missing", _transportName,
                    static_cast<unsigned>(left), static_cast<unsigned>(pkts.size()));
        }
        return done;
    }

private:
    std::unique_ptr<fujinet::io::Channel> _channel;
    const char* _transportName;
    bool _parallelReads{false};
    std::uint16_t _sessionId{0};
    bool _mounted{false};
    std::uint8_t _sequenceNum{0};
    bool _noDirX{false};
    bool _hasDeadline{false};
    bool _deadlineHit{false};
    std::chrono::steady_clock::time_point _deadline{};
};

} // namespace fujinet::tnfs
