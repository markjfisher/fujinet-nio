#pragma once

#include "fujinet/platform/posix/visor_usb/host.h"
#include "fujinet/diag/diagnostic_provider.h"
#include "fujinet/io/core/request_handler.h"
#include "fujinet/io/transport/transport.h"

namespace fujinet::io { class FujiDevice; }

namespace fujinet::platform::posix::visor_usb {
// POSIX-only ownership adapter. Composes existing framers/codecs; no service logic.
class Ingress final : public io::ITransport, public diag::IDiagnosticProvider {
public:
    Ingress(io::IRequestHandler& router, Options options = {}, io::FujiDevice* fuji = nullptr);
    ~Ingress();
    void poll() override;
    bool receive(io::IORequest& request) override;
    void send(const io::IOResponse& response) override;
    bool supports_work_wait() const override { return true; }
    bool wait_for_work(std::chrono::milliseconds timeout) override;
    std::string_view provider_id() const noexcept override { return "visor"; }
    void list_commands(std::vector<diag::DiagCommandSpec>& out) const override;
    diag::DiagResult execute(const diag::DiagArgsView& args) override;
private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
};
} // namespace fujinet::platform::posix::visor_usb
