#include "doctest.h"

#include "fujinet/diag/diagnostic_atari_sio.h"
#include "fujinet/io/atari_sio_stats.h"

#include <string>
#include <string_view>
#include <vector>

using fujinet::diag::DiagArgsView;
using fujinet::diag::DiagStatus;
using fujinet::io::AtariSioStats;

namespace {

fujinet::diag::DiagResult run(fujinet::diag::IDiagnosticProvider& p, std::vector<std::string_view> argv)
{
    DiagArgsView args;
    args.argv = std::move(argv);
    return p.execute(args);
}

bool contains(const std::string& text, std::string_view part) { return text.find(part) != std::string::npos; }

struct Fixture {
    AtariSioStats stats;
    int resets{0};
    std::unique_ptr<fujinet::diag::IDiagnosticProvider> provider = fujinet::diag::create_atari_sio_diagnostic_provider(
        [this] { return stats; }, [this] { ++resets; stats = {}; });
};

} // namespace

TEST_CASE("sio diagnostics need both a snapshot and a reset")
{
    CHECK(fujinet::diag::create_atari_sio_diagnostic_provider(nullptr, [] {}) == nullptr);
    CHECK(fujinet::diag::create_atari_sio_diagnostic_provider([] { return AtariSioStats{}; }, nullptr) == nullptr);
}

TEST_CASE("sio.status lists its commands and reports every counter")
{
    Fixture f;
    REQUIRE(f.provider != nullptr);
    CHECK(f.provider->provider_id() == "sio");
    std::vector<fujinet::diag::DiagCommandSpec> cmds;
    f.provider->list_commands(cmds);
    REQUIRE(cmds.size() == 3);
    CHECK(cmds[0].name == "sio.status");
    CHECK(cmds[1].name == "sio.trace");
    CHECK(cmds[2].name == "sio.reset");

    f.stats.cmd_edges = 12;
    f.stats.cmd_line_low = true;
    f.stats.uart_rx_errors = 3;
    f.stats.uart_discarded_bytes = 12;
    f.stats.alt_discarded_bytes = 8;
    f.stats.capture = {.rx_bytes = 40, .frames = 7, .aborts_edge = 1, .aborts_timeout = 2, .aborts_overflow = 3, .aborts_rx_error = 4};
    f.stats.router.disk_commands = 5;
    f.stats.router.nio_commands = 1;
    f.stats.router.other_commands = 1;
    f.stats.router.has_last_frame = true;
    f.stats.router.last_frame = {0x31, 0x52, 0x01, 0x00, 0x84};

    const auto r = run(*f.provider, {"sio.status"});
    REQUIRE(r.status == DiagStatus::Ok);
    for (const char* expected : {"cmd_edges: 12", "low (asserted)", "rx_bytes: 40", "uart_rx_errors: 3", "discarded buffered bytes: main=12 alternate=8", "frames: 7",
                                 "disk(D1:-D8:): 5", "nio(0x7f): 1", "other devices: 1", "edge=1", "timeout=2",
                                 "overflow=3", "rx_error=4", "0x31 0x52 0x01 0x00 0x84", "device 0x31", "(read sector)"}) {
        CAPTURE(expected);
        CHECK(contains(r.text, expected));
    }
}

TEST_CASE("sio.status shows an idle CMD line and no last frame before anything happens")
{
    Fixture f;
    const auto r = run(*f.provider, {"sio.status"});
    REQUIRE(r.status == DiagStatus::Ok);
    CHECK(contains(r.text, "high (idle)"));
    CHECK(contains(r.text, "last_frame: none yet"));
}

TEST_CASE("sio.trace shows the newest events in chronological order")
{
    Fixture f;
    f.stats.capture.trace_count = 18;
    f.stats.capture.trace[2].time_us = 42;
    f.stats.capture.trace[2].event = 2;
    f.stats.capture.trace[2].pending = 3;
    f.stats.capture.trace[2].frame = {0x7f, 'P', 0, 0, 0xcf};
    f.stats.capture.trace[1].time_us = 99;
    const auto r = run(*f.provider, {"sio.trace"});
    REQUIRE(r.status == DiagStatus::Ok);
    CHECK(contains(r.text, "us=42 event=2"));
    CHECK(contains(r.text, "pending=3"));
    CHECK(contains(r.text, "0x7f 0x50 0x00 0x00 0xcf"));
    CHECK(r.text.find("us=42") < r.text.find("us=99"));
}

