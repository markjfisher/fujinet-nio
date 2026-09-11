#include "doctest.h"
#include "fujinet/io/protocol/fuji_bus_packet.h"

using namespace fujinet::io::protocol;

static ByteBuffer corrupt_one_byte(ByteBuffer buf)
{
    if (buf.size() > 5)
        buf[5] ^= 0xFF;
    return buf;
}

static FujiBusPacket make_reference_packet()
{
    auto dev = static_cast<WireDeviceId>(42);
    auto cmd = static_cast<std::uint8_t>(99);

    // 3 params of different sizes + no payload
    return FujiBusPacket(dev, cmd,
                         std::uint8_t{0x11},
                         std::uint16_t{0x2233},
                         std::uint32_t{0x44556677});
}

// Compares "interesting" user-visible fields of a packet
static void check_packets_equal(const FujiBusPacket& a, const FujiBusPacket& b)
{
    CHECK(a.device()     == b.device());
    CHECK(a.command()    == b.command());
    CHECK(a.paramCount() == b.paramCount());

    for (unsigned i = 0; i < a.paramCount(); ++i)
        CHECK(a.param(i) == b.param(i));

    auto da = a.data();
    auto db = b.data();

    CHECK(da.has_value() == db.has_value());
    if (da && db)
    {
        // compare as bytes via string_data_or("") or similar if you’ve added that helper
        std::string sa(da->begin(), da->end());
        std::string sb(db->begin(), db->end());
        CHECK(sa == sb);
    }
}

static void check_slip_framed(const ByteBuffer& bytes)
{
    REQUIRE(!bytes.empty());
    CHECK(bytes.front() == to_byte(SlipByte::End));
    CHECK(bytes.back()  == to_byte(SlipByte::End));
}

// --------------------------------------------------------------------------------
// TEST CASES
// --------------------------------------------------------------------------------

TEST_CASE("serialize() produces a SLIP-framed packet")
{
    // Use some arbitrary device/command IDs:
    auto dev = static_cast<WireDeviceId>(1);
    auto cmd = static_cast<std::uint8_t>(2);

    FujiBusPacket pkt(dev, cmd, std::uint8_t{0x12}, std::uint16_t{0x3456});
    ByteBuffer serialized = pkt.serialize();

    check_slip_framed(serialized);
}

TEST_CASE("simple roundtrip: no payload, a few params")
{
    auto dev = static_cast<WireDeviceId>(1);
    auto cmd = static_cast<std::uint8_t>(2);

    FujiBusPacket pkt(dev, cmd,
                      std::uint8_t{0x11},
                      std::uint16_t{0x2233},
                      std::uint32_t{0x44556677});

    ByteBuffer serialized = pkt.serialize();

    // SLIP framing
    check_slip_framed(serialized);

    auto parsed = FujiBusPacket::fromSerialized(serialized);
    REQUIRE(parsed);

    CHECK(parsed->device()     == dev);
    CHECK(parsed->command()    == cmd);
    CHECK(parsed->paramCount() == 3);
    CHECK(parsed->param(0)     == 0x11);
    CHECK(parsed->param(1)     == 0x2233);
    CHECK(parsed->param(2)     == 0x44556677u);
    CHECK_FALSE(parsed->data().has_value());
}

TEST_CASE("roundtrip with binary payload (includes SLIP specials)")
{
    auto dev = static_cast<WireDeviceId>(3);
    auto cmd = static_cast<std::uint8_t>(4);

    ByteBuffer payload{0x00, 0xC0, 0xDB, 0xFF}; // includes SLIP_END and SLIP_ESCAPE

    FujiBusPacket pkt(dev, cmd,
                      std::uint8_t{0xAA},
                      payload);

    ByteBuffer serialized = pkt.serialize();

    // still SLIP framed even with special bytes inside
    check_slip_framed(serialized);

    auto parsed = FujiBusPacket::fromSerialized(serialized);
    REQUIRE(parsed);

    CHECK(parsed->device()     == dev);
    CHECK(parsed->command()    == cmd);
    CHECK(parsed->paramCount() == 1);
    CHECK(parsed->param(0)     == 0xAA);

    REQUIRE(parsed->data().has_value());
    const ByteBuffer& back = *parsed->data();
    REQUIRE(back.size() == payload.size());
    CHECK(std::equal(back.begin(), back.end(), payload.begin()));
}

