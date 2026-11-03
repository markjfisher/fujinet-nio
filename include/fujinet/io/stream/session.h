#pragma once
#include "fujinet/io/core/request_handler.h"
#include "fujinet/io/stream/replay_slot.h"
#include <vector>
namespace fujinet::io::stream {
// Single experimental stream per ingress; all network/file work goes through routing.
class Session {
public:
    static constexpr unsigned max_data = 120, header = 11;
    explicit Session(IRequestHandler& router, const void* owner);
    ~Session();
    std::vector<std::uint8_t> query() const;
    std::vector<std::uint8_t> execute(const std::vector<std::uint8_t>& request);
private:
    IOResponse call(std::uint8_t device, std::uint8_t command, std::vector<std::uint8_t> payload);
    std::vector<std::uint8_t> perform(const std::vector<std::uint8_t>& request);
    IRequestHandler& _router;
    const void* _owner;
    std::uint32_t _token, _next{0}, _read{0}, _write{0};
    std::uint16_t _handle{0};
    bool _open{false}, _poisoned{false}, _exhausted{false};
    ReplaySlot<header + max_data, header + max_data, std::uint32_t> _replay;
};
}
