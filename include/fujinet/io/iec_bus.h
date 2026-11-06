#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace fujinet::io {

// The Commodore serial (IEC) bus as devices see it: the commands sent under ATN,
// and the data bytes between them. Nothing here knows how those reach the FujiNet.
// An ingress decodes them (NetIEC from an emulator; bus lines on hardware) and
// calls IecBus, which keeps track of who is addressed and calls the device.
// docs/iec/README.md describes the layers.

struct IecReadResult {
    std::size_t bytes{0};
    // The talker is done: the last byte returned (if any) is the final one.
    bool eoi{false};
};

// A device on the bus at one device number. Channels are 0-15.
class IIecDevice {
public:
    virtual ~IIecDevice() = default;

    // Whether the device answers when addressed. An absent device makes the
    // computer report DEVICE NOT PRESENT.
    virtual bool present() const { return true; }

    // OPEN: `name` is everything sent between the OPEN command and UNLISTEN.
    virtual void open(std::uint8_t channel, const std::uint8_t* name, std::size_t nameLength) = 0;
    virtual void close(std::uint8_t channel) = 0;

    // LISTEN with a data channel; data arrives through receive() until unlisten().
    // receive() returns the bytes taken: fewer than offered tells the computer the
    // channel is refusing data.
    virtual void listen(std::uint8_t channel) = 0;
    virtual std::size_t receive(const std::uint8_t* data, std::size_t length, bool eoi) = 0;
    virtual void unlisten() = 0;

    // TALK with a data channel; send() is asked for data until untalk().
    virtual void talk(std::uint8_t channel) = 0;
    virtual IecReadResult send(std::uint8_t* dst, std::size_t maxBytes) = 0;
    virtual void untalk() = 0;

    // The computer was reset.
    virtual void bus_reset() = 0;
};

enum class IecAtnStatus : std::uint8_t {
    Acknowledged,
    NotPresent,
};

class IecBus {
public:
    static constexpr std::uint8_t max_device = 30; // 31 in a command means UNLISTEN/UNTALK

    // The device is not owned. Returns false for a number the bus cannot address.
    bool attach(std::uint8_t device, IIecDevice& iecDevice);

    // Bit n is set when device n answers now.
    std::uint32_t device_mask() const;

    // Bytes sent under ATN, in order: a primary command, usually followed by a
    // secondary. NotPresent if one of them addressed a device that does not answer,
    // or there was nothing addressed for it to apply to.
    IecAtnStatus atn(const std::uint8_t* bytes, std::size_t length);

    // Data from the computer to the listener. Returns the bytes taken; 0 if there
    // is no listener.
    std::size_t write(const std::uint8_t* data, std::size_t length, bool eoi);

    // Data from the talker to the computer. Without a talker: nothing, with EOI.
    IecReadResult read(std::uint8_t* dst, std::size_t maxBytes);

    // The computer was reset: forget who was addressed and tell every device.
    void reset();

    // Forget who was addressed without telling anyone: a new bus master knows nothing
    // of a transaction its predecessor left unfinished, so that must not complete.
    void forget_addressing();

private:
    // Longest OPEN name kept; a drive's longest legal command is well under this.
    static constexpr std::size_t max_name_bytes = 255;
    static constexpr int none = -1;

    IIecDevice* at(int device) const;
    IecAtnStatus command(std::uint8_t byte);
    void end_listener();
    void end_talker();

    std::array<IIecDevice*, max_device + 1> _devices{};
    int _listener{none};
    int _talker{none};
    bool _talking{false};    // TALK followed by its channel
    bool _listening{false};  // LISTEN followed by a data channel
    bool _opening{false};    // LISTEN followed by OPEN: data bytes are the name
    std::uint8_t _openChannel{0};
    std::vector<std::uint8_t> _name;
};

} // namespace fujinet::io
