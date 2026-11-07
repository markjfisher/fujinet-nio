#include "fujinet/platform/posix/visor_usb/ingress.h"
#include "fujinet/io/core/packet_io.h"
#include "fujinet/io/transport/native_framer.h"
#include "fujinet/io/transport/fujibus_transport.h"
#include "fujinet/io/protocol/wire_device_ids.h"
#include <cstring>
#include <limits>

namespace fujinet::platform::posix::visor_usb {
struct Ingress::Impl {
    struct Channel final : io::Channel, io::IPacketIO {
        Host& host;
        bool staged{false};
        explicit Channel(Host& h) : IPacketIO(io::visor_usb::packet_capacity), host(h) {}
        io::IPacketIO* packet_io() override { return this; }
        bool available() override { return false; }
        std::size_t read(std::uint8_t*, std::size_t) override { return 0; }
        void write(const std::uint8_t*, std::size_t) override { host.retire("Unexpected byte I/O"); }
        io::PacketReceiveResult receive(std::uint8_t* buffer, std::size_t size) override {
            io::protocol::ByteBuffer p;
            if (!host.take(p)) return {io::PacketIOStatus::NoData};
            if (p.size() > size) { host.retire("Packet capacity mismatch"); return {io::PacketIOStatus::Oversized}; }
            staged = true;
            std::memcpy(buffer, p.data(), p.size()); return {io::PacketIOStatus::Ok, p.size()};
        }
        io::PacketIOStatus send(const std::uint8_t* buffer, std::size_t size) override {
            return host.send(io::protocol::ByteBuffer(buffer, buffer + size)) ? io::PacketIOStatus::Ok : io::PacketIOStatus::UnknownCompletion;
        }
        io::PacketIOStatus reset() override {
            host.retire("Local reset cannot establish remote quiescence");
            return io::PacketIOStatus::UnknownCompletion;
        }
    };
    io::IRequestHandler& router;
    Host host;
    Channel channel{host};
    std::unique_ptr<io::NativeFramer> framer;
    std::unique_ptr<io::FujiBusTransport> transport;
    std::unique_ptr<std::uint64_t> owner, retiredOwner;
    std::uint64_t generation{0}, pendingGeneration{0};
    io::RequestID nextId{0}, pendingId{0};
    Impl(io::IRequestHandler& r, Options o) : router(r), host(std::move(o)) {}
    void cleanup() {
        if (owner) router.resetClient(io::protocol::to_device_id(io::protocol::WireDeviceId::NetworkService), owner.get());
        transport.reset(); framer.reset();
        if (owner) retiredOwner = std::move(owner);
        pendingId = 0; channel.staged = false;
    }
    void synchronize() {
        if (!host.active()) { cleanup(); return; }
        if (generation != host.generation()) {
            // Retain the last token through allocation so adjacent sessions never
            // reuse an owner address. Older tokens have completed owner cleanup.
            auto fresh = std::make_unique<std::uint64_t>(host.generation());
            cleanup(); owner = std::move(fresh); generation = host.generation();
            framer = std::make_unique<io::NativeFramer>(channel);
            transport = std::make_unique<io::FujiBusTransport>(channel, *framer);
        }
    }
};
Ingress::Ingress(io::IRequestHandler& r, Options o) : _impl(std::make_unique<Impl>(r, std::move(o))) {}
Ingress::~Ingress() { _impl->host.retire("Ingress shutdown"); _impl->cleanup(); }
void Ingress::poll()
{
    _impl->host.poll(); _impl->synchronize();
    if (_impl->transport) _impl->transport->poll();
}
bool Ingress::receive(io::IORequest& request)
{
    // Pump again before consuming a staged NativeFramer packet, including after
    // a synchronous service call. A detached generation must not route queued work.
    poll();
    if (!_impl->transport || _impl->pendingId) return false;
    _impl->channel.staged = false;
    if (!_impl->transport->receive(request)) return false;
    if (_impl->nextId == std::numeric_limits<io::RequestID>::max()) {
        _impl->host.retire("Request identity exhausted"); _impl->synchronize(); return false;
    }
    request.id = ++_impl->nextId;
    request.clientOwner = _impl->owner.get();
    _impl->pendingId = request.id; _impl->pendingGeneration = _impl->generation;
    return true;
}
void Ingress::send(const io::IOResponse& response)
{
    _impl->host.poll(); _impl->synchronize();
    if (!_impl->transport || !_impl->pendingId || response.id != _impl->pendingId ||
        _impl->pendingGeneration != _impl->generation) return;
    _impl->transport->send(response);
    if (_impl->framer->sendStatus() != io::PacketIOStatus::Ok)
        _impl->host.retire("Response framing/send failed; no replay, close/reopen required");
    _impl->pendingId = 0;
    _impl->synchronize();
}
bool Ingress::wait_for_work(std::chrono::milliseconds timeout)
{
    const auto ready = [this] {
        // Lifecycle changes need core-thread synchronize/owner cleanup. A staged
        // packet remains actionable after NativeFramer took it out of Host.
        return (_impl->host.active() != bool(_impl->owner)) ||
            (_impl->host.active() && _impl->generation != _impl->host.generation()) ||
            (!_impl->pendingId && (_impl->channel.staged || _impl->host.needs_poll()));
    };
    if (ready()) return true;
    _impl->host.wait(timeout);
    // Callbacks only update private state; poll() performs service cleanup.
    return ready();
}
void Ingress::list_commands(std::vector<diag::DiagCommandSpec>& out) const
{
    out.push_back({"visor.status", "show USB session, endpoints and last error", "visor.status", true});
}
diag::DiagResult Ingress::execute(const diag::DiagArgsView& args)
{
    if (args.argv.size() != 1 || args.argv[0] != "visor.status") return diag::DiagResult::invalid_args("usage: visor.status");
    return diag::DiagResult::ok(_impl->host.status() + "\r\n");
}
} // namespace fujinet::platform::posix::visor_usb
