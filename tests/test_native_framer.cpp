#include "doctest.h"

#include "fujinet/io/transport/native_framer.h"
#include "fujinet/io/transport/slip_framer.h"
#include "fujinet/io/transport/fujibus_transport.h"
#include "fujinet/core/bootstrap.h"
#include "fujinet/core/core.h"
#include "packet_io_double.h"
#include "fujibus_wire_fixtures.h"

#include <limits>
#include <type_traits>

using namespace fujinet::io;
using namespace packet_io_test;

static_assert(!std::is_copy_constructible_v<NativeFramer>);
static_assert(!std::is_copy_assignable_v<NativeFramer>);
static_assert(!std::is_move_constructible_v<NativeFramer>);
static_assert(!std::is_move_assignable_v<NativeFramer>);

// Deliberately stateless adapter: tests must establish framer-owned latches,
// rather than accidentally relying on the bounded double to enforce them.
class ScriptedPacketIO : public IPacketIO {
public:
    ScriptedPacketIO() : IPacketIO(8) {}
    PacketReceiveResult receive(std::uint8_t* buffer, std::size_t limit) override {
        ++receives;
        if (limit) buffer[0] = 0xAB;
        return receiveResult;
    }
    PacketIOStatus send(const std::uint8_t*, std::size_t) override {
        ++sends;
        return sendResult;
    }
    PacketIOStatus reset() override { return resetResult; }
    PacketReceiveResult receiveResult{PacketIOStatus::Ok, 1};
    PacketIOStatus sendResult{PacketIOStatus::Ok}, resetResult{PacketIOStatus::Ok};
    std::size_t receives{0}, sends{0};
};

