#include "fujinet/platform/posix/visor_usb/ingress.h"
#include "fujinet/io/core/packet_io.h"
#include "fujinet/io/transport/native_framer.h"
#include "fujinet/io/transport/fujibus_transport.h"
#include "fujinet/io/protocol/wire_device_ids.h"
#include "fujinet/io/devices/fuji_device.h"
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
    io::FujiDevice* fuji;
    Channel channel{host};
    std::unique_ptr<io::NativeFramer> framer;
    std::unique_ptr<io::FujiBusTransport> transport;
    std::unique_ptr<std::uint64_t> owner, retiredOwner;
    std::uint64_t generation{0}, pendingGeneration{0};
    io::RequestID nextId{0}, pendingId{0};
    Impl(io::IRequestHandler& r, Options o, io::FujiDevice* f, Host::Now now)
        : router(r), host(std::move(o), std::move(now)), fuji(f) {}
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
Ingress::Ingress(io::IRequestHandler& r, Options o, io::FujiDevice* f, Host::Now now)
    : _impl(std::make_unique<Impl>(r, std::move(o), f, std::move(now))) {}
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
    out.push_back({"visor.status", "show effective selection sources, USB session and last error", "visor.status", true});
    out.push_back({"visor.get", "show configured USB selection", "visor.get", true});
    out.push_back({"visor.set", "set selection; restart to apply", "visor.set <port|bus|path> <value|unset>", false});
    out.push_back({"visor.save", "save configured selection to fujinet.yaml", "visor.save", false});
}
diag::DiagResult Ingress::execute(const diag::DiagArgsView& args)
{
    if (args.argv.empty()) return diag::DiagResult::invalid_args("missing command");
    const auto command = args.argv[0];
    if (command == "visor.status" && args.argv.size() == 1)
        return diag::DiagResult::ok(_impl->host.status() + "\r\n");
    if (command != "visor.get" && command != "visor.set" && command != "visor.save")
        return diag::DiagResult::invalid_args("usage: visor.status|get|set|save");
    if (!_impl->fuji) return diag::DiagResult::not_ready("FujiDevice not available");
    auto& stored = _impl->fuji->config_mut().visor;
    try {
        if (command == "visor.get" && args.argv.size() == 1) {
            return diag::DiagResult::ok("port=" + std::to_string(stored.port) +
                " bus=" + (stored.bus ? std::to_string(stored.bus) : "unset") +
                " path=" + (stored.path.empty() ? "unset" : stored.path) +
                "\r\nConfigured values; restart to apply. Environment overrides remain effective.\r\n");
        }
        if (command == "visor.set" && args.argv.size() == 3) {
            auto next = stored;
            const auto field = args.argv[1], value = args.argv[2];
            if (field == "port") next.port = config::parse_visor_number(value, 2);
            else if (field == "bus") next.bus = value == "unset" ? 0 : config::parse_visor_number(value, 255);
            else if (field == "path") {
                if (value.empty()) throw config::InvalidVisorConfig("use unset to clear path");
                next.path = value == "unset" ? "" : std::string(value);
            } else return diag::DiagResult::invalid_args("unknown field (port, bus or path)");
            next.validate(); stored = std::move(next);
            return diag::DiagResult::ok("Configured; visor.save to persist, restart to apply. Environment overrides take precedence.\r\n");
        }
        if (command == "visor.save" && args.argv.size() == 1) {
            auto* store = _impl->fuji->config_store();
            if (!store) return diag::DiagResult::not_ready("config store not available");
            stored.validate();
            store->save(_impl->fuji->config());
            const auto saved = store->load().visor;
            if (saved.port != stored.port || saved.bus != stored.bus || saved.path != stored.path)
                return diag::DiagResult::error("Visor settings read-back mismatch");
            return diag::DiagResult::ok("Saved visor settings; restart to apply.\r\n");
        }
    } catch (const config::InvalidVisorConfig& error) {
        return diag::DiagResult::invalid_args(error.what());
    } catch (const std::exception& error) {
        return diag::DiagResult::error(error.what());
    }
    return diag::DiagResult::invalid_args("usage: visor.get | visor.set <port|bus|path> <value|unset> | visor.save");
}
} // namespace fujinet::platform::posix::visor_usb
