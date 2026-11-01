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
    // Optional monotonic microsecond clock for deterministic tests; production uses steady_clock.
    AtariNetSioIngress(std::unique_ptr<fujinet::io::Channel> udp,
                       fujinet::disk::DiskService& disks,
                       fujinet::io::IRequestHandler& routing,
                       std::uint64_t (*clock)() = nullptr);
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
                            fujinet::io::IRequestHandler& routing);

} // namespace fujinet::platform::posix
