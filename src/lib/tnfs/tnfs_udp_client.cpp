
#include "fujinet/tnfs/tnfs_protocol.h"
#include "fujinet/io/core/channel.h"
#include "fujinet/core/logging.h"

#include <cstring>
#include <vector>
#include <memory>

namespace fujinet::tnfs {

static constexpr const char* TAG = "tnfs";
static constexpr std::size_t PACKET_SIZE = 532; // 4-byte header + 528-byte payload

class UdpTnfsClient : public ITnfsClient {
private:
    std::unique_ptr<fujinet::io::Channel> _channel;
    uint16_t _sessionId;
    uint8_t _sequenceNum;

public:
    explicit UdpTnfsClient(std::unique_ptr<fujinet::io::Channel> channel)
        : _channel(std::move(channel)),
          _sessionId(0),
          _sequenceNum(0)
    {
        FN_LOGI(TAG, "UDP TNFS client created");
    }

    ~UdpTnfsClient() override {
        umount();
    }

    bool mount(const std::string& mountPath, const std::string& user, const std::string& password) override {
        TnfsPacket pkt = {};
        pkt.command = CMD_MOUNT;
        pkt.sequenceNum = _sequenceNum++;

        // Version 1.2
        pkt.payload[0] = 0x02; // Minor
        pkt.payload[1] = 0x01; // Major

        std::size_t offset = 2;

        // Mount path
        if (!mountPath.empty()) {
            std::strncpy(reinterpret_cast<char*>(pkt.payload + offset), mountPath.c_str(), sizeof(pkt.payload) - offset - 1);
            offset += mountPath.size() + 1;
        } else {
            pkt.payload[offset++] = '\0';
        }

        // User
        if (!user.empty()) {
            std::strncpy(reinterpret_cast<char*>(pkt.payload + offset), user.c_str(), sizeof(pkt.payload) - offset - 1);
            offset += user.size() + 1;
        } else {
            pkt.payload[offset++] = '\0';
        }

        // Password
        if (!password.empty()) {
            std::strncpy(reinterpret_cast<char*>(pkt.payload + offset), password.c_str(), sizeof(pkt.payload) - offset - 1);
            offset += password.size() + 1;
        } else {
            pkt.payload[offset++] = '\0';
        }

        if (!sendAndReceive(pkt, offset)) {
            FN_LOGE(TAG, "Mount failed: no response");
            return false;
        }

        if (pkt.payload[0] != RESULT_SUCCESS) {
            FN_LOGE(TAG, "Mount failed: %d", pkt.payload[0]);
            return false;
        }

        _sessionId = (static_cast<uint16_t>(pkt.sessionIdH) << 8) | pkt.sessionIdL;

        FN_LOGI(TAG, "Mounted successfully, session ID: 0x%04X", _sessionId);
        return true;
    }

    bool umount() override {
        if (_sessionId == 0) {
            return true;
        }

        TnfsPacket pkt = {};
        pkt.sessionIdL = static_cast<uint8_t>(_sessionId & 0xFF);
        pkt.sessionIdH = static_cast<uint8_t>((_sessionId >> 8) & 0xFF);
        pkt.sequenceNum = _sequenceNum++;
        pkt.command = CMD_UNMOUNT;

        if (!sendAndReceive(pkt, 0)) {
            FN_LOGE(TAG, "Unmount failed: no response");
            return false;
        }

        if (pkt.payload[0] != RESULT_SUCCESS) {
            FN_LOGE(TAG, "Unmount failed: %d", pkt.payload[0]);
            return false;
        }

        _sessionId = 0;
        FN_LOGI(TAG, "Unmounted successfully");
        return true;
    }

