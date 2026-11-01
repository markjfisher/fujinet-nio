#include "fujinet/platform/posix/atari_netsio_fujibus_channel.h"

#include "fujinet/io/atari_sio_disk_frontend.h"
#include "fujinet/io/atari_sio_ingress_router.h"
#include "fujinet/io/atari_sio_nio_frontend.h"
#include "fujinet/io/transport/legacy/netsio_protocol.h"

#include <chrono>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

namespace fujinet::platform::posix {
namespace {

std::uint64_t monotonic_microseconds()
{
    using namespace std::chrono;
    return static_cast<std::uint64_t>(duration_cast<microseconds>(
        steady_clock::now().time_since_epoch()).count());
}

constexpr std::uint8_t sio_ack = 'A';
constexpr std::uint8_t sio_nak = 'N';

bool is_atari_sio_device(std::uint8_t device)
{
    return device == 0x7f || (device >= 0x31 && device <= 0x38);
}

} // namespace

class AtariNetSioIngress::Impl {
public:
    Impl(std::unique_ptr<fujinet::io::Channel> udp,
         fujinet::disk::DiskService& disks,
         fujinet::io::IRequestHandler& routing, std::uint64_t (*clock)())
        : udp(std::move(udp))
        , disk(disks)
        , nio(routing)
        , router(disk, nio, [this](const std::vector<std::uint8_t>& bytes) { replies.push_back(bytes); })
    {
        nio.set_clock(clock ? clock : monotonic_microseconds);
        netsio.sendDeviceConnect();
        write_netsio();
        netsio.sendSpeedChange(19200);
        write_netsio();
        send_alive();
        netsio.sendProceedOn();
        write_netsio();
    }

    ~Impl()
    {
        netsio.sendDeviceDisconnect();
        write_netsio();
    }

    bool supports_work_wait() const { return udp && udp->supports_readable_wait(); }
    bool wait_for_work(std::chrono::milliseconds timeout) { return udp && udp->wait_for_readable(timeout); }

    void poll()
    {
        if (!udp) return;
        send_alive_if_due();
        std::uint8_t bytes[512];
        while (udp->available()) {
            const std::size_t count = udp->read(bytes, sizeof(bytes));
            if (!count || !netsio.parseMessage(bytes, count)) continue;
            handle_message(netsio.getMessageType(), netsio.getPayload());
        }
    }

private:
    void write_netsio()
    {
        if (!udp) return;
        const auto& message = netsio.getEncodedMessage();
        if (!message.empty()) udp->write(message.data(), message.size());
    }

    void send_alive()
    {
        netsio.sendAliveRequest();
        write_netsio();
        last_alive = std::chrono::steady_clock::now();
    }

    void send_alive_if_due()
    {
        if (std::chrono::steady_clock::now() - last_alive >= std::chrono::seconds(10)) send_alive();
    }

    void send_initial_host_speed_if_needed(std::uint8_t type)
    {
        using namespace fujinet::io::transport::legacy;
        if (initial_host_speed_sent || type >= netsio::DEVICE_DISCONNECT) return;
        netsio.sendSpeedChange(19200);
        write_netsio();
        initial_host_speed_sent = true;
    }

    void request_credit_before_sync_response()
    {
        using namespace fujinet::io::transport::legacy;
        netsio.sendCreditStatus(0);
        write_netsio();

        // Keep the established NetSIO READY/credit ordering: the hub grants
        // credit before the synchronous response starts the SIO phase.
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(5);
        std::uint8_t bytes[512];
        while (udp && std::chrono::steady_clock::now() < deadline && udp->available()) {
            const std::size_t count = udp->read(bytes, sizeof(bytes));
            if (!count || !netsio.parseMessage(bytes, count)) continue;
            const auto type = netsio.getMessageType();
            if (type == netsio::CREDIT_UPDATE) break;
            if (type == netsio::ALIVE_REQUEST) {
                netsio.sendAliveResponse();
                write_netsio();
            }
        }
    }

    void send_empty_sync(std::uint8_t sync)
    {
        using namespace fujinet::io::transport::legacy;
        netsio.sendSyncResponse(sync, netsio::EMPTY_SYNC, 0, 0);
        write_netsio();
    }

    void send_ack_sync(std::uint8_t sync, std::uint8_t ack_byte, std::uint16_t write_size)
    {
        using namespace fujinet::io::transport::legacy;
        request_credit_before_sync_response();
        netsio.sendSyncResponse(sync, netsio::ACK_SYNC, ack_byte, write_size);
        write_netsio();
    }

    void send_reply(std::uint8_t sync, const std::vector<std::uint8_t>& reply,
                    std::uint16_t write_size = 0)
    {
        if (reply.empty()) {
            send_empty_sync(sync);
            return;
        }
        send_ack_sync(sync, reply.front(), write_size);
        if (reply.size() < 2) return;

        // The frontend owns literal SIO data.  NetSIO is only the virtual
        // carrier: it neither pads nor reframes the native packet.
        netsio.sendDataByte(reply[1]);
        write_netsio();
        if (reply.size() > 3) {
            netsio.sendDataBlock(reply.data() + 2, reply.size() - 3);
            write_netsio();
        }
        if (reply.size() > 2) {
            netsio.sendDataByte(reply.back());
            write_netsio();
        }
    }

    std::vector<std::uint8_t> take_reply()
    {
        if (replies.empty()) return {};
        auto reply = std::move(replies.front());
        replies.erase(replies.begin());
        return reply;
    }

    void begin_command()
    {
        router.reset_command_collection();
        command_frame.clear();
        pending_write.clear();
        pending_write_bytes = 0;
        pending_write_bad = false;
        command_active = true;
    }

