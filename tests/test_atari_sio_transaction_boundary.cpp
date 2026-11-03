#include "doctest.h"

#include "fujinet/io/atari_sio_transaction_boundary.h"

TEST_CASE("Atari SIO CMD edge records one host-testable transaction boundary")
{
    fujinet::io::AtariSioTransactionBoundary boundary;
    CHECK_FALSE(boundary.consume());
    boundary.note_cmd_edge();
    CHECK(boundary.consume());
    CHECK_FALSE(boundary.consume());
    boundary.note_cmd_edge();
    boundary.note_cmd_edge();
    CHECK(boundary.consume() == 2);
    CHECK_FALSE(boundary.consume());
}

TEST_CASE("Atari SIO reply trace records only control metadata")
{
    fujinet::io::AtariSioCapture capture;
    capture.trace_reply(100, 2100, 124017, 22, 'A', 'C');
    const auto& stats = capture.stats();
    REQUIRE(stats.trace_count == 1);
    const auto& t = stats.trace[0];
    CHECK(t.event == 7);
    CHECK(t.time_us == 100);
    CHECK(t.duration_us == 2100);
    CHECK(t.baud == 124017);
    CHECK(t.pending == 22);
    CHECK(t.frame[0] == 'A');
    CHECK(t.frame[1] == 'C');
    CHECK(t.frame[2] == 0);
}

#include <algorithm>
#include <vector>

namespace {
struct CaptureUart {
    std::vector<std::uint8_t> software, driver;
    std::uint32_t errors = 0;
    bool available() {
        software.insert(software.end(), driver.begin(), driver.end());
        driver.clear();
        return !software.empty();
    }
    std::size_t read(std::uint8_t* out, std::size_t size) {
        auto n = std::min(size, software.size());
        std::copy_n(software.begin(), n, out);
        software.erase(software.begin(), software.begin() + n);
        return n;
    }
    void discardInput() { software.clear(); driver.clear(); }
    std::uint32_t rx_errors() const { return errors; }
};
struct CaptureRouter {
    std::vector<std::uint8_t> command, data;
    std::size_t pending = 0;
    std::size_t write_size = 2;
    void reset_command_collection() { command.clear(); pending = 0; }
    std::size_t pending_write_bytes() const { return pending; }
    void consume(std::uint8_t byte) {
        if (pending) { data.push_back(byte); if (data.size() == write_size + 1) pending = 0; }
        else { command.push_back(byte); if (command.size() == 5 && command[0] == 0x31) pending = write_size; }
    }
};
}

TEST_CASE("Atari SIO capture gates commands on release and separates write data")
{
    fujinet::io::AtariSioCapture capture;
    fujinet::io::AtariSioTransactionBoundary boundary;
    CaptureUart uart;
    CaptureRouter router;
    bool low = true;
    std::int64_t time = 1;
    auto poll = [&] { capture.poll(boundary, router, uart, [&] { return low; }, [&] { return time; }); };
    boundary.note_cmd_edge();
    uart.driver = {0x31, 1, 2};
    poll();
    CHECK(router.command.empty());
    uart.software = {3};
    uart.driver = {4};
    poll();
    CHECK(router.command.empty());
    low = false;
    boundary.note_cmd_edge();
    poll();
    CHECK((router.command == std::vector<std::uint8_t>{0x31, 1, 2, 3, 4}));
    REQUIRE(capture.stats().trace_count == 1);
    CHECK(capture.stats().trace[0].event == 1);
    CHECK((capture.stats().trace[0].frame == std::array<std::uint8_t, 5>{0x31, 1, 2, 3, 4}));
    CHECK(router.pending == 2);
    uart.driver = {10, 11, 12};
    poll();
    CHECK((router.data == std::vector<std::uint8_t>{10, 11, 12}));
    CHECK(router.pending == 0);
}

TEST_CASE("Atari SIO capture drops observed ambiguous edges and recovers")
{
    fujinet::io::AtariSioCapture capture;
    fujinet::io::AtariSioTransactionBoundary boundary;
    CaptureUart uart;
    CaptureRouter router;
    bool low = false;
    std::int64_t time = 1;
    auto poll = [&] { capture.poll(boundary, router, uart, [&] { return low; }, [&] { return time; }); };
    uart.software = {0x31}; uart.driver = {1, 2, 3, 4};
    boundary.note_cmd_edge(); boundary.note_cmd_edge();
    poll();
    CHECK(router.command.empty());
    CHECK(uart.software.empty()); CHECK(uart.driver.empty());
    CHECK(capture.stats().trace_count == 1);
    CHECK(capture.stats().trace[0].event == 6);
    poll();
    boundary.note_cmd_edge(); boundary.note_cmd_edge(); poll();
    CHECK(capture.stats().trace_count == 1);
    poll();
    low = true; boundary.note_cmd_edge();
    uart.driver = {0x31, 1, 2, 3, 4}; poll();
    CHECK(router.command.empty());
    low = false; boundary.note_cmd_edge(); poll();
    CHECK(router.command.size() == 5);
}

TEST_CASE("Atari SIO capture aborts overflow and partial frames and discards stray input")
{
    fujinet::io::AtariSioCapture capture;
    fujinet::io::AtariSioTransactionBoundary boundary;
    CaptureUart uart;
    CaptureRouter router;
    bool low = false;
    std::int64_t time = 1;
    auto poll = [&] { capture.poll(boundary, router, uart, [&] { return low; }, [&] { return time; }); };
    uart.driver = {0x31, 1, 2, 3, 4}; poll();
    CHECK(router.command.empty());
    low = true; boundary.note_cmd_edge(); uart.driver = {0x31, 1}; poll();
    ++uart.errors; poll();
    low = false; boundary.note_cmd_edge(); poll(); poll();
    CHECK(router.command.empty());
    low = true; boundary.note_cmd_edge(); uart.driver = {0x31, 1}; poll();
    low = false; boundary.note_cmd_edge(); poll();
    time += 50001; poll(); poll();
    CHECK(router.command.empty());
    low = true; boundary.note_cmd_edge(); uart.driver = {0x31, 1, 2, 3, 4}; poll();
    low = false; boundary.note_cmd_edge(); poll();
    CHECK(router.command.size() == 5);
}

