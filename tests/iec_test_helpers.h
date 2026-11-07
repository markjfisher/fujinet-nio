#pragma once

#include "fujinet/io/iec_bus.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace fujinet::tests {

// The bus commands a computer sends, spelled out, so a test reads as the sequence a
// KERNAL produces rather than as hex.
inline io::IecAtnStatus atn(io::IecBus& bus, std::initializer_list<std::uint8_t> bytes)
{
    const std::vector<std::uint8_t> list(bytes);
    return bus.atn(list.data(), list.size());
}

inline constexpr std::uint8_t unlisten = 0x3F;
inline constexpr std::uint8_t untalk = 0x5F;
inline constexpr std::uint8_t listen(std::uint8_t device) { return static_cast<std::uint8_t>(0x20 + device); }
inline constexpr std::uint8_t talk(std::uint8_t device) { return static_cast<std::uint8_t>(0x40 + device); }
inline constexpr std::uint8_t data_channel(std::uint8_t c) { return static_cast<std::uint8_t>(0x60 + c); }
inline constexpr std::uint8_t close_channel(std::uint8_t c) { return static_cast<std::uint8_t>(0xE0 + c); }
inline constexpr std::uint8_t open_channel(std::uint8_t c) { return static_cast<std::uint8_t>(0xF0 + c); }

// OPEN: a name byte at a time, as VICE's real-device path hands them over.
inline void open_file(io::IecBus& bus, std::uint8_t device, std::uint8_t channel, const std::string& name)
{
    atn(bus, {listen(device), open_channel(channel)});
    for (const char c : name) {
        const auto byte = static_cast<std::uint8_t>(c);
        bus.write(&byte, 1, false);
    }
    atn(bus, {unlisten});
}

struct ReadOutcome {
    std::vector<std::uint8_t> bytes;
    bool eoiOnLastByte{false};  // EOI came with the final byte, as a LOAD needs
    bool emptyEnd{false};       // EOI came alone, with nothing delivered in that read
};

// TALK and read to the end, one byte per read, as the KERNAL does. The UNTALK is left
// to the caller, as a LOAD that fails leaves it out.
inline ReadOutcome read_all(io::IecBus& bus, std::uint8_t device, std::uint8_t channel,
                            std::size_t limit = 1u << 20)
{
    atn(bus, {talk(device), data_channel(channel)});
    ReadOutcome outcome;
    while (outcome.bytes.size() < limit) {
        std::uint8_t byte = 0;
        const io::IecReadResult result = bus.read(&byte, 1);
        if (result.bytes == 1) {
            outcome.bytes.push_back(byte);
        }
        if (result.eoi) {
            outcome.eoiOnLastByte = result.bytes == 1;
            outcome.emptyEnd = result.bytes == 0;
            break;
        }
    }
    return outcome;
}

inline std::string as_text(const std::vector<std::uint8_t>& bytes)
{
    return std::string(bytes.begin(), bytes.end());
}

} // namespace fujinet::tests