TEST_CASE("sio.status names the native commands and leaves unknown ones as hex")
{
    Fixture f;
    f.stats.router.has_last_frame = true;
    f.stats.router.last_frame = {0x7f, 'P', 0, 0, 0};
    CHECK(contains(run(*f.provider, {"sio.status"}).text, "(poll)"));
    f.stats.router.last_frame = {0x31, 0x99, 0, 0, 0};
    const auto text = run(*f.provider, {"sio.status"}).text;
    CHECK(contains(text, "command 0x99"));
    CHECK_FALSE(contains(text, "command 0x99 ("));
}

TEST_CASE("sio.status reading points at the first thing that is wrong, in signal order")
{
    Fixture f;
    auto reading = [&] { return run(*f.provider, {"sio.status"}).text; };

    CHECK(contains(reading(), "CMD line has not changed"));            // no edges at all

    f.stats.cmd_edges = 4;
    CHECK(contains(reading(), "no bytes arrive on the SIO data-in"));  // CMD toggles, silent UART

    f.stats.capture.rx_bytes = 20;
    CHECK(contains(reading(), "no complete command frame"));           // bytes but no frame

    f.stats.capture.frames = 3;
    f.stats.router.other_commands = 3;
    CHECK(contains(reading(), "other devices only"));                  // frames, none ours

    f.stats.router.disk_commands = 2;
    CHECK(contains(reading(), "disk commands reach the disk frontend"));

    f.stats.router.disk_commands = 0;
    f.stats.router.nio_commands = 1;
    CHECK(contains(reading(), "native (0x7f) commands reach the endpoint"));
}

TEST_CASE("sio.reset zeroes the counters through the ingress, and unknown commands are not ours")
{
    Fixture f;
    f.stats.capture.frames = 9;
    const auto r = run(*f.provider, {"sio.reset"});
    CHECK(r.status == DiagStatus::Ok);
    CHECK(f.resets == 1);
    CHECK(f.stats.capture.frames == 0);
    CHECK(run(*f.provider, {"sio.nonsense"}).status == DiagStatus::NotFound);
}

TEST_CASE("sio.status shows when the ingress was ready and when the Atari first spoke")
{
    Fixture f;
    f.stats.reply.ready_us = 2'235'000;
    f.stats.first_edge_us = 2'790'000;
    f.stats.capture.first_frame_us = 2'802'000;
    auto text = run(*f.provider, {"sio.status"}).text;
    CHECK(contains(text, "ingress ready 2235   first CMD edge 2790   first frame 2802"));
    CHECK_FALSE(contains(text, "watch:"));    // ready before the Atari spoke: nothing to flag

    f.stats = {};
    text = run(*f.provider, {"sio.status"}).text;
    CHECK(contains(text, "ingress ready none   first CMD edge none   first frame none"));
}

TEST_CASE("sio.status reports the reaction, reply and handler times in milliseconds")
{
    Fixture f;
    f.stats.capture.lag_us_max = 400;
    f.stats.reply.replies = 122;
    f.stats.reply.reply_us_last = 1200;
    f.stats.reply.reply_us_max = 3100;
    f.stats.reply.handle_us_last = 900;
    f.stats.reply.handle_us_max = 2800;
    const auto text = run(*f.provider, {"sio.status"}).text;
    CHECK(contains(text, "react: CMD release -> start of processing, max 0.4 ms"));
    CHECK(contains(text, "reply: CMD release -> first byte, last 1.2 ms, max 3.1 ms, over 16 ms: 0   (replies 122)"));
    CHECK(contains(text, "handler: last 0.9 ms, max 2.8 ms"));
    CHECK_FALSE(contains(text, "watch:"));
}