    bool stat(const std::string& path, TnfsStat& outStat) override {
        if (_sessionId == 0) {
            FN_LOGE(TAG, "Not mounted");
            return false;
        }

        TnfsPacket pkt = {};
        pkt.sessionIdL = static_cast<uint8_t>(_sessionId & 0xFF);
        pkt.sessionIdH = static_cast<uint8_t>((_sessionId >> 8) & 0xFF);
        pkt.sequenceNum = _sequenceNum++;
        pkt.command = CMD_STAT;

        std::strncpy(reinterpret_cast<char*>(pkt.payload), path.c_str(), sizeof(pkt.payload) - 1);
        std::size_t offset = path.size() + 1;

        if (!sendAndReceive(pkt, offset)) {
            FN_LOGE(TAG, "Stat failed: no response");
            return false;
        }

        if (pkt.payload[0] != RESULT_SUCCESS) {
            FN_LOGE(TAG, "Stat failed: %d", pkt.payload[0]);
            return false;
        }

        // Parse stat response
        outStat.isDir = static_cast<bool>(pkt.payload[1]);
        outStat.filesize = (static_cast<uint32_t>(pkt.payload[5]) << 24) |
                       (static_cast<uint32_t>(pkt.payload[4]) << 16) |
                       (static_cast<uint32_t>(pkt.payload[3]) << 8) |
                       (static_cast<uint32_t>(pkt.payload[2]));
        outStat.aTime = (static_cast<uint32_t>(pkt.payload[9]) << 24) |
                    (static_cast<uint32_t>(pkt.payload[8]) << 16) |
                    (static_cast<uint32_t>(pkt.payload[7]) << 8) |
                    (static_cast<uint32_t>(pkt.payload[6]));
        outStat.mTime = (static_cast<uint32_t>(pkt.payload[13]) << 24) |
                    (static_cast<uint32_t>(pkt.payload[12]) << 16) |
                    (static_cast<uint32_t>(pkt.payload[11]) << 8) |
                    (static_cast<uint32_t>(pkt.payload[10]));
        outStat.cTime = (static_cast<uint32_t>(pkt.payload[17]) << 24) |
                    (static_cast<uint32_t>(pkt.payload[16]) << 16) |
                    (static_cast<uint32_t>(pkt.payload[15]) << 8) |
                    (static_cast<uint32_t>(pkt.payload[14]));
        outStat.mode = (static_cast<uint16_t>(pkt.payload[19]) << 8) |
                   (static_cast<uint16_t>(pkt.payload[18]));

        return true;
    }

    bool exists(const std::string& path) override {
        TnfsStat stat;
        return this->stat(path, stat);
    }

    bool isDirectory(const std::string& path) override {
        TnfsStat stat;
        if (!this->stat(path, stat)) {
            return false;
        }
        return stat.isDir;
    }

    bool createDirectory(const std::string& path) override {
        if (_sessionId == 0) {
            FN_LOGE(TAG, "Not mounted");
            return false;
        }

        TnfsPacket pkt = {};
        pkt.sessionIdL = static_cast<uint8_t>(_sessionId & 0xFF);
        pkt.sessionIdH = static_cast<uint8_t>((_sessionId >> 8) & 0xFF);
        pkt.sequenceNum = _sequenceNum++;
        pkt.command = CMD_MKDIR;

        std::strncpy(reinterpret_cast<char*>(pkt.payload), path.c_str(), sizeof(pkt.payload) - 1);
        std::size_t offset = path.size() + 1;

        if (!sendAndReceive(pkt, offset)) {
            FN_LOGE(TAG, "Mkdir failed: no response");
            return false;
        }

        if (pkt.payload[0] != RESULT_SUCCESS) {
            FN_LOGE(TAG, "Mkdir failed: %d", pkt.payload[0]);
            return false;
        }

        return true;
    }

    bool removeDirectory(const std::string& path) override {
        if (_sessionId == 0) {
            FN_LOGE(TAG, "Not mounted");
            return false;
        }

        TnfsPacket pkt = {};
        pkt.sessionIdL = static_cast<uint8_t>(_sessionId & 0xFF);
        pkt.sessionIdH = static_cast<uint8_t>((_sessionId >> 8) & 0xFF);
        pkt.sequenceNum = _sequenceNum++;
        pkt.command = CMD_RMDIR;

        std::strncpy(reinterpret_cast<char*>(pkt.payload), path.c_str(), sizeof(pkt.payload) - 1);
        std::size_t offset = path.size() + 1;

        if (!sendAndReceive(pkt, offset)) {
            FN_LOGE(TAG, "Rmdir failed: no response");
            return false;
        }

        if (pkt.payload[0] != RESULT_SUCCESS) {
            FN_LOGE(TAG, "Rmdir failed: %d", pkt.payload[0]);
            return false;
        }

        return true;
    }

