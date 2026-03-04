
#include "fujinet/platform/posix/fs_factory.h"
#include "fujinet/fs/fs_stdio.h"
#include "fujinet/fs/tnfs_filesystem.h"
#include "fujinet/core/logging.h"
#include "fujinet/platform/posix/udp_channel.h"
#include "fujinet/tnfs/tnfs_protocol.h"

#include <filesystem>
#include <system_error>

namespace fujinet::platform::posix {

static const char* TAG = "fs";

std::unique_ptr<fujinet::fs::IFileSystem>
create_host_filesystem(const std::string& rootDir)
{
    namespace fs = std::filesystem;
    using fujinet::fs::FileSystemKind;

    fs::path root(rootDir);

    // Policy:
    // - If the root path is relative, we consider it "owned" by the app
    //   and create it if missing.
    // - Absolute paths are assumed to be user-/system-managed.
    if (root.is_relative() && !fs::exists(root)) {
        std::error_code ec;
        fs::create_directories(root, ec);
        if (ec) {
            FN_LOGE(TAG,
                    "Failed to create host filesystem root '%s': %s",
                    rootDir.c_str(),
                    ec.message().c_str());
            // We still return the FS; open() will fail later with a clearer error
        }
    }

    return fujinet::fs::create_stdio_filesystem(
        rootDir,
        "host",
        FileSystemKind::HostPosix
    );
}

std::unique_ptr<fujinet::fs::IFileSystem> create_tnfs_filesystem(const std::string& host, uint16_t port, const std::string& mountPath, const std::string& user, const std::string& password) {
    auto channel = fujinet::platform::create_udp_channel(host, port);
    auto client = fujinet::tnfs::make_udp_tnfs_client(std::move(channel));

    if (!client->mount(mountPath, user, password)) {
        return nullptr;
    }

    return fujinet::fs::make_tnfs_filesystem(std::move(client));
}



} // namespace fujinet::platform::posix
