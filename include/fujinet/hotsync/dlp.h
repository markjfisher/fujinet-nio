#pragma once

// Desktop Link Protocol message encoding.
// Reference: palm-sync src/protocols/dlp-protocol.ts, Palm OS SDK DLCommon.h

#include "fujinet/hotsync/bytes.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace fujinet::hotsync {

enum class DlpFunc : std::uint8_t {
    ReadUserInfo = 0x10,
    WriteUserInfo = 0x11,
    ReadSysInfo = 0x12,
    GetSysDateTime = 0x13,
    SetSysDateTime = 0x14,
    ReadStorageInfo = 0x15,
    ReadDBList = 0x16,
    OpenDB = 0x17,
    CreateDB = 0x18,
    CloseDB = 0x19,
    DeleteDB = 0x1A,
    ReadAppBlock = 0x1B,
    WriteAppBlock = 0x1C,
    ReadSortBlock = 0x1D,
    WriteSortBlock = 0x1E,
    ReadRecord = 0x20,
    WriteRecord = 0x21,
    DeleteRecord = 0x22,
    ReadResource = 0x23,
    WriteResource = 0x24,
    ResetSystem = 0x29,
    AddSyncLogEntry = 0x2A,
    ReadOpenDBInfo = 0x2B,
    OpenConduit = 0x2E,
    EndOfSync = 0x2F,
};

enum class DlpError : std::uint16_t {
    None = 0,
    System = 1,
    IllegalReq = 2,
    Memory = 3,
    Param = 4,
    NotFound = 5,
    NoneOpen = 6,
    DatabaseOpen = 7,
    TooManyOpenDatabases = 8,
    AlreadyExists = 9,
    CantOpen = 10,
    RecordDeleted = 11,
    RecordBusy = 12,
    NotSupported = 13,
    ReadOnly = 15,
    NotEnoughSpace = 16,
    LimitExceeded = 17,
    CancelSync = 18,
    // Not a Palm code: the transport failed before a reply arrived.
    Transport = 0xFFFF,
};

const char* dlp_error_name(DlpError error);

inline constexpr std::uint8_t kDlpArgIdBase = 0x20;

// Big-endian field writer for one DLP argument.
class DlpArgWriter {
public:
    DlpArgWriter& u8(std::uint8_t v);
    DlpArgWriter& u16(std::uint16_t v);
    DlpArgWriter& u32(std::uint32_t v);
    DlpArgWriter& bytes(const std::uint8_t* data, std::size_t len);
    DlpArgWriter& bytes(const Bytes& data) { return bytes(data.data(), data.size()); }
    DlpArgWriter& cstring(const std::string& s);
    // A four-character type or creator ID.
    DlpArgWriter& type_id(const char id[4]);

    const Bytes& data() const { return _data; }

private:
    Bytes _data;
};

// Bounds-checked big-endian field reader over one DLP argument. A read past
// the end yields zeros and marks the reader failed.
class DlpArgReader {
public:
    explicit DlpArgReader(const Bytes& data) : _data(data) {}

    std::uint8_t u8();
    std::uint16_t u16();
    std::uint32_t u32();
    Bytes bytes(std::size_t len);
    Bytes rest();
    std::string cstring();
    void skip(std::size_t len);

    std::size_t remaining() const { return _pos < _data.size() ? _data.size() - _pos : 0; }
    bool failed() const { return _failed; }

private:
    const Bytes& _data;
    std::size_t _pos{0};
    bool _failed{false};
};

struct DlpRequest {
    struct Arg {
        std::uint8_t index;
        Bytes data;
    };

    DlpFunc func;
    std::vector<Arg> args;

    explicit DlpRequest(DlpFunc f) : func(f) {}
    // Arguments are numbered from kDlpArgIdBase; a few requests use index 1.
    DlpRequest& arg(const DlpArgWriter& writer, std::uint8_t index = 0);
    Bytes encode() const;
};

struct DlpResponse {
    DlpError error{DlpError::Transport};
    // Indexed by argument ID minus kDlpArgIdBase; absent arguments are empty.
    std::vector<Bytes> args;

    bool ok() const { return error == DlpError::None; }
    const Bytes& arg(std::size_t index) const;
    bool decode(DlpFunc expected, const Bytes& message);
};

} // namespace fujinet::hotsync