TEST_CASE("Atari SIO capture rejects excess command bytes and aborts a write on assertion")
{
    fujinet::io::AtariSioCapture capture;
    fujinet::io::AtariSioTransactionBoundary boundary;
    CaptureUart uart;
    CaptureRouter router;
    bool low = true;
    std::int64_t time = 1;
    auto poll = [&] { capture.poll(boundary, router, uart, [&] { return low; }, [&] { return time; }); };
    boundary.note_cmd_edge(); uart.driver = {0x31, 1, 2, 3, 4, 5}; poll();
    CHECK(router.command.empty());
    low = false; boundary.note_cmd_edge(); poll(); poll();
    low = true; boundary.note_cmd_edge(); uart.driver = {0x31, 1, 2, 3, 4}; poll();
    low = false; boundary.note_cmd_edge(); poll();
    CHECK(router.pending == 2);
    low = true; boundary.note_cmd_edge(); uart.driver = {0x32, 1, 2, 3, 4}; poll();
    CHECK(router.pending == 0);
    CHECK(router.data.empty());
    low = false; boundary.note_cmd_edge(); poll();
    CHECK(router.command.size() == 5);
    CHECK(router.command[0] == 0x32);
}

TEST_CASE("Atari SIO capture rejects an error seen before assertion poll")
{
    fujinet::io::AtariSioCapture capture;
    fujinet::io::AtariSioTransactionBoundary boundary;
    CaptureUart uart; CaptureRouter router;
    bool low = false; std::int64_t time = 1;
    auto poll = [&] { capture.poll(boundary, router, uart, [&] { return low; }, [&] { return time; }); };
    poll();
    ++uart.errors;
    low = true; boundary.note_cmd_edge(); uart.driver = {0x31, 1, 2, 3, 4}; poll();
    low = false; boundary.note_cmd_edge(); poll(); poll();
    CHECK(router.command.empty());
}

TEST_CASE("Atari SIO capture waits for final buffered byte after release")
{
    fujinet::io::AtariSioCapture capture;
    fujinet::io::AtariSioTransactionBoundary boundary;
    CaptureUart uart; CaptureRouter router;
    bool low = true; std::int64_t time = 1;
    auto poll = [&] { capture.poll(boundary, router, uart, [&] { return low; }, [&] { return time; }); };
    boundary.note_cmd_edge(); uart.driver = {0x31, 1, 2, 3}; poll();
    low = false; boundary.note_cmd_edge(); poll();
    CHECK(router.command.empty());
    CHECK(capture.needs_immediate_poll());
    uart.driver = {4}; poll();
    CHECK(router.command.size() == 5);
}

TEST_CASE("Atari SIO capture polls without sleeping while a write waits for its data frame")
{
    fujinet::io::AtariSioCapture capture;
    fujinet::io::AtariSioTransactionBoundary boundary;
    CaptureUart uart; CaptureRouter router;
    bool low = true; std::int64_t time = 1;
    auto poll = [&] { capture.poll(boundary, router, uart, [&] { return low; }, [&] { return time; }); };
    CHECK_FALSE(capture.needs_immediate_poll());                   // idle: the loop may sleep
    boundary.note_cmd_edge(); uart.driver = {0x31, 'W', 0, 0, 0x88}; poll();
    low = false; boundary.note_cmd_edge(); poll();                 // the write command was accepted
    REQUIRE(router.pending == 2);
    CHECK(capture.needs_immediate_poll());                         // waiting for the data frame
    uart.driver = {10, 11}; poll();                                // part of it
    CHECK(capture.needs_immediate_poll());
    uart.driver = {12}; poll();                                    // the checksum byte: done
    CHECK(router.pending == 0);
    CHECK_FALSE(capture.needs_immediate_poll());
}

TEST_CASE("Atari SIO capture extends write deadline on byte progress")
{
    fujinet::io::AtariSioCapture capture;
    fujinet::io::AtariSioTransactionBoundary boundary;
    CaptureUart uart; CaptureRouter router;
    bool low = true; std::int64_t time = 1;
    auto poll = [&] { capture.poll(boundary, router, uart, [&] { return low; }, [&] { return time; }); };
    boundary.note_cmd_edge(); uart.driver = {0x31, 1, 2, 3, 4}; poll();
    low = false; boundary.note_cmd_edge(); poll();
    time += 150000; uart.driver = {1}; poll();
    time += 150000; uart.driver = {2, 3}; poll();
    CHECK(router.data.size() == 3);
    CHECK(router.pending == 0);
}

TEST_CASE("Atari SIO capture accepts a 512-byte write longer than its idle timeout")
{
    fujinet::io::AtariSioCapture capture;
    fujinet::io::AtariSioTransactionBoundary boundary;
    CaptureUart uart; CaptureRouter router; router.write_size = 512;
    bool low = true; std::int64_t time = 1;
    auto poll = [&] { capture.poll(boundary, router, uart, [&] { return low; }, [&] { return time; }); };
    boundary.note_cmd_edge(); uart.driver = {0x31, 1, 2, 3, 4}; poll();
    low = false; boundary.note_cmd_edge(); poll();
    for (int i = 0; i < 32; ++i) {
        time += 8333; uart.driver.assign(16, 0x55); poll();
    }
    time += 521; uart.driver = {0}; poll();
    CHECK(router.data.size() == 513);
    CHECK(router.pending == 0);
}

TEST_CASE("Atari SIO boundary counts every edge for diagnostics, apart from the consumed ones")
{
    fujinet::io::AtariSioTransactionBoundary boundary;
    CHECK(boundary.total_edges() == 0);
    boundary.note_cmd_edge();
    boundary.note_cmd_edge();
    CHECK(boundary.consume() == 2);
    CHECK(boundary.total_edges() == 2);   // consume() does not take them away
    boundary.note_cmd_edge();
    CHECK(boundary.total_edges() == 3);
    boundary.reset_total_edges();
    CHECK(boundary.total_edges() == 0);
    CHECK(boundary.consume() == 1);       // and resetting the count does not lose a pending edge
}