TEST_CASE("roundtrip with textual payload via std::string")
{
    auto dev = static_cast<WireDeviceId>(5);
    auto cmd = static_cast<std::uint8_t>(6);

    std::string tz = "Europe/London";

    FujiBusPacket pkt(dev, cmd,
                      std::uint16_t{0x1234},
                      tz); // uses string overload

    ByteBuffer serialized = pkt.serialize();

    auto parsed = FujiBusPacket::fromSerialized(serialized);
    REQUIRE(parsed);

    CHECK(parsed->device()     == dev);
    CHECK(parsed->command()    == cmd);
    CHECK(parsed->paramCount() == 1);
    CHECK(parsed->param(0)     == 0x1234);

    auto tz_opt = parsed->dataAsString();
    REQUIRE(tz_opt.has_value());
    CHECK(tz_opt.value() == tz);
}

TEST_CASE("checksum detects corruption")
{
    auto dev = static_cast<WireDeviceId>(7);
    auto cmd = static_cast<std::uint8_t>(8);

    FujiBusPacket pkt(dev, cmd,
                      std::uint8_t{0x10},
                      std::uint8_t{0x20},
                      ByteBuffer{0x01, 0x02, 0x03, 0x04});

    ByteBuffer serialized = pkt.serialize();

    auto ok = FujiBusPacket::fromSerialized(serialized);
    REQUIRE(ok);

    ByteBuffer bad = corrupt_one_byte(serialized);
    auto broken = FujiBusPacket::fromSerialized(bad);
    CHECK(broken == nullptr); // parse should fail due to checksum mismatch
}

TEST_CASE("multiple descriptors: many u8 followed by u16")
{
    auto dev = static_cast<WireDeviceId>(9);
    auto cmd = static_cast<std::uint8_t>(10);

    // 5 uint8_t params forces:
    // - first descriptor: 4×u8
    // - second descriptor: 1×u8
    FujiBusPacket pkt(dev, cmd,
                      std::uint8_t{1},
                      std::uint8_t{2},
                      std::uint8_t{3},
                      std::uint8_t{4},
                      std::uint8_t{5},
                      std::uint16_t{0xABCD});

    ByteBuffer serialized = pkt.serialize();

    auto parsed = FujiBusPacket::fromSerialized(serialized);
    REQUIRE(parsed);

    CHECK(parsed->device()     == dev);
    CHECK(parsed->command()    == cmd);
    CHECK(parsed->paramCount() == 6);

    CHECK(parsed->param(0) == 1);
    CHECK(parsed->param(1) == 2);
    CHECK(parsed->param(2) == 3);
    CHECK(parsed->param(3) == 4);
    CHECK(parsed->param(4) == 5);
    CHECK(parsed->param(5) == 0xABCD);
}

TEST_CASE("parser skips junk before first SLIP_END")
{
    FujiBusPacket refPkt = make_reference_packet();
    ByteBuffer clean = refPkt.serialize();

    // Prepend a bunch of garbage bytes before the valid SLIP frame
    ByteBuffer noisy;
    noisy.push_back(0x00);
    noisy.push_back(0xAA);
    noisy.push_back(0x55);
    noisy.push_back(0xFF);
    noisy.insert(noisy.end(), clean.begin(), clean.end());

    // Parsing the noisy buffer should still succeed and match the reference packet
    auto parsed = FujiBusPacket::fromSerialized(noisy);
    REQUIRE(parsed);

    check_packets_equal(refPkt, *parsed);
}

TEST_CASE("invalid: no SLIP_END at all")
{
    ByteBuffer buf{0x01, 0x02, 0x03, 0x04, 0x05}; // totally bogus

    auto parsed = FujiBusPacket::fromSerialized(buf);
    CHECK(parsed == nullptr);
}

TEST_CASE("invalid: SLIP framed but decoded data too short for header")
{
    // After SLIP decode, we'll just get the middle bytes.
    ByteBuffer buf{to_byte(SlipByte::End), 0x01, 0x02, to_byte(SlipByte::End)};

    auto parsed = FujiBusPacket::fromSerialized(buf);
    CHECK(parsed == nullptr);
}

