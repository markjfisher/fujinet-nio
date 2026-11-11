#include "fujinet/platform/posix/visor_usb/host.h"
#include "fujinet/io/protocol/fuji_bus_packet.h"
#include "fujinet/core/logging.h"
#include <libusb.h>
#include <algorithm>
#include <array>
#include <cstdlib>
#include <deque>
#include <stdexcept>
#include <limits>

namespace fujinet::platform::posix::visor_usb {
namespace {
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;
}
Options resolve_options(const config::VisorConfig& stored)
{
    stored.validate();
    auto selected = stored;
    Options o;
    if (const char* p = std::getenv("FN_VISOR_USB_PORT")) {
        selected.port = config::parse_visor_number(p, 2); o.portSource = "environment";
    }
    if (const char* p = std::getenv("FN_VISOR_USB_BUS")) {
        selected.bus = config::parse_visor_number(p, 255); o.busSource = "environment";
    }
    if (const char* p = std::getenv("FN_VISOR_USB_PATH")) {
        selected.path = p; o.pathSource = "environment";
        if (selected.path.empty()) throw config::InvalidVisorConfig("FN_VISOR_USB_PATH must not be empty");
    }
    selected.validate();
    o.port = selected.port; o.bus = selected.bus;
    std::string_view rest(selected.path);
    while (!rest.empty()) {
        const auto dot = rest.find('.');
        o.path.push_back(config::parse_visor_number(rest.substr(0, dot), 255));
        if (dot == rest.npos) break;
        rest.remove_prefix(dot + 1);
    }
    return o;
}
Options options_from_environment() { return resolve_options({}); }
struct Host::Impl {
    struct Slot {
        Impl* self;
        libusb_transfer* transfer{nullptr};
        bool pending{false};
        bool input;
        std::uint64_t generation{0};
    };
    Options options;
    Host::Now now;
    libusb_context* context{nullptr};
    libusb_device_handle* handle{nullptr};
    libusb_device* blocked{nullptr}; // kept referenced until verified absent
    int interface{-1};
    io::visor_usb::EndpointPair pair{};
    Slot rx{this, nullptr, false, true}, tx{this, nullptr, false, false};
    std::array<unsigned char, 64> input{};
    std::deque<io::protocol::ByteBuffer> outgoing;
    std::size_t offset{0};
    io::visor_usb::RawPackets incoming;
    bool active{false};
    std::uint64_t generation{0};
    Clock::time_point nextDiscovery{}, partialSince{}, txSince{};
    std::string error{"Not connected"}, deviceInfo;

