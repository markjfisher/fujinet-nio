#include "doctest.h"

#include "fujinet/io/iec_bus.h"
#include "iec_test_helpers.h"

#include <algorithm>
#include <string>
#include <vector>

using namespace fujinet::io;
using namespace fujinet::tests;

namespace {

// Records every call a bus makes on a device, as text.
class RecordingDevice final : public IIecDevice {
public:
    bool isPresent{true};
    std::size_t acceptLimit{static_cast<std::size_t>(-1)};
    std::vector<std::uint8_t> toSend;
    std::string log;

    bool present() const override { return isPresent; }
    void open(std::uint8_t channel, const std::uint8_t* name, std::size_t length) override
    {
        log += "open" + std::to_string(channel) + "[" + std::string(reinterpret_cast<const char*>(name), length) + "] ";
    }
    void close(std::uint8_t channel) override { log += "close" + std::to_string(channel) + " "; }
    void listen(std::uint8_t channel) override { log += "listen" + std::to_string(channel) + " "; }
    std::size_t receive(const std::uint8_t* data, std::size_t length, bool eoi) override
    {
        log += "data[" + std::string(reinterpret_cast<const char*>(data), length) + "]" + (eoi ? "E " : " ");
        return length < acceptLimit ? length : acceptLimit;
    }
    void unlisten() override { log += "unlisten "; }
    void talk(std::uint8_t channel) override { log += "talk" + std::to_string(channel) + " "; }
    IecReadResult send(std::uint8_t* dst, std::size_t maxBytes) override
    {
        const std::size_t n = toSend.size() < maxBytes ? toSend.size() : maxBytes;
        std::copy_n(toSend.begin(), n, dst);
        toSend.erase(toSend.begin(), toSend.begin() + static_cast<std::ptrdiff_t>(n));
        log += "send" + std::to_string(n) + " ";
        return IecReadResult{n, toSend.empty()};
    }
    void untalk() override { log += "untalk "; }
    void bus_reset() override { log += "reset "; }
};

constexpr std::uint8_t dev = 8;

} // namespace

TEST_CASE("IEC bus: an OPEN's name runs to UNLISTEN, a byte at a time or all at once")
{
    IecBus bus;
    RecordingDevice device;
    bus.attach(dev, device);

    open_file(bus, dev, 0, "NAME");
    CHECK(device.log == "open0[NAME] ");

    device.log.clear();
    atn(bus, {listen(dev), open_channel(2)});
    const std::string name = "A,S,R";
    CHECK(bus.write(reinterpret_cast<const std::uint8_t*>(name.data()), name.size(), false) == name.size());
    CHECK(device.log.empty());
    atn(bus, {unlisten});
    CHECK(device.log == "open2[A,S,R] ");
}

TEST_CASE("IEC bus: a primary and its secondary may arrive together or apart")
{
    IecBus bus;
    RecordingDevice device;
    bus.attach(dev, device);

    CHECK(atn(bus, {listen(dev), data_channel(2)}) == IecAtnStatus::Acknowledged);
    CHECK(atn(bus, {unlisten}) == IecAtnStatus::Acknowledged);
    CHECK(atn(bus, {listen(dev)}) == IecAtnStatus::Acknowledged);
    CHECK(atn(bus, {data_channel(3)}) == IecAtnStatus::Acknowledged);
    CHECK(atn(bus, {unlisten}) == IecAtnStatus::Acknowledged);
    CHECK(device.log == "listen2 unlisten listen3 unlisten ");
}

TEST_CASE("IEC bus: the channel is the low nibble of a data secondary, bit 4 ignored")
{
    IecBus bus;
    RecordingDevice device;
    bus.attach(dev, device);

    atn(bus, {listen(dev), 0x6F});
    atn(bus, {unlisten});
    atn(bus, {talk(dev), 0x7F});
    atn(bus, {untalk});
    CHECK(device.log == "listen15 unlisten talk15 untalk ");
}

TEST_CASE("IEC bus: LOAD's whole sequence reaches the device in order")
{
    IecBus bus;
    RecordingDevice device;
    device.toSend = {0x01, 0x08, 0xAA};
    bus.attach(dev, device);

    open_file(bus, dev, 0, "F");
    const ReadOutcome read = read_all(bus, dev, 0);
    CHECK(read.bytes == std::vector<std::uint8_t>{0x01, 0x08, 0xAA});
    CHECK(read.eoiOnLastByte);
    atn(bus, {untalk});
    atn(bus, {listen(dev), close_channel(0)});
    atn(bus, {unlisten});
    atn(bus, {unlisten}); // the bridge sends its own after a CLOSE, then the KERNAL's

    CHECK(device.log == "open0[F] talk0 send1 send1 send1 untalk close0 ");
}

TEST_CASE("IEC bus: LISTEN ends talking and TALK ends listening")
{
    IecBus bus;
    RecordingDevice device;
    bus.attach(dev, device);

    SUBCASE("RUN/STOP during LOAD sends LISTEN and CLOSE with no UNTALK")
    {
        atn(bus, {talk(dev), data_channel(0)});
        atn(bus, {listen(dev), close_channel(0)});
        atn(bus, {unlisten});
        CHECK(device.log == "talk0 untalk close0 ");
    }
    SUBCASE("a TALK with no UNLISTEN first still ends the listening session")
    {
        atn(bus, {listen(dev), data_channel(15)});
        atn(bus, {talk(dev), data_channel(15)});
        CHECK(device.log == "listen15 unlisten talk15 ");
    }
    SUBCASE("a TALK with no UNLISTEN first still completes an OPEN")
    {
        atn(bus, {listen(dev), open_channel(0)});
        const std::uint8_t byte = 'X';
        bus.write(&byte, 1, false);
        atn(bus, {talk(dev), data_channel(0)});
        CHECK(device.log == "open0[X] talk0 ");
    }
}