    bool removeFile(const std::string& path) override {
        if (_sessionId == 0) {
            FN_LOGE(TAG, "Not mounted");
            return false;
        }

        TnfsPacket pkt = {};
        pkt.sessionIdL = static_cast<uint8_t>(_sessionId & 0xFF);
        pkt.sessionIdH = static_cast<uint8_t>((_sessionId >> 8) & 0xFF);
        pkt.sequenceNum = _sequenceNum++;
        pkt.command = CMD_UNLINK;

        std::strncpy(reinterpret_cast<char*>(pkt.payload), path.c_str(), sizeof(pkt.payload) - 1);
        std::size_t offset = path.size() + 1;

        if (!sendAndReceive(pkt, offset)) {
            FN_LOGE(TAG, "Unlink failed: no response");
            return false;
        }

        if (pkt.payload[0] != RESULT_SUCCESS) {
            FN_LOGE(TAG, "Unlink failed: %d", pkt.payload[0]);
            return false;
        }

        return true;
    }

    bool rename(const std::string& from, const std::string& to) override {
        if (_sessionId == 0) {
            FN_LOGE(TAG, "Not mounted");
            return false;
        }

        TnfsPacket pkt = {};
        pkt.sessionIdL = static_cast<uint8_t>(_sessionId & 0xFF);
        pkt.sessionIdH = static_cast<uint8_t>((_sessionId >> 8) & 0xFF);
        pkt.sequenceNum = _sequenceNum++;
        pkt.command = CMD_RENAME;

        std::strncpy(reinterpret_cast<char*>(pkt.payload), from.c_str(), sizeof(pkt.payload) - 1);
        std::size_t offset = from.size() + 1;
        std::strncpy(reinterpret_cast<char*>(pkt.payload + offset), to.c_str(), sizeof(pkt.payload) - offset - 1);
        offset += to.size() + 1;

        if (!sendAndReceive(pkt, offset)) {
            FN_LOGE(TAG, "Rename failed: no response");
            return false;
        }

        if (pkt.payload[0] != RESULT_SUCCESS) {
            FN_LOGE(TAG, "Rename failed: %d", pkt.payload[0]);
            return false;
        }

        return true;
    }

    std::vector<std::string> listDirectory(const std::string& path) override {
        std::vector<std::string> entries;

        if (_sessionId == 0) {
            FN_LOGE(TAG, "Not mounted");
            return entries;
        }

        TnfsPacket pkt = {};
        pkt.sessionIdL = static_cast<uint8_t>(_sessionId & 0xFF);
        pkt.sessionIdH = static_cast<uint8_t>((_sessionId >> 8) & 0xFF);
        pkt.sequenceNum = _sequenceNum++;
        pkt.command = CMD_OPENDIR;

        std::strncpy(reinterpret_cast<char*>(pkt.payload), path.c_str(), sizeof(pkt.payload) - 1);
        std::size_t offset = path.size() + 1;

        if (!sendAndReceive(pkt, offset)) {
            FN_LOGE(TAG, "Opendir failed: no response");
            return entries;
        }

        if (pkt.payload[0] != RESULT_SUCCESS) {
            FN_LOGE(TAG, "Opendir failed: %d", pkt.payload[0]);
            return entries;
        }

        int dirHandle = pkt.payload[1];

        while (true) {
            TnfsPacket readPkt = {};
            readPkt.sessionIdL = static_cast<uint8_t>(_sessionId & 0xFF);
            readPkt.sessionIdH = static_cast<uint8_t>((_sessionId >> 8) & 0xFF);
            readPkt.sequenceNum = _sequenceNum++;
            readPkt.command = CMD_READDIR;
            readPkt.payload[0] = static_cast<uint8_t>(dirHandle);

            if (!sendAndReceive(readPkt, 1)) {
                FN_LOGE(TAG, "Readdir failed: no response");
                break;
            }

            if (readPkt.payload[0] == RESULT_END_OF_FILE) {
                break;
            }

            if (readPkt.payload[0] != RESULT_SUCCESS) {
                FN_LOGE(TAG, "Readdir failed: %d", readPkt.payload[0]);
                break;
            }

            std::string entry(reinterpret_cast<char*>(readPkt.payload + 1));
            if (entry != "." && entry != "..") {
                entries.push_back(entry);
            }
        }

        TnfsPacket closePkt = {};
        closePkt.sessionIdL = static_cast<uint8_t>(_sessionId & 0xFF);
        closePkt.sessionIdH = static_cast<uint8_t>((_sessionId >> 8) & 0xFF);
        closePkt.sequenceNum = _sequenceNum++;
        closePkt.command = CMD_CLOSEDIR;
        closePkt.payload[0] = static_cast<uint8_t>(dirHandle);
        sendAndReceive(closePkt, 1);

        return entries;
    }