    Impl(Options o, Host::Now clock) : options(std::move(o)), now(std::move(clock))
    {
        if ((options.port != 1 && options.port != 2) || options.path.size() > 7)
            throw std::invalid_argument("Invalid Visor USB options");
        const int rc = libusb_init(&context);
        if (rc) throw std::runtime_error(libusb_error_name(rc));
        rx.transfer = libusb_alloc_transfer(0); tx.transfer = libusb_alloc_transfer(0);
        if (!rx.transfer || !tx.transfer) {
            if (rx.transfer) libusb_free_transfer(rx.transfer);
            if (tx.transfer) libusb_free_transfer(tx.transfer);
            libusb_exit(context); throw std::bad_alloc();
        }
    }
    ~Impl()
    {
        libusb_free_transfer(rx.transfer); libusb_free_transfer(tx.transfer);
        if (blocked) libusb_unref_device(blocked);
        libusb_exit(context);
    }
    void report(const std::string& text)
    {
        if (error != text) FN_LOGW("visor-usb", "%s", text.c_str());
        error = text;
    }
    void stop(const std::string& why, bool quarantine = true)
    {
        active = false; incoming.clear(); partialSince = {};
        report(why);
        if (quarantine && handle && !blocked) blocked = libusb_ref_device(libusb_get_device(handle));
        // Buffers/outgoing remain intact until both cancellation callbacks return.
        if (rx.pending) libusb_cancel_transfer(rx.transfer);
        if (tx.pending) libusb_cancel_transfer(tx.transfer);
    }
    void finish_close()
    {
        if (active || rx.pending || tx.pending) return;
        if (handle) {
            if (interface >= 0) libusb_release_interface(handle, interface);
            libusb_close(handle); handle = nullptr; interface = -1;
        }
        outgoing.clear(); offset = 0;
    }
    static void LIBUSB_CALL completed(libusb_transfer* transfer)
    {
        auto& slot = *static_cast<Slot*>(transfer->user_data);
        auto& s = *slot.self;
        slot.pending = false;
        if (!s.active || slot.generation != s.generation) return;
        const int actual = transfer->actual_length;
        if (actual < 0 || actual > transfer->length) { s.stop("USB actual length overflow"); return; }
        if (slot.input) {
            if (transfer->status != LIBUSB_TRANSFER_COMPLETED && transfer->status != LIBUSB_TRANSFER_TIMED_OUT) {
                s.stop("USB IN failed; close/reopen required"); return;
            }
            const bool wasPartial = s.incoming.partial();
            const auto completedBefore = s.incoming.completed();
            if (!s.incoming.append({transfer->buffer, static_cast<std::size_t>(actual)})) {
                s.stop("Invalid/overflowed raw FujiBus stream; no resynchronization"); return;
            }
            if (!s.incoming.partial()) s.partialSince = {};
            else if (!wasPartial || completedBefore != s.incoming.completed()) s.partialSince = s.now();
        } else {
            // Never retry an error, including a zero-byte timeout. Completion is ambiguous.
            if (transfer->status != LIBUSB_TRANSFER_COMPLETED || actual == 0) {
                s.stop("USB OUT completion ambiguous; no replay, close/reopen required"); return;
            }
            s.offset += static_cast<std::size_t>(actual);
            if (s.offset == s.outgoing.front().size()) { s.outgoing.pop_front(); s.offset = 0; s.txSince = s.now(); }
        }
    }
    void submit(Slot& slot)
    {
        if (!active || slot.pending || (!slot.input && outgoing.empty())) return;
        auto* data = slot.input ? input.data() : outgoing.front().data() + offset;
        const auto length = slot.input ? pair.input.mps : outgoing.front().size() - offset;
        slot.generation = generation;
        libusb_fill_bulk_transfer(slot.transfer, handle,
            slot.input ? pair.input.address : pair.output.address,
            data, static_cast<int>(length), completed, &slot, 250);
        const int rc = libusb_submit_transfer(slot.transfer);
        if (rc) { stop(std::string("USB submit failed: ") + libusb_error_name(rc)); return; }
        slot.pending = true;
    }
    bool matches(libusb_device* device)
    {
        libusb_device_descriptor d{};
        if (libusb_get_device_descriptor(device, &d) || d.idVendor != 0x082d || d.idProduct != 0x0100) return false;
        if (options.bus && libusb_get_bus_number(device) != options.bus) return false;
        if (!options.path.empty()) {
            std::array<std::uint8_t, 7> path{};
            const int n = libusb_get_port_numbers(device, path.data(), path.size());
            if (n != static_cast<int>(options.path.size()) || !std::equal(options.path.begin(), options.path.end(), path.begin())) return false;
        }
        return true;
    }
    void discover()
    {
        if (now() < nextDiscovery) return;
        nextDiscovery = now() + 1s;
        libusb_device** list = nullptr;
        const auto n = libusb_get_device_list(context, &list);
        if (n < 0) { report("USB enumeration failed"); return; }
        if (blocked) {
            bool present = false;
            for (std::ptrdiff_t i = 0; i < n; ++i) present |= list[i] == blocked;
            if (present) { libusb_free_device_list(list, 1); return; }
            libusb_unref_device(blocked); blocked = nullptr;
        }
        libusb_device* chosen = nullptr;
        unsigned count = 0;
        for (std::ptrdiff_t i = 0; i < n; ++i) if (matches(list[i])) { chosen = list[i]; ++count; }
        if (count != 1) {
            report(count ? "Multiple matching Visors; set visor.bus and visor.path (or FN_VISOR_USB_BUS/PATH)" : "Visor absent");
            libusb_free_device_list(list, 1); return;
        }
        int rc = libusb_open(chosen, &handle);
        if (!rc) setup();
        else report(std::string("Cannot open Visor: ") + libusb_error_name(rc));
        libusb_free_device_list(list, 1);
        finish_close();
    }
    void setup_failed(const std::string& why)
    {
        // No packet transfers have been submitted during setup. Release any
        // claim and retry discovery without retiring a physical session.
        stop(why, false);
    }
    void setup()
    {
        int configuration = 0;
        int rc = libusb_get_configuration(handle, &configuration);
        if (rc || configuration != 1) { setup_failed("Expected active USB configuration 1; not changing configuration"); return; }
        libusb_config_descriptor* desc = nullptr;
        rc = libusb_get_active_config_descriptor(libusb_get_device(handle), &desc);
        if (rc) { setup_failed("Cannot read active USB descriptors"); return; }
        std::vector<io::visor_usb::Endpoint> eps;
        for (unsigned i = 0; i < desc->bNumInterfaces; ++i) {
            const auto& iface = desc->interface[i];
            for (int a = 0; a < iface.num_altsetting; ++a) {
                const auto& alt = iface.altsetting[a];
                for (unsigned e = 0; e < alt.bNumEndpoints; ++e) {
                    const auto& ep = alt.endpoint[e];
                    eps.push_back({desc->bConfigurationValue, alt.bInterfaceNumber, alt.bAlternateSetting,
                        alt.bInterfaceClass, ep.bEndpointAddress, unsigned(ep.bmAttributes & 3), ep.wMaxPacketSize});
                }
            }
        }
        libusb_free_config_descriptor(desc);
        // Ownership first: libusb answers it from the kernel, so a device whose interface
        // another driver holds is turned away without a single vendor request reaching it.
        // The Generic port's bulk endpoints are addressed by port number (OUT) and 0x80|port
        // (IN), so the interface is known from the descriptors before the connection query.
        for (const auto& ep : eps) {
            if (ep.configuration != 1 || ep.alternate != 0 || ep.interfaceClass != 255 ||
                ep.address != (0x80u | options.port)) continue;
            rc = libusb_kernel_driver_active(handle, ep.interface);
            if (rc == 1) { setup_failed("Interface owned by kernel driver; manual release required (no auto-detach)"); return; }
            if (rc < 0 && rc != LIBUSB_ERROR_NOT_SUPPORTED) { setup_failed("Cannot inspect USB interface ownership"); return; }
            break;
        }
        // Old request is authoritative for this tested family. Extended request is
        // diagnostic only; PIPE is expected on Prism and never selects HotSync.
        std::array<unsigned char, 20> info{};
        rc = libusb_control_transfer(handle, 0xc2, 4, 0, 0, info.data(), info.size(), 250);
        if (rc < 0 && rc != LIBUSB_ERROR_PIPE) { setup_failed("Extended connection query failed"); return; }
        rc = libusb_control_transfer(handle, 0xc2, 3, 0, 0, info.data(), 6, 250);
        auto ports = io::visor_usb::connection_info({info.data(), rc < 0 ? 0u : std::size_t(rc)});
        auto selected = ports ? io::visor_usb::select_generic_pair(*ports, eps, options.port) : std::nullopt;
        if (!selected) { setup_failed("Malformed connection info or missing/ambiguous Generic bulk pair"); return; }
        pair = *selected;
        rc = libusb_kernel_driver_active(handle, pair.input.interface);
        if (rc == 1) { setup_failed("Interface owned by kernel driver; manual release required (no auto-detach)"); return; }
        if (rc < 0 && rc != LIBUSB_ERROR_NOT_SUPPORTED) { setup_failed("Cannot inspect USB interface ownership"); return; }
        rc = libusb_claim_interface(handle, pair.input.interface);
        if (rc) { setup_failed(std::string("Cannot claim interface: ") + libusb_error_name(rc)); return; }
        interface = pair.input.interface;
        unsigned char alternate = 255;
        rc = libusb_control_transfer(handle, 0x81, 10, 0, interface, &alternate, 1, 250);
        if (rc != 1 || alternate != pair.input.alternate) { setup_failed("Active USB alternate setting mismatch"); return; }
        if (generation == std::numeric_limits<std::uint64_t>::max()) {
            setup_failed("USB generation exhausted; restart process"); return;
        }
        ++generation;
        auto* device = libusb_get_device(handle);
        deviceInfo = "082d:0100 bus=" + std::to_string(libusb_get_bus_number(device)) +
            " address=" + std::to_string(libusb_get_device_address(device));
        std::array<std::uint8_t, 7> path{};
        const int depth = libusb_get_port_numbers(device, path.data(), path.size());
        deviceInfo += " path=";
        for (int i = 0; i < depth; ++i) deviceInfo += (i ? "." : "") + std::to_string(path[i]);
        incoming.clear(); outgoing.clear(); offset = 0; active = true;
        report("Connected Generic " + std::to_string(options.port));
    }
    void pump(std::chrono::milliseconds timeout)
    {
        timeval tv{0, static_cast<long>(std::clamp<long long>(timeout.count(), 0, 10) * 1000)};
        const int rc = libusb_handle_events_timeout_completed(context, &tv, nullptr);
        if (rc && rc != LIBUSB_ERROR_INTERRUPTED) stop("USB event handling failed");
    }
    void poll()
    {
        pump(0ms);
        if (active && incoming.partial() && now() - partialSince >= 5s) stop("Truncated raw packet deadline");
        if (active && !outgoing.empty() && now() - txSince >= 5s) stop("USB OUT total deadline; no replay");
        finish_close();
        if (!active && !handle && !rx.pending && !tx.pending) discover();
        submit(rx); submit(tx);
    }
};
Host::Host(Options o, Now now) : _impl(std::make_unique<Impl>(std::move(o), std::move(now))) {}
Host::~Host()
{
    _impl->stop("Host shutdown");
    // Cancellation bounds use wall time even when operational time is injected.
    const auto deadline = Clock::now() + 2s;
    while ((_impl->rx.pending || _impl->tx.pending) && Clock::now() < deadline) _impl->pump(10ms);
    if (_impl->rx.pending || _impl->tx.pending) {
        // Exceptional OS failure: fail closed, never free buffers still owned by USB.
        // Deliberately retain context/handle/storage until process exit, not a UAF.
        FN_LOGE("visor-usb", "Cancellation did not finish; retaining USB storage until process exit");
        (void)_impl.release();
    } else _impl->finish_close();
}
void Host::poll() { _impl->poll(); }
void Host::wait(std::chrono::milliseconds t) { _impl->pump(t); }
bool Host::active() const { return _impl->active; }
bool Host::needs_poll() const { return active() && (_impl->incoming.ready() || !_impl->rx.pending ||
        (!_impl->outgoing.empty() && !_impl->tx.pending)); }
std::uint64_t Host::generation() const { return _impl->generation; }
bool Host::take(io::protocol::ByteBuffer& p) { return active() && _impl->incoming.pop(p); }
bool Host::send(const io::protocol::ByteBuffer& p)
{
    if (!active()) return false;
    if (p.empty() || p.size() > io::visor_usb::packet_capacity ||
        !io::protocol::FujiBusPacket::fromRaw(p) || _impl->outgoing.size() >= 4) {
        retire("Invalid response or USB output queue full"); return false;
    }
    if (_impl->outgoing.empty()) _impl->txSince = _impl->now();
    _impl->outgoing.push_back(p); _impl->submit(_impl->tx); return active();
}
void Host::retire(const std::string& reason) { _impl->stop(reason); }
std::string Host::status() const
{
    std::string path;
    for (auto component : _impl->options.path) path += (path.empty() ? "" : ".") + std::to_string(component);
    const auto& o = _impl->options;
    return "selection_port=" + std::to_string(o.port) + " source=" + o.portSource +
        "\r\nselection_bus=" + (o.bus ? std::to_string(o.bus) : "unset") + " source=" + o.busSource +
        "\r\nselection_path=" + (path.empty() ? "unset" : path) + " source=" + o.pathSource + "\r\n" +
        "active=" + std::to_string(active()) + " generation=" + std::to_string(generation()) +
        " port=" + std::to_string(_impl->options.port) + " in=" + std::to_string(_impl->pair.input.address) +
        " out=" + std::to_string(_impl->pair.output.address) + " mps=" + std::to_string(_impl->pair.input.mps) +
        " tx_queue=" + std::to_string(_impl->outgoing.size()) + " " + _impl->deviceInfo + " last=" + _impl->error;
}
} // namespace fujinet::platform::posix::visor_usb