TEST_CASE("invalid: SLIP frame with only END markers")
{
    ByteBuffer buf{to_byte(SlipByte::End), to_byte(SlipByte::End)}; // decodes to empty payload

    auto parsed = FujiBusPacket::fromSerialized(buf);
    CHECK(parsed == nullptr);
}

TEST_CASE("invalid: missing trailing SLIP_END")
{
    FujiBusPacket pkt = make_reference_packet();
    ByteBuffer serialized = pkt.serialize();

    REQUIRE(serialized.size() >= 2);
    serialized.pop_back(); // drop last SLIP_END

    auto parsed = FujiBusPacket::fromSerialized(serialized);
    CHECK(parsed == nullptr);
}

TEST_CASE("invalid: missing leading SLIP_END and no other SLIP_END")
{
    FujiBusPacket pkt = make_reference_packet();
    ByteBuffer serialized = pkt.serialize();

    // serialized should be: END ... END
    check_slip_framed(serialized);

    // Drop the first SLIP_END and *also* the last so there are none
    serialized.erase(serialized.begin());   // remove first END
    serialized.pop_back();                  // remove last END

    auto parsed = FujiBusPacket::fromSerialized(serialized);
    CHECK(parsed == nullptr);
}

#include "fujibus_wire_fixtures.h"

// Only map SLIP escapes: no packet fields or checksums are synthesized here.
static ByteBuffer literal_slip_partner(const ByteBuffer& raw)
{
    ByteBuffer slip{0xC0};
    for (auto byte : raw) {
        if (byte == 0xC0) {
            slip.insert(slip.end(), {0xDB, 0xDC});
        } else if (byte == 0xDB) {
            slip.insert(slip.end(), {0xDB, 0xDD});
        } else {
            slip.push_back(byte);
        }
    }
    slip.push_back(0xC0);
    return slip;
}

TEST_CASE("literal FujiBus raw and SLIP fixture partners agree")
{
    namespace fixtures = fujibus_wire_fixtures;
    for (const auto* fixture : {&fixtures::minimum, &fixtures::typed,
                               &fixtures::binary, &fixtures::success, &fixtures::error}) {
        CAPTURE(fixture->name);
        CHECK(literal_slip_partner(fixture->raw) == fixture->slip);
    }
    for (const auto& fixture : fixtures::malformed) {
        CAPTURE(fixture.name);
        CHECK(literal_slip_partner(fixture.raw) == fixture.slip);
    }
}

TEST_CASE("literal FujiBus minimum header serialization and parsing")
{
    const auto& fixture = fujibus_wire_fixtures::minimum;
    FujiBusPacket packet(static_cast<WireDeviceId>(0x01), 0x02);
    CHECK(packet.serialize() == fixture.slip);
    auto parsed = FujiBusPacket::fromSerialized(fixture.slip);
    REQUIRE(parsed);
    CHECK(static_cast<std::uint8_t>(parsed->device()) == 0x01);
    CHECK(parsed->command() == 0x02);
    CHECK(parsed->paramCount() == 0);
    CHECK_FALSE(parsed->data().has_value());
    CHECK(parsed->serialize() == fixture.slip);
}

TEST_CASE("literal FujiBus descriptors cover every count and width index")
{
    const auto& fixture = fujibus_wire_fixtures::typed;
    FujiBusPacket packet(static_cast<WireDeviceId>(0x2A), 0x63);
    packet.addParamU8(0x11).addParamU16(0x2233)
          .addParamU8(0x21).addParamU8(0x22)
          .addParamU16(0x4455).addParamU16(0x6677)
          .addParamU8(0x31).addParamU8(0x32).addParamU8(0x33)
          .addParamU32(0x01020304)
          .addParamU8(0x41).addParamU8(0x42).addParamU8(0x43).addParamU8(0x44);
    CHECK(packet.serialize() == fixture.slip);
    auto parsed = FujiBusPacket::fromSerialized(fixture.slip);
    REQUIRE(parsed);
    CHECK(static_cast<std::uint8_t>(parsed->device()) == 0x2A);
    CHECK(parsed->command() == 0x63);
    const std::uint32_t values[] = {0x11, 0x2233, 0x21, 0x22, 0x4455, 0x6677,
                                   0x31, 0x32, 0x33, 0x01020304, 0x41, 0x42, 0x43, 0x44};
    const bool is_u8[] = {true, false, true, true, false, false,
                          true, true, true, false, true, true, true, true};
    REQUIRE(parsed->paramCount() == 14);
    for (unsigned i = 0; i < 14; ++i) {
        CAPTURE(i);
        CHECK(parsed->param(i) == values[i]);
        std::uint8_t value = 0;
        CHECK(parsed->tryParamU8(i, value) == is_u8[i]);
        if (is_u8[i]) CHECK(value == values[i]);
    }
    CHECK_FALSE(parsed->data().has_value());
    // Exact re-serialization also locks the U16/U32 widths and descriptor grouping.
    CHECK(parsed->serialize() == fixture.slip);
}