    int open(const std::string& path, uint16_t openMode, uint16_t createPerms) override {
        if (_sessionId == 0) {
            FN_LOGE(TAG, "Not mounted");
            return -1;
        }

        TnfsPacket pkt = {};
        pkt.sessionIdL = static_cast<uint8_t>(_sessionId & 0xFF);
        pkt.sessionIdH = static_cast<uint8_t>((_sessionId >> 8) & 0xFF);
        pkt.sequenceNum = _sequenceNum++;
        pkt.command = CMD_OPEN;

        pkt.payload[0] = static_cast<uint8_t>(openMode & 0xFF);
        pkt.payload[1] = static_cast<uint8_t>((openMode >> 8) & 0xFF);
        pkt.payload[2] = static_cast<uint8_t>(createPerms & 0xFF);
        pkt.payload[3] = static_cast<uint8_t>((createPerms >> 8) & 0xFF);

        std::strncpy(reinterpret_cast<char*>(pkt.payload + 4), path.c_str(), sizeof(pkt.payload) - 5);
        std::size_t offset = 4 + path.size() + 1;

        if (!sendAndReceive(pkt, offset)) {
            FN_LOGE(TAG, "Open failed: no response");
            return -1;
        }

        if (pkt.payload[0] != RESULT_SUCCESS) {
            FN_LOGE(TAG, "Open failed: %d", pkt.payload[0]);
            return -1;
        }

        int fileHandle = static_cast<int>(pkt.payload[1]);
        return fileHandle;
    }

    bool close(int fileHandle) override {
        if (_sessionId == 0) {
            FN_LOGE(TAG, "Not mounted");
            return false;
        }

        TnfsPacket pkt = {};
        pkt.sessionIdL = static_cast<uint8_t>(_sessionId & 0xFF);
        pkt.sessionIdH = static_cast<uint8_t>((_sessionId >> 8) & 0xFF);
        pkt.sequenceNum = _sequenceNum++;
        pkt.command = CMD_CLOSE;

        pkt.payload[0] = static_cast<uint8_t>(fileHandle);

        if (!sendAndReceive(pkt, 1)) {
            FN_LOGE(TAG, "Close failed: no response");
            return false;
        }

        if (pkt.payload[0] != RESULT_SUCCESS) {
            FN_LOGE(TAG, "Close failed: %d", pkt.payload[0]);
            return false;
        }

        return true;
    }

    std::size_t read(int fileHandle, void* buffer, std::size_t bytes) override {
        if (_sessionId == 0) {
            FN_LOGE(TAG, "Not mounted");
            return 0;
        }

        if (bytes > 512) {
            bytes = 512;
        }

        TnfsPacket pkt = {};
        pkt.sessionIdL = static_cast<uint8_t>(_sessionId & 0xFF);
        pkt.sessionIdH = static_cast<uint8_t>((_sessionId >> 8) & 0xFF);
        pkt.sequenceNum = _sequenceNum++;
        pkt.command = CMD_READ;

        pkt.payload[0] = static_cast<uint8_t>(fileHandle);
        pkt.payload[1] = static_cast<uint8_t>(bytes & 0xFF);
        pkt.payload[2] = static_cast<uint8_t>((bytes >> 8) & 0xFF);

        if (!sendAndReceive(pkt, 3)) {
            FN_LOGE(TAG, "Read failed: no response");
            return 0;
        }

        if (pkt.payload[0] == RESULT_END_OF_FILE) {
            return 0;
        }

        if (pkt.payload[0] != RESULT_SUCCESS) {
            FN_LOGE(TAG, "Read failed: %d", pkt.payload[0]);
            return 0;
        }

        uint16_t bytesRead = (static_cast<uint16_t>(pkt.payload[2]) << 8) |
                           (static_cast<uint16_t>(pkt.payload[1]));
        if (bytesRead > bytes) {
            bytesRead = static_cast<uint16_t>(bytes);
        }

        std::memcpy(buffer, pkt.payload + 3, bytesRead);
        return bytesRead;
    }