TEST_CASE("sio.status warns about late ACKs, slow handlers and a first edge before the ingress was ready")
{
    Fixture f;
    f.stats.reply.reply_over_deadline = 2;
    f.stats.reply.reply_us_max = 21'500;
    auto text = run(*f.provider, {"sio.status"}).text;
    CHECK(contains(text, "watch: 2 command reply(ies) started more than 16 ms after CMD was released (worst 21.5 ms)"));

    f.stats = {};
    f.stats.reply.handle_us_max = 800'000;
    text = run(*f.provider, {"sio.status"}).text;
    CHECK(contains(text, "watch: a request spent 800.0 ms in its handler"));
    CHECK(contains(text, "leaves the native endpoint stuck"));

    f.stats = {};
    f.stats.reply.handle_us_max = 99'999;      // just under the limit: no warning
    CHECK_FALSE(contains(run(*f.provider, {"sio.status"}).text, "watch:"));

    f.stats = {};
    f.stats.poll_gap_us_max = 1'350'000;
    f.stats.poll_gap_at_us = 1'650'000;
    text = run(*f.provider, {"sio.status"}).text;
    CHECK(contains(text, "while CMD was active, 1350.0 ms, starting at 1650"));
    CHECK(contains(text, "watch: the SIO task went 1350.0 ms without polling while CMD was active"));

    f.stats = {};
    f.stats.stale_replies_dropped = 3;
    CHECK(contains(run(*f.provider, {"sio.status"}).text, "native replies never read, discarded: 3"));
    f.stats.stale_replies_dropped = 0;
    CHECK_FALSE(contains(run(*f.provider, {"sio.status"}).text, "never read"));

    f.stats = {};
    f.stats.reply.ready_us = 2'000'000;
    f.stats.first_edge_us = 1'900'000;
    CHECK(contains(run(*f.provider, {"sio.status"}).text, "watch: the first CMD edge came before the ingress was ready"));
}

TEST_CASE("sio.status shows the UART baud and sio.baud is offered only with a probe")
{
    Fixture f;
    f.stats.uart_baud_set = 19200;
    f.stats.uart_baud_hw = 19201;
    CHECK(contains(run(*f.provider, {"sio.status"}).text, "baud: set 19200, UART hardware reports 19201"));
    CHECK(run(*f.provider, {"sio.baud"}).status == DiagStatus::NotFound);
}

TEST_CASE("sio.baud compares the UART rate with the POKEY rate for each high-speed index")
{
    AtariSioStats stats;
    stats.uart_baud_set = 19200;
    stats.uart_baud_hw = 19200;
    std::vector<std::uint32_t> probed;
    auto provider = fujinet::diag::create_atari_sio_diagnostic_provider(
        [&] { return stats; }, [] {},
        [&](std::uint32_t target) { probed.push_back(target); return target + target / 100; });  // 1% fast
    std::vector<fujinet::diag::DiagCommandSpec> cmds;
    provider->list_commands(cmds);
    REQUIRE(cmds.size() == 4);

    const auto text = run(*provider, {"sio.baud"}).text;
    REQUIRE(probed.size() == 11);                 // indexes 0..10
    CHECK(probed[0] == 124017);                   // the legacy table: index 0 is 124,017 baud
    CHECK(probed[6] == 67431);                    // index 6 is 67,431 baud
    CHECK(contains(text, "SIO UART now: set 19200, hardware 19200"));
    CHECK(contains(text, "125257"));              // 1% above the index 0 target
    CHECK(contains(text, "+1.0%"));               // against the target
    CHECK(contains(text, "run with the Atari idle"));
}

