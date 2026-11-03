#pragma once

#include "fujinet/config/fuji_config.h"
#include "fujinet/io/atari_sio_disk_frontend.h"
#include "fujinet/io/atari_sio_ingress_router.h"
#include "fujinet/io/atari_sio_nio_frontend.h"
#include "fujinet/io/atari_sio_speed.h"
#include "fujinet/io/atari_sio_transaction_boundary.h"
#include "fujinet/io/core/work_source.h"

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

namespace fujinet::platform::esp32 {

class UartChannel;

// The single owner of Atari SIO UART/GPIO sequencing. D1:-D8: remain direct
// DiskService traffic; 0x7f is the native packet endpoint on this same ingress.
class AtariSioIngress final : public io::IWorkSource {
public:
    AtariSioIngress(const config::FujiConfig& config,
                    disk::DiskService& disks,
                    io::IRequestHandler& routing);
    ~AtariSioIngress() override;

    void poll();
    // Counters for bring-up (sio.status). stats() reads the CMD line too.
    io::AtariSioStats stats() const;
    bool advertise_hsio_index(int index);
    void reset_stats();
    // Briefly sets the SIO UART to `target` baud, returns what the hardware reports, and restores the
    // current baud. For the sio.baud check only: run it with the Atari idle.
    std::uint32_t probe_baud(std::uint32_t target);
    // High-speed SIO (docs/atari/hsio.md): -1 turns it off, 0..9 sets the index answered to 3F. It takes
    // effect when the Atari next asks, and is not saved. False for an index out of range.
    // `fallback` false keeps the UART at the high speed however many command frames are unreadable.
    bool set_hsio_index(int index, bool fallback = true);
    // PROC (proceed) to the Atari: drive the open-drain pin low (asserted) or release it. False when the
    // board maps no PROC pin. pulse_proc() asserts it `count` times for `low_ms` with `high_ms` between and
    // blocks the calling task for the whole time, so run it with the Atari idle (sio.proc).
    bool set_proc(bool asserted);
    bool pulse_proc(unsigned count, unsigned low_ms, unsigned high_ms);
    // The tunable gaps and window (sio.tune); not saved.
    void set_tuning(const io::AtariSioTuning& tuning)
    {
        _tuning = tuning;
        _capture.set_ignore_write_glitches(tuning.ignore_write_glitches);
        _nio.set_proc_notify(tuning.proc_notify);
    }
    // ISR-safe edge recorder; UART/event work remains the sole wake source.
    void note_cmd_edge() noexcept;
    bool supports_work_wait() const override;
    bool wait_for_work(std::chrono::milliseconds timeout) override;

private:
    void send(const std::vector<std::uint8_t>& bytes);
    void enter_high_speed();
    // The second listener follows whichever speed the main UART is not at.
    void retune_alt();
    void poll_alt();
    // The Atari used the other speed: switch the main UART to it. Used by the bad-frame rule and by the
    // second listener.
    void toggle_speed();
    // For the capture: a command frame the second listener decoded for the command that just ended.
    bool take_alt_frame(std::array<std::uint8_t, 5>& frame, bool unambiguous);
    void leave_high_speed();
    void service_proc_notice();
    // The ACK for a native W's data frame, sent before the request runs (AtariSioNioFrontend::set_early_ack).
    void send_early_ack();

    std::unique_ptr<UartChannel> _uart;
    std::unique_ptr<UartChannel> _alt;
    std::array<std::uint8_t, 5> _alt_window{};
    std::size_t _alt_used{0};
    std::uint32_t _alt_valid{0};
    std::uint32_t _alt_ambiguous_dropped{0};
    std::array<std::uint8_t, 5> _alt_last{};
    bool _alt_pending{false};
    std::uint32_t _alt_found_us{0};
    io::AtariSioDiskFrontend _disk;
    io::AtariSioNioFrontend _nio;
    io::AtariSioIngressRouter _router;
    io::AtariSioTransactionBoundary _boundary;
    io::AtariSioCapture _capture;
    io::AtariSioReplyStats _reply;
    io::AtariSioSpeed _speed;
    io::AtariSioTuning _tuning;
    io::AtariSioTxnStats _txn;
    std::uint32_t _hsio_switches{0};
    std::uint32_t _hsio_fallbacks{0};
    std::uint32_t _last_poll_us{0};
    std::uint32_t _edges_at_last_poll{0};
    std::uint32_t _poll_gap_us_max{0};
    std::uint32_t _poll_gap_at_us{0};
    int _cmd_pin{-1};
    int _proc_pin{-1};
    std::uint32_t _proc_asserts{0};
    // PROC for a readable network session (docs/atari/proc.md): the network service's notice only sets
    // the flag; poll() pulses the line when the bus is idle, at most once per _proc_min_gap_us.
    bool _proc_wanted{false};
    std::uint32_t _proc_notices{0};
    std::uint32_t _proc_pulses{0};
    std::uint32_t _proc_last_pulse_us{0};
    std::uint32_t _early_ack_us{0};   // when that ACK left, so the COMPLETE that follows keeps the ACK gap (0 = none)
    void* _owner_task{nullptr};
};

} // namespace fujinet::platform::esp32
