#pragma once

// Typed DLP commands issued by the desktop side of a HotSync.
// Reference: palm-sync src/protocols/dlp-commands.ts

#include "fujinet/hotsync/dlp.h"
#include "fujinet/hotsync/dlp_transport.h"

#include <cstdint>
#include <string>
#include <vector>

namespace fujinet::hotsync {

struct DlpDateTime {
    std::uint16_t year{0}; // 0 means "never"
    std::uint8_t month{0};
    std::uint8_t day{0};
    std::uint8_t hour{0};
    std::uint8_t minute{0};
    std::uint8_t second{0};
};

struct DlpSysInfo {
    std::uint32_t romVersion{0};
    std::uint32_t localizationId{0};
    std::uint16_t dlpMajor{0};
    std::uint16_t dlpMinor{0};
    std::uint32_t maxRecordSize{0};
};

struct DlpUserInfo {
    std::uint32_t userId{0};
    std::uint32_t viewerId{0};
    std::uint32_t lastSyncPc{0};
    DlpDateTime successfulSync;
    DlpDateTime lastSync;
    std::string userName;
};

// Database attribute bits (DlpDbInfo::flags).
inline constexpr std::uint16_t kDbResource = 0x0001;
inline constexpr std::uint16_t kDbReadOnly = 0x0002;
inline constexpr std::uint16_t kDbBackup = 0x0008;
inline constexpr std::uint16_t kDbResetAfterInstall = 0x0020;

struct DlpDbInfo {
    std::uint8_t miscFlags{0};
    std::uint16_t flags{0};
    char type[5] = {};
    char creator[5] = {};
    std::uint16_t version{0};
    std::uint32_t modNum{0};
    DlpDateTime created;
    DlpDateTime modified;
    DlpDateTime backedUp;
    std::uint16_t index{0};
    std::string name;
};

// DlpRecord::attributes bits.
inline constexpr std::uint8_t kRecordDeleted = 0x80;
inline constexpr std::uint8_t kRecordDirty = 0x40;
inline constexpr std::uint8_t kRecordArchived = 0x08;

struct DlpRecord {
    std::uint32_t id{0};
    std::uint8_t attributes{0};
    std::uint8_t category{0};
    Bytes data;
};

struct DlpResource {
    char type[5] = {};
    std::uint16_t id{0};
    Bytes data;
};

// OpenDB mode bits.
inline constexpr std::uint8_t kOpenRead = 0x80;
inline constexpr std::uint8_t kOpenWrite = 0x40;
inline constexpr std::uint8_t kOpenExclusive = 0x20;
inline constexpr std::uint8_t kOpenSecret = 0x10;

// ReadDBList search bits.
inline constexpr std::uint8_t kListRam = 0x80;
inline constexpr std::uint8_t kListRom = 0x40;
inline constexpr std::uint8_t kListMultiple = 0x20;

// WriteUserInfo bits naming the fields to change.
inline constexpr std::uint8_t kModUserId = 0x80;
inline constexpr std::uint8_t kModLastSyncPc = 0x40;
inline constexpr std::uint8_t kModLastSyncDate = 0x20;
inline constexpr std::uint8_t kModUserName = 0x10;
inline constexpr std::uint8_t kModViewerId = 0x08;

class DlpClient {
public:
    explicit DlpClient(DlpTransport& transport) : _transport(transport) {}

    DlpError read_sys_info(DlpSysInfo& out);
    DlpError read_user_info(DlpUserInfo& out);
    DlpError write_user_info(const DlpUserInfo& info, std::uint8_t modFlags);
    // Appends one batch to out; nextIndex is where the following batch starts.
    DlpError read_db_list(std::uint8_t flags, std::uint16_t start, std::vector<DlpDbInfo>& out,
                          std::uint16_t& nextIndex);

    DlpError open_conduit();
    DlpError open_db(const std::string& name, std::uint8_t mode, std::uint8_t& handle);
    DlpError create_db(const DlpDbInfo& info, std::uint8_t& handle);
    DlpError close_db(std::uint8_t handle);
    DlpError delete_db(const std::string& name);
    DlpError read_open_db_record_count(std::uint8_t handle, std::uint16_t& count);

    DlpError read_app_block(std::uint8_t handle, Bytes& out);
    DlpError read_sort_block(std::uint8_t handle, Bytes& out);
    DlpError write_app_block(std::uint8_t handle, const Bytes& data);
    DlpError write_sort_block(std::uint8_t handle, const Bytes& data);

    DlpError get_sys_date_time(DlpDateTime& out);

    DlpError read_record_by_index(std::uint8_t handle, std::uint16_t index, DlpRecord& out);
    DlpError read_record_by_id(std::uint8_t handle, std::uint32_t recordId, DlpRecord& out);
    // record.id 0 adds a record; newId is the ID the device gave it.
    DlpError write_record(std::uint8_t handle, const DlpRecord& record, std::uint32_t* newId = nullptr);
    DlpError delete_record(std::uint8_t handle, std::uint32_t recordId);
    DlpError read_resource_by_index(std::uint8_t handle, std::uint16_t index, DlpResource& out);
    DlpError write_resource(std::uint8_t handle, const DlpResource& resource);

    DlpError add_sync_log_entry(const std::string& text);
    DlpError reset_system();
    DlpError end_of_sync(std::uint16_t termCode = 0);

private:
    DlpError execute(const DlpRequest& request, DlpResponse& response);
    DlpError read_block(DlpFunc func, std::uint8_t handle, Bytes& out);
    DlpError write_block(DlpFunc func, std::uint8_t handle, const Bytes& data);

    DlpTransport& _transport;
};

} // namespace fujinet::hotsync