TEST_CASE("sio.hsio sets or clears the index, and sio.status shows the high-speed state")
{
    AtariSioStats stats;
    int set_to = -99;
    bool fallback = false;
    auto provider = fujinet::diag::create_atari_sio_diagnostic_provider(
        [&] { return stats; }, [] {}, {},
        [&](int index, bool fb) { if (index > 9) return false; set_to = index; fallback = fb; return true; });
    std::vector<fujinet::diag::DiagCommandSpec> cmds;
    provider->list_commands(cmds);
    REQUIRE(cmds.size() == 4);

    CHECK(contains(run(*provider, {"sio.status"}).text, "hsio: off (3F is NAKed"));

    auto r = run(*provider, {"sio.hsio", "6"});
    CHECK(r.status == DiagStatus::Ok);
    CHECK(set_to == 6);
    CHECK(contains(r.text, "index 6 (67431 baud)"));
    CHECK(contains(r.text, "not saved"));
    CHECK(fallback);

    r = run(*provider, {"sio.hsio", "6", "nofallback"});
    CHECK(r.status == DiagStatus::Ok);
    CHECK(!fallback);
    CHECK(contains(r.text, "no fallback"));
    CHECK(run(*provider, {"sio.hsio", "6", "later"}).status == DiagStatus::InvalidArgs);
    CHECK(run(*provider, {"sio.hsio", "off", "nofallback"}).status == DiagStatus::InvalidArgs);

    CHECK(run(*provider, {"sio.hsio", "off"}).status == DiagStatus::Ok);
    CHECK(set_to == -1);
    CHECK(run(*provider, {"sio.hsio", "10"}).status == DiagStatus::InvalidArgs);
    CHECK(run(*provider, {"sio.hsio", "fast"}).status == DiagStatus::InvalidArgs);
    CHECK(set_to == -1);

    stats.tx_noise_edges = 12;
    CHECK(contains(run(*provider, {"sio.status"}).text, "CMD edges while we transmitted, dropped as noise: 12"));
    stats.tx_noise_edges = 0;
    CHECK_FALSE(contains(run(*provider, {"sio.status"}).text, "dropped as noise"));
    stats.alt_baud = 19200;
    stats.alt_valid_frames = 7;
    stats.alt_ambiguous_dropped = 2;
    stats.alt_last_frame = {0x7f, 0x51, 0x00, 0x00, 0xd0};
    CHECK(contains(run(*provider, {"sio.status"}).text,
                   "second listener (UART2, same RX pin) at 19200 baud: command frames decoded 7, used instead of the main UART's 0, last 0x7f 0x51 0x00 0x00 0xd0 (device 0x7f, command 0x51 query)"));
    CHECK(contains(run(*provider, {"sio.status"}).text, "frames discarded with ambiguous CMD edges: 2"));
    stats.capture.frames_from_second_listener = 1;
    stats.capture.first_alt_decisions[0].frame = stats.alt_last_frame;
    stats.capture.first_alt_decisions[0].edges = 2;
    stats.capture.first_alt_decisions[0].collecting = true;
    CHECK(contains(run(*provider, {"sio.status"}).text,
                   "CMD edges 2, collecting 1, released 0, main bytes already collected 0"));
    stats.capture.frames_from_second_listener = 0;
    stats.alt_baud = 0;
    CHECK_FALSE(contains(run(*provider, {"sio.status"}).text, "second listener"));
    stats.router.refused = 3;
    stats.router.first_refused = {0x31, 0xd2, 0x01, 0x00, 0x00};
    stats.router.last_refused = {0x7f, 0x3f, 0x20, 0x00, 0x00};
    stats.router.has_last_other = true;
    stats.router.last_other = {0x70, 0x3f, 0x20, 0x00, 0xcf};
    stats.router.other_first_count = 2;
    stats.router.other_first[0] = {0x71, 0x3f, 0x20, 0x00, 0xd0};
    stats.router.other_first[1] = {0x71, 0x53, 0x00, 0x00, 0xc5};
    const auto refused = run(*provider, {"sio.status"}).text;
    CHECK(contains(refused, "refused (answered NAK, for our devices): 3   first 0x31 0xd2 0x01 0x00 0x00 (device 0x31, command 0xd2)"));
    CHECK(contains(refused, "last 0x7f 0x3f 0x20 0x00 0x00 (device 0x7f, command 0x3f high-speed query)"));
    CHECK(contains(refused, "other device, frame 1: 0x71 0x3f 0x20 0x00 0xd0 (device 0x71, command 0x3f high-speed query)"));
    CHECK(contains(refused, "other device, frame 2: 0x71 0x53"));
    CHECK(contains(refused, "last frame for another device: 0x70 0x3f 0x20 0x00 0xcf (device 0x70, command 0x3f high-speed query)"));

    stats.hsio_index = 6;
    stats.hsio_high = true;
    stats.hsio_switches = 2;
    stats.hsio_fallbacks = 1;
    stats.capture.frames_bad = 4;
    stats.router.hsio_queries_disk = 2;
    stats.router.hsio_queries_nio = 0;
    const auto text = run(*provider, {"sio.status"}).text;
    CHECK(contains(text, "hsio: index 6 (67431 baud), HIGH SPEED now   switches 2, fallbacks 1, unreadable command frames 4"));
    CHECK(contains(text, "3F asked: D1:-D8: 2, 0x7f 0"));
    CHECK(contains(run(*provider, {"sio.hsio"}).text, "HIGH SPEED now"));

    stats.capture.first_bad = {fujinet::io::AtariSioBadFrame::Reason::Overflow, 5, {0x31, 0x52, 0x01, 0x00, 0x84}};
    stats.capture.last_bad = {fujinet::io::AtariSioBadFrame::Reason::Checksum, 5, {0x31, 0x52, 0x02, 0x00, 0x00}};
    stats.hsio_fallback_enabled = false;
    const auto bad = run(*provider, {"sio.status"}).text;
    CHECK(contains(bad, "(fallback off)"));
    CHECK(contains(bad, "first unreadable: more than 5 bytes, 5 byte(s): 0x31 0x52 0x01 0x00 0x84"));
    CHECK(contains(bad, "last unreadable:  bad checksum, 5 byte(s): 0x31 0x52 0x02 0x00 0x00"));
}

