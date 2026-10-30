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
//   installed/        files moved here once installed
//   backup/<user>/    databases read back from the device
class Storage {
public:
    virtual ~Storage() = default;

    virtual std::vector<std::string> pending_installs() = 0;
    // False when the file is missing, unreadable or larger than maxBytes.
    virtual bool read_install(const std::string& fileName, Bytes& out, std::size_t maxBytes) = 0;
    virtual bool mark_installed(const std::string& fileName) = 0;
    virtual bool write_backup(const std::string& user, const std::string& fileName, const Bytes& data) = 0;
};

} // namespace fujinet::hotsync
