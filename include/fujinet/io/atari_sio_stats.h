#pragma once

#include <array>
#include <cstdint>

namespace fujinet::io {

// Counters for bringing up the physical Atari SIO ingress. They are plain counts
// bumped on the polling path (no logging there: SIO timing is tight), read by the
// sio.status console command.

// A command frame the capture could not read, kept so sio.status can show what arrived.
struct AtariSioBadFrame {
    enum class Reason : std::uint8_t { None, Checksum, Overflow, RxError, Short };
    Reason reason{Reason::None};
    std::uint8_t length{0};            // bytes collected before it was rejected (at most 5 are kept)
    std::array<std::uint8_t, 5> bytes{};
};

// What the capture state machine saw, from the UART and the CMD line.
struct AtariSioCaptureStats {
    struct Trace {
        std::uint32_t time_us{0}, edges{0};
        std::uint32_t duration_us{0}, baud{0};
        std::array<std::uint8_t, 5> frame{};
        std::uint16_t pending{0};
        std::uint8_t event{0}, used{0};
        bool low{false}, collecting{false}, released{false};
    };
    std::array<Trace, 16> trace{};
    std::uint32_t trace_count{0};
    struct AltDecision {
        std::array<std::uint8_t, 5> frame{};
        std::uint32_t edges{0};
        std::uint8_t main_bytes{0};
        bool collecting{false};
        bool released{false};
    };
    std::uint32_t rx_bytes{0};         // bytes read from the SIO UART
    std::uint32_t frames{0};           // complete 5-byte command frames handed to the router
    std::uint32_t frames_from_second_listener{0};  // frames the second listener decoded when the main UART could not
    std::array<AltDecision, 8> first_alt_decisions{};
    std::uint32_t frames_bad{0};       // command frames that could not be read: bad checksum, UART error or overflow
    AtariSioBadFrame first_bad;        // the first and the most recent of them
    AtariSioBadFrame last_bad;
    std::uint32_t aborts_edge{0};      // CMD edges that did not fit a frame (extra, early or missing)
    std::uint32_t frames_lost{0};      // command frames abandoned while being received (CMD went low, no 5 good bytes): the Atari retries them
    std::uint32_t aborts_edge_in_write{0};  // ... of which while an accepted write waited for its data frame
    std::uint32_t write_glitches_ignored{0};  // CMD edges ignored there because the line read high (sio.tune glitch)
    std::uint32_t aborts_timeout{0};   // frame not complete in 50 ms, or write data not in 200 ms
    std::uint32_t aborts_overflow{0};  // more than 5 command bytes while CMD was low
    std::uint32_t aborts_rx_error{0};  // UART framing/parity/overflow error during a frame or data
    // Timing, in microseconds (the clock the ingress was given; since boot on the device).
    std::uint32_t first_frame_us{0};   // when the first complete command frame was processed (0 = none)
    std::uint32_t lag_us_max{0};       // CMD release edge -> frame processing started: how fast the loop reacts
};

// When each reply started, measured where the ingress sends it. A command reply is judged against the
// CMD release edge (the Atari expects the ACK within 16 ms); every reply is judged against when its
// processing started, which is the time the handler took.
struct AtariSioReplyStats {
    static constexpr std::uint32_t ack_deadline_us = 16000;

    std::uint32_t ready_us{0};         // when the ingress was ready to serve (set once, kept across resets)
    std::uint32_t replies{0};
    std::uint32_t reply_us_last{0};    // command replies: CMD release edge -> first reply byte sent
    std::uint32_t reply_us_max{0};
    std::uint32_t reply_over_deadline{0};
    std::uint32_t handle_us_last{0};   // any reply: processing started -> reply sent
    std::uint32_t handle_us_max{0};
    // Replies with a data frame (over 64 bytes): the time to write the data and for the UART to send it
    // (flushed), to compare with the wire time at the current baud. The gaps before it are not included.
    std::uint32_t data_replies{0};
    std::uint32_t data_bytes_total{0};
    std::uint32_t data_us_total{0};
    std::uint32_t data_bytes_last{0};
    std::uint32_t data_us_last{0};