TEST_CASE("literal FujiBus binary payload escaping and carry checksum")
{
    const auto& fixture = fujibus_wire_fixtures::binary;
    FujiBusPacket packet(static_cast<WireDeviceId>(0x03), 0x04);
    packet.setData({0x00, 0xC0, 0xDB, 0xFF});
    CHECK(packet.serialize() == fixture.slip);
    auto parsed = FujiBusPacket::fromSerialized(fixture.slip);
    REQUIRE(parsed);
    CHECK(static_cast<std::uint8_t>(parsed->device()) == 0x03);
    CHECK(parsed->command() == 0x04);
    CHECK(parsed->paramCount() == 0);
    REQUIRE(parsed->data().has_value());
    CHECK(*parsed->data() == ByteBuffer{0x00, 0xC0, 0xDB, 0xFF});
    CHECK(parsed->serialize() == fixture.slip);
}

TEST_CASE("literal FujiBus checksum-only corruption is rejected")
{
    auto corrupt = fujibus_wire_fixtures::minimum.slip;
    corrupt[5] = 0x08; // Only checksum changes: correct literal is 09.
    CHECK(FujiBusPacket::fromSerialized(corrupt) == nullptr);
}

TEST_CASE("literal FujiBus checksum-valid structural errors are rejected")
{
    for (const auto& fixture : fujibus_wire_fixtures::malformed) {
        CAPTURE(fixture.name);
        CHECK(FujiBusPacket::fromSerialized(fixture.slip) == nullptr);
    }
}

TEST_CASE("raw APIs consume and produce independent literal vectors")
{
    namespace fixtures = fujibus_wire_fixtures;
    for (const auto* fixture : {&fixtures::minimum, &fixtures::typed,
                               &fixtures::binary, &fixtures::success, &fixtures::error}) {
        CAPTURE(fixture->name);
        auto raw = FujiBusPacket::fromRaw(fixture->raw);
        auto serial = FujiBusPacket::fromSerialized(fixture->slip);
        REQUIRE(raw);
        REQUIRE(serial);
        check_packets_equal(*raw, *serial);
        CHECK(raw->serializeRaw() == fixture->raw);
        CHECK(serial->serializeRaw() == fixture->raw);
        CHECK(raw->serialize() == fixture->slip);
        CHECK(FujiBusPacket::fromRaw(fixture->slip) == nullptr);
    }
    for (const auto* fixture : {&fixtures::success, &fixtures::error}) {
        auto packet = FujiBusPacket::fromRaw(fixture->raw);
        REQUIRE(packet);
        REQUIRE(packet->paramCount() == 1);
        std::uint8_t status = 0xFF;
        CHECK(packet->tryParamU8(0, status));
        CHECK(status == (fixture == &fixtures::success ? 0 : 5));
        REQUIRE(packet->data());
        CHECK(*packet->data() == ByteBuffer{0, 0xC0, 0xDB, 0xFF});
    }
}

TEST_CASE("raw framing is explicit even when header bytes are SLIP specials")
{
    // C0+DB+06 = 1A1 -> A2, no parameters or payload.
    const ByteBuffer raw{0xC0, 0xDB, 6, 0, 0xA2, 0};
    auto packet = FujiBusPacket::fromRaw(raw);
    REQUIRE(packet);
    CHECK(static_cast<std::uint8_t>(packet->device()) == 0xC0);
    CHECK(packet->command() == 0xDB);
    CHECK(packet->serializeRaw() == raw);
    CHECK(packet->serialize() == ByteBuffer{0xC0, 0xDB, 0xDC, 0xDB, 0xDD, 6, 0, 0xA2, 0, 0xC0});
    CHECK(FujiBusPacket::fromSerialized(raw) == nullptr);
}

