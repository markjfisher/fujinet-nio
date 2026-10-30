#pragma once

// One HotSync, from the first DLP request to EndOfSync: identify the user,
// install queued files, back up databases, then stamp the device's user info.
// Reference: palm-sync src/sync-utils/{sync-device,write-db,read-db}.ts

#include "fujinet/hotsync/dlp_client.h"
#include "fujinet/hotsync/palm_database.h"
#include "fujinet/hotsync/storage.h"

#include <cstddef>
#include <cstdint>
#include <string>

namespace fujinet::hotsync {

enum class BackupPolicy : std::uint8_t {
    None,
    // Databases whose backup bit is set, which is what Palm Desktop saves.
    Flagged,
    All,
};

// "FJNT": marks this FujiNet as the last machine the device synced with.
inline constexpr std::uint32_t kFujiNetPcId = 0x464A4E54;

// The largest database held in memory for an install or a backup. On ESP32
// allocations this size come from PSRAM.
inline constexpr std::size_t kDefaultMaxDatabaseBytes = 1024 * 1024;

struct SessionOptions {
    // Given to a device that has never been synced.
    std::string defaultUserName{"FujiNet"};
    std::uint32_t newUserId{0};
    std::uint32_t pcId{kFujiNetPcId};
    BackupPolicy backup{BackupPolicy::Flagged};
    std::size_t maxDatabaseBytes{kDefaultMaxDatabaseBytes};
    // Local wall-clock time of the sync; a zero year leaves the device's
    // last-sync date alone.
    DlpDateTime now;
};

struct SessionReport {
    std::string userName;
    int installed{0};
    int installFailures{0};
    int backedUp{0};
    int backupFailures{0};
    DlpError error{DlpError::None};
};

class Session {
public:
    Session(DlpTransport& transport, Storage& storage, SessionOptions options);

    // The transport must already have completed accept().
    SessionReport run();

private:
    DlpError identify_user();
    DlpError install_pending();
    DlpError install_file(const std::string& fileName);
    DlpError write_database(const PalmDatabase& db);
    DlpError write_contents(std::uint8_t handle, const PalmDatabase& db);
    DlpError backup_databases();
    DlpError backup_database(const DlpDbInfo& info);
    DlpError read_contents(std::uint8_t handle, PalmDatabase& db);
    DlpError finish();
    void log(const std::string& line);

    DlpClient _dlp;
    Storage& _storage;
    SessionOptions _options;
    DlpUserInfo _user;
    bool _userIsNew{false};
    bool _resetAfterSync{false};
    SessionReport _report;
};

// Device names can hold characters a FAT file name cannot.
std::string safe_file_name(const std::string& name);
std::string backup_file_name(const DlpDbInfo& info);

} // namespace fujinet::hotsync
