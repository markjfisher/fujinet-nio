#pragma once

#include <array>
#include <cstddef>
#include <atomic>
#include <cstdint>

#include "fujinet/io/atari_sio_stats.h"

namespace fujinet::io {

// A counter detects multiple observed edges between polls; unlike a boolean it
// cannot silently turn two observed edges into one. Hardware may still merge
// interrupts while masked, so this is not a physical edge timestamp.
class AtariSioTransactionBoundary {
public:
    // `at_us` is the interrupt's clock reading, for diagnostics only (0 when the caller has none).
    void note_cmd_edge(std::uint32_t at_us = 0) noexcept
    {
        _edges.fetch_add(1, std::memory_order_relaxed);
        if (_total.fetch_add(1, std::memory_order_relaxed) == 0) {
            _first_edge_us.store(at_us, std::memory_order_relaxed);
        }
        _last_edge_us.store(at_us, std::memory_order_relaxed);
    }
    std::uint32_t consume() noexcept { return _edges.exchange(0, std::memory_order_acq_rel); }
    // Every edge since the last reset, for diagnostics; consume() does not affect it.
    std::uint32_t total_edges() const noexcept { return _total.load(std::memory_order_relaxed); }
    void reset_total_edges() noexcept
    {
        _total.store(0, std::memory_order_relaxed);
        _first_edge_us.store(0, std::memory_order_relaxed);
    }
    std::uint32_t first_edge_us() const noexcept { return _first_edge_us.load(std::memory_order_relaxed); }
    std::uint32_t last_edge_us() const noexcept { return _last_edge_us.load(std::memory_order_relaxed); }
private:
    std::atomic<std::uint32_t> _edges{0};
    std::atomic<std::uint32_t> _total{0};
    std::atomic<std::uint32_t> _first_edge_us{0};
    std::atomic<std::uint32_t> _last_edge_us{0};
};

// State machine used by the ESP32 ingress. The callables expose the existing
// GPIO, UART and clock operations so the identical ordering logic can run in
// host tests without ESP-IDF.
class AtariSioCapture {
public:
    // True while the loop must not sleep: a command frame is being collected, or an accepted write is
    // waiting for its data frame (no CMD edge wakes the task for that one, so sleeping a tick, 10 ms, added
    // up to 10 ms to every write, native or disk).
    bool needs_immediate_poll() const noexcept { return _collecting || _write_started_us != 0; }

    // While a write waits for its data frame the Atari does not touch CMD, so edges seen with the line
    // high are noise; ignoring them keeps the write going instead of aborting it (the Atari would time out).
    void set_ignore_write_glitches(bool on) noexcept { _ignore_write_glitches = on; }
    // The command frame handled last (the command letter is byte 1).
    const std::array<std::uint8_t, 5>& last_frame() const noexcept { return _frame; }

    template<class Router, class Uart, class Asserted, class Now>
    void poll(AtariSioTransactionBoundary& boundary, Router& router, Uart& uart,
              Asserted asserted, Now now)
    {
        poll(boundary, router, uart, asserted, now, [](std::array<std::uint8_t, 5>&, bool) { return false; });
    }