TEST_CASE("raw rejects short checksum and structural failures without a packet")
{
    const auto& minimum = fujibus_wire_fixtures::minimum.raw;
    for (std::size_t length = 0; length < 6; ++length) {
        CHECK(FujiBusPacket::fromRaw(ByteBuffer(minimum.begin(), minimum.begin() + length)) == nullptr);
    }
    auto corrupt = minimum;
    corrupt[4] = 8;
    CHECK(FujiBusPacket::fromRaw(corrupt) == nullptr);
    for (const auto& fixture : fujibus_wire_fixtures::malformed) {
        CAPTURE(fixture.name);
        CHECK(FujiBusPacket::fromRaw(fixture.raw) == nullptr);
    }
    // A valid first parameter followed by a missing U32 must still return null.
    // 1+2+8+81+7+11 = A4.
    CHECK(FujiBusPacket::fromRaw({1, 2, 8, 0, 0xA4, 0x81, 7, 0x11}) == nullptr);
}

TEST_CASE("raw distinguishes trailing bytes outside length from payload inside it")
{
    auto trailing = fujibus_wire_fixtures::minimum.raw;
    trailing.push_back(0); // checksum stays valid but declared length excludes byte
    CHECK(FujiBusPacket::fromRaw(trailing) == nullptr);
    // Included payload: 1+2+7+AA = B4.
    auto packet = FujiBusPacket::fromRaw({1, 2, 7, 0, 0xB4, 0, 0xAA});
    REQUIRE(packet);
    REQUIRE(packet->data());
    CHECK(*packet->data() == ByteBuffer{0xAA});
}

TEST_CASE("raw and serial preserve reserved and zero-count descriptor semantics")
{
    const ByteBuffer inputs[] = {
        {1, 2, 6, 0, 0x81, 0x78}, // reserved bits ignored, zero fields
        {1, 2, 7, 0, 0x8A, 0x80, 0}, // continued zero-count descriptors
        {1, 2, 8, 0, 0xAE, 0xF8, 0x79, 0x31}, // zero count then reserved U8
    };
    for (unsigned i = 0; i < 3; ++i) {
        CAPTURE(i);
        auto raw = FujiBusPacket::fromRaw(inputs[i]);
        auto serial = FujiBusPacket::fromSerialized(literal_slip_partner(inputs[i]));
        REQUIRE(raw);
        REQUIRE(serial);
        check_packets_equal(*raw, *serial);
        CHECK(raw->paramCount() == (i == 2 ? 1 : 0));
        if (i == 2) CHECK(raw->param(0) == 0x31);
        CHECK_FALSE(raw->data());
    }
    // Two continued zero-count descriptors but no final descriptor: 1+2+7+80+80=10A -> 0B.
    const ByteBuffer truncated{1, 2, 7, 0, 0x0B, 0x80, 0x80};
    CHECK(FujiBusPacket::fromRaw(truncated) == nullptr);
    CHECK(FujiBusPacket::fromSerialized(literal_slip_partner(truncated)) == nullptr);
}

