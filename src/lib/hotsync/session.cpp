// Derived from palm-sync (https://github.com/jichu4n/palm-sync),
// Copyright Chuan Ji, Apache License 2.0. See docs/hotsync.md.

// HotSync builds only (FN_WITH_HOTSYNC): the ESP32 build compiles every
// source, so elsewhere this file compiles to nothing.
#if FN_WITH_HOTSYNC == 1

#include "fujinet/hotsync/session.h"

#include "fujinet/core/logging.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <utility>

namespace fujinet::hotsync {

namespace {

constexpr const char* TAG = "hotsync";

// Databases every device regenerates; backing them up only wastes time.
const char* const kSkipBackup[] = {"Unsaved Preferences"};

// EndOfSync termination codes.
constexpr std::uint16_t kEndCancelled = 2;
constexpr std::uint16_t kEndError = 3;

bool has_install_extension(const std::string& fileName)
{
    const std::size_t dot = fileName.rfind('.');
    if (dot == std::string::npos) {
        return false;
    }
    std::string ext = fileName.substr(dot + 1);
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext == "prc" || ext == "pdb" || ext == "pqa";
}

// Per-database errors are skipped; a dead link or a user cancel ends the sync.
bool is_fatal(DlpError err)
{
    return err == DlpError::Transport || err == DlpError::CancelSync;
}

} // namespace

std::string safe_file_name(const std::string& name)
{
    std::string safe;
    safe.reserve(name.size());
    for (char c : name) {
        const bool keep = std::isalnum(static_cast<unsigned char>(c)) || c == ' ' || c == '-' ||
                          c == '_' || c == '.';
        safe += keep ? c : '_';
    }
    return safe;
}

std::string backup_file_name(const DlpDbInfo& info)
{
    return safe_file_name(info.name) + ((info.flags & kDbResource) ? ".prc" : ".pdb");
}

Session::Session(DlpTransport& transport, Storage& storage, SessionOptions options)
    : _dlp(transport), _storage(storage), _options(std::move(options))
{
}

SessionReport Session::run()
{
    DlpError err = identify_user();
    if (err == DlpError::None) {
        err = _dlp.open_conduit();
    }
    if (err == DlpError::None) {
        err = install_pending();
    }
    if (err == DlpError::None) {
        err = backup_databases();
    }
    if (err == DlpError::None) {
        err = finish();
    } else if (err != DlpError::Transport) {
        _dlp.end_of_sync(err == DlpError::CancelSync ? kEndCancelled : kEndError);
    }

    _report.error = err;
    _report.userName = _user.userName;
    FN_LOGI(TAG, "done for '%s': %d installed, %d backed up, result %s",
            _report.userName.c_str(), _report.installed, _report.backedUp, dlp_error_name(err));
    return _report;
}

DlpError Session::identify_user()
{
    DlpSysInfo sys;
    DlpError err = _dlp.read_sys_info(sys);
    if (err != DlpError::None) {
        return err;
    }
    err = _dlp.read_user_info(_user);
    if (err != DlpError::None) {
        return err;
    }

    if (_user.userId == 0 || _user.userName.empty()) {
        _userIsNew = true;
        _user.userId = _options.newUserId;
        _user.userName = _options.defaultUserName;
    }
    FN_LOGI(TAG, "device ROM %08lx, DLP %u.%u, user '%s'%s",
            static_cast<unsigned long>(sys.romVersion), sys.dlpMajor, sys.dlpMinor,
            _user.userName.c_str(), _userIsNew ? " (new)" : "");
    return DlpError::None;
}

DlpError Session::install_pending()
{
    for (const std::string& fileName : _storage.pending_installs()) {
        if (!has_install_extension(fileName)) {
            continue;
        }
        const DlpError err = install_file(fileName);
        if (is_fatal(err)) {
            return err;
        }
        if (err == DlpError::None) {
            ++_report.installed;
            _storage.mark_installed(fileName);
            log("Installed " + fileName);
        } else {
            ++_report.installFailures;
            log("Install failed: " + fileName + " (" + dlp_error_name(err) + ")");
        }
    }
    return DlpError::None;
}

DlpError Session::install_file(const std::string& fileName)
{
    Bytes file;
    PalmDatabase db;
    if (!_storage.read_install(fileName, file, _options.maxDatabaseBytes) || !db.parse(file)) {
        FN_LOGW(TAG, "cannot read %s", fileName.c_str());
        return DlpError::Param;
    }
    file = Bytes(); // the parsed copy is all that is needed now
    FN_LOGI(TAG, "installing %s as '%s' (%s/%s)", fileName.c_str(), db.info.name.c_str(),
            db.info.type, db.info.creator);
    return write_database(db);
}

DlpError Session::write_database(const PalmDatabase& db)
{
    // Replace any existing copy, as Palm Desktop's install conduit does.
    DlpError err = _dlp.delete_db(db.info.name);
    if (err != DlpError::None && err != DlpError::NotFound) {
        return err;
    }

    std::uint8_t handle = 0;
    err = _dlp.create_db(db.info, handle);
    if (err != DlpError::None) {
        return err;
    }

    err = write_contents(handle, db);
    const DlpError closeErr = _dlp.close_db(handle);
    if (err == DlpError::None) {
        err = closeErr;
    }

    if (err == DlpError::None &&
        ((db.info.flags & kDbResetAfterInstall) || std::memcmp(db.info.creator, "ptch", 4) == 0)) {
        _resetAfterSync = true;
    }
    return err;
}

DlpError Session::write_contents(std::uint8_t handle, const PalmDatabase& db)
{
    DlpError err = DlpError::None;
    if (!db.appInfo.empty()) {
        err = _dlp.write_app_block(handle, db.appInfo);
    }
    if (err == DlpError::None && !db.sortInfo.empty()) {
        err = _dlp.write_sort_block(handle, db.sortInfo);
    }
    for (std::size_t i = 0; err == DlpError::None && i < db.resources.size(); ++i) {
        err = _dlp.write_resource(handle, db.resources[i]);
        if (std::memcmp(db.resources[i].type, "boot", 4) == 0) {
            _resetAfterSync = true;
        }
    }
    for (std::size_t i = 0; err == DlpError::None && i < db.records.size(); ++i) {
        err = _dlp.write_record(handle, db.records[i]);
    }
    return err;
}

DlpError Session::backup_databases()
{
    if (_options.backup == BackupPolicy::None) {
        return DlpError::None;
    }

    std::vector<DlpDbInfo> databases;
    std::uint16_t start = 0;
    for (;;) {
        std::uint16_t next = 0;
        const DlpError err = _dlp.read_db_list(kListRam | kListMultiple, start, databases, next);
        if (err == DlpError::NotFound) {
            break;
        }
        if (err != DlpError::None) {
            return err;
        }
        start = next;
    }

    for (const DlpDbInfo& info : databases) {
        if (_options.backup == BackupPolicy::Flagged && !(info.flags & kDbBackup)) {
            continue;
        }
        if (std::find(std::begin(kSkipBackup), std::end(kSkipBackup), info.name) != std::end(kSkipBackup)) {
            continue;
        }
        const DlpError err = backup_database(info);
        if (is_fatal(err)) {
            return err;
        }
        if (err == DlpError::None) {
            ++_report.backedUp;
        } else {
            ++_report.backupFailures;
            FN_LOGW(TAG, "backup of '%s' failed: %s", info.name.c_str(), dlp_error_name(err));
        }
    }
    if (_report.backedUp > 0) {
        log("Backed up " + std::to_string(_report.backedUp) + " databases");
    }
    return DlpError::None;
}

DlpError Session::backup_database(const DlpDbInfo& info)
{
    std::uint8_t handle = 0;
    DlpError err = _dlp.open_db(info.name, kOpenRead | kOpenSecret, handle);
    if (err != DlpError::None) {
        return err;
    }

    PalmDatabase db;
    db.info = info;
    db.created = palm_seconds_from_date(info.created);
    db.modified = palm_seconds_from_date(info.modified);
    db.backedUp = palm_seconds_from_date(info.backedUp);
    err = read_contents(handle, db);
    const DlpError closeErr = _dlp.close_db(handle);
    if (err == DlpError::None) {
        err = closeErr;
    }
    if (err != DlpError::None) {
        return err;
    }

    if (!_storage.write_backup(safe_file_name(_user.userName), backup_file_name(info), db.serialize())) {
        return DlpError::System;
    }
    return DlpError::None;
}

DlpError Session::read_contents(std::uint8_t handle, PalmDatabase& db)
{
    // A missing app or sort block is normal.
    DlpError err = _dlp.read_app_block(handle, db.appInfo);
    if (err != DlpError::None && err != DlpError::NotFound) {
        return err;
    }
    err = _dlp.read_sort_block(handle, db.sortInfo);
    if (err != DlpError::None && err != DlpError::NotFound) {
        return err;
    }

    std::uint16_t count = 0;
    err = _dlp.read_open_db_record_count(handle, count);
    if (err != DlpError::None) {
        return err;
    }

    std::size_t total = db.appInfo.size() + db.sortInfo.size();
    for (std::uint16_t i = 0; i < count; ++i) {
        if (db.is_resource_db()) {
            DlpResource resource;
            err = _dlp.read_resource_by_index(handle, i, resource);
            total += resource.data.size();
            db.resources.push_back(std::move(resource));
        } else {
            DlpRecord record;
            err = _dlp.read_record_by_index(handle, i, record);
            total += record.data.size();
            db.records.push_back(std::move(record));
        }
        if (err != DlpError::None) {
            return err;
        }
        if (total > _options.maxDatabaseBytes) {
            FN_LOGW(TAG, "'%s' is larger than %zu bytes; not backed up", db.info.name.c_str(),
                    _options.maxDatabaseBytes);
            return DlpError::LimitExceeded;
        }
    }
    return DlpError::None;
}

DlpError Session::finish()
{
    std::uint8_t modFlags = kModLastSyncPc;
    if (_userIsNew) {
        modFlags |= kModUserId | kModUserName;
    }
    _user.lastSyncPc = _options.pcId;
    if (_options.now.year != 0) {
        _user.lastSync = _options.now;
        modFlags |= kModLastSyncDate;
    }
    const DlpError err = _dlp.write_user_info(_user, modFlags);
    if (err != DlpError::None) {
        return err;
    }

    log("FujiNet HotSync complete");
    if (_resetAfterSync) {
        _dlp.reset_system();
    }
    return _dlp.end_of_sync(0);
}

void Session::log(const std::string& line)
{
    FN_LOGI(TAG, "%s", line.c_str());
    _dlp.add_sync_log_entry(line + "\n");
}

} // namespace fujinet::hotsync

#endif // FN_WITH_HOTSYNC