    // Unsigned subtraction, so the clock's 32-bit wrap does not matter for short intervals.
    void record(std::uint32_t send_us, std::uint32_t processing_start_us, bool command_reply,
                std::uint32_t cmd_release_edge_us) noexcept
    {
        ++replies;
        handle_us_last = send_us - processing_start_us;
        if (handle_us_last > handle_us_max) handle_us_max = handle_us_last;
        if (command_reply) {
            // A stray CMD edge can be stamped a little after send_us was read, which would make the
            // unsigned difference wrap to ~71 minutes. The release was then not that edge: count 0.
            const auto since_release = static_cast<std::int32_t>(send_us - cmd_release_edge_us);
            reply_us_last = since_release < 0 ? 0 : static_cast<std::uint32_t>(since_release);
            if (reply_us_last > reply_us_max) reply_us_max = reply_us_last;
            if (reply_us_last > ack_deadline_us) ++reply_over_deadline;
        }
    }
    void record_data(std::uint32_t bytes, std::uint32_t tx_us) noexcept
    {
        if (bytes <= 64) return;
        ++data_replies;
        data_bytes_total += bytes;
        data_us_total += tx_us;
        data_bytes_last = bytes;
        data_us_last = tx_us;
    }
    void reset() noexcept
    {
        const std::uint32_t ready = ready_us;
        *this = {};
        ready_us = ready;
    }
};

// What the router did with each frame.
struct AtariSioRouterStats {
    std::uint32_t disk_commands{0};    // D1:-D8: (0x31-0x38)
    std::uint32_t nio_commands{0};     // the native endpoint (0x7F)
    std::uint32_t other_commands{0};   // frames for other bus devices, ignored on purpose
    bool has_last_frame{false};
    std::array<std::uint8_t, 5> last_frame{};  // device, command, aux1, aux2, checksum
    // Frames for our devices that we answered with NAK: a command we do not have, or one that is not
    // allowed in the current state. The first and the latest, to show what a driver asks that we lack.
    // 3F (high-speed index) queries received, whether or not they were answered, by owner.
    std::uint32_t hsio_queries_disk{0};
    std::uint32_t hsio_queries_nio{0};
    std::uint32_t refused{0};
    std::array<std::uint8_t, 5> first_refused{};
    std::array<std::uint8_t, 5> last_refused{};
    // The latest frame for a device that is not ours (ignored on purpose).
    bool has_last_other{false};
    std::array<std::uint8_t, 5> last_other{};
    // The first few frames for other devices since the reset, in order: a driver's probe (3F) comes
    // before the command it probes for, and the retries that follow would otherwise hide it.
    static constexpr std::size_t other_kept = 8;
    std::uint8_t other_first_count{0};
    std::array<std::array<std::uint8_t, 5>, other_kept> other_first{};
};

// Where the time goes, per command: for each native command (Q, W, P, R; everything else in one bucket)
// the time we took from the CMD release to the end of the last byte we sent for it (`service`), and the
// time from then to the CMD release of the Atari's next command (`idle`: the Atari's own processing and
// the time its next command frame takes to send). Summed over all the commands, service plus idle is the
// elapsed time of a run, so what the Atari side costs can be told apart from what the FujiNet costs.
struct AtariSioTxnStats {
    static constexpr std::size_t buckets = 5;   // Q, W, P, R, other
    static constexpr std::uint32_t max_idle_us = 1'000'000;  // a longer pause is the user, not the protocol
    struct Entry {
        std::uint32_t count{0};
        std::uint64_t service_us{0};
        std::uint64_t idle_us{0};
    };
    std::array<Entry, buckets> entry{};