TEST_SUITE("NativeFramer") {

TEST_CASE("queued packets preserve boundaries and receive-slot backpressure bounds work") {
    PacketIODouble io;
    PacketChannel ch(&io);
    NativeFramer framer;
    const ByteBuffer first{0, 0xC0, 0xDB, 0xFF}, second{2, 3};
    REQUIRE(io.enqueue(first));
    REQUIRE(io.enqueue(second));
    framer.poll(ch);
    CHECK(framer.receiveStatus() == PacketIOStatus::Ok);
    CHECK(io.receiveCalls == 1);
    for (unsigned i = 0; i < 100; ++i) framer.poll(ch);
    CHECK(framer.receiveStatus() == PacketIOStatus::Backpressure);
    CHECK(io.receiveCalls == 1);
    CHECK(io.rxRecords() == 1);
    ByteBuffer out{9};
    REQUIRE(framer.nextPacket(out));
    CHECK(out == first);
    CHECK_FALSE(framer.nextPacket(out));
    CHECK(out.empty());
    framer.poll(ch);
    REQUIRE(framer.nextPacket(out));
    CHECK(out == second);
    framer.poll(ch);
    CHECK(framer.receiveStatus() == PacketIOStatus::NoData);
    CHECK_FALSE(framer.nextPacket(out));
    CHECK(out.empty());
    CHECK(ch.byteCalls == 0);
}

TEST_CASE("delayed packets need explicit bounded delivery steps") {
    PacketIODouble io(8, 2, 16);
    PacketChannel ch(&io);
    NativeFramer framer;
    REQUIRE(io.enqueue({1}, 2));
    REQUIRE(io.enqueue({2}));
    for (unsigned i = 0; i < 100; ++i) {
        framer.poll(ch);
        CHECK(framer.receiveStatus() == PacketIOStatus::NoData);
    }
    CHECK(io.receiveCalls == 100);
    io.advance();
    CHECK(io.advanceWork == 2);
    framer.poll(ch);
    CHECK(framer.receiveStatus() == PacketIOStatus::NoData);
    io.advance();
    framer.poll(ch);
    ByteBuffer out;
    REQUIRE(framer.nextPacket(out));
    CHECK(out == ByteBuffer{1});
    framer.poll(ch);
    REQUIRE(framer.nextPacket(out));
    CHECK(out == ByteBuffer{2});
}

TEST_CASE("partial records complete once or truncate without leaking prefixes") {
    PacketIODouble io(8, 3, 16);
    PacketChannel ch(&io);
    NativeFramer framer;
    REQUIRE(io.enqueuePartial({0xC0}, 3));
    ByteBuffer out{9};
    for (unsigned i = 0; i < 3; ++i) {
        framer.poll(ch);
        CHECK(framer.receiveStatus() == PacketIOStatus::Incomplete);
        CHECK_FALSE(framer.nextPacket(out));
        CHECK(out.empty());
    }
    REQUIRE(io.appendFront({0xDB}, false));
    framer.poll(ch);
    CHECK(framer.receiveStatus() == PacketIOStatus::Incomplete);
    REQUIRE(io.appendFront({0xFF}, true));
    framer.poll(ch);
    REQUIRE(framer.nextPacket(out));
    CHECK(out == ByteBuffer{0xC0, 0xDB, 0xFF});
    CHECK_FALSE(framer.nextPacket(out));
    REQUIRE(io.enqueuePartial({7}, 3));
    REQUIRE(io.enqueue({8}));
    REQUIRE(io.truncateFront());
    framer.poll(ch);
    CHECK(framer.receiveStatus() == PacketIOStatus::Truncated);
    CHECK_FALSE(framer.nextPacket(out));
    CHECK(out.empty());
    framer.poll(ch);
    REQUIRE(framer.nextPacket(out));
    CHECK(out == ByteBuffer{8});
}

TEST_CASE("empty exact-capacity and metadata-only oversized records have explicit outcomes") {
    PacketIODouble io(8, 4, 16);
    PacketChannel ch(&io);
    NativeFramer framer;
    REQUIRE(io.enqueue({}));
    const ByteBuffer exact(8, 0xAB);
    REQUIRE(io.enqueue(exact));
    REQUIRE(io.enqueueOversized(std::numeric_limits<std::size_t>::max()));
    REQUIRE(io.enqueue({5}));
    CHECK(io.rxBytes() == 9);
    ByteBuffer out{9};
    framer.poll(ch);
    CHECK(framer.capacity() == 8);
    CHECK(framer.receiveStatus() == PacketIOStatus::EmptyPacket);
    CHECK_FALSE(framer.nextPacket(out));
    CHECK(out.empty());
    framer.poll(ch);
    REQUIRE(framer.nextPacket(out));
    CHECK(out == exact);
    framer.poll(ch);
    CHECK(framer.receiveStatus() == PacketIOStatus::Oversized);
    CHECK_FALSE(framer.nextPacket(out));
    CHECK(out.empty());
    framer.poll(ch);
    REQUIRE(framer.nextPacket(out));
    CHECK(out == ByteBuffer{5});
    framer.sendPacket(ch, {});
    CHECK(framer.sendStatus() == PacketIOStatus::EmptyPacket);
    framer.sendPacket(ch, ByteBuffer(9, 1));
    CHECK(framer.sendStatus() == PacketIOStatus::Oversized);
    CHECK(io.sendCalls == 0);
    framer.sendPacket(ch, exact);
    CHECK(framer.sendStatus() == PacketIOStatus::Ok);
    REQUIRE(io.takeSent(out));
    CHECK(out == exact);
}

TEST_CASE("effective raw capacity is capped at 65535 for both directions") {
    PacketIODouble io(65536, 2, 131072);
    PacketChannel ch(&io);
    NativeFramer framer(io);
    CHECK(framer.capacity() == 65535);
    const ByteBuffer exact(65535, 0xFF), oversized(65536, 0xAB);
    REQUIRE(io.enqueue(oversized));
    REQUIRE(io.enqueue(exact));
    framer.poll(ch);
    CHECK(framer.receiveStatus() == PacketIOStatus::Oversized);
    ByteBuffer out{9};
    CHECK_FALSE(framer.nextPacket(out));
    CHECK(out.empty());
    framer.poll(ch);
    REQUIRE(framer.nextPacket(out));
    CHECK(out == exact);
    framer.sendPacket(ch, oversized);
    CHECK(framer.sendStatus() == PacketIOStatus::Oversized);
    CHECK(io.sendCalls == 0);
    framer.sendPacket(ch, exact);
    REQUIRE(io.takeSent(out));
    CHECK(out == exact);
    CHECK(framer.capacity() == 65535);
}

TEST_CASE("double bounds both record count and bytes including partial and scheduled data") {
    PacketIODouble io(8, 2, 5);
    REQUIRE(io.enqueuePartial({1, 2, 3}, 6, 1));
    REQUIRE(io.enqueue({4, 5}, 2));
    CHECK_FALSE(io.enqueue({}));
    CHECK_FALSE(io.appendFront({6}, false));
    CHECK(io.rxBytes() == 5);
    CHECK(io.rxRecords() == 2);
    io.advance();
    CHECK(io.advanceWork == 2);
    CHECK(io.reset() == PacketIOStatus::Ok);
    REQUIRE(io.enqueue({1, 2, 3, 4, 5}));
    CHECK_FALSE(io.enqueue({6}));
    CHECK(io.rxRecords() == 1);
    CHECK(io.rxBytes() == 5);
}

TEST_CASE("transmit queue enforces record and byte backpressure without partial acceptance or replay") {
    for (bool byteBound : {false, true}) {
        PacketIODouble io(8, byteBound ? 4 : 1, byteBound ? 3 : 32);
        PacketChannel ch(&io);
        NativeFramer framer;
        framer.sendPacket(ch, {1, 2, 3});
        CHECK(framer.sendStatus() == PacketIOStatus::Ok);
        framer.sendPacket(ch, {4});
        CHECK(framer.sendStatus() == PacketIOStatus::Backpressure);
        CHECK(io.sendCalls == 2);
        CHECK(io.txRecords() == 1);
        CHECK(io.txBytes() == 3);
        ByteBuffer out;
        REQUIRE(io.takeSent(out));
        CHECK(out == ByteBuffer{1, 2, 3});
        for (unsigned i = 0; i < 3; ++i) framer.poll(ch);
        CHECK_FALSE(io.takeSent(out));
        CHECK(io.sendCalls == 2);
        framer.sendPacket(ch, {5});
        REQUIRE(io.takeSent(out));
        CHECK(out == ByteBuffer{5});
    }
}

TEST_CASE("unavailable peer and definite send failure are separate from receive outcomes") {
    PacketIODouble io;
    PacketChannel ch(&io);
    NativeFramer framer;
    io.peerAvailable = false;
    framer.poll(ch);
    CHECK(framer.receiveStatus() == PacketIOStatus::Unavailable);
    framer.sendPacket(ch, {1});
    CHECK(framer.sendStatus() == PacketIOStatus::Unavailable);
    CHECK(io.sendCalls == 1);
    CHECK(io.txBytes() == 0);
    io.peerAvailable = true;
    REQUIRE(io.enqueue({2}));
    framer.poll(ch);
    CHECK(framer.receiveStatus() == PacketIOStatus::Ok);
    REQUIRE(io.setNextSendResult(PacketIOStatus::SendFailed));
    framer.sendPacket(ch, {3});
    CHECK(framer.sendStatus() == PacketIOStatus::SendFailed);
    CHECK(framer.receiveStatus() == PacketIOStatus::Ok);
    CHECK(io.sendCalls == 2);
    CHECK(io.txRecords() == 0);
    ByteBuffer out;
    REQUIRE(framer.nextPacket(out));
    CHECK(out == ByteBuffer{2});
    framer.poll(ch);
    CHECK(framer.receiveStatus() == PacketIOStatus::NoData);
    CHECK(framer.sendStatus() == PacketIOStatus::SendFailed);
    CHECK(io.sendCalls == 2);
    framer.sendPacket(ch, {4});
    CHECK(framer.sendStatus() == PacketIOStatus::Ok);
    REQUIRE(io.takeSent(out));
    CHECK(out == ByteBuffer{4});
}

TEST_CASE("unknown completion blocks send and stale receive across successful local resets") {
    PacketIODouble io;
    PacketChannel ch(&io);
    NativeFramer framer;
    REQUIRE(io.enqueue({1}));
    framer.poll(ch);
    REQUIRE(io.setNextSendResult(PacketIOStatus::UnknownCompletion));
    framer.sendPacket(ch, {2});
    CHECK(framer.sendStatus() == PacketIOStatus::UnknownCompletion);
    CHECK(framer.unknownCompletion());
    ByteBuffer out{9};
    CHECK_FALSE(framer.nextPacket(out));
    CHECK(out.empty());
    framer.sendPacket(ch, {3});
    framer.poll(ch);
    CHECK(framer.receiveStatus() == PacketIOStatus::UnknownCompletion);
    CHECK(io.receiveCalls == 1);
    CHECK(io.sendCalls == 1);
    CHECK(framer.reset(ch) == PacketIOStatus::Ok);
    CHECK(framer.resetStatus() == PacketIOStatus::Ok);
    CHECK(framer.sendStatus() == PacketIOStatus::UnknownCompletion);
    CHECK(framer.unknownCompletion());
    REQUIRE(io.enqueue({4}));
    framer.poll(ch);
    framer.sendPacket(ch, {5});
    CHECK_FALSE(framer.nextPacket(out));
    CHECK(io.receiveCalls == 1);
    CHECK(io.sendCalls == 1);
    CHECK(io.txBytes() == 0);
    // The adapter itself also retains ambiguity if a fresh local framer is made.
    NativeFramer replacement(io);
    replacement.sendPacket(ch, {6});
    CHECK(replacement.sendStatus() == PacketIOStatus::UnknownCompletion);
    CHECK(io.sendCalls == 2);
    CHECK(io.resetCalls == 1);
    NativeFramer receiveFirst(io);
    receiveFirst.poll(ch);
    CHECK(receiveFirst.receiveStatus() == PacketIOStatus::UnknownCompletion);
    CHECK(receiveFirst.unknownCompletion());
    CHECK(receiveFirst.reset(ch) == PacketIOStatus::Ok);
    const auto sends = io.sendCalls, receives = io.receiveCalls;
    receiveFirst.poll(ch);
    receiveFirst.sendPacket(ch, {7});
    CHECK(io.sendCalls == sends);
    CHECK(io.receiveCalls == receives);
}

TEST_CASE("local reset discards ready queued partial scheduled and transmitted data") {
    PacketIODouble io;
    PacketChannel ch(&io);
    NativeFramer framer;
    REQUIRE(io.enqueue({1}));
    framer.poll(ch);
    REQUIRE(io.enqueuePartial({2}, 2));
    REQUIRE(io.enqueue({3}, 1));
    REQUIRE(io.enqueue({4}));
    framer.sendPacket(ch, {5});
    CHECK(framer.reset(ch) == PacketIOStatus::Ok);
    CHECK(io.rxRecords() == 0);
    CHECK(io.txRecords() == 0);
    CHECK(io.rxBytes() == 0);
    CHECK(io.txBytes() == 0);
    ByteBuffer out{9};
    CHECK_FALSE(framer.nextPacket(out));
    CHECK(out.empty());
    io.advance();
    CHECK(io.advanceWork == 0);
    framer.poll(ch);
    CHECK(framer.receiveStatus() == PacketIOStatus::NoData);
    CHECK_FALSE(framer.nextPacket(out));
    CHECK_FALSE(io.appendFront({6}, true));
    REQUIRE(io.enqueue({7}));
    framer.poll(ch);
    REQUIRE(framer.nextPacket(out));
    CHECK(out == ByteBuffer{7});
    framer.sendPacket(ch, {8});
    REQUIRE(io.takeSent(out));
    CHECK(out == ByteBuffer{8});
}

TEST_CASE("reset failure locks I/O and drops the ready slot until successful reset") {
    PacketIODouble io;
    PacketChannel ch(&io);
    NativeFramer framer;
    REQUIRE(io.enqueue({1}));
    framer.poll(ch);
    io.resetFailure = true;
    CHECK(framer.reset(ch) == PacketIOStatus::ResetFailed);
    CHECK(framer.resetStatus() == PacketIOStatus::ResetFailed);
    REQUIRE(io.enqueue({2}));
    framer.poll(ch);
    framer.sendPacket(ch, {3});
    CHECK(framer.receiveStatus() == PacketIOStatus::ResetRequired);
    CHECK(framer.sendStatus() == PacketIOStatus::ResetRequired);
    CHECK(io.receiveCalls == 1);
    CHECK(io.sendCalls == 0);
    ByteBuffer out{9};
    CHECK_FALSE(framer.nextPacket(out));
    CHECK(out.empty());
    io.resetFailure = false;
    CHECK(framer.reset(ch) == PacketIOStatus::Ok);
    io.advance();
    framer.poll(ch);
    CHECK(framer.receiveStatus() == PacketIOStatus::NoData);
    REQUIRE(io.enqueue({4}));
    framer.poll(ch);
    REQUIRE(framer.nextPacket(out));
    CHECK(out == ByteBuffer{4});
}

TEST_CASE("unsupported and zero-capacity channels fail closed without byte I/O") {
    PacketIODouble invalid(0);
    for (auto* adapter : {static_cast<IPacketIO*>(nullptr), static_cast<IPacketIO*>(&invalid)}) {
        PacketChannel ch(adapter);
        NativeFramer framer;
        const auto status = adapter ? PacketIOStatus::InvalidCapacity : PacketIOStatus::Unsupported;
        framer.poll(ch);
        CHECK(framer.receiveStatus() == status);
        framer.sendPacket(ch, {1});
        CHECK(framer.sendStatus() == status);
        CHECK(framer.reset(ch) == status);
        ByteBuffer out{9};
        CHECK_FALSE(framer.nextPacket(out));
        CHECK(out.empty());
        CHECK(framer.capacity() == 0);
        CHECK(ch.byteCalls == 0);
    }
    CHECK(invalid.receiveCalls == 0);
    CHECK(invalid.sendCalls == 0);
    CHECK(invalid.resetCalls == 0);
}

TEST_CASE("adapter identity change cannot expose the old receive slot or reset another adapter") {
    for (unsigned mutationPath : {0, 1, 2}) {
        PacketIODouble original, other;
        PacketChannel ch(&original);
        NativeFramer framer;
        REQUIRE(original.enqueue({1}));
        framer.poll(ch);
        ch.adapter = &other;
        if (mutationPath == 1) {
            framer.sendPacket(ch, {2});
            CHECK(framer.sendStatus() == PacketIOStatus::AdapterChanged);
        } else if (mutationPath == 2) {
            ByteBuffer stale{9};
            CHECK_FALSE(framer.nextPacket(stale));
            CHECK(stale.empty());
            CHECK(framer.receiveStatus() == PacketIOStatus::AdapterChanged);
        } else {
            framer.poll(ch);
            CHECK(framer.receiveStatus() == PacketIOStatus::AdapterChanged);
        }
        ByteBuffer out{9};
        CHECK_FALSE(framer.nextPacket(out));
        CHECK(out.empty());
        CHECK(framer.reset(ch) == PacketIOStatus::AdapterChanged);
        CHECK(other.resetCalls == 0);
        CHECK(other.receiveCalls == 0);
        CHECK(other.sendCalls == 0);
        ch.adapter = &original;
        framer.poll(ch);
        CHECK(framer.receiveStatus() == PacketIOStatus::ResetRequired);
        CHECK(framer.reset(ch) == PacketIOStatus::Ok);
        REQUIRE(original.enqueue({3}));
        framer.poll(ch);
        REQUIRE(framer.nextPacket(out));
        CHECK(out == ByteBuffer{3});
        CHECK(ch.byteCalls == 0);
    }
}

TEST_CASE("real FujiBus transport preserves literal packets and exposes native send failure") {
    PacketIODouble io;
    PacketChannel ch(&io);
    NativeFramer framer;
    FujiBusTransport transport(ch, framer);
    for (const auto* fixture : {&fujibus_wire_fixtures::success, &fujibus_wire_fixtures::error}) {
        REQUIRE(io.enqueue(fixture->raw));
        transport.poll();
        IOResponse response;
        REQUIRE(transport.receiveResponse(response));
        CHECK(response.deviceId == 0xFB);
        CHECK(response.command == 1);
        CHECK(response.status == (fixture == &fujibus_wire_fixtures::success ? StatusCode::Ok : StatusCode::IOError));
        CHECK(response.payload == ByteBuffer{0, 0xC0, 0xDB, 0xFF});
        transport.send(response);
        CHECK(framer.sendStatus() == PacketIOStatus::Ok);
        ByteBuffer out;
        REQUIRE(io.takeSent(out));
        CHECK(out == fixture->raw);
        REQUIRE(io.setNextSendResult(PacketIOStatus::SendFailed));
        const auto calls = io.sendCalls;
        transport.send(response);
        CHECK(framer.sendStatus() == PacketIOStatus::SendFailed);
        CHECK(io.sendCalls == calls + 1);
        CHECK_FALSE(io.takeSent(out));
        transport.poll();
        CHECK(framer.sendStatus() == PacketIOStatus::SendFailed);
        CHECK(io.sendCalls == calls + 1);
    }
    CHECK(ch.byteCalls == 0);
}

TEST_CASE("real transport serialization rejection cannot leave native send success stale") {
    PacketIODouble io;
    PacketChannel ch(&io);
    NativeFramer framer;
    FujiBusTransport transport(ch, framer);
    IOResponse response;
    response.deviceId = 1;
    response.command = 2;
    response.status = StatusCode::Ok;
    transport.send(response);
    CHECK(framer.sendStatus() == PacketIOStatus::Ok);
    CHECK(io.sendCalls == 1);
    response.payload.resize(65536, 1);
    transport.send(response);
    CHECK(framer.sendStatus() == PacketIOStatus::EmptyPacket);
    CHECK(io.sendCalls == 1);
    CHECK(io.txRecords() == 1);
    CHECK(ch.byteCalls == 0);
}

TEST_CASE("native bootstrap requires usable packet capability and serial bootstrap remains available") {
    fujinet::build::BuildProfile profile{};
    profile.primaryTransport = fujinet::build::TransportKind::FujiBusNative;
    // The existing Zorro profile selects a byte-only PTY: no native fallback.
    profile.primaryChannel = fujinet::build::ChannelKind::Pty;
    fujinet::core::FujinetCore core;
    PacketChannel unsupported;
    CHECK(fujinet::core::setup_transports(core, unsupported, profile) == nullptr);
    PacketIODouble invalid(0), valid;
    PacketChannel invalidChannel(&invalid), validChannel(&valid);
    CHECK(fujinet::core::setup_transports(core, invalidChannel, profile) == nullptr);
    auto* native = fujinet::core::setup_transports(core, validChannel, profile);
    REQUIRE(native != nullptr);
    REQUIRE(valid.enqueue(fujibus_wire_fixtures::minimum.raw));
    native->poll();
    IORequest request;
    REQUIRE(native->receive(request));
    CHECK(request.deviceId == 1);
    CHECK(request.command == 2);
    CHECK(validChannel.byteCalls == 0);
    CHECK(unsupported.byteCalls == 0);
    CHECK(invalidChannel.byteCalls == 0);
    profile.primaryTransport = fujinet::build::TransportKind::FujiBusSlip;
    CHECK(fujinet::core::setup_transports(core, unsupported, profile) != nullptr);
}

TEST_CASE("framer latches adapter reset-required outcomes independently of adapter behavior") {
    for (bool sendFault : {false, true}) {
        ScriptedPacketIO io;
        PacketChannel ch(&io);
        NativeFramer framer;
        if (sendFault) {
            framer.poll(ch); // A ready slot must be invalidated by the send fault.
            io.sendResult = PacketIOStatus::ResetRequired;
            framer.sendPacket(ch, {1});
        } else {
            io.receiveResult = {PacketIOStatus::ResetRequired};
            framer.poll(ch);
        }
        const auto receives = io.receives, sends = io.sends;
        io.receiveResult = {PacketIOStatus::Ok, 1};
        io.sendResult = PacketIOStatus::Ok;
        framer.poll(ch);
        framer.sendPacket(ch, {2});
        CHECK(framer.receiveStatus() == PacketIOStatus::ResetRequired);
        CHECK(framer.sendStatus() == PacketIOStatus::ResetRequired);
        CHECK(io.receives == receives);
        CHECK(io.sends == sends);
        ByteBuffer out{9};
        CHECK_FALSE(framer.nextPacket(out));
        CHECK(out.empty());
        REQUIRE(framer.reset(ch) == PacketIOStatus::Ok);
        framer.poll(ch);
        REQUIRE(framer.nextPacket(out));
        CHECK(out == ByteBuffer{0xAB});
    }
}

TEST_CASE("unknown completion reported by reset remains latched after a later successful reset") {
    ScriptedPacketIO io;
    PacketChannel ch(&io);
    NativeFramer framer;
    framer.poll(ch);
    io.resetResult = PacketIOStatus::UnknownCompletion;
    CHECK(framer.reset(ch) == PacketIOStatus::UnknownCompletion);
    CHECK(framer.unknownCompletion());
    io.resetResult = PacketIOStatus::Ok;
    CHECK(framer.reset(ch) == PacketIOStatus::Ok);
    framer.poll(ch);
    framer.sendPacket(ch, {1});
    CHECK(framer.receiveStatus() == PacketIOStatus::UnknownCompletion);
    CHECK(framer.sendStatus() == PacketIOStatus::UnknownCompletion);
    CHECK(io.receives == 1);
    CHECK(io.sends == 0);
    ByteBuffer out{9};
    CHECK_FALSE(framer.nextPacket(out));
    CHECK(out.empty());
}

TEST_CASE("double validates send fault injection and latches reset-required state") {
    PacketIODouble io;
    for (const auto invalid : {PacketIOStatus::NoData, PacketIOStatus::Incomplete,
                              PacketIOStatus::Truncated, PacketIOStatus::ResetFailed,
                              PacketIOStatus::Unsupported, PacketIOStatus::InvalidCapacity,
                              PacketIOStatus::AdapterChanged}) {
        CHECK_FALSE(io.setNextSendResult(invalid));
    }
    CHECK_FALSE(io.setNextSendResult(PacketIOStatus::SendFailed, true));
    const std::uint8_t data = 1;
    for (const auto legal : {PacketIOStatus::Ok, PacketIOStatus::Backpressure,
                            PacketIOStatus::Unavailable, PacketIOStatus::SendFailed,
                            PacketIOStatus::EmptyPacket, PacketIOStatus::Oversized}) {
        REQUIRE(io.setNextSendResult(legal));
        CHECK(io.send(&data, 1) == legal);
    }
    CHECK(io.acceptedCount == 1);
    REQUIRE(io.setNextSendResult(PacketIOStatus::ResetRequired));
    CHECK(io.send(&data, 1) == PacketIOStatus::ResetRequired);
    REQUIRE(io.setNextSendResult(PacketIOStatus::Ok));
    CHECK(io.send(&data, 1) == PacketIOStatus::ResetRequired);
    std::uint8_t out = 0;
    CHECK(io.receive(&out, 1).status == PacketIOStatus::ResetRequired);
    REQUIRE(io.reset() == PacketIOStatus::Ok);
    CHECK(io.send(&data, 1) == PacketIOStatus::Ok);
    CHECK(io.acceptedCount == 2);
}

TEST_CASE("locally accepted ambiguous send cannot be accepted again after reset or retry") {
    PacketIODouble io;
    PacketChannel ch(&io);
    NativeFramer framer;
    REQUIRE(io.setNextSendResult(PacketIOStatus::UnknownCompletion, true));
    framer.sendPacket(ch, {1, 2, 3});
    CHECK(framer.sendStatus() == PacketIOStatus::UnknownCompletion);
    CHECK(io.acceptedCount == 1);
    CHECK(io.txRecords() == 1);
    CHECK(io.txBytes() == 3);
    REQUIRE(framer.reset(ch) == PacketIOStatus::Ok);
    CHECK(io.txRecords() == 0);
    for (unsigned i = 0; i < 3; ++i) framer.sendPacket(ch, {1, 2, 3});
    CHECK(io.sendCalls == 1);
    CHECK(io.acceptedCount == 1);
    const std::uint8_t data = 1;
    CHECK(io.send(&data, 1) == PacketIOStatus::UnknownCompletion);
    CHECK(io.acceptedCount == 1);
}

TEST_CASE("failed reset can retain bounded local state but none is accessible before successful reset") {
    PacketIODouble io;
    PacketChannel ch(&io);
    NativeFramer framer;
    REQUIRE(io.enqueue({1}));
    framer.poll(ch);
    REQUIRE(io.enqueuePartial({2}, 2));
    REQUIRE(io.enqueue({3}, 1));
    REQUIRE(io.enqueue({4}));
    framer.sendPacket(ch, {5});
    io.resetFailure = io.retainOnResetFailure = true;
    CHECK(framer.reset(ch) == PacketIOStatus::ResetFailed);
    CHECK(io.rxRecords() == 3);
    CHECK(io.rxBytes() == 3);
    CHECK(io.txRecords() == 1);
    io.advance();
    REQUIRE(io.appendFront({6}, true));
    framer.poll(ch);
    framer.sendPacket(ch, {7});
    CHECK(io.receiveCalls == 1);
    CHECK(io.sendCalls == 1);
    ByteBuffer out{9};
    CHECK_FALSE(framer.nextPacket(out));
    CHECK(out.empty());
    CHECK_FALSE(io.takeSent(out));
    std::uint8_t buffer[8]{};
    CHECK(io.receive(buffer, sizeof(buffer)).status == PacketIOStatus::ResetRequired);
    CHECK(io.rxRecords() == 3);
    io.resetFailure = false;
    REQUIRE(framer.reset(ch) == PacketIOStatus::Ok);
    CHECK(io.rxRecords() == 0);
    CHECK(io.txRecords() == 0);
    CHECK(io.rxBytes() == 0);
    io.advance();
    framer.poll(ch);
    CHECK(framer.receiveStatus() == PacketIOStatus::NoData);
    CHECK_FALSE(framer.nextPacket(out));
}

TEST_CASE("accepted send and extracted receive buffers have independent ownership") {
    PacketIODouble io;
    PacketChannel ch(&io);
    NativeFramer framer;
    ByteBuffer source{1, 2, 3};
    framer.sendPacket(ch, source);
    source.assign({9});
    ByteBuffer sent;
    REQUIRE(io.takeSent(sent));
    CHECK(sent == ByteBuffer{1, 2, 3});
    REQUIRE(io.enqueue({4, 5}));
    framer.poll(ch);
    ByteBuffer first;
    REQUIRE(framer.nextPacket(first));
    REQUIRE(io.enqueue({6}));
    framer.poll(ch);
    ByteBuffer second;
    REQUIRE(framer.nextPacket(second));
    REQUIRE(framer.reset(ch) == PacketIOStatus::Ok);
    CHECK(first == ByteBuffer{4, 5});
    CHECK(second == ByteBuffer{6});
}

TEST_CASE("invalid successful receive lengths never expose buffer contents") {
    ScriptedPacketIO io;
    PacketChannel ch(&io);
    NativeFramer framer;
    for (const auto size : {std::size_t{0}, std::size_t{9}}) {
        io.receiveResult = {PacketIOStatus::Ok, size};
        framer.poll(ch);
        CHECK(framer.receiveStatus() == (size ? PacketIOStatus::Oversized : PacketIOStatus::EmptyPacket));
        ByteBuffer out{9};
        CHECK_FALSE(framer.nextPacket(out));
        CHECK(out.empty());
    }
    io.receiveResult = {PacketIOStatus::Ok, 1};
    framer.poll(ch);
    ByteBuffer out;
    REQUIRE(framer.nextPacket(out));
    CHECK(out == ByteBuffer{0xAB});
}

TEST_CASE("real transport rejects responses above smaller adapter capacity without sending or replay") {
    PacketIODouble io(8);
    PacketChannel ch(&io);
    NativeFramer framer;
    FujiBusTransport transport(ch, framer);
    IOResponse response;
    response.deviceId = 1;
    response.command = 2;
    response.status = StatusCode::Ok;
    response.payload = {1, 2, 3}; // Valid raw serialization, larger than 8 bytes.
    transport.send(response);
    CHECK(framer.sendStatus() == PacketIOStatus::Oversized);
    CHECK(io.sendCalls == 0);
    transport.poll();
    CHECK(io.sendCalls == 0);
    CHECK(io.acceptedCount == 0);
}

TEST_CASE("serial transport drops oversized serialization and later sends valid response bytes") {
    class CaptureChannel : public PacketChannel {
    public:
        void write(const std::uint8_t* bytes, std::size_t size) override {
            tx.insert(tx.end(), bytes, bytes + size);
        }
        ByteBuffer tx;
    } ch;
    SlipFramer framer;
    FujiBusTransport transport(ch, framer);
    IOResponse response;
    response.deviceId = 0xFB;
    response.command = 1;
    response.status = StatusCode::Ok;
    response.payload.resize(65536, 1);
    transport.send(response);
    CHECK(ch.tx.empty());
    response.payload = {0, 0xC0, 0xDB, 0xFF};
    transport.send(response);
    CHECK(ch.tx == fujibus_wire_fixtures::success.slip);
}

} // TEST_SUITE
