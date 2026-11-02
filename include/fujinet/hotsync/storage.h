#pragma once

#include "fujinet/hotsync/bytes.h"

#include <cstddef>
#include <string>
#include <vector>

namespace fujinet::hotsync {

// Where a HotSync reads apps to install and writes backups.
//
// Layout under the HotSync root:
//   install/          .prc/.pdb/.pqa files queued for the next sync
//   installing/       files a sync has taken from install/; one it could not
//                     install stays here and is tried again at the next sync
//   installed/        files moved here once installed
//   backup/<user>/    databases read back from the device
//
// Files can be queued while a sync runs, under a name the sync is installing.
// So the sync claims each file before reading it: from then on that file is
// the sync's, and one queued under the same name is a new file, left for the
// next sync. What ends up in installed/ is always what the device received.
// It claims only a file that already reads as a whole database: a client may
// still be writing one, and a rename must not take it from under its writer.
class Storage {
public:
    virtual ~Storage() = default;

    // Files waiting to be installed: queued ones, and claimed ones an earlier
    // sync could not install.
    virtual std::vector<std::string> pending_installs() = 0;
    // Reads the file claim_install() would take, without taking it. False
    // when it is missing, unreadable or larger than maxBytes.
    virtual bool peek_install(const std::string& fileName, Bytes& out, std::size_t maxBytes) = 0;
    // Takes a pending file for this sync. A newly queued file replaces one an
    // earlier sync claimed under the same name. False when there is none.
    virtual bool claim_install(const std::string& fileName) = 0;
    // The claimed file. False when it is missing, unreadable or larger than
    // maxBytes.
    virtual bool read_install(const std::string& fileName, Bytes& out, std::size_t maxBytes) = 0;
    // The claimed file was installed.
    virtual bool mark_installed(const std::string& fileName) = 0;
    // Replaces the backup of that name only once the new one is complete.
    virtual bool write_backup(const std::string& user, const std::string& fileName, const Bytes& data) = 0;
};

} // namespace fujinet::hotsync
