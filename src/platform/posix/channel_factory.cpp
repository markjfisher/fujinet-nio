#include "fujinet/platform/channel_factory.h"

#include "fujinet/build/profile.h"
#include "fujinet/config/fuji_config.h"
#include "fujinet/io/core/channel.h"
#include "fujinet/io/core/packet_link.h"
#include "fujinet/platform/posix/pty_channel.h"
#include "fujinet/platform/posix/serial_channel.h"
#include "fujinet/platform/posix/tcp_server_channel.h"
#include "fujinet/platform/posix/udp_channel.h"

#include <iostream>
#include <memory>
#include <string>

#if !defined(_WIN32)

namespace fujinet::platform {

// A profile that asks for a packet link carries its packets over this stream.
// `networked`: TCP can stall in transit, so its default record timeout is longer.
static std::unique_ptr<fujinet::io::Channel>
with_packet_link(const build::BuildProfile& profile, const config::FujiConfig& config,
                 std::unique_ptr<fujinet::io::Channel> stream, bool networked)
{
    const auto settings = fujinet::io::packet_link_settings(
        config.channel.packetLink.capacity, config.channel.packetLink.recordTimeoutMs, networked);
    if (profile.packetLink && stream) {
        std::cout << "[ChannelFactory] Carrying FujiBus packets over a packet link (capacity "
                  << settings.capacity << ", record timeout " << settings.recordTimeoutMs << " ms).\n";
    }
    return fujinet::io::with_packet_link(profile.packetLink, std::move(stream), settings);
}

std::unique_ptr<fujinet::io::Channel>
create_channel_for_profile(const build::BuildProfile& profile, const config::FujiConfig& config,
                           disk::DiskService* disks)
{
    using build::ChannelKind;

    switch (profile.primaryChannel) {
    case ChannelKind::Pty:
        std::cout << "[ChannelFactory] Using PTY channel (Pty).\n";
        return posix::create_pty_channel(config);

    case ChannelKind::VisorUsbHost:
        // Dedicated profile registers an ownership-aware packet ingress in main_posix.
        return nullptr;

    case ChannelKind::UsbCdcDevice:
        std::cout << "[ChannelFactory] UsbCdcDevice not supported on POSIX.\n";
        return nullptr;

    case ChannelKind::TcpSocket:
        std::cout << "[ChannelFactory] Using TCP server channel (TcpSocket) on "
                  << config.channel.tcpHost << ":" << config.channel.tcpPort << std::endl;
        return with_packet_link(profile, config,
            posix::create_tcp_server_channel(config.channel.tcpHost, config.channel.tcpPort),
            true);

    case ChannelKind::UdpSocket: {
        const std::string host = config.netsio.host;
        const std::uint16_t port = config.netsio.port;

        std::cout << "[ChannelFactory] Using UDP channel (NetSIO) to " << host << ":" << port << std::endl;

        (void)disks;
        return create_udp_channel(host, port);
    }

    case ChannelKind::UartGpio:
        std::cout << "[ChannelFactory] UartGpio not supported on POSIX (use Pty or UdpSocket for SIO testing).\n";
        return nullptr;

    case ChannelKind::SioGpio:
        std::cout << "[ChannelFactory] SioGpio not supported on POSIX.\n";
        return nullptr;

    case ChannelKind::SerialPort:
        std::cout << "[ChannelFactory] Using RS-232 serial channel.\n";
        return with_packet_link(profile, config, posix::create_serial_channel(config), false);
    }

    std::cout << "[ChannelFactory] Unknown ChannelKind.\n";
    return nullptr;
}

} // namespace fujinet::platform

#else // _WIN32

namespace fujinet::platform {

class DummyChannel : public fujinet::io::Channel {
public:
    bool available() override { return false; }
    std::size_t read(std::uint8_t* buffer, std::size_t maxLen) override
    {
        (void)buffer;
        (void)maxLen;
        return 0;
    }
    void write(const std::uint8_t* buffer, std::size_t len) override
    {
        (void)buffer;
        (void)len;
    }
};

std::unique_ptr<fujinet::io::Channel>
create_channel_for_profile(const build::BuildProfile& /*profile*/, const config::FujiConfig& /*config*/)
{
    std::cout << "[PtyChannel] PTY not supported on this platform; using dummy Channel.\n";
    return std::make_unique<DummyChannel>();
}

} // namespace fujinet::platform

#endif // !_WIN32
