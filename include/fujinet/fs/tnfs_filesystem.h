
#pragma once

#include "fujinet/fs/filesystem.h"
#include "fujinet/tnfs/tnfs_protocol.h"

namespace fujinet::fs {

std::unique_ptr<IFileSystem> make_tnfs_filesystem();
std::unique_ptr<IFileSystem> make_tnfs_filesystem(
    const std::string& host,
    std::uint16_t port = 16384,
    const std::string& mountPath = "/",
    const std::string& user = "",
    const std::string& password = "");
std::unique_ptr<IFileSystem> make_tnfs_filesystem(std::shared_ptr<tnfs::ITnfsClient> client);
std::unique_ptr<IFileSystem> make_tnfs_filesystem(std::unique_ptr<tnfs::ITnfsClient> client);

} // namespace fujinet::fs
