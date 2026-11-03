#include "doctest.h"
#include "fujinet/io/stream/replay_slot.h"
#include <array>
#include <vector>

namespace {
using Slot = fujinet::io::stream::ReplaySlot<8, 8>;
using Decision = Slot::Decision;

struct Peer {
    Slot slot;
    unsigned effects{0};
    std::vector<std::uint8_t> call(std::uint16_t sequence,
                                 const std::vector<std::uint8_t>& request,
                                 const std::vector<std::uint8_t>& nextResult) {
        const auto decision = slot.inspect(sequence, request.data(), request.size());
        if (decision == Decision::Reject) return {0xff};
        if (decision == Decision::Execute) {
            REQUIRE(nextResult.size() <= 8);
            ++effects; // socket consumption/enqueue happens only on Execute
            REQUIRE(slot.commit(sequence, request.data(), request.size(),
                                nextResult.data(), nextResult.size()));
        }
        return {slot.result(), slot.result() + slot.result_size()};
    }
};
}

TEST_CASE("stream lost read reply replays bytes without consuming again") {
    Peer p;
    CHECK(p.call(0, {2, 8}, {0, 3, 'a', 'b', 'c'}) ==
          std::vector<std::uint8_t>{0, 3, 'a', 'b', 'c'});
    for (unsigned retry = 0; retry < 10; ++retry)
        CHECK(p.call(0, {2, 8}, {0, 1, 'd'}) ==
              std::vector<std::uint8_t>{0, 3, 'a', 'b', 'c'});
    CHECK(p.effects == 1);
    CHECK(p.call(1, {2, 8}, {0, 1, 'd'}) == std::vector<std::uint8_t>{0, 1, 'd'});
    CHECK(p.effects == 2);
}

TEST_CASE("stream lost write completion preserves partial count and rejects altered retries") {
    Peer p;
    CHECK(p.call(0, {3, 'a', 'b', 'c'}, {0, 2}) == std::vector<std::uint8_t>{0, 2});
    CHECK(p.call(0, {3, 'a', 'b', 'c'}, {0, 3}) == std::vector<std::uint8_t>{0, 2});
    CHECK(p.call(0, {3, 'a', 'b', 'd'}, {0, 3}) == std::vector<std::uint8_t>{0xff});
    CHECK(p.call(0, {3, 'a', 'b'}, {0, 2}) == std::vector<std::uint8_t>{0xff});
    CHECK(p.effects == 1);
    CHECK(p.call(1, {3, 'c'}, {0, 1}) == std::vector<std::uint8_t>{0, 1});
    CHECK(p.effects == 2);
}

TEST_CASE("stream would-block is stable until a new operation sequence") {
    Peer p;
    CHECK(p.call(0, {2, 8}, {4}) == std::vector<std::uint8_t>{4});
    CHECK(p.call(0, {2, 8}, {0, 1, 'x'}) == std::vector<std::uint8_t>{4});
    CHECK(p.effects == 1);
    CHECK(p.call(1, {2, 8}, {0, 1, 'x'}) == std::vector<std::uint8_t>{0, 1, 'x'});
    CHECK(p.effects == 2);
}

TEST_CASE("stream rejects gaps and old operations without discarding retained reply") {
    Peer p;
    CHECK(p.call(1, {4}, {0}) == std::vector<std::uint8_t>{0xff});
    p.call(0, {2}, {0, 'x'});
    CHECK(p.call(2, {4}, {0}) == std::vector<std::uint8_t>{0xff});
    CHECK(p.call(0, {2}, {}) == std::vector<std::uint8_t>{0, 'x'});
    p.call(1, {4}, {0});
    CHECK(p.call(0, {2}, {}) == std::vector<std::uint8_t>{0xff});
    CHECK(p.call(1, {4}, {1}) == std::vector<std::uint8_t>{0});
    CHECK(p.effects == 2);
}

TEST_CASE("stream replay has fixed bounds and failed commit preserves previous result") {
    Slot slot;
    std::array<std::uint8_t, 9> bytes{};
    CHECK(slot.inspect(0, bytes.data(), 9) == Decision::Reject);
    CHECK(slot.inspect(0, nullptr, 1) == Decision::Reject);
    CHECK_FALSE(slot.commit(0, bytes.data(), 8, bytes.data(), 9));
    CHECK_FALSE(slot.commit(0, bytes.data(), 8, nullptr, 1));
    REQUIRE(slot.commit(0, bytes.data(), 8, bytes.data(), 8));
    CHECK_FALSE(slot.commit(1, bytes.data(), 8, bytes.data(), 9));
    CHECK(slot.inspect(0, bytes.data(), 8) == Decision::Replay);
    CHECK(slot.result_size() == 8);
    CHECK_FALSE(slot.commit(0, bytes.data(), 8, bytes.data(), 8));
}

TEST_CASE("stream sequence cannot silently wrap and reset starts a fresh session") {
    Slot slot;
    for (unsigned n = 0; n <= 65535; ++n) {
        REQUIRE(slot.inspect(static_cast<std::uint16_t>(n), nullptr, 0) == Decision::Execute);
        REQUIRE(slot.commit(static_cast<std::uint16_t>(n), nullptr, 0, nullptr, 0));
    }
    CHECK(slot.inspect(65535, nullptr, 0) == Decision::Replay);
    CHECK(slot.inspect(0, nullptr, 0) == Decision::Reject);
    slot.reset();
    CHECK(slot.result_size() == 0);
    CHECK(slot.inspect(65535, nullptr, 0) == Decision::Reject);
    CHECK(slot.inspect(0, nullptr, 0) == Decision::Execute);
}
