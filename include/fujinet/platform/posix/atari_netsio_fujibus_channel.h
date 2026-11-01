#pragma once

#include "fujinet/io/core/channel.h"
#include "fujinet/io/core/request_handler.h"
#include "fujinet/io/core/work_source.h"
#include "fujinet/disk/disk_service.h"

#include <chrono>
#include <cstdint>
#include <memory>

namespace fujinet::platform::posix {

// The POSIX side of the one Atari SIO ingress.  NetSIO is only its virtual
// wire carrier; raw FujiBus packets never become a Channel/SLIP stream here.
class AtariNetSioIngress final : public fujinet::io::IWorkSource {
public:
    // Optional clock and delay hooks allow deterministic tests; production uses
    // steady_clock and a wall-clock sleep for the configured ACK gap.
    AtariNetSioIngress(std::unique_ptr<fujinet::io::Channel> udp,
                       fujinet::disk::DiskService& disks,
                       fujinet::io::IRequestHandler& routing,
                       std::uint64_t (*clock)() = nullptr,
                       std::uint32_t ack_gap_us = 250,
                       void (*delay)(std::uint32_t) = nullptr);
    ~AtariNetSioIngress() override;

    void poll();
    bool supports_work_wait() const override;
    bool wait_for_work(std::chrono::milliseconds timeout) override;

private:
    class Impl;
    std::unique_ptr<Impl> _impl;
};

std::unique_ptr<AtariNetSioIngress>
create_atari_netsio_ingress(std::unique_ptr<fujinet::io::Channel> udp,
                            fujinet::disk::DiskService& disks,
                            fujinet::io::IRequestHandler& routing,
                            std::uint32_t ack_gap_us = 250);

} // namespace fujinet::platform::posix