    void finish_command(std::uint8_t sync)
    {
        command_active = false;
        if (command_frame.size() != fujinet::io::AtariSioNioFrontend::command_bytes) {
            router.reset_command_collection();
            send_reply(sync, {sio_nak});
            return;
        }
        if (!is_atari_sio_device(command_frame.front())) {
            send_empty_sync(sync);
            return;
        }

        for (const auto byte : command_frame) router.consume(byte);
        const auto reply = take_reply();
        const auto expected = router.pending_write_bytes();
        if (expected != 0 && reply == std::vector<std::uint8_t>{sio_ack}) {
            pending_write_bytes = expected;
            send_reply(sync, reply, static_cast<std::uint16_t>(expected + 1));
            return;
        }
        send_reply(sync, reply.empty() ? std::vector<std::uint8_t>{sio_nak} : reply);
    }

    void receive_data(const std::vector<std::uint8_t>& data)
    {
        if (command_active) {
            command_frame.insert(command_frame.end(), data.begin(), data.end());
            return;
        }
        if (!pending_write_bytes) return;
        if (pending_write.size() + data.size() > pending_write_bytes) {
            pending_write_bad = true;
            return;
        }
        pending_write.insert(pending_write.end(), data.begin(), data.end());
    }

    void receive_data_checksum(std::uint8_t checksum, std::uint8_t sync)
    {
        if (!pending_write_bytes) {
            send_empty_sync(sync);
            return;
        }
        if (pending_write_bad || pending_write.size() != pending_write_bytes) {
            router.reset_command_collection();
            pending_write.clear();
            pending_write_bytes = 0;
            pending_write_bad = false;
            send_reply(sync, {sio_nak});
            return;
        }

        for (const auto byte : pending_write) router.consume(byte);
        router.consume(checksum);
        pending_write.clear();
        pending_write_bytes = 0;
        pending_write_bad = false;
        const auto reply = take_reply();
        send_reply(sync, reply.empty() ? std::vector<std::uint8_t>{sio_nak} : reply);
    }

    void reset_device()
    {
        router.reset_command_collection();
        nio.reset_host();
        command_frame.clear();
        pending_write.clear();
        pending_write_bytes = 0;
        pending_write_bad = false;
        replies.clear();
    }

    void handle_message(std::uint8_t type, const std::vector<std::uint8_t>& payload)
    {
        using namespace fujinet::io::transport::legacy;
        send_initial_host_speed_if_needed(type);
        switch (type) {
        case netsio::COMMAND_ON:
            begin_command();
            break;
        case netsio::COMMAND_OFF:
            command_active = false;
            break;
        case netsio::COMMAND_OFF_SYNC:
            if (!payload.empty()) finish_command(payload[0]);
            break;
        case netsio::DATA_BLOCK:
            receive_data(payload);
            break;
        case netsio::DATA_BYTE:
            if (!payload.empty()) receive_data({payload[0]});
            break;
        case netsio::DATA_BYTE_SYNC:
            if (payload.size() == 2) receive_data_checksum(payload[0], payload[1]);
            break;
        case netsio::PING_REQUEST:
            netsio.sendPingResponse();
            write_netsio();
            break;
        case netsio::ALIVE_REQUEST:
            netsio.sendAliveResponse();
            write_netsio();
            break;
        case netsio::WARM_RESET:
        case netsio::COLD_RESET:
            initial_host_speed_sent = false;
            reset_device();
            break;
        case netsio::MOTOR_ON:
        case netsio::MOTOR_OFF:
        case netsio::PROCEED_ON:
        case netsio::PROCEED_OFF:
        case netsio::INTERRUPT_ON:
        case netsio::INTERRUPT_OFF:
        case netsio::SPEED_CHANGE:
        case netsio::ALIVE_RESPONSE:
        case netsio::PING_RESPONSE:
        case netsio::CREDIT_STATUS:
        case netsio::CREDIT_UPDATE:
        case netsio::BUS_IDLE:
            break;
        default:
            break;
        }
    }

    std::unique_ptr<fujinet::io::Channel> udp;
    fujinet::io::AtariSioDiskFrontend disk;
    fujinet::io::AtariSioNioFrontend nio;
    fujinet::io::AtariSioIngressRouter router;
    fujinet::io::transport::legacy::NetSIOProtocol netsio;
    std::vector<std::vector<std::uint8_t>> replies;
    std::vector<std::uint8_t> command_frame;
    std::vector<std::uint8_t> pending_write;
    std::size_t pending_write_bytes{0};
    bool pending_write_bad{false};
    bool command_active{false};
    bool initial_host_speed_sent{false};
    std::chrono::steady_clock::time_point last_alive{};
};

AtariNetSioIngress::AtariNetSioIngress(std::unique_ptr<fujinet::io::Channel> udp,
                                       fujinet::disk::DiskService& disks,
                                       fujinet::io::IRequestHandler& routing,
                                       std::uint64_t (*clock)())
    : _impl(std::make_unique<Impl>(std::move(udp), disks, routing, clock))
{}

AtariNetSioIngress::~AtariNetSioIngress() = default;
void AtariNetSioIngress::poll() { _impl->poll(); }
bool AtariNetSioIngress::supports_work_wait() const { return _impl->supports_work_wait(); }
bool AtariNetSioIngress::wait_for_work(std::chrono::milliseconds timeout) { return _impl->wait_for_work(timeout); }

std::unique_ptr<AtariNetSioIngress>
create_atari_netsio_ingress(std::unique_ptr<fujinet::io::Channel> udp,
                            fujinet::disk::DiskService& disks,
                            fujinet::io::IRequestHandler& routing)
{
    return std::make_unique<AtariNetSioIngress>(std::move(udp), disks, routing);
}

} // namespace fujinet::platform::posix
