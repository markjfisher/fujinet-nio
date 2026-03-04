
#include "fujinet/platform/posix/fs_factory.h"
#include "fujinet/fs/fs_stdio.h"
#include "fujinet/fs/tnfs_filesystem.h"
#include "fujinet/core/logging.h"
#include "fujinet/platform/posix/udp_channel.h"
#include "fujinet/platform/posix/tcp_channel.h"
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

std::unique_ptr<fujinet::fs::IFileSystem> create_tnfs_filesystem(bool useTcp) {
    FN_LOGI(TAG, "Registering TNFS filesystem provider (dynamic endpoints)");

    fujinet::fs::TnfsClientFactory factory = [useTcp](const fujinet::fs::TnfsEndpoint& endpoint)
        -> std::unique_ptr<fujinet::tnfs::ITnfsClient>
    {
        std::unique_ptr<fujinet::io::Channel> channel;
        if (useTcp) {
            channel = fujinet::platform::create_tcp_channel(endpoint.host, endpoint.port);
            return fujinet::tnfs::make_tcp_tnfs_client(std::move(channel));
        }
        channel = fujinet::platform::create_udp_channel(endpoint.host, endpoint.port);
        return fujinet::tnfs::make_udp_tnfs_client(std::move(channel));
    };

    return fujinet::fs::make_tnfs_filesystem(std::move(factory));
}
} // namespace fujinet::platform::posix