TEST_CASE("sio.tune shows and sets the gaps and the window, and sio.status shows the data-frame rate")
{
    AtariSioStats stats;
    stats.uart_baud_set = 108800;
    stats.reply.record_data(30, 3000);     // too short to count
    stats.reply.record_data(258, 24'000);
    fujinet::io::AtariSioTuning set_to;
    int calls = 0;
    auto provider = fujinet::diag::create_atari_sio_diagnostic_provider(
        [&] { return stats; }, [] {}, {}, {},
        [&](const fujinet::io::AtariSioTuning& t) { set_to = t; stats.tuning = t; ++calls; });
    std::vector<fujinet::diag::DiagCommandSpec> cmds;
    provider->list_commands(cmds);
    REQUIRE(cmds.size() == 4);

    auto r = run(*provider, {"sio.tune"});
    CHECK(r.status == DiagStatus::Ok);
    CHECK(calls == 0);
    CHECK(contains(r.text, "ack gap 250 us, data gap 0 us"));
    CHECK(!contains(r.text, "not the defaults"));

    CHECK(run(*provider, {"sio.tune", "data", "1000"}).status == DiagStatus::Ok);
    CHECK(set_to.data_gap_us == 1000);
    CHECK(set_to.ack_gap_us == 250);
    r = run(*provider, {"sio.tune", "window", "5"});
    CHECK(set_to.alt_window_us == 5000);
    CHECK(contains(r.text, "not the defaults"));
    CHECK(run(*provider, {"sio.tune", "ack", "x"}).status == DiagStatus::InvalidArgs);
    CHECK(run(*provider, {"sio.tune", "bogus", "1"}).status == DiagStatus::InvalidArgs);
    CHECK(run(*provider, {"sio.tune", "glitch", "0"}).status == DiagStatus::Ok);
    CHECK(!set_to.ignore_write_glitches);
    CHECK(run(*provider, {"sio.tune", "reset"}).status == DiagStatus::Ok);
    CHECK(set_to.ignore_write_glitches);
    CHECK(set_to.data_gap_us == 0);
    CHECK(!set_to.proc_notify);
    r = run(*provider, {"sio.tune", "proc", "1"});
    CHECK(set_to.proc_notify);
    CHECK(contains(r.text, "PROC notify on"));
    CHECK(contains(r.text, "not the defaults"));
    CHECK(run(*provider, {"sio.tune", "reset"}).status == DiagStatus::Ok);
    CHECK(!set_to.proc_notify);

    const auto text = run(*provider, {"sio.status"}).text;
    CHECK(contains(text, "data frames (over 64 bytes): 1 sent, 93.0 us/byte on average (the wire: 91.9)"));
    CHECK(contains(text, "last 258 bytes in"));
}

TEST_CASE("sio.save hands the current high-speed index and tuning to the config store")
{
    AtariSioStats stats;
    stats.hsio_index = 1;
    stats.tuning.data_gap_us = 500;
    int saved_index = -99;
    std::uint32_t saved_gap = 0;
    std::string error;
    auto provider = fujinet::diag::create_atari_sio_diagnostic_provider(
        [&] { return stats; }, [] {}, {}, {}, [](const fujinet::io::AtariSioTuning&) {},
        [&](int index, const fujinet::io::AtariSioTuning& t) {
            saved_index = index; saved_gap = t.data_gap_us; return error;
        });
    std::vector<fujinet::diag::DiagCommandSpec> cmds;
    provider->list_commands(cmds);
    REQUIRE(cmds.size() == 5);      // status, trace, tune, save, reset

    auto r = run(*provider, {"sio.save"});
    CHECK(r.status == DiagStatus::Ok);
    CHECK(saved_index == 1);
    CHECK(saved_gap == 500);
    CHECK(contains(r.text, "saved"));

    error = "config store not available";
    r = run(*provider, {"sio.save"});
    CHECK(r.status == DiagStatus::NotReady);
    CHECK(contains(r.text, "config store not available"));
}

TEST_CASE("sio.proc asserts, releases and pulses the PROC line, and sio.status shows it")
{
    AtariSioStats stats;
    stats.proc_available = true;
    std::vector<bool> levels;
    std::vector<unsigned> pulses;
    auto provider = fujinet::diag::create_atari_sio_diagnostic_provider(
        [&] { return stats; }, [] {}, {}, {}, {}, {},
        [&](bool asserted) { levels.push_back(asserted); stats.proc_low = asserted; if (asserted) ++stats.proc_asserts; return true; },
        [&](unsigned count, unsigned low, unsigned high) { pulses = {count, low, high}; return true; });

    CHECK(contains(run(*provider, {"sio.status"}).text, "proc: released (high), driven low 0 times"));
    stats.tuning.proc_notify = true;
    stats.proc_notices = 7;
    stats.proc_pulses = 3;
    CHECK(contains(run(*provider, {"sio.status"}).text, "notify on: 7 readable notices, 3 pulses"));
    stats.tuning.proc_notify = false;
    stats.proc_notices = 0;
    stats.proc_pulses = 0;
    CHECK(contains(run(*provider, {"sio.proc"}).text, "released (high)"));

    CHECK(contains(run(*provider, {"sio.proc", "low"}).text, "ASSERTED (low), driven low 1 times"));
    CHECK(contains(run(*provider, {"sio.proc", "high"}).text, "released (high)"));
    REQUIRE(levels.size() == 2);
    CHECK(levels[0] == true);
    CHECK(levels[1] == false);

    CHECK(contains(run(*provider, {"sio.proc", "pulse"}).text, "pulsed PROC 1 time(s), low 10 ms, high 10 ms"));
    CHECK(pulses == std::vector<unsigned>{1, 10, 10});
    run(*provider, {"sio.proc", "pulse", "5", "20", "30"});
    CHECK(pulses == std::vector<unsigned>{5, 20, 30});

    // Out of range or malformed: refused, and nothing is driven.
    pulses.clear();
    for (const auto& bad : std::vector<std::vector<std::string_view>>{
             {"sio.proc", "pulse", "0"}, {"sio.proc", "pulse", "101"}, {"sio.proc", "pulse", "1", "0"},
             {"sio.proc", "pulse", "1", "1001"}, {"sio.proc", "pulse", "x"}, {"sio.proc", "flip"},
             {"sio.proc", "low", "1"}, {"sio.proc", "pulse", "1", "1", "1", "1"}}) {
        CHECK(run(*provider, bad).status != fujinet::diag::DiagStatus::Ok);
    }
    CHECK(pulses.empty());
}

TEST_CASE("sio.proc is absent without the callbacks, and says so on a board with no PROC pin")
{
    AtariSioStats stats;
    auto plain = fujinet::diag::create_atari_sio_diagnostic_provider([&] { return stats; }, [] {});
    std::vector<fujinet::diag::DiagCommandSpec> cmds;
    plain->list_commands(cmds);
    for (const auto& c : cmds) CHECK(c.name != "sio.proc");
    CHECK_FALSE(contains(run(*plain, {"sio.status"}).text, "proc:"));

    auto nopin = fujinet::diag::create_atari_sio_diagnostic_provider(
        [&] { return stats; }, [] {}, {}, {}, {}, {}, [](bool) { return false; },
        [](unsigned, unsigned, unsigned) { return false; });
    CHECK(contains(run(*nopin, {"sio.status"}).text, "this board maps no PROC pin"));
    CHECK(run(*nopin, {"sio.proc", "low"}).status != fujinet::diag::DiagStatus::Ok);
}