TEST_CASE("Atari SIO capture counts bytes, frames and each kind of abort")
{
    using fujinet::io::AtariSioCaptureStats;
    fujinet::io::AtariSioCapture capture;
    fujinet::io::AtariSioTransactionBoundary boundary;
    CaptureUart uart;
    CaptureRouter router;
    bool low = false;
    std::int64_t time = 1;
    auto poll = [&] { capture.poll(boundary, router, uart, [&] { return low; }, [&] { return time; }); };

    // A good frame: CMD low, five bytes, CMD high.
    low = true; boundary.note_cmd_edge(); uart.driver = {0x31, 1, 2, 3, 4}; poll();
    low = false; boundary.note_cmd_edge(); poll();
    CHECK(capture.stats().rx_bytes == 5);
    CHECK(capture.stats().frames == 1);
    CHECK(capture.stats().aborts_edge == 0);

    // Two edges between polls cannot be told from noise: an edge abort.
    low = false; boundary.note_cmd_edge(); boundary.note_cmd_edge(); poll();
    CHECK(capture.stats().aborts_edge == 1);
    CHECK(capture.stats().frames_lost == 0);   // no frame was being received
    poll();   // release the discard

    // A UART error during a frame.
    low = true; boundary.note_cmd_edge(); uart.driver = {0x31, 1}; poll();
    ++uart.errors; poll();
    CHECK(capture.stats().aborts_rx_error == 1);
    CHECK(capture.stats().frames_lost == 1);
    low = false; boundary.note_cmd_edge(); poll(); poll();

    // A frame that never completes.
    low = true; boundary.note_cmd_edge(); uart.driver = {0x31, 1}; poll();
    time += 50001; poll();
    CHECK(capture.stats().aborts_timeout == 1);
    CHECK(capture.stats().frames_lost == 2);
    low = false; boundary.note_cmd_edge(); poll(); poll();

    // More command bytes than a frame holds.
    low = true; boundary.note_cmd_edge(); uart.driver = {0x31, 1, 2, 3, 4, 5, 6}; poll();
    CHECK(capture.stats().aborts_overflow == 1);
    CHECK(capture.stats().frames_lost == 3);

    capture.reset_stats();
    const AtariSioCaptureStats& zero = capture.stats();
    CHECK(zero.rx_bytes == 0);
    CHECK(zero.frames == 0);
    CHECK(zero.frames_lost == 0);
    CHECK(zero.aborts_edge + zero.aborts_timeout + zero.aborts_overflow + zero.aborts_rx_error == 0);
}

TEST_CASE("Atari SIO boundary keeps the first and last CMD edge times for diagnostics")
{
    fujinet::io::AtariSioTransactionBoundary boundary;
    CHECK(boundary.first_edge_us() == 0);
    boundary.note_cmd_edge(1000);
    boundary.note_cmd_edge(1400);
    CHECK(boundary.first_edge_us() == 1000);
    CHECK(boundary.last_edge_us() == 1400);
    CHECK(boundary.consume() == 2);       // consuming edges for the state machine keeps the record
    CHECK(boundary.first_edge_us() == 1000);
    boundary.reset_total_edges();
    CHECK(boundary.first_edge_us() == 0);
    boundary.note_cmd_edge(5000);         // the first edge after a reset is the new first
    CHECK(boundary.first_edge_us() == 5000);
}

TEST_CASE("Atari SIO capture times the first frame, the reaction lag, and what it is processing")
{
    fujinet::io::AtariSioCapture capture;
    fujinet::io::AtariSioTransactionBoundary boundary;
    CaptureUart uart;
    CaptureRouter router;
    bool low = false;
    std::int64_t time = 0;
    auto poll = [&] { capture.poll(boundary, router, uart, [&] { return low; }, [&] { return time; }); };

    // CMD asserted at 1000 us, the frame arrives, CMD released at 2000 us, and the loop gets to it at 2500 us.
    low = true; time = 1000; boundary.note_cmd_edge(1000); uart.driver = {0x31, 1, 2, 3, 4}; poll();
    low = false; time = 2000; boundary.note_cmd_edge(2000);
    time = 2500; poll();
    CHECK(capture.stats().frames == 1);
    CHECK(capture.stats().first_frame_us == 2500);
    CHECK(capture.stats().lag_us_max == 500);
    CHECK(capture.processing_command());
    CHECK(capture.processing_start_us() == 2500);

    // A write's data frame is processed later and is not a command.
    time = 9000; uart.driver = {10, 11, 12}; poll();
    CHECK_FALSE(capture.processing_command());
    CHECK(capture.processing_start_us() == 9000);

    // The next command: the first frame stays, the worst lag is kept.
    low = true; time = 10000; boundary.note_cmd_edge(10000); uart.driver = {0x31, 1, 2, 3, 4}; poll();
    low = false; time = 10100; boundary.note_cmd_edge(10100);
    time = 10300; poll();
    CHECK(capture.stats().frames == 2);
    CHECK(capture.stats().first_frame_us == 2500);
    CHECK(capture.stats().lag_us_max == 500);   // 200 us this time, 500 us is still the worst
}

