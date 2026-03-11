#pragma once

#include "fujinet/tnfs/tnfs_protocol.h"
#include "fujinet/io/core/channel.h"
#include "fujinet/core/logging.h"

#include <chrono>
#include <cstring>
#include <memory>
#include <thread>
#include <vector>

namespace fujinet::tnfs {

static constexpr const char* TAG = "tnfs";

class CommonTnfsClient : public ITnfsClient {
public:
    CommonTnfsClient(std::unique_ptr<fujinet::io::Channel> channel, const char* transportName)
        : _channel(std::move(channel))
        , _transportName(transportName)
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

        _sessionId = read_u16le(pkt.sessionIdL, pkt.sessionIdH);
        FN_LOGI(TAG, "Mounted %s session 0x%04X", _transportName, static_cast<unsigned>(_sessionId));
        return true;
    }

    bool umount() override
    {
        if (_sessionId == 0) {
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
        if (_sessionId == 0) {
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
        if (_sessionId == 0) {
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

    int open(const std::string& path, uint16_t openMode, uint16_t createPerms) override
    {
        if (_sessionId == 0) {
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
        if (_sessionId == 0) {
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
        if (_sessionId == 0) {
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
        if (_sessionId == 0) {
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
        if (_sessionId == 0) {
            return false;
        }

        fill_session_header(pkt, command);
        std::size_t offset = 0;
        if (!append_cstring(pkt.payload, sizeof(pkt.payload), offset, path)) {
            return false;
        }
        return send_and_receive(pkt, offset);
    }

    bool lseek_internal(int fileHandle, std::uint32_t offset, std::uint8_t whence, std::uint32_t& outPos)
    {
        if (_sessionId == 0) {
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

    bool send_and_receive(TnfsPacket& pkt, std::size_t payloadSize)
    {
        static constexpr std::chrono::milliseconds kTimeoutPerAttempt(1500);
        static constexpr std::chrono::milliseconds kPollDelay(10);
        static constexpr int kMaxAttempts = 3;
        static constexpr std::size_t kMinResponseSize = 5;

        const std::uint8_t expectedSeq = pkt.sequenceNum;
        std::vector<std::uint8_t> tx(4 + payloadSize);
        std::memcpy(tx.data(), &pkt, tx.size());

        for (int attempt = 0; attempt < kMaxAttempts; ++attempt) {
            _channel->write(tx.data(), tx.size());
            const auto deadline = std::chrono::steady_clock::now() + kTimeoutPerAttempt;

            while (std::chrono::steady_clock::now() < deadline) {
                TnfsPacket response{};
                const std::size_t bytesRead = _channel->read(reinterpret_cast<std::uint8_t*>(&response), sizeof(response));
                if (bytesRead < kMinResponseSize) {
                    std::this_thread::sleep_for(kPollDelay);
                    continue;
                }

                if (response.sequenceNum != expectedSeq) {
                    continue;
                }
                if (_sessionId != 0) {
                    const std::uint16_t respSession = read_u16le(response.sessionIdL, response.sessionIdH);
                    if (respSession != _sessionId) {
                        continue;
                    }
                }

                pkt = response;
                return true;
            }
        }

        FN_LOGE(TAG, "%s TNFS timeout for command 0x%02X", _transportName, static_cast<unsigned>(pkt.command));
        return false;
    }

private:
    std::unique_ptr<fujinet::io::Channel> _channel;
    const char* _transportName;
    std::uint16_t _sessionId{0};
    std::uint8_t _sequenceNum{0};
};

} // namespace fujinet::tnfs