    static std::size_t bucket_for(std::uint8_t command) noexcept
    {
        return command == 'Q' ? 0 : command == 'W' ? 1 : command == 'P' ? 2 : command == 'R' ? 3 : 4;
    }
    // A command frame was released at `release_us` and its first reply is about to go out. This closes the
    // previous command: its service ran to `_end_us` and its idle time to this release.
    void begin(std::uint8_t command, std::uint32_t release_us) noexcept
    {
        if (_open) {
            const auto idle = static_cast<std::int32_t>(release_us - _end_us);
            const auto service = static_cast<std::int32_t>(_end_us - _release_us);
            if (idle >= 0 && static_cast<std::uint32_t>(idle) <= max_idle_us && service >= 0) {
                Entry& e = entry[_bucket];
                ++e.count;
                e.service_us += static_cast<std::uint32_t>(service);
                e.idle_us += static_cast<std::uint32_t>(idle);
            }
        }
        _open = true;
        _bucket = bucket_for(command);
        _release_us = release_us;
        _end_us = release_us;
    }
    // The last byte of a reply to the open command left the UART at `end_us`.
    void end(std::uint32_t end_us) noexcept { _end_us = end_us; }
    void reset() noexcept { *this = {}; }
private:
    bool _open{false};
    std::size_t _bucket{4};
    std::uint32_t _release_us{0};
    std::uint32_t _end_us{0};
};

// Values the ingress uses that are tunable from the console (sio.tune) to find out what each one buys;
// the defaults are the ones the evidence in docs/atari/hsio-report.md supports.
struct AtariSioTuning {
    std::uint32_t ack_gap_us{250};      // ACK -> COMPLETE (the legacy DELAY_T5)
    std::uint32_t data_gap_us{0};       // COMPLETE -> data frame, at high speed only (the legacy DELAY_HSIO_DATA is 1000)
    std::uint32_t alt_window_us{10000}; // how long a frame heard by the second listener stays usable
    bool ignore_write_glitches{true};   // while a write waits for its data, CMD edges with the line high are noise
    bool proc_notify{false};            // pulse PROC when a network session becomes readable; Q flag bit 1 says so
};

// The whole picture the console shows; the ingress fills in the hardware parts.
struct AtariSioStats {
    std::uint32_t cmd_edges{0};        // CMD line edges seen by the interrupt since the last reset
    std::uint32_t first_edge_us{0};    // when the first CMD edge was seen (0 = none)
    bool cmd_line_low{false};          // CMD level right now (low = asserted by the Atari)
    std::uint32_t uart_rx_errors{0};   // the SIO UART's own error count
    AtariSioCaptureStats capture;
    AtariSioRouterStats router;
    AtariSioReplyStats reply;
    // Longest time between two polls of the ingress during which the Atari signalled CMD (a stalled
    // SIO task that could miss a command). Idle gaps with no CMD edge are not counted.
    std::uint32_t poll_gap_us_max{0};
    std::uint32_t poll_gap_at_us{0};   // when that longest gap began (same clock as first_edge_us)
    std::uint32_t uart_baud_set{0};    // the baud the SIO UART was last told to use (0 = unknown)
    std::uint32_t uart_baud_hw{0};     // what the UART hardware reports back after its divider rounding
    int hsio_index{-1};                // high-speed SIO index answered to 3F (-1 = off)
    bool hsio_high{false};             // the UART is at the high speed right now
    std::uint32_t hsio_switches{0};    // times the UART was switched to high speed
    std::uint32_t hsio_fallbacks{0};   // times it was switched back after unreadable command frames
    bool hsio_fallback_enabled{true};  // false: stay at the high speed even if command frames are unreadable
    // The second listener (docs/atari/hsio.md): another UART on the same RX pin at the speed the main one
    // is not using. Observation only: it counts command frames it decoded with a valid checksum.
    std::uint32_t alt_baud{0};         // 0 = no second listener
    std::uint32_t alt_valid_frames{0};
    std::uint32_t alt_ambiguous_dropped{0};
    std::array<std::uint8_t, 5> alt_last_frame{};
    // CMD edges that arrived while we were transmitting a reply and were still pending when it ended with
    // CMD high: the Atari does not touch CMD then, so they were noise (the data-out line coupling onto it).
    std::uint32_t tx_noise_edges{0};
    AtariSioTuning tuning;
    AtariSioTxnStats txn;
    std::uint32_t stale_replies_dropped{0};  // native replies the Atari never read, discarded after 2 s
    // The PROC (proceed) line to the Atari, open drain: low asserts it and the Atari's PIA sees a falling
    // edge. Driven only by sio.proc so far (docs/atari/proc.md).
    bool proc_available{false};        // the board maps a PROC pin
    bool proc_low{false};              // the pin reads low (asserted) right now
    std::uint32_t proc_asserts{0};     // times it was driven low (a pulse is one)
    std::uint32_t proc_notices{0};     // 'a session is readable' notices received from the network service
    std::uint32_t proc_pulses{0};      // PROC pulses sent for them (notices that came close together share one)
};

} // namespace fujinet::io