TEST_CASE("Atari SIO reply timing measures the ACK against the CMD release and the handler against its start")
{
    fujinet::io::AtariSioReplyStats reply;
    reply.ready_us = 2'235'000;

    // A command: released at 100 ms, processing began at 100.4 ms, the ACK started at 101.2 ms.
    reply.record(101'200, 100'400, true, 100'000);
    CHECK(reply.replies == 1);
    CHECK(reply.reply_us_last == 1200);
    CHECK(reply.handle_us_last == 800);
    CHECK(reply.reply_over_deadline == 0);

    // A data frame's reply is a handler time only: it has no CMD release to measure from.
    reply.record(500'000, 100'000, false, 0);
    CHECK(reply.handle_us_max == 400'000);
    CHECK(reply.reply_us_last == 1200);        // unchanged by a non-command reply
    CHECK(reply.reply_us_max == 1200);

    // A late ACK is counted against the 16 ms deadline.
    reply.record(120'000, 119'000, true, 100'000);
    CHECK(reply.reply_us_last == 20'000);
    CHECK(reply.reply_us_max == 20'000);
    CHECK(reply.reply_over_deadline == 1);

    // A stray CMD edge stamped after the reply started is not the release: counted as 0, not ~71 min.
    reply.record(130'000, 129'000, true, 130'100);
    CHECK(reply.reply_us_last == 0);
    CHECK(reply.reply_us_max == 20'000);
    CHECK(reply.reply_over_deadline == 1);

    reply.reset();
    CHECK(reply.replies == 0);
    CHECK(reply.reply_us_max == 0);
    CHECK(reply.ready_us == 2'235'000);        // when the ingress came up is not a counter
}

TEST_CASE("Atari SIO reply timing survives the 32-bit microsecond clock wrapping")
{
    fujinet::io::AtariSioReplyStats reply;
    // Released 200 us before the clock wraps; the ACK starts 1000 us after the wrap.
    reply.record(1000, 4'294'967'000u, true, 4'294'967'096u);
    CHECK(reply.reply_us_last == 1200);
    CHECK(reply.handle_us_last == 1296);
}

TEST_CASE("Atari SIO capture counts command frames that cannot be read, for the high-speed fallback")
{
    fujinet::io::AtariSioCapture capture;
    fujinet::io::AtariSioTransactionBoundary boundary;
    CaptureUart uart;
    CaptureRouter router;
    bool low = false;
    std::int64_t time = 1;
    auto poll = [&] { capture.poll(boundary, router, uart, [&] { return low; }, [&] { return time; }); };
    const auto frame = [&](std::vector<std::uint8_t> bytes) {
        low = true; boundary.note_cmd_edge(); uart.driver = std::move(bytes); poll();
        low = false; boundary.note_cmd_edge(); poll();
    };

    frame({0x31, 'S', 0, 0, 0x84});            // a good frame
    CHECK(capture.bad_frames_in_a_row() == 0);
    CHECK(capture.stats().frames_bad == 0);

    frame({0x31, 'S', 0, 0, 0x00});            // bad checksum
    CHECK(capture.bad_frames_in_a_row() == 1);
    frame({0x31, 'S', 0, 0, 0x01});
    CHECK(capture.bad_frames_in_a_row() == 2);
    CHECK(capture.stats().frames_bad == 2);
    using Reason = fujinet::io::AtariSioBadFrame::Reason;
    CHECK(capture.stats().first_bad.reason == Reason::Checksum);
    CHECK(capture.stats().first_bad.length == 5);
    CHECK(capture.stats().first_bad.bytes[4] == 0x00);
    CHECK(capture.stats().last_bad.bytes[4] == 0x01);

    frame({0x31, 'S', 0, 0, 0x84});            // a good one ends the run
    CHECK(capture.bad_frames_in_a_row() == 0);

    // A UART error while the command is collected, and too many bytes: what the wrong speed looks like.
    low = true; boundary.note_cmd_edge(); uart.driver = {0x31, 'S'}; poll();
    ++uart.errors; poll();
    CHECK(capture.bad_frames_in_a_row() == 1);
    low = false; boundary.note_cmd_edge(); poll(); poll();
    low = true; boundary.note_cmd_edge(); uart.driver = {1, 2, 3, 4, 5, 6, 7}; poll();
    CHECK(capture.bad_frames_in_a_row() == 2);
    low = false; boundary.note_cmd_edge(); poll(); poll();

    // A frame that ends after a byte or two (a faster frame read by a slower UART) counts too.
    capture.clear_bad_frames();
    low = true; boundary.note_cmd_edge(); uart.driver = {0x38}; poll();
    low = false; boundary.note_cmd_edge(); poll();
    low = true; boundary.note_cmd_edge(); poll();
    CHECK(capture.bad_frames_in_a_row() == 1);
    CHECK(capture.stats().last_bad.reason == fujinet::io::AtariSioBadFrame::Reason::Short);
    low = false; boundary.note_cmd_edge(); poll(); poll();

    // Noise is not a speed change: edge and timeout aborts do not count.
    capture.clear_bad_frames();
    low = false; boundary.note_cmd_edge(); boundary.note_cmd_edge(); poll();
    CHECK(capture.bad_frames_in_a_row() == 0);
    poll();
    low = true; boundary.note_cmd_edge(); uart.driver = {0x31, 1}; poll();
    time += 50001; poll();
    CHECK(capture.bad_frames_in_a_row() == 0);
}

TEST_CASE("Atari SIO capture takes the frame a second listener decoded when the main UART could not read it")
{
    fujinet::io::AtariSioCapture capture;
    fujinet::io::AtariSioTransactionBoundary boundary;
    CaptureUart uart;
    CaptureRouter router;
    bool low = false;
    std::int64_t time = 1;
    bool alt_ready = false;
    int alt_taken = 0;
    const std::array<std::uint8_t, 5> heard{0x7f, 0x51, 0, 0, 0xd0};
    auto poll = [&] {
        capture.poll(boundary, router, uart, [&] { return low; }, [&] { return time; },
                     [&](std::array<std::uint8_t, 5>& frame, bool unambiguous) {
                         if (!alt_ready) return false;
                         alt_ready = false;
                         if (!unambiguous) return false;
                         ++alt_taken;
                         frame = heard;
                         return true;
                     });
    };

    // The main UART read stretched garbage: more than 5 bytes while CMD was low (an overflow abort).
    low = true; boundary.note_cmd_edge();
    uart.driver = {0xf8, 0x38, 0x00, 0x38, 0x00, 0x00, 0x38};
    poll();
    CHECK(router.command.empty());
    // CMD released; the second listener has the real frame. It is handled, once, and counted.
    low = false; boundary.note_cmd_edge(42);
    alt_ready = true;
    poll();
    CHECK((router.command == std::vector<std::uint8_t>{0x7f, 0x51, 0, 0, 0xd0}));
    CHECK(alt_taken == 1);
    CHECK(capture.stats().frames_from_second_listener == 1);
    CHECK(capture.processing_release_us() == 42);
    boundary.note_cmd_edge(100); // later activity must not rewrite the command's timestamp
    CHECK(capture.processing_release_us() == 42);
    boundary.consume();
    CHECK(capture.stats().first_alt_decisions[0].frame == heard);
    CHECK(capture.stats().first_alt_decisions[0].edges == 1);
    CHECK_FALSE(capture.stats().first_alt_decisions[0].collecting);
    CHECK(capture.bad_frames_in_a_row() == 0);

    // The next command is read by the main UART as usual, and the listener is not asked for it again.
    router.command.clear();
    low = true; boundary.note_cmd_edge();
    uart.driver = {0x31, 'S', 0, 0, 0x84};
    poll();
    low = false; boundary.note_cmd_edge();
    poll();
    CHECK((router.command == std::vector<std::uint8_t>{0x31, 'S', 0, 0, 0x84}));
    CHECK(alt_taken == 1);
    CHECK(capture.stats().frames_from_second_listener == 1);
    capture.reset_stats();
    CHECK(capture.stats().frames_from_second_listener == 0);
    CHECK(capture.stats().first_alt_decisions[0].frame[0] == 0);
}

TEST_CASE("Atari SIO discards an alternate frame after ambiguous CMD edges")
{
    fujinet::io::AtariSioCapture capture;
    fujinet::io::AtariSioTransactionBoundary boundary;
    CaptureUart uart;
    CaptureRouter router;
    bool pending = true;
    int toggles = 0;
    boundary.note_cmd_edge();
    boundary.note_cmd_edge();
    auto alternate = [&](std::array<std::uint8_t, 5>& frame, bool unambiguous) {
        if (!pending) return false;
        pending = false;
        if (!unambiguous) return false;
        ++toggles;
        frame = {0x31, 'R', 0x1c, 0, 0x9f};
        return true;
    };
    auto poll = [&] { capture.poll(boundary, router, uart, [] { return false; }, [] { return 1; }, alternate); };
    poll();
    CHECK_FALSE(pending);
    CHECK(toggles == 0);
    CHECK(router.command.empty());
    CHECK(capture.stats().frames_from_second_listener == 0);
    poll();
    CHECK(router.command.empty());
}

TEST_CASE("Atari SIO per-command time splits our service time from the Atari's idle time")
{
    fujinet::io::AtariSioTxnStats txn;
    txn.begin('W', 1000);       // the first command only opens
    txn.end(1500);
    txn.begin('P', 3000);       // W: served 1000..1500, then the Atari took until 3000
    txn.end(3200);
    txn.begin('R', 4000);       // P: served 3000..3200, idle 800
    txn.end(9000);
    txn.begin('W', 12000);      // R: served 4000..9000, idle 3000
    CHECK(txn.entry[1].count == 1);
    CHECK(txn.entry[1].service_us == 500);
    CHECK(txn.entry[1].idle_us == 1500);
    CHECK(txn.entry[2].service_us == 200);
    CHECK(txn.entry[2].idle_us == 800);
    CHECK(txn.entry[3].service_us == 5000);
    CHECK(txn.entry[3].idle_us == 3000);

    // A pause of seconds (the user at the keyboard) is not protocol time.
    txn.end(12100);
    txn.begin('Q', 12100 + 5'000'000);
    CHECK(txn.entry[1].count == 1);
    // Another device's commands go in the last bucket.
    CHECK(fujinet::io::AtariSioTxnStats::bucket_for(0x3f) == 4);
}

namespace {
struct GlitchUart {
    std::vector<std::uint8_t> data;
    bool available() const { return !data.empty(); }
    std::size_t read(std::uint8_t* out, std::size_t cap)
    {
        const auto n = std::min(cap, data.size());
        std::copy(data.begin(), data.begin() + static_cast<std::ptrdiff_t>(n), out);
        data.erase(data.begin(), data.begin() + static_cast<std::ptrdiff_t>(n));
        return n;
    }
    std::uint32_t rx_errors() const { return 0; }
    void discardInput() { data.clear(); }
};
struct GlitchRouter {
    std::size_t pending{0};
    std::size_t pending_write_bytes() const { return pending; }
    void reset_command_collection() { pending = 0; }
    void consume(std::uint8_t) {}
};
} // namespace

TEST_CASE("Atari SIO capture can ignore CMD glitches while a write waits for its data")
{
    for (const bool filter : {false, true}) {
        fujinet::io::AtariSioTransactionBoundary boundary;
        fujinet::io::AtariSioCapture capture;
        capture.set_ignore_write_glitches(filter);
        GlitchRouter router;
        GlitchUart uart;
        std::int64_t t = 1'000'000;
        const auto asserted = [] { return false; };    // CMD reads high
        const auto now = [&t] { return t; };

        // The write was accepted by a command frame: the router waits for 8 data bytes.
        router.pending = 8;
        capture.poll(boundary, router, uart, asserted, now);
        CHECK(capture.needs_immediate_poll() == false);   // no write start time yet in this harness
        // A glitch: two CMD edges while the line is high, then data arrives.
        boundary.note_cmd_edge(t);
        boundary.note_cmd_edge(t + 10);
        uart.data = {1, 2, 3};
        capture.poll(boundary, router, uart, asserted, now);
        if (filter) {
            CHECK(capture.stats().aborts_edge == 0);
            CHECK(capture.stats().write_glitches_ignored == 2);
            CHECK(router.pending == 8);
        } else {
            CHECK(capture.stats().aborts_edge == 1);
            CHECK(capture.stats().aborts_edge_in_write == 1);
            CHECK(router.pending == 0);
        }
    }
}

TEST_CASE("SIO flight recording retains prehistory and freezes after sixteen following events")
{
    using namespace fujinet::io;
    AtariSioCapture capture;
    AtariSioTransactionBoundary boundary;
    for (unsigned i=0;i<140;++i) capture.trace_reply(0xffe00000U+i, 10, 19200, 1, 'A', 0);
    boundary.note_cmd_edge(180000);
    capture.trace_poll_gap(200000,20000,boundary,true,1);
    CHECK(capture.stats().flight_triggered);
    CHECK_FALSE(capture.stats().flight);
    for (unsigned i=0;i<15;++i) capture.trace_reply(200001+i,10,19200,1,'C',0);
    CHECK_FALSE(capture.stats().flight);
    capture.trace_reply(200016,10,19200,1,'A',0,true);
    const auto saved=capture.stats().flight;
    REQUIRE(saved);
    CHECK(saved->count==157);
    CHECK(saved->trigger_count==141);
    const auto& trigger=saved->entries[140%128];
    CHECK(trigger.event==8);
    CHECK(trigger.last_edge_us==180000);
    CHECK(trigger.poll_gap_us==20000);
    CHECK(trigger.edge_batch==1);
    CHECK(saved->entries[156%128].event==9);
    CHECK(saved->entries[29].time_us==0xffe0001dU); // oldest of 128 retained records
    for (unsigned i=0;i<200;++i) capture.trace_reply(300000+i,10,19200,1,'C',0);
    CHECK(saved->count==157);
    CHECK(saved->entries[29].time_us==0xffe0001dU);
    capture.reset_stats();
    CHECK_FALSE(capture.stats().flight);
    CHECK_FALSE(capture.stats().flight_triggered);
    CHECK(saved->count==157); // old snapshots survive reset without mutation
}

TEST_CASE("SIO flight recording freezes on elapsed time with clock wrap and no later traffic")
{
    using namespace fujinet::io;
    AtariSioCapture capture;
    AtariSioTransactionBoundary boundary;
    for (unsigned i=0;i<32;++i) capture.trace_reply(0xffe00000U+i,10,19200,1,'A',0);
    capture.trace_poll_gap(0xfffffff0U,15999,boundary,false);
    CHECK_FALSE(capture.stats().flight_triggered);
    capture.trace_poll_gap(0xfffffff0U,16000,boundary,false);
    capture.trace_poll_gap(999983,0,boundary,false);
    CHECK_FALSE(capture.stats().flight);
    capture.trace_poll_gap(999984,0,boundary,false);
    REQUIRE(capture.stats().flight);
    CHECK(capture.stats().flight->count==33);
}

TEST_CASE("SIO flight recording triggers on ambiguous idle CMD edges with boundary metadata")
{
    using namespace fujinet::io;
    AtariSioCapture capture;
    AtariSioTransactionBoundary boundary;
    CaptureUart uart;
    CaptureRouter router;
    for (unsigned i=0;i<32;++i) capture.trace_reply(0xffe00000U+i,10,19200,1,'A',0);
    boundary.note_cmd_edge(100);boundary.note_cmd_edge(200);
    capture.poll(boundary,router,uart,[]{return false;},[]{return 300;});
    CHECK(capture.stats().flight_triggered);
    CHECK(capture.stats().flight_trigger_event==6);
    capture.trace_poll_gap(1000300,0,boundary,false);
    REQUIRE(capture.stats().flight);
    const auto& t=capture.stats().flight->entries[33];
    CHECK(t.event==6);CHECK(t.edge_batch==2);CHECK(t.last_edge_us==200);
    CHECK_FALSE(t.low);
    CHECK(t.used==0);
}

TEST_CASE("SIO flight warmup requires elapsed time and history and restarts after reset")
{
    using namespace fujinet::io;
    AtariSioCapture capture;
    AtariSioTransactionBoundary boundary;
    capture.trace_poll_gap(100,20000,boundary,true,2);
    CHECK_FALSE(capture.stats().flight_triggered);
    capture.trace_poll_gap(2000100,20000,boundary,true,2);
    CHECK_FALSE(capture.stats().flight_armed); // time alone is insufficient
    for (unsigned i=0;i<30;++i) capture.trace_reply(2000101+i,10,19200,1,'A',0);
    capture.trace_poll_gap(2000200,20000,boundary,true,2);
    REQUIRE(capture.stats().flight_triggered);
    capture.reset_stats();
    for (unsigned i=0;i<40;++i) capture.trace_reply(3000000+i,10,19200,1,'A',0);
    capture.trace_poll_gap(4999999,20000,boundary,true,2);
    CHECK_FALSE(capture.stats().flight_armed); // history alone is insufficient
    capture.trace_poll_gap(5000000,20000,boundary,true,2);
    CHECK(capture.stats().flight_triggered);
}

TEST_CASE("SIO flight history records command edge observations and collection decisions")
{
    using namespace fujinet::io;
    AtariSioCapture capture;
    AtariSioTransactionBoundary boundary;
    CaptureUart uart;
    CaptureRouter router;
    std::uint32_t time=100;
    bool low=true;
    for (unsigned i=0;i<32;++i) capture.trace_reply(0xffe00000U+i,10,19200,1,'A',0);
    boundary.note_cmd_edge(time);
    capture.poll(boundary,router,uart,[&]{return low;},[&]{return time;});
    time=200; low=false; boundary.note_cmd_edge(time);
    capture.poll(boundary,router,uart,[&]{return low;},[&]{return time;});
    time=300; boundary.note_cmd_edge(time); // extra edge after release, no bytes
    capture.poll(boundary,router,uart,[&]{return low;},[&]{return time;});
    REQUIRE(capture.stats().flight_triggered);
    capture.trace_poll_gap(1000300,0,boundary,false);
    const auto saved=capture.stats().flight;
    REQUIRE(saved);
    CHECK(saved->entries[32].event==10);
    CHECK(saved->entries[32].low);
    CHECK_FALSE(saved->entries[32].collecting);
    CHECK(saved->entries[33].event==11);
    CHECK(saved->entries[33].collecting);
    CHECK(saved->entries[34].event==10);
    CHECK_FALSE(saved->entries[34].low);
    CHECK(saved->entries[35].event==12);
    CHECK(saved->entries[35].released);
    CHECK(saved->entries[36].event==10);
    CHECK(saved->entries[37].event==2);
    CHECK(saved->entries[37].used==0);
    CHECK(saved->entries[37].last_edge_us==300);
}

TEST_CASE("Reply noise followed by next command assertion does not discard the command")
{
    using namespace fujinet::io;
    // Reproduce the old deferred filtering outcome: low at flush completion
    // keeps both raw edges, and the command collector rejects the next command.
    {
        AtariSioTransactionBoundary old_boundary;
        AtariSioCapture old_capture;
        CaptureUart old_uart;
        CaptureRouter old_router;
        old_boundary.note_cmd_edge(95045000);
        old_boundary.note_cmd_edge(95045950);
        old_capture.poll(old_boundary,old_router,old_uart,[]{return true;},[]{return 95047810;});
        CHECK(old_capture.stats().aborts_edge==1);
        CHECK_FALSE(old_capture.needs_immediate_poll());
    }

    AtariSioTransactionBoundary boundary;
    AtariSioCapture capture;
    CaptureUart uart;
    CaptureRouter router;
    boundary.begin_reply([]{return false;});
    boundary.note_bus_edge(95045000,false); // reply-time high interrupt
    boundary.note_bus_edge(95045950,true);  // next COMMAND before flush returns
    boundary.end_reply();
    capture.poll(boundary,router,uart,[]{return true;},[]{return 95047810;});
    CHECK(capture.stats().aborts_edge==0);
    CHECK(capture.needs_immediate_poll());
    boundary.note_bus_edge(95050481,false);
    uart.driver={0x7f,0x54,0x0b,0x00,0xde};
    capture.poll(boundary,router,uart,[]{return false;},[]{return 95050776;});
    CHECK(capture.stats().frames==1);
    CHECK(router.command==std::vector<std::uint8_t>{0x7f,0x54,0x0b,0x00,0xde});
    CHECK(boundary.reply_noise()==1);
    CHECK(boundary.total_edges()==3);
}

TEST_CASE("Reply filtering preserves ambiguous boundaries and never clears a racing assertion")
{
    using namespace fujinet::io;
    for (unsigned scenario=0;scenario<4;++scenario) {
        AtariSioTransactionBoundary boundary;
        if (scenario==0) boundary.note_bus_edge(10,true); // already pending
        boundary.begin_reply([&]{
            if (scenario==1) boundary.note_bus_edge(11,true); // during level check
            return scenario==2; // low at entry, even without recorded interrupt
        });
        if (scenario==3) boundary.note_bus_edge(12,true); // during transmission
        boundary.note_bus_edge(20,false);
        boundary.note_bus_edge(30,true);
        boundary.end_reply();
        CHECK(boundary.reply_noise()==0);
        CHECK(boundary.consume()==(scenario==2 ? 2 : 3));
    }
    AtariSioTransactionBoundary boundary;
    boundary.begin_reply([]{return false;});
    boundary.end_reply();
    boundary.note_bus_edge(40,false); // outside transmission is never filtered
    CHECK(boundary.consume()==1);
    CHECK(boundary.reply_noise()==0);
}

TEST_CASE("Trigger preserves individual IRQ levels filtering and reply state")
{
    using namespace fujinet::io;
    AtariSioTransactionBoundary boundary;
    AtariSioCapture capture;
    CaptureUart uart;
    CaptureRouter router;
    for (unsigned i=0;i<40;++i) {
        capture.trace_reply(0xffe00000U+i,10,19200,1,'A',0);
        boundary.note_bus_edge(i,false);
        boundary.consume();
    }
    boundary.begin_reply([]{return false;});
    boundary.note_bus_edge(100,false); // filtered high
    boundary.note_bus_edge(200,true);  // first low retained
    boundary.note_bus_edge(410,true);  // duplicate low remains ambiguous
    boundary.end_reply();
    capture.poll(boundary,router,uart,[]{return true;},[]{return 500;});
    REQUIRE(capture.stats().flight_triggered);
    capture.trace_poll_gap(1000500,0,boundary,false);
    const auto saved=capture.stats().flight;
    REQUIRE(saved);
    CHECK(saved->trigger_edges[0].sequence==28);
    CHECK(saved->trigger_edges[13].time_us==100);
    CHECK(saved->trigger_edges[13].flags==6); // high, filtered, state 1
    CHECK(saved->trigger_edges[14].flags==5); // low, retained, state 1
    CHECK(saved->trigger_edges[15].flags==9); // low, retained, state 2
    for (unsigned i=0;i<32;++i) boundary.note_bus_edge(400+i,false);
    CHECK(saved->trigger_edges[15].time_us==410);
    capture.reset_stats();
    CHECK(saved->trigger_edges[15].time_us==410);
}

TEST_CASE("Released command tolerates one observed high but rejects new assertions and ambiguity")
{
    using namespace fujinet::io;
    for (unsigned scenario=0;scenario<7;++scenario) {
        AtariSioTransactionBoundary boundary;
        AtariSioCapture capture;
        CaptureUart uart;
        CaptureRouter router;
        boundary.note_bus_edge(100,true);
        capture.poll(boundary,router,uart,[]{return true;},[]{return 110;});
        boundary.note_bus_edge(4500,false);
        capture.poll(boundary,router,uart,[]{return false;},[]{return 4600;});
        if (scenario==2) boundary.note_bus_edge(5700,true);
        if (scenario==3) boundary.note_bus_edge(5700,false);
        boundary.note_bus_edge(5800,scenario==1);
        uart.driver={0x7f,0x53,0x11,0x00,0xe3};
        if (scenario==5) uart.driver.push_back(0);
        if (scenario==6) ++uart.errors;
        capture.poll(boundary,router,uart,[&]{return scenario==1;},[&]{return scenario==4 ? 50200 : 5900;});
        if (scenario==0) {
            CHECK(capture.stats().frames==1);
            CHECK(capture.stats().aborts_edge==0);
            CHECK(router.command==std::vector<std::uint8_t>{0x7f,0x53,0x11,0x00,0xe3});
        } else {
            CHECK(capture.stats().frames==0);
            CHECK(router.command.empty());
            if (scenario==4) CHECK(capture.stats().aborts_timeout==1);
            else if (scenario==5) CHECK(capture.stats().aborts_overflow==1);
            else if (scenario==6) CHECK(capture.stats().aborts_rx_error==1);
            else CHECK(capture.stats().aborts_edge==1);
        }
    }
}

TEST_CASE("Idle high then low handover preserves assertion without accepting other pairs")
{
    using namespace fujinet::io;
    for (unsigned scenario=0;scenario<6;++scenario) {
        AtariSioTransactionBoundary boundary;
        AtariSioCapture capture;
        CaptureUart uart;
        CaptureRouter router;
        if (scenario==3) router.pending=8;
        if (scenario==4) {
            boundary.note_bus_edge(10,true);
            capture.poll(boundary,router,uart,[]{return true;},[]{return 20;});
        }
        if (scenario==5) { // raw edges lack a trustworthy observation history
            boundary.note_cmd_edge(100);
            boundary.note_cmd_edge(208);
        } else {
            boundary.note_bus_edge(100,scenario==1);
            boundary.note_bus_edge(208,scenario!=2);
        }
        capture.poll(boundary,router,uart,[&]{return scenario!=2;},[]{return 2000;});
        if (scenario==0) {
            REQUIRE(capture.stats().aborts_edge==0);
            REQUIRE(capture.needs_immediate_poll());
            boundary.note_bus_edge(4646,false);
            uart.driver={0x7f,0x54,0x0b,0x00,0xde};
            capture.poll(boundary,router,uart,[]{return false;},[]{return 4800;});
            CHECK(capture.stats().frames==1);
            CHECK(router.command==std::vector<std::uint8_t>{0x7f,0x54,0x0b,0x00,0xde});
        } else {
            CHECK(capture.stats().aborts_edge==1);
            CHECK(capture.stats().frames==0);
            CHECK(router.command.empty());
        }
    }
}

TEST_CASE("Reply low pair recovery is limited to the observed short reply handover")
{
    using namespace fujinet::io;
    for (unsigned scenario=0;scenario<5;++scenario) {
        AtariSioTransactionBoundary boundary;
        AtariSioCapture capture;
        CaptureUart uart;
        CaptureRouter router;
        if (scenario!=1) boundary.begin_reply([]{return false;});
        boundary.note_bus_edge(1000,true);
        if (scenario==2) boundary.end_reply();
        boundary.note_bus_edge(scenario==3 ? 1201 : 1115,true);
        boundary.end_reply();
        if (scenario==4) router.pending=8;
        capture.poll(boundary,router,uart,[]{return true;},[]{return 2000;});
        if (scenario==0) {
            CHECK(capture.stats().aborts_edge==0);
            boundary.note_bus_edge(5500,false);
            uart.driver={0x7f,0x54,0x0b,0,0xde};
            capture.poll(boundary,router,uart,[]{return false;},[]{return 6000;});
            CHECK(capture.stats().frames==1);
            CHECK(router.command.size()==5);
        } else {
            CHECK(capture.stats().aborts_edge==1);
            CHECK(router.command.empty());
        }
    }
}

TEST_CASE("Stream retry-sized gap freezes history without a capture abort")
{
    using namespace fujinet::io;
    for (const unsigned gap : {100000,300000,1100000}) {
        AtariSioTransactionBoundary boundary;
        AtariSioCapture capture;
        CaptureUart uart;
        CaptureRouter router;
        for (unsigned i=0;i<40;++i) capture.trace_reply(0xffe00000U+i,10,19200,1,'A',0);
        const auto command=[&](unsigned t) {
            boundary.note_bus_edge(t,true);
            capture.poll(boundary,router,uart,[]{return true;},[&]{return t;});
            boundary.note_bus_edge(t+4500,false);
            uart.driver={0x7f,0x54,0x0b,0,0xde};
            capture.poll(boundary,router,uart,[]{return false;},[&]{return t+5000;});
        };
        command(10000);command(10000+gap);
        CHECK(capture.stats().aborts_edge==0);
        CHECK(capture.stats().frames==2);
        CHECK(capture.stats().flight_triggered==(gap==300000));
        if (gap==300000) CHECK(capture.stats().flight_trigger_event==17);
    }
}

TEST_CASE("Idle high observation does not require an idle poll before the next command")
{
    using namespace fujinet::io;
    AtariSioTransactionBoundary boundary;
    AtariSioCapture capture;
    CaptureUart uart;
    CaptureRouter router;
    boundary.note_bus_edge(158126166,false);
    capture.poll(boundary,router,uart,[]{return false;},[]{return 158126378;});
    CHECK(capture.stats().aborts_edge==1);
    // No intervening poll: the next assertion wakes the sleeping ingress.
    boundary.note_bus_edge(158134260,true);
    capture.poll(boundary,router,uart,[]{return true;},[]{return 158134316;});
    CHECK(capture.needs_immediate_poll());
    boundary.note_bus_edge(158138651,false);
    uart.driver={0x7f,0x53,0x11,0,0xe3};
    capture.poll(boundary,router,uart,[]{return false;},[]{return 158138814;});
    CHECK(capture.stats().frames==1);
    CHECK(router.command==std::vector<std::uint8_t>{0x7f,0x53,0x11,0,0xe3});
    CHECK(capture.stats().frames_lost==0);
}