TEST_CASE("IEC bus: idle UNLISTEN and UNTALK are acknowledged and do nothing")
{
    IecBus bus;
    RecordingDevice device;
    bus.attach(dev, device);

    CHECK(atn(bus, {unlisten}) == IecAtnStatus::Acknowledged);
    CHECK(atn(bus, {untalk}) == IecAtnStatus::Acknowledged);
    CHECK(device.log.empty());
}

TEST_CASE("IEC bus: a device that does not answer is not present, and takes nothing")
{
    IecBus bus;
    RecordingDevice device;
    device.isPresent = false;
    bus.attach(dev, device);

    CHECK(bus.device_mask() == 0);
    CHECK(atn(bus, {listen(dev), data_channel(0)}) == IecAtnStatus::NotPresent);
    const std::uint8_t byte = 1;
    CHECK(bus.write(&byte, 1, false) == 0);
    CHECK(atn(bus, {unlisten}) == IecAtnStatus::Acknowledged);

    CHECK(atn(bus, {talk(9), data_channel(0)}) == IecAtnStatus::NotPresent);
    std::uint8_t out = 0;
    const IecReadResult read = bus.read(&out, 1);
    CHECK(read.bytes == 0);
    CHECK(read.eoi);
    CHECK(device.log.empty());
}

TEST_CASE("IEC bus: the device mask has a bit per device that answers")
{
    IecBus bus;
    RecordingDevice eight;
    RecordingDevice nine;
    nine.isPresent = false;
    RecordingDevice thirty;
    CHECK(bus.attach(8, eight));
    CHECK(bus.attach(9, nine));
    CHECK(bus.attach(30, thirty));
    CHECK_FALSE(bus.attach(31, thirty));
    CHECK(bus.device_mask() == ((1u << 8) | (1u << 30)));
    nine.isPresent = true;
    CHECK(bus.device_mask() == ((1u << 8) | (1u << 9) | (1u << 30)));
}

TEST_CASE("IEC bus: a secondary with nothing addressed, or an empty request, is refused")
{
    IecBus bus;
    RecordingDevice device;
    bus.attach(dev, device);
    CHECK(atn(bus, {data_channel(0)}) == IecAtnStatus::NotPresent);
    CHECK(atn(bus, {}) == IecAtnStatus::NotPresent);
    CHECK(device.log.empty());
}

TEST_CASE("IEC bus: with no secondary, data and reads use channel 0")
{
    IecBus bus;
    RecordingDevice device;
    device.toSend = {7};
    bus.attach(dev, device);

    atn(bus, {listen(dev)});
    const std::uint8_t byte = 'x';
    CHECK(bus.write(&byte, 1, false) == 1);
    atn(bus, {unlisten});

    atn(bus, {talk(dev)});
    std::uint8_t out = 0;
    CHECK(bus.read(&out, 1).bytes == 1);
    CHECK(out == 7);
    atn(bus, {untalk});

    CHECK(device.log == "listen0 data[x] unlisten talk0 send1 untalk ");
}

TEST_CASE("IEC bus: a device's refusal to take data reaches the bus master")
{
    IecBus bus;
    RecordingDevice device;
    device.acceptLimit = 0;
    bus.attach(dev, device);

    atn(bus, {listen(dev), data_channel(2)});
    const std::uint8_t byte = 'x';
    CHECK(bus.write(&byte, 1, false) == 0);
}

TEST_CASE("IEC bus: with no listener or talker nothing is taken and a read ends at once")
{
    IecBus bus;
    RecordingDevice device;
    bus.attach(dev, device);

    const std::uint8_t byte = 'x';
    CHECK(bus.write(&byte, 1, false) == 0);
    std::uint8_t out = 0;
    const IecReadResult read = bus.read(&out, 1);
    CHECK(read.bytes == 0);
    CHECK(read.eoi);
}

TEST_CASE("IEC bus: reset tells every device and forgets who was addressed")
{
    IecBus bus;
    RecordingDevice eight;
    RecordingDevice thirty;
    bus.attach(8, eight);
    bus.attach(30, thirty);

    atn(bus, {listen(8), open_channel(0)});
    const std::uint8_t byte = 'x';
    bus.write(&byte, 1, false);
    bus.reset();
    CHECK(eight.log == "reset ");
    CHECK(thirty.log == "reset ");

    // The name was abandoned: UNLISTEN does not open anything.
    atn(bus, {unlisten});
    CHECK(eight.log == "reset ");
}

TEST_CASE("IEC bus: a new bus master drops an unfinished transaction without resetting devices")
{
    IecBus bus;
    RecordingDevice device;
    bus.attach(dev, device);

    atn(bus, {listen(dev), open_channel(0)});
    const std::uint8_t byte = 'x';
    bus.write(&byte, 1, false);
    bus.forget_addressing();
    atn(bus, {unlisten});
    CHECK(device.log.empty());
}
