
#pragma once

#include "fujinet/fs/filesystem.h"
#include "fujinet/tnfs/tnfs_protocol.h"

#include <functional>

namespace fujinet::fs {

struct TnfsEndpoint {
    std::string host;
    std::uint16_t port{tnfs::DEFAULT_PORT};
    std::string mountPath{"/"};
    std::string user;
    std::string password;
};

using TnfsClientFactory = std::function<std::unique_ptr<tnfs::ITnfsClient>(const TnfsEndpoint&)>;

std::unique_ptr<IFileSystem> make_tnfs_filesystem();
std::unique_ptr<IFileSystem> make_tnfs_filesystem(TnfsClientFactory clientFactory);
std::unique_ptr<IFileSystem> make_tnfs_filesystem(std::shared_ptr<tnfs::ITnfsClient> client);
std::unique_ptr<IFileSystem> make_tnfs_filesystem(std::unique_ptr<tnfs::ITnfsClient> client);

} // namespace fujinet::fs
