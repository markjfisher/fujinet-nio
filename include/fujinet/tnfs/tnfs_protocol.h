
#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <memory>

#include "fujinet/fs/filesystem.h"

namespace fujinet::io {
class Channel;
}

namespace fujinet::tnfs {

static constexpr uint16_t DEFAULT_PORT = 16384;

static constexpr uint8_t CMD_MOUNT = 0x00;
static constexpr uint8_t CMD_UNMOUNT = 0x01;

static constexpr uint8_t CMD_OPENDIR = 0x10;
static constexpr uint8_t CMD_READDIR = 0x11;
static constexpr uint8_t CMD_CLOSEDIR = 0x12;
static constexpr uint8_t CMD_MKDIR = 0x13;
static constexpr uint8_t CMD_RMDIR = 0x14;
static constexpr uint8_t CMD_TELLDIR = 0x15;
static constexpr uint8_t CMD_SEEKDIR = 0x16;
static constexpr uint8_t CMD_OPENDIRX = 0x17;
static constexpr uint8_t CMD_READDIRX = 0x18;

static constexpr uint8_t CMD_READ = 0x21;
static constexpr uint8_t CMD_WRITE = 0x22;
static constexpr uint8_t CMD_CLOSE = 0x23;
static constexpr uint8_t CMD_STAT = 0x24;
static constexpr uint8_t CMD_LSEEK = 0x25;
static constexpr uint8_t CMD_UNLINK = 0x26;
static constexpr uint8_t CMD_CHMOD = 0x27;
static constexpr uint8_t CMD_RENAME = 0x28;
static constexpr uint8_t CMD_OPEN = 0x29;

static constexpr uint8_t CMD_SIZE = 0x30;
static constexpr uint8_t CMD_FREE = 0x31;

static constexpr uint16_t OPENMODE_READ = 0x0001;
static constexpr uint16_t OPENMODE_WRITE = 0x0002;
static constexpr uint16_t OPENMODE_READWRITE = 0x0003;
static constexpr uint16_t OPENMODE_WRITE_APPEND = 0x0008;
static constexpr uint16_t OPENMODE_WRITE_CREATE = 0x0100;
static constexpr uint16_t OPENMODE_WRITE_TRUNCATE = 0x0200;
static constexpr uint16_t OPENMODE_CREATE_EXCLUSIVE = 0x0400;

static constexpr uint8_t RESULT_SUCCESS = 0x00;
static constexpr uint8_t RESULT_NOT_PERMITTED = 0x01;
static constexpr uint8_t RESULT_FILE_NOT_FOUND = 0x02;
static constexpr uint8_t RESULT_IO_ERROR = 0x03;
static constexpr uint8_t RESULT_NO_SUCH_DEVICE = 0x04;
static constexpr uint8_t RESULT_LIST_TOO_LONG = 0x05;
static constexpr uint8_t RESULT_BAD_FILENUM = 0x06;
static constexpr uint8_t RESULT_TRY_AGAIN = 0x07;
static constexpr uint8_t RESULT_OUT_OF_MEMORY = 0x08;
static constexpr uint8_t RESULT_ACCESS_DENIED = 0x09;
static constexpr uint8_t RESULT_RESOURCE_BUSY = 0x0A;
static constexpr uint8_t RESULT_FILE_EXISTS = 0x0B;
static constexpr uint8_t RESULT_NOT_A_DIRECTORY = 0x0C;
static constexpr uint8_t RESULT_IS_DIRECTORY = 0x0D;
static constexpr uint8_t RESULT_INVALID_ARGUMENT = 0x0E;
static constexpr uint8_t RESULT_FILE_TABLE_OVERFLOW = 0x0F;
static constexpr uint8_t RESULT_TOO_MANY_FILES_OPEN = 0x10;
static constexpr uint8_t RESULT_FILE_TOO_LARGE = 0x11;
static constexpr uint8_t RESULT_NO_SPACE_ON_DEVICE = 0x12;
static constexpr uint8_t RESULT_CANNOT_SEEK_PIPE = 0x13;
static constexpr uint8_t RESULT_READONLY_FILESYSTEM = 0x14;
static constexpr uint8_t RESULT_NAME_TOO_LONG = 0x15;
static constexpr uint8_t RESULT_FUNCTION_UNIMPLEMENTED = 0x16;
static constexpr uint8_t RESULT_DIRECTORY_NOT_EMPTY = 0x17;
static constexpr uint8_t RESULT_TOO_MANY_SYMLINKS = 0x18;
static constexpr uint8_t RESULT_NO_DATA_AVAILABLE = 0x19;
static constexpr uint8_t RESULT_OUT_OF_STREAMS = 0x1A;
static constexpr uint8_t RESULT_PROTOCOL_ERROR = 0x1B;
static constexpr uint8_t RESULT_BAD_FILE_DESCRIPTOR = 0x1C;
static constexpr uint8_t RESULT_TOO_MANY_USERS = 0x1D;
static constexpr uint8_t RESULT_OUT_OF_BUFFER_SPACE = 0x1E;
static constexpr uint8_t RESULT_ALREADY_IN_PROGRESS = 0x1F;
static constexpr uint8_t RESULT_STALE_HANDLE = 0x20;
static constexpr uint8_t RESULT_END_OF_FILE = 0x21;
static constexpr uint8_t RESULT_INVALID_HANDLE = 0xFF;

struct TnfsPacket {
    uint8_t sessionIdL;
    uint8_t sessionIdH;
    uint8_t sequenceNum;
    uint8_t command;
    uint8_t payload[528];
};

struct TnfsStat {
    bool isDir;
    uint32_t filesize;
    uint32_t aTime;
    uint32_t mTime;
    uint32_t cTime;
    uint16_t mode;
};

class ITnfsClient {
public:
    virtual ~ITnfsClient() = default;

    virtual bool mount(const std::string& mountPath, const std::string& user = "", const std::string& password = "") = 0;
    virtual bool umount() = 0;

    virtual bool stat(const std::string& path, TnfsStat& stat) = 0;
    virtual bool exists(const std::string& path) = 0;
    virtual bool isDirectory(const std::string& path) = 0;

    virtual bool createDirectory(const std::string& path) = 0;
    virtual bool removeDirectory(const std::string& path) = 0;
    virtual bool removeFile(const std::string& path) = 0;
    virtual bool rename(const std::string& from, const std::string& to) = 0;

    virtual std::vector<std::string> listDirectory(const std::string& path) = 0;

    virtual int open(const std::string& path, uint16_t openMode, uint16_t createPerms) = 0;
    virtual bool close(int fileHandle) = 0;

    virtual std::size_t read(int fileHandle, void* buffer, std::size_t bytes) = 0;
    virtual std::size_t write(int fileHandle, const void* buffer, std::size_t bytes) = 0;
    virtual bool seek(int fileHandle, uint32_t offset) = 0;
    virtual uint32_t tell(int fileHandle) = 0;
};

std::unique_ptr<ITnfsClient> make_udp_tnfs_client(std::unique_ptr<fujinet::io::Channel> channel);
std::unique_ptr<ITnfsClient> make_tcp_tnfs_client(std::unique_ptr<fujinet::io::Channel> channel);

} // namespace fujinet::tnfs