    // `alt_frame(frame, unambiguous)` (a second listener at the other speed): true, with a good command frame, when
    // one was decoded for the command that just ended. The capture then drops what the main UART made
    // of it and handles that frame instead. The callable switches the speed before it returns true, so
    // the reply goes out at the speed the Atari used. When unambiguous is false,
    // the callable must discard any pending frame without switching speed.
    template<class Router, class Uart, class Asserted, class Now, class AltFrame>
    void poll(AtariSioTransactionBoundary& boundary, Router& router, Uart& uart,
              Asserted asserted, Now now, AltFrame alt_frame)
    {
        const auto abort = [this, &router, &uart, &boundary, &asserted, &now](std::uint32_t AtariSioCaptureStats::* reason) {
            const bool idle_edge = reason == &AtariSioCaptureStats::aborts_edge &&
                                   !_collecting && !router.pending_write_bytes();
            if (!idle_edge || _stats.trace_count == 0 ||
                _stats.trace[(_stats.trace_count - 1) % _stats.trace.size()].event != 6) {
                auto& t = _stats.trace[_stats.trace_count++ % _stats.trace.size()];
                t.time_us = static_cast<std::uint32_t>(now());
                t.edges = boundary.total_edges();
                t.frame = _frame;
                t.pending = static_cast<std::uint16_t>(router.pending_write_bytes());
                t.event = idle_edge ? 6 : reason == &AtariSioCaptureStats::aborts_edge ? 2 :
                          reason == &AtariSioCaptureStats::aborts_timeout ? 3 :
                          reason == &AtariSioCaptureStats::aborts_overflow ? 4 : 5;
                t.used = static_cast<std::uint8_t>(_frame_used);
                t.low = asserted(); t.collecting = _collecting; t.released = _released;
            }
            ++(_stats.*reason);
            if (_collecting) ++_stats.frames_lost;
            if (reason == &AtariSioCaptureStats::aborts_edge && !_collecting && router.pending_write_bytes()) {
                ++_stats.aborts_edge_in_write;
            }
            // A command frame the UART could not read (framing errors or too many bytes) is what the
            // Atari sending at another speed looks like. Edge and timeout aborts are noise, not speed.
            // A frame that ended (CMD released) with some bytes but fewer than 5: what a frame at a faster
            // speed looks like to a slower UART. Needs a byte, so a bare CMD glitch is not counted.
            if (_collecting && _released && _frame_used >= 1 && _frame_used < _frame.size() &&
                reason == &AtariSioCaptureStats::aborts_edge) {
                note_bad_frame(AtariSioBadFrame::Reason::Short);
            }
            if (_collecting && (reason == &AtariSioCaptureStats::aborts_rx_error ||
                                reason == &AtariSioCaptureStats::aborts_overflow)) {
                note_bad_frame(reason == &AtariSioCaptureStats::aborts_overflow
                                   ? AtariSioBadFrame::Reason::Overflow
                                   : AtariSioBadFrame::Reason::RxError);
            }
            router.reset_command_collection();
            _collecting = false;
            _released = false;
            _frame_used = 0;
            _write_started_us = 0;
            uart.discardInput();
            _rx_errors = uart.rx_errors();
            _discard_until_release = true;
        };
        if (_write_started_us && now() - _write_started_us > 200000) {
            abort(&AtariSioCaptureStats::aborts_timeout);
            return;
        }
        std::uint32_t edges = boundary.consume();
        const bool low = asserted();
        if (_ignore_write_glitches && edges != 0 && !low && !_collecting && router.pending_write_bytes()) {
            _stats.write_glitches_ignored += edges;
            edges = 0;
        }
        if (!low) {
            std::array<std::uint8_t, 5> from_alt{};
            // Multiple pending CMD edges cannot identify the transaction that
            // produced an alternate frame. Discard it without changing baud.
            if (alt_frame(from_alt, edges <= 1)) {
                if (_stats.frames_from_second_listener < _stats.first_alt_decisions.size()) {
                    auto& decision = _stats.first_alt_decisions[_stats.frames_from_second_listener];
                    decision.frame = from_alt;
                    decision.edges = edges;
                    decision.main_bytes = static_cast<std::uint8_t>(_frame_used);
                    decision.collecting = _collecting;
                    decision.released = _released;
                }
                router.reset_command_collection();
                uart.discardInput();
                _rx_errors = uart.rx_errors();
                _frame = from_alt;
                _collecting = false;
                _released = false;
                _discard_until_release = false;
                _write_started_us = 0;
                ++_stats.frames_from_second_listener;
                _bad_frames_in_a_row = 0;   // the garbage the main UART made of it was this frame
                dispatch_frame(boundary, router, now);
                return;
            }
        }
        if (_discard_until_release) {
            uart.discardInput();
            if (!low && edges == 0) _discard_until_release = false;
            return;
        }
        if (edges > 1 || (edges == 1 && (low == _collecting || _released))) {
            abort(&AtariSioCaptureStats::aborts_edge);
            return;
        }
        if (edges == 1 && low) {
            router.reset_command_collection();
            _frame_used = 0;
            _write_started_us = 0;
            _collecting = true;
            _released = false;
            _started_us = now();
        }
        if (_collecting && now() - _started_us > 50000) {
            abort(&AtariSioCaptureStats::aborts_timeout);
            return;
        }
        std::uint8_t bytes[256];
        while (uart.available()) {
            const auto count = uart.read(bytes, sizeof(bytes));
            _stats.rx_bytes += static_cast<std::uint32_t>(count);
            const bool rx_error = uart.rx_errors() != _rx_errors;
            std::uint32_t seen = boundary.consume();
            const bool low_now = asserted();
            if (_ignore_write_glitches && seen != 0 && !low_now && !low && !_collecting &&
                router.pending_write_bytes()) {
                _stats.write_glitches_ignored += seen;
                seen = 0;
            }
            if (rx_error || seen != 0 || low_now != low) {
                abort(rx_error ? &AtariSioCaptureStats::aborts_rx_error : &AtariSioCaptureStats::aborts_edge);
                return;
            }
            for (std::size_t i = 0; i < count; ++i) {
                if (_collecting) {
                    if (_frame_used == _frame.size()) { abort(&AtariSioCaptureStats::aborts_overflow); return; }
                    _frame[_frame_used++] = bytes[i];
                } else if (router.pending_write_bytes()) {
                    _processing_start_us = static_cast<std::uint32_t>(now());
                    _processing_command = false;
                    router.consume(bytes[i]);
                    _write_started_us = router.pending_write_bytes() ? now() : 0;
                }
                // Idle bytes, including traffic for another peripheral, are ignored.
            }
        }
        if ((_collecting || router.pending_write_bytes()) && uart.rx_errors() != _rx_errors) {
            abort(&AtariSioCaptureStats::aborts_rx_error);
            return;
        }
        if (_collecting && edges == 1 && !low) _released = true;
        if (_collecting && _released && _frame_used == _frame.size()) {
            if (boundary.consume() != 0 || asserted()) {
                abort(&AtariSioCaptureStats::aborts_edge);
                return;
            }
            _collecting = false;
            _released = false;
            if (frame_checksum_ok()) {
                _bad_frames_in_a_row = 0;
            } else {
                note_bad_frame(AtariSioBadFrame::Reason::Checksum);
            }
            dispatch_frame(boundary, router, now);
        }
        if (!_collecting && !router.pending_write_bytes()) _rx_errors = uart.rx_errors();
    }
    const AtariSioCaptureStats& stats() const noexcept { return _stats; }
    // For the ingress's reply timing: when the frame or data now being handled started processing,
    // and whether it is a command frame (judged against the CMD release edge) or a data frame.
    std::uint32_t processing_start_us() const noexcept { return _processing_start_us; }
    std::uint32_t processing_release_us() const noexcept { return _processing_release_us; }
    bool processing_command() const noexcept { return _processing_command; }
    void reset_stats() noexcept { _stats = {}; }
    // Metadata only: never retain a response body in diagnostics.
    void trace_reply(std::uint32_t time, std::uint32_t duration, std::uint32_t baud,
                     std::uint16_t length, std::uint8_t first, std::uint8_t second) noexcept
    {
        auto& t = _stats.trace[_stats.trace_count++ % _stats.trace.size()];
        t = {};
        t.time_us = time; t.duration_us = duration; t.baud = baud;
        t.event = 7; t.pending = length; t.frame[0] = first; t.frame[1] = second;
    }
    // Command frames in a row that could not be read. The ingress uses it to leave high-speed SIO.
    std::uint32_t bad_frames_in_a_row() const noexcept { return _bad_frames_in_a_row; }
    void clear_bad_frames() noexcept { _bad_frames_in_a_row = 0; }
private:
    // Hands the complete command frame in `_frame` to the router and starts timing its reply.
    template<class Router, class Now>
    void dispatch_frame(AtariSioTransactionBoundary& boundary, Router& router, Now now)
    {
        auto& t = _stats.trace[_stats.trace_count++ % _stats.trace.size()];
        t = {};
        t.time_us = static_cast<std::uint32_t>(now());
        t.edges = boundary.total_edges();
        t.frame = _frame;
        t.event = 1;
        t.used = 5;
        ++_stats.frames;
        const auto start = static_cast<std::uint32_t>(now());
        if (_stats.frames == 1) _stats.first_frame_us = start;
        const std::uint32_t lag = start - boundary.last_edge_us();
        if (lag > _stats.lag_us_max) _stats.lag_us_max = lag;
        _processing_start_us = start;
        _processing_release_us = boundary.last_edge_us();
        _processing_command = true;
        for (const auto byte : _frame) router.consume(byte);
        _write_started_us = router.pending_write_bytes() ? now() : 0;
        _frame_used = 0;
    }
    void note_bad_frame(AtariSioBadFrame::Reason reason) noexcept
    {
        AtariSioBadFrame bad;
        bad.reason = reason;
        bad.length = static_cast<std::uint8_t>(_frame_used);
        bad.bytes = _frame;
        if (_stats.frames_bad == 0) _stats.first_bad = bad;
        _stats.last_bad = bad;
        ++_stats.frames_bad;
        ++_bad_frames_in_a_row;
    }
    bool frame_checksum_ok() const noexcept
    {
        std::uint16_t sum = 0;
        for (std::size_t i = 0; i + 1 < _frame.size(); ++i) {
            sum = static_cast<std::uint16_t>(sum + _frame[i]);
            sum = static_cast<std::uint16_t>((sum & 0xffu) + (sum >> 8));
        }
        return static_cast<std::uint8_t>(sum) == _frame.back();
    }

    AtariSioCaptureStats _stats;
    std::uint32_t _bad_frames_in_a_row{0};
    std::uint32_t _processing_start_us{0};
    std::uint32_t _processing_release_us{0};
    bool _processing_command{false};
    std::array<std::uint8_t, 5> _frame{};
    std::size_t _frame_used{0};
    std::int64_t _started_us{0};
    std::int64_t _write_started_us{0};
    std::uint32_t _rx_errors{0};
    bool _collecting{false};
    bool _released{false};
    bool _discard_until_release{false};
    bool _ignore_write_glitches{false};
};

} // namespace fujinet::io
