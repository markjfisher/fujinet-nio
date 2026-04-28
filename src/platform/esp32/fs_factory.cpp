#include "fujinet/fs/fs_stdio.h"
#include "fujinet/platform/esp32/fs_factory.h"
#include "fujinet/platform/esp32/fs_init.h"
#include "fujinet/fs/http_filesystem.h"
#include "fujinet/fs/tnfs_filesystem.h"
#include "fujinet/core/logging.h"
#include "fujinet/io/devices/network_protocol.h"
#include "fujinet/platform/network_registry.h"
#include "fujinet/platform/esp32/udp_channel.h"
#include "fujinet/platform/esp32/tcp_channel.h"
#include "fujinet/tnfs/tnfs_protocol.h"

#include <memory>

namespace fujinet::platform::esp32 {

using fujinet::fs::FileSystemKind;

static constexpr const char* TAG = "fs";

std::unique_ptr<fujinet::fs::IFileSystem> create_flash_filesystem()
{
    // assume init_littlefs() has already mounted /fujifs
    return fujinet::fs::create_stdio_filesystem(
        "/fujifs",
        "flash",
        FileSystemKind::LocalFlash
    );
}

std::unique_ptr<fujinet::fs::IFileSystem> create_sdcard_filesystem()
{
    // once SD is mounted at /sdcard
    return fujinet::fs::create_stdio_filesystem(
        "/sdcard",
        "sd0",
        FileSystemKind::LocalSD
    );
}

std::unique_ptr<fujinet::fs::IFileSystem> create_tnfs_filesystem(bool useTcp) {
    fujinet::fs::TnfsClientFactory factory = [useTcp](const fujinet::fs::TnfsEndpoint& endpoint)
        -> std::unique_ptr<fujinet::tnfs::ITnfsClient>
    {
        const bool useTcpForEndpoint = endpoint.useTcp || useTcp;
        if (useTcpForEndpoint) {
            auto channel = fujinet::platform::create_tcp_channel(endpoint.host, endpoint.port);
            return fujinet::tnfs::make_tcp_tnfs_client(std::move(channel));
        }

        auto channel = fujinet::platform::create_udp_channel(endpoint.host, endpoint.port);
        return fujinet::tnfs::make_udp_tnfs_client(std::move(channel));
    };

    return fujinet::fs::make_tnfs_filesystem(std::move(factory));
}

std::unique_ptr<fujinet::fs::IFileSystem> create_http_filesystem(const fujinet::config::TlsConfig& tlsConfig)
{
    auto registry = std::make_shared<fujinet::io::ProtocolRegistry>(fujinet::platform::make_default_network_registry(tlsConfig));
    fujinet::fs::HttpProtocolFactory factory = [registry](std::string_view schemeLower)
        -> std::unique_ptr<fujinet::io::INetworkProtocol>
    {
        return registry->create(schemeLower);
    };

    return fujinet::fs::make_http_filesystem(std::move(factory));
}

} // namespace fujinet::platform::esp32
