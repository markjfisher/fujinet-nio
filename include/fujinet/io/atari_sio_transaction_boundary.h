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
    // Classify a reply-time high-level interrupt before a later real assertion
    // can join its batch. Once low is observed, preserve every subsequent edge.
    void note_bus_edge(std::uint32_t at_us, bool low) noexcept
    {
        const auto state = _reply_state.load(std::memory_order_acquire);
        const bool filtered = !low && state == 1;
        // Single ISR writer; atomic fields and version checks permit a bounded
        // task-side snapshot without a lock, allocation, or ISR logging.
        const auto sequence = ++_edge_sequence;
        auto& slot = _observations[sequence % _observations.size()];
        slot.version.store(sequence * 2 + 1, std::memory_order_seq_cst);
        slot.time.store(at_us, std::memory_order_seq_cst);
        slot.flags.store((low ? 1U : 0U) | (filtered ? 2U : 0U) | (state << 2), std::memory_order_seq_cst);
        slot.version.store(sequence * 2, std::memory_order_seq_cst);
        _published_sequence.store(sequence, std::memory_order_release);
        if (low) {
            // Only a live filtering window needs switching to preservation.
            // A completed reply stays outside; begin_reply checks pending edges
            // and the pin after enabling its own new window.
            std::uint32_t filtering = 1;
            _reply_state.compare_exchange_strong(filtering, 2, std::memory_order_acq_rel);
        }
        else if (filtered) {
            if (_total.fetch_add(1, std::memory_order_relaxed) == 0)
                _first_edge_us.store(at_us, std::memory_order_relaxed);
            _last_edge_us.store(at_us, std::memory_order_relaxed);
            _reply_noise.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        note_cmd_edge(at_us);
    }
    std::array<AtariSioEdgeObservation, 16> edge_snapshot() const noexcept
    {
        std::array<AtariSioEdgeObservation, 16> out{};
        const auto last = _published_sequence.load(std::memory_order_acquire);
        const auto count = last < out.size() ? last : out.size();
        for (std::uint32_t i=0; i<count; ++i) {
            const auto sequence = last - count + 1 + i;
            const auto& slot = _observations[sequence % out.size()];
            const auto before = slot.version.load(std::memory_order_seq_cst);
            const auto time = slot.time.load(std::memory_order_seq_cst);
            const auto flags = slot.flags.load(std::memory_order_seq_cst);
            const auto after = slot.version.load(std::memory_order_seq_cst);
            if (before == sequence * 2 && before == after)
                out[i] = {time, sequence, static_cast<std::uint8_t>(flags)};
        }
        return out;
    }
    std::uint32_t observed_sequence() const noexcept {
        return _published_sequence.load(std::memory_order_acquire);
    }
    bool single_high_after(std::uint32_t sequence) const noexcept {
        const auto last = observed_sequence();
        if (last - sequence != 1) return false;
        const auto& slot = _observations[last % _observations.size()];
        const auto before = slot.version.load(std::memory_order_seq_cst);
        const auto flags = slot.flags.load(std::memory_order_seq_cst);
        const auto after = slot.version.load(std::memory_order_seq_cst);
        return before == last * 2 && before == after && (flags & 3) == 0;
    }
    bool high_pair_after(std::uint32_t sequence) const noexcept {
        const auto last = observed_sequence();
        if (last - sequence != 2) return false;
        const auto& first = _observations[(last - 1) % _observations.size()];
        const auto& second = _observations[last % _observations.size()];
        const auto fv = first.version.load(std::memory_order_seq_cst);
        const auto sv = second.version.load(std::memory_order_seq_cst);
        const auto ff = first.flags.load(std::memory_order_seq_cst);
        const auto sf = second.flags.load(std::memory_order_seq_cst);
        return fv == (last - 1) * 2 && sv == last * 2 && ff == 0 && sf == 0 &&
            first.version.load(std::memory_order_seq_cst) == fv &&
            second.version.load(std::memory_order_seq_cst) == sv && observed_sequence() == last;
    }
    bool idle_high_then_low() const noexcept {
        const auto last = observed_sequence();
        if (last < 2) return false;
        const auto& high = _observations[(last - 1) % _observations.size()];
        const auto& low = _observations[last % _observations.size()];
        const auto hv = high.version.load(std::memory_order_seq_cst);
        const auto lv = low.version.load(std::memory_order_seq_cst);
        const auto hf = high.flags.load(std::memory_order_seq_cst);
        const auto lf = low.flags.load(std::memory_order_seq_cst);
        return hv == (last - 1) * 2 && lv == last * 2 && hf == 0 && lf == 1 &&
            high.version.load(std::memory_order_seq_cst) == hv &&
            low.version.load(std::memory_order_seq_cst) == lv && observed_sequence() == last;
    }
    bool reply_low_pair() const noexcept {
        const auto last = observed_sequence();
        if (last < 2) return false;
        const auto& first = _observations[(last - 1) % _observations.size()];
        const auto& second = _observations[last % _observations.size()];
        const auto fv = first.version.load(std::memory_order_seq_cst);
        const auto sv = second.version.load(std::memory_order_seq_cst);
        const auto ff = first.flags.load(std::memory_order_seq_cst);
        const auto sf = second.flags.load(std::memory_order_seq_cst);
        const auto start = first.time.load(std::memory_order_seq_cst);
        const auto end = second.time.load(std::memory_order_seq_cst);
        // Two low samples in one reply window, shorter than a complete command
        // even at supported maximum HSIO. Never coalesce arbitrary low batches.
        return fv == (last - 1) * 2 && sv == last * 2 && ff == 5 && sf == 9 &&
            end - start <= 200 && first.version.load(std::memory_order_seq_cst) == fv &&
            second.version.load(std::memory_order_seq_cst) == sv && observed_sequence() == last;
    }
    template<class Asserted> void begin_reply(Asserted asserted) noexcept
    {
        _reply_state.store(1, std::memory_order_release);
        // Check after enabling: a racing assertion either marks state 2 in the
        // ISR or leaves a pending edge here. Never clear pending boundaries.
        if (asserted() || _edges.load(std::memory_order_acquire) != 0)
            _reply_state.store(2, std::memory_order_release);
    }
    void end_reply() noexcept { _reply_state.store(0, std::memory_order_release); }
    std::uint32_t reply_noise() const noexcept { return _reply_noise.load(std::memory_order_relaxed); }
    void reset_reply_noise() noexcept { _reply_noise.store(0, std::memory_order_relaxed); }
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
    struct ObservationSlot {
        std::atomic<std::uint32_t> version{0}, time{0}, flags{0};
    };
    std::array<ObservationSlot, 16> _observations{};
    std::uint32_t _edge_sequence{0}; // ISR-owned; not reset by the task
    std::atomic<std::uint32_t> _published_sequence{0};
    std::atomic<std::uint32_t> _reply_state{0}, _reply_noise{0};
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
        advance_flight(static_cast<std::uint32_t>(now()));
        _poll_edges = 0;
        const auto abort = [this, &router, &uart, &boundary, &asserted, &now](std::uint32_t AtariSioCaptureStats::* reason) {
            const bool abort_low = asserted();
            const bool idle_edge = reason == &AtariSioCaptureStats::aborts_edge &&
                                   !_collecting && !router.pending_write_bytes();
            const bool live = !idle_edge || _stats.trace_count == 0 ||
                _stats.trace[(_stats.trace_count - 1) % _stats.trace.size()].event != 6;
            {
                AtariSioCaptureStats::Trace t{};
                t.time_us = static_cast<std::uint32_t>(now());
                t.edges = boundary.total_edges();
                t.frame = _frame;
                t.pending = static_cast<std::uint16_t>(router.pending_write_bytes());
                t.event = idle_edge ? 6 : reason == &AtariSioCaptureStats::aborts_edge ? 2 :
                          reason == &AtariSioCaptureStats::aborts_timeout ? 3 :
                          reason == &AtariSioCaptureStats::aborts_overflow ? 4 : 5;
                t.used = static_cast<std::uint8_t>(_frame_used);
                t.low = abort_low; t.collecting = _collecting; t.released = _released;
                t.last_edge_us = boundary.last_edge_us();
                t.edge_batch = _poll_edges;
                t.poll_gap_us = _poll_gap_us;
                const bool triggered = _stats.flight_triggered;
                record_trace(t, live);
                if (!triggered && _stats.flight_triggered) _flight->trigger_edges = boundary.edge_snapshot();
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
            // A lone idle-high observation is already released. Waiting for an
            // additional edge-free idle poll can swallow the next assertion
            // when the task sleeps until that assertion wakes it (min-010).
            // Active/ambiguous aborts still require the existing resync barrier.
            _discard_until_release = !(idle_edge && !abort_low && _poll_edges == 1);
        };
        if (_write_started_us && now() - _write_started_us > 200000) {
            abort(&AtariSioCaptureStats::aborts_timeout);
            return;
        }
        std::uint32_t edges = boundary.consume();
        _poll_edges = edges;
        const bool low = asserted();
        if (edges) trace_boundary(10, now(), boundary, low, edges);
        if (_ignore_write_glitches && edges != 0 && !low && !_collecting && router.pending_write_bytes()) {
            _stats.write_glitches_ignored += edges;
            edges = 0;
        }
        if (released_high_noise(boundary, edges, low)) {
            trace_boundary(14, now(), boundary, low, edges);
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
        // In idle state, a high observation cannot start a command. Preserve
        // the following assertion only when the ISR history proves that exact
        // two-observation sequence, both outside a reply and unfiltered.
        if (edges == 2 && low && !_collecting && !router.pending_write_bytes() &&
            boundary.idle_high_then_low()) {
            trace_boundary(15, now(), boundary, low, edges);
            edges = 1;
        }
        if (edges == 2 && low && !_collecting && !router.pending_write_bytes() &&
            boundary.reply_low_pair()) {
            trace_boundary(16, now(), boundary, low, edges);
            edges = 1;
        }
        // Equivalent to processing release then event-14 noise in two polls.
        // Require the entire ISR sequence since this command's assertion:
        // two unfiltered highs, with no intervening low or missing history.
        if (edges == 2 && !low && _collecting && !_released &&
            boundary.high_pair_after(_release_irq_sequence)) {
            trace_boundary(18, now(), boundary, low, edges);
            edges = 1;
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
            _release_irq_sequence = boundary.observed_sequence();
            _started_us = now();
            trace_boundary(11, _started_us, boundary, low, edges);
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
            _poll_edges += seen;
            const bool low_now = asserted();
            if (seen) trace_boundary(10, now(), boundary, low_now, seen);
            if (_ignore_write_glitches && seen != 0 && !low_now && !low && !_collecting &&
                router.pending_write_bytes()) {
                _stats.write_glitches_ignored += seen;
                seen = 0;
            }
            if (!rx_error && !low && released_high_noise(boundary, seen, low_now)) {
                trace_boundary(14, now(), boundary, low_now, seen);
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
        if (_collecting && edges == 1 && !low) {
            _released = true;
            _release_irq_sequence = boundary.observed_sequence();
            trace_boundary(12, now(), boundary, low, edges);
        }
        if (_collecting && _released && _frame_used == _frame.size()) {
            auto final_edges = boundary.consume();
            _poll_edges += final_edges;
            if (final_edges) trace_boundary(10, now(), boundary, asserted(), final_edges);
            if (released_high_noise(boundary, final_edges, asserted())) {
                trace_boundary(14, now(), boundary, false, final_edges);
                final_edges = 0;
            }
            if (final_edges != 0 || asserted()) {
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
    void reset_stats() {
        // Allocate before replacing the recorder, so old frozen readers retain
        // a stable image. Allocation never occurs in the hot recording path.
        auto fresh = std::make_shared<AtariSioFlightTrace>();
        _stats = {}; _flight = std::move(fresh); _poll_gap_us = 0;
        _flight_started = false; _flight_start_us = 0; _stream_seen = false;
    }
    void trace_poll_gap(std::uint32_t time, std::uint32_t gap,
                        const AtariSioTransactionBoundary& boundary, bool low, std::uint32_t batch = 0) noexcept {
        _poll_gap_us = gap;
        advance_flight(time);
        if (gap < 16000) return;
        AtariSioCaptureStats::Trace t{};
        t.time_us=time; t.event=8; t.poll_gap_us=gap; t.edge_batch=batch;
        t.edges=boundary.total_edges(); t.last_edge_us=boundary.last_edge_us();
        t.low=low; t.collecting=_collecting; t.released=_released;
        const bool triggered = _stats.flight_triggered;
        record_trace(t);
        if (!triggered && _stats.flight_triggered) _flight->trigger_edges = boundary.edge_snapshot();
    }
    // Metadata only: never retain a response body in diagnostics.
    void trace_reply(std::uint32_t time, std::uint32_t duration, std::uint32_t baud,
                     std::uint16_t length, std::uint8_t first, std::uint8_t second, bool early = false) noexcept
    {
        AtariSioCaptureStats::Trace t{};
        t = {};
        t.time_us = time; t.duration_us = duration; t.baud = baud;
        t.event = early ? 9 : 7; t.pending = length; t.frame[0] = first; t.frame[1] = second;
        record_trace(t);
    }
    // Command frames in a row that could not be read. The ingress uses it to leave high-speed SIO.
    std::uint32_t bad_frames_in_a_row() const noexcept { return _bad_frames_in_a_row; }
    void clear_bad_frames() noexcept { _bad_frames_in_a_row = 0; }
private:
    std::uint32_t _release_irq_sequence{0}, _stream_last_us{0};
    bool _stream_seen{false};
    bool released_high_noise(const AtariSioTransactionBoundary& boundary,
                             std::uint32_t edges, bool low) noexcept {
        if (!_collecting || !_released || low || edges != 1 ||
            !boundary.single_high_after(_release_irq_sequence)) return false;
        _release_irq_sequence = boundary.observed_sequence();
        return true;
    }
    std::shared_ptr<AtariSioFlightTrace> _flight = std::make_shared<AtariSioFlightTrace>();
    std::uint32_t _poll_edges{0}, _poll_gap_us{0};
    bool _flight_started{false};
    std::uint32_t _flight_start_us{0};
    void advance_flight(std::uint32_t time) noexcept {
        if (!_flight_started) { _flight_started = true; _flight_start_us = time; }
        if (!_stats.flight_armed && time - _flight_start_us >= 2000000 && _flight->count >= 32)
            _stats.flight_armed = true;
        finish_flight(time);
    }
    // Poll-observed edge batches, not individual GPIO transitions. Never log in the ISR.
    void trace_boundary(std::uint8_t event, std::uint32_t time,
                        const AtariSioTransactionBoundary& boundary, bool low, std::uint32_t batch) noexcept {
        AtariSioCaptureStats::Trace t{};
        t.time_us=time; t.event=event; t.edges=boundary.total_edges();
        t.last_edge_us=boundary.last_edge_us(); t.edge_batch=batch; t.low=low;
        t.collecting=_collecting; t.released=_released; t.used=_frame_used;
        t.poll_gap_us=_poll_gap_us;
        record_trace(t, false);
    }
    void finish_flight(std::uint32_t time) noexcept {
        if (_stats.flight_triggered && !_stats.flight &&
            (time - _stats.flight_trigger_us >= 1000000 ||
             _flight->count - _flight->trigger_count >= 16))
            _stats.flight = _flight;
    }
    void record_trace(const AtariSioCaptureStats::Trace& t, bool live = true) noexcept {
        if (live) _stats.trace[_stats.trace_count++ % _stats.trace.size()] = t;
        advance_flight(t.time_us);
        if (_stats.flight) return;
        _flight->entries[_flight->count++ % _flight->entries.size()] = t;
        const bool failure = (t.event >= 2 && t.event <= 5) ||
            (t.event == 6 && (t.low || t.edge_batch > 1)) || t.event == 8 || t.event == 17;
        if (_stats.flight_armed && !_stats.flight_triggered && failure) {
            _stats.flight_triggered = true;
            _stats.flight_trigger_us = t.time_us;
            _stats.flight_trigger_event = t.event;
            _flight->trigger_count = _flight->count;
        }
        finish_flight(t.time_us);
    }
    // Hands the complete command frame in `_frame` to the router and starts timing its reply.
    template<class Router, class Now>
    void dispatch_frame(AtariSioTransactionBoundary& boundary, Router& router, Now now)
    {
        AtariSioCaptureStats::Trace t{};
        t = {};
        t.time_us = static_cast<std::uint32_t>(now());
        t.edges = boundary.total_edges();
        t.frame = _frame;
        t.event = 1;
        t.used = 5;
        t.last_edge_us = boundary.last_edge_us(); t.edge_batch = _poll_edges;
        t.poll_gap_us = _poll_gap_us;
        const bool stream = _frame[0] == 0x7f && (_frame[1] == 'S' || _frame[1] == 'T');
        if (stream && _stream_seen) {
            const auto gap = t.time_us - _stream_last_us;
            if (gap >= 200000 && gap <= 1000000) {
                auto delayed = t;
                delayed.event = 17; delayed.duration_us = gap;
                const bool triggered = _stats.flight_triggered;
                record_trace(delayed);
                if (!triggered && _stats.flight_triggered) _flight->trigger_edges = boundary.edge_snapshot();
            }
        }
        _stream_seen = stream;
        _stream_last_us = t.time_us;
        record_trace(t);
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
