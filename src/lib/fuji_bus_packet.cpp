#include "fujinet/io/protocol/fuji_bus_packet.h"

#include <algorithm> // std::find
#include <cstddef>   // std::size_t, offsetof
#include <cstdint>
#include <cstring>   // std::memcpy

using fujinet::io::protocol::SlipByte;

namespace fujinet::io::protocol {

// On-wire header layout (must stay exactly this size/layout)
struct FujiBusHeader {
    std::uint8_t  device;   // Destination Device
    std::uint8_t  command;  // Command
    std::uint16_t length;   // Total length of packet including header
    std::uint8_t  checksum; // Checksum of entire packet
    std::uint8_t  descr;    // Describes the fields that follow (first descriptor)
};

static_assert(sizeof(FujiBusHeader) == 6, "FujiBusHeader must be 6 bytes");
static_assert(offsetof(FujiBusHeader, checksum) == 4, "checksum offset mismatch");

// Descriptor bit masks
constexpr std::uint8_t FUJI_DESCR_COUNT_MASK  = 0x07;
constexpr std::uint8_t FUJI_DESCR_EXCEEDS_U8  = 0x04;
constexpr std::uint8_t FUJI_DESCR_EXCEEDS_U16 = 0x02;
constexpr std::uint8_t FUJI_DESCR_ADDTL_MASK  = 0x80;
constexpr std::uint8_t MAX_BYTES_PER_DESCR    = 4;

// Tables describing how many fields / what size correspond to a descriptor nibble.
// Indexed by (descr & FUJI_DESCR_COUNT_MASK).
constexpr std::uint8_t FIELD_SIZE_TABLE[8] = {0, 1, 1, 1, 1, 2, 2, 4};
constexpr std::uint8_t NUM_FIELDS_TABLE[8] = {0, 1, 2, 3, 4, 1, 2, 1};

namespace {

// write `size` bytes of `value` (1,2,4) in little-endian
inline void write_le(ByteBuffer& buf, std::uint32_t value, std::size_t size)
{
    for (std::size_t i = 0; i < size; ++i) {
        buf.push_back(static_cast<std::uint8_t>((value >> (8U * i)) & 0xFFU));
    }
}

// read `size` bytes in little-endian from `buf[offset..offset+size)`
inline std::uint32_t read_le(const ByteBuffer& buf, std::size_t offset, std::size_t size)
{
    std::uint32_t v = 0;
    for (std::size_t i = 0; i < size; ++i) {
        v |= static_cast<std::uint32_t>(buf[offset + i]) << (8U * i);
    }
    return v;
}

} // namespace

ByteBuffer FujiBusPacket::decodeSLIP(const ByteBuffer& input) const
{
    ByteBuffer output;
    output.reserve(input.size());  // worst case, same size

    const std::size_t len = input.size();
    std::size_t idx = 0;

    // Find the first SLIP_END
    while (idx < len && input[idx] != to_byte(SlipByte::End)) {
        ++idx;
    }

    if (idx == len) {
        // no frame start found
        return {};
    }

    // Decode from the byte after the first SLIP_END
    for (++idx; idx < len; ++idx) {
        std::uint8_t val = input[idx];
        if (val == to_byte(SlipByte::End)) {
            break;
        }

        if (val == to_byte(SlipByte::Escape)) {
            if (++idx >= len) {
                break; // truncated escape
            }
            val = input[idx];
            if (val == to_byte(SlipByte::EscEnd)) {
                output.push_back(to_byte(SlipByte::End));
            } else if (val == to_byte(SlipByte::EscEsc)) {
                output.push_back(to_byte(SlipByte::Escape));
            }
            // else: ignore malformed escape
        } else {
            output.push_back(val);
        }
    }

    return output;
}

ByteBuffer FujiBusPacket::encodeSLIP(const ByteBuffer& input) const
{
    ByteBuffer output;
    output.reserve(input.size() * 2U + 2U);  // worst case, double size + 2 ENDs

    // Avoids a compiler warning with some GCC versions about push_back on an
    // empty-but-reserved vector.
    output.resize(1);
    output[0] = to_byte(SlipByte::End);

    for (std::uint8_t val : input) {
        if (val == to_byte(SlipByte::End) || val == to_byte(SlipByte::Escape)) {
            output.push_back(to_byte(SlipByte::Escape));
            if (val == to_byte(SlipByte::End)) {
                output.push_back(to_byte(SlipByte::EscEnd));
            } else {
                output.push_back(to_byte(SlipByte::EscEsc));
            }
        } else {
            output.push_back(val);
        }
    }

    output.push_back(to_byte(SlipByte::End));
    return output;
}

std::uint8_t FujiBusPacket::calcChecksum(const ByteBuffer& buf) const
{
    std::uint16_t chk = 0;

    for (std::uint8_t b : buf) {
        chk += b;
        chk = static_cast<std::uint16_t>((chk >> 8) + (chk & 0xFFU)); // fold carry
    }

    return static_cast<std::uint8_t>(chk);
}

bool FujiBusPacket::parse(const ByteBuffer& input)
{
    ByteBuffer slipEncoded;

    // Find first SLIP_END, and treat the frame as starting there.
    auto it = std::find(input.begin(), input.end(), to_byte(SlipByte::End));
    if (it != input.end()) {
        slipEncoded.assign(it, input.end());
    } else {
        slipEncoded = input;
    }

    if (slipEncoded.size() < sizeof(FujiBusHeader) + 2U) {
        return false;
    }
    if (slipEncoded.front() != to_byte(SlipByte::End) || slipEncoded.back() != to_byte(SlipByte::End)) {
        return false;
    }

    ByteBuffer decoded = decodeSLIP(slipEncoded);

    if (decoded.size() < sizeof(FujiBusHeader)) {
        return false;
    }

    // Extract header from the front of decoded safely (no pointer aliasing).
    FujiBusHeader hdr{};
    std::memcpy(&hdr, decoded.data(), sizeof(FujiBusHeader));

    if (hdr.length != decoded.size()) {
        return false;
    }

    // Verify checksum:
    // - ck1 is the transmitted checksum
    // - ck2 is computed with the checksum byte zeroed
    const std::uint8_t ck1 = hdr.checksum;

    ByteBuffer tmp = decoded;
    tmp[offsetof(FujiBusHeader, checksum)] = 0;
    const std::uint8_t ck2 = calcChecksum(tmp);

    if (ck1 != ck2) {
        return false;
    }

    _device  = static_cast<WireDeviceId>(hdr.device);
    _command = hdr.command;

    // ---- Descriptors & params ----

    std::size_t offset = sizeof(FujiBusHeader);
    ByteBuffer descrBytes;

    // First descriptor is in the header
    std::uint8_t dsc = hdr.descr;
    descrBytes.push_back(dsc);

    // Additional descriptors follow the header whenever bit 7 is set
    while (dsc & FUJI_DESCR_ADDTL_MASK) {
        if (offset >= decoded.size()) {
            return false; // malformed
        }

        dsc = decoded[offset++];
        descrBytes.push_back(dsc);
    }

    // Now decode each descriptor into fields
    for (std::uint8_t dbyte : descrBytes) {
        unsigned fieldDesc  = dbyte & FUJI_DESCR_COUNT_MASK; // 0..7
        unsigned fieldCount = NUM_FIELDS_TABLE[fieldDesc];
        if (!fieldCount) {
            continue;
        }

        unsigned fieldSize  = FIELD_SIZE_TABLE[fieldDesc];

        for (unsigned idx = 0; idx < fieldCount; ++idx) {
            if (offset + fieldSize > decoded.size()) {
                return false;
            }

            std::uint32_t val = read_le(decoded, offset, fieldSize);
            _params.emplace_back(val, static_cast<std::uint8_t>(fieldSize));
            offset += fieldSize;
        }
    }

    // Remaining bytes (if any) are payload
    if (offset < decoded.size()) {
        _data.emplace(decoded.begin() + static_cast<std::ptrdiff_t>(offset),
                      decoded.end());
    }

    return true;
}

ByteBuffer FujiBusPacket::serialize() const
{
    // Start with an empty header.
    FujiBusHeader hdr{};
    hdr.device   = static_cast<std::uint8_t>(_device);
    hdr.command  = static_cast<std::uint8_t>(_command);
    hdr.length   = sizeof(FujiBusHeader);
    hdr.checksum = 0;
    hdr.descr    = 0;

    // Reserve space for header; we'll fill it in later.
    ByteBuffer output(sizeof(FujiBusHeader), 0);

    // ---- Parameters & descriptors ----
    if (!_params.empty()) {
        ByteBuffer descr;
        std::size_t lenParams = _params.size();

        std::size_t idx = 0;
        while (idx < lenParams) {
            unsigned fieldSize    = 0;
            unsigned bytesWritten = 0;
            unsigned count        = 0;

            // Group as many params as fit in this descriptor.
            for (; (idx + count) < lenParams; ++count) {
                const PacketParam& param = _params[idx + count];

                if ((fieldSize != 0 && fieldSize != param.size) ||
                    bytesWritten == MAX_BYTES_PER_DESCR) {
                    break;
                }

                fieldSize = param.size;
                write_le(output, param.value, param.size);
                bytesWritten += param.size;
            }

            std::uint8_t fieldDescr = static_cast<std::uint8_t>(count);
            if (fieldSize > 1) {
                fieldDescr |= FUJI_DESCR_EXCEEDS_U8;
                if (fieldSize > 2) {
                    fieldDescr |= FUJI_DESCR_EXCEEDS_U16;
                }
            }

            descr.push_back(static_cast<std::uint8_t>(fieldDescr | FUJI_DESCR_ADDTL_MASK));
            idx += count;
        }

        // Clear the "additional descriptors" bit on the last descriptor.
        if (!descr.empty()) {
            descr.back() &= static_cast<std::uint8_t>(~FUJI_DESCR_ADDTL_MASK);
            hdr.descr = descr[0];
            // Insert additional descriptors (if any) immediately after header.
            if (descr.size() > 1) {
                output.insert(output.begin() + static_cast<std::ptrdiff_t>(sizeof(FujiBusHeader)),
                              descr.begin() + 1, descr.end());
            }
        }
    }

    // ---- Payload ----
    if (_data) {
        output.insert(output.end(), _data->begin(), _data->end());
    }

    // Finalise header.
    hdr.length = static_cast<std::uint16_t>(output.size());

    // Write header into the beginning of the buffer.
    std::memcpy(output.data(), &hdr, sizeof(FujiBusHeader));

    // Compute checksum over full packet with checksum field currently 0.
    std::uint8_t checksum = calcChecksum(output);
    output[offsetof(FujiBusHeader, checksum)] = checksum;

    // SLIP-encode the whole packet.
    return encodeSLIP(output);
}

// ------------------ Factory ------------------
std::unique_ptr<FujiBusPacket> FujiBusPacket::fromSerialized(const ByteBuffer& input)
{
    auto packet = std::make_unique<FujiBusPacket>();
    if (!packet->parse(input)) {
        return nullptr;
    }
    return packet;
}

} // namespace fujinet::io::protocol