    std::size_t write(int fileHandle, const void* buffer, std::size_t bytes) override {
        if (_sessionId == 0) {
            FN_LOGE(TAG, "Not mounted");
            return 0;
        }

        if (bytes > 512) {
            bytes = 512;
        }

        TnfsPacket pkt = {};
        pkt.sessionIdL = static_cast<uint8_t>(_sessionId & 0xFF);
        pkt.sessionIdH = static_cast<uint8_t>((_sessionId >> 8) & 0xFF);
        pkt.sequenceNum = _sequenceNum++;
        pkt.command = CMD_WRITE;

        pkt.payload[0] = static_cast<uint8_t>(fileHandle);
        pkt.payload[1] = static_cast<uint8_t>(bytes & 0xFF);
        pkt.payload[2] = static_cast<uint8_t>((bytes >> 8) & 0xFF);

        std::memcpy(pkt.payload + 3, buffer, bytes);

        if (!sendAndReceive(pkt, 3 + bytes)) {
            FN_LOGE(TAG, "Write failed: no response");
            return 0;
        }

        if (pkt.payload[0] != RESULT_SUCCESS) {
            FN_LOGE(TAG, "Write failed: %d", pkt.payload[0]);
            return 0;
        }

        uint16_t bytesWritten = (static_cast<uint16_t>(pkt.payload[2]) << 8) |
                              (static_cast<uint16_t>(pkt.payload[1]));
        return bytesWritten;
    }

    bool seek(int fileHandle, uint32_t offset) override {
        if (_sessionId == 0) {
            FN_LOGE(TAG, "Not mounted");
            return false;
        }

        TnfsPacket pkt = {};
        pkt.sessionIdL = static_cast<uint8_t>(_sessionId & 0xFF);
        pkt.sessionIdH = static_cast<uint8_t>((_sessionId >> 8) & 0xFF);
        pkt.sequenceNum = _sequenceNum++;
        pkt.command = CMD_LSEEK;

        pkt.payload[0] = static_cast<uint8_t>(fileHandle);
        pkt.payload[1] = static_cast<uint8_t>(offset & 0xFF);
        pkt.payload[2] = static_cast<uint8_t>((offset >> 8) & 0xFF);
        pkt.payload[3] = static_cast<uint8_t>((offset >> 16) & 0xFF);
        pkt.payload[4] = static_cast<uint8_t>((offset >> 24) & 0xFF);
        pkt.payload[5] = 0; // SEEK_SET

        if (!sendAndReceive(pkt, 6)) {
            FN_LOGE(TAG, "Seek failed: no response");
            return false;
        }

        if (pkt.payload[0] != RESULT_SUCCESS) {
            FN_LOGE(TAG, "Seek failed: %d", pkt.payload[0]);
            return false;
        }

        return true;
    }

    uint32_t tell(int fileHandle) override {
        // TODO: Implement tell using LSEEK with SEEK_CUR and offset 0
        FN_LOGE(TAG, "Tell not implemented");
        return 0;
    }

private:
    bool sendAndReceive(TnfsPacket& pkt, std::size_t payloadSize) {
        // Send packet
        std::vector<uint8_t> buffer(PACKET_SIZE);
        std::memcpy(buffer.data(), &pkt, 4 + payloadSize);
        _channel->write(buffer.data(), 4 + payloadSize);

        // Wait for response
        static constexpr int timeoutMs = 2000;
        static constexpr int retryCount = 5;

        for (int i = 0; i < retryCount; ++i) {
            if (!_channel->available()) {
                // TODO: Implement sleep
                continue;
            }

            std::size_t bytesRead = _channel->read(reinterpret_cast<uint8_t*>(&pkt), PACKET_SIZE);
            if (bytesRead < 5) {
                continue;
            }

            if (pkt.sequenceNum != _sequenceNum - 1) {
                continue;
            }

            if (_sessionId != 0) {
                uint16_t responseSessionId = (static_cast<uint16_t>(pkt.sessionIdH) << 8) | pkt.sessionIdL;
                if (responseSessionId != _sessionId) {
                    continue;
                }
            }

            return true;
        }

        return false;
    }
};

std::unique_ptr<ITnfsClient> make_udp_tnfs_client(std::unique_ptr<fujinet::io::Channel> channel) {
    return std::make_unique<UdpTnfsClient>(std::move(channel));
}

} // namespace fujinet::tnfs