TEST_CASE("serial parser retains first-frame and malformed-escape compatibility")
{
    const auto& minimum = fujibus_wire_fixtures::minimum;
    auto check_minimum = [&](const ByteBuffer& bytes) {
        auto packet = FujiBusPacket::fromSerialized(bytes);
        REQUIRE(packet);
        CHECK(packet->serializeRaw() == minimum.raw);
    };
    auto noisy = minimum.slip;
    noisy.insert(noisy.begin(), {0, 0xDB, 0xAA});
    check_minimum(noisy);
    auto consecutive = minimum.slip;
    consecutive.insert(consecutive.begin(), 0xC0);
    CHECK(FujiBusPacket::fromSerialized(consecutive) == nullptr);
    auto trailing = minimum.slip;
    trailing.push_back(0xC0);
    check_minimum(trailing);
    trailing.insert(trailing.end(), fujibus_wire_fixtures::binary.slip.begin(),
                    fujibus_wire_fixtures::binary.slip.end());
    check_minimum(trailing); // first frame wins
    trailing.push_back(0x55);
    CHECK(FujiBusPacket::fromSerialized(trailing) == nullptr); // final END required
    trailing.push_back(0xC0);
    check_minimum(trailing); // even trailing junk is ignored with final END
    auto malformed = minimum.slip;
    malformed.insert(malformed.begin() + 1, {0xDB, 0x42});
    check_minimum(malformed); // both bytes of unknown escape ignored
    auto escaped_end = minimum.slip;
    escaped_end.insert(escaped_end.end() - 1, 0xDB);
    check_minimum(escaped_end); // DB C0 is ignored, consuming the final END
    escaped_end.pop_back();
    CHECK(FujiBusPacket::fromSerialized(escaped_end) == nullptr); // dangling DB
}

TEST_CASE("raw size boundary includes the header parameters and extra descriptors")
{
    for (bool params : {false, true}) {
        CAPTURE(params);
        FujiBusPacket packet(static_cast<WireDeviceId>(1), 2);
        // U8 then U16: extra descriptor + three param bytes, total overhead 10.
        if (params) packet.addParamU8(0).addParamU16(0);
        packet.setData(ByteBuffer(65535 - (params ? 10 : 6), 0));
        ByteBuffer expected{1, 2, 0xFF, 0xFF, static_cast<std::uint8_t>(params ? 0x89 : 3),
                            static_cast<std::uint8_t>(params ? 0x81 : 0)};
        if (params) expected.insert(expected.end(), {5, 0, 0, 0});
        expected.resize(65535, 0);
        CHECK(packet.serializeRaw() == expected);
        auto parsed = FujiBusPacket::fromRaw(expected);
        REQUIRE(parsed);
        CHECK(parsed->serializeRaw() == expected);
        CHECK(packet.serialize() == literal_slip_partner(expected));
        packet.setData(ByteBuffer(65536 - (params ? 10 : 6), 0));
        CHECK(packet.serializeRaw().empty());
        expected.push_back(0);
        expected[2] = expected[3] = 0; // legacy wraps the length; folded FF+FF had no effect
        CHECK(FujiBusPacket::fromRaw(expected) == nullptr);
        CHECK(packet.serialize() == literal_slip_partner(expected));
        CHECK(FujiBusPacket::fromSerialized(packet.serialize()) == nullptr);
    }
}

TEST_CASE("raw rejects parameter-only overflow before building output")
{
    FujiBusPacket packet(static_cast<WireDeviceId>(1), 2);
    // 52424 U8 fields, 13106 descriptors: 6 + 13105 + 52424 = 65535.
    for (unsigned i = 0; i < 52424; ++i) packet.addParamU8(0);
    auto raw = packet.serializeRaw();
    REQUIRE(raw.size() == 65535);
    auto parsed = FujiBusPacket::fromRaw(raw);
    REQUIRE(parsed);
    CHECK(parsed->paramCount() == 52424);
    packet.addParamU8(0); // new descriptor and U8 make 65537 bytes
    CHECK(packet.serializeRaw().empty());
}

TEST_CASE("raw header encodes and parses an asymmetric little-endian length")
{
    // Total 0x0107 = 6 header + 257 zero payload bytes.
    // Independent checksum: device 01 + command 02 + length 07 01 = 0B.
    ByteBuffer expected{0x01, 0x02, 0x07, 0x01, 0x0B, 0x00};
    expected.resize(0x0107, 0);
    FujiBusPacket packet(static_cast<WireDeviceId>(0x01), 0x02);
    packet.setData(ByteBuffer(257, 0));
    CHECK(packet.serializeRaw() == expected);
    auto parsed = FujiBusPacket::fromRaw(expected);
    REQUIRE(parsed);
    CHECK(static_cast<std::uint8_t>(parsed->device()) == 0x01);
    CHECK(parsed->command() == 0x02);
    CHECK(parsed->paramCount() == 0);
    REQUIRE(parsed->data());
    CHECK(*parsed->data() == ByteBuffer(257, 0));
    CHECK(parsed->serializeRaw() == expected);
}
