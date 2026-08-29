#include "doctest.h"

#include "fujinet/io/transport/iframer.h"

using namespace fujinet::io;

// Minimal concrete IFramer to confirm the interface is implementable.
class StubFramer : public IFramer {
public:
    void poll(Channel&) override {}
    bool nextPacket(ByteBuffer& out) override { out = _packet; return !_packet.empty(); }
    void sendPacket(Channel&, const ByteBuffer&) override {}

    void enqueue(ByteBuffer pkt) { _packet = std::move(pkt); }
private:
    ByteBuffer _packet;
};

TEST_CASE("IFramer interface is implementable and self-contained") {
    StubFramer framer;

    ByteBuffer out;
    CHECK_FALSE(framer.nextPacket(out));

    framer.enqueue({0x01, 0x02, 0x03});
    CHECK(framer.nextPacket(out));
    CHECK(out == ByteBuffer{0x01, 0x02, 0x03});
}

// Static assertion: IFramer is abstract (cannot be instantiated directly).
static_assert(!std::is_constructible_v<IFramer>,
    "IFramer must be a pure abstract class");
